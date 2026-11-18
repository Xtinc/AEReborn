#ifndef AEREBORN_PLUGINS_ADAPTER_H
#define AEREBORN_PLUGINS_ADAPTER_H

#include "core/graph.h"
#include <memory>

namespace aereborn
{
namespace detail
{

// Conversion state prepared by Adapter; graph rate/period/frames come from its node template.
class PCMConverter
{
  public:
    static Result<std::unique_ptr<PCMConverter>> create(PCMSpec spec, SampleRate rate, BlockPeriod period,
                                                       std::size_t frames, std::size_t channels, bool input,
                                                       const ChMatrix &matrix) noexcept;
    void reset_pcm() noexcept;
    std::size_t pcm_frames() const noexcept
    {
        return pcm_frames_;
    }
    std::size_t pcm_bytes() const noexcept
    {
        return pcm_frames_ * spec_.channels * sample_bytes_;
    }
    std::size_t periods() const noexcept
    {
        return spec_.periods;
    }
    void decode(const std::uint8_t *pcm, std::size_t period_index, OBlock output) noexcept;
    void encode(IBlock input, std::size_t period_index, std::uint8_t *pcm) noexcept;

  private:
    PCMConverter(PCMSpec spec, SampleRate src_fs, SampleRate dst_fs, BlockPeriod period, const ChMatrix &matrix);
    RetCode prepare_pcm(std::size_t graph_frames);
    std::size_t frame_offset(std::size_t period_index) const noexcept;
    void resample(IBlock input, OBlock output) noexcept;

    const PCMSpec spec_;
    const ChMatrix matrix_;
    const std::uint64_t period_frames_x1000_; // PCM frames per engine period in units of 1/1000 frame.
    const std::size_t sample_bytes_;
    const unsigned int sample_bits_;
    const std::uint32_t src_fs_;
    const std::uint32_t dst_fs_;
    std::size_t pcm_frames_ = 0;
    Buffer pcm_samples_;
    Buffer mapped_samples_; // Current-period workspace in device channels; never queues audio.
    Buffer history_;
    std::vector<float> kernel_;
    std::int64_t phase_ = 0; // Next output sample's source position, in units of 1 / dst_fs_.
};

// Shared binding and ownership for the two PCM directions. All calls are serialized.
template <class Byte> class PCMTransfer
{
  public:
    std::size_t pcm_frames() const noexcept
    {
        return converter_ ? converter_->pcm_frames() : 0;
    }
    std::size_t pcm_bytes() const noexcept
    {
        return converter_ ? converter_->pcm_bytes() : 0;
    }
    RetCode bind_pcm(View<Byte> bytes) noexcept
    {
        if (!converter_)
        {
            return ErrorCode::NotPrepared;
        }
        if (bytes.size() != pcm_bytes())
        {
            return ErrorCode::InvalidArg;
        }
        if (pcm_)
        {
            return ErrorCode::BufferFull;
        }
        pcm_ = bytes.data();
        return {};
    }

  protected:
    RetCode configure_transfer(Result<std::unique_ptr<PCMConverter>> result) noexcept
    {
        if (!result)
        {
            return result.ret_code();
        }
        pcm_ = nullptr;
        period_index_ = 0;
        converter_ = std::move(result).value();
        return {};
    }
    void advance_pcm() noexcept
    {
        if (++period_index_ == converter_->periods())
        {
            pcm_ = nullptr;
            period_index_ = 0;
        }
    }
    void reset_transfer() noexcept
    {
        pcm_ = nullptr;
        period_index_ = 0;
        if (converter_)
        {
            converter_->reset_pcm();
        }
    }

    std::unique_ptr<PCMConverter> converter_;
    std::size_t period_index_ = 0;
    Byte *pcm_ = nullptr;
};

} // namespace detail

// Fixed graph channels; configure on a non-audio thread, serialized with bind/reset/process.
// Borrow interleaved PCM for spec.periods process calls. Device I/O and queues belong to the backend.
template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class IAdapter final : public NodeT<0, 1, Rate, Period>, private detail::PCMTransfer<const std::uint8_t>
{
  public:
    explicit IAdapter(std::size_t channels) : NodeT<0, 1, Rate, Period>({}, {{channels}})
    {
    }
    using detail::PCMTransfer<const std::uint8_t>::pcm_frames;
    using detail::PCMTransfer<const std::uint8_t>::pcm_bytes;
    using detail::PCMTransfer<const std::uint8_t>::bind_pcm;

    // A failed configuration preserves the active converter and borrowed PCM.
    RetCode configure_pcm(PCMSpec spec, const ChMatrix &matrix = {}) noexcept
    {
        return configure_transfer(detail::PCMConverter::create(spec, Rate, Period, calc_block_frames<Rate, Period>(),
                                                               this->outputs()[0], true, matrix));
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto output = context.outputs[0];
        if (!pcm_)
        {
            output.clear();
            return;
        }
        converter_->decode(pcm_, period_index_, output);
        advance_pcm();
    }
    void reset() noexcept override
    {
        reset_transfer();
    }
};

template <SampleRate Rate = SampleRate::Hz48000, BlockPeriod Period = BlockPeriod::Ms10>
class OAdapter final : public NodeT<1, 0, Rate, Period>, private detail::PCMTransfer<std::uint8_t>
{
  public:
    explicit OAdapter(std::size_t channels) : NodeT<1, 0, Rate, Period>({{channels}}, {})
    {
    }
    using detail::PCMTransfer<std::uint8_t>::pcm_frames;
    using detail::PCMTransfer<std::uint8_t>::pcm_bytes;
    using detail::PCMTransfer<std::uint8_t>::bind_pcm;

    RetCode configure_pcm(PCMSpec spec, const ChMatrix &matrix = {}) noexcept
    {
        return configure_transfer(detail::PCMConverter::create(spec, Rate, Period, calc_block_frames<Rate, Period>(),
                                                               this->inputs()[0], false, matrix));
    }
    void process(const ProcContext &context) noexcept override
    {
        if (!pcm_)
        {
            return;
        }
        converter_->encode(context.inputs[0], period_index_, pcm_);
        advance_pcm();
    }
    void reset() noexcept override
    {
        reset_transfer();
    }
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_ADAPTER_H
