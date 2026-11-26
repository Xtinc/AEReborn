#include "tests/support/audio_fixture.h"
#include "tests/support/tone.h"
#include "core/fring.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <future>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
const PortSpec mono(rate, 1, SampleFormat::F32P);

void graph_constraints()
{
    Engine engine(period), other(period);
    ClockedGraph clock(engine);
    expect_error(RetCode::EPARAM, [] { Engine invalid(static_cast<BlockPeriod>(0)); });
    expect_error(RetCode::ESTATE, [&] { engine.start(); });
    const auto source = engine.emplace_node<Tone>(rate, 1, 997, 0.25f);
    const auto first = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto second = engine.emplace_node<Matrix>(rate, 1);
    const auto stereo = engine.emplace_node<Matrix>(rate, 2);
    const auto foreign = other.emplace_node<Tone>(rate, 1, 997, 0.25f);
    const auto sink = engine.emplace_node<PacketSink>(mono);
    const auto input = connect(engine, source, first);
    connect(engine, first, second);
    connect(engine, second, sink);
    expect_error(RetCode::DUPLICATE, [&] { connect(engine, source, first); });
    expect_error(RetCode::INPUTBUSY, [&] { connect(engine, second, first); });
    expect_error(RetCode::EPORT, [&] { connect(engine, source, first, 2); });
    expect_error(RetCode::INVFMT, [&] { connect(engine, source, stereo); });
    expect_error(RetCode::ENODE, [&] { connect(engine, foreign, first, 1); });
    expect_error(RetCode::ETYPE, [&] { engine.node<Mixer>(source); });
    engine.set_paused(second, true);
    expect_error(RetCode::CYCLE, [&] { connect(engine, second, first, 1); });
    engine.set_paused(second, false);
    engine.commit();
    clock.start();
    clock.advance();
    const auto *result = engine.node<PacketSink>(sink);
    check(result->frames == 240 && result->sample(1) > 0, "Rejected edits leave the valid route active");
    engine.disconnect(input);
    clock.commit();
    clock.advance();
    check(result->sample(1) == 0, "An unconnected mixer input renders silence");
    expect_error(RetCode::ECONN, [&] { engine.disconnect(input); });
}

class SplitSignal final : public NodeT<2, 2>
{
  public:
    SplitSignal() : NodeT({mono, mono}, {mono, mono}) {}

    void process(const ProcessContext &context) noexcept override
    {
        const auto first = context.inputs[0].f32p_view();
        const auto second = context.inputs[1].f32p_view();
        const auto generated = context.outputs[0].f32p_view();
        const auto copied = context.outputs[1].f32p_view();
        const bool copy = copy_enabled.load(std::memory_order_relaxed);
        for (std::size_t frame = 0; frame < copied.frames(); ++frame)
        {
            const auto sample = first.channel(0)[frame] + second.channel(0)[frame];
            generated.channel(0)[frame] = sample + 0.25f;
            copied.channel(0)[frame] = copy ? sample : 0.0f;
        }
        context.set_output_active(0, true);
        if (!copy)
            context.set_output_active(1, false);
    }

    std::atomic<bool> copy_enabled{true};
};

void composite_node_id()
{
    Engine engine(period);
    const auto source = engine.emplace_node<Tone>(rate, 1, 997, 0.25f);
    const auto split = engine.emplace_node<SplitSignal>();
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto sink = engine.emplace_node<PacketSink>(mono);
    engine.connect(source, split[1]);
    const auto route = engine.connect(split[1], mix[1]);
    engine.connect(mix, sink);
    engine.commit();

    expect_error(RetCode::EPORT, [&] { engine.connect(source, mix[65536]); });
    expect_error(RetCode::EPORT, [&] { engine.connect(split[65536], mix); });
    expect_error(RetCode::EPORT, [&] { engine.connect(sink, source); });
    expect_error(RetCode::EPORT, [&] { engine.connect(mix, source); });
    expect_error(RetCode::EPORT, [&] { engine.remove_node(mix[1]); });
    expect_error(RetCode::EPORT, [&] { engine.replace_node(mix[1], std::make_unique<Mixer>(rate, 1, 2)); });
    expect_error(RetCode::EPORT, [&] { engine.set_paused(mix[1], true); });
    expect_error(RetCode::EPORT, [&] { engine.node(mix[1]); });
    expect_error(RetCode::EPORT, [&] { engine.plan_node(mix[1]); });
    check(!engine.dirty() && engine.node(mix[0]) == engine.plan_node(mix), "Rejected edits leave the graph intact");
    const auto connections = engine.plan_connections();
    check(connections.size() == 3 && connections[1].id == route && connections[1].from == split[1] &&
              connections[1].to == mix[1],
          "The committed connection preserves both port indices");

    engine.remove_node(split);
    expect_error(RetCode::ECONN, [&] { engine.disconnect(route); });
    engine.connect(source, mix[1]);
    engine.commit();
    check(engine.plan_connections().size() == 2, "Removing a node clears links at nonzero ports");
}

