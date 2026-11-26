#include "tests/support/io_fixture.h"
#include "backend/null_backend.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "backend/usr_playback.h"
#include "backend/wav_backend.h"
#include "plugins/mixer.h"
#include "tests/support/test_backend.h"
#include "tests/support/tone.h"

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

constexpr std::size_t frames = 240;
const PortSpec planar(rate, 1, SampleFormat::F32P), pcm(rate, 1, SampleFormat::F32LE);

void slow_callback_parallel_archive()
{
    TempWave files("slow-listener");
    const auto path = files.path();
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t frame, std::size_t) {
        return static_cast<float>(frame) / 8192;
    });
    const auto extra = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return -0.25f; });
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto raw = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto live = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto archive = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, source, raw);
    connect(engine, source, mix);
    connect(engine, extra, mix, 1);
    connect(engine, mix, archive);
    connect(engine, mix, live);
    engine.commit();
    constexpr std::size_t ticks = 16;
    Pcm raw_pcm(pcm, ticks * frames), live_pcm(pcm, ticks * frames);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, fits = true;
    std::size_t delivered = 0;
    UsrPlayback callback(pcm, [&](IBBlock block) noexcept {
        std::unique_lock<std::mutex> lock(mutex);
        fits &= raw_pcm.copy_from(delivered, block);
        delivered += block.frames();
        entered = true;
        changed.notify_one();
        changed.wait(lock, [&] { return release; });
    });
    const auto unblock = [&]() noexcept {
        const std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_one();
    };
    ScopeExit<decltype(unblock)> release_on_failure(unblock);
    PullSink listener(pcm);
    WavPlayback writer(path, SampleFormat::F32LE);
    callback.bind(*engine.node<Playback>(raw));
    listener.bind(*engine.node<Playback>(live));
    writer.bind(*engine.node<Playback>(archive));
    callback.start();
    listener.start();
    writer.start();
    clock.start();
    for (std::size_t tick = 0; tick < ticks; ++tick)
    {
        wait_for_playback(writer, frames);
        clock.advance();
        if (tick == 0)
        {
            std::unique_lock<std::mutex> lock(mutex);
            check(changed.wait_for(lock, 3s, [&] { return entered; }), "The first callback is held");
        }
        check(listener.pull(live_pcm.output(tick * frames, frames)) == frames, "The live mix advances during the stall");
    }
    const auto queued = QueueState::queued(callback);
    check(queued == 2 * QueueState::target(callback) && queued < (ticks - 1) * frames,
          "The blocked callback's queue fills and overflows");
    unblock();
    release_on_failure.dismiss();
    callback.stop();
    writer.unbind();
    check(fits && delivered == frames + queued && callback.last_error() == RetCode::OK &&
              writer.last_error() == RetCode::OK,
          "The slow branch delivers exactly its first block and the queued prefix");
    for (std::size_t frame = 0; frame < delivered; ++frame)
        check(raw_pcm.sample(frame) == static_cast<float>(frame) / 8192, "The retained callback prefix stays in order");
    for (std::size_t frame = 0; frame < live_pcm.frames; ++frame)
        check(live_pcm.sample(frame) == static_cast<float>(frame) / 8192 - 0.25f, "The fast mix loses no frames");
    check(read_wave(path, pcm, live_pcm.frames).bytes == live_pcm.bytes, "The archive matches the continuous live mix");
}

void irregular_push_and_pull()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto bias = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return -0.125f; });
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto listener = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto monitor = engine.emplace_node<PacketSink>(planar);
    connect(engine, source, mix);
    connect(engine, bias, mix, 1);
    connect(engine, mix, listener);
    connect(engine, source, monitor);
    engine.commit();
    auto &capture = *engine.node<Capture>(source);
    const auto *dry = engine.node<PacketSink>(monitor);
    PushSource push(pcm);
    PullSink pull(pcm);
    push.bind(capture);
    pull.bind(*engine.node<Playback>(listener));
    push.start();
    pull.start();
    clock.start();
    const auto capacity = QueueState::capture_space(push);
    const auto ticks = 4 * capacity / frames;
    Pcm input(pcm, ticks * frames), output(pcm, ticks * frames);
    for (std::size_t frame = 0; frame < input.frames; ++frame)
        input.set(frame, 0, static_cast<float>(frame) / 16384);

    const auto initial = push.push(input.input(0, capacity + 37));
    check(initial == capacity && push.push(input.input(initial, 37)) == 0,
          "A full queue accepts exactly its remaining capacity");
    std::size_t pushed = initial, pulled = 0;
    for (std::size_t tick = 0; tick < ticks; ++tick)
    {
        clock.advance();
        for (std::size_t frame = 0; frame < frames; ++frame)
            check(dry->sample(frame) == input.sample(tick * frames + frame), "The parallel dry tap keeps input order");
        for (const auto size : {std::size_t{97}, std::size_t{317}})
        {
            const auto count = std::min(size, input.frames - pushed);
            pushed += push.push(input.input(pushed, count));
        }
        // Leave a partial block queued on even ticks, then read across the boundary.
        const std::size_t count = tick % 2 ? 367 : 113;
        check(pull.pull(output.output(pulled, count)) == count, "Pull spans the requested blocks");
        pulled += count;
    }
    check(pushed == input.frames && pulled == input.frames, "All partially offered input is delivered");
    for (std::size_t frame = 0; frame < output.frames; ++frame)
        check(output.sample(frame) == input.sample(frame) - 0.125f, "The mix retains every frame across ring wrap");
}

