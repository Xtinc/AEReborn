#include "virtual.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aereborn
{
namespace
{
RetCode resize_pcm(PCMSpec spec, BlockPeriod period, std::vector<std::uint8_t> &pcm)
{
    if (!pcm.empty())
    {
        return {};
    }
    const auto period_ms = static_cast<std::uint32_t>(period);
    const auto sample_bytes = pcm_sample_bytes(spec.format);
    if (!is_pcm_sample_rate(spec.sample_rate) || sample_bytes == 0 || spec.channels == 0 || spec.periods == 0 ||
        !is_block_period(period))
    {
        return ErrorCode::InvalidArg;
    }
    const auto period_frames_x1000 = std::uint64_t{static_cast<std::uint32_t>(spec.sample_rate)} * period_ms;
    if (spec.periods > std::numeric_limits<std::uint64_t>::max() / period_frames_x1000)
    {
        return ErrorCode::TooLarge;
    }
    const auto frames_x1000 = period_frames_x1000 * spec.periods;
    if (frames_x1000 % 1000 != 0)
    {
        return ErrorCode::InvalidArg;
    }
    const auto frames = frames_x1000 / 1000;
    if (spec.channels > pcm.max_size() / sample_bytes / frames)
    {
        return ErrorCode::TooLarge;
    }
    pcm.resize(static_cast<std::size_t>(frames) * spec.channels * sample_bytes);
    return {};
}

// Generate the requested wire representation directly, independently of Adapter's conversion code.
void write_sample(float sample, PCMFormat format, std::uint8_t *output) noexcept
{
    std::uint32_t word;
    if (format == PCMFormat::F32LE)
    {
        std::memcpy(&word, &sample, sizeof(word));
    }
    else
    {
        const auto bits = pcm_sample_bits(format);
        const auto scale = std::ldexp(1.0, bits - 1);
        const auto scaled = std::round(static_cast<double>(sample) * scale);
        const auto clamped = std::max(-scale, std::min(scale - 1, scaled));
        word = static_cast<std::uint32_t>(static_cast<std::int64_t>(clamped));
        if (bits < 32)
        {
            word &= (std::uint32_t{1} << bits) - 1;
        }
    }
    for (std::size_t byte = 0; byte < pcm_sample_bytes(format); ++byte)
    {
        output[byte] = static_cast<std::uint8_t>(word >> (8 * byte));
    }
}
} // namespace

VirtualSource::VirtualSource(PCMSpec spec, BlockPeriod period, double frequency_hz, float amplitude)
    : spec_(spec), period_(period), frequency_hz_(frequency_hz), amplitude_(amplitude)
{
}

RetCode VirtualSource::prepare()
{
    const auto sample_rate = static_cast<std::uint32_t>(spec_.sample_rate);
    if (!(frequency_hz_ >= 0 && frequency_hz_ < sample_rate / 2.0) || !std::isfinite(amplitude_))
    {
        return ErrorCode::InvalidArg;
    }
    return resize_pcm(spec_, period_, pcm_);
}

RetCode VirtualSource::generate() noexcept
{
    if (pcm_.empty())
    {
        return ErrorCode::NotPrepared;
    }
    constexpr double two_pi = 6.28318530717958647692;
    const auto step = two_pi * frequency_hz_ / static_cast<std::uint32_t>(spec_.sample_rate);
    const auto phase = std::fmod(static_cast<double>(frame_position_) * step, two_pi);
    const auto sample_bytes = pcm_sample_bytes(spec_.format);
    const auto frames = pcm_frames();
    auto *output = pcm_.data();
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto sample = amplitude_ * static_cast<float>(std::sin(phase + frame * step));
        for (std::size_t ch = 0; ch < spec_.channels; ++ch)
        {
            write_sample(sample, spec_.format, output);
            output += sample_bytes;
        }
    }
    frame_position_ += frames;
    generated_ = true;
    return {};
}

std::size_t VirtualSource::pcm_frames() const noexcept
{
    return pcm_.empty() ? 0 : pcm_.size() / spec_.channels / pcm_sample_bytes(spec_.format);
}

void VirtualSource::reset() noexcept
{
    frame_position_ = 0;
    generated_ = false;
}

View<const std::uint8_t> VirtualSource::pcm() const noexcept
{
    return {pcm_.data(), generated_ ? pcm_.size() : 0};
}

VirtualSink::VirtualSink(PCMSpec spec, BlockPeriod period) : spec_(spec), period_(period)
{
}

RetCode VirtualSink::prepare()
{
    return resize_pcm(spec_, period_, pcm_);
}

RetCode VirtualSink::receive(View<const std::uint8_t> pcm) noexcept
{
    if (pcm_.empty())
    {
        return ErrorCode::NotPrepared;
    }
    if (pcm.size() != pcm_.size())
    {
        return ErrorCode::InvalidArg;
    }
    std::copy(pcm.begin(), pcm.end(), pcm_.begin());
    received_ = true;
    return {};
}

void VirtualSink::reset() noexcept
{
    received_ = false;
}

View<const std::uint8_t> VirtualSink::pcm() const noexcept
{
    return {pcm_.data(), received_ ? pcm_.size() : 0};
}

bool VirtualSink::matches(View<const std::uint8_t> expected) const noexcept
{
    return received_ && expected.size() == pcm_.size() && std::equal(expected.begin(), expected.end(), pcm_.begin());
}

} // namespace aereborn
