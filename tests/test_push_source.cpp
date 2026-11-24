#include "backend/null_backend.h"
#include "backend/push_source.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "support/audio_fixture.h"
#include "support/test_backend.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace aereborn;
using aereborn::test::check;
using aereborn::test::expect_error;
using aereborn::test::write_f32le;
using namespace std::chrono_literals;

constexpr auto rate = SampleRate::Hz48000;
constexpr auto graph_period = BlockPeriod::Ms05;
constexpr std::size_t graph_frames = 240;

template <class T, class = void> struct CanBindPlayback : std::false_type
{
};

template <class T>
struct CanBindPlayback<T, decltype(void(std::declval<T &>().bind(std::declval<Playback &>())))> : std::true_type
{
};

static_assert(!CanBindPlayback<PushSource>::value, "PushSource only binds to capture");

void render(Capture &capture, OFBlock output, std::uint64_t tick = 0) noexcept
{
    const OBBlock block(reinterpret_cast<std::uint8_t *>(output.channel(0)), capture.spec(), output.frames());
    capture.process({graph_period, tick, {}, {&block, 1}});
}

void lifecycle()
{
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    const PortSpec wrong_format(rate, 1, SampleFormat::F32LE);
    const std::array<std::uint8_t, 2> pcm{{0, 64}};
    const IBBlock pulse(pcm.data(), spec, 1);
    Capture capture(rate, 1, 20ms);
    PushSource source(spec), other(spec);

    expect_error(ErrorCode::InvalidArg, [] { PushSource invalid(PortSpec{}); });
    expect_error(ErrorCode::InvalidArg, [] { PushSource planar({rate, 1, SampleFormat::F32P}); });
    expect_error(ErrorCode::NotReady, [&] { source.start(); });
    expect_error(ErrorCode::NotReady, [&] { source.bind(capture); });
    check(source.push(pulse) == 0, "An unbound source accepts no frames");
    expect_error(ErrorCode::FormatMismatch, [&] { source.push({nullptr, wrong_format, 0}); });
    expect_error(ErrorCode::InvalidArg, [&] { source.push({nullptr, spec, 1}); });

    capture.prepare(graph_period);
    source.bind(capture);
    expect_error(ErrorCode::Busy, [&] { other.bind(capture); });
    check(source.device_spec() == spec && source.device_max_frames() == 480 && source.push(pulse) == 0,
          "Binding records the format and caps the internal batch at 10 ms");
    source.start();
    expect_error(ErrorCode::Busy, [&] { source.start(); });
    check(source.push({nullptr, spec, 0}) == 0 && source.push(pulse) == 1,
          "A matching empty block is accepted and one frame can be queued");
    source.stop();
    source.stop();
    check(source.bound() && !source.running() && source.push(pulse) == 0, "Stop retains the binding");
    source.start();

    std::array<float, graph_frames> samples{};
    const OFBlock output(samples.data(), 1, samples.size(), samples.size());
    render(capture, output);
    check(samples.front() == 0, "Restart does not play frames queued before stop");

    std::uint64_t tick = 0;
    for (const auto gap : {1, 16, 3, 7, 200})
    {
        check(source.push(pulse) == 1, "Push resumes without waiting for prefill");
        render(capture, output, tick++);
        check(samples.front() == 0.5f && std::all_of(samples.begin() + 1, samples.end(), [](float v) { return v == 0; }),
              "A short push plays immediately and the graph fills missing frames with silence");
        for (int i = 0; i < gap; ++i)
        {
            render(capture, output, tick++);
            check(std::all_of(samples.begin(), samples.end(), [](float v) { return v == 0; }), "Empty queue is silent");
        }
        check(source.running(), "An input gap leaves the source running");
    }

    source.unbind();
    check(!source.bound() && !source.running() && source.push(pulse) == 0, "A running source can be unbound");
    other.bind(capture);
    other.start();
    check(other.push(pulse) == 1, "Unbind releases the endpoint for another source");
}

void endpoint_ownership()
{
    Capture capture(rate, 1, 20ms);
    Capture stereo(rate, 2, 20ms);
    Capture short_period(rate, 1, 80us);
    capture.prepare(graph_period);
    stereo.prepare(graph_period);
    short_period.prepare(graph_period);
    PushSource source({rate, 1, SampleFormat::S16LE});
    PushSource slower({SampleRate::Hz8000, 1, SampleFormat::S16LE});
    NullBackend device(SampleFormat::S16LE);

    source.bind(capture);
    expect_error(ErrorCode::Busy, [&] { device.bind(capture); });
    source.unbind();
    device.bind(capture);
    expect_error(ErrorCode::Busy, [&] { source.bind(capture); });
    device.unbind();

    expect_error(ErrorCode::FormatMismatch, [&] { source.bind(stereo); });
    check(!source.bound() && !source.device_max_frames() && source.device_spec() == PortSpec{},
          "A failed format check clears the negotiated state");
    device.bind(stereo);
    device.unbind();

    expect_error(ErrorCode::InvalidArg, [&] { slower.bind(short_period); });
    device.bind(short_period);
}

