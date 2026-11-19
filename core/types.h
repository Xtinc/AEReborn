#ifndef AEREBORN_CORE_TYPES_H
#define AEREBORN_CORE_TYPES_H

#include "block.h"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

constexpr bool is_block_period(BlockPeriod period) noexcept
{
    return period == BlockPeriod::Ms05 || period == BlockPeriod::Ms10 || period == BlockPeriod::Ms20 ||
           period == BlockPeriod::Ms40;
}

// A copyable graph timing specification. A zero frame count marks an invalid combination.
class EngineSpec
{
  public:
    constexpr EngineSpec(SampleRate sample_rate, BlockPeriod block_period) noexcept
        : sample_rate_(sample_rate), block_period_(block_period)
    {
        if (sample_rate > SampleRate::Hz96000 || !is_block_period(block_period))
        {
            return;
        }
        const auto frames_x1000 = static_cast<std::uint32_t>(sample_rate) * static_cast<std::uint32_t>(block_period);
        block_frames_ = frames_x1000 % 1000 == 0 ? frames_x1000 / 1000 : 0;
    }
    constexpr SampleRate sample_rate() const noexcept
    {
        return sample_rate_;
    }
    constexpr BlockPeriod block_period() const noexcept
    {
        return block_period_;
    }
    constexpr std::size_t block_frames() const noexcept
    {
        return block_frames_;
    }
    constexpr bool operator==(const EngineSpec &other) const noexcept
    {
        return sample_rate_ == other.sample_rate_ && block_period_ == other.block_period_;
    }

  private:
    SampleRate sample_rate_;
    BlockPeriod block_period_;
    std::size_t block_frames_ = 0;
};

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

    // Validate the timing and return the number of device frames in a complete transfer.
    Result<std::size_t> transfer_frames(BlockPeriod period) const noexcept;
};

enum class PortDirection { Input, Output };

struct PortSpec
{
    enum class Layout { PlanarFloat, InterleavedPCM };

    PortSpec(std::size_t channels = 0) noexcept : spec{SampleRate{}, PCMFormat::F32LE, channels} {}
    explicit PortSpec(PCMSpec pcm) noexcept : spec(pcm), layout(Layout::InterleavedPCM) {}
    bool is_pcm() const noexcept { return layout == Layout::InterleavedPCM; }
    bool operator==(const PortSpec &other) const noexcept
    {
        return layout == other.layout && spec.sample_rate == other.spec.sample_rate &&
               spec.format == other.spec.format && spec.channels == other.spec.channels &&
               spec.periods == other.spec.periods;
    }

    PCMSpec spec; // For planar ports the engine resolves sample_rate; periods applies only to PCM.
    Layout layout = Layout::PlanarFloat;
};

// IDs are never reused, including across Engine instances in this process.
struct NodeId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept;
    bool operator==(NodeId other) const noexcept;
    bool operator!=(NodeId other) const noexcept;
    bool operator<(NodeId other) const noexcept;
};

struct ConnId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept;
    bool operator==(ConnId other) const noexcept;
    bool operator!=(ConnId other) const noexcept;
    bool operator<(ConnId other) const noexcept;
};

using ObserverId = std::uint64_t;

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

struct ObservationPort
{
    ObservationPort(IPort port) noexcept : node(port.node), index(port.index), direction(PortDirection::Input)
    {
    }
    ObservationPort(OPort port) noexcept : node(port.node), index(port.index), direction(PortDirection::Output)
    {
    }

    NodeId node;
    std::size_t index;
    PortDirection direction;
};

// Rows are destination channels; columns are source channels.
class ChMatrix
{
  public:
    ChMatrix() = default; // Empty means identity, requiring equal channel counts.
    ChMatrix(std::size_t output_channels, std::size_t input_channels, View<const float> coefficients);
    ChMatrix(std::size_t output_channels, std::size_t input_channels, std::initializer_list<float> coefficients);
    // Add mapped samples to output. The caller validates the block sizes and channel counts.
    void mix_add(IBlock input, OBlock output, float gain = 1.0f) const noexcept;
    void mix_ramp_add(IBlock input, OBlock output, float from, float to) const noexcept;
    bool matches(std::size_t input_channels, std::size_t output_channels) const noexcept;
    bool empty() const noexcept;
    std::size_t input_channels() const noexcept;
    std::size_t output_channels() const noexcept;
    const std::vector<float> &coefficients() const noexcept;

  private:
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

struct ProcContext
{
    std::uint64_t frame_position;
    View<const IBlock> inputs;
    View<const OBlock> outputs;
};

} // namespace aereborn

#endif // AEREBORN_CORE_TYPES_H