void capture_gaps_keep_playback_timeline()
{
    TempWave files("capture-gaps");
    const auto input_path = files.path("-input.wav"), output_path = files.path("-recorded.wav");
    constexpr std::size_t short_frames = 7;
    Pcm wave(pcm, frames + short_frames), expected(pcm, 8 * frames);
    for (std::size_t frame = 0; frame < short_frames; ++frame)
    {
        wave.set(frames + frame, 0, -0.5f);
        expected.set(3 * frames + frame, 0, 0.25f);
        expected.set(6 * frames + frame, 0, -0.5f);
    }
    write_wave(input_path, wave);

    Engine engine(period);
    ClockedGraph clock(engine);
    const auto capture = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto monitor = engine.emplace_node<PacketSink>(planar);
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto playback = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, capture, monitor);
    connect(engine, capture, mix);
    connect(engine, mix, playback);
    engine.commit();
    auto &source = *engine.node<Capture>(capture);
    const auto *dry = engine.node<PacketSink>(monitor);
    WavPlayback writer(output_path, SampleFormat::F32LE);
    writer.bind(*engine.node<Playback>(playback));
    writer.start();
    clock.start();
    const auto advance = [&](bool active, const char *message) {
        wait_for_playback(writer, frames);
        clock.advance();
        check(dry->frames == frames && dry->active == active, message);
    };
    advance(false, "Offline capture delivers an inactive block");

    TestBackend prefilling(pcm, frames);
    prefilling.block = true;
    prefilling.bind(source);
    prefilling.start();
    advance(false, "Capture stays inactive while waiting for prefill");
    check(source.online() && !source.steady(), "The capture is online and still prefilling");
    prefilling.unbind();

    PushSource push(pcm);
    push.bind(source);
    push.start();
    Pcm packet(pcm, frames);
    check(push.push(packet.input(0, frames)) == frames, "The queue accepts an intentional zero block");
    advance(true, "Pushed zero samples keep the capture active");
    for (std::size_t frame = 0; frame < short_frames; ++frame)
        packet.set(frame, 0, 0.25f);
    check(push.push(packet.input(0, short_frames)) == short_frames, "The queue accepts a partial block");
    advance(true, "A short read keeps the capture active");
    check(dry->sample(short_frames - 1) == 0.25f && dry->sample(short_frames) == 0.0f,
          "Capture zero-fills the missing suffix of an active short read");
    advance(false, "An empty queue makes the next block inactive");
    check(source.online(), "The sender can stay online across an empty tick");
    push.unbind();

    WavCapture reader(input_path, false);
    reader.bind(source);
    reader.start();
    wait_for_capture(reader, wave.frames);
    advance(true, "Intentional silence from a WAV file is active");
    advance(true, "The final partial WAV block is active");
    wait_for([&] { return !reader.running(); }, "WAV capture reaches EOF after the graph consumes the queue");
    advance(false, "A drained WAV capture becomes inactive");
    reader.unbind();
    writer.unbind();
    check(read_wave(output_path, pcm, expected.frames).bytes == expected.bytes,
          "WAV playback records every tick, including inactive blocks and partial-block padding");
}

void priority_mixer_silent_packets()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto background = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t, std::size_t) { return 0.5f; });
    const auto call = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto mixed = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto monitor = engine.emplace_node<PacketSink>(planar);
    connect(engine, background, mixed);
    connect(engine, call, mixed, 1);
    connect(engine, mixed, monitor);
    auto &mixer = *engine.node<Mixer>(mixed);
    mixer.set_high_priority(1, true);
    engine.commit();
    const auto *result = engine.node<PacketSink>(monitor);
    PushSource push(pcm);
    push.bind(*engine.node<Capture>(call));
    push.start();
    clock.start();
    clock.advance();
    check(result->active && result->sample(0) == 0.5f && !mixer.suppressed(0),
          "Background plays at unity before the call");

    Pcm packet(pcm, 2 * frames);
    check(push.push(packet.input(0, packet.frames)) == packet.frames, "The call queues two intentional silent blocks");
    clock.advance();
    check(result->active && mixer.suppressed(0), "Even a silent call triggers suppression");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto gain = static_cast<float>((frames - 1 - frame) / static_cast<double>(frames));
        check(result->sample(frame) == 0.5f * gain, "The background fades out over the first silent block");
    }
    clock.advance();
    check(result->active && mixer.suppressed(0), "The second intentional zero block remains active");
    for (std::size_t frame = 0; frame < frames; ++frame)
        check(result->sample(frame) == 0.0f, "The active call keeps the background silent");

    clock.advance();
    check(result->active && !mixer.suppressed(0), "Suppression releases on the first empty tick");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto gain = static_cast<float>((frame + 1) / static_cast<double>(frames));
        check(result->sample(frame) == 0.5f * gain, "The background fades in when the call queue drains");
    }
    clock.advance();
    check(result->sample(0) == 0.5f && result->sample(frames - 1) == 0.5f, "The next block stays at unity");
}

