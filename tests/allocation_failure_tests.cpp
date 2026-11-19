#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
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
    const PortSpec input(rate, 1, Format::F32LE), output(SampleRate::Hz16000, 1);
    for (long failure = 0;; ++failure)
    {
        Conversion actual(input, output), reference(input, output);
        std::vector<std::uint8_t> bytes(960);
        for (std::size_t i = 0; i < 240; ++i)
            write_f32le(bytes.data() + 4 * i, static_cast<float>(std::sin(two_pi * 997 * i / 48000)));
        actual.run({bytes.data(), bytes.size()});
        reference.run({bytes.data(), bytes.size()});
        checked(actual.clock.stop());
        auto *convert = checked(actual.engine.node<Convert>(actual.convert_id));
        RetCode code;
        const bool failed = allocation_fails_after(failure, [&] { code = convert->prepare(period); });
        if (!failed)
        {
            checked(code);
            check(failure > 0, "Fault injection visits converter allocations");
            break;
        }
        for (std::size_t i = 0; i < 240; ++i)
            write_f32le(bytes.data() + 4 * i, static_cast<float>(std::sin(two_pi * 997 * (240 + i) / 48000)));
        actual.run({bytes.data(), bytes.size()});
        reference.run({bytes.data(), bytes.size()});
        check(std::equal(actual.sink->bytes().begin(), actual.sink->bytes().end(), reference.sink->bytes().begin()),
              "Failed converter preparation retains the original filter history and buffers");
    }
}
void identity_needs_no_workspace()
{
    for (const auto format : {Format::F32P, Format::S16LE})
    {
        const PortSpec spec(rate, 2, format);
        Convert convert(spec, spec);
        RetCode code;
        const bool failed = allocation_fails_after(0, [&] { code = convert.prepare(period); });
        check(!failed, "Identity conversion needs no allocated conversion state or workspace");
        checked(code);
    }
}
void plan_allocation_rollback()
{
    for (long failure = 0;; ++failure)
    {
        Engine engine(period);
        ManualClock clock(engine);
        const auto source =
            checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 1.0f; }));
        const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
        checked(engine.connect({source}, {sink}));
        const auto initial = checked(engine.prepare());
        checked(clock.tick());
        const auto converter =
            checked(engine.emplace_node<Convert>(PortSpec(rate, 1), PortSpec(SampleRate::Hz16000, 1)));
        const auto added = checked(engine.emplace_node<PacketSink>(PortSpec(SampleRate::Hz16000, 1)));
        checked(engine.connect({source}, {converter}));
        checked(engine.connect({converter}, {added}));
        Result<PlanRevision> revision(ErrorCode::InvalidArg);
        const bool failed = allocation_fails_after(failure, [&] { revision = engine.prepare(); });
        if (!failed)
        {
            checked(revision);
            check(failure > 0, "Fault injection visits plan allocations");
            break;
        }
        checked(clock.tick());
        check(engine.applied_revision() == initial && checked(engine.node<PacketSink>(sink))->sample(0) == 1,
              "Failed plan construction preserves the running graph");
        checked(engine.prepare());
        checked(clock.tick());
    }
}
void render_uses_prepared_buffers()
{
    Engine engine(period);
    ManualClock clock(engine);
    const auto source = checked(
        engine.emplace_node<FloatSource>(SampleRate::Hz44100, 1, [](std::uint64_t, std::size_t) { return 0.25f; }));
    const auto converter = checked(engine.emplace_node<Convert>(PortSpec(SampleRate::Hz44100, 1), PortSpec(rate, 1)));
    const auto mixer = checked(engine.emplace_node<Mixer>(rate, 1, 1));
    const auto matrix = checked(engine.emplace_node<Matrix>(rate, 1));
    const auto sink = checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    checked(engine.connect({source}, {converter}));
    checked(engine.connect({converter}, {mixer}));
    checked(engine.connect({mixer}, {matrix}));
    checked(engine.connect({matrix}, {sink}));
    checked(engine.prepare());
    checked(clock.start());
    checked(engine.emplace_node<PacketSink>(PortSpec(rate, 1)));
    const auto revision = checked(engine.prepare());
    checked(checked(engine.node<Matrix>(matrix))->set_gain(0.5f));
    checked(checked(engine.node<Mixer>(mixer))->set_muted(0, true));
    allocation_test::heap_calls = 0;
    allocation_test::measure_heap = true;
    const auto code = clock.tick();
    allocation_test::measure_heap = false;
    checked(code);
    check(allocation_test::heap_calls == 0 && engine.applied_revision() == revision,
          "Plan switching, fractional blocks, conversion, matrix gain and mixer mute allocate and reclaim nothing on "
          "the driver");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"buffer_resize_rollback", buffer_resize_rollback},
                      {"conversion_rollback", conversion_rollback},
                      {"identity_needs_no_workspace", identity_needs_no_workspace},
                      {"plan_allocation_rollback", plan_allocation_rollback},
                      {"render_uses_prepared_buffers", render_uses_prepared_buffers}});
}
#endif
