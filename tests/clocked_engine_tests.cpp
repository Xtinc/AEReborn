#include "core/observer.h"
#include "core/runtime.h"
#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/test_node.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

class Signal final : public NodeT<0, 1>
{
  public:
    Signal(std::size_t channels, bool positions) : NodeT({}, {{channels}}), positions_(positions)
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto output = context.outputs[0];
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            for (std::size_t frame = 0; frame < output.frames(); ++frame)
            {
                output.channel(ch)[frame] = positions_
                    ? static_cast<float>(context.frame_position + frame + 4096 * ch) : 1.0f;
            }
        }
    }

  private:
    bool positions_;
};

class Meter final : public NodeT<1, 0>
{
  public:
    Meter() : NodeT({{1}}, {})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto input = context.inputs[0];
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
        : NodeT({{1}}, {}), destroyed_(std::move(destroyed))
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

void parameter_queue_and_switch()
{
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto sink = checked(engine.emplace_node<Meter>());
    const auto conn = checked(engine.connect({source}, {sink}));
    const auto initial = checked(engine.prepare());
    const auto *meter = checked(engine.node<Meter>(sink));
    checked(clock.tick());
    check(meter->last == 1, "The prepared connection starts at its configured gain");

    for (std::size_t i = 0; i < 64; ++i)
    {
        checked(engine.set_gain(conn, 2));
    }
    expect_error(ErrorCode::BufferFull, engine.set_gain(conn, 8));
    checked(engine.emplace_node<Meter>());
    expect_error(ErrorCode::BufferFull, engine.prepare());
    checked(clock.tick());
    check(engine.applied_revision() == initial && meter->last == 2 &&
              std::abs(meter->first - (1.0f + 1.0f / render_frames)) < 1e-6f,
          "Accepted parameter commands ramp for one block without building a plan");

    checked(engine.set_gain(conn, 4));
    const auto next = checked(engine.prepare());
    checked(engine.emplace_node<Meter>());
    expect_error(ErrorCode::BufferFull, engine.prepare());
    checked(engine.set_gain(conn, 3));
    check(engine.applied_revision() == initial, "A submitted plan has not reached the driver");
    checked(clock.tick());
    check(engine.applied_revision() == next && meter->last == 3 &&
              std::abs(meter->first - (2.0f + 1.0f / render_frames)) < 1e-6f,
          "Parameter FIFO ordering and the current gain survive a plan switch");

    const auto stopped_revision = checked(engine.prepare());
    checked(engine.set_gain(conn, 0.5f));
    checked(clock.stop());
    check(engine.applied_revision() == stopped_revision, "Stopping applies already accepted graph commands");
    const auto position = engine.frame_position();
    checked(clock.tick());
    check(engine.frame_position() == position + render_frames && meter->first == 0.5f && meter->last == 0.5f,
          "Restart resumes at the stopped frame with the last accepted gain already applied");
}

void plan_reclamation()
{
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    Engine engine(render_rate, render_period);
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

struct Packet
{
    std::uint64_t frame;
    std::uint64_t dropped;
    std::size_t frames;
    std::size_t channels;
    float first;
    float last;
};

class PacketLog
{
  public:
    void add(const Observation &packet)
    {
        const auto audio = packet.audio;
        const std::lock_guard<std::mutex> lock(mutex_);
        packets_.push_back({packet.frame_position, packet.dropped_frames, audio.frames(), audio.channels(),
                            audio.channel(0)[0], audio.channel(audio.channels() - 1)[audio.frames() - 1]});
        cv_.notify_all();
    }
    Packet get(std::size_t index)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        check(cv_.wait_for(lock, 2s, [&] { return packets_.size() > index; }), "Timed out waiting for an observation");
        return packets_[index];
    }
    std::size_t size()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return packets_.size();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Packet> packets_;
};

class CallbackGate
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
        check(cv_.wait_for(lock, 2s, [&] { return entered_; }), "The observer never entered its callback");
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
        RetCode start(EngineSpec, Tick, void *) override
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

    PacketLog packets;
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, true));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{source},
        [&](const Observation &packet) { packets.add(packet); })));
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
    check(engine.applied_revision() == revision && packets.get(0).frame == 0,
          "A retry starts the original graph and subscription");
}

