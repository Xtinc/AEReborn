#include "audio_interface.h"
#include "test_common.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    const auto snapshot = audio.dump_json();
    const auto start = snapshot.find("\"tick\":");
    check(start != std::string::npos, "Expect the current render tick in diagnostics");
    return std::stoull(snapshot.substr(start + 7));
}

void advance(const AudioCenter &audio, std::uint64_t blocks)
{
    const auto target = tick(audio) + blocks;
    wait_for([&] { return tick(audio) >= target; }, "Wait for the expected render ticks", 4s);
}

std::string backend_state(NodeId id, const char *state)
{
    return "\"node\":" + std::to_string(id.node()) + ",\"state\":\"" + state + '"';
}

const PortSpec stereo_pcm{BandWidth::Full, 2, SampleFormat::S16LE};
const PortSpec stereo_planar{BandWidth::Full, 2, SampleFormat::F32P};

struct CallbackSource
{
    PortSpec spec;
    std::size_t batch_frames;
    int levels[2];
    const std::thread::id control_thread = std::this_thread::get_id();
    std::atomic<std::size_t> calls{0};
    std::atomic<bool> valid{true};

    static void fill(const void *input, void *output, std::size_t frames, PortSpec spec, void *data) noexcept
    {
        auto &self = *static_cast<CallbackSource *>(data);
        if (input || !output || spec != self.spec || frames != self.batch_frames ||
            std::this_thread::get_id() == self.control_thread)
        {
            self.valid = false;
            return;
        }
        auto *samples = static_cast<unsigned char *>(output);
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (std::size_t ch = 0; ch < spec.channels; ++ch)
            {
                const auto value = static_cast<std::uint16_t>(self.levels[ch]);
                *samples++ = static_cast<unsigned char>(value);
                *samples++ = static_cast<unsigned char>(value >> 8);
            }
        ++self.calls;
    }
};

struct CallbackSink
{
    static constexpr std::size_t steady_frames = 960;

    CallbackSink(PortSpec spec, std::size_t batch_frames)
        : spec(spec), batch_frames(batch_frames), samples(96000 * spec.channels * 2)
    {
    }

    static void receive(const void *input, void *output, std::size_t frames, PortSpec spec, void *data) noexcept
    {
        auto &self = *static_cast<CallbackSink *>(data);
        const auto delivered = self.delivered.load();
        const auto frame_bytes = 2 * self.spec.channels;
        if (!input || output || spec != self.spec || !frames || frames > self.batch_frames ||
            frames > self.samples.size() / frame_bytes - delivered ||
            std::this_thread::get_id() == self.control_thread)
        {
            self.valid = false;
            return;
        }
        std::memcpy(self.samples.data() + delivered * frame_bytes, input, frames * frame_bytes);
        self.last_batch = frames;
        ++self.calls;
        // Publish the completed prefix. The worker only writes beyond it on subsequent calls.
        self.delivered = delivered + frames;
    }

    std::size_t wait_for_levels(std::size_t first_frame, int left, int right, int tolerance = 0) const
    {
        auto frame = first_frame;
        std::size_t matching = 0;
        wait_for([&] {
            const auto ready = delivered.load();
            for (; frame < ready; ++frame)
            {
                const auto offset = frame * 4;
                const auto match = std::abs(sample(samples, offset) - left) <= tolerance &&
                                   std::abs(sample(samples, offset + 2) - right) <= tolerance;
                matching = match ? matching + 1 : 0;
                if (matching == steady_frames)
                    return true;
            }
            return false;
        }, "Callback output reaches the expected stereo levels for 960 consecutive frames");
        return frame + 1 - steady_frames;
    }

    static int sample(const Bytes &samples, std::size_t offset)
    {
        const auto word = static_cast<int>(little_endian(samples, offset, 2));
        return word < 32768 ? word : word - 65536;
    }

    const PortSpec spec;
    const std::size_t batch_frames;
    const std::thread::id control_thread = std::this_thread::get_id();
    Bytes samples;
    std::atomic<std::size_t> delivered{0};
    std::atomic<std::size_t> calls{0};
    std::size_t last_batch = 0;
    std::atomic<bool> valid{true};
};

std::string diagnostic_entry(const std::string &snapshot, const std::string &prefix)
{
    const auto start = snapshot.find(prefix);
    check(start != std::string::npos, "Expected diagnostic entry is present");
    return snapshot.substr(start, snapshot.find('\n', start) - start);
}

std::string endpoint_entry(const std::string &snapshot, NodeId id)
{
    return diagnostic_entry(snapshot, "{\"id\":" + std::to_string(id.node()) + ",\"type\":");
}

std::size_t queue_counter(const std::string &entry, const char *name)
{
    const auto field = std::string("\"") + name + "\":";
    const auto start = entry.find(field);
    check(start != std::string::npos, "Expected queue counter is present");
    return std::stoull(entry.substr(start + field.size()));
}

