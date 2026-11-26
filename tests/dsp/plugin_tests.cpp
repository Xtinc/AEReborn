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
    expect_error(RetCode::EPARAM, [&] { Convert wrong(mono, stereo); });
    expect_error(RetCode::EPARAM, [] { Matrix wrong(rate, 1, 2, {1}); });
    expect_error(RetCode::EPARAM, [] { Mixer wrong(rate, 1, 0); });
    Matrix gain(rate, 1);
    Mixer mix(rate, 1, 2);
    expect_error(RetCode::EPORT, [&] { mix.set_muted(2, true); });
    expect_error(RetCode::EPARAM, [&] { gain.set_gain(std::numeric_limits<float>::infinity()); });
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

void matrix_and_mix_activity()
{
    const PortSpec mono(rate, 1, SampleFormat::F32P);
    const float signal[]{1.0f, 1.0f, 1.0f, 1.0f};
    const float silence[4]{};
    float scaled[4]{1.0f, 1.0f, 1.0f, 1.0f}, mixed[4]{};
    const IBBlock source(reinterpret_cast<const std::uint8_t *>(signal), mono, 4);
    const IBBlock gap(reinterpret_cast<const std::uint8_t *>(silence), mono, 4);
    const OBBlock output(reinterpret_cast<std::uint8_t *>(scaled), mono, 4);
    const OBBlock sum(reinterpret_cast<std::uint8_t *>(mixed), mono, 4);
    std::uint8_t active = 1, inactive = 0, scale_active = 1, mix_active = 1;
    Matrix matrix(rate, 1);
    matrix.prepare(period);
    Mixer mixer(rate, 1, 2);

    matrix.set_gain(0.25f);
    matrix.process({period, 0, {&gap, 1}, {&output, 1}, {&inactive, 1}, {&scale_active, 1}});
    check(!scale_active && std::memcmp(scaled, silence, sizeof(scaled)) == 0,
          "Matrix clears the old output when its input goes inactive");
    matrix.process({period, 1, {&source, 1}, {&output, 1}, {&active, 1}, {&scale_active, 1}});
    check(scale_active && scaled[0] == 0.25f && scaled[3] == 0.25f,
          "A gain changed during an inactive block is in effect when input resumes");
    const IBBlock inputs[]{output, gap};
    const std::uint8_t input_flags[]{1, 0};
    mixer.process({period, 1, {inputs, 2}, {&sum, 1}, {input_flags, 2}, {&mix_active, 1}});
    check(mix_active && std::memcmp(mixed, scaled, sizeof(mixed)) == 0,
          "Mixer emits its active, unmuted route");
    mixer.set_muted(0, true);
    mixer.process({period, 2, {inputs, 2}, {&sum, 1}, {input_flags, 2}, {&mix_active, 1}});
    check(!mix_active && std::memcmp(mixed, silence, sizeof(mixed)) == 0,
          "Muting the only active input clears the previous mix");
    mixer.set_muted(0, false);
    mixer.process({period, 3, {inputs, 2}, {&sum, 1}, {input_flags, 2}, {&mix_active, 1}});
    check(mix_active && std::memcmp(mixed, scaled, sizeof(mixed)) == 0,
          "Unmuting restores the mix and its activity flag");

    matrix.set_gain(0.0f);
    matrix.process({period, 4, {&source, 1}, {&output, 1}, {&active, 1}, {&scale_active, 1}});
    check(scale_active && scaled[0] == 0.1875f && scaled[3] == 0.0f, "The fade-out block stays active");
    matrix.process({period, 5, {&source, 1}, {&output, 1}, {&active, 1}, {&scale_active, 1}});
    check(!scale_active && std::memcmp(scaled, silence, sizeof(scaled)) == 0,
          "Steady zero gain emits inactive, zero-filled output");
}

