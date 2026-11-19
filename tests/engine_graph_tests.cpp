#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/test_node.h"
#include "core/engine.h"
#include "core/observer.h"
#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
static_assert(EngineSpec(SampleRate::Hz16000, BlockPeriod::Ms05).block_frames() == 80, "16 kHz / 5 ms");
static_assert(EngineSpec(SampleRate::Hz44100, BlockPeriod::Ms10).block_frames() == 441, "44.1 kHz / 10 ms");
static_assert(EngineSpec(SampleRate::Hz11025, BlockPeriod::Ms40).block_frames() == 441, "11.025 kHz / 40 ms");
static_assert(EngineSpec(SampleRate::Hz96000, BlockPeriod::Ms40).block_frames() == 3840, "96 kHz / 40 ms");
static_assert(std::is_convertible<OBlock, IBlock>::value, "Writable blocks become read-only views");
static_assert(!std::is_convertible<IBlock, OBlock>::value, "Read-only blocks cannot become writable");

template <class Exception, class Operation> void expect_exception(Operation operation)
{
    try
    {
        operation();
    }
    catch (const Exception &)
    {
        return;
    }
    check(false, "Expected exception was not thrown");
}

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
            check(read.channel(ch)[frame] == (frame < 4 ? 0 : ch + 1), "Planar stride and processing span are independent");
        }
    }
    expect_error(ErrorCode::InvalidArg, buffer.resize(0, 8));
    expect_error(ErrorCode::InvalidArg, buffer.resize(1, 0));
    expect_error(ErrorCode::TooLarge, buffer.resize(std::numeric_limits<std::size_t>::max(), 8));
    check(buffer.capacity_frames() == 8 && buffer.block(8).channel(1)[7] == 2,
          "Rejected resize preserves storage and layout");
    const View<const float> empty(nullptr, 0);
    check(empty.begin() == empty.end(), "An empty view supports iteration without pointer arithmetic on null");
}

void channel_matrix_validation()
{
    const float coefficients[] = {1, 2, 3, 4, 5, 6};
    const ChMatrix matrix(2, 3, View<const float>{coefficients, 6});
    check(matrix.output_channels() == 2 && matrix.input_channels() == 3 && matrix.coefficients()[3] == 4,
          "Matrix rows are destinations and columns are sources");
    expect_exception<std::invalid_argument>([] { ChMatrix(0, 1, {}); });
    expect_exception<std::invalid_argument>([] { ChMatrix(2, 1, {1}); });
    expect_exception<std::invalid_argument>([] { ChMatrix(1, 1, {std::numeric_limits<float>::infinity()}); });
    expect_exception<std::invalid_argument>([] { ChMatrix(1, 1, {std::numeric_limits<float>::quiet_NaN()}); });
    expect_exception<std::length_error>([] {
        const auto huge = std::numeric_limits<std::size_t>::max();
        ChMatrix(huge, huge, {});
    });
}

void gain_ramp_endpoints()
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    const ChMatrix identity;
    const float crossing[] = {1, nan, 1, 1};
    float mixed[4]{};
    identity.mix_ramp_add({crossing, 1, 4, 4}, {mixed, 1, 4, 4}, 1.0f, -1.0f);
    check(mixed[0] == 0.5f && mixed[1] == 0 && mixed[2] == -0.5f && mixed[3] == -1.0f,
          "An exactly zero gain inside the ramp skips the source sample");

    const float fade[] = {1, infinity};
    float faded[] = {0, 2};
    const ChMatrix half(1, 1, {0.5f});
    half.mix_ramp_add({fade, 1, 2, 2}, {faded, 1, 2, 2}, 1.0f, 0.0f);
    check(faded[0] == 0.25f && faded[1] == 2, "A silent last frame leaves the existing mix intact");

    const float single = 1.0f;
    float final = 0.0f;
    identity.mix_ramp_add({&single, 1, 1, 1}, {&final, 1, 1, 1}, std::numeric_limits<float>::max(), 1.0f);
    check(final == 1.0f, "A single-frame ramp uses the exact target gain");
}

void engine_timing_validation()
{
    const EngineSpec invalid_specs[] = {
        {SampleRate::Hz44100, BlockPeriod::Ms05},
        {static_cast<SampleRate>(0), BlockPeriod::Ms10},
        {static_cast<SampleRate>(96100), BlockPeriod::Ms10},
        {SampleRate::Hz48000, static_cast<BlockPeriod>(0)},
        {SampleRate::Hz48000, static_cast<BlockPeriod>(15)},
    };
    for (const auto spec : invalid_specs)
    {
        Engine engine(spec.sample_rate(), spec.block_period());
        ManualClock clock(engine);
        check(engine.spec().block_frames() == 0, "Invalid graph timing cannot define a block size");
        const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
        checked(engine.add_observer(std::make_unique<Observer>(OPort{source}, [](const Observation &) {})));
        expect_error(ErrorCode::InvalidArg, engine.prepare());
        expect_error(ErrorCode::NotPrepared, clock.tick());
    }

    Engine custom_rate(static_cast<SampleRate>(12300), BlockPeriod::Ms10);
    checked(custom_rate.prepare());
    ManualClock custom_clock(custom_rate);
    checked(custom_clock.tick());
    check(custom_rate.frame_position() == 123, "Graph rates within the range can define integral frames");
}

