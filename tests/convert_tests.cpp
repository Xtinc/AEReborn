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
        SampleFormat format;
        std::vector<std::uint8_t> bytes;
        float peak;
    } cases[] = {
        {SampleFormat::S16LE, {0, 0x80, 0, 0x40, 0, 0x20, 0xff, 0x7f}, 32767.0f / 32768},
        {SampleFormat::S24_3LE, {0, 0, 0x80, 0, 0, 0x40, 0, 0, 0x20, 0xff, 0xff, 0x7f}, 8388607.0f / 8388608},
        {SampleFormat::S24LE, {0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0, 0xff, 0xff, 0x7f, 0}, 8388607.0f / 8388608},
        {SampleFormat::S32LE, {0, 0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0xff, 0xff, 0xff, 0x7f}, 1},
        {SampleFormat::F32LE, {0, 0, 0x80, 0xbf, 0, 0, 0, 0x3f, 0, 0, 0x80, 0x3e, 0, 0, 0xc0, 0x3f}, 1.5f}};
    for (const auto &item : cases)
    {
        const PortSpec pcm(rate, 2, item.format), planar(rate, 2, SampleFormat::F32P);
        Conversion decode(pcm, planar), encode(planar, pcm);
        auto bytes = item.bytes;
        bytes.resize(pcm.max_frames(period) * 2 * sample_bytes(item.format));
        decode.run({bytes.data(), bytes.size()});
        check(decode.sink->sample(0, 0) == -1 && decode.sink->sample(0, 1) == 0.5f &&
                  decode.sink->sample(1, 0) == 0.25f && decode.sink->sample(1, 1) == item.peak,
              "Decode matches independent signed PCM and float byte references");
        std::vector<float> samples(480);
        samples[0] = item.format == SampleFormat::F32LE ? -1.0f : -2.0f;
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
void pcm_to_pcm()
{
    const PortSpec input(rate, 2, SampleFormat::S32LE), output(rate, 2, SampleFormat::S24LE);
    Conversion conversion(input, output);
    // The first sample rounds to a half step in the float workspace.
    std::vector<std::uint8_t> bytes{0x7f, 0, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0, 0x20, 0xff, 0xff, 0xff, 0x7f};
    bytes.resize(input.max_frames(period) * input.channels * sample_bytes(input.format));
    conversion.run({bytes.data(), bytes.size()});
    const std::array<std::uint8_t, 16> expected{{1, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0, 0x20, 0, 0xff, 0xff, 0x7f, 0}};
    check(std::equal(expected.begin(), expected.end(), conversion.sink->bytes().begin()),
          "PCM conversion preserves float rounding, channel order, and canonical S24LE padding");
}
void float_layout()
{
    const PortSpec planar(rate, 2, SampleFormat::F32P), interleaved(rate, 2, SampleFormat::F32LE);
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
    for (const auto format : {SampleFormat::F32P, SampleFormat::F32LE, SampleFormat::S24LE})
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
    for (const std::size_t channels : {1, 2})
    {
        const PortSpec native(rate, channels, SampleFormat::F32P), wire(rate, channels, SampleFormat::F32LE);
        Conversion encode(native, wire), decode(wire, native);
        const auto frames = native.max_frames(period);
        std::vector<float> values(frames * channels);
        const std::uint32_t words[] = {0x80000000u, 0x7fc12345u, 0x7f800001u, 0xff800000u};
        for (std::size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
            std::memcpy(&values[(i % channels) * frames + i / channels], &words[i], sizeof(words[0]));
        const auto input = float_bytes(values);
        encode.run({input.data(), input.size()});
        decode.run(encode.sink->bytes());
        check(std::equal(input.begin(), input.end(), decode.sink->bytes().begin()),
              "Float layout conversion preserves signed zero and NaN payloads");
    }
}
void unaligned_pcm()
{
    const PortSpec pcm(rate, 1, SampleFormat::F32LE), planar(rate, 1, SampleFormat::F32P);
    Conversion conversion(pcm, planar);
    std::vector<std::uint8_t> bytes(961);
    write_f32le(bytes.data() + 1, 0.375f);
    conversion.run({bytes.data() + 1, 960});
    check(conversion.sink->sample(0) == 0.375f, "Unaligned PCM is decoded without float pointer aliasing");
}
void converter_validation()
{
    const PortSpec mono(rate, 1, SampleFormat::F32P), stereo(rate, 2, SampleFormat::F32P);
    expect_exception<std::invalid_argument>([&] { Convert invalid(mono, stereo); });
    expect_exception<std::invalid_argument>([&] { Convert invalid(PortSpec(rate, 1, static_cast<SampleFormat>(99)), mono); });
    expect_exception<std::invalid_argument>(
        [&] { Convert invalid(mono, PortSpec(static_cast<SampleRate>(1), 1, SampleFormat::F32P)); });
}
void integer_limits()
{
    const PortSpec planar(rate, 1, SampleFormat::F32P), pcm(rate, 1, SampleFormat::S16LE);
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
    const PortSpec input(rate, 1, SampleFormat::F32P), output(rate, 1, SampleFormat::S16LE);
    Convert convert(input, output);
    checked(convert.prepare(period));
    const float value = 0.5f;
    std::uint8_t bytes[]{0xcd, 0xcd};
    const IBBlock empty_input{reinterpret_cast<const std::uint8_t *>(&value), input, 0};
    const OBBlock empty_output{bytes, output, 0};
    convert.process({period, 0, {&empty_input, 1}, {&empty_output, 1}});
    check(bytes[0] == 0xcd && bytes[1] == 0xcd, "An empty block writes no PCM samples");
}

void fixed_slices()
{
    const PortSpec planar(rate, 2, SampleFormat::F32P), pcm(rate, 2, SampleFormat::S16LE);
    const std::uint8_t bytes[]{0, 0, 0, 0, 0, 0x20, 0, 0xc0, 0, 0x40, 0, 0x10};
    const IBBlock input(bytes, pcm, 3);
    std::array<float, 10> samples;
    samples.fill(-2.0f);
    const OBBlock output(reinterpret_cast<std::uint8_t *>(samples.data()), planar, 5);
    Convert decode(pcm, planar), copy(planar, planar);
    expect_error(ErrorCode::NotPrepared, decode.required_input(1));
    expect_error(ErrorCode::NotPrepared, decode.process(input, output));
    checked(decode.prepare(3, 3));
    checked(copy.prepare(3, 3));
    check(checked(decode.required_input(3)) == 3, "Fixed same-rate conversion needs one input per output");
    const auto decoded = checked(decode.process(IBBlock{input.byte_view().data() + 4, pcm, 2},
                                                output.f32p_view().subblock(2, 3)));
    check(decoded.consumed == 2 && decoded.produced == 2 &&
              samples == std::array<float, 10>{{-2, -2, 0.25f, 0.5f, -2, -2, -2, -0.5f, 0.125f, -2}},
          "Sliced PCM decode preserves both channel strides and the unwritten tail");

    const std::uint32_t nan = 0x7fc12345;
    std::memcpy(&samples[2], &nan, sizeof(nan));
    samples[7] = -0.0f;
    std::array<float, 12> copied;
    copied.fill(-3.0f);
    const OBBlock dest(reinterpret_cast<std::uint8_t *>(copied.data()), planar, 6);
    const auto passthrough = checked(copy.process(output.f32p_view().subblock(2, 2), dest.f32p_view().subblock(1, 2)));
    check(passthrough.consumed == 2 && passthrough.produced == 2 &&
              std::memcmp(copied.data() + 1, samples.data() + 2, 2 * sizeof(float)) == 0 &&
              std::memcmp(copied.data() + 7, samples.data() + 7, 2 * sizeof(float)) == 0 &&
              copied[0] == -3 && copied[3] == -3 && copied[6] == -3 && copied[9] == -3,
          "Planar slice passthrough preserves NaN payloads, signed zero and the surrounding frames");
}

void stereo_short_fir_output()
{
    const PortSpec planar(rate, 2, SampleFormat::F32P);
    std::array<float, 256 * 2> samples;
    std::fill_n(samples.begin(), 256, 0.25f);
    std::fill_n(samples.begin() + 256, 256, -0.5f);
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(samples.data()), planar, 256);
    std::array<float, 200> reference{};
    const OBBlock expected(reinterpret_cast<std::uint8_t *>(reference.data()), planar, 100);
    Convert to_planar(planar, planar);
    checked(to_planar.prepare(256, 256, 0.002));
    check(checked(to_planar.process(input.f32p_view().subblock(4, 100), expected.f32p_view())).produced == 100,
          "Planar reference includes filter startup and steady output");
    for (const auto format : {SampleFormat::S16LE, SampleFormat::S24_3LE})
    {
        const PortSpec pcm(rate, 2, format);
        const auto width = sample_bytes(format);
        std::vector<std::uint8_t> bytes(110 * 2 * width, 0xcd);
        const OBBlock output(bytes.data(), pcm, 110);
        Convert convert(planar, pcm);
        checked(convert.prepare(256, 256, 0.002));
        const auto transfer = checked(convert.process(input.f32p_view().subblock(4, 100),
                                                       OBBlock{output.byte_view().data() + 10 * width, pcm, 100}));
        check(transfer.consumed == 100 && transfer.produced == 100, "Short block fits the prepared FIR workspace");
        const auto scale = static_cast<double>(std::uint32_t{1} << (sample_bits(format) - 1));
        for (std::size_t frame = 0; frame < 100; ++frame)
        {
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                const auto value = static_cast<std::int32_t>(std::round(reference[ch * 100 + frame] * scale));
                const auto word = static_cast<std::uint32_t>(value);
                const auto offset = ((5 + frame) * 2 + ch) * width;
                for (std::size_t byte = 0; byte < width; ++byte)
                    check(bytes[offset + byte] == static_cast<std::uint8_t>(word >> (8 * byte)),
                          "Both channels use the workspace stride when encoding a short output block");
            }
        }
        check(bytes[5 * 2 * width - 1] == 0xcd && bytes[105 * 2 * width] == 0xcd,
              "FIR encoding leaves surrounding frames alone");
    }
}