void mixer_priority_limits()
{
    Mixer mixer(rate, 1, Mixer::max_inputs);
    check(mixer.inputs().size() == 64, "Mixer accepts 64 inputs");
    expect_error(RetCode::TOOLARGE, [] { Mixer invalid(rate, 1, 65); });
    const auto end = mixer.inputs().size();
    expect_error(RetCode::EPORT, [&] { mixer.set_muted(end, true); });
    expect_error(RetCode::EPORT, [&] { mixer.muted(end); });
    expect_error(RetCode::EPORT, [&] { mixer.set_high_priority(end, true); });
    expect_error(RetCode::EPORT, [&] { mixer.high_priority(end); });
    expect_error(RetCode::EPORT, [&] { mixer.suppressed(end); });

    const auto high = end - 1;
    mixer.set_muted(high, true);
    mixer.set_high_priority(high, true);
    check(mixer.muted(high) && mixer.high_priority(high) && !mixer.muted(0) && !mixer.high_priority(0),
          "The highest bit can be set independently");
    mixer.set_muted(high, false);

    const PortSpec mono(rate, 1, SampleFormat::F32P);
    const float silence = 0.0f, low_sample = 0.5f, high_sample = 0.25f;
    float mixed = 0.0f;
    std::array<IBBlock, Mixer::max_inputs> inputs;
    inputs.fill({reinterpret_cast<const std::uint8_t *>(&silence), mono, 1});
    inputs.front() = {reinterpret_cast<const std::uint8_t *>(&low_sample), mono, 1};
    inputs.back() = {reinterpret_cast<const std::uint8_t *>(&high_sample), mono, 1};
    std::array<std::uint8_t, Mixer::max_inputs> flags{};
    flags.front() = flags.back() = 1;
    const OBBlock output(reinterpret_cast<std::uint8_t *>(&mixed), mono, 1);
    mixer.process({period, 0, {inputs.data(), inputs.size()}, {&output, 1}, {flags.data(), flags.size()}});
    check(mixed == high_sample && mixer.suppressed(0) && !mixer.suppressed(high),
          "The highest input bit participates in the priority decision");
}

void mixer_priority_transitions()
{
    const PortSpec stereo(rate, 2, SampleFormat::F32P);
    Mixer mixer(rate, 2, 3);
    mixer.set_high_priority(1, true);
    mixer.set_high_priority(2, true);
    const float low[]{4, 4, 4, 4, 8, 8, 8, 8};
    const float high1[]{1, 1, 1, 1, 2, 2, 2, 2};
    const float high2[]{2, 2, 2, 2, 4, 4, 4, 4};
    const float silence[8]{};
    float mixed[8]{};
    const IBBlock sources[]{{reinterpret_cast<const std::uint8_t *>(low), stereo, 4},
                            {reinterpret_cast<const std::uint8_t *>(high1), stereo, 4},
                            {reinterpret_cast<const std::uint8_t *>(high2), stereo, 4}};
    const IBBlock gap(reinterpret_cast<const std::uint8_t *>(silence), stereo, 4);
    const OBBlock output(reinterpret_cast<std::uint8_t *>(mixed), stereo, 4);
    const auto check_mix = [&](std::array<std::uint8_t, 3> flags, std::array<float, 4> expected) {
        const IBBlock inputs[]{flags[0] ? sources[0] : gap, flags[1] ? sources[1] : gap, flags[2] ? sources[2] : gap};
        std::uint8_t active = 0;
        mixer.process({period, 0, {inputs, 3}, {&output, 1}, {flags.data(), flags.size()}, {&active, 1}});
        check(active != 0, "Each priority transition emits an active block");
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
            check(mixed[frame] == expected[frame] && mixed[frame + expected.size()] == 2 * expected[frame],
                  "Both channels follow the expected frame-by-frame ramp");
    };

    check_mix({{1, 0, 0}}, {{4, 4, 4, 4}});
    check_mix({{1, 1, 1}}, {{6, 5, 4, 3}});
    check_mix({{1, 1, 1}}, {{3, 3, 3, 3}});
    check(mixer.suppressed(0) && !mixer.suppressed(1) && !mixer.suppressed(2), "High-priority inputs coexist");
    check_mix({{1, 1, 0}}, {{1, 1, 1, 1}});
    check(mixer.suppressed(0), "One active high-priority input keeps the low input suppressed");

    mixer.set_muted(1, true);
    check_mix({{1, 1, 0}}, {{1, 2, 3, 4}});
    check(!mixer.suppressed(0), "Muting the final high-priority input releases suppression");
    check_mix({{1, 1, 0}}, {{4, 4, 4, 4}});
    mixer.set_muted(1, false);
    check_mix({{0, 1, 0}}, {{1, 1, 1, 1}});
    check(!mixer.suppressed(0), "An inactive input is absent from the suppression report");
    check_mix({{1, 1, 0}}, {{1, 1, 1, 1}});
    check(mixer.suppressed(0), "The returning normal input is already suppressed");

    mixer.set_high_priority(0, true);
    check_mix({{1, 1, 0}}, {{2, 3, 4, 5}});
    check_mix({{1, 1, 0}}, {{5, 5, 5, 5}});
    mixer.set_high_priority(0, false);
    check_mix({{1, 1, 0}}, {{4, 3, 2, 1}});
    check_mix({{1, 0, 0}}, {{1, 2, 3, 4}});
    check_mix({{1, 0, 0}}, {{4, 4, 4, 4}});
}

