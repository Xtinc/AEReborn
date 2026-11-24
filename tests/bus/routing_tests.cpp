#include "tests/support/io_fixture.h"
#include "backend/push_source.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "tests/support/tone.h"

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

void subgroup_and_monitor()
{
    const auto sample_rate = SampleRate::Hz44100;
    const PortSpec stereo(sample_rate, 2, SampleFormat::F32P);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<FloatSource>(sample_rate, 4, [](std::uint64_t frame, std::size_t ch) {
        return static_cast<float>(static_cast<int>((frame + 3 * ch) % 17) - 8) / 32;
    });
    const auto pair_a =
        engine.emplace_node<Matrix>(sample_rate, 4, 2, std::initializer_list<float>{1, 0, 0, 0, 0, 1, 0, 0});
    const auto pair_b =
        engine.emplace_node<Matrix>(sample_rate, 4, 2, std::initializer_list<float>{0, 0, 1, 0, 0, 0, 0, 1});
    const auto tone = engine.emplace_node<Tone>(sample_rate, 2, 997, 0.125f);
    const auto subgroup = engine.emplace_node<Mixer>(sample_rate, 2, 2);
    const auto fader = engine.emplace_node<Matrix>(sample_rate, 2, 0.5f);
    const auto master = engine.emplace_node<Mixer>(sample_rate, 2, 2);
    const auto pre = engine.emplace_node<PacketSink>(stereo);
    const auto post = engine.emplace_node<PacketSink>(stereo);
    const auto out = engine.emplace_node<PacketSink>(stereo);
    connect(engine, input, pair_a);
    connect(engine, input, pair_b);
    connect(engine, pair_a, subgroup);
    connect(engine, tone, subgroup, 1);
    connect(engine, subgroup, pre);
    connect(engine, subgroup, fader);
    connect(engine, fader, post);
    connect(engine, fader, master);
    connect(engine, pair_b, master, 1);
    connect(engine, master, out);
    engine.commit();
    const auto *source = engine.node<FloatSource>(input);
    const auto *before = engine.node<PacketSink>(pre);
    const auto *after = engine.node<PacketSink>(post);
    const auto *output = engine.node<PacketSink>(out);
    clock.start();
    std::size_t start = 0;
    for (std::size_t tick = 0; tick < 8; ++tick)
    {
        const bool silent = tick == 3 || tick == 4;
        engine.node<Mixer>(master)->set_muted(0, silent);
        engine.node<Mixer>(master)->set_muted(1, silent);
        clock.advance();
        const std::size_t frames = tick % 2 ? 220 : 221;
        check(before->frames == frames && after->frames == frames && output->frames == frames &&
                  source->calls == tick + 1,
              "The fractional frame count is shared and the source renders once");
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                const auto time = start + frame;
                const auto first = (static_cast<int>((time + 3 * ch) % 17) - 8) / 32.0;
                const auto second = (static_cast<int>((time + 3 * (ch + 2)) % 17) - 8) / 32.0;
                const auto submix = first + 0.125 * std::sin(two_pi * 997 * time / 44100);
                check(std::abs(before->sample(frame, ch) - submix) < 1e-6 &&
                          std::abs(after->sample(frame, ch) - submix * 0.5) < 1e-6 &&
                          std::abs(output->sample(frame, ch) - (silent ? 0 : submix * 0.5 + second)) < 1e-6,
                      "Pre-fader and post-fader taps continue while the master is muted");
            }
        start += frames;
    }
}

class HalfPcm final : public NodeT<1, 1>
{
  public:
    explicit HalfPcm(PortSpec spec) : NodeT({spec}, {spec}) {}

    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0].byte_view();
        const auto output = context.outputs[0].byte_view();
        for (std::size_t i = 0; i < input.size(); i += 2)
        {
            const int word = input[i] | (input[i + 1] << 8);
            const auto value = static_cast<std::uint16_t>((word < 32768 ? word : word - 65536) / 2);
            output[i] = static_cast<std::uint8_t>(value);
            output[i + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        frames = context.inputs[0].frames();
    }
    std::size_t frames = 0;
};

