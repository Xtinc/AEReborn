#include "plugins/endpoint.h"
#include "backend/alsa_backend.h"
#include "backend/null_backend.h"
#include "backend/wasapi_backend.h"
#include "backend/wav_backend.h"
#include "backend/wav_file.h"
#include "core/os.h"
#include "support/test_backend.h"
#include "support/audio_fixture.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <thread>

#if LINUX_OS_ENVIRONMENT
#include <csignal>
#include <sys/resource.h>
#endif

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using aereborn::Capture;
using aereborn::Playback;
using namespace std::chrono;

constexpr auto graph_period = BlockPeriod::Ms02;
constexpr auto short_packet = microseconds(2667);

struct FloatPacket
{
    FloatPacket(PortSpec spec, std::size_t frames) : spec(spec), frames(frames), samples(spec.channels * frames) {}

    OBBlock block() noexcept
    {
        return {reinterpret_cast<std::uint8_t *>(samples.data()), spec, frames};
    }

    OFBlock view() noexcept { return block().f32p_view(); }

    PortSpec spec;
    std::size_t frames;
    std::vector<float> samples;
};

struct FloatDevicePacket
{
    FloatDevicePacket(SampleRate rate, std::size_t frames)
        : spec(rate, 1, SampleFormat::F32LE), frames(frames), bytes(frames * sizeof(float))
    {
    }

    OBBlock block(std::size_t offset, std::size_t count)
    {
        return {bytes.data() + offset * sizeof(float), spec, count};
    }

    OBBlock block() { return block(0, frames); }

    void fill(float value)
    {
        for (std::size_t i = 0; i < frames; ++i)
            write_f32le(bytes.data() + i * sizeof(float), value);
    }

    float sample(std::size_t i) const { return read_f32le(bytes.data() + i * sizeof(float)); }

    PortSpec spec;
    std::size_t frames;
    std::vector<std::uint8_t> bytes;
};

void render(Capture &node, OBBlock block)
{
    node.process({graph_period, 0, {}, {&block, 1}});
}

void render(Playback &node, IBBlock block)
{
    node.process({graph_period, 0, {&block, 1}, {}});
}

void wait_for_exit(Backend &backend)
{
    const auto deadline = steady_clock::now() + seconds(3);
    while (backend.running() && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(1));
    check(!backend.running(), "The I/O worker exits");
}

void stop_in_flight()
{
    Playback playback(rate, 1, short_packet);
    playback.prepare(graph_period);
    FloatPacket input(playback.spec(), 96);
    std::fill(input.samples.begin(), input.samples.end(), 0.25f);
    FloatDevicePacket output(rate, 128);
    TestBackend backend(output.spec, output.frames, nullptr, false);
    backend.output = output.block();
    backend.bind(playback);
    backend.start();

    std::atomic<unsigned> rendered{0};
    std::atomic<bool> done{false};
    std::thread graph([&] {
        while (!done.load(std::memory_order_acquire))
        {
            render(playback, input.block());
            rendered.fetch_add(1, std::memory_order_release);
        }
    });
    const auto wait_for_render = [&](unsigned blocks) {
        const auto deadline = steady_clock::now() + seconds(2);
        while (rendered.load(std::memory_order_acquire) < blocks && steady_clock::now() < deadline)
            std::this_thread::yield();
        return rendered.load(std::memory_order_acquire) >= blocks;
    };
    const bool filled = wait_for_render(8);
    backend.stop();
    const bool continued = wait_for_render(rendered.load(std::memory_order_relaxed) + 8);
    done.store(true, std::memory_order_release);
    graph.join();
    check(filled && continued && backend.written && output.sample(0) == 0.25f,
          "Stopping sync playback drains audio while graph rendering continues");
    backend.written = 0;
    backend.start();
    backend.stop();
    check(!backend.written && backend.last_error() == ErrorCode::Ok,
          "Graph blocks rendered after stop do not appear in the next recording");
}

void streaming_ranges()
{
    const SampleRate pairs[][2] = {{SampleRate::Hz96000, SampleRate::Hz8000},
                                  {SampleRate::Hz8000, SampleRate::Hz96000},
                                  {SampleRate::Hz44100, SampleRate::Hz48000}};
    for (const auto &pair : pairs)
    {
        const PortSpec src(pair[0], 2, SampleFormat::F32P), dst(pair[1], 2, SampleFormat::F32P);
        SincResampler whole, split;
        whole.prepare(2, pair[0], pair[1]);
        split.prepare(2, pair[0], pair[1]);
        FloatPacket input(src, 1410), reference(dst, 110), output(dst, 110);
        for (std::size_t i = 0; i < input.samples.size(); ++i)
            input.samples[i] = static_cast<float>(std::sin(i * 0.13));
        std::fill(output.samples.begin(), output.samples.end(), -2.0f);
        const auto need = whole.required_input(100);
        const auto pass = whole.process(input.view().subblock(3, need), reference.view().subblock(3, 100));
        check(pass.consumed == need && pass.produced == 100, "Demand equals the actual source consumption");

        std::size_t read = 0, written = 0;
        const std::array<std::size_t, 6> spans{{1, 1, 11, 17, 31, 39}};
        for (const auto span : spans)
        {
            const auto needed = split.required_input(span);
            if (pair[0] == SampleRate::Hz8000 && written == 1)
                check(needed == 0, "Valid upsampling can generate another frame from history");
            const auto part = split.process(input.view().subblock(3 + read, needed),
                                                    output.view().subblock(3 + written, span));
            check(part.consumed == needed && part.produced == span, "No tail storage is required with exact demands");
            read += part.consumed;
            written += part.produced;
        }
        check(read == need && written == 100, "One-piece and segmented processing consume the same source ranges");
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const auto *actual = output.block().f32p_view().channel(ch);
            const auto *expected = reference.block().f32p_view().channel(ch);
            check(actual[2] == -2 && actual[103] == -2 && std::equal(actual + 3, actual + 103, expected + 3),
                  "Segmented FIR preserves phase, channel stride and surrounding frames");
        }
    }
}

