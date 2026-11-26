#ifndef AEREBORN_TESTS_IO_FIXTURE_H
#define AEREBORN_TESTS_IO_FIXTURE_H

#include "audio_fixture.h"
#include "backend/backend.h"
#include "backend/wav_file.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace aereborn
{
namespace test
{

template <class Ready> void wait_for(Ready ready, const char *message)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(3);
    while (!ready())
    {
        check(steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(milliseconds(1));
    }
}

// Observe the same queue metrics used by the backend while the test clock is parked.
class QueueState : private Backend
{
  public:
    static std::size_t queued(const Backend &backend) { return (backend.*&QueueState::available)(); }
    static std::size_t target(const Backend &backend) { return (backend.*&QueueState::target_frames)(); }
    static std::size_t capture_space(const Backend &backend) { return (backend.*&QueueState::capture_free_frames)(); }
    static bool primed(const Backend &backend) { return (backend.*&QueueState::steady)(); }
};

inline void wait_for_capture(const Backend &source, std::size_t frames)
{
    wait_for([&] { return QueueState::queued(source) >= frames; }, "Capture supplies the next graph block");
}

inline void wait_for_playback(const Backend &sink, std::size_t frames)
{
    wait_for([&] { return QueueState::queued(sink) + frames <= 2 * QueueState::target(sink); },
             "Playback has room for the next graph block");
}

class TempWave
{
  public:
    explicit TempWave(const char *name)
        : stem_(std::string("aereborn-") + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
    {
    }
    ~TempWave()
    {
        for (const auto &path : paths_)
            std::remove(path.c_str());
    }
    TempWave(const TempWave &) = delete;
    TempWave &operator=(const TempWave &) = delete;

    std::string path(const char *suffix = ".wav")
    {
        paths_.push_back(stem_ + suffix);
        return paths_.back();
    }

  private:
    const std::string stem_;
    std::vector<std::string> paths_;
};

struct Pcm
{
    Pcm(PortSpec spec, std::size_t frames)
        : spec(spec), frames(frames), bytes(frames * spec.channels * sample_bytes(spec.format))
    {
    }

    IBBlock input(std::size_t first, std::size_t count) const
    {
        return {bytes.data() + first * spec.channels * sample_bytes(spec.format), spec, count};
    }
    OBBlock output(std::size_t first, std::size_t count)
    {
        return {bytes.data() + first * spec.channels * sample_bytes(spec.format), spec, count};
    }
    bool copy_from(std::size_t first, IBBlock block) noexcept
    {
        if (block.spec() != spec || first > frames || block.frames() > frames - first)
            return false;
        const auto source = block.byte_view();
        if (!source.empty())
            std::memcpy(output(first, block.frames()).byte_view().data(), source.data(), source.size());
        return true;
    }
    float sample(std::size_t frame, std::size_t ch = 0) const
    {
        return read_f32le(bytes.data() + (frame * spec.channels + ch) * 4);
    }
    void set(std::size_t frame, std::size_t ch, float value)
    {
        write_f32le(bytes.data() + (frame * spec.channels + ch) * 4, value);
    }

    PortSpec spec;
    std::size_t frames;
    std::vector<std::uint8_t> bytes;
};

inline Pcm read_wave(const std::string &path, PortSpec spec, std::size_t frames)
{
    WavFile file;
    throw_if_error(file.open_read(path));
    check(file.spec() == spec && file.frame_count() == frames, "WAV has the expected format and frame count");
    Pcm pcm(spec, frames);
    check(file.read(pcm.output(0, frames)).value_or_throw() == frames, "WAV provides every declared frame");
    return pcm;
}

inline void write_wave(const std::string &path, const Pcm &pcm)
{
    WavFile file;
    throw_if_error(file.open_write(path, pcm.spec, WavFile::WriteMode::Exclusive));
    throw_if_error(file.write(pcm.input(0, pcm.frames)));
    throw_if_error(file.close());
}

} // namespace test
} // namespace aereborn
#endif
