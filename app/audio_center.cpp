#include "audio_interface.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "backend/usr_capture.h"
#include "backend/usr_playback.h"
#include "backend/wav_backend.h"
#include "core/command_queue.h"
#include "device_service.h"
#include "diag/inspect.h"
#include "graph_service.h"
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace aereborn
{
namespace
{

thread_local RetCode thread_last_error;

template <class Fn> auto invoke(Fn &&fn) noexcept -> decltype(fn())
{
    try
    {
        return fn();
    }
    catch (const Error &error)
    {
        thread_last_error = error.code();
    }
    catch (const std::bad_alloc &)
    {
        thread_last_error = RetCode::TOOLARGE;
    }
    catch (...)
    {
        thread_last_error = RetCode::FAILED;
    }
    return decltype(fn())();
}

template <class Fn> auto dispatch(CommandQueue &commands, Fn &&fn) noexcept -> decltype(fn())
{
    thread_last_error = RetCode::OK;
    return invoke([&] { return commands.sync(fn); });
}

template <class Fn> RetCode call_status(CommandQueue &commands, Fn &&fn) noexcept
{
    dispatch(commands, fn);
    return thread_last_error;
}

template <class Driver>
struct ActiveBinding final : NodeBinding
{
    template <class... Args>
    explicit ActiveBinding(Args &&...args) : driver(std::forward<Args>(args)...) {}

    const Backend *backend() const noexcept override { return &driver; }
    void start() override { driver.start(); }
    void stop() noexcept override { driver.stop(); }
    void unbind() noexcept override { driver.unbind(); }

    Driver driver;
};

template <class Driver>
struct PassiveBinding final : NodeBinding
{
    // Constructing the driver validates the PCM spec before the frame limit is calculated.
    explicit PassiveBinding(PortSpec spec)
        : driver(spec), max_frames_(std::numeric_limits<std::size_t>::max() / spec.channels / sample_bytes(spec.format))
    {
    }

    const Backend *backend() const noexcept override { return &driver; }

    void start() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.start();
    }

    void stop() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.stop();
    }

    void unbind() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.unbind();
    }

    Driver driver;

    template <class Byte, class Block>
    Result<std::size_t> transfer(Byte *data, std::size_t frames, std::size_t (Driver::*operation)(Block))
    {
        if (frames && !data)
            return RetCode::EPARAM;
        if (frames > max_frames_)
            return RetCode::TOOLARGE;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (!driver.running())
            return driver.bound() ? RetCode::ESTATE : RetCode::ENODE;
        return (driver.*operation)({data, driver.device_spec(), frames});
    }

  private:
    std::mutex mutex_;
    const std::size_t max_frames_;
};

template <class Endpoint, class Binding>
NodeId add_endpoint(GraphService &graph, std::unique_ptr<Endpoint> endpoint, std::shared_ptr<Binding> binding)
{
    auto *node = endpoint.get();
    auto *endpoint_binding = binding.get();
    return graph.add_endpoint(std::move(endpoint), std::move(binding),
                              [endpoint_binding, node] { endpoint_binding->driver.bind(*node); });
}

template <class Endpoint, class Binding>
NodeId add_pcm_endpoint(GraphService &graph, PortSpec spec, std::chrono::microseconds io_period,
                        std::shared_ptr<Binding> binding)
{
    // The binding validates bandwidth. Reject sub-frame periods before allocating the queue.
    constexpr auto micros_per_second = 1000000u;
    const auto rate = static_cast<std::uint32_t>(spec.bandwidth);
    const std::chrono::microseconds min_period{(micros_per_second + rate - 1) / rate};
    if (io_period < min_period)
        throw Error(RetCode::EPARAM);
    auto endpoint = std::make_unique<Endpoint>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(graph, std::move(endpoint), std::move(binding));
}

template <class Binding> bool matches(const NodeBinding &binding) noexcept
{
    return dynamic_cast<const Binding *>(&binding) != nullptr;
}

