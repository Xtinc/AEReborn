#ifndef AEREBORN_PLUGINS_MIXER_H
#define AEREBORN_PLUGINS_MIXER_H
#include "core/graph.h"
#include <atomic>
#include <deque>
#include <vector>
namespace aereborn
{
/// @brief Mix a fixed number of inputs into one F32P output.
/// @note All ports have the same sample rate, channel count, and format.
class Mixer final : public Node
{
  public:
    /// @brief Construct a mixer with all inputs unmuted.
    /// @param rate Sample rate of every port.
    /// @param channels Channel count of every port.
    /// @param inputs Number of input ports.
    /// @throws Error InvalidArg If the port specification is invalid or inputs is zero.
    /// @throws Error TooLarge if the input count exceeds storage limits.
    Mixer(SampleRate rate, std::size_t channels, std::size_t inputs);

    /// @copydoc Node::inputs
    /// @note The view borrows port specifications from this mixer and remains valid until destruction.
    View<const PortSpec> inputs() const noexcept override;
    /// @copydoc Node::outputs
    /// @note The view borrows the port specification from this mixer and remains valid until destruction.
    View<const PortSpec> outputs() const noexcept override;

    /// @brief Set whether one input is muted.
    /// @param input Zero-based input port index.
    /// @param muted True to mute the input, false to unmute it.
    /// @throws Error InvalidPort if the index is out of range.
    /// @note Safe to call concurrently with process(). Each non-empty input block uses one mute value.
    void set_muted(std::size_t input, bool muted);

    /// @brief Read the requested mute state of one input.
    /// @param input Zero-based input port index.
    /// @return The mute value.
    /// @throws Error InvalidPort if the index is out of range.
    /// @note Safe to call concurrently with set_muted() and process().
    bool muted(std::size_t input) const;

    /// @copydoc Node::process
    /// @note Sums unmuted inputs, leaving silence when every input is muted.
    void process(const ProcessContext &context) noexcept override;

  private:
    const PortSpec output_;
    const std::vector<PortSpec> inputs_;
    std::deque<std::atomic<bool>> muted_;
};
} // namespace aereborn
#endif
