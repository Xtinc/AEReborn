#include "plugins/tone.h"
#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void tone_timeline()
{
    constexpr auto tone_period = BlockPeriod::Ms10;
    Engine engine(tone_period);
    ManualClock clock(engine);
    const PortSpec spec(SampleRate::Hz11025, 2);
    const auto source = checked(engine.emplace_node<Tone>(spec.sample_rate, 2, 997, 0.25f));
    const auto sink = checked(engine.emplace_node<PacketSink>(spec));
    checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
    auto *out = checked(engine.node<PacketSink>(sink));
    for (std::uint64_t tick = 0; tick < 5; ++tick)
    {
        checked(clock.tick());
        check(out->frames == spec.frames(tone_period, tick), "Tone follows the configured fractional block period");
        for (std::size_t frame = 0; frame < out->frames; ++frame)
        {
            const auto expected =
                0.25 * std::sin(two_pi * 997 * (spec.frame_position(tone_period, tick) + frame) / 11025);
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
    expect_exception<std::invalid_argument>([] { Tone invalid(rate, 1, 24000); });
    expect_exception<std::invalid_argument>([] { Tone invalid(rate, 1, -1); });
    expect_exception<std::invalid_argument>([] { Tone invalid(rate, 1, 440, std::numeric_limits<float>::infinity()); });
    expect_exception<std::invalid_argument>([] { Tone invalid(rate, 0); });
    Tone silent(rate, 1, 0, 0);
    expect_error(ErrorCode::InvalidArg, silent.prepare(static_cast<BlockPeriod>(0)));
    checked(silent.prepare(period));
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"tone_timeline", tone_timeline}, {"tone_validation", tone_validation}});
}
