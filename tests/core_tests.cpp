#include "virtual_nodes.h"
#include <limits>
#include <type_traits>

std::array<std::uint64_t, 4> ids_from_other_translation_unit();

namespace
{
using namespace aereborn;
using namespace aereborn::test;
static_assert(Engine<SampleRate::Hz16000, BlockPeriod::Ms05>::block_frames() == 80, "16 kHz / 5 ms");
static_assert(Engine<SampleRate::Hz44100, BlockPeriod::Ms10>::block_frames() == 441, "44.1 kHz / 10 ms");
static_assert(Engine<SampleRate::Hz11025, BlockPeriod::Ms40>::block_frames() == 441, "11.025 kHz / 40 ms");
static_assert(Engine<SampleRate::Hz96000, BlockPeriod::Ms40>::block_frames() == 3840, "96 kHz / 40 ms");
static_assert(std::is_convertible<OBlock, IBlock>::value, "Writable blocks become read-only views");
static_assert(!std::is_convertible<IBlock, OBlock>::value, "Read-only blocks cannot become writable");
static_assert(!std::is_convertible<std::unique_ptr<IAdapter<>>, std::unique_ptr<Node<render_rate, render_period>>>::value,
              "Nodes must match the engine period");

void blocks()
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

void matrices()
{
    const auto matrix = checked(ChMatrix::create(2, 3, {1, 2, 3, 4, 5, 6}));
    check(matrix.output_channels() == 2 && matrix.input_channels() == 3 && matrix.coefficients()[3] == 4,
          "Matrix rows are destinations and columns are sources");
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(0, 1, {}));
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(2, 1, {1}));
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(1, 1, {std::numeric_limits<float>::infinity()}));
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(1, 1, {std::numeric_limits<float>::quiet_NaN()}));
    const auto huge = std::numeric_limits<std::size_t>::max();
    expect_error(ErrorCode::TooLarge, ChMatrix::create(huge, huge, {}));
}

void graph_validation()
{
    RoutingEngine engine;
    expect_error(ErrorCode::InvalidArg, engine.add_node(nullptr));
    expect_error(ErrorCode::InvalidArg, engine.emplace_node<CaptureAdapter>(0));
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
    RoutingEngine other;
    const auto foreign = checked(other.emplace_node<VirtualNode<>>());
    expect_error(ErrorCode::UnknownNode, engine.connect({foreign}, {sink}));
    expect_error(ErrorCode::UnknownNode, engine.connect({source}, {foreign}));
    checked(engine.prepare());
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect_error(ErrorCode::InvalidArg, engine.set_gain(connection, nan));
    expect_error(ErrorCode::InvalidArg, engine.connect({source}, {stereo}, {nan, false, {}}));
    checked(engine.process()); // Rejected graph edits must not invalidate the prepared graph.
    checked(engine.remove_node(source));
    expect_error(ErrorCode::UnknownNode, engine.node(source));
    const auto &read_only = engine;
    expect_error(ErrorCode::UnknownNode, read_only.node(source));
    expect_error(ErrorCode::UnknownConn, engine.disconnect(connection));
    expect_error(ErrorCode::UnknownConn, engine.set_muted(connection, true));
    expect_error(ErrorCode::UnknownConn, engine.set_gain(connection, 1));
    expect_error(ErrorCode::UnknownNode, engine.remove_node(source));
}

void cycles()
{
    RoutingEngine engine;
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
    checked(engine.process());
    check(checked(engine.node<VirtualNode<2, 2>>(a))->calls == 1, "Rejected feedback leaves the valid graph executable");
    const auto x = checked(engine.emplace_node<VirtualNode<>>());
    const auto y = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({x}, {y}));
    expect_error(ErrorCode::Cycle, engine.connect({y}, {x}));
}

void preparation()
{
    RoutingEngine engine;
    expect_error(ErrorCode::NotPrepared, engine.process());
    checked(engine.prepare());
    checked(engine.process());
    check(engine.frame_position() == render_frames, "An empty prepared graph advances one period");
    const auto processor = checked(engine.emplace_node<VirtualNode<>>());
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({processor}, {sink}));
    auto *node = checked(engine.node<VirtualNode<>>(processor));
    node->preparation = ErrorCode::InvalidArg;
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(engine.frame_position() == render_frames && node->calls == 0, "Preparation failure does not process or advance time");
    node->preparation = {};
    checked(engine.prepare());
    checked(engine.prepare());
    checked(engine.process());
    check(node->prepares == 2 && node->calls == 1, "Preparation is retryable and then idempotent");
    const auto huge = checked(engine.emplace_node<PlaybackAdapter>(std::numeric_limits<std::size_t>::max()));
    expect_error(ErrorCode::TooLarge, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    checked(engine.remove_node(huge));
    checked(engine.prepare());
    checked(engine.process());
    check(node->prepares == 2 && node->calls == 2, "Capacity failure preserves previously prepared DSP state");
    engine.reset();
    check(engine.frame_position() == 0 && node->resets == 1, "Reset reaches nodes and restarts the graph clock");
}

