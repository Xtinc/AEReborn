#include "core/engine.h"
#include "support/manual_clock.h"
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
static_assert(std::is_convertible<OBlock, IBlock>::value, "Writable blocks become read-only views");
static_assert(!std::is_convertible<IBlock, OBlock>::value, "Read-only blocks cannot become writable");
static_assert(std::is_convertible<OPortBlock, IPortBlock>::value, "Port outputs become read-only inputs");
static_assert(!std::is_convertible<IPortBlock, OPortBlock>::value, "An input cannot become a writable port output");

void audio_block_layout()
{
    Buffer buffer;
    checked(buffer.resize(2, 8));
    auto full = buffer.block(8);
    std::fill_n(full.channel(0), 8, 1.0f);
    std::fill_n(full.channel(1), 8, 2.0f);
    buffer.block(4).clear();
    const IBlock read = full;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (std::size_t frame = 0; frame < 8; ++frame)
        {
            check(read.channel(ch)[frame] == (frame < 4 ? 0 : ch + 1),
                  "Planar stride and processing span are independent");
        }
    }
    expect_error(ErrorCode::InvalidArg, buffer.resize(0, 8));
    expect_error(ErrorCode::InvalidArg, buffer.resize(1, 0));
    expect_error(ErrorCode::TooLarge, buffer.resize(std::numeric_limits<std::size_t>::max(), 8));
    const auto retained = buffer.block(8);
    check(retained.channel(1) - retained.channel(0) == 8 && retained.channel(1)[7] == 2,
          "Rejected resize preserves storage and layout");
    const View<const float> empty(nullptr, 0);
    check(empty.begin() == empty.end(), "An empty view supports iteration without pointer arithmetic on null");
}

void engine_timing_validation()
{
    for (const auto period : {static_cast<BlockPeriod>(0), static_cast<BlockPeriod>(15)})
    {
        Engine engine(period);
        ManualClock clock(engine);
        expect_error(ErrorCode::InvalidArg, engine.prepare());
        expect_error(ErrorCode::NotPrepared, clock.tick());
    }
    const PortSpec spec(SampleRate::Hz44100, 2);
    check(spec.max_frames(render_period) == 221, "Capacity covers the longer fractional block");
    std::size_t total = 0;
    for (std::uint64_t tick = 0; tick < 200; ++tick)
    {
        const auto frames = spec.frames(render_period, tick);
        check(frames == (tick % 2 ? 220 : 221), "Fractional frames follow the shared tick timeline");
        total += frames;
        check(total == spec.frame_position(render_period, tick + 1), "No cumulative rounding drift");
    }
    check(total == 44100, "One second contains exactly 44100 frames");
    const auto large = std::numeric_limits<std::uint64_t>::max() - 1;
    check(spec.frames(render_period, large) == 221, "Per-block frame counts avoid tick multiplication overflow");
}

void connection_validation()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    expect_error(ErrorCode::InvalidArg, engine.add_node(nullptr));
    expect_error(ErrorCode::InvalidArg, engine.emplace_node<VirtualNode<0, 1>>(0));
    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    const auto stereo = checked(engine.emplace_node<VirtualNode<1, 0>>(2));
    const auto connection = checked(engine.connect({source}, {sink}));
    expect_error(ErrorCode::DuplicateConn, engine.connect({source}, {sink}));
    const auto second = checked(engine.emplace_node<VirtualNode<0, 1>>());
    expect_error(ErrorCode::InputConnected, engine.connect({second}, {sink}));
    expect_error(ErrorCode::InvalidPort, engine.connect({sink}, {source}));
    expect_error(ErrorCode::InvalidPort, engine.connect({source, 1}, {sink}));
    expect_error(ErrorCode::InvalidPort, engine.connect({source}, {sink, 1}));
    expect_error(ErrorCode::FormatMismatch, engine.connect({source}, {stereo}));
    expect_error(ErrorCode::InvalidType, engine.node<VirtualNode<>>(source));
    Engine other(render_period);
    const auto foreign = checked(other.emplace_node<VirtualNode<>>());
    expect_error(ErrorCode::UnknownNode, engine.connect({foreign}, {sink}));
    expect_error(ErrorCode::UnknownNode, engine.connect({source}, {foreign}));
    checked(engine.prepare());
    checked(clock.tick()); // Rejected graph edits must not invalidate the prepared graph.
    checked(engine.remove_node(source));
    expect_error(ErrorCode::UnknownNode, engine.node(source));
    expect_error(ErrorCode::UnknownConn, engine.disconnect(connection));
    expect_error(ErrorCode::UnknownNode, engine.remove_node(source));
}

