#ifndef AEREBORN_PLUGINS_MIXER_H
#define AEREBORN_PLUGINS_MIXER_H
#include "core/graph.h"
#include <atomic>
#include <cstdint>
#include <vector>
namespace aereborn
{
/// @brief Mix F32P inputs with optional high-priority suppression.
/// @note All ports have the same sample rate, channel count, and format.
/// @note Any active, unmuted high-priority input suppresses normal inputs over one block.
class Mixer final : public Node
{
  public:
    static constexpr std::size_t max_inputs = 64;

    /// @copydoc Node::type_name
    const char *type_name() const noexcept override { return "Mixer"; }

    /// @brief Construct a mixer with all inputs unmuted at normal priority.
    /// @param rate Sample rate of every port.
    /// @param channels Channel count of every port.
    /// @param inputs Number of input ports in [1, max_inputs].
    /// @throws Error RetCode::EPARAM If the port specification is invalid or inputs is zero.
    /// @throws Error RetCode::TOOLARGE if inputs exceeds max_inputs.
    Mixer(BandWidth rate, std::size_t channels, std::size_t inputs);

    /// @copydoc Node::inputs
    /// @note The view borrows port specifications from this mixer and remains valid until destruction.
    View<const PortSpec> inputs() const noexcept override;
    /// @copydoc Node::outputs
    /// @note The view borrows the port specification from this mixer and remains valid until destruction.
    View<const PortSpec> outputs() const noexcept override;

    /// @brief Set whether one input is muted.
    /// @param input Zero-based input port index.
    /// @param muted True to mute the input, false to unmute it.
    /// @throws Error RetCode::EPORT if the index is out of range.
    /// @note Safe to call concurrently with process(). Each nonempty block reads one mute mask.
    void set_muted(std::size_t input, bool muted);

    /// @brief Read the requested mute state of one input.
    /// @param input Zero-based input port index.
    /// @return The mute value.
    /// @throws Error RetCode::EPORT if the index is out of range.
    /// @note Safe to call concurrently with set_muted() and process().
    bool muted(std::size_t input) const;

    /// @brief Set whether an input has high priority.
    /// @param input Zero-based input port index.
    /// @param high True for high priority, false for normal priority.
    /// @throws Error RetCode::EPORT if the index is out of range.
    /// @note Safe to call concurrently with process(). High-priority inputs mix together.
    void set_high_priority(std::size_t input, bool high);

    /// @brief Read the requested priority of an input.
    /// @param input Zero-based input port index.
    /// @return True if the input has high priority.
    /// @throws Error RetCode::EPORT if the index is out of range.
    /// @note Safe to call concurrently with set_high_priority() and process().
    bool high_priority(std::size_t input) const;

    /// @brief Report whether an active, unmuted input was suppressed in the latest nonempty block.
    /// @param input Zero-based input port index.
    /// @return True during the fade-out and steady suppression blocks.
    /// @throws Error RetCode::EPORT if the index is out of range.
    /// @note Safe to call concurrently with process().
    bool suppressed(std::size_t input) const;

    /// @brief Initialize runtime suppression state on the control thread.
    void prepare(BlockPeriod period) override;

    /// @copydoc Node::process
    /// @note Each nonempty block reads the mute and priority masks once.
    void process(const ProcessContext &context) noexcept override;

    /// @brief Clear runtime suppression while keeping the requested mute and priority settings.
    /// @pre Serialize with process().
    void reset() noexcept override;

  private:
    const PortSpec output_;
    const std::vector<PortSpec> inputs_;
    std::atomic<std::uint64_t> muted_{0};
    std::atomic<std::uint64_t> high_{0};
    std::atomic<std::uint64_t> suppressed_{0};
    std::uint64_t was_suppressed_ = 0;
};
} // namespace aereborn
#endif
