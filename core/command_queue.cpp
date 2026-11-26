#include "command_queue.h"

#include <algorithm>
#include <cassert>

namespace aereborn
{
namespace
{

thread_local const CommandQueue *current_queue = nullptr;

CommandQueue::Clock::time_point deadline_after(CommandQueue::Duration delay) noexcept
{
    using Clock = CommandQueue::Clock;
    const auto now = Clock::now();
    const auto latest = Clock::time_point::max();
    const auto remaining = latest - std::max(now, Clock::time_point{});
    const auto max_delay = std::chrono::duration_cast<CommandQueue::Duration>(remaining);
    return delay <= max_delay ? now + delay : latest;
}

void run_nothrow(CommandQueue::Task &task) noexcept
{
    try
    {
        task();
    }
    catch (...)
    {
        assert(false && "CommandQueue async/timer/stop task must not throw");
    }
}

} // namespace

CommandQueue::CommandQueue()
{
    queue_.reserve(16);
    worker_ = std::thread(&CommandQueue::run, this);
}

void CommandQueue::stop(Task on_stop) noexcept
{
    assert(!on_thread() && "Cannot stop CommandQueue from its own worker thread");
    if (!worker_.joinable())
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        on_stop_.swap(on_stop);
    }
    cv_.notify_one();
    worker_.join();
}

bool CommandQueue::on_thread() const noexcept
{
    return current_queue == this;
}

bool CommandQueue::post(Task task)
{
    if (!task)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return false;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

CommandQueue::TimerId CommandQueue::post_after(Duration delay, Task task)
{
    return add_timer(delay < Duration::zero() ? Duration::zero() : delay, Duration::zero(), std::move(task));
}

CommandQueue::TimerId CommandQueue::post_every(Duration interval, Task task)
{
    if (interval <= Duration::zero())
        return 0;
    return add_timer(interval, interval, std::move(task));
}

CommandQueue::TimerId CommandQueue::add_timer(Duration delay, Duration interval, Task task)
{
    if (!task)
        return 0;
    TimerId id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return 0;
        // Keep the callback in task until any vector growth has succeeded.
        timers_.emplace_back();
        auto &timer = timers_.back();
        timer.id = id = next_timer_id_++;
        timer.deadline = deadline_after(delay);
        timer.interval = interval;
        timer.task.swap(task);
    }
    cv_.notify_one();
    return id;
}

std::vector<CommandQueue::Timer>::iterator CommandQueue::find_timer(TimerId id) noexcept
{
    return std::find_if(timers_.begin(), timers_.end(),
                        [id](const Timer &timer) noexcept { return timer.id == id; });
}

void CommandQueue::erase_timer(std::vector<Timer>::iterator it) noexcept
{
    assert(!it->task && "Extract task before erasing timer under lock");
    const auto last = timers_.end() - 1;
    if (it != last)
        it->swap(*last);
    timers_.pop_back();
}

void CommandQueue::cancel(TimerId id) noexcept
{
    if (id == 0)
        return;
    Task dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = find_timer(id);
        if (it != timers_.end())
        {
            dropped.swap(it->task);
            erase_timer(it);
        }
    }
}

std::vector<CommandQueue::Timer>::iterator CommandQueue::earliest_timer() noexcept
{
    return std::min_element(timers_.begin(), timers_.end(), [](const Timer &a, const Timer &b) noexcept {
        if (a.deadline == b.deadline)
            return a.id < b.id;
        return a.deadline < b.deadline;
    });
}

void CommandQueue::run() noexcept
{
    current_queue = this;
    const auto clear_thread = []() noexcept { current_queue = nullptr; };
    ScopeExit<decltype(clear_thread)> clear_guard(clear_thread);

    std::vector<Task> batch;
    std::vector<Timer> dropped_timers;
    Task on_stop;
    bool after_batch = false;

    for (;;)
    {
        Timer due{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;)
            {
                if (stopping_)
                {
                    batch.swap(queue_);
                    if (batch.empty())
                    {
                        dropped_timers.swap(timers_);
                        on_stop.swap(on_stop_);
                    }
                    break;
                }

                // Give a due timer a turn after each immediate batch.
                const auto timer = after_batch || queue_.empty() ? earliest_timer() : timers_.end();
                if (timer != timers_.end() && timer->deadline <= Clock::now())
                {
                    due.id = timer->id;
                    due.interval = timer->interval;
                    due.task.swap(timer->task);
                    if (due.interval == Duration::zero())
                        erase_timer(timer);
                    break;
                }

                if (!queue_.empty())
                {
                    batch.swap(queue_);
                    break;
                }

                if (timer == timers_.end())
                    cv_.wait(lock);
                else
                {
                    const auto deadline = timer->deadline;
                    cv_.wait_until(lock, deadline);
                }
            }
        }

        if (!batch.empty())
        {
            for (auto &task : batch)
                run_nothrow(task);
            batch.clear();
            after_batch = true;
            continue;
        }

        if (!due.task)
            break;

        run_nothrow(due.task);
        after_batch = false;
        if (due.interval > Duration::zero())
        {
            const auto deadline = deadline_after(due.interval);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_)
            {
                const auto timer = find_timer(due.id);
                if (timer != timers_.end())
                {
                    timer->deadline = deadline;
                    timer->task.swap(due.task);
                }
            }
        }
    }

    dropped_timers.clear();
    if (on_stop)
        run_nothrow(on_stop);
}

} // namespace aereborn
