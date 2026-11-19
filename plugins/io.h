#ifndef AEREBORN_PLUGINS_IO_H
#define AEREBORN_PLUGINS_IO_H
#include "core/graph.h"
namespace aereborn
{
namespace detail
{
template <class Byte> class TickBuffer
{
  public:
    RetCode prepare(BlockPeriod period) noexcept;
    RetCode bind(PortSpec spec, View<Byte> bytes, std::uint64_t tick) noexcept;
    View<Byte> take(std::uint64_t tick) noexcept;
    void reset() noexcept { bytes_ = {nullptr, 0}; }

  private:
    BlockPeriod period_{};
    View<Byte> bytes_{nullptr, 0};
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
    void process(const ProcContext &context) noexcept override;
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
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    detail::TickBuffer<std::uint8_t> buffer_;
};
} // namespace aereborn
#endif