void callback_parameters()
{
    AudioCenter audio;
    const AudioCallback ignore = [](const void *, void *, std::size_t, PortSpec, void *) {};
    using Factory = NodeId (AudioCenter::*)(PortSpec, AudioCallback, void *, std::chrono::microseconds);
    for (Factory create : {&AudioCenter::create_icallback, &AudioCenter::create_ocallback})
    {
        expect(audio.commit());
        const auto before = audio.dump_json();
        for (const auto spec : {PortSpec{}, stereo_planar, PortSpec{BandWidth::Full, 0, SampleFormat::S16LE},
                                PortSpec{BandWidth::Full, 2, static_cast<SampleFormat>(100)}})
        {
            check(!(audio.*create)(spec, ignore, nullptr, 10ms), "Reject an invalid PCM specification");
            expect(AudioCenter::last_error(), RetCode::EPARAM);
        }
        check(!(audio.*create)(stereo_pcm, nullptr, nullptr, 10ms), "Reject a null callback");
        expect(AudioCenter::last_error(), RetCode::EPARAM);
        for (const auto period : {-1us, 0us, 20us})
        {
            check(!(audio.*create)(stereo_pcm, ignore, nullptr, period), "Reject periods below one frame");
            expect(AudioCenter::last_error(), RetCode::EPARAM);
        }
        check(!(audio.*create)(stereo_pcm, ignore, nullptr, std::chrono::microseconds::max()),
              "Reject a period that exceeds queue capacity");
        expect(AudioCenter::last_error(), RetCode::TOOLARGE);
        check(audio.dump_json() == before, "Rejected callback parameters leave the graph unchanged");
        for (const auto format : {SampleFormat::S16LE, SampleFormat::S24_3LE, SampleFormat::S24LE,
                                 SampleFormat::S32LE, SampleFormat::F32LE})
        {
            const auto id = (audio.*create)({BandWidth::Full, 2, format}, ignore, nullptr, 21us);
            check(static_cast<bool>(id), "Accept each interleaved format at the first whole-frame period");
            expect(audio.remove(id));
        }
    }
}

void callback_mix_and_diagnostics()
{
    TempWave files("callback-mix");
    const auto input = files.path("-input.wav");
    const auto archive = files.path("-mix.wav");
    write_constant_stereo(input);
    CallbackSource source{stereo_pcm, 480, {4096, -4096}};
    CallbackSink sink(stereo_pcm, 480);
    AudioCenter audio;
    const auto user = audio.create_icallback(stereo_pcm, CallbackSource::fill, &source);
    const auto wave = audio.create_iwav(input, -1);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto swap = audio.create_matrix(BandWidth::Full, ChannelMap::select(2, {1, 0}));
    const auto callback = audio.create_ocallback(stereo_pcm, CallbackSink::receive, &sink);
    const auto wav_sink = audio.create_owav(archive);
    check(user && wave && mix && swap && callback && wav_sink, "Create the callback and WAV mix graph");
    check(audio.output_spec(user) == stereo_planar, "The callback source exposes planar float32 to the graph");
    expect(audio.connect(user, mix[0]));
    expect(audio.connect(wave, mix[1]));
    expect(audio.connect(mix, swap));
    expect(audio.connect(swap, callback));
    expect(audio.connect(swap, wav_sink));
    const auto stopped = audio.dump_json();
    for (const auto id : {user, callback})
        check(stopped.find(backend_state(id, "Stopped")) != std::string::npos, "New callbacks are stopped");
    expect(audio.start());
    const auto first_steady_frame = sink.wait_for_levels(0, 4096, 12288);
    const auto snapshot = audio.dump_json();
    for (const auto &prefix : {std::string("{\"type\":\"UsrCapture\",") + backend_state(user, "Running"),
                               std::string("{\"type\":\"UsrPlayback\",") + backend_state(callback, "Running")})
    {
        const auto backend = diagnostic_entry(snapshot, prefix);
        check(backend.find("\"device\":{\"rate\":48000,\"channels\":2,\"format\":\"S16LE\"},"
                           "\"max_frames\":480,\"fir\":false,\"async\":false") != std::string::npos,
              "Callback diagnostics show fixed PCM, batch size, and synchronous conversion without FIR");
    }
    for (const auto id : {user, callback})
    {
        const auto entry = endpoint_entry(snapshot, id);
        check(entry.find("\"frames\":null") != std::string::npos ||
                  queue_counter(entry, "frames") <= queue_counter(entry, "capacity"),
              "Callback diagnostics show a bounded queue");
        queue_counter(entry, id == user ? "underrun" : "overflow");
    }
    expect(audio.stop());
    expect(audio.remove(wav_sink));
    check(source.valid && sink.valid, "Both callbacks have the declared thread and buffer contract");
    const auto recorded = read_s16(archive, 48000, 2);
    constexpr std::size_t frame_bytes = 4;
    constexpr auto steady_bytes = CallbackSink::steady_frames * frame_bytes;
    const auto first = sink.samples.begin() + first_steady_frame * frame_bytes;
    for (std::size_t offset = 0; offset + steady_bytes <= recorded.size(); offset += frame_bytes)
        if (std::equal(first, first + steady_bytes, recorded.begin() + offset))
            return;
    check(false, "The WAV branch receives the same stereo mix as the callback");
}

void callback_explicit_conversion()
{
    const PortSpec mono_pcm{BandWidth::Wide, 1, SampleFormat::S16LE};
    const PortSpec wide_stereo{BandWidth::Wide, 2, SampleFormat::F32P};
    const PortSpec cd_stereo{BandWidth::CDQuality, 2, SampleFormat::F32P};
    const PortSpec cd_pcm{BandWidth::CDQuality, 2, SampleFormat::S16LE};
    CallbackSource source{mono_pcm, 160, {8192, 0}};
    CallbackSink sink(cd_pcm, 441);
    AudioCenter audio;
    const auto user = audio.create_icallback(mono_pcm, CallbackSource::fill, &source);
    const auto matrix = audio.create_matrix(BandWidth::Wide, ChannelMap::dense(1, 2, {0.75f, -0.375f}));
    const auto up = audio.create_convert(wide_stereo, stereo_planar);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 1);
    const auto down = audio.create_convert(stereo_planar, cd_stereo);
    const auto callback = audio.create_ocallback(cd_pcm, CallbackSink::receive, &sink);
    check(user && matrix && up && mix && down && callback, "Create the explicit channel and rate conversion graph");
    expect(audio.connect(user, up), RetCode::INVFMT);
    expect(audio.connect(matrix, mix), RetCode::INVFMT);
    expect(audio.connect(mix, callback), RetCode::INVFMT);
    expect(audio.connect(user, matrix));
    expect(audio.connect(matrix, up));
    expect(audio.connect(up, mix));
    expect(audio.connect(mix, down));
    expect(audio.connect(down, callback));
    expect(audio.start());
    sink.wait_for_levels(0, 6144, -3072, 1);
    expect(audio.stop());
    check(source.valid && sink.valid, "Conversion keeps each callback's original PCM specification");
}

