#include "backend/null_backend.h"
#include "backend/push_source.h"
#include "backend/usr_playback.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/clocked_graph.h"
#include "support/pcm_reference.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <ratio>
#include <thread>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

constexpr auto rate = SampleRate::Hz48000;
constexpr auto graph_period = BlockPeriod::Ms05;

void feed(Playback &endpoint, const float *samples, std::size_t frames, std::uint64_t tick = 0) noexcept
{
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(samples), endpoint.spec(), frames);
    endpoint.process({graph_period, tick, {&input, 1}, {}});
}

void lifecycle()
{
    const PortSpec pcm(rate, 1, SampleFormat::F32LE);
    const auto consume = [](IBBlock) noexcept {};
    expect_error(ErrorCode::InvalidArg, [&] { UsrPlayback invalid({}, consume); });
    expect_error(ErrorCode::InvalidArg, [&] { UsrPlayback planar({rate, 1, SampleFormat::F32P}, consume); });
    expect_error(ErrorCode::InvalidArg, [&] { UsrPlayback no_callback(pcm, {}); });

    Capture capture(rate, 1, 10ms);
    Playback playback(rate, 1, 10ms);
    Playback stereo(rate, 2, 10ms);
    Playback tiny_period(rate, 1, 1us);
    UsrPlayback callback(pcm, consume);
    Backend &control = callback;
    ActiveBackend &device = callback;
    NullBackend other(SampleFormat::F32LE);
    expect_error(ErrorCode::NotReady, [&] { control.start(); });
    expect_error(ErrorCode::NotReady, [&] { callback.bind(playback); });

    capture.prepare(graph_period);
    playback.prepare(graph_period);
    stereo.prepare(graph_period);
    tiny_period.prepare(graph_period);
    expect_error(ErrorCode::InvalidArg, [&] { device.bind(capture); });
    other.bind(capture);
    other.unbind();
    expect_error(ErrorCode::InvalidArg, [&] { callback.bind(tiny_period); });
    expect_error(ErrorCode::FormatMismatch, [&] { callback.bind(stereo); });
    other.bind(stereo);
    other.unbind();
    check(!control.bound() && !control.device_max_frames(), "Failed callbacks leave no binding or batch budget");

    callback.bind(playback);
    control.start();
    expect_error(ErrorCode::Busy, [&] { control.start(); });
    control.stop();
    check(control.bound() && !control.running() && control.last_error() == ErrorCode::Ok,
          "An empty stop retains the endpoint for restart");
    control.start();
    control.unbind();
    check(!control.bound() && !control.running(), "Polymorphic unbind joins and detaches the callback worker");
    other.bind(playback);
}

void check_batches(SampleRate sample_rate, std::chrono::microseconds io_period, std::size_t expected)
{
    const PortSpec pcm(sample_rate, 1, SampleFormat::F32LE);
    Playback endpoint(sample_rate, 1, io_period);
    endpoint.prepare(graph_period);
    std::vector<float> input(2 * expected + 1);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i % 63) / 64.0f;

    std::array<std::size_t, 3> batches{};
    std::size_t calls = 0, delivered = 0;
    bool valid = true;
    const auto control_thread = std::this_thread::get_id();
    UsrPlayback output(pcm, [&](IBBlock block) noexcept {
        if (calls == batches.size() || delivered + block.frames() > input.size())
        {
            valid = false;
            return;
        }
        valid &= block.spec() == pcm && std::this_thread::get_id() != control_thread;
        const auto *data = block.byte_view().data();
        for (std::size_t i = 0; i < block.frames(); ++i)
            valid &= read_f32le(data + i * 4) == input[delivered + i];
        batches[calls++] = block.frames();
        delivered += block.frames();
    });

    output.bind(endpoint);
    check(output.device_max_frames() == expected, "Binding computes one fixed size in the output sample rate");
    output.start();
    std::size_t offset = 0;
    for (std::uint64_t tick = 0; offset < input.size(); ++tick)
    {
        const auto frames = std::min(pcm.frames(graph_period, tick), input.size() - offset);
        feed(endpoint, input.data() + offset, frames, tick);
        offset += frames;
    }
    output.stop();
    check(valid && calls == 3 && batches == std::array<std::size_t, 3>{{expected, expected, 1}},
          "The worker delivers fixed batches followed by the short drained batch");
    check(delivered == input.size() && output.last_error() == ErrorCode::Ok,
          "Reblocking preserves all samples, including any graph tick remainder");
}

