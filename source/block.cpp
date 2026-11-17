#include "block.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aereborn
{

void OBlock::clear() const noexcept
{
    for (std::size_t ch = 0; ch < channels_; ++ch)
    {
        std::fill_n(channel(ch), frames_, 0.0f);
    }
}

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
    assert(frames <= capacity_frames_);
    return {samples_.data(), channels_, frames, capacity_frames_};
}

IBlock Buffer::block(std::size_t frames) const noexcept
{
    assert(frames <= capacity_frames_);
    return {samples_.data(), channels_, frames, capacity_frames_};
}

} // namespace aereborn