void stream_step_changes()
{
    const PortSpec spec(rate, 1, SampleFormat::F32P);
    SincResampler stream;
    stream.prepare(1, rate, rate, 0.002);
    FloatPacket input(spec, 1000), output(spec, 120);
    for (std::size_t i = 0; i < input.frames; ++i)
        input.samples[i] = static_cast<float>(std::sin(i * 0.07));

    std::size_t read = 0, count = 0;
    double source_position = 0;
    for (const auto scale : {1.001, 1.0, 0.999, 1.0, 1.002, 0.998})
    {
        const auto need = stream.required_input(120, scale);
        const auto part = stream.process(input.view().subblock(read, need), output.view(), scale);
        check(part.consumed == need && part.produced == 120, "A new step preserves the source accounting");
        for (const auto sample : output.samples)
        {
            if (count++ >= 96)
                check(std::abs(sample - std::sin((source_position - 48) * 0.07)) < 0.002,
                      "Changing step through unity preserves the filter delay and phase");
            source_position += scale;
        }
        read += part.consumed;
    }
}

template <class Endpoint>
void start_gated(Endpoint &node, TestBackend &backend)
{
    backend.gated = true;
    backend.bind(node);
    backend.start();
    check(backend.wait_for_io(), "The I/O worker reaches its first batch");
}

void run_batch(TestBackend &backend)
{
    check(backend.run_batch(), "The I/O worker finishes the batch");
}

void lifecycle()
{
    BackendCalls calls;
    Capture capture(rate, 1, short_packet);
    const PortSpec pcm(rate, 1, SampleFormat::F32LE);
    TestBackend backend(pcm, 128, &calls), replacement(pcm, 128);
    expect_error(ErrorCode::NotReady, [&] { backend.start(); });
    expect_error(ErrorCode::NotReady, [&] { backend.bind(capture); });
    check(!calls.opens && !backend.bound(), "Binding needs a prepared graph endpoint");
    expect_error(ErrorCode::InvalidArg, [&] { capture.prepare(static_cast<BlockPeriod>(0)); });
    expect_error(ErrorCode::InvalidArg, [&] { capture.prepare(static_cast<BlockPeriod>(3)); });
    capture.prepare(graph_period);

    const auto check_rebind = [&] {
        replacement.bind(capture);
        replacement.unbind();
    };
    const auto reject = [&](PortSpec spec, std::size_t frames, ErrorCode error) {
        TestBackend invalid(spec, frames, &calls);
        const auto before = calls.closes;
        expect_error(error, [&] { invalid.bind(capture); });
        check(calls.closes == before + 1 && !invalid.bound() && !invalid.device_max_frames(),
              "Failed negotiation closes the device and clears the batch limit");
        check_rebind();
    };
    reject(PortSpec(rate, 1, SampleFormat::F32P), 128, ErrorCode::InvalidArg);
    reject(PortSpec(rate, 2, SampleFormat::F32LE), 128, ErrorCode::FormatMismatch);
    reject(pcm, 0, ErrorCode::InvalidArg);
    reject(pcm, 129, ErrorCode::TooLarge);

    const auto before_failed_opens = calls.closes;
    backend.open_result = ErrorCode::IoFailed;
    expect_error(ErrorCode::IoFailed, [&] { backend.bind(capture); });
    check(!backend.bound() && calls.closes == before_failed_opens + 1, "An open error rolls back the binding");
    check_rebind();
    backend.open_result = {};
    backend.throw_on_open = true;
    expect_exception<std::bad_alloc>([&] { backend.bind(capture); });
    check(!backend.bound() && calls.closes == before_failed_opens + 2, "An open exception rolls back the binding");
    check_rebind();
    backend.throw_on_open = false;

    backend.start_result = ErrorCode::DeviceLost;
    backend.bind(capture);
    check(calls.rate == rate && calls.channels == 1 && calls.period == short_packet,
          "Open receives the endpoint's device period and graph format hints");
    check(backend.bound() && backend.device_spec() == pcm && backend.device_max_frames() == 128 && !backend.running(),
          "Bind publishes the negotiated specification before starting I/O");
    expect_error(ErrorCode::Busy, [&] { backend.bind(capture); });
    expect_error(ErrorCode::Busy, [&] { replacement.bind(capture); });
    expect_error(ErrorCode::Busy, [&] { capture.prepare(BlockPeriod::Ms05); });
    expect_error(ErrorCode::DeviceLost, [&] { backend.start(); });
    check(backend.bound() && !backend.running() && calls.stops == 0,
          "A failed start keeps the binding without calling the device stop hook");

    backend.start_result = {};
    backend.block = true;
    backend.start();
    check(backend.wait_for_io(), "A started capture reaches device I/O");
    check(calls.start_thread != std::this_thread::get_id(), "The concrete device starts on the I/O worker");
    expect_error(ErrorCode::Busy, [&] { backend.start(); });
    backend.stop();
    backend.stop();
    check(!backend.running() && backend.bound() && backend.last_error() == ErrorCode::Ok &&
              calls.stops == 1 && calls.cancels == 1 &&
              calls.stop_thread == calls.start_thread,
          "Stop cancels a blocked read and keeps the backend open without recording an error");
    expect_error(ErrorCode::Busy, [&] { replacement.bind(capture); });
    backend.start();
    check(backend.wait_for_io(), "The same binding starts again");
    const auto before = calls.closes;
    backend.unbind();
    backend.unbind();
    check(!backend.bound() && !backend.device_spec().valid() && !backend.device_max_frames() &&
              calls.closes == before + 1 && calls.stops == 2 && calls.cancels == 2,
          "Repeated unbind releases the binding once");
    replacement.bind(capture);
}

