#ifndef AEREBORN_TESTS_AUDIO_FIXTURE_H
#define AEREBORN_TESTS_AUDIO_FIXTURE_H
#include "clocked_graph.h"
#include "pcm_reference.h"
#include "test_harness.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>
namespace aereborn
{
namespace test
{
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
constexpr double two_pi = 6.28318530717958647692;

class FloatSource : public NodeT<0, 1>
{
  public:
    using Generator = std::function<float(std::uint64_t, std::size_t)>;
    FloatSource(SampleRate rate, std::size_t channels, Generator generate)
        : NodeT({}, {PortSpec(rate, channels, SampleFormat::F32P)}), generate_(std::move(generate))
    {
    }
    void process(const ProcessContext &context) noexcept override
    {
        ++calls;
        const auto output = context.outputs[0].f32p_view();
        const auto start = outputs()[0].frame_position(context.block_period, context.tick_position);
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
            for (std::size_t frame = 0; frame < output.frames(); ++frame)
                output.channel(ch)[frame] = generate_(start + frame, ch);
    }
    std::size_t calls = 0;

  private:
    const Generator generate_;
};

class PacketSink final : public NodeT<1, 0>
{
  public:
    explicit PacketSink(PortSpec spec) : NodeT({spec}, {}) { assert(spec.format == SampleFormat::F32P); }
    void prepare(BlockPeriod period) override
    {
        const auto spec = inputs()[0];
        samples_.resize(spec.max_frames(period) * spec.channels);
    }
    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0];
        active = context.has_input(0);
        frames = input.frames();
        const auto bytes = input.byte_view();
        if (!bytes.empty())
            std::memcpy(samples_.data(), bytes.data(), bytes.size());
    }
    float sample(std::size_t frame, std::size_t ch = 0) const noexcept
    {
        assert(frame < frames && ch < inputs()[0].channels);
        return samples_[ch * frames + frame];
    }
    std::size_t frames = 0;
    bool active = false;

  private:
    std::vector<float> samples_;
};

inline ConnId connect(Engine &engine, NodeId from, NodeId to, std::size_t input = 0)
{
    return engine.connect(OPORT(from, 0), IPORT(to, input));
}
} // namespace test
} // namespace aereborn
#endif
