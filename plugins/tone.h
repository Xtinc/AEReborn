#ifndef AEREBORN_PLUGINS_TONE_H
#define AEREBORN_PLUGINS_TONE_H
#include "core/graph.h"
namespace aereborn
{
class Tone final : public NodeT<0, 1>
{
  public:
    Tone(SampleRate rate, std::size_t channels, double frequency = 440, float amplitude = 0.25f);
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &context) noexcept override;

  private:
    const double frequency_;
    const float amplitude_;
};
} // namespace aereborn
#endif
