#include "tests/support/io_fixture.h"
#include "tests/support/test_backend.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include <fstream>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
const PortSpec pcm(rate, 1, SampleFormat::F32LE);

void endpoint_ownership_and_restart()
{
    Capture capture(rate, 1, 5ms);
    Playback playback(rate, 1, 5ms);
    expect_error(RetCode::EPARAM, [] { Capture invalid(rate, 0, 5ms); });
    expect_error(RetCode::EPARAM, [] { Playback invalid(rate, 1, 0ms); });
    PushSource source(pcm), other(pcm);
    PullSink sink(pcm), competing(pcm);
    expect_error(RetCode::ESTATE, [&] { source.bind(capture); });
    expect_error(RetCode::ESTATE, [&] { sink.bind(playback); });
    capture.prepare(period);
    playback.prepare(period);
    PushSource wrong({rate, 2, SampleFormat::F32LE});
    expect_error(RetCode::INVFMT, [&] { wrong.bind(capture); });
    source.bind(capture);
    sink.bind(playback);
    expect_error(RetCode::BUSY, [&] { other.bind(capture); });
    expect_error(RetCode::BUSY, [&] { competing.bind(playback); });
    expect_error(RetCode::BUSY, [&] { capture.prepare(BlockPeriod::INR_10MS); });
    expect_error(RetCode::BUSY, [&] { playback.prepare(BlockPeriod::INR_10MS); });
    source.start();
    sink.start();
    expect_error(RetCode::BUSY, [&] { source.start(); });
    const std::uint8_t sample[4]{0, 0, 0x80, 0x3e};
    check(source.push({sample, pcm, 1}) == 1 && QueueState::queued(source) == 1, "Capture queues external PCM");
    source.stop();
    source.start();
    check(QueueState::queued(source) == 0, "A restarted endpoint starts empty");
    source.unbind();
    sink.unbind();
    other.bind(capture);
    competing.bind(playback);
    check(other.bound() && competing.bound(), "Unbind releases each endpoint for another handle");
}

void device_worker_errors()
{
    check(std::atomic<RetCode>{RetCode{}}.is_lock_free(), "Backend worker status uses lock-free atomics");
    Capture capture(rate, 1, 5ms);
    capture.prepare(period);
    BackendCalls calls;
    TestBackend source(pcm, 240, &calls);
    source.start_result = RetCode::IOFAILED;
    source.bind(capture);
    expect_error(RetCode::IOFAILED, [&] { source.start(); });
    check(source.bound() && !source.running(), "A worker startup failure leaves a retryable binding");
    source.start_result = {};
    source.cadence = 1ms;
    source.empty_capture = true;
    source.start();
    wait_for([&] { return !source.running(); }, "The worker times out with empty capture batches");
    check(source.last_error() == RetCode::TIMEOUT && calls.starts == 2 && calls.stops == 1 &&
              calls.start_thread == calls.stop_thread && calls.stop_thread != std::this_thread::get_id(),
          "The I/O worker owns its lifecycle and reports the progress timeout");
    source.stop();
    source.block = true;
    source.start();
    check(source.wait_for_io(), "The restarted worker reaches the blocking I/O call");
    source.unbind();
    check(!source.bound() && !source.running() && source.last_error() == RetCode::OK && calls.cancels == 1,
          "Unbind cancels blocked I/O and joins the worker");
}

void wav_seek_and_padding()
{
    TempWave files("wav-seek");
    const auto path = files.path();
    const auto invalid = files.path("-invalid.wav");
    const PortSpec spec(rate, 1, SampleFormat::S24_3LE);
    const std::uint8_t original[]{1, 2, 3, 4, 5, 6, 7, 8, 9}, middle[]{10, 11, 12};
    WavFile file;
    throw_if_error(file.open_write(path, spec, WavFile::WriteMode::Exclusive));
    throw_if_error(file.write({original, spec, 3}));
    throw_if_error(file.seek(1));
    throw_if_error(file.write({middle, spec, 1}));
    check(file.tell() == 2 && file.frame_count() == 3, "Overwriting a frame keeps the existing tail");
    throw_if_error(file.close());
    {
        std::ifstream bytes(path, std::ios::binary | std::ios::ate);
        check(bytes.tellg() == std::streamoff(54), "The odd-sized payload has a RIFF pad byte");
        bytes.seekg(-1, std::ios::end);
        check(bytes.get() == 0, "The padding byte is zero");
    }
    throw_if_error(file.open_read(path));
    {
        std::ofstream bad(invalid, std::ios::binary);
        bad << "RIFF";
    }
    check(!file.open_read(invalid) && file.spec() == spec && file.frame_count() == 3,
          "An invalid file preserves the active reader");
    std::uint8_t readback[9]{};
    check(file.read({readback, spec, 3}).value_or_throw() == 3 && file.read({readback, spec, 1}).value_or_throw() == 0,
          "The reader returns the complete payload and then EOF");
    const std::uint8_t expected[]{1, 2, 3, 10, 11, 12, 7, 8, 9};
    check(std::equal(std::begin(readback), std::end(readback), std::begin(expected)), "Seeking updates exactly one frame");
}

