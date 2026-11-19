#include "support/audio_fixture.h"
#include <limits>
namespace
{
using namespace aereborn;
using namespace aereborn::test;
void buffer_binding()
{
    Capture oversized(PortSpec(rate, std::numeric_limits<std::size_t>::max(), Format::S16LE));
    checked(oversized.prepare(period));
    expect_error(ErrorCode::TooLarge, oversized.bind({nullptr, 0}, 0));
    const PortSpec spec(SampleRate::Hz44100, 1, Format::S16LE);
    Engine engine(period);
    ManualClock clock(engine);
    const auto input = checked(engine.emplace_node<Capture>(spec)),
               output = checked(engine.emplace_node<Playback>(spec));
    auto *capture = checked(engine.node<Capture>(input));
    auto *playback = checked(engine.node<Playback>(output));
    checked(engine.connect({input}, {output}));
    std::vector<std::uint8_t> bytes(442, 0x31), packet(442, 0xcd);
    expect_error(ErrorCode::NotPrepared, capture->bind({bytes.data(), bytes.size()}, 0));
    checked(engine.prepare());
    expect_error(ErrorCode::InvalidArg, capture->bind({bytes.data(), 440}, 0));
    expect_error(ErrorCode::InvalidArg, playback->bind({nullptr, 442}, 0));
    checked(capture->bind({bytes.data(), 442}, 0));
    checked(playback->bind({packet.data(), 442}, 0));
    expect_error(ErrorCode::BufferFull, capture->bind({bytes.data(), 442}, 0));
    checked(clock.tick());
    check(packet == bytes, "Device nodes copy one matching block without conversion");
    const auto previous = packet;
    checked(clock.tick());
    check(packet == previous, "Unbound playback leaves external memory alone");
    checked(playback->bind({packet.data(), 442}, 2));
    checked(clock.tick());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t b) { return b == 0; }),
          "Missing capture is silence");
    checked(clock.reset());
    checked(capture->bind({bytes.data(), 442}, 0));
    checked(playback->bind({packet.data(), 442}, 0));
    checked(clock.tick());
    check(packet == bytes, "Reset releases old bindings and restarts the period pattern");
}
void future_binding()
{
    const PortSpec spec(rate, 1, Format::S16LE);
    Engine engine(period);
    ManualClock clock(engine);
    const auto source = checked(engine.emplace_node<Capture>(spec)),
               sink = checked(engine.emplace_node<PacketSink>(spec));
    checked(engine.connect({source}, {sink}));
    checked(engine.prepare());
    std::vector<std::uint8_t> bytes(480, 0x21);
    auto *capture = checked(engine.node<Capture>(source));
    checked(capture->bind({bytes.data(), bytes.size()}, 1));
    checked(clock.tick());
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->bytes()[0] == 0, "A future binding is not consumed early");
    checked(clock.tick());
    check(out->bytes()[0] == 0x21, "The bound tick selects the correct data");
}
void topology_format_switch()
{
    const PortSpec planar(rate, 1), old_pcm(rate, 1, Format::S16LE), new_pcm(rate, 1, Format::F32LE);
    Engine engine(period);
    ManualClock clock(engine);
    auto input = checked(engine.emplace_node<Capture>(old_pcm));
    auto convert = checked(engine.emplace_node<Convert>(old_pcm, planar));
    const auto sink = checked(engine.emplace_node<PacketSink>(planar));
    checked(engine.connect({input}, {convert}));
    checked(engine.connect({convert}, {sink}));
    checked(engine.prepare());
    std::vector<std::uint8_t> old_bytes(480);
    old_bytes[1] = 0x40;
    checked(checked(engine.node<Capture>(input))->bind({old_bytes.data(), old_bytes.size()}, 0));
    checked(clock.tick());
    auto *out = checked(engine.node<PacketSink>(sink));
    check(out->sample(0) == 0.5f, "Initial PCM device is decoded");
    expect_error(ErrorCode::FormatMismatch, engine.replace_node(input, std::make_unique<Capture>(new_pcm)));
    checked(engine.remove_node(input));
    checked(engine.remove_node(convert));
    input = checked(engine.emplace_node<Capture>(new_pcm));
    convert = checked(engine.emplace_node<Convert>(new_pcm, planar));
    checked(engine.connect({input}, {convert}));
    checked(engine.connect({convert}, {sink}));
    const auto revision = checked(engine.prepare());
    std::vector<std::uint8_t> new_bytes(960);
    write_f32le(new_bytes.data(), -0.25f);
    checked(checked(engine.node<Capture>(input))->bind({new_bytes.data(), new_bytes.size()}, 1));
    checked(clock.tick());
    check(
        engine.applied_revision() == revision && out->sample(0) == -0.25f && engine.tick_position() == 2,
        "An explicit format replacement switches the device and converter together without resetting downstream time");
}
} // namespace
int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"buffer_binding", buffer_binding},
                      {"future_binding", future_binding},
                      {"topology_format_switch", topology_format_switch}});
}
