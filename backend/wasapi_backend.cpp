#include "wasapi_backend.h"

#if !WINDOWS_OS_ENVIRONMENT
#error "WASAPI requires a Windows Vista or later SDK target (_WIN32_WINNT >= 0x0600)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <utility>

namespace aereborn
{
namespace
{

struct ReleaseInterface
{
    void operator()(IUnknown *ptr) const noexcept { ptr->Release(); }
};

template <class T> using Interface = std::unique_ptr<T, ReleaseInterface>;

struct FreeComBuffer
{
    void operator()(void *ptr) const noexcept { CoTaskMemFree(ptr); }
};

struct CloseEvent
{
    void operator()(void *event) const noexcept { CloseHandle(event); }
};

class ComApartment
{
  public:
    explicit ComApartment(DWORD mode) noexcept : result(CoInitializeEx(nullptr, mode)) {}
    ~ComApartment() { if (SUCCEEDED(result) && active) CoUninitialize(); }
    HRESULT result;
    bool active = true;
};

RetCode record_failure(WasapiBackend::Failure &failure, const char *operation, HRESULT error) noexcept
{
    failure = {operation, static_cast<std::int32_t>(error)};
    if (error == AUDCLNT_E_WRONG_ENDPOINT_TYPE)
        return RetCode::EPARAM;
    if (error == AUDCLNT_E_UNSUPPORTED_FORMAT)
        return RetCode::INVFMT;
    if (error == AUDCLNT_E_DEVICE_INVALIDATED || error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return RetCode::DEVLOST;
    return RetCode::IOFAILED;
}

RetCode activate(const std::wstring &id, EDataFlow direction, Interface<IAudioClient> &client,
                 std::wstring *resolved, WasapiBackend::Failure &failure)
{
    IMMDeviceEnumerator *enumerator_ptr = nullptr;
    auto rc = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator_ptr));
    if (FAILED(rc))
        return record_failure(failure, "CoCreateInstance(MMDeviceEnumerator)", rc);
    const Interface<IMMDeviceEnumerator> enumerator(enumerator_ptr);

    IMMDevice *device_ptr = nullptr;
    rc = id.empty() ? enumerator->GetDefaultAudioEndpoint(direction, eConsole, &device_ptr)
                    : enumerator->GetDevice(id.c_str(), &device_ptr);
    if (FAILED(rc))
        return record_failure(failure, id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice", rc);
    const Interface<IMMDevice> device(device_ptr);
    if (resolved)
    {
        if (id.empty())
        {
            LPWSTR id_ptr = nullptr;
            rc = device->GetId(&id_ptr);
            if (FAILED(rc))
                return record_failure(failure, "IMMDevice::GetId", rc);
            const std::unique_ptr<wchar_t, FreeComBuffer> device_id(id_ptr);
            resolved->assign(device_id.get());
        }
        else
        {
            IMMEndpoint *endpoint_ptr = nullptr;
            rc = device->QueryInterface(IID_PPV_ARGS(&endpoint_ptr));
            if (FAILED(rc))
                return record_failure(failure, "QueryInterface(IMMEndpoint)", rc);
            const Interface<IMMEndpoint> endpoint(endpoint_ptr);
            EDataFlow flow;
            rc = endpoint->GetDataFlow(&flow);
            if (FAILED(rc))
                return record_failure(failure, "IMMEndpoint::GetDataFlow", rc);
            if (flow != direction)
                return record_failure(failure, "WASAPI endpoint direction", AUDCLNT_E_WRONG_ENDPOINT_TYPE);
            *resolved = id;
        }
    }
    IAudioClient *client_ptr = nullptr;
    rc = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client_ptr));
    if (FAILED(rc))
        return record_failure(failure, "IMMDevice::Activate(IAudioClient)", rc);
    client.reset(client_ptr);
    return {};
}

SampleFormat sample_format(const WAVEFORMATEX &format) noexcept
{
    auto tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            return {};
        const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
        constexpr GUID pcm{1, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        constexpr GUID floating{3, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        if (IsEqualGUID(ext.SubFormat, pcm))
            tag = WAVE_FORMAT_PCM;
        else if (IsEqualGUID(ext.SubFormat, floating))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else
            return {};
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32)
        return SampleFormat::F32LE;
    if (tag == WAVE_FORMAT_PCM)
    {
        switch (format.wBitsPerSample)
        {
        case 16: return SampleFormat::S16LE;
        case 24: return SampleFormat::S24_3LE;
        case 32: return SampleFormat::S32LE;
        }
    }
    return {};
}

Result<UINT32> initialize(IAudioClient &client, const WAVEFORMATEX &format, WasapiBackend::Failure &failure) noexcept
{
    auto rc = client.Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0, &format, nullptr);
    if (FAILED(rc))
        return record_failure(failure, "IAudioClient::Initialize", rc);
    UINT32 capacity = 0;
    rc = client.GetBufferSize(&capacity);
    if (FAILED(rc))
        return record_failure(failure, "IAudioClient::GetBufferSize", rc);
    return capacity;
}

} // namespace

