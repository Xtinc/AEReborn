#ifndef AEREBORN_TESTS_AUDIO_FIXTURE_H
#define AEREBORN_TESTS_AUDIO_FIXTURE_H
#include "clocked_graph.h"
#include "pcm_reference.h"
#include "plugins/convert.h"
#include "plugins/io.h"
#include "test_harness.h"
#include <algorithm>
#include <cmath>
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
    RetCode prepare(BlockPeriod) override
    {
        ++prepares;
        return {};
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
    std::size_t calls = 0, prepares = 0;

  private:
    const Generator generate_;
};

class PacketSink final : public NodeT<1, 0>
{
  public:
    explicit PacketSink(PortSpec spec) : NodeT({spec}, {}) {}
    RetCode prepare(BlockPeriod period) override
    {
        const auto spec = inputs()[0];
        bytes_.resize(spec.max_frames(period) * spec.channels * sample_bytes(spec.format));
        return {};
    }
    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0];
        frames = input.frames();
        size_ = input.byte_view().size();
        address = input.byte_view().data();
        std::copy(input.byte_view().begin(), input.byte_view().end(), bytes_.begin());
    }
    View<const std::uint8_t> bytes() const noexcept { return {bytes_.data(), size_}; }
    float sample(std::size_t frame, std::size_t ch = 0) const noexcept
    {
        const auto spec = inputs()[0];
        if (spec.format == SampleFormat::F32LE)
            return read_f32le(bytes_.data() + (frame * spec.channels + ch) * 4);
        assert(spec.format == SampleFormat::F32P);
        float value;
        std::memcpy(&value, bytes_.data() + (ch * frames + frame) * 4, 4);
        return value;
    }
    std::size_t frames = 0;
    const void *address = nullptr;

  private:
    std::vector<std::uint8_t> bytes_;
    std::size_t size_ = 0;
};

struct Conversion
{
    Conversion(PortSpec input, PortSpec output, BlockPeriod period = test::period)
        : engine(period), clock(engine)
    {
        const auto input_id = checked(engine.emplace_node<Capture>(input));
        const auto convert_id = checked(engine.emplace_node<Convert>(input, output));
        const auto sink_id = checked(engine.emplace_node<PacketSink>(output));
        checked(engine.connect({input_id}, {convert_id}));
        checked(engine.connect({convert_id}, {sink_id}));
        checked(engine.commit());
        capture = checked(engine.node<Capture>(input_id));
        sink = checked(engine.node<PacketSink>(sink_id));
        clock.start();
    }
    void run(View<const std::uint8_t> bytes)
    {
        checked(capture->bind(bytes, engine.tick_position()));
        clock.advance();
    }
    Engine engine;
    ClockedGraph clock;
    Capture *capture;
    PacketSink *sink;
};

inline std::vector<std::uint8_t> float_bytes(const std::vector<float> &samples)
{
    std::vector<std::uint8_t> result(samples.size() * sizeof(float));
    std::memcpy(result.data(), samples.data(), result.size());
    return result;
}
} // namespace test
} // namespace aereborn
#endif
