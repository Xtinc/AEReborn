#include "core/engine.h"
#include "plugins/nodes.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <type_traits>

using namespace aereborn;

static_assert(std::is_convertible<OBlock, IBlock>::value, "Outputs must support read-only views");
static_assert(!std::is_convertible<IBlock, OBlock>::value, "Inputs must remain read-only");
static_assert(std::is_same<decltype(std::declval<IBlock>().channel(0)), const float *>::value,
              "Input samples must remain const");
static_assert(std::is_convertible<View<IBlock>, View<const IBlock>>::value, "Views must preserve const conversion");
static_assert(!std::is_convertible<View<const IBlock>, View<IBlock>>::value, "Views must not discard const");

namespace
{
constexpr auto test_rate = SampleRate::Hz16000;
constexpr auto test_period = BlockPeriod::Ms05;
using TestEngine = Engine<test_rate, test_period>;
constexpr auto test_frames = TestEngine::block_frames();
using Source = BufferSourceNode<test_rate, test_period>;
using Sink = BufferSinkNode<test_rate, test_period>;
using Gain = GainNode<test_rate, test_period>;
using Mixer = MixerNode<test_rate, test_period>;
using Sine = SineNode<test_rate, test_period>;

void check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void near(float actual, float expected)
{
    check(std::isfinite(actual) && std::abs(actual - expected) < 0.00001f, "Unexpected sample value");
}

template <class F> void expect_error(ErrorCode expected, F action)
{
    try
    {
        action();
    }
    catch (const GraphError &error)
    {
        check(error.code() == expected, "Unexpected graph error code");
        return;
    }
    throw std::runtime_error("Expected a graph error");
}

void render(TestEngine &engine)
{
    check(engine.process(), "Rendering failed");
}

IBlock captured(TestEngine &engine, NodeId sink)
{
    return engine.node<Sink>(sink).block();
}

// Nonuniform port widths catch assumptions about port indices and sample offsets.
class TwoPortNode final : public NodeT<2, 2, test_rate, test_period>
{
  public:
    TwoPortNode() : NodeT({{ISpec{1}, ISpec{2}}}, {{OSpec{2}, OSpec{1}}})
    {
    }
    void attributes(std::ostream &stream) const override
    {
        stream << "TwoPortNode {}";
    }
    void process(const ProcContext &context) noexcept override
    {
        for (std::size_t i = 0; i < test_frames; ++i)
        {
            context.outputs[0].channel(0)[i] = context.inputs[0].channel(0)[i] + context.inputs[1].channel(0)[i];
            context.outputs[0].channel(1)[i] = context.inputs[1].channel(1)[i];
            context.outputs[1].channel(0)[i] = -context.inputs[0].channel(0)[i];
        }
    }
};

class CountingNode final : public NodeT<0, 1, test_rate, test_period>
{
  public:
    CountingNode() : NodeT({}, {{1}})
    {
    }
    void attributes(std::ostream &stream) const override
    {
        stream << "CountingNode { preparations=" << preparations << ", calls=" << calls << " }";
    }
    void prepare() override
    {
        ++preparations;
        calls = 0;
    }
    void process(const ProcContext &context) noexcept override
    {
        ++calls;
        std::fill_n(context.outputs[0].channel(0), test_frames, static_cast<float>(calls));
    }
    void reset() noexcept override
    {
        calls = 0;
    }
    int preparations = 0;
    int calls = 0;
};

class RetrySink final : public NodeT<1, 0, test_rate, test_period>
{
  public:
    RetrySink() : NodeT({{1}}, {})
    {
    }
    void attributes(std::ostream &stream) const override
    {
        stream << "RetrySink { attempts=" << attempts << " }";
    }
    void prepare() override
    {
        if (++attempts == 1)
        {
            throw std::runtime_error("Simulated preparation failure");
        }
    }
    void process(const ProcContext &) noexcept override
    {
    }
    int attempts = 0;
};

void block_views()
{
    Buffer buffer(2, 5);
    auto full = buffer.block(5);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        std::fill_n(full.channel(ch), 5, static_cast<float>(ch + 1));
    }
    buffer.block(2).clear();
    const Buffer &readonly = buffer;
    const auto input = readonly.block(5);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        near(input.channel(ch)[0], 0);
        near(input.channel(ch)[1], 0);
        near(input.channel(ch)[2], static_cast<float>(ch + 1));
        near(input.channel(ch)[4], static_cast<float>(ch + 1));
    }
    IBlock readonly_view = full;
    readonly_view.set_frames(2);
    check(readonly_view.frames() == 2 && full.frames() == 5, "Copied block lengths must be independent");
    near(readonly_view.channel(1)[1], 0);
}