void callback_live_control()
{
    TempWave files("callback-live-control");
    const auto input = files.path();
    write_constant_stereo(input);
    CallbackSource source{stereo_pcm, 480, {0, 0}};
    CallbackSink sink(stereo_pcm, 480);
    AudioCenter audio;
    const auto high = audio.create_icallback(stereo_pcm, CallbackSource::fill, &source);
    const auto normal = audio.create_iwav(input, -1);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto callback = audio.create_ocallback(stereo_pcm, CallbackSink::receive, &sink);
    check(high && normal && mix && callback, "Create the priority graph");
    expect(audio.connect(normal, mix[1]));
    expect(audio.connect(mix, callback));
    expect(audio.set_high_priority(mix[0], true));
    expect(audio.start());
    sink.wait_for_levels(0, 8192, 8192);
    check(source.calls == 0, "An unconnected callback source has no worker in the plan");
    expect(audio.connect(high, mix[0]));
    expect(audio.commit());
    wait_for([&] { return source.calls > 0; }, "The committed callback source starts");
    sink.wait_for_levels(sink.delivered, 0, 0);
    expect(audio.set_paused(high, true));
    expect(audio.commit());
    const auto paused = source.calls.load();
    sink.wait_for_levels(sink.delivered, 8192, 8192);
    check(source.calls == paused, "Committed pause joins the source and releases high-priority suppression");
    expect(audio.set_paused(high, false));
    expect(audio.commit());
    wait_for([&] { return source.calls > paused; }, "The resumed source starts again");
    sink.wait_for_levels(sink.delivered, 0, 0);
    expect(audio.remove(high));
    const auto removed = source.calls.load();
    sink.wait_for_levels(sink.delivered, 8192, 8192);
    check(source.calls == removed && source.valid && sink.valid, "Removal joins the source during playback");
}

void callback_isolation_and_drain()
{
    struct SlowSink
    {
        CallbackSink sink{stereo_pcm, 336};
        std::promise<void> release;
        std::future<void> ready = release.get_future();
        std::atomic<bool> entered{false};

        static void receive(const void *input, void *output, std::size_t frames, PortSpec spec, void *data)
        {
            auto &self = *static_cast<SlowSink *>(data);
            if (!self.entered.exchange(true) && self.ready.wait_for(5s) != std::future_status::ready)
                self.sink.valid = false;
            CallbackSink::receive(input, output, frames, spec, &self.sink);
        }
    };
    CallbackSource source{stereo_pcm, 480, {4096, -4096}};
    CallbackSink fast(stereo_pcm, 480);
    SlowSink slow;
    std::future<RetCode> pause;
    AudioCenter audio;
    const auto user = audio.create_icallback(stereo_pcm, CallbackSource::fill, &source);
    const auto fast_sink = audio.create_ocallback(stereo_pcm, CallbackSink::receive, &fast);
    const auto slow_sink = audio.create_ocallback(stereo_pcm, SlowSink::receive, &slow, 7ms);
    check(user && fast_sink && slow_sink, "Create two independent playback workers");
    expect(audio.connect(user, fast_sink));
    expect(audio.connect(user, slow_sink));
    expect(audio.start());
    wait_for([&] { return slow.entered.load(); }, "The slow output callback is blocked in its first batch");
    const auto before = tick(audio);
    fast.wait_for_levels(fast.delivered, 4096, -4096);
    wait_for([&] { return queue_counter(endpoint_entry(audio.dump_json(), slow_sink), "overflow") > 0; },
             "Only the blocked branch fills its playback queue");
    const auto snapshot = audio.dump_json();
    const auto slow_entry = endpoint_entry(snapshot, slow_sink);
    const auto queued = queue_counter(slow_entry, "frames");
    check(tick(audio) > before && slow.sink.delivered == 0 && queued == queue_counter(slow_entry, "capacity") &&
              queue_counter(endpoint_entry(snapshot, fast_sink), "overflow") == 0,
          "The clock and fast branch continue while the slow queue remains full");
    expect(audio.set_paused(slow_sink, true));
    expect(audio.disconnect(user, fast_sink));
    pause = std::async(std::launch::async, [&] { return audio.commit(); });
    // Silence on the fast branch proves the new plan is active before the slow worker resumes.
    fast.wait_for_levels(fast.delivered, 0, 0);
    slow.release.set_value();
    expect(pause.get());
    check(source.valid && fast.valid && slow.sink.valid && slow.sink.delivered == 336 + queued &&
              slow.sink.last_batch == queued % 336 && slow.sink.last_batch > 0,
          "Pause returns after the in-flight batch and every queued frame, including the short tail");
    expect(audio.stop());

    CallbackSink tail(stereo_pcm, 480000);
    AudioCenter draining;
    check(static_cast<bool>(draining.create_ocallback(stereo_pcm, CallbackSink::receive, &tail, 10s)),
          "Create a sink whose batch exceeds the test run");
    expect(draining.start());
    advance(draining, 3);
    check(tail.calls == 0, "Steady playback waits for a full batch");
    expect(draining.stop());
    check(tail.calls == 1 && tail.valid && tail.last_batch == tail.delivered &&
              tail.last_batch >= 960 && tail.last_batch < tail.batch_frames && !has_signal(tail.samples),
          "Stop joins the worker after delivering a single short batch of queued silence");
}

