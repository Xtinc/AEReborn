#include "engine.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace aereborn
{
namespace
{
std::uint64_t next_identifier() noexcept
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
    View<const PortSpec> ports(IPort) const noexcept { return inputs; }
    View<const PortSpec> ports(OPort) const noexcept { return outputs; }
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
};

// Float storage contains actual float objects; PCM storage is byte-addressable.
// Input views alias an upstream output or an immutable zero-filled buffer.
struct PortBuffer
{
    Result<OPortBlock> prepare(PortSpec format, BlockPeriod period)
    {
        const auto frames = format.max_frames(period);
        const auto width = sample_bytes(format.format);
        if (format.format == Format::F32P)
        {
            if (format.channels > audio.max_size() / frames)
                return ErrorCode::TooLarge;
            audio.resize(format.channels * frames);
        }
        else
        {
            if (format.channels > pcm.max_size() / frames / width)
                return ErrorCode::TooLarge;
            pcm.resize(format.channels * frames * width);
        }
        auto *data = format.format == Format::F32P ? reinterpret_cast<std::uint8_t *>(audio.data()) : pcm.data();
        return OPortBlock{data, format, frames};
    }
    std::vector<float> audio;
    std::vector<std::uint8_t> pcm;
};
struct PlanNode
{
    NodeId id;
    std::shared_ptr<GraphNode> entry;
    std::vector<IPortBlock> inputs;
    std::vector<OPortBlock> outputs;
};
struct RenderPlan
{
    PlanRevision revision = 0;
    std::vector<PortBuffer> buffers;
    std::vector<PlanNode> nodes;
    Result<OPortBlock> add_buffer(PortSpec format, BlockPeriod period)
    {
        if (buffers.size() == buffers.max_size())
            return ErrorCode::TooLarge;
        // Views point into the owned allocations, which stay valid when PortBuffer is moved.
        buffers.emplace_back();
        return buffers.back().prepare(format, period);
    }
    void render(BlockPeriod period, std::uint64_t tick) noexcept
    {
        for (auto &item : nodes)
        {
            for (auto &input : item.inputs)
                input = {input.bytes().data(), input.spec(), input.spec().frames(period, tick)};
            for (auto &output : item.outputs)
                output = {output.bytes().data(), output.spec(), output.spec().frames(period, tick)};
            item.entry->node->process(
                {tick, {item.inputs.data(), item.inputs.size()}, {item.outputs.data(), item.outputs.size()}});
        }
    }
};
using Incoming = std::map<NodeId, std::vector<const Connection *>>;
using NodeIndex = std::map<NodeId, std::size_t>;
} // namespace

struct Engine::Runtime
{
    explicit Runtime(BlockPeriod period) : period(period) {}
    const BlockPeriod period;
    std::map<NodeId, std::shared_ptr<GraphNode>> nodes;
    std::map<ConnId, Connection> connections;
    std::set<NodeId> paused;
    std::unique_ptr<RenderPlan> current;
    std::unique_ptr<RenderPlan> pending;
    std::mutex plans_mutex;
    RenderPlan *active = nullptr;
    std::atomic<RenderPlan *> submitted{nullptr};
    std::unique_ptr<ClockDriver> driver;
    std::unique_ptr<RuntimeTask> maintenance;
    std::atomic<PlanRevision> applied{0};
    std::atomic<std::uint64_t> position{0};
    bool dirty = true;

