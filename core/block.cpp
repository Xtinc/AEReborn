#include "block.h"
#include <limits>
#include <stdexcept>

namespace aereborn
{

Buffer::Buffer(std::size_t channels, std::size_t capacity_frames)
    : channels_(channels), capacity_frames_(capacity_frames)
{
    if (channels == 0 || capacity_frames == 0 || channels > std::numeric_limits<std::size_t>::max() / capacity_frames)
    {
        throw std::invalid_argument("Buffer requires valid nonzero dimensions");
    }
    samples_.resize(channels * capacity_frames, 0.0f);
}

OBlock Buffer::block(std::size_t frames) noexcept
{
    return {samples_.data(), channels_, frames, capacity_frames_};
}

IBlock Buffer::block(std::size_t frames) const noexcept
{
    return {samples_.data(), channels_, frames, capacity_frames_};
}

} // namespace aereborn
