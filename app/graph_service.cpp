#include "graph_service.h"
#include "core/command_queue.h"
#include "diag/inspect.h"
#include "plugins/convert.h"
#include "plugins/matrix.h"
#include "plugins/mixer.h"
#include <algorithm>
#include <cassert>
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

} // namespace

GraphService::GraphService(CommandQueue &commands, BlockPeriod period)
    : commands_(commands), engine_(period)
{
}

GraphService::~GraphService()
{
    assert(commands_.on_thread());
    // External calls have already returned, so teardown can unbind directly.
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
        if (const auto &endpoint = it->second.endpoint)
            endpoint->unbind();
    engine_.stop();
}

NodeId GraphService::add_node(std::unique_ptr<Node> node)
{
    assert(commands_.on_thread());
    return insert_node(std::move(node), /*published=*/true)->first;
}

GraphService::NodeMap::iterator GraphService::insert_node(std::unique_ptr<Node> node, bool published)
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

NodeId GraphService::add_endpoint(std::unique_ptr<Node> endpoint, std::shared_ptr<NodeBinding> binding,
                                 const std::function<void()> &bind)
{
    assert(commands_.on_thread());
    endpoint->prepare(engine_.block_period());
    // Reserve the table entry before bind() can create a file, then publish after binding.
    const auto entry = insert_node(std::move(endpoint), /*published=*/false);
    const auto id = entry->first;
    const auto cleanup = [&]() noexcept {
        binding.reset(); // Destroy the unpublished backend before removing the node it borrows.
        detach_node(entry);
        engine_.remove_node(id);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);
    bind();
    {
        const std::lock_guard<std::mutex> lock(nodes_mutex_);
        entry->second.endpoint = std::move(binding);
        entry->second.published = true;
    }
    rollback.dismiss();
    return id;
}

Result<std::shared_ptr<NodeBinding>> GraphService::find_binding(NodeId node, BindingMatcher matches) noexcept
{
    if (node.port() != 0)
        return RetCode::EPORT;

    const std::lock_guard<std::mutex> lock(nodes_mutex_);
    const auto entry = nodes_.find(node);
    if (entry == nodes_.end() || !entry->second.published)
        return RetCode::ENODE;
    const auto &endpoint = entry->second.endpoint;
    if (endpoint && matches(*endpoint))
        return endpoint;
    return RetCode::ETYPE;
}

NodeId GraphService::create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs)
{
    return add_node(std::make_unique<Mixer>(bandwidth, channels, inputs));
}

NodeId GraphService::create_matrix(BandWidth bandwidth, std::size_t input_channels, std::size_t output_channels,
                                   View<const float> coefficients, float gain)
{
    return add_node(std::make_unique<Matrix>(bandwidth, input_channels, output_channels, coefficients, gain));
}

NodeId GraphService::create_convert(PortSpec input, PortSpec output)
{
    return add_node(std::make_unique<Convert>(input, output));
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

NodeBinding *GraphService::find_endpoint(NodeId node) noexcept
{
    const auto entry = nodes_.find(node);
    return entry == nodes_.end() ? nullptr : entry->second.endpoint.get();
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
        if (auto *endpoint = find_endpoint(*it))
            if (!contains_node(current, *it))
                endpoint->stop();
    for (const auto id : current)
        if (auto *endpoint = find_endpoint(id))
            if (!contains_node(old_plan, id))
                endpoint->start();
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
        if (auto *endpoint = find_endpoint(id))
            endpoint->start();
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

std::string GraphService::dump_json(const DeviceStatusFn &device_status)
{
    assert(commands_.on_thread());
    std::vector<const Backend *> drivers;
    std::vector<ConfiguredNode> nodes;
    std::vector<Connection> connections;
    nodes.reserve(nodes_.size());
    connections.reserve(links_.size());
    for (const auto &entry : nodes_)
    {
        const auto &endpoint = entry.second.endpoint;
        nodes.push_back({entry.first, engine_.node(entry.first), entry.second.paused,
                         device_status ? device_status(entry.first) : DeviceStatus{}});
        if (const auto *backend = endpoint ? endpoint->backend() : nullptr)
            drivers.push_back(backend);
    }
    for (const auto &entry : links_)
        connections.push_back(entry.second);
    std::sort(connections.begin(), connections.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return aereborn::dump_json(engine_, {nodes.data(), nodes.size()}, {connections.data(), connections.size()},
                              {drivers.data(), drivers.size()});
}

} // namespace aereborn