void feedback_rejection()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    const auto a = checked(engine.emplace_node<VirtualNode<2, 2>>());
    const auto b = checked(engine.emplace_node<VirtualNode<>>());
    const auto c = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({a}, {b}));
    checked(engine.connect({b}, {c}));
    checked(engine.connect({c}, {sink}));
    checked(engine.prepare());
    expect_error(ErrorCode::Cycle, engine.connect({c}, {a, 1}));
    expect_error(ErrorCode::Cycle, engine.connect({a, 1}, {a, 1}));
    checked(engine.set_paused(b, true));
    expect_error(ErrorCode::Cycle, engine.connect({c}, {a, 1}));
    checked(engine.set_paused(b, false));
    checked(clock.tick());
    check(checked(engine.node<VirtualNode<2, 2>>(a))->calls == 1,
          "Rejected feedback leaves the valid graph executable");
    const auto x = checked(engine.emplace_node<VirtualNode<>>());
    const auto y = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({x}, {y}));
    expect_error(ErrorCode::Cycle, engine.connect({y}, {x}));
}

void prepare_retry()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    expect_error(ErrorCode::NotPrepared, clock.tick());
    checked(engine.prepare());
    checked(clock.tick());
    check(engine.tick_position() == 1, "An empty prepared graph advances one period");
    const auto processor = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({processor}, {sink}));
    auto *node = checked(engine.node<VirtualNode<>>(processor));
    node->preparation = ErrorCode::InvalidArg;
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    checked(clock.tick());
    check(engine.tick_position() == 2 && node->calls == 0, "Preparation failure leaves the old plan running");
    node->preparation = {};
    checked(engine.prepare());
    checked(engine.prepare());
    checked(clock.tick());
    check(node->prepares == 2 && node->calls == 1, "Preparation is retryable and then idempotent");
    const auto huge = checked(engine.emplace_node<VirtualNode<1, 0>>(std::numeric_limits<std::size_t>::max()));
    expect_error(ErrorCode::TooLarge, engine.prepare());
    checked(clock.tick());
    checked(engine.remove_node(huge));
    checked(engine.prepare());
    checked(clock.tick());
    check(node->prepares == 2 && node->calls == 3, "Capacity failure preserves previously prepared DSP state");
    checked(clock.reset());
    check(engine.tick_position() == 0 && node->resets == 1, "Reset reaches nodes and restarts the graph clock");
}

void buffer_pool_capacity()
{
    Engine engine(render_period);
    const auto channels = std::vector<float>{}.max_size() / (2 * render_frames) + 1;
    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>(channels));
    const auto sink = checked(engine.emplace_node<VirtualNode<2, 0>>(channels));
    checked(engine.connect({source}, {sink, 0}));
    expect_error(ErrorCode::TooLarge, engine.prepare());
    checked(engine.remove_node(sink));
    checked(engine.prepare());
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
    RetCode prepare(BlockPeriod spec) override
    {
        if (fail_preparation)
        {
            throw std::runtime_error("test preparation");
        }
        return VirtualNode<>::prepare(spec);
    }

    bool fail_preparation = true;
};

void node_exception_propagation()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    checked(engine.prepare());
    expect_exception<std::bad_alloc>([&]
                                     { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::bad_alloc())); });
    expect_exception<std::length_error>(
        [&] { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::length_error("test capacity"))); });
    expect_exception<std::runtime_error>(
        [&] { engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::runtime_error("test construction"))); });
    checked(clock.tick());

    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
    const auto processor = checked(engine.emplace_node<ThrowingNode>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({source}, {processor}));
    checked(engine.connect({processor}, {sink}));
    expect_exception<std::runtime_error>([&] { engine.prepare(); });
    checked(clock.tick());
    check(engine.tick_position() == 2, "The previous plan runs after a preparation exception");
    auto *node = checked(engine.node<ThrowingNode>(processor));
    node->fail_preparation = false;
    checked(engine.prepare());
    checked(clock.tick());
    check(checked(engine.node<VirtualNode<0, 1>>(source))->prepares == 1 && node->calls == 1,
          "Retry after an exception preserves prepared dependencies");
}

