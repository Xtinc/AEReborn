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
    explicit Observer(PortSpec spec) : NodeT({spec}, {}) {}
    const char *type_name() const noexcept override { return "Monitor <A> & \"B\""; }
    void process(const ProcessContext &) noexcept override {}
};

void diagnostic_reports()
{
    Engine engine(period);
    check(engine.plan_nodes().empty() && engine.plan_connections().empty() &&
              dump_text(engine).find("Engine: Unprepared | period=5ms | tick=0 | plan_gen=0") == 0 &&
              dump_dot(engine).find(">UNPREPARED<") != std::string::npos,
          "Diagnostics work before the initial commit");
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
    const auto text = dump_text(engine, backends);
    const auto graph = dump_dot(engine, backends);
    check(text.find("high=") == std::string::npos && text.find("suppressed=") == std::string::npos &&
              graph.find("[HIGH]") == std::string::npos && graph.find("[SUPP]") == std::string::npos,
          "A mixer without priorities keeps the ordinary diagnostic format");
    std::ostringstream text_stream, graph_stream;
    dump_text(text_stream, engine, backends);
    dump_dot(graph_stream, engine, backends);
    check(text == text_stream.str() && graph == graph_stream.str(), "Both overloads produce the same output");
    const auto input_label = "#" + std::to_string(input.value) + ": ";
    check(text.find("Plan Order: [") != std::string::npos && text.find("in0..1=48000/2ch/F32P") != std::string::npos &&
              text.find("muted=[0, 1]") != std::string::npos && text.find("gain=0.50") != std::string::npos &&
              text.find("out0=16000/1ch/S16LE") != std::string::npos && text.find(input_label) != std::string::npos &&
              text.find("PushSource (sync) state=Running dev=48000/2ch/F32LE") != std::string::npos,
          "Text reports the port formats, DSP settings, connections, and attached backends");
    const auto detached_section = text.find("Detached Backends:\n");
    check(detached_section != std::string::npos &&
              text.find(RetCode(RetCode::DEVLOST).what(), detached_section) != std::string::npos &&
              text.find("#" + std::to_string(pruned.value) + ": ") == std::string::npos,
          "A stopped backend and its error remain visible after its connection is pruned");

    const auto capture_id = std::to_string(capture.node());
    const auto muted_id = std::to_string(muted_capture.node());
    const auto mixer_id = std::to_string(mixer.node());
    const auto matrix_id = std::to_string(matrix.node());
    const auto playback_id = std::to_string(playback.node());
    check(graph.find("digraph AEReborn") == 0 && graph.find("shape=plain") != std::string::npos &&
              graph.find("STYLE=\"ROUNDED\" BGCOLOR=\"#DFF4F0\" COLOR=\"#2E8B7D\"><TR><TD ALIGN=\"LEFT\">")
                  != std::string::npos &&
              graph.find(">READY<") != std::string::npos && graph.find(">DIRTY<") != std::string::npos &&
              graph.find(">ONLINE<") != std::string::npos && graph.find(">OFFLINE<") != std::string::npos &&
              graph.find("Queue 720/1920 (38%)") != std::string::npos &&
              graph.find("Monitor &lt;A&gt; &amp; &quot;B&quot;") != std::string::npos,
          "DOT cards fill the rounded outline and show status, queue numbers, and safely escaped titles");
    check(graph.find("PORT=\"i1\"") != std::string::npos && graph.find("in1 [MUTE]") != std::string::npos &&
              graph.find("PORT=\"o0\" ROWSPAN=\"2\"") != std::string::npos &&
              graph.find("n" + muted_id + ":e -> n" + mixer_id + ":i1:w [label=\"48k 2ch F32P (muted)\", style=dashed")
                  != std::string::npos &&
              graph.find("n" + mixer_id + ":o0:e -> n" + matrix_id + ":w") != std::string::npos &&
              graph.find("2ch -&gt; 1ch | Gain 0.50") != std::string::npos &&
              graph.find("48k F32P -&gt; 16k S16LE") != std::string::npos,
          "DOT anchors ports, mutes the matching edge, and displays channel, gain and format conversion");
    check(graph.find("b" + capture_id + ":e -> n" + capture_id + ":w") != std::string::npos &&
              graph.find("b" + muted_id + ":e -> n" + muted_id + ":w [style=dashed, color=\"#DC2626\", label=\"ASRC\"")
                  != std::string::npos &&
              graph.find("n" + playback_id + ":e -> b" + playback_id + ":w [style=dashed, color=\"#64748B\", label=\"FIR\"")
                  != std::string::npos &&
              graph.find("tooltip=\"" + input_label + "[" + capture_id + "]:0") != std::string::npos &&
              graph.find("cluster_detached") != std::string::npos && graph.find("ERR: DEVLOST") != std::string::npos &&
              graph.find("ERR: EOS") != std::string::npos &&
              graph.find(RetCode(RetCode::DEVLOST).what()) != std::string::npos,
          "DOT labels backend modes and error codes and retains detailed tooltips for edges and failures");

    engine.node<Matrix>(matrix)->set_gain(0.0f);
    check(dump_dot(engine, backends).find("Gain 0.00 [MUTE]") != std::string::npos, "Zero gain is visible on the card");
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

    const auto text = dump_text(engine);
    check(text.find("muted=[0, 0, 1] high=[0, 1, 1] suppressed=[1, 0, 0]") != std::string::npos,
          "Text reports requested priorities and latest suppression in port order");
    const auto graph = dump_dot(engine);
    const auto mixer_edge =
        "n" + std::to_string(source.node()) + ":e -> n" + std::to_string(mixed.node()) + ":i";
    check(graph.find("in0 [SUPP]</FONT>") != std::string::npos &&
              graph.find("in1 [HIGH]") != std::string::npos && graph.find("in2 [HIGH MUTE]") != std::string::npos,
          "DOT labels suppressed, high, and muted high inputs");
    check(graph.find(mixer_edge + "0:w [label=\"48k 1ch F32P (suppressed)\", style=dashed, "
                                  "color=\"#D97706\", fontcolor=\"#92400E\"") != std::string::npos &&
              graph.find("48000/1ch/F32P, suppressed)") != std::string::npos &&
              graph.find(mixer_edge + "2:w [label=\"48k 1ch F32P (muted)\", style=dashed, color=\"#9CA3AF\"") !=
                  std::string::npos,
          "DOT gives suppressed and muted connections distinct edge styles and tooltips");
    const auto single_id = std::to_string(single.node());
    check(graph.find("in0 [HIGH]") != std::string::npos &&
              graph.find(" -> n" + single_id + ":i0:w") != std::string::npos &&
              graph.find("n" + single_id + ":o0:e -> n" + std::to_string(extra_monitor.node()) + ":w") !=
                  std::string::npos,
          "A single high-priority mixer exposes matching port labels and edge anchors");

    single_mixer.set_high_priority(0, false);
    single_mixer.set_muted(0, true);
    const auto muted_graph = dump_dot(engine);
    const auto single_card = muted_graph.find("\n  n" + single_id + " [");
    check(single_card != std::string::npos &&
              muted_graph.find("in0 [MUTE]</FONT>", single_card) < muted_graph.find('\n', single_card + 1),
          "A single muted mixer displays the mute label on its input port");
    const auto single_edge = "n" + std::to_string(source.node()) + ":e -> n" + single_id + ":i0:w";
    check(muted_graph.find(single_edge + " [label=\"48k 1ch F32P (muted)\", style=dashed, color=\"#9CA3AF\"") !=
              std::string::npos,
          "The single muted mixer's input edge is anchored, dashed and grey");

    mixer.set_muted(1, true);
    clock.advance();
    check(dump_text(engine).find("muted=[0, 1, 1] high=[0, 1, 1] suppressed=[0, 0, 0]") != std::string::npos &&
              dump_dot(engine).find("[SUPP]") == std::string::npos,
          "The reports clear suppression when the high-priority inputs are muted");
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
    const auto prefilling_graph = dump_dot(engine);
    check(prefilling_graph.find(">RUNNING<") != std::string::npos &&
              prefilling_graph.find(">PREFILL<") != std::string::npos,
          "DOT reports running and prefill states");
    std::this_thread::sleep_for(15ms);
    clock.advance();
    check(capture.online() && !capture.steady() && capture.underrun_frames() == 0,
          "Startup silence while waiting for prefill does not count as an underrun");
    check(dump_dot(engine).find(">LATE ") != std::string::npos, "DOT highlights missed periods");
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
