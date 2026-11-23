#ifndef AEREBORN_TESTS_IO_H
#define AEREBORN_TESTS_IO_H
#include "core/graph.h"
#include <limits>
namespace aereborn
{
namespace test
{
namespace detail
{
template <class Byte> class TickBuffer
{
  public:
    void prepare(BlockPeriod period) noexcept
    {
        if (period_ != period)
            reset();
        period_ = period;
    }

    void bind(PortSpec spec, View<Byte> bytes, std::uint64_t tick)
    {
        if (!bytes.empty() && !bytes.data())
            throw Error(ErrorCode::InvalidArg);
        if (period_ == BlockPeriod{})
            throw Error(ErrorCode::NotReady);
        if (!bytes_.empty())
            throw Error(ErrorCode::Busy);
        const auto bytes_per_channel = spec.frames(period_, tick) * sample_bytes(spec.format);
        // Every supported rate and period produces a nonempty block.
        if (spec.channels > std::numeric_limits<std::size_t>::max() / bytes_per_channel)
            throw Error(ErrorCode::TooLarge);
        if (bytes.size() != spec.channels * bytes_per_channel)
            throw Error(ErrorCode::InvalidArg);
        bytes_ = bytes;
        tick_ = tick;
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
    void prepare(BlockPeriod period) noexcept override;
    // Must be serialized with process(), like reset().
    void bind(View<const std::uint8_t> bytes, std::uint64_t tick);
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    detail::TickBuffer<const std::uint8_t> buffer_;
};
class Playback final : public NodeT<1, 0>
{
  public:
    explicit Playback(PortSpec spec);
    void prepare(BlockPeriod period) noexcept override;
    // Must be serialized with process(), like reset().
    void bind(View<std::uint8_t> bytes, std::uint64_t tick);
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    detail::TickBuffer<std::uint8_t> buffer_;
};
} // namespace test
} // namespace aereborn
#endif
