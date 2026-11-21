#include "convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
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

RetCode prepare_filter(Buffer &history, std::vector<float> &kernel, std::size_t channels, double cutoff)
{
    const auto code = history.resize(channels, taps);
    if (!code)
        return code;
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
    return {};
}

void filter_frame(OFBlock past, const std::vector<float> &kernel, IFBlock input, OFBlock output,
                  std::size_t out_frame, std::size_t through, std::size_t phase_index, float phase_fraction) noexcept
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

template <class Byte> bool matches(ByteBlock<Byte> block, PortSpec spec) noexcept
{
    return block.spec() == spec;
}

template <class Float> bool matches(FloatBlock<Float> block, PortSpec spec) noexcept
{
    return spec.format == SampleFormat::F32P && block.channels() == spec.channels;
}

template <class Byte> bool valid_block(ByteBlock<Byte> block) noexcept
{
    return !block.frames() || block.byte_view().data() != nullptr;
}

template <class Float> bool valid_block(FloatBlock<Float> block) noexcept
{
    return block.valid();
}

constexpr auto frame_limit = std::numeric_limits<std::size_t>::max() / 16;

bool valid_offset(double offset) noexcept
{
    return offset >= 0.0 && offset < 0.5;
}

bool needs_fir(PortSpec input, PortSpec output, double offset) noexcept
{
    return input.sample_rate != output.sample_rate || offset > 0;
}

double nominal_step(PortSpec input, PortSpec output) noexcept
{
    return static_cast<double>(static_cast<std::uint32_t>(input.sample_rate)) /
           static_cast<std::uint32_t>(output.sample_rate);
}

Result<std::size_t> workspace_bound(PortSpec input, PortSpec output, std::size_t frames, double offset,
                                    bool bound_input) noexcept
{
    if (!input.valid() || !output.valid() || !valid_offset(offset))
        return ErrorCode::InvalidArg;
    if (input.channels != output.channels)
        return ErrorCode::FormatMismatch;
    if (frames > frame_limit)
        return ErrorCode::TooLarge;
    if (!frames || !needs_fir(input, output, offset))
        return frames;

    const auto nominal = nominal_step(input, output);
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
        return ErrorCode::TooLarge;
    return static_cast<std::size_t>(rounded) + 1;
}
} // namespace

template <class Byte, class Float> struct Convert::NativeBlock
{
    NativeBlock(ByteBlock<Byte> block) noexcept : frames(block.frames())
    {
        if (block.spec().format == SampleFormat::F32P)
            planar = block.f32p_view();
        else
            pcm = block.byte_view().data();
    }

    NativeBlock(FloatBlock<Float> block) noexcept : planar(block), frames(block.frames()) {}

    FloatBlock<Float> f32p_view() const noexcept { return planar.subblock(0, frames); }

    FloatBlock<Float> planar;
    std::size_t frames;
    Byte *pcm = nullptr;
};

void Convert::Position::advance(std::uint64_t step, std::uint64_t unit) noexcept
{
    const auto next = fraction + step;
    through += static_cast<std::size_t>(next / unit);
    fraction = next % unit;
}

Convert::Position Convert::tick_position(std::uint32_t src_fs, std::uint32_t dst_fs, const ProcessContext &context) noexcept
{
    // Express the difference between the two rounded block starts in 1 / dst_fs units.
    const auto rounding = [&context](std::uint32_t rate) {
        const auto period = static_cast<std::uint32_t>(context.block_period);
        const auto rem = (context.tick_position % 1000) * ((std::uint64_t{rate} * period) % 1000) % 1000;
        return static_cast<std::int64_t>((1000 - rem) % 1000);
    };
    const auto phase = (rounding(dst_fs) * src_fs - rounding(src_fs) * dst_fs) / 1000;
    const auto start = static_cast<std::uint64_t>(dst_fs + phase);
    return {static_cast<std::size_t>(start / dst_fs), start % dst_fs};
}

RetCode Convert::check_output(std::size_t frames, double scale) const noexcept
{
    if (!max_output_frames_)
        return ErrorCode::NotPrepared;
    if (!(scale >= 1 - max_rate_offset_ && scale <= 1 + max_rate_offset_))
        return ErrorCode::InvalidArg;
    return frames <= max_output_frames_ ? RetCode{} : RetCode{ErrorCode::TooLarge};
}

