#include "matrix.h"
#include <cmath>
#include <cstring>
namespace aereborn
{
namespace
{
template <bool Accumulate> void write_scaled(float &dst, const float &src, float gain) noexcept
{
    if (Accumulate)
        dst += src * gain;
    else
        dst = src * gain;
}

template <bool Accumulate> void apply_steady_gain(float *dst, const float *src, std::size_t frames, float gain) noexcept
{
    if (Accumulate && gain == 0)
        return;
    for (std::size_t frame = 0; frame < frames; ++frame)
        write_scaled<Accumulate>(dst[frame], src[frame], gain);
}

template <bool Accumulate> void write_ramp_sample(float &dst, const float &src, float gain) noexcept
{
    if (gain != 0)
        write_scaled<Accumulate>(dst, src, gain);
    else if (!Accumulate)
        dst = 0.0f;
}

template <bool Accumulate>
void apply_ramping_gain(float *dst, const float *src, std::size_t frames, float coefficient, float start, float target,
                        double scale) noexcept
{
    const auto last = frames - 1;
    const auto begin = static_cast<double>(start);
    const auto end = static_cast<double>(target);
    for (std::size_t frame = 0; frame < last; ++frame)
    {
        // Scale after adding the weighted endpoints so exact crossings stay zero with fused multiply-add.
        const auto gain = static_cast<float>((begin * (last - frame) + end * (frame + 1)) * scale);
        write_ramp_sample<Accumulate>(dst[frame], src[frame], Accumulate ? coefficient * gain : gain);
    }
    write_ramp_sample<Accumulate>(dst[last], src[last], Accumulate ? coefficient * target : target);
}
} // namespace

Matrix::Matrix(SampleRate rate, std::size_t channels, float gain)
    : NodeT({PortSpec(rate, channels, SampleFormat::F32P)}, {PortSpec(rate, channels, SampleFormat::F32P)}),
      target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !std::isfinite(gain))
        throw Error(ErrorCode::InvalidArg, "Invalid matrix specification");
}
Matrix::Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels, View<const float> coefficients,
               float gain)
    : NodeT({PortSpec(rate, input_channels, SampleFormat::F32P)},
            {PortSpec(rate, output_channels, SampleFormat::F32P)}),
      target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !outputs()[0].valid() || !std::isfinite(gain))
        throw Error(ErrorCode::InvalidArg, "Invalid matrix specification");
    if (output_channels > coefficients_.max_size() / input_channels)
        throw Error(ErrorCode::TooLarge, "Channel matrix exceeds supported size");
    if (coefficients.size() != input_channels * output_channels)
        throw Error(ErrorCode::InvalidArg, "Channel matrix dimensions do not match coefficients");
    for (const auto value : coefficients)
        if (!std::isfinite(value))
            throw Error(ErrorCode::InvalidArg, "Matrix coefficients must be finite");
    coefficients_.assign(coefficients.begin(), coefficients.end());
}
Matrix::Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels,
               std::initializer_list<float> coefficients, float gain)
    : Matrix(rate, input_channels, output_channels, {coefficients.begin(), coefficients.size()}, gain)
{
}
void Matrix::set_gain(float gain)
{
    if (!std::isfinite(gain))
        throw Error(ErrorCode::InvalidArg);
    target_.store(gain, std::memory_order_release);
}
float Matrix::gain() const noexcept
{
    return target_.load(std::memory_order_acquire);
}
void Matrix::prepare(BlockPeriod)
{
    reset();
}
void Matrix::reset() noexcept
{
    gain_ = target_.load(std::memory_order_acquire);
}
void Matrix::process(const ProcessContext &context) noexcept
{
    const auto &output_block = context.outputs[0];
    const auto output = output_block.f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    const auto target = target_.load(std::memory_order_acquire);
    const bool steady = gain_ == target;
    const bool active = context.has_input(0) && !(steady && target == 0.0f);
    context.set_output_active(0, active);
    if (!active)
    {
        output.clear();
        gain_ = target;
        return;
    }
    const auto &input_block = context.inputs[0];
    const auto input = input_block.f32p_view();
    const auto scale = steady ? 0.0 : 1.0 / frames;
    if (coefficients_.empty())
    {
        if (steady && target == 1.0f)
        {
            const auto bytes = input_block.byte_view();
            std::memcpy(output_block.byte_view().data(), bytes.data(), bytes.size());
            return;
        }
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            auto *dst = output.channel(ch);
            const auto *src = input.channel(ch);
            if (steady)
                apply_steady_gain<false>(dst, src, frames, target);
            else
                apply_ramping_gain<false>(dst, src, frames, 1.0f, gain_, target, scale);
        }
    }
    else
    {
        output.clear();
        const auto input_channels = input.channels();
        for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
        {
            auto *dst = output.channel(dst_ch);
            const auto *row = coefficients_.data() + dst_ch * input_channels;
            for (std::size_t src_ch = 0; src_ch < input_channels; ++src_ch)
            {
                const auto coefficient = row[src_ch];
                if (coefficient == 0)
                    continue;
                const auto *src = input.channel(src_ch);
                if (steady)
                    apply_steady_gain<true>(dst, src, frames, coefficient * target);
                else
                    apply_ramping_gain<true>(dst, src, frames, coefficient, gain_, target, scale);
            }
        }
    }
    gain_ = target;
}
} // namespace aereborn
