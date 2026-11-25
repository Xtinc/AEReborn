#include "mixer.h"
namespace aereborn
{
namespace
{
PortSpec mixer_output(SampleRate rate, std::size_t channels, std::size_t inputs)
{
    const PortSpec spec(rate, channels, SampleFormat::F32P);
    if (!spec.valid() || inputs == 0)
        throw Error(ErrorCode::InvalidArg, "Mixer needs valid ports and at least one input");
    if (inputs > std::vector<PortSpec>{}.max_size())
        throw Error(ErrorCode::TooLarge);
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
void Mixer::set_muted(std::size_t input, bool muted)
{
    if (input >= inputs_.size())
        throw Error(ErrorCode::InvalidPort);
    muted_[input].store(muted, std::memory_order_release);
}
bool Mixer::muted(std::size_t input) const
{
    if (input >= inputs_.size())
        throw Error(ErrorCode::InvalidPort);
    return muted_[input].load(std::memory_order_acquire);
}
void Mixer::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    output.clear();
    bool active = false;
    for (std::size_t i = 0; i < inputs_.size(); ++i)
    {
        if (muted_[i].load(std::memory_order_acquire) || !context.has_input(i))
            continue;
        active = true;
        const auto input = context.inputs[i].f32p_view();
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            auto *dst = output.channel(ch);
            const auto *src = input.channel(ch);
            for (std::size_t frame = 0; frame < frames; ++frame)
                dst[frame] += src[frame];
        }
    }
    context.set_output_active(0, active);
}
} // namespace aereborn
