#ifndef AEREBORN_CORE_TYPES_H
#define AEREBORN_CORE_TYPES_H

#include "block.h"
#include <cstdint>

namespace aereborn
{

/// @brief Supported sample rates, in hertz.
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

/// @brief Supported durations of one engine tick, in milliseconds.
enum class BlockPeriod : std::uint32_t
{
    Ms02 = 2,
    Ms05 = 5,
    Ms10 = 10,
    Ms20 = 20,
    Ms40 = 40
};

/// @brief Sample storage format, including channel layout and byte order.
///
/// F32P stores native planar float32. All other formats store interleaved
/// little-endian PCM.
enum class SampleFormat
{
    F32P,
    S16LE,
    S24_3LE, ///< Signed 24-bit PCM packed into three bytes per sample.
    S24LE,   ///< Signed 24-bit PCM stored in a four-byte slot per sample.
    S32LE,
    F32LE
};

constexpr bool is_sample_rate(SampleRate sample_rate) noexcept
{
    switch (sample_rate)
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
constexpr bool is_block_period(BlockPeriod period) noexcept
{
    return period == BlockPeriod::Ms02 || period == BlockPeriod::Ms05 || period == BlockPeriod::Ms10 ||
           period == BlockPeriod::Ms20 || period == BlockPeriod::Ms40;
}
constexpr unsigned int sample_bits(SampleFormat format) noexcept
{
    switch (format)
    {
    case SampleFormat::S16LE:
        return 16;
    case SampleFormat::S24_3LE:
    case SampleFormat::S24LE:
        return 24;
    case SampleFormat::F32P:
    case SampleFormat::S32LE:
    case SampleFormat::F32LE:
        return 32;
    }
    return 0;
}
constexpr std::size_t sample_bytes(SampleFormat format) noexcept
{
    return format == SampleFormat::S24LE ? 4 : sample_bits(format) / 8;
}

/// @brief Fixed sample rate, channel count, and format declared by a port.
///
/// Connections require all three fields to match. Each frame contains one sample
/// per channel.
struct PortSpec
{
    /// @brief Construct an empty placeholder with no valid rate or channel count.
    constexpr PortSpec() noexcept = default;

    /// @brief Specify the sample layout of one port.
    /// @param sample_rate Port sample rate in hertz.
    /// @param channels Number of channels per frame.
    /// @param format Sample representation and layout.
    constexpr PortSpec(SampleRate sample_rate, std::size_t channels, SampleFormat format) noexcept
        : channels(channels), sample_rate(sample_rate), format(format)
    {
    }
    constexpr bool valid() const noexcept
    {
        return is_sample_rate(sample_rate) && channels > 0 && sample_bytes(format) != 0;
    }
    constexpr bool operator==(const PortSpec &other) const noexcept
    {
        return sample_rate == other.sample_rate && channels == other.channels && format == other.format;
    }
    constexpr bool operator!=(const PortSpec &other) const noexcept
    {
        return !(*this == other);
    }

    /// @brief Return the maximum frame count in one block for buffer sizing.
    /// @param period Duration of the engine tick.
    std::size_t max_frames(BlockPeriod period) const noexcept;

    /// @brief Return the actual frame count for a tick.
    /// @param period Duration of the engine tick.
    /// @param tick Zero-based tick position.
    /// @note The count is the difference between two consecutive frame positions.
    std::size_t frames(BlockPeriod period, std::uint64_t tick) const noexcept;

    /// @brief Return the zero-based frame position at the start of a tick.
    /// @param period Duration of the engine tick.
    /// @param tick Zero-based tick position.
    /// @note Fractional frame positions round up.
    std::uint64_t frame_position(BlockPeriod period, std::uint64_t tick) const noexcept;

    std::size_t channels = 0;
    SampleRate sample_rate{};
    SampleFormat format{};
};

/// @brief Borrow audio through a byte view carrying the port specification and frame count.
/// @tparam Byte Use std::uint8_t for writable bytes or const std::uint8_t for read-only bytes.
/// @note The port specification determines how to interpret the samples.
///
/// The render plan owns the storage provided to a node. Use its views only for the
/// duration of the process call.
template <class Byte> class ByteBlock
{
    template <class> friend class ByteBlock;
    using Float = typename std::conditional<std::is_const<Byte>::value, const float, float>::type;

  public:
    ByteBlock() noexcept = default;

    /// @brief Interpret a borrowed buffer according to the port specification.
    /// @param data Address of the buffer.
    /// @param spec Sample rate, channel count, and format of the buffer.
    /// @param frames Number of frames available in the buffer for this block.
    /// @pre A nonempty block refers to storage large enough for the specified frames.
    ByteBlock(Byte *data, PortSpec spec, std::size_t frames) noexcept : data_(data), spec_(spec), frames_(frames)
    {
    }
    template <class OtherByte, typename std::enable_if<std::is_convertible<OtherByte *, Byte *>::value, int>::type = 0>
    ByteBlock(ByteBlock<OtherByte> other) noexcept : data_(other.data_), spec_(other.spec_), frames_(other.frames_)
    {
    }
    PortSpec spec() const noexcept
    {
        return spec_;
    }
    std::size_t frames() const noexcept
    {
        return frames_;
    }
    View<Byte> byte_view() const noexcept
    {
        return {data_, frames_ * spec_.channels * sample_bytes(spec_.format)};
    }
    FloatBlock<Float> f32p_view() const noexcept
    {
        assert(spec_.format == SampleFormat::F32P);
        return {reinterpret_cast<Float *>(data_), spec_.channels, frames_, frames_};
    }
  private:
    Byte *data_ = nullptr;
    PortSpec spec_;
    std::size_t frames_ = 0;
};

using IBBlock = ByteBlock<const std::uint8_t>;
using OBBlock = ByteBlock<std::uint8_t>;

/// @brief Stable identity of a node in the graph, including across instance replacement.
/// @note Zero is the empty ID. The engine resolves nonzero IDs against the graph.
struct NodeId
{
    std::uint64_t value = 0;
    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }
    constexpr bool operator==(NodeId other) const noexcept
    {
        return value == other.value;
    }
    constexpr bool operator!=(NodeId other) const noexcept
    {
        return !(*this == other);
    }
    constexpr bool operator<(NodeId other) const noexcept
    {
        return value < other.value;
    }
};

/// @brief Identity assigned to one connection when it is created.
/// @note Zero is the empty ID. Reconnecting the same ports creates a new ID.
struct ConnId
{
    std::uint64_t value = 0;
    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }
    constexpr bool operator==(ConnId other) const noexcept
    {
        return value == other.value;
    }
    constexpr bool operator!=(ConnId other) const noexcept
    {
        return !(*this == other);
    }
    constexpr bool operator<(ConnId other) const noexcept
    {
        return value < other.value;
    }
};

