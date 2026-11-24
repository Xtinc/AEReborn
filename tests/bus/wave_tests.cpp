#include "tests/support/io_fixture.h"
#include "backend/pull_sink.h"
#include "backend/usr_playback.h"
#include "backend/wav_backend.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include "tests/support/tone.h"

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;

constexpr auto archive_rate = SampleRate::Hz44100;
const PortSpec stereo(rate, 2, SampleFormat::F32P);
const PortSpec music(rate, 2, SampleFormat::F32LE);
const char *dukou = AEREBORN_TEST_DATA "/dukou_excerpt.wav";
const char *california = AEREBORN_TEST_DATA "/california_hotel_excerpt.wav";
constexpr std::size_t dukou_frames = 24000;
constexpr std::size_t california_frames = 36000;

void check_s16(const Pcm &recording, const Pcm &original)
{
    for (std::size_t frame = 0; frame < recording.frames; ++frame)
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const double scaled = 32768.0 * original.sample(frame, ch);
            const auto value = std::max(-32768l, std::min(32767l, std::lround(scaled)));
            const auto word = static_cast<std::uint16_t>(value);
            const auto i = (2 * frame + ch) * 2;
            check(recording.bytes[i] == (word & 0xff) && recording.bytes[i + 1] == (word >> 8),
                  "The split archive preserves the original song, channel order and PCM quantization");
        }
}

void four_channel_song_archive()
{
    const auto song_a = read_wave(dukou, music, dukou_frames);
    const auto song_b = read_wave(california, music, california_frames);
    Pcm combined({rate, 4, SampleFormat::F32LE}, dukou_frames);
    for (std::size_t frame = 0; frame < combined.frames; ++frame)
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            combined.set(frame, ch, song_a.sample(frame, ch));
            combined.set(frame, ch + 2, song_b.sample(frame, ch));
        }

    TempWave files("song-archive");
    const auto input_path = files.path("-input.wav");
    const auto a_path = files.path("-dukou.wav");
    const auto b_path = files.path("-california.wav");
    const auto mix_path = files.path("-mix.wav");
    write_wave(input_path, combined);

    const PortSpec reduced(archive_rate, 2, SampleFormat::F32P);
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto input = engine.emplace_node<Capture>(rate, 4, 5ms);
    const auto a = engine.emplace_node<Matrix>(rate, 4, 2, std::initializer_list<float>{1, 0, 0, 0, 0, 1, 0, 0});
    const auto b = engine.emplace_node<Matrix>(rate, 4, 2, std::initializer_list<float>{0, 0, 1, 0, 0, 0, 0, 1});
    const auto mix = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto gain = engine.emplace_node<Matrix>(rate, 2, 0.5f);
    const auto resample = engine.emplace_node<Convert>(stereo, reduced);
    const auto out_a = engine.emplace_node<Playback>(rate, 2, 5ms);
    const auto out_b = engine.emplace_node<Playback>(rate, 2, 5ms);
    const auto out_mix = engine.emplace_node<Playback>(archive_rate, 2, 5ms);
    const auto tap = engine.emplace_node<PacketSink>(stereo);
    const auto rate_tap = engine.emplace_node<PacketSink>(reduced);
    connect(engine, input, a);
    connect(engine, input, b);
    connect(engine, a, out_a);
    connect(engine, b, out_b);
    connect(engine, a, mix);
    connect(engine, b, mix, 1);
    connect(engine, mix, gain);
    connect(engine, gain, tap);
    connect(engine, gain, resample);
    connect(engine, resample, out_mix);
    connect(engine, resample, rate_tap);
    engine.commit();

    auto &before = *engine.node<PacketSink>(tap);
    auto &after = *engine.node<PacketSink>(rate_tap);
    WavCapture source(input_path, false);
    WavPlayback archive_a(a_path, SampleFormat::S16LE), archive_b(b_path, SampleFormat::S16LE);
    WavPlayback archive_mix(mix_path, SampleFormat::F32LE);
    source.bind(*engine.node<Capture>(input));
    archive_a.bind(*engine.node<Playback>(out_a));
    archive_b.bind(*engine.node<Playback>(out_b));
    archive_mix.bind(*engine.node<Playback>(out_mix));
    const auto archives = {&archive_a, &archive_b, &archive_mix};
    source.start();
    for (auto *archive : archives)
        archive->start();
    clock.start();
    constexpr std::size_t input_frames = 240;
    Pcm resampled({archive_rate, 2, SampleFormat::F32LE}, 22050);
    std::size_t written = 0;
    const auto expected_mix = [&](std::size_t frame, std::size_t ch) {
        return 0.5f * song_a.sample(frame, ch) + 0.5f * song_b.sample(frame, ch);
    };
    for (std::size_t tick = 0; tick < dukou_frames / input_frames; ++tick)
    {
        const std::size_t output_frames = tick % 2 ? 220 : 221;
        wait_for_capture(source, input_frames);
        wait_for_playback(archive_a, input_frames);
        wait_for_playback(archive_b, input_frames);
        wait_for_playback(archive_mix, output_frames);
        clock.advance();
        check(before.frames == input_frames && after.frames == output_frames, "The two rates use the same tick timeline");
        for (std::size_t frame = 0; frame < input_frames; ++frame)
            for (std::size_t ch = 0; ch < 2; ++ch)
                check(before.sample(frame, ch) == expected_mix(tick * input_frames + frame, ch),
                      "The 48 kHz mix adds the corresponding channels of both songs");
        for (std::size_t frame = 0; frame < output_frames; ++frame)
            for (std::size_t ch = 0; ch < 2; ++ch)
                resampled.set(written + frame, ch, after.sample(frame, ch));
        written += output_frames;
    }
    wait_for([&] { return !source.running(); }, "The four-channel file reaches EOF");
    check(source.last_error() == ErrorCode::EndOfStream, "The input is consumed completely");
    for (auto *archive : archives)
    {
        archive->unbind();
        check(archive->last_error() == ErrorCode::Ok, "The archive drains and finalizes");
    }

    const PortSpec split_spec(rate, 2, SampleFormat::S16LE);
    check_s16(read_wave(a_path, split_spec, dukou_frames), song_a);
    check_s16(read_wave(b_path, split_spec, dukou_frames), song_b);
    const auto recording = read_wave(mix_path, resampled.spec, resampled.frames);
    check(written == recording.frames && recording.bytes == resampled.bytes,
          "The 44.1 kHz archive preserves every rendered stereo frame");
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        double error = 0, energy = 0;
        for (std::size_t frame = 100; frame < recording.frames; ++frame)
        {
            // Linear interpolation is an independent time-domain reference. Allow for the low-pass FIR response.
            const auto position = frame * (48000.0 / 44100) - 48;
            const auto i = static_cast<std::size_t>(position);
            const auto fraction = position - i;
            const auto value = (1 - fraction) * expected_mix(i, ch) + fraction * expected_mix(i + 1, ch);
            const auto delta = recording.sample(frame, ch) - value;
            error += delta * delta;
            energy += value * value;
        }
        check(energy > 0 && error < 0.01 * energy, "Each recorded channel follows the time-aligned mix");
    }
}

