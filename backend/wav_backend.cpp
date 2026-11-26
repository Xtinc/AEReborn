#include "wav_backend.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace aereborn
{
namespace
{

constexpr auto segment_number_width = std::numeric_limits<std::uint32_t>::digits10 + 1;

} // namespace

WavCapture::WavCapture(std::string path, bool loop)
    : ActiveBackend(false), path_(std::move(path)), loop_(loop)
{
}

WavCapture::WavCapture(std::string path, Repeat repeats)
    : ActiveBackend(false), path_(std::move(path)), repeats_(repeats.count)
{
}

Result<PortSpec> WavCapture::probe() noexcept
{
    const auto status = file_.status();
    const auto opened = status == RetCode::ESTATE ? file_.open_read(path_) : status;
    if (!opened)
        return opened;
    return file_.spec();
}

Result<PortSpec> WavCapture::on_open(bool capture, PortSpec, std::chrono::microseconds period)
{
    if (!capture)
        return RetCode::EPARAM;
    const auto input = probe();
    if (!input)
        return input.ret_code();
    const auto spec = input.value();
    const auto count = frames_in_period(spec.bandwidth, period);
    if (!count)
        return count.ret_code();

    repeats_left_ = repeats_;
    set_device_max_frames(count.value());
    return spec;
}

void WavCapture::on_close() noexcept
{
    file_.close();
}

Result<std::size_t> WavCapture::on_read(OBBlock output) noexcept
{
    const auto spec = output.spec();
    const auto width = spec.channels * sample_bytes(spec.format);
    auto *dest = output.byte_view().data();
    std::size_t frames = 0;
    while (frames < output.frames())
    {
        if (file_.tell() == file_.frame_count())
        {
            if (!file_.frame_count() || (!loop_ && !repeats_left_))
                break;
            const auto code = file_.seek(0);
            if (!code)
                return code;
            if (!loop_)
                --repeats_left_;
        }
        const auto read = file_.read({dest + frames * width, spec, output.frames() - frames});
        if (!read)
            return read;
        frames += read.value();
    }
    if (!frames)
        return RetCode::EOS;
    return frames;
}

WavPlayback::WavPlayback(std::string path, SampleFormat format)
    : WavPlayback(std::move(path), format, std::chrono::seconds::zero())
{
}

WavPlayback::WavPlayback(std::string path, SampleFormat format, std::chrono::seconds segment_duration,
                         WavFile::WriteMode write_mode)
    : ActiveBackend(false), path_(std::move(path)), format_(format), segment_duration_(segment_duration),
      write_mode_(write_mode)
{
    if (segment_duration_.count() < 0)
        throw Error(RetCode::EPARAM);
    if (!WavFile::supports_write_format(format_))
        throw Error(RetCode::INVFMT);
}

Result<PortSpec> WavPlayback::on_open(bool capture, PortSpec logical, std::chrono::microseconds period)
{
    if (capture)
        return RetCode::EPARAM;
    const auto count = frames_in_period(logical.bandwidth, period);
    if (!count)
        return count.ret_code();
    logical.format = format_;
    if (segment_duration_.count())
    {
        const auto limit = WavFile::max_write_frames(logical);
        if (!limit)
            return limit.ret_code();
        const auto hz = static_cast<std::uint32_t>(logical.bandwidth);
        const auto seconds = static_cast<std::uint64_t>(segment_duration_.count());
        if (seconds > limit.value() / hz)
            return RetCode::TOOLARGE;
        segment_frames_ = static_cast<std::size_t>(seconds * hz);

        segment_path_ = path_;
        const auto separator = path_.find_last_of("/\\");
        const auto name_start = separator == std::string::npos ? 0 : separator + 1;
        if (name_start == path_.size())
            return RetCode::EPARAM;
        auto extension = path_.find_last_of('.');
        if (extension == std::string::npos || extension <= name_start)
        {
            extension = segment_path_.size();
            segment_path_ += ".wav";
        }
        segment_path_.insert(extension, 1, '-');
        segment_number_offset_ = extension + 1;
        segment_path_.insert(segment_number_offset_, segment_number_width, '0');
        segment_index_ = 0;
    }
    set_device_max_frames(count.value());
    return logical;
}

RetCode WavPlayback::on_bind_ready(PortSpec spec) noexcept
{
    // Create the file only after allocation succeeds so the same path is available for a retry.
    return open_file(spec);
}

RetCode WavPlayback::on_start() noexcept
{
    const auto state = file_.status();
    return state == RetCode::ESTATE ? open_file(device_spec()) : state;
}

void WavPlayback::on_stop() noexcept
{
    const auto flushed = file_.flush();
    if (flushed != RetCode::ESTATE)
        record_io_error(flushed);
}

RetCode WavPlayback::open_file(PortSpec spec) noexcept
{
    if (!segment_frames_)
        return file_.open_write(path_, spec, write_mode_);
    for (;;)
    {
        if (segment_index_ == std::numeric_limits<std::uint32_t>::max())
            return RetCode::TOOLARGE;
        auto index = ++segment_index_;
        for (auto pos = segment_number_offset_ + segment_number_width; pos != segment_number_offset_; index /= 10)
            segment_path_[--pos] = static_cast<char>('0' + index % 10);
        const auto opened = file_.open_write(segment_path_, spec, WavFile::WriteMode::Exclusive);
        if (opened != RetCode::BUSY)
            return opened;
    }
}

void WavPlayback::on_close() noexcept
{
    const auto result = file_.close();
    if (last_error())
        record_io_error(result);
}

RetCode WavPlayback::on_write(IBBlock input) noexcept
{
    if (!segment_frames_)
        return file_.write(input);

    const auto spec = input.spec();
    const auto width = spec.channels * sample_bytes(spec.format);
    const auto *bytes = input.byte_view().data();
    std::size_t written = 0;
    while (written < input.frames())
    {
        if (file_.frame_count() == segment_frames_)
        {
            const auto closed = file_.close();
            if (!closed)
                return closed;
            const auto opened = open_file(spec);
            if (!opened)
                return opened;
        }
        const auto count = std::min(input.frames() - written, segment_frames_ - file_.frame_count());
        const auto code = file_.write({bytes + written * width, spec, count});
        if (!code)
            return code;
        written += count;
    }
    return {};
}

} // namespace aereborn
