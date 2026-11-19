#include "support/manual_clock.h"
#include "support/test_harness.h"
#include "support/pcm_reference.h"
#include "core/engine.h"
#include "core/observer.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
constexpr std::size_t block_frames = 240;
constexpr double two_pi = 6.283185307179586;

class Observations
{
  public:
    struct Packet
    {
        PortSpec format;
        std::uint64_t frame;
        std::uint64_t dropped;
        float first_sample;
        std::vector<std::uint8_t> pcm;
    };

    void add(const Observation &item)
    {
        Packet packet{item.port, item.frame_position, item.dropped_frames,
                      item.audio.channels() == 0 ? 0.0f : item.audio.channel(0)[0], {}};
        if (!item.pcm.empty())
        {
            packet.pcm.assign(item.pcm.begin(), item.pcm.end());
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        packets_.push_back(std::move(packet));
        ready_.notify_one();
    }

    Packet get(std::size_t index)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        check(ready_.wait_for(lock, std::chrono::seconds(2), [&] { return packets_.size() > index; }),
              "Timed out waiting for a port observation");
        return packets_[index];
    }

  private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Packet> packets_;
};

void adapter_port_observation()
{
    Observations capture_pcm, capture_audio, playback_audio, playback_pcm;
    std::promise<void> old_callback_started, release_old_callback;
    auto old_callback = old_callback_started.get_future();
    auto release = release_old_callback.get_future();
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec pcm{rate, PCMFormat::S16LE, 1};
    const auto input_id = checked(engine.emplace_node<IAdapter>(1, pcm));
    const auto output_id = checked(engine.emplace_node<OAdapter>(1, pcm));
    check(checked(engine.port_spec(IPort{input_id})).is_pcm() &&
              !checked(engine.port_spec(OPort{input_id})).is_pcm() &&
              !checked(engine.port_spec(IPort{output_id})).is_pcm() &&
              checked(engine.port_spec(OPort{output_id})).is_pcm(),
          "Each adapter exposes one input and one output with the actual data layout");
    expect_error(ErrorCode::InvalidPort, engine.connect({output_id}, {input_id}));
    checked(engine.connect({input_id}, {output_id}, {0.5f, false, {}}));
    const auto add_observer = [&](ObservationPort port, Observations &packets) {
        auto observer = std::make_unique<Observer>(port, [&packets](const Observation &packet) { packets.add(packet); });
        checked(engine.add_observer(std::move(observer)));
    };
    add_observer(IPort{input_id}, capture_pcm);
    add_observer(OPort{input_id}, capture_audio);
    add_observer(IPort{output_id}, playback_audio);
    auto playback_observer = std::make_unique<Observer>(OPort{output_id}, [&](const Observation &packet) {
        if (packet.frame_position == block_frames)
        {
            old_callback_started.set_value();
            check(release.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                  "Timed out waiting for the replacement plan");
        }
        playback_pcm.add(packet);
    });
    const auto pcm_observer = checked(engine.add_observer(std::move(playback_observer)));
    checked(engine.prepare());
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    std::vector<std::uint8_t> source(input->pcm_bytes());
    source[1] = 0x40;
    std::vector<std::uint8_t> device(output->pcm_bytes());
    checked(input->bind_pcm({source.data(), source.size()}));
    checked(output->bind_pcm({device.data(), device.size()}));
    checked(clock.tick());
    check(capture_pcm.get(0).pcm == source && capture_audio.get(0).first_sample == 0.5f &&
              playback_audio.get(0).first_sample == 0.25f && playback_pcm.get(0).pcm == device && device[1] == 0x20,
          "One subscription interface observes both sides and sees the same PCM written to the device");

    checked(clock.tick()); // An unbound capture skips PCM. Playback produces PCM without a device.
    check(old_callback.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              capture_audio.get(1).first_sample == 0 && playback_audio.get(1).first_sample == 0,
          "Playback remains observable without a bound device");

    const PCMSpec larger{rate, PCMFormat::F32LE, 1};
    checked(engine.replace_node(output_id, std::make_unique<OAdapter>(1, larger)));
    expect_error(ErrorCode::Busy, engine.prepare());
    checked(engine.set_paused(output_id, true));
    const auto paused = checked(engine.prepare());
    check(engine.applied_revision() != paused, "The pause is submitted at a block boundary");
    checked(input->bind_pcm({source.data(), source.size()}));
    checked(clock.tick());
    const auto missed = capture_pcm.get(1);
    check(engine.applied_revision() == paused && missed.frame == 2 * block_frames &&
              missed.dropped == block_frames && missed.pcm == source &&
              capture_audio.get(2).first_sample == 0.5f,
          "The capture branch and its callbacks continue while playback is paused");

    const PCMSpec invalid{rate, PCMFormat::F32LE, 1, 0};
    checked(engine.replace_node(output_id, std::make_unique<OAdapter>(1, invalid)));
    checked(engine.set_paused(output_id, false));
    expect_error(ErrorCode::InvalidArg, engine.prepare());
    check(engine.applied_revision() == paused, "Preparation failure leaves the affected output paused");
    checked(engine.replace_node(output_id, std::make_unique<OAdapter>(1, larger)));
    std::promise<void> preparing;
    auto preparing_next = preparing.get_future();
    auto resumed = std::async(std::launch::async, [&] {
        preparing.set_value();
        return engine.prepare();
    });
    preparing_next.wait();
    const auto waiting = resumed.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
    checked(input->bind_pcm({source.data(), source.size()}));
    checked(clock.tick());
    check(capture_audio.get(3).frame == 3 * block_frames, "Waiting for playback leaves other callbacks schedulable");
    release_old_callback.set_value();
    const auto revision = checked(resumed.get());
    const auto old = playback_pcm.get(1);
    check(waiting && old.frame == block_frames && old.format.spec.format == PCMFormat::S16LE &&
              old.pcm == std::vector<std::uint8_t>(device.size()),
          "Preparing the resumed branch waits for the old callback to finish reading its PCM");

    checked(input->bind_pcm({source.data(), source.size()}));
    checked(clock.tick());
    const auto converted = playback_pcm.get(2);
    check(engine.applied_revision() == revision && converted.frame == 4 * block_frames &&
              converted.dropped == 2 * block_frames && converted.format.spec.format == PCMFormat::F32LE &&
              converted.pcm.size() == block_frames * 4 && read_f32le(converted.pcm.data()) == 0.25f &&
              playback_audio.get(2).first_sample == 0.25f,
          "The same subscriptions resume with the new PCM format and the previous graph gain");
    checked(engine.remove_observer(pcm_observer));
}

