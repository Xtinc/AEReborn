#include "tone.h"
#include <cmath>
#include <cstring>
namespace aereborn
{
Tone::Tone(BandWidth rate, std::size_t channels, double frequency, float amplitude)
    : NodeT({}, {PortSpec(rate, channels, SampleFormat::F32P)}), frequency_(frequency), amplitude_(amplitude)
{
    if (!outputs()[0].valid() || !(frequency >= 0 && frequency < static_cast<std::uint32_t>(rate) / 2.0) ||
        !std::isfinite(amplitude))
        throw Error(RetCode::EPARAM, "Invalid tone specification");
}
void Tone::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    const auto spec = outputs()[0];
    const auto step = 6.28318530717958647692 * frequency_ / static_cast<std::uint32_t>(spec.bandwidth);
    const auto start = spec.frame_position(context.block_period, context.tick_position);
    const auto phase = std::fmod(static_cast<double>(start) * step, 6.28318530717958647692);
    auto *first = output.channel(0);
    for (std::size_t frame = 0; frame < frames; ++frame)
        first[frame] = amplitude_ * static_cast<float>(std::sin(phase + frame * step));
    for (std::size_t ch = 1; ch < output.channels(); ++ch)
        std::memcpy(output.channel(ch), first, frames * sizeof(float));
}
} // namespace aereborn