void push_pull_parameters()
{
    AudioCenter audio;
    for (const auto create : {&AudioCenter::create_ipush, &AudioCenter::create_opull})
    {
        for (const auto spec : {stereo_planar, PortSpec{BandWidth::Unknown, 2, SampleFormat::S16LE},
                                PortSpec{BandWidth::Full, 0, SampleFormat::S16LE}})
        {
            check(!(audio.*create)(spec, 10ms), "Reject an invalid interleaved PCM specification");
            expect(AudioCenter::last_error(), RetCode::EPARAM);
        }
        for (const auto period : {-1us, 0us, 20us})
        {
            check(!(audio.*create)(stereo_pcm, period), "Reject a period shorter than one frame");
            expect(AudioCenter::last_error(), RetCode::EPARAM);
        }
        check(!(audio.*create)(stereo_pcm, std::chrono::microseconds::max()), "Reject an oversized queue");
        expect(AudioCenter::last_error(), RetCode::TOOLARGE);
        for (const auto format : {SampleFormat::S24LE, SampleFormat::S32LE})
        {
            const auto node = (audio.*create)({BandWidth::Full, 2, format}, 21us);
            check(static_cast<bool>(node), "Accept PCM in four-byte slots at the minimum valid period");
            expect(audio.remove(node));
        }
    }

    TempWave files("push-pull-parameters");
    const auto source = audio.create_ipush(stereo_pcm);
    const auto sink = audio.create_opull(stereo_pcm);
    const auto mixer = audio.create_mixer(BandWidth::Full, 2, 1);
    const auto wav = audio.create_owav(files.path());
    check(source && sink && mixer && wav, "Create the nodes used by the error checks");
    check(audio.output_spec(source) == stereo_planar, "A push source exposes the matching F32P graph port");
    unsigned char pcm[4]{};
    const auto verify = [&](NodeId push_id, NodeId pull_id, unsigned char *data, std::size_t frames, RetCode error) {
        check(audio.push(push_id, data, frames) == 0, "Push returns zero frames");
        expect(AudioCenter::last_error(), error);
        check(audio.pull(pull_id, data, frames) == 0, "Pull returns zero frames");
        expect(AudioCenter::last_error(), error);
    };
    verify(source[1], sink[1], pcm, 1, RetCode::EPORT);
    verify({}, {}, pcm, 1, RetCode::ENODE);
    verify(sink, source, pcm, 1, RetCode::ETYPE);
    verify(mixer, wav, pcm, 1, RetCode::ETYPE);
    verify(source, sink, nullptr, 1, RetCode::EPARAM);
    verify(source, sink, pcm, std::numeric_limits<std::size_t>::max() / sizeof(pcm) + 1, RetCode::TOOLARGE);
    verify(source, sink, nullptr, 0, RetCode::ESTATE);
    expect(audio.connect(source, sink));
    expect(audio.start());
    verify(source, sink, nullptr, 0, RetCode::OK);
    advance(audio, 2);
    check(queue_counter(endpoint_entry(audio.dump_json(), sink), "frames") > 0, "Playback queues frames before pause");
    expect(audio.set_paused(source, true));
    expect(audio.set_paused(sink, true));
    expect(audio.commit());
    verify(source, sink, pcm, 1, RetCode::ESTATE);
    check(queue_counter(endpoint_entry(audio.dump_json(), sink), "frames") == 0, "Pause discards the playback queue");
    expect(audio.remove(source));
    expect(audio.remove(sink));
    verify(source, sink, pcm, 1, RetCode::ENODE);
}

void push_pull_pcm()
{
    const auto frames = std::size_t{12000};
    const auto block = std::size_t{480};
    for (const auto format : {SampleFormat::S16LE, SampleFormat::S24_3LE, SampleFormat::F32LE})
    {
        const PortSpec spec{BandWidth::Full, 2, format};
        const auto width = format == SampleFormat::S16LE ? 2u : format == SampleFormat::S24_3LE ? 3u : 4u;
        const auto frame_bytes = 2 * width;
        Bytes pcm(frames * frame_bytes);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto phase = static_cast<std::uint32_t>(frame % 1024);
            const auto left = format == SampleFormat::F32LE ? 0x3e800000u + (phase << 11) : (0x1000u + phase);
            const auto right = format == SampleFormat::F32LE ? 0xbe800000u + (phase << 11) : (0xc000u - phase);
            const auto shift = format == SampleFormat::S24_3LE ? 8u : 0u;
            for (unsigned byte = 0; byte < width; ++byte)
            {
                pcm[frame * frame_bytes + byte] = static_cast<unsigned char>((left << shift) >> (8 * byte));
                pcm[frame * frame_bytes + width + byte] = static_cast<unsigned char>((right << shift) >> (8 * byte));
            }
        }
        AudioCenter audio;
        const auto source = audio.create_ipush(spec, 40ms);
        const auto mix = audio.create_mixer(BandWidth::Full, 2, 1);
        const auto swap = audio.create_matrix(BandWidth::Full, ChannelMap::select(2, {1, 0}));
        const auto sink = audio.create_opull(spec, 100ms);
        check(source && mix && swap && sink, "Create each same-rate PCM route");
        expect(audio.connect(source, mix));
        expect(audio.connect(mix, swap));
        expect(audio.connect(swap, sink));
        expect(audio.start());
        const auto offer = [&](std::size_t offset) {
            const auto count = audio.push(source, pcm.data() + offset * frame_bytes, frames - offset);
            expect(AudioCenter::last_error());
            return count;
        };
        auto sent = offer(0);
        check(sent > block && sent < frames, "A burst crosses scratch slices and stops at the queue limit");
        const auto immediately_accepted = offer(sent);
        sent += immediately_accepted;
        bool full = immediately_accepted == 0;
        advance(audio, 3);
        const auto snapshot = audio.dump_json();
        for (const auto &prefix : {std::string("{\"type\":\"PushSource\",") + backend_state(source, "Running"),
                                   std::string("{\"type\":\"PullSink\",") + backend_state(sink, "Running")})
        {
            const auto entry = diagnostic_entry(snapshot, prefix);
            check(entry.find("\"max_frames\":480,\"fir\":false,\"async\":false,\"active\":false") !=
                      std::string::npos,
                  "Passive diagnostics show the slice limit and synchronous PCM without FIR");
        }
        const auto source_entry = endpoint_entry(snapshot, source);
        const auto queued = queue_counter(source_entry, "frames");
        check(queued > block && queued <= queue_counter(source_entry, "capacity"), "Capture reports live queue fill");
        queue_counter(source_entry, "underrun");
        queue_counter(endpoint_entry(snapshot, sink), "overflow");

        Bytes out(4 * block * frame_bytes);
        std::size_t received = 0;
        bool sliced_pull = false;
        wait_for([&] {
            if (sent < frames)
            {
                const auto count = offer(sent);
                full |= count == 0;
                sent += count;
            }
            std::size_t count;
            do
            {
                std::fill(out.begin(), out.end(), 0xa5);
                count = audio.pull(sink, out.data(), out.size() / frame_bytes);
                expect(AudioCenter::last_error());
                sliced_pull |= count > block;
                check(std::all_of(out.begin() + count * frame_bytes, out.end(),
                                  [](unsigned char byte) { return byte == 0xa5; }), "Pull leaves the suffix intact");
                for (std::size_t frame = 0; frame < count; ++frame)
                {
                    const auto actual = out.begin() + frame * frame_bytes;
                    if (std::all_of(actual, actual + frame_bytes, [](unsigned char byte) { return byte == 0; }))
                        continue;
                    check(received < frames, "The burst does not duplicate frames");
                    const auto expected = pcm.begin() + received * frame_bytes;
                    check(std::equal(actual, actual + width, expected + width) &&
                              std::equal(actual + width, actual + frame_bytes, expected),
                          "Burst chunks preserve every sample in order through the channel swap");
                    ++received;
                }
            } while (count);
            return received == frames;
        }, "Receive the entire pushed PCM burst");
        check(sent == frames && full && sliced_pull, "Short writes resume at the correct offset and pulls span slices");
    }
}

