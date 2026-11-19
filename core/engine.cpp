#include "engine.h"
#include "observer.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace aereborn
{
namespace
{

std::uint64_t get_next_identifier() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

struct GraphNode
{
    explicit GraphNode(std::unique_ptr<Node> instance)
        : node(std::move(instance)), inputs(node->inputs()), outputs(node->outputs())
    {
    }

    View<const PortSpec> ports(PortDirection direction) const noexcept
    {
        return direction == PortDirection::Input ? inputs : outputs;
    }

    std::unique_ptr<Node> node;
    const View<const PortSpec> inputs;
    const View<const PortSpec> outputs;
    bool prepared = false;
};

bool same_ports(View<const PortSpec> a, View<const PortSpec> b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct Connection
{
    OPort from;
    IPort to;
    ConnOpts options;
};

struct PlanConnection
{
    IBlock source;
    OBlock destination;
    ChMatrix matrix;
    float gain;
    float target;
    std::size_t previous;
};

struct PlanNode
{
    NodeId id;
    std::shared_ptr<GraphNode> entry;
    std::vector<IBlock> inputs;
    std::vector<OBlock> outputs;
    View<float> input_samples{nullptr, 0};
    std::size_t first_connection = 0;
    std::size_t connection_count = 0;
};

struct PlanObserver
{
    std::shared_ptr<Observer> observer;
    IBlock source;
    const Node *pcm_node;
};

struct ObserverUpdate
{
    Observer *observer;
    Observer::Format format;
};

struct RenderPlan
{
    static constexpr auto no_connection = std::numeric_limits<std::size_t>::max();

    PlanRevision revision = 0;
    std::vector<PlanNode> nodes;
    std::vector<PlanConnection> connections;
    std::vector<PlanObserver> observers;
    std::vector<std::shared_ptr<Observer>> paused_observers;
    std::vector<float> samples;
    std::map<NodeId, std::size_t> node_index;
    std::map<ConnId, std::size_t> connection_index;

    void start_observers() noexcept
    {
        for (const auto &item : observers)
        {
            item.observer->start();
        }
    }

    void render(std::uint64_t frame) noexcept
    {
        for (const auto &item : nodes)
        {
            std::fill(item.input_samples.begin(), item.input_samples.end(), 0.0f);
            for (std::size_t i = item.first_connection; i < item.first_connection + item.connection_count; ++i)
            {
                auto &connection = connections[i];
                if (connection.gain != connection.target)
                {
                    connection.matrix.mix_ramp_add(connection.source, connection.destination,
                                                   connection.gain, connection.target);
                    connection.gain = connection.target;
                }
                else if (connection.gain != 0.0f)
                {
                    connection.matrix.mix_add(connection.source, connection.destination, connection.gain);
                }
            }
            item.entry->node->process({frame, {item.inputs.data(), item.inputs.size()},
                                       {item.outputs.data(), item.outputs.size()}});
        }
        for (const auto &item : observers)
        {
            if (item.pcm_node)
            {
                const auto port = item.observer->port();
                item.observer->write(item.pcm_node->pcm_block(port.direction, port.index), frame);
            }
            else
            {
                item.observer->write(item.source, frame);
            }
        }
    }
};

struct Command
{
    RenderPlan *plan = nullptr;
    PlanConnection *connection = nullptr;
    float gain = 0.0f;
};

using ConnectionEntry = std::pair<const ConnId, Connection>;
using Incoming = std::map<NodeId, std::vector<const ConnectionEntry *>>;

float effective_gain(float gain, bool muted) noexcept
{
    return muted ? 0.0f : gain;
}

} // namespace

struct Engine::Runtime
{
    explicit Runtime(EngineSpec spec) : spec(spec)
    {
    }

    const EngineSpec spec;
    std::map<NodeId, std::shared_ptr<GraphNode>> nodes;
    std::map<ConnId, Connection> connections;
    std::map<ObserverId, std::shared_ptr<Observer>> observers;
    std::set<NodeId> paused_nodes;
    std::unique_ptr<RenderPlan> current;
    std::unique_ptr<RenderPlan> pending;
    std::mutex plans_mutex; // Only the control caller and the reclamation task use this mutex.
    RenderPlan *active = nullptr; // Accessed only by the driver, or by control after the driver has joined.
    std::unique_ptr<ClockDriver> driver;
    std::unique_ptr<RuntimeTask> maintenance;
    std::unique_ptr<ObserverWorkers> observer_workers;
    SpscRing<Command> commands{64};
    std::atomic<PlanRevision> applied{0};
    std::atomic<std::uint64_t> position{0};
    bool dirty = true;

    bool on_execution_thread() const noexcept
    {
        return engine_execution_context == this;
    }

    bool lock_free() const noexcept
    {
        return commands.lock_free() && applied.is_lock_free() && position.is_lock_free();
    }

    PortSpec resolve_port_format(PortSpec format) const noexcept
    {
        if (!format.is_pcm())
        {
            format.spec.sample_rate = spec.sample_rate();
        }
        return format;
    }

    Result<PortSpec> port_spec(ObservationPort port) const noexcept
    {
        const auto found = nodes.find(port.node);
        if (found == nodes.end())
        {
            return ErrorCode::UnknownNode;
        }
        const auto ports = found->second->ports(port.direction);
        if (port.index >= ports.size())
        {
            return ErrorCode::InvalidPort;
        }
        return resolve_port_format(ports[port.index]);
    }

    bool would_cycle(NodeId from, NodeId to) const
    {
        std::vector<NodeId> stack{to};
        std::set<NodeId> visited{to};
        while (!stack.empty())
        {
            const auto id = stack.back();
            stack.pop_back();
            if (id == from)
            {
                return true;
            }
            for (const auto &item : connections)
            {
                const auto &connection = item.second;
                if (connection.from.node == id && visited.insert(connection.to.node).second)
                {
                    stack.push_back(connection.to.node);
                }
            }
        }
        return false;
    }

    RetCode visit(NodeId id, const Incoming &incoming, RenderPlan &plan, std::size_t &total,
                  std::size_t max_channels)
    {
        if (plan.node_index.count(id))
        {
            return {};
        }
        const auto found = incoming.find(id);
        if (found != incoming.end())
        {
            for (const auto *connection : found->second)
            {
                const auto code = visit(connection->second.from.node, incoming, plan, total, max_channels);
                if (!code)
                {
                    return code;
                }
            }
        }

        const auto &entry = nodes.find(id)->second;
        if (driver)
        {
            const auto old = current->node_index.find(id);
            if (old != current->node_index.end())
            {
                const auto &running = current->nodes[old->second].entry;
                if (entry != running && (!same_ports(entry->inputs, running->inputs) ||
                                         !same_ports(entry->outputs, running->outputs)))
                {
                    return ErrorCode::Busy;
                }
            }
        }
        for (const auto ports : {entry->inputs, entry->outputs})
        {
            for (const auto &port : ports)
            {
                if (port.is_pcm())
                {
                    continue;
                }
                const auto channels = port.spec.channels;
                if (channels > max_channels - total)
                {
                    return ErrorCode::TooLarge;
                }
                total += channels;
            }
        }
        if (!entry->prepared)
        {
            const auto code = entry->node->prepare(spec);
            if (!code)
            {
                return code;
            }
            entry->prepared = true;
        }
        plan.node_index.emplace(id, plan.nodes.size());
        plan.nodes.push_back(PlanNode{id, entry, std::vector<IBlock>(entry->inputs.size()),
                                      std::vector<OBlock>(entry->outputs.size())});
        return {};
    }

    RetCode build_plan(RenderPlan &plan, std::vector<ObserverUpdate> &updates)
    {
        const auto frames = spec.block_frames();
        if (frames == 0)
        {
            return ErrorCode::InvalidArg;
        }
        Incoming incoming;
        for (const auto &item : connections)
        {
            const auto &connection = item.second;
            if (!paused_nodes.count(connection.from.node) && !paused_nodes.count(connection.to.node))
            {
                incoming[connection.to.node].push_back(&item);
            }
        }
        for (auto &item : incoming)
        {
            auto &ports = item.second;
            std::sort(ports.begin(), ports.end(), [](const ConnectionEntry *lhs, const ConnectionEntry *rhs) {
                const auto &a = lhs->second;
                const auto &b = rhs->second;
                return std::tie(a.to.index, a.from.node, a.from.index) <
                       std::tie(b.to.index, b.from.node, b.from.index);
            });
        }
        std::size_t total_channels = 0;
        const auto max_channels = plan.samples.max_size() / frames;
        for (const auto &item : nodes)
        {
            if (paused_nodes.count(item.first))
            {
                continue;
            }
            const auto outputs = item.second->outputs;
            if (outputs.empty() || std::any_of(outputs.begin(), outputs.end(), [](const PortSpec &port) {
                    return port.is_pcm();
                }))
            {
                const auto code = visit(item.first, incoming, plan, total_channels, max_channels);
                if (!code)
                {
                    return code;
                }
            }
        }
        for (const auto &item : observers)
        {
            const auto node = item.second->port().node;
            if (paused_nodes.count(node))
            {
                if (driver && current->node_index.count(node))
                {
                    plan.paused_observers.push_back(item.second);
                }
                continue;
            }
            const auto code = visit(node, incoming, plan, total_channels, max_channels);
            if (!code)
            {
                return code;
            }
        }

        plan.samples.resize(total_channels * frames);
        const std::vector<const ConnectionEntry *> no_inputs;
        std::size_t offset = 0;
        for (auto &node : plan.nodes)
        {
            const auto found = incoming.find(node.id);
            const auto &inputs = found == incoming.end() ? no_inputs : found->second;
            auto edge = inputs.begin();
            node.first_connection = plan.connections.size();
            node.connection_count = inputs.size();
            const auto start = offset;
            for (std::size_t port = 0; port < node.inputs.size(); ++port)
            {
                const auto &format = node.entry->inputs[port];
                if (format.is_pcm())
                {
                    continue;
                }
                const auto channels = format.spec.channels;
                const OBlock destination{plan.samples.data() + offset, channels, frames, frames};
                node.inputs[port] = destination;
                offset += channels * frames;
                // visit() placed every source before this node, and incoming edges are sorted by port.
                for (; edge != inputs.end() && (*edge)->second.to.index == port; ++edge)
                {
                    const auto &record = **edge;
                    const auto &connection = record.second;
                    const auto source = plan.nodes[plan.node_index.find(connection.from.node)->second]
                                            .outputs[connection.from.index];
                    auto previous = RenderPlan::no_connection;
                    if (driver)
                    {
                        const auto old = current->connection_index.find(record.first);
                        if (old != current->connection_index.end())
                        {
                            previous = old->second;
                        }
                    }
                    const auto gain = effective_gain(connection.options.gain, connection.options.muted);
                    plan.connection_index.emplace(record.first, plan.connections.size());
                    plan.connections.push_back({source, destination, connection.options.matrix, gain, gain, previous});
                }
            }
            node.input_samples = {offset == start ? nullptr : plan.samples.data() + start, offset - start};
            for (std::size_t port = 0; port < node.outputs.size(); ++port)
            {
                const auto &format = node.entry->outputs[port];
                if (format.is_pcm())
                {
                    continue;
                }
                const auto channels = format.spec.channels;
                node.outputs[port] = {plan.samples.data() + offset, channels, frames, frames};
                offset += channels * frames;
            }
        }
        for (const auto &item : observers)
        {
            const auto &observer = item.second;
            const auto port = observer->port();
            if (paused_nodes.count(port.node))
            {
                continue;
            }
            auto &node = plan.nodes[plan.node_index.find(port.node)->second];
            const auto format = resolve_port_format(node.entry->ports(port.direction)[port.index]);
            auto prepared = observer->prepare_format(spec, format);
            if (!prepared)
            {
                return prepared.ret_code();
            }
            if (prepared.value() || (driver && current->node_index.count(port.node) == 0))
            {
                updates.push_back({observer.get(), std::move(prepared).value()});
            }
            const IBlock source = port.direction == PortDirection::Input ? node.inputs[port.index]
                                                                          : IBlock(node.outputs[port.index]);
            plan.observers.push_back({observer, source, format.is_pcm() ? node.entry->node.get() : nullptr});
        }
        return {};
    }

    void activate(RenderPlan *next, bool smooth) noexcept
    {
        if (smooth)
        {
            for (auto &connection : next->connections)
            {
                if (connection.previous != RenderPlan::no_connection)
                {
                    connection.gain = active->connections[connection.previous].gain;
                }
            }
            for (const auto &observer : next->paused_observers)
            {
                observer->request_stop();
            }
            next->start_observers();
        }
        active = next;
        // The previous plan is no longer touched by the driver after this release.
        applied.store(next->revision, std::memory_order_release);
    }

    std::unique_ptr<RenderPlan> collect() noexcept
    {
        if (pending && applied.load(std::memory_order_acquire) == pending->revision)
        {
            auto retired = std::move(current);
            current = std::move(pending);
            return retired;
        }
        return {};
    }

    void maintain()
    {
        const EngineExecutionScope scope(this);
        std::unique_ptr<RenderPlan> retired;
        {
            const std::lock_guard<std::mutex> lock(plans_mutex);
            retired = collect();
            if (observer_workers)
            {
                for (const auto &item : current->observers)
                {
                    observer_workers->schedule(item.observer, this);
                }
            }
        }
    }

    void apply_commands(bool smooth) noexcept
    {
        const auto end = commands.published();
        while (const auto *command = commands.read_slot(end))
        {
            if (command->plan)
            {
                activate(command->plan, smooth);
            }
            else
            {
                command->connection->target = command->gain;
                if (!smooth)
                {
                    command->connection->gain = command->gain;
                }
            }
            commands.consume();
        }
    }

    static void tick(void *context) noexcept
    {
        auto &runtime = *static_cast<Runtime *>(context);
        const EngineExecutionScope scope(context);
        runtime.apply_commands(true);
        const auto frame = runtime.position.load(std::memory_order_relaxed);
        runtime.active->render(frame);
        runtime.position.store(frame + runtime.spec.block_frames(), std::memory_order_release);
    }

    RetCode set_parameter(Connection &connection, ConnId id, float gain, bool muted) noexcept
    {
        std::unique_ptr<RenderPlan> retired;
        const std::lock_guard<std::mutex> lock(plans_mutex);
        retired = collect();
        auto *plan = pending ? pending.get() : current.get();
        if (plan)
        {
            const auto found = plan->connection_index.find(id);
            if (found != plan->connection_index.end())
            {
                auto &live = plan->connections[found->second];
                const auto target = effective_gain(gain, muted);
                if (driver)
                {
                    auto *command = commands.write_slot();
                    if (!command)
                    {
                        return ErrorCode::BufferFull;
                    }
                    *command = {nullptr, &live, target};
                    commands.publish();
                }
                else
                {
                    live.gain = live.target = target;
                }
            }
        }
        connection.options.gain = gain;
        connection.options.muted = muted;
        return {};
    }
};

Engine::Engine(SampleRate sample_rate, BlockPeriod block_period)
    : runtime_(std::make_unique<Runtime>(EngineSpec{sample_rate, block_period}))
{
}

Engine::~Engine()
{
    stop();
}

Result<NodeId> Engine::add_node(std::unique_ptr<Node> instance)
{
    if (runtime_->on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (!instance)
    {
        return ErrorCode::InvalidArg;
    }
    GraphNode entry(std::move(instance));
    for (const auto ports : {entry.inputs, entry.outputs})
    {
        if (std::any_of(ports.begin(), ports.end(), [](const PortSpec &port) {
                return port.spec.channels == 0;
            }))
        {
            return ErrorCode::InvalidArg;
        }
    }
    const NodeId id{get_next_identifier()};
    runtime_->nodes.emplace(id, std::make_shared<GraphNode>(std::move(entry)));
    runtime_->dirty = true;
    return id;
}

RetCode Engine::remove_node(NodeId id) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto found = runtime.nodes.find(id);
    if (found == runtime.nodes.end())
    {
        return ErrorCode::UnknownNode;
    }
    for (const auto &item : runtime.observers)
    {
        if (item.second->port().node == id)
        {
            return ErrorCode::Busy;
        }
    }
    for (auto it = runtime.connections.begin(); it != runtime.connections.end();)
    {
        if (it->second.from.node == id || it->second.to.node == id)
        {
            it = runtime.connections.erase(it);
        }
        else
        {
            ++it;
        }
    }
    runtime.nodes.erase(found);
    runtime.paused_nodes.erase(id);
    runtime.dirty = true;
    return {};
}

RetCode Engine::replace_node(NodeId id, std::unique_ptr<Node> replacement)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto found = runtime.nodes.find(id);
    if (found == runtime.nodes.end())
    {
        return ErrorCode::UnknownNode;
    }
    if (!replacement)
    {
        return ErrorCode::InvalidArg;
    }
    GraphNode entry(std::move(replacement));
    const auto compatible = [](View<const PortSpec> before, View<const PortSpec> after) {
        return before.size() == after.size() &&
               std::equal(before.begin(), before.end(), after.begin(), [](const PortSpec &a, const PortSpec &b) {
                   return a.layout == b.layout && b.spec.channels > 0 &&
                          (a.is_pcm() || a.spec.channels == b.spec.channels);
               });
    };
    const auto &before = *found->second;
    if (!compatible(before.inputs, entry.inputs) || !compatible(before.outputs, entry.outputs))
    {
        return ErrorCode::InvalidArg;
    }
    found->second = std::make_shared<GraphNode>(std::move(entry));
    runtime.dirty = true;
    return {};
}

RetCode Engine::set_paused(NodeId id, bool paused)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (runtime.nodes.count(id) == 0)
    {
        return ErrorCode::UnknownNode;
    }
    const bool changed = paused ? runtime.paused_nodes.insert(id).second : runtime.paused_nodes.erase(id) != 0;
    runtime.dirty |= changed;
    return {};
}

