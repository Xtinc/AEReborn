#ifndef AEREBORN_PLUGINS_CONVERT_H
#define AEREBORN_PLUGINS_CONVERT_H

#include "core/graph.h"
#include "core/resampler.h"

namespace aereborn
{

/// @brief Convert sample format and sample rate synchronously inside an Engine graph.
/// @note Port specifications stay fixed. Use Matrix to change the channel count.
class Convert final : public NodeT<1, 1>
{
  public:
    /// @copydoc Node::type_name
    const char *type_name() const noexcept override { return "Convert"; }

    /// @brief Construct a converter with fixed input and output specifications.
    /// @param input Source sample format, sample rate, and channel count.
    /// @param output Destination specification with the same channel count.
    /// @throws Error InvalidArg If either spec is invalid or the channel counts differ.
    Convert(PortSpec input, PortSpec output);

    /// @brief Prepare conversion buffers and reset the resampler for the graph period.
    /// @param period Graph block period used to size the buffers.
    /// @throws Error InvalidArg for an unsupported period, or TooLarge if a buffer exceeds its limit.
    /// @note A failed preparation preserves the existing buffers and resampler state.
    void prepare(BlockPeriod period) override;

    /// @copydoc Node::process
    /// @note Resampling aligns its fractional phase to the graph tick.
    void process(const ProcessContext &context) noexcept override;

    /// @brief Clear resampling history and phase while keeping prepared storage.
    /// @pre Serialize with process().
    void reset() noexcept override;

  private:
    bool same_spec_ = false;
    SincResampler resampler_;
    Buffer decoded_;   ///< Allocated only when input is PCM and (resampler_.needs_fir() || output is different PCM).
    Buffer resampled_; ///< Allocated only when resampler_.needs_fir() && output is PCM.
};

} // namespace aereborn

#endif
