#include "convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace aereborn
{
namespace
{

template <std::size_t Bytes> std::uint32_t read_le(const std::uint8_t *input) noexcept
{
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < Bytes; ++i)
    {
        raw |= static_cast<std::uint32_t>(input[i]) << (8 * i);
    }
    return raw;
}

template <std::size_t Bytes> void write_le(std::uint32_t raw, std::uint8_t *output) noexcept
{
    for (std::size_t i = 0; i < Bytes; ++i)
    {
        output[i] = static_cast<std::uint8_t>(raw >> (8 * i));
    }
}

template <unsigned int Bits> float decode_integer(std::uint32_t raw) noexcept
{
    constexpr auto scale = std::int64_t{1} << (Bits - 1);
    const auto value = std::int64_t{raw} & (2 * scale - 1);
    return static_cast<float>(value >= scale ? value - 2 * scale : value) / static_cast<float>(scale);
}

template <unsigned int Bits> std::uint32_t encode_integer(float sample) noexcept
{
    constexpr auto scale = std::int64_t{1} << (Bits - 1);
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

void decode_f32le(const std::uint8_t *pcm, OFBlock output) noexcept
{
    if (output.frames() == 0)
        return;
    if (output.channels() == 1 && native_f32le())
    {
        std::memcpy(output.channel(0), pcm, output.frames() * sizeof(float));
        return;
    }
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch, pcm += sizeof(float))
        {
            const auto raw = read_le<sizeof(float)>(pcm);
            std::memcpy(output.channel(ch) + frame, &raw, sizeof(raw));
        }
    }
}

void encode_f32le(IFBlock input, std::uint8_t *pcm) noexcept
{
    if (input.frames() == 0)
        return;
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
            write_le<sizeof(raw)>(raw, pcm);
        }
    }
}

template <SampleFormat Format> void decode_pcm(const std::uint8_t *source, OFBlock output) noexcept
{
    constexpr auto width = sample_bytes(Format);
    constexpr auto bits = sample_bits(Format);
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
        for (std::size_t ch = 0; ch < output.channels(); ++ch, source += width)
            output.channel(ch)[frame] = decode_integer<bits>(read_le<width>(source));
}

template <SampleFormat Format> void encode_pcm(IFBlock input, std::uint8_t *destination) noexcept
{
    constexpr auto width = sample_bytes(Format);
    constexpr auto bits = sample_bits(Format);
    for (std::size_t frame = 0; frame < input.frames(); ++frame)
        for (std::size_t ch = 0; ch < input.channels(); ++ch, destination += width)
            write_le<width>(encode_integer<bits>(input.channel(ch)[frame]), destination);
}

constexpr std::size_t order = 48;
constexpr std::size_t taps = 2 * order;
constexpr std::uint32_t phase_steps = 100;
constexpr double pi = 3.141592653589793;

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

class SincResampler
{
  public:
    SincResampler(SampleRate input, SampleRate output) noexcept
        : src_fs_(static_cast<std::uint32_t>(input)), dst_fs_(static_cast<std::uint32_t>(output))
    {
    }
    RetCode prepare(std::size_t channels)
    {
        const auto code = history_.resize(channels, taps);
        if (!code)
            return code;
        kernel_.resize((phase_steps + 1) * taps);
        const auto cutoff = std::min(1.0, static_cast<double>(dst_fs_) / src_fs_) * 0.98;
        for (std::uint32_t phase = 0; phase <= phase_steps; ++phase)
        {
            auto *coffs = kernel_.data() + phase * taps;
            const auto fraction = static_cast<double>(phase) / phase_steps;
            double scale = 0;
            for (std::size_t i = 0; i < taps; ++i)
            {
                coffs[i] = static_cast<float>(sinc_kernel(fraction + order - 1 - i, cutoff));
                scale += coffs[i];
            }
            for (std::size_t i = 0; i < taps; ++i)
                coffs[i] = static_cast<float>(coffs[i] / scale);
        }
        return {};
    }
    void reset() noexcept
    {
        history_.clear();
    }
    void process(IFBlock input, OFBlock output, BlockPeriod period, std::uint64_t tick) noexcept;

  private:
    const std::uint32_t src_fs_;
    const std::uint32_t dst_fs_;
    Buffer history_;
    std::vector<float> kernel_;
};

