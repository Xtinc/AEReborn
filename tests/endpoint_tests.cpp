#include "plugins/device_stream.h"
#include "support/audio_fixture.h"
#include <array>
#include <limits>
#include <thread>

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

void queue_ranges()
{
    const PortSpec spec(rate, 2, SampleFormat::F32P);
    Capture capture(spec.sample_rate, spec.channels, short_packet);
    FloatPacket source(spec, 1024), graph(spec, 96);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t frame = 0; frame < source.frames; ++frame)
            source.block().f32p_view().channel(ch)[frame] = static_cast<float>(10000 * ch + frame + 1);

    expect_error(ErrorCode::NotPrepared, capture.set_online(true));
    expect_error(ErrorCode::NotPrepared, capture.write(source.view().subblock(0, 1)));
    expect_error(ErrorCode::InvalidArg, capture.prepare(static_cast<BlockPeriod>(0)));
    expect_error(ErrorCode::InvalidArg, capture.prepare(static_cast<BlockPeriod>(3)));
    check(capture.status().capacity_frames == 0 && !capture.online(), "An invalid period leaves capture unprepared");
    checked(capture.prepare(graph_period));
    const auto status = capture.status();
    check(status.capacity_frames == 768 && status.target_frames == 384,
          "Small capture packets retain the eight-block baseline and its midpoint target");
    checked(capture.set_online(true));
    expect_error(ErrorCode::FormatMismatch, capture.write(IFBlock{nullptr, 1, 0, 0}));
    expect_error(ErrorCode::InvalidArg, capture.write(IFBlock{nullptr, 2, 1, 1}));
    check(checked(capture.write(source.view().subblock(10, 128))) == 128, "A planar range keeps its original channel stride");
    render(capture, graph.block());
    check(!capture.steady() && capture.status().queued_frames == 128 && graph.samples[0] == 0,
          "Capture holds partial prefill and supplies silence");
    check(checked(capture.write(source.view().subblock(138, 272))) == 272, "Prefill reaches midpoint");
    render(capture, graph.block());
    check(capture.steady() && graph.samples[0] == 11 && graph.samples[96] == 10011, "Consumer publishes steady");

    check(checked(capture.write(source.view().subblock(420, 600))) == 464, "Only the available complete frames fit");
    check(capture.status().overflow_frames == 136, "Steady overflow is counted in frames");
    for (std::size_t i = 0; i < 8; ++i)
    {
        render(capture, graph.block());
        const auto view = graph.block().f32p_view();
        for (std::size_t frame = 0; frame < 96; ++frame)
        {
            const auto index = i * 96 + frame;
            const auto expected = index < 304 ? 107 + index : 421 + index - 304;
            check(view.channel(0)[frame] == expected && view.channel(1)[frame] == expected + 10000,
                  "Ring and cursor wrapping preserve channel order and sample bits");
        }
    }
    render(capture, graph.block());
    check(capture.status().underrun_frames == 96 && graph.samples.back() == 0, "Short capture pads the entire block");
    checked(capture.set_online(false));
    render(capture, graph.block());
    check(!capture.steady() && capture.status().underrun_frames == 96, "Offline silence is excluded from deficits");
    capture.reset();
    check(capture.status().overflow_frames == 0 && capture.status().underrun_frames == 0,
          "Engine reset clears accumulated diagnostics");
}

