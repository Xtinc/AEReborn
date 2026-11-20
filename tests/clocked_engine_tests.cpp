#include "support/clocked_graph.h"
#include "support/test_harness.h"
#include "support/test_node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

class Signal final : public NodeT<0, 1>
{
  public:
    Signal() : NodeT({}, {{PortSpec(render_rate, 1)}}) {}
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &context) noexcept override
    {
        const auto output = context.outputs[0].f32p_view();
        std::fill_n(output.channel(0), output.frames(), 1.0f);
    }
};

class Meter final : public NodeT<1, 0>
{
  public:
    Meter() : NodeT({{PortSpec(render_rate, 1)}}, {})
    {
    }
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0].f32p_view();
        last = input.channel(0)[input.frames() - 1];
    }
    std::atomic<float> last{0};
};

class LifetimeSink final : public NodeT<1, 0>
{
  public:
    LifetimeSink(std::atomic<bool> &destroyed, std::atomic<bool> &on_control)
        : NodeT({{PortSpec(render_rate, 1)}}, {}), destroyed_(destroyed), on_control_(on_control),
          control_thread_(std::this_thread::get_id())
    {
    }
    ~LifetimeSink() override
    {
        on_control_.store(std::this_thread::get_id() == control_thread_);
        destroyed_.store(true);
    }
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &) noexcept override
    {
    }

  private:
    std::atomic<bool> &destroyed_;
    std::atomic<bool> &on_control_;
    const std::thread::id control_thread_;
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
    ClockedGraph clock(engine);
    const auto source = checked(engine.emplace_node<Signal>());
    const auto sink = checked(engine.emplace_node<Meter>());
    checked(engine.connect({source}, {sink}));
    checked(engine.commit());
    clock.start();
    clock.advance();
    const auto added = checked(engine.emplace_node<Meter>());
    const auto *new_meter = checked(engine.node<Meter>(added));
    checked(engine.connect({source}, {added}));
    clock.commit();
    clock.advance();
    check(new_meter->last == 1, "Blocks after commit render the submitted plan");
    check(checked(engine.node<Meter>(sink))->last == 1, "The replacement preserves connected data");
}

void commit_releases_replaced_node()
{
    std::atomic<bool> destroyed{false};
    std::atomic<bool> on_control{false};
    Engine engine(render_period);
    ClockedGraph clock(engine);
    const auto sink = checked(engine.emplace_node<LifetimeSink>(destroyed, on_control));
    checked(engine.commit());
    clock.start();
    checked(engine.replace_node(sink, std::make_unique<Meter>()));
    check(!destroyed.load(), "The active plan still owns the replaced node");
    clock.commit();
    check(destroyed.load() && on_control.load(), "Commit destroys the replaced node on the calling thread");
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

class BlockingSink final : public NodeT<1, 0>
{
  public:
    BlockingSink(ProcessGate &gate, std::atomic<bool> &destroyed)
        : NodeT({{PortSpec(render_rate, 1)}}, {}), gate_(gate), destroyed_(destroyed)
    {
    }
    ~BlockingSink() override { destroyed_.store(true); }
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &) noexcept override { gate_.hold(); }

  private:
    ProcessGate &gate_;
    std::atomic<bool> &destroyed_;
};

void in_flight_commit()
{
    ProcessGate gate;
    std::atomic<bool> destroyed{false};
    Engine engine(render_period);
    const auto sink = checked(engine.emplace_node<BlockingSink>(gate, destroyed));
    checked(engine.commit());
    checked(engine.start());
    gate.wait_for_entry();
    checked(engine.remove_node(sink));

    std::promise<void> entered;
    auto called = entered.get_future();
    auto completion = std::async(std::launch::async, [&] {
        entered.set_value();
        return engine.commit();
    });
    called.wait();
    const bool waiting = completion.wait_for(30ms) == std::future_status::timeout;
    const bool kept_during_tick = !destroyed.load();
    gate.release();
    checked(completion.get());
    check(waiting && kept_during_tick, "Commit waits for the in-flight block and keeps its node alive");
    check(destroyed.load(), "Commit releases the removed node before returning");
}

class ReentrySink final : public NodeT<1, 0>
{
  public:
    ReentrySink(Engine &engine, std::size_t &constructions)
        : NodeT({{PortSpec(render_rate, 1)}}, {}), engine_(engine), constructions_(constructions)
    {
        ++constructions_;
    }
    RetCode prepare(BlockPeriod) override { return {}; }
    void process(const ProcessContext &) noexcept override
    {
        commit_code = engine_.commit().code();
        stop_code = engine_.stop().code();
        add_code = engine_.add_node(nullptr).ret_code().code();
        emplace_code = engine_.emplace_node<ReentrySink>(engine_, constructions_).ret_code().code();
    }

    ErrorCode commit_code = ErrorCode::Ok;
    ErrorCode stop_code = ErrorCode::Ok;
    ErrorCode add_code = ErrorCode::Ok;
    ErrorCode emplace_code = ErrorCode::Ok;

