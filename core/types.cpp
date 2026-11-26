#include "types.h"

#include <algorithm>

namespace aereborn
{

void Buffer::resize(std::size_t channels, std::size_t capacity_frames)
{
    if (channels == 0 || capacity_frames == 0)
        throw Error(RetCode::EPARAM);
    if (channels > samples_.max_size() / capacity_frames)
        throw Error(RetCode::TOOLARGE);
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

constexpr std::uint64_t NodeId::port_mask;
constexpr std::uint64_t NodeId::max_node;
constexpr std::size_t NodeId::invalid_port;

bool PortSpec::valid() const noexcept
{
    return is_bandwidth(bandwidth) && channels > 0 && sample_bytes(format) != 0;
}

} // namespace aereborn
