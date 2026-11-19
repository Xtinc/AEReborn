#include "mixer.h"
#include <stdexcept>
namespace aereborn
{
Mixer::Mixer(SampleRate rate, std::size_t channels, std::size_t inputs)
    : output_(rate, channels), inputs_(inputs, output_), muted_(new std::atomic<bool>[inputs]), gains_(inputs, 1)
{
    if (!output_.valid() || inputs == 0)
        throw std::invalid_argument("Mixer needs valid ports and at least one input");
    for (std::size_t i = 0; i < inputs; ++i)
        muted_[i].store(false, std::memory_order_relaxed);
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
    muted_[input].store(muted, std::memory_order_release);
    return {};
}
RetCode Mixer::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (!muted_[0].is_lock_free())
        return ErrorCode::Unsupported;
    reset();
    return {};
}
void Mixer::reset() noexcept
{
    for (std::size_t i = 0; i < inputs_.size(); ++i)
        gains_[i] = muted_[i].load(std::memory_order_acquire) ? 0.0f : 1.0f;
}
void Mixer::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].audio();
    output.clear();
    for (std::size_t i = 0; i < inputs_.size(); ++i)
    {
        const auto input = context.inputs[i].audio();
        const auto target = muted_[i].load(std::memory_order_acquire) ? 0.0f : 1.0f;
        const auto step = output.frames() ? static_cast<double>(target - gains_[i]) / output.frames() : 0;
        for (std::size_t frame = 0; frame < output.frames(); ++frame)
        {
            const auto gain =
                frame + 1 == output.frames() ? target : static_cast<float>(gains_[i] + step * (frame + 1));
            if (gain == 0)
                continue;
            for (std::size_t ch = 0; ch < output.channels(); ++ch)
                output.channel(ch)[frame] += input.channel(ch)[frame] * gain;
        }
        gains_[i] = target;
    }
}
} // namespace aereborn
