#ifndef AEREBORN_TESTS_GAIN_H
#define AEREBORN_TESTS_GAIN_H

#include "core/graph.h"
#include <cmath>

namespace aereborn
{
namespace test
{

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class GainNode final : public NodeT<1, 1, Rate, Period>
{
  public:
    explicit GainNode(std::size_t channels, float gain = 1.0f);
    RetCode set_gain(float linear_gain) noexcept;
    void process(const ProcContext &context) noexcept override;

  private:
    RetCode prepare() noexcept override;
    float gain_;
};

template <SampleRate Rate, BlockPeriod Period>
GainNode<Rate, Period>::GainNode(std::size_t channels, float gain)
    : NodeT<1, 1, Rate, Period>({{channels}}, {{channels}}), gain_(gain)
{
}

template <SampleRate Rate, BlockPeriod Period> RetCode GainNode<Rate, Period>::prepare() noexcept
{
    return std::isfinite(gain_) ? ErrorCode::Ok : ErrorCode::InvalidArg;
}

template <SampleRate Rate, BlockPeriod Period> RetCode GainNode<Rate, Period>::set_gain(float linear_gain) noexcept
{
    if (!std::isfinite(linear_gain))
    {
        return ErrorCode::InvalidArg;
    }
    gain_ = linear_gain;
    return {};
}

template <SampleRate Rate, BlockPeriod Period> void GainNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        const auto *input_samples = input.channel(ch);
        auto *output_samples = output.channel(ch);
        for (std::size_t frame = 0; frame < calc_block_frames<Rate, Period>(); ++frame)
        {
            output_samples[frame] = input_samples[frame] * gain_;
        }
    }
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_GAIN_H