void queue_ranges()
{
    const PortSpec logical(rate, 2, SampleFormat::F32P), pcm(rate, 2, SampleFormat::F32LE);
    Capture capture(rate, 2, short_packet);
    capture.prepare(graph_period);
    FloatPacket graph(logical, 96);
    std::array<std::uint8_t, 128 * 8> bytes{};
    for (std::size_t frame = 0; frame < 128; ++frame)
    {
        write_f32le(bytes.data() + frame * 8, 0.25f);
        write_f32le(bytes.data() + frame * 8 + 4, -0.5f);
    }
    TestBackend backend(pcm, 128);
    backend.input = {bytes.data(), pcm, 128};
    backend.read_frames = 64;
    start_gated(capture, backend);
    run_batch(backend);
    render(capture, graph.block());
    check(graph.samples.back() == 0, "Capture renders silence below its prefill watermark");
    backend.read_frames = 128;
    for (int i = 0; i < 2; ++i)
        run_batch(backend);
    render(capture, graph.block());
    check(graph.samples.back() == 0, "A partial queue remains buffered across graph ticks");
    run_batch(backend);
    render(capture, graph.block());
    check(std::abs(graph.view().channel(0)[95] - 0.25f) < 1e-5f &&
              std::abs(graph.view().channel(1)[95] + 0.5f) < 1e-5f, "Interleaved capture preserves planar channels");

    for (int i = 0; i < 10; ++i)
        run_batch(backend);
    for (int i = 0; i < 8; ++i)
    {
        render(capture, graph.block());
        check(std::abs(graph.samples.back() + 0.5f) < 1e-5f, "A full capture queue supplies eight complete blocks");
    }
    render(capture, graph.block());
    check(graph.samples.back() == 0, "An empty capture queue pads the block with silence");
    backend.stop();
}

void planar_playback()
{
    const PortSpec logical(SampleRate::Hz44100, 2, SampleFormat::F32P);
    const PortSpec pcm(SampleRate::Hz44100, 2, SampleFormat::F32LE);
    Playback playback(logical.sample_rate, logical.channels, microseconds(2903));
    playback.prepare(graph_period);
    TestBackend backend(pcm, 128);
    std::array<std::uint8_t, 128 * 8> bytes{};
    backend.output = {bytes.data(), pcm, 128};
    start_gated(playback, backend);
    for (std::size_t tick = 0; tick < 6; ++tick)
    {
        FloatPacket packet(logical, logical.frames(graph_period, tick));
        std::fill_n(packet.view().channel(0), packet.frames, 0.25f);
        std::fill_n(packet.view().channel(1), packet.frames, -0.5f);
        render(playback, packet.block());
        if (tick == 3)
        {
            run_batch(backend);
            check(backend.written == 128 && std::all_of(bytes.begin(), bytes.end(),
                                                        [](std::uint8_t byte) { return byte == 0; }),
                  "Four 44.1 kHz graph blocks remain below the playback prefill target");
        }
    }
    run_batch(backend);
    check(backend.written == 128, "A prefilled playback emits one complete batch");
    check(std::abs(read_f32le(bytes.data() + 127 * 8) - 0.25f) < 1e-5f &&
              std::abs(read_f32le(bytes.data() + 127 * 8 + 4) + 0.5f) < 1e-5f, "Playback interleaves both channels");
    backend.batch_frames = 61;
    run_batch(backend);
    check(backend.written == 61, "Playback honors variable writable frame counts");
    backend.batch_frames = 128;
    for (int batch = 0; batch < 3; ++batch)
        run_batch(backend);
    check(backend.written == 128 && std::abs(read_f32le(bytes.data() + 48 * 8) - 0.25f) < 1e-5f &&
              read_f32le(bytes.data() + 127 * 8) == 0 && read_f32le(bytes.data() + 127 * 8 + 4) == 0,
          "A partially filled playback batch keeps its audio and clears both channels in the tail");
    run_batch(backend);
    check(std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
          "A fully depleted playback queue writes a silent batch");
    backend.batch_frames = 129;
    check(!backend.run_batch(), "A malformed batch ends the worker");
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::IoFailed, "A malformed batch stops playback and reports an error");
}

void offline_capture()
{
    const PortSpec logical(SampleRate::Hz8000, 1, SampleFormat::F32P);
    const PortSpec pcm(SampleRate::Hz96000, 1, SampleFormat::F32LE);
    Capture capture(logical.sample_rate, 1, milliseconds(2));
    capture.prepare(graph_period);
    FloatDevicePacket input(pcm.sample_rate, 192);
    input.fill(0.25f);
    TestBackend backend(pcm, input.frames);
    backend.input = input.block();
    FloatPacket graph(logical, 16);
    std::fill(graph.samples.begin(), graph.samples.end(), 1.0f);
    render(capture, graph.block());
    check(graph.samples.back() == 0, "Unstarted capture renders silence");
    start_gated(capture, backend);
    for (std::size_t packet = 0; packet < 4; ++packet)
        run_batch(backend);
    render(capture, graph.block());
    check(std::abs(graph.samples.back() - 0.25f) < 1e-5f, "Four downsampled batches reach prefill");
    backend.stop();
    std::fill(graph.samples.begin(), graph.samples.end(), 1.0f);
    render(capture, graph.block());
    check(graph.samples.back() == 0, "Offline capture renders silence even with previously queued audio");
    backend.start();
    run_batch(backend);
    render(capture, graph.block());
    check(graph.samples.back() == 0, "Restart discards old capture frames and waits for fresh prefill");
}

void playback_silence()
{
    const PortSpec logical(SampleRate::Hz8000, 1, SampleFormat::F32P);
    const PortSpec pcm(SampleRate::Hz96000, 1, SampleFormat::F32LE);
    Playback playback(logical.sample_rate, 1, short_packet);
    playback.prepare(graph_period);
    FloatDevicePacket output(pcm.sample_rate, 3);
    TestBackend backend(pcm, 1);
    backend.output = output.block(1, 1);
    output.fill(3);
    start_gated(playback, backend);
    run_batch(backend);
    check(output.sample(1) == 0 && output.sample(0) == 3 && output.sample(2) == 3,
          "Prefill emits silence only into the requested device range");

    FloatPacket input(logical, 16);
    std::fill(input.samples.begin(), input.samples.end(), 0.5f);
    FloatPacket reference(PortSpec(pcm.sample_rate, 1, SampleFormat::F32P), 2);
    SincResampler fir;
    fir.prepare(1, logical.sample_rate, pcm.sample_rate, max_clock_drift_ppm / 1000000.0);
    const auto first = fir.process(input.view().subblock(0, 1), reference.view());
    check(first.produced == reference.frames && reference.samples[1] != 0,
          "One source frame provides two distinguishable initial upsampled frames");
    const auto prefill = [&] {
        for (int i = 0; i < 4; ++i)
            render(playback, input.block());
        for (const auto expected : reference.samples)
        {
            run_batch(backend);
            check(output.sample(1) == expected, "Live playback starts at the initial FIR phase and uses its history");
        }
    };
    prefill();

    backend.stop();
    backend.start();
    run_batch(backend);
    check(output.sample(1) == 0, "Restart waits for fresh prefill");
    prefill();

    backend.io_result = ErrorCode::DeviceLost;
    check(!backend.run_batch(), "Device loss ends the worker");
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::DeviceLost, "A failed batch stops playback and reports device loss");
    backend.io_result = {};
    backend.start();
    run_batch(backend);
    check(backend.last_error() == ErrorCode::Ok && output.sample(1) == 0,
          "Restart after an error clears the error and waits for fresh prefill");
    prefill();
}