void push_pull_conversion()
{
    TempWave files("push-pull-conversion");
    const auto input = files.path("-input.wav");
    const auto archive = files.path("-mix.wav");
    write_constant_stereo(input);
    const PortSpec mono{BandWidth::Wide, 1, SampleFormat::S16LE};
    const PortSpec wide_stereo{BandWidth::Wide, 2, SampleFormat::F32P};
    AudioCenter audio;
    const auto push = audio.create_ipush(mono, 80ms);
    const auto matrix = audio.create_matrix(BandWidth::Wide, ChannelMap::dense(1, 2, {1.0f, -1.0f}));
    const auto convert = audio.create_convert(wide_stereo, stereo_planar);
    const auto wav = audio.create_iwav(input, -1);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto pull = audio.create_opull(stereo_pcm);
    const auto writer = audio.create_owav(archive);
    check(push && matrix && convert && wav && mix && pull && writer, "Create the heterogeneous mix graph");
    expect(audio.connect(push, convert), RetCode::INVFMT);
    expect(audio.connect(matrix, mix), RetCode::INVFMT);
    expect(audio.connect(push, matrix));
    expect(audio.connect(matrix, convert));
    expect(audio.connect(convert, mix[0]));
    expect(audio.connect(wav, mix[1]));
    expect(audio.connect(mix, pull));
    expect(audio.connect(mix, writer));
    expect(audio.start());
    Bytes pcm(1600 * 2);
    for (std::size_t byte = 1; byte < pcm.size(); byte += 2)
        pcm[byte] = 0x10;
    check(audio.push(push, pcm.data(), 1600) == 1600, "Queue 100 ms of mono PCM for conversion");
    Bytes out(1920 * 4), steady;
    wait_for([&] {
        const auto frames = audio.pull(pull, out.data(), out.size() / 4);
        expect(AudioCenter::last_error());
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto offset = frame * 4;
            if (std::abs(CallbackSink::sample(out, offset) - 12288) > 1 ||
                std::abs(CallbackSink::sample(out, offset + 2) - 4096) > 1)
                steady.clear();
            else
                steady.insert(steady.end(), out.begin() + offset, out.begin() + offset + 4);
            if (steady.size() == 960 * 4)
                return true;
        }
        return false;
    }, "The converted mono signal mixes with stereo WAV at the expected levels");
    expect(audio.stop());
    expect(audio.remove(writer));
    const auto recorded = read_s16(archive, 48000, 2);
    for (std::size_t offset = 0; offset + steady.size() <= recorded.size(); offset += 4)
        if (std::equal(steady.begin(), steady.end(), recorded.begin() + offset))
            return;
    check(false, "WAV and pull branches receive identical converted stereo samples");
}

void push_pull_priority()
{
    CallbackSource normal{stereo_pcm, 480, {8192, 8192}};
    AudioCenter audio;
    const auto voice = audio.create_ipush(stereo_pcm);
    const auto background = audio.create_icallback(stereo_pcm, CallbackSource::fill, &normal);
    const auto mix = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto sink = audio.create_opull(stereo_pcm, 40ms);
    check(voice && background && mix && sink, "Create the high-priority push route");
    expect(audio.connect(voice, mix[0]));
    expect(audio.connect(background, mix[1]));
    expect(audio.connect(mix, sink));
    expect(audio.set_high_priority(mix[0], true));
    expect(audio.start());
    advance(audio, 4);
    Bytes out(8 * 480 * 4);
    while (audio.pull(sink, out.data(), out.size() / 4))
        expect(AudioCenter::last_error());
    expect(AudioCenter::last_error());
    Bytes voice_pcm(2 * 480 * 4, 0);
    check(audio.push(voice, voice_pcm.data(), 2 * 480) == 2 * 480, "Two intentional silent blocks enter the high-priority queue");
    std::vector<int> levels;
    wait_for([&] {
        const auto frames = audio.pull(sink, out.data(), out.size() / 4);
        expect(AudioCenter::last_error());
        for (std::size_t frame = 0; frame < frames; ++frame)
            levels.push_back(CallbackSink::sample(out, frame * 4));
        return levels.size() >= 6 * 480;
    }, "Observe suppression and recovery after the push queue drains");
    const auto fade = std::find_if(levels.begin(), levels.end(), [](int level) { return level > 0 && level < 8192; });
    check(levels.end() - fade >= 4 * 480, "The normal input begins fading on the first pushed block");
    for (int frame = 0; frame < 480; ++frame)
    {
        check(std::abs(fade[frame] - 8192 * (479 - frame) / 480) <= 1 &&
                  fade[480 + frame] == 0 &&
                  std::abs(fade[960 + frame] - 8192 * (frame + 1) / 480) <= 1 &&
                  fade[1440 + frame] == 8192,
              "The two active blocks suppress the normal input and its first empty block fades back in");
    }
    expect(audio.stop());
    check(normal.valid, "The background callback remains active through recovery");
}

