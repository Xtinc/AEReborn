#include "support/audio_fixture.h"
#include <array>
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void pcm_formats()
{
    const struct
    {
        Format format;
        std::vector<std::uint8_t> bytes;
        float peak;
    } cases[] = {
        {Format::S16LE, {0, 0x80, 0, 0x40, 0, 0x20, 0xff, 0x7f}, 32767.0f / 32768},
        {Format::S24_3LE, {0, 0, 0x80, 0, 0, 0x40, 0, 0, 0x20, 0xff, 0xff, 0x7f}, 8388607.0f / 8388608},
        {Format::S24LE, {0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0, 0xff, 0xff, 0x7f, 0}, 8388607.0f / 8388608},
        {Format::S32LE, {0, 0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0xff, 0xff, 0xff, 0x7f}, 1},
        {Format::F32LE, {0, 0, 0x80, 0xbf, 0, 0, 0, 0x3f, 0, 0, 0x80, 0x3e, 0, 0, 0xc0, 0x3f}, 1.5f}};
    for (const auto &item : cases)
    {
        const PortSpec pcm(rate, 2, item.format), planar(rate, 2);
        Conversion decode(pcm, planar), encode(planar, pcm);
        auto bytes = item.bytes;
        bytes.resize(pcm.max_frames(period) * 2 * sample_bytes(item.format));
        decode.run({bytes.data(), bytes.size()});
        check(decode.sink->sample(0, 0) == -1 && decode.sink->sample(0, 1) == 0.5f &&
                  decode.sink->sample(1, 0) == 0.25f && decode.sink->sample(1, 1) == item.peak,
              "Decode matches independent signed PCM and float byte references");
        std::vector<float> samples(480);
        samples[0] = item.format == Format::F32LE ? -1.0f : -2.0f;
        samples[1] = 0.25f;
        samples[240] = 0.5f;
        samples[241] = 1.5f;
        const auto input = float_bytes(samples);
        encode.run({input.data(), input.size()});
        const auto actual = encode.sink->bytes();
        check(std::equal(actual.begin(), actual.end(), bytes.begin()),
              "Encode saturates and writes canonical PCM bytes");
        for (std::size_t frame = 2; frame < 240; ++frame)
            check(decode.sink->sample(frame, 0) == 0 && decode.sink->sample(frame, 1) == 0,
                  "Remaining frames are silence");
    }
}
void float_layout()
{
    const PortSpec planar(rate, 2), interleaved(rate, 2, Format::F32LE);
    Conversion conversion(planar, interleaved);
    std::vector<float> input(480);
    for (std::size_t i = 0; i < 240; ++i)
    {
        input[i] = static_cast<float>(i);
        input[240 + i] = -static_cast<float>(i);
    }
    const auto bytes = float_bytes(input);
    conversion.run({bytes.data(), bytes.size()});
    for (std::size_t i = 0; i < 240; ++i)
        check(conversion.sink->sample(i, 0) == input[i] && conversion.sink->sample(i, 1) == input[240 + i],
              "Planar to interleaved channel order");
}
void bit_preservation()
{
    for (const auto format : {Format::F32P, Format::F32LE, Format::S24LE})
    {
        const PortSpec spec(rate, 2, format);
        Conversion conversion(spec, spec);
        std::vector<std::uint8_t> bytes(spec.max_frames(period) * 2 * sample_bytes(format));
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(i * 73);
        conversion.run({bytes.data(), bytes.size()});
        check(std::equal(bytes.begin(), bytes.end(), conversion.sink->bytes().begin()),
              "Identity conversion preserves every bit, including NaNs and padding");
    }
    const PortSpec native(rate, 1), wire(rate, 1, Format::F32LE);
    Conversion encode(native, wire), decode(wire, native);
    std::vector<float> values(240);
    const std::uint32_t words[] = {0x80000000u, 0x7fc12345u, 0x7f800000u, 0xff800000u};
    for (std::size_t i = 0; i < 4; ++i)
        std::memcpy(&values[i], &words[i], 4);
    const auto input = float_bytes(values);
    encode.run({input.data(), input.size()});
    decode.run(encode.sink->bytes());
    check(std::equal(input.begin(), input.end(), decode.sink->bytes().begin()),
          "Float layout conversion preserves signed zero and NaN payloads");
}
void unaligned_pcm()
{
    const PortSpec pcm(rate, 1, Format::F32LE), planar(rate, 1);
    Conversion conversion(pcm, planar);
    std::vector<std::uint8_t> bytes(961);
    write_f32le(bytes.data() + 1, 0.375f);
    conversion.run({bytes.data() + 1, 960});
    check(conversion.sink->sample(0) == 0.375f, "Unaligned PCM is decoded without float pointer aliasing");
}
void converter_validation()
{
    const PortSpec mono(rate, 1), stereo(rate, 2);
    Convert invalid(mono, stereo);
    expect_error(ErrorCode::ChannelMismatch, invalid.prepare(period));
    expect_error(ErrorCode::InvalidArg, invalid.prepare(static_cast<BlockPeriod>(0)));
    Convert bad(PortSpec(rate, 1, static_cast<Format>(99)), mono);
    expect_error(ErrorCode::InvalidArg, bad.prepare(period));
}
void integer_limits()
{
    const PortSpec planar(rate, 1), pcm(rate, 1, Format::S16LE);
    Conversion conversion(planar, pcm);
    std::vector<float> values(240);
    values[0] = std::numeric_limits<float>::quiet_NaN();
    values[1] = std::numeric_limits<float>::infinity();
    values[2] = -std::numeric_limits<float>::infinity();
    values[3] = 0.5f / 32768;
    const auto bytes = float_bytes(values);
    conversion.run({bytes.data(), bytes.size()});
    const std::uint8_t expected[] = {0, 0, 0xff, 0x7f, 0, 0x80, 1, 0};
    check(std::equal(expected, expected + 8, conversion.sink->bytes().begin()),
          "Integer encoding handles NaN, clipping and half-step rounding");
}
void empty_blocks()
{
    const PortSpec input(static_cast<SampleRate>(1), 1);
    const PortSpec output(static_cast<SampleRate>(1), 1, Format::S16LE);
    Conversion conversion(input, output);
    const float value = 0.5f;
    std::uint8_t bytes[sizeof(value)];
    std::memcpy(bytes, &value, sizeof(value));
    std::size_t total = 0;
    for (std::uint64_t tick = 0; tick < 201; ++tick)
    {
        const auto frames = input.frames(period, tick);
        conversion.run({bytes, frames * sizeof(value)});
        check(conversion.sink->frames == frames, "An empty fractional block remains empty through conversion");
        if (frames)
            check(conversion.sink->bytes()[0] == 0 && conversion.sink->bytes()[1] == 0x40,
                  "Sparse sample blocks retain their value");
        total += frames;
    }
    check(total == 2, "Fractional accounting includes zero-frame periods");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"pcm_formats", pcm_formats},
                      {"float_layout", float_layout},
                      {"bit_preservation", bit_preservation},
                      {"unaligned_pcm", unaligned_pcm},
                      {"converter_validation", converter_validation},
                      {"integer_limits", integer_limits},
                      {"empty_blocks", empty_blocks}});
}