void stream(PortSpec spec)
{
    constexpr std::size_t channels = 2;
    const std::array<std::size_t, 4> packet_frames{{1, 13, 1470, 12000}};
    const auto total_frames = packet_frames[0] + packet_frames[1] + packet_frames[2] + packet_frames[3];
    const auto sample_width = sample_bytes(spec.format);
    const auto frame_width = channels * sample_width;
    std::vector<std::uint8_t> bytes(total_frames * frame_width);
    Buffer source;
    source.resize(channels, total_frames);
    const auto input = source.block(total_frames);
    for (std::size_t frame = 0; frame < total_frames; ++frame)
    {
        for (std::size_t ch = 0; ch < channels; ++ch)
        {
            const auto sample = static_cast<int>((frame * 67 + ch * 173) % 1009) - 504;
            input.channel(ch)[frame] = sample / 32768.0f;
            auto *dest = bytes.data() + frame * frame_width + ch * sample_width;
            if (spec.format == SampleFormat::F32LE)
                write_f32le(dest, input.channel(ch)[frame]);
            else
            {
                const auto bits = static_cast<std::uint16_t>(sample);
                dest[0] = static_cast<std::uint8_t>(bits);
                dest[1] = static_cast<std::uint8_t>(bits >> 8);
            }
        }
    }

    // Compare streaming and backpressure with the same PCM processed in one contiguous block.
    const auto fir = spec.sample_rate != rate;
    IFBlock expected = input;
    Buffer reference;
    if (fir)
    {
        SincResampler resampler;
        resampler.prepare(channels, spec.sample_rate, rate);
        const auto bound = SincResampler::max_output_frames(spec.sample_rate, rate, total_frames);
        reference.resize(channels, bound);
        const auto result = resampler.process(input, reference.block(bound));
        check(result.consumed == total_frames, "Reference consumed the complete stream");
        expected = reference.block(result.produced);
    }

    Capture capture(rate, channels, 20ms);
    capture.prepare(graph_period);
    PushSource backend(spec);
    backend.bind(capture);
    backend.start();
    Buffer output;
    output.resize(channels, graph_frames);
    std::size_t rendered = 0;
    const auto verify_block = [&] {
        const auto block = output.block(graph_frames);
        render(capture, block, rendered / graph_frames);
        for (std::size_t ch = 0; ch < channels; ++ch)
            for (std::size_t i = 0; i < graph_frames; ++i)
            {
                const auto frame = rendered + i;
                const auto sample = frame < expected.frames() ? expected.channel(ch)[frame] : 0.0f;
                check(block.channel(ch)[i] == sample, "The graph receives each accepted sample in order");
            }
        rendered += graph_frames;
    };

    bool short_write = false;
    std::size_t accepted = 0;
    for (auto remaining : packet_frames)
    {
        while (remaining)
        {
            const auto count = backend.push({bytes.data() + accepted * frame_width, spec, remaining});
            check(count <= remaining, "Push returns an input frame count");
            accepted += count;
            remaining -= count;
            if (remaining)
            {
                short_write = true;
                check(backend.push({bytes.data() + accepted * frame_width, spec, remaining}) == 0,
                      "A retry without graph reads still meets backpressure");
                verify_block();
                check(rendered < expected.frames(), "Backpressure clears as the graph reads");
            }
        }
    }
    check(short_write && accepted == total_frames, "Oversized packets make progress through short writes");
    while (rendered < expected.frames())
        verify_block();
    verify_block();

    if (fir)
    {
        backend.stop();
        backend.start();
        const auto frames = backend.device_max_frames();
        std::fill_n(bytes.data(), frames * frame_width, std::uint8_t{0});
        check(backend.push({bytes.data(), spec, frames}) == frames, "Restart accepts a fresh stream");
        const auto block = output.block(graph_frames);
        render(capture, block);
        for (std::size_t ch = 0; ch < channels; ++ch)
            check(std::all_of(block.channel(ch), block.channel(ch) + graph_frames, [](float v) { return v == 0; }),
                  "Restart clears FIR history from the previous stream");
    }
}