void SincResampler::process(IFBlock input, OFBlock output, BlockPeriod period, std::uint64_t tick) noexcept
{
    const auto past = history_.block(taps);
    // src_pos is in units of 1 / the output sample rate; the first taps frames address history.
    const auto rounding = [period, tick](std::uint32_t rate) {
        const auto remainder =
            (tick % 1000) * ((std::uint64_t{rate} * static_cast<std::uint32_t>(period)) % 1000) % 1000;
        return static_cast<std::int64_t>((1000 - remainder) % 1000);
    };
    const auto phase = (rounding(dst_fs_) * src_fs_ - rounding(src_fs_) * dst_fs_) / 1000;
    auto src_pos = static_cast<std::uint64_t>(dst_fs_ + phase);
    Coefficients coffs;
    for (std::size_t frame = 0; frame < output.frames(); ++frame, src_pos += src_fs_)
    {
        calc_coffs(kernel_.data(), dst_fs_, src_pos, coffs);
        const auto frame_index = static_cast<std::size_t>(src_pos / dst_fs_);
        const auto history_offset = std::min(frame_index, taps);
        const auto input_offset = frame_index - history_offset;
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            output.channel(ch)[frame] = interpolate(past.channel(ch) + history_offset, input.channel(ch) + input_offset,
                                                    taps - history_offset, coffs);
        }
    }

    const auto copy_frames = std::min(input.frames(), taps);
    const auto keep_frames = taps - copy_frames;
    for (std::size_t ch = 0; ch < input.channels(); ++ch)
    {
        auto *history_samples = past.channel(ch);
        std::memmove(history_samples, history_samples + copy_frames, keep_frames * sizeof(float));
        std::copy_n(input.channel(ch) + input.frames() - copy_frames, copy_frames, history_samples + keep_frames);
    }
}

} // namespace

Convert::Codec Convert::codec_for(SampleFormat format) noexcept
{
    switch (format)
    {
    case SampleFormat::S16LE:
        return {decode_pcm<SampleFormat::S16LE>, encode_pcm<SampleFormat::S16LE>};
    case SampleFormat::S24_3LE:
        return {decode_pcm<SampleFormat::S24_3LE>, encode_pcm<SampleFormat::S24_3LE>};
    case SampleFormat::S24LE:
        return {decode_pcm<SampleFormat::S24LE>, encode_pcm<SampleFormat::S24LE>};
    case SampleFormat::S32LE:
        return {decode_pcm<SampleFormat::S32LE>, encode_pcm<SampleFormat::S32LE>};
    case SampleFormat::F32LE:
        return {decode_f32le, encode_f32le};
    case SampleFormat::F32P:
        break;
    }
    return {nullptr, nullptr};
}

struct Convert::State
{
    State(SampleRate input, SampleRate output) : resampler(input, output)
    {
    }

    RetCode prepare(PortSpec input, PortSpec output, BlockPeriod period)
    {
        const bool resampling = input.sample_rate != output.sample_rate;
        if (input.format != SampleFormat::F32P)
        {
            const auto code = decoded.resize(input.channels, input.max_frames(period));
            if (!code)
                return code;
        }
        if (!resampling)
            return {};
        if (output.format != SampleFormat::F32P)
        {
            const auto code = converted.resize(output.channels, output.max_frames(period));
            if (!code)
                return code;
        }
        return resampler.prepare(input.channels);
    }

    Buffer decoded;
    Buffer converted;
    SincResampler resampler;
};

Convert::Convert(PortSpec input, PortSpec output)
    : NodeT({input}, {output}), decode_(codec_for(input.format).first), encode_(codec_for(output.format).second)
{
    if (!input.valid() || !output.valid())
        throw std::invalid_argument("Invalid converter port specification");
    if (input.channels != output.channels)
        throw std::invalid_argument("Converter input and output channel counts must match");
}

Convert::~Convert() = default;

RetCode Convert::prepare(BlockPeriod period)
{
    const auto input = inputs()[0], output = outputs()[0];
    const bool same_rate = input.sample_rate == output.sample_rate;
    if (input == output || (same_rate && (!decode_ || !encode_)))
        return {};
    auto next = std::make_unique<State>(input.sample_rate, output.sample_rate);
    const auto code = next->prepare(input, output, period);
    if (code)
        state_.swap(next);
    return code;
}

void Convert::reset() noexcept
{
    if (state_)
        state_->resampler.reset();
}

void Convert::process(const ProcessContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    const auto input_spec = input.spec();
    const auto output_spec = output.spec();
    if (input_spec == output_spec)
    {
        const auto bytes = input.byte_view();
        if (!bytes.empty())
            std::memcpy(output.byte_view().data(), bytes.data(), bytes.size());
        return;
    }

    const bool resampling = input_spec.sample_rate != output_spec.sample_rate;
    IFBlock samples;
    if (!decode_)
        samples = input.f32p_view();
    else
    {
        const auto decoded = resampling || encode_ ? state_->decoded.block(input.frames()) : output.f32p_view();
        decode_(input.byte_view().data(), decoded);
        samples = decoded;
    }
    if (resampling)
    {
        const auto converted = encode_ ? state_->converted.block(output.frames()) : output.f32p_view();
        state_->resampler.process(samples, converted, context.block_period, context.tick_position);
        samples = converted;
    }
    if (encode_)
        encode_(samples, output.byte_view().data());
}
} // namespace aereborn
