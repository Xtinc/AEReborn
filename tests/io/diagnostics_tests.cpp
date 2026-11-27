#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "backend/usr_playback.h"
#include "diag/inspect.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "tests/support/io_fixture.h"
#include "tests/support/test_backend.h"
#include <array>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

const PortSpec mono(rate, 1, SampleFormat::F32P);
const PortSpec pcm(rate, 1, SampleFormat::F32LE);

class Observer final : public NodeT<1, 0>
{
  public:
    explicit Observer(PortSpec spec) : NodeT({spec}, {}) {}
    const char *type_name() const noexcept override { return "Monitor <A> & \"B\""; }
    void process(const ProcessContext &) noexcept override {}
};

class SampledBackend final : public Backend
{
  public:
    void start() override {}
    void stop() noexcept override {}
    void unbind() noexcept override {}
    bool running() const noexcept override { return ++reads == 1; }

    mutable unsigned reads = 0;
};

void diagnostic_reports()
{
    Engine engine(period);
    const auto initial = dump_json(engine);
    check(engine.plan_nodes().empty() && engine.plan_connections().empty() &&
              initial.find("\"state\":\"Unprepared\",\"period_ms\":5,\"plan\":0,\"tick\":0") != std::string::npos &&
              initial.find("\"plan_order\":[]") != std::string::npos,
          "Diagnostics work before the initial commit");
    SampledBackend sampled;
    const Backend *backend = &sampled;
    const auto sampled_graph = dump_json(engine, {&backend, 1});
    check(sampled.reads == 1 && sampled_graph.find("\"node\":null,\"state\":\"Running\"") != std::string::npos,
          "The detached backend state is sampled once");
    const PortSpec stereo_pcm(rate, 2, SampleFormat::F32LE);
    const PortSpec converted(BandWidth::Wide, 1, SampleFormat::S16LE);
    const auto capture = engine.emplace_node<Capture>(rate, 2, 5ms);
    const auto muted_capture = engine.emplace_node<Capture>(rate, 2, 5ms);
    const auto mixer = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto matrix = engine.emplace_node<Matrix>(rate, 2, 1, std::initializer_list<float>{0.5f, 0.5f}, 0.5f);
    const auto convert = engine.emplace_node<Convert>(mono, converted);
    const auto observer = engine.emplace_node<Observer>(converted);
    const auto playback = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto detached = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto input = connect(engine, capture, mixer);
    connect(engine, muted_capture, mixer, 1);
    connect(engine, mixer, matrix);
    connect(engine, matrix, convert);
    connect(engine, convert, observer);
    connect(engine, matrix, playback);
    const auto pruned = connect(engine, matrix, detached);
    engine.node<Mixer>(mixer)->set_muted(1, true);
    engine.commit();

    PushSource source(stereo_pcm);
    PullSink sink({BandWidth::CDQuality, 1, SampleFormat::F32LE});
    TestBackend failed_source(stereo_pcm, 240);
    TestBackend disconnected(pcm, 240);
    source.bind(*engine.node<Capture>(capture));
    sink.bind(*engine.node<Playback>(playback));
    source.start();
    sink.start();
    const std::uint8_t silence[240 * 2 * sizeof(float)]{};
    for (int block = 0; block < 3; ++block)
        check(source.push({silence, stereo_pcm, 240}) == 240, "The report fixture has a partially filled capture queue");
    failed_source.bind(*engine.node<Capture>(muted_capture));
    failed_source.io_result = RetCode::EOS;
    failed_source.start();
    wait_for([&] { return !failed_source.running(); }, "The capture backend reaches the end of stream");
    disconnected.bind(*engine.node<Playback>(detached));
    disconnected.io_result = RetCode::DEVLOST;
    disconnected.start();
    wait_for([&] { return !disconnected.running(); }, "The worker reports the device failure");

    engine.set_paused(detached, true);
    engine.commit();
    engine.set_paused(muted_capture, true);
    expect_error(RetCode::ENODE, [&] { engine.plan_node(detached); });
    const auto nodes = engine.plan_nodes();
    check(nodes.size() == 7 && nodes[0] == capture && nodes[1] == muted_capture && nodes[2] == mixer &&
              nodes[3] == matrix && nodes[4] == convert && nodes[5] == observer && nodes[6] == playback,
          "The published order includes only executed nodes");
    const std::array<const Backend *, 5> backend_list{{&source, &failed_source, nullptr, &sink, &disconnected}};
    const View<const Backend *const> backends(backend_list.data(), backend_list.size());
    const auto muted_id = std::to_string(muted_capture.node());
    const auto mixer_id = std::to_string(mixer.node());
    const auto snapshot = dump_json(engine, backends);
    auto order = std::string("\"plan_order\":[");
    const char *separator = "";
    for (const auto id : {capture, muted_capture, mixer, matrix, convert, observer, playback})
    {
        order += separator + std::to_string(id.node());
        separator = ",";
    }
    check(snapshot.find(order + ']') != std::string::npos, "JSON lists the actual execution order");
    const auto paused_id = std::to_string(detached.node());
    const auto detached_error = std::string("\"node\":null,\"state\":\"Stopped\",\"error\":\"DEVLOST\",\"error_message\":\"")
                                + RetCode(RetCode::DEVLOST).what() + '"';
    check(snapshot.find("\"type\":\"Monitor <A> & \\\"B\\\"\"") != std::string::npos &&
              snapshot.find("\"id\":" + paused_id + ",\"type\":\"Playback\"") == std::string::npos &&
              snapshot.find("\"id\":" + muted_id + ",\"type\":\"Capture\",\"in_plan\":true,\"paused\":false")
                  != std::string::npos &&
              snapshot.find("\"id\":" + std::to_string(pruned.value) + ",\"from\":") == std::string::npos &&
              snapshot.find("\"to\":[" + paused_id + ",0]") == std::string::npos &&
              snapshot.find(detached_error) != std::string::npos,
          "Engine JSON reports committed nodes and connections together with detached backend status");
    check(snapshot.find("\"state\":\"Ready\"") != std::string::npos &&
              snapshot.find("\"dirty\":true") != std::string::npos &&
              snapshot.find("\"capacity\":1920,\"frames\":720") != std::string::npos &&
              snapshot.find("\"from\":[" + muted_id + ",0],\"to\":[" + mixer_id + ",1]") != std::string::npos &&
              snapshot.find("\"id\":" + std::to_string(input.value) + ",\"from\":") != std::string::npos &&
              snapshot.find("\"gain\":0.5") != std::string::npos &&
              snapshot.find("\"high\":false,\"muted\":true,\"suppressed\":false") != std::string::npos &&
              snapshot.find("\"format\":\"S16LE\"") != std::string::npos &&
              snapshot.find("\"error\":\"EOS\"") != std::string::npos,
          "JSON reports queue levels, exact ports, sample formats, and backend errors");

    engine.node<Matrix>(matrix)->set_gain(0.0f);
    check(dump_json(engine, backends).find("\"gain\":0") != std::string::npos, "JSON reports zero gain");
    source.unbind();
    check(!source.endpoint_node() && !source.logical_spec().valid(), "Unbinding removes the graph association");
    check(disconnected.uses_resampler(), "The bound asynchronous backend has a prepared FIR resampler");
    disconnected.unbind();
    check(!disconnected.uses_resampler(), "Unbinding clears the prepared resampler");
}