void observer_pcm_batching()
{
    Observations observed;
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec capture_spec{rate, PCMFormat::F32LE, 1};
    const PCMSpec playback_spec{SampleRate::Hz44100, PCMFormat::S16LE, 1, 2};
    const auto input_id = checked(engine.emplace_node<IAdapter>(1, capture_spec));
    const auto output_id = checked(engine.emplace_node<OAdapter>(1, playback_spec));
    checked(engine.connect({input_id}, {output_id}));
    checked(engine.add_observer(std::make_unique<Observer>(OPort{output_id},
        [&](const Observation &batch) { observed.add(batch); }, ObserverOptions{std::chrono::seconds(2), 3})));
    checked(engine.prepare());

    VirtualSource source(capture_spec, period, 997, 0.5f);
    checked(source.prepare());
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    std::vector<std::uint8_t> device(output->pcm_bytes());
    std::size_t deliveries = 0;
    std::uint64_t frame = 0;
    for (int i = 0; i < 4; ++i)
    {
        checked(output->bind_pcm({device.data(), device.size()}));
        for (int period_index = 0; period_index < 2; ++period_index)
        {
            checked(source.generate());
            checked(input->bind_pcm(source.pcm()));
            checked(clock.tick());
        }
        const auto encoded = device;
        std::fill(device.begin(), device.end(), 0xcd); // The device can reuse its buffer once both ticks finish.
        std::vector<std::uint8_t> bytes;
        while (bytes.size() < device.size())
        {
            const auto batch = observed.get(deliveries++);
            check(batch.frame == frame && batch.dropped == 0 && batch.format.is_pcm() &&
                      batch.format.spec.sample_rate == SampleRate::Hz44100 &&
                      batch.format.spec.format == PCMFormat::S16LE,
                  "A PCM batch reports its actual wire format and engine time");
            const auto blocks = batch.pcm.size() == device.size() ? 2 : 1;
            if (blocks == 1)
            {
                const auto samples = bytes.empty() ? 221 : 220;
                check(batch.pcm.size() == samples * pcm_sample_bytes(playback_spec.format),
                      "Fractional PCM blocks retain their actual size");
            }
            frame += blocks * block_frames;
            bytes.insert(bytes.end(), batch.pcm.begin(), batch.pcm.end());
        }
        check(bytes == encoded, "The observer owns the encoded bytes after the device reuses its buffer");
    }
}