void restart_graph()
{
    const PortSpec logical(rate, 1, SampleFormat::F32P), pcm(rate, 1, SampleFormat::F32LE);
    Engine engine(graph_period);
    ClockedGraph clock(engine);
    const auto capture_id = engine.emplace_node<Capture>(rate, 1, short_packet);
    const auto playback_id = engine.emplace_node<Playback>(rate, 1, short_packet);
    const auto sink_id = engine.emplace_node<PacketSink>(logical);
    const auto bypass = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 7; });
    engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0));
    engine.connect(OPORT(bypass, 0), IPORT(sink_id, 0));
    engine.commit();
    auto *capture = engine.node<Capture>(capture_id);
    auto *playback = engine.node<Playback>(playback_id);
    auto *monitor = engine.node<PacketSink>(sink_id);
    FloatDevicePacket input(rate, 96), output(rate, 96);
    input.fill(0.25f);
    TestBackend source(pcm, 96);
    TestBackend sink(pcm, 96);
    source.input = input.block();
    sink.output = output.block();
    start_gated(*capture, source);
    start_gated(*playback, sink);
    clock.start();
    for (int tick = 0; tick < 12; ++tick)
    {
        run_batch(source);
        clock.advance();
        run_batch(sink);
    }
    check(std::abs(output.sample(95) - 0.25f) < 1e-5f, "Device frames reach playback through the graph connection");
    source.unbind();
    sink.stop();
    clock.advance();
    check(monitor->sample(0) == 7 && !source.running() && !sink.running(),
          "Stopping endpoints leaves the bypass running");

    engine.replace_node(capture_id, std::make_unique<Capture>(rate, 1, milliseconds(20)));
    clock.commit();
    capture = engine.node<Capture>(capture_id);
    TestBackend bigger_source(pcm, 960);
    bigger_source.bind(*capture);
    check(engine.node<Playback>(playback_id) == playback,
          "A replacement endpoint accepts a larger device batch in the original graph");
    expect_error(ErrorCode::DuplicateConn, [&] { engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0)); });
    clock.stop();
    engine.reset();
}

void virtual_device_audio()
{
    Engine engine(graph_period);
    ClockedGraph graph(engine);
    const auto capture_id = engine.emplace_node<Capture>(rate, 1, milliseconds(20));
    const auto playback_id = engine.emplace_node<Playback>(rate, 1, milliseconds(20));
    engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0));
    engine.commit();
    auto &capture = *engine.node<Capture>(capture_id);
    auto &playback = *engine.node<Playback>(playback_id);
    constexpr std::size_t frames = 512;
    graph.start();
    for (const auto &spec : {PortSpec(SampleRate::Hz44100, 1, SampleFormat::S16LE),
                            PortSpec(rate, 1, SampleFormat::F32LE), PortSpec(rate, 1, SampleFormat::S24_3LE)})
    {
        const auto width = sample_bytes(spec.format);
        const auto span = frames * width;
        std::vector<std::uint8_t> recorded(span), played(span);
        for (std::size_t frame = 0; frame < frames; ++frame)
            if (spec.format == SampleFormat::F32LE)
                write_f32le(recorded.data() + frame * width, -0.5f);
            else
                recorded[(frame + 1) * width - 1] = spec.format == SampleFormat::S16LE ? 0x20 : 0xe0;

        TestBackend input(spec, frames);
        TestBackend output(spec, frames);
        input.input = {recorded.data(), spec, frames};
        output.output = {played.data(), spec, frames};
        start_gated(capture, input);
        start_gated(playback, output);
        std::size_t packets = 0;
        for (std::size_t tick = 1; tick <= 200; ++tick)
        {
            if ((packets + 1) * frames * 1000 <= tick * 2 * static_cast<std::uint32_t>(spec.sample_rate))
            {
                run_batch(input);
                run_batch(output);
                if (++packets > 16)
                {
                    check(output.written == frames, "The virtual device and real graph deliver full packets");
                    if (spec.format == SampleFormat::F32LE)
                        check(std::abs(read_f32le(played.data() + span - 4) + 0.5f) < 1e-5f,
                              "Reopening the device retains the graph and switches its samples");
                    else
                        check(played.back() == recorded.back(), "Packed audio matches its interleaved device format");
                }
            }
            graph.advance();
        }

    }
    graph.stop();
}



void thread_errors()
{
    const PortSpec spec(rate, 1, SampleFormat::F32LE), logical(rate, 1, SampleFormat::F32P);
    for (const auto error : {ErrorCode::EndOfStream, ErrorCode::DeviceLost, ErrorCode::IoFailed, ErrorCode::Cancelled})
    {
        Capture capture(rate, 1, short_packet);
        capture.prepare(graph_period);
        TestBackend backend(spec, 128);
        backend.io_result = error;
        backend.bind(capture);
        backend.start();
        wait_for_exit(backend);
        check(backend.last_error() == (error == ErrorCode::Cancelled ? ErrorCode::Ok : error),
              "Worker preserves terminal errors and excludes cancellation");
        FloatPacket output(logical, 96);
        std::fill(output.samples.begin(), output.samples.end(), 1);
        render(capture, output.block());
        check(output.samples.back() == 0, "Faulted capture renders silence");
    }

    Engine engine(graph_period);
    ClockedGraph graph(engine);
    const auto capture_id = engine.emplace_node<Capture>(rate, 1, short_packet);
    const auto sink_id = engine.emplace_node<PacketSink>(logical);
    const auto bypass_sink_id = engine.emplace_node<PacketSink>(logical);
    const auto bypass = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 7; });
    engine.connect(OPORT(capture_id, 0), IPORT(sink_id, 0));
    engine.connect(OPORT(bypass, 0), IPORT(bypass_sink_id, 0));
    engine.commit();
    auto &capture = *engine.node<Capture>(capture_id);
    TestBackend backend(spec, 96);
    backend.io_result = ErrorCode::DeviceLost;
    backend.bind(capture);
    graph.start();
    backend.start();
    wait_for_exit(backend);
    graph.advance();
    check(engine.node<PacketSink>(sink_id)->sample(0) == 0 &&
              engine.node<PacketSink>(bypass_sink_id)->sample(0) == 7,
          "A backend failure silences its branch while the running graph retains the bypass");
    graph.stop();
}