void parallel_pcm_algorithm()
{
    const PortSpec dry_spec(rate, 1, SampleFormat::F32P);
    const PortSpec pcm(SampleRate::Hz16000, 1, SampleFormat::S16LE);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Tone>(rate, 1, 875, 0.25f);
    const auto down = engine.emplace_node<Convert>(dry_spec, pcm);
    const auto algorithm = engine.emplace_node<HalfPcm>(pcm);
    const auto up = engine.emplace_node<Convert>(pcm, dry_spec);
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto wet = engine.emplace_node<PacketSink>(dry_spec);
    const auto output = engine.emplace_node<PacketSink>(dry_spec);
    connect(engine, input, down);
    connect(engine, down, algorithm);
    connect(engine, algorithm, up);
    connect(engine, up, wet);
    connect(engine, up, mix, 1);
    connect(engine, input, mix);
    connect(engine, mix, output);
    engine.commit();
    const auto *filtered = engine.node<PacketSink>(wet);
    const auto *result = engine.node<PacketSink>(output);
    clock.start();
    for (std::size_t tick = 0; tick < 20; ++tick)
    {
        clock.advance();
        check(engine.node<HalfPcm>(algorithm)->frames == 80 && result->frames == 240 && filtered->frames == 240,
              "The PCM algorithm and master run at their own block sizes");
        if (tick > 1)
            for (std::size_t frame = 0; frame < 240; ++frame)
            {
                const auto time = (tick * 240 + frame) / 48000.0;
                const auto dry = 0.25 * std::sin(two_pi * 875 * time);
                const auto wet_sample = 0.125 * std::sin(two_pi * 875 * (time - 0.004));
                check(std::abs(filtered->sample(frame) - wet_sample) < 0.003 &&
                          std::abs(result->sample(frame) - (dry + wet_sample)) < 0.003,
                      "The dry signal stays aligned while the processed path has two FIR delays");
            }
    }
}

void live_input_switch()
{
    const PortSpec stereo(rate, 2, SampleFormat::F32P);
    const PortSpec mono_pcm(rate, 1, SampleFormat::S16LE), stereo_pcm(rate, 2, SampleFormat::F32LE);
    Engine engine(period);
    ClockedGraph clock(engine);
    auto input = engine.emplace_node<Capture>(rate, 1, 5ms);
    auto mapping = engine.emplace_node<Matrix>(rate, 1, 2, std::initializer_list<float>{1, 1});
    const auto healthy = engine.emplace_node<Tone>(rate, 2, 997, 0.125f);
    const auto fader = engine.emplace_node<Matrix>(rate, 2, 0.5f);
    const auto mixer = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto tap = engine.emplace_node<PacketSink>(stereo);
    const auto master = engine.emplace_node<PacketSink>(stereo);
    connect(engine, input, mapping);
    connect(engine, mapping, fader);
    connect(engine, fader, mixer);
    connect(engine, healthy, mixer, 1);
    connect(engine, healthy, tap);
    connect(engine, mixer, master);
    engine.commit();
    PushSource first(mono_pcm), second(stereo_pcm);
    first.bind(*engine.node<Capture>(input));
    first.start();
    Pcm in_mono(mono_pcm, 240), in_stereo(stereo_pcm, 240);
    for (std::size_t frame = 0; frame < 240; ++frame)
    {
        in_mono.bytes[2 * frame + 1] = 0x40;
        in_stereo.set(frame, 0, 0.25f);
        in_stereo.set(frame, 1, -0.5f);
    }
    auto *gain = engine.node<Matrix>(fader);
    auto *mix = engine.node<Mixer>(mixer);
    const auto *monitor = engine.node<PacketSink>(tap);
    const auto *output = engine.node<PacketSink>(master);
    clock.start();
    const auto verify_block = [&](PushSource &source, const Pcm &packet, float left, float right, bool ramp) {
        const auto tick = engine.tick_position();
        check(source.push(packet.input(0, 240)) == 240, "The active capture accepts the input block");
        clock.advance();
        check(engine.tick_position() == tick + 1 && output->frames == 240 && monitor->frames == 240,
              "The new plan renders on the continuing tick sequence");
        for (std::size_t frame = 0; frame < 240; ++frame)
        {
            const auto ref = 0.125 * std::sin(two_pi * 997 * (tick * 240 + frame) / 48000);
            const auto scale = ramp ? 0.5 + 0.5 * (frame + 1) / 240 : 1;
            check(std::abs(monitor->sample(frame, 0) - ref) < 1e-6 &&
                      std::abs(output->sample(frame, 0) - ref - left * scale) < 1e-6 &&
                      std::abs(output->sample(frame, 1) - ref - right * scale) < 1e-6,
                  "Gain, mute and input format affect just the routed branch");
        }
    };
    verify_block(first, in_mono, 0.25f, 0.25f, false);
    gain->set_gain(1.0f);
    verify_block(first, in_mono, 0.5f, 0.5f, true);
    mix->set_muted(0, true);
    verify_block(first, in_mono, 0, 0, false);
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(input, std::make_unique<Capture>(rate, 2, 5ms)); });
    first.unbind();
    engine.remove_node(input);
    engine.remove_node(mapping);
    input = engine.emplace_node<Capture>(rate, 2, 5ms);
    mapping = engine.emplace_node<Matrix>(rate, 2, 2, std::initializer_list<float>{0, 1, 1, 0});
    connect(engine, input, mapping);
    connect(engine, mapping, fader);
    const auto tick = engine.tick_position();
    clock.commit();
    check(engine.tick_position() >= tick, "Committing the new source preserves graph time");
    second.bind(*engine.node<Capture>(input));
    second.start();
    mix->set_muted(0, false);
    verify_block(second, in_stereo, -0.5f, 0.25f, false);
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"subgroup_and_monitor", subgroup_and_monitor},
                                                {"parallel_pcm_algorithm", parallel_pcm_algorithm},
                                                {"live_input_switch", live_input_switch}});
}
