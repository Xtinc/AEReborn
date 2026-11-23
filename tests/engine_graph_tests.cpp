#include "core/engine.h"
#include "core/fring.h"
#include "support/clocked_graph.h"
#include "support/test_harness.h"
#include "support/test_node.h"
#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
static_assert(std::is_convertible<OFBlock, IFBlock>::value, "Writable blocks become read-only views");
static_assert(!std::is_convertible<IFBlock, OFBlock>::value, "Read-only blocks cannot become writable");
static_assert(std::is_convertible<OBBlock, IBBlock>::value, "Port outputs become read-only inputs");
static_assert(!std::is_convertible<IBBlock, OBBlock>::value, "An input cannot become a writable port output");
static_assert(!std::is_constructible<PortSpec, SampleRate, std::size_t>::value,
              "Specifying a port requires a sample format");
constexpr RetCode busy(ErrorCode::Busy);
static_assert(busy.code() == ErrorCode::Busy, "The stored error code is accessible");
static_assert(busy == RetCode(ErrorCode::Busy) && busy != RetCode{}, "Return codes compare by error code");
static_assert(busy == ErrorCode::Busy && ErrorCode::Busy == busy, "Equality accepts an error code on either side");
static_assert(busy != ErrorCode::Ok && ErrorCode::Ok != busy, "Inequality accepts an error code on either side");
static_assert(std::is_same<decltype(std::declval<Result<std::size_t> &>().value()), std::size_t &>::value,
              "A mutable result exposes its value by reference");

void frame_ring()
{
    FrameRing ring;
    ring.prepare(2, 5);
    std::array<float, 16> samples{}, played{};
    const OFBlock input(samples.data(), 2, 8, 8), output(played.data(), 2, 8, 8);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t frame = 0; frame < input.frames(); ++frame)
            input.channel(ch)[frame] = static_cast<float>(100 * ch + frame + 1);
    check(ring.write(input.subblock(0, 1)) == 1 && ring.read(output.subblock(0, 1)) == 1,
          "The ring can start subsequent transfers at a nonzero physical offset");
    for (int pass = 0; pass < 6; ++pass)
    {
        check(ring.write(input.subblock(1, 4)) == 4 && ring.write(input.subblock(5, 2)) == 1,
              "Ring stores complete frames up to capacity");
        check(ring.write(input) == 0 && ring.available() == 5, "A full ring keeps its old samples");
        expect_error(ErrorCode::TooLarge, [&] { ring.prepare(2, std::numeric_limits<std::size_t>::max()); });
        played.fill(-1.0f);
        check(ring.read(output.subblock(2, 6)) == 5 && ring.read(output) == 0,
              "Reading drains only available frames after a rejected prepare");
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            check(std::equal(input.channel(ch) + 1, input.channel(ch) + 6,
                             output.channel(ch) + 2),
                  "Both channels retain their order across cursor wraps");
            check(output.channel(ch)[1] == -1 && output.channel(ch)[7] == -1,
                  "Raw ring reads leave surrounding frames alone");
        }
        check(ring.read(OFBlock{nullptr, 2, 0, 0}) == 0 && ring.write(IFBlock{nullptr, 2, 0, 0}) == 0,
              "Empty views do not touch sample storage");
    }
}

void audio_block_layout()
{
    Buffer buffer;
    buffer.resize(2, 8);
    auto full = buffer.block(8);
    std::fill_n(full.channel(0), 8, 1.0f);
    std::fill_n(full.channel(1), 8, 2.0f);
    buffer.block(4).clear();
    const IFBlock read = full;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (std::size_t frame = 0; frame < 8; ++frame)
        {
            check(read.channel(ch)[frame] == (frame < 4 ? 0 : ch + 1),
                  "Planar stride and processing span are independent");
        }
    }
    expect_error(ErrorCode::InvalidArg, [&] { buffer.resize(0, 8); });
    expect_error(ErrorCode::InvalidArg, [&] { buffer.resize(1, 0); });
    expect_error(ErrorCode::TooLarge, [&] { buffer.resize(std::numeric_limits<std::size_t>::max(), 8); });
    const auto retained = buffer.block(8);
    check(retained.channel(1) - retained.channel(0) == 8 && retained.channel(1)[7] == 2,
          "Rejected resize preserves storage and layout");
    const View<const float> empty;
    check(empty.data() == nullptr && empty.begin() == empty.end(),
          "A default view supports iteration without pointer arithmetic on null");
}