void frame_bounds()
{
    const PortSpec slow(SampleRate::Hz8000, 1, SampleFormat::F32P);
    const PortSpec fast(SampleRate::Hz96000, 1, SampleFormat::F32P);
    const auto limit = std::numeric_limits<std::size_t>::max() / 16;
    check(checked(Convert::max_output_frames(slow, slow, 100)) == 100 &&
              checked(Convert::max_input_frames(slow, slow, 100)) == 100 &&
              checked(Convert::max_output_frames(slow, fast, 0, 0.25)) == 0,
          "A zero batch or same-rate passthrough needs no extra workspace frames");
    check(checked(Convert::max_output_frames(slow, fast, 100, 0.25)) == 1616 &&
              checked(Convert::max_input_frames(fast, slow, 100, 0.25)) == 1501,
          "Frame bounds include rate correction and pending upsampling output from history");
    expect_error(ErrorCode::InvalidArg, Convert::max_output_frames(slow, fast, 100, 0.5));
    expect_error(ErrorCode::FormatMismatch,
                 Convert::max_input_frames(slow, PortSpec(rate, 2, SampleFormat::F32P), 100));
    expect_error(ErrorCode::TooLarge, Convert::max_output_frames(slow, fast, limit, 0.25));
    expect_error(ErrorCode::TooLarge, Convert::max_input_frames(fast, slow, limit, 0.25));

    Convert convert(fast, slow);
    checked(convert.prepare(16, limit, 0.25));
    check(checked(convert.required_input(2, 1.25)) == 16, "The caller sets the allowed correction range");
    expect_error(ErrorCode::TooLarge, convert.required_input(limit, 1.25));
    expect_error(ErrorCode::InvalidArg, convert.prepare(16, 2, -0.01));
    expect_error(ErrorCode::InvalidArg, convert.required_input(2, 1.26));
    check(checked(convert.required_input(2, 1.25)) == 16, "Invalid calls preserve the active limits and phase");

    Convert expand(slow, fast);
    const auto max_out = checked(Convert::max_output_frames(slow, fast, 1, 0.25));
    checked(expand.prepare(1, max_out, 0.25));
    float sample = 0.25f;
    std::vector<float> expanded(max_out);
    const IBBlock source(reinterpret_cast<const std::uint8_t *>(&sample), slow, 1);
    const OBBlock dest(reinterpret_cast<std::uint8_t *>(expanded.data()), fast, max_out);
    check(checked(expand.process(source.f32p_view(), dest.f32p_view().subblock(0, 1), 0.75)).consumed == 1,
          "The first capped call leaves additional output pending in history");
    check(checked(expand.process(source, dest, 0.75)).produced == 31,
          "The output bound covers pending history and the new source frame in the same call");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"pcm_formats", pcm_formats},
                      {"pcm_to_pcm", pcm_to_pcm},
                      {"float_layout", float_layout},
                      {"bit_preservation", bit_preservation},
                      {"unaligned_pcm", unaligned_pcm},
                      {"converter_validation", converter_validation},
                      {"integer_limits", integer_limits},
                      {"empty_blocks", empty_blocks},
                      {"fixed_slices", fixed_slices},
                      {"stereo_short_fir_output", stereo_short_fir_output},
                      {"frame_bounds", frame_bounds}});
}
