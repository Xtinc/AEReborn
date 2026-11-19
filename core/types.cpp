#include "types.h"

#include <algorithm>

namespace aereborn
{

RetCode Buffer::resize(std::size_t channels, std::size_t capacity_frames)
{
    if (channels == 0 || capacity_frames == 0)
    {
        return ErrorCode::InvalidArg;
    }
    if (channels > samples_.max_size() / capacity_frames)
    {
        return ErrorCode::TooLarge;
    }
    const auto sample_count = channels * capacity_frames;
    const auto retained_samples = std::min(samples_.size(), sample_count);
    samples_.resize(sample_count);
    std::fill_n(samples_.begin(), retained_samples, 0.0f);
    channels_ = channels;
    capacity_frames_ = capacity_frames;
    return {};
}

OBlock Buffer::block(std::size_t frames) noexcept
{
    return {samples_.data(), channels_, frames, capacity_frames_};
}

std::size_t Buffer::capacity_frames() const noexcept
{
    return capacity_frames_;
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
    const auto start = (tick % 1000) * fraction;
    return static_cast<std::size_t>(units / 1000 + (start + fraction + 999) / 1000 - (start + 999) / 1000);
}

std::uint64_t PortSpec::frame_position(BlockPeriod period, std::uint64_t tick) const noexcept
{
    const auto units = std::uint64_t{static_cast<std::uint32_t>(sample_rate)} * static_cast<std::uint32_t>(period);
    // Split whole seconds from the remainder to avoid multiplying tick by the sample rate.
    return (tick / 1000) * units + ((tick % 1000) * units + 999) / 1000;
}

NodeId::operator bool() const noexcept
{
    return value != 0;
}

bool NodeId::operator==(NodeId other) const noexcept
{
    return value == other.value;
}

bool NodeId::operator!=(NodeId other) const noexcept
{
    return !(*this == other);
}

bool NodeId::operator<(NodeId other) const noexcept
{
    return value < other.value;
}

ConnId::operator bool() const noexcept
{
    return value != 0;
}

bool ConnId::operator==(ConnId other) const noexcept
{
    return value == other.value;
}

bool ConnId::operator!=(ConnId other) const noexcept
{
    return !(*this == other);
}

bool ConnId::operator<(ConnId other) const noexcept
{
    return value < other.value;
}

} // namespace aereborn
