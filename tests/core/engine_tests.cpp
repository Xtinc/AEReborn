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
    expect_error(ErrorCode::InvalidArg, [] { Engine invalid(static_cast<BlockPeriod>(0)); });
    expect_error(ErrorCode::NotReady, [&] { engine.start(); });
    const auto source = engine.emplace_node<Tone>(rate, 1, 997, 0.25f);
    const auto first = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto second = engine.emplace_node<Matrix>(rate, 1);
    const auto stereo = engine.emplace_node<Matrix>(rate, 2);
    const auto foreign = other.emplace_node<Tone>(rate, 1, 997, 0.25f);
    const auto sink = engine.emplace_node<PacketSink>(mono);
    const auto input = connect(engine, source, first);
    connect(engine, first, second);
    connect(engine, second, sink);
    expect_error(ErrorCode::DuplicateConn, [&] { connect(engine, source, first); });
    expect_error(ErrorCode::InputBusy, [&] { connect(engine, second, first); });
    expect_error(ErrorCode::InvalidPort, [&] { connect(engine, source, first, 2); });
    expect_error(ErrorCode::FormatMismatch, [&] { connect(engine, source, stereo); });
    expect_error(ErrorCode::UnknownNode, [&] { connect(engine, foreign, first, 1); });
    expect_error(ErrorCode::InvalidType, [&] { engine.node<Mixer>(source); });
    engine.set_paused(second, true);
    expect_error(ErrorCode::Cycle, [&] { connect(engine, second, first, 1); });
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
    expect_error(ErrorCode::UnknownConn, [&] { engine.disconnect(input); });
}

class PreparingGain final : public NodeT<1, 1>
{
  public:
    PreparingGain() : NodeT({mono}, {mono}) {}
    void prepare(BlockPeriod) override
    {
        ++prepares;
        if (fail)
            throw Error(ErrorCode::IoFailed);
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
    auto *result = engine.node<PacketSink>(sink);
    clock.start();
    clock.advance();
    check(result->sample(1) == 0.125f, "The original graph is committed");
    const auto inserted = engine.emplace_node<PreparingGain>();
    auto *processor = engine.node<PreparingGain>(inserted);
    engine.disconnect(direct);
    connect(engine, source, inserted);
    connect(engine, inserted, sink);
    expect_error(ErrorCode::IoFailed, [&] { engine.commit(); });
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
    const PortSpec spec(SampleRate::Hz44100, 2, SampleFormat::F32P);
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
                                                {"commit_failure_and_retry", commit_failure_and_retry},
                                                {"clock_boundary_barriers", clock_boundary_barriers},
                                                {"fractional_frames_and_ring_wrap", fractional_frames_and_ring_wrap}});
}