    bool on_execution_thread() const noexcept { return engine_execution_context == this; }
    template <class Port> Result<PortSpec> port_spec(Port port) const noexcept
    {
        const auto found = nodes.find(port.node);
        if (found == nodes.end())
            return ErrorCode::UnknownNode;
        const auto ports = found->second->ports(port);
        if (port.index >= ports.size())
            return ErrorCode::InvalidPort;
        return ports[port.index];
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
                return true;
            for (const auto &item : connections)
                if (item.second.from.node == id && visited.insert(item.second.to.node).second)
                    stack.push_back(item.second.to.node);
        }
        return false;
    }
    RetCode visit(NodeId id, const Incoming &incoming, RenderPlan &plan, NodeIndex &node_index)
    {
        if (node_index.count(id))
            return {};
        const auto found = incoming.find(id);
        if (found != incoming.end())
            for (const auto *connection : found->second)
            {
                const auto code = visit(connection->from.node, incoming, plan, node_index);
                if (!code)
                    return code;
            }
        const auto &entry = nodes.find(id)->second;
        if (!entry->prepared)
        {
            const auto code = entry->node->prepare(period);
            if (!code)
                return code;
            entry->prepared = true;
        }
        node_index.emplace(id, plan.nodes.size());
        plan.nodes.push_back(
            {id, entry, std::vector<IPortBlock>(entry->inputs.size()), std::vector<OPortBlock>(entry->outputs.size())});
        return {};
    }
    RetCode build_plan(RenderPlan &plan)
    {
        if (!is_block_period(period))
            return ErrorCode::InvalidArg;
        Incoming incoming;
        NodeIndex node_index;
        for (const auto &item : connections)
            if (!paused.count(item.second.from.node) && !paused.count(item.second.to.node))
                incoming[item.second.to.node].push_back(&item.second);
        for (const auto &item : nodes)
            if (!paused.count(item.first) && item.second->outputs.empty())
            {
                const auto code = visit(item.first, incoming, plan, node_index);
                if (!code)
                    return code;
            }
        for (auto &item : plan.nodes)
        {
            for (std::size_t port = 0; port < item.outputs.size(); ++port)
            {
                const auto block = plan.add_buffer(item.entry->outputs[port], period);
                if (!block)
                    return block.ret_code();
                item.outputs[port] = block.value();
            }
            const auto found = incoming.find(item.id);
            if (found != incoming.end())
                for (const auto *connection : found->second)
                    item.inputs[connection->to.index] =
                        plan.nodes[node_index.find(connection->from.node)->second].outputs[connection->from.index];
            for (std::size_t port = 0; port < item.inputs.size(); ++port)
                if (!item.inputs[port].bytes().data())
                {
                    const auto block = plan.add_buffer(item.entry->inputs[port], period);
                    if (!block)
                        return block.ret_code();
                    item.inputs[port] = block.value();
                }
        }
        return {};
    }
    void activate(RenderPlan *plan) noexcept
    {
        active = plan;
        applied.store(plan->revision, std::memory_order_release);
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
        }
    }
    void apply_pending() noexcept
    {
        if (auto *plan = submitted.exchange(nullptr, std::memory_order_acquire))
            activate(plan);
    }
    static void tick(void *context) noexcept
    {
        auto &runtime = *static_cast<Runtime *>(context);
        const EngineExecutionScope scope(context);
        runtime.apply_pending();
        const auto tick = runtime.position.load(std::memory_order_relaxed);
        runtime.active->render(runtime.period, tick);
        runtime.position.store(tick + 1, std::memory_order_release);
    }
};

Engine::Engine(BlockPeriod period) : runtime_(std::make_unique<Runtime>(period)) {}
Engine::~Engine()
{
    stop();
}

