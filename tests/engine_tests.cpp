#include "core/engine.h"
#include "checked.h"
#include "nodes.h"
#include "plugins/virtual.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace aereborn;
using namespace aereborn::test;

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
using Sine = VirtualSource<test_rate, test_period>;

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

void expect_error(ErrorCode expected, RetCode status)
{
    check(status.code() == expected, status.message());
}

template <class T> void expect_error(ErrorCode expected, const Result<T> &result)
{
    expect_error(expected, result.ret_code());
}

void render(TestEngine &engine)
{
    checked(engine.process());
}

IBlock captured(TestEngine &engine, NodeId sink)
{
    return checked(engine.node<Sink>(sink))->block();
}

// Nonuniform port widths catch assumptions about port indices and sample offsets.
class TwoPortNode final : public NodeT<2, 2, test_rate, test_period>
{
  public:
    TwoPortNode() : NodeT({{1, 2}}, {{2, 1}})
    {
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
    RetCode prepare() noexcept override
    {
        ++preparations;
        calls = 0;
        return {};
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
    explicit RetrySink(bool throw_on_failure = false) : NodeT({{1}}, {}), throw_on_failure_(throw_on_failure)
    {
    }
    RetCode prepare() override
    {
        if (++attempts == 1)
        {
            if (throw_on_failure_)
            {
                throw std::runtime_error("Simulated exception in a custom node");
            }
            return ErrorCode::NoMemory;
        }
        return {};
    }
    void process(const ProcContext &) noexcept override
    {
    }
    int attempts = 0;

  private:
    bool throw_on_failure_;
};

void block_views()
{
    Buffer buffer;
    checked(buffer.resize(2, 5));
    auto full = buffer.block(5);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        std::fill_n(full.channel(ch), 5, static_cast<float>(ch + 1));
    }
    buffer.block(2).clear();
    const IBlock input = full;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        near(input.channel(ch)[0], 0);
        near(input.channel(ch)[1], 0);
        near(input.channel(ch)[2], static_cast<float>(ch + 1));
        near(input.channel(ch)[4], static_cast<float>(ch + 1));
    }
    const IBlock short_input = buffer.block(2);
    check(short_input.frames() == 2 && full.frames() == 5, "Block views can cover independent spans");
    near(short_input.channel(1)[1], 0);
}

void mixing_and_fanout()
{
    TestEngine engine;
    const auto a = checked(engine.emplace_node<Source>(1));
    const auto b = checked(engine.emplace_node<Source>(1));
    const auto mix = checked(engine.emplace_node<Gain>(1, 1.0f));
    const auto main = checked(engine.emplace_node<Sink>(1));
    const auto gain = checked(engine.emplace_node<Gain>(1, 0.25f));
    const auto monitor = checked(engine.emplace_node<Sink>(1));
    checked(engine.connect({a}, {mix}));
    const auto connection = checked(engine.connect({b}, {mix}, {0.5f, false, {}}));
    checked(engine.connect({mix}, {main}));
    checked(engine.connect({mix}, {gain}));
    checked(engine.connect({gain}, {monitor}));
    checked(engine.prepare());
    const float first[] = {1, 2, 3, 4};
    const float second[] = {2, 2, 2, 2};
    auto feed = [&] {
        checked(checked(engine.node<Source>(a))->push_interleaved(first, 4));
        checked(checked(engine.node<Source>(b))->push_interleaved(second, 4));
    };
    feed();
    render(engine);
    for (std::size_t i = 0; i < 4; ++i)
    {
        near(captured(engine, main).channel(0)[i], first[i] + 1);
        near(captured(engine, monitor).channel(0)[i], (first[i] + 1) * 0.25f);
    }
    checked(engine.set_muted(connection, true));
    feed();
    render(engine);
    near(captured(engine, main).channel(0)[0], 1);
    checked(engine.set_muted(connection, false));
    checked(engine.set_gain(connection, -1));
    checked(checked(engine.node<Gain>(gain))->set_gain(2));
    feed();
    render(engine);
    near(captured(engine, main).channel(0)[0], -1);
    near(captured(engine, monitor).channel(0)[0], -2);
    check(engine.frame_position() == 3 * test_frames, "Unexpected timeline");
    check(checked(engine.node<Sink>(main))->frame_position() == 2 * test_frames, "Sink timestamp must mark the block start");
}

