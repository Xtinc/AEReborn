#include "plugins/endpoint.h"
#include "support/pcm_reference.h"
#include "support/test_backend.h"
#include "support/test_harness.h"
#include <array>
#include <cmath>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;

struct ToneMeasure
{
    void observe(float sample, bool measure)
    {
        check(sample > 0.125f && sample < 0.875f, "Steady resampling does not insert silence");
        if (!measure)
            return;
        if (previous < 0.5f && sample >= 0.5f)
        {
            // Interpolate the upward crossing to resolve sub-frame changes in the period.
            const auto crossing = frames - 1 + (0.5 - previous) / (sample - previous);
            if (crossings++ == 0)
                first = crossing;
            last = crossing;
        }
        ++frames;
        previous = sample;
    }

    double scale(std::size_t period) const
    {
        check(crossings > 1, "The output contains the test tone");
        return static_cast<double>((crossings - 1) * period) / (last - first);
    }

    std::size_t frames = 0;
    std::size_t crossings = 0;
    double first = 0;
    double last = 0;
    float previous = 0.5f;
};

void check_endpoint_drift(std::size_t packet_frames, double jitter_seconds)
{
    constexpr auto sample_rate = SampleRate::Hz8000;
    constexpr auto period = BlockPeriod::Ms02;
    const auto device_period = std::chrono::microseconds(packet_frames * 1000000 / static_cast<unsigned>(sample_rate));
    const PortSpec logical(sample_rate, 1, SampleFormat::F32P), device(sample_rate, 1, SampleFormat::F32LE);
    std::array<float, 16> tone{};
    const auto radians = 2 * std::acos(-1.0) / tone.size();
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = static_cast<float>(0.5 + 0.25 * std::sin(radians * i));
    std::vector<std::uint8_t> device_input(packet_frames * sizeof(float));
    for (std::size_t i = 0; i < packet_frames; ++i)
        write_f32le(device_input.data() + i * sizeof(float), tone[i % tone.size()]);
    constexpr double jitter[] = {-0.5, 1, 0, -1, 0.5};

    for (const auto ppm : {-300, 300})
    {
        Capture capture(sample_rate, 1, device_period);
        Playback playback(sample_rate, 1, device_period);
        capture.prepare(period);
        playback.prepare(period);
        std::vector<std::uint8_t> device_output(packet_frames * sizeof(float));
        TestBackend source(device, packet_frames);
        TestBackend sink(device, packet_frames);
        source.input = {device_input.data(), device, packet_frames};
        sink.output = {device_output.data(), device, packet_frames};
        source.gated = true;
        sink.gated = true;
        source.bind(capture);
        sink.bind(playback);
        source.start();
        sink.start();
        check(source.wait_for_io() && sink.wait_for_io(), "Device workers reach their first batch");

        std::array<float, 16> samples{};
        const OBBlock output(reinterpret_cast<std::uint8_t *>(samples.data()), logical, samples.size());
        const IBBlock input(reinterpret_cast<const std::uint8_t *>(tone.data()), logical, tone.size());
        const auto device_seconds = std::chrono::duration<double>(device_period).count() / (1 + ppm * 1e-6);
        const auto graph_seconds = static_cast<unsigned>(period) / 1000.0;
        std::uint64_t graph_tick = 0;
        std::size_t device_tick = 0;
        ToneMeasure captured, played;
        for (;;)
        {
            const auto graph_time = (graph_tick + 1) * graph_seconds + jitter_seconds * jitter[graph_tick % 5];
            const auto device_time = (device_tick + 1) * device_seconds + jitter_seconds * jitter[(device_tick + 2) % 5];
            if (std::min(graph_time, device_time) > 100)
                break;
            if (graph_time <= device_time)
            {
                capture.process({period, graph_tick, {}, {&output, 1}});
                playback.process({period, graph_tick++, {&input, 1}, {}});
                if (graph_time > 2)
                    for (const auto sample : samples)
                        captured.observe(sample, graph_time > 70);
            }
            else
            {
                check(source.run_batch() && sink.run_batch(), "Both backends complete the scheduled device batch");
                ++device_tick;
                if (device_time > 2)
                    for (std::size_t i = 0; i < packet_frames; ++i)
                        played.observe(read_f32le(device_output.data() + i * sizeof(float)), device_time > 70);
            }
        }
        source.stop();
        sink.stop();
        check(source.last_error() == ErrorCode::Ok && sink.last_error() == ErrorCode::Ok,
              "Both device workers finish the simulated clock run");
        const auto expected = ppm * 1e-6;
        const auto capture_scale = captured.scale(tone.size());
        const auto playback_scale = played.scale(tone.size());
        check(std::abs(capture_scale - 1 - expected) < 0.0002 &&
                  std::abs(playback_scale - 1 + expected) < 0.0002,
              "Actual capture and playback frame transfers compensate the device clock");
    }
}

void endpoint_integration()
{
    check_endpoint_drift(128, 0);
}

void small_packet_jitter()
{
    check_endpoint_drift(64, 0.00075);
}

void large_packet_jitter()
{
    check_endpoint_drift(256, 0.001);
}
} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"small_packet_jitter", small_packet_jitter},
                                  {"large_packet_jitter", large_packet_jitter},
                                  {"endpoint_integration", endpoint_integration}});
}
