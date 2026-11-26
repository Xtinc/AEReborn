#ifndef AEREBORN_CORE_TYPES_H
#define AEREBORN_CORE_TYPES_H

#include "audio_types.h"
#include "block.h"
#include <cstdint>

namespace aereborn
{

constexpr bool is_bandwidth(BandWidth bandwidth) noexcept
{
    switch (bandwidth)
    {
    case BandWidth::Narrow:
    case BandWidth::QuarterCD:
    case BandWidth::Wide:
    case BandWidth::HalfCD:
    case BandWidth::SemiSuperWide:
    case BandWidth::SuperWideband:
    case BandWidth::CDQuality:
    case BandWidth::Full:
    case BandWidth::DoubleCD:
    case BandWidth::UltraFull:
        return true;
    case BandWidth::Unknown:
        break;
    }
    return false;
}
constexpr bool is_block_period(BlockPeriod period) noexcept
{
    return period == BlockPeriod::INR_02MS || period == BlockPeriod::INR_05MS || period == BlockPeriod::INR_10MS ||
           period == BlockPeriod::INR_20MS || period == BlockPeriod::INR_40MS;
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

/// @brief Check whether format is a supported interleaved little-endian PCM format.
constexpr bool is_interleaved_pcm(SampleFormat format) noexcept
{
    return format != SampleFormat::F32P && sample_bytes(format) != 0;
}

inline bool valid_pcm_spec(PortSpec spec) noexcept
{
    return spec.format != SampleFormat::F32P && spec.valid();
}

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

/// @brief Directed connection in a committed render plan.
struct Connection
{
    ConnId id;
    NodeId from;
    NodeId to;
};

/// @brief Timing, port blocks, and activity flags passed to a node for one process call.
/// @note All views and their backing storage are borrowed for the duration of that call.
/// @note Each supplied activity view has one flag per corresponding port.
struct ProcessContext
{
    BlockPeriod block_period;                ///< Duration of the current engine tick.
    std::uint64_t tick_position;             ///< Zero-based position of the current tick.
    View<const IBBlock> inputs;              ///< Read-only input blocks in port index order.
    View<const OBBlock> outputs;             ///< Output blocks with writable samples in port index order.
    View<const std::uint8_t> input_active{}; ///< Per-input flags. Zero guarantees a zero-filled block.
    View<std::uint8_t> output_active{};      ///< Per-output flags initialized by the engine.

    /// @brief Check whether the input block should be processed as active.
    /// @pre index is a valid input port index.
    /// @note Returns true when a standalone caller omits the activity view.
    bool has_input(std::size_t index) const noexcept
    {
        return input_active.empty() || input_active[index] != 0;
    }

    /// @brief Set the output block's activity flag.
    /// @pre index is a valid output port index.
    /// @note An inactive output must be filled with zero bytes. With no activity view, this is a no-op.
    void set_output_active(std::size_t index, bool active) const noexcept
    {
        if (!output_active.empty())
            output_active[index] = active ? 1 : 0;
    }
};

} // namespace aereborn

#endif
