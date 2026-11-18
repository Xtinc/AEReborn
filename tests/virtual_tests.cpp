#include "checked.h"
#include "pcm.h"
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

void formats()
{
    const struct
    {
        PCMFormat format;
        std::vector<std::uint8_t> half;
    } cases[] = {
        {PCMFormat::S16LE, {0, 0x40}}, {PCMFormat::S24_3LE, {0, 0, 0x40}},
        {PCMFormat::S24LE, {0, 0, 0x40, 0}}, {PCMFormat::S32LE, {0, 0, 0, 0x40}},
        {PCMFormat::F32LE, {0, 0, 0, 0x3f}}
    };
    for (const auto &test : cases)
    {
        const PCMSpec spec{rate, test.format, 2};
        VirtualSource source(spec, period, 12000, 0.5f);
        VirtualSink sink(spec, period);
        expect_error(ErrorCode::NotPrepared, source.generate());
        expect_error(ErrorCode::NotPrepared, sink.receive({nullptr, 0}));
        checked(source.prepare());
        checked(sink.prepare());
        check(source.pcm().empty() && sink.pcm().empty(), "Preparation must not publish generated or received data");
        checked(source.generate());
        const auto pcm = source.pcm();
        const auto width = test.half.size();
        check(source.pcm_frames() == 240 && pcm.size() == 480 * width, "Virtual source generates a complete stereo packet");
        auto negative = test.half;
        negative[pcm_sample_bits(test.format) / 8 - 1] |= 0x80;
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            check(std::equal(test.half.begin(), test.half.end(), pcm.begin() + (2 + ch) * width) &&
                      std::equal(negative.begin(), negative.end(), pcm.begin() + (6 + ch) * width),
                  "Known positive and negative samples have the requested wire representation");
        }
        const std::vector<std::uint8_t> first(pcm.begin(), pcm.end());
        auto received = first;
        checked(sink.receive({received.data(), received.size()}));
        received[0] ^= 1;
        check(sink.matches(pcm), "Virtual sink owns a copy of received PCM");
        expect_error(ErrorCode::InvalidArg, sink.receive({received.data(), received.size() - 1}));
        check(sink.matches(pcm), "Rejected receive preserves the previous packet");
        checked(sink.receive({received.data(), received.size()}));
        check(!sink.matches(pcm), "Virtual sink detects changed bytes");
        source.reset();
        sink.reset();
        check(source.pcm().empty() && sink.pcm().empty(), "Reset clears availability without reallocating buffers");
        checked(source.generate());
        check(std::equal(first.begin(), first.end(), source.pcm().begin()), "Source reset restarts its own timeline");
    }
}

void timeline()
{
    VirtualSource source({rate, PCMFormat::F32LE, 2, 2}, period, 997, 0.25f);
    checked(source.prepare());
    for (std::size_t transfer = 0; transfer < 4; ++transfer)
    {
        checked(source.prepare()); // Repeated preparation must not reset oscillator state.
        checked(source.generate());
        const auto pcm = source.pcm();
        for (std::size_t frame = 0; frame < 480; ++frame)
        {
            const auto expected = 0.25 * std::sin(6.283185307179586 * 997 * (transfer * 480 + frame) / 48000);
            check(std::abs(read_f32le(pcm.data() + frame * 8) - expected) < 1e-6 &&
                      read_f32le(pcm.data() + frame * 8) == read_f32le(pcm.data() + frame * 8 + 4),
                  "Virtual source has a continuous timeline independent of graph rendering");
        }
    }
}

void settings()
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
        {"formats", formats}, {"timeline", timeline}, {"settings", settings}
    });
}
