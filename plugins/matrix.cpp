#include "matrix.h"
#include <cmath>
#include <stdexcept>
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

template <bool Accumulate, class Ramp>
void apply_gain(float *dst, const float *src, std::size_t frames, float coefficient, float target, bool steady,
                const Ramp &ramp_gain) noexcept
{
    const auto scaled_target = Accumulate ? coefficient * target : target;
    if (steady)
    {
        if (scaled_target != 0)
            for (std::size_t frame = 0; frame < frames; ++frame)
                write_scaled<Accumulate>(dst[frame], src[frame], scaled_target);
        return;
    }

    const auto write_frame = [&](std::size_t frame, float gain) noexcept {
        if (gain != 0)
            write_scaled<Accumulate>(dst[frame], src[frame], gain);
        else if (!Accumulate)
            dst[frame] = 0.0f;
    };
    const auto last = frames - 1;
    for (std::size_t frame = 0; frame < last; ++frame)
    {
        const auto gain = ramp_gain(frame);
        write_frame(frame, Accumulate ? coefficient * gain : gain);
    }
    write_frame(last, scaled_target);
}
} // namespace

Matrix::Matrix(SampleRate rate, std::size_t channels, float gain)
    : NodeT({{PortSpec(rate, channels)}}, {{PortSpec(rate, channels)}}), target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !std::isfinite(gain))
        throw std::invalid_argument("Invalid matrix specification");
}
Matrix::Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels, View<const float> coefficients,
               float gain)
    : NodeT({{PortSpec(rate, input_channels)}}, {{PortSpec(rate, output_channels)}}), target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !outputs()[0].valid() || !std::isfinite(gain))
        throw std::invalid_argument("Invalid matrix specification");
    if (output_channels > coefficients_.max_size() / input_channels)
        throw std::length_error("Channel matrix exceeds supported size");
    if (coefficients.size() != input_channels * output_channels)
        throw std::invalid_argument("Channel matrix dimensions do not match coefficients");
    for (const auto value : coefficients)
        if (!std::isfinite(value))
            throw std::invalid_argument("Matrix coefficients must be finite");
    coefficients_.assign(coefficients.begin(), coefficients.end());
}
Matrix::Matrix(SampleRate rate, std::size_t input_channels, std::size_t output_channels,
               std::initializer_list<float> coefficients, float gain)
    : Matrix(rate, input_channels, output_channels, {coefficients.begin(), coefficients.size()}, gain)
{
}
RetCode Matrix::set_gain(float gain) noexcept
{
    if (!std::isfinite(gain))
        return ErrorCode::InvalidArg;
    target_.store(gain, std::memory_order_release);
    return {};
}
RetCode Matrix::prepare(BlockPeriod)
{
    reset();
    return {};
}
void Matrix::reset() noexcept
{
    gain_ = target_.load(std::memory_order_acquire);
}
void Matrix::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;
    const auto target = target_.load(std::memory_order_acquire);
    const bool steady = gain_ == target;
    if (steady && target == 0)
    {
        output.clear();
        return;
    }
    const auto input = context.inputs[0].f32p_view();
    const auto last = frames - 1;
    const auto scale = steady ? 0.0 : 1.0 / frames;
    const auto ramp_gain = [start = static_cast<double>(gain_), end = static_cast<double>(target), last,
                            scale](std::size_t frame) noexcept {
        // Scale after adding the weighted endpoints so exact crossings stay zero with fused multiply-add.
        return static_cast<float>((start * (last - frame) + end * (frame + 1)) * scale);
    };
    if (coefficients_.empty())
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
            apply_gain<false>(output.channel(ch), input.channel(ch), frames, 1.0f, target, steady, ramp_gain);
    }
    else
    {
        output.clear();
        for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
        {
            auto *dst = output.channel(dst_ch);
            for (std::size_t src_ch = 0; src_ch < input.channels(); ++src_ch)
            {
                const auto coefficient = coefficients_[dst_ch * input.channels() + src_ch];
                if (coefficient != 0)
                    apply_gain<true>(dst, input.channel(src_ch), frames, coefficient, target, steady, ramp_gain);
            }
        }
    }
    gain_ = target;
}
} // namespace aereborn