void engine_timing_validation()
{
    for (const auto period : {static_cast<BlockPeriod>(0), static_cast<BlockPeriod>(15)})
        expect_error(ErrorCode::InvalidArg, [&] { Engine engine(period); });
    const struct
    {
        BlockPeriod period;
        std::uint64_t ticks_per_second;
        std::uint64_t fractional_cycle_ticks;
        std::size_t short_frames;
        std::size_t integer_frames;
    } cases[] = {{BlockPeriod::Ms02, 500, 5, 88, 96}, {render_period, 200, 2, 220, 240}};
    const PortSpec fractional(SampleRate::Hz44100, 2, SampleFormat::F32P);
    const PortSpec integer(SampleRate::Hz48000, 1, SampleFormat::F32P);
    const auto large = std::numeric_limits<std::uint64_t>::max() - 1;
    for (const auto &test : cases)
    {
        Engine engine(test.period);
        engine.commit();
        check(engine.block_period() == test.period, "The engine exposes its configured block period");
        check(fractional.max_frames(test.period) == test.short_frames + 1,
              "Capacity covers the longer fractional block");
        std::size_t total = 0;
        for (std::uint64_t tick = 0; tick < test.ticks_per_second; ++tick)
        {
            const auto frames = fractional.frames(test.period, tick);
            check(frames == test.short_frames + (tick % test.fractional_cycle_ticks == 0),
                  "Fractional frames follow the shared tick timeline");
            total += frames;
            check(total == fractional.frame_position(test.period, tick + 1), "No cumulative rounding drift");
        }
        check(total == 44100, "One second contains exactly 44100 frames");
        check(fractional.frames(test.period, large) == test.short_frames + (large % test.fractional_cycle_ticks == 0),
              "Per-block frame counts avoid tick multiplication overflow");
        check(integer.frames(test.period, large) == test.integer_frames,
              "Integer frame counts are constant across the tick timeline");
    }
}

void connection_validation()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    expect_error(ErrorCode::InvalidArg, [&] { engine.add_node(nullptr); });
    expect_error(ErrorCode::InvalidArg, [&] { engine.emplace_node<VirtualNode<0, 1>>(0); });
    const auto source = engine.emplace_node<VirtualNode<0, 1>>();
    const auto sink = engine.emplace_node<VirtualNode<1, 0>>();
    const auto stereo = engine.emplace_node<VirtualNode<1, 0>>(2);
    const auto connection = engine.connect(OPORT(source, 0), IPORT(sink, 0));
    expect_error(ErrorCode::DuplicateConn, [&] { engine.connect(OPORT(source, 0), IPORT(sink, 0)); });
    const auto second = engine.emplace_node<VirtualNode<0, 1>>();
    expect_error(ErrorCode::InputBusy, [&] { engine.connect(OPORT(second, 0), IPORT(sink, 0)); });
    expect_error(ErrorCode::InvalidPort, [&] { engine.connect(OPORT(sink, 0), IPORT(source, 0)); });
    expect_error(ErrorCode::InvalidPort, [&] { engine.connect(OPORT(source, 1), IPORT(sink, 0)); });
    expect_error(ErrorCode::InvalidPort, [&] { engine.connect(OPORT(source, 0), IPORT(sink, 1)); });
    expect_error(ErrorCode::FormatMismatch, [&] { engine.connect(OPORT(source, 0), IPORT(stereo, 0)); });
    expect_error(ErrorCode::InvalidType, [&] { engine.node<VirtualNode<>>(source); });
    Engine other(render_period);
    const auto foreign = other.emplace_node<VirtualNode<>>();
    expect_error(ErrorCode::UnknownNode, [&] { engine.connect(OPORT(foreign, 0), IPORT(sink, 0)); });
    expect_error(ErrorCode::UnknownNode, [&] { engine.connect(OPORT(source, 0), IPORT(foreign, 0)); });
    engine.commit();
    clock.start();
    clock.advance(); // Rejected graph edits must not invalidate the prepared graph.
    engine.remove_node(source);
    expect_error(ErrorCode::UnknownNode, [&] { engine.node(source); });
    expect_error(ErrorCode::UnknownConn, [&] { engine.disconnect(connection); });
    expect_error(ErrorCode::UnknownNode, [&] { engine.remove_node(source); });
}