Result<std::size_t> Convert::required_input(std::size_t frames, double scale) const noexcept
{
    const auto code = check_output(frames, scale);
    if (!code)
        return code;
    if (!frames)
        return std::size_t{0};

    auto needed = static_cast<std::uint64_t>(frames);
    if (need_fir_)
    {
        const auto step = step_units(nominal_ * scale);
        const auto whole = step >> fraction_bits;
        const auto count = static_cast<std::uint64_t>(frames - 1);
        // Up to 18 source frames per output are possible at the allowed offset limit.
        if (whole && count > max_input_frames_ / whole)
            return ErrorCode::TooLarge;
        const auto frac = std::uint64_t{static_cast<std::uint32_t>(step)};
        const auto low = position_.fraction + frac * static_cast<std::uint32_t>(count);
        needed = position_.through + count * whole + (count >> fraction_bits) * frac + (low >> fraction_bits);
    }
    if (needed > max_input_frames_)
        return ErrorCode::TooLarge;
    return static_cast<std::size_t>(needed);
}

Convert::Transfer Convert::run(NativeBlock<const std::uint8_t, const float> input, NativeBlock<std::uint8_t, float> output,
             double scale, const ProcessContext *tick) noexcept
{
    if (!need_fir_)
    {
        input.frames = output.frames = std::min(input.frames, output.frames);
        if (!input.frames)
            return {};
        if (same_spec_)
        {
            if (encode_)
                std::memcpy(output.pcm, input.pcm, input.frames * inputs()[0].channels * sample_bytes(inputs()[0].format));
            else
                for (std::size_t ch = 0; ch < input.planar.channels(); ++ch)
                    std::memcpy(output.planar.channel(ch), input.planar.channel(ch), input.frames * sizeof(float));
            return {input.frames, output.frames};
        }
    }
    else if (!output.frames && !tick)
        return {};

    IFBlock samples;
    if (decode_)
    {
        const auto workspace = need_fir_ || encode_ ? workspace_.decoded.block(input.frames) : output.f32p_view();
        decode_(input.pcm, workspace);
        samples = workspace;
    }
    else
        samples = input.f32p_view();

    if (!need_fir_)
    {
        if (encode_)
            encode_(samples, output.pcm);
        return {input.frames, output.frames};
    }

    Position phase;
    std::uint64_t step, unit;
    if (tick)
    {
        const auto src_fs = static_cast<std::uint32_t>(tick->inputs[0].spec().sample_rate);
        const auto dst_fs = static_cast<std::uint32_t>(tick->outputs[0].spec().sample_rate);
        phase = tick_position(src_fs, dst_fs, *tick);
        step = src_fs;
        unit = dst_fs;
    }
    else
    {
        phase = position_;
        step = step_units(nominal_ * scale);
        unit = phase_unit;
    }

    const auto dest = encode_ ? workspace_.converted.block(output.frames) : output.f32p_view();
    const auto past = workspace_.history.block(taps);
    Transfer result;
    std::size_t last = 0;
    while (result.produced < output.frames && phase.through <= input.frames)
    {
        last = phase.through;
        const auto phase_pos = phase.fraction * phase_steps;
        const auto index = static_cast<std::size_t>(phase_pos / unit);
        const auto blend = static_cast<float>(phase_pos % unit) / static_cast<float>(unit);
        filter_frame(past, workspace_.kernel, samples, dest, result.produced, last, index, blend);
        ++result.produced;
        phase.advance(step, unit);
    }

    // The graph supplies a new block every tick. A stream keeps unconsumed input with the caller.
    result.consumed = input.frames;
    if (!tick)
    {
        const auto skipped = phase.through ? phase.through - 1 : 0;
        result.consumed = std::min(input.frames, std::max(last, skipped));
        position_ = {phase.through - result.consumed, phase.fraction};
    }
    remember(workspace_.history, samples, result.consumed);
    if (encode_ && result.produced)
        encode_(workspace_.converted.block(result.produced), output.pcm);
    return result;
}

