#include "mixer.h"
#include <stdexcept>
namespace aereborn
{
namespace
{
PortSpec mixer_output(SampleRate rate, std::size_t channels, std::size_t inputs)
{
    const PortSpec spec(rate, channels, SampleFormat::F32P);
    if (!spec.valid() || inputs == 0)
        throw std::invalid_argument("Mixer needs valid ports and at least one input");
    return spec;
}
} // namespace
Mixer::Mixer(SampleRate rate, std::size_t channels, std::size_t inputs)
    : output_(mixer_output(rate, channels, inputs)), inputs_(inputs, output_)
{
    for (std::size_t i = 0; i < inputs; ++i)
        muted_.emplace_back(false);
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
Result<bool> Mixer::muted(std::size_t input) const noexcept
{
    if (input >= inputs_.size())
        return ErrorCode::InvalidPort;
    return muted_[input].load(std::memory_order_acquire);
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
        if (muted_[i].load(std::memory_order_acquire))
            continue;
        const auto input = context.inputs[i].f32p_view();
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            auto *dst = output.channel(ch);
            const auto *src = input.channel(ch);
            for (std::size_t frame = 0; frame < frames; ++frame)
                dst[frame] += src[frame];
        }
    }
}
} // namespace aereborn