class ThrowingNode : public VirtualNode<>
{
  public:
    explicit ThrowingNode(ErrorCode construction)
    {
        if (construction == ErrorCode::NoMemory)
        {
            throw std::bad_alloc();
        }
        if (construction == ErrorCode::TooLarge)
        {
            throw std::length_error("test capacity");
        }
        if (construction == ErrorCode::NodeFailed)
        {
            throw std::runtime_error("test construction");
        }
    }
    RetCode prepare() override
    {
        throw std::runtime_error("test preparation");
    }
};

void exceptions()
{
    RoutingEngine engine;
    expect_error(ErrorCode::NoMemory, engine.emplace_node<ThrowingNode>(ErrorCode::NoMemory));
    expect_error(ErrorCode::TooLarge, engine.emplace_node<ThrowingNode>(ErrorCode::TooLarge));
    expect_error(ErrorCode::NodeFailed, engine.emplace_node<ThrowingNode>(ErrorCode::NodeFailed));
    const auto bad = checked(engine.emplace_node<ThrowingNode>(ErrorCode::Ok));
    const auto sink = checked(engine.emplace_node<VirtualNode<1, 0>>());
    checked(engine.connect({bad}, {sink}));
    expect_error(ErrorCode::NodeFailed, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    checked(engine.remove_node(bad));
    checked(engine.prepare());
    checked(engine.process());
}

void identifiers()
{
    const auto remote = ids_from_other_translation_unit();
    RoutingEngine engine;
    const auto a = checked(engine.emplace_node<VirtualNode<>>());
    const auto b = checked(engine.emplace_node<VirtualNode<>>());
    const auto connection = checked(engine.connect({a}, {b}));
    for (std::size_t i = 0; i < remote.size(); ++i)
    {
        for (std::size_t j = i + 1; j < remote.size(); ++j)
        {
            check(remote[i] != remote[j], "Identifiers are shared across engine specializations");
        }
        for (const auto local : {a.value, b.value, connection.value})
        {
            check(remote[i] != local, "Identifiers are shared across translation units");
        }
    }
    checked(engine.remove_node(a));
    const auto replacement = checked(engine.emplace_node<VirtualNode<>>());
    check(replacement != a, "Removed identifiers are not reused");
}

void topology()
{
    RoutingEngine engine;
    Capture input(engine, 1, 997, 0.25f);
    Playback main(engine, 1);
    const auto processor = checked(engine.emplace_node<VirtualNode<>>());
    checked(engine.connect({input.id}, {processor}));
    checked(engine.connect({processor}, {main.id}));
    checked(engine.prepare());
    input.generate();
    main.bind();
    checked(engine.process());
    input.generate();
    main.bind();
    const auto previous = main.bytes;
    Playback monitor(engine, 1);
    checked(engine.connect({processor}, {monitor.id}));
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(main.bytes == previous && engine.frame_position() == render_frames, "Graph edits do not consume pending PCM");
    expect_error(ErrorCode::BufferFull, input.adapter->bind_pcm(input.device.pcm()));
    checked(engine.prepare());
    monitor.bind();
    checked(engine.process());
    main.receive();
    monitor.receive();
    main.expect([](std::size_t frame, std::size_t) { return tone(997, 0.25, render_frames + frame); });
    check(monitor.device.matches(main.device.pcm()), "A newly attached monitor receives the same existing output");
    auto *node = checked(engine.node<VirtualNode<>>(processor));
    check(node->calls == 2 && node->prepares == 1 && node->resets == 0, "Rebuilding keeps existing node state");
    checked(engine.remove_node(processor));
    checked(engine.prepare());
    main.bind();
    monitor.bind();
    checked(engine.process());
    main.receive();
    monitor.receive();
    main.expect([](std::size_t, std::size_t) { return 0; });
    monitor.expect([](std::size_t, std::size_t) { return 0; });
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"blocks", blocks}, {"matrices", matrices}, {"graph_validation", graph_validation}, {"cycles", cycles},
        {"preparation", preparation}, {"exceptions", exceptions}, {"identifiers", identifiers}, {"topology", topology}
    });
}