Convert::Convert(PortSpec input, PortSpec output) : NodeT({input}, {output})
{
    if (!input.valid() || !output.valid())
        throw std::invalid_argument("Invalid converter port specification");
    if (input.channels != output.channels)
        throw std::invalid_argument("Converter input and output channel counts must match");
    decode_ = codec_for(input.format).first;
    encode_ = codec_for(output.format).second;
    same_spec_ = input == output;
    nominal_ = nominal_step(input, output);
}

RetCode Convert::prepare(std::size_t max_in, std::size_t max_out, double offset)
{
    if (!max_in || !max_out || !valid_offset(offset))
        return ErrorCode::InvalidArg;
    const auto input = inputs()[0], output = outputs()[0];
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (max_in > frame_limit || max_out > frame_limit || max_in > limit / sample_bytes(input.format) / input.channels ||
        max_out > limit / sample_bytes(output.format) / output.channels)
        return ErrorCode::TooLarge;
    if (offset != max_rate_offset_ || max_in > alloc_input_frames_ || max_out > alloc_output_frames_)
    {
        const auto fir = needs_fir(input, output, offset);
        Workspace next;
        if (decode_ && (fir || (encode_ && !same_spec_)))
        {
            const auto code = next.decoded.resize(input.channels, max_in);
            if (!code)
                return code;
        }
        if (fir)
        {
            if (encode_)
            {
                const auto code = next.converted.resize(output.channels, max_out);
                if (!code)
                    return code;
            }
            const auto cutoff = static_cast<double>(static_cast<std::uint32_t>(output.sample_rate)) /
                                static_cast<std::uint32_t>(input.sample_rate) / (1 + offset);
            const auto code = prepare_filter(next.history, next.kernel, input.channels, std::min(1.0, cutoff) * 0.98);
            if (!code)
                return code;
        }
        workspace_ = std::move(next);
        need_fir_ = fir;
        max_rate_offset_ = offset;
        alloc_input_frames_ = max_in;
        alloc_output_frames_ = max_out;
    }
    max_input_frames_ = max_in;
    max_output_frames_ = max_out;
    reset();
    return {};
}

RetCode Convert::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    return prepare(inputs()[0].max_frames(period), outputs()[0].max_frames(period));
}

void Convert::reset() noexcept
{
    position_ = {};
    workspace_.history.clear();
}

void Convert::process(const ProcessContext &context) noexcept
{
    const auto transfer = run(context.inputs[0], context.outputs[0], 1, &context);
    assert(transfer.produced == context.outputs[0].frames());
    (void)transfer;
}

template <class Input, class Output>
Result<Convert::Transfer> Convert::process_blocks(Input input, Output output, double scale) noexcept
{
    const auto code = check_output(output.frames(), scale);
    if (!code)
        return code;
    if (!matches(input, inputs()[0]) || !matches(output, outputs()[0]))
        return ErrorCode::FormatMismatch;
    if (input.frames() > max_input_frames_)
        return ErrorCode::TooLarge;
    if (!valid_block(input) || !valid_block(output))
        return ErrorCode::InvalidArg;
    return run(input, output, scale);
}

Result<Convert::Transfer> Convert::process(IBBlock input, OBBlock output, double scale) noexcept
{
    return process_blocks(input, output, scale);
}

Result<Convert::Transfer> Convert::process(IBBlock input, OFBlock output, double scale) noexcept
{
    return process_blocks(input, output, scale);
}

Result<Convert::Transfer> Convert::process(IFBlock input, OBBlock output, double scale) noexcept
{
    return process_blocks(input, output, scale);
}

Result<Convert::Transfer> Convert::process(IFBlock input, OFBlock output, double scale) noexcept
{
    return process_blocks(input, output, scale);
}

Result<std::size_t> Convert::max_output_frames(PortSpec input, PortSpec output, std::size_t frames,
                                               double offset) noexcept
{
    return workspace_bound(input, output, frames, offset, false);
}

Result<std::size_t> Convert::max_input_frames(PortSpec input, PortSpec output, std::size_t frames,
                                              double offset) noexcept
{
    return workspace_bound(input, output, frames, offset, true);
}

} // namespace aereborn
