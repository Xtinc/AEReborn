#include "types.h"

#include <algorithm>

namespace aereborn
{

void Buffer::resize(std::size_t channels, std::size_t capacity_frames)
{
    if (channels == 0 || capacity_frames == 0)
        throw Error(ErrorCode::InvalidArg);
    if (channels > samples_.max_size() / capacity_frames)
        throw Error(ErrorCode::TooLarge);
    const auto sample_count = channels * capacity_frames;
    const auto retained_samples = std::min(samples_.size(), sample_count);
    samples_.resize(sample_count);
    std::fill_n(samples_.begin(), retained_samples, 0.0f);
    channels_ = channels;
    capacity_frames_ = capacity_frames;
}

OFBlock Buffer::block(std::size_t frames) noexcept
{
    return {samples_.data(), channels_, frames, capacity_frames_};
}

void Buffer::clear() noexcept
{
    std::fill(samples_.begin(), samples_.end(), 0.0f);
}

std::size_t PortSpec::max_frames(BlockPeriod period) const noexcept
{
    const auto units = std::uint64_t{static_cast<std::uint32_t>(sample_rate)} * static_cast<std::uint32_t>(period);
    return static_cast<std::size_t>((units + 999) / 1000);
}

std::size_t PortSpec::frames(BlockPeriod period, std::uint64_t tick) const noexcept
{
    const auto units = std::uint64_t{static_cast<std::uint32_t>(sample_rate)} * static_cast<std::uint32_t>(period);
    const auto fraction = units % 1000;
    if (fraction == 0)
        return static_cast<std::size_t>(units / 1000);
    const auto start = (tick % 1000) * fraction;
    return static_cast<std::size_t>(units / 1000 + (start + fraction + 999) / 1000 - (start + 999) / 1000);
}

std::uint64_t PortSpec::frame_position(BlockPeriod period, std::uint64_t tick) const noexcept
{
    const auto units = std::uint64_t{static_cast<std::uint32_t>(sample_rate)} * static_cast<std::uint32_t>(period);
    // Split whole seconds from the remainder to avoid multiplying tick by the sample rate.
    return (tick / 1000) * units + ((tick % 1000) * units + 999) / 1000;
}

} // namespace aereborn
