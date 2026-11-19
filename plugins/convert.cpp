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

struct Convert::State
{
    State(PortSpec input, PortSpec output)
        : input(input), output(output), src_fs(static_cast<std::uint32_t>(input.sample_rate)),
          dst_fs(static_cast<std::uint32_t>(output.sample_rate))
    {
    }
    RetCode prepare(BlockPeriod period)
    {
        if (input.format != Format::F32P && (src_fs != dst_fs || output.format != Format::F32P))
        {
            const auto code = decoded.resize(input.channels, input.max_frames(period));
            if (!code)
                return code;
        }
        if (src_fs != dst_fs)
        {
            if (output.format != Format::F32P)
            {
                const auto code = converted.resize(output.channels, output.max_frames(period));
                if (!code)
                    return code;
            }
            const auto code = history.resize(input.channels, taps);
            if (!code)
                return code;
            kernel.resize((phase_steps + 1) * taps);
            const auto cutoff = std::min(1.0, static_cast<double>(dst_fs) / src_fs) * 0.98;
            for (std::uint32_t phase = 0; phase <= phase_steps; ++phase)
            {
                auto *coffs = kernel.data() + phase * taps;
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
        }
        return {};
    }
    void resample(IBlock input, OBlock output, BlockPeriod period, std::uint64_t tick) noexcept;
    const PortSpec input;
    const PortSpec output;
    const std::uint32_t src_fs;
    const std::uint32_t dst_fs;
    Buffer decoded;
    Buffer converted;
    Buffer history;
    std::vector<float> kernel;
};

void Convert::State::resample(IBlock input, OBlock output, BlockPeriod period, std::uint64_t tick) noexcept
{
    const auto past = history.block(taps);
    // src_pos is in units of 1 / dst_fs; the first taps frames address history.
    const auto rounding = [period, tick](std::uint32_t rate)
    {
        const auto remainder =
            (tick % 1000) * ((std::uint64_t{rate} * static_cast<std::uint32_t>(period)) % 1000) % 1000;
        return static_cast<std::int64_t>((1000 - remainder) % 1000);
    };
    const auto phase = (rounding(dst_fs) * src_fs - rounding(src_fs) * dst_fs) / 1000;
    auto src_pos = static_cast<std::uint64_t>(dst_fs + phase);
    Coefficients coffs;
    for (std::size_t frame = 0; frame < output.frames(); ++frame, src_pos += src_fs)
    {
        calc_coffs(kernel.data(), dst_fs, src_pos, coffs);
        const auto frame_index = static_cast<std::size_t>(src_pos / dst_fs);
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

Convert::Convert(PortSpec input, PortSpec output) : NodeT({{input}}, {{output}})
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
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (input == output)
        return {};
    auto next = std::make_unique<State>(input, output);
    const auto code = next->prepare(period);
    if (code)
        state_.swap(next);
    return code;
}
void Convert::reset() noexcept
{
    if (state_)
        state_->history.clear();
}
void Convert::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    if (input.spec() == output.spec())
    {
        std::memcpy(output.bytes().data(), input.bytes().data(), input.bytes().size());
        return;
    }
    auto &state = *state_;
    IBlock samples;
    if (state.input.format == Format::F32P)
        samples = input.audio();
    else
    {
        const auto decoded = state.output.format == Format::F32P && state.src_fs == state.dst_fs
                                 ? output.audio()
                                 : state.decoded.block(input.frames());
        const auto *source = input.bytes().data();
        if (state.input.format == Format::F32LE)
            copy_f32le(source, decoded);
        else
        {
            const auto width = sample_bytes(state.input.format);
            for (std::size_t frame = 0; frame < input.frames(); ++frame)
                for (std::size_t ch = 0; ch < state.input.channels; ++ch, source += width)
                    decoded.channel(ch)[frame] =
                        decode_integer(read_le(source, width), sample_bits(state.input.format));
        }
        samples = decoded;
    }
    if (state.src_fs != state.dst_fs)
    {
        const auto converted =
            state.output.format == Format::F32P ? output.audio() : state.converted.block(output.frames());
        state.resample(samples, converted, context.block_period, context.tick_position);
        samples = converted;
    }
    if (state.output.format == Format::F32P)
        return;
    auto *destination = output.bytes().data();
    if (state.output.format == Format::F32LE)
        copy_f32le(samples, destination);
    else
    {
        const auto width = sample_bytes(state.output.format);
        for (std::size_t frame = 0; frame < samples.frames(); ++frame)
            for (std::size_t ch = 0; ch < state.output.channels; ++ch, destination += width)
                write_le(encode_integer(samples.channel(ch)[frame], sample_bits(state.output.format)), destination,
                         width);
    }
}
} // namespace aereborn
