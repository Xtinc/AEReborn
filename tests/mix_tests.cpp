#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void matrix_mapping()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto src = checked(engine.emplace_node<FloatSource>(rate, 3, [](std::uint64_t, std::size_t ch)
                                                              { return static_cast<float>(ch + 1); }));
    const auto matrix =
        checked(engine.emplace_node<Matrix>(rate, 3, 2, std::initializer_list<float>{1, 0, -0.5f, 0, 2, 1}, 0.5f));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 2)));
    checked(engine.connect({src}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.commit());
    clock.start();
    clock.advance();
    const auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0, 0) == -0.25f && out->sample(0, 1) == 3.5f,
          "Matrix rows map and combine source channels with gain");
}
void matrix_validation()
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect_exception<std::invalid_argument>([] { Matrix(rate, 0); });
    expect_exception<std::invalid_argument>([] { Matrix(rate, 2, 1, {1}); });
    expect_exception<std::invalid_argument>([&] { Matrix(rate, 1, 1, {nan}); });
    expect_exception<std::length_error>(
        []
        {
            const auto n = std::numeric_limits<std::size_t>::max();
            Matrix(rate, n, n, {});
        });
    Matrix matrix(rate, 1);
    expect_error(ErrorCode::InvalidArg, matrix.set_gain(nan));
}
void check_gain_ramp(bool identity)
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = checked(engine.emplace_node<FloatSource>(
        rate, 1, [](std::uint64_t frame, std::size_t)
        { return frame % 240 == 119 ? std::numeric_limits<float>::quiet_NaN() : 1.0f; }));
    const auto matrix = checked(identity ? engine.emplace_node<Matrix>(rate, 1)
                                         : engine.emplace_node<Matrix>(rate, 1, 1, std::initializer_list<float>{2}));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({source}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.commit());
    clock.start();
    clock.advance();
    auto *gain = checked(engine.node<Matrix>(matrix));
    auto *out = checked(engine.node<PacketSink>(sink));
    checked(gain->set_gain(-1));
    clock.advance();
    const auto coefficient = identity ? 1.0f : 2.0f;
    check(std::abs(out->sample(0) - coefficient * (1.0f - 2.0f / 240)) < 1e-6 && out->sample(119) == 0 &&
              out->sample(239) == -coefficient,
          "Gain ramps have exact endpoints and skip an exactly zero crossing");
    checked(gain->set_gain(0));
    clock.advance();
    clock.advance();
    for (std::size_t frame = 0; frame < 240; ++frame)
        check(out->sample(frame) == 0, "Zero gain suppresses non-finite inputs");
    checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    clock.commit();
    clock.advance();
    check(out->sample(0) == 0, "A graph rebuild preserves the matrix target and current gain");
}
void gain_ramp_and_zero()
{
    check_gain_ramp(true);
    check_gain_ramp(false);
}
void mixer_mute()
{
    expect_exception<std::invalid_argument>([] { Mixer(rate, 1, 0); });
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto a = checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; }));
    const auto b = checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 2.0f; }));
    const auto mix = checked(engine.emplace_node<Mixer>(rate, 1, 2));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({a}, {mix, 0}));
    checked(engine.connect({b}, {mix, 1}));
    checked(engine.connect({mix}, {sink}));
    checked(engine.commit());
    clock.start();
    clock.advance();
    auto *mixer = checked(engine.node<Mixer>(mix));
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0) == 3, "Both mixer inputs contribute");
    expect_error(ErrorCode::InvalidPort, mixer->set_muted(2, true));
    checked(mixer->set_muted(0, true));
    clock.advance();
    check(std::abs(out->sample(0) - (3.0f - 1.0f / 240)) < 1e-6 && out->sample(239) == 2,
          "An input mute ramps only that source");
    clock.advance();
    check(out->sample(0) == 2, "Mute remains stable");
    checked(mixer->set_muted(0, false));
    clock.advance();
    check(out->sample(239) == 3, "Unmute restores the selected source");
}
void empty_block_ramps()
{
    const PortSpec spec(rate, 2);
    Matrix gain(rate, 2);
    Matrix mapped(rate, 2, 2, {1, 0, 0, 1});
    Mixer mixer(rate, 2, 1);
    checked(gain.prepare(period));
    checked(mapped.prepare(period));
    checked(mixer.prepare(period));
    checked(gain.set_gain(0));
    checked(mapped.set_gain(0));
    checked(mixer.set_muted(0, true));

    Node *plugins[]{&gain, &mapped, &mixer};
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const float samples[]{1.0f, nan, 2.0f, nan};
    float result[4]{};
    for (auto *plugin : plugins)
    {
        const IPortBlock empty_input{reinterpret_cast<const std::uint8_t *>(samples), spec, 0};
        const OPortBlock empty_output{reinterpret_cast<std::uint8_t *>(result), spec, 0};
        plugin->process({period, 0, {&empty_input, 1}, {&empty_output, 1}});
        const IPortBlock input{reinterpret_cast<const std::uint8_t *>(samples), spec, 2};
        const OPortBlock output{reinterpret_cast<std::uint8_t *>(result), spec, 2};
        plugin->process({period, 1, {&input, 1}, {&output, 1}});
        check(result[0] == 0.5f && result[1] == 0 && result[2] == 1.0f && result[3] == 0,
              "An empty block preserves both channels' ramp and the mute endpoint suppresses NaN");
    }
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"matrix_mapping", matrix_mapping},
                      {"matrix_validation", matrix_validation},
                      {"gain_ramp_and_zero", gain_ramp_and_zero},
                      {"mixer_mute", mixer_mute},
                      {"empty_block_ramps", empty_block_ramps}});
}
