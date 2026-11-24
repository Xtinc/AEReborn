#include "tests/support/audio_fixture.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <array>
#include <limits>

namespace
{
using namespace aereborn;
using namespace aereborn::test;

void pcm_byte_values()
{
    const PortSpec mono(rate, 1, SampleFormat::F32P);
    for (const auto spec : {PortSpec(rate, 1, SampleFormat::S16LE), PortSpec(rate, 1, SampleFormat::S24_3LE),
                            PortSpec(rate, 1, SampleFormat::S24LE), PortSpec(rate, 1, SampleFormat::S32LE)})
    {
        const auto width = sample_bytes(spec.format);
        const auto sign = sample_bits(spec.format) / 8 - 1;
        std::vector<std::uint8_t> bytes(1 + 3 * width), actual(bytes.size(), 0xcd);
        auto *raw = bytes.data() + 1;
        raw[sign] = 0x80;
        raw[width + sign] = 0x40;
        raw[2 * width + sign] = 0xe0;
        if (spec.format == SampleFormat::S24LE)
            raw[3] = 0xff;
        float samples[3]{};
        const IBBlock input(raw, spec, 3);
        const OBBlock planar(reinterpret_cast<std::uint8_t *>(samples), mono, 3);
        const OBBlock result(actual.data() + 1, spec, 3);
        Convert decode(spec, mono), encode(mono, spec);
        decode.prepare(period);
        encode.prepare(period);
        const IBBlock planar_input = planar;
        decode.process({period, 0, {&input, 1}, {&planar, 1}});
        check(samples[0] == -1.0f && samples[1] == 0.5f && samples[2] == -0.25f,
              "Unaligned integer PCM decodes with the correct sign and scale");
        encode.process({period, 0, {&planar_input, 1}, {&result, 1}});
        if (spec.format == SampleFormat::S24LE)
            raw[3] = 0;
        check(actual.front() == 0xcd && std::equal(bytes.begin() + 1, bytes.end(), actual.begin() + 1),
              "Integer encoding emits canonical little-endian sample bytes");
    }

    const PortSpec s16(rate, 1, SampleFormat::S16LE);
    const float limits[]{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 0.5f / 32768};
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(limits), mono, 4);
    std::array<std::uint8_t, 8> result{};
    const OBBlock output(result.data(), s16, 4);
    Convert encode(mono, s16);
    encode.prepare(period);
    encode.process({period, 0, {&input, 1}, {&output, 1}});
    const std::array<std::uint8_t, 8> expected{{0, 0, 0xff, 0x7f, 0, 0x80, 1, 0}};
    check(result == expected, "Integer encoding handles NaN, clipping and half-step rounding");
}

void float_channel_bits()
{
    const PortSpec planar(rate, 2, SampleFormat::F32P), pcm(rate, 2, SampleFormat::F32LE);
    const std::uint32_t words[]{0x80000000, 0x7fc12468, 0x7f800000, 0x3f800000, 0xff800000, 0x7fc00401};
    float source[6], decoded[6]{}, bypass[6]{};
    std::memcpy(source, words, sizeof(words));
    std::array<std::uint8_t, 25> wire{};
    const IBBlock input(reinterpret_cast<std::uint8_t *>(source), planar, 3);
    const OBBlock interleaved(wire.data() + 1, pcm, 3);
    const IBBlock encoded = interleaved;
    const OBBlock output(reinterpret_cast<std::uint8_t *>(decoded), planar, 3);
    const OBBlock identity(reinterpret_cast<std::uint8_t *>(bypass), planar, 3);
    Convert pack(planar, pcm), unpack(pcm, planar);
    Matrix matrix(rate, 2);
    pack.prepare(period);
    unpack.prepare(period);
    matrix.prepare(period);
    pack.process({period, 0, {&input, 1}, {&interleaved, 1}});
    for (std::size_t frame = 0; frame < 3; ++frame)
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t byte = 0; byte < 4; ++byte)
                check(wire[1 + (2 * frame + ch) * 4 + byte] ==
                          static_cast<std::uint8_t>(words[ch * 3 + frame] >> (8 * byte)),
                      "Planar floats interleave without changing their bits");
    unpack.process({period, 0, {&encoded, 1}, {&output, 1}});
    matrix.process({period, 0, {&input, 1}, {&identity, 1}});
    check(std::memcmp(source, decoded, sizeof(source)) == 0 && std::memcmp(source, bypass, sizeof(source)) == 0,
          "Identity matrix and reverse layout preserve signed zeros and NaN payloads");
}

