#include "audio_interface.h"
#include "test_common.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{

using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
using Bytes = std::vector<unsigned char>;

void expect(RetCode actual, RetCode expected = RetCode::OK)
{
    if (actual != expected)
        throw std::runtime_error(std::string("Expected ") + expected.what() + ", got " + actual.what());
}

Bytes read_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "Open the WAV file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::string &path, const Bytes &data)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close();
    check(static_cast<bool>(file), "Write the WAV fixture");
}

void write_constant_stereo(const std::string &path)
{
    Bytes data{
        'R', 'I', 'F',  'F',  0, 0, 0, 0,    'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,  0,   0, 0, 1, 0,
        2,   0,   0x80, 0xbb, 0, 0, 0, 0xee, 2,   0,   4,   0,   16,  0,   'd', 'a', 't', 'a', 0, 0, 0, 0,
    };
    constexpr auto payload = 480 * 4;
    const auto store_size = [&](std::size_t offset, std::uint32_t size) {
        for (std::size_t i = 0; i < 4; ++i)
            data[offset + i] = static_cast<unsigned char>(size >> (8 * i));
    };
    store_size(4, payload + 36);
    store_size(40, payload);
    data.resize(44 + payload);
    for (std::size_t high_byte = 45; high_byte < data.size(); high_byte += 2)
        data[high_byte] = 0x20; // Signed 16-bit sample for 0.25.
    write_file(path, data);
}

std::uint32_t little_endian(const Bytes &data, std::size_t offset, std::size_t width)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= static_cast<std::uint32_t>(data.at(offset + i)) << (8 * i);
    return value;
}

Bytes read_s16(const std::string &path, std::uint32_t rate, std::size_t channels)
{
    const auto data = read_file(path);
    check(data.size() >= 44 && std::string(data.begin(), data.begin() + 4) == "RIFF" &&
              std::string(data.begin() + 8, data.begin() + 12) == "WAVE" &&
              std::string(data.begin() + 36, data.begin() + 40) == "data",
          "Expect a PCM RIFF/WAVE header");
    check(little_endian(data, 20, 2) == 1 && little_endian(data, 22, 2) == channels &&
              little_endian(data, 24, 4) == rate && little_endian(data, 34, 2) == 16,
          "Expect the configured WAV specification");
    check(little_endian(data, 4, 4) == data.size() - 8 && little_endian(data, 40, 4) == data.size() - 44 &&
              (data.size() - 44) % (channels * 2) == 0,
          "Expect finalized RIFF and data lengths");
    return {data.begin() + 44, data.end()};
}

bool has_signal(const Bytes &data)
{
    return std::any_of(data.begin(), data.end(), [](unsigned char byte) { return byte != 0; });
}

std::size_t find_steady_level(const Bytes &data, std::size_t start, std::uint16_t level, std::size_t channels = 2)
{
    std::size_t matching = 0;
    for (auto sample = start; sample < data.size(); sample += 2)
    {
        matching = little_endian(data, sample, 2) == level ? matching + 1 : 0;
        if (matching == 2 * 480 * channels)
            return sample + 2;
    }
    throw std::runtime_error("Expected two blocks at level " + std::to_string(level));
}

std::uint64_t tick(const AudioCenter &audio)
{
    const auto text = audio.dump_text();
    const auto start = text.find(" | tick=");
    check(start != std::string::npos, "Expect the current render tick in diagnostics");
    return std::stoull(text.substr(start + 8));
}

void advance(const AudioCenter &audio, std::uint64_t blocks)
{
    const auto target = tick(audio) + blocks;
    wait_for([&] { return tick(audio) >= target; }, "Wait for the expected render ticks", 4s);
}