void push_pull_concurrency()
{
    AudioCenter audio;
    const auto source = audio.create_ipush(stereo_pcm);
    const auto matrix = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
    const auto sink = audio.create_opull(stereo_pcm);
    check(source && matrix && sink, "Create the concurrent push/pull route");
    expect(audio.connect(source, matrix));
    expect(audio.connect(matrix, sink));
    expect(audio.start());

    struct Counts
    {
        std::atomic<std::size_t> frames{0};
        std::atomic<std::size_t> stopped{0};
    };
    Counts sent, received;
    std::atomic<bool> done{false};
    const auto push = [&](void *pcm, std::size_t frames) { return audio.push(source, pcm, frames); };
    const auto pull = [&](void *pcm, std::size_t frames) { return audio.pull(sink, pcm, frames); };
    const auto run = [&](const auto &transfer, Counts &counts) {
        unsigned char pcm[4 * 247]{};
        while (!done)
        {
            counts.frames += transfer(pcm, 247);
            const auto status = AudioCenter::last_error();
            if (status == RetCode::ENODE)
            {
                check(transfer(pcm, 247) == 0, "Subsequent calls return zero after removal");
                expect(AudioCenter::last_error(), RetCode::ENODE);
                return;
            }
            if (status == RetCode::ESTATE)
                ++counts.stopped;
            else
                expect(status);
            std::this_thread::yield();
        }
    };
    std::future<void> producer, second_producer, consumer;
    struct StopTransfers
    {
        std::atomic<bool> &done;
        ~StopTransfers() { done = true; }
    } stop{done};
    producer = std::async(std::launch::async, [&] { run(push, sent); });
    second_producer = std::async(std::launch::async, [&] { run(push, sent); });
    consumer = std::async(std::launch::async, [&] { run(pull, received); });
    wait_for([&] { return sent.frames > 0 && received.frames > 0; }, "Both workers transfer frames");
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        expect(audio.set_gain(matrix, iteration % 2 ? 0.5f : 1.0f));
        const auto unrelated = audio.create_mixer(BandWidth::Full, 1, 1);
        check(static_cast<bool>(unrelated), "An unrelated node can be created during transfers");
        expect(audio.remove(unrelated));
        expect(audio.disconnect(matrix));
        expect(audio.set_paused(source, true));
        expect(audio.set_paused(sink, true));
        expect(audio.commit());
        const auto stopped_pushes = sent.stopped.load();
        const auto stopped_pulls = received.stopped.load();
        wait_for([&] { return sent.stopped > stopped_pushes && received.stopped > stopped_pulls; },
                 "Both workers observe the committed pause");
        const auto sent_before = sent.frames.load();
        const auto received_before = received.frames.load();
        expect(audio.connect(source, matrix));
        expect(audio.set_paused(source, false));
        expect(audio.set_paused(sink, false));
        expect(audio.commit());
        wait_for([&] { return sent.frames > sent_before && received.frames > received_before; },
                 "Both workers resume after reconnection");
    }
    expect(audio.stop());
    const auto stopped_pushes = sent.stopped.load();
    const auto stopped_pulls = received.stopped.load();
    wait_for([&] { return sent.stopped > stopped_pushes && received.stopped > stopped_pulls; },
             "Both workers observe stop");
    expect(audio.start());
    expect(audio.remove(source));
    expect(audio.remove(sink));
    wait_for([&] {
        return producer.wait_for(0ms) == std::future_status::ready &&
               second_producer.wait_for(0ms) == std::future_status::ready &&
               consumer.wait_for(0ms) == std::future_status::ready;
    }, "Each worker observes removal");
    expect(AudioCenter::last_error());
    producer.get();
    second_producer.get();
    consumer.get();
}

