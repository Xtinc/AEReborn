#ifndef AEREBORN_PLUGINS_NODES_H
#define AEREBORN_PLUGINS_NODES_H

#include "core/graph.h"
#include <algorithm>
#include <cmath>

namespace aereborn
{

// Built-in node implementations. Source, processor, mixer and sink are usage roles.
// One bounded staging block. push() copies data; underflow produces silence.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class BufferSourceNode final : public NodeT<0, 1, Rate, Period>
{
  public:
    explicit BufferSourceNode(std::size_t channels);
    void attributes(std::ostream &stream) const override;
    void prepare() override;
    void push(IBlock block);
    void push_interleaved(const float *samples, std::size_t frames);
    std::size_t pending_frames() const noexcept
    {
        return pending_frames_;
    }
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    void validate_push(std::size_t frames) const;
    Buffer pending_;
    std::size_t pending_frames_ = 0;
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class SineNode final : public NodeT<0, 1, Rate, Period>
{
  public:
    explicit SineNode(std::size_t channels = 1, double frequency = 440.0, float amplitude = 0.25f);
    void attributes(std::ostream &stream) const override;
    void prepare() override;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    double frequency_;
    float amplitude_;
    double phase_ = 0.0;
    double phase_step_ = 0.0;
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class GainNode final : public NodeT<1, 1, Rate, Period>
{
  public:
    explicit GainNode(std::size_t channels, float gain = 1.0f);
    void attributes(std::ostream &stream) const override;
    void set_gain(float linear_gain);
    void process(const ProcContext &context) noexcept override;

  private:
    float gain_;
};

// Multiple connections are summed by Engine; this node copies the sum to its output.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class MixerNode final : public NodeT<1, 1, Rate, Period>
{
  public:
    explicit MixerNode(std::size_t channels);
    void attributes(std::ostream &stream) const override;
    void process(const ProcContext &context) noexcept override;
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class BufferSinkNode final : public NodeT<1, 0, Rate, Period>
{
  public:
    explicit BufferSinkNode(std::size_t channels);
    void attributes(std::ostream &stream) const override;
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

namespace detail
{
constexpr double two_pi = 6.28318530717958647692;
} // namespace detail

template <SampleRate Rate, BlockPeriod Period>
BufferSourceNode<Rate, Period>::BufferSourceNode(std::size_t channels) : NodeT<0, 1, Rate, Period>({}, {{channels}})
{
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSourceNode<Rate, Period>::attributes(std::ostream &stream) const
{
    stream << "BufferSourceNode { capacity_frames=" << pending_.capacity_frames()
           << ", pending_frames=" << pending_frames_ << " }";
}

template <SampleRate Rate, BlockPeriod Period> void BufferSourceNode<Rate, Period>::prepare()
{
    pending_ = Buffer(this->outputs()[0].channels, calc_block_frames<Rate, Period>());
    reset();
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSourceNode<Rate, Period>::validate_push(std::size_t frames) const
{
    if (pending_.capacity_frames() == 0)
    {
        throw GraphError(ErrorCode::NotPrepared, "Prepare before pushing audio");
    }
    if (frames > pending_.capacity_frames())
    {
        throw GraphError(ErrorCode::InvalidArgument, "Input block exceeds staging capacity");
    }
    if (frames != 0 && pending_frames_ != 0)
    {
        throw GraphError(ErrorCode::BufferFull, "Consume the pending input block before pushing again");
    }
}

template <SampleRate Rate, BlockPeriod Period> void BufferSourceNode<Rate, Period>::push(IBlock block)
{
    validate_push(block.frames());
    if (block.channels() != this->outputs()[0].channels)
    {
        throw GraphError(ErrorCode::ChannelMismatch, "Input block has a different channel count");
    }
    if (block.frames() == 0)
    {
        return;
    }
    const auto destination = pending_.block(block.frames());
    for (std::size_t ch = 0; ch < block.channels(); ++ch)
    {
        std::copy_n(block.channel(ch), block.frames(), destination.channel(ch));
    }
    pending_frames_ = block.frames();
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSourceNode<Rate, Period>::push_interleaved(const float *samples, std::size_t frames)
{
    validate_push(frames);
    if (frames == 0)
    {
        return;
    }
    if (!samples)
    {
        throw GraphError(ErrorCode::InvalidArgument, "Input samples cannot be null");
    }
    auto destination = pending_.block(frames);
    for (std::size_t ch = 0; ch < destination.channels(); ++ch)
    {
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            destination.channel(ch)[frame] = samples[frame * destination.channels() + ch];
        }
    }
    pending_frames_ = frames;
}

template <SampleRate Rate, BlockPeriod Period>
void BufferSourceNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    constexpr auto frames = calc_block_frames<Rate, Period>();
    const auto output = context.outputs[0];
    const auto input = pending_.block(frames);
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        std::copy_n(input.channel(ch), pending_frames_, output.channel(ch));
        std::fill(output.channel(ch) + pending_frames_, output.channel(ch) + frames, 0.0f);
    }
    pending_frames_ = 0;
}

template <SampleRate Rate, BlockPeriod Period> void BufferSourceNode<Rate, Period>::reset() noexcept
{
    pending_frames_ = 0;
}

template <SampleRate Rate, BlockPeriod Period>
SineNode<Rate, Period>::SineNode(std::size_t channels, double frequency, float amplitude)
    : NodeT<0, 1, Rate, Period>({}, {{channels}}), frequency_(frequency), amplitude_(amplitude)
{
    if (!std::isfinite(frequency) || frequency < 0.0 || !std::isfinite(amplitude))
    {
        throw GraphError(ErrorCode::InvalidArgument, "Invalid sine parameters");
    }
}

template <SampleRate Rate, BlockPeriod Period> void SineNode<Rate, Period>::prepare()
{
    constexpr auto sample_rate = static_cast<std::uint32_t>(Rate);
    if (frequency_ >= sample_rate / 2.0)
    {
        throw GraphError(ErrorCode::InvalidArgument, "Sine frequency must be below Nyquist");
    }
    phase_step_ = detail::two_pi * frequency_ / sample_rate;
}

template <SampleRate Rate, BlockPeriod Period> void SineNode<Rate, Period>::attributes(std::ostream &stream) const
{
    stream << "SineNode { frequency=" << frequency_ << ", amplitude=" << amplitude_ << " }";
}

template <SampleRate Rate, BlockPeriod Period> void SineNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0];
    for (std::size_t frame = 0; frame < calc_block_frames<Rate, Period>(); ++frame)
    {
        const auto sample = amplitude_ * static_cast<float>(std::sin(phase_));
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            output.channel(ch)[frame] = sample;
        }
        phase_ += phase_step_;
        if (phase_ >= detail::two_pi)
        {
            phase_ -= detail::two_pi;
        }
    }
}

template <SampleRate Rate, BlockPeriod Period> void SineNode<Rate, Period>::reset() noexcept
{
    phase_ = 0.0;
}

template <SampleRate Rate, BlockPeriod Period>
GainNode<Rate, Period>::GainNode(std::size_t channels, float gain)
    : NodeT<1, 1, Rate, Period>({{channels}}, {{channels}})
{
    set_gain(gain);
}

template <SampleRate Rate, BlockPeriod Period> void GainNode<Rate, Period>::set_gain(float linear_gain)
{
    if (!std::isfinite(linear_gain))
    {
        throw GraphError(ErrorCode::InvalidArgument, "Gain must be finite");
    }
    gain_ = linear_gain;
}

template <SampleRate Rate, BlockPeriod Period> void GainNode<Rate, Period>::attributes(std::ostream &stream) const
{
    stream << "GainNode { gain=" << gain_ << " }";
}

template <SampleRate Rate, BlockPeriod Period> void GainNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        for (std::size_t frame = 0; frame < calc_block_frames<Rate, Period>(); ++frame)
        {
            output.channel(ch)[frame] = input.channel(ch)[frame] * gain_;
        }
    }
}

