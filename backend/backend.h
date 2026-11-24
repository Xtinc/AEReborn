#ifndef AEREBORN_BACKEND_BACKEND_H
#define AEREBORN_BACKEND_BACKEND_H

#include "core/resampler.h"
#include "plugins/endpoint.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace aereborn
{

/// @brief Common base for interleaved PCM backends bound to a Capture or Playback node.
/// @note Borrows the bound endpoint; unbind or destroy before removing the node or destroying Engine.
/// @note Serialize bind(), start(), stop(), unbind(), and destruction on the control side.
///       Query bound() and device specifications on that side.
/// @note CRITICAL: Concrete derived destructors MUST call unbind() as their first statement.
class Backend
{
  public:
    virtual ~Backend();

    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    /// @brief Open the source via on_open(), prepare DSP buffers, and bind to a prepared Capture node.
    /// @throws Error If the endpoint is unprepared, occupied, or negotiation fails.
    void bind(Capture &endpoint);
    /// @brief Open the sink via on_open(), prepare DSP buffers, and bind to a prepared Playback node.
    /// @throws Error If the endpoint is unprepared, occupied, or negotiation fails.
    void bind(Playback &endpoint);

    /// @brief Launch the I/O thread, wait for on_start() to finish on it, and bring the endpoint online.
    /// @throws Error If unbound, already running, or on_start() reports an error.
    /// @note Thread creation exceptions propagate and leave the backend bound for retry.
    void start();
    /// @brief Stop I/O, join the thread, take the endpoint offline, and reset DSP/drift state.
    /// @note Sync playback closes graph writes and drains queued frames and the FIR tail before stopping.
    /// @note Keeps the endpoint bound and prepared buffers intact for fast restart.
    void stop() noexcept;
    /// @brief Stop I/O, release resources via on_close(), reset workspace, and detach the endpoint.
    void unbind() noexcept;

    /// @brief Report whether this backend has an endpoint and a prepared workspace.
    bool bound() const noexcept { return capture_ != nullptr || playback_ != nullptr; }
    /// @brief Report whether the worker is active, draining, or stopping.
    bool running() const noexcept;
    /// @brief Return the most recently recorded I/O error.
    RetCode last_error() const noexcept { return last_error_.load(std::memory_order_acquire); }
    /// @brief Return the negotiated PCM specification, or an empty specification when unbound.
    PortSpec device_spec() const noexcept { return workspace_.device_spec; }
    /// @brief Return the negotiated device batch limit, or zero when unbound.
    /// @note During on_open(), reports the limit set so far by the driver.
    std::size_t device_max_frames() const noexcept { return workspace_.device_max_frames; }

  protected:
    /// @param async True for external-clock hardware (ASRC + PI + timeout); false for graph-clock backends.
    explicit Backend(bool async) noexcept : async_(async) {}

    /// @brief Report whether the backend is bound to a capture endpoint.
    /// @pre The backend is bound. Call on the control side or from an I/O hook.
    bool is_capture() const noexcept { return capture_ != nullptr; }

    /// @brief Publish an I/O failure for last_error().
    /// @pre Call from the worker or during control-side cleanup after it joins.
    void record_io_error(RetCode error) noexcept;

    /// @brief Compute frames contained in one period at the given sample rate.
    /// @pre The sample rate is valid and the period is positive.
    static Result<std::size_t> frames_in_period(SampleRate rate, std::chrono::microseconds period) noexcept;

    /// @brief Return the rounded-up time left for the current async I/O batch, or zero at its deadline.
    /// @pre Call from an async I/O hook on the worker thread.
    int remaining_io_ms() const noexcept;

    /// @brief Open the device or file and return its negotiated interleaved PCM specification.
    /// @note Call set_device_max_frames() before returning success.
    /// @note Runs on the control thread. Setup exceptions propagate after on_close() rolls back the open.
    virtual Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds target_period) = 0;

    /// @brief Set the negotiated device batch limit during on_open().
    void set_device_max_frames(std::size_t frames) noexcept { workspace_.device_max_frames = frames; }

    /// @brief Start the stream and acquire any thread-affine resources on the I/O worker thread.
    /// @note Synchronized with start(); on failure, roll back before returning the error.
    virtual RetCode on_start() noexcept { return {}; }

    /// @brief Stop the stream and release worker-thread resources after a successful on_start().
    virtual void on_stop() noexcept {}

    /// @brief Wake blocked I/O hooks promptly so they return Cancelled. Idempotent and non-blocking.
    /// @note Called only from control-side stop() while bound; must remain safe until on_close().
    virtual void on_cancel() noexcept {}

    /// @brief Release handles acquired in on_open() on the control thread after the worker has joined.
    /// @note Also called after a failed on_open(). Handle partial setup and unopened resources.
    virtual void on_close() noexcept {}

    /// @brief Read one batch of interleaved PCM frames (capture backends override this).
    virtual Result<std::size_t> on_read(OBBlock output) noexcept;

    /// @brief Wait until writable and return next batch size in [1, device_max_frames()].
    virtual Result<std::size_t> on_wait_writable() noexcept { return device_max_frames(); }

    /// @brief Write one batch of interleaved PCM frames (playback backends override this).
    virtual RetCode on_write(IBBlock input) noexcept;

    /// @brief Write a silent batch. The block provides scratch space for backends that need PCM bytes.
    virtual RetCode on_write_silence(OBBlock scratch) noexcept;

  private:
    struct Workspace
    {
        PortSpec device_spec;
        std::size_t device_max_frames = 0;
        std::size_t logical_frames = 0;
        SincResampler resampler;
        Buffer device_planar;  ///< Allocated only when resampler.needs_fir() is true.
        Buffer logical_planar;
        std::vector<std::uint8_t> device_bytes;
    };

    struct ASRC
    {
        double filtered_error = 0;
        double integral = 0;
        double correction = 0;

        double scale() const noexcept { return 1.0 + correction; }
        void reset() noexcept { *this = {}; }
        void update(std::size_t queued_before, std::size_t queued_after, std::size_t target_frames,
                    SampleRate logical_rate, std::size_t device_frames, SampleRate device_rate) noexcept;
    };

    enum class State { Stopped, Running, Draining, Stopping };

    template <class Endpoint> void bind_endpoint(Endpoint &endpoint, Endpoint *&slot);
    std::chrono::microseconds endpoint_period() const noexcept;
    std::chrono::milliseconds io_timeout() const noexcept;
    void wait_backpressure() noexcept;
    void go_offline(RetCode error) noexcept;

    OBBlock device_block(std::size_t frames) noexcept;
    void emit_capture(IFBlock input, double scale) noexcept;
    RetCode transfer_capture(std::size_t &device_frames) noexcept;
    Result<std::size_t> next_playback_batch() noexcept;
    RetCode emit_playback(IFBlock input, std::size_t device_frames, double scale) noexcept;
    RetCode transfer_playback_async(std::size_t &device_frames) noexcept;

    void run() noexcept;
    RetCode run_async() noexcept;
    RetCode run_sync_capture() noexcept;
    RetCode run_sync_playback() noexcept;
    RetCode drain_capture() noexcept;
    RetCode drain_playback() noexcept;

    const bool async_;
    Capture *capture_ = nullptr;
    Playback *playback_ = nullptr;
    Workspace workspace_;
    ASRC asrc_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool has_input_ = false;
    std::chrono::steady_clock::time_point io_deadline_;
    std::thread worker_;
    std::atomic<State> worker_state_{State::Stopped};
    std::atomic<RetCode> last_error_{RetCode{}};
};

} // namespace aereborn

#endif
