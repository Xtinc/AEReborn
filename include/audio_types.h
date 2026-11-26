#ifndef AEREBORN_AUDIO_TYPES_H
#define AEREBORN_AUDIO_TYPES_H

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) || defined(_WIN64)
#include <sdkddkver.h>
#if _WIN32_WINNT >= 0x0600 // Windows Vista
#define WINDOWS_OS_ENVIRONMENT 1
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif

#if defined(__linux__) || defined(__linux) || defined(linux)
#define LINUX_OS_ENVIRONMENT 1
#else
#define LINUX_OS_ENVIRONMENT 0
#endif

namespace aereborn
{

/// @brief Non-throwing status code shared across engine boundaries and public APIs.
/// @note Stores a single 32-bit code without allocating.
class RetCode
{
  public:
    /// @brief Status values shared with backend workers and control callers.
    /// @note BUSY and IOFAILED avoid collisions with platform errno macros.
    enum Code : std::int32_t
    {
        FAILED = -18, ///< Generic unclassified failure
        EPARAM,       ///< Invalid argument
        EPORT,        ///< Port index out of range or nonzero port passed to node-level call
        ETYPE,        ///< Node has a different type
        ESTATE,       ///< Object is not prepared, committed, or in a valid state
        BUSY,         ///< Resource busy or control call from engine clock thread
        EINPUTBUSY,   ///< Destination input port already has a source
        ENODE,        ///< Node does not belong to this engine or was removed
        ECONN,        ///< Connection does not exist
        IOFAILED,     ///< Device open, file, or underlying I/O failure
        INVFMT,       ///< Formats or port layouts do not match
        TOOLARGE,     ///< Requested capacity, frame count, or port count exceeds limit
        DUPLICATE,    ///< These ports are already connected
        CYCLE,        ///< Connection would create a feedback cycle
        DEVLOST,      ///< Audio device is no longer available
        EOS,          ///< Finite stream reached end of stream
        CANCELLED,    ///< Blocking I/O operation was cancelled
        TIMEOUT,      ///< Async I/O operation timed out

        // Non-error status
        OK = 0,       ///< Success
        NOACTION = 1  ///< Idempotent operation performed no action
    };

    constexpr RetCode(Code code = OK) noexcept : code_(code) {}

    /// @brief Check whether the operation returned OK.
    constexpr explicit operator bool() const noexcept { return code_ == OK; }
    constexpr Code code() const noexcept { return code_; }

    friend constexpr bool operator==(RetCode lhs, RetCode rhs) noexcept { return lhs.code_ == rhs.code_; }
    friend constexpr bool operator!=(RetCode lhs, RetCode rhs) noexcept { return lhs.code_ != rhs.code_; }

    constexpr const char *what() const noexcept
    {
        switch (code_)
        {
        case OK:         return "Success";
        case NOACTION:   return "No action";
        case FAILED:     return "Operation failed";
        case EPARAM:     return "Invalid argument";
        case EPORT:      return "Port index is out of range";
        case ETYPE:      return "Node has a different type";
        case ESTATE:     return "Object is not prepared, committed, or bound";
        case BUSY:       return "The current execution context or state prevents this operation";
        case EINPUTBUSY: return "Input already has a source (use a mixer for multiple sources)";
        case ENODE:      return "Node does not belong to this engine or was removed";
        case ECONN:      return "Connection does not exist";
        case IOFAILED:   return "I/O operation failed";
        case INVFMT:     return "Formats or port layouts do not match";
        case TOOLARGE:   return "Requested capacity exceeds the supported size";
        case DUPLICATE:  return "These ports are already connected";
        case CYCLE:      return "Connection would create a feedback cycle";
        case DEVLOST:    return "Audio device is no longer available";
        case EOS:        return "End of stream";
        case CANCELLED:  return "I/O operation was cancelled";
        case TIMEOUT:    return "I/O operation timed out";
        }
        return "Unknown error code";
    }

  private:
    Code code_;
};

static_assert(sizeof(RetCode) == 4, "RetCode must remain a 4-byte value");

/// @brief Supported audio bandwidths (sample rates in Hz).
enum class BandWidth : std::uint32_t
{
    Unknown       = 0,
    Narrow        = 8000,
    QuarterCD     = 11025,
    Wide          = 16000,
    HalfCD        = 22050,
    SemiSuperWide = 24000,
    SuperWideband = 32000,
    CDQuality     = 44100,
    Full          = 48000,
    DoubleCD      = 88200,
    UltraFull     = 96000
};

/// @brief Supported durations of one engine tick, in milliseconds.
enum class BlockPeriod : std::uint32_t
{
    INR_02MS = 2,
    INR_05MS = 5,
    INR_10MS = 10,
    INR_20MS = 20,
    INR_40MS = 40
};

/// @brief Sample storage format, including channel layout and byte order.
/// @note F32P stores native planar float32. All other formats store interleaved little-endian PCM.
enum class SampleFormat : std::uint32_t
{
    F32P = 0, ///< Native planar float32
    S16LE,    ///< Signed 16-bit interleaved little-endian PCM
    S24_3LE,  ///< Signed 24-bit PCM packed into 3 bytes per sample
    S24LE,    ///< Signed 24-bit PCM stored in a 4-byte slot per sample
    S32LE,    ///< Signed 32-bit interleaved little-endian PCM
    F32LE     ///< 32-bit IEEE float interleaved little-endian PCM
};

/// @brief Fixed bandwidth, channel count, and format declared by a port or PCM boundary.
/// @note Connections require all three fields to match. Each frame contains one sample per channel.
struct PortSpec
{
    /// @brief Construct an empty placeholder with no bandwidth or channels.
    constexpr PortSpec() noexcept = default;

