#ifndef AEREBORN_TESTS_ROUTING_FIXTURE_H
#define AEREBORN_TESTS_ROUTING_FIXTURE_H

#include "manual_clock.h"
#include "pcm_reference.h"
#include "test_harness.h"
#include "test_node.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"
#include <cmath>
#include <initializer_list>
#include <vector>

namespace aereborn
{
namespace test
{

inline double tone(double frequency, double amplitude, std::size_t frame)
{
    return amplitude * std::sin(6.283185307179586 * frequency * frame / 48000);
}

struct Capture
{
    Capture(Engine &engine, std::size_t channels, double frequency, float amplitude)
        : device({render_rate, PCMFormat::F32LE, channels}, render_period, frequency, amplitude),
          id(checked(engine.emplace_node<IAdapter>(channels,
                                                  PCMSpec{render_rate, PCMFormat::F32LE, channels}))),
          adapter(checked(engine.node<IAdapter>(id)))
    {
        checked(device.prepare());
    }
    void generate()
    {
        checked(device.generate());
        checked(adapter->bind_pcm(device.pcm()));
    }
    VirtualSource device;
    NodeId id;
    IAdapter *adapter;
};

struct Playback
{
    Playback(Engine &engine, std::size_t channels)
        : device({render_rate, PCMFormat::F32LE, channels}, render_period),
          id(checked(engine.emplace_node<OAdapter>(channels,
                                                  PCMSpec{render_rate, PCMFormat::F32LE, channels}))),
          adapter(checked(engine.node<OAdapter>(id))), channels(channels),
          bytes(render_frames * channels * sizeof(float))
    {
        checked(device.prepare());
    }
    void bind()
    {
        checked(adapter->bind_pcm({bytes.data(), bytes.size()}));
    }
    void receive()
    {
        checked(device.receive({bytes.data(), bytes.size()}));
    }
    template <class Expected> void expect(Expected expected) const
    {
        const auto pcm = device.pcm();
        check(pcm.size() == render_frames * channels * 4, "Virtual sink must receive a complete period");
        for (std::size_t frame = 0; frame < render_frames; ++frame)
        {
            for (std::size_t ch = 0; ch < channels; ++ch)
            {
                const auto actual = read_f32le(pcm.data() + (frame * channels + ch) * 4);
                check(std::abs(actual - expected(frame, ch)) < 1e-6, "Virtual sink differs from independent route reference");
            }
        }
    }
    VirtualSink device;
    NodeId id;
    OAdapter *adapter;
    const std::size_t channels;
    std::vector<std::uint8_t> bytes;
};

inline void render_route(ManualClock &clock, std::initializer_list<Capture *> sources,
                         std::initializer_list<Playback *> sinks)
{
    for (auto *source : sources)
    {
        source->generate();
    }
    for (auto *sink : sinks)
    {
        sink->bind();
    }
    checked(clock.tick());
    for (auto *sink : sinks)
    {
        sink->receive();
    }
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_ROUTING_FIXTURE_H
