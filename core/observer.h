#ifndef AEREBORN_CORE_OBSERVER_H
#define AEREBORN_CORE_OBSERVER_H

#include "types.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

namespace aereborn
{

struct ObserverOptions
{
    std::chrono::nanoseconds interval{}; // Reserved. Delivery currently runs at 30 Hz.
    std::size_t queue_blocks = 16;
};

// The views remain valid for the duration of the callback.
struct Observation
{
    IBlock audio;
    std::uint64_t frame_position;             // Engine frames for both graph audio and PCM.
    std::uint64_t dropped_frames;             // Missing engine frames before this batch.
    View<const std::uint8_t> pcm{nullptr, 0}; // Interleaved bytes for a PCM port.
    PortSpec port{};
};

using ObserverCallback = std::function<void(const Observation &)>;

class ObserverWorkers;

class Observer
{
  public:
    Observer(ObservationPort port, ObserverCallback callback, ObserverOptions options = {});
    ObservationPort port() const noexcept { return port_; }

    class PreparedFormat;
    struct DestroyFormat
    {
        void operator()(PreparedFormat *format) const noexcept;
    };
    using Format = std::unique_ptr<PreparedFormat, DestroyFormat>;

    // Prepare with validated graph timing and the resolved port format.
    Result<Format> prepare_format(EngineSpec spec, PortSpec format) const;
    // Install a prepared buffer, or clear a reusable one, after the writer is detached.
    void configure(Format format) noexcept;
    void write(IBlock samples, std::uint64_t frame) noexcept;
    void write(View<const std::uint8_t> bytes, std::uint64_t frame) noexcept;
    void start() noexcept;
    void request_stop() noexcept;
    void clear_when_idle() noexcept;
    void cancel() noexcept;
    void reset_when_idle() noexcept;

  private:
    enum class State { Idle, Running, Disabled };
    enum class Task { Idle, Queued, Stale, Delivering };

    friend class ObserverWorkers;
    void write(IBlock samples, View<const std::uint8_t> bytes, std::uint64_t frame) noexcept;
    void wait_for_delivery(std::unique_lock<std::mutex> &lock) noexcept;
    void run_task() noexcept;
    void deliver() noexcept;

    const ObservationPort port_;
    const std::size_t queue_blocks_;
    ObserverCallback callback_;
    Format buffer_;
    std::mutex task_mutex_;
    std::condition_variable delivery_finished_;
    Task task_ = Task::Idle;
    std::atomic<State> state_{State::Idle};
    static constexpr auto no_frame = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> first_frame_{no_frame};
    std::uint64_t delivered_end_ = no_frame;
};

} // namespace aereborn

#endif // AEREBORN_CORE_OBSERVER_H