void mixer_priority_diagnostics()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.5f; });
    const auto mixed = engine.emplace_node<Mixer>(rate, 1, 3);
    const auto single = engine.emplace_node<Mixer>(rate, 1, 1);
    const auto monitor = engine.emplace_node<Observer>(mono);
    const auto extra_monitor = engine.emplace_node<Observer>(mono);
    connect(engine, source, mixed);
    connect(engine, source, mixed, 1);
    connect(engine, source, mixed, 2);
    connect(engine, source, single);
    connect(engine, mixed, monitor);
    connect(engine, single, extra_monitor);
    auto &mixer = *engine.node<Mixer>(mixed);
    auto &single_mixer = *engine.node<Mixer>(single);
    mixer.set_high_priority(1, true);
    mixer.set_high_priority(2, true);
    mixer.set_muted(2, true);
    single_mixer.set_high_priority(0, true);
    engine.commit();
    clock.start();
    clock.advance();

    const auto snapshot = dump_json(engine);
    check(snapshot.find("\"slots\":[{\"high\":false,\"muted\":false,\"suppressed\":true},"
                        "{\"high\":true,\"muted\":false,\"suppressed\":false},"
                        "{\"high\":true,\"muted\":true,\"suppressed\":false}]") != std::string::npos &&
              snapshot.find("\"slots\":[{\"high\":true,\"muted\":false,\"suppressed\":false}]") != std::string::npos,
          "JSON reports suppression, priority and mute in port order");

    single_mixer.set_high_priority(0, false);
    single_mixer.set_muted(0, true);
    check(dump_json(engine).find("\"slots\":[{\"high\":false,\"muted\":true,\"suppressed\":false}]")
              != std::string::npos,
          "JSON reports the single mixer's mute state");

    mixer.set_muted(1, true);
    clock.advance();
    check(dump_json(engine).find("\"suppressed\":true") == std::string::npos,
          "JSON clears suppression when the high-priority inputs are muted");
}