void planar_playback()
{
    const PortSpec spec(SampleRate::Hz44100, 2, SampleFormat::F32P);
    Playback playback(spec.sample_rate, spec.channels, microseconds(2903));
    checked(playback.prepare(graph_period));
    expect_error(ErrorCode::InvalidArg, playback.prepare(static_cast<BlockPeriod>(0)));
    const auto status = playback.status();
    check(status.capacity_frames == 712 && status.target_frames == 356,
          "An invalid period leaves the prepared fractional queue and target intact");
    checked(playback.set_online(true));
    FloatPacket output(spec, 410);
    std::fill(output.samples.begin(), output.samples.end(), -1.0f);
    const auto push = [&](std::size_t tick) {
        FloatPacket block(spec, spec.frames(graph_period, tick));
        const auto first = spec.frame_position(graph_period, tick);
        for (std::size_t ch = 0; ch < spec.channels; ++ch)
            for (std::size_t frame = 0; frame < block.frames; ++frame)
                block.view().channel(ch)[frame] = static_cast<float>(1000 * ch + first + frame + 1);
        render(playback, block.block());
    };
    for (std::size_t tick = 0; tick < 5; ++tick)
        push(tick);
    check(checked(playback.read(output.view().subblock(3, 400))) == 400, "A planar read can start at a nonzero offset");
    for (std::size_t ch = 0; ch < spec.channels; ++ch)
    {
        const auto *samples = output.view().channel(ch);
        check(samples[2] == -1 && samples[403] == -1, "Reading preserves samples outside the requested range");
        for (std::size_t frame = 0; frame < 400; ++frame)
            check(samples[3 + frame] == 1000 * ch + frame + 1, "Playback preserves each channel");
    }

    for (std::size_t tick = 5; tick < 9; ++tick)
        push(tick);
    const auto got = checked(playback.read(output.view().subblock(1, 405)));
    check(got == spec.frame_position(graph_period, 9) - 400, "Playback reports actual frames after wrap");
    for (std::size_t ch = 0; ch < spec.channels; ++ch)
    {
        const auto *samples = output.view().channel(ch);
        for (std::size_t frame = 0; frame < got; ++frame)
            check(samples[1 + frame] == 1000 * ch + 401 + frame, "Ring wrapping preserves sample order");
        check(std::all_of(samples + 1 + got, samples + 406, [](float sample) { return sample == 0; }),
              "A short read zeroes its missing tail in each channel");
    }
}

void stop_in_flight()
{
    aereborn::EndpointQueue queue(PortSpec(rate, 1, SampleFormat::F32P), short_packet);
    checked(queue.prepare(graph_period));
    checked(queue.set_online(true));
    check(queue.enter_graph(), "Graph begins a queue access");
    std::atomic<bool> stopped{false};
    std::thread closer([&] {
        queue.set_online(false);
        stopped.store(true, std::memory_order_release);
    });
    const auto deadline = steady_clock::now() + seconds(2);
    while (queue.online() && steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool closed = !queue.online();
    const bool early = stopped.load(std::memory_order_acquire);
    std::array<float, 96> samples{};
    queue.write(IFBlock{samples.data(), 1, samples.size(), samples.size()});
    queue.leave_graph();
    closer.join();
    check(closed && !early && queue.status().queued_frames == 0, "Offline waits for late graph writes and then clears");
}

void restart_graph()
{
    const PortSpec spec(rate, 1, SampleFormat::F32P);
    Engine engine(graph_period);
    ClockedGraph clock(engine);
    const auto capture_id = checked(engine.emplace_node<Capture>(spec.sample_rate, spec.channels, short_packet));
    const auto playback_id = checked(engine.emplace_node<Playback>(spec.sample_rate, spec.channels, short_packet));
    const auto monitor_id = checked(engine.emplace_node<PacketSink>(spec));
    const auto bypass_id = checked(engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 7; }));
    checked(engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0)));
    checked(engine.connect(OPORT(bypass_id, 0), IPORT(monitor_id, 0)));
    checked(engine.commit());
    auto *capture = checked(engine.node<Capture>(capture_id));
    auto *playback = checked(engine.node<Playback>(playback_id));
    auto *monitor = checked(engine.node<PacketSink>(monitor_id));
    FloatPacket packet(spec, 2308), out(spec, 96);
    checked(capture->set_online(true));
    checked(playback->set_online(true));
    std::fill(packet.samples.begin(), packet.samples.end(), 1.0f);
    check(checked(capture->write(packet.view())) == 768, "Old device fills its queue");
    clock.start();
    for (int i = 0; i < 4; ++i)
        clock.advance();
    check(playback->status().queued_frames == 384, "Old frames reached the far end");

    checked(capture->set_online(false));
    checked(playback->set_online(false));
    clock.advance();
    check(monitor->sample(0) == 7 && capture->status().queued_frames == 0 && playback->status().queued_frames == 0,
          "The bypass continues while both endpoints clear their queues");
    check(checked(playback->read(out.view().subblock(0, 96))) == 0 && out.samples.back() == 0, "Offline playback pads silence");

    checked(engine.replace_node(capture_id, std::make_unique<Capture>(spec.sample_rate, spec.channels, milliseconds(20))));
    clock.commit();
    capture = checked(engine.node<Capture>(capture_id));
    check(capture->status().capacity_frames == 2308 &&
          checked(engine.node<Playback>(playback_id)) == playback,
          "The large-device budget takes effect with the same endpoint identifier");
    expect_error(ErrorCode::DuplicateConn, engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0)));

    checked(capture->set_online(true));
    checked(playback->set_online(true));
    std::fill(packet.samples.begin(), packet.samples.end(), 2.0f);
    check(checked(capture->write(packet.view().subblock(0, 1154))) == 1154, "New device refills the cleared path");
    for (int i = 0; i < 4; ++i)
        clock.advance();
    check(checked(playback->read(out.view().subblock(0, 96))) == 96 &&
          std::all_of(out.samples.begin(), out.samples.end(), [](float v) { return v == 2.0f; }),
          "Only the replacement device's frames reach the existing connection");
    checked(capture->set_online(false));
    checked(playback->set_online(false));
    clock.stop();
    checked(engine.reset());
}