void feedback_rejection()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    const auto a = engine.emplace_node<VirtualNode<2, 2>>();
    const auto b = engine.emplace_node<VirtualNode<>>();
    const auto c = engine.emplace_node<VirtualNode<>>();
    const auto sink = engine.emplace_node<VirtualNode<1, 0>>();
    engine.connect(OPORT(a, 0), IPORT(b, 0));
    engine.connect(OPORT(b, 0), IPORT(c, 0));
    engine.connect(OPORT(c, 0), IPORT(sink, 0));
    engine.commit();
    expect_error(ErrorCode::Cycle, [&] { engine.connect(OPORT(c, 0), IPORT(a, 1)); });
    expect_error(ErrorCode::Cycle, [&] { engine.connect(OPORT(a, 1), IPORT(a, 1)); });
    engine.set_paused(b, true);
    expect_error(ErrorCode::Cycle, [&] { engine.connect(OPORT(c, 0), IPORT(a, 1)); });
    engine.set_paused(b, false);
    clock.start();
    clock.advance();
    check(engine.node<VirtualNode<2, 2>>(a)->calls == 1,
          "Rejected feedback leaves the valid graph executable");
    const auto x = engine.emplace_node<VirtualNode<>>();
    const auto y = engine.emplace_node<VirtualNode<>>();
    engine.connect(OPORT(x, 0), IPORT(y, 0));
    expect_error(ErrorCode::Cycle, [&] { engine.connect(OPORT(y, 0), IPORT(x, 0)); });
}

void commit_retry()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    expect_error(ErrorCode::NotReady, [&] { engine.start(); });
    engine.commit();
    clock.start();
    clock.advance();
    check(engine.tick_position() == 1, "A prepared graph advances one period");
    const auto processor = engine.emplace_node<VirtualNode<>>();
    const auto sink = engine.emplace_node<VirtualNode<1, 0>>();
    engine.connect(OPORT(processor, 0), IPORT(sink, 0));
    auto *node = engine.node<VirtualNode<>>(processor);
    node->preparation = ErrorCode::InvalidArg;
    expect_error(ErrorCode::InvalidArg, [&] { engine.commit(); });
    clock.advance();
    check(engine.tick_position() == 2 && node->calls == 0, "Preparation failure leaves the old plan running");
    node->preparation = {};
    clock.commit();
    engine.commit(); // A clean draft succeeds while the clock is parked.
    const auto calls_after_commit = node->calls;
    clock.advance();
    check(node->prepares == 2 && node->calls == calls_after_commit + 1, "Preparation is retryable and then idempotent");
    const auto huge = engine.emplace_node<VirtualNode<1, 0>>(std::numeric_limits<std::size_t>::max());
    expect_error(ErrorCode::TooLarge, [&] { engine.commit(); });
    clock.advance();
    engine.remove_node(huge);
    clock.commit();
    const auto previous_calls = node->calls;
    clock.advance();
    check(node->prepares == 2 && node->calls == previous_calls + 1, "Capacity failure preserves previously prepared DSP state");
    clock.stop();
    engine.reset();
    check(engine.tick_position() == 0 && node->resets == 1, "Reset reaches nodes and restarts the graph clock");
}

void buffer_pool_capacity()
{
    Engine engine(render_period);
    const auto channels = std::vector<float>{}.max_size() / (2 * render_frames) + 1;
    const auto source = engine.emplace_node<VirtualNode<0, 1>>(channels);
    const auto sink = engine.emplace_node<VirtualNode<2, 0>>(channels);
    engine.connect(OPORT(source, 0), IPORT(sink, 0));
    expect_error(ErrorCode::TooLarge, [&] { engine.commit(); });
    engine.remove_node(sink);
    engine.commit();
}

class ThrowingNode : public VirtualNode<>
{
  public:
    explicit ThrowingNode(std::exception_ptr construction_error = {})
    {
        if (construction_error)
        {
            std::rethrow_exception(construction_error);
        }
    }
    void prepare(BlockPeriod spec) override
    {
        if (fail_preparation)
        {
            throw std::runtime_error("test preparation");
        }
        VirtualNode<>::prepare(spec);
    }

    bool fail_preparation = true;
};

void node_exception_propagation()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    engine.commit();
    expect_exception<std::bad_alloc>([&]
                                     { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::bad_alloc())); });
    expect_exception<std::length_error>(
        [&] { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::length_error("test capacity"))); });
    expect_exception<std::runtime_error>(
        [&] { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::runtime_error("test construction"))); });
    clock.start();
    clock.advance();

    const auto source = engine.emplace_node<VirtualNode<0, 1>>();
    const auto processor = engine.emplace_node<ThrowingNode>();
    const auto sink = engine.emplace_node<VirtualNode<1, 0>>();
    engine.connect(OPORT(source, 0), IPORT(processor, 0));
    engine.connect(OPORT(processor, 0), IPORT(sink, 0));
    expect_exception<std::runtime_error>([&] { engine.commit(); });
    clock.advance();
    check(engine.tick_position() == 2, "The previous plan runs after a preparation exception");
    auto *node = engine.node<ThrowingNode>(processor);
    node->fail_preparation = false;
    clock.commit();
    const auto previous_calls = node->calls;
    clock.advance();
    check(engine.node<VirtualNode<0, 1>>(source)->prepares == 1 && node->calls == previous_calls + 1,
          "Retry after an exception preserves prepared dependencies");
}

