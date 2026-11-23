#ifndef AEREBORN_BACKEND_WAV_FILE_H
#define AEREBORN_BACKEND_WAV_FILE_H

#include "core/types.h"
#include <cstdio>
#include <string>

namespace aereborn
{

/// @brief Read or write one RIFF/WAVE file using frame offsets.
/// @note Serialize access. Unsigned 8-bit PCM and 64-bit float are read as F32LE.
class WavFile
{
  public:
    enum class WriteMode { Truncate, Exclusive };

    WavFile() = default;
    ~WavFile();
    WavFile(const WavFile &) = delete;
    WavFile &operator=(const WavFile &) = delete;

    /// @brief Open a WAV file at frame zero. Invalid input leaves the current reader usable.
    RetCode open_read(const std::string &path) noexcept;
    /// @brief Open a file for writing. Exclusive returns Busy for an existing path.
    /// @pre Close the current file before opening an output.
    RetCode open_write(const std::string &path, PortSpec spec, WriteMode mode) noexcept;
    /// @brief Close the file and finalize the header when writing.
    RetCode close() noexcept;
    /// @brief Flush buffered output to the operating system and report any write error.
    RetCode flush() noexcept;
    /// @brief Report an I/O error or NotReady if there is no open file.
    RetCode status() const noexcept;
    /// @brief Report whether the sample format is supported for RIFF/WAVE output.
    static bool supports_write_format(SampleFormat format) noexcept;
    /// @brief Return the RIFF frame limit for a supported output specification.
    static Result<std::size_t> max_write_frames(PortSpec spec) noexcept;

    /// @brief Return the interleaved PCM specification used for I/O.
    PortSpec spec() const noexcept { return spec_; }
    /// @brief Return the number of frames in the open file.
    std::size_t frame_count() const noexcept { return frame_count_; }
    /// @brief Return the zero-based index of the next frame to read or write.
    std::size_t tell() const noexcept { return position_; }
    /// @brief Seek to an existing frame or to the end of the data.
    RetCode seek(std::size_t frame) noexcept;
    /// @brief Read up to output.frames() frames, returning zero at EOF.
    Result<std::size_t> read(OBBlock output) noexcept;
    /// @brief Write an interleaved block at the current frame position.
    RetCode write(IBBlock input) noexcept;

  private:
    std::FILE *file_ = nullptr;
    bool writing_ = false;
    PortSpec spec_;
    std::uint64_t data_offset_ = 0;
    std::size_t file_frame_bytes_ = 0;
    std::size_t frame_count_ = 0;
    std::size_t position_ = 0;
    std::size_t write_limit_ = 0;
};

} // namespace aereborn

#endif
