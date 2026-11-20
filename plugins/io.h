#ifndef AEREBORN_PLUGINS_IO_H
#define AEREBORN_PLUGINS_IO_H
#include "core/graph.h"
#include <limits>
namespace aereborn
{
namespace detail
{
template <class Byte> class TickBuffer
{
  public:
    RetCode prepare(BlockPeriod period) noexcept
    {
        if (period_ != period)
            reset();
        period_ = period;
        return {};
    }

    RetCode bind(PortSpec spec, View<Byte> bytes, std::uint64_t tick) noexcept
    {
        if (!bytes.empty() && !bytes.data())
            return ErrorCode::InvalidArg;
        if (period_ == BlockPeriod{})
            return ErrorCode::NotPrepared;
        if (!bytes_.empty())
            return ErrorCode::BufferFull;
        const auto bytes_per_channel = spec.frames(period_, tick) * sample_bytes(spec.format);
        // Every supported rate and period produces a nonempty block.
        if (spec.channels > std::numeric_limits<std::size_t>::max() / bytes_per_channel)
            return ErrorCode::TooLarge;
        if (bytes.size() != spec.channels * bytes_per_channel)
            return ErrorCode::InvalidArg;
        bytes_ = bytes;
        tick_ = tick;
        return {};
    }

    View<Byte> take(std::uint64_t tick) noexcept
    {
        if (bytes_.empty() || tick < tick_)
            return {};
        const auto bytes = bytes_;
        reset();
        return tick == tick_ ? bytes : View<Byte>{};
    }

    void reset() noexcept { bytes_ = {}; }

  private:
    BlockPeriod period_{};
    View<Byte> bytes_;
    std::uint64_t tick_ = 0;
};
} // namespace detail

// Capture and Playback borrow one tick of memory from the caller. Keep it alive through
// that tick. Use a backend queue for buffers exchanged across threads.
class Capture final : public NodeT<0, 1>
{
  public:
    explicit Capture(PortSpec spec);
    RetCode prepare(BlockPeriod period) override;
    // Must be serialized with process(), like reset().
    RetCode bind(View<const std::uint8_t> bytes, std::uint64_t tick) noexcept;
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    detail::TickBuffer<const std::uint8_t> buffer_;
};
class Playback final : public NodeT<1, 0>
{
  public:
    explicit Playback(PortSpec spec);
    RetCode prepare(BlockPeriod period) override;
    // Must be serialized with process(), like reset().
    RetCode bind(View<std::uint8_t> bytes, std::uint64_t tick) noexcept;
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    detail::TickBuffer<std::uint8_t> buffer_;
};
} // namespace aereborn
#endif
