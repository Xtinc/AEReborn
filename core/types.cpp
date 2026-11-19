#include "types.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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
    samples_.resize(channels * capacity_frames);
    std::fill(samples_.begin(), samples_.end(), 0.0f);
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

ChMatrix::ChMatrix(std::size_t output_channels, std::size_t input_channels, View<const float> coefficients)
    : input_channels_(input_channels), output_channels_(output_channels)
{
    if (input_channels == 0 || output_channels == 0)
    {
        throw std::invalid_argument("Channel matrix dimensions must be positive");
    }
    if (output_channels > coefficients_.max_size() / input_channels)
    {
        throw std::length_error("Channel matrix exceeds the supported size");
    }
    if (coefficients.size() != output_channels * input_channels)
    {
        throw std::invalid_argument("Channel matrix coefficient count does not match its dimensions");
    }
    for (const auto coefficient : coefficients)
    {
        if (!std::isfinite(coefficient))
        {
            throw std::invalid_argument("Channel matrix coefficients must be finite");
        }
    }
    coefficients_.assign(coefficients.begin(), coefficients.end());
}

ChMatrix::ChMatrix(std::size_t output_channels, std::size_t input_channels, std::initializer_list<float> coefficients)
    : ChMatrix(output_channels, input_channels, View<const float>{coefficients.begin(), coefficients.size()})
{
}

bool ChMatrix::empty() const noexcept
{
    return coefficients_.empty();
}

std::size_t ChMatrix::input_channels() const noexcept
{
    return input_channels_;
}

std::size_t ChMatrix::output_channels() const noexcept
{
    return output_channels_;
}

const std::vector<float> &ChMatrix::coefficients() const noexcept
{
    return coefficients_;
}

static void mix_channel(IBlock input, OBlock output, std::size_t src_ch, std::size_t dst_ch, float gain) noexcept
{
    if (gain == 0.0f)
    {
        return;
    }
    const auto *src = input.channel(src_ch);
    auto *dst = output.channel(dst_ch);
    for (std::size_t frame = 0; frame < output.frames(); ++frame)
    {
        dst[frame] += src[frame] * gain;
    }
}

void ChMatrix::mix_add(IBlock input, OBlock output, float gain) const noexcept
{
    if (empty())
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            mix_channel(input, output, ch, ch, gain);
        }
        return;
    }
    for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
    {
        for (std::size_t src_ch = 0; src_ch < input.channels(); ++src_ch)
        {
            mix_channel(input, output, src_ch, dst_ch, coefficients_[dst_ch * input.channels() + src_ch] * gain);
        }
    }
}

} // namespace aereborn
