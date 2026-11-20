#ifndef AEREBORN_TESTS_TEST_NODE_H
#define AEREBORN_TESTS_TEST_NODE_H

#include "core/graph.h"

namespace aereborn
{
namespace test
{

constexpr auto render_rate = SampleRate::Hz48000;
constexpr auto render_period = BlockPeriod::Ms05;
constexpr std::size_t render_frames = 240;

// Stateless test processor that sums its input ports.
template <std::size_t I = 1, std::size_t O = 1>
class VirtualNode : public NodeT<I, O>
{
  public:
    explicit VirtualNode(std::size_t channels = 1, float gain = 1.0f)
        : NodeT<I, O>(ports<I>(channels), ports<O>(channels)), gain_(gain)
    {
    }
    RetCode prepare(BlockPeriod) override
    {
        ++prepares;
        return preparation;
    }
    void process(const ProcessContext &context) noexcept override
    {
        ++calls;
        for (const auto block : context.outputs)
        {
            const auto output = block.f32p_view();
            output.clear();
            for (const auto source : context.inputs)
            {
                const auto input = source.f32p_view();
                for (std::size_t ch = 0; ch < output.channels(); ++ch)
                {
                    for (std::size_t frame = 0; frame < output.frames(); ++frame)
                    {
                        output.channel(ch)[frame] += input.channel(ch)[frame] * gain_;
                    }
                }
            }
        }
    }
    void reset() noexcept override
    {
        ++resets;
    }

    RetCode preparation;
    std::size_t prepares = 0;
    std::size_t calls = 0;
    std::size_t resets = 0;

  private:
    template <std::size_t N> static std::array<PortSpec, N> ports(std::size_t channels)
    {
        std::array<PortSpec, N> result{};
        result.fill(PortSpec(render_rate, channels, SampleFormat::F32P));
        return result;
    }
    const float gain_;
};

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_TEST_NODE_H
