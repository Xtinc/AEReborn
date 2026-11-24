#include "backend/null_backend.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "plugins/matrix.h"
#include "support/clocked_graph.h"
#include "support/pcm_reference.h"
#include "support/test_harness.h"
#include <algorithm>
#include <array>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;

void feed(Playback &endpoint, const float *samples, std::size_t frames) noexcept
{
    const IBBlock input(reinterpret_cast<const std::uint8_t *>(samples), endpoint.spec(), frames);
    endpoint.process({period, 0, {&input, 1}, {}});
}

void lifecycle()
{
    const PortSpec pcm(rate, 1, SampleFormat::S16LE);
    Playback endpoint(rate, 1, 20ms);
    Playback stereo(rate, 2, 20ms);
    PullSink sink(pcm);
    NullBackend device(SampleFormat::S16LE);
    Backend &control = sink;
    std::array<std::uint8_t, 2> bytes{};
    const OBBlock out(bytes.data(), pcm, 1);

    expect_error(ErrorCode::InvalidArg, [] { PullSink invalid(PortSpec{}); });
    expect_error(ErrorCode::InvalidArg, [] { PullSink planar({rate, 1, SampleFormat::F32P}); });
    expect_error(ErrorCode::NotReady, [&] { control.start(); });
    expect_error(ErrorCode::NotReady, [&] { sink.bind(endpoint); });
    expect_error(ErrorCode::InvalidArg, [&] { sink.pull({nullptr, pcm, 1}); });
    expect_error(ErrorCode::FormatMismatch, [&] { sink.pull({nullptr, {}, 0}); });
    check(sink.pull(out) == 0 && sink.pull({nullptr, pcm, 0}) == 0, "Stopped and empty pulls return no frames");

    stereo.prepare(period);
    expect_error(ErrorCode::FormatMismatch, [&] { sink.bind(stereo); });
    check(!control.bound() && !control.device_max_frames(), "Failed binding clears the provisional state");
    device.bind(stereo);
    device.unbind();

    endpoint.prepare(period);
    sink.bind(endpoint);
    expect_error(ErrorCode::Busy, [&] { device.bind(endpoint); });
    control.unbind();
    device.bind(endpoint);
    expect_error(ErrorCode::Busy, [&] { sink.bind(endpoint); });
    device.unbind();
    sink.bind(endpoint);
    check(control.bound() && control.device_spec() == pcm && control.device_max_frames() == 480,
          "The base exposes the prepared format and slice limit");
    control.start();
    expect_error(ErrorCode::Busy, [&] { control.start(); });
    const float sample = 0.5f;
    feed(endpoint, &sample, 1);
    control.stop();
    control.stop();
    check(control.bound() && !control.running(), "Stop retains the binding");
    control.start();
    check(sink.pull(out) == 0, "Restart discards the old queue");
    const auto negative = -sample;
    feed(endpoint, &negative, 1);
    check(sink.pull(out) == 1 && bytes[0] == 0 && bytes[1] == 192, "A new stream starts with an empty queue");
    control.unbind();
    check(!control.bound() && !control.running() && control.last_error() == ErrorCode::Ok, "Unbind stops a passive sink");
}

void fixed_pcm()
{
    constexpr std::size_t frames = 700;
    std::array<float, frames> samples{};
    for (std::size_t i = 0; i < frames; ++i)
        samples[i] = static_cast<float>(static_cast<int>(i % 15) - 7) / 8.0f;
    samples.back() = -0.0f;

    for (const auto format : {SampleFormat::S16LE, SampleFormat::F32LE})
    {
        Playback endpoint(rate, 1, 20ms);
        endpoint.prepare(period);
        const PortSpec pcm(rate, 1, format);
        const auto width = sample_bytes(format);
        PullSink sink(pcm);
        sink.bind(endpoint);
        sink.start();
        for (std::size_t offset = 0; offset < frames; offset += 240)
            feed(endpoint, samples.data() + offset, std::min<std::size_t>(240, frames - offset));

        std::vector<std::uint8_t> bytes((frames + 8) * width, 0xad);
        check(sink.pull({bytes.data(), pcm, frames + 8}) == frames, "Pull can span more than one scratch slice");
        for (std::size_t i = 0; i < frames; ++i)
        {
            const auto *actual = bytes.data() + i * width;
            if (format == SampleFormat::F32LE)
            {
                std::array<std::uint8_t, 4> expected{};
                write_f32le(expected.data(), samples[i]);
                check(std::equal(expected.begin(), expected.end(), actual), "F32LE samples preserve their bits");
            }
            else
            {
                const auto expected = static_cast<std::uint16_t>(static_cast<int>(samples[i] * 32768));
                check(actual[0] == (expected & 0xff) && actual[1] == (expected >> 8), "S16LE samples retain order");
            }
        }
        const auto suffix = bytes.data() + frames * width;
        check(sink.pull({suffix, pcm, 8}) == 0 && std::all_of(suffix, bytes.data() + bytes.size(),
                  [](std::uint8_t byte) { return byte == 0xad; }), "Short and empty pulls leave the suffix intact");
    }
}

