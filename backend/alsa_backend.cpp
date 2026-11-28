#include "alsa_backend.h"

#include <alsa/asoundlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <utility>

namespace aereborn
{
namespace
{

RetCode alsa_error(int error) noexcept
{
    return error == -ENODEV || error == -ENXIO || error == -ENOENT ? RetCode::DEVLOST : RetCode::IOFAILED;
}

std::string normalize_device_name(std::string card, unsigned int device)
{
    if (card.empty())
        return "default";
    if (card == "default" || card.find(':') != std::string::npos)
        return card;
    return "hw:CARD=" + card + ",DEV=" + std::to_string(device);
}

} // namespace

AlsaBackend::AlsaBackend(std::string card, unsigned int device, bool use_mmap, bool fail_on_timeout)
    : ActiveBackend(true, fail_on_timeout), device_(normalize_device_name(std::move(card), device)), use_mmap_(use_mmap)
{
}

Result<PortSpec> AlsaBackend::configure_pcm(snd_pcm_t *handle, bool mmap, PortSpec logical,
                                            std::chrono::microseconds duration) noexcept
{
    const auto access = mmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(handle, hw) < 0 ||
        snd_pcm_hw_params_set_access(handle, hw, access) < 0 ||
        snd_pcm_hw_params_set_channels(handle, hw, static_cast<unsigned>(logical.channels)) < 0)
        return RetCode::INVFMT;

    const std::pair<snd_pcm_format_t, SampleFormat> formats[] = {
        {SND_PCM_FORMAT_FLOAT_LE, SampleFormat::F32LE}, {SND_PCM_FORMAT_S32_LE, SampleFormat::S32LE},
        {SND_PCM_FORMAT_S24_3LE, SampleFormat::S24_3LE}, {SND_PCM_FORMAT_S24_LE, SampleFormat::S24LE},
        {SND_PCM_FORMAT_S16_LE, SampleFormat::S16LE}};
    SampleFormat format{};
    for (const auto &entry : formats)
        if (snd_pcm_hw_params_set_format(handle, hw, entry.first) >= 0)
        {
            format = entry.second;
            break;
        }

    auto hz = static_cast<unsigned>(logical.bandwidth);
    if (format == SampleFormat::F32P || snd_pcm_hw_params_set_rate_near(handle, hw, &hz, nullptr) < 0 ||
        !is_bandwidth(static_cast<BandWidth>(hz)))
        return RetCode::INVFMT;
    const auto rate = static_cast<BandWidth>(hz);
    const auto budget = frames_in_period(rate, duration);
    if (!budget)
        return budget.ret_code();

    auto period = static_cast<snd_pcm_uframes_t>(budget.value());
    int period_dir = -1;
    if (snd_pcm_hw_params_set_period_size_near(handle, hw, &period, &period_dir) < 0)
        return RetCode::INVFMT;

    // Keep the period constraints for the buffer-size fallback.
    snd_pcm_hw_params_t *buffer_params;
    snd_pcm_hw_params_alloca(&buffer_params);
    snd_pcm_hw_params_copy(buffer_params, hw);
    unsigned periods = 2;
    if (snd_pcm_hw_params_set_periods_near(handle, hw, &periods, nullptr) < 0)
    {
        if (period > std::numeric_limits<snd_pcm_uframes_t>::max() / 2)
            return RetCode::TOOLARGE;
        auto buffer_frames = 2 * period;
        hw = buffer_params;
        if (snd_pcm_hw_params_set_buffer_size_near(handle, hw, &buffer_frames) < 0)
            return RetCode::INVFMT;
    }
    const auto rc = snd_pcm_hw_params(handle, hw);
    if (rc == -EINVAL || rc == -ENOSYS || rc == -EOPNOTSUPP)
        return RetCode::INVFMT;
    if (rc < 0)
        return alsa_error(rc);
    set_device_max_frames(std::min<std::size_t>(period, budget.value()));
    return PortSpec{rate, logical.channels, format};
}

Result<PortSpec> AlsaBackend::on_open(bool capture, PortSpec logical, std::chrono::microseconds duration)
{
    if (logical.channels > std::numeric_limits<unsigned>::max())
        return RetCode::TOOLARGE;
    snd_pcm_t *handle = nullptr;
    const auto rc = snd_pcm_open(&handle, device_.c_str(), capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                                 SND_PCM_NONBLOCK);
    if (rc < 0)
        return alsa_error(rc);
    std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm(handle, snd_pcm_close);

    auto mmap = use_mmap_;
    auto spec = configure_pcm(handle, mmap, logical, duration);
    if (mmap && spec.ret_code() == RetCode::INVFMT)
    {
        mmap = false;
        spec = configure_pcm(handle, mmap, logical, duration);
    }
    if (!spec)
        return spec;

    const auto frames = device_max_frames();
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(handle, sw) < 0 || snd_pcm_sw_params_set_avail_min(handle, sw, frames) < 0)
        return RetCode::IOFAILED;
    if (!capture)
    {
        snd_pcm_uframes_t boundary = 0;
        if (snd_pcm_sw_params_set_start_threshold(handle, sw, frames) < 0 ||
            snd_pcm_sw_params_set_silence_threshold(handle, sw, 0) < 0 ||
            snd_pcm_sw_params_get_boundary(sw, &boundary) < 0 ||
            snd_pcm_sw_params_set_silence_size(handle, sw, boundary) < 0)
            return RetCode::IOFAILED;
    }
    if (snd_pcm_sw_params(handle, sw) < 0)
        return RetCode::IOFAILED;

    const auto count = snd_pcm_poll_descriptors_count(handle);
    if (count <= 0)
        return RetCode::IOFAILED;
    std::vector<pollfd> descriptors(static_cast<std::size_t>(count) + 1);
    if (snd_pcm_poll_descriptors(handle, descriptors.data(), count) != count)
        return RetCode::IOFAILED;
    const auto wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake < 0)
        return RetCode::IOFAILED;

    descriptors.back() = {wake, POLLIN, 0};
    poll_ = std::move(descriptors);
    mmap_ = mmap;
    pcm_ = pcm.release();
    return spec;
}

