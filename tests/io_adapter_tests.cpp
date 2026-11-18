#include "checked.h"
#include "core/engine.h"
#include "nodes/buffer_sink.h"
#include "nodes/buffer_source.h"
#include "plugins/adapter.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace
{
using namespace aereborn;
using aereborn::test::BufferSinkNode;
using aereborn::test::BufferSourceNode;
using aereborn::test::checked;

constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
using TestEngine = Engine<rate, period>;
using Input = IAdapter<rate, period>;
using Output = OAdapter<rate, period>;
using Source = BufferSourceNode<rate, period>;
using Sink = BufferSinkNode<rate, period>;
constexpr auto block_frames = TestEngine::block_frames();
constexpr double two_pi = 6.283185307179586;

void check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void test_formats()
{
    struct FormatCase
    {
        PCMFormat format;
        std::vector<std::uint8_t> pcm;
        float peak;
    };
    const FormatCase cases[] = {
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
        const auto input_id = checked(engine.emplace_node<Input>(spec));
        const auto sink_id = checked(engine.emplace_node<Sink>(2));
        const auto source_id = checked(engine.emplace_node<Source>(2));
        const auto output_id = checked(engine.emplace_node<Output>(spec));
        checked(engine.connect({input_id}, {sink_id}));
        checked(engine.connect({source_id}, {output_id}));
        checked(engine.prepare());

        auto input_pcm = test.pcm;
        if (test.format == PCMFormat::S24LE)
        {
            for (std::size_t i = 3; i < input_pcm.size(); i += 4)
            {
                input_pcm[i] = 0xa5;
            }
        }
        auto *input = checked(engine.node<Input>(input_id));
        input_pcm.resize(test.pcm.size() * block_frames / 2);
        checked(input->bind_pcm({input_pcm.data(), input_pcm.size()}));
        const float graph_samples[] = {test.format == PCMFormat::F32LE ? -1.0f : -2.0f, 0.5f, 0.25f, 1.5f};
        checked(checked(engine.node<Source>(source_id))->push_interleaved(graph_samples, 2));
        auto *output = checked(engine.node<Output>(output_id));
        std::vector<std::uint8_t> encoded(input_pcm.size());
        checked(output->bind_pcm({encoded.data(), encoded.size()}));
        checked(engine.process());
        const auto block = checked(engine.node<Sink>(sink_id))->block();
        check(block.frames() == block_frames && block.channel(0)[0] == -1.0f && block.channel(0)[1] == 0.25f &&
                  block.channel(1)[0] == 0.5f && block.channel(1)[1] == test.peak &&
                  block.channel(0)[2] == 0.0f && block.channel(1)[block_frames - 1] == 0.0f,
              "PCM decode, interleaving, or zero padding");

        auto expected = test.pcm;
        expected.resize(encoded.size());
        check(encoded == expected, "PCM encode, interleaving, or saturation");
    }
}

void test_period_staging()
{
    TestEngine engine;
    const PCMSpec spec{rate, PCMFormat::S16LE, 1};
    const auto input_id = checked(engine.emplace_node<Input>(spec));
    const auto output_id = checked(engine.emplace_node<Output>(spec));
    checked(engine.connect({input_id}, {output_id}));
    auto *input = checked(engine.node<Input>(input_id));
    auto *output = checked(engine.node<Output>(output_id));
    std::array<std::uint8_t, block_frames * 2> pcm;
    for (std::size_t i = 0; i < pcm.size(); ++i)
    {
        pcm[i] = static_cast<std::uint8_t>(i);
    }
    std::array<std::uint8_t, block_frames * 2> packet;
    check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::NotPrepared &&
              output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::NotPrepared,
          "I/O endpoints require prepare");
    checked(engine.prepare());
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
    check(packet == pcm && engine.frame_position() == 2 * block_frames,
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

void test_device_cadence()
{
    constexpr std::size_t input_periods = 2;
    constexpr std::size_t output_periods = 3;
    const PCMSpec input_spec{SampleRate::Hz44100, PCMFormat::S16LE, 1, input_periods};
    const PCMSpec output_spec{rate, PCMFormat::S16LE, 1, output_periods};
    Input invalid_period({input_spec.sample_rate, input_spec.format, input_spec.channels});
    check(invalid_period.prepare().code() == ErrorCode::InvalidArg, "One 5 ms period cannot hold integer 44.1 kHz frames");
    Output invalid_rate({static_cast<SampleRate>(48001), PCMFormat::S16LE, 1});
    Input zero_periods({rate, PCMFormat::S16LE, 1, 0});
    check(invalid_rate.prepare().code() == ErrorCode::InvalidArg && zero_periods.prepare().code() == ErrorCode::InvalidArg,
          "Require a listed PCM rate and a positive period multiplier");

    TestEngine engine;
    const auto input_id = checked(engine.emplace_node<Input>(input_spec));
    const auto output_id = checked(engine.emplace_node<Output>(output_spec));
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

void write_float(std::uint8_t *bytes, float sample)
{
    std::uint32_t word;
    std::memcpy(&word, &sample, sizeof(word));
    for (unsigned int i = 0; i < 4; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>(word >> (8 * i));
    }
}

float read_float(const std::uint8_t *bytes)
{
    const auto word = std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8) |
                      (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
    float sample;
    std::memcpy(&sample, &word, sizeof(sample));
    return sample;
}

void test_f32_copy()
{
    constexpr std::size_t periods = 2;
    constexpr auto frames = periods * block_frames;
    constexpr auto bytes = frames * sizeof(float);
    TestEngine engine;
    const PCMSpec spec{rate, PCMFormat::F32LE, 1, periods};
    const auto input_id = checked(engine.emplace_node<Input>(spec));
    const auto output_id = checked(engine.emplace_node<Output>(spec));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());

    alignas(float) std::array<std::uint8_t, bytes + 1> capture{};
    alignas(float) std::array<std::uint8_t, bytes + 1> playback{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        write_float(capture.data() + 1 + frame * sizeof(float), 1.25f + static_cast<float>(frame) / 256.0f);
    }
    checked(checked(engine.node<Input>(input_id))->bind_pcm({capture.data() + 1, bytes}));
    checked(checked(engine.node<Output>(output_id))->bind_pcm({playback.data() + 1, bytes}));
    for (std::size_t step = 0; step < periods; ++step)
    {
        checked(engine.process());
    }
    check(playback == capture, "Same-rate mono F32LE preserves sample bytes in unaligned device buffers");
}

template <SampleRate EngineRate, BlockPeriod EnginePeriod = period>
std::vector<float> input_resample(SampleRate source_rate, std::size_t pcm_frames, std::size_t periods_per_transfer)
{
    using Input = IAdapter<EngineRate, EnginePeriod>;
    using Capture = BufferSinkNode<EngineRate, EnginePeriod>;
    using EngineType = Engine<EngineRate, EnginePeriod>;
    EngineType engine;
    constexpr auto graph_frames = EngineType::block_frames();
    const auto source_hz = static_cast<std::uint32_t>(source_rate);
    const PCMSpec spec{source_rate, PCMFormat::F32LE, 2, periods_per_transfer};
    const auto input_id = checked(engine.template emplace_node<Input>(spec));
    const auto sink_id = checked(engine.template emplace_node<Capture>(2));
    checked(engine.connect({input_id}, {sink_id}));
    checked(engine.prepare());
    auto *input = checked(engine.template node<Input>(input_id));
    auto *sink = checked(engine.template node<Capture>(sink_id));
    check(input->pcm_frames() == pcm_frames, "Input requests a fixed integer-period transfer");

    std::vector<std::uint8_t> pcm(pcm_frames * 8);
    std::vector<float> converted;
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        for (std::size_t i = 0; i < pcm_frames; ++i)
        {
            const auto sample = std::sin(two_pi * 1000 * (transfer * pcm_frames + i) / source_hz);
            write_float(pcm.data() + 8 * i, static_cast<float>(sample));
            write_float(pcm.data() + 8 * i + 4, static_cast<float>(-0.5 * sample));
        }
        checked(input->bind_pcm({pcm.data(), pcm.size()}));
        for (std::size_t step = 0; step < periods_per_transfer; ++step)
        {
            checked(engine.process());
            const auto block = sink->block();
            check(block.frames() == graph_frames, "Input resampling fills a whole graph period");
            for (std::size_t i = 0; i < graph_frames; ++i)
            {
                check(std::abs(block.channel(1)[i] + 0.5f * block.channel(0)[i]) < 0.000001f,
                      "Resampling keeps separate filter history for each channel");
            }
            converted.insert(converted.end(), block.channel(0), block.channel(0) + graph_frames);
            if (transfer == 0 && step == 0 && periods_per_transfer > 1)
            {
                engine.reset();
                checked(input->bind_pcm({pcm.data(), pcm.size()}));
                checked(engine.process());
                check(std::equal(converted.begin(), converted.end(), sink->block().channel(0)),
                      "Reset during a device transfer clears fractional phase and filter history");
            }
            if (step + 1 < periods_per_transfer)
            {
                check(input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull,
                      "The graph must consume every period before the next PCM transfer");
            }
        }
    }
    check(engine.frame_position() == converted.size(), "Input transfer leaves the graph period unchanged");
    engine.reset();
    checked(engine.process());
    const auto reset_block = sink->block();
    check(std::all_of(reset_block.channel(0), reset_block.channel(0) + graph_frames,
                      [](float sample) { return sample == 0.0f; }),
          "Reset discards pending audio and filter history");
    return converted;
}

template <SampleRate EngineRate = rate, BlockPeriod EnginePeriod = period>
std::vector<float> output_resample(SampleRate target_rate, double frequency, std::size_t pcm_frames,
                                   std::size_t periods_per_transfer)
{
    using Source = BufferSourceNode<EngineRate, EnginePeriod>;
    using Output = OAdapter<EngineRate, EnginePeriod>;
    using EngineType = Engine<EngineRate, EnginePeriod>;
    constexpr auto graph_hz = static_cast<std::uint32_t>(EngineRate);
    constexpr auto graph_frames = EngineType::block_frames();
    EngineType engine;
    const auto source_id = checked(engine.template emplace_node<Source>(1));
    const PCMSpec spec{target_rate, PCMFormat::F32LE, 1, periods_per_transfer};
    const auto output_id = checked(engine.template emplace_node<Output>(spec));
    checked(engine.connect({source_id}, {output_id}));
    checked(engine.prepare());
    auto *source = checked(engine.template node<Source>(source_id));
    auto *output = checked(engine.template node<Output>(output_id));
    check(output->pcm_frames() == pcm_frames, "Output advertises the configured device frame count");

    std::vector<float> converted;
    std::vector<std::uint8_t> pcm(4 * pcm_frames);
    std::size_t frame = 0;
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(output->bind_pcm({pcm.data(), pcm.size()}));
        for (std::size_t step = 0; step < periods_per_transfer; ++step)
        {
            float samples[graph_frames];
            for (auto &sample : samples)
            {
                sample = static_cast<float>(std::sin(two_pi * frequency * frame++ / graph_hz));
            }
            checked(source->push_interleaved(samples, graph_frames));
            checked(engine.process());
            if (step + 1 < periods_per_transfer)
            {
                check(output->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull,
                      "The output remains bound until the device period ends");
            }
        }
        for (std::size_t i = 0; i < pcm_frames; ++i)
        {
            converted.push_back(read_float(pcm.data() + 4 * i));
        }
    }
    return converted;
}