void mixer_priority_single_frame_and_reset()
{
    const PortSpec mono(rate, 1, SampleFormat::F32P);
    Mixer mixer(rate, 1, 2);
    mixer.set_high_priority(1, true);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    float low[]{0.5f, nan}, high[]{0.25f, 0.25f}, mixed[2]{};
    std::uint8_t input_flags[]{1, 1}, active = 0;
    const auto render = [&](std::size_t frames) {
        const IBBlock inputs[]{{reinterpret_cast<const std::uint8_t *>(low), mono, frames},
                               {reinterpret_cast<const std::uint8_t *>(high), mono, frames}};
        const OBBlock output(reinterpret_cast<std::uint8_t *>(mixed), mono, frames);
        mixer.process({period, 0, {inputs, 2}, {&output, 1}, {input_flags, 2}, {&active, 1}});
    };
    render(2);
    check(mixed[0] == 0.5f && mixed[1] == 0.25f, "The zero-gain fade-out endpoint skips its NaN source");
    low[0] = std::numeric_limits<float>::infinity();
    render(2);
    check(mixed[0] == 0.25f && mixed[1] == 0.25f, "Steady suppression isolates NaN and infinity");

    mixer.set_muted(0, true);
    render(0);
    check(mixer.suppressed(0), "A zero-frame call preserves the previous suppression report");
    mixer.reset();
    check(!mixer.suppressed(0) && mixer.muted(0) && mixer.high_priority(1),
          "Reset clears suppression and retains requested settings");
    mixer.set_muted(0, false);
    low[0] = 0.5f;
    render(2);
    check(mixed[0] == 0.5f, "Reset discards the previous gain so a new fade-out begins");

    mixer.reset();
    low[0] = nan;
    render(1);
    check(active && mixed[0] == 0.25f, "A one-frame fade-out immediately reaches zero gain");
    low[0] = 0.5f;
    high[0] = 0.0f;
    input_flags[1] = 0;
    render(1);
    check(active && mixed[0] == 0.5f && !mixer.suppressed(0), "A one-frame fade-in immediately reaches unity");
    low[0] = 0.0f;
    input_flags[0] = 0;
    render(1);
    check(!active && mixed[0] == 0.0f, "The mix keeps a whole inactive block zero-filled");
}