void capture_format_switch()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec capture_pcm{rate, PCMFormat::S16LE, 2, 2};
    const PCMSpec playback_pcm{rate, PCMFormat::F32LE, 2};
    const auto input_id = checked(engine.emplace_node<IAdapter>(2, capture_pcm));
    const auto output_id = checked(engine.emplace_node<OAdapter>(2, playback_pcm));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(engine.connect({input_id}, {output_id}, {0.5f, false, {}}));
    checked(engine.prepare());
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    const auto bind_output = [&] { checked(output->bind_pcm({packet.data(), packet.size()})); };
    VirtualSink sink({rate, PCMFormat::F32LE, 2}, period);
    checked(sink.prepare());
    {
        VirtualSource old_device({rate, PCMFormat::S16LE, 2, 2}, period, 431, 0.25f);
        checked(old_device.prepare());
        checked(old_device.generate());
        checked(input->bind_pcm(old_device.pcm()));
        bind_output();
        checked(clock.tick()); // Leave one old-device period bound.
        input->reset();           // Unplug releases borrowed memory before the device is destroyed.
    }
    bind_output();
    checked(clock.tick());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }),
          "Unplugged capture produces silence while the graph clock continues");

    const ChMatrix matrix(2, 1, {1, -0.5f});
    const PCMSpec new_spec{SampleRate::Hz16000, PCMFormat::S16LE, 1};
    checked(engine.set_paused(input_id, true));
    const auto paused = checked(engine.prepare());
    checked(clock.tick());
    check(engine.applied_revision() == paused, "Capture is detached before installing its new format");
    checked(engine.replace_node(input_id, std::make_unique<IAdapter>(2, new_spec, matrix)));
    checked(engine.set_paused(input_id, false));
    checked(engine.prepare());
    input = checked(engine.node<IAdapter>(input_id));
    VirtualSource device(new_spec, period, 1000, 0.5f);
    checked(device.prepare());
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(device.generate());
        checked(input->bind_pcm(device.pcm()));
        bind_output();
        checked(clock.tick());
        checked(sink.receive({packet.data(), packet.size()}));
        for (std::size_t frame = 0; frame < block_frames; ++frame)
        {
            const auto index = transfer * block_frames + frame;
            if (index > 288)
            {
                const auto expected = 0.25 * std::sin(two_pi * 1000 * (static_cast<double>(index) / 48000 - 48.0 / 16000));
                check(std::abs(read_f32le(sink.pcm().data() + frame * 8) - expected) < 0.005 &&
                          std::abs(read_f32le(sink.pcm().data() + frame * 8 + 4) + 0.5 * expected) < 0.005,
                      "16 kHz mono capture is resampled and mapped into the original stereo graph");
            }
        }
    }

    // Successful reconfiguration of a running resampler also discards its bound PCM and history.
    checked(input->bind_pcm(device.pcm()));
    checked(engine.replace_node(input_id, std::make_unique<IAdapter>(2, new_spec, matrix)));
    checked(engine.prepare());
    input = checked(engine.node<IAdapter>(input_id));
    VirtualSource silence(new_spec, period, 0, 0);
    checked(silence.prepare());
    checked(silence.generate());
    checked(input->bind_pcm(silence.pcm()));
    bind_output();
    checked(clock.tick());
    check(std::all_of(packet.begin(), packet.end(), [](std::uint8_t byte) { return byte == 0; }) &&
              checked(engine.node<IAdapter>(input_id)) == input && input->outputs()[0].spec.channels == 2 &&
              engine.frame_position() == 14 * block_frames,
          "Reconfiguration starts with fresh history while preserving node, channels and graph time");
}