void node_diagnostics()
{
    TestEngine engine;
    const auto source = engine.emplace_node<Source>(2);
    const auto gain = engine.emplace_node<Gain>(2, 1.0f);
    const auto sink = engine.emplace_node<Sink>(2);
    const auto mixer = engine.emplace_node<Mixer>(1);
    const auto sine = engine.emplace_node<Sine>(2, 250, 0.25f);
    engine.connect(engine.output(source), engine.input(gain));
    engine.connect(engine.output(gain), engine.input(sink));
    engine.prepare();
    const float samples[] = {1, 10, 2, 20, 3, 30, 4, 40};
    engine.node<Source>(source).push_interleaved(samples, 4);
    render(engine);
    engine.node<Source>(source).push_interleaved(samples + 4, 2);
    engine.node<Gain>(gain).set_gain(-0.5f);

    // Printing through const Node references must include subclass properties and all port specifications.
    const TestEngine &graph = engine;
    std::ostringstream stream;
    stream << graph.node(source) << '\n'
           << graph.node(gain) << '\n'
           << graph.node(sink) << '\n'
           << graph.node(mixer) << '\n'
           << graph.node(sine) << '\n'
           << TwoPortNode{};
    const auto text = stream.str();
    check(text.find("BufferSourceNode { capacity_frames=80, pending_frames=2 }") != std::string::npos,
          "Source diagnostics must reflect the pending audio");
    check(text.find("GainNode { gain=-0.5 }") != std::string::npos, "Diagnostics must reflect parameter updates");
    check(text.find("BufferSinkNode { frame_position=0, frames=80 }") != std::string::npos,
          "Sink diagnostics must reflect the last processed block");
    check(text.find("MixerNode {}\n  IPort[0] channels=1 mode=Mix") != std::string::npos,
          "Mixer diagnostics must include its input mode");
    check(text.find("SineNode { frequency=250, amplitude=0.25 }") != std::string::npos,
          "Generator diagnostics must include its parameters");
    check(text.find("TwoPortNode {}\n  IPort[0] channels=1 mode=Single\n  IPort[1] channels=2 mode=Single"
                    "\n  OPort[0] channels=2\n  OPort[1] channels=1") != std::string::npos,
          "Diagnostics must preserve port directions, indices and channel counts");

    check(engine.frame_position() == test_frames && engine.node<Source>(source).pending_frames() == 2,
          "Diagnostics must not advance processing");
    render(engine);
    near(captured(engine, sink).channel(0)[0], -1.5f);
    near(captured(engine, sink).channel(1)[1], -20.0f);
}

void mixing_and_fanout()
{
    TestEngine engine;
    const auto a = engine.emplace_node<Source>(1);
    const auto b = engine.emplace_node<Source>(1);
    const auto mix = engine.emplace_node<Mixer>(1);
    const auto main = engine.emplace_node<Sink>(1);
    const auto gain = engine.emplace_node<Gain>(1, 0.25f);
    const auto monitor = engine.emplace_node<Sink>(1);
    engine.connect(engine.output(a), engine.input(mix));
    const auto connection = engine.connect(engine.output(b), engine.input(mix), {0.5f, false, {}});
    engine.connect(engine.output(mix), engine.input(main));
    engine.connect(engine.output(mix), engine.input(gain));
    engine.connect(engine.output(gain), engine.input(monitor));
    engine.prepare();
    const float first[] = {1, 2, 3, 4};
    const float second[] = {2, 2, 2, 2};
    auto feed = [&] {
        engine.node<Source>(a).push_interleaved(first, 4);
        engine.node<Source>(b).push_interleaved(second, 4);
    };
    feed();
    render(engine);
    for (std::size_t i = 0; i < 4; ++i)
    {
        near(captured(engine, main).channel(0)[i], first[i] + 1);
        near(captured(engine, monitor).channel(0)[i], (first[i] + 1) * 0.25f);
    }
    engine.set_muted(connection, true);
    feed();
    render(engine);
    near(captured(engine, main).channel(0)[0], 1);
    engine.set_muted(connection, false);
    engine.set_gain(connection, -1);
    engine.node<Gain>(gain).set_gain(2);
    feed();
    render(engine);
    near(captured(engine, main).channel(0)[0], -1);
    near(captured(engine, monitor).channel(0)[0], -2);
    check(engine.frame_position() == 3 * test_frames, "Unexpected timeline");
    check(engine.node<Sink>(main).frame_position() == 2 * test_frames, "Sink timestamp must mark the block start");
}