void playback_stop()
{
    const PortSpec spec(rate, 1, SampleFormat::F32LE);
    for (const bool during_write : {false, true})
    {
        Playback playback(rate, 1, short_packet);
        playback.prepare(graph_period);
        TestBackend backend(spec, 128);
        backend.block = true;
        backend.pace_write = during_write;
        backend.bind(playback);
        backend.start();
        check(backend.wait_for_io(), "Playback reaches its blocking wait or write");
        backend.stop();
        check(!backend.running() && backend.last_error() == ErrorCode::Ok,
              "Playback stop wakes the backend and joins without a spurious error");
    }
}

void sync_playback_restart()
{
    Playback playback(rate, 1, milliseconds(2));
    playback.prepare(graph_period);
    FloatPacket input(playback.spec(), 96);
    FloatDevicePacket output(rate, 96);
    BackendCalls calls;
    TestBackend backend(output.spec, output.frames, &calls, false);
    backend.output = output.block();
    backend.write_result = ErrorCode::DeviceLost;
    backend.bind(playback);
    backend.start();
    render(playback, input.block());
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::DeviceLost, "Sync playback reports a write failure before restart");

    backend.write_result = {};
    backend.start();
    std::fill(input.samples.begin(), input.samples.end(), 0.25f);
    render(playback, input.block());
    backend.stop();
    check(calls.starts == 2 && calls.stops == 2 && calls.cancels == 0 &&
              backend.last_error() == ErrorCode::Ok && output.sample(95) == 0.25f,
          "Sync playback restarts after self-exit and drains fresh audio");
}

template <class Endpoint>
void check_io_timeout(PortSpec spec)
{
    Endpoint node(rate, 1, milliseconds(5));
    node.prepare(graph_period);
    TestBackend backend(spec, 240);
    backend.cadence = milliseconds(5);
    backend.slow_batch = 2;
    backend.slow_delay = milliseconds(100);
    backend.bind(node);
    backend.start();
    check(backend.wait_for_io(3) && backend.last_error() == ErrorCode::Ok,
          "A delay inside the inactivity window allows the next batch");
    backend.stop();

    backend.slow_delay = milliseconds(600);
    backend.start();
    check(backend.wait_for_io(2) && backend.last_error() == ErrorCode::Ok, "The stalled batch begins online");
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::Timeout, "A stalled batch exceeds the inactivity deadline");
}

void io_timeout()
{
    const PortSpec spec(rate, 1, SampleFormat::F32LE);
    check_io_timeout<Capture>(spec);
    check_io_timeout<Playback>(spec);
}

void empty_capture_timeout()
{
    const PortSpec spec(rate, 1, SampleFormat::F32LE);
    BackendCalls calls;
    Capture capture(rate, 1, milliseconds(5));
    capture.prepare(graph_period);
    TestBackend backend(spec, 240, &calls);
    backend.cadence = milliseconds(5);
    backend.first_data_batch = 2;
    backend.last_data_batch = 120;
    backend.bind(capture);
    backend.start();
    check(backend.wait_for_io(121),
          "A brief startup gap fits the timeout and nonempty reads warm the monitor");
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::Timeout, "Fast empty reads eventually reach the no-data timeout");
    backend.stop();
    check(calls.stops == 1 && !calls.cancels, "Worker timeout stops the device without requesting cancellation");
}

void startup_empty_capture_timeout()
{
    const PortSpec spec(rate, 1, SampleFormat::F32LE);
    Capture capture(rate, 1, milliseconds(5));
    capture.prepare(graph_period);
    TestBackend backend(spec, 240);
    backend.cadence = milliseconds(1);
    backend.read_frames = 0;
    backend.bind(capture);
    backend.start();
    wait_for_exit(backend);
    check(backend.last_error() == ErrorCode::Timeout,
          "Continuous empty reads time out even before the first nonempty batch");
}

void sync_playback_drain()
{
    constexpr auto slow = SampleRate::Hz8000;
    constexpr auto fast = SampleRate::Hz96000;
    const SampleRate rates[][2] = {{slow, slow}, {slow, fast}, {fast, slow}};
    for (const auto &pair : rates)
    {
        const auto source_rate = pair[0];
        const auto rate = pair[1];
        const PortSpec logical(source_rate, 1, SampleFormat::F32P);
        const PortSpec device(rate, 1, SampleFormat::F32LE);
        FloatPacket input(logical, logical.max_frames(graph_period));
        input.samples.back() = 1.0f;
        Playback playback(source_rate, 1, milliseconds(5));
        playback.prepare(graph_period);
        SincResampler reference;
        reference.prepare(1, source_rate, rate);
        FloatPacket padded(logical, input.frames + reference.tail_frames());
        std::copy(input.samples.begin(), input.samples.end(), padded.samples.begin());
        Buffer expected;
        const auto limit = SincResampler::max_output_frames(source_rate, rate, padded.frames);
        expected.resize(1, limit);
        const auto converted = reference.process(padded.view(), expected.block(limit));

        for (const auto batch : {std::size_t{1}, std::size_t{32}})
        {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(limit * sizeof(float));
            BackendCalls calls;
            TestBackend sink(device, batch, &calls, false);
            sink.recorded = &bytes;
            sink.bind(playback);
            sink.start();
            sink.stop();
            check(bytes.empty(), "An empty stream writes zero frames, including when a FIR is configured");
            sink.start();
            render(playback, input.block());
            sink.stop();
            check(sink.last_error() == ErrorCode::Ok && calls.stops == 2 &&
                      bytes.size() == converted.produced * sizeof(float),
                  "Sync playback drains the real frames and FIFO tail at either batch size");
            for (std::size_t frame = 0; frame < converted.produced; ++frame)
                check(read_f32le(bytes.data() + frame * sizeof(float)) == expected.block(limit).channel(0)[frame],
                      "Drain preserves the last-frame impulse and its complete response");
        }
    }
}