void same_rate_stream()
{
    stream({rate, 2, SampleFormat::S16LE});
    stream({rate, 2, SampleFormat::F32LE});
}

void resampled_stream()
{
    stream({SampleRate::Hz16000, 2, SampleFormat::S16LE});
    stream({SampleRate::Hz44100, 2, SampleFormat::S16LE});
    stream({SampleRate::Hz96000, 2, SampleFormat::S16LE});
}

void mix_and_isolation()
{
    const PortSpec mono(SampleRate::Hz16000, 1, SampleFormat::S16LE);
    const PortSpec stereo(rate, 2, SampleFormat::F32LE);
    const PortSpec bus(rate, 2, SampleFormat::F32P);
    Engine engine(graph_period);
    test::ClockedGraph clock(engine);
    const auto voice = engine.emplace_node<Capture>(rate, 1, 20ms);
    const auto network = engine.emplace_node<Capture>(rate, 2, 20ms);
    const auto device = engine.emplace_node<Capture>(rate, 2, 20ms);
    const auto upmix = engine.emplace_node<Matrix>(rate, 1, 2, std::initializer_list<float>{1.0f, 1.0f});
    const auto mixer = engine.emplace_node<Mixer>(rate, 2, 3);
    const auto sink = engine.emplace_node<test::PacketSink>(bus);
    engine.connect(OPORT(voice, 0), IPORT(upmix, 0));
    engine.connect(OPORT(upmix, 0), IPORT(mixer, 0));
    engine.connect(OPORT(network, 0), IPORT(mixer, 1));
    engine.connect(OPORT(device, 0), IPORT(mixer, 2));
    engine.connect(OPORT(mixer, 0), IPORT(sink, 0));
    engine.commit();

    constexpr std::size_t mono_frames = 80;
    std::array<std::uint8_t, mono_frames * 2> voice_bytes{};
    std::array<std::uint8_t, graph_frames * 8> network_bytes{}, device_bytes{};
    for (std::size_t i = 0; i < mono_frames; ++i)
        voice_bytes[2 * i + 1] = 32;
    for (std::size_t i = 0; i < graph_frames; ++i)
    {
        write_f32le(network_bytes.data() + i * 8, 0.125f);
        write_f32le(network_bytes.data() + i * 8 + 4, -0.125f);
        write_f32le(device_bytes.data() + i * 8, 0.25f);
        write_f32le(device_bytes.data() + i * 8 + 4, 0.125f);
    }

    PushSource tts(mono), net(stereo);
    test::TestBackend hardware(stereo, graph_frames, nullptr, false);
    hardware.gated = true;
    hardware.input = {device_bytes.data(), stereo, graph_frames};
    tts.bind(*engine.node<Capture>(voice));
    net.bind(*engine.node<Capture>(network));
    hardware.bind(*engine.node<Capture>(device));
    tts.start();
    net.start();
    hardware.start();
    check(hardware.wait_for_io(), "The device worker starts");
    clock.start();

    auto *output = engine.node<test::PacketSink>(sink);
    const auto mix_block = [&](bool voice_active) {
        if (voice_active)
            check(tts.push({voice_bytes.data(), mono, mono_frames}) == mono_frames, "Voice packet is accepted");
        check(net.push({network_bytes.data(), stereo, graph_frames}) == graph_frames, "Network packet is accepted");
        check(hardware.run_batch(), "The device supplies its next batch");
        clock.advance();
        check(output->frames == graph_frames, "The graph renders a complete mixer block");
    };

    mix_block(true);
    mix_block(true);
    // The FIR's steady-state gain varies slightly with interpolation phase.
    constexpr float tolerance = 0.0001f;
    for (std::size_t i = 0; i < graph_frames; ++i)
        check(std::abs(output->sample(i, 0) - 0.625f) < tolerance &&
                  std::abs(output->sample(i, 1) - 0.25f) < tolerance,
              "Resampled voice, network PCM and the device mix with the correct channel mapping");
    tts.stop();
    mix_block(false);
    for (std::size_t i = 0; i < graph_frames; ++i)
        check(output->sample(i, 0) == 0.375f && output->sample(i, 1) == 0,
              "Stopping voice leaves the other mixer inputs intact");
}
} // namespace

int main(int argc, char **argv)
{
    return test::run_tests(argc, argv,
                           {{"lifecycle", lifecycle}, {"endpoint_ownership", endpoint_ownership},
                            {"same_rate_stream", same_rate_stream}, {"resampled_stream", resampled_stream},
                            {"mix_and_isolation", mix_and_isolation}});
}