void multiple_ports_and_matrices()
{
    TestEngine engine;
    const auto mono = engine.emplace_node<Source>(1);
    const auto stereo = engine.emplace_node<Source>(2);
    const auto processor = engine.emplace_node<TwoPortNode>();
    const auto stereo_sink = engine.emplace_node<Sink>(2);
    const auto mono_sink = engine.emplace_node<Sink>(1);
    const auto mono_connection = engine.connect(engine.output(mono), engine.input(processor, 0));
    engine.connect(engine.output(stereo), engine.input(processor, 1), {1, false, ChMatrix(2, 2, {0, 1, 1, 0})});
    engine.connect(engine.output(processor, 0), engine.input(stereo_sink));
    engine.connect(engine.output(processor, 1), engine.input(mono_sink));
    engine.prepare();
    const float a[] = {1, 2, 3, 4};
    const float b[] = {10, 20, 11, 21, 12, 22, 13, 23};
    engine.node<Source>(mono).push({a, 1, 4, 4});
    engine.node<Source>(stereo).push_interleaved(b, 4);
    render(engine);
    near(captured(engine, stereo_sink).channel(0)[1], 23);
    near(captured(engine, stereo_sink).channel(1)[1], 11);
    near(captured(engine, mono_sink).channel(0)[1], -2);
    near(captured(engine, stereo_sink).channel(0)[2], 25);
    near(captured(engine, stereo_sink).channel(1)[2], 12);
    near(captured(engine, stereo_sink).channel(0)[4], 0);
    render(engine);
    for (std::size_t i = 0; i < test_frames; ++i)
    {
        near(captured(engine, stereo_sink).channel(0)[i], 0);
        near(captured(engine, stereo_sink).channel(1)[i], 0);
        near(captured(engine, mono_sink).channel(0)[i], 0);
    }
    engine.disconnect(mono_connection);
    engine.prepare();
    render(engine);
    near(captured(engine, mono_sink).channel(0)[0], 0);
    near(captured(engine, stereo_sink).channel(0)[0], 0);
}

void source_staging()
{
    TestEngine engine;
    const auto source = engine.emplace_node<Source>(1);
    const auto sink = engine.emplace_node<Sink>(1);
    engine.connect(engine.output(source), engine.input(sink));
    auto &input = engine.node<Source>(source);
    std::array<float, test_frames> samples;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<float>(i + 1);
    }
    expect_error(ErrorCode::NotPrepared, [&] { input.push_interleaved(samples.data(), samples.size()); });
    check(!engine.process(), "New graphs require preparation");
    check(engine.frame_position() == 0, "Rejected processing must not advance the timeline");
    engine.prepare();
    expect_error(ErrorCode::InvalidArgument, [&] { input.push_interleaved(samples.data(), test_frames + 1); });
    expect_error(ErrorCode::InvalidArgument, [&] { input.push_interleaved(nullptr, 1); });
    input.push_interleaved(samples.data(), samples.size());
    input.push_interleaved(nullptr, 0);
    expect_error(ErrorCode::BufferFull, [&] { input.push_interleaved(samples.data(), 1); });
    engine.emplace_node<Mixer>(1);
    check(!engine.process(), "Edited graphs require preparation");
    check(engine.frame_position() == 0 && input.pending_frames() == test_frames,
          "Rejected processing must not consume pending audio");
    engine.prepare();
    render(engine);
    near(captured(engine, sink).channel(0)[0], 1);
    near(captured(engine, sink).channel(0)[test_frames - 1], static_cast<float>(test_frames));
    check(input.pending_frames() == 0 && engine.frame_position() == test_frames,
          "Processing must consume one complete period");

    input.push_interleaved(samples.data(), 2);
    render(engine);
    near(captured(engine, sink).channel(0)[0], 1);
    near(captured(engine, sink).channel(0)[1], 2);
    for (std::size_t i = 2; i < test_frames; ++i)
    {
        near(captured(engine, sink).channel(0)[i], 0);
    }
    check(input.pending_frames() == 0 && engine.frame_position() == 2 * test_frames,
          "Short input must still advance a full period");
    render(engine);
    for (std::size_t i = 0; i < test_frames; ++i)
    {
        near(captured(engine, sink).channel(0)[i], 0);
    }
    input.push_interleaved(samples.data(), samples.size());
    engine.reset();
    check(input.pending_frames() == 0 && engine.frame_position() == 0, "Reset must clear staging and timeline");
    check(captured(engine, sink).frames() == 0, "Reset must release the captured view");
    render(engine);
    near(captured(engine, sink).channel(0)[0], 0);
}

