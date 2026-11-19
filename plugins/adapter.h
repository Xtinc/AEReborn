#ifndef AEREBORN_PLUGINS_ADAPTER_H
#define AEREBORN_PLUGINS_ADAPTER_H

#include "core/graph.h"
#include <memory>

namespace aereborn
{

class PCMConverter;

// Fixed graph channels; configure on a non-audio thread, serialized with bind/reset/process.
// Borrow interleaved PCM for spec.periods process calls. Device I/O and queues belong to the backend.
class IAdapter final : public NodeT<0, 1>
{
  public:
    IAdapter(std::size_t channels, EngineSpec graph_spec);
    ~IAdapter() override;
    std::size_t pcm_frames() const noexcept;
    std::size_t pcm_bytes() const noexcept;
    RetCode bind_pcm(View<const std::uint8_t> bytes) noexcept;

    // A failed configuration preserves the active converter and borrowed PCM.
    RetCode configure_pcm(PCMSpec spec, const ChMatrix &matrix = {});
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    RetCode prepare(const EngineSpec &graph_spec) noexcept override;

    const EngineSpec graph_spec_;
    std::unique_ptr<PCMConverter> converter_;
    const std::uint8_t *pcm_ = nullptr;
};

class OAdapter final : public NodeT<1, 0>
{
  public:
    OAdapter(std::size_t channels, EngineSpec graph_spec);
    ~OAdapter() override;
    std::size_t pcm_frames() const noexcept;
    std::size_t pcm_bytes() const noexcept;
    RetCode bind_pcm(View<std::uint8_t> bytes) noexcept;

    RetCode configure_pcm(PCMSpec spec, const ChMatrix &matrix = {});
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    RetCode prepare(const EngineSpec &graph_spec) noexcept override;

    const EngineSpec graph_spec_;
    std::unique_ptr<PCMConverter> converter_;
    std::uint8_t *pcm_ = nullptr;
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_ADAPTER_H
