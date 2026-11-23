#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include <cstdint>
#include <cstring>
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void matrix_identity_bits()
{
    const PortSpec spec(rate, 2, SampleFormat::F32P);
    Matrix matrix(rate, 2);
    matrix.prepare(period);

    // Distinct planar channels with signed zero, subnormals and NaN payloads.
    const std::uint32_t words[]{0x80000000u, 0x00000001u, 0x7f800001u, 0x80000001u, 0x7fc12345u, 0x3f800000u};
    float samples[6], result[6]{};
    std::memcpy(samples, words, sizeof(samples));
    const IBBlock input{reinterpret_cast<const std::uint8_t *>(samples), spec, 3};
    const OBBlock output{reinterpret_cast<std::uint8_t *>(result), spec, 3};
    matrix.process({period, 0, {&input, 1}, {&output, 1}});
    check(std::memcmp(result, words, sizeof(result)) == 0, "Unity gain preserves every sample bit");
}
void matrix_mapping()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto src = engine.emplace_node<FloatSource>(rate, 3, [](std::uint64_t, std::size_t ch)
                                                              { return static_cast<float>(ch + 1); });
    const auto matrix =
        engine.emplace_node<Matrix>(rate, 3, 2, std::initializer_list<float>{1, 0, -0.5f, 0, 2, 1}, 0.5f);
    const auto sink = engine.emplace_node<PacketSink>(PortSpec(rate, 2, SampleFormat::F32P));
    engine.connect(OPORT(src, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(sink, 0));
    engine.commit();
    clock.start();
    clock.advance();
    const auto *out = engine.node<PacketSink>(sink);
    check(out->sample(0, 0) == -0.25f && out->sample(0, 1) == 3.5f,
          "Matrix rows map and combine source channels with gain");
}
void matrix_validation()
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect_error(ErrorCode::InvalidArg, [] { Matrix(rate, 0); });
    expect_error(ErrorCode::InvalidArg, [] { Matrix(rate, 2, 1, {1}); });
    expect_error(ErrorCode::InvalidArg, [&] { Matrix(rate, 1, 1, {nan}); });
    expect_error(ErrorCode::TooLarge,
        []
        {
            const auto n = std::numeric_limits<std::size_t>::max();
            Matrix(rate, n, n, {});
        });
    Matrix matrix(rate, 1);
    expect_error(ErrorCode::InvalidArg, [&] { matrix.set_gain(nan); });
}
void check_gain_ramp(bool identity)
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<FloatSource>(
        rate, 1, [](std::uint64_t frame, std::size_t)
        { return frame % 240 == 119 ? std::numeric_limits<float>::quiet_NaN() : 1.0f; });
    const auto matrix = identity ? engine.emplace_node<Matrix>(rate, 1)
                                         : engine.emplace_node<Matrix>(rate, 1, 1, std::initializer_list<float>{2});
    const auto sink = engine.emplace_node<PacketSink>(PortSpec(rate, 1, SampleFormat::F32P));
    engine.connect(OPORT(source, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(sink, 0));
    engine.commit();
    clock.start();
    clock.advance();
    auto *gain = engine.node<Matrix>(matrix);
    auto *out = engine.node<PacketSink>(sink);
    check(gain->gain() == 1.0f, "Matrix reports its initial gain target");
    gain->set_gain(-1);
    check(gain->gain() == -1.0f, "Matrix reports the new gain target before processing");
    clock.advance();
    const auto coefficient = identity ? 1.0f : 2.0f;
    check(std::abs(out->sample(0) - coefficient * (1.0f - 2.0f / 240)) < 1e-6 && out->sample(119) == 0 &&
              out->sample(239) == -coefficient,
          "Gain ramps have exact endpoints and skip an exactly zero crossing");
    gain->set_gain(0);
    clock.advance();
    clock.advance();
    for (std::size_t frame = 0; frame < 240; ++frame)
        check(out->sample(frame) == 0, "Zero gain suppresses non-finite inputs");
    engine.emplace_node<PacketSink>(PortSpec(rate, 1, SampleFormat::F32P));
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
    expect_error(ErrorCode::InvalidArg, [] { Mixer(rate, 1, 0); });
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto a = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; });
    const auto b = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 2.0f; });
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto sink = engine.emplace_node<PacketSink>(PortSpec(rate, 1, SampleFormat::F32P));
    engine.connect(OPORT(a, 0), IPORT(mix, 0));
    engine.connect(OPORT(b, 0), IPORT(mix, 1));
    engine.connect(OPORT(mix, 0), IPORT(sink, 0));
    engine.commit();
    clock.start();
    clock.advance();
    auto *mixer = engine.node<Mixer>(mix);
    auto *out = engine.node<PacketSink>(sink);
    check(out->sample(0) == 3, "Both mixer inputs contribute");
    check(!mixer->muted(0) && !mixer->muted(1), "Mixer inputs start unmuted");
    expect_error(ErrorCode::InvalidPort, [&] { mixer->set_muted(2, true); });
    expect_error(ErrorCode::InvalidPort, [&] { mixer->muted(2); });
    mixer->set_muted(0, true);
    check(mixer->muted(0) && !mixer->muted(1), "Mixer reports the mute setting of each input");
    clock.advance();
    check(out->sample(0) == 2 && out->sample(239) == 2, "Mute skips the selected input for the whole block");
    clock.advance();
    check(out->sample(0) == 2, "Mute remains stable");
    mixer->set_muted(0, false);
    check(!mixer->muted(0), "Mixer reports the unmute setting before processing");
    clock.advance();
    check(out->sample(0) == 3 && out->sample(239) == 3, "Unmute restores the selected input for the whole block");
}
void empty_block_ramps()
{
    const PortSpec spec(rate, 2, SampleFormat::F32P);
    Matrix gain(rate, 2);
    Matrix mapped(rate, 2, 2, {1, 0, 0, 1});
    gain.prepare(period);
    mapped.prepare(period);
    gain.set_gain(0);
    mapped.set_gain(0);

    Matrix *plugins[]{&gain, &mapped};
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const float samples[]{1.0f, nan, 2.0f, nan};
    float result[4]{};
    for (auto *plugin : plugins)
    {
        const IBBlock empty_input{reinterpret_cast<const std::uint8_t *>(samples), spec, 0};
        const OBBlock empty_output{reinterpret_cast<std::uint8_t *>(result), spec, 0};
        plugin->process({period, 0, {&empty_input, 1}, {&empty_output, 1}});
        const IBBlock input{reinterpret_cast<const std::uint8_t *>(samples), spec, 2};
        const OBBlock output{reinterpret_cast<std::uint8_t *>(result), spec, 2};
        plugin->process({period, 1, {&input, 1}, {&output, 1}});
        check(result[0] == 0.5f && result[1] == 0 && result[2] == 1.0f && result[3] == 0,
              "An empty block preserves both channels' ramp and the zero-gain endpoint suppresses NaN");
    }
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"matrix_identity_bits", matrix_identity_bits},
                      {"matrix_mapping", matrix_mapping},
                      {"matrix_validation", matrix_validation},
                      {"gain_ramp_and_zero", gain_ramp_and_zero},
                      {"mixer_mute", mixer_mute},
                      {"empty_block_ramps", empty_block_ramps}});
}
