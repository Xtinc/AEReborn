#include "checked.h"
#include "pcm.h"
#include "core/engine.h"
#include "plugins/adapter.h"
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
int main()
{
    std::cout << "SKIP: MSVC debug iterator proxy constructors allocate inside noexcept\n";
    return 77;
}
#else
namespace allocation_test
{
long fail_after = -1;
}

// Fail each allocation in configure_pcm(), including failures after workspace preparation.
void *operator new(std::size_t size)
{
    if (allocation_test::fail_after == 0)
    {
        throw std::bad_alloc();
    }
    if (allocation_test::fail_after > 0)
    {
        --allocation_test::fail_after;
    }
    if (auto *memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void *memory) noexcept
{
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
using TestEngine = Engine<rate, period>;
using Input = IAdapter<rate, period>;
using Output = OAdapter<rate, period>;
constexpr auto block_frames = TestEngine::block_frames();
constexpr double two_pi = 6.283185307179586;

void configuration_rollback()
{
    // MSVC debug vectors allocate iterator proxies inside noexcept constructors.
    // Inject allocation failures in Release; Debug still covers invalid/capacity rollback.
    const auto mono_to_stereo = checked(ChMatrix::create(2, 1, {1, -0.5f}));
    const auto stereo_to_mono = checked(ChMatrix::create(1, 2, {0.25f, 0.75f}));
    for (const bool replace_input : {true, false})
    {
        for (long failure = 0;; ++failure)
        {
            TestEngine engine;
            const auto input_id = checked(engine.emplace_node<Input>(2));
            const auto output_id = checked(engine.emplace_node<Output>(2));
            auto *input = checked(engine.node<Input>(input_id));
            auto *output = checked(engine.node<Output>(output_id));
            checked(input->configure_pcm({SampleRate::Hz44100, PCMFormat::F32LE, 2, 2}));
            checked(output->configure_pcm({rate, PCMFormat::F32LE, 2, 2}));
            checked(engine.connect({input_id}, {output_id}));
            checked(engine.prepare());
            std::vector<std::uint8_t> pcm(input->pcm_bytes());
            std::vector<std::uint8_t> packet(output->pcm_bytes(), 0xcd);
            for (std::size_t frame = 0; frame < 441; ++frame)
            {
                const auto sample = static_cast<float>(std::sin(two_pi * 1000 * frame / 44100));
                write_f32le(pcm.data() + frame * 8, sample);
                write_f32le(pcm.data() + frame * 8 + 4, -0.5f * sample);
            }
            checked(input->bind_pcm({pcm.data(), pcm.size()}));
            checked(output->bind_pcm({packet.data(), packet.size()}));
            checked(engine.process());
            const auto previous = packet;

            allocation_test::fail_after = failure;
            const PCMSpec replacement{SampleRate::Hz16000, PCMFormat::S16LE, 1};
            const auto code = replace_input ? input->configure_pcm(replacement, mono_to_stereo)
                                            : output->configure_pcm(replacement, stereo_to_mono);
            allocation_test::fail_after = -1;
            if (code)
            {
                check(failure > 0, "Fault injection must exercise the configuration allocation path");
                break;
            }
            check(code.code() == ErrorCode::NoMemory && packet == previous &&
                      input->pcm_frames() == 441 && output->pcm_frames() == 480 &&
                      input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull &&
                      output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::BufferFull,
                  "Every allocation failure preserves the partially consumed device buffers");
            checked(engine.process());
            for (std::size_t frame = block_frames; frame < 2 * block_frames; ++frame)
            {
                const auto expected = std::sin(two_pi * 1000 * (static_cast<double>(frame) / 48000 - 48.0 / 44100));
                check(std::abs(read_f32le(packet.data() + frame * 8) - expected) < 0.01 &&
                          std::abs(read_f32le(packet.data() + frame * 8 + 4) + 0.5 * expected) < 0.01,
                      "Failed reconfiguration preserves phase, history and both period indices");
            }
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"configuration_rollback", configuration_rollback}
    });
}
#endif