double rms(const std::vector<float> &samples)
{
    double energy = 0;
    for (std::size_t i = 96; i < samples.size(); ++i)
    {
        energy += samples[i] * samples[i];
    }
    return std::sqrt(energy / (samples.size() - 96));
}

void check_continuity(const std::vector<float> &samples, std::uint32_t source_rate, std::uint32_t target_rate)
{
    const auto first = 96 * target_rate / source_rate + 1;
    for (std::size_t i = first; i < samples.size(); ++i)
    {
        const auto seconds = static_cast<double>(i) / target_rate - 48.0 / source_rate;
        const auto expected = std::sin(two_pi * 1000 * seconds);
        check(std::abs(samples[i] - expected) < 0.01, "Resampling stays phase-continuous after the FIR delay");
    }
}

void test_resampling()
{
    check_continuity(input_resample<rate>(SampleRate::Hz44100, 441, 2), 44100, 48000);
    check_continuity(output_resample(SampleRate::Hz44100, 1000, 441, 2), 48000, 44100);

    constexpr auto reverse_rate = SampleRate::Hz44100;
    constexpr auto reverse_period = BlockPeriod::Ms10;
    check_continuity(input_resample<reverse_rate, reverse_period>(rate, 480, 1), 48000, 44100);
    check_continuity(output_resample<reverse_rate, reverse_period>(rate, 1000, 480, 1), 44100, 48000);

    check_continuity(input_resample<rate>(SampleRate::Hz11025, 441, 8), 11025, 48000);
    check_continuity(input_resample<SampleRate::Hz96000>(SampleRate::Hz8000, 40, 1), 8000, 96000);
    check_continuity(input_resample<SampleRate::Hz8000>(SampleRate::Hz96000, 480, 1), 96000, 8000);

    const auto passband = output_resample(SampleRate::Hz16000, 1000, 80, 1);
    const auto stopband = output_resample(SampleRate::Hz16000, 12000, 80, 1);
    check(rms(passband) > 0.69 && rms(stopband) < 0.01 * rms(passband), "Downsampling must suppress aliasing");
}
} // namespace

int main()
{
    try
    {
        test_formats();
        test_f32_copy();
        test_period_staging();
        test_device_cadence();
        test_resampling();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
