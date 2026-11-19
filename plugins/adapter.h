#ifndef AEREBORN_PLUGINS_ADAPTER_H
#define AEREBORN_PLUGINS_ADAPTER_H

#include "core/graph.h"
#include <memory>

namespace aereborn
{

class PCMConverter;

// Adapter configuration is fixed for the lifetime of each node. Engine::prepare prepares the converter.
// Borrow interleaved PCM for spec.periods ticks, through the end of the final tick.
// Serialize bind/reset with the tick. Device I/O and queues belong to the backend.
class IAdapter final : public NodeT<1, 1>
{
  public:
    IAdapter(std::size_t channels, PCMSpec pcm, ChMatrix matrix = {});
    ~IAdapter() override;
    std::size_t pcm_frames() const noexcept;
    std::size_t pcm_bytes() const noexcept;
    RetCode bind_pcm(View<const std::uint8_t> bytes) noexcept;

    View<const std::uint8_t> pcm_block(PortDirection, std::size_t) const noexcept override;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    RetCode prepare(const EngineSpec &graph_spec) override;

    const ChMatrix matrix_;
    std::unique_ptr<PCMConverter> converter_;
    View<const std::uint8_t> pcm_{nullptr, 0};
    View<const std::uint8_t> last_slice_{nullptr, 0};
};

class OAdapter final : public NodeT<1, 1>
{
  public:
    OAdapter(std::size_t channels, PCMSpec pcm, ChMatrix matrix = {});
    ~OAdapter() override;
    std::size_t pcm_frames() const noexcept;
    std::size_t pcm_bytes() const noexcept;
    RetCode bind_pcm(View<std::uint8_t> bytes) noexcept;

    View<const std::uint8_t> pcm_block(PortDirection, std::size_t) const noexcept override;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    RetCode prepare(const EngineSpec &graph_spec) override;

    const ChMatrix matrix_;
    std::unique_ptr<PCMConverter> converter_;
    View<std::uint8_t> pcm_{nullptr, 0};
    View<const std::uint8_t> last_slice_{nullptr, 0};
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_ADAPTER_H
