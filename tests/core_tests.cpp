#include "core/engine.h"
#include "checked.h"

#include <cmath>
#include <iostream>

using namespace aereborn;
using aereborn::test::checked;

std::array<std::uint64_t, 4> ids_from_other_translation_unit();

namespace
{
constexpr auto test_rate = SampleRate::Hz16000;
constexpr auto test_period = BlockPeriod::Ms05;
using TestEngine = Engine<test_rate, test_period>;
constexpr auto test_frames = aereborn::calc_block_frames<test_rate, test_period>();

void check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class ValueNode final : public NodeT<0, 1, test_rate, test_period>
{
  public:
    explicit ValueNode(float value) : NodeT({}, {{1}}), value_(value)
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        ++calls;
        std::fill_n(context.outputs[0].channel(0), test_frames, value_);
    }
    int calls = 0;

  private:
    float value_;
};

class CopyNode final : public NodeT<1, 1, test_rate, test_period>
{
  public:
    CopyNode() : NodeT({{1}}, {{1}})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        ++calls;
        std::copy_n(context.inputs[0].channel(0), test_frames, context.outputs[0].channel(0));
    }
    int calls = 0;
};

// Multiple connections are a core port capability, independent of any built-in mixer.
class CaptureNode final : public NodeT<1, 0, test_rate, test_period>
{
  public:
    CaptureNode() : NodeT({{1}}, {})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        block = context.inputs[0];
    }
    IBlock block;
};

void expect_block(TestEngine &engine, NodeId capture, float expected)
{
    const auto block = checked(engine.node<CaptureNode>(capture))->block;
    check(block.frames() == test_frames, "Unexpected block length");
    for (std::size_t i = 0; i < test_frames; ++i)
    {
        check(std::isfinite(block.channel(0)[i]) && std::abs(block.channel(0)[i] - expected) < 0.00001f,
              "Unexpected mixed sample");
    }
}
} // namespace

int main()
{
    try
    {
        TestEngine engine;
        // Create downstream nodes first; execution order must follow dependencies, not IDs.
        const auto unused = checked(engine.emplace_node<CopyNode>());
        const auto capture = checked(engine.emplace_node<CaptureNode>());
        const auto a = checked(engine.emplace_node<ValueNode>(2.0f));
        const auto b = checked(engine.emplace_node<ValueNode>(4.0f));
        // This inactive connection sorts before the active connections in the shared index.
        checked(engine.connect({a}, {unused}));
        const auto first = checked(engine.connect({a}, {capture}));
        const auto second = checked(engine.connect({b}, {capture}, {0.5f, false, {}}));
        const auto remote_ids = ids_from_other_translation_unit();
        for (std::size_t i = 0; i < remote_ids.size(); ++i)
        {
            for (std::size_t j = i + 1; j < remote_ids.size(); ++j)
            {
                check(remote_ids[i] != remote_ids[j], "IDs must be unique across engine specifications");
            }
            for (const auto local_id : {unused.value, capture.value, a.value, b.value, first.value, second.value})
            {
                check(local_id != remote_ids[i], "IDs must be unique across translation units");
            }
        }
        check(!engine.process() && engine.frame_position() == 0 && checked(engine.node<ValueNode>(a))->calls == 0,
              "Unprepared processing must not execute nodes or advance time");
        checked(engine.prepare());
        checked(engine.process());
        expect_block(engine, capture, 4);
        check(checked(engine.node<ValueNode>(a))->calls == 1 && checked(engine.node<CopyNode>(unused))->calls == 0,
              "Only nodes reaching an outputless node should execute");

        checked(engine.set_gain(second, 1));
        checked(engine.set_muted(first, true));
        checked(engine.process());
        expect_block(engine, capture, 4);

        checked(engine.remove_node(a));
        const auto c = checked(engine.emplace_node<ValueNode>(10.0f));
        const auto third = checked(engine.connect({c}, {capture}, {0.1f, false, {}}));
        const auto another_capture = checked(engine.emplace_node<CaptureNode>());
        checked(engine.connect({b}, {another_capture}));
        check(!engine.process(), "Topology edits must invalidate bindings");
        checked(engine.prepare());
        checked(engine.process());
        expect_block(engine, capture, 5);
        expect_block(engine, another_capture, 4);
        check(checked(engine.node<ValueNode>(b))->calls == 3, "Fanout must execute a custom node only once");

        checked(engine.disconnect(third));
        checked(engine.prepare());
        checked(engine.process());
        expect_block(engine, capture, 4);
        expect_block(engine, another_capture, 4);
        std::cout << "PASS: core graph without built-in plugins\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
