#ifndef AEREBORN_AUDIO_INTERFACE_H
#define AEREBORN_AUDIO_INTERFACE_H

#include "audio_types.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace aereborn
{

class CommandQueue;
class GraphService;

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
    /// @brief Create an identity channel map with linear gain. Returns an empty handle on failure.
    NodeId create_matrix(BandWidth bandwidth, std::size_t channels, float gain = 1.0f);
    /// @brief Create a channel map with row-major coefficients (output by input) and linear gain.
    /// @return A node handle, or an empty handle on failure.
    NodeId create_matrix(BandWidth bandwidth, std::size_t input_channels, std::size_t output_channels,
                         const std::vector<float> &coefficients, float gain = 1.0f);

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
