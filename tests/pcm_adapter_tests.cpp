#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/pcm_reference.h"
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
constexpr std::size_t block_frames = 240;

void pcm_format_conversion()
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
        Engine engine(rate, period);
        ManualClock clock(engine);
        const PCMSpec spec{rate, test.format, 2};
        const PCMSpec float_spec{rate, PCMFormat::F32LE, 2};
        const auto input_id = checked(engine.emplace_node<IAdapter>(spec.channels, engine.spec()));
        checked(checked(engine.node<IAdapter>(input_id))->configure_pcm(spec));
        const auto capture_id = checked(engine.emplace_node<OAdapter>(float_spec.channels, engine.spec()));
        checked(checked(engine.node<OAdapter>(capture_id))->configure_pcm(float_spec));
        const auto playback_id = checked(engine.emplace_node<IAdapter>(float_spec.channels, engine.spec()));
        checked(checked(engine.node<IAdapter>(playback_id))->configure_pcm(float_spec));
        const auto output_id = checked(engine.emplace_node<OAdapter>(spec.channels, engine.spec()));
        checked(checked(engine.node<OAdapter>(output_id))->configure_pcm(spec));
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
        checked(checked(engine.node<IAdapter>(input_id))->bind_pcm({input_pcm.data(), input_pcm.size()}));
        checked(checked(engine.node<OAdapter>(capture_id))->bind_pcm({decoded.data(), decoded.size()}));
        checked(checked(engine.node<IAdapter>(playback_id))->bind_pcm({graph_pcm.data(), graph_pcm.size()}));
        checked(checked(engine.node<OAdapter>(output_id))->bind_pcm({encoded.data(), encoded.size()}));
        checked(clock.tick());
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

void buffer_binding()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec spec{rate, PCMFormat::S16LE, 1};
    const auto input_id = checked(engine.emplace_node<IAdapter>(spec.channels, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(spec.channels, engine.spec()));
    checked(engine.connect({input_id}, {output_id}));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(clock.reset()); // Reset must also accept converters with no allocated history.
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
    checked(clock.tick());
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
    checked(engine.prepare());
    check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull &&
              output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::BufferFull,
          "A bound device period survives repeated preparation and cannot be replaced early");
    checked(clock.tick());
    check(packet == pcm, "An engine period writes directly to the bound output buffer");
    checked(clock.tick());
    check(packet == pcm && engine.frame_position() == 3 * block_frames,
          "The graph continues after the device buffers are released");
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(clock.tick());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Missing input produces silence");

    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(clock.reset());
    check(engine.frame_position() == 0, "Reset resets the graph clock");
    checked(input->bind_pcm({pcm.data(), pcm.size()}));
    checked(output->bind_pcm({packet.data(), packet.size()}));
    checked(clock.tick());
}

void unaligned_float_pcm()
{
    constexpr std::size_t periods = 2;
    constexpr auto frames = periods * block_frames;
    constexpr auto bytes = frames * sizeof(float);
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec spec{rate, PCMFormat::F32LE, 1, periods};
    const auto input_id = checked(engine.emplace_node<IAdapter>(spec.channels, engine.spec()));
    checked(checked(engine.node<IAdapter>(input_id))->configure_pcm(spec));
    const auto output_id = checked(engine.emplace_node<OAdapter>(spec.channels, engine.spec()));
    checked(checked(engine.node<OAdapter>(output_id))->configure_pcm(spec));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());

    alignas(float) std::array<std::uint8_t, bytes + 1> capture{};
    alignas(float) std::array<std::uint8_t, bytes + 1> playback{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        write_f32le(capture.data() + 1 + frame * sizeof(float), 1.25f + static_cast<float>(frame) / 256.0f);
    }
    checked(checked(engine.node<IAdapter>(input_id))->bind_pcm({capture.data() + 1, bytes}));
    checked(checked(engine.node<OAdapter>(output_id))->bind_pcm({playback.data() + 1, bytes}));
    for (std::size_t step = 0; step < periods; ++step)
    {
        checked(clock.tick());
    }
    check(playback == capture, "Same-rate mono F32LE preserves sample bytes in unaligned device buffers");
}