void check_resampling(SampleRate input_rate, SampleRate output_rate)
{
    constexpr std::size_t frames = 799;
    Buffer input;
    input.resize(1, frames);
    const auto samples = input.block(frames).channel(0);
    for (std::size_t i = 0; i < frames; ++i)
        samples[i] = static_cast<float>(i % 19) / 32.0f;

    SincResampler reference;
    reference.prepare(1, input_rate, output_rate);
    const auto bound = SincResampler::max_output_frames(input_rate, output_rate, frames);
    Buffer expected;
    expected.resize(1, bound);
    const auto converted = reference.process(input.block(frames), expected.block(bound));
    check(converted.consumed == frames, "The reference consumes the entire input");

    const PortSpec pcm(output_rate, 1, SampleFormat::F32LE);
    Playback endpoint(input_rate, 1, 20ms);
    endpoint.prepare(period);
    PullSink sink(pcm);
    sink.bind(endpoint);
    sink.start();
    std::array<std::uint8_t, 601 * 4> bytes{};
    std::size_t pulled = 0;
    const auto pull = [&](std::size_t count) {
        const auto got = sink.pull({bytes.data(), pcm, count});
        check(got <= count && pulled + got <= converted.produced, "Pull respects both output bounds");
        for (std::size_t i = 0; i < got; ++i)
            check(read_f32le(bytes.data() + i * 4) == expected.block(bound).channel(0)[pulled + i],
                  "Fragmented pulls preserve the reference waveform and phase");
        pulled += got;
        return got;
    };

    check(pull(1) == 0, "FIR starts with no queued data");
    feed(endpoint, samples, 1);
    check(pull(1) == 1, "One source frame yields the first output");
    if (input_rate < output_rate)
        check(pull(1) == 1, "Upsampling can continue using history with an empty queue");
    feed(endpoint, samples + 1, 1);
    if (input_rate > output_rate)
        check(pull(1) == 0 && pull(1) == 0, "Insufficient input leaves the downsampling phase and queue intact");

    for (std::size_t offset = 2; offset < frames; offset += 73)
    {
        feed(endpoint, samples + offset, std::min<std::size_t>(73, frames - offset));
        while (pull(13))
        {
        }
    }
    check(pull(601) == 0 && pulled == converted.produced, "All available output is delivered without tail padding");
}

void resampled_stream()
{
    check_resampling(SampleRate::Hz48000, SampleRate::Hz16000);
    check_resampling(SampleRate::Hz16000, SampleRate::Hz48000);
}

void clocked_round_trip()
{
    constexpr std::size_t frames = 240;
    const PortSpec source_pcm(rate, 1, SampleFormat::F32LE);
    const PortSpec sink_pcm(rate, 2, SampleFormat::F32LE);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto capture = engine.emplace_node<Capture>(rate, 1, 5ms);
    const auto matrix = engine.emplace_node<Matrix>(rate, 1, 2, std::initializer_list<float>{1.0f, -1.0f});
    const auto playback = engine.emplace_node<Playback>(rate, 2, 5ms);
    engine.connect(OPORT(capture, 0), IPORT(matrix, 0));
    engine.connect(OPORT(matrix, 0), IPORT(playback, 0));
    engine.commit();

    PushSource source(source_pcm);
    PullSink sink(sink_pcm);
    source.bind(*engine.node<Capture>(capture));
    sink.bind(*engine.node<Playback>(playback));
    source.start();
    sink.start();
    clock.start();
    std::array<std::uint8_t, frames * 4> input{};
    std::array<std::uint8_t, frames * 2 * 8> output{};

    for (const auto sample : {0.25f, -0.5f})
    {
        for (std::size_t i = 0; i < frames; ++i)
            write_f32le(input.data() + i * 4, sample);
        check(source.push({input.data(), source_pcm, frames}) == frames, "Push accepts a complete input block");
        clock.advance();
        if (sample > 0)
        {
            check(sink.pull({output.data(), sink_pcm, 1}) == 1 && read_f32le(output.data()) == sample,
                  "Pull can consume a prefix of the first graph block");
        }
    }

    const auto got = sink.pull({output.data(), sink_pcm, 2 * frames});
    check(got == 2 * frames - 1, "Pull spans the remainder of the first block and the second block");
    for (std::size_t i = 0; i < got; ++i)
    {
        const auto expected = i < frames - 1 ? 0.25f : -0.5f;
        check(read_f32le(output.data() + i * 8) == expected && read_f32le(output.data() + i * 8 + 4) == -expected,
              "Pull retains frame order and the graph's stereo mapping");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv, {{"lifecycle", lifecycle}, {"fixed_pcm", fixed_pcm},
                                 {"resampled_stream", resampled_stream}, {"clocked_round_trip", clocked_round_trip}});
}
