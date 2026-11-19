#include "tone.h"
#include <cmath>
#include <stdexcept>
namespace aereborn
{
Tone::Tone(SampleRate rate, std::size_t channels, double frequency, float amplitude)
    : NodeT({}, {{PortSpec(rate, channels)}}), frequency_(frequency), amplitude_(amplitude)
{
    if (!outputs()[0].valid() || !(frequency >= 0 && frequency < static_cast<std::uint32_t>(rate) / 2.0) ||
        !std::isfinite(amplitude))
        throw std::invalid_argument("Invalid tone specification");
}
void Tone::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].audio();
    const auto spec = outputs()[0];
    const auto step = 6.28318530717958647692 * frequency_ / static_cast<std::uint32_t>(spec.sample_rate);
    const auto start = spec.frame_position(context.block_period, context.tick_position);
    const auto phase = std::fmod(static_cast<double>(start) * step, 6.28318530717958647692);
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
    {
        const auto sample = amplitude_ * static_cast<float>(std::sin(phase + frame * step));
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
            output.channel(ch)[frame] = sample;
    }
}
} // namespace aereborn
