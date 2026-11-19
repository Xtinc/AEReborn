#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/pcm_reference.h"
#include "core/engine.h"
#include "core/observer.h"
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
thread_local long fail_after = -1;
thread_local bool measure_heap = false;
thread_local std::size_t heap_calls = 0;
}

// Fail each allocation in prepare(), including failures after workspace preparation.
void *operator new(std::size_t size)
{
    if (allocation_test::measure_heap)
    {
        ++allocation_test::heap_calls;
    }
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
    if (memory && allocation_test::measure_heap)
    {
        ++allocation_test::heap_calls;
    }
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    ::operator delete(memory);
}

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
constexpr std::size_t block_frames = 240;
constexpr double two_pi = 6.283185307179586;

template <class Operation> bool allocation_fails_after(long allocations, Operation operation)
{
    allocation_test::fail_after = allocations;
    try
    {
        operation();
    }
    catch (const std::bad_alloc &)
    {
        allocation_test::fail_after = -1;
        return true;
    }
    catch (...)
    {
        allocation_test::fail_after = -1;
        throw;
    }
    allocation_test::fail_after = -1;
    return false;
}

void buffer_resize_rollback()
{
    Buffer buffer;
    checked(buffer.resize(1, 8));
    buffer.block(8).channel(0)[7] = 1.0f;
    const bool failed = allocation_fails_after(0, [&] { buffer.resize(2, 32); });
    check(failed && buffer.capacity_frames() == 8 && buffer.block(8).channel(0)[7] == 1.0f,
          "A failed growth preserves the buffer's layout and contents");
    checked(buffer.resize(2, 32));
    check(buffer.block(32).channel(0)[7] == 0.0f, "A successful resize clears existing samples");
}

void pcm_configuration_rollback()
{
    const ChMatrix mono_to_stereo(2, 1, {1, -0.5f});
    const ChMatrix stereo_to_mono(1, 2, {0.25f, 0.75f});
    for (const bool replace_input : {true, false})
    {
        for (long failure = 0;; ++failure)
        {
            Engine engine(rate, period);
            ManualClock clock(engine);
            const PCMSpec capture_spec{SampleRate::Hz44100, PCMFormat::F32LE, 2, 2};
            const PCMSpec playback_spec{rate, PCMFormat::F32LE, 2, 2};
            const auto input_id = checked(engine.emplace_node<IAdapter>(2, capture_spec));
            const auto output_id = checked(engine.emplace_node<OAdapter>(2, playback_spec));
            auto *input = checked(engine.node<IAdapter>(input_id));
            auto *output = checked(engine.node<OAdapter>(output_id));
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
            checked(clock.tick());
            const auto previous = packet;
            checked(clock.stop());

            const PCMSpec replacement{SampleRate::Hz16000, PCMFormat::S16LE, 1};
            RetCode code;
            if (replace_input)
            {
                checked(engine.replace_node(input_id, std::make_unique<IAdapter>(2, replacement,
                                                                                mono_to_stereo)));
            }
            else
            {
                checked(engine.replace_node(output_id, std::make_unique<OAdapter>(2, replacement,
                                                                                 stereo_to_mono)));
            }
            const bool failed = allocation_fails_after(failure, [&] { code = engine.prepare().ret_code(); });
            if (!failed)
            {
                checked(code);
                check(failure > 0, "Fault injection must exercise the configuration allocation path");
                break;
            }
            check(packet == previous && input->pcm_frames() == 441 && output->pcm_frames() == 480 &&
                      input->bind_pcm({pcm.data(), pcm.size()}).code() == ErrorCode::BufferFull &&
                      output->bind_pcm({packet.data(), packet.size()}).code() == ErrorCode::BufferFull,
                  "Every allocation failure preserves the partially consumed device buffers");
            checked(clock.tick());
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

void render_uses_prepared_buffers()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec capture_spec{SampleRate::Hz16000, PCMFormat::F32LE, 1};
    const PCMSpec playback_spec{rate, PCMFormat::F32LE, 1};
    const auto input_id = checked(engine.emplace_node<IAdapter>(1, capture_spec));
    const auto output_id = checked(engine.emplace_node<OAdapter>(1, playback_spec));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    const auto connection = checked(engine.connect({input_id}, {output_id}));
    checked(engine.prepare());
    checked(clock.start());

    const auto observe = [](const Observation &) {};
    checked(engine.add_observer(std::make_unique<Observer>(OPort{input_id}, observe)));
    checked(engine.add_observer(std::make_unique<Observer>(IPort{input_id}, observe)));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{output_id}, observe)));
    const auto revision = checked(engine.prepare());
    checked(engine.set_gain(connection, 0.5f));
    std::vector<std::uint8_t> capture(input->pcm_bytes());
    std::vector<std::uint8_t> playback(output->pcm_bytes());
    checked(input->bind_pcm({capture.data(), capture.size()}));
    checked(output->bind_pcm({playback.data(), playback.size()}));

    allocation_test::heap_calls = 0;
    allocation_test::measure_heap = true;
    const auto code = clock.tick();
    allocation_test::measure_heap = false;
    checked(code);
    check(allocation_test::heap_calls == 0 && engine.applied_revision() == revision,
          "Plan activation, parameter ramps, resampling and observer publication do not allocate or reclaim on the driver");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"buffer_resize_rollback", buffer_resize_rollback},
        {"pcm_configuration_rollback", pcm_configuration_rollback},
        {"render_uses_prepared_buffers", render_uses_prepared_buffers}
    });
}
#endif
