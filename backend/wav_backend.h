#ifndef AEREBORN_BACKEND_WAV_BACKEND_H
#define AEREBORN_BACKEND_WAV_BACKEND_H

#include "backend.h"
#include "wav_file.h"
#include <string>

namespace aereborn
{

/// @brief Graph-clocked RIFF/WAVE source. Reports RetCode::EOS when all passes have been read.
/// @note Unsigned 8-bit PCM and 64-bit float files are exposed as F32LE.
/// @note Restart resumes at the next file frame to read. Rebinding opens the file from the beginning.
class WavCapture final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "WavCapture"; }

    /// @brief Number of additional passes after the first one.
    struct Repeat
    {
        std::size_t count;
    };

    /// @brief Play once or loop indefinitely.
    explicit WavCapture(std::string path, bool loop);
    /// @brief Play once and then repeat the requested number of times.
    explicit WavCapture(std::string path, Repeat repeats);
    ~WavCapture() override { unbind(); }

    /// @brief Open the input to obtain its PCM specification. The next bind reuses the reader.
    /// @pre Call on the control side before binding.
    Result<PortSpec> probe() noexcept;

    using ActiveBackend::bind;
    void bind(Playback &) = delete;

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds period) override;
    void on_close() noexcept override;
    Result<std::size_t> on_read(OBBlock output) noexcept override;

    std::string path_;
    bool loop_ = false;
    std::size_t repeats_ = 0;
    WavFile file_;
    std::size_t repeats_left_ = 0;
};

/// @brief Graph-clocked RIFF/WAVE sink supporting S16LE, S24_3LE, S32LE and F32LE.
/// @note Stop drains and flushes the audio. Unbind and rotation finalize the RIFF and data lengths.
class WavPlayback final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "WavPlayback"; }

    /// @brief Record to one file, truncating its existing contents at bind time.
    /// @throws Error If the format is unsupported for WAV output.
    explicit WavPlayback(std::string path, SampleFormat format);
    /// @brief Record numbered segments of the given audio duration. Zero selects a single file.
    /// @param write_mode Open mode for a single file.
    /// @note Numbered segments are created exclusively and existing numbers are skipped.
    /// @throws Error If the format is unsupported for WAV output or the duration is negative.
    WavPlayback(std::string path, SampleFormat format, std::chrono::seconds segment_duration,
                WavFile::WriteMode write_mode = WavFile::WriteMode::Truncate);
    ~WavPlayback() override { unbind(); }
    using ActiveBackend::bind;
    void bind(Capture &) = delete;

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds period) override;
    RetCode on_bind_ready(PortSpec spec) noexcept override;
    RetCode on_start() noexcept override;
    void on_stop() noexcept override;
    void on_close() noexcept override;
    RetCode on_write(IBBlock input) noexcept override;
    RetCode open_file(PortSpec spec) noexcept;

    std::string path_;
    const SampleFormat format_;
    const std::chrono::seconds segment_duration_;
    const WavFile::WriteMode write_mode_;
    WavFile file_;
    std::string segment_path_;
    std::size_t segment_number_offset_ = 0;
    std::uint32_t segment_index_ = 0;
    std::size_t segment_frames_ = 0;
};

} // namespace aereborn

#endif
