#include "tests/support/io_fixture.h"
#include "tests/support/test_backend.h"
#include "backend/null_backend.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "core/command_queue.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "app/graph_service.h"
#include "audio_interface.h"
#include <cstdlib>
#include <future>
#include <memory>
#include <new>

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
int main()
{
    std::cout << "SKIP: MSVC debug iterator proxies allocate inside noexcept\n";
    return 77;
}
#else
namespace heap
{
thread_local int fail_after = -1;
thread_local bool measuring = false;
thread_local std::size_t operations = 0;
std::atomic<std::size_t> worker_operations{0};
} // namespace heap

void *operator new(std::size_t size)
{
    if (heap::measuring)
        ++heap::operations;
    if (heap::fail_after == 0)
        throw std::bad_alloc();
    if (heap::fail_after > 0)
        --heap::fail_after;
    if (auto *result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}

void operator delete(void *memory) noexcept
{
    if (memory && heap::measuring)
        ++heap::operations;
    std::free(memory);
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
const PortSpec planar(rate, 1, SampleFormat::F32P), pcm(rate, 1, SampleFormat::F32LE);

template <class Operation> bool allocation_fails(int countdown, Operation operation)
{
    heap::fail_after = countdown;
    const auto restore = []() noexcept { heap::fail_after = -1; };
    ScopeExit<decltype(restore)> reset(restore);
    try
    {
        operation();
        return false;
    }
    catch (const std::bad_alloc &)
    {
        return true;
    }
}

void command_queue_timer_allocation_rollback()
{
    std::promise<void> fired;
    auto completed = fired.get_future();
    bool released = false;
    CommandQueue queue;
    const auto release = [&]() noexcept {
        queue.cancel(1);
        released = true;
    };
    auto guard = std::make_shared<ScopeExit<decltype(release)>>(release);
    CommandQueue::Task task = [guard = std::move(guard)] { (void)guard; };
    check(allocation_fails(0, [&] { queue.post_after(1h, std::move(task)); }),
          "Growing the timer list propagates the injected allocation failure");
    check(released, "The rejected task releases its capture outside the queue lock");
    check(queue.post_after(0ms, [&] { fired.set_value(); }) == 1, "A failed registration leaves the first ID available");
    check(completed.wait_for(3s) == std::future_status::ready, "The retried timer runs");
}

void commit_allocation_rollback()
{
    const PortSpec output(BandWidth::Wide, 1, SampleFormat::F32P);
    for (int at = 0; at < 128; ++at)
    {
        Engine engine(period);
        ClockedGraph clock(engine);
        const auto source = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.5f; });
        const auto original = engine.emplace_node<PacketSink>(planar);
        connect(engine, source, original);
        engine.commit();
        clock.start();
        clock.advance();
        const auto converter = engine.emplace_node<Convert>(planar, output);
        const auto added = engine.emplace_node<PacketSink>(output);
        connect(engine, source, converter);
        connect(engine, converter, added);
        auto *before = engine.node<PacketSink>(original);
        auto *after = engine.node<PacketSink>(added);
        clock.resume();
        const auto failed = allocation_fails(at, [&] { engine.commit(); });
        clock.advance();
        if (failed)
        {
            check(before->sample(0) == 0.5f && !after->frames, "A failed allocation keeps the old plan rendering");
            clock.commit();
        }
        clock.advance();
        check(before->sample(0) == 0.5f && after->frames == 80, "The complete route becomes visible on successful commit");
        if (!failed)
        {
            check(at > 0, "The graph exercises allocation failure injection");
            return;
        }
    }
    check(false, "All graph preparation allocations were visited");
}

void channel_map_allocation_status()
{
    ChannelMap empty, dense;
    const std::vector<float> coefficients{1.0f, 0.0f};
    check(!allocation_fails(0, [&] { empty = ChannelMap(2, 1); }),
          "Construction reports allocation failure through the map");
    check(!allocation_fails(0, [&] { dense = ChannelMap::dense(2, 1, coefficients); }),
          "Dense construction catches allocation failure when copying the coefficients");
    check(empty.route(10, 10).status() == RetCode::TOOLARGE && dense.status() == RetCode::TOOLARGE,
          "Allocation failure is retained as the first error");
}