Result<Node *> Engine::node(NodeId id) noexcept
{
    const auto result = static_cast<const Engine &>(*this).node(id);
    if (!result)
    {
        return result.ret_code();
    }
    return const_cast<Node *>(result.value());
}

Result<const Node *> Engine::node(NodeId id) const noexcept
{
    if (runtime_->on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto found = runtime_->nodes.find(id);
    if (found == runtime_->nodes.end())
    {
        return ErrorCode::UnknownNode;
    }
    return found->second->node.get();
}

Result<ConnId> Engine::connect(OPort from, IPort to, const ConnOpts &options)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto source = runtime.port_spec(from);
    if (!source)
    {
        return source.ret_code();
    }
    const auto destination = runtime.port_spec(to);
    if (!destination)
    {
        return destination.ret_code();
    }
    if (source.value().is_pcm() || destination.value().is_pcm())
    {
        return ErrorCode::InvalidPort;
    }
    const auto source_channels = source.value().spec.channels;
    const auto destination_channels = destination.value().spec.channels;
    if (!std::isfinite(options.gain))
    {
        return ErrorCode::InvalidArg;
    }
    if (!options.matrix.matches(source_channels, destination_channels))
    {
        return ErrorCode::ChannelMismatch;
    }
    for (const auto &item : runtime.connections)
    {
        const auto &connection = item.second;
        if (connection.from.node == from.node && connection.from.index == from.index &&
            connection.to.node == to.node && connection.to.index == to.index)
        {
            return ErrorCode::DuplicateConn;
        }
    }
    if (runtime.would_cycle(from.node, to.node))
    {
        return ErrorCode::Cycle;
    }
    const ConnId id{get_next_identifier()};
    runtime.connections.emplace(id, Connection{from, to, options});
    runtime.dirty = true;
    return id;
}

