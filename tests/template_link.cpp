#include "checked.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

#include <array>
#include <vector>

std::array<std::uint64_t, 4> ids_from_other_translation_unit()
{
    using namespace aereborn;
    using namespace aereborn::test;
    constexpr auto rate = SampleRate::Hz16000;
    constexpr auto period = BlockPeriod::Ms05;
    const PCMSpec spec{rate, PCMFormat::F32LE, 1};
    Engine<rate, period> engine;
    const auto input_id = checked(engine.emplace_node<IAdapter<rate, period>>(spec.channels));
    checked(checked(engine.node<IAdapter<rate, period>>(input_id))->configure_pcm(spec));
    const auto output_id = checked(engine.emplace_node<OAdapter<rate, period>>(spec.channels));
    checked(checked(engine.node<OAdapter<rate, period>>(output_id))->configure_pcm(spec));
    const auto connection = checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    VirtualSource source(spec, period, 1000.0);
    checked(source.prepare());
    checked(source.generate());
    const auto pcm = source.pcm();
    std::vector<std::uint8_t> received(pcm.size());
    checked(checked(engine.node<IAdapter<rate, period>>(input_id))->bind_pcm(pcm));
    checked(checked(engine.node<OAdapter<rate, period>>(output_id))->bind_pcm({received.data(), received.size()}));
    checked(engine.process());
    check(engine.frame_position() == 80 && std::equal(pcm.begin(), pcm.end(), received.begin()),
          "Adapter templates must link and render from another translation unit");
    Engine<SampleRate::Hz48000, BlockPeriod::Ms10> other;
    const auto other_input = checked(other.emplace_node<IAdapter<>>(1));
    return {{input_id.value, output_id.value, connection.value, other_input.value}};
}
