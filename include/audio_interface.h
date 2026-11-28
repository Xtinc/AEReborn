#ifndef AEREBORN_AUDIO_INTERFACE_H
#define AEREBORN_AUDIO_INTERFACE_H

#include "audio_types.h"
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace aereborn
{

class CommandQueue;
class GraphService;
class DeviceService;

/// @brief Hardware endpoint options. RECOVERSOFT and RECOVERHARD are mutually exclusive.
enum class DevFlags : std::uint32_t
{
    NONE        = 0,       ///< No options enabled.
    RECOVERSOFT = 1u << 0, ///< Wait for unavailable devices and retry I/O failures. Keep running on timeouts.
    RECOVERHARD = 1u << 1, ///< Also recover on timeouts and reopen periodically.
    MEMCPY      = 1u << 2  ///< Force ALSA RW_INTERLEAVED. Accepted and ignored on Windows.
};

constexpr DevFlags operator|(DevFlags lhs, DevFlags rhs) noexcept
{
    return static_cast<DevFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr DevFlags operator&(DevFlags lhs, DevFlags rhs) noexcept
{
    return static_cast<DevFlags>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

/// @brief Test whether every bit in flag is set.
constexpr bool has_flag(DevFlags flags, DevFlags flag) noexcept
{
    return (flags & flag) == flag;
}

/// @brief Graph-side configuration for a hardware audio endpoint.
/// @note Empty name or "default" selects the system default. ALSA passes names containing ':' through
///       and expands a bare card name to hw:CARD=<name>,DEV=<device>. WASAPI accepts an endpoint ID
///       or a UTF-8 friendly name and ignores device.
/// @note The backend negotiates sample format and rate. The opened stream must accept exactly channels.
///       ALSA hw: applies that constraint to the hardware. ALSA PCM plugins can convert below the stream.
struct DevConfig
{
    std::string name;
    unsigned int device = 0;
    BandWidth bandwidth = BandWidth::Full;
    BlockPeriod period = BlockPeriod::INR_10MS; ///< Device batch and queue budget in milliseconds.
    std::size_t channels = 2;
};

/// @brief Build a weighted channel map for AudioCenter::create_matrix().
/// @note Indices are zero-based and follow the direction from input to output.
///       Construction and editing keep the first error. Later edits leave the map alone.
class ChannelMap
{
  public:
    /// @brief Construct an empty map with status EPARAM.
    ChannelMap() noexcept = default;
    /// @brief Construct a silent map with the given input and output channel counts.
    explicit ChannelMap(std::size_t input_channels, std::size_t output_channels) noexcept;

    /// @brief Route every input to the corresponding output at unity gain.
    static ChannelMap identity(std::size_t channels) noexcept;
    /// @brief Pick inputs in output order. An input may appear more than once.
    static ChannelMap select(std::size_t input_channels, const std::vector<std::size_t> &sources) noexcept;
    /// @copydoc select(std::size_t, const std::vector<std::size_t>&)
    static ChannelMap select(std::size_t input_channels, std::initializer_list<std::size_t> sources) noexcept;
    /// @brief Pick count consecutive input channels starting at first.
    static ChannelMap slice(std::size_t input_channels, std::size_t first, std::size_t count) noexcept;
    /// @brief Average the input channels into a single output.
    static ChannelMap to_mono(std::size_t input_channels) noexcept;
    /// @brief Copy one input channel to every output.
    static ChannelMap from_mono(std::size_t output_channels) noexcept;
    /// @brief Copy row-major coefficients, one row of input weights per output.
    static ChannelMap dense(std::size_t input_channels, std::size_t output_channels,
                            const std::vector<float> &coefficients) noexcept;
    /// @copydoc dense(std::size_t, std::size_t, const std::vector<float>&)
    static ChannelMap dense(std::size_t input_channels, std::size_t output_channels,
                            std::initializer_list<float> coefficients) noexcept;

    /// @brief Set the weight from one input to one output. Zero disconnects that route.
    ChannelMap &route(std::size_t from, std::size_t to, float weight = 1.0f) noexcept;
    /// @brief Replace an output's weights with gain divided equally among distinct inputs.
    ChannelMap &mix(const std::vector<std::size_t> &from, std::size_t to, float gain = 1.0f) noexcept;
    /// @copydoc mix(const std::vector<std::size_t>&, std::size_t, float)
    ChannelMap &mix(std::initializer_list<std::size_t> from, std::size_t to, float gain = 1.0f) noexcept;
    /// @brief Multiply the weights feeding one output by factor.
    ChannelMap &scale_output(std::size_t to, float factor) noexcept;

    /// @brief Report whether this map can be passed to AudioCenter::create_matrix().
    bool valid() const noexcept;
    /// @brief Report EPARAM for invalid maps or edits and TOOLARGE for capacity or allocation failures.
    RetCode status() const noexcept;
    std::size_t input_channels() const noexcept { return input_channels_; }
    std::size_t output_channels() const noexcept { return output_channels_; }
    /// @brief Return a routing weight, or zero for an invalid map or index.
    float weight(std::size_t from, std::size_t to) const noexcept;
    /// @brief Borrow row-major coefficients until the map is edited, moved, or destroyed.
    const std::vector<float> &coefficients() const noexcept { return coefficients_; }

  private:
    static ChannelMap select(std::size_t input_channels, const std::size_t *sources, std::size_t count) noexcept;
    static ChannelMap dense(std::size_t input_channels, std::size_t output_channels,
                            const float *coefficients, std::size_t count) noexcept;
    ChannelMap &mix(const std::size_t *from, std::size_t count, std::size_t to, float gain) noexcept;
    float *output_row(std::size_t to) noexcept;

    std::size_t input_channels_ = 0;
    std::size_t output_channels_ = 0;
    std::vector<float> coefficients_;
    RetCode status_{RetCode::EPARAM};
};

/// @brief Control a graph of hardware, WAV, callback, and push/pull endpoints, mixers, and channel matrices.
/// @note Control calls serialize on a control thread and may block.
/// @pre All external calls must return before destruction begins.
class AudioCenter
{
  public:
    /// @brief Construct a graph with the given software clock period.
    /// @throws std::runtime_error If the period is unsupported or the control thread cannot start.
    explicit AudioCenter(BlockPeriod period = BlockPeriod::INR_10MS);

    /// @brief Unbind all endpoints, finalize recordings, and stop the software clock.
    ~AudioCenter();

    AudioCenter(const AudioCenter &) = delete;
    AudioCenter &operator=(const AudioCenter &) = delete;

    /// @brief Create a hardware capture endpoint with an F32P graph output.
    /// @param config Device selection and graph sample rate, channel count, and device I/O budget.
    /// @param flags Optional recovery and access mode flags.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note With recovery enabled, a device that fails with DEVLOST or IOFAILED can start offline.
    ///       The endpoint supplies silence while offline and retries while in the running plan.
    NodeId create_idev(const DevConfig &config = {}, DevFlags flags = DevFlags::NONE);

    /// @brief Create a hardware playback endpoint with an F32P graph input.
    /// @param config Device selection and graph sample rate, channel count, and device I/O budget.
    /// @param flags Optional recovery and access mode flags.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note With recovery enabled, a device that fails with DEVLOST or IOFAILED can start offline.
    ///       The endpoint discards input while offline and retries while in the running plan.
    NodeId create_odev(const DevConfig &config = {}, DevFlags flags = DevFlags::NONE);

    /// @brief Probe an existing WAV file and create a source for its sample rate and channels.
    /// @param repeat Zero plays once, a positive value adds passes, and a negative value loops.
    /// @param io_period External I/O batch duration used for queue sizing.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note Restarting resumes at the next frame to read. Recreate the node to rewind.
    NodeId create_iwav(const std::string &path, int repeat = 0,
                       std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Create a WAV output and open its file without overwriting an existing file.
    /// @param spec WAV sample rate, channels, and one of S16LE, S24_3LE, S32LE, or F32LE.
    /// @param segment_duration Positive values select numbered files of this audio duration.
    /// @param io_period External I/O batch duration used for queue sizing.
    /// @return A node handle, or an empty handle on failure. An existing single file reports BUSY.
    /// @note Stop drains buffered audio. Removal and destruction finalize the open file.
    NodeId create_owav(const std::string &path, PortSpec spec = {BandWidth::Full, 2, SampleFormat::S16LE},
                       std::chrono::seconds segment_duration = std::chrono::seconds(0),
                       std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Create a callback source with an F32P graph output at spec's sample rate and channel count.
    /// @param spec Fixed interleaved PCM specification (S16LE, S24_3LE, S24LE, S32LE, or F32LE).
    /// @param callback Non-null function that fills a zero-initialized output buffer on a worker thread.
    /// @param user_data Caller pointer that must stay valid while the endpoint can call the callback.
    /// @param io_period Duration used to size batches and the queue. The batch frame count rounds down.
    /// @return A node handle, or an empty handle on failure. See last_error().
    NodeId create_icallback(PortSpec spec, AudioCallback callback, void *user_data = nullptr,
                            std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Create a callback sink with an F32P graph input at spec's sample rate and channel count.
    /// @param spec Fixed interleaved PCM specification (S16LE, S24_3LE, S24LE, S32LE, or F32LE).
    /// @param callback Non-null function that receives read-only input on a worker thread.
    /// @param user_data Caller pointer that must stay valid while the endpoint can call the callback.
    /// @param io_period Duration used to size batches and the queue. The batch frame count rounds down.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note Stop and committed pause drain buffered audio before returning. The last batch may be shorter.
    NodeId create_ocallback(PortSpec spec, AudioCallback callback, void *user_data = nullptr,
                            std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Create a caller-driven PCM source with an F32P graph output.
    /// @param spec Fixed sample rate, channel count, and interleaved PCM format (S16LE, S24_3LE, S24LE, S32LE, or F32LE).
    /// @param io_period Burst duration budget used to size the queue.
    /// @return A node handle, or an empty handle on failure. See last_error().
    NodeId create_ipush(PortSpec spec, std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Create a caller-driven PCM sink with an F32P graph input.
    /// @param spec Fixed sample rate, channel count, and one of create_ipush()'s interleaved PCM formats.
    /// @param io_period Burst duration budget used to size the queue.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note Stop and committed pause discard buffered frames.
    NodeId create_opull(PortSpec spec, std::chrono::microseconds io_period = std::chrono::milliseconds(10));

    /// @brief Offer interleaved PCM to a push source without waiting for queue space.
    /// @param node Handle from create_ipush(), with port index zero.
    /// @param data Borrowed PCM in the declared format, valid for this call. May be null for zero frames.
    /// @param frames Number of input frames to offer.
    /// @return Frames accepted. Resume a short write at this frame offset.
    /// @note Intended for application threads. Calls serialize per node with each other and lifecycle changes.
    ///       Handle lookup and transfer run on the calling thread.
    /// @note last_error() reports OK on success or backpressure, ESTATE when stopped, or EPORT, ENODE, ETYPE,
    ///       EPARAM, or TOOLARGE for invalid handles, buffers, or sizes.
    std::size_t push(NodeId node, const void *data, std::size_t frames) noexcept;

    /// @brief Read available interleaved PCM from a pull sink without waiting for audio.
    /// @param node Handle from create_opull(), with port index zero.
    /// @param data Borrowed writable PCM in the declared format, valid for this call. May be null for zero frames.
    /// @param frames Maximum number of output frames to read.
    /// @return Frames written. The unwritten suffix is left intact.
    /// @note Uses the same serialization and last_error() rules as push().
    std::size_t pull(NodeId node, void *data, std::size_t frames) noexcept;

    /// @brief Create an F32P mixer with 1 to 64 inputs. Returns an empty handle on failure.
    NodeId create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs);
    /// @brief Create a node by copying a channel map and applying the given linear gain.
    /// @return A node handle, or an empty handle on failure.
    /// @note Use ChannelMap::identity(channels) for a gain-only node. Keep the map available for reading until this call returns. See last_error() on failure.
    NodeId create_matrix(BandWidth bandwidth, const ChannelMap &map, float gain = 1.0f);

    /// @brief Create a sample rate and format converter. Input and output must have the same channel count.
    /// @return A node handle, or an empty handle on failure. See last_error().
    /// @note Use create_matrix() to change the channel count.
    NodeId create_convert(PortSpec input, PortSpec output);

    /// @brief Read the graph-side specification of an output port.
    /// @return The port specification, or an invalid specification on failure. See last_error().
    PortSpec output_spec(NodeId port) const;

    /// @brief Stage a connection between ports with identical sample rate, channel count and sample format.
    /// @return INVFMT if the port specifications differ.
    RetCode connect(NodeId from, NodeId to);
    /// @brief Stage removal of the given connection.
    RetCode disconnect(NodeId from, NodeId to);
    /// @brief Stage removal of the connection at the destination input.
    /// @note After commit, unpaused outputs without a source receive silence while the graph runs.
    RetCode disconnect(NodeId to);
    /// @brief Remove a node and its connections from the draft. The handle must select port zero.
    /// @note Endpoint removal unbinds immediately, including when an old plan still contains it.
    RetCode remove(NodeId node);

    /// @brief Publish the draft and reconcile backend starts and stops while running.
    /// @note Graph preparation failure retains the current plan. If a backend fails to start
    /// after publication, the new plan remains current and the graph stops. Hardware endpoints
    /// with recovery enabled handle their start failures locally.
    RetCode commit();
    /// @brief Commit pending edits, start the backends in the plan, and start the clock.
    RetCode start();
    /// @brief Stop the backends and clock. WAV and callback outputs drain and pull sinks discard queued audio.
    /// @note WAV files stay open.
    RetCode stop() noexcept;

    /// @brief Stage a node pause or resume for the next commit. The handle must select port zero.
    RetCode set_paused(NodeId node, bool paused);
    /// @brief Set a matrix's linear gain target. The handle must select port zero.
    RetCode set_gain(NodeId matrix, float gain);
    /// @brief Set a mixer's mute flag for the given input port.
    RetCode set_muted(NodeId mixer_port, bool muted);
    /// @brief Set a mixer's high-priority flag for the given input port.
    RetCode set_high_priority(NodeId mixer_port, bool high);

    /// @brief Describe the configured graph as JSON with live backend metrics and committed execution order.
    /// @return The snapshot, or an empty string on failure. See last_error().
    std::string dump_json() const;

    /// @brief Return the status of the most recent control, diagnostic, or push/pull call on this thread.
    /// @note Successful calls store OK. Construction, destruction, and this query leave it alone.
    static RetCode last_error() noexcept;

  private:
    std::unique_ptr<CommandQueue> commands_;
    std::unique_ptr<GraphService> graph_;
    std::unique_ptr<DeviceService> devices_;
};

} // namespace aereborn

#endif // AEREBORN_AUDIO_INTERFACE_H
