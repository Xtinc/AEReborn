#ifndef AEREBORN_TESTS_MIXER_H
#define AEREBORN_TESTS_MIXER_H

#include "core/graph.h"
#include <algorithm>

namespace aereborn
{
namespace test
{

// Multiple connections are summed by Engine; this node copies the sum to its output.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class MixerNode final : public NodeT<1, 1, Rate, Period>
{
  public:
    explicit MixerNode(std::size_t channels);
    void process(const ProcContext &context) noexcept override;
};

template <SampleRate Rate, BlockPeriod Period>
MixerNode<Rate, Period>::MixerNode(std::size_t channels) : NodeT<1, 1, Rate, Period>({{channels}}, {{channels}})
{
}

template <SampleRate Rate, BlockPeriod Period>
void MixerNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        std::copy_n(input.channel(ch), calc_block_frames<Rate, Period>(), output.channel(ch));
    }
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_MIXER_H
