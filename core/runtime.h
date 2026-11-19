#ifndef AEREBORN_CORE_RUNTIME_H
#define AEREBORN_CORE_RUNTIME_H

#include "engine.h"
#include <atomic>
#include <limits>
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
    std::vector<T> &slots() noexcept
    {
        return slots_;
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
    void clear_when_idle() noexcept
    {
        tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
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

std::unique_ptr<ClockDriver> make_timer_driver();

class RuntimeTask
{
  public:
    virtual ~RuntimeTask() = default;
};

std::unique_ptr<RuntimeTask> start_runtime_task(EngineSpec spec, std::function<void()> run);

class ObserverDelivery
{
  public:
    ObserverDelivery(EngineSpec spec, std::size_t channels, ObserverOptions options, ObserverCallback callback);
    ~ObserverDelivery();
    static bool fits(std::size_t channels, std::size_t block_frames, ObserverOptions options);
    bool lock_free() const noexcept;
    void start(const void *context);
    void request_stop() noexcept;
    void stop() noexcept;
    void disable() noexcept;
    void reset_when_idle() noexcept;
    void publish(IBlock block, std::uint64_t frame) noexcept;

  private:
    struct Packet
    {
        std::uint64_t frame = 0;
        std::vector<float> samples;
    };
    void deliver();

    const EngineSpec spec_;
    const std::size_t channels_;
    const std::size_t frames_;
    ObserverCallback callback_;
    SpscRing<Packet> queue_;
    std::vector<float> scratch_;
    std::vector<float> delivery_;
    std::unique_ptr<RuntimeTask> worker_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> running_{false};
    static constexpr auto no_frame = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> first_frame_{no_frame};
    std::uint64_t delivered_end_ = no_frame;
    std::uint64_t expected_frame_ = no_frame;
    std::uint64_t delivery_start_ = 0;
    std::size_t filled_ = 0;
};

} // namespace aereborn

#endif // AEREBORN_CORE_RUNTIME_H
