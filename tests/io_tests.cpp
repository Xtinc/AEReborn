#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void buffer_binding()
{
    Capture oversized(PortSpec(rate, std::numeric_limits<std::size_t>::max(), SampleFormat::S16LE));
    checked(oversized.prepare(period));
    expect_error(ErrorCode::TooLarge, oversized.bind({nullptr, 0}, 0));
    const PortSpec spec(SampleRate::Hz44100, 1, SampleFormat::S16LE);
    std::vector<std::uint8_t> bytes(442, 0x31), packet(442, 0xcd);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = checked(engine.emplace_node<Capture>(spec)),
               output = checked(engine.emplace_node<Playback>(spec));
    auto *capture = checked(engine.node<Capture>(input));
    auto *playback = checked(engine.node<Playback>(output));
    checked(engine.connect({input}, {output}));
    expect_error(ErrorCode::NotPrepared, capture->bind({bytes.data(), bytes.size()}, 0));
    checked(engine.commit());
    expect_error(ErrorCode::InvalidArg, capture->bind({bytes.data(), 440}, 0));
    expect_error(ErrorCode::InvalidArg, playback->bind({nullptr, 442}, 0));
    checked(capture->bind({bytes.data(), 442}, 0));
    checked(playback->bind({packet.data(), 442}, 0));
    expect_error(ErrorCode::BufferFull, capture->bind({bytes.data(), 442}, 0));
    clock.start();
    clock.advance();
    check(packet == bytes, "Device nodes copy one matching block without conversion");
    const auto previous = packet;
    clock.advance();
    check(packet == previous, "Unbound playback leaves external memory alone");
    checked(playback->bind({packet.data(), 442}, 2));
    clock.advance();
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t b) { return b == 0; }),
          "Missing capture is silence");
    clock.stop();
    checked(engine.reset());
    clock.start();
    checked(capture->bind({bytes.data(), 442}, 0));
    checked(playback->bind({packet.data(), 442}, 0));
    clock.advance();
    check(packet == bytes, "Reset releases old bindings and restarts the period pattern");
}
void future_binding()
{
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    std::vector<std::uint8_t> bytes(480, 0x21);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto source = checked(engine.emplace_node<Capture>(spec)),
               sink = checked(engine.emplace_node<PacketSink>(spec));
    checked(engine.connect({source}, {sink}));
    checked(engine.commit());
    auto *capture = checked(engine.node<Capture>(source));
    checked(capture->bind({bytes.data(), bytes.size()}, 1));
    clock.start();
    clock.advance();
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->bytes()[0] == 0, "A future binding is not consumed early");
    clock.advance();
    check(out->bytes()[0] == 0x21, "The bound tick selects the correct data");
    checked(capture->bind({bytes.data(), bytes.size()}, 1));
    clock.advance();
    check(out->bytes()[0] == 0, "An expired binding is discarded instead of copied");
    checked(capture->bind({bytes.data(), bytes.size()}, engine.tick_position()));
    clock.advance();
    check(out->bytes()[0] == 0x21, "Discarding a binding releases the slot for the next tick");
}
void binding_period_change()
{
    const PortSpec spec(rate, 1, SampleFormat::S16LE);
    Capture capture(spec);
    Playback playback(spec);
    std::uint8_t bytes[960]{};
    checked(capture.prepare(period));
    checked(playback.prepare(period));
    checked(capture.bind({bytes, 480}, 0));
    checked(playback.bind({bytes, 480}, 0));

    checked(capture.prepare(period));
    checked(playback.prepare(period));
    expect_error(ErrorCode::BufferFull, capture.bind({bytes, 480}, 0));
    expect_error(ErrorCode::BufferFull, playback.bind({bytes, 480}, 0));

    checked(capture.prepare(BlockPeriod::Ms10));
    checked(playback.prepare(BlockPeriod::Ms10));
    expect_error(ErrorCode::InvalidArg, capture.bind({bytes, 480}, 0));
    expect_error(ErrorCode::InvalidArg, playback.bind({bytes, 480}, 0));
    checked(capture.bind({bytes, 960}, 0));
    checked(playback.bind({bytes, 960}, 0));
}
void topology_format_switch()
{
    const PortSpec planar(rate, 1, SampleFormat::F32P);
    const PortSpec old_pcm(rate, 1, SampleFormat::S16LE), new_pcm(rate, 1, SampleFormat::F32LE);
    std::vector<std::uint8_t> old_bytes(480), new_bytes(960);
    Engine engine(period);
    ClockedGraph clock(engine);
    auto input = checked(engine.emplace_node<Capture>(old_pcm));
    auto convert = checked(engine.emplace_node<Convert>(old_pcm, planar));
    const auto sink = checked(engine.emplace_node<PacketSink>(planar));
    checked(engine.connect({input}, {convert}));
    checked(engine.connect({convert}, {sink}));
    checked(engine.commit());
    old_bytes[1] = 0x40;
    checked(checked(engine.node<Capture>(input))->bind({old_bytes.data(), old_bytes.size()}, 0));
    clock.start();
    clock.advance();
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0) == 0.5f, "Initial PCM device is decoded");
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(input, std::make_unique<Capture>(new_pcm)));
    checked(engine.remove_node(input));
    checked(engine.remove_node(convert));
    input = checked(engine.emplace_node<Capture>(new_pcm));
    convert = checked(engine.emplace_node<Convert>(new_pcm, planar));
    checked(engine.connect({input}, {convert}));
    checked(engine.connect({convert}, {sink}));
    clock.commit();
    const auto tick = engine.tick_position();
    write_f32le(new_bytes.data(), -0.25f);
    checked(checked(engine.node<Capture>(input))->bind({new_bytes.data(), new_bytes.size()}, tick));
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
