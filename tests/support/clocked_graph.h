#ifndef AEREBORN_TESTS_CLOCKED_GRAPH_H
#define AEREBORN_TESTS_CLOCKED_GRAPH_H

#include "core/engine.h"
#include "test_harness.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace aereborn
{
namespace test
{

// Add this fixture before the test's nodes so its unconnected sink runs first.
// Parking there lets the control test observe the previous completed block and
// prepare input for the next block while the real clock owns execution.
class ClockedGraph
{
  public:
    explicit ClockedGraph(Engine &engine)
        : engine_(engine), gate_(checked(engine.node<Gate>(checked(engine.emplace_node<Gate>()))))
    {
    }
    ~ClockedGraph() { stop(); }
    ClockedGraph(const ClockedGraph &) = delete;
    ClockedGraph &operator=(const ClockedGraph &) = delete;

    void start()
    {
        gate_->arm();
        checked(engine_.start());
        require_block(gate_->wait_for_entry());
    }

    // Park at the next block boundary, releasing the current block if parked.
    void advance() { require_block(gate_->park_at_next_block()); }

    // Let the clock reach a block boundary while the control side commits.
    void commit()
    {
        resume();
        checked(engine_.commit());
        advance();
    }

    void resume() { gate_->open(); }

    void stop() noexcept
    {
        gate_->open();
        engine_.stop();
    }

  private:
    class Gate final : public NodeT<1, 0>
    {
      public:
        Gate() : NodeT({PortSpec(SampleRate::Hz48000, 1, SampleFormat::F32P)}, {}) {}
        void process(const ProcessContext &) noexcept override
        {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            changed_.notify_one();
            changed_.wait(lock, [&] { return open_ || !entered_; });
        }

        bool wait_for_entry()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return wait_at_gate(lock);
        }

        void open() noexcept
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            changed_.notify_one();
        }

        bool park_at_next_block()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            open_ = false;
            entered_ = false;
            changed_.notify_one();
            return wait_at_gate(lock);
        }

        // The fixture arms the gate before starting the clock.
        void arm() noexcept
        {
            open_ = false;
            entered_ = false;
        }

      private:
        bool wait_at_gate(std::unique_lock<std::mutex> &lock)
        {
            return changed_.wait_for(lock, std::chrono::seconds(2), [&] { return entered_; });
        }

        std::mutex mutex_;
        std::condition_variable changed_;
        bool open_ = false;
        bool entered_ = false;
    };

    void require_block(bool reached)
    {
        if (!reached)
        {
            // Stop before a failed test unwinds past node buffers borrowed for this block.
            stop();
            check(false, "The clock did not reach the next block");
        }
    }

    Engine &engine_;
    Gate *const gate_;
};

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_CLOCKED_GRAPH_H
