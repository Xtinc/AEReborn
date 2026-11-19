#include "adapter.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace aereborn
{
namespace
{

constexpr std::size_t order = 48;
constexpr std::size_t taps = 2 * order;
constexpr std::uint32_t phase_steps = 100;
constexpr double pi = 3.141592653589793;

std::uint32_t read_le(const std::uint8_t *input, std::size_t bytes) noexcept
{
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < bytes; ++i)
    {
        raw |= static_cast<std::uint32_t>(input[i]) << (8 * i);
    }
    return raw;
}

void write_le(std::uint32_t raw, std::uint8_t *output, std::size_t bytes) noexcept
{
    for (std::size_t i = 0; i < bytes; ++i)
    {
        output[i] = static_cast<std::uint8_t>(raw >> (8 * i));
    }
}

float decode_integer(std::uint32_t raw, unsigned int bits) noexcept
{
    const auto scale = std::int64_t{1} << (bits - 1);
    const auto value = std::int64_t{raw} & (2 * scale - 1);
    return static_cast<float>(value >= scale ? value - 2 * scale : value) / static_cast<float>(scale);
}

std::uint32_t encode_integer(float sample, unsigned int bits) noexcept
{
    const auto scale = std::int64_t{1} << (bits - 1);
    const auto normalized = std::isnan(sample) ? 0.0f : std::max(-1.0f, std::min(1.0f, sample));
    const auto rounded = static_cast<std::int64_t>(std::round(static_cast<double>(normalized) * scale));
    const auto value = std::min(scale - 1, rounded);
    return static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(2 * scale - 1);
}

bool native_f32le() noexcept
{
    const float one = 1.0f;
    constexpr std::uint8_t one_le[] = {0, 0, 0x80, 0x3f};
    return std::memcmp(&one, one_le, sizeof(one)) == 0;
}

void copy_f32le(const std::uint8_t *pcm, OBlock output) noexcept
{
    if (output.channels() == 1 && native_f32le())
    {
        std::memcpy(output.channel(0), pcm, output.frames() * sizeof(float));
        return;
    }
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch, pcm += sizeof(float))
        {
            const auto raw = read_le(pcm, sizeof(float));
            std::memcpy(output.channel(ch) + frame, &raw, sizeof(raw));
        }
    }
}

void copy_f32le(IBlock input, std::uint8_t *pcm) noexcept
{
    if (input.channels() == 1 && native_f32le())
    {
        std::memcpy(pcm, input.channel(0), input.frames() * sizeof(float));
        return;
    }
    for (std::size_t frame = 0; frame < input.frames(); ++frame)
    {
        for (std::size_t ch = 0; ch < input.channels(); ++ch, pcm += sizeof(float))
        {
            std::uint32_t raw;
            std::memcpy(&raw, input.channel(ch) + frame, sizeof(raw));
            write_le(raw, pcm, sizeof(raw));
        }
    }
}

// Window and cutoff ported from AudioDriver's SincInterpolator.
double sinc_kernel(double x, double cutoff) noexcept
{
    if (std::abs(x) >= order)
    {
        return 0.0;
    }
    const auto angle = (2.0 * pi * (x + 1.0 - order)) / (taps - 1);
    const auto window =
        0.3635819 - 0.4891775 * std::cos(angle) + 0.1365995 * std::cos(2 * angle) - 0.0106411 * std::cos(3 * angle);
    const auto sinc_angle = pi * cutoff * x;
    const auto sinc = sinc_angle == 0 ? 1.0 : std::sin(sinc_angle) / sinc_angle;
    return sinc * window;
}

using Coefficients = std::array<float, taps>;

void calc_coffs(const float *kernel, std::uint32_t dst_fs, std::uint64_t src_pos, Coefficients &coffs) noexcept
{
    const auto phase_pos = (src_pos % dst_fs) * phase_steps;
    const auto phase_index = phase_pos / dst_fs;
    const auto phase_frac = static_cast<float>(phase_pos % dst_fs) / static_cast<float>(dst_fs);
    const auto *lo_coffs = kernel + phase_index * taps;
    const auto *hi_coffs = lo_coffs + taps;
    for (std::size_t i = 0; i < taps; ++i)
    {
        coffs[i] = lo_coffs[i] + (hi_coffs[i] - lo_coffs[i]) * phase_frac;
    }
}

float interpolate(const float *history, const float *input, std::size_t history_taps,
                  const Coefficients &coffs) noexcept
{
    double sum = 0;
    for (std::size_t i = 0; i < history_taps; ++i)
    {
        sum += history[i] * coffs[i];
    }
    for (std::size_t i = history_taps; i < taps; ++i)
    {
        sum += input[i - history_taps] * coffs[i];
    }
    return static_cast<float>(sum);
}

} // namespace

