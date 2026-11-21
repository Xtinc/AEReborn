#ifndef AEREBORN_PLUGINS_DRIFT_H
#define AEREBORN_PLUGINS_DRIFT_H

#include "core/types.h"

namespace aereborn
{

/// @brief Device-side PI controller for the mean logical queue fill.
class DriftController
{
  public:
    double correction() const noexcept { return correction_; }

    /// @brief Return the correction limit in ppm for integer queue sizing.
    static std::uint64_t max_correction_ppm() noexcept;
    /// @brief Return the same limit as a fractional rate offset for Convert.
    static double max_correction() noexcept;

    void update(std::size_t before, std::size_t after, std::size_t target, SampleRate logical_rate,
                std::size_t device_frames, SampleRate device_rate) noexcept;

  private:
    double filtered_ = 0;
    double integral_ = 0;
    double correction_ = 0;
};

} // namespace aereborn

#endif
