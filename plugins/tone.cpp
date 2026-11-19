#include "tone.h"
#include <cmath>
namespace aereborn
{
Tone::Tone(SampleRate rate, std::size_t channels, double frequency, float amplitude)
    : NodeT({}, {{PortSpec(rate, channels)}}), frequency_(frequency), amplitude_(amplitude)
{
}
RetCode Tone::prepare(BlockPeriod period)
{
    const auto rate = static_cast<std::uint32_t>(outputs()[0].sample_rate);
    if (!is_block_period(period) || !outputs()[0].valid() || !(frequency_ >= 0 && frequency_ < rate / 2.0) ||
        !std::isfinite(amplitude_))
        return ErrorCode::InvalidArg;
    period_ = period;
    return {};
}
void Tone::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].audio();
    const auto spec = outputs()[0];
    const auto step = 6.28318530717958647692 * frequency_ / static_cast<std::uint32_t>(spec.sample_rate);
    const auto start = spec.frame_position(period_, context.tick_position);
    const auto phase = std::fmod(static_cast<double>(start) * step, 6.28318530717958647692);
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
    {
        const auto sample = amplitude_ * static_cast<float>(std::sin(phase + frame * step));
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
            output.channel(ch)[frame] = sample;
    }
}
} // namespace aereborn
