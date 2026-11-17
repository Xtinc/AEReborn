#include "core/engine.h"

namespace
{
template <aereborn::SampleRate Rate, aereborn::BlockPeriod Period>
class LinkNode final : public aereborn::NodeT<1, 1, Rate, Period>
{
  public:
    LinkNode() : aereborn::NodeT<1, 1, Rate, Period>({{1}}, {{1}})
    {
    }
    void attributes(std::ostream &stream) const override
    {
        stream << "LinkNode {}";
    }
    void process(const aereborn::ProcContext &context) noexcept override
    {
        std::copy_n(context.inputs[0].channel(0), aereborn::Engine<Rate, Period>::block_frames(),
                    context.outputs[0].channel(0));
    }
};
} // namespace

std::array<std::uint64_t, 4> ids_from_other_translation_unit()
{
    using namespace aereborn;
    Engine<SampleRate::Hz16000, BlockPeriod::Ms05> engine;
    const auto a = engine.emplace_node<LinkNode<SampleRate::Hz16000, BlockPeriod::Ms05>>();
    const auto b = engine.emplace_node<LinkNode<SampleRate::Hz16000, BlockPeriod::Ms05>>();
    const auto connection = engine.connect(engine.output(a), engine.input(b));
    engine.prepare();
    if (!engine.process() || engine.frame_position() != 80)
    {
        throw std::runtime_error("Engine template failed in another translation unit");
    }
    Engine<SampleRate::Hz48000, BlockPeriod::Ms10> defaults;
    const auto c = defaults.emplace_node<LinkNode<SampleRate::Hz48000, BlockPeriod::Ms10>>();
    return {{a.value, b.value, connection.value, c.value}};
}
