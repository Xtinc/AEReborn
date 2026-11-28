#include "device_service.h"
#include "audio_interface.h"
#include "diag/inspect.h"
#include "graph_service.h"
#include "plugins/endpoint.h"
#if defined(AEREBORN_HAS_ALSA)
#include "backend/alsa_backend.h"
#elif WINDOWS_OS_ENVIRONMENT
#include "backend/wasapi_backend.h"
#endif
#include <algorithm>
#include <cassert>
#include <new>
#include <utility>

namespace aereborn
{
namespace
{

constexpr auto recovery_flags = DevFlags::RECOVERSOFT | DevFlags::RECOVERHARD;
#if defined(AEREBORN_HAS_ALSA) || WINDOWS_OS_ENVIRONMENT
constexpr auto device_health_interval = std::chrono::seconds(1);
constexpr auto device_reset_interval = std::chrono::seconds(10); // Shortened during device recovery testing.
#endif

} // namespace

#if defined(AEREBORN_HAS_ALSA) || WINDOWS_OS_ENVIRONMENT
struct DeviceService::DeviceBinding final : NodeBinding
{
    using Clock = CommandQueue::Clock;
    enum class Status { Stopped, Online, Waiting, Failed };

    DeviceBinding(const DevConfig &config, DevFlags flags) : config_(config), flags_(flags) {}
    ~DeviceBinding() override { unbind(); }

    void attach(DeviceService &service, NodeId id) noexcept
    {
        service_ = &service;
        id_ = id;
    }

    void bind(Capture &node)
    {
        capture_ = &node;
        bind_initial();
    }

    void bind(Playback &node)
    {
        playback_ = &node;
        bind_initial();
    }

    const Backend *backend() const noexcept override { return driver_.get(); }

    DeviceStatus device_status() const noexcept
    {
        auto status = status_;
        auto error = last_error_;
        if (status == Status::Online && !driver_->running())
        {
            error = worker_error();
            status = failure_status(error);
        }
        constexpr const char *names[] = {"stopped", "online", "waiting", "failed"};
        return {names[static_cast<unsigned>(status)], error};
    }

    void start() override
    {
        start_driver();
        if (needs_poll())
            service_->on_device_started();
    }

    void stop() noexcept override
    {
        const auto was_polled = needs_poll();
        status_ = Status::Stopped;
        if (driver_)
        {
            driver_->stop();
            const auto error = driver_->last_error();
            if (!error)
                last_error_ = error;
            if (!error || config_.name.empty() || config_.name == "default")
                driver_.reset();
        }
        if (was_polled)
            service_->on_device_stopped();
    }

    void unbind() noexcept override
    {
        if (service_)
        {
            service_->unregister(id_);
            service_ = nullptr;
        }
        driver_.reset();
        status_ = Status::Stopped;
    }

    bool needs_poll() const noexcept
    {
        return recovers() && (status_ == Status::Online || status_ == Status::Waiting);
    }

    bool poll(Clock::time_point now) noexcept
    {
        if (!needs_poll())
            return false;
        if (status_ == Status::Online)
        {
            if (!driver_->running())
            {
                fail(worker_error());
                if (status_ == Status::Failed)
                    return false;
            }
            else if (!strict() || now < reset_at_)
                return true;
            else
                driver_.reset();
        }
        start_driver();
        return needs_poll();
    }

  private:
    bool recovers() const noexcept { return (flags_ & recovery_flags) != DevFlags::NONE; }
    bool strict() const noexcept { return has_flag(flags_, DevFlags::RECOVERHARD); }

    static bool device_unavailable(RetCode code) noexcept
    {
        return code == RetCode::DEVLOST || code == RetCode::IOFAILED;
    }

    void bind_initial()
    {
        try
        {
            open_driver();
        }
        catch (const Error &error)
        {
            if (!recovers() || !device_unavailable(error.code()))
                throw;
            last_error_ = error.code();
        }
    }

    void open_driver()
    {
        const auto fail_on_timeout = !has_flag(flags_, DevFlags::RECOVERSOFT);
#if defined(AEREBORN_HAS_ALSA)
        auto driver = std::make_unique<AlsaBackend>(config_.name, config_.device, !has_flag(flags_, DevFlags::MEMCPY),
                                                    fail_on_timeout);
#else
        auto driver = std::make_unique<WasapiBackend>(config_.name, fail_on_timeout);
#endif
        if (capture_)
            driver->bind(*capture_);
        else
            driver->bind(*playback_);
        driver_ = std::move(driver);
    }

