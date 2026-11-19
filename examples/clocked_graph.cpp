#include "core/engine.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace
{
using namespace aereborn;

void checked(RetCode code)
{
    if (!code)
    {
        throw std::runtime_error(code.message());
    }
}

template <class T> T checked(Result<T> result)
{
    checked(result.ret_code());
    return std::move(result).value();
}

class Tone final : public NodeT<0, 1>
{
  public:
    Tone() : NodeT({}, {{1}})
    {
    }
    RetCode prepare(const EngineSpec &spec) override
    {
        step_ = 6.283185307179586 * 440 / static_cast<std::uint32_t>(spec.sample_rate());
        return {};
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto output = context.outputs[0];
        for (std::size_t frame = 0; frame < output.frames(); ++frame)
        {
            output.channel(0)[frame] = 0.25f * static_cast<float>(std::sin((context.frame_position + frame) * step_));
        }
    }

  private:
    double step_ = 0;
};

class PassThrough final : public NodeT<1, 1>
{
  public:
    PassThrough() : NodeT({{1}}, {{1}})
    {
    }
    void process(const ProcContext &context) noexcept override
    {
        const auto input = context.inputs[0];
        const auto output = context.outputs[0];
        std::copy_n(input.channel(0), output.frames(), output.channel(0));
    }
};
} // namespace

int main()
{
    try
    {
        std::mutex mutex;
        std::condition_variable received;
        std::size_t packets = 0;
        Engine engine(SampleRate::Hz48000, BlockPeriod::Ms05);
        const auto tone = checked(engine.emplace_node<Tone>());
        const auto bus = checked(engine.emplace_node<PassThrough>());
        const auto conn = checked(engine.connect({tone}, {bus}));
        const auto observer = checked(engine.subscribe({bus}, {2 * engine.spec().block_frames()},
            [&](const Observation &packet) {
                const std::lock_guard<std::mutex> lock(mutex);
                std::cout << "frame=" << packet.frame_position << " frames=" << packet.audio.frames()
                          << " dropped=" << packet.dropped_frames << '\n';
                ++packets;
                received.notify_one();
            }));
        checked(engine.prepare());
        checked(engine.start());

        const auto wait_for_packets = [&](std::size_t count) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!received.wait_for(lock, std::chrono::seconds(3), [&] { return packets >= count; }))
            {
                throw std::runtime_error("Timed out waiting for observer audio");
            }
        };
        wait_for_packets(2);
        checked(engine.set_gain(conn, 0.5f));
        wait_for_packets(4);
        checked(engine.unsubscribe(observer));
        checked(engine.stop());
        std::cout << "stopped at frame " << engine.frame_position() << '\n';
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
