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
    : output_(mixer_output(rate, channels, inputs)), inputs_(inputs, output_),
      targets_(std::make_unique<std::atomic<bool>[]>(inputs)), muted_(inputs)
{
    for (std::size_t i = 0; i < inputs; ++i)
        std::atomic_init(&targets_[i], false);
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
    if (input >= inputs_.size())
        return ErrorCode::InvalidPort;
    targets_[input].store(muted, std::memory_order_release);
    return {};
}
RetCode Mixer::prepare(BlockPeriod)
{
    reset();
    return {};
}
void Mixer::reset() noexcept
{
    for (std::size_t i = 0; i < inputs_.size(); ++i)
        muted_[i] = targets_[i].load(std::memory_order_acquire);
}
void Mixer::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    output.clear();
    for (std::size_t i = 0; i < inputs_.size(); ++i)
    {
        const bool target = targets_[i].load(std::memory_order_acquire);
        const bool ramping = muted_[i] != target;
        if (target && !ramping)
            continue;
        const auto input = context.inputs[i].f32p_view();
        const auto gain = muted_[i] ? 0.0 : 1.0;
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
        muted_[i] = target;
    }
}
} // namespace aereborn
