#include "mixer.h"
#include <stdexcept>
namespace aereborn
{
namespace
{
PortSpec mixer_output(SampleRate rate, std::size_t channels, std::size_t inputs)
{
    const PortSpec spec(rate, channels);
    if (!spec.valid() || inputs == 0)
        throw std::invalid_argument("Mixer needs valid ports and at least one input");
    return spec;
}
} // namespace
Mixer::Mixer(SampleRate rate, std::size_t channels, std::size_t inputs)
    : output_(mixer_output(rate, channels, inputs)), inputs_(inputs, output_), states_(inputs)
{
}
View<const PortSpec> Mixer::inputs() const noexcept
{
    return {inputs_.data(), inputs_.size()};
}
View<const PortSpec> Mixer::outputs() const noexcept
{
    return {&output_, 1};
}
RetCode Mixer::set_muted(std::size_t input, bool muted) noexcept
{
    if (input >= states_.size())
        return ErrorCode::InvalidPort;
    states_[input].target.store(muted, std::memory_order_release);
    return {};
}
RetCode Mixer::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (!states_.front().target.is_lock_free())
        return ErrorCode::Unsupported;
    reset();
    return {};
}
void Mixer::reset() noexcept
{
    for (auto &state : states_)
        state.muted = state.target.load(std::memory_order_acquire);
}
void Mixer::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].audio();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    output.clear();
    for (std::size_t i = 0; i < states_.size(); ++i)
    {
        auto &state = states_[i];
        const bool target = state.target.load(std::memory_order_acquire);
        const bool ramping = state.muted != target;
        if (target && !ramping)
            continue;
        const auto input = context.inputs[i].audio();
        const auto gain = state.muted ? 0.0 : 1.0;
        const auto step = ramping ? (target ? -1.0 : 1.0) / frames : 0.0;
        const auto last = frames - 1;
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            auto *dst = output.channel(ch);
            const auto *src = input.channel(ch);
            if (ramping)
            {
                for (std::size_t frame = 0; frame < last; ++frame)
                    dst[frame] += src[frame] * static_cast<float>(gain + step * (frame + 1));
                if (!target)
                    dst[last] += src[last];
            }
            else
            {
                for (std::size_t frame = 0; frame < frames; ++frame)
                    dst[frame] += src[frame];
            }
        }
        state.muted = target;
    }
}
} // namespace aereborn