void channel_mapping()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const auto input_id = checked(engine.emplace_node<IAdapter>(2, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(2, engine.spec()));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());

    // Deliberately non-square matrices exercise both conversion directions.
    const ChMatrix capture_matrix(2, 3, {0, 1, 0, 0.5f, 0, 0.5f});
    const ChMatrix playback_matrix(4, 2, {1, 0, 0, 1, 1, -1, 0, 0});
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
        checked(clock.tick());
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
    const ChMatrix swap(2, 2, {0, 1, 1, 0});
    checked(input->configure_pcm({rate, PCMFormat::F32LE, 2}, swap));
    checked(output->configure_pcm({rate, PCMFormat::F32LE, 2}));
    captured.assign(input->pcm_bytes(), 0);
    playback.assign(output->pcm_bytes(), 0);
    write_f32le(captured.data(), 0.25f);
    write_f32le(captured.data() + 4, -0.5f);
    checked(input->bind_pcm({captured.data(), captured.size()}));
    checked(output->bind_pcm({playback.data(), playback.size()}));
    checked(clock.tick());
    check(read_f32le(playback.data()) == -0.5f && read_f32le(playback.data() + 4) == 0.25f &&
              std::all_of(playback.begin() + 8, playback.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Same-channel mapping replaces the previous converter without adding delay or retaining a tail");
}

void invalid_pcm_configuration()
{
    IAdapter input(2, {rate, period});
    OAdapter output(2, {rate, period});
    const ChMatrix mono_to_stereo(2, 1, {1, -0.5f});
    const ChMatrix stereo_to_mono(1, 2, {0.25f, 0.75f});
    const PCMSpec mono{SampleRate::Hz16000, PCMFormat::S16LE, 1};
    check(input.configure_pcm(mono).code() == ErrorCode::ChannelMismatch &&
              output.configure_pcm(mono).code() == ErrorCode::ChannelMismatch &&
              input.configure_pcm(mono, stereo_to_mono).code() == ErrorCode::ChannelMismatch &&
              output.configure_pcm(mono, mono_to_stereo).code() == ErrorCode::ChannelMismatch,
          "Unequal channels require explicit matrices with the correct direction");
    check(input.pcm_bytes() == 0 && output.pcm_bytes() == 0,
          "Failed first configuration does not publish PCM state");
    checked(input.configure_pcm(mono, mono_to_stereo));
    checked(output.configure_pcm(mono, stereo_to_mono));
    const PCMSpec invalid_specs[] = {
        {SampleRate::Hz16000, static_cast<PCMFormat>(99), 1},
        {static_cast<SampleRate>(0), PCMFormat::S16LE, 1},
        {static_cast<SampleRate>(48001), PCMFormat::S16LE, 1},
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
    IAdapter zero_channels(0, {rate, period});
    check(zero_channels.configure_pcm(mono, mono_to_stereo).code() == ErrorCode::InvalidArg,
          "Standalone adapters reject zero logical channels");
}

void graph_timing_mismatch()
{
    Engine invalid_graph(rate, static_cast<BlockPeriod>(0));
    IAdapter standalone(1, invalid_graph.spec());
    expect_error(ErrorCode::InvalidArg, standalone.configure_pcm({rate, PCMFormat::F32LE, 1}));

    Engine engine(rate, period);
    ManualClock clock(engine);
    const EngineSpec other_spec(SampleRate::Hz24000, BlockPeriod::Ms10);
    const auto frames = engine.spec().block_frames();
    check(frames == block_frames && frames == other_spec.block_frames(),
          "Equal frame counts can represent different graph rates and periods");
    const auto output = checked(engine.emplace_node<OAdapter>(1, engine.spec()));
    const auto wrong_input = checked(engine.emplace_node<IAdapter>(1, other_spec));
    checked(engine.connect({wrong_input}, {output}));
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    expect_error(ErrorCode::NotPrepared, clock.tick());
    checked(engine.remove_node(wrong_input));

    const auto input = checked(engine.emplace_node<IAdapter>(1, engine.spec()));
    checked(checked(engine.node<IAdapter>(input))->configure_pcm({rate, PCMFormat::F32LE, 1}));
    checked(engine.connect({input}, {output}));
    const auto wrong_output = checked(engine.emplace_node<OAdapter>(1, other_spec));
    checked(engine.connect({input}, {wrong_output}));
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    expect_error(ErrorCode::NotPrepared, clock.tick());
    checked(engine.remove_node(wrong_output));
    checked(engine.prepare());
    checked(clock.tick());
    check(engine.frame_position() == block_frames, "Preparing the corrected graph makes it executable");
}

void zero_contributions()
{
    const ChMatrix selector(2, 2, {0, 1, 0, 0});
    for (const bool map_at_input : {false, true})
    {
        Engine engine(rate, period);
        ManualClock clock(engine);
        const auto input_id = checked(engine.emplace_node<IAdapter>(2, engine.spec()));
        const auto output_id = checked(engine.emplace_node<OAdapter>(2, engine.spec()));
        auto *input = checked(engine.node<IAdapter>(input_id));
        auto *output = checked(engine.node<OAdapter>(output_id));
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
            checked(clock.tick());
            check(read_f32le(packet.data()) == 0.5f * (gain / block_frames) &&
                      std::all_of(packet.begin() + 4, packet.end(), [](std::uint8_t byte) { return byte == 0; }),
                  "Zero gains and zero matrix coefficients suppress non-finite unused channels");
        }
    }
}

void integer_sample_limits()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const auto input_id = checked(engine.emplace_node<IAdapter>(1, engine.spec()));
    const auto output_id = checked(engine.emplace_node<OAdapter>(1, engine.spec()));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
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
    checked(clock.tick());
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
        {"pcm_format_conversion", pcm_format_conversion},
        {"buffer_binding", buffer_binding},
        {"graph_timing_mismatch", graph_timing_mismatch},
        {"unaligned_float_pcm", unaligned_float_pcm},
        {"channel_mapping", channel_mapping},
        {"invalid_pcm_configuration", invalid_pcm_configuration},
        {"zero_contributions", zero_contributions}, {"integer_sample_limits", integer_sample_limits}
    });
}
