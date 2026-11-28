#ifndef AEREBORN_BACKEND_ALSA_BACKEND_H
#define AEREBORN_BACKEND_ALSA_BACKEND_H

#include "backend.h"

#ifdef AEREBORN_HAS_ALSA
#include <poll.h>
#include <string>
#include <vector>

struct _snd_pcm;

namespace aereborn
{

/// @brief External-clock Linux ALSA backend supporting both Capture and Playback.
/// @note An empty card name or "default" selects the ALSA default PCM. PCM names such as hw: or plug:
///       are passed through. A bare card name is expanded to hw:CARD=<card>,DEV=<device>.
/// @note Prefers MMAP_INTERLEAVED when use_mmap is true and falls back to RW_INTERLEAVED.
class AlsaBackend final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override
    {
        return bound() ? (is_capture() ? "AlsaCapture" : "AlsaPlayback") : "AlsaBackend";
    }

    /// @brief Create an ALSA backend whose direction is selected when it binds.
    /// @param card Bare card name, explicit PCM name, or empty for the default PCM.
    /// @param device Hardware device index for a bare card name.
    /// @param use_mmap Prefer interleaved MMAP over ordinary interleaved I/O.
    /// @param fail_on_timeout Stop the worker when PCM I/O times out.
    explicit AlsaBackend(std::string card, unsigned int device, bool use_mmap, bool fail_on_timeout = true);
    ~AlsaBackend() override
    {
        unbind();
    }

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds period) override;
    RetCode on_start() noexcept override;
    void on_stop() noexcept override;
    void on_cancel() noexcept override;
    void on_close() noexcept override;

    Result<std::size_t> on_read(OBBlock output) noexcept override;
    Result<std::size_t> on_wait_writable() noexcept override;
    RetCode on_write(IBBlock input) noexcept override;

    Result<PortSpec> configure_pcm(_snd_pcm *pcm, bool mmap, PortSpec logical,
                                   std::chrono::microseconds duration) noexcept;
    RetCode prepare_stream() noexcept;
    RetCode wait_ready() noexcept;
    RetCode recover(int error) noexcept;

    std::string device_;
    const bool use_mmap_;
    bool mmap_ = false;
    _snd_pcm *pcm_ = nullptr;
    std::vector<pollfd> poll_;
};

} // namespace aereborn

#endif
#endif
