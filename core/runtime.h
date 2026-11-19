#ifndef AEREBORN_CORE_RUNTIME_H
#define AEREBORN_CORE_RUNTIME_H

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace aereborn
{

// One producer and one consumer. The spare slot distinguishes full from empty.
template <class T> class SpscRing
{
  public:
    explicit SpscRing(std::size_t capacity) : slots_(capacity + 1)
    {
    }
    bool lock_free() const noexcept
    {
        return head_.is_lock_free() && tail_.is_lock_free();
    }
    T *write_slot() noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        return next(head) == tail_.load(std::memory_order_acquire) ? nullptr : &slots_[head];
    }
    void publish() noexcept
    {
        head_.store(next(head_.load(std::memory_order_relaxed)), std::memory_order_release);
    }
    std::size_t published() const noexcept
    {
        return head_.load(std::memory_order_acquire);
    }
    const T *read_slot(std::size_t end) const noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        return tail == end ? nullptr : &slots_[tail];
    }
    void consume() noexcept
    {
        tail_.store(next(tail_.load(std::memory_order_relaxed)), std::memory_order_release);
    }

  private:
    std::size_t next(std::size_t index) const noexcept
    {
        return index + 1 == slots_.size() ? 0 : index + 1;
    }

    std::vector<T> slots_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

extern thread_local const void *engine_execution_context;

class EngineExecutionScope
{
  public:
    explicit EngineExecutionScope(const void *context) noexcept : previous_(engine_execution_context)
    {
        engine_execution_context = context;
    }
    ~EngineExecutionScope()
    {
        engine_execution_context = previous_;
    }

  private:
    const void *previous_;
};

class ClockDriver;
std::unique_ptr<ClockDriver> make_timer_driver();

class RuntimeTask
{
  public:
    virtual ~RuntimeTask() = default;
};

std::unique_ptr<RuntimeTask> start_background_task(std::function<void()> run);

class Observer;

class ObserverWorkers
{
  public:
    ObserverWorkers();
    ~ObserverWorkers();
    void schedule(const std::shared_ptr<Observer> &observer, const void *context) noexcept;

  private:
    struct Pool;
    std::unique_ptr<Pool> pool_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_RUNTIME_H
