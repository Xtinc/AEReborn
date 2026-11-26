#ifndef AEREBORN_CORE_RESAMPLER_H
#define AEREBORN_CORE_RESAMPLER_H

#include "types.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aereborn
{

constexpr std::uint64_t max_clock_drift_ppm = 2000;

/// @brief Decode interleaved PCM frames into planar F32P samples.
/// @pre is_interleaved_pcm(input.spec().format), matching channels and frames, non-overlapping buffers.
void decode_pcm(IBBlock input, OFBlock output) noexcept;

/// @brief Encode planar F32P samples into interleaved PCM frames.
/// @pre is_interleaved_pcm(output.spec().format), matching channels and frames, non-overlapping buffers.
void encode_pcm(IFBlock input, OBBlock output) noexcept;

/// @brief Polyphase windowed-sinc resampler operating on planar F32P blocks.
class SincResampler
{
  public:
    struct Transfer
    {
        std::size_t consumed = 0;
        std::size_t produced = 0;
    };

    /// @brief Prepare filter kernel and history for channels, rates, and rate offset in [0, 0.5).
    /// @note Replaces filter storage and resets state on success; failure preserves previous state.
    void prepare(std::size_t channels, BandWidth src_fs, BandWidth dst_fs, double max_rate_offset = 0.0);

    /// @brief Zero input frames sufficient to flush the FIR history at end of stream.
    std::size_t tail_frames() const noexcept;

    /// @brief Clear FIR history and reset stream phase while keeping prepared storage.
    void reset() noexcept;

    /// @brief True when rate conversion or non-zero rate offset requires FIR interpolation.
    bool needs_fir() const noexcept { return need_fir_; }

    /// @brief Synchronous graph tick with exact rational phase alignment.
    /// @pre Prepared for a fixed ratio. Blocks match the prepared channels and the tick geometry.
    void process_tick(IFBlock input, OFBlock output, BlockPeriod period, std::uint64_t tick) noexcept;

    /// @brief Exact input frames needed at current phase to produce output_frames.
    /// @pre Prepared, rate_scale within the prepared range, and frame counts within the supported bounds.
    std::size_t required_input(std::size_t output_frames, double rate_scale = 1.0) const noexcept;

    /// @brief Variable-rate streaming resample at nominal step * rate_scale.
    /// @pre Prepared, rate_scale within the prepared range, and frame counts within the supported bounds.
    /// @pre Valid input and output blocks match the prepared channel count and do not overlap.
    /// @note With the same scale and phase, all offered input is consumed if its size
    ///       does not exceed required_input(output.frames(), rate_scale).
    Transfer process(IFBlock input, OFBlock output, double rate_scale = 1.0) noexcept;

    /// @brief Bound streaming output for the given input batch and rate offset.
    /// @throws Error RetCode::EPARAM for invalid rates or offset, or RetCode::TOOLARGE for an unsupported frame count.
    static std::size_t max_output_frames(BandWidth src_fs, BandWidth dst_fs, std::size_t input_frames,
                                         double max_rate_offset = 0.0);
    /// @brief Bound streaming input for the given output batch and rate offset.
    /// @throws Error RetCode::EPARAM for invalid rates or offset, or RetCode::TOOLARGE for an unsupported frame count.
    static std::size_t max_input_frames(BandWidth src_fs, BandWidth dst_fs, std::size_t output_frames,
                                        double max_rate_offset = 0.0);

  private:
    struct Position
    {
        std::size_t through = 1;
        std::uint64_t fraction = 0;
        void advance(std::uint64_t step, std::uint64_t unit) noexcept;
    };

    Transfer run(IFBlock input, OFBlock output, Position &phase, std::uint64_t step,
                 std::uint64_t unit, bool tick) noexcept;

    bool need_fir_ = false;
    std::size_t channels_ = 0;
    BandWidth src_fs_{};
    BandWidth dst_fs_{};
    double nominal_ = 0.0;
    double max_rate_offset_ = 0.0;
    Position position_;
    Buffer history_;
    std::vector<float> kernel_;
};

} // namespace aereborn

#endif
