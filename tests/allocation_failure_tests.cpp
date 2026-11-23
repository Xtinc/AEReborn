#include "plugins/endpoint.h"
#include "support/test_backend.h"
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
std::atomic<std::size_t> worker_heap_calls{0};
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
    buffer.resize(1, 8);
    buffer.block(8).channel(0)[7] = 1.0f;
    const bool failed = allocation_fails_after(0, [&] { buffer.resize(2, 32); });
    const auto retained = buffer.block(8);
    check(failed && retained.channels() == 1 && retained.channel(0)[7] == 1.0f,
          "A failed growth preserves the buffer's layout and contents");
    buffer.resize(2, 32);
    check(buffer.block(32).channel(0)[7] == 0.0f, "A successful resize clears existing samples");
}

void conversion_rollback()
{
    const PortSpec input(rate, 1, SampleFormat::F32LE), output(SampleRate::Hz16000, 1, SampleFormat::F32P);
    std::array<std::uint8_t, 960> bytes{};
    const IBBlock input_block(bytes.data(), input, 240);
    const auto fill_input = [&](std::size_t first) {
        for (std::size_t i = 0; i < 240; ++i)
            write_f32le(bytes.data() + 4 * i, static_cast<float>(std::sin(two_pi * 997 * (first + i) / 48000)));
    };
    for (long failure = 0;; ++failure)
    {
        Convert actual(input, output), reference(input, output);
        actual.prepare(period);
        reference.prepare(period);
        std::array<float, 80> actual_samples{}, reference_samples{};
        const OBBlock actual_block(reinterpret_cast<std::uint8_t *>(actual_samples.data()), output, 80);
        const OBBlock reference_block(reinterpret_cast<std::uint8_t *>(reference_samples.data()), output, 80);
        fill_input(0);
        actual.process({period, 0, {&input_block, 1}, {&actual_block, 1}});
        reference.process({period, 0, {&input_block, 1}, {&reference_block, 1}});

        const bool failed = allocation_fails_after(failure, [&] { actual.prepare(BlockPeriod::Ms10); });
        if (!failed)
        {
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
            const bool failed = allocation_fails_after(0, [&] { convert.prepare(period); });
            check(!failed, "Direct conversion prepares without heap allocation");
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
            engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; });
        const auto sink = engine.emplace_node<PacketSink>(input);
        engine.connect(OPORT(source, 0), IPORT(sink, 0));
        engine.commit();
        clock.start();
        clock.advance();
        const auto converter = engine.emplace_node<Convert>(input, output);
        const auto added = engine.emplace_node<PacketSink>(output);
        engine.connect(OPORT(source, 0), IPORT(converter, 0));
        engine.connect(OPORT(converter, 0), IPORT(added, 0));
        clock.resume();
        const bool failed = allocation_fails_after(failure, [&] { engine.commit(); });
        clock.advance();
        if (!failed)
        {
            check(failure > 0, "Fault injection visits plan allocations");
            break;
        }
        auto *new_sink = engine.node<PacketSink>(added);
        clock.advance();
        check(new_sink->frames == 0 && engine.node<PacketSink>(sink)->sample(0) == 1,
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
    engine.emplace_node<HeapProbe>(true);
    const auto source =
        engine.emplace_node<FloatSource>(SampleRate::Hz44100, 1, [](std::uint64_t, std::size_t) { return 0.25f; });
    const PortSpec input(SampleRate::Hz44100, 1, SampleFormat::F32P);
    const PortSpec output(rate, 1, SampleFormat::F32P);
    const auto converter = engine.emplace_node<Convert>(input, output);
    const auto mixer = engine.emplace_node<Mixer>(rate, 1, 1);
    const auto matrix = engine.emplace_node<Matrix>(rate, 1);
    const auto sink = engine.emplace_node<PacketSink>(output);
    engine.connect(OPORT(source, 0), IPORT(converter, 0));
    engine.connect(OPORT(converter, 0), IPORT(mixer, 0));
    engine.connect(OPORT(mixer, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(sink, 0));
    engine.commit();
    clock.start();
    const auto probe = engine.emplace_node<HeapProbe>(false);
    engine.node<Matrix>(matrix)->set_gain(0.5f);
    engine.node<Mixer>(mixer)->set_muted(0, true);
    clock.commit();
    clock.advance();
    check(engine.node<HeapProbe>(probe)->heap_calls == 0,
          "Plan switching and rendering perform no heap operations on the clock thread");
}

void backend_binding_rollback()
{
    const PortSpec device(rate, 1, SampleFormat::F32LE);
    aereborn::Capture capture(rate, 1, std::chrono::microseconds(2667));
    capture.prepare(BlockPeriod::Ms02);
    BackendCalls calls;
    TestBackend source(device, 128, &calls), replacement(device, 128);
    for (long failure = 0;; ++failure)
    {
        const auto failed = allocation_fails_after(failure, [&] { source.bind(capture); });
        if (!failed)
        {
            check(failure > 0, "Fault injection reaches bind-time workspace allocations");
            source.unbind();
            break;
        }
        check(!source.bound() && calls.opens == calls.closes,
              "A bind exception closes the device and leaves the backend unbound");
        replacement.bind(capture);
        replacement.unbind();
    }
}

void endpoint_uses_prepared_buffers()
{
    constexpr auto graph_period = BlockPeriod::Ms02;
    const PortSpec port(rate, 1, SampleFormat::F32P);
    const PortSpec native(rate, 1, SampleFormat::F32LE);
    aereborn::Capture capture(port.sample_rate, port.channels, std::chrono::microseconds(2667));
    aereborn::Playback playback(port.sample_rate, port.channels, std::chrono::microseconds(2667));
    capture.prepare(graph_period);
    playback.prepare(graph_period);
    TestBackend input(native, 128);
    TestBackend output(native, 128);
    const auto measure_worker = [](bool active) {
        if (active)
            allocation_test::heap_calls = 0;
        else
            allocation_test::worker_heap_calls.fetch_add(allocation_test::heap_calls, std::memory_order_relaxed);
        allocation_test::measure_heap = active;
    };
    input.gated = output.gated = true;
    input.measure_io = output.measure_io = measure_worker;
    input.bind(capture);
    output.bind(playback);
    std::array<float, 96> graph_samples{};
    const OBBlock graph_output(reinterpret_cast<std::uint8_t *>(graph_samples.data()), port, 96);
    const IBBlock graph_input = graph_output;

    allocation_test::heap_calls = 0;
    allocation_test::worker_heap_calls.store(0, std::memory_order_relaxed);
    bool succeeded = true;
    for (int restart = 0; restart < 3; ++restart)
    {
        input.start();
        output.start();
        check(input.wait_for_io() && output.wait_for_io(), "Both workers reach their first batch");
        allocation_test::measure_heap = true;
        for (std::uint64_t tick = 0; tick < 16; ++tick)
        {
            succeeded &= input.run_batch();
            capture.process({graph_period, tick, {}, {&graph_output, 1}});
            playback.process({graph_period, tick, {&graph_input, 1}, {}});
            succeeded &= output.run_batch();
        }
        allocation_test::measure_heap = false;
        input.stop();
        output.stop();
        succeeded &= input.last_error() == ErrorCode::Ok && output.last_error() == ErrorCode::Ok;
    }
    check(succeeded && allocation_test::heap_calls == 0 && allocation_test::worker_heap_calls.load() == 0,
          "The graph and device worker paths reuse prepared storage across restarts");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"buffer_resize_rollback", buffer_resize_rollback},
                      {"conversion_rollback", conversion_rollback},
                      {"direct_conversion_needs_no_workspace", direct_conversion_needs_no_workspace},
                      {"plan_allocation_rollback", plan_allocation_rollback},
                      {"render_uses_prepared_buffers", render_uses_prepared_buffers},
                      {"backend_binding_rollback", backend_binding_rollback},
                      {"endpoint_uses_prepared_buffers", endpoint_uses_prepared_buffers}});
}
#endif
