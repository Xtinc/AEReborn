#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/test_node.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

class Signal final : public NodeT<0, 1>
{
  public:
    Signal(std::size_t channels, bool positions) : NodeT({}, {{PortSpec(render_rate, channels)}}), positions_(positions)
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto output = context.outputs[0].audio();
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            for (std::size_t frame = 0; frame < output.frames(); ++frame)
            {
                output.channel(ch)[frame] = positions_
                    ? static_cast<float>(context.tick_position * render_frames + frame + 4096 * ch) : 1.0f;
            }
        }
    }

  private:
    bool positions_;
};

class Meter final : public NodeT<1, 0>
{
  public:
    Meter() : NodeT({{PortSpec(render_rate, 1)}}, {})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto input = context.inputs[0].audio();
        first = input.channel(0)[0];
        last = input.channel(0)[input.frames() - 1];
    }
    float first = 0;
    float last = 0;
};

class LifetimeSink final : public NodeT<1, 0>
{
  public:
    explicit LifetimeSink(std::shared_ptr<std::atomic<bool>> destroyed)
        : NodeT({{PortSpec(render_rate, 1)}}, {}), destroyed_(std::move(destroyed))
    {
    }
    ~LifetimeSink() override
    {
        destroyed_->store(true);
    }
    void process(const ProcContext &) noexcept override
    {
    }

  private:
    std::shared_ptr<std::atomic<bool>> destroyed_;
};

template <class Predicate> void wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate())
    {
        check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for the runtime");
        std::this_thread::sleep_for(1ms);
    }
}

void plan_switch()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto sink = checked(engine.emplace_node<Meter>());
    checked(engine.connect({source}, {sink}));
    const auto first = checked(engine.prepare());
    checked(clock.tick());
    checked(engine.emplace_node<Meter>());
    const auto next = checked(engine.prepare());
    checked(engine.emplace_node<Meter>());
    expect_error(ErrorCode::BufferFull, engine.prepare());
    check(engine.applied_revision() == first, "A submitted plan waits for a block boundary");
    checked(clock.tick());
    check(engine.applied_revision() == next && checked(engine.node<Meter>(sink))->last == 1,
          "The replacement preserves connected data and applies at a block boundary");
    const auto stopped = checked(engine.prepare());
    checked(clock.stop());
    check(engine.applied_revision() == stopped, "Stopping applies an accepted pending plan");
    const auto tick = engine.tick_position();
    checked(clock.tick());
    check(engine.tick_position() == tick + 1, "Restart continues the common tick timeline");
}

void plan_reclamation()
{
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    Engine engine(render_period);
    ManualClock clock(engine);
    const auto sink = checked(engine.emplace_node<LifetimeSink>(destroyed));
    checked(engine.prepare());
    checked(clock.tick());
    checked(engine.remove_node(sink));
    const auto revision = checked(engine.prepare());
    check(!destroyed->load(), "The published graph keeps removed nodes alive until replacement");
    checked(clock.tick());
    check(engine.applied_revision() == revision, "The driver acknowledges the replacement");
    wait_until([&] { return destroyed->load(); });
}

class ProcessGate
{
  public:
    void hold()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait_for(lock, 5s, [&] { return released_; });
    }
    void wait_for_entry()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        check(cv_.wait_for(lock, 2s, [&] { return entered_; }), "The node never entered process()");
    }
    void release()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

void start_failure_rollback()
{
    struct StartError : std::runtime_error
    {
        StartError() : std::runtime_error("Clock failed to start")
        {
        }
    };
    class FailingDriver final : public ClockDriver
    {
      public:
        FailingDriver(bool &stopped, bool throws) : stopped_(stopped), throws_(throws)
        {
        }
        RetCode start(BlockPeriod, Tick, void *) override
        {
            if (throws_)
            {
                throw StartError{};
            }
            return ErrorCode::InvalidArg;
        }
        void stop() noexcept override
        {
            stopped_ = true;
        }

      private:
        bool &stopped_;
        bool throws_;
    };

    Engine engine(render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, true));
    const auto sink = checked(engine.emplace_node<Meter>());
    checked(engine.connect({source}, {sink}));
    const auto revision = checked(engine.prepare());
    bool stopped = false;
    expect_error(ErrorCode::InvalidArg, engine.start(std::make_unique<FailingDriver>(stopped, false)));
    check(stopped, "The driver is stopped after a failed start");
    stopped = false;
    bool threw = false;
    try
    {
        engine.start(std::make_unique<FailingDriver>(stopped, true));
    }
    catch (const StartError &)
    {
        threw = true;
    }
    check(stopped && threw, "The driver is stopped and the startup exception propagates");
    checked(clock.tick());
    check(engine.applied_revision() == revision && engine.tick_position() == 1 &&
              checked(engine.node<Meter>(sink))->last == render_frames - 1,
          "A retry starts the original graph at frame zero");
}