void matrix_and_mix_contracts()
{
    const PortSpec mono(rate, 1, SampleFormat::F32P), stereo(rate, 2, SampleFormat::F32P);
    expect_error(ErrorCode::InvalidArg, [&] { Convert wrong(mono, stereo); });
    expect_error(ErrorCode::InvalidArg, [] { Matrix wrong(rate, 1, 2, {1}); });
    expect_error(ErrorCode::InvalidArg, [] { Mixer wrong(rate, 1, 0); });
    Matrix gain(rate, 1);
    Mixer mix(rate, 1, 2);
    expect_error(ErrorCode::InvalidPort, [&] { mix.set_muted(2, true); });
    expect_error(ErrorCode::InvalidArg, [&] { gain.set_gain(std::numeric_limits<float>::infinity()); });
    gain.prepare(period);
    gain.set_gain(-1);
    const float original[]{1, 1, 1, 1};
    float scaled[4]{}, result[4]{};
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(original), mono, 4);
    const OBBlock output(reinterpret_cast<std::uint8_t *>(scaled), mono, 4);
    const OBBlock mixed(reinterpret_cast<std::uint8_t *>(result), mono, 4);
    const IBBlock empty(reinterpret_cast<const std::uint8_t *>(original), mono, 0);
    const OBBlock empty_output(reinterpret_cast<std::uint8_t *>(scaled), mono, 0);
    gain.process({period, 0, {&empty, 1}, {&empty_output, 1}});
    gain.process({period, 1, {&input, 1}, {&output, 1}});
    const float expected[]{0.5f, 0, -0.5f, -1};
    check(std::equal(scaled, scaled + 4, expected), "An empty block does not consume the gain ramp");
    const IBBlock inputs[]{input, output};
    mix.set_muted(0, true);
    mix.process({period, 1, {inputs, 2}, {&mixed, 1}});
    check(std::equal(result, result + 4, expected), "Muting one input leaves the other input intact");
    mix.set_muted(0, false);
    mix.process({period, 2, {inputs, 2}, {&mixed, 1}});
    for (std::size_t frame = 0; frame < 4; ++frame)
        check(result[frame] == 1.0f + expected[frame], "Unmuting restores the sum");
}

void fractional_rate_and_antialias()
{
    for (const auto target : {SampleRate::Hz44100, SampleRate::Hz16000})
    {
        const PortSpec input(rate, 2, SampleFormat::F32P), output(target, 2, SampleFormat::F32P);
        Engine engine(period);
        ClockedGraph clock(engine);
        const auto signal = engine.emplace_node<FloatSource>(rate, 2, [](std::uint64_t frame, std::size_t ch) {
            const auto hz = ch == 0 ? 1000 : 12000;
            return static_cast<float>(0.5 * std::sin(two_pi * hz * frame / 48000));
        });
        const auto converter = engine.emplace_node<Convert>(input, output);
        const auto sink = engine.emplace_node<PacketSink>(output);
        connect(engine, signal, converter);
        connect(engine, converter, sink);
        engine.commit();
        const auto *sample = engine.node<PacketSink>(sink);
        clock.start();
        std::size_t offset = 0;
        double stop_energy = 0;
        const auto hz = static_cast<unsigned>(target);
        for (std::size_t tick = 0; tick < 12; ++tick)
        {
            clock.advance();
            const auto frames = hz == 16000 ? 80 : (tick % 2 ? 220 : 221);
            check(sample->frames == static_cast<std::size_t>(frames), "Each conversion emits its own block size");
            if (tick > 0)
                for (int i = 0; i < frames; ++i)
                {
                    const auto time = (offset + i) / static_cast<double>(hz) - 0.001;
                    check(std::abs(sample->sample(i, 0) - 0.5 * std::sin(two_pi * 1000 * time)) < 0.001,
                          "The pass band follows one continuous sample timeline");
                    if (hz == 16000)
                        stop_energy += sample->sample(i, 1) * sample->sample(i, 1);
                    else
                        check(std::abs(sample->sample(i, 1) - 0.5 * std::sin(two_pi * 12000 * time)) < 0.001,
                              "The higher-rate channel retains its independent signal");
                }
            offset += frames;
        }
        check(offset == hz * 12 / 200 && stop_energy < 0.01, "The total frame count and rejection band are correct");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"pcm_byte_values", pcm_byte_values},
                                                {"float_channel_bits", float_channel_bits},
                                                {"matrix_and_mix_contracts", matrix_and_mix_contracts},
                                                {"fractional_rate_and_antialias", fractional_rate_and_antialias}});
}