// Conversion state prepared from the device PCM and graph timing specifications.
class PCMConverter
{
  public:
    PCMConverter(PCMSpec spec, SampleRate src_fs, SampleRate dst_fs, BlockPeriod period, const ChMatrix &matrix);
    RetCode prepare_pcm(std::size_t graph_frames);
    void reset() noexcept;
    std::size_t pcm_frames() const noexcept
    {
        return pcm_frames_;
    }
    std::size_t pcm_bytes() const noexcept
    {
        return pcm_frames_ * spec_.channels * sample_bytes_;
    }
    void decode(const std::uint8_t *pcm, OBlock output) noexcept;
    void encode(IBlock input, std::uint8_t *pcm) noexcept;
    // Return true when the current device buffer is complete.
    bool advance_period() noexcept;

  private:
    std::size_t frame_offset(std::size_t period_index) const noexcept;
    void resample(IBlock input, OBlock output) noexcept;

    const PCMSpec spec_;
    const ChMatrix matrix_;
    const std::uint64_t period_frames_x1000_; // PCM frames per engine period in units of 1/1000 frame.
    const std::size_t sample_bytes_;
    const unsigned int sample_bits_;
    const std::uint32_t src_fs_;
    const std::uint32_t dst_fs_;
    std::size_t pcm_frames_ = 0;
    std::size_t period_index_ = 0;
    Buffer pcm_samples_;
    Buffer mapped_samples_; // Current-period workspace in device channels; never queues audio.
    Buffer history_;
    std::vector<float> kernel_;
    std::int64_t phase_ = 0; // Next output sample's source position, in units of 1 / dst_fs_.
};

PCMConverter::PCMConverter(PCMSpec spec, SampleRate src_fs, SampleRate dst_fs, BlockPeriod period,
                           const ChMatrix &matrix)
    : spec_(spec), matrix_(matrix), period_frames_x1000_(std::uint64_t{static_cast<std::uint32_t>(period)} *
                                                         static_cast<std::uint32_t>(spec.sample_rate)),
      sample_bytes_(pcm_sample_bytes(spec.format)), sample_bits_(pcm_sample_bits(spec.format)),
      src_fs_(static_cast<std::uint32_t>(src_fs)), dst_fs_(static_cast<std::uint32_t>(dst_fs))
{
}

RetCode PCMConverter::prepare_pcm(std::size_t graph_frames)
{
    if (sample_bytes_ == 0 || !is_pcm_sample_rate(spec_.sample_rate) || spec_.periods == 0)
    {
        return ErrorCode::InvalidArg;
    }
    if (spec_.periods > std::numeric_limits<std::uint64_t>::max() / period_frames_x1000_)
    {
        return ErrorCode::TooLarge;
    }
    const auto transfer_frames_x1000 = period_frames_x1000_ * spec_.periods;
    if (transfer_frames_x1000 % 1000 != 0)
    {
        return ErrorCode::InvalidArg;
    }
    const auto transfer_frames = transfer_frames_x1000 / 1000;
    if (spec_.channels > std::numeric_limits<std::size_t>::max() / sample_bytes_ / transfer_frames)
    {
        return ErrorCode::TooLarge;
    }
    if (!matrix_.empty())
    {
        const auto code = mapped_samples_.resize(spec_.channels, graph_frames);
        if (!code)
        {
            return code;
        }
    }
    if (src_fs_ != dst_fs_)
    {
        const auto samples_code = pcm_samples_.resize(spec_.channels, frame_offset(1));
        if (!samples_code)
        {
            return samples_code;
        }
        const auto history_code = history_.resize(spec_.channels, taps);
        if (!history_code)
        {
            return history_code;
        }
        kernel_.resize((phase_steps + 1) * taps);
        const auto cutoff = std::min(1.0, static_cast<double>(dst_fs_) / src_fs_) * 0.98;
        for (std::uint32_t phase_index = 0; phase_index <= phase_steps; ++phase_index)
        {
            auto *coffs = kernel_.data() + phase_index * taps;
            const auto phase_frac = static_cast<double>(phase_index) / phase_steps;
            double scale = 0;
            for (std::size_t i = 0; i < taps; ++i)
            {
                coffs[i] = static_cast<float>(sinc_kernel(phase_frac + order - 1 - i, cutoff));
                scale += coffs[i];
            }
            for (std::size_t i = 0; i < taps; ++i)
            {
                coffs[i] = static_cast<float>(coffs[i] / scale);
            }
        }
    }
    pcm_frames_ = static_cast<std::size_t>(transfer_frames);
    return {};
}

