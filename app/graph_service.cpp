#include "graph_service.h"
#include "backend/pull_sink.h"
#include "backend/push_source.h"
#include "backend/usr_capture.h"
#include "backend/usr_playback.h"
#include "backend/wav_backend.h"
#include "core/command_queue.h"
#include "diag/inspect.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>
#include <vector>

namespace aereborn
{
namespace
{

bool contains_node(View<const NodeId> nodes, NodeId id) noexcept
{
    return !nodes.empty() && std::find(nodes.begin(), nodes.end(), id) != nodes.end();
}

void validate_io_period(BandWidth bandwidth, std::chrono::microseconds io_period)
{
    // Backend construction validates bandwidth. Reject sub-frame periods before allocating the queue.
    constexpr auto micros_per_second = 1000000u;
    const auto rate = static_cast<std::uint32_t>(bandwidth);
    const std::chrono::microseconds min_period{(micros_per_second + rate - 1) / rate};
    if (io_period < min_period)
        throw Error(RetCode::EPARAM);
}

} // namespace

struct NodeBinding
{
    template <class Driver> struct Active;
    template <class Driver> struct Passive;
    struct Push;
    struct Pull;

    virtual ~NodeBinding() = default;
    virtual Backend &backend() noexcept = 0;
    virtual void start() { backend().start(); }
    virtual void stop() noexcept { backend().stop(); }
    virtual void unbind() noexcept { backend().unbind(); }
    virtual Result<std::size_t> push(const std::uint8_t *, std::size_t) { return RetCode::ETYPE; }
    virtual Result<std::size_t> pull(std::uint8_t *, std::size_t) { return RetCode::ETYPE; }
};

template <class Driver>
struct NodeBinding::Active final : NodeBinding
{
    template <class... Args>
    explicit Active(Args &&...args) : driver(std::forward<Args>(args)...) {}

    Backend &backend() noexcept override { return driver; }

    Driver driver;
};

template <class Driver>
struct NodeBinding::Passive : NodeBinding
{
    // Constructing the driver validates the PCM spec before the frame limit is calculated.
    explicit Passive(PortSpec spec)
        : driver(spec), max_frames_(std::numeric_limits<std::size_t>::max() / spec.channels / sample_bytes(spec.format))
    {
    }

    Backend &backend() noexcept final { return driver; }

    void start() final
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.start();
    }

    void stop() noexcept final
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.stop();
    }

    void unbind() noexcept final
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        driver.unbind();
    }

    Driver driver;

  protected:
    template <class Byte, class Fn>
    Result<std::size_t> transfer(Byte *data, std::size_t frames, Fn fn)
    {
        if (frames && !data)
            return RetCode::EPARAM;
        if (frames > max_frames_)
            return RetCode::TOOLARGE;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (!driver.running())
            return driver.bound() ? RetCode::ESTATE : RetCode::ENODE;
        return fn({data, driver.device_spec(), frames});
    }

  private:
    std::mutex mutex_;
    const std::size_t max_frames_;
};

struct NodeBinding::Push final : Passive<PushSource>
{
    using Passive<PushSource>::Passive;

    Result<std::size_t> push(const std::uint8_t *data, std::size_t frames) override
    {
        return transfer(data, frames, [this](IBBlock block) { return driver.push(block); });
    }
};

struct NodeBinding::Pull final : Passive<PullSink>
{
    using Passive<PullSink>::Passive;

    Result<std::size_t> pull(std::uint8_t *data, std::size_t frames) override
    {
        return transfer(data, frames, [this](OBBlock block) { return driver.pull(block); });
    }
};

GraphService::GraphService(CommandQueue &commands, BlockPeriod period) : commands_(commands), engine_(period) {}

GraphService::~GraphService()
{
    assert(commands_.on_thread());
    // External calls have already returned, so teardown can unbind directly.
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
        if (const auto &endpoint = it->second.endpoint)
            endpoint->backend().unbind();
    engine_.stop();
}