void connection_validation()
{
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    expect_error(ErrorCode::InvalidArg, engine.add_node(nullptr));
    expect_error(ErrorCode::InvalidArg, engine.emplace_node<VirtualNode<0, 1>>(0));
    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    const auto stereo = checked(engine.emplace_node<VirtualNode<1, 0>>(2));
    const auto connection = checked(engine.connect({source}, {sink}));
    expect_error(ErrorCode::DuplicateConn, engine.connect({source}, {sink}));
    expect_error(ErrorCode::InvalidPort, engine.connect({sink}, {source}));
    expect_error(ErrorCode::InvalidPort, engine.connect({source, 1}, {sink}));
    expect_error(ErrorCode::InvalidPort, engine.connect({source}, {sink, 1}));
    expect_error(ErrorCode::ChannelMismatch, engine.connect({source}, {stereo}));
    expect_error(ErrorCode::InvalidType, engine.node<VirtualNode<>>(source));
    Engine other(render_rate, render_period);
    const auto foreign = checked(other.emplace_node<VirtualNode<>>());
    expect_error(ErrorCode::UnknownNode, engine.connect({foreign}, {sink}));
    expect_error(ErrorCode::UnknownNode, engine.connect({source}, {foreign}));
    checked(engine.prepare());
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect_error(ErrorCode::InvalidArg, engine.set_gain(connection, nan));
    expect_error(ErrorCode::InvalidArg, engine.connect({source}, {stereo}, {nan, false, {}}));
    checked(clock.tick()); // Rejected graph edits must not invalidate the prepared graph.
    checked(engine.remove_node(source));
    expect_error(ErrorCode::UnknownNode, engine.node(source));
    const auto &read_only = engine;
    expect_error(ErrorCode::UnknownNode, read_only.node(source));
    expect_error(ErrorCode::UnknownConn, engine.disconnect(connection));
    expect_error(ErrorCode::UnknownConn, engine.set_muted(connection, true));
    expect_error(ErrorCode::UnknownConn, engine.set_gain(connection, 1));
    expect_error(ErrorCode::UnknownNode, engine.remove_node(source));
}

void feedback_rejection()
{
    Engine engine(render_rate, render_period);
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
    checked(clock.tick());
    check(checked(engine.node<VirtualNode<2, 2>>(a))->calls == 1, "Rejected feedback leaves the valid graph executable");
    const auto x = checked(engine.emplace_node<VirtualNode<>>());
    const auto y = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({x}, {y}));
    expect_error(ErrorCode::Cycle, engine.connect({y}, {x}));
}

void prepare_retry()
{
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    expect_error(ErrorCode::NotPrepared, clock.tick());
    checked(engine.prepare());
    checked(clock.tick());
    check(engine.frame_position() == render_frames, "An empty prepared graph advances one period");
    const auto processor = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({processor}, {sink}));
    auto *node = checked(engine.node<VirtualNode<>>(processor));
    node->preparation = ErrorCode::InvalidArg;
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    checked(clock.tick());
    check(engine.frame_position() == 2 * render_frames && node->calls == 0,
          "Preparation failure leaves the old plan running");
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
    check(engine.frame_position() == 0 && node->resets == 1, "Reset reaches nodes and restarts the graph clock");
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
    RetCode prepare(const EngineSpec &spec) override
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
    Engine engine(render_rate, render_period);
    ManualClock clock(engine);
    checked(engine.prepare());
    expect_exception<std::bad_alloc>([&] {
        engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::bad_alloc()));
    });
    expect_exception<std::length_error>([&] {
        engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::length_error("test capacity")));
    });
    expect_exception<std::runtime_error>([&] {
        engine.emplace_node<ThrowingNode>(std::make_exception_ptr(std::runtime_error("test construction")));
    });
    checked(clock.tick());

    const auto source = checked(engine.emplace_node<VirtualNode<0, 1>>());
    const auto processor = checked(engine.emplace_node<ThrowingNode>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({source}, {processor}));
    checked(engine.connect({processor}, {sink}));
    expect_exception<std::runtime_error>([&] { engine.prepare(); });
    checked(clock.tick());
    check(engine.frame_position() == 2 * render_frames, "The previous plan runs after a preparation exception");
    auto *node = checked(engine.node<ThrowingNode>(processor));
    node->fail_preparation = false;
    checked(engine.prepare());
    checked(clock.tick());
    check(checked(engine.node<VirtualNode<0, 1>>(source))->prepares == 1 && node->calls == 1,
          "Retry after an exception preserves prepared dependencies");
}

void handle_uniqueness()
{
    Engine engine(render_rate, render_period);
    const auto a = checked(engine.emplace_node<VirtualNode<>>());
    const auto b = checked(engine.emplace_node<VirtualNode<>>());
    const auto connection = checked(engine.connect({a}, {b}));

    Engine other(SampleRate::Hz16000, BlockPeriod::Ms10);
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
    return aereborn::test::run_tests(argc, argv, {
        {"audio_block_layout", audio_block_layout}, {"channel_matrix_validation", channel_matrix_validation}, {"engine_timing_validation", engine_timing_validation},
        {"gain_ramp_endpoints", gain_ramp_endpoints},
        {"connection_validation", connection_validation}, {"feedback_rejection", feedback_rejection},
        {"prepare_retry", prepare_retry}, {"node_exception_propagation", node_exception_propagation}, {"handle_uniqueness", handle_uniqueness}
    });
}
