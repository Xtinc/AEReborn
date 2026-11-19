#include "support/test_harness.h"
#include "support/pcm_reference.h"
#include "plugins/virtual.h"
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;

void source_wire_formats()
{
    const struct
    {
        PCMFormat format;
        std::vector<std::uint8_t> positive;
        std::vector<std::uint8_t> negative;
    } cases[] = {
        {PCMFormat::S16LE, {0, 0x40}, {0, 0xc0}},
        {PCMFormat::S24_3LE, {0, 0, 0x40}, {0, 0, 0xc0}},
        {PCMFormat::S24LE, {0, 0, 0x40, 0}, {0, 0, 0xc0, 0}},
        {PCMFormat::S32LE, {0, 0, 0, 0x40}, {0, 0, 0, 0xc0}},
        {PCMFormat::F32LE, {0, 0, 0, 0x3f}, {0, 0, 0, 0xbf}},
    };
    for (const auto &test : cases)
    {
        const PCMSpec spec{rate, test.format, 2};
        VirtualSource source(spec, period, 12000, 0.5f);
        checked(source.prepare());
        checked(source.generate());
        const auto pcm = source.pcm();
        const auto width = test.positive.size();
        check(source.pcm_frames() == 240 && pcm.size() == 480 * width, "Virtual source generates a complete stereo packet");
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            check(std::equal(test.positive.begin(), test.positive.end(), pcm.begin() + (2 + ch) * width) &&
                      std::equal(test.negative.begin(), test.negative.end(), pcm.begin() + (6 + ch) * width),
                  "Known positive and negative samples have the requested wire representation");
        }
    }
}

void sink_packet_ownership()
{
    VirtualSink sink({rate, PCMFormat::S16LE, 1}, period);
    std::array<std::uint8_t, 240 * 2> packet{};
    packet[0] = 0x40;
    const auto original = packet;
    const View<const std::uint8_t> expected{original.data(), original.size()};
    expect_error(ErrorCode::NotPrepared, sink.receive({packet.data(), packet.size()}));
    checked(sink.prepare());
    check(sink.pcm().empty(), "Preparing the sink does not publish a packet");
    checked(sink.receive({packet.data(), packet.size()}));
    packet[0] ^= 1;
    check(sink.matches(expected), "The sink owns a copy of received PCM");
    expect_error(ErrorCode::InvalidArg, sink.receive({packet.data(), packet.size() - 1}));
    check(sink.matches(expected), "A rejected receive preserves the previous packet");
    const View<const std::uint8_t> replacement{packet.data(), packet.size()};
    checked(sink.receive(replacement));
    check(sink.matches(replacement) && !sink.matches(expected), "The sink compares the latest packet byte for byte");
    sink.reset();
    check(sink.pcm().empty(), "Reset clears the received packet");
    checked(sink.receive(replacement));
}

void source_timeline()
{
    VirtualSource source({rate, PCMFormat::F32LE, 2, 2}, period, 997, 0.25f);
    expect_error(ErrorCode::NotPrepared, source.generate());
    checked(source.prepare());
    check(source.pcm().empty(), "Preparing the source does not publish a packet");
    std::vector<std::uint8_t> first;
    for (std::size_t transfer = 0; transfer < 4; ++transfer)
    {
        checked(source.prepare()); // Repeated preparation must not reset oscillator state.
        checked(source.generate());
        const auto pcm = source.pcm();
        check(pcm.size() == 480 * 8, "A stereo float packet covers two full periods");
        if (transfer == 0)
        {
            first.assign(pcm.begin(), pcm.end());
        }
        for (std::size_t frame = 0; frame < 480; ++frame)
        {
            const auto expected = 0.25 * std::sin(6.283185307179586 * 997 * (transfer * 480 + frame) / 48000);
            check(std::abs(read_f32le(pcm.data() + frame * 8) - expected) < 1e-6 &&
                      read_f32le(pcm.data() + frame * 8) == read_f32le(pcm.data() + frame * 8 + 4),
                  "Virtual source has a continuous timeline independent of graph rendering");
        }
    }
    source.reset();
    check(source.pcm().empty(), "Reset clears the generated packet");
    checked(source.generate());
    const auto restarted = source.pcm();
    check(restarted.size() == first.size() && std::equal(first.begin(), first.end(), restarted.begin()),
          "Source reset restarts its own timeline");
}

void configuration_validation()
{
    VirtualSource fractional({SampleRate::Hz44100, PCMFormat::S16LE, 1}, period);
    expect_error(ErrorCode::InvalidArg, fractional.prepare());
    VirtualSource complete({SampleRate::Hz44100, PCMFormat::S16LE, 1, 2}, period);
    checked(complete.prepare());
    check(complete.pcm_frames() == 441, "A complete 10 ms packet contains integer 44.1 kHz frames");
    VirtualSource bad_frequency({rate, PCMFormat::F32LE, 1}, period, 24000);
    VirtualSource bad_amplitude({rate, PCMFormat::F32LE, 1}, period, 440, std::numeric_limits<float>::quiet_NaN());
    expect_error(ErrorCode::InvalidArg, bad_frequency.prepare());
    expect_error(ErrorCode::InvalidArg, bad_amplitude.prepare());
    const PCMSpec invalid[] = {
        {rate, static_cast<PCMFormat>(99), 1}, {rate, PCMFormat::S16LE, 0},
        {static_cast<SampleRate>(0), PCMFormat::S16LE, 1}, {rate, PCMFormat::S16LE, 1, 0}
    };
    for (const auto spec : invalid)
    {
        VirtualSink sink(spec, period);
        expect_error(ErrorCode::InvalidArg, sink.prepare());
    }
    VirtualSink bad_period({rate, PCMFormat::S16LE, 1}, static_cast<BlockPeriod>(0));
    expect_error(ErrorCode::InvalidArg, bad_period.prepare());
    VirtualSink oversized({rate, PCMFormat::F32LE, std::numeric_limits<std::size_t>::max()}, period);
    expect_error(ErrorCode::TooLarge, oversized.prepare());
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"source_wire_formats", source_wire_formats},
        {"source_timeline", source_timeline},
        {"sink_packet_ownership", sink_packet_ownership},
        {"configuration_validation", configuration_validation}
    });
}
