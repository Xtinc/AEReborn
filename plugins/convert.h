#ifndef AEREBORN_PLUGINS_CONVERT_H
#define AEREBORN_PLUGINS_CONVERT_H
#include "core/graph.h"
#include <memory>
namespace aereborn
{
// Fixed format/rate conversion. Channel counts must match; use Matrix to change them.
class Convert final : public NodeT<1, 1>
{
  public:
    Convert(PortSpec input, PortSpec output);
    ~Convert() override;
    RetCode prepare(BlockPeriod period) override;
    void process(const ProcessContext &context) noexcept override;
    void reset() noexcept override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace aereborn
#endif