void fixed_batches()
{
    const std::chrono::duration<int, std::ratio<1, 30>> requested{1};
    auto period = std::chrono::duration_cast<std::chrono::microseconds>(requested);
    if (period < requested)
        period += 1us;
    check(period == 33334us, "Round the requested fractional period once before binding");
    check_batches(SampleRate::Hz44100, period, 1470);
    check_batches(SampleRate::Hz44100, 33333us, 1469);
    check_batches(rate, period, 1600);
    check_batches(rate, 10ms, 480);
    check_batches(SampleRate::Hz44100, 5ms, 220);
}

void fir_drain()
{
    constexpr auto input_rate = SampleRate::Hz16000;
    constexpr std::size_t frames = 37;
    const PortSpec pcm(rate, 1, SampleFormat::F32LE);
    Playback endpoint(input_rate, 1, 2ms);
    endpoint.prepare(graph_period);

    SincResampler reference;
    reference.prepare(1, input_rate, rate);
    const auto count = frames + reference.tail_frames();
    Buffer input, expected;
    input.resize(1, count);
    input.block(count).clear();
    const auto *samples = input.block(count).channel(0);
    input.block(count).channel(0)[frames - 1] = 0.75f;
    const auto bound = SincResampler::max_output_frames(input_rate, rate, count);
    expected.resize(1, bound);
    const auto result = reference.process(input.block(count), expected.block(bound));
    const auto *waveform = expected.block(bound).channel(0);

    std::size_t delivered = 0;
    bool valid = true;
    UsrPlayback output(pcm, [&](IBBlock block) noexcept {
        if (delivered + block.frames() > result.produced)
        {
            valid = false;
            return;
        }
        const auto *bytes = block.byte_view().data();
        for (std::size_t i = 0; i < block.frames(); ++i)
            valid &= read_f32le(bytes + i * 4) == waveform[delivered + i];
        delivered += block.frames();
    });
    output.bind(endpoint);
    output.start();
    feed(endpoint, samples, frames);
    output.stop();
    check(valid && delivered == result.produced, "Stop delivers the complete response of the last input frame");

    delivered = 0;
    output.start();
    feed(endpoint, samples, frames);
    output.stop();
    check(valid && delivered == result.produced, "Restart resets the resampler before delivering the next stream");
}

