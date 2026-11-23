#include "convert.h"

#include <cstring>

namespace aereborn
{

Convert::Convert(PortSpec input, PortSpec output)
    : NodeT({input}, {output}), same_spec_(input == output)
{
    if (!input.valid() || !output.valid() || input.channels != output.channels)
        throw Error(ErrorCode::InvalidArg, "Invalid converter port specifications");
}

void Convert::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        throw Error(ErrorCode::InvalidArg);
    if (same_spec_)
        return;
    const auto input = inputs()[0], output = outputs()[0];
    const auto in_pcm = is_interleaved_pcm(input.format), out_pcm = is_interleaved_pcm(output.format);
    const auto fir = input.sample_rate != output.sample_rate;
    Buffer decoded, resampled;
    if (in_pcm && (fir || out_pcm))
        decoded.resize(input.channels, input.max_frames(period));
    if (fir)
    {
        if (out_pcm)
            resampled.resize(output.channels, output.max_frames(period));
        resampler_.prepare(input.channels, input.sample_rate, output.sample_rate);
    }
    decoded_ = std::move(decoded);
    resampled_ = std::move(resampled);
}

void Convert::process(const ProcessContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    if (same_spec_)
    {
        // The byte copy preserves float bit patterns and PCM padding.
        const auto bytes = input.byte_view();
        if (!bytes.empty())
            std::memcpy(output.byte_view().data(), bytes.data(), bytes.size());
        return;
    }

    const auto in_pcm = is_interleaved_pcm(input.spec().format);
    const auto out_pcm = is_interleaved_pcm(output.spec().format);
    const auto fir = resampler_.needs_fir();
    IFBlock source;
    if (in_pcm)
    {
        const auto decoded = fir || out_pcm ? decoded_.block(input.frames()) : output.f32p_view();
        decode_pcm(input, decoded);
        source = decoded;
    }
    else
        source = input.f32p_view();

    if (fir)
    {
        const auto dest = out_pcm ? resampled_.block(output.frames()) : output.f32p_view();
        resampler_.process_tick(source, dest, context.block_period, context.tick_position);
        source = dest;
    }
    if (out_pcm)
        encode_pcm(source, output);
}

void Convert::reset() noexcept
{
    resampler_.reset();
}

} // namespace aereborn
