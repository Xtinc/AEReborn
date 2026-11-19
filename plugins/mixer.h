#ifndef AEREBORN_PLUGINS_MIXER_H
#define AEREBORN_PLUGINS_MIXER_H
#include "core/graph.h"
#include <atomic>
#include <memory>
#include <vector>
namespace aereborn
{
// Fixed input count and one F32P output; all ports have exactly the same specification.
class Mixer final : public Node
{
  public:
    Mixer(SampleRate rate, std::size_t channels, std::size_t inputs);
    View<const PortSpec> inputs() const noexcept override;
    View<const PortSpec> outputs() const noexcept override;
    RetCode set_muted(std::size_t input, bool muted) noexcept;
    RetCode prepare(BlockPeriod period) override;
    void process(const ProcContext &context) noexcept override;
    void reset() noexcept override;

  private:
    const PortSpec output_;
    const std::vector<PortSpec> inputs_;
    std::unique_ptr<std::atomic<bool>[]> muted_;
    std::vector<float> gains_;
};
} // namespace aereborn
#endif