  private:
    Engine &engine_;
    std::size_t &constructions_;
};

void clock_reentry()
{
    std::size_t constructions = 0;
    Engine engine(render_period);
    const auto id = checked(engine.emplace_node<ReentrySink>(engine, constructions));
    const auto *sink = checked(engine.node<ReentrySink>(id));
    checked(engine.commit());
    checked(engine.start());
    wait_until([&] { return engine.tick_position() > 0; });
    checked(engine.stop());
    check(sink->commit_code == ErrorCode::Busy && sink->stop_code == ErrorCode::Busy &&
              sink->add_code == ErrorCode::Busy && sink->emplace_code == ErrorCode::Busy,
          "Clock-thread control calls return Busy");
    check(constructions == 1, "Rejected emplacement avoids constructing a node on the clock thread");
}

void clock_restart()
{
    Engine engine(render_period);
    checked(engine.stop());
    expect_error(ErrorCode::NotPrepared, engine.start());
    checked(engine.commit());
    check(engine.tick_position() == 0, "Preparing an empty graph does not render a block");
    checked(engine.start());
    expect_error(ErrorCode::Busy, engine.start());
    expect_error(ErrorCode::Busy, engine.reset());
    wait_until([&] { return engine.tick_position() > 0; });
    checked(engine.stop());
    checked(engine.stop());
    const auto completed = engine.tick_position();
    checked(engine.start());
    wait_until([&] { return engine.tick_position() > completed; });
    checked(engine.stop());
    checked(engine.reset());
    check(engine.tick_position() == 0, "Reset restarts the logical timeline");
}

void live_graph_edits()
{
    Engine engine(render_period);
    const auto source = checked(engine.emplace_node<Signal>());
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto output = checked(engine.node<VirtualNode<>>(bus));
    checked(engine.connect({source}, {bus}));
    const auto endpoint = checked(engine.emplace_node<Meter>());
    checked(engine.connect({bus}, {endpoint}));
    checked(engine.commit());
    checked(engine.start());
    wait_until([&] { return engine.tick_position() >= 1; });
    const auto first = engine.tick_position();

    NodeId sink;
    for (int i = 0; i < 12; ++i)
    {
        if (sink != NodeId{})
        {
            checked(engine.remove_node(sink));
        }
        sink = checked(engine.emplace_node<Meter>());
        checked(engine.connect({bus}, {sink}));
        const auto *meter = checked(engine.node<Meter>(sink));
        checked(engine.commit());
        wait_until([&] { return meter->last == 1; });
    }

    check(engine.tick_position() > first, "The timer keeps running through control-side edits");
    checked(engine.stop());
    check(output->prepares == 1 && output->resets == 0 && output->calls > 10,
          "Live topology changes keep the existing node and its DSP state");
}

void pause_shared_branch()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    const auto source = checked(engine.emplace_node<Signal>());
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    const auto other_sink = checked(engine.emplace_node<Meter>());
    const auto *branch = checked(engine.node<VirtualNode<>>(bus));
    const auto *endpoint = checked(engine.node<VirtualNode<1, 0>>(sink));
    const auto *other = checked(engine.node<Meter>(other_sink));
    checked(engine.connect({source}, {bus}));
    checked(engine.connect({bus}, {sink}));
    checked(engine.connect({source}, {other_sink}));
    checked(engine.commit());
    clock.start();
    clock.advance();

    checked(engine.set_paused(bus, true));
    checked(engine.set_paused(sink, true));
    clock.commit();
    const auto calls_before_pause = branch->calls;
    clock.advance();
    check(branch->calls == calls_before_pause && endpoint->calls == calls_before_pause && other->last == 1,
          "The pause detaches a bus and sink at the same boundary while the shared source keeps feeding another sink");

    checked(engine.set_paused(bus, false));
    checked(engine.set_paused(sink, false));
    clock.commit();
    const auto calls_before_resume = branch->calls;
    const auto endpoint_calls_before_resume = endpoint->calls;
    const auto resume_tick = engine.tick_position();
    clock.advance();
    check(branch->calls == calls_before_resume + 1 && endpoint->calls == endpoint_calls_before_resume + 1 &&
              branch->prepares == 1 && branch->resets == 0 && engine.tick_position() == resume_tick + 1,
          "Resume restores the original branch connections and node state without resetting the clock");
}

void stop_barrier()
{
    ProcessGate gate;
    std::atomic<bool> destroyed{false};
    Engine engine(render_period);
    checked(engine.emplace_node<BlockingSink>(gate, destroyed));
    checked(engine.commit());
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
    check(!destroyed.load(), "Stopping keeps the current graph alive");
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
        {"commit_releases_replaced_node", commit_releases_replaced_node},
        {"in_flight_commit", in_flight_commit},
        {"clock_reentry", clock_reentry},
        {"clock_restart", clock_restart},
        {"live_graph_edits", live_graph_edits},
        {"pause_shared_branch", pause_shared_branch},
        {"stop_barrier", stop_barrier}
    });
}