void observer_registration_and_prepare()
{
    PacketLog packets;
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, true));
    const auto callback = [&](const Observation &packet) { packets.add(packet); };
    expect_error(ErrorCode::InvalidArg, engine.add_observer(nullptr));
    expect_error(ErrorCode::UnknownNode, engine.add_observer(std::make_unique<Observer>(OPort{}, callback)));
    expect_error(ErrorCode::InvalidPort, engine.add_observer(std::make_unique<Observer>(IPort{source}, callback)));

    const auto removed = checked(engine.add_observer(std::make_unique<Observer>(OPort{source}, callback)));
    checked(engine.remove_observer(removed));
    expect_error(ErrorCode::UnknownObserver, engine.remove_observer(removed));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{source}, callback)));
    const auto revision = checked(engine.prepare());
    checked(clock.tick());
    check(packets.get(0).frame == 0, "The prepared observer receives its first rendered block");

    const auto no_callback =
        checked(engine.add_observer(std::make_unique<Observer>(OPort{source}, ObserverCallback{})));
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    checked(engine.remove_observer(no_callback));

    for (const auto capacity : {std::size_t{0}, std::numeric_limits<std::size_t>::max()})
    {
        const auto id = checked(engine.add_observer(
            std::make_unique<Observer>(OPort{source}, callback, ObserverOptions{0ns, capacity})));
        expect_error(capacity == 0 ? ErrorCode::InvalidArg : ErrorCode::TooLarge, engine.prepare());
        checked(engine.remove_observer(id));
    }
    checked(clock.tick());
    check(engine.applied_revision() == revision && packets.get(1).frame == render_frames,
          "Failed observer preparation leaves the previous plan rendering and delivering");

    const auto staged = checked(engine.add_observer(std::make_unique<Observer>(OPort{source}, callback)));
    checked(clock.reset());
    checked(engine.remove_observer(staged));
    checked(engine.prepare());
    checked(clock.tick());
    check(packets.get(2).frame == 0, "Stop and reset accept an observer whose queue was never prepared");
}

void observer_timed_delivery()
{
    PacketLog fast;
    PacketLog slow;
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(2, true));
    auto fast_observer = std::make_unique<Observer>(OPort{source},
        [&](const Observation &packet) { fast.add(packet); }, ObserverOptions{1ms});
    const auto fast_id = checked(engine.add_observer(std::move(fast_observer)));
    checked(engine.prepare());
    checked(clock.start());
    const auto slow_id = checked(engine.add_observer(std::make_unique<Observer>(OPort{source},
        [&](const Observation &packet) { slow.add(packet); }, ObserverOptions{10s})));
    expect_error(ErrorCode::Busy, engine.remove_node(source));
    std::this_thread::sleep_for(40ms);
    check(fast.size() == 0 && slow.size() == 0, "An empty observer buffer does not invoke the callback");
    checked(clock.tick());
    check(fast.get(0).frames == render_frames && slow.size() == 0,
          "A live subscription waits to be added to the render plan");
    const auto revision = checked(engine.prepare());
    for (int i = 0; i < 3; ++i)
    {
        checked(clock.tick());
    }
    for (auto *log : {&fast, &slow})
    {
        std::size_t received = 0;
        for (std::size_t i = log == &fast ? 1 : 0; received < 3 * render_frames; ++i)
        {
            const auto batch = log->get(i);
            const auto frame = render_frames + received;
            check(batch.frame == frame && batch.dropped == 0 && batch.channels == 2 &&
                      batch.first == frame && batch.last == 4096 + frame + batch.frames - 1 &&
                      batch.frames % render_frames == 0,
                  "The shared timer delivers the available planar samples in order at either requested interval");
            received += batch.frames;
        }
        check(received == 3 * render_frames, "Delivery keeps the available length instead of padding to the timer period");
    }
    check(engine.applied_revision() == revision, "A block activates the newly submitted subscription");

    checked(engine.remove_observer(fast_id));
    checked(engine.remove_observer(slow_id));
    checked(engine.remove_node(source));
    checked(engine.prepare());
    checked(clock.tick());
}

void observer_overflow_isolation()
{
    PacketLog slow;
    PacketLog fast;
    CallbackGate gate;
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(1, true));
    auto observer = std::make_unique<Observer>(OPort{source}, [&](const Observation &packet) {
        if (packet.frame_position == 0)
        {
            gate.hold();
        }
        slow.add(packet);
    }, ObserverOptions{1ms, 1});
    checked(engine.add_observer(std::move(observer)));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{source},
        [&](const Observation &packet) { fast.add(packet); })));
    checked(engine.prepare());
    checked(clock.tick());
    gate.wait_for_entry();
    check(fast.get(0).frame == 0, "Both observers receive the first rendered block");
    for (std::size_t tick = 1; tick <= 3; ++tick)
    {
        checked(clock.tick());
        const auto packet = fast.get(tick);
        check(packet.frame == tick * render_frames && packet.dropped == 0,
              "A fast observer keeps receiving blocks while another callback is held");
    }
    gate.release();
    check(slow.get(1).frame == render_frames, "The old queued block survives overflow");
    checked(clock.tick());
    const auto after_gap = slow.get(2);
    check(after_gap.frame == 4 * render_frames && after_gap.dropped == 2 * render_frames,
          "The slow observer reports the exact gap when delivery resumes");
}

