#ifndef AEREBORN_BACKEND_BACKEND_H
#define AEREBORN_BACKEND_BACKEND_H

#include "core/resampler.h"
#include "core/retcode.h"
#include "plugins/endpoint.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace aereborn
{

/// @brief Owns endpoint occupation and the PCM workspace for external I/O.
/// @note Unbind or destroy the backend before removing its endpoint or destroying Engine.
/// Serialize diagnostic queries with backend lifecycle calls and destruction.
class Backend
{
  public:
    virtual ~Backend();
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    virtual void start() = 0;
    virtual void stop() noexcept = 0;
    virtual void unbind() noexcept = 0;

    /// @brief Return a static diagnostic name for the backend and its bound direction, when known.
    virtual const char *type_name() const noexcept { return "Backend"; }

    /// @brief Report whether the endpoint and all working buffers are ready.
    bool bound() const noexcept { return device_spec_.channels != 0; }
    virtual bool running() const noexcept = 0;
    /// @brief Return the runtime I/O status. Passive calls report errors directly.
    virtual RetCode last_error() const noexcept { return {}; }
    /// @brief Return the negotiated PCM specification, or an empty specification when unbound.
    PortSpec device_spec() const noexcept { return device_spec_; }
    /// @brief Return the device batch limit, including the provisional value set by on_open().
    std::size_t device_max_frames() const noexcept { return device_max_frames_; }

    /// @brief Borrow the bound graph endpoint, or return null when unbound.
    /// @note Use until unbind or destruction, subject to the endpoint lifetime contract.
    const Node *endpoint_node() const noexcept;

    /// @brief Return the endpoint layout, or an empty specification when no endpoint is claimed.
    PortSpec logical_spec() const noexcept;

    /// @brief Report whether the binding uses FIR resampling.
    bool uses_resampler() const noexcept { return needs_fir(); }

  protected:
    Backend() = default;

    void claim(Capture &endpoint);
    void claim(Playback &endpoint);
    // The adapter has validated the device rate, channels, and batch size.
    void prepare(PortSpec logical, BandWidth device_rate, double max_rate_offset);
    // Publish only after the adapter has prepared all of its resources.
    void complete_binding(PortSpec device) noexcept { device_spec_ = device; }

    // Keep endpoint occupation through driver rollback, even after clearing the workspace.
    void reset_workspace() noexcept;
    void release_endpoint() noexcept;
    void release() noexcept;

    void open_queue(bool prefill) noexcept;
    void close_queue() noexcept;
    void close_playback_graph() noexcept;

    bool is_capture() const noexcept { return capture_ != nullptr; }
    std::chrono::microseconds endpoint_period() const noexcept;
    void set_device_max_frames(std::size_t frames) noexcept { device_max_frames_ = frames; }

    bool steady() const noexcept;
    std::size_t available() const noexcept;
    std::size_t target_frames() const noexcept;
    std::size_t capture_free_frames() const noexcept;
    std::size_t logical_frames() const noexcept { return logical_frames_; }
    bool needs_fir() const noexcept { return resampler_.needs_fir(); }
    std::size_t tail_frames() const noexcept { return resampler_.tail_frames(); }

    void decode_to_capture(IBBlock input, double scale) noexcept;
    void flush_capture_tail(std::size_t frames) noexcept;

    std::size_t required_input(std::size_t frames, double scale = 1.0) const noexcept
    {
        return resampler_.required_input(frames, scale);
    }
    OFBlock logical_block(std::size_t frames) noexcept { return logical_planar_.block(frames); }
    std::size_t read_playback(OFBlock output) noexcept;
    OBBlock encode_from_playback(IFBlock input, OBBlock output, double scale) noexcept;

    static Result<std::size_t> frames_in_period(BandWidth rate, std::chrono::microseconds period) noexcept;
    static void validate_channels(std::size_t logical_channels, std::size_t device_channels);

  private:
    template <class Endpoint> void claim_endpoint(Endpoint &endpoint, Endpoint *&slot);
    void emit_capture(IFBlock input, double scale) noexcept;

    Capture *capture_ = nullptr;
    Playback *playback_ = nullptr;
    PortSpec device_spec_;
    std::size_t device_max_frames_ = 0;
    std::size_t logical_frames_ = 0;
    SincResampler resampler_;
    Buffer device_planar_;
    Buffer logical_planar_;
};

/// @brief Caller-driven I/O without its own worker thread.
/// @note Serialize all calls, including queries and destruction, on the calling side.
class PassiveBackend : public Backend
{
  public:
    void start() final;
    void stop() noexcept final;
    void unbind() noexcept final;
    bool running() const noexcept final { return running_; }

  protected:
    ~PassiveBackend() override { unbind(); }

    template <class Endpoint> void bind_pcm(Endpoint &endpoint, PortSpec device)
    {
        claim(endpoint);
        const auto cleanup = [this]() noexcept { release(); };
        ScopeExit<decltype(cleanup)> rollback(cleanup);

        // Bound scratch size and the queue threshold for large endpoints.
        const auto period = std::min(endpoint_period(), std::chrono::microseconds(10000));
        set_device_max_frames(frames_in_period(device.bandwidth, period).value_or_throw());
        const auto logical = endpoint.spec();
        validate_channels(logical.channels, device.channels);
        prepare(logical, device.bandwidth, 0.0);
        complete_binding(device);
        rollback.dismiss();
    }

  private:
    bool running_ = false;
};

/// @brief Pumps PCM on a worker thread between an endpoint and a device, file, or callback.
/// @note Serialize binding, lifecycle calls, and destruction on the control side.
///       Query binding state and device specifications on that side.
/// @note Derived destructors must call unbind() before destroying resources used by I/O hooks.
class ActiveBackend : public Backend
{
  public:
    /// @brief I/O worker lifecycle state.
    enum class State { Stopped, Running, Draining, Stopping };

    /// @brief Open the source via on_open(), prepare DSP buffers, and bind to a prepared Capture node.
    /// @throws Error If the endpoint is unprepared, occupied, or negotiation fails.
    void bind(Capture &endpoint);
    /// @brief Open the sink via on_open(), prepare DSP buffers, and bind to a prepared Playback node.
    /// @throws Error If the endpoint is unprepared, occupied, or negotiation fails.
    void bind(Playback &endpoint);

    /// @brief Launch the I/O worker, wait for on_start(), and bring the endpoint online.
    /// @throws Error If unbound, already running, or on_start() reports an error.
    /// @note Thread creation exceptions propagate and leave the backend bound for retry.
    void start() final;
    /// @brief Stop I/O, take the endpoint offline, and reset DSP/drift state.
    /// @note Sync playback closes graph writes and drains queued frames and the FIR tail before stopping.
    /// @note Keeps the endpoint bound and prepared buffers intact for fast restart.
    void stop() noexcept final;
    /// @brief Stop I/O, release resources via on_close(), reset workspace, and detach the endpoint.
    void unbind() noexcept final;

    /// @brief Report whether I/O is running, draining, or stopping.
    bool running() const noexcept final;
    RetCode last_error() const noexcept final { return last_error_.load(std::memory_order_acquire); }

    /// @brief Read the current I/O worker state.
    State state() const noexcept { return worker_state_.load(std::memory_order_acquire); }

    /// @brief Report whether the worker follows an independent device clock with ASRC.
    bool is_async() const noexcept { return async_; }

    /// @brief Read the latest ASRC correction in ppm. Zero before correction or while offline.
    float drift_ppm() const noexcept;

  protected:
    /// @param async True for independent hardware clocks, false for graph-clocked I/O.
    /// @param fail_on_timeout Stop an asynchronous worker when PCM I/O times out.
    explicit ActiveBackend(bool async, bool fail_on_timeout = true) noexcept
        : async_(async), fail_on_timeout_(fail_on_timeout) {}

    /// @brief Publish an I/O failure for last_error().
    /// @pre Call from the worker or during control-side cleanup after it joins.
    void record_io_error(RetCode error) noexcept;

    /// @brief Return the rounded-up time left for the current async I/O batch, or zero at its deadline.
    /// @pre Call from an async I/O hook on the worker thread.
    int remaining_io_ms() const noexcept;

    /// @brief Open the device or file and return its negotiated interleaved PCM specification.
    /// @note Call set_device_max_frames() before returning success.
    /// @note Runs on the control thread. Setup exceptions propagate after on_close() rolls back the open.
    virtual Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds target_period) = 0;

    /// @brief Finish opening the device after all binding buffers are allocated.
    virtual RetCode on_bind_ready(PortSpec) noexcept { return {}; }

    /// @brief Start the stream and acquire thread-affine resources on the I/O worker.
    /// @note Synchronized with start(); on failure, roll back before returning the error.
    virtual RetCode on_start() noexcept { return {}; }

    /// @brief Stop the stream and release worker resources after a successful on_start().
    virtual void on_stop() noexcept {}

    /// @brief Wake blocked I/O hooks promptly so they return RetCode::CANCELLED. Idempotent and non-blocking.
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
    struct ASRC
    {
        double filtered_error = 0;
        double integral = 0;
        std::atomic<double> correction{0.0};

        double scale() const noexcept { return 1.0 + correction.load(std::memory_order_relaxed); }
        void reset() noexcept;
        void update(std::size_t queued_before, std::size_t queued_after, std::size_t target_frames,
                    BandWidth logical_rate, std::size_t device_frames, BandWidth device_rate) noexcept;
    };

    template <class Endpoint> void bind_endpoint(Endpoint &endpoint);
    std::chrono::milliseconds io_timeout() const noexcept;
    void wait_backpressure() noexcept;
    void go_offline(RetCode error) noexcept;

    OBBlock device_block(std::size_t frames) noexcept;
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
    const bool fail_on_timeout_;
    std::vector<std::uint8_t> device_bytes_;
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