void multiple_ports_and_matrices()
{
    TestEngine engine;
    const auto mono = checked(engine.emplace_node<Source>(1));
    const auto stereo = checked(engine.emplace_node<Source>(2));
    const auto processor = checked(engine.emplace_node<TwoPortNode>());
    const auto stereo_sink = checked(engine.emplace_node<Sink>(2));
    const auto mono_sink = checked(engine.emplace_node<Sink>(1));
    const auto mono_connection = checked(engine.connect({mono}, {processor, 0}));
    auto swap_channels = checked(ChMatrix::create(2, 2, {0, 1, 1, 0}));
    checked(engine.connect({stereo}, {processor, 1}, {1, false, std::move(swap_channels)}));
    checked(engine.connect({processor, 0}, {stereo_sink}));
    checked(engine.connect({processor, 1}, {mono_sink}));
    checked(engine.prepare());
    const float a[] = {1, 2, 3, 4};
    const float b[] = {10, 20, 11, 21, 12, 22, 13, 23};
    checked(checked(engine.node<Source>(mono))->push({a, 1, 4, 4}));
    checked(checked(engine.node<Source>(stereo))->push_interleaved(b, 4));
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
    checked(engine.disconnect(mono_connection));
    checked(engine.prepare());
    render(engine);
    near(captured(engine, mono_sink).channel(0)[0], 0);
    near(captured(engine, stereo_sink).channel(0)[0], 0);
}

void source_staging()
{
    TestEngine engine;
    const auto source = checked(engine.emplace_node<Source>(1));
    const auto sink = checked(engine.emplace_node<Sink>(1));
    checked(engine.connect({source}, {sink}));
    auto &input = *checked(engine.node<Source>(source));
    std::array<float, test_frames> samples;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<float>(i + 1);
    }
    expect_error(ErrorCode::NotPrepared, input.push_interleaved(samples.data(), samples.size()));
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(engine.frame_position() == 0, "Rejected processing must not advance the timeline");
    checked(engine.prepare());
    expect_error(ErrorCode::InvalidArg, input.push_interleaved(samples.data(), test_frames + 1));
    expect_error(ErrorCode::InvalidArg, input.push_interleaved(nullptr, 1));
    checked(input.push_interleaved(samples.data(), samples.size()));
    checked(input.push_interleaved(nullptr, 0));
    expect_error(ErrorCode::BufferFull, input.push_interleaved(samples.data(), 1));
    checked(engine.emplace_node<Mixer>(1));
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(engine.frame_position() == 0 && input.pending_frames() == test_frames,
          "Rejected processing must not consume pending audio");
    checked(engine.prepare());
    render(engine);
    near(captured(engine, sink).channel(0)[0], 1);
    near(captured(engine, sink).channel(0)[test_frames - 1], static_cast<float>(test_frames));
    check(input.pending_frames() == 0 && engine.frame_position() == test_frames,
          "Processing must consume one complete period");

    checked(input.push_interleaved(samples.data(), 2));
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
    checked(input.push_interleaved(samples.data(), samples.size()));
    engine.reset();
    check(input.pending_frames() == 0 && engine.frame_position() == 0, "Reset must clear staging and timeline");
    check(captured(engine, sink).frames() == 0, "Reset must release the captured view");
    render(engine);
    near(captured(engine, sink).channel(0)[0], 0);
}

void channel_count_conversion()
{
    TestEngine engine;
    const auto source = checked(engine.emplace_node<Source>(2));
    const auto downmix = checked(engine.emplace_node<Mixer>(1));
    const auto sink = checked(engine.emplace_node<Sink>(2));
    auto stereo_to_mono = checked(ChMatrix::create(1, 2, {0.25f, 0.75f}));
    auto mono_to_stereo = checked(ChMatrix::create(2, 1, {1, -1}));
    const auto connection = checked(engine.connect({source}, {downmix}, {2, false, std::move(stereo_to_mono)}));
    checked(engine.connect({downmix}, {sink}, {1, false, std::move(mono_to_stereo)}));
    checked(engine.prepare());
    const float samples[] = {2, 6, 4, 8};
    checked(checked(engine.node<Source>(source))->push_interleaved(samples, 2));
    render(engine);
    near(captured(engine, sink).channel(0)[0], 10);
    near(captured(engine, sink).channel(1)[1], -14);
    checked(engine.set_muted(connection, true));
    checked(checked(engine.node<Source>(source))->push_interleaved(samples, 2));
    render(engine);
    near(captured(engine, sink).channel(0)[0], 0);
    near(captured(engine, sink).channel(1)[1], 0);
}