std::shared_ptr<Observer> make_running_observer(std::size_t capacity, ObserverCallback callback)
{
    const EngineSpec spec(render_rate, render_period);
    auto observer = std::make_shared<Observer>(OPort{NodeId{1}}, std::move(callback), ObserverOptions{0ns, capacity});
    observer->configure(checked(observer->prepare_format(spec, PortSpec(1))));
    observer->start();
    return observer;
}

void observer_bounded_delivery()
{
    CallbackGate gate;
    PacketLog packets;
    auto delivery = make_running_observer(3, [&](const Observation &observation) {
        packets.add(observation);
        if (observation.frame_position == 0)
        {
            gate.hold();
        }
    });
    float samples[render_frames]{};
    const IBlock block{samples, 1, render_frames, render_frames};
    {
        ObserverWorkers workers;
        delivery->write(block, 0);
        delivery->write(block, 2 * render_frames);
        delivery->write(block, 3 * render_frames);
        workers.schedule(delivery, nullptr);
        gate.wait_for_entry();
        delivery->write(block, 4 * render_frames); // Wrap while the first callback holds the scratch buffer.
        gate.release();
    }
    check(packets.size() == 2 && packets.get(1).frame == 2 * render_frames &&
              packets.get(1).frames == 2 * render_frames && packets.get(1).dropped == render_frames,
          "One job delivers every continuous batch in its snapshot and accounts for the gap");
    {
        ObserverWorkers workers;
        workers.schedule(delivery, nullptr);
    }
    check(packets.size() == 3 && packets.get(2).frame == 4 * render_frames && packets.get(2).dropped == 0,
          "Data written during delivery remains in the queue for the next job");
}

void observer_queued_task_invalidation()
{
    CallbackGate first;
    CallbackGate second;
    PacketLog packets;
    PacketLog rearmed_packets;
    auto blocker_a = make_running_observer(1, [&](const Observation &) { first.hold(); });
    auto blocker_b = make_running_observer(1, [&](const Observation &) { second.hold(); });
    auto cancelled = make_running_observer(1, [&](const Observation &observation) { packets.add(observation); });
    auto resumed = make_running_observer(1, [&](const Observation &observation) { packets.add(observation); });
    auto rearmed = make_running_observer(1, [&](const Observation &observation) { rearmed_packets.add(observation); });
    float samples[render_frames]{};
    const IBlock block{samples, 1, render_frames, render_frames};
    {
        ObserverWorkers workers;
        for (const auto &delivery : {blocker_a, blocker_b})
        {
            delivery->write(block, 0);
            workers.schedule(delivery, nullptr);
        }
        first.wait_for_entry();
        second.wait_for_entry();
        for (const auto &delivery : {cancelled, resumed, rearmed})
        {
            delivery->write(block, 0);
            workers.schedule(delivery, nullptr);
        }
        auto completion = std::async(std::launch::async, [&] {
            cancelled->cancel();
            for (const auto &delivery : {resumed, rearmed})
            {
                delivery->request_stop();
                delivery->configure(Observer::Format{});
                delivery->start();
                delivery->write(block, render_frames);
            }
            workers.schedule(rearmed, nullptr);
        });
        const auto ready = completion.wait_for(500ms) == std::future_status::ready;
        first.release();
        second.release();
        completion.get();
        check(ready, "Cancellation and reconfiguration do not wait for unrelated running callbacks");
    }
    check(packets.size() == 0, "Old queued tasks cannot deliver after cancellation or reconfiguration");
    check(rearmed_packets.size() == 1 && rearmed_packets.get(0).frame == render_frames,
          "A new scheduling request can reuse the pending handler after reconfiguration");
    {
        ObserverWorkers workers;
        workers.schedule(resumed, nullptr);
    }
    check(packets.size() == 1 && packets.get(0).frame == render_frames && packets.get(0).dropped == render_frames,
          "The resumed subscription delivers only new data and preserves its frame accounting");
}

void live_graph_edits()
{
    PacketLog packets;
    Engine engine(render_rate, render_period);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto output = checked(engine.node<VirtualNode<>>(bus));
    const auto conn = checked(engine.connect({source}, {bus}));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{bus},
        [&](const Observation &packet) { packets.add(packet); })));
    auto revision = checked(engine.prepare());
    checked(engine.start());
    const auto first = packets.get(0);
    check(first.first == 1 && first.last == 1, "The timer renders the initial graph");

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
        checked(engine.set_gain(conn, i % 2 ? 1.0f : 0.5f));
        wait_until([&] { return engine.applied_revision() == next; });
        revision = next;
    }

    const auto last = packets.get(10);
    check(last.frame > first.frame, "The timer and observer keep running through control-side edits");
    checked(engine.stop());
    check(output->prepares == 1 && output->resets == 0 && output->calls > 10,
          "Live topology changes keep the existing node and its DSP state");
}