void channel_count_conversion()
{
    TestEngine engine;
    const auto source = engine.emplace_node<Source>(2);
    const auto downmix = engine.emplace_node<Mixer>(1);
    const auto sink = engine.emplace_node<Sink>(2);
    const auto connection =
        engine.connect(engine.output(source), engine.input(downmix), {2, false, ChMatrix(1, 2, {0.25f, 0.75f})});
    engine.connect(engine.output(downmix), engine.input(sink), {1, false, ChMatrix(2, 1, {1, -1})});
    engine.prepare();
    const float samples[] = {2, 6, 4, 8};
    engine.node<Source>(source).push_interleaved(samples, 2);
    render(engine);
    near(captured(engine, sink).channel(0)[0], 10);
    near(captured(engine, sink).channel(1)[1], -14);
    engine.set_muted(connection, true);
    engine.node<Source>(source).push_interleaved(samples, 2);
    render(engine);
    near(captured(engine, sink).channel(0)[0], 0);
    near(captured(engine, sink).channel(1)[1], 0);
}

void topology_preserves_state()
{
    TestEngine engine;
    const auto source = engine.emplace_node<CountingNode>();
    const auto a = engine.emplace_node<Sink>(1);
    engine.connect(engine.output(source), engine.input(a));
    engine.prepare();
    engine.prepare();
    auto &counter = engine.node<CountingNode>(source);
    check(counter.preparations == 1, "Preparation must be idempotent");
    render(engine);
    const auto b = engine.emplace_node<Sink>(1);
    engine.connect(engine.output(source), engine.input(b));
    check(!engine.process(), "Topology edits must invalidate the plan");
    engine.prepare();
    render(engine);
    check(counter.preparations == 1 && counter.calls == 2, "Fanout must execute the existing source once");
    near(captured(engine, a).channel(0)[0], 2);
    near(captured(engine, b).channel(0)[0], 2);
    engine.remove_node(a);
    engine.prepare();
    render(engine);
    near(captured(engine, b).channel(0)[0], 3);
    check(engine.connection_count() == 1, "Removing a node must remove incident connections");
    engine.remove_node(b);
    engine.prepare();
    render(engine);
    check(counter.calls == 3 && engine.frame_position() == 4 * test_frames,
          "Nodes without a downstream sink must remain idle");
}

void graph_validation()
{
    TestEngine engine;
    const auto a = engine.emplace_node<Mixer>(1);
    const auto b = engine.emplace_node<Mixer>(1);
    const auto sink = engine.emplace_node<Sink>(1);
    engine.connect(engine.output(a), engine.input(b));
    const auto connection = engine.connect(engine.output(b), engine.input(sink));
    engine.prepare();
    expect_error(ErrorCode::Cycle, [&] { engine.connect(engine.output(b), engine.input(a)); });
    expect_error(ErrorCode::Cycle, [&] { engine.connect(engine.output(a), engine.input(a)); });
    expect_error(ErrorCode::DuplicateConnection, [&] { engine.connect(engine.output(a), engine.input(b)); });
    expect_error(ErrorCode::InputAlreadyConnected, [&] { engine.connect(engine.output(a), engine.input(sink)); });
    expect_error(ErrorCode::InvalidPort, [&] { engine.output(sink); });
    expect_error(ErrorCode::InvalidNodeType, [&] { engine.node<Sine>(a); });
    render(engine);
    engine.remove_node(b);
    expect_error(ErrorCode::UnknownNode, [&] { engine.node(b); });
    expect_error(ErrorCode::UnknownConnection, [&] { engine.set_muted(connection, true); });
    TestEngine other;
    const auto foreign = other.emplace_node<Mixer>(1);
    expect_error(ErrorCode::UnknownNode, [&] { engine.input(foreign); });
    const auto stereo = engine.emplace_node<Source>(2);
    expect_error(ErrorCode::ChannelMismatch, [&] { engine.connect(engine.output(stereo), engine.input(a)); });
    expect_error(ErrorCode::ChannelMismatch,
                 [&] { engine.connect(engine.output(stereo), engine.input(a), {1, false, ChMatrix::identity(2)}); });
}