void topology_preserves_state()
{
    TestEngine engine;
    const auto source = checked(engine.emplace_node<CountingNode>());
    auto &counter = *checked(engine.node<CountingNode>(source));
    checked(engine.prepare());
    check(counter.preparations == 0, "Nodes outside the execution plan need no resources");
    const auto a = checked(engine.emplace_node<Sink>(1));
    const auto first_conn = checked(engine.connect({source}, {a}));
    checked(engine.prepare());
    checked(engine.prepare());
    check(counter.preparations == 1, "Preparation must be idempotent");
    render(engine);
    const auto b = checked(engine.emplace_node<Sink>(1));
    checked(engine.connect({source}, {b}));
    expect_error(ErrorCode::NotPrepared, engine.process());
    checked(engine.prepare());
    render(engine);
    check(counter.preparations == 1 && counter.calls == 2, "Fanout must execute the existing source once");
    near(captured(engine, a).channel(0)[0], 2);
    near(captured(engine, b).channel(0)[0], 2);
    checked(engine.remove_node(a));
    checked(engine.prepare());
    render(engine);
    near(captured(engine, b).channel(0)[0], 3);
    expect_error(ErrorCode::UnknownConn, engine.disconnect(first_conn));
    checked(engine.remove_node(b));
    checked(engine.prepare());
    render(engine);
    check(counter.calls == 3 && engine.frame_position() == 4 * test_frames,
          "Nodes without a downstream sink must remain idle");
}

void graph_validation()
{
    TestEngine engine;
    const auto a = checked(engine.emplace_node<Mixer>(1));
    const auto b = checked(engine.emplace_node<Mixer>(1));
    const auto sink = checked(engine.emplace_node<Sink>(1));
    checked(engine.connect({a}, {b}));
    const auto connection = checked(engine.connect({b}, {sink}));
    checked(engine.connect({a}, {sink}));
    checked(engine.prepare());
    expect_error(ErrorCode::Cycle, engine.connect({b}, {a}));
    expect_error(ErrorCode::Cycle, engine.connect({a}, {a}));
    expect_error(ErrorCode::DuplicateConn, engine.connect({a}, {b}));
    expect_error(ErrorCode::InvalidPort, engine.connect({sink}, {a}));
    expect_error(ErrorCode::InvalidPort, engine.connect({a}, {sink, 1}));
    expect_error(ErrorCode::InvalidType, engine.node<Sine>(a));
    render(engine);
    checked(engine.remove_node(b));
    expect_error(ErrorCode::UnknownNode, engine.node(b));
    expect_error(ErrorCode::UnknownNode, engine.node<Sine>(b));
    expect_error(ErrorCode::UnknownConn, engine.set_muted(connection, true));
    TestEngine other;
    const auto foreign = checked(other.emplace_node<Mixer>(1));
    expect_error(ErrorCode::UnknownNode, engine.connect({foreign}, {a}));
    expect_error(ErrorCode::UnknownNode, engine.connect({a}, {foreign}));
    const auto stereo = checked(engine.emplace_node<Source>(2));
    expect_error(ErrorCode::ChannelMismatch, engine.connect({stereo}, {a}));
    expect_error(ErrorCode::ChannelMismatch,
                 engine.connect({stereo}, {a}, {1, false, checked(ChMatrix::create(2, 2, {1, 0, 0, 1}))}));
}

