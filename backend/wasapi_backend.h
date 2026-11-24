#ifndef AEREBORN_BACKEND_WASAPI_BACKEND_H
#define AEREBORN_BACKEND_WASAPI_BACKEND_H

#include "backend.h"
#include "core/os.h"

#if WINDOWS_OS_ENVIRONMENT
#include <array>
#include <cstdint>
#include <string>

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioRenderClient;

namespace aereborn
{

/// @brief Event-driven WASAPI backend for a Windows Vista or later SDK target.
class WasapiBackend final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override
    {
        return bound() ? (is_capture() ? "WasapiCapture" : "WasapiPlayback") : "WasapiBackend";
    }

    /// @brief Native failure details. operation is null when no native failure was recorded.
    struct Failure
    {
        const char *operation = nullptr;
        std::int32_t hresult = 0;
    };

    /// @brief Construct a backend whose direction is selected when it binds.
    /// @param device Endpoint ID, or empty for the system default in the selected direction.
    explicit WasapiBackend(std::wstring device);
    ~WasapiBackend() override { unbind(); }

    /// @brief Return the latest native failure since binding or starting, or an empty value while running.
    /// @note Call on the control thread, serialized with bind(), start(), stop(), and unbind().
    Failure last_failure() const noexcept { return running() ? Failure{} : failure_; }

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds period) override;
    RetCode on_start() noexcept override;
    void on_stop() noexcept override;
    void on_cancel() noexcept override;
    void on_close() noexcept override;

    Result<std::size_t> on_read(OBBlock output) noexcept override;
    Result<std::size_t> on_wait_writable() noexcept override;
    RetCode on_write(IBBlock input) noexcept override;
    RetCode on_write_silence(OBBlock scratch) noexcept override;
    RetCode wait() noexcept;
    RetCode write(IAudioRenderClient &render, std::uint32_t frames, View<const std::uint8_t> bytes) noexcept;

    Failure failure_;
    std::wstring device_;
    std::wstring device_id_;
    std::array<std::uint8_t, 40> format_{};
    std::vector<std::uint8_t> packet_;
    std::size_t packet_offset_ = 0;
    std::size_t packet_size_ = 0;
    std::uint32_t capacity_ = 0;
    void *ready_ = nullptr;
    void *cancelled_ = nullptr;
    IAudioClient *client_ = nullptr;
    IAudioCaptureClient *capture_ = nullptr;
    IAudioRenderClient *render_ = nullptr;
};

} // namespace aereborn
#endif
#endif
