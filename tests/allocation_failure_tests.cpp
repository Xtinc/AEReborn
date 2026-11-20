#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include <array>
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
} // namespace allocation_test

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
    const auto retained = buffer.block(8);
    check(failed && retained.channels() == 1 && retained.channel(0)[7] == 1.0f,
          "A failed growth preserves the buffer's layout and contents");
    checked(buffer.resize(2, 32));
    check(buffer.block(32).channel(0)[7] == 0.0f, "A successful resize clears existing samples");
}

void conversion_rollback()
{
    const PortSpec input(rate, 1, SampleFormat::F32LE), output(SampleRate::Hz16000, 1, SampleFormat::F32P);
    std::array<std::uint8_t, 960> bytes{};
    const IPortBlock input_block(bytes.data(), input, 240);
    const auto fill_input = [&](std::size_t first) {
        for (std::size_t i = 0; i < 240; ++i)
            write_f32le(bytes.data() + 4 * i, static_cast<float>(std::sin(two_pi * 997 * (first + i) / 48000)));
    };
    for (long failure = 0;; ++failure)
    {
        Convert actual(input, output), reference(input, output);
        checked(actual.prepare(period));
        checked(reference.prepare(period));
        std::array<float, 80> actual_samples{}, reference_samples{};
        const OPortBlock actual_block(reinterpret_cast<std::uint8_t *>(actual_samples.data()), output, 80);
        const OPortBlock reference_block(reinterpret_cast<std::uint8_t *>(reference_samples.data()), output, 80);
        fill_input(0);
        actual.process({period, 0, {&input_block, 1}, {&actual_block, 1}});
        reference.process({period, 0, {&input_block, 1}, {&reference_block, 1}});

        RetCode code;
        const bool failed = allocation_fails_after(failure, [&] { code = actual.prepare(period); });
        if (!failed)
        {
            checked(code);
            check(failure > 0, "Fault injection visits converter allocations");
            break;
        }
        fill_input(240);
        actual.process({period, 1, {&input_block, 1}, {&actual_block, 1}});
        reference.process({period, 1, {&input_block, 1}, {&reference_block, 1}});
        check(actual_samples == reference_samples, "Failed converter preparation retains filter history and buffers");
    }
}
void direct_conversion_needs_no_workspace()
{
    for (const auto input_format : {SampleFormat::F32P, SampleFormat::S16LE})
    {
        for (const auto output_format : {SampleFormat::F32P, SampleFormat::S16LE})
        {
            Convert convert(PortSpec(rate, 2, input_format), PortSpec(rate, 2, output_format));
            RetCode code;
            const bool failed = allocation_fails_after(0, [&] { code = convert.prepare(period); });
            check(!failed, "Direct conversion needs no allocated conversion state or workspace");
            checked(code);
        }
    }
}
void plan_allocation_rollback()
{
    const PortSpec input(rate, 1, SampleFormat::F32P);
    const PortSpec output(SampleRate::Hz16000, 1, SampleFormat::F32P);
    for (long failure = 0;; ++failure)
    {
        Engine engine(period);
        ClockedGraph clock(engine);
        const auto source =
            checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; }));
        const auto sink = checked(engine.emplace_node<PacketSink>(input));
        checked(engine.connect({source}, {sink}));
        checked(engine.commit());
        clock.start();
        clock.advance();
        const auto converter = checked(engine.emplace_node<Convert>(input, output));
        const auto added = checked(engine.emplace_node<PacketSink>(output));
        checked(engine.connect({source}, {converter}));
        checked(engine.connect({converter}, {added}));
        RetCode code;
        clock.resume();
        const bool failed = allocation_fails_after(failure, [&] { code = engine.commit(); });
        clock.advance();
        if (!failed)
        {
            checked(code);
            check(failure > 0, "Fault injection visits plan allocations");
            break;
        }
        auto *new_sink = checked(engine.node<PacketSink>(added));
        clock.advance();
        check(new_sink->frames == 0 && checked(engine.node<PacketSink>(sink))->sample(0) == 1,
              "Failed plan construction preserves the running graph");
        clock.commit();
        clock.advance();
        check(new_sink->frames == 80, "Retry applies the new graph");
    }
}

class HeapProbe final : public NodeT<1, 0>
{
  public:
    explicit HeapProbe(bool begin) : NodeT({PortSpec(rate, 1, SampleFormat::F32P)}, {}), begin_(begin) {}
    void process(const ProcessContext &context) noexcept override
    {
        if (begin_ && context.tick_position == 0)
        {
            allocation_test::heap_calls = 0;
            allocation_test::measure_heap = true;
        }
        if (!begin_ && allocation_test::measure_heap)
        {
            allocation_test::measure_heap = false;
            heap_calls = allocation_test::heap_calls;
        }
    }
    std::size_t heap_calls = 1;

  private:
    const bool begin_;
};

void render_uses_prepared_buffers()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    checked(engine.emplace_node<HeapProbe>(true));
    const auto source = checked(
        engine.emplace_node<FloatSource>(SampleRate::Hz44100, 1, [](std::uint64_t, std::size_t) { return 0.25f; }));
    const PortSpec input(SampleRate::Hz44100, 1, SampleFormat::F32P);
    const PortSpec output(rate, 1, SampleFormat::F32P);
    const auto converter = checked(engine.emplace_node<Convert>(input, output));
    const auto mixer = checked(engine.emplace_node<Mixer>(rate, 1, 1));
    const auto matrix = checked(engine.emplace_node<Matrix>(rate, 1));
    const auto sink = checked(engine.emplace_node<PacketSink>(output));
    checked(engine.connect({source}, {converter}));
    checked(engine.connect({converter}, {mixer}));
    checked(engine.connect({mixer}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.commit());
    clock.start();
    const auto probe = checked(engine.emplace_node<HeapProbe>(false));
    checked(checked(engine.node<Matrix>(matrix))->set_gain(0.5f));
    checked(checked(engine.node<Mixer>(mixer))->set_muted(0, true));
    clock.commit();
    clock.advance();
    check(checked(engine.node<HeapProbe>(probe))->heap_calls == 0,
          "Plan switching and rendering perform no heap operations on the clock thread");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"buffer_resize_rollback", buffer_resize_rollback},
                      {"conversion_rollback", conversion_rollback},
                      {"direct_conversion_needs_no_workspace", direct_conversion_needs_no_workspace},
                      {"plan_allocation_rollback", plan_allocation_rollback},
                      {"render_uses_prepared_buffers", render_uses_prepared_buffers}});
}
#endif
