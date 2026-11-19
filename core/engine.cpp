#include "engine.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace aereborn
{
namespace
{
thread_local const void *engine_execution_context = nullptr;

class EngineExecutionScope
{
  public:
    explicit EngineExecutionScope(const void *context) noexcept : previous_(engine_execution_context)
    {
        engine_execution_context = context;
    }
    ~EngineExecutionScope() { engine_execution_context = previous_; }

  private:
    const void *previous_;
};

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

struct DraftNode
{
    std::shared_ptr<GraphNode> entry;
    // Points into the connection map. Only the control side reads these links.
    std::vector<const Connection *> inputs;
    bool paused = false;
};

std::size_t pool_units_per_channel(Format format, std::size_t frames) noexcept
{
    return format == Format::F32P ? frames : frames * sample_bytes(format);
}

struct PoolSize
{
    const std::size_t limit;
    std::size_t outputs = 0;
    std::size_t silence = 0;

    bool count(PortSpec spec, BlockPeriod period, bool unconnected) noexcept
    {
        const auto units = pool_units_per_channel(spec.format, spec.max_frames(period));
        if (spec.channels > limit / units)
            return false;
        const auto size = spec.channels * units;
        if (unconnected)
            silence = std::max(silence, size);
        else
        {
            if (size > limit - outputs)
                return false;
            outputs += size;
        }
        return silence <= limit - outputs;
    }
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
    // The zero-filled prefix of each pool is shared by unconnected inputs.
    std::vector<float> audio_pool;
    std::vector<std::uint8_t> pcm_pool;
    std::vector<PlanNode> nodes;
    OPortBlock block(PortSpec spec, std::size_t frames, std::size_t offset) noexcept
    {
        auto *data = spec.format == Format::F32P ? reinterpret_cast<std::uint8_t *>(audio_pool.data() + offset)
                                                : pcm_pool.data() + offset;
        return {data, spec, frames};
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
                {period, tick, {item.inputs.data(), item.inputs.size()}, {item.outputs.data(), item.outputs.size()}});
        }
    }
};
using NodeIndex = std::map<NodeId, std::size_t>;
using Connections = std::map<ConnId, Connection>;
} // namespace

struct Engine::Runtime
{
    explicit Runtime(BlockPeriod period) : period(period) {}
    const BlockPeriod period;
    std::map<NodeId, DraftNode> nodes;
    Connections connections;
    std::unique_ptr<RenderPlan> current;
    std::unique_ptr<RenderPlan> pending;
    std::mutex plans_mutex;
    std::condition_variable plans_changed;
    std::thread reclaimer;
    bool stop_reclaimer = false;
    std::atomic<RenderPlan *> published{nullptr};
    std::unique_ptr<ClockDriver> driver;
    std::atomic<PlanRevision> applied{0};
    std::atomic<std::uint64_t> position{0};
    bool dirty = true;

