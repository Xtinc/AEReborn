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

/// @brief Control a graph of WAV endpoints, mixers, and channel matrices.
/// @note Calls on a live instance serialize on a control thread and may block.
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

    /// @brief Create an F32P mixer with 1 to 64 inputs. Returns an empty handle on failure.
    NodeId create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs);
    /// @brief Create a node by copying a channel map and applying the given linear gain.
    /// @return A node handle, or an empty handle on failure.
    /// @note Use ChannelMap::identity(channels) for a gain-only node. Keep the map available for reading until this call returns. See last_error() on failure.
    NodeId create_matrix(BandWidth bandwidth, const ChannelMap &map, float gain = 1.0f);

    /// @brief Stage a connection from an output to an input, inserting format bridges as needed.
    RetCode connect(NodeId from, NodeId to);
    /// @brief Stage removal of the given connection and its automatic bridges.
    RetCode disconnect(NodeId from, NodeId to);
    /// @brief Stage removal of the connection at the destination input and its automatic bridges.
    /// @note Unconnected, unpaused WAV outputs record silence while running.
    RetCode disconnect(NodeId to);
    /// @brief Remove a node and its connections from the draft. The handle must select port zero.
    /// @note Endpoint removal unbinds immediately, including when an old plan still contains it.
    RetCode remove(NodeId node);

    /// @brief Publish the draft and reconcile backend starts and stops while running.
    /// @note Graph preparation failure retains the current plan. If a backend fails to start
    /// after publication, the new plan remains current and all backends and the clock stop.
    RetCode commit();
    /// @brief Commit pending edits, start the backends in the plan, and start the clock.
    RetCode start();
    /// @brief Stop the backends and clock, draining queued audio without closing WAV files.
    RetCode stop() noexcept;

    /// @brief Stage a node pause or resume for the next commit. The handle must select port zero.
    RetCode set_paused(NodeId node, bool paused);
    /// @brief Set a matrix's linear gain target. The handle must select port zero.
    RetCode set_gain(NodeId matrix, float gain);
    /// @brief Set a mixer's mute flag for the given input port.
    RetCode set_muted(NodeId mixer_port, bool muted);
    /// @brief Set a mixer's high-priority flag for the given input port.
    RetCode set_high_priority(NodeId mixer_port, bool high);

    /// @brief Describe the committed graph and live backends as text. Returns empty on failure.
    std::string dump_text() const;
    /// @brief Describe the committed graph and live backends as DOT. Returns empty on failure.
    std::string dump_dot() const;

    /// @brief Return the status of the most recent control or diagnostic call on this thread.
    /// @note Successful calls store OK. Construction, destruction, and this query leave it alone.
    static RetCode last_error() noexcept;

  private:
    std::unique_ptr<CommandQueue> commands_;
    std::unique_ptr<GraphService> graph_;
};

} // namespace aereborn

#endif // AEREBORN_AUDIO_INTERFACE_H
