#include "drift.h"
#include <algorithm>
#include <cmath>

namespace aereborn
{
namespace
{
constexpr std::uint64_t clock_correction_ppm = 2000;
constexpr double max_clock_correction = clock_correction_ppm / 1000000.0;
} // namespace

std::uint64_t DriftController::max_correction_ppm() noexcept
{
    return clock_correction_ppm;
}

double DriftController::max_correction() noexcept
{
    return max_clock_correction;
}

void DriftController::update(std::size_t before, std::size_t after, std::size_t target, SampleRate logical_rate,
                             std::size_t device_frames, SampleRate device_rate) noexcept
{
    constexpr double filter_seconds = 0.25;
    constexpr double proportional_gain = 0.5;
    constexpr double integral_gain = 0.06;

    const auto dt = static_cast<double>(device_frames) / static_cast<std::uint32_t>(device_rate);
    const auto fill = (static_cast<double>(before) + static_cast<double>(after)) * 0.5;
    const auto error = (fill - static_cast<double>(target)) / static_cast<std::uint32_t>(logical_rate);
    filtered_ += -std::expm1(-dt / filter_seconds) * (error - filtered_);
    const auto proportional = proportional_gain * filtered_;
    const auto integral = integral_ + integral_gain * filtered_ * dt;
    const auto requested = proportional + integral;
    if (std::abs(requested) <= max_clock_correction || requested * filtered_ < 0)
        integral_ = integral;
    correction_ = std::max(-max_clock_correction, std::min(max_clock_correction, proportional + integral_));
}

} // namespace aereborn