    void start_driver()
    {
        try
        {
            if (!driver_)
                open_driver();
            driver_->start();
            if (strict())
                reset_at_ = Clock::now() + device_reset_interval;
            last_error_ = {};
            status_ = Status::Online;
        }
        catch (const Error &error)
        {
            fail(error.code());
            if (!recovers())
                throw;
        }
        catch (const std::bad_alloc &)
        {
            fail(RetCode::TOOLARGE);
            if (!recovers())
                throw;
        }
        catch (...)
        {
            fail(RetCode::FAILED);
            if (!recovers())
                throw;
        }
    }

    RetCode worker_error() const noexcept
    {
        const auto error = driver_->last_error();
        return error ? RetCode::IOFAILED : error.code();
    }

    Status failure_status(RetCode error) const noexcept
    {
        return recovers() && (device_unavailable(error) || (strict() && error == RetCode::TIMEOUT))
                   ? Status::Waiting : Status::Failed;
    }

    void fail(RetCode error) noexcept
    {
        last_error_ = error;
        driver_.reset();
        status_ = failure_status(error);
    }

    const DevConfig config_;
    const DevFlags flags_;
    DeviceService *service_ = nullptr;
    NodeId id_;
    Capture *capture_ = nullptr;
    Playback *playback_ = nullptr;
    std::unique_ptr<ActiveBackend> driver_;
    RetCode last_error_;
    Status status_ = Status::Stopped;
    Clock::time_point reset_at_;
};
#endif

DeviceService::DeviceService(CommandQueue &commands, GraphService &graph) : commands_(commands), graph_(graph)
{
}

DeviceService::~DeviceService()
{
    assert(devices_.empty());
    cancel_poll();
}

template <class Endpoint> NodeId DeviceService::create_device(const DevConfig &config, DevFlags flags)
{
    assert(commands_.on_thread());
    constexpr auto known_flags = recovery_flags | DevFlags::MEMCPY;
    if (!is_block_period(config.period) || config.name.find('\0') != std::string::npos ||
        (flags & known_flags) != flags || has_flag(flags, recovery_flags))
        throw Error(RetCode::EPARAM);

    const std::chrono::milliseconds period(static_cast<std::uint32_t>(config.period));
    auto endpoint = std::make_unique<Endpoint>(config.bandwidth, config.channels, period);
#if defined(AEREBORN_HAS_ALSA) || WINDOWS_OS_ENVIRONMENT
    auto binding = std::make_shared<DeviceBinding>(config, flags);
    auto *node = endpoint.get();
    auto *device = binding.get();
    const auto id = graph_.add_endpoint(std::move(endpoint), std::move(binding), [device, node] { device->bind(*node); });
    const auto cleanup = [&]() noexcept { graph_.remove(id); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    devices_.emplace(id, device);
    device->attach(*this, id);
    rollback.dismiss();
    return id;
#else
    throw Error(RetCode::IOFAILED);
#endif
}

NodeId DeviceService::create_idev(const DevConfig &config, DevFlags flags)
{
    return create_device<Capture>(config, flags);
}

NodeId DeviceService::create_odev(const DevConfig &config, DevFlags flags)
{
    return create_device<Playback>(config, flags);
}

void DeviceService::cancel_poll() noexcept
{
    commands_.cancel(poll_timer_);
    poll_timer_ = 0;
}

#if defined(AEREBORN_HAS_ALSA) || WINDOWS_OS_ENVIRONMENT
DeviceStatus DeviceService::status_of(NodeId node) const noexcept
{
    const auto device = devices_.find(node);
    return device == devices_.end() ? DeviceStatus{} : device->second->device_status();
}

void DeviceService::on_device_started()
{
    if (!poll_timer_)
        poll_timer_ = commands_.post_every(device_health_interval, [this] { poll_devices(); });
}

void DeviceService::on_device_stopped() noexcept
{
    if (poll_timer_ && std::none_of(devices_.begin(), devices_.end(), [](const auto &entry) {
            return entry.second->needs_poll();
        }))
        cancel_poll();
}

void DeviceService::unregister(NodeId id) noexcept
{
    const auto entry = devices_.find(id);
    const auto was_polled = entry->second->needs_poll();
    devices_.erase(entry);
    if (was_polled)
        on_device_stopped();
}

void DeviceService::poll_devices() noexcept
{
    const auto now = CommandQueue::Clock::now();
    bool required = false;
    for (const auto &entry : devices_)
        required |= entry.second->poll(now);
    if (!required)
        cancel_poll();
}
#else
DeviceStatus DeviceService::status_of(NodeId) const noexcept
{
    return {};
}
#endif

} // namespace aereborn
