#ifndef AEREBORN_CORE_COMMAND_QUEUE_H
#define AEREBORN_CORE_COMMAND_QUEUE_H

#include "retcode.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace aereborn
{

/// @brief Single-threaded serialized command queue with steady-clock timers.
/// @note All submitted commands, timers, and the optional stop hook execute sequentially
///       on one dedicated worker thread.
/// @note Unrepresentable timer deadlines are clamped to Clock::time_point::max().
/// @note Real-time Node::process() paths must not call sync() or post(); use pre-allocated
///       handoff primitives instead. ActiveBackend callbacks must not call sync().
class CommandQueue
{
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;
    using Task = std::function<void()>;
    using TimerId = std::uint64_t;

    /// @brief Construct the queue and launch its worker thread.
    /// @note Thread creation exceptions propagate without leaving a running worker.
    CommandQueue();

    /// @brief Stop the queue and join the worker thread.
    /// @pre Serialize destruction with stop() on the owning side. External calls must have returned.
    ~CommandQueue() { stop(); }

    CommandQueue(const CommandQueue &) = delete;
    CommandQueue &operator=(const CommandQueue &) = delete;

    /// @brief Stop accepting commands and selecting timers. Finish in-flight work, drain accepted
    ///        immediate commands, drop pending timers, optionally run on_stop, and join.
    /// @param on_stop Optional cleanup task run on the worker thread after draining queued
    ///        commands and destroying dropped timers. The callback must not throw.
    /// @pre Must not be called from this queue's worker thread. Serialize calls to stop() and
    ///      destruction on the owning side.
    /// @note Idempotent after the worker has joined; subsequent calls ignore on_stop.
    /// @note A timer becomes in flight when selected under the queue mutex. Its callback may
    ///       begin or finish after shutdown closes submissions.
    void stop(Task on_stop = {}) noexcept;

    /// @brief Report whether the caller is currently executing on this queue's worker thread.
    bool on_thread() const noexcept;

    /// @brief Execute fn synchronously on the worker thread and return its result.
    /// @tparam Fn Callable invoked as an lvalue without copying or moving it.
    /// @return Result of fn. The caller is responsible for the lifetime of any returned reference.
    /// @throws Error RetCode::ESTATE if an external submission finds the queue stopped or stopping.
    /// @note Exceptions from fn propagate to the caller.
    /// @note Executes inline without queueing if already on the worker thread (including inside on_stop).
    template <class Fn> auto sync(Fn &&fn) -> decltype(fn())
    {
        if (on_thread())
            return fn();

        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::ref(fn));
        auto future = task->get_future();
        if (!post([task]() noexcept { (*task)(); }))
            throw Error(RetCode::ESTATE);
        return future.get();
    }

    /// @brief Submit an asynchronous command without waiting for execution to finish.
    /// @param task Callable to execute; must not throw uncaught exceptions.
    /// @return True if queued, false if task is empty or the queue is stopping.
    /// @note Always queues even when called from the worker thread. May briefly lock or allocate.
    bool post(Task task);

    /// @brief Schedule a one-shot delayed command on the worker thread.
    /// @param delay Delay before execution (negative values are clamped to zero).
    /// @param task Callable to execute; must not throw uncaught exceptions.
    /// @return Nonzero timer ID on success, or 0 if task is empty or the queue is stopping.
    TimerId post_after(Duration delay, Task task);

    /// @brief Schedule a periodic command on the worker thread.
    /// @param interval Positive interval between executions (rescheduled after each execution).
    /// @param task Callable to execute (preserved across firings without copying); must not throw.
    /// @return Nonzero timer ID on success, or 0 if interval <= 0, task is empty, or stopping.
    TimerId post_every(Duration interval, Task task);

    /// @brief Cancel a pending timer.
    /// @param id Timer ID returned by post_after() or post_every() (0 or unknown ID is a no-op).
    /// @note Safe to call from any thread, including inside the timer's own callback.
    /// @note A callback already selected by the worker runs to completion. Cancellation prevents
    ///       future firings. The caller releases any pending callback it removes outside the mutex.
    void cancel(TimerId id) noexcept;

  private:
    struct Timer
    {
        TimerId id = 0;
        Clock::time_point deadline;
        Duration interval{0}; // 0 = one-shot
        Task task;            // Empty while currently executing on the worker thread

        void swap(Timer &other) noexcept
        {
            std::swap(id, other.id);
            std::swap(deadline, other.deadline);
            std::swap(interval, other.interval);
            task.swap(other.task);
        }
    };

    TimerId add_timer(Duration delay, Duration interval, Task task);
    std::vector<Timer>::iterator find_timer(TimerId id) noexcept;
    std::vector<Timer>::iterator earliest_timer() noexcept;
    void erase_timer(std::vector<Timer>::iterator it) noexcept;
    void run() noexcept;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Task> queue_;
    std::vector<Timer> timers_;
    Task on_stop_;
    TimerId next_timer_id_ = 1;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_COMMAND_QUEUE_H