void device_loss_and_rebind()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto steady = engine.emplace_node<FloatSource>(rate, 1, [](std::uint64_t frame, std::size_t) {
        return static_cast<float>(frame % 19) / 128;
    });
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto output = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, input, mix);
    connect(engine, steady, mix, 1);
    connect(engine, mix, output);
    engine.commit();
    auto &capture = *engine.node<Capture>(input);
    Pcm packet(pcm, frames), rendered(pcm, frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        packet.set(frame, 0, 0.25f);
    TestBackend device(pcm, frames, nullptr, false);
    device.input = packet.input(0, frames);
    device.gated = true;
    NullBackend fallback(SampleFormat::S16LE);
    PushSource recovered(pcm);
    PullSink listener(pcm);
    device.bind(capture);
    listener.bind(*engine.node<Playback>(output));
    device.start();
    listener.start();
    check(device.wait_for_io() && device.run_batch(), "The device supplies its first live block");
    clock.start();
    const auto verify = [&](float contribution) {
        const auto tick = engine.tick_position();
        clock.advance();
        check(listener.pull(rendered.output(0, frames)) == frames, "Master playback continues through failover");
        for (std::size_t frame = 0; frame < frames; ++frame)
            check(rendered.sample(frame) == contribution + static_cast<float>((tick * frames + frame) % 19) / 128,
                  "The healthy branch stays aligned while the device changes");
    };
    verify(0.25f);
    device.io_result = RetCode::DEVLOST;
    check(!device.run_batch(), "The next device transfer ends on loss");
    wait_for([&] { return !device.running(); }, "The lost device worker stops");
    check(device.last_error() == RetCode::DEVLOST && QueueState::queued(device) == 0,
          "Loss closes the capture queue");
    verify(0);
    device.unbind();
    fallback.bind(capture);
    fallback.start();
    wait_for_capture(fallback, frames);
    verify(0);
    fallback.unbind();
    recovered.bind(capture);
    recovered.start();
    check(recovered.push(packet.input(0, frames)) == frames, "The recovered endpoint accepts input");
    verify(0.25f);
}

void restart_shared_outputs()
{
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto tone = engine.emplace_node<Tone>(rate, 1, 1000, 0.125f);
    const auto mix = engine.emplace_node<Mixer>(rate, 1, 2);
    const auto pull_node = engine.emplace_node<Playback>(rate, 1, 5ms);
    const auto null_node = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, input, mix);
    connect(engine, tone, mix, 1);
    connect(engine, mix, pull_node);
    connect(engine, mix, null_node);
    engine.commit();
    auto &discard = *engine.node<Playback>(null_node);
    PushSource source(pcm);
    PullSink listener(pcm);
    NullBackend sink(SampleFormat::S16LE);
    source.bind(*engine.node<Capture>(input));
    listener.bind(*engine.node<Playback>(pull_node));
    sink.bind(discard);
    Pcm input_pcm(pcm, 2 * frames), output(pcm, frames);
    for (const auto level : {0.25f, -0.5f})
    {
        for (std::size_t frame = 0; frame < input_pcm.frames; ++frame)
            input_pcm.set(frame, 0, level);
        check(source.push(input_pcm.input(0, frames)) == 0 && listener.pull(output.output(0, frames)) == 0,
              "Stopped handles transfer no frames");
        source.start();
        listener.start();
        sink.start();
        clock.start();
        check(engine.tick_position() == 0, "A restarted timeline begins from tick zero");
        check(source.push(input_pcm.input(0, input_pcm.frames)) == input_pcm.frames, "The input queues two blocks");
        clock.advance();
        check(listener.pull(output.output(0, frames)) == frames, "The listener delivers the first fresh block");
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto expected = level + 0.125 * std::sin(two_pi * 1000 * frame / 48000);
            check(std::abs(output.sample(frame) - expected) < 1e-6, "The mix starts with new samples and zero tone phase");
        }
        wait_for([&] { return QueueState::queued(sink) == 0; }, "Null playback consumes the fresh block");
        check(sink.running() && sink.last_error() == RetCode::OK, "Null playback remains ready after consuming");
        source.stop();
        listener.stop();
        sink.stop();
        clock.stop();
        engine.reset();
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"slow_callback_parallel_archive", slow_callback_parallel_archive},
                                                {"irregular_push_and_pull", irregular_push_and_pull},
                                                {"capture_gaps_keep_playback_timeline", capture_gaps_keep_playback_timeline},
                                                {"priority_mixer_silent_packets", priority_mixer_silent_packets},
                                                {"device_loss_and_rebind", device_loss_and_rebind},
                                                {"restart_shared_outputs", restart_shared_outputs}});
}