void node_replacement()
{
    Engine engine(render_period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<VirtualNode<0, 1>>();
    const auto sink = engine.emplace_node<VirtualNode<1, 0>>();
    engine.connect(OPORT(source, 0), IPORT(sink, 0));
    engine.commit();
    clock.start();
    clock.advance();
    auto *original = engine.node<VirtualNode<0, 1>>(source);

    expect_error(ErrorCode::InvalidArg, [&] { engine.replace_node(source, nullptr); });
    expect_error(ErrorCode::UnknownNode, [&] { engine.replace_node({}, std::make_unique<VirtualNode<0, 1>>()); });
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>(0)); });
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>(2)); });
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(source, std::make_unique<VirtualNode<1, 1>>()); });
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(source, std::make_unique<VirtualNode<0, 2>>()); });
    clock.advance();
    check(engine.node(source) == original && original->calls == 2,
          "Rejected replacements preserve the running node and its connections");

    engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>());
    auto *replacement = engine.node<VirtualNode<0, 1>>(source);
    clock.commit();
    auto *old_sink = engine.node<VirtualNode<1, 0>>(sink);
    const auto preceding_calls = old_sink->calls;
    const auto preceding_replacement_calls = replacement->calls;
    clock.advance();
    check(replacement->prepares == 1 && replacement->calls == preceding_replacement_calls + 1 &&
              old_sink->calls == preceding_calls + 1,
          "A compatible replacement takes over existing connections at the next block boundary");

    engine.replace_node(sink, std::make_unique<VirtualNode<1, 0>>());
    clock.commit();
    const auto *new_sink = engine.node<VirtualNode<1, 0>>(sink);
    const auto new_sink_calls = new_sink->calls;
    const auto replacement_calls = replacement->calls;
    clock.advance();
    check(replacement->calls == replacement_calls + 1 && new_sink->calls == new_sink_calls + 1,
          "Replacing an input node keeps it connected to its upstream source");

    clock.stop();
    old_sink = engine.node<VirtualNode<1, 0>>(sink);
    engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>());
    auto *draft = engine.node<VirtualNode<0, 1>>(source);
    const auto orphan = engine.emplace_node<VirtualNode<0, 1>>();
    engine.remove_node(sink);
    engine.reset();
    check(replacement->resets == 1 && old_sink->resets == 1 && draft->resets == 1 &&
              engine.node<VirtualNode<0, 1>>(orphan)->resets == 1,
          "Reset reaches retained and draft node instances exactly once, including removed sinks and orphan sources");
}

void handle_uniqueness()
{
    Engine engine(render_period);
    const auto a = engine.emplace_node<VirtualNode<>>();
    const auto b = engine.emplace_node<VirtualNode<>>();
    const auto connection = engine.connect(OPORT(a, 0), IPORT(b, 0));

    Engine other(BlockPeriod::Ms10);
    const auto x = other.emplace_node<VirtualNode<>>();
    const auto y = other.emplace_node<VirtualNode<>>();
    const auto other_connection = other.connect(OPORT(x, 0), IPORT(y, 0));
    std::array<std::uint64_t, 6> ids{{a.value, b.value, connection.value, x.value, y.value, other_connection.value}};
    std::sort(ids.begin(), ids.end());
    check(ids.front() != 0 && std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "Node and connection IDs are nonzero and unique across engines");

    engine.remove_node(a);
    const auto replacement = engine.emplace_node<VirtualNode<>>();
    const auto replacement_connection = engine.connect(OPORT(replacement, 0), IPORT(b, 0));
    check(replacement != a && replacement_connection != connection, "Removed identifiers are not reused");
}

} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv,
                                     {{"audio_block_layout", audio_block_layout},
                                      {"frame_ring", frame_ring},
                                      {"engine_timing_validation", engine_timing_validation},
                                      {"connection_validation", connection_validation},
                                      {"feedback_rejection", feedback_rejection},
                                      {"commit_retry", commit_retry},
                      {"buffer_pool_capacity", buffer_pool_capacity},
                                      {"node_exception_propagation", node_exception_propagation},
                                      {"handle_uniqueness", handle_uniqueness},
                                      {"node_replacement", node_replacement}});
}