void port_activity()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto tone = engine.emplace_node<Tone>(rate, 1, 1000, 0.125f);
    const auto silence = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.0f; });
    const auto split = engine.emplace_node<SplitSignal>();
    const auto mixer = engine.emplace_node<Mixer>(rate, 1, 1);
    const auto mixed = engine.emplace_node<PacketSink>(mono);
    const auto direct = engine.emplace_node<PacketSink>(mono);
    const auto copied = engine.emplace_node<PacketSink>(mono);
    const auto fanout = engine.emplace_node<PacketSink>(mono);
    const auto source = engine.emplace_node<PacketSink>(mono);
    connect(engine, tone, source);
    connect(engine, split, mixer);
    connect(engine, mixer, mixed);
    connect(engine, split, direct);
    engine.connect(split[1], copied);
    engine.connect(split[1], fanout);
    engine.commit();
    const auto *mixed_result = engine.node<PacketSink>(mixed);
    const auto *direct_result = engine.node<PacketSink>(direct);
    const auto *copied_result = engine.node<PacketSink>(copied);
    const auto *fanout_result = engine.node<PacketSink>(fanout);
    const auto *source_result = engine.node<PacketSink>(source);
    const auto check_gap = [&] {
        check(mixed_result->active && direct_result->active && mixed_result->sample(1) == 0.25f &&
                  direct_result->sample(1) == 0.25f,
              "An explicit active flag keeps the generated signal on both routes");
        check(!copied_result->active && !fanout_result->active && copied_result->sample(1) == 0.0f &&
                  fanout_result->sample(1) == 0.0f,
              "With all inputs inactive, both readers see the inactive default and zero samples");
    };
    const auto check_signal = [&] {
        check(source_result->active && source_result->sample(1) > 0.0f,
              "A source with no inputs defaults to active");
        check(copied_result->active && fanout_result->active &&
                  copied_result->sample(1) == source_result->sample(1) &&
                  fanout_result->sample(1) == source_result->sample(1),
              "An active second input restores the output default on both readers");
    };

    clock.start();
    clock.advance();
    check_gap();
    connect(engine, tone, split, 1);
    clock.commit();
    clock.advance();
    check_signal();
    auto &copy_enabled = engine.node<SplitSignal>(split)->copy_enabled;
    copy_enabled.store(false, std::memory_order_relaxed);
    clock.advance();
    check(!copied_result->active && copied_result->sample(1) == 0.0f,
          "An inactive output clears samples left by the last active block");
    copy_enabled.store(true, std::memory_order_relaxed);
    clock.advance();
    check_signal();
    engine.set_paused(tone, true);
    clock.commit();
    clock.advance();
    check_gap();
    check(!source_result->active, "A paused source leaves its directly connected sink inactive");
    connect(engine, silence, split);
    clock.commit();
    clock.advance();
    check(copied_result->active && fanout_result->active && copied_result->sample(1) == 0.0f,
          "An active first input propagates even when its samples are all zero");
    engine.set_paused(tone, false);
    clock.commit();
    clock.advance();
    check_signal();
}

class PreparingGain final : public NodeT<1, 1>
{
  public:
    PreparingGain() : NodeT({mono}, {mono}) {}
    void prepare(BlockPeriod) override
    {
        ++prepares;
        if (fail)
            throw Error(RetCode::IOFAILED);
    }
    void process(const ProcessContext &context) noexcept override
    {
        ++calls;
        const auto input = context.inputs[0].f32p_view();
        const auto output = context.outputs[0].f32p_view();
        for (std::size_t frame = 0; frame < output.frames(); ++frame)
            output.channel(0)[frame] = 2.0f * input.channel(0)[frame];
    }
    void reset() noexcept override { ++resets; }

    bool fail = true;
    std::size_t prepares = 0, calls = 0, resets = 0;
};