template <SampleRate Rate, BlockPeriod Period>
MixerNode<Rate, Period>::MixerNode(std::size_t channels)
    : NodeT<1, 1, Rate, Period>({{channels, IMode::Mix}}, {{channels}})
{
}

template <SampleRate Rate, BlockPeriod Period> void MixerNode<Rate, Period>::attributes(std::ostream &stream) const
{
    stream << "MixerNode {}";
}

template <SampleRate Rate, BlockPeriod Period>
void MixerNode<Rate, Period>::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0];
    for (std::size_t ch = 0; ch < output.channels(); ++ch)
    {
        std::copy_n(context.inputs[0].channel(ch), calc_block_frames<Rate, Period>(), output.channel(ch));
    }
}

template <SampleRate Rate, BlockPeriod Period>
BufferSinkNode<Rate, Period>::BufferSinkNode(std::size_t channels) : NodeT<1, 0, Rate, Period>({{channels}}, {})
{
}

template <SampleRate Rate, BlockPeriod Period> void BufferSinkNode<Rate, Period>::attributes(std::ostream &stream) const
{
    stream << "BufferSinkNode { frame_position=" << frame_position_ << ", frames=" << last_block_.frames() << " }";
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

} // namespace aereborn

#endif // AEREBORN_PLUGINS_NODES_H
