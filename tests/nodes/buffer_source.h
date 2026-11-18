#ifndef AEREBORN_TESTS_BUFFER_SOURCE_H
#define AEREBORN_TESTS_BUFFER_SOURCE_H

#include "core/graph.h"
#include <algorithm>

namespace aereborn
{
namespace test
{

// One bounded staging block. push() copies data; underflow produces silence.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class BufferSourceNode final : public NodeT<0, 1, Rate, Period>
{
  public:
    explicit BufferSourceNode(std::size_t channels);
    RetCode prepare() noexcept override;
    RetCode push(IBlock input) noexcept;
    RetCode push_interleaved(const float *samples, std::size_t frames) noexcept;
    std::size_t pending_frames() const noexcept
    {
        return pending_frames_;
    }
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    RetCode validate_push(std::size_t frames) const noexcept;
    Buffer pending_;
    std::size_t pending_frames_ = 0;
};

template <SampleRate Rate, BlockPeriod Period>
BufferSourceNode<Rate, Period>::BufferSourceNode(std::size_t channels) : NodeT<0, 1, Rate, Period>({}, {{channels}})
{
}

template <SampleRate Rate, BlockPeriod Period> RetCode BufferSourceNode<Rate, Period>::prepare() noexcept
{
    const auto code = pending_.resize(this->outputs()[0], calc_block_frames<Rate, Period>());
    if (code)
    {
        reset();
    }
    return code;
}

template <SampleRate Rate, BlockPeriod Period>
RetCode BufferSourceNode<Rate, Period>::validate_push(std::size_t frames) const noexcept
{
    const auto capacity_frames = pending_.capacity_frames();
    if (capacity_frames == 0)
    {
        return ErrorCode::NotPrepared;
    }
    if (frames > capacity_frames)
    {
        return ErrorCode::InvalidArg;
    }
    if (frames != 0 && pending_frames_ != 0)
    {
        return ErrorCode::BufferFull;
    }
    return {};
}

template <SampleRate Rate, BlockPeriod Period> RetCode BufferSourceNode<Rate, Period>::push(IBlock input) noexcept
{
    const auto frames = input.frames();
    const auto code = validate_push(frames);
    if (!code)
    {
        return code;
    }
    if (input.channels() != this->outputs()[0])
    {
        return ErrorCode::ChannelMismatch;
    }
    if (frames == 0)
    {
        return {};
    }
    const auto pending = pending_.block(frames);
    for (std::size_t ch = 0; ch < input.channels(); ++ch)
    {
        std::copy_n(input.channel(ch), frames, pending.channel(ch));
    }
    pending_frames_ = frames;
    return {};
}

template <SampleRate Rate, BlockPeriod Period>
RetCode BufferSourceNode<Rate, Period>::push_interleaved(const float *samples, std::size_t frames) noexcept
{
    const auto code = validate_push(frames);
    if (!code)
    {
        return code;
    }
    if (frames == 0)
    {
        return {};
    }
    if (!samples)
    {
        return ErrorCode::InvalidArg;
    }
    const auto pending = pending_.block(frames);
    const auto channels = pending.channels();
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        auto *ch_samples = pending.channel(ch);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            ch_samples[frame] = samples[frame * channels + ch];
        }
    }
    pending_frames_ = frames;
    return {};
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSourceNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    constexpr auto frames = calc_block_frames<Rate, Period>();
    const auto output = context.outputs[0];
    const auto pending = pending_.block(frames);
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        auto *output_samples = output.channel(ch);
        std::copy_n(pending.channel(ch), pending_frames_, output_samples);
        std::fill(output_samples + pending_frames_, output_samples + frames, 0.0f);
    }
    pending_frames_ = 0;
}

template <SampleRate Rate, BlockPeriod Period> void BufferSourceNode<Rate, Period>::reset() noexcept
{
    pending_frames_ = 0;
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_BUFFER_SOURCE_H