void channel_map()
{
    const auto selected = ChannelMap::select(3, std::vector<std::size_t>{2, 0, 2});
    check(selected.valid() && selected.input_channels() == 3 && selected.output_channels() == 3 &&
              selected.coefficients() == std::vector<float>{0, 0, 1, 1, 0, 0, 0, 0, 1},
          "Selection reorders and duplicates input channels in output order");
    const auto sliced = ChannelMap::slice(3, 1, 2);
    check(sliced.coefficients() == std::vector<float>{0, 1, 0, 0, 0, 1}, "Slice preserves the selected channel order");
    const auto mono = ChannelMap::to_mono(4);
    check(mono.coefficients() == std::vector<float>{0.25f, 0.25f, 0.25f, 0.25f}, "Downmix averages all channels");
    ChannelMap replaced(3, 1);
    replaced.route(2, 0).mix({0, 1}, 0);
    check(replaced.coefficients() == std::vector<float>{0.5f, 0.5f, 0},
          "A mix replaces every weight in its output row");

    const std::vector<float> raw{1.0f, -0.5f};
    auto moved_from = ChannelMap::dense(2, 1, raw);
    auto moved = std::move(moved_from);
    check(moved.valid() && moved.coefficients() == raw, "A dense map preserves coefficients through a move");
    expect(moved_from.route(0, 0).status(), RetCode::EPARAM);
    check(moved_from.weight(0, 0) == 0.0f, "Moved-from maps are safe to query");

    const auto infinity = std::numeric_limits<float>::infinity();
    const auto largest = std::numeric_limits<float>::max();
    expect(ChannelMap().status(), RetCode::EPARAM);
    expect(ChannelMap(0, 2).status(), RetCode::EPARAM);
    expect(ChannelMap::identity(std::numeric_limits<std::size_t>::max()).status(), RetCode::TOOLARGE);
    expect(ChannelMap::select(3, {}).status(), RetCode::EPARAM);
    expect(ChannelMap::select(3, {3}).status(), RetCode::EPARAM);
    expect(ChannelMap::slice(3, 1, std::numeric_limits<std::size_t>::max()).status(), RetCode::EPARAM);
    expect(ChannelMap::dense(2, 1, {1.0f}).status(), RetCode::EPARAM);
    expect(ChannelMap::dense(1, 1, {std::numeric_limits<float>::quiet_NaN()}).status(), RetCode::EPARAM);
    expect(ChannelMap(2, 1).mix({}, 0).status(), RetCode::EPARAM);
    expect(ChannelMap(2, 1).mix({0, 2}, 0).status(), RetCode::EPARAM);
    expect(ChannelMap(2, 1).route(0, 0, infinity).status(), RetCode::EPARAM);
    auto overflow = ChannelMap::dense(2, 1, {1.0f, largest});
    overflow.scale_output(0, 2.0f);
    expect(overflow.status(), RetCode::EPARAM);
    check(overflow.coefficients() == std::vector<float>{1.0f, largest},
          "A scaling overflow leaves the full row intact");

    TempWave files("channel-map");
    const auto input = files.path("-input.wav");
    const auto output = files.path("-output.wav");
    write_constant_stereo(input);
    AudioCenter audio;
    check(!audio.create_matrix(BandWidth::Full, ChannelMap()), "Invalid maps do not create nodes");
    expect(AudioCenter::last_error(), RetCode::EPARAM);
    check(!audio.create_matrix(BandWidth::Full, ChannelMap::identity(std::numeric_limits<std::size_t>::max())),
          "Map capacity failures do not create nodes");
    expect(AudioCenter::last_error(), RetCode::TOOLARGE);
    const auto source = audio.create_iwav(input, -1);
    ChannelMap negative(2, 1);
    negative.route(0, 0, 2.0f).mix(std::vector<std::size_t>{0, 1}, 0, -1.0f);
    const auto matrix = audio.create_matrix(BandWidth::Full, negative, 0.5f);
    const auto sink = audio.create_owav(output, {BandWidth::Full, 1, SampleFormat::S16LE});
    check(source && matrix && sink, "Create a two-to-one route from the map");
    expect(audio.connect(source, matrix));
    expect(audio.connect(matrix, sink));
    expect(audio.start());
    advance(audio, 8);
    expect(audio.remove(sink));
    find_steady_level(read_s16(output, 48000, 1), 0, static_cast<std::uint16_t>(-4096), 1);
}

