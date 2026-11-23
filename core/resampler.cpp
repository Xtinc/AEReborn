#include "resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
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
constexpr unsigned fraction_bits = 32;
constexpr auto phase_unit = std::uint64_t{1} << fraction_bits;
constexpr double pi = 3.141592653589793;

std::uint64_t step_units(double step) noexcept
{
    return static_cast<std::uint64_t>(step * phase_unit + 0.5);
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

void calc_coffs(const float *kernel, std::size_t phase_index, float phase_frac, Coefficients &coffs) noexcept
{
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

void prepare_filter(Buffer &history, std::vector<float> &kernel, std::size_t channels, double cutoff)
{
    history.resize(channels, taps);
    kernel.resize((phase_steps + 1) * taps);
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

void filter_frame(OFBlock past, const std::vector<float> &kernel, IFBlock input, OFBlock output, std::size_t out_frame,
                  std::size_t through, std::size_t phase_index, float phase_fraction) noexcept
{
    const auto history_offset = std::min(through, taps);
    const auto input_offset = through - history_offset;
    Coefficients coffs;
    calc_coffs(kernel.data(), phase_index, phase_fraction, coffs);
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        const auto *samples = through ? input.channel(ch) + input_offset : nullptr;
        output.channel(ch)[out_frame] =
            interpolate(past.channel(ch) + history_offset, samples, taps - history_offset, coffs);
    }
}

void remember(Buffer &history, IFBlock input, std::size_t consumed) noexcept
{
    if (!consumed)
        return;
    const auto past = history.block(taps);
    const auto copy_frames = std::min(consumed, taps);
    const auto keep_frames = taps - copy_frames;
    for (std::size_t ch = 0; ch < past.channels(); ++ch)
    {
        auto *history_samples = past.channel(ch);
        std::memmove(history_samples, history_samples + copy_frames, keep_frames * sizeof(float));
        std::copy_n(input.channel(ch) + consumed - copy_frames, copy_frames, history_samples + keep_frames);
    }
}

using DecodeFn = void (*)(const std::uint8_t *, OFBlock);
using EncodeFn = void (*)(IFBlock, std::uint8_t *);

std::pair<DecodeFn, EncodeFn> codec_for(SampleFormat format) noexcept
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

constexpr auto frame_limit = std::numeric_limits<std::size_t>::max() / 16;

bool valid_offset(double offset) noexcept
{
    return offset >= 0.0 && offset < 0.5;
}

bool needs_filter(SampleRate src_fs, SampleRate dst_fs, double offset) noexcept
{
    return src_fs != dst_fs || offset > 0;
}

double nominal_step(SampleRate src_fs, SampleRate dst_fs) noexcept
{
    return static_cast<double>(static_cast<std::uint32_t>(src_fs)) / static_cast<std::uint32_t>(dst_fs);
}

std::size_t workspace_bound(SampleRate src_fs, SampleRate dst_fs, std::size_t frames, double offset,
                            bool bound_input)
{
    if (!is_sample_rate(src_fs) || !is_sample_rate(dst_fs) || !valid_offset(offset))
        throw Error(ErrorCode::InvalidArg);
    if (frames > frame_limit)
        throw Error(ErrorCode::TooLarge);
    if (!frames || !needs_filter(src_fs, dst_fs, offset))
        return frames;

    const auto nominal = nominal_step(src_fs, dst_fs);
    const auto step = nominal * (bound_input ? 1 + offset : 1 - offset);
    const auto fixed = static_cast<long double>(step_units(step)) / phase_unit;
    // Cover both the nominal rate envelope and the actual quantized stream step.
    const auto edge = bound_input ? std::max<long double>(step, fixed) : std::min<long double>(step, fixed);
    const auto ratio = bound_input ? edge : 1 / edge;
    // After a capped upsampling call the next output may use the last source frame in history.
    const auto history = bound_input ? 0.0L : std::max(0.0L, 1 - edge);
    const auto count = static_cast<long double>(frames) + history;
    auto scaled = count * ratio;
    // For exceptionally large bounds, round outward even when long double has double precision.
    const auto precision_limit = std::ldexp(1.0L, std::numeric_limits<long double>::digits - 3);
    if (count >= precision_limit || scaled >= precision_limit)
    {
        const auto infinity = std::numeric_limits<long double>::infinity();
        scaled = std::nextafter(std::nextafter(count, infinity) * std::nextafter(ratio, infinity), infinity);
    }
    const auto rounded = std::ceil(scaled);
    if (rounded >= static_cast<long double>(frame_limit))
        throw Error(ErrorCode::TooLarge);
    return static_cast<std::size_t>(rounded) + 1;
}
} // namespace

void decode_pcm(IBBlock input, OFBlock output) noexcept
{
    const auto codec = codec_for(input.spec().format).first;
    assert(codec && input.spec().channels == output.channels() && input.frames() == output.frames());
    codec(input.byte_view().data(), output);
}

void encode_pcm(IFBlock input, OBBlock output) noexcept
{
    const auto codec = codec_for(output.spec().format).second;
    assert(codec && output.spec().channels == input.channels() && output.frames() == input.frames());
    codec(input, output.byte_view().data());
}

void SincResampler::Position::advance(std::uint64_t step, std::uint64_t unit) noexcept
{
    const auto next = fraction + step;
    through += static_cast<std::size_t>(next / unit);
    fraction = next % unit;
}

void SincResampler::prepare(std::size_t channels, SampleRate src_fs, SampleRate dst_fs, double offset)
{
    if (!channels || !is_sample_rate(src_fs) || !is_sample_rate(dst_fs) || !valid_offset(offset))
        throw Error(ErrorCode::InvalidArg);

    Buffer history;
    std::vector<float> kernel;
    const auto fir = needs_filter(src_fs, dst_fs, offset);
    const auto nominal = nominal_step(src_fs, dst_fs);
    if (fir)
    {
        const auto cutoff =
            static_cast<double>(static_cast<std::uint32_t>(dst_fs)) / static_cast<std::uint32_t>(src_fs) / (1 + offset);
        prepare_filter(history, kernel, channels, std::min(1.0, cutoff) * 0.98);
    }
    history_ = std::move(history);
    kernel_ = std::move(kernel);
    need_fir_ = fir;
    channels_ = channels;
    src_fs_ = src_fs;
    dst_fs_ = dst_fs;
    nominal_ = nominal;
    max_rate_offset_ = offset;
    position_ = {};
}

std::size_t SincResampler::tail_frames() const noexcept
{
    return need_fir_ ? taps : 0;
}

void SincResampler::reset() noexcept
{
    position_ = {};
    history_.clear();
}

void SincResampler::process_tick(IFBlock input, OFBlock output, BlockPeriod period, std::uint64_t tick) noexcept
{
    assert(channels_ && max_rate_offset_ == 0 && input.channels() == channels_ && output.channels() == channels_);
    const auto src_fs = static_cast<std::uint32_t>(src_fs_);
    const auto dst_fs = static_cast<std::uint32_t>(dst_fs_);
    const auto rounding = [period, tick](std::uint32_t rate) {
        const auto rem = (tick % 1000) * ((std::uint64_t{rate} * static_cast<std::uint32_t>(period)) % 1000) % 1000;
        return static_cast<std::int64_t>((1000 - rem) % 1000);
    };
    const auto phase = (rounding(dst_fs) * src_fs - rounding(src_fs) * dst_fs) / 1000;
    const auto start = static_cast<std::uint64_t>(dst_fs + phase);
    Position position{static_cast<std::size_t>(start / dst_fs), start % dst_fs};
    const auto result = run(input, output, position, src_fs, dst_fs, true);
    assert(result.produced == output.frames());
    (void)result;
}

std::size_t SincResampler::required_input(std::size_t frames, double scale) const noexcept
{
    assert(channels_ && scale >= 1 - max_rate_offset_ && scale <= 1 + max_rate_offset_);
    assert(frames <= frame_limit);
    if (!frames || !need_fir_)
        return frames;

    const auto step = step_units(nominal_ * scale);
    const auto whole = step >> fraction_bits;
    const auto count = static_cast<std::uint64_t>(frames - 1);
    assert(!whole || count <= frame_limit / whole);
    const auto frac = std::uint64_t{static_cast<std::uint32_t>(step)};
    const auto low = position_.fraction + frac * static_cast<std::uint32_t>(count);
    const auto needed = position_.through + count * whole + (count >> fraction_bits) * frac + (low >> fraction_bits);
    assert(needed <= frame_limit);
    return static_cast<std::size_t>(needed);
}

SincResampler::Transfer SincResampler::process(IFBlock input, OFBlock output, double scale) noexcept
{
    assert(channels_ && scale >= 1 - max_rate_offset_ && scale <= 1 + max_rate_offset_);
    assert(input.channels() == channels_ && output.channels() == channels_);
    assert(input.frames() <= frame_limit && output.frames() <= frame_limit);
    assert(input.valid() && output.valid());
    return run(input, output, position_, step_units(nominal_ * scale), phase_unit, false);
}

SincResampler::Transfer SincResampler::run(IFBlock input, OFBlock output, Position &phase, std::uint64_t step,
                                           std::uint64_t unit, bool tick) noexcept
{
    if (!need_fir_)
    {
        const auto frames = std::min(input.frames(), output.frames());
        if (frames)
            for (std::size_t ch = 0; ch < channels_; ++ch)
                std::memcpy(output.channel(ch), input.channel(ch), frames * sizeof(float));
        return {frames, frames};
    }
    if (!output.frames() && !tick)
        return {};

    const auto past = history_.block(taps);
    Transfer result;
    std::size_t last = 0;
    while (result.produced < output.frames() && phase.through <= input.frames())
    {
        last = phase.through;
        const auto phase_pos = phase.fraction * phase_steps;
        const auto index = static_cast<std::size_t>(phase_pos / unit);
        const auto blend = static_cast<float>(phase_pos % unit) / static_cast<float>(unit);
        filter_frame(past, kernel_, input, output, result.produced, last, index, blend);
        ++result.produced;
        phase.advance(step, unit);
    }

    // The graph supplies a new block every tick. A stream keeps unconsumed input with the caller.
    result.consumed = input.frames();
    if (!tick)
    {
        const auto skipped = phase.through ? phase.through - 1 : 0;
        result.consumed = std::min(input.frames(), std::max(last, skipped));
        phase.through -= result.consumed;
    }
    remember(history_, input, result.consumed);
    return result;
}

std::size_t SincResampler::max_output_frames(SampleRate src_fs, SampleRate dst_fs, std::size_t frames,
                                             double offset)
{
    return workspace_bound(src_fs, dst_fs, frames, offset, false);
}

std::size_t SincResampler::max_input_frames(SampleRate src_fs, SampleRate dst_fs, std::size_t frames,
                                            double offset)
{
    return workspace_bound(src_fs, dst_fs, frames, offset, true);
}

} // namespace aereborn
