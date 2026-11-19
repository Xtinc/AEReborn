#include "support/test_harness.h"
#include "support/pcm_reference.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

#include <cmath>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
constexpr std::size_t block_frames = 240;
constexpr double two_pi = 6.283185307179586;

void capture_format_switch()
{
    Engine engine(rate, period);
    const auto input_id = checked(engine.emplace_node<IAdapter>(2, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(2, engine.spec()));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(engine.connect({input_id}, {output_id}, {0.5f, false, {}}));
    checked(engine.prepare());
    checked(input->configure_pcm({rate, PCMFormat::S16LE, 2, 2}));
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 2}));
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    const auto bind_output = [&] { checked(output->bind_pcm({packet.data(), packet.size()})); };
    VirtualSink sink({rate, PCMFormat::F32LE, 2}, period);
    checked(sink.prepare());
    {
        VirtualSource old_device({rate, PCMFormat::S16LE, 2, 2}, period, 431, 0.25f);
        checked(old_device.prepare());
        checked(old_device.generate());
        checked(input->bind_pcm(old_device.pcm()));
        bind_output();
        checked(engine.process()); // Leave one old-device period bound.
        input->reset();           // Unplug releases borrowed memory before the device is destroyed.
    }
    bind_output();
    checked(engine.process());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Unplugged capture produces silence while the graph clock continues");

    const ChMatrix matrix(2, 1, {1, -0.5f});
    const PCMSpec new_spec{SampleRate::Hz16000, PCMFormat::S16LE, 1};
    checked(input->configure_pcm(new_spec, matrix));
    VirtualSource device(new_spec, period, 1000, 0.5f);
    checked(device.prepare());
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(device.generate());
        checked(input->bind_pcm(device.pcm()));
        bind_output();
        checked(engine.process());
        checked(sink.receive({packet.data(), packet.size()}));
        for (std::size_t frame = 0; frame < block_frames; ++frame)
        {
            const auto index = transfer * block_frames + frame;
            if (index > 288)
            {
                const auto expected = 0.25 * std::sin(two_pi * 1000 * (static_cast<double>(index) / 48000 - 48.0 / 16000));
                check(std::abs(read_f32le(sink.pcm().data() + frame * 8) - expected) < 0.005 &&
                          std::abs(read_f32le(sink.pcm().data() + frame * 8 + 4) + 0.5 * expected) < 0.005,
                      "16 kHz mono capture is resampled and mapped into the original stereo graph");
            }
        }
    }

    // Successful reconfiguration of a running resampler also discards its bound PCM and history.
    checked(input->bind_pcm(device.pcm()));
    checked(input->configure_pcm(new_spec, matrix));
    VirtualSource silence(new_spec, period, 0, 0);
    checked(silence.prepare());
    checked(silence.generate());
    checked(input->bind_pcm(silence.pcm()));
    bind_output();
    checked(engine.process());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }) &&
              checked(engine.node<IAdapter>(input_id)) == input && input->outputs()[0] == 2 &&
              engine.frame_position() == 13 * block_frames,
          "Reconfiguration starts with fresh history while preserving node, channels and graph time");
}

void playback_format_switch()
{
    Engine engine(rate, period);
    const auto input_id = checked(engine.emplace_node<IAdapter>(2, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(2, engine.spec()));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(engine.connect({input_id}, {output_id}, {0.5f, false, {}}));
    checked(engine.prepare());
    const PCMSpec source_spec{rate, PCMFormat::F32LE, 1};
    checked(input->configure_pcm(source_spec, ChMatrix(2, 1, {1, 0.5f})));
    VirtualSource source(source_spec, period, 1000, 1);
    checked(source.prepare());
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 2, 2}));
    std::vector<std::uint8_t> old_packet(output->pcm_bytes(), 0xcd);
    const auto feed = [&] {
        checked(source.generate());
        checked(input->bind_pcm(source.pcm()));
    };
    checked(output->bind_pcm({old_packet.data(), old_packet.size()}));
    feed();
    checked(engine.process());
    const auto previous = old_packet;

    const ChMatrix matrix(1, 2, {0.25f, 0.75f});
    checked(output->configure_pcm({SampleRate::Hz16000, PCMFormat::S16LE, 1}, matrix));
    feed(); // No new buffer yet: drop this output period and do not write the old device.
    checked(engine.process());
    check(old_packet == previous, "Switching playback cancels a partially written old device buffer");
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    VirtualSink sink({SampleRate::Hz16000, PCMFormat::S16LE, 1}, period);
    checked(sink.prepare());
    const auto start_frame = engine.frame_position();
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(output->bind_pcm({packet.data(), packet.size()}));
        feed();
        checked(engine.process());
        checked(sink.receive({packet.data(), packet.size()}));
        const auto pcm = sink.pcm();
        for (std::size_t frame = 0; frame < 80; ++frame)
        {
            const auto word = static_cast<std::uint16_t>(pcm[frame * 2]) |
                              (static_cast<std::uint16_t>(pcm[frame * 2 + 1]) << 8);
            const auto sample = (word >= 32768 ? static_cast<int>(word) - 65536 : static_cast<int>(word)) / 32768.0;
            const auto index = transfer * 80 + frame;
            if (index > 32)
            {
                const auto seconds = static_cast<double>(start_frame) / 48000 + static_cast<double>(index) / 16000 -
                                     48.0 / 48000;
                check(std::abs(sample - 0.3125 * std::sin(two_pi * 1000 * seconds)) < 0.005,
                      "Stereo graph playback is mapped and resampled to the new 16 kHz mono device");
            }
        }
    }
    // Replacing an active resampler with the original format must not retain a filter tail or index.
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 2}));
    std::vector<std::uint8_t> stereo(output->pcm_bytes(), 0xcd);
    checked(output->bind_pcm({stereo.data(), stereo.size()}));
    checked(engine.process()); // Missing source is silence.
    check(std::all_of(stereo.begin(), stereo.end(), [](std::uint8_t byte) { return byte == 0; }) &&
              old_packet == previous && checked(engine.node<OAdapter>(output_id)) == output &&
              output->inputs()[0] == 2 && engine.frame_position() == 13 * block_frames,
          "Playback reconfiguration preserves logical identity and time, without old-device writes or history");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"capture_format_switch", capture_format_switch},
        {"playback_format_switch", playback_format_switch}
    });
}
