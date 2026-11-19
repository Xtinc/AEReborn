#ifndef AEREBORN_PLUGINS_TONE_H
#define AEREBORN_PLUGINS_TONE_H
#include "core/graph.h"
namespace aereborn
{
class Tone final : public NodeT<0, 1>
{
  public:
    Tone(SampleRate rate, std::size_t channels, double frequency = 440, float amplitude = 0.25f);
    RetCode prepare(BlockPeriod period) override;
    void process(const ProcContext &context) noexcept override;

  private:
    const double frequency_;
    const float amplitude_;
    BlockPeriod period_{};
};
} // namespace aereborn
#endif
