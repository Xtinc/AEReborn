#ifndef AEREBORN_TESTS_BUFFER_SINK_H
#define AEREBORN_TESTS_BUFFER_SINK_H

#include "core/graph.h"

namespace aereborn
{
namespace test
{

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class BufferSinkNode final : public NodeT<1, 0, Rate, Period>
{
  public:
    explicit BufferSinkNode(std::size_t channels);
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;
    // Borrowed until the next process(), prepare(), reset() or topology edit.
    IBlock block() const noexcept
    {
        return last_block_;
    }
    std::uint64_t frame_position() const noexcept
    {
        return frame_position_;
    }

  private:
    IBlock last_block_;
    std::uint64_t frame_position_ = 0;
};

template <SampleRate Rate, BlockPeriod Period>
BufferSinkNode<Rate, Period>::BufferSinkNode(std::size_t channels) : NodeT<1, 0, Rate, Period>({{channels}}, {})
{
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSinkNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    last_block_ = context.inputs[0];
    frame_position_ = context.frame_position;
}

template <SampleRate Rate, BlockPeriod Period> void BufferSinkNode<Rate, Period>::reset() noexcept
{
    last_block_ = {};
    frame_position_ = 0;
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_BUFFER_SINK_H
