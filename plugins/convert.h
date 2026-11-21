#ifndef AEREBORN_PLUGINS_CONVERT_H
#define AEREBORN_PLUGINS_CONVERT_H

#include "core/graph.h"
#include <vector>

namespace aereborn
{

/// @brief Convert sample format and sample rate at a fixed or variable ratio.
/// @note Port specifications stay fixed. Use Matrix to change the channel count.
/// @note Serialize preparation, processing, frame queries and reset.
class Convert final : public NodeT<1, 1>
{
  public:
    /// @brief Source frames consumed and destination frames produced.
    struct Transfer
    {
        std::size_t consumed = 0;
        std::size_t produced = 0;
    };

    /// @brief Construct a converter for the given ports.
    /// @throws std::invalid_argument If either spec is invalid or the channel counts differ.
    Convert(PortSpec input, PortSpec output);

    /// @brief Prepare bounded frame ranges and an allowed relative rate offset in [0, 0.5).
    /// @note Reuse storage and reset phase and history when the new bounds fit and the offset matches.
    /// @note A failed preparation preserves the previous state.
    RetCode prepare(std::size_t max_input_frames, std::size_t max_output_frames, double max_rate_offset = 0.0);

    /// @brief Convert native blocks using the nominal source step multiplied by rate_scale.
    /// @param rate_scale Multiplier within [1 - max_rate_offset, 1 + max_rate_offset].
    /// @pre The input and output buffers do not overlap.
    /// @note Unconsumed input stays with the caller. Only produced output frames are written.
    /// @note Planar views use the configured port's rate and must match its channel count.
    /// @note An empty output leaves conversion state unchanged.
    Result<Transfer> process(IBBlock input, OBBlock output, double rate_scale = 1.0) noexcept;
    Result<Transfer> process(IBBlock input, OFBlock output, double rate_scale = 1.0) noexcept;
    Result<Transfer> process(IFBlock input, OBBlock output, double rate_scale = 1.0) noexcept;
    Result<Transfer> process(IFBlock input, OFBlock output, double rate_scale = 1.0) noexcept;

    /// @brief Query the exact input needed at the current phase. The result may be zero.
    /// @note Pass the same rate_scale to the subsequent process call.
    Result<std::size_t> required_input(std::size_t output_frames, double rate_scale = 1.0) const noexcept;

    /// @brief Reset FIR history and phase, keeping the prepared storage.
    void reset() noexcept override;

    /// @brief Prepare for fixed-ratio graph processing at the given tick period.
    RetCode prepare(BlockPeriod period) override;
    /// @copydoc Node::process
    void process(const ProcessContext &context) noexcept override;

    /// @brief Bound the output workspace for a batch of input frames at the allowed rate offset.
    /// @note A zero batch returns zero. Use required_input() to query output available from history.
    static Result<std::size_t> max_output_frames(PortSpec input, PortSpec output, std::size_t input_frames,
                                                double max_rate_offset = 0.0) noexcept;

    /// @brief Bound the input workspace for a batch of output frames at the allowed rate offset.
    static Result<std::size_t> max_input_frames(PortSpec input, PortSpec output, std::size_t output_frames,
                                               double max_rate_offset = 0.0) noexcept;

  private:
    template <class Byte, class Float> struct NativeBlock;
    struct Position
    {
        std::size_t through = 1;
        std::uint64_t fraction = 0;
        void advance(std::uint64_t step, std::uint64_t unit) noexcept;
    };

    struct Workspace
    {
        Buffer decoded;
        Buffer converted;
        Buffer history;
        std::vector<float> kernel;
    };

    static Position tick_position(std::uint32_t src_fs, std::uint32_t dst_fs, const ProcessContext &context) noexcept;
    RetCode check_output(std::size_t frames, double scale) const noexcept;
    template <class Input, class Output>
    Result<Transfer> process_blocks(Input input, Output output, double rate_scale) noexcept;
    Transfer run(NativeBlock<const std::uint8_t, const float> input, NativeBlock<std::uint8_t, float> output,
                 double scale, const ProcessContext *tick = nullptr) noexcept;

    void (*decode_)(const std::uint8_t *, OFBlock) = nullptr;
    void (*encode_)(IFBlock, std::uint8_t *) = nullptr;
    bool same_spec_ = false;
    bool need_fir_ = false;
    double nominal_ = 0;
    double max_rate_offset_ = 0;
    std::size_t alloc_input_frames_ = 0;
    std::size_t alloc_output_frames_ = 0;
    std::size_t max_input_frames_ = 0;
    std::size_t max_output_frames_ = 0;
    Position position_;
    Workspace workspace_;
};

} // namespace aereborn
#endif