/// @brief Address of an input port in the graph.
struct IPort
{
    NodeId node;           ///< Node that declares the port.
    std::size_t index = 0; ///< Zero-based input port index.
    constexpr bool operator==(IPort other) const noexcept
    {
        return node == other.node && index == other.index;
    }
    constexpr bool operator!=(IPort other) const noexcept
    {
        return !(*this == other);
    }
};

/// @brief Address of an output port in the graph.
struct OPort
{
    NodeId node;           ///< Node that declares the port.
    std::size_t index = 0; ///< Zero-based output port index.
    constexpr bool operator==(OPort other) const noexcept
    {
        return node == other.node && index == other.index;
    }
    constexpr bool operator!=(OPort other) const noexcept
    {
        return !(*this == other);
    }
};

/// @brief Directed connection in a committed render plan.
struct Connection
{
    ConnId id;
    OPort from;
    IPort to;
};

/// @brief Timing and port blocks passed to a node for one process call.
/// @note The port block views and their buffers are borrowed for the duration of that call.
struct ProcessContext
{
    BlockPeriod block_period;       ///< Duration of the current engine tick.
    std::uint64_t tick_position;    ///< Zero-based position of the current tick.
    View<const IBBlock> inputs;  ///< Read-only input blocks in port index order.
    View<const OBBlock> outputs; ///< Output blocks with writable samples in port index order.
};

} // namespace aereborn

/// @brief Address an input port by node ID and zero-based index.
#define IPORT(node_id, port_index) ::aereborn::IPort{(node_id), (port_index)}
/// @brief Address an output port by node ID and zero-based index.
#define OPORT(node_id, port_index) ::aereborn::OPort{(node_id), (port_index)}

#endif