void sync_capture_eos()
{
    constexpr auto device_rate = SampleRate::Hz8000;
    constexpr auto graph_rate = SampleRate::Hz48000;
    const PortSpec logical(graph_rate, 1, SampleFormat::F32P);
    for (const auto graph_running : {false, true})
    {
        Capture capture(graph_rate, 1, milliseconds(2));
        capture.prepare(graph_period);
        FloatDevicePacket input(device_rate, 16);
        write_f32le(input.bytes.data() + 15 * sizeof(float), 1.0f);
        TestBackend source(input.spec, input.frames, nullptr, false);
        source.input = input.block();
        source.eof_batch = 2;
        source.bind(capture);
        source.start();
        check(source.wait_for_io(2), "Short source reaches EOS");
        FloatPacket output(logical, 96);
        if (graph_running)
        {
            float peak = 0;
            const auto deadline = steady_clock::now() + seconds(2);
            while (source.running() && steady_clock::now() < deadline)
            {
                render(capture, output.block());
                for (const auto sample : output.samples)
                    peak = std::max(peak, std::abs(sample));
                std::this_thread::sleep_for(milliseconds(2));
            }
            check(!source.running() && source.last_error() == ErrorCode::EndOfStream && peak > 0.9f,
                  "EOS feeds the last input's FIR response to the graph before going offline");
        }
        else
        {
            check(source.running(), "EOS keeps queued samples online before the graph advances");
            source.stop();
            check(source.last_error() == ErrorCode::Ok, "Cancellation ends the EOS wait without recording an error");
        }
        std::fill(output.samples.begin(), output.samples.end(), 1.0f);
        render(capture, output.block());
        check(!source.running() && std::all_of(output.samples.begin(), output.samples.end(),
                                               [](float sample) { return sample == 0; }),
              "Capture renders silence after EOS or cancellation");
    }
}

void null_backends()
{
    expect_error(ErrorCode::InvalidArg, [] { NullBackend invalid(SampleFormat::F32P); });
    expect_error(ErrorCode::InvalidArg, [] { NullBackend invalid(SampleFormat{}); });

    Capture capture(rate, 1, milliseconds(2));
    Playback playback(rate, 1, milliseconds(2));
    capture.prepare(graph_period);
    playback.prepare(graph_period);
    NullBackend source(SampleFormat::S16LE);
    NullBackend sink(SampleFormat::S16LE);
    source.bind(capture);
    sink.bind(playback);
    check(source.device_max_frames() == 96, "Null negotiates one graph block for the given I/O budget");
    source.start();
    sink.start();

    FloatPacket block(capture.spec(), 96);
    std::fill(block.samples.begin(), block.samples.end(), 1.0f);
    render(capture, block.block());
    check(std::all_of(block.samples.begin(), block.samples.end(), [](float sample) { return sample == 0; }),
          "A null source supplies silence");
    render(playback, block.block());
    sink.stop();
    source.stop();
    check(sink.last_error() == ErrorCode::Ok && source.last_error() == ErrorCode::Ok,
          "Null playback and capture stop cleanly");
    source.unbind();
    sink.unbind();
    source.bind(playback);
    sink.bind(capture);

}

struct WavFiles
{
    const std::string stem = "aereborn-wave-" + std::to_string(steady_clock::now().time_since_epoch().count());
    std::vector<std::string> paths;

    std::string path(const char *suffix)
    {
        paths.push_back(stem + suffix);
        return paths.back();
    }
    ~WavFiles()
    {
        for (const auto &path : paths)
            std::remove(path.c_str());
    }
};

void wav_round_trip()
{
    expect_error(ErrorCode::FormatMismatch, [] { WavPlayback invalid("", SampleFormat::F32P); });
    expect_error(ErrorCode::FormatMismatch, [] { WavPlayback invalid("", SampleFormat::S24LE, seconds(1)); });

    for (const auto format : {SampleFormat::S16LE, SampleFormat::S24_3LE, SampleFormat::S32LE, SampleFormat::F32LE})
    {
        WavFiles files;
        const auto path = files.path(".wav");
        Playback playback(rate, 1, milliseconds(2));
        playback.prepare(graph_period);
        WavPlayback writer(path, format);
        writer.bind(playback);
        writer.start();
        FloatPacket block(playback.spec(), 3);
        block.samples = {-0.5f, 0.25f, 0.5f};
        render(playback, block.block());
        writer.stop();
#if !WINDOWS_OS_ENVIRONMENT
        {
            std::ifstream bytes(path, std::ios::binary | std::ios::ate);
            const auto header_bytes = format == SampleFormat::F32LE ? 58 : 44;
            check(bytes.tellg() == static_cast<std::streamoff>(header_bytes + 3 * sample_bytes(format)),
                  "Stopping flushes the PCM payload while the file remains bound");
        }
#endif
        writer.unbind();
        check(writer.last_error() == ErrorCode::Ok, "WAV writes a short final batch and finalizes the file");

        for (const auto loop : {false, true})
        {
            Capture capture(rate, 1, milliseconds(2));
            capture.prepare(graph_period);
            WavCapture reader(path, loop);
            reader.bind(capture);
            reader.start();
            check(reader.device_spec().format == format, "WAV negotiates its recorded format");
            FloatPacket result(capture.spec(), 3);
            const auto deadline = steady_clock::now() + seconds(2);
            for (int copies = loop ? 2 : 1; copies > 0; )
            {
                render(capture, result.block());
                if (result.samples == block.samples)
                    --copies;
                else
                    check(steady_clock::now() < deadline, "WAV supplies the recorded short block");
                if (copies)
                    std::this_thread::sleep_for(milliseconds(1));
            }
            if (loop)
                reader.stop();
            else
            {
                wait_for_exit(reader);
                check(reader.last_error() == ErrorCode::EndOfStream, "WAV reports EOF after draining");
            }
        }
    }
}