Result<PortSpec> WasapiBackend::on_open(bool capture, PortSpec logical, std::chrono::microseconds period)
{
    failure_ = {};
    ComApartment com(COINIT_APARTMENTTHREADED);
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return record_failure(failure_, "CoInitializeEx", com.result);

    Interface<IAudioClient> probe;
    std::wstring resolved;
    const auto code = activate(device_, capture ? eCapture : eRender, probe, &resolved, failure_);
    if (!code)
        return code;
    WAVEFORMATEX *format_ptr = nullptr;
    const auto rc = probe->GetMixFormat(&format_ptr);
    if (FAILED(rc))
        return record_failure(failure_, "IAudioClient::GetMixFormat", rc);
    const std::unique_ptr<WAVEFORMATEX, FreeComBuffer> format(format_ptr);
    const PortSpec spec{static_cast<BandWidth>(format->nSamplesPerSec), format->nChannels, sample_format(*format)};
    if (!valid_pcm_spec(spec) || logical.channels != spec.channels ||
        format->nBlockAlign != spec.channels * sample_bytes(spec.format) ||
        sizeof(*format) + format->cbSize > format_.size())
        return RetCode::INVFMT;
    const auto budget = frames_in_period(spec.bandwidth, period);
    if (!budget)
        return budget.ret_code();

    const auto initialized = initialize(*probe, *format, failure_);
    if (!initialized)
        return initialized.ret_code();
    const auto capacity = initialized.value();
    std::vector<std::uint8_t> packet;
    if (capture)
    {
        if (capacity > packet.max_size() / format->nBlockAlign)
            return RetCode::TOOLARGE;
        packet.resize(static_cast<std::size_t>(capacity) * format->nBlockAlign);
    }

    std::unique_ptr<void, CloseEvent> ready(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!ready)
        return record_failure(failure_, "CreateEventW(ready)", HRESULT_FROM_WIN32(GetLastError()));
    std::unique_ptr<void, CloseEvent> cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancelled)
        return record_failure(failure_, "CreateEventW(cancelled)", HRESULT_FROM_WIN32(GetLastError()));

    static_assert(sizeof(WAVEFORMATEXTENSIBLE) == 40, "Unexpected WAVEFORMATEXTENSIBLE layout");
    format_.fill(0);
    std::memcpy(format_.data(), format.get(), sizeof(*format) + format->cbSize);
    device_id_ = std::move(resolved);
    packet_ = std::move(packet);
    ready_ = ready.release();
    cancelled_ = cancelled.release();
    set_device_max_frames(std::min<std::size_t>(budget.value(), capacity));
    return spec;
}

void WasapiBackend::on_close() noexcept
{
    if (!ready_)
        return;
    CloseHandle(cancelled_);
    CloseHandle(ready_);
    cancelled_ = ready_ = nullptr;
    device_id_.clear();
    std::vector<std::uint8_t>().swap(packet_);
}

RetCode WasapiBackend::on_start() noexcept
{
    failure_ = {};
    if (!ResetEvent(ready_) || !ResetEvent(cancelled_))
        return record_failure(failure_, "ResetEvent", HRESULT_FROM_WIN32(GetLastError()));
    const auto source = is_capture();
    ComApartment com(COINIT_MULTITHREADED);
    if (FAILED(com.result))
        return record_failure(failure_, "CoInitializeEx", com.result);
    Interface<IAudioClient> client;
    const auto code = activate(device_id_, source ? eCapture : eRender, client, nullptr, failure_);
    if (!code)
        return code;
    WAVEFORMATEXTENSIBLE format;
    std::memcpy(&format, format_.data(), sizeof(format));
    const auto initialized = initialize(*client, format.Format, failure_);
    if (!initialized)
        return initialized.ret_code();
    const auto capacity = initialized.value();
    if (source && capacity > packet_.size() / format.Format.nBlockAlign)
        return RetCode::TOOLARGE;
    auto rc = client->SetEventHandle(ready_);
    if (FAILED(rc))
        return record_failure(failure_, "IAudioClient::SetEventHandle", rc);

    IAudioCaptureClient *capture = nullptr;
    IAudioRenderClient *render = nullptr;
    rc = source ? client->GetService(IID_PPV_ARGS(&capture)) : client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(rc))
        return record_failure(failure_, "IAudioClient::GetService", rc);
    Interface<IAudioCaptureClient> capture_service(capture);
    Interface<IAudioRenderClient> render_service(render);
    if (render_service)
    {
        // Prime the endpoint buffer before the audio engine starts consuming it.
        const auto primed = write(*render_service, capacity, {});
        if (!primed)
            return primed;
    }
    rc = client->Start();
    if (FAILED(rc))
        return record_failure(failure_, "IAudioClient::Start", rc);

    packet_offset_ = packet_size_ = 0;
    capacity_ = capacity;
    client_ = client.release();
    capture_ = capture_service.release();
    render_ = render_service.release();
    com.active = false;
    return {};
}

