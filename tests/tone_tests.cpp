#include "plugins/tone.h"
#include "support/audio_fixture.h"
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void tone_timeline()
{
    Engine engine(period);
    ManualClock clock(engine);
    const PortSpec spec(SampleRate::Hz44100, 2);
    const auto source = checked(engine.emplace_node<Tone>(spec.sample_rate, 2, 997, 0.25f));
    const auto sink = checked(engine.emplace_node<PacketSink>(spec));
    checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
    auto *out = checked(engine.node<PacketSink>(sink));
    for (std::uint64_t tick = 0; tick < 5; ++tick)
    {
        checked(clock.tick());
        for (std::size_t frame = 0; frame < out->frames; ++frame)
        {
            const auto expected = 0.25 * std::sin(two_pi * 997 * (spec.frame_position(period, tick) + frame) / 44100);
            check(std::abs(out->sample(frame) - expected) < 1e-7 && out->sample(frame, 0) == out->sample(frame, 1),
                  "Tone uses each port's continuous sample timeline");
        }
    }
    checked(clock.reset());
    checked(clock.tick());
    check(out->sample(0) == 0, "Reset restarts the tone at zero phase");
}
void tone_validation()
{
    Tone invalid(rate, 1, 24000);
    expect_error(ErrorCode::InvalidArg, invalid.prepare(period));
    Tone negative(rate, 1, -1);
    expect_error(ErrorCode::InvalidArg, negative.prepare(period));
    Tone silent(rate, 1, 0, 0);
    checked(silent.prepare(period));
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"tone_timeline", tone_timeline}, {"tone_validation", tone_validation}});
}
