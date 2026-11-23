#ifndef AEREBORN_PLUGINS_ENDPOINT_H
#define AEREBORN_PLUGINS_ENDPOINT_H

#include "core/fring.h"
#include "core/graph.h"
#include <atomic>
#include <chrono>
#include <cstddef>

namespace aereborn
{

class Backend;

/// @brief Graph-side capture node receiving planar F32P frames from its bound backend.
/// @note The graph is the sole consumer. Serialize prepare() with binding.
class Capture final : public NodeT<0, 1>
{
  public:
    /// @brief Construct a capture node with one fixed F32P output.
    /// @param rate Graph-side sample rate.
    /// @param channels Number of output channels.
    /// @param io_period Maximum duration of one external batch, used to size the queue.
    /// @throws Error InvalidArg If the port specification is invalid or the I/O period is nonpositive.
    Capture(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period);

    /// @brief Prepare the queue for the graph period on the control side.
    /// @param period Graph block period.
    /// @throws Error InvalidArg for an unsupported period, Busy if changing it while bound, or TooLarge if the
    /// queue exceeds its capacity limit.
    void prepare(BlockPeriod period) override;

    /// @copydoc Node::process
    /// @note Fills missing output frames with silence, including while offline or waiting for prefill.
    void process(const ProcessContext &context) noexcept override;

    /// @brief Return the fixed F32P output specification.
    PortSpec spec() const noexcept { return outputs()[0]; }

  private:
    friend class Backend;

    /// @brief Allow graph reads. With prefill enabled, reads wait for half the queue capacity.
    /// @pre The backend is bound and the graph gate is closed.
    /// @note Called by the I/O worker before its first write.
    void open(bool prefill) noexcept;

    /// @brief Close graph reads, wait for queue access to finish, and discard remaining frames.
    /// @note Called by the I/O worker after its last write.
    void close() noexcept;

    /// @brief Queue as many input frames as space allows, from the I/O worker.
    void write(IFBlock input) noexcept { ring_.write(input); }

    /// @brief Report whether the queue has released or bypassed its startup prefill gate.
    bool steady() const noexcept { return primed_.load(std::memory_order_acquire); }

    /// @brief Return the external batch duration budget used to size the queue.
    std::chrono::microseconds io_period() const noexcept { return io_period_; }

    /// @brief Return the queue capacity in frames, or zero before preparation.
    /// @pre Serialize with prepare().
    std::size_t capacity() const noexcept { return ring_.capacity(); }

    /// @brief Return the half-capacity prefill and drift target in frames.
    /// @pre Serialize with prepare().
    std::size_t target_frames() const noexcept { return ring_.capacity() / 2; }

    /// @brief Return the current number of queued frames.
    /// @pre Call from the external producer or while both sides are idle.
    std::size_t available() const noexcept { return ring_.available(); }

    const std::chrono::microseconds io_period_;
    BlockPeriod period_{};
    FrameRing ring_;
    std::atomic<bool> backend_bound_{false};
    std::atomic<unsigned> graph_state_{0};
    std::atomic<bool> primed_{false};
};

/// @brief Graph-side playback node delivering planar F32P frames to its bound backend.
/// @note The graph is the sole producer. Serialize prepare() with binding.
class Playback final : public NodeT<1, 0>
{
  public:
    /// @brief Construct a playback node with one fixed F32P input.
    /// @param rate Graph-side sample rate.
    /// @param channels Number of input channels.
    /// @param io_period Maximum duration of one external batch, used to size the queue.
    /// @throws Error InvalidArg If the port specification is invalid or the I/O period is nonpositive.
    Playback(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period);

    /// @brief Prepare the queue for the graph period on the control side.
    /// @param period Graph block period.
    /// @throws Error InvalidArg for an unsupported period, Busy if changing it while bound, or TooLarge if the
    /// queue exceeds its capacity limit.
    void prepare(BlockPeriod period) override;

    /// @copydoc Node::process
    /// @note Copies graph input while graph writes are enabled and space is available.
    void process(const ProcessContext &context) noexcept override;

    /// @brief Return the fixed F32P input specification.
    PortSpec spec() const noexcept { return inputs()[0]; }

  private:
    friend class Backend;

    /// @brief Allow graph writes. With prefill enabled, I/O reads wait for half the queue capacity.
    /// @pre The backend is bound and the graph gate is closed.
    /// @note Called by the I/O worker before its first read.
    void open(bool prefill) noexcept;

    /// @brief Close graph writes and wait for queue access to finish, leaving frames for the I/O worker.
    void close_graph() noexcept;

    /// @brief Close graph writes, wait for queue access to finish, and discard remaining frames.
    /// @note Called by the I/O worker after its last read.
    void close() noexcept;

    /// @brief Copy available planar F32P frames from the queue into the output view.
    /// @param output Writable sample view borrowed for the duration of the call.
    /// @return Frames copied, or zero while waiting for prefill or when the queue is empty.
    /// @pre A nonempty view matches the node's channel count. Call from the I/O worker.
    std::size_t read(OFBlock output) noexcept;

    /// @brief Report whether the queue has released or bypassed its startup prefill gate.
    bool steady() const noexcept { return primed_.load(std::memory_order_acquire); }

    /// @brief Return the external batch duration budget used to size the queue.
    std::chrono::microseconds io_period() const noexcept { return io_period_; }

    /// @brief Return the queue capacity in frames, or zero before preparation.
    /// @pre Serialize with prepare().
    std::size_t capacity() const noexcept { return ring_.capacity(); }

    /// @brief Return the half-capacity prefill and drift target in frames.
    /// @pre Serialize with prepare().
    std::size_t target_frames() const noexcept { return ring_.capacity() / 2; }

    /// @brief Return the current number of queued frames.
    /// @pre Call from the external consumer or while both sides are idle.
    std::size_t available() const noexcept { return ring_.available(); }

    const std::chrono::microseconds io_period_;
    BlockPeriod period_{};
    FrameRing ring_;
    std::atomic<bool> backend_bound_{false};
    std::atomic<unsigned> graph_state_{0};
    std::atomic<bool> primed_{false};
};

} // namespace aereborn

#endif