void check_convert_tail(BandWidth from, BandWidth to, std::uint64_t flush_blocks)
{
    constexpr auto tick_period = BlockPeriod::INR_02MS;
    const PortSpec input_spec(from, 2, SampleFormat::F32P), output_spec(to, 2, SampleFormat::F32P);
    Convert convert(input_spec, output_spec);
    SincResampler reference;
    convert.prepare(tick_period);
    reference.prepare(2, from, to);
    std::vector<float> input(2 * input_spec.max_frames(tick_period));
    std::vector<float> output(2 * output_spec.max_frames(tick_period)), expected(output.size());
    const auto run_block = [&](std::uint64_t tick, bool active, bool expected_active) {
        const auto frames = input_spec.frames(tick_period, tick);
        const auto output_frames = output_spec.frames(tick_period, tick);
        const OBBlock writable(reinterpret_cast<std::uint8_t *>(input.data()), input_spec, frames);
        const IBBlock in = writable;
        const OBBlock out(reinterpret_cast<std::uint8_t *>(output.data()), output_spec, output_frames);
        const auto source = writable.f32p_view();
        const OFBlock ref(expected.data(), 2, output_frames, output_frames);
        source.clear();
        if (active)
        {
            source.channel(0)[frames - 1] = 1.0f;
            source.channel(1)[0] = -0.5f;
        }
        const auto bytes = out.byte_view();
        std::memset(bytes.data(), 0xcd, bytes.size());
        const std::uint8_t input_flag = active ? 1 : 0;
        std::uint8_t output_flag = input_flag;
        convert.process({tick_period, tick, {&in, 1}, {&out, 1}, {&input_flag, 1}, {&output_flag, 1}});
        reference.process_tick(source, ref, tick_period, tick);
        check(output_flag == expected_active, "Convert stays active through the last flush block");
        check(std::memcmp(bytes.data(), expected.data(), bytes.size()) == 0,
              "Convert matches uninterrupted resampling across the gap and on return");
    };

    constexpr std::uint64_t resume_tick = 12;
    run_block(0, true, true);
    expect_error(RetCode::EPARAM, [&] { convert.prepare(static_cast<BlockPeriod>(0)); });
    for (std::uint64_t tick = 1; tick < resume_tick; ++tick)
        run_block(tick, false, tick <= flush_blocks);
    run_block(resume_tick, true, true);
    convert.reset();
    reference.reset();
    run_block(resume_tick + 1, false, false);
    run_block(resume_tick + 2, true, true);
    convert.prepare(tick_period);
    reference.reset();
    run_block(resume_tick + 3, false, false);
}

void convert_activity_and_tail()
{
    check_convert_tail(BandWidth::Full, BandWidth::Wide, 1);
    check_convert_tail(BandWidth::Wide, BandWidth::Full, 3);
    check_convert_tail(BandWidth::QuarterCD, BandWidth::CDQuality, 5);

    const PortSpec input_spec(rate, 1, SampleFormat::F32P);
    const float zero[2]{};
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(zero), input_spec, 2);
    for (const auto format : {SampleFormat::F32P, SampleFormat::S16LE, SampleFormat::S24_3LE,
                              SampleFormat::S24LE, SampleFormat::S32LE, SampleFormat::F32LE})
    {
        const PortSpec output_spec(rate, 1, format);
        Convert convert(input_spec, output_spec);
        convert.prepare(period);
        float planar[2]{};
        std::uint8_t pcm[8]{};
        const OBBlock output(format == SampleFormat::F32P ? reinterpret_cast<std::uint8_t *>(planar) : pcm,
                             output_spec, 2);
        const auto bytes = output.byte_view();
        std::uint8_t input_flag = 1, output_flag = 0;
        convert.process({period, 0, {&input, 1}, {&output, 1}, {&input_flag, 1}, {&output_flag, 1}});
        check(output_flag == 1, "A real zero block stays active through same-rate conversion");
        input_flag = 0;
        std::memset(bytes.data(), 0xcd, bytes.size());
        convert.process({period, 1, {&input, 1}, {&output, 1}, {&input_flag, 1}, {&output_flag, 1}});
        check(!output_flag && std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
              "An inactive same-rate input clears the output bytes in every PCM format");
    }
}

void fractional_rate_and_antialias()
{
    for (const auto target : {BandWidth::CDQuality, BandWidth::Wide})
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
                                                {"matrix_and_mix_activity", matrix_and_mix_activity},
                                                {"mixer_priority_limits", mixer_priority_limits},
                                                {"mixer_priority_transitions", mixer_priority_transitions},
                                                {"mixer_priority_single_frame_and_reset", mixer_priority_single_frame_and_reset},
                                                {"fractional_rate_and_antialias", fractional_rate_and_antialias},
                                                {"convert_activity_and_tail", convert_activity_and_tail}});
}
