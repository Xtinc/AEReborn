#include "core/engine.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "plugins/tone.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace
{
using namespace aereborn;
void checked(RetCode code)
{
    if (!code)
        throw std::runtime_error(code.message());
}
template <class T> T checked(Result<T> result)
{
    checked(result.ret_code());
    return std::move(result).value();
}
class Meter final : public NodeT<1, 0>
{
  public:
    explicit Meter(std::atomic<float> &level) : NodeT({{PortSpec(SampleRate::Hz48000, 1)}}, {}), level_(level) {}
    RetCode prepare(BlockPeriod) override
    {
        return {};
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
        const auto tone = checked(engine.emplace_node<Tone>(SampleRate::Hz48000, 1));
        const auto mix = checked(engine.emplace_node<Mixer>(SampleRate::Hz48000, 1, 1));
        const auto matrix = checked(engine.emplace_node<Matrix>(SampleRate::Hz48000, 1));
        const auto sink = checked(engine.emplace_node<Meter>(level));
        checked(engine.connect({tone}, {mix}));
        checked(engine.connect({mix}, {matrix}));
        checked(engine.connect({matrix}, {sink}));
        auto *gain = checked(engine.node<Matrix>(matrix));
        checked(engine.commit());
        checked(engine.start());
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
        checked(gain->set_gain(0.5f));
        wait();
        checked(engine.stop());
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