template <class Driver, class Byte, class Block>
std::size_t transfer_pcm(GraphService &graph, NodeId node, Byte *data, std::size_t frames,
                         std::size_t (Driver::*operation)(Block)) noexcept
{
    using Binding = PassiveBinding<Driver>;
    return invoke([&] {
        const auto endpoint = graph.find_binding(node, matches<Binding>);
        const auto result = endpoint ? static_cast<Binding &>(*endpoint.value()).transfer(data, frames, operation)
                                     : Result<std::size_t>(endpoint.ret_code());
        thread_last_error = result.ret_code();
        return result ? result.value() : 0;
    });
}

} // namespace

AudioCenter::AudioCenter(BlockPeriod period)
    : commands_(std::make_unique<CommandQueue>())
{
    commands_->sync([&] {
        auto graph = std::make_unique<GraphService>(*commands_, period);
        auto devices = std::make_unique<DeviceService>(*commands_, *graph);
        graph_ = std::move(graph);
        devices_ = std::move(devices);
    });
}

AudioCenter::~AudioCenter()
{
    const auto shutdown = [this]() noexcept {
        devices_->cancel_poll();
        graph_.reset();
        devices_.reset();
    };
    commands_->stop(std::cref(shutdown)); // stop() joins before the borrowed callable goes out of scope.
}

NodeId AudioCenter::create_idev(const DevConfig &config, DevFlags flags)
{
    return dispatch(*commands_, [&] { return devices_->create_idev(config, flags); });
}

NodeId AudioCenter::create_odev(const DevConfig &config, DevFlags flags)
{
    return dispatch(*commands_, [&] { return devices_->create_odev(config, flags); });
}

NodeId AudioCenter::create_iwav(const std::string &path, int repeat, std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        using Binding = ActiveBinding<WavCapture>;
        auto binding = repeat > 0 ? std::make_shared<Binding>(path, WavCapture::Repeat{static_cast<std::size_t>(repeat)})
                                  : std::make_shared<Binding>(path, repeat < 0);
        const auto spec = binding->driver.probe().value_or_throw();
        auto endpoint = std::make_unique<Capture>(spec.bandwidth, spec.channels, io_period);
        return add_endpoint(*graph_, std::move(endpoint), std::move(binding));
    });
}

NodeId AudioCenter::create_owav(const std::string &path, PortSpec spec, std::chrono::seconds segment_duration,
                               std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        if (!spec.valid())
            throw Error(RetCode::EPARAM);
        throw_if_error(WavFile::max_write_frames(spec).ret_code());
        auto endpoint = std::make_unique<Playback>(spec.bandwidth, spec.channels, io_period);
        auto binding = std::make_shared<ActiveBinding<WavPlayback>>(path, spec.format, segment_duration,
                                                                   WavFile::WriteMode::Exclusive);
        return add_endpoint(*graph_, std::move(endpoint), std::move(binding));
    });
}

NodeId AudioCenter::create_icallback(PortSpec spec, AudioCallback callback, void *user_data,
                                    std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        if (!callback)
            throw Error(RetCode::EPARAM);
        auto binding = std::make_shared<ActiveBinding<UsrCapture>>(spec, [callback, user_data](OBBlock block) noexcept {
            callback(nullptr, block.byte_view().data(), block.frames(), block.spec(), user_data);
        });
        return add_pcm_endpoint<Capture>(*graph_, spec, io_period, std::move(binding));
    });
}

NodeId AudioCenter::create_ocallback(PortSpec spec, AudioCallback callback, void *user_data,
                                    std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        if (!callback)
            throw Error(RetCode::EPARAM);
        auto binding = std::make_shared<ActiveBinding<UsrPlayback>>(spec, [callback, user_data](IBBlock block) noexcept {
            callback(block.byte_view().data(), nullptr, block.frames(), block.spec(), user_data);
        });
        return add_pcm_endpoint<Playback>(*graph_, spec, io_period, std::move(binding));
    });
}