    bool on_execution_thread() const noexcept { return engine_execution_context == this; }
    template <class Port> Result<DraftNode *> port_owner(Port port) noexcept
    {
        const auto found = nodes.find(port.node);
        if (found == nodes.end())
            return ErrorCode::UnknownNode;
        const auto ports = found->second.entry->ports(port);
        if (port.index >= ports.size())
            return ErrorCode::InvalidPort;
        return &found->second;
    }
    bool would_cycle(NodeId from, NodeId to) const
    {
        std::vector<NodeId> stack{from};
        std::set<NodeId> visited{from};
        while (!stack.empty())
        {
            const auto id = stack.back();
            stack.pop_back();
            if (id == to)
                return true;
            for (const auto *connection : nodes.find(id)->second.inputs)
                if (connection && visited.insert(connection->from.node).second)
                    stack.push_back(connection->from.node);
        }
        return false;
    }
    Connections::iterator erase_connection(Connections::iterator connection) noexcept
    {
        const auto to = connection->second.to;
        nodes.find(to.node)->second.inputs[to.index] = nullptr;
        return connections.erase(connection);
    }
    const Connection *active_input(const Connection *connection) const noexcept
    {
        return connection && !nodes.find(connection->from.node)->second.paused ? connection : nullptr;
    }
    RetCode visit(NodeId id, RenderPlan &plan, NodeIndex &node_index)
    {
        if (node_index.count(id))
            return {};
        const auto &draft = nodes.find(id)->second;
        for (const auto *input : draft.inputs)
            if (const auto *connection = active_input(input))
            {
                const auto code = visit(connection->from.node, plan, node_index);
                if (!code)
                    return code;
            }
        const auto &entry = draft.entry;
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
        NodeIndex node_index;
        for (const auto &item : nodes)
            if (!item.second.paused && item.second.entry->outputs.empty())
            {
                const auto code = visit(item.first, plan, node_index);
                if (!code)
                    return code;
            }
        PoolSize audio{plan.audio_pool.max_size()}, pcm{plan.pcm_pool.max_size()};
        for (const auto &item : plan.nodes)
        {
            for (const auto spec : item.entry->outputs)
                if (!(spec.format == Format::F32P ? audio : pcm).count(spec, period, false))
                    return ErrorCode::TooLarge;
            const auto &inputs = nodes.find(item.id)->second.inputs;
            for (std::size_t port = 0; port < item.inputs.size(); ++port)
            {
                if (active_input(inputs[port]))
                    continue;
                const auto spec = item.entry->inputs[port];
                if (!(spec.format == Format::F32P ? audio : pcm).count(spec, period, true))
                    return ErrorCode::TooLarge;
            }
        }
        plan.audio_pool.resize(audio.silence + audio.outputs);
        plan.pcm_pool.resize(pcm.silence + pcm.outputs);
        auto audio_offset = audio.silence;
        auto pcm_offset = pcm.silence;
        for (auto &item : plan.nodes)
        {
            for (std::size_t port = 0; port < item.outputs.size(); ++port)
            {
                const auto spec = item.entry->outputs[port];
                const auto frames = spec.max_frames(period);
                auto &offset = spec.format == Format::F32P ? audio_offset : pcm_offset;
                item.outputs[port] = plan.block(spec, frames, offset);
                offset += spec.channels * pool_units_per_channel(spec.format, frames);
            }
            const auto &inputs = nodes.find(item.id)->second.inputs;
            for (std::size_t port = 0; port < item.inputs.size(); ++port)
            {
                if (const auto *connection = active_input(inputs[port]))
                    item.inputs[port] =
                        plan.nodes[node_index.find(connection->from.node)->second].outputs[connection->from.index];
                else
                {
                    const auto spec = item.entry->inputs[port];
                    item.inputs[port] = plan.block(spec, spec.max_frames(period), 0);
                }
            }
        }
        return {};
    }
    RenderPlan &apply_published() noexcept
    {
        // The acknowledged plan stays owned until a later tick acknowledges its replacement.
        auto &plan = *published.load(std::memory_order_acquire);
        if (applied.load(std::memory_order_relaxed) != plan.revision)
            applied.store(plan.revision, std::memory_order_release);
        return plan;
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
    void reclaim_plans()
    {
        const EngineExecutionScope scope(this);
        const auto check_delay = std::chrono::milliseconds(static_cast<std::uint32_t>(period));
        std::unique_lock<std::mutex> lock(plans_mutex);
        while (!stop_reclaimer)
        {
            plans_changed.wait(lock, [this] { return stop_reclaimer || pending; });
            if (stop_reclaimer)
                break;
            if (auto retired = collect())
            {
                lock.unlock();
                retired.reset();
                lock.lock();
            }
            else
            {
                // Only check again while a submission is outstanding. The tick only acknowledges it atomically.
                plans_changed.wait_for(lock, check_delay);
            }
        }
    }
    static void tick(void *context) noexcept
    {
        auto &runtime = *static_cast<Runtime *>(context);
        const EngineExecutionScope scope(context);
        const auto tick = runtime.position.load(std::memory_order_relaxed);
        runtime.apply_published().render(runtime.period, tick);
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
    std::vector<const Connection *> inputs(entry->inputs.size());
    runtime_->nodes.emplace(id, DraftNode{std::move(entry), std::move(inputs)});
    runtime_->dirty = true;
    return id;
}
RetCode Engine::remove_node(NodeId id) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime.nodes.find(id);
    if (found == runtime.nodes.end())
        return ErrorCode::UnknownNode;
    for (auto it = runtime.connections.begin(); it != runtime.connections.end();)
        if (it->second.from.node == id || it->second.to.node == id)
            it = runtime.erase_connection(it);
        else
            ++it;
    runtime.nodes.erase(found);
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
    auto &current = found->second.entry;
    if (!same_ports(current->inputs, entry->inputs) || !same_ports(current->outputs, entry->outputs))
        return ErrorCode::FormatMismatch;
    current = std::move(entry);
    runtime.dirty = true;
    return {};
}
RetCode Engine::set_paused(NodeId id, bool paused)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime.nodes.find(id);
    if (found == runtime.nodes.end())
        return ErrorCode::UnknownNode;
    runtime.dirty |= found->second.paused != paused;
    found->second.paused = paused;
    return {};
}
Result<Node *> Engine::node(NodeId id) noexcept
{
    if (runtime_->on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime_->nodes.find(id);
    if (found == runtime_->nodes.end())
        return ErrorCode::UnknownNode;
    return found->second.entry->node.get();
}
Result<ConnId> Engine::connect(OPort from, IPort to)
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto source = runtime.port_owner(from);
    if (!source)
        return source.ret_code();
    const auto destination = runtime.port_owner(to);
    if (!destination)
        return destination.ret_code();
    auto &draft = *destination.value();
    if (source.value()->entry->outputs[from.index] != draft.entry->inputs[to.index])
        return ErrorCode::FormatMismatch;
    auto &input = draft.inputs[to.index];
    if (input)
        return input->from.node == from.node && input->from.index == from.index ? ErrorCode::DuplicateConn
                                                                               : ErrorCode::InputConnected;
    if (runtime.would_cycle(from.node, to.node))
        return ErrorCode::Cycle;
    const ConnId id{next_identifier()};
    const auto inserted = runtime.connections.emplace(id, Connection{from, to});
    input = &inserted.first->second;
    runtime.dirty = true;
    return id;
}
RetCode Engine::disconnect(ConnId id) noexcept
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const auto found = runtime.connections.find(id);
    if (found == runtime.connections.end())
        return ErrorCode::UnknownConn;
    runtime.erase_connection(found);
    runtime.dirty = true;
    return {};
}
Result<PlanRevision> Engine::prepare()
{
    auto &runtime = *runtime_;
    if (runtime.on_execution_thread())
        return ErrorCode::Busy;
    const EngineExecutionScope scope(&runtime);
    std::unique_ptr<RenderPlan> retired;
    PlanRevision revision;
    {
        const std::lock_guard<std::mutex> lock(runtime.plans_mutex);
        retired = runtime.collect();
        if (!runtime.dirty)
            return (runtime.pending ? runtime.pending : runtime.current)->revision;
        if (runtime.pending)
            return ErrorCode::BufferFull;
        revision = runtime.current ? runtime.current->revision + 1 : 1;
    }
    retired.reset();
    auto plan = std::make_unique<RenderPlan>();
    const auto code = runtime.build_plan(*plan);
    if (!code)
        return code;
    plan->revision = revision;
    if (runtime.driver)
    {
        if (!runtime.reclaimer.joinable())
        {
            runtime.stop_reclaimer = false;
            runtime.reclaimer = std::thread([&runtime] { runtime.reclaim_plans(); });
        }
        {
            const std::lock_guard<std::mutex> lock(runtime.plans_mutex);
            runtime.pending = std::move(plan);
            runtime.published.store(runtime.pending.get(), std::memory_order_release);
        }
        runtime.plans_changed.notify_one();
    }
    else
    {
        runtime.current = std::move(plan);
        runtime.published.store(runtime.current.get(), std::memory_order_release);
        runtime.apply_published();
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
    if (!runtime.published.is_lock_free() || !runtime.applied.is_lock_free() || !runtime.position.is_lock_free())
        return ErrorCode::Unsupported;
    runtime.driver = driver ? std::move(driver) : make_timer_driver();
    try
    {
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
        if (runtime.reclaimer.joinable())
        {
            {
                const std::lock_guard<std::mutex> lock(runtime.plans_mutex);
                runtime.stop_reclaimer = true;
            }
            runtime.plans_changed.notify_one();
            runtime.reclaimer.join();
        }
        const EngineExecutionScope scope(&runtime);
        if (runtime.pending)
        {
            runtime.apply_published();
            runtime.collect();
        }
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
            if (draft == runtime.nodes.end() || draft->second.entry != item.entry)
                item.entry->node->reset();
        }
    for (const auto &item : runtime.nodes)
        item.second.entry->node->reset();
    runtime.position.store(0, std::memory_order_release);
    return {};
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
