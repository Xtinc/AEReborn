#ifndef AEREBORN_PLUGINS_CONVERT_H
#define AEREBORN_PLUGINS_CONVERT_H
#include "core/graph.h"
#include <memory>
namespace aereborn
{
/// @brief Convert the sample format, sample rate, or both.
/// @note Use Matrix to change the channel count.
class Convert final : public NodeT<1, 1>
{
  public:
    /// @brief Construct a converter with fixed port specifications.
    /// @param input Source port specification.
    /// @param output Destination port specification.
    /// @throws std::invalid_argument If either specification is invalid or channel counts differ.
    Convert(PortSpec input, PortSpec output);
    ~Convert() override;

    /// @brief Prepare the workspace required by the selected conversion.
    /// @param period Block period validated by the engine.
    /// @return Success or a workspace sizing error.
    /// @note Allocation failures propagate. Failed preparation preserves the previous state.
    RetCode prepare(BlockPeriod period) override;
    /// @copydoc Node::process
    void process(const ProcessContext &context) noexcept override;

    /// @brief Clear the resampler history, if prepared.
    /// @pre Reset must be serialized with processing.
    void reset() noexcept override;

  private:
    using DecodeFn = void (*)(const std::uint8_t *, OFBlock);
    using EncodeFn = void (*)(IFBlock, std::uint8_t *);
    using Codec = std::pair<DecodeFn, EncodeFn>;
    struct State;
    static Codec codec_for(SampleFormat format) noexcept;

    const DecodeFn decode_;
    const EncodeFn encode_;
    std::unique_ptr<State> state_;
};
} // namespace aereborn
#endif
