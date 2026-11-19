#ifndef AEREBORN_PLUGINS_IO_H
#define AEREBORN_PLUGINS_IO_H
#include "core/graph.h"
namespace aereborn
{
// Borrow exactly one tick's bytes. Serialize bind/reset with processing and keep the memory
// alive through the specified tick. Device I/O, queues and longer transfers belong to the backend.
class Capture final : public NodeT<0, 1>
{
  public:
    explicit Capture(PortSpec spec) : NodeT({}, {{spec}}) {}
    RetCode prepare(BlockPeriod period) override;
    RetCode bind(View<const std::uint8_t> bytes, std::uint64_t tick) noexcept;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    BlockPeriod period_{};
    View<const std::uint8_t> bytes_{nullptr, 0};
    std::uint64_t tick_ = 0;
    bool bound_ = false;
};
class Playback final : public NodeT<1, 0>
{
  public:
    explicit Playback(PortSpec spec) : NodeT({{spec}}, {}) {}
    RetCode prepare(BlockPeriod period) override;
    RetCode bind(View<std::uint8_t> bytes, std::uint64_t tick) noexcept;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    BlockPeriod period_{};
    View<std::uint8_t> bytes_{nullptr, 0};
    std::uint64_t tick_ = 0;
    bool bound_ = false;
};
} // namespace aereborn
#endif