void repeated_tracks_and_segments()
{
    const auto song_a = read_wave(dukou, music, dukou_frames);
    const auto song_b = read_wave(california, music, california_frames);
    TempWave files("segments");
    const auto base = files.path();
    const auto first_path = files.path("-0000000001.wav");
    const auto second_path = files.path("-0000000002.wav");
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto a = engine.emplace_node<Capture>(rate, 2, 5ms);
    const auto b = engine.emplace_node<Capture>(rate, 2, 5ms);
    const auto mixer = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto record = engine.emplace_node<Playback>(rate, 2, 5ms);
    const auto monitor = engine.emplace_node<Playback>(rate, 2, 5ms);
    connect(engine, a, mixer);
    connect(engine, b, mixer, 1);
    connect(engine, mixer, record);
    connect(engine, mixer, monitor);
    engine.commit();
    WavCapture loop(dukou, WavCapture::Repeat{1}), once(california, false);
    WavPlayback writer(base, SampleFormat::F32LE, 1s);
    PullSink listener(music);
    loop.bind(*engine.node<Capture>(a));
    once.bind(*engine.node<Capture>(b));
    writer.bind(*engine.node<Playback>(record));
    listener.bind(*engine.node<Playback>(monitor));
    loop.start();
    once.start();
    writer.start();
    listener.start();
    clock.start();
    constexpr std::size_t frames = 240, segment = 48000;
    Pcm heard(music, 57600);
    const auto expected = [&](std::size_t frame, std::size_t ch) {
        const auto repeated = frame < 2 * dukou_frames ? song_a.sample(frame % dukou_frames, ch) : 0;
        const auto single = frame < california_frames ? song_b.sample(frame, ch) : 0;
        return repeated + single;
    };
    for (std::size_t start = 0; start < heard.frames; start += frames)
    {
        if (start < 2 * dukou_frames)
            wait_for_capture(loop, frames);
        if (start < california_frames)
            wait_for_capture(once, frames);
        wait_for_playback(writer, frames);
        clock.advance();
        check(listener.pull(heard.output(start, frames)) == frames, "The live listener receives the whole block");
        for (std::size_t frame = start; frame < start + frames; ++frame)
            for (std::size_t ch = 0; ch < 2; ++ch)
                check(heard.sample(frame, ch) == expected(frame, ch), "Repeat and EOF preserve the mixed timeline");
    }
    wait_for([&] { return !loop.running() && !once.running(); }, "Both tracks reach EOF");
    check(loop.last_error() == ErrorCode::EndOfStream && once.last_error() == ErrorCode::EndOfStream,
          "Both sources report natural completion");
    writer.unbind();
    check(writer.last_error() == ErrorCode::Ok, "Both segments finalize");
    const auto front = read_wave(first_path, music, segment);
    const auto back = read_wave(second_path, music, heard.frames - segment);
    check(std::equal(front.bytes.begin(), front.bytes.end(), heard.bytes.begin()) &&
              std::equal(back.bytes.begin(), back.bytes.end(), heard.bytes.begin() + front.bytes.size()),
          "Two recorded segments concatenate to the samples heard live");
}

