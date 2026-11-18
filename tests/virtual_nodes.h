#ifndef AEREBORN_TESTS_VIRTUAL_NODES_H
#define AEREBORN_TESTS_VIRTUAL_NODES_H

#include "checked.h"
#include "pcm.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"
#include <cmath>

namespace aereborn
{
namespace test
{
constexpr auto render_rate = SampleRate::Hz48000;
constexpr auto render_period = BlockPeriod::Ms05;
using RoutingEngine = Engine<render_rate, render_period>;
using CaptureAdapter = IAdapter<render_rate, render_period>;
using PlaybackAdapter = OAdapter<render_rate, render_period>;
constexpr auto render_frames = RoutingEngine::block_frames();

inline double tone(double frequency, double amplitude, std::size_t frame)
{
    return amplitude * std::sin(6.283185307179586 * frequency * frame / 48000);
}

// Stateless test processor. Fan-in at a port is performed by Engine; this node sums its input ports.
template <std::size_t I = 1, std::size_t O = 1>
class VirtualNode : public NodeT<I, O, render_rate, render_period>
{
  public:
    explicit VirtualNode(std::size_t channels = 1, float gain = 1.0f)
        : NodeT<I, O, render_rate, render_period>(ports<I>(channels), ports<O>(channels)), gain_(gain)
    {
    }
    RetCode prepare() override
    {
        ++prepares;
        return preparation;
    }
    void process(const ProcContext &context) noexcept override
    {
        ++calls;
        for (const auto output : context.outputs)
        {
            output.clear();
            for (const auto input : context.inputs)
            {
                for (std::size_t ch = 0; ch < output.channels(); ++ch)
                {
                    for (std::size_t frame = 0; frame < output.frames(); ++frame)
                    {
                        output.channel(ch)[frame] += input.channel(ch)[frame] * gain_;
                    }
                }
            }
        }
    }
    void reset() noexcept override
    {
        ++resets;
    }

    RetCode preparation;
    std::size_t prepares = 0;
    std::size_t calls = 0;
    std::size_t resets = 0;

  private:
    template <std::size_t N> static std::array<std::size_t, N> ports(std::size_t channels)
    {
        std::array<std::size_t, N> result{};
        result.fill(channels);
        return result;
    }
    const float gain_;
};

struct Capture
{
    Capture(RoutingEngine &engine, std::size_t channels, double frequency, float amplitude)
        : device({render_rate, PCMFormat::F32LE, channels}, render_period, frequency, amplitude),
          id(checked(engine.emplace_node<CaptureAdapter>(channels))),
          adapter(checked(engine.node<CaptureAdapter>(id)))
    {
        checked(device.prepare());
        checked(adapter->configure_pcm({render_rate, PCMFormat::F32LE, channels}));
    }
    void generate()
    {
        checked(device.generate());
        checked(adapter->bind_pcm(device.pcm()));
    }
    VirtualSource device;
    NodeId id;
    CaptureAdapter *adapter;
};

struct Playback
{
    Playback(RoutingEngine &engine, std::size_t channels)
        : device({render_rate, PCMFormat::F32LE, channels}, render_period),
          id(checked(engine.emplace_node<PlaybackAdapter>(channels))),
          adapter(checked(engine.node<PlaybackAdapter>(id))), channels(channels)
    {
        checked(device.prepare());
        checked(adapter->configure_pcm({render_rate, PCMFormat::F32LE, channels}));
        bytes.resize(adapter->pcm_bytes());
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
    PlaybackAdapter *adapter;
    const std::size_t channels;
    std::vector<std::uint8_t> bytes;
};

} // namespace test
} // namespace aereborn
#endif
