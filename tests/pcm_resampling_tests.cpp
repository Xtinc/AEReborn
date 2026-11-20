#include "support/audio_fixture.h"
namespace
{
using namespace aereborn;
using namespace aereborn::test;
std::vector<float> resample(SampleRate from, SampleRate to, double frequency)
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = checked(engine.emplace_node<FloatSource>(
        from, 2,
        [=](std::uint64_t frame, std::size_t ch)
        {
            return static_cast<float>(std::sin(two_pi * frequency * frame / static_cast<std::uint32_t>(from)) *
                                      (ch ? -0.5 : 1));
        }));
    const auto converter = checked(engine.emplace_node<Convert>(PortSpec(from, 2), PortSpec(to, 2)));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(to, 2)));
    checked(engine.connect({source}, {converter}));
    checked(engine.connect({converter}, {sink}));
    checked(engine.commit());
    auto *out = checked(engine.node<PacketSink>(sink));
    std::vector<float> result;
    clock.start();
    for (std::size_t tick = 0; tick < 40; ++tick)
    {
        clock.advance();
        check(out->frames == PortSpec(to, 2).frames(period, tick),
              "Fractional output sizes follow the engine timeline");
        for (std::size_t frame = 0; frame < out->frames; ++frame)
        {
            const auto value = out->sample(frame);
            check(std::abs(out->sample(frame, 1) + 0.5f * value) < 1e-6,
                  "Each channel has independent resampler history");
            result.push_back(value);
        }
    }
    clock.stop();
    checked(engine.reset());
    clock.start();
    clock.advance();
    for (std::size_t frame = 0; frame < out->frames; ++frame)
        check(out->sample(frame) == result[frame], "Reset clears the filter history and restarts the shared timeline");
    return result;
}
void rate_continuity()
{
    const SampleRate pairs[][2] = {{rate, SampleRate::Hz44100},
                                   {SampleRate::Hz44100, rate},
                                   {rate, SampleRate::Hz11025},
                                   {SampleRate::Hz11025, rate},
                                   {SampleRate::Hz8000, SampleRate::Hz96000},
                                   {SampleRate::Hz96000, SampleRate::Hz8000}};
    for (const auto &pair : pairs)
    {
        const auto from = static_cast<std::uint32_t>(pair[0]), to = static_cast<std::uint32_t>(pair[1]);
        const auto samples = resample(pair[0], pair[1], 1000);
        for (std::size_t i = 96 * to / from + 1; i < samples.size(); ++i)
            check(std::abs(samples[i] - std::sin(two_pi * 1000 * (static_cast<double>(i) / to - 48.0 / from))) < 0.01,
                  "Resampling remains phase-continuous after the FIR delay");
    }
}
void antialias_filter()
{
    const auto pass = resample(rate, SampleRate::Hz16000, 1000), stop = resample(rate, SampleRate::Hz16000, 12000);
    const auto rms = [](const std::vector<float> &x)
    {
        double e = 0;
        for (std::size_t i = 96; i < x.size(); ++i)
            e += x[i] * x[i];
        return std::sqrt(e / (x.size() - 96));
    };
    check(rms(pass) > 0.69 && rms(stop) < rms(pass) * 0.01,
          "Downsampling suppresses frequencies above the target Nyquist limit");
}
void late_join()
{
    Engine engine(BlockPeriod::Ms10);
    ClockedGraph clock(engine);
    constexpr auto input_rate = SampleRate::Hz11025;
    const auto source = checked(
        engine.emplace_node<FloatSource>(input_rate, 1, [](std::uint64_t frame, std::size_t)
                                         { return static_cast<float>(std::sin(two_pi * 997 * frame / 11025)); }));
    const auto original = checked(engine.emplace_node<Convert>(PortSpec(input_rate, 1), PortSpec(rate, 1)));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({source}, {original}));
    checked(engine.connect({original}, {sink}));
    checked(engine.commit());
    clock.start(); // Submit while the first, longer input block is in flight.
    const auto added = checked(engine.emplace_node<Convert>(PortSpec(input_rate, 1), PortSpec(rate, 1)));
    const auto monitor = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({source}, {added}));
    checked(engine.connect({added}, {monitor}));
    clock.commit();
    auto *a = checked(engine.node<PacketSink>(sink));
    auto *b = checked(engine.node<PacketSink>(monitor));
    clock.advance();
    clock.advance();
    check(a->frames == 480 && b->frames == 480, "Resamplers use the configured 10 ms block period");
    for (std::size_t frame = 0; frame < a->frames; ++frame)
        check(a->sample(frame) == b->sample(frame),
              "New converters align fractional phase to the existing graph timeline");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(
        argc, argv,
        {{"rate_continuity", rate_continuity}, {"antialias_filter", antialias_filter}, {"late_join", late_join}});
}
