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
    if (inputs > Mixer::max_inputs)
        throw Error(ErrorCode::TooLarge);
    return spec;
}

constexpr std::uint64_t input_bit(std::size_t input) noexcept
{
    return std::uint64_t{1} << input;
}

std::uint64_t checked_bit(std::size_t input, std::size_t count)
{
    if (input >= count)
        throw Error(ErrorCode::InvalidPort);
    return input_bit(input);
}

void set_bit(std::atomic<std::uint64_t> &mask, std::uint64_t bit, bool enabled) noexcept
{
    if (enabled)
        mask.fetch_or(bit, std::memory_order_release);
    else
        mask.fetch_and(~bit, std::memory_order_release);
}

void accumulate_unity(OFBlock output, IFBlock input) noexcept
{
    const auto frames = output.frames();
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        auto *dst = output.channel(ch);
        const auto *src = input.channel(ch);
        for (std::size_t frame = 0; frame < frames; ++frame)
            dst[frame] += src[frame];
    }
}

template <bool FadeOut>
void accumulate_ramp(OFBlock output, IFBlock input, double scale) noexcept
{
    const auto last = output.frames() - 1;
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        auto *dst = output.channel(ch);
        const auto *src = input.channel(ch);
        for (std::size_t frame = 0; frame < last; ++frame)
        {
            const auto step = FadeOut ? last - frame : frame + 1;
            dst[frame] += src[frame] * static_cast<float>(step * scale);
        }
        // The final frame reaches unity on fade-in. At zero gain no source read is needed.
        if (!FadeOut)
            dst[last] += src[last];
    }
}
} // namespace

constexpr std::size_t Mixer::max_inputs;

Mixer::Mixer(SampleRate rate, std::size_t channels, std::size_t inputs)
    : output_(mixer_output(rate, channels, inputs)), inputs_(inputs, output_)
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
void Mixer::set_muted(std::size_t input, bool muted)
{
    set_bit(muted_, checked_bit(input, inputs_.size()), muted);
}

bool Mixer::muted(std::size_t input) const
{
    const auto bit = checked_bit(input, inputs_.size());
    return (muted_.load(std::memory_order_acquire) & bit) != 0;
}

void Mixer::set_high_priority(std::size_t input, bool high)
{
    set_bit(high_, checked_bit(input, inputs_.size()), high);
}

bool Mixer::high_priority(std::size_t input) const
{
    const auto bit = checked_bit(input, inputs_.size());
    return (high_.load(std::memory_order_acquire) & bit) != 0;
}

bool Mixer::suppressed(std::size_t input) const
{
    const auto bit = checked_bit(input, inputs_.size());
    return (suppressed_.load(std::memory_order_relaxed) & bit) != 0;
}

void Mixer::prepare(BlockPeriod)
{
    reset();
}

void Mixer::reset() noexcept
{
    was_suppressed_ = 0;
    suppressed_.store(0, std::memory_order_relaxed);
}

void Mixer::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;

    const auto muted = muted_.load(std::memory_order_acquire);
    const auto high = high_.load(std::memory_order_acquire);
    const auto candidates = high & ~muted;
    const auto count = inputs_.size();
    std::uint64_t suppress = 0;
    if (candidates)
    {
        const auto normal = (~std::uint64_t{0} >> (max_inputs - count)) & ~high;
        if (normal)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                if ((candidates & input_bit(i)) && context.has_input(i))
                {
                    suppress = normal;
                    break;
                }
            }
        }
    }

    const auto steady_suppressed = was_suppressed_ & suppress;
    const auto fading = was_suppressed_ ^ suppress;
    double scale = 0.0;
    output.clear();
    bool emitted = false;
    std::uint64_t suppressed = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto bit = input_bit(i);
        if (!context.has_input(i) || (muted & bit))
            continue;
        suppressed |= suppress & bit;
        if (steady_suppressed & bit)
            continue;

        emitted = true;
        const auto input = context.inputs[i].f32p_view();
        if (!(fading & bit))
        {
            accumulate_unity(output, input);
            continue;
        }
        if (scale == 0.0)
            scale = 1.0 / frames;
        if (suppress & bit)
            accumulate_ramp<true>(output, input, scale);
        else
            accumulate_ramp<false>(output, input, scale);
    }
    if (was_suppressed_ | suppress)
        suppressed_.store(suppressed, std::memory_order_relaxed);
    // Align offline normal inputs with the current suppression before they return.
    was_suppressed_ = suppress;
    context.set_output_active(0, emitted);
}
} // namespace aereborn