void pause_shared_branch()
{
    Engine engine(render_rate, render_period);
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
              branch->prepares == 1 && branch->resets == 0 && engine.frame_position() == 3 * render_frames,
          "Resume restores the original branch connections and node state without resetting the clock");
}

void stop_and_observer_removal_barriers()
{
    for (const bool cancel : {false, true})
    {
        CallbackGate gate;
        PacketLog packets;
        Engine engine(render_rate, render_period);
        const auto source = checked(engine.emplace_node<Signal>(1, true));
        auto observer = std::make_unique<Observer>(OPort{source}, [&](const Observation &packet) {
            if (packet.frame_position == 0)
            {
                expect_error(ErrorCode::Busy, engine.stop());
                gate.hold();
            }
            packets.add(packet);
        });
        const auto id = checked(engine.add_observer(std::move(observer)));
        checked(engine.prepare());
        checked(engine.start()); // Use the real timer and its independent delivery worker.
        gate.wait_for_entry();
        std::promise<void> entered;
        auto called = entered.get_future();
        auto completion = std::async(std::launch::async, [&] {
            entered.set_value();
            return cancel ? engine.remove_observer(id) : engine.stop();
        });
        called.wait();
        const auto waiting = completion.wait_for(30ms) == std::future_status::timeout;
        gate.release();
        checked(completion.get());
        check(waiting, "Completion waits for an in-flight application callback");
        const auto count = packets.size();
        const auto position = engine.frame_position();
        if (cancel)
        {
            wait_until([&] { return engine.frame_position() > position; });
            check(packets.size() == count, "Cancellation ends delivery while the timer continues");
            checked(engine.stop());
            const auto stopped = engine.frame_position();
            checked(engine.start());
            wait_until([&] { return engine.frame_position() >= stopped + 8 * render_frames; });
            checked(engine.stop());
            check(packets.size() == count, "Restart leaves the cancelled observer inactive in the old plan");
        }
        else
        {
            std::this_thread::sleep_for(20ms);
            check(engine.frame_position() == position && packets.size() == count,
                  "Stopping ends both clock ticks and observation callbacks");
            checked(engine.start());
            const auto restarted = packets.get(count);
            check(restarted.frame >= position, "Restart resumes registered subscriptions at the retained frame position");
            checked(engine.stop());
        }
    }
}

void remove_observer_during_render()
{
    class BlockingSink final : public NodeT<1, 0>
    {
      public:
        explicit BlockingSink(CallbackGate &gate) : NodeT({{1}}, {}), gate_(gate)
        {
        }
        void process(const ProcContext &) noexcept override
        {
            gate_.hold();
        }

      private:
        CallbackGate &gate_;
    };

    CallbackGate gate;
    PacketLog packets;
    Engine engine(render_rate, render_period);
    const auto source = checked(engine.emplace_node<Signal>(1, true));
    checked(engine.emplace_node<BlockingSink>(gate));
    const auto id = checked(engine.add_observer(std::make_unique<Observer>(OPort{source},
        [&](const Observation &packet) { packets.add(packet); })));
    checked(engine.prepare());
    checked(engine.start());
    gate.wait_for_entry();

    auto cancellation = std::async(std::launch::async, [&] { return engine.remove_observer(id); });
    const auto status = cancellation.wait_for(500ms);
    gate.release();
    checked(cancellation.get());
    check(status == std::future_status::ready, "Cancellation only waits for the observer's own work");
    wait_until([&] { return engine.frame_position() > 0; });
    checked(engine.stop());
    check(packets.size() == 0, "The released render cannot deliver to the cancelled observer");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"parameter_queue_and_switch", parameter_queue_and_switch},
        {"plan_reclamation", plan_reclamation},
        {"start_failure_rollback", start_failure_rollback},
        {"observer_registration_and_prepare", observer_registration_and_prepare},
        {"observer_timed_delivery", observer_timed_delivery},
        {"observer_overflow_isolation", observer_overflow_isolation},
        {"observer_bounded_delivery", observer_bounded_delivery},
        {"observer_queued_task_invalidation", observer_queued_task_invalidation},
        {"live_graph_edits", live_graph_edits},
        {"pause_shared_branch", pause_shared_branch},
        {"stop_and_observer_removal_barriers", stop_and_observer_removal_barriers},
        {"remove_observer_during_render", remove_observer_during_render}
    });
}