RetCode AlsaBackend::on_start() noexcept
{
    std::uint64_t signal = 0;
    ssize_t bytes;
    do
        bytes = ::read(poll_.back().fd, &signal, sizeof(signal));
    while (bytes < 0 && errno == EINTR);
    if (bytes < 0 && errno != EAGAIN)
        return RetCode::IOFAILED;
    return prepare_stream();
}

RetCode AlsaBackend::prepare_stream() noexcept
{
    auto rc = snd_pcm_prepare(pcm_);
    if (rc >= 0 && is_capture())
    {
        rc = snd_pcm_start(pcm_);
        if (rc < 0)
            snd_pcm_drop(pcm_);
    }
    return rc < 0 ? alsa_error(rc) : RetCode{};
}

void AlsaBackend::on_stop() noexcept
{
    snd_pcm_drop(pcm_);
}

void AlsaBackend::on_cancel() noexcept
{
    constexpr std::uint64_t signal = 1;
    ssize_t bytes;
    do
        bytes = ::write(poll_.back().fd, &signal, sizeof(signal));
    while (bytes < 0 && errno == EINTR);
}

void AlsaBackend::on_close() noexcept
{
    if (!pcm_)
        return;
    ::close(poll_.back().fd);
    poll_.clear();
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
}

RetCode AlsaBackend::recover(int error) noexcept
{
    if (error == -ESTRPIPE)
    {
        pollfd cancel{poll_.back().fd, POLLIN, 0};
        constexpr unsigned resume_attempts = 10;
        for (unsigned attempt = 1; ; ++attempt)
        {
            const auto resumed = snd_pcm_resume(pcm_);
            if (resumed >= 0)
                return {};
            if (resumed == -ENODEV || resumed == -ENXIO)
                return RetCode::DEVLOST;
            if (resumed != -EAGAIN || attempt == resume_attempts)
                break;

            int rc;
            do
                rc = ::poll(&cancel, 1, std::min(50, remaining_io_ms()));
            while (rc < 0 && errno == EINTR);
            if (rc < 0)
                return RetCode::IOFAILED;
            if (cancel.revents & POLLIN)
                return RetCode::CANCELLED;
            if (!remaining_io_ms())
                return RetCode::TIMEOUT;
        }
    }
    else if (error != -EPIPE)
        return alsa_error(error);
    return prepare_stream();
}

RetCode AlsaBackend::wait_ready() noexcept
{
    for (;;)
    {
        const auto timeout = remaining_io_ms();
        if (!timeout)
            return RetCode::TIMEOUT;
        const auto rc = ::poll(poll_.data(), poll_.size(), timeout);
        if (!rc)
            return RetCode::TIMEOUT;
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return RetCode::IOFAILED;
        }
        if (poll_.back().revents & POLLIN)
            return RetCode::CANCELLED;
        unsigned short events = 0;
        const auto code = snd_pcm_poll_descriptors_revents(pcm_, poll_.data(), poll_.size() - 1, &events);
        if ((events & (POLLHUP | POLLNVAL)) ||
            ((code < 0 || (events & POLLERR)) && snd_pcm_state(pcm_) == SND_PCM_STATE_DISCONNECTED))
            return RetCode::DEVLOST;
        if (code < 0)
            return alsa_error(code);
        if (events & (POLLIN | POLLOUT | POLLERR))
            return {};
    }
}

Result<std::size_t> AlsaBackend::on_read(OBBlock output) noexcept
{
    const auto read_frames = mmap_ ? snd_pcm_mmap_readi : snd_pcm_readi;
    for (;;)
    {
        const auto ready = wait_ready();
        if (!ready)
            return ready;
        const auto rc = read_frames(pcm_, output.byte_view().data(), output.frames());
        if (rc > 0)
            return static_cast<std::size_t>(rc);
        if (rc < 0 && rc != -EAGAIN && rc != -EINTR)
        {
            const auto code = recover(static_cast<int>(rc));
            if (!code)
                return code;
        }
    }
}

Result<std::size_t> AlsaBackend::on_wait_writable() noexcept
{
    for (;;)
    {
        const auto ready = wait_ready();
        if (!ready)
            return ready;
        const auto frames = snd_pcm_avail_update(pcm_);
        if (frames > 0)
            return std::min<std::size_t>(frames, device_max_frames());
        if (frames < 0 && frames != -EAGAIN && frames != -EINTR)
        {
            const auto code = recover(static_cast<int>(frames));
            if (!code)
                return code;
        }
    }
}

RetCode AlsaBackend::on_write(IBBlock input) noexcept
{
    const auto width = input.spec().channels * sample_bytes(input.spec().format);
    const auto *bytes = input.byte_view().data();
    std::size_t written = 0;
    auto count = input.frames();
    const auto write_frames = mmap_ ? snd_pcm_mmap_writei : snd_pcm_writei;
    for (;;)
    {
        const auto rc = write_frames(pcm_, bytes + written * width, count);
        if (rc > 0)
        {
            written += static_cast<std::size_t>(rc);
            if (written == input.frames())
                return {};
        }
        else if (rc < 0 && rc != -EAGAIN && rc != -EINTR)
        {
            const auto code = recover(static_cast<int>(rc));
            if (!code)
                return code;
        }

        const auto next = on_wait_writable();
        if (!next)
            return next.ret_code();
        count = std::min(next.value(), input.frames() - written);
    }
}

} // namespace aereborn
