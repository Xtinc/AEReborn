#ifndef AEREBORN_PLUGINS_ADAPTER_H
#define AEREBORN_PLUGINS_ADAPTER_H

#include "core/graph.h"

namespace aereborn
{

class PCMConverter
{
  protected:
    PCMConverter(PCMSpec spec, SampleRate src_fs, SampleRate dst_fs, BlockPeriod period) noexcept;
    RetCode prepare_pcm() noexcept;
    void reset_pcm() noexcept;
    std::size_t pcm_frames() const noexcept
    {
        return pcm_frames_;
    }
    std::size_t pcm_bytes() const noexcept
    {
        return pcm_frames_ * spec_.channels * sample_bytes_;
    }
    RetCode check_pcm_buffer(std::size_t bytes, bool bound) const noexcept;
    void decode(const std::uint8_t *pcm, std::size_t period_index, OBlock output) noexcept;
    void encode(IBlock input, std::size_t period_index, std::uint8_t *pcm) noexcept;

    const PCMSpec spec_;

  private:
    std::size_t frame_offset(std::size_t period_index) const noexcept;
    void resample(IBlock input, OBlock output) noexcept;

    const std::uint64_t period_frames_x1000_; // PCM frames per engine period in units of 1/1000 frame.
    const std::size_t sample_bytes_;
    const unsigned int sample_bits_;
    const std::uint32_t src_fs_;
    const std::uint32_t dst_fs_;
    std::size_t pcm_frames_ = 0;
    Buffer pcm_samples_;
    Buffer history_;
    std::vector<float> kernel_;
    std::int64_t phase_ = 0; // Next output sample's source position, in units of 1 / dst_fs_.
};

// Bind device buffers serially with the engine and keep them valid until the device period ends.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class IAdapter final : public NodeT<0, 1, Rate, Period>, private PCMConverter
{
  public:
    explicit IAdapter(PCMSpec spec)
        : NodeT<0, 1, Rate, Period>({}, {{spec.channels}}), PCMConverter(spec, spec.sample_rate, Rate, Period)
    {
    }
    using PCMConverter::pcm_frames;

    RetCode prepare() noexcept override
    {
        return prepare_pcm();
    }
    RetCode bind_pcm(View<const std::uint8_t> bytes) noexcept
    {
        const auto code = check_pcm_buffer(bytes.size(), pcm_ != nullptr);
        if (code)
        {
            pcm_ = bytes.data();
        }
        return code;
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto output = context.outputs[0];
        if (!pcm_)
        {
            output.clear();
            return;
        }
        decode(pcm_, period_index_, output);
        if (++period_index_ == spec_.periods)
        {
            pcm_ = nullptr;
            period_index_ = 0;
        }
    }
    void reset() noexcept override
    {
        pcm_ = nullptr;
        period_index_ = 0;
        reset_pcm();
    }

  private:
    std::size_t period_index_ = 0;
    const std::uint8_t *pcm_ = nullptr;
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class OAdapter final : public NodeT<1, 0, Rate, Period>, private PCMConverter
{
  public:
    explicit OAdapter(PCMSpec spec)
        : NodeT<1, 0, Rate, Period>({{spec.channels}}, {}), PCMConverter(spec, Rate, spec.sample_rate, Period)
    {
    }
    using PCMConverter::pcm_frames;

    RetCode prepare() noexcept override
    {
        return prepare_pcm();
    }
    RetCode bind_pcm(View<std::uint8_t> bytes) noexcept
    {
        const auto code = check_pcm_buffer(bytes.size(), pcm_ != nullptr);
        if (code)
        {
            pcm_ = bytes.data();
        }
        return code;
    }
    void process(const ProcContext &context) noexcept override
    {
        if (!pcm_)
        {
            return;
        }
        encode(context.inputs[0], period_index_, pcm_);
        if (++period_index_ == spec_.periods)
        {
            pcm_ = nullptr;
            period_index_ = 0;
        }
    }
    void reset() noexcept override
    {
        pcm_ = nullptr;
        period_index_ = 0;
        reset_pcm();
    }

  private:
    std::size_t period_index_ = 0;
    std::uint8_t *pcm_ = nullptr;
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_ADAPTER_H
