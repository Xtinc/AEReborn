#include "core/engine.h"
#include "checked.h"

namespace
{
template <aereborn::SampleRate Rate, aereborn::BlockPeriod Period>
class LinkNode final : public aereborn::NodeT<1, 1, Rate, Period>
{
  public:
    LinkNode() : aereborn::NodeT<1, 1, Rate, Period>({{1}}, {{1}})
    {
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
    using aereborn::test::checked;
    Engine<SampleRate::Hz16000, BlockPeriod::Ms05> engine;
    const auto a = checked(engine.emplace_node<LinkNode<SampleRate::Hz16000, BlockPeriod::Ms05>>());
    const auto b = checked(engine.emplace_node<LinkNode<SampleRate::Hz16000, BlockPeriod::Ms05>>());
    const auto connection = checked(engine.connect({a}, {b}));
    checked(engine.prepare());
    checked(engine.process());
    if (engine.frame_position() != 80)
    {
        throw std::runtime_error("Engine template failed in another translation unit");
    }
    Engine<SampleRate::Hz48000, BlockPeriod::Ms10> defaults;
    const auto c = checked(defaults.emplace_node<LinkNode<SampleRate::Hz48000, BlockPeriod::Ms10>>());
    return {{a.value, b.value, connection.value, c.value}};
}
