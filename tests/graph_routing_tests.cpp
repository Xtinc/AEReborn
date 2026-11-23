#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include "support/tone.h"
#include <array>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void explicit_mix_and_fanout()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto a = engine.emplace_node<Tone>(rate, 2, 997, 0.25f);
    const auto b = engine.emplace_node<Tone>(rate, 2, 431, 0.125f);
    const auto mix = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto matrix =
        engine.emplace_node<Matrix>(rate, 2, 2, std::initializer_list<float>{0, 1, 1, 0}, 0.5f);
    const auto first = engine.emplace_node<PacketSink>(PortSpec(rate, 2, SampleFormat::F32P));
    const auto second = engine.emplace_node<PacketSink>(PortSpec(rate, 2, SampleFormat::F32P));
    engine.connect(OPORT(a, 0), IPORT(mix, 0));
    engine.connect(OPORT(b, 0), IPORT(mix, 1));
    engine.connect(OPORT(mix, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(first, 0));
    engine.connect(OPORT(matrix, 0), IPORT(second, 0));
    engine.commit();
    auto *one = engine.node<PacketSink>(first);
    auto *two = engine.node<PacketSink>(second);
    clock.start();
    for (std::size_t tick = 0; tick < 5; ++tick)
    {
        clock.advance();
        check(one->address == two->address, "Fan-out shares one immutable upstream buffer");
        for (std::size_t frame = 0; frame < 240; ++frame)
        {
            const auto t = static_cast<double>(tick * 240 + frame) / 48000;
            const auto expected = 0.5 * (0.25 * std::sin(two_pi * 997 * t) + 0.125 * std::sin(two_pi * 431 * t));
            check(std::abs(one->sample(frame) - expected) < 1e-6 && one->sample(frame) == two->sample(frame),
                  "Explicit mixer matches an independent sum");
        }
    }
}
class Algorithm final : public NodeT<1, 1>
{
  public:
    Algorithm()
        : NodeT({PortSpec(SampleRate::Hz16000, 1, SampleFormat::S16LE)},
                {PortSpec(SampleRate::Hz16000, 1, SampleFormat::S16LE)})
    {
    }
    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0].byte_view();
        const auto output = context.outputs[0].byte_view();
        for (std::size_t i = 0; i < input.size(); i += 2)
        {
            const auto word = static_cast<unsigned int>(input[i]) | (static_cast<unsigned int>(input[i + 1]) << 8);
            const auto sample = (word >= 32768 ? static_cast<int>(word) - 65536 : static_cast<int>(word)) / 2;
            const auto encoded = static_cast<unsigned int>(sample);
            output[i] = static_cast<std::uint8_t>(encoded);
            output[i + 1] = static_cast<std::uint8_t>(encoded >> 8);
        }
        frames = context.inputs[0].frames();
        ++calls;
    }
    std::size_t frames = 0, calls = 0;
};
void algorithm_chain()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const PortSpec bus(rate, 1, SampleFormat::F32P), pcm(SampleRate::Hz16000, 1, SampleFormat::S16LE);
    const auto tone = engine.emplace_node<Tone>(rate, 1, 1000, 0.25f);
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 1);
    const auto down = engine.emplace_node<Convert>(bus, pcm);
    const auto algorithm = engine.emplace_node<Algorithm>();
    const auto up = engine.emplace_node<Convert>(pcm, bus);
    const auto sink = engine.emplace_node<PacketSink>(bus);
    expect_error(ErrorCode::FormatMismatch, [&] { engine.connect(OPORT(mix, 0), IPORT(algorithm, 0)); });
    engine.connect(OPORT(tone, 0), IPORT(mix, 0));
    engine.connect(OPORT(mix, 0), IPORT(down, 0));
    engine.connect(OPORT(down, 0), IPORT(algorithm, 0));
    engine.connect(OPORT(algorithm, 0), IPORT(up, 0));
    engine.connect(OPORT(up, 0), IPORT(sink, 0));
    engine.commit();
    auto *output = engine.node<PacketSink>(sink);
    auto *algo = engine.node<Algorithm>(algorithm);
    clock.start();
    for (std::size_t tick = 0; tick < 20; ++tick)
    {
        clock.advance();
        check(algo->frames == 80 && output->frames == 240, "Every port receives its own rate's frame count");
        if (tick > 4)
            for (std::size_t f = 0; f < 240; ++f)
            {
                const auto seconds = static_cast<double>(tick * 240 + f) / 48000 - 48.0 / 48000 - 48.0 / 16000;
                check(std::abs(output->sample(f) - 0.125 * std::sin(two_pi * 1000 * seconds)) < 0.003,
                      "Algorithm chain preserves time through two explicit converters");
            }
    }
    check(algo->calls == 20 && engine.tick_position() == 20, "The engine only advances base periods");
}
void silence_and_pruning()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source =
        engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; });
    const auto sink = engine.emplace_node<PacketSink>(PortSpec(rate, 1, SampleFormat::F32P));
    engine.commit();
    clock.start();
    clock.advance();
    auto *input = engine.node<FloatSource>(source);
    auto *output = engine.node<PacketSink>(sink);
    check(input->calls == 0 && input->prepares == 0 && output->sample(0) == 0,
          "Orphan sources are pruned and disconnected inputs are silence");
    const auto conn = engine.connect(OPORT(source, 0), IPORT(sink, 0));
    clock.commit();
    const auto connected_calls = input->calls;
    clock.advance();
    check(input->calls == connected_calls + 1 && output->sample(0) == 1,
          "Connecting a sink activates its upstream graph");
    engine.disconnect(conn);
    clock.commit();
    const auto calls = input->calls;
    clock.advance();
    check(input->calls == calls && output->sample(0) == 0, "Disconnecting removes the source and exposes a silent buffer");
}
void shared_silence()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const PortSpec planar(rate, 1, SampleFormat::F32P), pcm(rate, 1, SampleFormat::S16LE);
    const PortSpec wide_planar(SampleRate::Hz44100, 2, SampleFormat::F32P);
    const PortSpec wide_pcm(SampleRate::Hz44100, 3, SampleFormat::S24_3LE);
    const auto source =
        engine.emplace_node<FloatSource>(wide_planar.sample_rate, 2,
                                                 [](std::uint64_t, std::size_t ch) { return static_cast<float>(ch + 1); });
    auto add_sink = [&](PortSpec spec) {
        return engine.node<PacketSink>(engine.emplace_node<PacketSink>(spec));
    };
    const auto *a = add_sink(planar), *b = add_sink(wide_planar), *c = add_sink(pcm), *d = add_sink(wide_pcm);
    const auto connected = engine.emplace_node<PacketSink>(wide_planar);
    const auto *output = engine.node<PacketSink>(connected);
    engine.connect(OPORT(source, 0), IPORT(connected, 0));
    engine.commit();
    clock.start();
    for (std::uint64_t tick = 0; tick < 2; ++tick)
    {
        clock.advance();
        check(a->address == b->address && c->address == d->address,
              "Unconnected inputs share one zero region per storage type");
        check(a->address != c->address && b->address != output->address,
              "The two storage types and the writable output have disjoint buffers");
        for (const auto *sink : {a, b, c, d})
        {
            const auto spec = sink->inputs()[0];
            const auto bytes = sink->bytes();
            check(bytes.size() == spec.frames(period, tick) * spec.channels * sample_bytes(spec.format) &&
                      std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
                  "Shared silence exposes each input's own tick size and format");
        }
        check(output->sample(0, 0) == 1 && output->sample(0, 1) == 2,
              "A fractional planar output packs both channels for each tick");
    }
}
void format_validation()
{
    Engine engine(period);
    for (const auto spec : {PortSpec(static_cast<SampleRate>(0), 1, SampleFormat::F32P),
                            PortSpec(static_cast<SampleRate>(48001), 1, SampleFormat::F32P),
                            PortSpec(static_cast<SampleRate>(96001), 1, SampleFormat::F32P),
                            PortSpec(rate, 0, SampleFormat::F32P),
                            PortSpec(rate, 1, static_cast<SampleFormat>(99))})
    {
        expect_error(ErrorCode::InvalidArg, [&] { engine.emplace_node<Capture>(spec); });
        expect_error(ErrorCode::InvalidArg, [&] { engine.emplace_node<Playback>(spec); });
        expect_error(ErrorCode::InvalidArg, [&] { engine.emplace_node<PacketSink>(spec); });
    }
    const auto source =
        engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.0f; });
    for (const auto spec : {PortSpec(rate, 2, SampleFormat::F32P),
                            PortSpec(SampleRate::Hz16000, 1, SampleFormat::F32P),
                            PortSpec(rate, 1, SampleFormat::F32LE),
                            PortSpec(rate, 1, SampleFormat::S16LE)})
    {
        const auto sink = engine.emplace_node<PacketSink>(spec);
        expect_error(ErrorCode::FormatMismatch, [&] { engine.connect(OPORT(source, 0), IPORT(sink, 0)); });
    }
    const auto mixer = engine.emplace_node<Mixer>(rate, 1, 1);
    const auto raw = engine.emplace_node<Capture>(PortSpec(rate, 1, SampleFormat::S16LE));
    expect_error(ErrorCode::FormatMismatch, [&] { engine.connect(OPORT(raw, 0), IPORT(mixer, 0)); });
}
void channel_split_same_tick()
{
    constexpr auto split_rate = SampleRate::Hz44100;
    Engine engine(period);
    ClockedGraph clock(engine);
    std::array<float, 32> left{}, right{};
    for (std::size_t ch = 0; ch < 4; ++ch)
    {
        left[ch * 8 + ch] = 1;
        right[ch * 8 + ch + 4] = 1;
    }
    const auto source =
        engine.emplace_node<FloatSource>(split_rate, 8, [](std::uint64_t frame, std::size_t ch)
                                                 { return frame % 7 ? 0.0f : static_cast<float>(ch + 1); });
    const auto other = engine.emplace_node<FloatSource>(split_rate, 4, [](std::uint64_t, std::size_t ch)
                                                                { return static_cast<float>(ch + 1) * 0.125f; });
    const auto a = engine.emplace_node<Matrix>(split_rate, 8, 4, View<const float>{left.data(), left.size()});
    const auto b =
        engine.emplace_node<Matrix>(split_rate, 8, 4, View<const float>{right.data(), right.size()});
    const auto mix = engine.emplace_node<Mixer>(split_rate, 4, 2);
    const auto first = engine.emplace_node<PacketSink>(PortSpec(split_rate, 4, SampleFormat::F32P));
    const auto second = engine.emplace_node<PacketSink>(PortSpec(split_rate, 4, SampleFormat::F32P));
    engine.connect(OPORT(source, 0), IPORT(a, 0));
    engine.connect(OPORT(source, 0), IPORT(b, 0));
    engine.connect(OPORT(a, 0), IPORT(first, 0));
    engine.connect(OPORT(b, 0), IPORT(mix, 0));
    engine.connect(OPORT(other, 0), IPORT(mix, 1));
    engine.connect(OPORT(mix, 0), IPORT(second, 0));
    engine.commit();
    const auto *input = engine.node<FloatSource>(source);
    const auto *out_a = engine.node<PacketSink>(first);
    const auto *out_b = engine.node<PacketSink>(second);
    std::size_t start = 0;
    clock.start();
    for (std::size_t tick = 0; tick < 8; ++tick)
    {
        clock.advance();
        const std::size_t frames = tick % 2 ? 220 : 221;
        check(out_a->frames == frames && out_b->frames == frames && input->calls == tick + 1,
              "Shared upstream views keep fractional block sizes and render once per tick");
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::size_t ch = 0; ch < 4; ++ch)
            {
                const bool pulse = (start + frame) % 7 == 0;
                check(out_a->sample(frame, ch) == (pulse ? static_cast<float>(ch + 1) : 0.0f) &&
                          out_b->sample(frame, ch) ==
                              (pulse ? static_cast<float>(ch + 5) : 0.0f) + static_cast<float>(ch + 1) * 0.125f,
                      "Split channels and mixed branches retain sample positions within the same tick");
            }
        start += frames;
    }
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"explicit_mix_and_fanout", explicit_mix_and_fanout},
                      {"algorithm_chain", algorithm_chain},
                      {"silence_and_pruning", silence_and_pruning},
                      {"shared_silence", shared_silence},
                      {"format_validation", format_validation},
                      {"channel_split_same_tick", channel_split_same_tick}});
}
