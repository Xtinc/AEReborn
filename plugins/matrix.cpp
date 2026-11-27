#include "matrix.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace aereborn
{
namespace
{
void accumulate_steady(float *dst, const float *src, std::size_t frames, float gain) noexcept
{
    if (gain == 0.0f)
        return;
    for (std::size_t frame = 0; frame < frames; ++frame)
        dst[frame] += src[frame] * gain;
}

void accumulate_ramp(float *dst, const float *src, std::size_t frames, float coefficient, float start, float target,
                     double scale) noexcept
{
    const auto last = frames - 1;
    const auto begin = static_cast<double>(start);
    const auto end = static_cast<double>(target);
    for (std::size_t frame = 0; frame < last; ++frame)
    {
        // Scale after adding the weighted endpoints so exact crossings stay zero with fused multiply-add.
        const auto gain = coefficient * static_cast<float>((begin * (last - frame) + end * (frame + 1)) * scale);
        if (gain != 0.0f)
            dst[frame] += src[frame] * gain;
    }
    const auto end_gain = coefficient * target;
    if (end_gain != 0.0f)
        dst[last] += src[last] * end_gain;
}

std::size_t find_unity_source(const float *row, std::size_t input_channels, float target) noexcept
{
    std::size_t found = input_channels;
    for (std::size_t src_ch = 0; src_ch < input_channels; ++src_ch)
    {
        if (row[src_ch] == 0.0f)
            continue;
        if (found != input_channels || row[src_ch] * target != 1.0f)
            return input_channels;
        found = src_ch;
    }
    return found;
}
} // namespace

Matrix::Matrix(BandWidth rate, std::size_t channels, float gain)
    : NodeT({PortSpec(rate, channels, SampleFormat::F32P)}, {PortSpec(rate, channels, SampleFormat::F32P)}),
      target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !std::isfinite(gain))
        throw Error(RetCode::EPARAM, "Invalid matrix specification");
    if (channels > coefficients_.max_size() / channels)
        throw Error(RetCode::TOOLARGE, "Channel matrix exceeds supported size");
    coefficients_.resize(channels * channels, 0.0f);
    for (std::size_t ch = 0; ch < channels; ++ch)
        coefficients_[ch * channels + ch] = 1.0f;
}
Matrix::Matrix(BandWidth rate, std::size_t input_channels, std::size_t output_channels, View<const float> coefficients,
               float gain)
    : NodeT({PortSpec(rate, input_channels, SampleFormat::F32P)},
            {PortSpec(rate, output_channels, SampleFormat::F32P)}),
      target_(gain), gain_(gain)
{
    if (!inputs()[0].valid() || !outputs()[0].valid() || !std::isfinite(gain))
        throw Error(RetCode::EPARAM, "Invalid matrix specification");
    if (output_channels > coefficients_.max_size() / input_channels)
        throw Error(RetCode::TOOLARGE, "Channel matrix exceeds supported size");
    if (coefficients.size() != input_channels * output_channels)
        throw Error(RetCode::EPARAM, "Channel matrix dimensions do not match coefficients");
    for (const auto value : coefficients)
        if (!std::isfinite(value))
            throw Error(RetCode::EPARAM, "Matrix coefficients must be finite");
    coefficients_.assign(coefficients.begin(), coefficients.end());
}
Matrix::Matrix(BandWidth rate, std::size_t input_channels, std::size_t output_channels,
               std::initializer_list<float> coefficients, float gain)
    : Matrix(rate, input_channels, output_channels, {coefficients.begin(), coefficients.size()}, gain)
{
}
void Matrix::set_gain(float gain)
{
    if (!std::isfinite(gain))
        throw Error(RetCode::EPARAM);
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
    const auto output = context.outputs[0].f32p_view();
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
    const auto input = context.inputs[0].f32p_view();
    const auto scale = steady ? 0.0 : 1.0 / frames;
    const auto input_channels = input.channels();
    for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
    {
        auto *dst = output.channel(dst_ch);
        const auto *row = coefficients_.data() + dst_ch * input_channels;
        if (steady)
        {
            const auto unity_src = find_unity_source(row, input_channels, target);
            if (unity_src < input_channels)
            {
                std::memcpy(dst, input.channel(unity_src), frames * sizeof(float));
                continue;
            }
        }
        std::fill_n(dst, frames, 0.0f);
        for (std::size_t src_ch = 0; src_ch < input_channels; ++src_ch)
        {
            const auto coefficient = row[src_ch];
            if (coefficient == 0.0f)
                continue;
            const auto *src = input.channel(src_ch);
            if (steady)
                accumulate_steady(dst, src, frames, coefficient * target);
            else
                accumulate_ramp(dst, src, frames, coefficient, gain_, target, scale);
        }
    }
    gain_ = target;
}
} // namespace aereborn