void wav_routing_and_bridge()
{
    const std::string input = AEREBORN_TEST_DATA "/dukou_excerpt.wav";
    TempWave files("audio-center-routing");
    const auto stereo = files.path("-stereo.wav");
    const auto mono = files.path("-mono.wav");
    const auto upmixed = files.path("-upmixed.wav");
    const auto added = files.path("-added.wav");
    const PortSpec cd_mono{BandWidth::CDQuality, 1, SampleFormat::S16LE};
    AudioCenter audio;
    const auto source = audio.create_iwav(input);
    const auto mixer = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto matrix = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
    const auto out_stereo = audio.create_owav(stereo);
    const auto out_mono = audio.create_owav(mono, cd_mono);
    check(source && mixer && matrix && out_stereo && out_mono, "Create all WAV routes");
    expect(audio.connect(source, mixer[1]));
    expect(audio.connect(mixer, matrix));
    expect(audio.connect(matrix, out_stereo));
    expect(audio.connect(matrix, out_mono));
    expect(audio.start());
    expect(audio.start(), RetCode::BUSY);

    const auto text = audio.dump_text();
    const auto bridge = text.rfind("] Matrix");
    const auto converter = text.find("] Convert");
    check(bridge != std::string::npos && converter != std::string::npos && bridge < converter,
          "Downmix precedes resampling");
    check(audio.dump_dot().find("digraph") != std::string::npos, "Expect DOT diagnostics");
    wait_for([&] { return audio.dump_text().find("err=End of stream") != std::string::npos; },
             "The finite input reaches EOS", 4s);
    const auto added_source = audio.create_iwav(input, -1);
    const auto added_sink = audio.create_owav(added);
    check(added_source && added_sink, "Create an independent branch while the engine is running");
    expect(audio.connect(added_source, added_sink));
    expect(audio.set_paused(out_stereo, true));
    expect(audio.commit());
    const auto changed = audio.dump_text();
    check(changed.find("err=End of stream") != std::string::npos &&
              changed.find("WavCapture (sync) state=Running") != std::string::npos,
          "The new branch starts while the original capture stays at EOS");
    advance(audio, 8);
    check(audio.dump_text().find("err=End of stream") != std::string::npos,
          "The original capture keeps its EOS status while the new branch runs");
    expect(audio.stop());
    expect(audio.stop());
    expect(audio.remove(out_stereo));
    expect(audio.remove(out_mono));
    expect(audio.remove(added_sink));
    expect(audio.remove(added_source));
    check(has_signal(read_s16(stereo, 48000, 2)), "Stereo branch contains audio");
    check(has_signal(read_s16(mono, 44100, 1)), "Downmixed and resampled branch contains audio");
    check(has_signal(read_s16(added, 48000, 2)), "The branch added during runtime records audio");

    const auto mono_source = audio.create_iwav(mono);
    const auto stereo_sink = audio.create_owav(upmixed);
    check(mono_source && stereo_sink, "Create the reverse specification route");
    expect(audio.connect(mono_source, stereo_sink));
    expect(audio.commit());
    const auto stopped = audio.dump_text();
    check(stopped.find("Engine: Ready") != std::string::npos &&
              stopped.find("WavCapture (sync) state=Stopped") != std::string::npos &&
              stopped.find("WavPlayback (sync) state=Stopped") != std::string::npos &&
              stopped.find("state=Running") == std::string::npos,
          "A stopped engine can publish a new plan without starting its backends");
    expect(audio.start());
    const auto reverse = audio.dump_text();
    const auto upconvert = reverse.find("] Convert");
    const auto upmix = reverse.find("] Matrix");
    check(upconvert != std::string::npos && upmix != std::string::npos && upconvert < upmix,
          "Mono resampling precedes channel duplication");
    advance(audio, 6);
    expect(audio.remove(stereo_sink));
    const auto upmixed_pcm = read_s16(upmixed, 48000, 2);
    check(has_signal(upmixed_pcm), "Upsampled branch contains audio");
    for (std::size_t sample = 0; sample < upmixed_pcm.size(); sample += 4)
        check(little_endian(upmixed_pcm, sample, 2) == little_endian(upmixed_pcm, sample + 2, 2),
              "Upmix duplicates mono into both channels");
}