NodeId AudioCenter::create_ipush(PortSpec spec, std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        auto binding = std::make_shared<PassiveBinding<PushSource>>(spec);
        return add_pcm_endpoint<Capture>(*graph_, spec, io_period, std::move(binding));
    });
}

NodeId AudioCenter::create_opull(PortSpec spec, std::chrono::microseconds io_period)
{
    return dispatch(*commands_, [&] {
        auto binding = std::make_shared<PassiveBinding<PullSink>>(spec);
        return add_pcm_endpoint<Playback>(*graph_, spec, io_period, std::move(binding));
    });
}

std::size_t AudioCenter::push(NodeId node, const void *data, std::size_t frames) noexcept
{
    return transfer_pcm(*graph_, node, static_cast<const std::uint8_t *>(data), frames, &PushSource::push);
}

std::size_t AudioCenter::pull(NodeId node, void *data, std::size_t frames) noexcept
{
    return transfer_pcm(*graph_, node, static_cast<std::uint8_t *>(data), frames, &PullSink::pull);
}

NodeId AudioCenter::create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs)
{
    return dispatch(*commands_, [&] { return graph_->create_mixer(bandwidth, channels, inputs); });
}

NodeId AudioCenter::create_matrix(BandWidth bandwidth, const ChannelMap &map, float gain)
{
    return dispatch(*commands_, [&] {
        throw_if_error(map.status());
        const auto &coefficients = map.coefficients();
        return graph_->create_matrix(bandwidth, map.input_channels(), map.output_channels(),
                                     {coefficients.data(), coefficients.size()}, gain);
    });
}

NodeId AudioCenter::create_convert(PortSpec input, PortSpec output)
{
    return dispatch(*commands_, [&] { return graph_->create_convert(input, output); });
}

PortSpec AudioCenter::output_spec(NodeId port) const
{
    return dispatch(*commands_, [&] { return graph_->port_spec(port, true); });
}

RetCode AudioCenter::connect(NodeId from, NodeId to)
{
    return call_status(*commands_, [&] { graph_->connect(from, to); });
}

RetCode AudioCenter::disconnect(NodeId from, NodeId to)
{
    return call_status(*commands_, [&] { graph_->disconnect(from, to); });
}

RetCode AudioCenter::disconnect(NodeId to)
{
    return call_status(*commands_, [&] { graph_->disconnect(to); });
}

RetCode AudioCenter::remove(NodeId node)
{
    return call_status(*commands_, [&] { graph_->remove(node); });
}

RetCode AudioCenter::commit()
{
    return call_status(*commands_, [&] { graph_->commit(); });
}

RetCode AudioCenter::start()
{
    return call_status(*commands_, [&] { graph_->start(); });
}

RetCode AudioCenter::stop() noexcept
{
    return call_status(*commands_, [&] { graph_->stop(); });
}

RetCode AudioCenter::set_paused(NodeId node, bool paused)
{
    return call_status(*commands_, [&] { graph_->set_paused(node, paused); });
}

RetCode AudioCenter::set_gain(NodeId matrix, float gain)
{
    return call_status(*commands_, [&] { graph_->set_gain(matrix, gain); });
}

RetCode AudioCenter::set_muted(NodeId mixer_port, bool muted)
{
    return call_status(*commands_, [&] { graph_->set_muted(mixer_port, muted); });
}

RetCode AudioCenter::set_high_priority(NodeId mixer_port, bool high)
{
    return call_status(*commands_, [&] { graph_->set_high_priority(mixer_port, high); });
}

std::string AudioCenter::dump_json() const
{
    return dispatch(*commands_, [&] {
        return graph_->dump_json([this](NodeId id) noexcept { return devices_->status_of(id); });
    });
}

RetCode AudioCenter::last_error() noexcept
{
    return thread_last_error;
}

} // namespace aereborn
