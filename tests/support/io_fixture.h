#ifndef AEREBORN_TESTS_IO_FIXTURE_H
#define AEREBORN_TESTS_IO_FIXTURE_H

#include "audio_fixture.h"
#include "backend/backend.h"
#include "backend/wav_file.h"
#include <cstring>
#include <string>
#include <vector>

namespace aereborn
{
namespace test
{

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