void streaming_ranges()
{
    const SampleRate pairs[][2] = {{SampleRate::Hz96000, SampleRate::Hz8000},
                                  {SampleRate::Hz8000, SampleRate::Hz96000},
                                  {SampleRate::Hz44100, SampleRate::Hz48000}};
    for (const auto &pair : pairs)
    {
        const PortSpec src(pair[0], 2, SampleFormat::F32P), dst(pair[1], 2, SampleFormat::F32P);
        Convert whole(src, dst), split(src, dst);
        checked(whole.prepare(1400, 100));
        checked(split.prepare(1400, 100));
        FloatPacket input(src, 1410), reference(dst, 110), output(dst, 110);
        for (std::size_t i = 0; i < input.samples.size(); ++i)
            input.samples[i] = static_cast<float>(std::sin(i * 0.13));
        std::fill(output.samples.begin(), output.samples.end(), -2.0f);
        const auto need = checked(whole.required_input(100));
        const auto pass = checked(whole.process(input.view().subblock(3, need), reference.view().subblock(3, 100)));
        check(pass.consumed == need && pass.produced == 100, "Demand equals the actual source consumption");

        std::size_t read = 0, written = 0;
        const std::array<std::size_t, 6> spans{{1, 1, 11, 17, 31, 39}};
        for (const auto span : spans)
        {
            const auto needed = checked(split.required_input(span));
            if (pair[0] == SampleRate::Hz8000 && written == 1)
                check(needed == 0, "Valid upsampling can generate another frame from history");
            const auto part = checked(split.process(input.view().subblock(3 + read, needed),
                                                    output.view().subblock(3 + written, span)));
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

        expect_error(ErrorCode::InvalidArg, split.required_input(20, std::numeric_limits<double>::quiet_NaN()));
        expect_error(ErrorCode::InvalidArg, split.process(input.view().subblock(0, 10), output.view().subblock(0, 1), 1.1));
        expect_error(ErrorCode::TooLarge, split.process(input.view().subblock(0, 1401), output.view().subblock(0, 1)));
        expect_error(ErrorCode::TooLarge, split.required_input(101));
        expect_error(ErrorCode::InvalidArg, split.process(IFBlock{nullptr, 2, 1, 1}, output.view().subblock(0, 1)));
        expect_error(ErrorCode::FormatMismatch, split.process(IBBlock{}, output.view().subblock(0, 1)));
        check(checked(split.required_input(10)) == checked(whole.required_input(10)),
              "Rejected operations preserve the current phase");
    }
}

void stream_step_changes()
{
    const PortSpec spec(rate, 1, SampleFormat::F32P);
    Convert stream(spec, spec);
    checked(stream.prepare(128, 120, 0.002));
    FloatPacket input(spec, 1000), output(spec, 120);
    for (std::size_t i = 0; i < input.frames; ++i)
        input.samples[i] = static_cast<float>(std::sin(i * 0.07));

    std::size_t read = 0, count = 0;
    double source_position = 0;
    for (const auto scale : {1.001, 1.0, 0.999, 1.0, 1.002, 0.998})
    {
        const auto need = checked(stream.required_input(120, scale));
        const auto part = checked(stream.process(input.view().subblock(read, need), output.block(), scale));
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

void offline_capture()
{
    const PortSpec logical(rate, 1, SampleFormat::F32P);
    const PortSpec native(SampleRate::Hz44100, 1, SampleFormat::F32LE);
    Capture capture(logical.sample_rate, logical.channels, short_packet);
    CaptureStream device(capture);
    checked(capture.prepare(graph_period));
    expect_error(ErrorCode::InvalidArg, device.prepare(logical, 117));
    checked(device.prepare(native, 117));
    FloatDevicePacket late(native.sample_rate, 117), silent(native.sample_rate, 117);
    FloatPacket graph(logical, 96);
    late.fill(0.75f);
    std::fill(graph.samples.begin(), graph.samples.end(), -1.0f);

    expect_error(ErrorCode::InvalidArg, device.transfer(IBBlock{nullptr, native, 1}));
    check(!capture.online() && checked(device.transfer(late.block())) == 0 && capture.status().queued_frames == 0,
          "An offline capture packet is validated and discarded");
    checked(capture.set_online(true));
    check(capture.online() && checked(device.transfer(silent.block())) == 128,
          "The first live packet starts at the original fractional phase");
    for (int i = 0; i < 3; ++i)
        checked(device.transfer(silent.block()));
    render(capture, graph.block());
    check(capture.steady() &&
              std::all_of(graph.samples.begin(), graph.samples.end(), [](float sample) { return sample == 0; }),
          "Offline input never reaches the FIR history of the first live output");
    checked(capture.set_online(false));
}

void playback_silence()
{
    const PortSpec spec(rate, 1, SampleFormat::F32P);
    Playback playback(spec.sample_rate, spec.channels, short_packet);
    PlaybackStream device(playback);
    checked(playback.prepare(graph_period));
    FloatDevicePacket output(spec.sample_rate, 140);
    expect_error(ErrorCode::InvalidArg, device.prepare(spec, 128));
    expect_error(ErrorCode::TooLarge, device.prepare(output.spec, 129));
    checked(device.prepare(output.spec, 128));
    checked(playback.set_online(true));
    FloatPacket input(spec, 96);
    std::fill(input.samples.begin(), input.samples.end(), 0.5f);
    render(playback, input.block());
    check(checked(device.transfer(output.block(4, 128))) == 0 && !playback.steady() && device.correction() == 0,
          "Device emits startup silence with PI idle");

    for (int i = 0; i < 3; ++i)
        render(playback, input.block());
    const auto prefilled = playback.status().queued_frames;
    expect_error(ErrorCode::InvalidArg, device.transfer(OBBlock{nullptr, output.spec, 1}));
    check(playback.status().queued_frames == prefilled && !playback.steady(),
          "An invalid device output cannot consume or prime the queue");
    check(checked(device.transfer(output.block(4, 128))) == 128 && playback.steady() && device.correction() == 0,
          "First complete request starts audio without enabling PI mid-transfer");
    check(output.sample(131) > 0.49f, "FIR history has live samples");
    for (int i = 0; i < 4; ++i)
        checked(device.transfer(output.block(4, 128)));
    output.fill(3.0f);
    const auto silent_frames = checked(device.transfer(output.block(4, 128)));
    bool all_silent = true;
    for (std::size_t i = 4; i < 132; ++i)
        all_silent &= output.sample(i) == 0;
    check(silent_frames == 0 && all_silent && output.sample(3) == 3 && output.sample(132) == 3,
          "Zero source read in steady state bypasses nonzero historical samples");

    checked(playback.set_online(false));
    checked(playback.set_online(true));
    output.fill(3.0f);
    check(checked(device.transfer(output.block(4, 128))) == 0 && output.sample(100) == 0,
          "Re-prefill emits silence even if the caller still holds an old converter history");
    checked(playback.set_online(false));

    const PortSpec slow_spec(SampleRate::Hz8000, 1, SampleFormat::F32P);
    const PortSpec fast_spec(SampleRate::Hz96000, 1, SampleFormat::F32LE);
    Playback upsampling(slow_spec.sample_rate, slow_spec.channels, short_packet);
    PlaybackStream fast_device(upsampling);
    checked(upsampling.prepare(graph_period));
    checked(fast_device.prepare(fast_spec, 1));
    checked(upsampling.set_online(true));
    FloatPacket slow_input(slow_spec, 16);
    FloatDevicePacket fast_output(fast_spec.sample_rate, 1);
    for (int i = 0; i < 4; ++i)
        render(upsampling, slow_input.block());
    check(checked(fast_device.transfer(fast_output.block())) == 1, "A live read completes prefill");
    const auto queued = upsampling.status().queued_frames;
    check(checked(fast_device.transfer(fast_output.block())) == 1 &&
              upsampling.status().queued_frames == queued,
          "Steady upsampling can produce a frame without reading from the queue");
    checked(upsampling.set_online(false));
    fast_output.fill(3);
    check(!upsampling.online() && checked(fast_device.transfer(fast_output.block())) == 0 && fast_output.sample(0) == 0,
          "Offline playback emits silence even when the converter needs no new source frames");
    expect_error(ErrorCode::InvalidArg, fast_device.transfer(OBBlock{nullptr, fast_spec, 1}));
    expect_error(ErrorCode::TooLarge, fast_device.transfer(OBBlock{nullptr, fast_spec, 2}));

    // A missed stream reset must not prevent the queue from completing prefill.
    checked(upsampling.set_online(true));
    check(checked(fast_device.transfer(fast_output.block())) == 0 && !upsampling.steady(),
          "Restart waits for the prefill threshold");
    for (int i = 0; i < 4; ++i)
        render(upsampling, slow_input.block());
    check(checked(fast_device.transfer(fast_output.block())) == 1 && upsampling.steady(),
          "A pending history-only frame cannot stall playback prefill");
}

void virtual_device_audio()
{
    const PortSpec logical(rate, 1, SampleFormat::F32P);
    Engine engine(graph_period);
    ClockedGraph graph(engine);
    const auto capture_id = checked(engine.emplace_node<Capture>(logical.sample_rate, logical.channels, milliseconds(20)));
    const auto playback_id = checked(engine.emplace_node<Playback>(logical.sample_rate, logical.channels, milliseconds(20)));
    checked(engine.connect(OPORT(capture_id, 0), IPORT(playback_id, 0)));
    checked(engine.commit());
    auto &capture = *checked(engine.node<Capture>(capture_id));
    auto &playback = *checked(engine.node<Playback>(playback_id));
    CaptureStream input(capture);
    PlaybackStream output(playback);
    expect_error(ErrorCode::NotPrepared, input.transfer(IBBlock{}));
    constexpr std::size_t frames = 512;
    graph.start();
    for (const auto &spec : {PortSpec(SampleRate::Hz44100, 1, SampleFormat::S16LE),
                            PortSpec(rate, 1, SampleFormat::F32LE), PortSpec(rate, 1, SampleFormat::S24_3LE)})
    {
        const auto span = frames * sample_bytes(spec.format);
        std::vector<std::uint8_t> recorded(span), played(span);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            if (spec.format == SampleFormat::S16LE)
                recorded[frame * 2 + 1] = 0x20;
            else if (spec.format == SampleFormat::F32LE)
                write_f32le(recorded.data() + frame * 4, -0.5f);
            else
                recorded[frame * 3 + 2] = 0xe0;
        }
        checked(input.prepare(spec, frames));
        checked(output.prepare(spec, frames));
        checked(capture.set_online(true));
        checked(playback.set_online(true));
        const IBBlock recorded_block(recorded.data(), spec, frames);
        const OBBlock played_block(played.data(), spec, frames);
        expect_error(ErrorCode::InvalidArg, input.transfer(IBBlock{nullptr, spec, 1}));
        check(checked(input.transfer(IBBlock{nullptr, spec, 0})) == 0 && capture.status().queued_frames == 0,
              "Empty or invalid capture ranges leave the queue untouched");
        std::size_t packets = 0;
        for (std::size_t tick = 1; tick <= 200; ++tick)
        {
            if ((packets + 1) * frames * 1000 <= tick * 2 * static_cast<std::uint32_t>(spec.sample_rate))
            {
                checked(input.transfer(recorded_block));
                const auto real = checked(output.transfer(played_block));
                if (++packets > 16)
                {
                    check(real == frames && capture.steady() && playback.steady(),
                          "Virtual capture, render and playback deliver complete steady packets");
                    if (spec.format == SampleFormat::S16LE)
                        check(played[span - 1] == 0x20, "Rate-converted PCM keeps its expected DC amplitude");
                    else if (spec.format == SampleFormat::F32LE)
                        check(std::abs(read_f32le(played.data() + span - 4) + 0.5f) < 1e-5,
                              "Reopened device delivers its new format and samples through the original graph");
                    else
                        check(played[span - 1] == 0xe0, "Packed 24-bit device audio reaches the original graph");
                }
            }
            graph.advance();
        }
        checked(capture.set_online(false));
        checked(playback.set_online(false));
    }
    graph.stop();
}

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
                                  {"virtual_device_audio", virtual_device_audio}});
}