void graph_service_allocation_rollback()
{
    TempWave files("graph-rollback");
    const auto path = files.path();
    CommandQueue queue;
    queue.sync([&] {
        GraphService graph(queue, period);
        NodeId sink;
        const PortSpec target(BandWidth::CDQuality, 1, SampleFormat::S16LE);
        for (int at = 0; at < 128; ++at)
        {
            if (!allocation_fails(at, [&] { sink = graph.create_owav(path, target, 0s, 10ms); }))
                break;
            WavFile file;
            check(file.open_read(path) == RetCode::IOFAILED, "Allocation failure must precede exclusive file creation");
        }
        check(static_cast<bool>(sink), "The same WAV path remains available for retry");

        const auto source = graph.create_matrix(BandWidth::Full, 2, 1.0f);
        graph.commit();
        bool rolled_back_partial_edit = false;
        for (int at = 0; at < 128; ++at)
        {
            const auto failed = allocation_fails(at, [&] { graph.connect(source, sink); });
            const auto draft = graph.dump_text();
            if (failed)
                rolled_back_partial_edit |= draft.find("dirty=true") != std::string::npos;
            graph.commit();
            const auto plan = graph.dump_text();
            const auto matrix = plan.find("] Matrix") != std::string::npos;
            const auto converter = plan.find("] Convert") != std::string::npos;
            if (failed)
            {
                check(!matrix && !converter, "A failed bridge never becomes part of a plan");
                expect_error(RetCode::ECONN, [&] { graph.disconnect(sink); });
            }
            else
            {
                check(matrix && converter && rolled_back_partial_edit, "A partially applied bridge can be retried");
                graph.disconnect(sink);
                graph.commit();
                check(graph.dump_text().find("] Convert") == std::string::npos, "Disconnect removes the bridge");
                return;
            }
        }
        check(false, "The bridge eventually connects after the allocation failures");
    });
}

void graph_service_live_commit_failure()
{
    CommandQueue queue;
    bool preparation_failed = false;
    int backend_start_failures = 0;
    for (int at = 0; at < 128; ++at)
    {
        TempWave files("live-commit");
        const auto first_path = files.path("-first.wav");
        const auto second_path = files.path("-second.wav");
        const auto third_path = files.path("-third.wav");
        const auto failed = queue.sync([&] {
            GraphService graph(queue, period);
            const auto first = graph.create_owav(first_path, pcm, 0s, 10ms);
            const auto second = graph.create_owav(second_path, pcm, 0s, 10ms);
            const auto third = graph.create_owav(third_path, pcm, 0s, 10ms);
            graph.set_paused(second, true);
            graph.set_paused(third, true);
            graph.start();
            graph.set_paused(second, false);
            graph.set_paused(third, false);

            if (!allocation_fails(at, [&] { graph.commit(); }))
                return false;
            const auto after = graph.dump_text();
            if (after.find("dirty=true") != std::string::npos)
            {
                preparation_failed = true;
                check(after.find("Engine: Running") != std::string::npos &&
                          after.find("[" + std::to_string(first.node()) + "] Playback") != std::string::npos &&
                          after.find("[" + std::to_string(third.node()) + "] Playback") == std::string::npos &&
                          after.find("state=Running") != std::string::npos,
                      "Preparation failure keeps the old plan and its backend running");
                graph.commit();
            }
            else
            {
                ++backend_start_failures;
                check(after.find("Engine: Ready") != std::string::npos &&
                          after.find("[" + std::to_string(third.node()) + "] Playback") != std::string::npos &&
                          after.find("state=Running") == std::string::npos,
                      "Failure after publication stops the clock and all backends and keeps the new plan");
                graph.start();
            }
            const auto recovered = graph.dump_text();
            const auto first_running = recovered.find("state=Running");
            const auto second_running = recovered.find("state=Running", first_running + 1);
            check(first_running != std::string::npos && second_running != std::string::npos &&
                      recovered.find("state=Running", second_running + 1) != std::string::npos,
                  "Retry brings up every backend");
            return true;
        });
        if (!failed)
        {
            check(preparation_failed && backend_start_failures >= 2,
                  "Visit preparation and both new backend starts during a live commit");
            return;
        }
    }
    check(false, "Live graph submission eventually succeeds");
}

template <class Driver, class Endpoint>
void check_binding_rollback(Driver &driver, Endpoint &endpoint, NullBackend &replacement)
{
    for (int at = 0; at < 128; ++at)
    {
        if (!allocation_fails(at, [&] { driver.bind(endpoint); }))
        {
            check(at > 0 && driver.bound(), "A retry eventually prepares the complete backend workspace");
            driver.unbind();
            return;
        }
        check(!driver.bound() && driver.device_spec() == PortSpec{} && !driver.device_max_frames(),
              "An allocation failure clears the provisional binding");
        replacement.bind(endpoint);
        replacement.unbind();
    }
    check(false, "All binding allocations were visited");
}

void backend_allocation_rollback()
{
    const PortSpec device(BandWidth::Wide, 1, SampleFormat::S16LE);
    Capture capture(rate, 1, 5ms);
    Playback playback(rate, 1, 5ms);
    capture.prepare(period);
    playback.prepare(period);
    BackendCalls calls;
    TestBackend active(device, 80, &calls);
    PushSource push(device);
    PullSink pull(device);
    NullBackend replacement(SampleFormat::F32LE);
    check_binding_rollback(active, capture, replacement);
    check(calls.opens == calls.closes, "The active driver closes every provisional device");
    check_binding_rollback(push, capture, replacement);
    check_binding_rollback(pull, playback, replacement);
}