void live_graph_edits()
{
    Engine engine(render_period);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto output = checked(engine.node<VirtualNode<>>(bus));
    const auto conn = checked(engine.connect({source}, {bus}));
    const auto endpoint = checked(engine.emplace_node<Meter>());
    checked(engine.connect({bus}, {endpoint}));
    auto revision = checked(engine.prepare());
    checked(engine.start());
    wait_until([&] { return engine.tick_position() >= 1; });
    const auto first = engine.tick_position();

    NodeId sink;
    for (int i = 0; i < 12; ++i)
    {
        if (sink)
        {
            checked(engine.remove_node(sink));
        }
        sink = checked(engine.emplace_node<Meter>());
        checked(engine.connect({bus}, {sink}));
        const auto next = checked(engine.prepare());
        check(next > revision, "Each topology edit submits a new revision");
        wait_until([&] { return engine.applied_revision() == next; });
        revision = next;
    }

    check(engine.tick_position() > first, "The timer keeps running through control-side edits");
    checked(engine.stop());
    check(output->prepares == 1 && output->resets == 0 && output->calls > 10,
          "Live topology changes keep the existing node and its DSP state");
}

void pause_shared_branch()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    const auto other_sink = checked(engine.emplace_node<Meter>());
    const auto *branch = checked(engine.node<VirtualNode<>>(bus));
    const auto *endpoint = checked(engine.node<VirtualNode<1, 0>>(sink));
    const auto *other = checked(engine.node<Meter>(other_sink));
    checked(engine.connect({source}, {bus}));
    checked(engine.connect({bus}, {sink}));
    checked(engine.connect({source}, {other_sink}));
    checked(engine.prepare());
    checked(clock.tick());

    checked(engine.set_paused(bus, true));
    checked(engine.set_paused(sink, true));
    const auto paused = checked(engine.prepare());
    checked(clock.tick());
    check(engine.applied_revision() == paused && branch->calls == 1 && endpoint->calls == 1 && other->last == 1,
          "The pause detaches a bus and sink at the same boundary while the shared source keeps feeding another sink");

    checked(engine.set_paused(bus, false));
    checked(engine.set_paused(sink, false));
    const auto resumed = checked(engine.prepare());
    checked(clock.tick());
    check(engine.applied_revision() == resumed && branch->calls == 2 && endpoint->calls == 2 &&
              branch->prepares == 1 && branch->resets == 0 && engine.tick_position() == 3,
          "Resume restores the original branch connections and node state without resetting the clock");
}

void stop_barrier()
{
    class BlockingSink final : public NodeT<1, 0>
    {
      public:
        BlockingSink(Engine &engine, ProcessGate &gate) : NodeT({{PortSpec(render_rate, 1)}}, {}), engine_(engine), gate_(gate) {}
        void process(const ProcContext &) noexcept override
        {
            expect_error(ErrorCode::Busy, engine_.stop());
            gate_.hold();
        }
      private:
        Engine &engine_;
        ProcessGate &gate_;
    };

    ProcessGate gate;
    Engine engine(render_period);
    checked(engine.emplace_node<BlockingSink>(engine, gate));
    checked(engine.prepare());
    checked(engine.start());
    gate.wait_for_entry();
    std::promise<void> entered;
    auto called = entered.get_future();
    auto completion = std::async(std::launch::async, [&] {
        entered.set_value();
        return engine.stop();
    });
    called.wait();
    const auto waiting = completion.wait_for(30ms) == std::future_status::timeout;
    gate.release();
    checked(completion.get());
    check(waiting, "Stopping waits for an in-flight node process call");
    const auto position = engine.tick_position();
    std::this_thread::sleep_for(20ms);
    check(engine.tick_position() == position, "No tick runs after stop returns");
    checked(engine.start());
    wait_until([&] { return engine.tick_position() > position; });
    checked(engine.stop());
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"plan_switch", plan_switch},
        {"plan_reclamation", plan_reclamation},
        {"start_failure_rollback", start_failure_rollback},
        {"live_graph_edits", live_graph_edits},
        {"pause_shared_branch", pause_shared_branch},
        {"stop_barrier", stop_barrier}
    });
}
