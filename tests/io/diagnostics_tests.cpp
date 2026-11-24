#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "diag/inspect.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "tests/support/io_fixture.h"
#include "tests/support/test_backend.h"
#include <array>
#include <sstream>

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
    Observer() : NodeT({mono}, {}) {}
    const char *type_name() const noexcept override { return "Monitor \"A\""; }
    void process(const ProcessContext &) noexcept override {}
};

void diagnostic_reports()
{
    Engine engine(period);
    check(engine.plan_nodes().empty() && engine.plan_connections().empty() &&
              dump_text(engine).find("Engine: Unprepared | period=5ms | tick=0 | plan_gen=0") == 0,
          "Diagnostics work before the initial commit");
    const auto capture = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto mixer = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto matrix = engine.emplace_node<Matrix>(rate, 1, 0.5f);
    const auto convert = engine.emplace_node<Convert>(mono, PortSpec(SampleRate::Hz44100, 1, SampleFormat::F32P));
    const auto playback = engine.emplace_node<Playback>(SampleRate::Hz44100, 1, 5ms);
    const auto observer = engine.emplace_node<Observer>();
    const auto detached = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto input = connect(engine, capture, mixer);
    connect(engine, mixer, matrix);
    connect(engine, matrix, convert);
    connect(engine, convert, playback);
    connect(engine, matrix, observer);
    const auto pruned = connect(engine, matrix, detached);
    engine.node<Mixer>(mixer)->set_muted(1, true);
    engine.commit();

    PushSource source(pcm);
    PullSink sink({SampleRate::Hz44100, 1, SampleFormat::F32LE});
    TestBackend disconnected(pcm, 240);
    source.bind(*engine.node<Capture>(capture));
    sink.bind(*engine.node<Playback>(playback));
    source.start();
    sink.start();
    const std::uint8_t silence[240 * sizeof(float)]{};
    for (int block = 0; block < 3; ++block)
        check(source.push({silence, pcm, 240}) == 240, "The report fixture has a partially filled capture queue");
    disconnected.bind(*engine.node<Playback>(detached));
    disconnected.io_result = ErrorCode::DeviceLost;
    disconnected.start();
    wait_for([&] { return !disconnected.running(); }, "The worker reports the device failure");

    engine.set_paused(detached, true);
    engine.commit();
    expect_error(ErrorCode::UnknownNode, [&] { engine.plan_node(detached); });
    const auto nodes = engine.plan_nodes();
    check(nodes.size() == 6 && nodes[0] == capture && nodes[1] == mixer && nodes[2] == matrix &&
              nodes[3] == convert && nodes[4] == playback && nodes[5] == observer,
          "The published order includes only executed nodes");
    const std::array<const Backend *, 4> backend_list{{&source, nullptr, &sink, &disconnected}};
    const View<const Backend *const> backends(backend_list.data(), backend_list.size());
    const auto text = dump_text(engine, backends);
    const auto graph = dump_dot(engine, backends);
    std::ostringstream text_stream, graph_stream;
    dump_text(text_stream, engine, backends);
    dump_dot(graph_stream, engine, backends);
    check(text == text_stream.str() && graph == graph_stream.str(), "Both overloads produce the same output");
    const auto input_label = "#" + std::to_string(input.value) + ": ";
    check(text.find("Plan Order: [") != std::string::npos && text.find("in0..1=48000/1ch/F32P") != std::string::npos &&
              text.find("muted=[0, 1]") != std::string::npos && text.find("gain=0.50") != std::string::npos &&
              text.find("out0=44100/1ch/F32P") != std::string::npos && text.find(input_label) != std::string::npos &&
              text.find("PushSource (sync) state=Running dev=48000/1ch/F32LE") != std::string::npos,
          "Text reports the port formats, DSP settings, connections, and attached backends");
    const auto detached_section = text.find("Detached Backends:\n");
    check(detached_section != std::string::npos &&
              text.find(RetCode(ErrorCode::DeviceLost).message(), detached_section) != std::string::npos &&
              text.find("#" + std::to_string(pruned.value) + ": ") == std::string::npos,
          "A stopped backend and its error remain visible after its connection is pruned");
    const auto capture_id = std::to_string(capture.value);
    const auto playback_id = std::to_string(playback.value);
    check(graph.find("digraph AEReborn") == 0 &&
              graph.find("label=\"" + std::string(u8"\u03A3 Mixer")) != std::string::npos &&
              graph.find(u8"\u25B0\u25B0\u25B1\u25B1\u25B1  720/1920") != std::string::npos &&
              graph.find(u8"\\n\u00D7 0.50") != std::string::npos &&
              graph.find(u8"\\n48 \u2192 44.1 kHz") != std::string::npos &&
              graph.find("\\n48 kHz / 1 ch") != std::string::npos &&
              graph.find("Monitor \\\"A\\\"") != std::string::npos &&
              graph.find("b" + capture_id + " -> n" + capture_id) != std::string::npos &&
              graph.find("n" + playback_id + " -> b" + playback_id) != std::string::npos &&
              graph.find("[tooltip=\"" + input_label + "[" + capture_id + "]:0") != std::string::npos &&
              graph.find("[label=\"" + input_label) == std::string::npos &&
              graph.find("cluster_detached") != std::string::npos && graph.find("label=\"! Backend") != std::string::npos,
          "DOT shows key numbers with compact symbols and keeps full connection details in tooltips");

    source.unbind();
    check(!source.endpoint_node() && !source.logical_spec().valid(), "Unbinding removes the graph association");
    check(disconnected.uses_resampler(), "The bound asynchronous backend has a prepared FIR resampler");
    disconnected.unbind();
    check(!disconnected.uses_resampler(), "Unbinding clears the prepared resampler");
}

void endpoint_diagnostics()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto capture_id = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto playback_id = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, capture_id, playback_id);
    engine.commit();
    auto &capture = *engine.node<Capture>(capture_id);
    auto &playback = *engine.node<Playback>(playback_id);
    TestBackend prefilling(pcm, 240);
    prefilling.block = true;
    prefilling.bind(capture);
    prefilling.start();
    clock.start();
    clock.advance();
    check(capture.online() && !capture.steady() && capture.underrun_frames() == 0,
          "Startup silence while waiting for prefill does not count as an underrun");
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
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"diagnostic_reports", diagnostic_reports},
                                                {"endpoint_diagnostics", endpoint_diagnostics}});
}