Result<NodeId> Engine::add_node(std::unique_ptr<Node> instance)
{
    if (runtime_->on_execution_thread())
        return ErrorCode::Busy;
    if (!instance)
        return ErrorCode::InvalidArg;
    auto entry = std::make_shared<GraphNode>(std::move(instance));
    if (entry->inputs.empty() && entry->outputs.empty())
        return ErrorCode::InvalidArg;
    for (const auto ports : {entry->inputs, entry->outputs})
        for (const auto &port : ports)
            if (!port.valid())
                return ErrorCode::InvalidArg;
    const NodeId id{next_identifier()};
    runtime_->nodes.emplace(id, std::move(entry));
    runtime_->dirty = true;
    return id;
}
RetCode Engine::remove_node(NodeId id) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    if (!runtime.nodes.erase(id))
        return ErrorCode::UnknownNode;
    for (auto it = runtime.connections.begin(); it != runtime.connections.end();)
        if (it->second.from.node == id || it->second.to.node == id)
            it = runtime.connections.erase(it);
        else
            ++it;
    runtime.paused.erase(id);
    runtime.dirty = true;
    return {};
}
RetCode Engine::replace_node(NodeId id, std::unique_ptr<Node> replacement)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime.nodes.find(id);
    if (found == runtime.nodes.end())
        return ErrorCode::UnknownNode;
    if (!replacement)
        return ErrorCode::InvalidArg;
    auto entry = std::make_shared<GraphNode>(std::move(replacement));
    if (!same_ports(found->second->inputs, entry->inputs) || !same_ports(found->second->outputs, entry->outputs))
        return ErrorCode::FormatMismatch;
    found->second = std::move(entry);
    runtime.dirty = true;
    return {};
}
RetCode Engine::set_paused(NodeId id, bool paused)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    if (!runtime.nodes.count(id))
        return ErrorCode::UnknownNode;
    const bool changed = paused ? runtime.paused.insert(id).second : runtime.paused.erase(id) != 0;
    runtime.dirty |= changed;
    return {};
}
Result<Node *> Engine::node(NodeId id) noexcept
{
    const auto result = static_cast<const Engine &>(*this).node(id);
    if (!result)
        return result.ret_code();
    return const_cast<Node *>(result.value());
}
Result<const Node *> Engine::node(NodeId id) const noexcept
{
    if (runtime_->on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime_->nodes.find(id);
    if (found == runtime_->nodes.end())
        return ErrorCode::UnknownNode;
    return found->second->node.get();
}
Result<ConnId> Engine::connect(OPort from, IPort to)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto source = runtime.port_spec(from);
    if (!source)
        return source.ret_code();
    const auto destination = runtime.port_spec(to);
    if (!destination)
        return destination.ret_code();
    if (source.value() != destination.value())
        return ErrorCode::FormatMismatch;
    for (const auto &item : runtime.connections)
        if (item.second.to.node == to.node && item.second.to.index == to.index)
            return item.second.from.node == from.node && item.second.from.index == from.index
                       ? ErrorCode::DuplicateConn
                       : ErrorCode::InputConnected;
    if (runtime.would_cycle(from.node, to.node))
        return ErrorCode::Cycle;
    const ConnId id{next_identifier()};
    runtime.connections.emplace(id, Connection{from, to});
    runtime.dirty = true;
    return id;
}
RetCode Engine::disconnect(ConnId id) noexcept
{
    if (runtime_->on_execution_thread())
        return ErrorCode::Busy;
    if (!runtime_->connections.erase(id))
        return ErrorCode::UnknownConn;
    runtime_->dirty = true;
    return {};
}
Result<PlanRevision> Engine::prepare()
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    std::unique_ptr<RenderPlan> retired;
    const std::lock_guard<std::mutex> lock(runtime.plans_mutex);
    retired = runtime.collect();
    if (!runtime.dirty)
        return (runtime.pending ? runtime.pending : runtime.current)->revision;
    if (runtime.pending)
        return ErrorCode::BufferFull;
    auto plan = std::make_unique<RenderPlan>();
    const EngineExecutionScope scope(&runtime);
    const auto code = runtime.build_plan(*plan);
    if (!code)
        return code;
    const auto revision = runtime.current ? runtime.current->revision + 1 : 1;
    plan->revision = revision;
    if (runtime.driver)
    {
        runtime.pending = std::move(plan);
        runtime.submitted.store(runtime.pending.get(), std::memory_order_release);
    }
    else
    {
        runtime.activate(plan.get());
        runtime.current = std::move(plan);
    }
    runtime.dirty = false;
    return revision;
}
RetCode Engine::start(std::unique_ptr<ClockDriver> driver)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread() || runtime.driver)
        return ErrorCode::Busy;
    if (!runtime.current)
        return ErrorCode::NotPrepared;
    if (!runtime.submitted.is_lock_free() || !runtime.applied.is_lock_free() || !runtime.position.is_lock_free())
        return ErrorCode::Unsupported;
    runtime.driver = driver ? std::move(driver) : make_timer_driver();
    try
    {
        runtime.maintenance = start_background_task([&runtime] { runtime.maintain(); });
        const auto code = runtime.driver->start(runtime.period, &Runtime::tick, &runtime);
        if (!code)
            stop();
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
        return ErrorCode::Busy;
    if (runtime.driver)
    {
        runtime.driver->stop();
        runtime.driver.reset();
        runtime.maintenance.reset();
        runtime.apply_pending();
        runtime.collect();
    }
    return {};
}
RetCode Engine::reset() noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread() || runtime.driver)
        return ErrorCode::Busy;
    if (runtime.current)
        for (const auto &item : runtime.current->nodes)
        {
            const auto draft = runtime.nodes.find(item.id);
            if (draft == runtime.nodes.end() || draft->second != item.entry)
                item.entry->node->reset();
        }
    for (const auto &item : runtime.nodes)
        item.second->node->reset();
    runtime.position.store(0, std::memory_order_release);
    return {};
}
BlockPeriod Engine::block_period() const noexcept
{
    return runtime_->period;
}
std::uint64_t Engine::tick_position() const noexcept
{
    return runtime_->position.load(std::memory_order_acquire);
}
PlanRevision Engine::applied_revision() const noexcept
{
    return runtime_->applied.load(std::memory_order_acquire);
}

} // namespace aereborn
