#include "plugins/drift.h"
#include "plugins/io.h"
#include "support/test_harness.h"

namespace
{
using namespace aereborn;
using namespace aereborn::test;

struct Profile
{
    BlockPeriod period;
    SampleRate rate;
    std::size_t packet_frames;
};

struct Observation
{
    std::size_t underruns = 0;
    std::size_t overflows = 0;
    double max_water_error = 0;
    double first_water = 0;
    double last_water = 0;
    double correction = 0;
};

double noise(std::uint32_t &seed) noexcept
{
    seed = seed * 1664525u + 1013904223u;
    return seed / 4294967296.0;
}

Observation simulate(Profile profile, bool capture, int ppm, unsigned seed, double jitter_ms, bool enabled)
{
    const PortSpec spec(profile.rate, 1, SampleFormat::F32P);
    const auto rate = static_cast<std::uint32_t>(profile.rate);
    const auto packet_us = (1000000 * profile.packet_frames + rate - 1) / rate;
    Capture endpoint(spec.sample_rate, spec.channels, std::chrono::microseconds(packet_us));
    checked(endpoint.prepare(profile.period));
    const auto status = endpoint.status();
    const auto capacity = status.capacity_frames;
    const auto target = status.target_frames;

    const auto graph_seconds = static_cast<std::uint32_t>(profile.period) / 1000.0;
    const auto device_seconds = static_cast<double>(profile.packet_frames) / (rate * (1 + ppm * 1e-6));
    const auto offset = device_seconds * (seed % 12) / 12;
    const auto jitter = jitter_ms / 1000;
    const auto jitter_origin = profile.period == BlockPeriod::Ms05 ? -jitter * 0.5 : 0;
    auto graph_seed = std::uint32_t{47365039} + 8453 * seed;
    auto device_seed = std::uint32_t{43702139} + 7621 * seed;
    const auto graph_offset = [&] { return jitter_origin + jitter * noise(graph_seed); };
    const auto device_offset = [&](std::size_t n) {
        if (jitter_ms >= 0)
            return jitter_origin + jitter * noise(device_seed);
        const double pattern[] = {0, -0.001, 0.001, 0.0005, -0.0005};
        return pattern[n % 5];
    };

    std::uint64_t graph_tick = 0;
    std::size_t device_tick = 0, fill = 0;
    double graph_time = graph_seconds + (jitter_ms >= 0 ? graph_offset() : 0);
    double device_time = device_seconds + offset + device_offset(0);
    double fraction = 0;
    double last = 0, integral = 0;
    double correction_area = 0, correction_time = 0;
    int window_end = 1;
    bool steady = false;
    aereborn::DriftController pi;
    Observation result;
    const auto push = [&](std::size_t frames) {
        if (steady && frames > capacity - fill)
            result.overflows += frames - (capacity - fill);
        fill += std::min(frames, capacity - fill);
    };
    const auto pop = [&](std::size_t frames) {
        if (frames > fill)
            result.underruns += frames - fill;
        fill -= std::min(frames, fill);
    };

    for (;;)
    {
        const auto time = std::min(graph_time, device_time);
        if (time > 300)
            break;

        if (time > 290)
        {
            const auto span = time - std::max(last, 290.0);
            correction_area += pi.correction() * span;
            correction_time += span;
        }
        if (time >= window_end)
        {
            const auto average = integral + (window_end - last) * fill;
            if (window_end == 11)
                result.first_water = average;
            if (window_end >= 11)
                result.max_water_error = std::max(result.max_water_error, std::abs(average - target));
            result.last_water = average;
            last = window_end++;
            integral = 0;
        }
        integral += (time - last) * fill;
        last = time;

        if (graph_time <= device_time)
        {
            const auto frames = spec.frames(profile.period, graph_tick++);
            graph_time = (graph_tick + 1) * graph_seconds + (jitter_ms >= 0 ? graph_offset() : 0);
            if (capture)
            {
                steady = steady || fill >= target;
                if (steady)
                    pop(frames);
            }
            else
                push(frames);
        }
        else
        {
            ++device_tick;
            device_time = (device_tick + 1) * device_seconds + offset + device_offset(device_tick);
            const auto before = fill;
            const auto was_steady = steady;
            if (!capture)
                steady = steady || fill >= target;
            if (capture || steady)
            {
                const auto step = 1 + pi.correction();
                fraction += capture ? profile.packet_frames / step : profile.packet_frames * step;
                const auto frames = static_cast<std::size_t>(fraction);
                fraction -= frames;
                if (capture)
                    push(frames);
                else
                    pop(frames);
            }
            if (enabled && was_steady)
                pi.update(before, fill, target, profile.rate, profile.packet_frames, profile.rate);
        }
    }
    result.correction = correction_area / correction_time;
    return result;
}

void verify(Profile profile, double jitter)
{
    for (const auto capture : {false, true})
        for (const auto ppm : {-300, 300})
            for (const auto seed : {0u, 5u, 100u, 107u})
            {
                const auto result = simulate(profile, capture, ppm, seed, jitter, true);
                const auto allowable = PortSpec(profile.rate, 1, SampleFormat::F32P).max_frames(profile.period);
                check(result.underruns == 0 && result.overflows == 0,
                      "Drift and burst jitter must fit from the first steady transfer");
                check(result.max_water_error <= allowable, "The one-second mean stays within one graph block of target");
                const auto expected = (capture ? 1 : -1) * ppm * 1e-6;
                check(std::abs(result.correction - expected) < 0.00035, "Watermark PI corrects the clock in the right direction");
            }
}

void drift_profiles()
{
    for (const std::size_t packet : {128, 512, 960})
    {
        verify({BlockPeriod::Ms02, SampleRate::Hz48000, packet}, 1.0);
        verify({BlockPeriod::Ms02, SampleRate::Hz48000, packet}, 1.5);
    }
    verify({BlockPeriod::Ms02, SampleRate::Hz44100, 512}, 1.5);
    verify({BlockPeriod::Ms05, SampleRate::Hz44100, 128}, -1);
    verify({BlockPeriod::Ms05, SampleRate::Hz44100, 128}, 2.0);
    verify({BlockPeriod::Ms40, SampleRate::Hz44100, 128}, -1);
}

void correction_disabled()
{
    const Profile profile{BlockPeriod::Ms02, SampleRate::Hz48000, 128};
    const auto capture = simulate(profile, true, 300, 7, 1.0, false);
    const auto playback = simulate(profile, false, 300, 7, 1.0, false);
    check(capture.overflows && capture.last_water > capture.first_water,
          "Without correction a fast capture device fills the queue");
    check(playback.underruns && playback.last_water < playback.first_water,
          "Without correction a fast playback device drains the queue");
}
} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"drift_profiles", drift_profiles}, {"correction_disabled", correction_disabled}});
}