void commit_failure_and_retry()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.125f; });
    const auto sink = engine.emplace_node<PacketSink>(mono);
    const auto direct = connect(engine, source, sink);
    engine.commit();
    const auto original_generation = engine.plan_generation();
    engine.commit();
    check(engine.plan_generation() == original_generation && !engine.dirty(),
          "An unchanged commit retains the active plan generation");
    auto *result = engine.node<PacketSink>(sink);
    clock.start();
    clock.advance();
    check(result->sample(1) == 0.125f, "The original graph is committed");
    const auto inserted = engine.emplace_node<PreparingGain>();
    auto *processor = engine.node<PreparingGain>(inserted);
    engine.disconnect(direct);
    connect(engine, source, inserted);
    connect(engine, inserted, sink);
    expect_error(RetCode::IOFAILED, [&] { engine.commit(); });
    const auto old_connections = engine.plan_connections();
    check(old_connections.size() == 1 && old_connections[0].id == direct &&
              engine.plan_generation() == original_generation && engine.dirty(),
          "A failed commit preserves the published connections and generation");
    clock.advance();
    check(result->sample(1) == 0.125f && processor->calls == 0, "Failed preparation leaves the old plan live");
    processor->fail = false;
    clock.commit();
    clock.advance();
    check(result->sample(1) == 0.25f && processor->calls > 0, "Retry publishes the complete new route");
    engine.set_paused(inserted, true);
    clock.commit();
    const auto calls = processor->calls;
    clock.advance();
    check(result->sample(1) == 0 && processor->calls == calls, "Pause drops the branch from the current plan");
    engine.set_paused(inserted, false);
    clock.commit();
    clock.advance();
    check(result->sample(1) == 0.25f && processor->prepares == 2, "Resume reuses the prepared processor");
    clock.stop();
    engine.reset();
    check(engine.tick_position() == 0 && processor->resets == 1, "Reset restarts graph time and node state");
    auto replacement = std::make_unique<PreparingGain>();
    replacement->fail = false;
    const auto *new_processor = replacement.get();
    engine.replace_node(inserted, std::move(replacement));
    check(engine.plan_node(inserted) == processor, "The active plan keeps the old instance through a draft replacement");
    const auto generation_before_replace = engine.plan_generation();
    engine.commit();
    check(engine.plan_node(inserted) == new_processor && engine.plan_generation() == generation_before_replace + 1,
          "Publication changes the borrowed instance and generation together");
}

class InFlightSink final : public NodeT<1, 0>
{
  public:
    InFlightSink(std::promise<void> &entered, std::shared_future<void> release, std::atomic<bool> &destroyed)
        : NodeT({mono}, {}), entered_(entered), release_(std::move(release)), destroyed_(destroyed)
    {
    }
    ~InFlightSink() override { destroyed_.store(true); }

    void process(const ProcessContext &) noexcept override
    {
        if (!reported_)
        {
            reported_ = true;
            entered_.set_value();
            release_.wait_for(3s);
        }
    }

  private:
    std::promise<void> &entered_;
    std::shared_future<void> release_;
    std::atomic<bool> &destroyed_;
    bool reported_ = false;
};

void clock_boundary_barriers()
{
    for (const bool commit : {false, true})
    {
        std::promise<void> entered, release;
        std::atomic<bool> destroyed{false};
        Engine engine(period);
        const auto sink = engine.emplace_node<InFlightSink>(entered, release.get_future().share(), destroyed);
        engine.commit();
        engine.start();
        check(entered.get_future().wait_for(2s) == std::future_status::ready, "The old plan enters rendering");
        engine.remove_node(sink);
        std::promise<void> calling;
        auto started = calling.get_future();
        auto completion = std::async(std::launch::async, [&] {
            calling.set_value();
            if (commit)
                engine.commit();
            else
                engine.stop();
        });
        started.wait();
        const auto blocked = completion.wait_for(25ms) == std::future_status::timeout;
        const auto alive = !destroyed.load();
        release.set_value();
        completion.get();
        check(blocked && alive, "Control waits until the in-flight block releases the old node");
        if (!commit)
            engine.commit();
        check(destroyed.load(), "Committing the removal releases the node");
    }
}

void fractional_frames_and_ring_wrap()
{
    const PortSpec spec(BandWidth::CDQuality, 2, SampleFormat::F32P);
    std::size_t total = 0;
    for (std::uint64_t tick = 0; tick < 200; ++tick)
    {
        const auto frames = spec.frames(period, tick);
        check(frames == (tick % 2 ? 220 : 221), "44100 Hz alternates 221 and 220 frames per tick");
        total += frames;
        check(spec.frame_position(period, tick + 1) == total, "Frame positions agree with accumulated blocks");
    }
    check(total == 44100, "The graph advances exactly one second of fractional blocks");
    FrameRing ring;
    ring.prepare(2, 5);
    Buffer input, output;
    input.resize(2, 8);
    output.resize(2, 5);
    auto source = input.block(8);
    auto result = output.block(5);
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t ch = 0; ch < 2; ++ch)
            source.channel(ch)[frame] = static_cast<float>(10 * ch + frame);
    check(ring.write(source) == 5 && ring.read(result.subblock(0, 3)) == 3 &&
              ring.write(source.subblock(5, 3)) == 3 && ring.read(result) == 5 && !ring.available(),
          "A full ring accepts only the free prefix and wraps across the storage boundary");
    for (std::size_t frame = 0; frame < 5; ++frame)
        for (std::size_t ch = 0; ch < 2; ++ch)
            check(result.channel(ch)[frame] == source.channel(ch)[frame + 3], "Planar ring wrap preserves channel order");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"graph_constraints", graph_constraints},
                                                {"composite_node_id", composite_node_id},
                                                {"port_activity", port_activity},
                                                {"commit_failure_and_retry", commit_failure_and_retry},
                                                {"clock_boundary_barriers", clock_boundary_barriers},
                                                {"fractional_frames_and_ring_wrap", fractional_frames_and_ring_wrap}});
}