void prepare_failure_and_retry()
{
    TestEngine engine;
    const auto source = checked(engine.emplace_node<CountingNode>());
    const auto sink = checked(engine.emplace_node<RetrySink>());
    checked(engine.connect({source}, {sink}));
    expect_error(ErrorCode::NoMemory, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(checked(engine.node<CountingNode>(source))->calls == 0 && engine.frame_position() == 0,
          "Failed preparation must not execute nodes or advance time");
    checked(engine.prepare());
    render(engine);
    check(checked(engine.node<RetrySink>(sink))->attempts == 2, "Failed nodes must be prepared again");
    check(checked(engine.node<CountingNode>(source))->preparations == 1, "Prepared nodes must retain their state after retry");

    // A failed rebuild must also leave an existing plan unusable until retry succeeds.
    const auto added_sink = checked(engine.emplace_node<RetrySink>(true));
    checked(engine.connect({source}, {added_sink}));
    expect_error(ErrorCode::NodeFailed, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    check(checked(engine.node<CountingNode>(source))->calls == 1 && engine.frame_position() == test_frames,
          "Failed rebuild must preserve the last processed state");
    checked(engine.prepare());
    render(engine);
    check(checked(engine.node<CountingNode>(source))->calls == 2, "Retry must preserve state and execute shared nodes once");
    check(checked(engine.node<CountingNode>(source))->preparations == 1, "Rebuilding must not prepare existing nodes again");
}

void status_and_capacity()
{
    const auto too_many = std::numeric_limits<std::size_t>::max();
    const auto invalid_gain = std::numeric_limits<float>::quiet_NaN();
    Buffer buffer;
    checked(buffer.resize(1, 2));
    buffer.block(2).channel(0)[0] = 1;
    expect_error(ErrorCode::InvalidArg, buffer.resize(0, 2));
    expect_error(ErrorCode::TooLarge, buffer.resize(too_many, 2));
    check(buffer.block(2).channels() == 1 && buffer.capacity_frames() == 2, "Failed resize must preserve the buffer");
    near(buffer.block(2).channel(0)[0], 1);
    expect_error(ErrorCode::TooLarge, ChMatrix::create(too_many, too_many, {1}));

    TestEngine engine;
    expect_error(ErrorCode::InvalidArg, engine.add_node(nullptr));
    const auto sink = checked(engine.emplace_node<Sink>(1));
    const auto invalid_node = checked(engine.emplace_node<Gain>(1, invalid_gain));
    checked(engine.connect({invalid_node}, {sink}));
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    checked(engine.remove_node(invalid_node));
    for (const auto frequency_hz : {-1.0, 8000.0})
    {
        const auto sine = checked(engine.emplace_node<Sine>(1, frequency_hz));
        checked(engine.connect({sine}, {sink}));
        expect_error(ErrorCode::InvalidArg, engine.prepare());
        checked(engine.remove_node(sine));
    }
    const auto gain = checked(engine.emplace_node<Gain>(1));
    const auto connection = checked(engine.connect({gain}, {sink}));
    checked(engine.prepare());
    expect_error(ErrorCode::InvalidArg, engine.set_gain(connection, invalid_gain));
    expect_error(ErrorCode::InvalidArg, checked(engine.node<Gain>(gain))->set_gain(invalid_gain));
    render(engine);

    const auto oversized = checked(engine.emplace_node<Sink>(too_many));
    expect_error(ErrorCode::TooLarge, engine.prepare());
    expect_error(ErrorCode::NotPrepared, engine.process());
    checked(engine.remove_node(oversized));
    checked(engine.prepare());
    render(engine);
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
    expect_error(ErrorCode::NotPrepared, empty.process());
    checked(empty.prepare());
    check(empty.process() && empty.frame_position() == 480, "Prepared empty graphs advance a full period");
    TestEngine engine;
    expect_error(ErrorCode::InvalidArg, engine.emplace_node<Mixer>(0));
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(2, 1, {1}));
    expect_error(ErrorCode::InvalidArg, ChMatrix::create(1, 1, {std::numeric_limits<float>::quiet_NaN()}));
    const auto source = checked(engine.emplace_node<Sine>(2, 4000, 0.5f));
    const auto sink = checked(engine.emplace_node<Sink>(2));
    checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
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
                 {"mixing and fanout", mixing_and_fanout},
                 {"multiple ports and matrices", multiple_ports_and_matrices},
                 {"channel count conversion", channel_count_conversion},
                 {"source staging", source_staging},
                 {"topology preserves state", topology_preserves_state},
                 {"graph validation", graph_validation},
                 {"prepare failure and retry", prepare_failure_and_retry},
                 {"status and capacity", status_and_capacity},
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