void graph_silence()
{
    const PortSpec pcm(rate, 1, SampleFormat::S16LE);
    Engine engine(graph_period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto endpoint = engine.emplace_node<Playback>(rate, 1, 5ms);
    engine.connect(OPORT(input, 0), IPORT(endpoint, 0));
    engine.commit();
    PushSource source(pcm);
    source.bind(*engine.node<Capture>(input));
    source.start();

    std::size_t batches = 0;
    bool silent = true;
    UsrPlayback output(pcm, [&](IBBlock block) noexcept {
        const auto bytes = block.byte_view();
        silent &= block.frames() == 240 && std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
        ++batches;
    });
    output.bind(*engine.node<Playback>(endpoint));
    output.start();
    clock.start();
    clock.advance();
    clock.advance();
    output.stop();
    check(batches == 2 && silent, "An active graph with a quiet source delivers its ordinary PCM silence");
}

void fanout_and_mix()
{
    constexpr auto raw_rate = SampleRate::Hz44100;
    constexpr std::size_t raw_frames = 441, mix_frames = 480;
    constexpr std::size_t channels = 16;
    constexpr std::size_t ticks = 13;
    const PortSpec raw_pcm(raw_rate, channels, SampleFormat::F32LE);
    const PortSpec extra_pcm(rate, 2, SampleFormat::F32LE);
    const PortSpec output_pcm(raw_rate, 2, SampleFormat::S16LE);
    Engine engine(BlockPeriod::Ms10);
    ClockedGraph clock(engine);
    const auto raw = engine.emplace_node<Capture>(raw_rate, channels, 10ms);
    const auto extra = engine.emplace_node<Capture>(rate, 2, 10ms);
    const auto raw_play = engine.emplace_node<Playback>(raw_rate, channels, 10ms);
    const auto mix_play = engine.emplace_node<Playback>(rate, 2, 33334us);
    std::array<float, 2 * channels> coefficients{};
    coefficients[0] = 1.0f;
    coefficients[channels + 1] = 1.0f;
    const auto matrix = engine.emplace_node<Matrix>(raw_rate, channels, 2,
                                                    View<const float>(coefficients.data(), coefficients.size()));
    const auto convert = engine.emplace_node<Convert>(PortSpec(raw_rate, 2, SampleFormat::F32P),
                                                      PortSpec(rate, 2, SampleFormat::F32P));
    const auto mixer = engine.emplace_node<Mixer>(rate, 2, 2);
    engine.connect(OPORT(raw, 0), IPORT(raw_play, 0));
    engine.connect(OPORT(raw, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(convert, 0));
    engine.connect(OPORT(convert, 0), IPORT(mixer, 0));
    engine.connect(OPORT(extra, 0), IPORT(mixer, 1));
    engine.connect(OPORT(mixer, 0), IPORT(mix_play, 0));
    engine.commit();

    std::array<std::uint8_t, raw_frames * channels * 4> raw_bytes{};
    std::array<std::uint8_t, mix_frames * 2 * 4> extra_bytes{};
    for (std::size_t f = 0; f < raw_frames; ++f)
        for (std::size_t ch = 0; ch < channels; ++ch)
            write_f32le(raw_bytes.data() + (f * channels + ch) * 4, static_cast<float>(ch + 1) / 8.0f);
    for (std::size_t f = 0; f < mix_frames; ++f)
    {
        write_f32le(extra_bytes.data() + f * 8, 0.125f);
        write_f32le(extra_bytes.data() + f * 8 + 4, 0.0625f);
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool release = false, raw_matches = true;
    std::size_t raw_delivered = 0, mix_delivered = 0;
    std::array<std::uint8_t, 7000 * 4> mixed_bytes{};
    bool mix_fits = true;
    PushSource raw_source(raw_pcm), extra_source(extra_pcm);
    UsrPlayback slow(raw_pcm, [&](IBBlock block) noexcept {
        std::unique_lock<std::mutex> lock(mutex);
        const auto bytes = block.byte_view();
        raw_matches &= bytes.size() == raw_bytes.size() && std::equal(bytes.begin(), bytes.end(), raw_bytes.begin());
        raw_delivered += block.frames();
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    UsrPlayback fast(output_pcm, [&](IBBlock block) noexcept {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto bytes = block.byte_view();
        if (mix_delivered + block.frames() <= mixed_bytes.size() / 4)
            std::copy(bytes.begin(), bytes.end(), mixed_bytes.begin() + mix_delivered * 4);
        else
            mix_fits = false;
        mix_delivered += block.frames();
        changed.notify_all();
    });
    const auto unblock = [&]() noexcept {
        const std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_all();
    };
    ScopeExit<decltype(unblock)> release_on_failure(unblock);

    raw_source.bind(*engine.node<Capture>(raw));
    extra_source.bind(*engine.node<Capture>(extra));
    slow.bind(*engine.node<Playback>(raw_play));
    fast.bind(*engine.node<Playback>(mix_play));
    Backend *const backends[] = {&raw_source, &extra_source, &slow, &fast};
    for (auto *backend : backends)
        backend->start();
    clock.start();
    for (std::size_t tick = 1; tick <= ticks; ++tick)
    {
        check(raw_source.push({raw_bytes.data(), raw_pcm, raw_frames}) == raw_frames, "Raw PCM reaches Capture");
        check(extra_source.push({extra_bytes.data(), extra_pcm, mix_frames}) == mix_frames, "Extra source reaches Capture");
        clock.advance();
        std::unique_lock<std::mutex> lock(mutex);
        if (tick == 1)
            check(changed.wait_for(lock, 2s, [&] { return raw_delivered == raw_frames; }), "The slow callback entered");
        if (tick % 4 == 0)
            check(changed.wait_for(lock, 2s, [&] { return mix_delivered >= tick / 4 * 1470; }),
                  "The mixed output makes progress while the other callback is held");
    }
    unblock();
    release_on_failure.dismiss();
    slow.stop();
    fast.stop();
    check(raw_matches && raw_delivered > raw_frames && raw_delivered < ticks * raw_frames,
          "The slow branch preserves queued samples and drops only the overflow");
    check(mix_fits && mix_delivered >= 3 * 1470 && fast.last_error() == ErrorCode::Ok,
          "The mixed branch delivers PCM throughout the slow callback");
    for (std::size_t i = 960; i < 1100; ++i)
    {
        const auto *pcm = mixed_bytes.data() + i * 4;
        const auto left = static_cast<int>(pcm[0]) + (static_cast<int>(pcm[1]) << 8);
        const auto right = static_cast<int>(pcm[2]) + (static_cast<int>(pcm[3]) << 8);
        check(std::abs(left - 8192) < 32 && std::abs(right - 10240) < 32,
              "Channel mapping, rate conversion and user mixing produce the expected stereo values");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"lifecycle", lifecycle}, {"fixed_batches", fixed_batches},
                                 {"fir_drain", fir_drain}, {"graph_silence", graph_silence},
                                 {"fanout_and_mix", fanout_and_mix}});
}
