#ifndef AEREBORN_PLUGINS_IO_H
#define AEREBORN_PLUGINS_IO_H

#include "core/graph.h"
#include "core/fring.h"
#include <atomic>
#include <chrono>

namespace aereborn
{

/// @brief Device-side snapshot. All sizes and counters are in logical frames.
struct QueueStatus
{
    std::size_t capacity_frames = 0;
    std::size_t queued_frames = 0;
    std::size_t overflow_frames = 0;
    std::size_t underrun_frames = 0;
    std::size_t target_frames = 0;
};

/// @brief Preallocated SPSC frame storage and the handoff between device control and graph processing.
class EndpointQueue
{
  public:
    EndpointQueue(PortSpec spec, std::chrono::microseconds device_period);
    RetCode prepare(BlockPeriod period);
    RetCode set_online(bool online) noexcept;
    void reset() noexcept;

    bool enter_graph() noexcept;
    void leave_graph() noexcept;
    bool online() const noexcept;
    bool steady() const noexcept;
    std::chrono::microseconds device_period() const noexcept { return device_period_; }
    QueueStatus status() const noexcept;
    RetCode check(IFBlock input) const noexcept;
    std::size_t write(IFBlock input) noexcept;
    std::size_t read(OFBlock output) noexcept;

  private:
    std::size_t target_frames() const noexcept { return ring_.capacity() / 2; }

    const PortSpec spec_;
    const std::chrono::microseconds device_period_;
    FrameRing ring_;
    BlockPeriod period_{};
    std::atomic<std::size_t> overflow_{0};
    std::atomic<std::size_t> underrun_{0};
    std::atomic<bool> primed_{false};
    std::atomic<unsigned> state_{0};
};

/// @brief Supply logical audio frames from a device thread to a fixed graph output.
class Capture final : public NodeT<0, 1>
{
  public:
    /// @brief Construct a port and its maximum device read duration.
    Capture(SampleRate rate, std::size_t channels, std::chrono::microseconds device_read_period);
    /// @copydoc Node::prepare
    RetCode prepare(BlockPeriod period) override;
    /// @copydoc Node::process
    void process(const ProcessContext &context) noexcept override;
    /// @brief Clear the queue and counters while both the device and graph are stopped.
    void reset() noexcept override;

    /// @brief Enqueue a range of logical frames. The buffer is borrowed only during the call.
    /// @return Accepted frames, or an error. Excess input is discarded.
    /// @pre Serialize device-side calls with one another.
    Result<std::size_t> write(IFBlock input) noexcept;
    /// @brief Open the queue or close it, wait for an in-flight graph access and discard queued data.
    /// @pre Call on the device control side with device I/O stopped and serialized.
    /// @note Before reopening the queue, call prepare() or reset() on its device stream, if present.
    RetCode set_online(bool online) noexcept { return queue_.set_online(online); }
    /// @brief Return whether the endpoint is open for device I/O.
    bool online() const noexcept { return queue_.online(); }
    /// @brief Return whether the consumer has completed prefill.
    bool steady() const noexcept { return queue_.steady(); }
    /// @brief Return the device read duration budget used to size the queue.
    std::chrono::microseconds device_read_period() const noexcept { return queue_.device_period(); }
    /// @brief Query counters and fill from the serialized device side.
    QueueStatus status() const noexcept { return queue_.status(); }

  private:
    EndpointQueue queue_;
};

/// @brief Supply frames from a fixed graph input to a device thread.
class Playback final : public NodeT<1, 0>
{
  public:
    /// @brief Construct a port and its maximum device write duration.
    Playback(SampleRate rate, std::size_t channels, std::chrono::microseconds device_write_period);
    /// @copydoc Node::prepare
    RetCode prepare(BlockPeriod period) override;
    /// @copydoc Node::process
    void process(const ProcessContext &context) noexcept override;
    /// @brief Clear the queue and counters while both the device and graph are stopped.
    void reset() noexcept override;

    /// @brief Read a range into a borrowed logical buffer, padding missing frames with silence.
    /// @return The number of real logical frames, or an error.
    /// @pre Serialize device-side calls with one another.
    Result<std::size_t> read(OFBlock output) noexcept;
    /// @copydoc Capture::set_online
    RetCode set_online(bool online) noexcept { return queue_.set_online(online); }
    /// @copydoc Capture::online
    bool online() const noexcept { return queue_.online(); }
    /// @copydoc Capture::steady
    bool steady() const noexcept { return queue_.steady(); }
    /// @brief Return the device write duration budget used to size the queue.
    std::chrono::microseconds device_write_period() const noexcept { return queue_.device_period(); }
    /// @copydoc Capture::status
    QueueStatus status() const noexcept { return queue_.status(); }

  private:
    EndpointQueue queue_;
};

} // namespace aereborn

#endif