class MeasureRender final : public NodeT<1, 0>
{
  public:
    explicit MeasureRender(bool start) : NodeT({planar}, {}), start_(start) {}

    void process(const ProcessContext &) noexcept override
    {
        if (start_)
        {
            heap::operations = 0;
            heap::measuring = true;
        }
        else
        {
            heap::measuring = false;
            operations += heap::operations;
            ++blocks;
        }
    }

    std::size_t operations = 0, blocks = 0;

  private:
    bool start_;
};

void render_and_passive_io_without_heap()
{
    const auto source_rate = BandWidth::CDQuality;
    const PortSpec source_spec(source_rate, 1, SampleFormat::F32LE);
    Engine engine(period);
    ClockedGraph clock(engine);
    engine.emplace_node<MeasureRender>(true);
    const auto capture = engine.emplace_node<Capture>(source_rate, 1, 5ms);
    const auto convert = engine.emplace_node<Convert>(PortSpec(source_rate, 1, SampleFormat::F32P), planar);
    const auto matrix = engine.emplace_node<Matrix>(rate, 1, 0.5f);
    const auto mixer = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto background = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.25f; });
    const auto playback = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, capture, convert);
    connect(engine, convert, matrix);
    connect(engine, matrix, mixer);
    connect(engine, background, mixer, 1);
    engine.node<Mixer>(mixer)->set_high_priority(0, true);
    connect(engine, mixer, playback);
    const auto probe = engine.emplace_node<MeasureRender>(false);
    engine.commit();
    const auto *render = engine.node<MeasureRender>(probe);
    PushSource input(source_spec);
    PullSink output(pcm);
    input.bind(*engine.node<Capture>(capture));
    output.bind(*engine.node<Playback>(playback));
    input.start();
    output.start();
    clock.start();
    Pcm source(source_spec, 221), sink(pcm, 240);
    std::size_t transfers = 0;
    for (std::size_t tick = 0; tick < 8; ++tick)
    {
        const std::size_t frames = tick % 2 ? 220 : 221;
        // Cover the initial idle block, streaming, tail flush, idle, and resumed input.
        const bool active = tick != 0 && (tick < 4 || tick >= 6);
        heap::operations = 0;
        heap::measuring = true;
        const auto pushed = active ? input.push(source.input(0, frames)) : 0;
        heap::measuring = false;
        transfers += heap::operations;
        clock.advance();
        heap::operations = 0;
        heap::measuring = true;
        const auto pulled = output.pull(sink.output(0, 240));
        heap::measuring = false;
        transfers += heap::operations;
        check(pushed == (active ? frames : 0) && pulled == 240,
              "Passive playback keeps transferring blocks across capture gaps");
    }
    check(render->blocks == 8 && render->operations == 0 && transfers == 0,
          "DSP, render buffers and passive backend I/O reuse prepared memory");
}

void active_workers_without_heap()
{
    Capture capture(rate, 1, 5ms);
    Playback playback(rate, 1, 5ms);
    capture.prepare(period);
    playback.prepare(period);
    TestBackend source(pcm, 240), sink(pcm, 240);
    const auto measure_worker = [](bool start) {
        if (start)
            heap::operations = 0;
        else
            heap::worker_operations.fetch_add(heap::operations, std::memory_order_relaxed);
        heap::measuring = start;
    };
    source.measure_io = sink.measure_io = measure_worker;
    source.gated = sink.gated = true;
    source.bind(capture);
    sink.bind(playback);
    float samples[240]{};
    const OBBlock block(reinterpret_cast<std::uint8_t *>(samples), planar, 240);
    const IBBlock output = block;
    heap::worker_operations = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        source.start();
        sink.start();
        check(source.wait_for_io() && sink.wait_for_io(), "Both workers reach their first batch");
        for (std::uint64_t tick = 0; tick < 16; ++tick)
        {
            check(source.run_batch(), "The capture worker supplies a batch");
            capture.process({period, tick, {}, {&block, 1}});
            playback.process({period, tick, {&output, 1}, {}});
            check(sink.run_batch(), "The playback worker consumes a batch");
        }
        source.stop();
        sink.stop();
        check(source.last_error() == RetCode::OK && sink.last_error() == RetCode::OK,
              "Both workers stop after steady transfers");
    }
    check(heap::worker_operations.load() == 0, "The restarted workers use the prepared endpoint buffers");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"commit_allocation_rollback", commit_allocation_rollback},
                                                {"backend_allocation_rollback", backend_allocation_rollback},
                                                {"channel_map_allocation_status", channel_map_allocation_status},
                                                {"graph_service_allocation_rollback", graph_service_allocation_rollback},
                                                {"graph_service_live_commit_failure", graph_service_live_commit_failure},
                                                {"command_queue_timer_allocation_rollback",
                                                 command_queue_timer_allocation_rollback},
                                                {"render_and_passive_io_without_heap", render_and_passive_io_without_heap},
                                                {"active_workers_without_heap", active_workers_without_heap}});
}
#endif