std::size_t PCMConverter::frame_offset(std::size_t period_index) const noexcept
{
    // Round the cumulative frame position up, keeping fractional periods aligned.
    const auto frames_x1000 = period_frames_x1000_ * period_index;
    return static_cast<std::size_t>(frames_x1000 / 1000 + (frames_x1000 % 1000 != 0));
}

void PCMConverter::decode(const std::uint8_t *pcm, OBlock output) noexcept
{
    const auto first_frame = frame_offset(period_index_);
    pcm += first_frame * spec_.channels * sample_bytes_;
    const bool resampling = src_fs_ != dst_fs_;
    const auto mapped = matrix_.empty() ? output : mapped_samples_.block(output.frames());
    const auto samples = resampling ? pcm_samples_.block(frame_offset(period_index_ + 1) - first_frame) : mapped;
    if (spec_.format == PCMFormat::F32LE)
    {
        copy_f32le(pcm, samples);
    }
    else
    {
        for (std::size_t frame = 0; frame < samples.frames(); ++frame)
        {
            for (std::size_t ch = 0; ch < samples.channels(); ++ch, pcm += sample_bytes_)
            {
                samples.channel(ch)[frame] = decode_integer(read_le(pcm, sample_bytes_), sample_bits_);
            }
        }
    }
    if (resampling)
    {
        resample(samples, mapped);
    }
    if (!matrix_.empty())
    {
        output.clear();
        matrix_.mix_add(mapped, output);
    }
}

void PCMConverter::encode(IBlock input, std::uint8_t *pcm) noexcept
{
    const auto first_frame = frame_offset(period_index_);
    pcm += first_frame * spec_.channels * sample_bytes_;
    if (!matrix_.empty())
    {
        const auto mapped = mapped_samples_.block(input.frames());
        mapped.clear();
        matrix_.mix_add(input, mapped);
        input = mapped;
    }
    if (src_fs_ != dst_fs_)
    {
        const auto samples = pcm_samples_.block(frame_offset(period_index_ + 1) - first_frame);
        resample(input, samples);
        input = samples;
    }
    if (spec_.format == PCMFormat::F32LE)
    {
        copy_f32le(input, pcm);
        return;
    }
    for (std::size_t frame = 0; frame < input.frames(); ++frame)
    {
        for (std::size_t ch = 0; ch < input.channels(); ++ch, pcm += sample_bytes_)
        {
            write_le(encode_integer(input.channel(ch)[frame], sample_bits_), pcm, sample_bytes_);
        }
    }
}

void PCMConverter::resample(IBlock input, OBlock output) noexcept
{
    const auto history = history_.block(taps);
    // src_pos is in units of 1 / dst_fs_; the first taps frames address history.
    auto src_pos = static_cast<std::uint64_t>(dst_fs_ + phase_);
    Coefficients coffs;
    for (std::size_t frame = 0; frame < output.frames(); ++frame, src_pos += src_fs_)
    {
        calc_coffs(kernel_.data(), dst_fs_, src_pos, coffs);
        const auto frame_index = static_cast<std::size_t>(src_pos / dst_fs_);
        const auto history_offset = std::min(frame_index, taps);
        const auto input_offset = frame_index - history_offset;
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            output.channel(ch)[frame] = interpolate(history.channel(ch) + history_offset,
                                                    input.channel(ch) + input_offset, taps - history_offset, coffs);
        }
    }

    phase_ +=
        static_cast<std::int64_t>(output.frames()) * src_fs_ - static_cast<std::int64_t>(input.frames()) * dst_fs_;
    const auto copy_frames = std::min(input.frames(), taps);
    const auto keep_frames = taps - copy_frames;
    for (std::size_t ch = 0; ch < input.channels(); ++ch)
    {
        auto *history_samples = history.channel(ch);
        std::memmove(history_samples, history_samples + copy_frames, keep_frames * sizeof(float));
        std::copy_n(input.channel(ch) + input.frames() - copy_frames, copy_frames, history_samples + keep_frames);
    }
}

void PCMConverter::reset() noexcept
{
    period_index_ = 0;
    phase_ = 0;
    history_.block(history_.capacity_frames()).clear();
}

bool PCMConverter::advance_period() noexcept
{
    if (++period_index_ == spec_.periods)
    {
        period_index_ = 0;
        return true;
    }
    return false;
}

