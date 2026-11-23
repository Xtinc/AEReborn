#include "core/engine.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "tests/support/tone.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace
{
using namespace aereborn;
class Meter final : public NodeT<1, 0>
{
  public:
    explicit Meter(std::atomic<float> &level)
        : NodeT({PortSpec(SampleRate::Hz48000, 1, SampleFormat::F32P)}, {}), level_(level)
    {
    }
    void process(const ProcessContext &context) noexcept override
    {
        const auto input = context.inputs[0].f32p_view();
        float level = 0;
        for (std::size_t i = 0; i < input.frames(); ++i)
            level = std::max(level, std::abs(input.channel(0)[i]));
        level_.store(level, std::memory_order_relaxed);
    }

  private:
    std::atomic<float> &level_;
};
} // namespace
int main()
{
    try
    {
        std::atomic<float> level{0};
        Engine engine(BlockPeriod::Ms05);
        const auto tone = engine.emplace_node<Tone>(SampleRate::Hz48000, 1);
        const auto mix = engine.emplace_node<Mixer>(SampleRate::Hz48000, 1, 1);
        const auto matrix = engine.emplace_node<Matrix>(SampleRate::Hz48000, 1);
        const auto sink = engine.emplace_node<Meter>(level);
        engine.connect(OPORT(tone, 0), IPORT(mix, 0));
        engine.connect(OPORT(mix, 0), IPORT(matrix, 0));
        engine.connect(OPORT(matrix, 0), IPORT(sink, 0));
        auto *gain = engine.node<Matrix>(matrix);
        engine.commit();
        engine.start();
        const auto wait = [&]
        {
            const auto target = engine.tick_position() + 4;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (engine.tick_position() < target)
            {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Audio timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::cout << "tick=" << engine.tick_position() << " peak=" << level.load() << '\n';
        };
        wait();
        gain->set_gain(0.5f);
        wait();
        engine.stop();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