RetCode Engine::disconnect(ConnId id) noexcept
{
    if (runtime_->on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (!runtime_->connections.erase(id))
    {
        return ErrorCode::UnknownConn;
    }
    runtime_->dirty = true;
    return {};
}

RetCode Engine::set_gain(ConnId id, float gain) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (!std::isfinite(gain))
    {
        return ErrorCode::InvalidArg;
    }
    const auto found = runtime.connections.find(id);
    if (found == runtime.connections.end())
    {
        return ErrorCode::UnknownConn;
    }
    return runtime.set_parameter(found->second, id, gain, found->second.options.muted);
}

RetCode Engine::set_muted(ConnId id, bool muted) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto found = runtime.connections.find(id);
    if (found == runtime.connections.end())
    {
        return ErrorCode::UnknownConn;
    }
    return runtime.set_parameter(found->second, id, found->second.options.gain, muted);
}

Result<PlanRevision> Engine::prepare()
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    std::unique_ptr<RenderPlan> retired;
    std::unique_lock<std::mutex> lock(runtime.plans_mutex);
    retired = runtime.collect();
    if (!runtime.dirty)
    {
        return (runtime.pending ? runtime.pending : runtime.current)->revision;
    }
    if (runtime.pending)
    {
        return ErrorCode::BufferFull;
    }
    Command *slot = nullptr;
    if (runtime.driver)
    {
        // Control is the only producer, so this slot stays available until publication.
        slot = runtime.commands.write_slot();
        if (!slot)
        {
            return ErrorCode::BufferFull;
        }
    }

    auto plan = std::make_unique<RenderPlan>();
    const EngineExecutionScope scope(&runtime);
    std::vector<ObserverUpdate> updates;
    const auto code = runtime.build_plan(*plan, updates);
    if (!code)
    {
        return code;
    }
    if (slot && !plan->observers.empty() && !runtime.observer_workers)
    {
        runtime.observer_workers = std::make_unique<ObserverWorkers>();
    }
    if (!updates.empty())
    {
        // The previous plan has detached these writers or the driver is stopped.
        // A user callback may still be finishing, so leave the shared scheduler free to run.
        lock.unlock();
        for (auto &update : updates)
        {
            update.observer->configure(std::move(update.format));
        }
        lock.lock();
    }

    const auto revision = runtime.current ? runtime.current->revision + 1 : 1;
    plan->revision = revision;
    if (slot)
    {
        runtime.pending = std::move(plan);
        *slot = {runtime.pending.get(), nullptr, 0};
        runtime.commands.publish();
    }
    else
    {
        runtime.activate(plan.get(), false);
        runtime.current = std::move(plan);
    }
    runtime.dirty = false;
    return revision;
}

