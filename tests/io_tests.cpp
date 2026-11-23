#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void buffer_binding()
{
    Capture oversized(PortSpec(rate, std::numeric_limits<std::size_t>::max(), SampleFormat::S16LE));
    oversized.prepare(period);
    expect_error(ErrorCode::TooLarge, [&] { oversized.bind({nullptr, 0}, 0); });
    const PortSpec spec(SampleRate::Hz44100, 1, SampleFormat::S16LE);
    std::vector<std::uint8_t> bytes(442, 0x31), packet(442, 0xcd);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Capture>(spec),
               output = engine.emplace_node<Playback>(spec);
    auto *capture = engine.node<Capture>(input);
    auto *playback = engine.node<Playback>(output);
    engine.connect(OPORT(input, 0), IPORT(output, 0));
    expect_error(ErrorCode::NotReady, [&] { capture->bind({bytes.data(), bytes.size()}, 0); });
    engine.commit();
    expect_error(ErrorCode::InvalidArg, [&] { capture->bind({bytes.data(), 440}, 0); });
    expect_error(ErrorCode::InvalidArg, [&] { playback->bind({nullptr, 442}, 0); });
    capture->bind({bytes.data(), 442}, 0);
    playback->bind({packet.data(), 442}, 0);
    expect_error(ErrorCode::Busy, [&] { capture->bind({bytes.data(), 442}, 0); });
    clock.start();
    clock.advance();
    check(packet == bytes, "Device nodes copy one matching block without conversion");
    const auto previous = packet;
    clock.advance();
    check(packet == previous, "Unbound playback leaves external memory alone");
    playback->bind({packet.data(), 442}, 2);
    clock.advance();
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t b) { return b == 0; }),
          "Missing capture is silence");
    clock.stop();
    engine.reset();
    clock.start();
    capture->bind({bytes.data(), 442}, 0);
    playback->bind({packet.data(), 442}, 0);
    clock.advance();
    check(packet == bytes, "Reset releases old bindings and restarts the period pattern");
}
void future_binding()
{
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    std::vector<std::uint8_t> bytes(480, 0x21);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = engine.emplace_node<Capture>(spec),
               sink = engine.emplace_node<PacketSink>(spec);
    engine.connect(OPORT(source, 0), IPORT(sink, 0));
    engine.commit();
    auto *capture = engine.node<Capture>(source);
    capture->bind({bytes.data(), bytes.size()}, 1);
    clock.start();
    clock.advance();
    auto *out = engine.node<PacketSink>(sink);
    check(out->bytes()[0] == 0, "A future binding is not consumed early");
    clock.advance();
    check(out->bytes()[0] == 0x21, "The bound tick selects the correct data");
    capture->bind({bytes.data(), bytes.size()}, 1);
    clock.advance();
    check(out->bytes()[0] == 0, "An expired binding is discarded instead of copied");
    capture->bind({bytes.data(), bytes.size()}, engine.tick_position());
    clock.advance();
    check(out->bytes()[0] == 0x21, "Discarding a binding releases the slot for the next tick");
}
void binding_period_change()
{
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    Capture capture(spec);
    Playback playback(spec);
    std::uint8_t bytes[960]{};
    capture.prepare(period);
    playback.prepare(period);
    capture.bind({bytes, 480}, 0);
    playback.bind({bytes, 480}, 0);

    capture.prepare(period);
    playback.prepare(period);
    expect_error(ErrorCode::Busy, [&] { capture.bind({bytes, 480}, 0); });
    expect_error(ErrorCode::Busy, [&] { playback.bind({bytes, 480}, 0); });

    capture.prepare(BlockPeriod::Ms10);
    playback.prepare(BlockPeriod::Ms10);
    expect_error(ErrorCode::InvalidArg, [&] { capture.bind({bytes, 480}, 0); });
    expect_error(ErrorCode::InvalidArg, [&] { playback.bind({bytes, 480}, 0); });
    capture.bind({bytes, 960}, 0);
    playback.bind({bytes, 960}, 0);
}
void topology_format_switch()
{
    const PortSpec planar(rate, 1, SampleFormat::F32P);
    const PortSpec old_pcm(rate, 1, SampleFormat::S16LE), new_pcm(rate, 1, SampleFormat::F32LE);
    std::vector<std::uint8_t> old_bytes(480), new_bytes(960);
    Engine engine(period);
    ClockedGraph clock(engine);
    auto input = engine.emplace_node<Capture>(old_pcm);
    auto convert = engine.emplace_node<Convert>(old_pcm, planar);
    const auto sink = engine.emplace_node<PacketSink>(planar);
    engine.connect(OPORT(input, 0), IPORT(convert, 0));
    engine.connect(OPORT(convert, 0), IPORT(sink, 0));
    engine.commit();
    old_bytes[1] = 0x40;
    engine.node<Capture>(input)->bind({old_bytes.data(), old_bytes.size()}, 0);
    clock.start();
    clock.advance();
    auto *out = engine.node<PacketSink>(sink);
    check(out->sample(0) == 0.5f, "Initial PCM device is decoded");
    expect_error(ErrorCode::FormatMismatch, [&] { engine.replace_node(input, std::make_unique<Capture>(new_pcm)); });
    engine.remove_node(input);
    engine.remove_node(convert);
    input = engine.emplace_node<Capture>(new_pcm);
    convert = engine.emplace_node<Convert>(new_pcm, planar);
    engine.connect(OPORT(input, 0), IPORT(convert, 0));
    engine.connect(OPORT(convert, 0), IPORT(sink, 0));
    clock.commit();
    const auto tick = engine.tick_position();
    write_f32le(new_bytes.data(), -0.25f);
    engine.node<Capture>(input)->bind({new_bytes.data(), new_bytes.size()}, tick);
    clock.advance();
    check(
        out->sample(0) == -0.25f && engine.tick_position() == tick + 1,
        "An explicit format replacement switches the device and converter together without resetting downstream time");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"buffer_binding", buffer_binding},
                      {"future_binding", future_binding},
                      {"binding_period_change", binding_period_change},
                      {"topology_format_switch", topology_format_switch}});
}
