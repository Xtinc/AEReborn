#include "checked.h"
#include "core/engine.h"
#include "nodes/mixer.h"
#include "plugins/virtual.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>

namespace
{
using namespace aereborn;
using aereborn::test::checked;

constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
constexpr auto sample_rate_hz = static_cast<std::uint32_t>(rate);
constexpr double two_pi = 6.28318530717958647692;
using TestEngine = Engine<rate, period>;
using Source = VirtualSource<rate, period>;
using Sink = VirtualSink<rate, period>;
using Mixer = aereborn::test::MixerNode<rate, period>;
constexpr auto frames = TestEngine::block_frames();

void check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void test_virtual_formats()
{
    const struct
    {
        PCMFormat format;
        std::initializer_list<std::uint8_t> half;
    } cases[] = {
        {PCMFormat::S16LE, {0, 0x40}},
        {PCMFormat::S24_3LE, {0, 0, 0x40}},
        {PCMFormat::S24LE, {0, 0, 0x40, 0}},
        {PCMFormat::S32LE, {0, 0, 0, 0x40}},
        {PCMFormat::F32LE, {0, 0, 0, 0x3f}},
    };
    for (const auto &test : cases)
    {
        TestEngine engine;
        const auto source_id = checked(engine.emplace_node<Source>(1, 12000.0, 0.5f, test.format));
        const auto sink_id = checked(engine.emplace_node<Sink>(1, test.format));
        checked(engine.connect({source_id}, {sink_id}));
        checked(engine.prepare());

        auto *sink = checked(engine.node<Sink>(sink_id));
        std::array<float, frames> samples;
        const float cycle[] = {0.0f, 0.5f, 0.0f, -0.5f};
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            samples[frame] = cycle[frame % 4];
        }
        const IBlock expected(samples.data(), 1, frames, frames);
        check(sink->pcm().empty() && !sink->matches(expected), "No PCM before the first block");
        checked(engine.process());
        check(sink->matches(expected, 1e-6f), "Received PCM must match the quarter-cycle sine");
        const auto captured = checked(engine.node<Source>(source_id))->pcm();
        const auto played = sink->pcm();
        const auto sample_bytes = test.half.size();
        check(captured.size() == frames * sample_bytes && played.size() == captured.size(),
              "Each virtual endpoint must use its configured PCM storage width");
        check(std::equal(test.half.begin(), test.half.end(), captured.begin() + sample_bytes),
              "The positive half-scale sample must have the configured PCM encoding");
        check(std::equal(captured.begin(), captured.end(), played.begin()), "Direct PCM routing must preserve the samples");
        samples[1] = -0.5f;
        check(!sink->matches(expected, 1e-6f), "Verification must detect an incorrect sample");
        engine.reset();
        check(sink->pcm().empty() && !sink->matches(expected), "Reset clears the received block");
    }
}

float reference_tone(std::uint64_t frame, double frequency_hz, double amplitude, unsigned int bits)
{
    const auto scale = std::ldexp(1.0, bits - 1);
    const double seconds = static_cast<double>(frame) / sample_rate_hz;
    return static_cast<float>(std::round(scale * amplitude * std::sin(two_pi * seconds * frequency_hz)) / scale);
}

void test_bus()
{
    TestEngine engine;
    constexpr double voice_hz = 997.0;
    constexpr double music_hz = 1663.0;
    constexpr float voice_amplitude = 0.25f;
    constexpr float music_amplitude = 0.125f;
    const auto voice = checked(engine.emplace_node<Source>(1, voice_hz, voice_amplitude, PCMFormat::S16LE));
    const auto music = checked(engine.emplace_node<Source>(1, music_hz, music_amplitude, PCMFormat::S24_3LE));
    const auto bus = checked(engine.emplace_node<Mixer>(2));
    const auto main_id = checked(engine.emplace_node<Sink>(2, PCMFormat::S24LE));
    const auto monitor_id = checked(engine.emplace_node<Sink>(1, PCMFormat::S32LE));
    const auto voice_matrix = checked(ChMatrix::create(2, 1, {1.0f, 0.5f}));
    const auto music_matrix = checked(ChMatrix::create(2, 1, {1.0f, -1.0f}));
    const auto monitor_matrix = checked(ChMatrix::create(1, 2, {0.25f, 0.75f}));

    float voice_gain = 1.0f;
    float music_gain = 0.5f;
    float monitor_gain = 0.5f;
    float direct_gain = 0.0f;
    const auto voice_conn = checked(engine.connect({voice}, {bus}, {voice_gain, false, voice_matrix}));
    const auto music_conn = checked(engine.connect({music}, {bus}, {music_gain, false, music_matrix}));
    checked(engine.connect({bus}, {main_id}));
    const auto monitor_conn = checked(engine.connect({bus}, {monitor_id}, {monitor_gain, false, monitor_matrix}));
    checked(engine.prepare());

    auto *main = checked(engine.node<Sink>(main_id));
    auto *monitor = checked(engine.node<Sink>(monitor_id));
    std::array<float, 2 * frames> main_samples;
    std::array<float, frames> monitor_samples;
    const OBlock main_ref(main_samples.data(), 2, frames, frames);
    const OBlock monitor_ref(monitor_samples.data(), 1, frames, frames);

    const auto verify_block = [&] {
        const auto first_frame = engine.frame_position();
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto voice_sample = reference_tone(first_frame + frame, voice_hz, voice_amplitude, 16);
            const auto music_sample = reference_tone(first_frame + frame, music_hz, music_amplitude, 24);
            const auto left = voice_sample * voice_gain + music_sample * music_gain;
            const auto right = voice_sample * (0.5f * voice_gain) - music_sample * music_gain;
            main_ref.channel(0)[frame] = left;
            main_ref.channel(1)[frame] = right;
            monitor_ref.channel(0)[frame] =
                left * (0.25f * monitor_gain) + right * (0.75f * monitor_gain) + voice_sample * direct_gain;
        }
        checked(engine.process());
        constexpr float tolerance = 2e-7f;
        check(main->matches(main_ref, tolerance), "Stereo bus mix must match both routed tones");
        check(monitor->matches(monitor_ref, tolerance), "Monitor must match the downmix and any direct route");
    };

    for (std::size_t block = 0; block < 4; ++block)
    {
        verify_block();
    }
    checked(engine.set_muted(music_conn, true));
    music_gain = 0.0f;
    voice_gain = 0.75f;
    checked(engine.set_gain(voice_conn, voice_gain));
    verify_block();

    direct_gain = 0.25f;
    const auto direct_conn = checked(engine.connect({voice}, {monitor_id}, {direct_gain, false, {}}));
    checked(engine.prepare());
    verify_block();

    checked(engine.set_muted(music_conn, false));
    music_gain = 0.5f;
    checked(engine.disconnect(monitor_conn));
    monitor_gain = 0.0f;
    checked(engine.prepare());
    verify_block();

    checked(engine.disconnect(direct_conn));
    direct_gain = 0.0f;
    checked(engine.prepare());
    verify_block();

    engine.reset();
    check(main->pcm().empty() && monitor->pcm().empty(), "Engine reset discards both received blocks");
    verify_block();
}
} // namespace

int main()
{
    try
    {
        test_virtual_formats();
        test_bus();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