void three_output_specs()
{
    constexpr std::size_t ticks = 20;
    const PortSpec wav_spec(archive_rate, 2, SampleFormat::S16LE);
    const PortSpec callback_spec(SampleRate::Hz16000, 1, SampleFormat::S16LE);
    TempWave files("three-outputs");
    const auto path = files.path();
    Engine engine(period);
    ClockedGraph clock(engine);
    const auto a = engine.emplace_node<Tone>(rate, 2, 1000, 0.25f);
    const auto b = engine.emplace_node<FloatSource>(rate, 2, [](std::uint64_t, std::size_t ch) {
        return ch == 0 ? 0.125f : -0.125f;
    });
    const auto mix = engine.emplace_node<Mixer>(rate, 2, 2);
    const auto resample = engine.emplace_node<Convert>(stereo, PortSpec(archive_rate, 2, SampleFormat::F32P));
    const auto downmix = engine.emplace_node<Matrix>(rate, 2, 1, std::initializer_list<float>{0.75f, 0.25f});
    const auto live = engine.emplace_node<Playback>(rate, 2, 5ms);
    const auto wav = engine.emplace_node<Playback>(archive_rate, 2, 5ms);
    const auto cb = engine.emplace_node<Playback>(rate, 1, 5ms);
    connect(engine, a, mix);
    connect(engine, b, mix, 1);
    connect(engine, mix, live);
    connect(engine, mix, resample);
    connect(engine, resample, wav);
    connect(engine, mix, downmix);
    connect(engine, downmix, cb);
    engine.commit();
    Pcm callback_pcm(callback_spec, ticks * 80 + 64);
    std::size_t callback_frames = 0;
    bool callback_fits = true;
    UsrPlayback client(callback_spec, [&](IBBlock block) noexcept {
        callback_fits &= callback_pcm.copy_from(callback_frames, block);
        callback_frames += block.frames();
    });
    WavPlayback file(path, wav_spec.format);
    PullSink pull(music);
    file.bind(*engine.node<Playback>(wav));
    client.bind(*engine.node<Playback>(cb));
    pull.bind(*engine.node<Playback>(live));
    file.start();
    client.start();
    pull.start();
    clock.start();
    Pcm block(music, 240);
    for (std::size_t tick = 0; tick < ticks; ++tick)
    {
        wait_for_playback(file, tick % 2 ? 220 : 221);
        wait_for_playback(client, 240);
        clock.advance();
        check(pull.pull(block.output(0, block.frames)) == block.frames, "The 48 kHz output delivers each block");
        for (std::size_t frame = 0; frame < block.frames; ++frame)
        {
            const auto tone = 0.25 * std::sin(two_pi * 1000 * (tick * 240 + frame) / 48000);
            check(std::abs(block.sample(frame, 0) - (tone + 0.125)) < 1e-6 &&
                      std::abs(block.sample(frame, 1) - (tone - 0.125)) < 1e-6,
                  "The native-rate output retains its stereo mix");
        }
    }
    file.unbind();
    client.stop();
    check(file.last_error() == ErrorCode::Ok && client.last_error() == ErrorCode::Ok, "Both workers drain successfully");
    const auto recorded = read_wave(path, wav_spec, ticks * 441 / 2);
    const auto s16 = [](const Pcm &pcm, std::size_t sample) {
        const int bits = pcm.bytes[2 * sample] | (pcm.bytes[2 * sample + 1] << 8);
        return (bits < 32768 ? bits : bits - 65536) / 32768.0;
    };
    for (std::size_t frame = 96; frame < recorded.frames; ++frame)
    {
        const auto tone = 0.25 * std::sin(two_pi * 1000 * (frame / 44100.0 - 0.001));
        check(std::abs(s16(recorded, 2 * frame) - (tone + 0.125)) < 0.002 &&
                  std::abs(s16(recorded, 2 * frame + 1) - (tone - 0.125)) < 0.002,
              "The resampled file contains the correct stereo channels");
    }
    check(callback_fits && callback_frames == ticks * 80 + 32, "The 16 kHz callback drains its FIR tail");
    for (std::size_t frame = 40; frame < ticks * 80; ++frame)
    {
        const auto tone = 0.25 * std::sin(two_pi * 1000 * (frame / 16000.0 - 0.001));
        check(std::abs(s16(callback_pcm, frame) - (tone + 0.0625)) < 0.002,
              "The 16 kHz callback contains the weighted mono mix");
    }
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"four_channel_song_archive", four_channel_song_archive},
                                                {"repeated_tracks_and_segments", repeated_tracks_and_segments},
                                                {"three_output_specs", three_output_specs}});
}
