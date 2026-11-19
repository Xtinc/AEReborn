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
    checked(engine.subscribe({source}, {render_frames}, [&](const Observation &packet) { packets.add(packet); }));
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

void observer_packetization()
{
    PacketLog long_packets;
    PacketLog short_packets;
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Signal>(2, true));
    const auto large_frames = render_frames + 17;
    const auto small_frames = render_frames / 2;
    const auto large = checked(engine.subscribe({source}, {large_frames},
        [&](const Observation &packet) { long_packets.add(packet); }));
    const auto small = checked(engine.subscribe({source}, {small_frames},
        [&](const Observation &packet) { short_packets.add(packet); }));
    expect_error(ErrorCode::Busy, engine.remove_node(source));
    checked(engine.prepare());
    for (int i = 0; i < 3; ++i)
    {
        checked(clock.tick());
    }
    const auto first = long_packets.get(0);
    const auto second = long_packets.get(1);
    const auto split = short_packets.get(1);
    check(first.frame == 0 && first.dropped == 0 && first.frames == large_frames && first.channels == 2 &&
              first.first == 0 && first.last == 4096 + large_frames - 1 &&
              second.frame == large_frames && second.dropped == 0 && second.first == large_frames,
          "An observer-only root renders contiguous planar packets spanning engine blocks");
    check(split.frame == small_frames && split.frames == small_frames && split.first == small_frames &&
              split.dropped == 0, "A subscriber can divide engine blocks into smaller packets");

    checked(engine.unsubscribe(large));
    checked(engine.unsubscribe(small));
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
    checked(engine.subscribe({source}, {render_frames, 1}, [&](const Observation &packet) {
        if (packet.frame_position == 0)
        {
            gate.hold();
        }
        slow.add(packet);
    }));
    checked(engine.subscribe({source}, {render_frames}, [&](const Observation &packet) { fast.add(packet); }));
    checked(engine.prepare());
    checked(clock.tick());
    gate.wait_for_entry();
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

void live_graph_edits()
{
    PacketLog packets;
    Engine engine(render_rate, render_period);
    const auto source = checked(engine.emplace_node<Signal>(1, false));
    const auto bus = checked(engine.emplace_node<VirtualNode<>>());
    const auto output = checked(engine.node<VirtualNode<>>(bus));
    const auto conn = checked(engine.connect({source}, {bus}));
    checked(engine.subscribe({bus}, {render_frames}, [&](const Observation &packet) { packets.add(packet); }));
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

void stop_and_unsubscribe_barriers()
{
    for (const bool cancel : {false, true})
    {
        CallbackGate gate;
        PacketLog packets;
        Engine engine(render_rate, render_period);
        const auto source = checked(engine.emplace_node<Signal>(1, true));
        const auto id = checked(engine.subscribe({source}, {render_frames}, [&](const Observation &packet) {
            if (packet.frame_position == 0)
            {
                expect_error(ErrorCode::Busy, engine.stop());
                gate.hold();
            }
            packets.add(packet);
        }));
        checked(engine.prepare());
        checked(engine.start()); // Use the real timer and its independent delivery worker.
        gate.wait_for_entry();
        std::promise<void> entered;
        auto called = entered.get_future();
        auto completion = std::async(std::launch::async, [&] {
            entered.set_value();
            return cancel ? engine.unsubscribe(id) : engine.stop();
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
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"parameter_queue_and_switch", parameter_queue_and_switch},
        {"plan_reclamation", plan_reclamation},
        {"start_failure_rollback", start_failure_rollback},
        {"observer_packetization", observer_packetization},
        {"observer_overflow_isolation", observer_overflow_isolation},
        {"live_graph_edits", live_graph_edits},
        {"stop_and_unsubscribe_barriers", stop_and_unsubscribe_barriers}
    });
}