class CrossingRate
{
  public:
    void add(float sample, bool measure)
    {
        check(sample > 0.125f && sample < 0.875f, "Clock correction keeps steady audio away from inserted silence");
        if (!measure)
            return;
        if (previous_ < 0.5f && sample >= 0.5f)
        {
            const auto position = frames_ - 1 + (0.5 - previous_) / (sample - previous_);
            if (crossings_++ == 0)
                first_ = position;
            last_ = position;
        }
        ++frames_;
        previous_ = sample;
    }

    double scale() const
    {
        check(crossings_ > 1, "The steady signal contains measurable zero crossings");
        return 16.0 * (crossings_ - 1) / (last_ - first_);
    }

  private:
    double first_ = 0, last_ = 0;
    std::size_t frames_ = 0, crossings_ = 0;
    float previous_ = 0.5f;
};

void sample_clock_drift()
{
    constexpr auto sample_rate = BandWidth::Narrow;
    constexpr auto graph_period = BlockPeriod::INR_02MS;
    constexpr std::size_t batch_frames = 128, block_frames = 16;
    const PortSpec native(sample_rate, 1, SampleFormat::F32LE), planar(sample_rate, 1, SampleFormat::F32P);
    Pcm wire_input(native, batch_frames), wire_output(native, batch_frames);
    float graph_samples[block_frames]{}, signal[block_frames]{};
    for (std::size_t i = 0; i < wire_input.frames; ++i)
    {
        const auto value = static_cast<float>(0.5 + 0.25 * std::sin(two_pi * (i % block_frames) / block_frames));
        wire_input.set(i, 0, value);
        signal[i % block_frames] = value;
    }
    const OBBlock input(reinterpret_cast<std::uint8_t *>(graph_samples), planar, block_frames);
    const IBBlock output(reinterpret_cast<std::uint8_t *>(signal), planar, block_frames);
    for (const auto ppm : {-300, 300})
    {
        Capture capture(sample_rate, 1, 16ms);
        Playback playback(sample_rate, 1, 16ms);
        capture.prepare(graph_period);
        playback.prepare(graph_period);
        TestBackend device_in(native, batch_frames), device_out(native, batch_frames);
        device_in.input = wire_input.input(0, batch_frames);
        device_out.output = wire_output.output(0, batch_frames);
        device_in.gated = device_out.gated = true;
        device_in.bind(capture);
        device_out.bind(playback);
        device_in.start();
        device_out.start();
        check(device_in.wait_for_io() && device_out.wait_for_io(), "Both external clocks reach their first batch");
        CrossingRate captured, played;
        const auto device_period = 0.016 / (1 + ppm * 1e-6);
        constexpr double jitter[]{-0.000375, 0.00075, 0.0, -0.00075, 0.000375};
        std::uint64_t tick = 0, packet = 0;
        for (;;)
        {
            const auto graph_time = (tick + 1) * 0.002 + jitter[tick % 5];
            const auto device_time = (packet + 1) * device_period + jitter[(packet + 2) % 5];
            if (std::min(graph_time, device_time) > 100)
                break;
            if (graph_time <= device_time)
            {
                capture.process({graph_period, tick, {}, {&input, 1}});
                playback.process({graph_period, tick++, {&output, 1}, {}});
                if (graph_time > 2)
                    for (const auto value : graph_samples)
                        captured.add(value, graph_time > 70);
            }
            else
            {
                check(device_in.run_batch() && device_out.run_batch(), "Each gated device completes its scheduled batch");
                ++packet;
                if (device_time > 2)
                    for (std::size_t frame = 0; frame < batch_frames; ++frame)
                        played.add(wire_output.sample(frame), device_time > 70);
            }
        }
        device_in.stop();
        device_out.stop();
        check(std::abs(captured.scale() - 1 - ppm * 1e-6) < 0.0002 &&
                  std::abs(played.scale() - 1 + ppm * 1e-6) < 0.0002 &&
                  device_in.last_error() == RetCode::OK && device_out.last_error() == RetCode::OK,
              "Both directions follow the device clock across jitter and signed drift");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"endpoint_ownership_and_restart", endpoint_ownership_and_restart},
                                                {"device_worker_errors", device_worker_errors},
                                                {"wav_seek_and_padding", wav_seek_and_padding},
                                                {"sample_clock_drift", sample_clock_drift}});
}