void WasapiBackend::on_stop() noexcept
{
    client_->Stop();
    if (capture_)
        capture_->Release();
    else
        render_->Release();
    client_->Release();
    capture_ = nullptr;
    render_ = nullptr;
    client_ = nullptr;
    CoUninitialize();
}

void WasapiBackend::on_cancel() noexcept
{
    SetEvent(cancelled_);
}

RetCode WasapiBackend::wait() noexcept
{
    const HANDLE handles[] = {cancelled_, ready_};
    const auto timeout = remaining_io_ms();
    if (!timeout)
        return RetCode::TIMEOUT;
    switch (WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(timeout)))
    {
    case WAIT_OBJECT_0: return RetCode::CANCELLED;
    case WAIT_OBJECT_0 + 1: return {};
    case WAIT_TIMEOUT: return RetCode::TIMEOUT;
    case WAIT_FAILED:
        return record_failure(failure_, "WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError()));
    default: return RetCode::IOFAILED;
    }
}

Result<std::size_t> WasapiBackend::on_read(OBBlock output) noexcept
{
    const auto width = output.spec().channels * sample_bytes(output.spec().format);
    const auto dest = output.byte_view();
    while (packet_offset_ == packet_size_)
    {
        BYTE *bytes = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        auto rc = capture_->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr);
        if (FAILED(rc))
            return record_failure(failure_, "IAudioCaptureClient::GetBuffer", rc);
        if (!frames)
        {
            // No packet needs releasing when GetBuffer reports zero frames.
            const auto ready = wait();
            if (!ready)
                return ready;
            continue;
        }

        assert(frames <= capacity_);
        const auto received = std::min<std::size_t>(frames, output.frames());
        const auto copied = received * width;
        const auto remaining = (frames - received) * width;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            std::memset(dest.data(), 0, copied);
            if (remaining)
                std::memset(packet_.data(), 0, remaining);
        }
        else
        {
            std::memcpy(dest.data(), bytes, copied);
            if (remaining)
                std::memcpy(packet_.data(), bytes + copied, remaining);
        }
        rc = capture_->ReleaseBuffer(frames);
        if (FAILED(rc))
            return record_failure(failure_, "IAudioCaptureClient::ReleaseBuffer", rc);
        if (remaining)
        {
            packet_offset_ = 0;
            packet_size_ = remaining;
        }
        return received;
    }

    const auto size = std::min(dest.size(), packet_size_ - packet_offset_);
    std::memcpy(dest.data(), packet_.data() + packet_offset_, size);
    packet_offset_ += size;
    return size / width;
}

Result<std::size_t> WasapiBackend::on_wait_writable() noexcept
{
    for (;;)
    {
        UINT32 used = 0;
        const auto rc = client_->GetCurrentPadding(&used);
        if (FAILED(rc))
            return record_failure(failure_, "IAudioClient::GetCurrentPadding", rc);
        if (used < capacity_)
            return std::min<std::size_t>(capacity_ - used, device_max_frames());
        const auto ready = wait();
        if (!ready)
            return ready;
    }
}

RetCode WasapiBackend::on_write(IBBlock input) noexcept
{
    return write(*render_, static_cast<UINT32>(input.frames()), input.byte_view());
}

RetCode WasapiBackend::on_write_silence(OBBlock scratch) noexcept
{
    return write(*render_, static_cast<UINT32>(scratch.frames()), {});
}

RetCode WasapiBackend::write(IAudioRenderClient &render, std::uint32_t frames, View<const std::uint8_t> bytes) noexcept
{
    BYTE *dest = nullptr;
    auto rc = render.GetBuffer(frames, &dest);
    if (FAILED(rc))
        return record_failure(failure_, "IAudioRenderClient::GetBuffer", rc);
    const auto silent = bytes.empty();
    if (!silent)
        std::memcpy(dest, bytes.data(), bytes.size());
    rc = render.ReleaseBuffer(frames, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    return FAILED(rc) ? record_failure(failure_, "IAudioRenderClient::ReleaseBuffer", rc) : RetCode{};
}

WasapiBackend::WasapiBackend(std::wstring device) : ActiveBackend(true), device_(std::move(device))
{
}

} // namespace aereborn
