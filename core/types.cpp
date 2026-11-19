#include "types.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

Result<std::size_t> PCMSpec::transfer_frames(BlockPeriod period) const noexcept
{
    if (!is_pcm_sample_rate(sample_rate) || !is_block_period(period) || periods == 0)
    {
        return ErrorCode::InvalidArg;
    }
    const auto period_frames_x1000 = std::uint64_t{static_cast<std::uint32_t>(sample_rate)} *
                                     static_cast<std::uint32_t>(period);
    if (periods > std::numeric_limits<std::uint64_t>::max() / period_frames_x1000)
    {
        return ErrorCode::TooLarge;
    }
    const auto frames_x1000 = period_frames_x1000 * periods;
    if (frames_x1000 % 1000 != 0)
    {
        return ErrorCode::InvalidArg;
    }
    const auto frames = frames_x1000 / 1000;
    if (frames > std::numeric_limits<std::size_t>::max())
    {
        return ErrorCode::TooLarge;
    }
    return static_cast<std::size_t>(frames);
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

bool ChMatrix::matches(std::size_t input_channels, std::size_t output_channels) const noexcept
{
    return empty() ? input_channels == output_channels
                   : input_channels_ == input_channels && output_channels_ == output_channels;
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

static void mix_ramp_channel(const float *src, float *dst, std::size_t frames, float coefficient,
                             float from, float to, double step) noexcept
{
    if (coefficient == 0.0f)
    {
        return;
    }
    const auto last_frame = frames - 1;
    for (std::size_t frame = 0; frame < last_frame; ++frame)
    {
        const auto gain = static_cast<float>(from + step * (frame + 1));
        const auto scaled_gain = coefficient * gain;
        if (scaled_gain != 0.0f) // Silent contributions also skip non-finite input samples.
        {
            dst[frame] += src[frame] * scaled_gain;
        }
    }
    const auto final_gain = coefficient * to;
    if (final_gain != 0.0f)
    {
        dst[last_frame] += src[last_frame] * final_gain;
    }
}

void ChMatrix::mix_ramp_add(IBlock input, OBlock output, float from, float to) const noexcept
{
    const auto frames = output.frames();
    if (frames == 0)
    {
        return;
    }
    const auto step = (static_cast<double>(to) - from) / frames;
    if (empty())
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            mix_ramp_channel(input.channel(ch), output.channel(ch), frames, 1.0f, from, to, step);
        }
        return;
    }
    for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
    {
        for (std::size_t src_ch = 0; src_ch < input.channels(); ++src_ch)
        {
            mix_ramp_channel(input.channel(src_ch), output.channel(dst_ch), frames,
                             coefficients_[dst_ch * input.channels() + src_ch], from, to, step);
        }
    }
}

} // namespace aereborn
