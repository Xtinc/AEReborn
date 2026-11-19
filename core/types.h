#ifndef AEREBORN_CORE_TYPES_H
#define AEREBORN_CORE_TYPES_H

#include "block.h"
#include <cstdint>

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

// F32P is native planar float32. The other formats are interleaved little-endian PCM.
enum class Format
{
    F32P,
    S16LE,
    S24_3LE,
    S24LE,
    S32LE,
    F32LE
};
constexpr unsigned int sample_bits(Format format) noexcept
{
    switch (format)
    {
    case Format::S16LE:
        return 16;
    case Format::S24_3LE:
    case Format::S24LE:
        return 24;
    case Format::F32P:
    case Format::S32LE:
    case Format::F32LE:
        return 32;
    }
    return 0;
}
constexpr std::size_t sample_bytes(Format format) noexcept
{
    return format == Format::S24LE ? 4 : sample_bits(format) / 8;
}

struct PortSpec
{
    constexpr PortSpec() noexcept = default;
    constexpr PortSpec(SampleRate rate, std::size_t channels, Format format = Format::F32P) noexcept
        : sample_rate(rate), channels(channels), format(format)
    {
    }
    constexpr bool valid() const noexcept
    {
        return static_cast<std::uint32_t>(sample_rate) > 0 && sample_rate <= SampleRate::Hz96000 && channels > 0 &&
               sample_bytes(format) != 0;
    }
    constexpr bool operator==(const PortSpec &other) const noexcept
    {
        return sample_rate == other.sample_rate && channels == other.channels && format == other.format;
    }
    constexpr bool operator!=(const PortSpec &other) const noexcept { return !(*this == other); }
    std::size_t max_frames(BlockPeriod period) const noexcept;
    std::size_t frames(BlockPeriod period, std::uint64_t tick) const noexcept;
    std::uint64_t frame_position(BlockPeriod period, std::uint64_t tick) const noexcept;

    SampleRate sample_rate{};
    std::size_t channels = 0;
    Format format = Format::F32P;
};

// A read-only or writable view of one port's current block. Storage is owned by Engine.
// audio() is for plugins whose declared ports are F32P; bytes() exposes any declared format.
template <class T> class PortBlock
{
    template <class> friend class PortBlock;
    using Sample = typename std::conditional<std::is_const<T>::value, const float, float>::type;

  public:
    PortBlock() noexcept = default;
    PortBlock(T *data, PortSpec spec, std::size_t frames) noexcept : data_(data), spec_(spec), frames_(frames) {}
    template <class U, typename std::enable_if<std::is_convertible<U *, T *>::value, int>::type = 0>
    PortBlock(PortBlock<U> other) noexcept : data_(other.data_), spec_(other.spec_), frames_(other.frames_)
    {
    }
    PortSpec spec() const noexcept { return spec_; }
    std::size_t frames() const noexcept { return frames_; }
    View<T> bytes() const noexcept { return {data_, frames_ * spec_.channels * sample_bytes(spec_.format)}; }
    Block<Sample> audio() const noexcept
    {
        assert(spec_.format == Format::F32P);
        return {reinterpret_cast<Sample *>(data_), spec_.channels, frames_, frames_};
    }

  private:
    T *data_ = nullptr;
    PortSpec spec_;
    std::size_t frames_ = 0;
};
using IPortBlock = PortBlock<const std::uint8_t>;
using OPortBlock = PortBlock<std::uint8_t>;

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

struct ProcContext
{
    std::uint64_t tick_position;
    View<const IPortBlock> inputs;
    View<const OPortBlock> outputs;
};

} // namespace aereborn
#endif