void endpoint_diagnostics()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto capture_id = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto playback_id = engine.emplace_node<Playback>(rate, 1, 10ms);
    connect(engine, capture_id, playback_id);
    engine.commit();
    auto &capture = *engine.node<Capture>(capture_id);
    auto &playback = *engine.node<Playback>(playback_id);
    TestBackend prefilling(pcm, 240);
    prefilling.block = true;
    prefilling.bind(capture);
    prefilling.start();
    clock.start();
    const auto prefilling_snapshot = dump_json(engine);
    check(prefilling_snapshot.find("\"state\":\"Running\"") != std::string::npos &&
              prefilling_snapshot.find("\"online\":true,\"steady\":false") != std::string::npos,
          "JSON reports running and prefill states");
    std::this_thread::sleep_for(15ms);
    clock.advance();
    check(capture.online() && !capture.steady() && capture.underrun_frames() == 0,
          "Startup silence while waiting for prefill does not count as an underrun");
    check(engine.missed_periods() > 0 && dump_json(engine).find("\"missed_periods\":0") == std::string::npos,
          "JSON reports nonzero missed periods");
    prefilling.unbind();

    PushSource source(pcm);
    PullSink sink(pcm);
    source.bind(capture);
    sink.bind(playback);
    source.start();
    sink.start();
    const auto frames_per_tick = mono.max_frames(period);
    const auto blocks = playback.capacity() / frames_per_tick + 1;
    for (std::size_t i = 0; i < blocks; ++i)
        clock.advance();
    std::size_t queued = 0;
    check(capture.underrun_frames() == blocks * frames_per_tick &&
              playback.overflow_frames() == blocks * frames_per_tick - playback.capacity() &&
              playback.estimate_queued_frames(queued) && queued == playback.capacity(),
          "An empty capture and full playback count exact graph-side missing frames");
    source.stop();
    sink.stop();
    check(!capture.online() && !capture.steady() && !playback.online() && !playback.steady(),
          "Fully closed capture and playback leave prefill inactive");
    source.start();
    sink.start();
    check(capture.underrun_frames() == 0 && playback.overflow_frames() == 0,
          "Reopening an endpoint clears the previous run's counters");
    clock.advance();
    clock.stop();
    source.stop();
    sink.stop();
    engine.reset();
    check(capture.underrun_frames() == 0 && playback.overflow_frames() == 0 &&
              engine.missed_periods() == 0 && engine.last_tick_us() == 0,
          "Engine reset clears endpoint and clock counters");

    sink.unbind();
    std::size_t drained = 0;
    UsrPlayback worker(pcm, [&](IBBlock block) noexcept { drained += block.frames(); });
    worker.bind(playback);
    worker.start();
    clock.start();
    clock.advance();
    check(playback.estimate_queued_frames(queued) && queued == frames_per_tick && queued < playback.capacity() / 2,
          "Synchronous playback has one short batch below the prefill watermark");
    worker.stop();
    clock.stop();
    check(drained == frames_per_tick && !playback.steady() && worker.last_error() == RetCode::OK,
          "Synchronous playback drains the short batch before resetting prefill");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"diagnostic_reports", diagnostic_reports},
                                                {"mixer_priority_diagnostics", mixer_priority_diagnostics},
                                                {"endpoint_diagnostics", endpoint_diagnostics}});
}