GraphService::NodeMap::iterator GraphService::add_node(std::unique_ptr<Node> node, bool published)
{
    const auto id = engine_.add_node(std::move(node));
    const auto cleanup = [&]() noexcept { engine_.remove_node(id); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    const std::lock_guard<std::mutex> lock(nodes_mutex_);
    const auto entry = nodes_.emplace(id, NodeEntry{{}, published}).first;
    rollback.dismiss();
    return entry;
}

std::shared_ptr<NodeBinding> GraphService::detach_node(NodeMap::iterator entry)
{
    const std::lock_guard<std::mutex> lock(nodes_mutex_);
    auto endpoint = std::move(entry->second.endpoint);
    nodes_.erase(entry);
    return endpoint;
}

template <class Endpoint, class Binding>
NodeId GraphService::add_endpoint(std::unique_ptr<Endpoint> endpoint, std::shared_ptr<Binding> binding)
{
    auto *node = endpoint.get();
    node->prepare(engine_.block_period());
    // Reserve the table entry before bind() can create a file, then publish after binding.
    const auto entry = add_node(std::move(endpoint), /*published=*/false);
    const auto id = entry->first;
    const auto cleanup = [&]() noexcept {
        binding.reset(); // Destroy the unpublished backend before removing the node it borrows.
        detach_node(entry);
        engine_.remove_node(id);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    binding->driver.bind(*node);
    {
        const std::lock_guard<std::mutex> lock(nodes_mutex_);
        entry->second.endpoint = std::move(binding);
        entry->second.published = true;
    }
    rollback.dismiss();
    return id;
}

NodeId GraphService::create_iwav(const std::string &path, int repeat, std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    using Binding = NodeBinding::Active<WavCapture>;
    auto binding = repeat > 0 ? std::make_shared<Binding>(path, WavCapture::Repeat{static_cast<std::size_t>(repeat)})
                              : std::make_shared<Binding>(path, repeat < 0);
    const auto spec = binding->driver.probe().value_or_throw();
    auto endpoint = std::make_unique<Capture>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

NodeId GraphService::create_owav(const std::string &path, PortSpec spec, std::chrono::seconds segment_duration,
                                 std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    if (!spec.valid())
        throw Error(RetCode::EPARAM);
    throw_if_error(WavFile::max_write_frames(spec).ret_code());
    auto endpoint = std::make_unique<Playback>(spec.bandwidth, spec.channels, io_period);
    auto binding = std::make_shared<NodeBinding::Active<WavPlayback>>(path, spec.format, segment_duration,
                                                                  WavFile::WriteMode::Exclusive);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

NodeId GraphService::create_icallback(PortSpec spec, AudioCallback callback, void *user_data,
                                     std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    if (!callback)
        throw Error(RetCode::EPARAM);
    auto binding = std::make_shared<NodeBinding::Active<UsrCapture>>(spec, [callback, user_data](OBBlock block) noexcept {
        callback(nullptr, block.byte_view().data(), block.frames(), block.spec(), user_data);
    });
    validate_io_period(spec.bandwidth, io_period);
    auto endpoint = std::make_unique<Capture>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

NodeId GraphService::create_ocallback(PortSpec spec, AudioCallback callback, void *user_data,
                                     std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    if (!callback)
        throw Error(RetCode::EPARAM);
    auto binding = std::make_shared<NodeBinding::Active<UsrPlayback>>(spec, [callback, user_data](IBBlock block) noexcept {
        callback(block.byte_view().data(), nullptr, block.frames(), block.spec(), user_data);
    });
    validate_io_period(spec.bandwidth, io_period);
    auto endpoint = std::make_unique<Playback>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

NodeId GraphService::create_ipush(PortSpec spec, std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    auto binding = std::make_shared<NodeBinding::Push>(spec);
    validate_io_period(spec.bandwidth, io_period);
    auto endpoint = std::make_unique<Capture>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

NodeId GraphService::create_opull(PortSpec spec, std::chrono::microseconds io_period)
{
    assert(commands_.on_thread());
    auto binding = std::make_shared<NodeBinding::Pull>(spec);
    validate_io_period(spec.bandwidth, io_period);
    auto endpoint = std::make_unique<Playback>(spec.bandwidth, spec.channels, io_period);
    return add_endpoint(std::move(endpoint), std::move(binding));
}

Result<std::shared_ptr<NodeBinding>> GraphService::find_io_endpoint(NodeId node)
{
    if (node.port() != 0)
        return RetCode::EPORT;

    const std::lock_guard<std::mutex> lock(nodes_mutex_);
    const auto entry = nodes_.find(node);
    if (entry == nodes_.end() || !entry->second.published)
        return RetCode::ENODE;
    if (!entry->second.endpoint)
        return RetCode::ETYPE;
    return entry->second.endpoint;
}

Result<std::size_t> GraphService::push(NodeId node, const void *data, std::size_t frames)
{
    const auto endpoint = find_io_endpoint(node);
    if (!endpoint)
        return endpoint.ret_code();
    return endpoint.value()->push(static_cast<const std::uint8_t *>(data), frames);
}

Result<std::size_t> GraphService::pull(NodeId node, void *data, std::size_t frames)
{
    const auto endpoint = find_io_endpoint(node);
    if (!endpoint)
        return endpoint.ret_code();
    return endpoint.value()->pull(static_cast<std::uint8_t *>(data), frames);
}

NodeId GraphService::create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Mixer>(bandwidth, channels, inputs))->first;
}

NodeId GraphService::create_matrix(BandWidth bandwidth, std::size_t input_channels, std::size_t output_channels,
                                   View<const float> coefficients, float gain)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Matrix>(bandwidth, input_channels, output_channels, coefficients, gain))->first;
}

NodeId GraphService::create_convert(PortSpec input, PortSpec output)
{
    assert(commands_.on_thread());
    return add_node(std::make_unique<Convert>(input, output))->first;
}

GraphService::NodeMap::iterator GraphService::find_node(NodeId node)
{
    assert(commands_.on_thread());
    if (node.port() != 0)
        throw Error(RetCode::EPORT);
    const auto entry = nodes_.find(node);
    if (entry == nodes_.end())
        throw Error(RetCode::ENODE);
    return entry;
}

PortSpec GraphService::port_spec(NodeId port, bool output)
{
    find_node(port.base());
    const auto *node = engine_.node(port.base());
    const auto ports = output ? node->outputs() : node->inputs();
    if (port.port() >= ports.size())
        throw Error(RetCode::EPORT);
    return ports[port.port()];
}

void GraphService::connect(NodeId from, NodeId to)
{
    assert(commands_.on_thread());
    // Reserve bookkeeping before editing the draft, so an allocation failure leaves it untouched.
    const auto found = links_.lower_bound(to);
    const bool insert = found == links_.end() || found->first != to;
    const auto entry = insert ? links_.emplace_hint(found, to, Connection{}) : found;
    const auto cleanup = [&]() noexcept {
        if (insert)
            links_.erase(entry);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    entry->second = {engine_.connect(from, to), from, to};
    rollback.dismiss();
}

void GraphService::disconnect(NodeId from, NodeId to)
{
    disconnect_input(to, &from);
}

void GraphService::disconnect(NodeId to)
{
    disconnect_input(to, nullptr);
}

void GraphService::disconnect_input(NodeId to, const NodeId *from)
{
    assert(commands_.on_thread());
    const auto link = links_.find(to);
    if (link == links_.end() || (from && link->second.from != *from))
    {
        if (from)
            port_spec(*from, true);
        port_spec(to, false);
        throw Error(RetCode::ECONN);
    }
    engine_.disconnect(link->second.id);
    links_.erase(link);
}

void GraphService::remove(NodeId node)
{
    if (const auto endpoint = detach_node(find_node(node)))
        endpoint->unbind();
    for (auto it = links_.begin(); it != links_.end();)
    {
        if (it->first.base() == node || it->second.from.base() == node)
            it = links_.erase(it);
        else
            ++it;
    }
    engine_.remove_node(node);
}

template <class Operation>
void GraphService::visit_endpoint(NodeId node, Operation operation)
{
    const auto entry = nodes_.find(node);
    if (entry != nodes_.end() && entry->second.endpoint)
        operation(*entry->second.endpoint);
}

void GraphService::commit()
{
    assert(commands_.on_thread());
    if (!engine_.dirty())
        return;

    const auto running = engine_.state() == Engine::State::Running;
    std::vector<NodeId> previous;
    if (running)
    {
        const auto plan = engine_.plan_nodes();
        if (!plan.empty())
            previous.assign(plan.begin(), plan.end());
    }

    engine_.commit();
    if (!running)
        return;

    const auto current = engine_.plan_nodes();
    const View<const NodeId> old_plan{previous.data(), previous.size()};
    const auto cleanup = [this]() noexcept { stop(); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    for (auto it = previous.rbegin(); it != previous.rend(); ++it)
        visit_endpoint(*it, [&](auto &endpoint) {
            if (!contains_node(current, *it))
                endpoint.stop();
        });
    for (const auto id : current)
        visit_endpoint(id, [&](auto &endpoint) {
            if (!contains_node(old_plan, id))
                endpoint.start();
        });
    rollback.dismiss();
}

void GraphService::start()
{
    assert(commands_.on_thread());
    if (engine_.state() == Engine::State::Running)
        throw Error(RetCode::BUSY);
    engine_.commit();
    const auto cleanup = [this]() noexcept { stop(); };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    for (const auto id : engine_.plan_nodes())
        visit_endpoint(id, [](auto &endpoint) { endpoint.start(); });
    engine_.start();
    rollback.dismiss();
}

void GraphService::stop() noexcept
{
    assert(commands_.on_thread());
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
        if (const auto &endpoint = it->second.endpoint)
            endpoint->stop();
    engine_.stop();
}

void GraphService::set_paused(NodeId node, bool paused)
{
    const auto entry = find_node(node);
    engine_.set_paused(node, paused);
    entry->second.paused = paused;
}

void GraphService::set_gain(NodeId matrix, float gain)
{
    find_node(matrix);
    engine_.node<Matrix>(matrix)->set_gain(gain);
}

void GraphService::set_muted(NodeId mixer_port, bool muted)
{
    find_node(mixer_port.base());
    engine_.node<Mixer>(mixer_port.base())->set_muted(mixer_port.port(), muted);
}

void GraphService::set_high_priority(NodeId mixer_port, bool high)
{
    find_node(mixer_port.base());
    engine_.node<Mixer>(mixer_port.base())->set_high_priority(mixer_port.port(), high);
}

std::string GraphService::dump_json()
{
    assert(commands_.on_thread());
    std::vector<const Backend *> drivers;
    std::vector<ConfiguredNode> nodes;
    std::vector<Connection> connections;
    nodes.reserve(nodes_.size());
    connections.reserve(links_.size());
    for (const auto &entry : nodes_)
    {
        nodes.push_back({entry.first, engine_.node(entry.first), entry.second.paused});
        if (const auto &endpoint = entry.second.endpoint)
            drivers.push_back(&endpoint->backend());
    }
    for (const auto &entry : links_)
        connections.push_back(entry.second);
    std::sort(connections.begin(), connections.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return aereborn::dump_json(engine_, {nodes.data(), nodes.size()}, {connections.data(), connections.size()},
                              {drivers.data(), drivers.size()});
}

} // namespace aereborn
