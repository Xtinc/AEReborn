#include "checked.h"
#include "pcm.h"
#include "core/engine.h"
#include "plugins/adapter.h"

#include <array>
#include <limits>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
using TestEngine = Engine<rate, period>;
using Input = IAdapter<rate, period>;
using Output = OAdapter<rate, period>;
constexpr auto block_frames = TestEngine::block_frames();

void formats()
{
    const struct
    {
        PCMFormat format;
        std::vector<std::uint8_t> pcm;
        float peak;
    } cases[] = {
        {PCMFormat::S16LE, {0, 0x80, 0, 0x40, 0, 0x20, 0xff, 0x7f}, 32767.0f / 32768.0f},
        {PCMFormat::S24_3LE, {0, 0, 0x80, 0, 0, 0x40, 0, 0, 0x20, 0xff, 0xff, 0x7f}, 8388607.0f / 8388608.0f},
        {PCMFormat::S24LE, {0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0, 0xff, 0xff, 0x7f, 0}, 8388607.0f / 8388608.0f},
        {PCMFormat::S32LE, {0, 0, 0, 0x80, 0, 0, 0, 0x40, 0, 0, 0, 0x20, 0xff, 0xff, 0xff, 0x7f}, 1.0f},
        {PCMFormat::F32LE, {0, 0, 0x80, 0xbf, 0, 0, 0, 0x3f, 0, 0, 0x80, 0x3e, 0, 0, 0xc0, 0x3f}, 1.5f},
    };
    for (const auto &test : cases)
    {
        TestEngine engine;
        const PCMSpec spec{rate, test.format, 2};
        const PCMSpec float_spec{rate, PCMFormat::F32LE, 2};
        const auto input_id = checked(engine.emplace_node<Input>(spec.channels));
        checked(checked(engine.node<Input>(input_id))->configure_pcm(spec));
        const auto capture_id = checked(engine.emplace_node<Output>(float_spec.channels));
        checked(checked(engine.node<Output>(capture_id))->configure_pcm(float_spec));
        const auto playback_id = checked(engine.emplace_node<Input>(float_spec.channels));
        checked(checked(engine.node<Input>(playback_id))->configure_pcm(float_spec));
        const auto output_id = checked(engine.emplace_node<Output>(spec.channels));
        checked(checked(engine.node<Output>(output_id))->configure_pcm(spec));
        checked(engine.connect({input_id}, {capture_id}));
        checked(engine.connect({playback_id}, {output_id}));
        checked(engine.prepare());
        auto input_pcm = test.pcm;
        if (test.format == PCMFormat::S24LE)
        {
            for (std::size_t byte = 3; byte < input_pcm.size(); byte += 4)
            {
                input_pcm[byte] = 0xa5;
            }
        }
        input_pcm.resize(block_frames * 2 * pcm_sample_bytes(test.format));
        std::vector<std::uint8_t> graph_pcm(block_frames * 8);
        const float samples[] = {test.format == PCMFormat::F32LE ? -1.0f : -2.0f, 0.5f, 0.25f, 1.5f};
        for (std::size_t i = 0; i < 4; ++i)
        {
            write_f32le(graph_pcm.data() + i * 4, samples[i]);
        }
        std::vector<std::uint8_t> decoded(graph_pcm.size());
        std::vector<std::uint8_t> encoded(input_pcm.size());
        checked(checked(engine.node<Input>(input_id))->bind_pcm({input_pcm.data(), input_pcm.size()}));
        checked(checked(engine.node<Output>(capture_id))->bind_pcm({decoded.data(), decoded.size()}));
        checked(checked(engine.node<Input>(playback_id))->bind_pcm({graph_pcm.data(), graph_pcm.size()}));
        checked(checked(engine.node<Output>(output_id))->bind_pcm({encoded.data(), encoded.size()}));
        checked(engine.process());
        const float expected_samples[] = {-1.0f, 0.5f, 0.25f, test.peak};
        for (std::size_t i = 0; i < decoded.size() / 4; ++i)
        {
            check(read_f32le(decoded.data() + i * 4) == (i < 4 ? expected_samples[i] : 0.0f),
                  "PCM decoding preserves channel order, storage width and zero samples");
        }
        auto expected_pcm = test.pcm;
        expected_pcm.resize(encoded.size());
        check(encoded == expected_pcm, "PCM encoding applies saturation and writes zero storage padding");
    }
}

