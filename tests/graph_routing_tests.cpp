#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "plugins/tone.h"
#include "support/audio_fixture.h"
#include <array>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void explicit_mix_and_fanout()
{
    Engine engine(period);
    ManualClock clock(engine);
    const auto a = checked(engine.emplace_node<Tone>(rate, 2, 997, 0.25f));
    const auto b = checked(engine.emplace_node<Tone>(rate, 2, 431, 0.125f));
    const auto mix = checked(engine.emplace_node<Mixer>(rate, 2, 2));
    const auto matrix =
        checked(engine.emplace_node<Matrix>(rate, 2, 2, std::initializer_list<float>{0, 1, 1, 0}, 0.5f));
    const auto first = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 2)));
    const auto second = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 2)));
    checked(engine.connect({a}, {mix, 0}));
    checked(engine.connect({b}, {mix, 1}));
    checked(engine.connect({mix}, {matrix}));
    checked(engine.connect({matrix}, {first}));
    checked(engine.connect({matrix}, {second}));
    checked(engine.prepare());
    auto *one = checked(engine.node<PacketSink>(first));
    auto *two = checked(engine.node<PacketSink>(second));
    for (std::size_t tick = 0; tick < 5; ++tick)
    {
        checked(clock.tick());
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
        : NodeT({{PortSpec(SampleRate::Hz16000, 1, Format::S16LE)}},
                {{PortSpec(SampleRate::Hz16000, 1, Format::S16LE)}})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto input = context.inputs[0].bytes();
        const auto output = context.outputs[0].bytes();
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
    ManualClock clock(engine);
    const PortSpec bus(rate, 1), pcm(SampleRate::Hz16000, 1, Format::S16LE);
    const auto tone = checked(engine.emplace_node<Tone>(rate, 1, 1000, 0.25f));
    const auto mix = checked(engine.emplace_node<Mixer>(rate, 1, 1));
    const auto down = checked(engine.emplace_node<Convert>(bus, pcm));
    const auto algorithm = checked(engine.emplace_node<Algorithm>());
    const auto up = checked(engine.emplace_node<Convert>(pcm, bus));
    const auto sink = checked(engine.emplace_node<PacketSink>(bus));
    expect_error(ErrorCode::FormatMismatch, engine.connect({mix}, {algorithm}));
    checked(engine.connect({tone}, {mix}));
    checked(engine.connect({mix}, {down}));
    checked(engine.connect({down}, {algorithm}));
    checked(engine.connect({algorithm}, {up}));
    checked(engine.connect({up}, {sink}));
    checked(engine.prepare());
    auto *output = checked(engine.node<PacketSink>(sink));
    auto *algo = checked(engine.node<Algorithm>(algorithm));
    for (std::size_t tick = 0; tick < 20; ++tick)
    {
        checked(clock.tick());
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
    ManualClock clock(engine);
    const auto source =
        checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; }));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.prepare());
    checked(clock.tick());
    auto *input = checked(engine.node<FloatSource>(source));
    auto *output = checked(engine.node<PacketSink>(sink));
    check(input->calls == 0 && input->prepares == 0 && output->sample(0) == 0,
          "Orphan sources are pruned and disconnected inputs are silence");
    const auto conn = checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
    checked(clock.tick());
    check(input->calls == 1 && output->sample(0) == 1, "Connecting a sink activates its upstream graph");
    checked(engine.disconnect(conn));
    checked(engine.prepare());
    checked(clock.tick());
    check(input->calls == 1 && output->sample(0) == 0, "Disconnecting removes the source and exposes a silent buffer");
}
void shared_silence()
{
    Engine engine(period);
    ManualClock clock(engine);
    const PortSpec planar(rate, 1), pcm(rate, 1, Format::S16LE);
    const PortSpec wide_planar(SampleRate::Hz44100, 2), wide_pcm(SampleRate::Hz44100, 3, Format::S24_3LE);
    const auto source =
        checked(engine.emplace_node<FloatSource>(wide_planar.sample_rate, 2,
                                                 [](std::uint64_t, std::size_t ch) { return static_cast<float>(ch + 1); }));
    auto add_sink = [&](PortSpec spec) {
        return checked(engine.node<PacketSink>(checked(engine.emplace_node<PacketSink>(spec))));
    };
    const auto *a = add_sink(planar), *b = add_sink(wide_planar), *c = add_sink(pcm), *d = add_sink(wide_pcm);
    const auto connected = checked(engine.emplace_node<PacketSink>(wide_planar));
    const auto *output = checked(engine.node<PacketSink>(connected));
    checked(engine.connect({source}, {connected}));
    checked(engine.prepare());
    for (std::uint64_t tick = 0; tick < 2; ++tick)
    {
        checked(clock.tick());
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
    for (const auto spec : {PortSpec(static_cast<SampleRate>(0), 1), PortSpec(static_cast<SampleRate>(48001), 1),
                            PortSpec(static_cast<SampleRate>(96001), 1),
                            PortSpec(rate, 0), PortSpec(rate, 1, static_cast<Format>(99))})
    {
        expect_exception<std::invalid_argument>([&] { engine.emplace_node<Capture>(spec); });
        expect_exception<std::invalid_argument>([&] { engine.emplace_node<Playback>(spec); });
        expect_error(ErrorCode::InvalidArg, engine.emplace_node<PacketSink>(spec));
    }
    const auto source =
        checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.0f; }));
    for (const auto spec : {PortSpec(rate, 2), PortSpec(SampleRate::Hz16000, 1), PortSpec(rate, 1, Format::F32LE),
                            PortSpec(rate, 1, Format::S16LE)})
    {
        const auto sink = checked(engine.emplace_node<PacketSink>(spec));
        expect_error(ErrorCode::FormatMismatch, engine.connect({source}, {sink}));
    }
    const auto mixer = checked(engine.emplace_node<Mixer>(rate, 1, 1));
    const auto raw = checked(engine.emplace_node<Capture>(PortSpec(rate, 1, Format::S16LE)));
    expect_error(ErrorCode::FormatMismatch, engine.connect({raw}, {mixer}));
}
void channel_split_same_tick()
{
    constexpr auto split_rate = SampleRate::Hz44100;
    Engine engine(period);
    ManualClock clock(engine);
    std::array<float, 32> left{}, right{};
    for (std::size_t ch = 0; ch < 4; ++ch)
    {
        left[ch * 8 + ch] = 1;
        right[ch * 8 + ch + 4] = 1;
    }
    const auto source =
        checked(engine.emplace_node<FloatSource>(split_rate, 8, [](std::uint64_t frame, std::size_t ch)
                                                 { return frame % 7 ? 0.0f : static_cast<float>(ch + 1); }));
    const auto other = checked(engine.emplace_node<FloatSource>(split_rate, 4, [](std::uint64_t, std::size_t ch)
                                                                { return static_cast<float>(ch + 1) * 0.125f; }));
    const auto a = checked(engine.emplace_node<Matrix>(split_rate, 8, 4, View<const float>{left.data(), left.size()}));
    const auto b =
        checked(engine.emplace_node<Matrix>(split_rate, 8, 4, View<const float>{right.data(), right.size()}));
    const auto mix = checked(engine.emplace_node<Mixer>(split_rate, 4, 2));
    const auto first = checked(engine.emplace_node<PacketSink>(PortSpec(split_rate, 4)));
    const auto second = checked(engine.emplace_node<PacketSink>(PortSpec(split_rate, 4)));
    checked(engine.connect({source}, {a}));
    checked(engine.connect({source}, {b}));
    checked(engine.connect({a}, {first}));
    checked(engine.connect({b}, {mix, 0}));
    checked(engine.connect({other}, {mix, 1}));
    checked(engine.connect({mix}, {second}));
    checked(engine.prepare());
    const auto *input = checked(engine.node<FloatSource>(source));
    const auto *out_a = checked(engine.node<PacketSink>(first));
    const auto *out_b = checked(engine.node<PacketSink>(second));
    std::size_t start = 0;
    for (std::size_t tick = 0; tick < 8; ++tick)
    {
        checked(clock.tick());
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