void wav_reader()
{
    WavFiles files;
    const auto u8_path = files.path("-u8.wav");
    const auto f64_path = files.path("-f64.wav");
    const auto truncated_path = files.path("-truncated.wav");
    const unsigned char u8[] = {
        'R', 'I', 'F', 'F', 50, 0, 0, 0, 'W', 'A', 'V', 'E',
        'J', 'U', 'N', 'K', 1, 0, 0, 0, 42, 0,
        'd', 'a', 't', 'a', 3, 0, 0, 0, 128, 255, 0, 0,
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0, 1, 0, 8, 0};
    const unsigned char f64[] = {
        'R', 'I', 'F', 'F', 52, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 1, 0, 0x40, 0x1f, 0, 0, 0, 0xfa, 0, 0, 8, 0, 64, 0,
        'd', 'a', 't', 'a', 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 0x3f, 0, 0, 0, 0, 0, 0, 0xd0, 0xbf};
    const auto write_fixture = [](const std::string &path, View<const unsigned char> bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        file.close();
        check(static_cast<bool>(file), "WAV input fixture was written");
    };
    write_fixture(u8_path, {u8, sizeof(u8)});
    write_fixture(f64_path, {f64, sizeof(f64)});
    write_fixture(truncated_path, {u8, sizeof(u8) - 1});

    WavFile reader;
    reader.open_read(u8_path).or_throw();
    check(reader.frame_count() == 3 && reader.spec().format == SampleFormat::F32LE,
          "The reader skips an odd ancillary chunk and discovers data before fmt");
    reader.seek(1).or_throw();
    FloatDevicePacket packet(SampleRate::Hz8000, 3);
    check(reader.read(packet.block()).value_or_throw() == 2 && reader.tell() == 3 &&
              packet.sample(0) == 127.0f / 128 && packet.sample(1) == -1,
          "Seeking and short reads count frames and decode unsigned PCM");
    check(reader.read(packet.block()).value_or_throw() == 0, "An exhausted reader returns zero frames");
    expect_error(ErrorCode::InvalidArg, [&] { reader.seek(4).or_throw(); });

    reader.open_read(f64_path).or_throw();
    check(reader.read(packet.block()).value_or_throw() == 2 && packet.sample(0) == 0.5f && packet.sample(1) == -0.25f,
          "The standalone reader converts little-endian Float64 to Float32");
    expect_error(ErrorCode::IoFailed, [&] { reader.open_read(truncated_path).or_throw(); });
    reader.seek(0).or_throw();
    check(reader.read(packet.block()).value_or_throw() == 2 && packet.sample(0) == 0.5f,
          "An invalid replacement file leaves the previous reader usable");

    Capture capture(SampleRate::Hz8000, 1, milliseconds(2));
    capture.prepare(graph_period);
    WavCapture source(u8_path, WavCapture::Repeat{2});
    source.bind(capture);
    source.start();
    FloatPacket result(capture.spec(), 9);
    const std::vector<float> expected{0, 127.0f / 128, -1, 0, 127.0f / 128, -1, 0, 127.0f / 128, -1};
    const auto deadline = steady_clock::now() + seconds(2);
    do
    {
        render(capture, result.block());
        if (result.samples == expected)
            break;
        check(steady_clock::now() < deadline, "Finite repeats join consecutive passes in one device batch");
        std::this_thread::sleep_for(milliseconds(1));
    } while (true);
    wait_for_exit(source);
    check(source.last_error() == ErrorCode::EndOfStream, "Capture stops after the configured number of passes");
}