void binding()
{
    TestEngine engine;
    const PCMSpec spec{rate, PCMFormat::S16LE, 1};
    const auto input_id = checked(engine.emplace_node<Input>(spec.channels));
    const auto output_id = checked(engine.emplace_node<Output>(spec.channels));
    checked(engine.connect({input_id}, {output_id}));
    auto *input = checked(engine.node<Input>(input_id));
    auto *output = checked(engine.node<Output>(output_id));
    engine.reset(); // Reset must also accept converters with no allocated history.
    std::array<std::uint8_t, block_frames * 2> pcm;
    for (std::size_t i = 0; i < pcm.size(); ++i)
    {
        pcm[i] = static_cast<std::uint8_t>(i);
    }
    std::array<std::uint8_t, block_frames * 2> packet;
    check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::NotPrepared &&
              output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::NotPrepared,
          "I/O endpoints require PCM configuration");
    checked(engine.prepare());
    check(input->pcm_frames() == 0 && input->pcm_bytes() == 0 &&
              output->pcm_frames() == 0 && output->pcm_bytes() == 0,
          "Unconfigured adapters report no device buffer requirements");
    checked(engine.process());
    check(engine.frame_position() == block_frames &&
              input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::NotPrepared &&
              output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::NotPrepared,
          "An unconfigured graph runs without implicitly configuring devices");
    checked(input->configure_pcm(spec));
    checked(output->configure_pcm(spec));
    check(input->pcm_frames() == block_frames && output->pcm_frames() == block_frames,
          "Same-rate transfers use the specified device period");
    check(input->bind_pcm({pcm.data(), pcm.size() - 2}).code() == ErrorCode::InvalidArg &&
              output->bind_pcm({packet.data(), packet.size() - 2}).code() == ErrorCode::InvalidArg,
          "Reject buffers shorter than a device period");

    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(input->prepare());
    checked(output->prepare());
    check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull &&
              output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::BufferFull,
          "A bound device period survives repeated preparation and cannot be replaced early");
    checked(engine.process());
    check(packet == pcm, "An engine period writes directly to the bound output buffer");
    checked(engine.process());
    check(packet == pcm && engine.frame_position() == 3 * block_frames,
          "The graph continues after the device buffers are released");
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(engine.process());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Missing input produces silence");

    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    engine.reset();
    check(engine.frame_position() == 0, "Reset resets the graph clock");
    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(engine.process());
}