void live_control_and_cleanup()
{
    TempWave files("audio-center-live");
    const auto input = files.path("-constant.wav");
    const auto output = files.path("-live.wav");
    const auto monitor = files.path("-monitor.wav");
    write_constant_stereo(input);
    AudioCenter audio;
    const auto source = audio.create_iwav(input, -1);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto gain = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
    const auto other_rate = audio.create_matrix(BandWidth::CDQuality, ChannelMap::identity(1));
    const auto sink = audio.create_owav(output);
    const auto monitor_sink = audio.create_owav(monitor);
    check(source && mix && gain && other_rate && sink && monitor_sink, "Create the live graph");
    expect(audio.connect(source, mix));
    expect(audio.connect(source, mix[1]));
    expect(audio.connect(mix, gain));
    expect(audio.connect(gain, sink));
    expect(audio.connect(gain, other_rate));
    expect(audio.connect(other_rate, gain), RetCode::INPUTBUSY);
    expect(audio.connect(gain, mix), RetCode::INPUTBUSY);
    expect(audio.disconnect(source, mix));
    expect(audio.connect(other_rate, mix), RetCode::CYCLE);
    expect(audio.connect(source, mix));
    expect(audio.connect(source, mix), RetCode::DUPLICATE);
    expect(audio.disconnect({}, sink), RetCode::ENODE);
    expect(audio.disconnect(gain[1], sink), RetCode::EPORT);
    expect(audio.disconnect(gain, sink));
    expect(audio.disconnect(sink), RetCode::ECONN);
    expect(audio.connect(gain, sink));
    expect(audio.connect(source, monitor_sink));
    expect(audio.remove(mix[1]), RetCode::EPORT);
    expect(audio.set_gain(mix, 0.5f), RetCode::ETYPE);
    expect(audio.set_muted(mix[2], true), RetCode::EPORT);
    expect(audio.start());
    advance(audio, 8);
    expect(audio.set_high_priority(mix[1], true));
    advance(audio, 8);
    expect(audio.set_gain(gain, 0.5f));
    advance(audio, 8);
    expect(audio.set_muted(mix, true));
    expect(audio.set_muted(mix[1], true));
    advance(audio, 8);
    expect(audio.set_muted(mix[1], false));
    advance(audio, 8);
    check(audio.dump_dot().find("[HIGH]") != std::string::npos, "Report the live priority setting");
    expect(audio.disconnect(sink));
    expect(audio.commit());
    advance(audio, 8);
    expect(audio.set_paused(sink, true));
    expect(audio.commit());
    check(audio.dump_text().find("[" + std::to_string(sink.node()) + "] Playback") == std::string::npos,
          "Pausing the recorder removes it from the running plan");
    advance(audio, 5);
    expect(audio.remove(sink));
    const auto sound = read_s16(output, 48000, 2);
    check(sound.size() >= 480 * 8 * 4 && has_signal(sound), "The recorder captures the routed signal");
    auto cursor = find_steady_level(sound, 0, 16384);
    cursor = find_steady_level(sound, cursor, 8192);
    cursor = find_steady_level(sound, cursor, 4096);
    cursor = find_steady_level(sound, cursor, 0);
    find_steady_level(sound, cursor, 4096);
    check(std::none_of(sound.end() - 480 * 4 * 4, sound.end(), [](unsigned char byte) { return byte != 0; }),
          "Disconnect keeps recording silence through the final blocks");

    expect(audio.set_paused(monitor_sink, true));
    expect(audio.commit());
    const auto paused = audio.dump_text();
    const auto detached = paused.find("Detached Backends:");
    check(paused.find("WavPlayback (sync) state=Stopped", detached) != std::string::npos,
          "A paused recorder stops and appears among detached backends");
    expect(audio.set_paused(monitor_sink, false));
    expect(audio.commit());
    const auto resumed = audio.dump_text();
    check(resumed.find("WavCapture (sync) state=Running") != std::string::npos &&
              resumed.find("WavPlayback (sync) state=Running") != std::string::npos,
          "Both endpoints restart when the recorder rejoins the plan");

    expect(audio.remove(source)); // Unbind while the old plan still includes this endpoint.
    expect(audio.commit());
    expect(audio.remove(monitor_sink));
    find_steady_level(read_s16(monitor, 48000, 2), 0, 8192);
    check(!audio.create_iwav(output + ".missing"), "Missing input returns an empty handle");
    expect(AudioCenter::last_error(), RetCode::IOFAILED);
}

void concurrency_and_teardown()
{
    const auto original = read_file(AEREBORN_TEST_DATA "/dukou_excerpt.wav");
    TempWave files("audio-center-teardown");
    const auto input = files.path("-existing.wav");
    const auto output = files.path("-teardown.wav");
    write_file(input, original);
    {
        AudioCenter audio;
        const auto source = audio.create_iwav(input, -1);
        check(!audio.create_owav(input), "Creating a WAV output must preserve the existing input path");
        expect(AudioCenter::last_error(), RetCode::BUSY);
        const auto matrix = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
        const auto mixer = audio.create_mixer(BandWidth::Full, 2, 2);
        const auto sink = audio.create_owav(output);
        check(source && matrix && mixer && sink, "Create the teardown graph");
        expect(audio.connect(source, matrix));
        expect(audio.connect(matrix, mixer));
        expect(audio.connect(mixer, sink));
        expect(audio.start());
        expect(audio.set_gain({}, 1.0f), RetCode::ENODE);
        auto control = std::async(std::launch::async, [&] {
            for (int i = 0; i < 10; ++i)
            {
                expect(audio.set_gain(matrix, 0.5f));
                expect(audio.set_muted(mixer[1], i % 2 != 0));
            }
            expect(AudioCenter::last_error());
        });
        auto diagnostics = std::async(std::launch::async, [&] {
            for (int i = 0; i < 10; ++i)
            {
                check(!audio.dump_text().empty(), "Concurrent diagnostics are available");
                expect(audio.commit());
            }
            expect(audio.set_gain(mixer, 0.5f), RetCode::ETYPE);
            expect(AudioCenter::last_error(), RetCode::ETYPE);
        });
        control.get();
        diagnostics.get();
        expect(AudioCenter::last_error(), RetCode::ENODE);
        advance(audio, 8);
    }
    check(original == read_file(input), "Existing input is byte-for-byte unchanged");
    check(has_signal(read_s16(output, 48000, 2)), "Destruction finalizes the active recording");
}

} // namespace

int main(int argc, char **argv)
{
    return run_tests(argc, argv,
                     {{"channel_map", channel_map},
                      {"wav_routing_and_bridge", wav_routing_and_bridge},
                      {"live_control_and_cleanup", live_control_and_cleanup},
                      {"concurrency_and_teardown", concurrency_and_teardown}});
}
