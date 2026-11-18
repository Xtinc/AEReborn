#ifndef AEREBORN_CORE_TYPES_H
#define AEREBORN_CORE_TYPES_H

#include "block.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <utility>
#include <vector>

namespace aereborn
{

enum class SampleRate : std::uint32_t
{
    Hz8000 = 8000,
    Hz11025 = 11025,
    Hz16000 = 16000,
    Hz22050 = 22050,
    Hz24000 = 24000,
    Hz32000 = 32000,
    Hz44100 = 44100,
    Hz48000 = 48000,
    Hz88200 = 88200,
    Hz96000 = 96000
};

constexpr bool is_pcm_sample_rate(SampleRate rate) noexcept
{
    switch (rate)
    {
    case SampleRate::Hz8000:
    case SampleRate::Hz11025:
    case SampleRate::Hz16000:
    case SampleRate::Hz22050:
    case SampleRate::Hz24000:
    case SampleRate::Hz32000:
    case SampleRate::Hz44100:
    case SampleRate::Hz48000:
    case SampleRate::Hz88200:
    case SampleRate::Hz96000:
        return true;
    }
    return false;
}

enum class BlockPeriod : std::uint32_t
{
    Ms05 = 5,
    Ms10 = 10,
    Ms20 = 20,
    Ms40 = 40
};

template <SampleRate Rate, BlockPeriod Period> constexpr std::size_t calc_block_frames() noexcept
{
    constexpr auto sample_rate_hz = static_cast<std::uint32_t>(Rate);
    constexpr auto period_ms = static_cast<std::uint32_t>(Period);
    static_assert(sample_rate_hz > 0 && Rate <= SampleRate::Hz96000, "Sample rate must be positive and at most 96 kHz");
    static_assert(Period == BlockPeriod::Ms05 || Period == BlockPeriod::Ms10 || Period == BlockPeriod::Ms20 ||
                      Period == BlockPeriod::Ms40,
                  "Unsupported block period");
    constexpr auto frames_x1000 = static_cast<std::uint64_t>(sample_rate_hz) * period_ms;
    static_assert(frames_x1000 % 1000 == 0, "Sample rate and block period must produce an integer frame count");
    return static_cast<std::size_t>(frames_x1000 / 1000);
}

enum class PCMFormat
{
    S16LE,
    S24_3LE, // Packed into three bytes.
    S24LE,   // Low 24 bits of a four-byte sample.
    S32LE,
    F32LE
};

// Sample width before padding, or zero for an unknown format.
constexpr unsigned int pcm_sample_bits(PCMFormat format) noexcept
{
    switch (format)
    {
    case PCMFormat::S16LE:
        return 16;
    case PCMFormat::S24_3LE:
    case PCMFormat::S24LE:
        return 24;
    case PCMFormat::S32LE:
    case PCMFormat::F32LE:
        return 32;
    }
    return 0;
}

constexpr std::size_t pcm_sample_bytes(PCMFormat format) noexcept
{
    return format == PCMFormat::S24LE ? 4 : pcm_sample_bits(format) / 8;
}

struct PCMSpec
{
    SampleRate sample_rate;
    PCMFormat format;
    std::size_t channels;
    std::size_t periods = 1; // Engine periods per PCM transfer, independent of the hardware period.
};

// IDs are never reused, including across Engine instances in this process.
struct NodeId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept
    {
        return value != 0;
    }
    bool operator==(NodeId other) const noexcept
    {
        return value == other.value;
    }
    bool operator!=(NodeId other) const noexcept
    {
        return !(*this == other);
    }
    bool operator<(NodeId other) const noexcept
    {
        return value < other.value;
    }
};

struct ConnId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept
    {
        return value != 0;
    }
    bool operator==(ConnId other) const noexcept
    {
        return value == other.value;
    }
    bool operator!=(ConnId other) const noexcept
    {
        return !(*this == other);
    }
    bool operator<(ConnId other) const noexcept
    {
        return value < other.value;
    }
};

struct IPort
{
    NodeId node;
    std::size_t index = 0;
};

struct OPort
{
    NodeId node;
    std::size_t index = 0;
};

// Rows are destination channels; columns are source channels.
class ChMatrix
{
  public:
    ChMatrix() = default; // Empty means identity, requiring equal channel counts.
    static Result<ChMatrix> create(std::size_t output_channels, std::size_t input_channels,
                                   View<const float> coefficients) noexcept
    try
    {
        if (input_channels == 0 || output_channels == 0)
        {
            return ErrorCode::InvalidArg;
        }
        if (output_channels > std::vector<float>().max_size() / input_channels)
        {
            return ErrorCode::TooLarge;
        }
        if (coefficients.size() != output_channels * input_channels)
        {
            return ErrorCode::InvalidArg;
        }
        for (const auto coefficient : coefficients)
        {
            if (!std::isfinite(coefficient))
            {
                return ErrorCode::InvalidArg;
            }
        }
        return ChMatrix(output_channels, input_channels, std::vector<float>(coefficients.begin(), coefficients.end()));
    }
    catch (const std::bad_alloc &)
    {
        return ErrorCode::NoMemory;
    }

    static Result<ChMatrix> create(std::size_t output_channels, std::size_t input_channels,
                                   std::initializer_list<float> coefficients) noexcept
    {
        return create(output_channels, input_channels, View<const float>{coefficients.begin(), coefficients.size()});
    }
    bool empty() const noexcept
    {
        return coefficients_.empty();
    }
    std::size_t input_channels() const noexcept
    {
        return input_channels_;
    }
    std::size_t output_channels() const noexcept
    {
        return output_channels_;
    }
    const std::vector<float> &coefficients() const noexcept
    {
        return coefficients_;
    }

  private:
    ChMatrix(std::size_t output_channels, std::size_t input_channels, std::vector<float> coefficients) noexcept
        : input_channels_(input_channels), output_channels_(output_channels), coefficients_(std::move(coefficients))
    {
    }
    std::size_t input_channels_ = 0;
    std::size_t output_channels_ = 0;
    std::vector<float> coefficients_;
};

struct ConnOpts
{
    float gain = 1.0f; // Linear gain; float mixing is not clipped.
    bool muted = false;
    ChMatrix matrix;
};

namespace detail
{
// Add into a prepared destination. Port and matrix dimensions are validated by the caller.
inline void mix(IBlock input, OBlock output, const ChMatrix &matrix, float gain = 1.0f) noexcept
{
    auto add_channel = [&](std::size_t src_ch, std::size_t dst_ch, float channel_gain) {
        if (channel_gain == 0.0f)
        {
            return;
        }
        const auto *src = input.channel(src_ch);
        auto *dst = output.channel(dst_ch);
        for (std::size_t frame = 0; frame < output.frames(); ++frame)
        {
            dst[frame] += src[frame] * channel_gain;
        }
    };
    if (matrix.empty())
    {
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            add_channel(ch, ch, gain);
        }
        return;
    }
    const auto &coefficients = matrix.coefficients();
    for (std::size_t dst_ch = 0; dst_ch < output.channels(); ++dst_ch)
    {
        for (std::size_t src_ch = 0; src_ch < input.channels(); ++src_ch)
        {
            add_channel(src_ch, dst_ch, coefficients[dst_ch * input.channels() + src_ch] * gain);
        }
    }
}
} // namespace detail

struct ProcContext
{
    std::uint64_t frame_position;
    View<const IBlock> inputs;
    View<const OBlock> outputs;
};

} // namespace aereborn

#endif // AEREBORN_CORE_TYPES_H
