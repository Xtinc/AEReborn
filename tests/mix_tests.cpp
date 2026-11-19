#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
template <class Exception, class F> void throws(F f)
{
    try
    {
        f();
    }
    catch (const Exception &)
    {
        return;
    }
    check(false, "Expected exception");
}
void matrix_mapping()
{
    Engine engine(period);
    ManualClock clock(engine);
    const auto src = checked(engine.emplace_node<FloatSource>(rate, 3, [](std::uint64_t, std::size_t ch)
                                                              { return static_cast<float>(ch + 1); }));
    const auto matrix =
        checked(engine.emplace_node<Matrix>(rate, 3, 2, std::initializer_list<float>{1, 0, -0.5f, 0, 2, 1}, 0.5f));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 2)));
    checked(engine.connect({src}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.prepare());
    checked(clock.tick());
    const auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0, 0) == -0.25f && out->sample(0, 1) == 3.5f,
          "Matrix rows map and combine source channels with gain");
}
void matrix_validation()
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    throws<std::invalid_argument>([] { Matrix(rate, 0); });
    throws<std::invalid_argument>([] { Matrix(rate, 2, 1, {1}); });
    throws<std::invalid_argument>([&] { Matrix(rate, 1, 1, {nan}); });
    throws<std::length_error>(
        []
        {
            const auto n = std::numeric_limits<std::size_t>::max();
            Matrix(rate, n, n, {});
        });
    Matrix matrix(rate, 1);
    expect_error(ErrorCode::InvalidArg, matrix.set_gain(nan));
}
void gain_ramp_and_zero()
{
    Engine engine(period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<FloatSource>(
        rate, 1, [](std::uint64_t frame, std::size_t)
        { return frame % 240 == 119 ? std::numeric_limits<float>::quiet_NaN() : 1.0f; }));
    const auto matrix = checked(engine.emplace_node<Matrix>(rate, 1));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({source}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.prepare());
    checked(clock.tick());
    auto *gain = checked(engine.node<Matrix>(matrix));
    auto *out = checked(engine.node<PacketSink>(sink));
    checked(gain->set_gain(-1));
    checked(clock.tick());
    check(std::abs(out->sample(0) - (1.0f - 2.0f / 240)) < 1e-6 && out->sample(119) == 0 && out->sample(239) == -1,
          "Gain ramps have exact endpoints and skip an exactly zero crossing");
    checked(gain->set_gain(0));
    checked(clock.tick());
    checked(clock.tick());
    for (std::size_t frame = 0; frame < 240; ++frame)
        check(out->sample(frame) == 0, "Zero gain suppresses non-finite inputs");
    checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.prepare());
    checked(clock.tick());
    check(out->sample(0) == 0, "A graph rebuild preserves the matrix target and current gain");
}
void mixer_mute()
{
    Engine engine(period);
    ManualClock clock(engine);
    const auto a = checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; }));
    const auto b = checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 2.0f; }));
    const auto mix = checked(engine.emplace_node<Mixer>(rate, 1, 2));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({a}, {mix, 0}));
    checked(engine.connect({b}, {mix, 1}));
    checked(engine.connect({mix}, {sink}));
    checked(engine.prepare());
    checked(clock.tick());
    auto *mixer = checked(engine.node<Mixer>(mix));
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0) == 3, "Both mixer inputs contribute");
    expect_error(ErrorCode::InvalidPort, mixer->set_muted(2, true));
    checked(mixer->set_muted(0, true));
    checked(clock.tick());
    check(std::abs(out->sample(0) - (3.0f - 1.0f / 240)) < 1e-6 && out->sample(239) == 2,
          "An input mute ramps only that source");
    checked(clock.tick());
    check(out->sample(0) == 2, "Mute remains stable");
    checked(mixer->set_muted(0, false));
    checked(clock.tick());
    check(out->sample(239) == 3, "Unmute restores the selected source");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"matrix_mapping", matrix_mapping},
                      {"matrix_validation", matrix_validation},
                      {"gain_ramp_and_zero", gain_ramp_and_zero},
                      {"mixer_mute", mixer_mute}});
}