void node_replacement()
{
    Engine engine(render_period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
    checked(clock.tick());
    auto *original = checked(engine.node<VirtualNode<0, 1>>(source));

    expect_error(ErrorCode::InvalidArg, engine.replace_node(source, nullptr));
    expect_error(ErrorCode::UnknownNode, engine.replace_node({}, std::make_unique<VirtualNode<0, 1>>()));
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>(0)));
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>(2)));
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(source, std::make_unique<VirtualNode<1, 1>>()));
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(source, std::make_unique<VirtualNode<0, 2>>()));
    checked(clock.tick());
    check(checked(engine.node(source)) == original && original->calls == 2,
          "Rejected replacements preserve the running node and its connections");

    checked(engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>()));
    auto *replacement = checked(engine.node<VirtualNode<0, 1>>(source));
    const auto revision = checked(engine.prepare());
    checked(clock.tick());
    check(engine.applied_revision() == revision && replacement->prepares == 1 && replacement->calls == 1 &&
              checked(engine.node<VirtualNode<1, 0>>(sink))->calls == 3,
          "A compatible replacement takes over existing connections at the next block boundary");

    checked(engine.replace_node(sink, std::make_unique<VirtualNode<1, 0>>()));
    checked(engine.prepare());
    checked(clock.tick());
    check(replacement->calls == 2 && checked(engine.node<VirtualNode<1, 0>>(sink))->calls == 1,
          "Replacing an input node keeps it connected to its upstream source");

    checked(clock.stop());
    auto *old_sink = checked(engine.node<VirtualNode<1, 0>>(sink));
    checked(engine.replace_node(source, std::make_unique<VirtualNode<0, 1>>()));
    auto *draft = checked(engine.node<VirtualNode<0, 1>>(source));
    const auto orphan = checked(engine.emplace_node<VirtualNode<0, 1>>());
    checked(engine.remove_node(sink));
    checked(engine.reset());
    check(replacement->resets == 1 && old_sink->resets == 1 && draft->resets == 1 &&
              checked(engine.node<VirtualNode<0, 1>>(orphan))->resets == 1,
          "Reset reaches retained and draft node instances exactly once, including removed sinks and orphan sources");
}

void handle_uniqueness()
{
    Engine engine(render_period);
    const auto a = checked(engine.emplace_node<VirtualNode<>>());
    const auto b = checked(engine.emplace_node<VirtualNode<>>());
    const auto connection = checked(engine.connect({a}, {b}));

    Engine other(BlockPeriod::Ms10);
    const auto x = checked(other.emplace_node<VirtualNode<>>());
    const auto y = checked(other.emplace_node<VirtualNode<>>());
    const auto other_connection = checked(other.connect({x}, {y}));
    std::array<std::uint64_t, 6> ids{{a.value, b.value, connection.value, x.value, y.value, other_connection.value}};
    std::sort(ids.begin(), ids.end());
    check(ids.front() != 0 && std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "Node and connection IDs are nonzero and unique across engines");

    checked(engine.remove_node(a));
    const auto replacement = checked(engine.emplace_node<VirtualNode<>>());
    const auto replacement_connection = checked(engine.connect({replacement}, {b}));
    check(replacement != a && replacement_connection != connection, "Removed identifiers are not reused");
}

} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv,
                                     {{"audio_block_layout", audio_block_layout},
                                      {"engine_timing_validation", engine_timing_validation},
                                      {"connection_validation", connection_validation},
                                      {"feedback_rejection", feedback_rejection},
                      {"prepare_retry", prepare_retry},
                      {"buffer_pool_capacity", buffer_pool_capacity},
                                      {"node_exception_propagation", node_exception_propagation},
                                      {"handle_uniqueness", handle_uniqueness},
                                      {"node_replacement", node_replacement}});
}
