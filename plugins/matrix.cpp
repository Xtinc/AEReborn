#include "matrix.h"
#include <cmath>
#include <stdexcept>
namespace aereborn
{
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
RetCode Matrix::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (!target_.is_lock_free())
        return ErrorCode::Unsupported;
    reset();
    return {};
}
void Matrix::reset() noexcept
{
    gain_ = target_.load(std::memory_order_acquire);
}
void Matrix::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0].audio();
    const auto output = context.outputs[0].audio();
    output.clear();
    const auto target = target_.load(std::memory_order_acquire);
    const auto frames = output.frames();
    const auto step = frames ? (static_cast<double>(target) - gain_) / frames : 0;
    for (std::size_t dst = 0; dst < output.channels(); ++dst)
    {
        const auto first = coefficients_.empty() ? dst : 0;
        const auto last = coefficients_.empty() ? dst + 1 : input.channels();
        for (std::size_t src = first; src < last; ++src)
        {
            const auto coefficient = coefficients_.empty() ? 1.0f : coefficients_[dst * input.channels() + src];
            if (coefficient == 0)
                continue;
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                const auto gain = frame + 1 == frames ? target : static_cast<float>(gain_ + step * (frame + 1));
                const auto scaled = coefficient * gain;
                if (scaled != 0)
                    output.channel(dst)[frame] += input.channel(src)[frame] * scaled;
            }
        }
    }
    gain_ = target;
}
} // namespace aereborn