RetCode Engine::start(std::unique_ptr<ClockDriver> driver)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread() || runtime.driver)
    {
        return ErrorCode::Busy;
    }
    if (!runtime.current)
    {
        return ErrorCode::NotPrepared;
    }
    if (!runtime.lock_free())
    {
        return ErrorCode::Unsupported;
    }
    runtime.driver = driver ? std::move(driver) : make_timer_driver();
    try
    {
        if (!runtime.current->observers.empty())
        {
            runtime.observer_workers = std::make_unique<ObserverWorkers>();
            runtime.current->start_observers();
        }
        runtime.maintenance = start_background_task([&runtime] { runtime.maintain(); });
        const auto code = runtime.driver->start(runtime.spec, &Runtime::tick, &runtime);
        if (!code)
        {
            stop();
        }
        return code;
    }
    catch (...)
    {
        stop();
        throw;
    }
}

RetCode Engine::stop() noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (runtime.driver)
    {
        runtime.driver->stop();
        runtime.driver.reset();
        for (const auto &item : runtime.observers)
        {
            item.second->request_stop();
        }
        runtime.maintenance.reset();
        runtime.observer_workers.reset();
        runtime.apply_commands(false);
        runtime.collect();
        for (const auto &item : runtime.observers)
        {
            item.second->clear_when_idle();
        }
    }
    return {};
}