void prepare_failure_and_retry()
{
    TestEngine engine;
    const auto source = engine.emplace_node<CountingNode>();
    const auto sink = engine.emplace_node<RetrySink>();
    engine.connect(engine.output(source), engine.input(sink));
    bool failed = false;
    try
    {
        engine.prepare();
    }
    catch (const std::runtime_error &)
    {
        failed = true;
    }
    check(failed && !engine.process(), "Failed preparation must not enable rendering");
    check(engine.node<CountingNode>(source).calls == 0 && engine.frame_position() == 0,
          "Failed preparation must not execute nodes or advance time");
    engine.prepare();
    render(engine);
    check(engine.node<RetrySink>(sink).attempts == 2, "Failed nodes must be prepared again");
    check(engine.node<CountingNode>(source).preparations == 1, "Prepared nodes must retain their state after retry");

    // A failed rebuild must also leave an existing plan unusable until retry succeeds.
    const auto added_sink = engine.emplace_node<RetrySink>();
    engine.connect(engine.output(source), engine.input(added_sink));
    failed = false;
    try
    {
        engine.prepare();
    }
    catch (const std::runtime_error &)
    {
        failed = true;
    }
    check(failed && !engine.process(), "Failed rebuild must not use the old plan");
    check(engine.node<CountingNode>(source).calls == 1 && engine.frame_position() == test_frames,
          "Failed rebuild must preserve the last processed state");
    engine.prepare();
    render(engine);
    check(engine.node<CountingNode>(source).calls == 2, "Retry must preserve state and execute shared nodes once");
    check(engine.node<CountingNode>(source).preparations == 1, "Rebuilding must not prepare existing nodes again");
}

void sine_and_specs()
{
    static_assert(Engine<SampleRate::Hz48000, BlockPeriod::Ms10>::block_frames() == 480, "Default period must be 10 ms at 48 kHz");
    static_assert(TestEngine::block_frames() == 80, "5 ms at 16 kHz must be 80 frames");
    static_assert(Engine<SampleRate::Hz44100, BlockPeriod::Ms10>::block_frames() == 441, "Frame counts must be exact");
    static_assert(Engine<SampleRate::Hz11025, BlockPeriod::Ms40>::block_frames() == 441,
                  "11.025 kHz permits a 40 ms period");
    static_assert(Engine<SampleRate::Hz88200, BlockPeriod::Ms05>::block_frames() == 441,
                  "88.2 kHz permits a 5 ms period");
    static_assert(Engine<SampleRate::Hz96000, BlockPeriod::Ms40>::block_frames() == 3840, "96 kHz is supported");
    static_assert(
        !std::is_convertible<std::unique_ptr<GainNode<>>, std::unique_ptr<Node<test_rate, test_period>>>::value,
        "Nodes with different processing specifications cannot be added");
    Engine<SampleRate::Hz48000, BlockPeriod::Ms10> empty;
    check(!empty.process(), "Empty graphs still require preparation");
    empty.prepare();
    check(empty.process() && empty.frame_position() == 480, "Prepared empty graphs advance a full period");
    expect_error(ErrorCode::InvalidArgument, [] { Mixer invalid(0); });
    expect_error(ErrorCode::InvalidArgument, [] { ChMatrix invalid(2, 1, {1}); });
    expect_error(ErrorCode::InvalidArgument, [] { ChMatrix invalid(1, 1, {std::numeric_limits<float>::quiet_NaN()}); });
    TestEngine engine;
    const auto source = engine.emplace_node<Sine>(2, 4000, 0.5f);
    const auto sink = engine.emplace_node<Sink>(2);
    engine.connect(engine.output(source), engine.input(sink));
    engine.prepare();
    render(engine);
    const float expected[] = {0, 0.5f, 0, -0.5f};
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (std::size_t i = 0; i < test_frames; ++i)
        {
            near(captured(engine, sink).channel(ch)[i], expected[i % 4]);
        }
    }
    engine.reset();
    render(engine);
    near(captured(engine, sink).channel(0)[0], 0);
    near(captured(engine, sink).channel(0)[1], 0.5f);
}
} // namespace

int main()
{
    const struct
    {
        const char *name;
        void (*run)();
    } tests[] = {{"block views", block_views},
                 {"node diagnostics", node_diagnostics},
                 {"mixing and fanout", mixing_and_fanout},
                 {"multiple ports and matrices", multiple_ports_and_matrices},
                 {"channel count conversion", channel_count_conversion},
                 {"source staging", source_staging},
                 {"topology preserves state", topology_preserves_state},
                 {"graph validation", graph_validation},
                 {"prepare failure and retry", prepare_failure_and_retry},
                 {"sine and specifications", sine_and_specs}};
    for (const auto &test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
}