void push_pull_during_control_wait()
{
    struct Gate
    {
        std::promise<void> release;
        std::future<void> ready = release.get_future();
        std::atomic<bool> entered{false};

        static void wait(const void *, void *, std::size_t, PortSpec, void *data)
        {
            auto &gate = *static_cast<Gate *>(data);
            if (!gate.entered.exchange(true))
                gate.ready.wait_for(5s);
        }
    } gate;
    std::future<RetCode> commit;
    AudioCenter audio;
    const auto source = audio.create_ipush(stereo_pcm);
    const auto sink = audio.create_opull(stereo_pcm);
    const auto idle = audio.create_ipush(stereo_pcm);
    const auto blocked = audio.create_ocallback(stereo_pcm, Gate::wait, &gate);
    const auto mixer = audio.create_mixer(BandWidth::Full, 2, 1);
    check(source && sink && idle && blocked && mixer, "Create independent passive and callback branches");
    expect(audio.connect(source, sink));
    expect(audio.start());
    unsigned char one_frame[4]{};
    check(audio.push(idle, one_frame, 1) == 0, "An unused push source is excluded from the plan");
    expect(AudioCenter::last_error(), RetCode::ESTATE);
    wait_for([&] { return gate.entered.load(); }, "The callback holds its first playback batch");
    expect(audio.set_paused(blocked, true));
    commit = std::async(std::launch::async, [&] { return audio.commit(); });
    check(commit.wait_for(30ms) == std::future_status::timeout, "The commit waits for the callback");
    check(audio.push(source, one_frame, 1) == 1, "Push proceeds while commit is held");
    expect(AudioCenter::last_error());
    check(audio.pull(sink, one_frame, 1) == 1, "Pull proceeds while commit is held");
    expect(AudioCenter::last_error());
    check(audio.push({}, one_frame, 1) == 0, "A missing handle is rejected while commit is held");
    expect(AudioCenter::last_error(), RetCode::ENODE);
    check(audio.push(mixer, one_frame, 1) == 0, "A mixer handle is rejected while commit is held");
    expect(AudioCenter::last_error(), RetCode::ETYPE);
    check(audio.pull(blocked, one_frame, 1) == 0, "A callback handle is rejected while commit is held");
    expect(AudioCenter::last_error(), RetCode::ETYPE);
    check(commit.wait_for(0ms) == std::future_status::timeout, "Transfers and handle errors bypass the control queue");
    gate.release.set_value();
    expect(commit.get());
}