void cadence()
{
    constexpr std::size_t input_periods = 2;
    constexpr std::size_t output_periods = 3;
    const PCMSpec input_spec{SampleRate::Hz44100, PCMFormat::S16LE, 1, input_periods};
    const PCMSpec output_spec{rate, PCMFormat::S16LE, 1, output_periods};
    Input invalid_period(1);
    check(invalid_period.configure_pcm({input_spec.sample_rate, input_spec.format, input_spec.channels}).code() ==
              ErrorCode::InvalidArg, "One 5 ms period cannot hold integer 44.1 kHz frames");
    Output invalid_rate(1);
    Input zero_periods(1);
    check(invalid_rate.configure_pcm({static_cast<SampleRate>(48001), PCMFormat::S16LE, 1}).code() == ErrorCode::InvalidArg &&
              zero_periods.configure_pcm({rate, PCMFormat::S16LE, 1, 0}).code() == ErrorCode::InvalidArg,
          "Require a listed PCM rate and a positive period multiplier");

    TestEngine engine;
    const auto input_id = checked(engine.emplace_node<Input>(input_spec.channels));
    checked(checked(engine.node<Input>(input_id))->configure_pcm(input_spec));
    const auto output_id = checked(engine.emplace_node<Output>(output_spec.channels));
    checked(checked(engine.node<Output>(output_id))->configure_pcm(output_spec));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    auto *input = checked(engine.node<Input>(input_id));
    auto *output = checked(engine.node<Output>(output_id));
    check(input->pcm_frames() == 441 && output->pcm_frames() == 720, "Compute device frame counts from period multiples");

    std::array<std::uint8_t, 441 * 2> captured{};
    std::array<std::uint8_t, 720 * 2> playback;
    std::size_t reads = 0;
    std::size_t writes = 0;
    for (std::size_t tick = 0; tick < 6; ++tick)
    {
        if (tick % input_periods == 0)
        {
            ++reads;
            checked(input->bind_pcm({captured.data(), captured.size()}));
        }
        if (tick % output_periods == 0)
        {
            playback.fill(0xff);
            checked(output->bind_pcm({playback.data(), playback.size()}));
        }
        checked(engine.process());
        if ((tick + 1) % output_periods == 0)
        {
            ++writes;
            check(std::all_of(playback.begin(), playback.end(), [](std::uint8_t byte) { return byte == 0; }),
                  "The device writes a complete bound buffer at its period boundary");
        }
    }
    check(reads == 3 && writes == 2 && engine.frame_position() == 6 * block_frames,
          "Device transfers follow a single base-period counter");
}

void float_copy()
{
    constexpr std::size_t periods = 2;
    constexpr auto frames = periods * block_frames;
    constexpr auto bytes = frames * sizeof(float);
    TestEngine engine;
    const PCMSpec spec{rate, PCMFormat::F32LE, 1, periods};
    const auto input_id = checked(engine.emplace_node<Input>(spec.channels));
    checked(checked(engine.node<Input>(input_id))->configure_pcm(spec));
    const auto output_id = checked(engine.emplace_node<Output>(spec.channels));
    checked(checked(engine.node<Output>(output_id))->configure_pcm(spec));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());

    alignas(float) std::array<std::uint8_t, bytes + 1> capture{};
    alignas(float) std::array<std::uint8_t, bytes + 1> playback{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        write_f32le(capture.data() + 1 + frame * sizeof(float), 1.25f + static_cast<float>(frame) / 256.0f);
    }
    checked(checked(engine.node<Input>(input_id))->bind_pcm({capture.data() + 1, bytes}));
    checked(checked(engine.node<Output>(output_id))->bind_pcm({playback.data() + 1, bytes}));
    for (std::size_t step = 0; step < periods; ++step)
    {
        checked(engine.process());
    }
    check(playback == capture, "Same-rate mono F32LE preserves sample bytes in unaligned device buffers");
}

