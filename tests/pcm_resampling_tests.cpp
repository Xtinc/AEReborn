#include "support/audio_fixture.h"
namespace
{
using namespace aereborn;
using namespace aereborn::test;
std::vector<float> resample(SampleRate from, SampleRate to, double frequency, BlockPeriod block_period = period)
{
    Engine engine(block_period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<FloatSource>(
        from, 2,
        [=](std::uint64_t frame, std::size_t ch)
        {
            return static_cast<float>(std::sin(two_pi * frequency * frame / static_cast<std::uint32_t>(from)) *
                                      (ch ? -0.5 : 1));
        });
    const PortSpec input(from, 2, SampleFormat::F32P), output(to, 2, SampleFormat::F32P);
    const auto converter = engine.emplace_node<Convert>(input, output);
    const auto sink = engine.emplace_node<PacketSink>(output);
    engine.connect(OPORT(source, 0), IPORT(converter, 0));
    engine.connect(OPORT(converter, 0), IPORT(sink, 0));
    engine.commit();
    auto *out = engine.node<PacketSink>(sink);
    std::vector<float> result;
    clock.start();
    for (std::size_t tick = 0; tick < 40; ++tick)
    {
        clock.advance();
        check(out->frames == output.frames(block_period, tick),
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
    engine.reset();
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
    for (const auto block_period : {BlockPeriod::Ms02, period})
    {
        for (const auto &pair : pairs)
        {
            const auto from = static_cast<std::uint32_t>(pair[0]), to = static_cast<std::uint32_t>(pair[1]);
            const auto samples = resample(pair[0], pair[1], 1000, block_period);
            for (std::size_t i = 96 * to / from + 1; i < samples.size(); ++i)
            {
                const auto expected = std::sin(two_pi * 1000 * (static_cast<double>(i) / to - 48.0 / from));
                check(std::abs(samples[i] - expected) < 0.01, "Resampling remains phase-continuous after the FIR delay");
            }
        }
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
    const auto source = engine.emplace_node<FloatSource>(input_rate, 1, [](std::uint64_t frame, std::size_t) {
        return static_cast<float>(std::sin(two_pi * 997 * frame / 11025));
    });
    const PortSpec input(input_rate, 1, SampleFormat::F32P), output(rate, 1, SampleFormat::F32P);
    const auto original = engine.emplace_node<Convert>(input, output);
    const auto sink = engine.emplace_node<PacketSink>(output);
    engine.connect(OPORT(source, 0), IPORT(original, 0));
    engine.connect(OPORT(original, 0), IPORT(sink, 0));
    engine.commit();
    clock.start(); // Submit while the first, longer input block is in flight.
    const auto added = engine.emplace_node<Convert>(input, output);
    const auto monitor = engine.emplace_node<PacketSink>(output);
    engine.connect(OPORT(source, 0), IPORT(added, 0));
    engine.connect(OPORT(added, 0), IPORT(monitor, 0));
    clock.commit();
    auto *a = engine.node<PacketSink>(sink);
    auto *b = engine.node<PacketSink>(monitor);
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
