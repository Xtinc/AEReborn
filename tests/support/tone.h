#ifndef AEREBORN_TESTS_TONE_H
#define AEREBORN_TESTS_TONE_H
#include "core/graph.h"
namespace aereborn
{
class Tone final : public NodeT<0, 1>
{
  public:
    /// @copydoc Node::type_name
    const char *type_name() const noexcept override { return "Tone"; }

    Tone(BandWidth rate, std::size_t channels, double frequency = 440, float amplitude = 0.25f);
    void process(const ProcessContext &context) noexcept override;

  private:
    const double frequency_;
    const float amplitude_;
};
} // namespace aereborn
#endif