namespace
{

enum class Direction
{
    Capture,
    Playback
};

RetCode configure_converter(std::unique_ptr<PCMConverter> &active, const EngineSpec &graph_spec, std::size_t channels,
                            PCMSpec spec, const ChMatrix &matrix, Direction direction)
{
    const auto frames = graph_spec.block_frames();
    if (frames == 0 || channels == 0 || spec.channels == 0)
    {
        return ErrorCode::InvalidArg;
    }

    const bool capture = direction == Direction::Capture;
    const auto src_channels = capture ? spec.channels : channels;
    const auto dst_channels = capture ? channels : spec.channels;
    if (matrix.empty() ? src_channels != dst_channels
                       : matrix.input_channels() != src_channels || matrix.output_channels() != dst_channels)
    {
        return ErrorCode::ChannelMismatch;
    }

    const auto src_fs = capture ? spec.sample_rate : graph_spec.sample_rate();
    const auto dst_fs = capture ? graph_spec.sample_rate() : spec.sample_rate;
    auto converter = std::make_unique<PCMConverter>(spec, src_fs, dst_fs, graph_spec.block_period(), matrix);
    const auto code = converter->prepare_pcm(frames);
    if (!code)
    {
        return code;
    }
    active = std::move(converter);
    return {};
}

RetCode check_pcm_binding(const PCMConverter *converter, std::size_t bytes, bool bound) noexcept
{
    if (!converter)
    {
        return ErrorCode::NotPrepared;
    }
    if (bytes != converter->pcm_bytes())
    {
        return ErrorCode::InvalidArg;
    }
    return bound ? ErrorCode::BufferFull : ErrorCode::Ok;
}

} // namespace

IAdapter::IAdapter(std::size_t channels, EngineSpec graph_spec) : NodeT<0, 1>({}, {{channels}}), graph_spec_(graph_spec)
{
}

IAdapter::~IAdapter() = default;

std::size_t IAdapter::pcm_frames() const noexcept
{
    return converter_ ? converter_->pcm_frames() : 0;
}

std::size_t IAdapter::pcm_bytes() const noexcept
{
    return converter_ ? converter_->pcm_bytes() : 0;
}

RetCode IAdapter::bind_pcm(View<const std::uint8_t> bytes) noexcept
{
    const auto code = check_pcm_binding(converter_.get(), bytes.size(), pcm_ != nullptr);
    if (code)
    {
        pcm_ = bytes.data();
    }
    return code;
}

RetCode IAdapter::configure_pcm(PCMSpec spec, const ChMatrix &matrix)
{
    const auto code = configure_converter(converter_, graph_spec_, outputs()[0], spec, matrix, Direction::Capture);
    if (code)
    {
        pcm_ = nullptr;
    }
    return code;
}

void IAdapter::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0];
    if (!pcm_)
    {
        output.clear();
        return;
    }
    converter_->decode(pcm_, output);
    if (converter_->advance_period())
    {
        pcm_ = nullptr;
    }
}

void IAdapter::reset() noexcept
{
    pcm_ = nullptr;
    if (converter_)
    {
        converter_->reset();
    }
}

RetCode IAdapter::prepare(const EngineSpec &graph_spec) noexcept
{
    return graph_spec_ == graph_spec ? ErrorCode::Ok : ErrorCode::InvalidArg;
}

OAdapter::OAdapter(std::size_t channels, EngineSpec graph_spec) : NodeT<1, 0>({{channels}}, {}), graph_spec_(graph_spec)
{
}

OAdapter::~OAdapter() = default;

std::size_t OAdapter::pcm_frames() const noexcept
{
    return converter_ ? converter_->pcm_frames() : 0;
}

std::size_t OAdapter::pcm_bytes() const noexcept
{
    return converter_ ? converter_->pcm_bytes() : 0;
}

RetCode OAdapter::bind_pcm(View<std::uint8_t> bytes) noexcept
{
    const auto code = check_pcm_binding(converter_.get(), bytes.size(), pcm_ != nullptr);
    if (code)
    {
        pcm_ = bytes.data();
    }
    return code;
}

RetCode OAdapter::configure_pcm(PCMSpec spec, const ChMatrix &matrix)
{
    const auto code = configure_converter(converter_, graph_spec_, inputs()[0], spec, matrix, Direction::Playback);
    if (code)
    {
        pcm_ = nullptr;
    }
    return code;
}

void OAdapter::process(const ProcContext &context) noexcept
{
    if (!pcm_)
    {
        return;
    }
    converter_->encode(context.inputs[0], pcm_);
    if (converter_->advance_period())
    {
        pcm_ = nullptr;
    }
}

void OAdapter::reset() noexcept
{
    pcm_ = nullptr;
    if (converter_)
    {
        converter_->reset();
    }
}

RetCode OAdapter::prepare(const EngineSpec &graph_spec) noexcept
{
    return graph_spec_ == graph_spec ? ErrorCode::Ok : ErrorCode::InvalidArg;
}

} // namespace aereborn