void playback_format_switch()
{
    Engine engine(rate, period);
    ManualClock clock(engine);
    const PCMSpec source_spec{rate, PCMFormat::F32LE, 1};
    const PCMSpec old_spec{rate, PCMFormat::F32LE, 2, 2};
    const auto input_id = checked(engine.emplace_node<IAdapter>(2, source_spec,
                                                  ChMatrix(2, 1, {1, 0.5f})));
    const auto output_id = checked(engine.emplace_node<OAdapter>(2, old_spec));
    auto *input = checked(engine.node<IAdapter>(input_id));
    auto *output = checked(engine.node<OAdapter>(output_id));
    checked(engine.connect({input_id}, {output_id}, {0.5f, false, {}}));
    checked(engine.prepare());
    VirtualSource source(source_spec, period, 1000, 1);
    checked(source.prepare());
    std::vector<std::uint8_t> old_packet(output->pcm_bytes(), 0xcd);
    const auto feed = [&] {
        checked(source.generate());
        checked(input->bind_pcm(source.pcm()));
    };
    checked(output->bind_pcm({old_packet.data(), old_packet.size()}));
    feed();
    checked(clock.tick());
    const auto previous = old_packet;

    const ChMatrix matrix(1, 2, {0.25f, 0.75f});
    const PCMSpec new_spec{SampleRate::Hz16000, PCMFormat::S16LE, 1};
    checked(engine.set_paused(output_id, true));
    checked(engine.prepare());
    checked(source.generate()); // Preserve the device's source time during the detached block.
    checked(clock.tick());
    check(old_packet == previous, "The pause stops writes to the partially filled device buffer");
    checked(engine.replace_node(output_id, std::make_unique<OAdapter>(2, new_spec, matrix)));
    checked(engine.set_paused(output_id, false));
    checked(engine.prepare());
    output = checked(engine.node<OAdapter>(output_id));
    feed(); // No new buffer yet: drop this output period and do not write the old device.
    checked(clock.tick());
    check(old_packet == previous, "Switching playback cancels a partially written old device buffer");
    std::vector<std::uint8_t> packet(output->pcm_bytes());
    VirtualSink sink({SampleRate::Hz16000, PCMFormat::S16LE, 1}, period);
    checked(sink.prepare());
    const auto start_frame = engine.frame_position();
    for (std::size_t transfer = 0; transfer < 10; ++transfer)
    {
        checked(output->bind_pcm({packet.data(), packet.size()}));
        feed();
        checked(clock.tick());
        checked(sink.receive({packet.data(), packet.size()}));
        const auto pcm = sink.pcm();
        for (std::size_t frame = 0; frame < 80; ++frame)
        {
            const auto word = static_cast<std::uint16_t>(pcm[frame * 2]) |
                              (static_cast<std::uint16_t>(pcm[frame * 2 + 1]) << 8);
            const auto sample = (word >= 32768 ? static_cast<int>(word) - 65536 : static_cast<int>(word)) / 32768.0;
            const auto index = transfer * 80 + frame;
            if (index > 32)
            {
                const auto seconds = static_cast<double>(start_frame) / 48000 + static_cast<double>(index) / 16000 -
                                     48.0 / 48000;
                check(std::abs(sample - 0.3125 * std::sin(two_pi * 1000 * seconds)) < 0.005,
                      "Stereo graph playback is mapped and resampled to the new 16 kHz mono device");
            }
        }
    }
    // Replacing an active resampler with the original format must not retain a filter tail or index.
    checked(output->bind_pcm({packet.data(), packet.size()}));
    const PCMSpec final_spec{rate, PCMFormat::F32LE, 2};
    checked(engine.set_paused(output_id, true));
    checked(engine.prepare());
    checked(clock.tick());
    checked(engine.replace_node(output_id, std::make_unique<OAdapter>(2, final_spec)));
    checked(engine.set_paused(output_id, false));
    checked(engine.prepare());
    output = checked(engine.node<OAdapter>(output_id));
    std::vector<std::uint8_t> stereo(output->pcm_bytes(), 0xcd);
    checked(output->bind_pcm({stereo.data(), stereo.size()}));
    checked(clock.tick()); // Missing source is silence.
    check(std::all_of(stereo.begin(), stereo.end(), [](std::uint8_t byte) { return byte == 0; }) &&
              old_packet == previous && checked(engine.node<OAdapter>(output_id)) == output &&
              output->inputs()[0].spec.channels == 2 && engine.frame_position() == 15 * block_frames,
          "Playback reconfiguration preserves logical identity and time, without old-device writes or history");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {
        {"adapter_port_observation", adapter_port_observation},
        {"observer_pcm_batching", observer_pcm_batching},
        {"capture_format_switch", capture_format_switch},
        {"playback_format_switch", playback_format_switch}
    });
}