RetCode Engine::reset() noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread() || runtime.driver)
    {
        return ErrorCode::Busy;
    }
    // A discarded draft can still leave a node in the last published plan.
    if (runtime.current)
    {
        for (const auto &item : runtime.current->nodes)
        {
            item.entry->node->reset();
        }
    }
    for (const auto &item : runtime.nodes)
    {
        const auto index = runtime.current ? runtime.current->node_index.find(item.first)
                                           : std::map<NodeId, std::size_t>::const_iterator{};
        if (!runtime.current || index == runtime.current->node_index.end() ||
            runtime.current->nodes[index->second].entry != item.second)
        {
            item.second->node->reset();
        }
    }
    for (const auto &item : runtime.observers)
    {
        item.second->reset_when_idle();
    }
    runtime.position.store(0, std::memory_order_release);
    return {};
}

Result<ObserverId> Engine::add_observer(std::unique_ptr<Observer> observer)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    if (!observer)
    {
        return ErrorCode::InvalidArg;
    }
    const auto found = runtime.port_spec(observer->port());
    if (!found)
    {
        return found.ret_code();
    }
    const auto id = get_next_identifier();
    runtime.observers.emplace(id, std::move(observer));
    runtime.dirty = true;
    return id;
}

Result<PortSpec> Engine::port_spec(ObservationPort port) const noexcept
{
    return runtime_->on_execution_thread() ? Result<PortSpec>{ErrorCode::Busy} : runtime_->port_spec(port);
}

RetCode Engine::remove_observer(ObserverId id) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
    {
        return ErrorCode::Busy;
    }
    const auto found = runtime.observers.find(id);
    if (found == runtime.observers.end())
    {
        return ErrorCode::UnknownObserver;
    }
    found->second->cancel();
    runtime.observers.erase(found);
    runtime.dirty = true;
    return {};
}

EngineSpec Engine::spec() const noexcept
{
    return runtime_->spec;
}

std::uint64_t Engine::frame_position() const noexcept
{
    return runtime_->position.load(std::memory_order_acquire);
}

PlanRevision Engine::applied_revision() const noexcept
{
    return runtime_->applied.load(std::memory_order_acquire);
}

} // namespace aereborn