void channel_mapping()
{
    TestEngine engine;
    const auto input_id = checked(engine.emplace_node<Input>(2));
    const auto output_id = checked(engine.emplace_node<Output>(2));
    auto *input = checked(engine.node<Input>(input_id));
    auto *output = checked(engine.node<Output>(output_id));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());

    // Deliberately non-square matrices exercise both conversion directions.
    const auto capture_matrix = checked(ChMatrix::create(2, 3, {0, 1, 0, 0.5f, 0, 0.5f}));
    const auto playback_matrix = checked(ChMatrix::create(4, 2, {1, 0, 0, 1, 1, -1, 0, 0}));
    checked(input->configure_pcm({rate, PCMFormat::S16LE, 3, 2}, capture_matrix));
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 4, 2}, playback_matrix));
    std::vector<std::uint8_t> captured(input->pcm_bytes());
    std::vector<std::uint8_t> playback(output->pcm_bytes(), 0xcd);
    const auto impulse = [](std::size_t frame) {
        return frame == 0 || frame == block_frames - 1 || frame == block_frames;
    };
    for (std::size_t frame = 0; frame < 2 * block_frames; ++frame)
    {
        const std::int16_t levels[] = {-16384, 8192, 24576};
        for (std::size_t ch = 0; ch < 3; ++ch)
        {
            const auto word = static_cast<std::uint16_t>(impulse(frame) ? levels[ch] : 0);
            captured[frame * 6 + ch * 2] = static_cast<std::uint8_t>(word);
            captured[frame * 6 + ch * 2 + 1] = static_cast<std::uint8_t>(word >> 8);
        }
    }
    checked(input->bind_pcm({captured.data(), captured.size()}));
    checked(output->bind_pcm({playback.data(), playback.size()}));
    for (std::size_t step = 0; step < 2; ++step)
    {
        checked(engine.process());
        const float expected[] = {0.25f, 0.125f, 0.125f, 0};
        for (std::size_t frame = step * block_frames; frame < (step + 1) * block_frames; ++frame)
        {
            for (std::size_t ch = 0; ch < 4; ++ch)
            {
                check(read_f32le(playback.data() + frame * 16 + ch * 4) == (impulse(frame) ? expected[ch] : 0),
                      "Channel mapping and PCM conversion write the current period without a frame of delay");
            }
        }
        if (step == 0)
        {
            check(std::all_of(playback.begin() + block_frames * 16, playback.end(),
                              [](std::uint8_t byte) { return byte == 0xcd; }),
                  "A multi-period transfer does not defer the first period or overwrite future periods");
        }
    }

    // Explicit matrices also apply when channel counts happen to match.
    const auto swap = checked(ChMatrix::create(2, 2, {0, 1, 1, 0}));
    checked(input->configure_pcm({rate, PCMFormat::F32LE, 2}, swap));
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 2}));
    captured.assign(input->pcm_bytes(), 0);
    playback.assign(output->pcm_bytes(), 0);
    write_f32le(captured.data(), 0.25f);
    write_f32le(captured.data() + 4, -0.5f);
    checked(input->bind_pcm({captured.data(), captured.size()}));
    checked(output->bind_pcm({playback.data(), playback.size()}));
    checked(engine.process());
    check(read_f32le(playback.data()) == -0.5f && read_f32le(playback.data() + 4) == 0.25f &&
              std::all_of(playback.begin() + 8, playback.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Same-channel mapping replaces the previous converter without adding delay or retaining a tail");
}

void configuration_errors()
{
    Input input(2);
    Output output(2);
    const auto mono_to_stereo = checked(ChMatrix::create(2, 1, {1, -0.5f}));
    const auto stereo_to_mono = checked(ChMatrix::create(1, 2, {0.25f, 0.75f}));
    const PCMSpec mono{SampleRate::Hz16000, PCMFormat::S16LE, 1};
    check(input.configure_pcm(mono).code() == ErrorCode::ChannelMismatch &&
              output.configure_pcm(mono).code() == ErrorCode::ChannelMismatch &&
              input.configure_pcm(mono, stereo_to_mono).code() == ErrorCode::ChannelMismatch &&
              output.configure_pcm(mono, mono_to_stereo).code() == ErrorCode::ChannelMismatch,
          "Unequal channels require explicit matrices with the correct direction");
    checked(input.prepare());
    checked(output.prepare());
    check(input.pcm_bytes() == 0 && output.pcm_bytes() == 0,
          "Failed first configuration does not publish PCM state");
    checked(input.configure_pcm(mono, mono_to_stereo));
    checked(output.configure_pcm(mono, stereo_to_mono));
    const PCMSpec invalid_specs[] = {
        {SampleRate::Hz16000, static_cast<PCMFormat>(99), 1},
        {static_cast<SampleRate>(0), PCMFormat::S16LE, 1},
        {SampleRate::Hz16000, PCMFormat::S16LE, 0},
        {SampleRate::Hz16000, PCMFormat::S16LE, 1, 0},
        {SampleRate::Hz44100, PCMFormat::S16LE, 1},
    };
    for (const auto spec : invalid_specs)
    {
        check(input.configure_pcm(spec, mono_to_stereo).code() == ErrorCode::InvalidArg &&
                  output.configure_pcm(spec, stereo_to_mono).code() == ErrorCode::InvalidArg,
              "Invalid PCM configurations are rejected without replacing the active converter");
    }
    const PCMSpec oversized{SampleRate::Hz16000, PCMFormat::S16LE, 1,
                            std::numeric_limits<std::size_t>::max()};
    check(input.configure_pcm(oversized, mono_to_stereo).code() == ErrorCode::TooLarge &&
              output.configure_pcm(oversized, stereo_to_mono).code() == ErrorCode::TooLarge &&
              input.pcm_frames() == 80 && input.pcm_bytes() == 160 &&
              output.pcm_frames() == 80 && output.pcm_bytes() == 160,
          "Capacity errors preserve the active device buffer requirements");
    Input zero_channels(0);
    check(zero_channels.configure_pcm(mono, mono_to_stereo).code() == ErrorCode::InvalidArg,
          "Standalone adapters reject zero logical channels");
}

void zero_contributions()
{
    const auto selector = checked(ChMatrix::create(2, 2, {0, 1, 0, 0}));
    for (const bool map_at_input : {false, true})
    {
        TestEngine engine;
        const auto input_id = checked(engine.emplace_node<Input>(2));
        const auto output_id = checked(engine.emplace_node<Output>(2));
        auto *input = checked(engine.node<Input>(input_id));
        auto *output = checked(engine.node<Output>(output_id));
        const PCMSpec spec{rate, PCMFormat::F32LE, 2};
        checked(input->configure_pcm(spec, map_at_input ? selector : ChMatrix{}));
        checked(output->configure_pcm(spec));
        const auto connection = checked(engine.connect({input_id}, {output_id},
                                                       {0, false, map_at_input ? ChMatrix{} : selector}));
        checked(engine.prepare());
        std::vector<std::uint8_t> pcm(input->pcm_bytes());
        std::vector<std::uint8_t> packet(output->pcm_bytes());
        write_f32le(pcm.data(), std::numeric_limits<float>::quiet_NaN());
        write_f32le(pcm.data() + 4, 0.5f);
        for (const float gain : {0.0f, 1.0f})
        {
            checked(engine.set_gain(connection, gain));
            checked(input->bind_pcm({pcm.data(), pcm.size()}));
            checked(output->bind_pcm({packet.data(), packet.size()}));
            checked(engine.process());
            check(read_f32le(packet.data()) == 0.5f * gain &&
                      std::all_of(packet.begin() + 4, packet.end(), [](std::uint8_t byte) { return byte == 0; }),
                  "Zero gains and zero matrix coefficients suppress non-finite unused channels");
        }
    }
}

void integer_limits()
{
    TestEngine engine;
    const auto input_id = checked(engine.emplace_node<Input>(1));
    const auto output_id = checked(engine.emplace_node<Output>(1));
    auto *input = checked(engine.node<Input>(input_id));
    auto *output = checked(engine.node<Output>(output_id));
    checked(input->configure_pcm({rate, PCMFormat::F32LE, 1}));
    checked(output->configure_pcm({rate, PCMFormat::S16LE, 1}));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    std::vector<std::uint8_t> pcm(input->pcm_bytes());
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    const float samples[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 2, -2};
    const std::uint16_t expected[] = {0, 0x7fff, 0x8000, 0x7fff, 0x8000};
    for (std::size_t i = 0; i < 5; ++i)
    {
        write_f32le(pcm.data() + i * 4, samples[i]);
    }
    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(engine.process());
    for (std::size_t frame = 0; frame < block_frames; ++frame)
    {
        const auto word = static_cast<std::uint16_t>(packet[frame * 2]) |
                          (static_cast<std::uint16_t>(packet[frame * 2 + 1]) << 8);
        check(word == (frame < 5 ? expected[frame] : 0), "Integer conversion maps NaN to zero and saturates infinities and overloads");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"formats", formats},
        {"binding", binding},
        {"cadence", cadence},
        {"float_copy", float_copy},
        {"channel_mapping", channel_mapping},
        {"configuration_errors", configuration_errors},
        {"zero_contributions", zero_contributions}, {"integer_limits", integer_limits}
    });
}