    /// @brief Specify the bandwidth in Hz, channels per frame and sample format.
    constexpr PortSpec(BandWidth bandwidth, std::size_t channels, SampleFormat format) noexcept
        : channels(channels), bandwidth(bandwidth), format(format)
    {
    }

    constexpr bool operator==(const PortSpec &other) const noexcept
    {
        return bandwidth == other.bandwidth && channels == other.channels && format == other.format;
    }
    constexpr bool operator!=(const PortSpec &other) const noexcept { return !(*this == other); }

    /// @brief Check whether bandwidth, channels, and format are valid.
    bool valid() const noexcept;

    /// @brief Return the maximum frame count in one block for buffer sizing.
    std::size_t max_frames(BlockPeriod period) const noexcept
    {
        const auto units = std::uint64_t{static_cast<std::uint32_t>(bandwidth)} * static_cast<std::uint32_t>(period);
        return static_cast<std::size_t>((units + 999) / 1000);
    }

    /// @brief Return the actual frame count for a tick.
    std::size_t frames(BlockPeriod period, std::uint64_t tick) const noexcept
    {
        const auto units = std::uint64_t{static_cast<std::uint32_t>(bandwidth)} * static_cast<std::uint32_t>(period);
        const auto fraction = units % 1000;
        if (fraction == 0)
            return static_cast<std::size_t>(units / 1000);
        const auto start = (tick % 1000) * fraction;
        return static_cast<std::size_t>(units / 1000 + (start + fraction + 999) / 1000 - (start + 999) / 1000);
    }

    /// @brief Return the zero-based frame position at the start of a tick (fractional positions round up).
    std::uint64_t frame_position(BlockPeriod period, std::uint64_t tick) const noexcept
    {
        const auto units = std::uint64_t{static_cast<std::uint32_t>(bandwidth)} * static_cast<std::uint32_t>(period);
        // Split whole seconds from the remainder to avoid multiplying tick by the sample rate.
        return (tick / 1000) * units + ((tick % 1000) * units + 999) / 1000;
    }

    std::size_t channels = 0;
    BandWidth bandwidth = BandWidth::Unknown;
    SampleFormat format{};
};

/// @brief Unified 64-bit node and port handle.
/// @note High 48 bits identify the node (0 = empty/invalid). Low 16 bits select the zero-based port
///       (default 0; 0xFFFF is reserved as an out-of-range sentinel to prevent wraparound).
struct NodeId
{
    static constexpr std::uint64_t port_mask = 0xFFFFu;
    static constexpr std::uint64_t max_node = 0xFFFFFFFFFFFFull;
    static constexpr std::size_t invalid_port = static_cast<std::size_t>(port_mask);

    std::uint64_t raw = 0;

    /// @brief Construct a handle from a 48-bit node identity and an optional port index.
    /// @return An empty handle if the node identity is zero or exceeds 48 bits.
    static constexpr NodeId make(std::uint64_t node_id, std::size_t port_idx = 0) noexcept
    {
        if (node_id > max_node)
            return {};
        return NodeId{node_id << 16}[port_idx];
    }

    constexpr explicit operator bool() const noexcept { return node() != 0; }
    constexpr std::uint64_t node() const noexcept { return raw >> 16; }
    constexpr std::size_t port() const noexcept { return static_cast<std::size_t>(raw & port_mask); }
    constexpr NodeId base() const noexcept { return {raw & ~port_mask}; }

    /// @brief Select a specific port on this node (e.g. mix[0], mix[1]).
    constexpr NodeId operator[](std::size_t port_idx) const noexcept
    {
        if (!*this)
            return {};
        const auto port = port_idx < invalid_port ? static_cast<std::uint64_t>(port_idx) : port_mask;
        return {base().raw | port};
    }

    constexpr bool operator==(NodeId other) const noexcept { return raw == other.raw; }
    constexpr bool operator!=(NodeId other) const noexcept { return raw != other.raw; }
    constexpr bool operator<(NodeId other) const noexcept { return raw < other.raw; }
};

static_assert(sizeof(NodeId) == 8, "NodeId must remain an 8-byte value");

} // namespace aereborn

#endif // AEREBORN_AUDIO_TYPES_H