void wav_file_write()
{
    WavFiles files;
    const auto path = files.path(".wav");
    const PortSpec spec(rate, 1, SampleFormat::S24_3LE);
    unsigned char samples[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const unsigned char replacement[] = {10, 11, 12};

    WavFile file;
    file.open_write(path, spec, WavFile::WriteMode::Truncate).or_throw();
    file.write({samples, spec, 3}).or_throw();
    file.seek(1).or_throw();
    file.write({replacement, spec, 1}).or_throw();
    check(file.tell() == 2 && file.frame_count() == 3, "Overwriting a frame preserves the file's remaining frames");
    file.close().or_throw();

    unsigned char actual[3 * 3 + 1]{};
    const auto size = sizeof(actual);
    {
        std::ifstream bytes(path, std::ios::binary);
        bytes.seekg(44);
        bytes.read(reinterpret_cast<char *>(actual), size);
        check(bytes && bytes.peek() == EOF && actual[size - 1] == 0, "Odd PCM payloads end with a RIFF pad byte");
    }
    file.open_read(path).or_throw();
    check(file.spec() == spec && file.frame_count() == 3 &&
              file.read({actual, spec, 3}).value_or_throw() == 3,
          "The same WavFile can reopen its output with the finalized frame count");
    const unsigned char expected[] = {1, 2, 3, 10, 11, 12, 7, 8, 9};
    check(std::equal(expected, expected + sizeof(expected), actual), "Writing preserves the supplied PCM bytes");
}

#if LINUX_OS_ENVIRONMENT
void wav_write_errors()
{
    const auto with_full_file = [](const auto &operation) {
        struct rlimit original;
        check(getrlimit(RLIMIT_FSIZE, &original) == 0 && original.rlim_cur >= 44, "Read the file size limit");
        // Allow an empty PCM header, then inject a write error on its first payload.
        const struct rlimit full{44, original.rlim_max};
        const auto handler = std::signal(SIGXFSZ, SIG_IGN);
        check(handler != SIG_ERR, "Suppress the file size limit signal");
        const auto limited = setrlimit(RLIMIT_FSIZE, &full);
        if (limited == 0)
            operation();
        const auto restored = setrlimit(RLIMIT_FSIZE, &original);
        const auto restored_handler = std::signal(SIGXFSZ, handler);
        check(limited == 0 && restored == 0 && restored_handler != SIG_ERR, "Set and restore the file size limit");
    };

    WavFiles files;
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    const unsigned char samples[] = {1, 2};
    WavFile file;
    file.open_write(files.path("-file.wav"), spec, WavFile::WriteMode::Truncate).or_throw();
    file.write({samples, spec, 1}).or_throw();
    RetCode flushed;
    with_full_file([&]() noexcept { flushed = file.flush(); });
    check(flushed == ErrorCode::IoFailed, "Flushing reports a buffered write failure");
    file.seek(0).or_throw();
    check(file.close() == ErrorCode::IoFailed, "Seeking and finalizing preserve an earlier write failure");

    Playback playback(rate, 1, milliseconds(2));
    playback.prepare(graph_period);
    WavPlayback writer(files.path("-backend.wav"), spec.format);
    writer.bind(playback);
    writer.start();
    FloatPacket input(playback.spec(), 1);
    render(playback, input.block());
    with_full_file([&]() noexcept { writer.stop(); });
    check(writer.last_error() == ErrorCode::IoFailed, "A sync playback stop reports a flush failure");
}
#endif

void wav_segments()
{
    WavFiles files;
    const auto base = files.path(".wav");
    const auto existing = files.path("-0000000001.wav");
    const auto first = files.path("-0000000002.wav");
    const auto second = files.path("-0000000003.wav");
    const auto unused = files.path("-0000000004.wav");
    {
        std::ofstream file(existing, std::ios::binary);
        file.put('x');
    }

    constexpr auto device_rate = SampleRate::Hz8000;
    Playback playback(device_rate, 1, milliseconds(1500));
    playback.prepare(graph_period);
    WavPlayback writer(base, SampleFormat::F32LE, seconds(1));
    writer.bind(playback);
    writer.start();
    FloatPacket input(playback.spec(), 16);
    std::fill(input.samples.begin(), input.samples.end(), 0.25f);
    for (int tick = 0; tick < 500; ++tick)
        render(playback, input.block());
    std::fill(input.samples.begin(), input.samples.end(), -0.5f);
    render(playback, input.block());
    writer.unbind();
    check(writer.last_error() == ErrorCode::Ok, "Playback finalizes both halves of a write across the segment boundary");
    check(std::ifstream(existing, std::ios::binary).get() == 'x' && !std::ifstream(unused),
          "Segment rotation preserves existing files and opens a new file only when there is audio");

    WavFile reader;
    FloatDevicePacket output(device_rate, 16);
    reader.open_read(first).or_throw();
    check(reader.frame_count() == 8000, "The first segment ends at exactly one second of samples");
    reader.seek(7984).or_throw();
    check(reader.read(output.block()).value_or_throw() == 16 && output.sample(15) == 0.25f,
          "The segment boundary preserves the tail of the first segment");
    reader.open_read(second).or_throw();
    check(reader.frame_count() == 16 && reader.read(output.block()).value_or_throw() == 16 &&
              output.sample(0) == -0.5f && output.sample(15) == -0.5f,
          "The second segment contains all remaining frames");
}

#if WINDOWS_OS_ENVIRONMENT
void wasapi_open_rollback()
{
    Capture capture(rate, 1, milliseconds(10));
    Playback playback(rate, 1, milliseconds(10));
    capture.prepare(graph_period);
    playback.prepare(graph_period);
    WasapiBackend source(L"aereborn-invalid-test-endpoint");
    WasapiBackend sink(L"aereborn-invalid-test-endpoint");
    expect_exception<Error>([&] { source.bind(capture); });
    expect_exception<Error>([&] { sink.bind(playback); });
    for (const auto *backend : {&source, &sink})
    {
        const auto failure = backend->last_failure();
        check(failure.operation && failure.hresult < 0, "WASAPI retains the native error after bind rollback");
    }
    NullBackend fallback_source(SampleFormat::S16LE);
    NullBackend fallback_sink(SampleFormat::S16LE);
    fallback_source.bind(capture);
    fallback_sink.bind(playback);
}
#endif

#ifdef AEREBORN_HAS_ALSA
void check_alsa_virtual(bool use_mmap)
{
    Capture capture(rate, 1, milliseconds(10));
    Playback playback(rate, 1, milliseconds(10));
    capture.prepare(graph_period);
    playback.prepare(graph_period);
    AlsaBackend source("plug:null", 0, use_mmap);
    AlsaBackend sink("plug:null", 0, use_mmap);
    source.bind(capture);
    sink.bind(playback);
    source.start();
    sink.start();
    FloatPacket block(capture.spec(), 96);
    for (int tick = 0; tick < 16; ++tick)
    {
        render(capture, block.block());
        render(playback, block.block());
        std::this_thread::sleep_for(milliseconds(2));
    }
    check(source.running() && sink.running(), "ALSA null capture and playback stay active across graph ticks");
    source.stop();
    sink.stop();
    check(source.last_error() == ErrorCode::Ok && sink.last_error() == ErrorCode::Ok,
          "ALSA stops without recording a cancellation as a fault");
}

void alsa_virtual()
{
    check_alsa_virtual(true);
    check_alsa_virtual(false);
}

#endif

} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"queue_ranges", queue_ranges},
                                  {"planar_playback", planar_playback},
                                  {"stop_in_flight", stop_in_flight},
                                  {"restart_graph", restart_graph},
                                  {"streaming_ranges", streaming_ranges},
                                  {"stream_step_changes", stream_step_changes},
                                  {"offline_capture", offline_capture},
                                  {"playback_silence", playback_silence},
                                  {"virtual_device_audio", virtual_device_audio},
                                  {"lifecycle", lifecycle},
                                  {"thread_errors", thread_errors},
                                  {"playback_stop", playback_stop},
                                  {"sync_playback_restart", sync_playback_restart},
                                  {"io_timeout", io_timeout},
                                  {"empty_capture_timeout", empty_capture_timeout},
                                  {"startup_empty_capture_timeout", startup_empty_capture_timeout},
                                  {"sync_playback_drain", sync_playback_drain},
                                  {"sync_capture_eos", sync_capture_eos},
                                  {"null_backends", null_backends},
#if WINDOWS_OS_ENVIRONMENT
                                  {"wasapi_open_rollback", wasapi_open_rollback},
#endif
#if LINUX_OS_ENVIRONMENT
                                  {"wav_write_errors", wav_write_errors},
#endif
#ifdef AEREBORN_HAS_ALSA
                                  {"alsa_virtual", alsa_virtual},
#endif
                                  {"wav_round_trip", wav_round_trip},
                                  {"wav_reader", wav_reader},
                                  {"wav_file_write", wav_file_write},
                                  {"wav_segments", wav_segments}});
}