bool runs_before(const std::string &snapshot, NodeId first, NodeId second)
{
    const auto order_start = snapshot.find("\"plan_order\":[");
    check(order_start != std::string::npos, "Expect the execution order in diagnostics");
    const auto first_id = order_start + sizeof("\"plan_order\":[") - 1;
    const auto order_end = snapshot.find(']', first_id);
    check(order_end != std::string::npos, "Expect the complete execution order");
    const auto order = ',' + snapshot.substr(first_id, order_end - first_id) + ',';
    const auto first_position = order.find(',' + std::to_string(first.node()) + ',');
    const auto second_position = order.find(',' + std::to_string(second.node()) + ',');
    return second_position != std::string::npos && first_position < second_position;
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

void wav_routing_and_conversion()
{
    const std::string input = AEREBORN_TEST_DATA "/dukou_excerpt.wav";
    TempWave files("audio-center-routing");
    const auto stereo = files.path("-stereo.wav");
    const auto mono = files.path("-mono.wav");
    const auto upmixed = files.path("-upmixed.wav");
    const auto added = files.path("-added.wav");
    const PortSpec cd_mono{BandWidth::CDQuality, 1, SampleFormat::S16LE};
    const PortSpec stereo_graph{BandWidth::Full, 2, SampleFormat::F32P};
    const PortSpec mono_graph{BandWidth::Full, 1, SampleFormat::F32P};
    const PortSpec cd_mono_graph{BandWidth::CDQuality, 1, SampleFormat::F32P};
    AudioCenter audio;
    const auto source = audio.create_iwav(input);
    const auto mixer = audio.create_mixer(BandWidth::Full, 2, 2);
    const auto matrix = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
    const auto out_stereo = audio.create_owav(stereo);
    const auto out_mono = audio.create_owav(mono, cd_mono);
    check(source && mixer && matrix && out_stereo && out_mono, "Create all WAV routes");
    check(audio.output_spec(source) == stereo_graph, "WAV sources expose their graph-side specifications");
    check(!audio.output_spec(source[1]).valid(), "An absent output port has no specification");
    expect(AudioCenter::last_error(), RetCode::EPORT);
    check(!audio.create_convert(stereo_graph, mono_graph), "Convert preserves the channel count");
    expect(AudioCenter::last_error(), RetCode::EPARAM);
    const auto downmix = audio.create_matrix(BandWidth::Full, ChannelMap::to_mono(2));
    const auto downsample = audio.create_convert(mono_graph, cd_mono_graph);
    const auto decode = audio.create_convert({BandWidth::Full, 2, SampleFormat::F32LE}, stereo_graph);
    check(downmix && downsample && decode, "Create explicit channel, rate and format converters");
    const auto before_rejected_links = audio.dump_json();
    expect(audio.connect(matrix, decode), RetCode::INVFMT);
    expect(audio.connect(matrix, downsample), RetCode::INVFMT);
    expect(audio.connect(downmix, out_mono), RetCode::INVFMT);
    check(audio.dump_json() == before_rejected_links, "Incompatible connections leave the configured graph intact");
    expect(audio.remove(decode));
    expect(audio.connect(source, mixer[1]));
    expect(audio.connect(mixer, matrix));
    expect(audio.connect(matrix, out_stereo));
    expect(audio.connect(matrix, downmix));
    expect(audio.connect(downmix, downsample));
    expect(audio.connect(downsample, out_mono));
    expect(audio.start());
    expect(audio.start(), RetCode::BUSY);

    const auto snapshot = audio.dump_json();
    check(runs_before(snapshot, downmix, downsample), "Downmix precedes resampling");
    check(snapshot.find("\"file\":\"dukou_excerpt.wav\"") != std::string::npos &&
              snapshot.find("\"type\":\"Convert\"") != std::string::npos &&
              snapshot.find("\"to\":[" + std::to_string(mixer.node()) + ",1],\"in_plan\":true") != std::string::npos,
          "JSON exposes explicit converters, exact destination ports and WAV file names");
    wait_for([&] { return audio.dump_json().find("\"error\":\"EOS\"") != std::string::npos; },
             "The finite input reaches EOS", 4s);
    const auto added_source = audio.create_iwav(input, -1);
    const auto added_sink = audio.create_owav(added);
    check(added_source && added_sink, "Create an independent branch while the engine is running");
    expect(audio.connect(added_source, added_sink));
    expect(audio.set_paused(out_stereo, true));
    const auto stereo_node = "\"id\":" + std::to_string(out_stereo.node()) + ",\"type\":\"Playback\",\"in_plan\":";
    const auto stereo_link = "\"from\":[" + std::to_string(matrix.node()) + ",0],\"to\":[" +
                             std::to_string(out_stereo.node()) + ",0],\"in_plan\":";
    const auto added_node = "\"id\":" + std::to_string(added_source.node()) + ",\"type\":\"Capture\",\"in_plan\":";
    const auto drafted = audio.dump_json();
    check(drafted.find(stereo_node + "true,\"paused\":true") != std::string::npos &&
              drafted.find(stereo_link + "true") != std::string::npos &&
              drafted.find(added_node + "false,\"paused\":false") != std::string::npos,
          "JSON shows pending pause and the newly configured branch alongside the committed plan");
    expect(audio.commit());
    const auto published = audio.dump_json();
    check(published.find(stereo_node + "false,\"paused\":true") != std::string::npos &&
              published.find(stereo_link + "false") != std::string::npos &&
              published.find(added_node + "true,\"paused\":false") != std::string::npos,
          "JSON retains the paused branch and updates plan membership after commit");
    check(published.find("\"error\":\"EOS\"") != std::string::npos &&
              published.find(backend_state(added_source, "Running")) != std::string::npos,
          "The new branch starts while the original capture stays at EOS");
    advance(audio, 8);
    check(audio.dump_json().find("\"error\":\"EOS\"") != std::string::npos,
          "The original capture keeps its EOS status while the new branch runs");
    expect(audio.stop());
    expect(audio.stop());
    expect(audio.remove(out_stereo));
    expect(audio.remove(out_mono));
    check(audio.output_spec(downsample) == cd_mono_graph, "Removing a sink keeps the explicit converter");
    expect(audio.remove(added_sink));
    expect(audio.remove(added_source));
    check(has_signal(read_s16(stereo, 48000, 2)), "Stereo branch contains audio");
    check(has_signal(read_s16(mono, 44100, 1)), "Downmixed and resampled branch contains audio");
    check(has_signal(read_s16(added, 48000, 2)), "The branch added during runtime records audio");

    const auto mono_source = audio.create_iwav(mono);
    const auto stereo_sink = audio.create_owav(upmixed);
    const auto upsample = audio.create_convert(cd_mono_graph, mono_graph);
    const auto upmix = audio.create_matrix(BandWidth::Full, ChannelMap::select(1, {0, 0}));
    check(mono_source && stereo_sink && upsample && upmix, "Create the reverse specification route");
    expect(audio.connect(mono_source, upsample));
    expect(audio.connect(upsample, upmix));
    expect(audio.connect(upmix, stereo_sink));
    expect(audio.commit());
    const auto stopped = audio.dump_json();
    check(stopped.find("\"engine\":{\"state\":\"Ready\"") != std::string::npos &&
              stopped.find(backend_state(mono_source, "Stopped")) != std::string::npos &&
              stopped.find(backend_state(stereo_sink, "Stopped")) != std::string::npos &&
              stopped.find("\"state\":\"Running\"") == std::string::npos,
          "A stopped engine can publish a new plan without starting its backends");
    expect(audio.start());
    check(runs_before(audio.dump_json(), upsample, upmix), "Mono resampling precedes channel duplication");
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
    const auto downstream = audio.create_matrix(BandWidth::Full, ChannelMap::identity(2));
    const auto sink = audio.create_owav(output);
    const auto monitor_sink = audio.create_owav(monitor);
    check(source && mix && gain && downstream && sink && monitor_sink, "Create the live graph");
    expect(audio.connect(source, mix));
    expect(audio.connect(source, mix[1]));
    expect(audio.connect(mix, gain));
    expect(audio.connect(gain, sink));
    expect(audio.connect(gain, downstream));
    expect(audio.connect(downstream, gain), RetCode::INPUTBUSY);
    expect(audio.connect(gain, mix), RetCode::INPUTBUSY);
    expect(audio.disconnect(source, mix));
    expect(audio.connect(downstream, mix), RetCode::CYCLE);
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
    check(audio.dump_json().find("\"high\":true,\"muted\":false") != std::string::npos,
          "Report the live priority setting");
    expect(audio.disconnect(sink));
    expect(audio.commit());
    advance(audio, 8);
    expect(audio.set_paused(sink, true));
    expect(audio.commit());
    check(audio.dump_json().find("\"id\":" + std::to_string(sink.node()) +
                                 ",\"type\":\"Playback\",\"in_plan\":false,\"paused\":true") != std::string::npos,
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
    const auto paused = audio.dump_json();
    check(paused.find(backend_state(monitor_sink, "Stopped")) != std::string::npos,
          "A paused recorder's backend stops");
    expect(audio.set_paused(monitor_sink, false));
    expect(audio.commit());
    const auto resumed = audio.dump_json();
    check(resumed.find(backend_state(source, "Running")) != std::string::npos &&
              resumed.find(backend_state(monitor_sink, "Running")) != std::string::npos,
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
                check(!audio.dump_json().empty(), "Concurrent diagnostics are available");
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
                      {"wav_routing_and_conversion", wav_routing_and_conversion},
                      {"live_control_and_cleanup", live_control_and_cleanup},
                      {"concurrency_and_teardown", concurrency_and_teardown},
                      {"callback_parameters", callback_parameters},
                      {"callback_mix_and_diagnostics", callback_mix_and_diagnostics},
                      {"callback_explicit_conversion", callback_explicit_conversion},
                      {"callback_live_control", callback_live_control},
                      {"callback_isolation_and_drain", callback_isolation_and_drain},
                      {"push_pull_parameters", push_pull_parameters},
                      {"push_pull_pcm", push_pull_pcm},
                      {"push_pull_conversion", push_pull_conversion},
                      {"push_pull_priority", push_pull_priority},
                      {"push_pull_concurrency", push_pull_concurrency},
                      {"push_pull_during_control_wait", push_pull_during_control_wait}});
}
