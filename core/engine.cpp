#include "engine.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace aereborn
{
namespace
{
thread_local const Engine *clock_engine = nullptr;

BlockPeriod checked_period(BlockPeriod period)
{
    if (!is_block_period(period))
        throw Error(ErrorCode::InvalidArg);
    return period;
}

std::uint64_t next_identifier() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

bool same_ports(View<const PortSpec> a, View<const PortSpec> b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
std::size_t pool_units_per_channel(SampleFormat format, std::size_t frames) noexcept
{
    return format == SampleFormat::F32P ? frames : frames * sample_bytes(format);
}

struct PoolSize
{
    const std::size_t limit;
    std::size_t outputs = 0;
    std::size_t silence = 0;
    std::size_t offset = 0;

    void count(PortSpec spec, BlockPeriod period, bool unconnected)
    {
        const auto units = pool_units_per_channel(spec.format, spec.max_frames(period));
        const auto available = unconnected ? limit - outputs : limit - outputs - silence;
        if (spec.channels > available / units)
            throw Error(ErrorCode::TooLarge);
        const auto size = spec.channels * units;
        if (unconnected)
            silence = std::max(silence, size);
        else
            outputs += size;
    }
};
struct PoolLayout
{
    PoolSize floats;
    PoolSize bytes;
    std::size_t input_count = 0;
    std::size_t output_count = 0;

    PoolSize &for_format(SampleFormat format) noexcept
    {
        return format == SampleFormat::F32P ? floats : bytes;
    }
};

template <class T>
View<T> port_range(T *data, std::size_t first, std::size_t count) noexcept
{
    if (count == 0)
        return {};
    return {data + first, count};
}
struct NodeEntry
{
    NodeEntry(NodeId id, std::unique_ptr<Node> instance, View<const PortSpec> inputs, View<const PortSpec> outputs)
        : id(id), node(std::move(instance)), inputs(inputs), outputs(outputs)
    {
    }
    View<const PortSpec> ports(IPort) const noexcept
    {
        return inputs;
    }
    View<const PortSpec> ports(OPort) const noexcept
    {
        return outputs;
    }

    const NodeId id;
    std::unique_ptr<Node> node;
    const View<const PortSpec> inputs;
    const View<const PortSpec> outputs;
    bool prepared = false;
};

struct Link
{
    static constexpr auto unconnected = std::numeric_limits<std::size_t>::max();
    std::size_t node_index = unconnected;
    std::size_t port_index = 0;
};

struct Topology
{
    std::vector<std::shared_ptr<NodeEntry>> nodes;
    std::vector<Link> links;
    std::vector<Connection> connections;
};
} // namespace

class Engine::Graph
{
  public:
    NodeId add_node(std::unique_ptr<Node> instance)
    {
        if (!instance)
            throw Error(ErrorCode::InvalidArg);
        const auto inputs = instance->inputs();
        const auto outputs = instance->outputs();
        if (inputs.empty() && outputs.empty())
            throw Error(ErrorCode::InvalidArg);
        for (const auto ports : {inputs, outputs})
            for (const auto &port : ports)
                if (!port.valid())
                    throw Error(ErrorCode::InvalidArg);

        std::vector<OPort> links(inputs.size());
        const NodeId id{next_identifier()};
        auto entry = std::make_shared<NodeEntry>(id, std::move(instance), inputs, outputs);
        nodes_.emplace(id, DraftNode{std::move(entry), std::move(links)});
        dirty_ = true;
        return id;
    }

    void remove_node(NodeId id)
    {
        const auto found = find_node(id);
        for (auto it = connections_.begin(); it != connections_.end();)
            if (it->second.from.node == id || it->second.to.node == id)
                it = erase_connection(it);
            else
                ++it;
        dirty_ = true;
        nodes_.erase(found);
    }

    void replace_node(NodeId id, std::unique_ptr<Node> replacement)
    {
        const auto found = find_node(id);
        if (!replacement)
            throw Error(ErrorCode::InvalidArg);
        const auto inputs = replacement->inputs();
        const auto outputs = replacement->outputs();
        auto &current = found->second.entry;
        if (!same_ports(current->inputs, inputs) || !same_ports(current->outputs, outputs))
            throw Error(ErrorCode::FormatMismatch);
        auto entry = std::make_shared<NodeEntry>(id, std::move(replacement), inputs, outputs);
        dirty_ = true;
        current = std::move(entry);
    }

    void set_paused(NodeId id, bool paused)
    {
        const auto found = find_node(id);
        dirty_ |= found->second.paused != paused;
        found->second.paused = paused;
    }

    Node *node(NodeId id)
    {
        const auto found = find_node(id);
        return found->second.entry->node.get();
    }

    ConnId connect(OPort from, IPort to)
    {
        const auto &source = port_owner(from);
        auto &destination = port_owner(to);
        if (source.entry->outputs[from.index] != destination.entry->inputs[to.index])
            throw Error(ErrorCode::FormatMismatch);
        auto &input = destination.inputs[to.index];
        if (input.node)
            throw Error(input == from ? ErrorCode::DuplicateConn : ErrorCode::InputBusy);
        if (would_cycle(from.node, to.node))
            throw Error(ErrorCode::Cycle);
        const ConnId id{next_identifier()};
        connections_.emplace(id, ConnectionPorts{from, to});
        input = from;
        dirty_ = true;
        return id;
    }

    void disconnect(ConnId id)
    {
        const auto found = connections_.find(id);
        if (found == connections_.end())
            throw Error(ErrorCode::UnknownConn);
        erase_connection(found);
        dirty_ = true;
    }

    Topology resolve() const
    {
        Topology topology;
        NodeIndex node_index;
        for (const auto &item : nodes_)
            if (!item.second.paused && item.second.entry->outputs.empty())
                visit(item.second, topology, node_index);

        for (const auto &item : connections_)
        {
            const auto &connection = item.second;
            if (node_index.count(connection.from.node) && node_index.count(connection.to.node))
                topology.connections.push_back({item.first, connection.from, connection.to});
        }
        return topology;
    }

    bool owns(const NodeEntry &entry) const noexcept
    {
        const auto found = nodes_.find(entry.id);
        return found != nodes_.end() && found->second.entry.get() == &entry;
    }

    void reset_nodes() noexcept
    {
        for (const auto &item : nodes_)
            item.second.entry->node->reset();
    }

    bool dirty() const noexcept
    {
        return dirty_;
    }

    void mark_clean() noexcept
    {
        dirty_ = false;
    }

  private:
    struct DraftNode
    {
        std::shared_ptr<NodeEntry> entry;
        std::vector<OPort> inputs;
        bool paused = false;
    };
    struct ConnectionPorts
    {
        OPort from;
        IPort to;
    };
    using Nodes = std::map<NodeId, DraftNode>;
    using Connections = std::map<ConnId, ConnectionPorts>;
    using NodeIndex = std::map<NodeId, std::size_t>;
    Nodes::iterator find_node(NodeId id)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            throw Error(ErrorCode::UnknownNode);
        return found;
    }

    template <class Port> DraftNode &port_owner(Port port)
    {
        const auto found = find_node(port.node);
        const auto ports = found->second.entry->ports(port);
        if (port.index >= ports.size())
            throw Error(ErrorCode::InvalidPort);
        return found->second;
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
            for (const auto input : nodes_.find(id)->second.inputs)
                if (input.node && visited.insert(input.node).second)
                    stack.push_back(input.node);
        }

        return false;
    }

    Connections::iterator erase_connection(Connections::iterator connection) noexcept
    {
        const auto to = connection->second.to;
        nodes_.find(to.node)->second.inputs[to.index] = {};
        return connections_.erase(connection);
    }

    void visit(const DraftNode &draft, Topology &topology, NodeIndex &node_index) const
    {
        if (node_index.count(draft.entry->id))
            return;

        for (const auto input : draft.inputs)
            if (input.node)
            {
                const auto &source = nodes_.find(input.node)->second;
                if (!source.paused)
                    visit(source, topology, node_index);
            }

        node_index.emplace(draft.entry->id, topology.nodes.size());
        topology.nodes.push_back(draft.entry);

        // An absent source is unconnected or was excluded by pause.
        for (const auto input : draft.inputs)
        {
            const auto source = input.node ? node_index.find(input.node) : node_index.end();
            topology.links.push_back(source == node_index.end() ? Link{} : Link{source->second, input.index});
        }
    }

    Nodes nodes_;
    Connections connections_;
    bool dirty_ = true;
};

class Engine::Clock
{
  public:
    ~Clock()
    {
        stop();
    }

    /// @brief Publish a plan under the clock mutex and wait for takeover.
    /// @tparam Plan Type of the render plan exchanged with the clock.
    /// @param submitted Slot that the clock clears after taking the plan.
    /// @param plan New plan. The caller keeps ownership throughout the wait.
    /// @pre The clock is running and the control side is the sole submitter.
    template <class Plan> void submit_and_wait(std::atomic<Plan *> &submitted, Plan *plan)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        submitted.store(plan, std::memory_order_release);
        applied_.wait(lock, [&submitted]() noexcept { return submitted.load(std::memory_order_acquire) == nullptr; });
    }

    /// @brief Launch the worker that drives the engine's ticks.
    /// @param engine Engine to drive, kept alive until the worker joins.
    void start(Engine &engine)
    {
        stopping_ = false;
        const auto interval = std::chrono::milliseconds(static_cast<std::uint32_t>(engine.period_));
        const auto first_deadline = std::chrono::steady_clock::now() + interval;
        thread_ = std::thread([this, &engine, interval, deadline = first_deadline]() mutable {
            clock_engine = &engine;
            std::unique_lock<std::mutex> lock(mutex_);
            while (!changed_.wait_until(lock, deadline, [this] { return stopping_; }))
            {
                lock.unlock();
                const auto started = std::chrono::steady_clock::now();
                const bool swapped = engine.tick();
                lock.lock();
                if (swapped)
                    applied_.notify_one();
                const auto finished = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(finished - started);
                engine.last_tick_us_.store(elapsed.count(), std::memory_order_relaxed);
                deadline += interval;
                if (finished >= deadline)
                {
                    const auto missed = (finished - deadline) / interval + 1;
                    engine.missed_periods_.fetch_add(missed, std::memory_order_relaxed);
                    deadline += interval * missed;
                }
            }
        });
    }

    void stop() noexcept
    {
        if (!thread_.joinable())
            return;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        thread_.join();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable applied_;
    bool stopping_ = false;
    std::thread thread_;
};

class Engine::Runner
{
  public:
    void commit(Topology topology, BlockPeriod period, Clock *live_clock)
    {
        auto plan = std::make_unique<Plan>();
        for (const auto &entry : topology.nodes)
            if (!entry->prepared)
            {
                entry->node->prepare(period);
                entry->prepared = true;
            }

        PoolLayout layout{{plan->float_pool.max_size()}, {plan->byte_pool.max_size()}};
        measure_layout(*plan, topology, period, layout);
        bind_blocks(*plan, topology, period, layout);

        plan->node_ids.reserve(topology.nodes.size());
        for (const auto &entry : topology.nodes)
            plan->node_ids.push_back(entry->id);
        plan->connections = std::move(topology.connections);
        plan->owners = std::move(topology.nodes);
        if (live_clock)
            live_clock->submit_and_wait(submitted_, plan.get());
        else
            rendering_ = plan.get();
        current_ = std::move(plan);
    }

    View<const NodeId> nodes() const noexcept
    {
        if (!current_)
            return {};
        return {current_->node_ids.data(), current_->node_ids.size()};
    }

    View<const Connection> connections() const noexcept
    {
        if (!current_)
            return {};
        return {current_->connections.data(), current_->connections.size()};
    }

    const Node *node(NodeId id) const
    {
        if (current_)
            for (const auto &entry : current_->owners)
                if (entry->id == id)
                    return entry->node.get();
        throw Error(ErrorCode::UnknownNode);
    }

    /// @brief Reset nodes held by the current plan after removal or replacement in the draft.
    /// @param draft Graph whose node instances are reset separately.
    /// @pre The clock is stopped.
    void reset(const Graph &draft) noexcept
    {
        if (!current_)
            return;
        for (const auto &entry : current_->owners)
            if (!draft.owns(*entry))
                entry->node->reset();
    }

    bool render(BlockPeriod period, std::uint64_t tick) noexcept
    {
        const auto plan = submitted_.load(std::memory_order_acquire);
        if (plan)
        {
            rendering_ = plan;
            // The control side may now retire the previous plan.
            submitted_.store(nullptr, std::memory_order_release);
        }

        rendering_->render(period, tick);
        return plan != nullptr;
    }

  private:
    struct Step
    {
        Node *node;
        std::size_t first_input;
        std::size_t input_count;
        std::size_t first_output;
        std::size_t output_count;
    };
    struct Plan
    {
        OBBlock block(PortSpec spec, std::size_t frames, std::size_t offset) noexcept
        {
            auto *data = spec.format == SampleFormat::F32P
                             ? reinterpret_cast<std::uint8_t *>(float_pool.data() + offset)
                             : byte_pool.data() + offset;
            return {data, spec, frames};
        }

        void render(BlockPeriod period, std::uint64_t tick) noexcept
        {
            if (has_fractional_ports)
            {
                const auto refresh_frames = [period, tick](auto &block) noexcept {
                    const auto spec = block.spec();
                    block = {block.byte_view().data(), spec, spec.frames(period, tick)};
                };
                for (auto &input : inputs)
                    refresh_frames(input);
                for (auto &output : outputs)
                    refresh_frames(output);
            }

            // Leave slot zero inactive for all inputs without a source.
            std::fill(output_active.begin() + 1, output_active.end(), std::uint8_t{1});
            for (const auto &step : steps)
            {
                for (std::size_t i = step.first_input; i < step.first_input + step.input_count; ++i)
                    input_active[i] = output_active[input_sources[i]];

                step.node->process({period, tick, port_range(inputs.data(), step.first_input, step.input_count),
                                    port_range(outputs.data(), step.first_output, step.output_count),
                                    port_range(input_active.data(), step.first_input, step.input_count),
                                    port_range(output_active.data() + 1, step.first_output, step.output_count)});
            }
        }

        bool has_fractional_ports = false;
        // The zero-filled prefix of each pool is shared by unconnected inputs.
        std::vector<float> float_pool;
        std::vector<std::uint8_t> byte_pool;
        std::vector<IBBlock> inputs;
        std::vector<OBBlock> outputs;
        std::vector<std::uint8_t> input_active;
        std::vector<std::uint8_t> output_active; // Slot zero is shared by inputs without a source.
        std::vector<std::size_t> input_sources;
        std::vector<Step> steps;
        std::vector<std::shared_ptr<NodeEntry>> owners;
        std::vector<NodeId> node_ids;
        std::vector<Connection> connections;
    };

    /// @brief Measure buffer capacity and record each node's port offsets.
    /// @param plan Plan receiving the execution steps and fractional-frame flag.
    /// @param topology Node order and input links used by those steps.
    /// @param period Block duration used to size each port at its maximum frame count.
    /// @param layout Pool limits and accumulated port and storage counts.
    /// @throws Error TooLarge if a step or port table or buffer pool would exceed capacity.
    static void measure_layout(Plan &plan, const Topology &topology, BlockPeriod period, PoolLayout &layout)
    {
        const auto period_ms = static_cast<std::uint32_t>(period);
        const auto fractional = [period_ms](PortSpec spec) {
            return (static_cast<std::uint32_t>(spec.sample_rate) * period_ms) % 1000 != 0;
        };
        if (topology.nodes.size() > plan.steps.max_size())
            throw Error(ErrorCode::TooLarge);
        const auto max_inputs = std::min({plan.inputs.max_size(), plan.input_active.max_size(),
                                          plan.input_sources.max_size()});
        const auto max_outputs = std::min(plan.outputs.max_size(), plan.output_active.max_size() - 1);
        plan.steps.reserve(topology.nodes.size());
        for (const auto &entry : topology.nodes)
        {
            const auto inputs = entry->inputs;
            const auto outputs = entry->outputs;
            if (inputs.size() > max_inputs - layout.input_count || outputs.size() > max_outputs - layout.output_count)
                throw Error(ErrorCode::TooLarge);
            plan.steps.push_back(
                {entry->node.get(), layout.input_count, inputs.size(), layout.output_count, outputs.size()});

            for (const auto spec : outputs)
            {
                layout.for_format(spec.format).count(spec, period, false);
                plan.has_fractional_ports |= fractional(spec);
            }
            for (std::size_t port = 0; port < inputs.size(); ++port)
            {
                if (topology.links[layout.input_count + port].node_index != Link::unconnected)
                    continue;
                const auto spec = inputs[port];
                layout.for_format(spec.format).count(spec, period, true);
                plan.has_fractional_ports |= fractional(spec);
            }
            layout.input_count += inputs.size();
            layout.output_count += outputs.size();
        }
    }

    /// @brief Allocate buffers and bind their input and output views.
    /// @param plan Plan owning the buffers and measured execution steps.
    /// @param topology Input links and port specifications in execution order.
    /// @param period Block duration used to size each bound block.
    /// @param layout Measured capacity and pool offsets advanced during binding.
    /// @pre measure_layout() succeeded for the same plan, topology, and period.
    static void bind_blocks(Plan &plan, const Topology &topology, BlockPeriod period, PoolLayout &layout)
    {
        plan.inputs.resize(layout.input_count);
        plan.outputs.resize(layout.output_count);
        plan.input_active.resize(layout.input_count);
        plan.output_active.resize(layout.output_count + 1);
        plan.input_sources.resize(layout.input_count);
        plan.float_pool.resize(layout.floats.silence + layout.floats.outputs);
        plan.byte_pool.resize(layout.bytes.silence + layout.bytes.outputs);
        layout.floats.offset = layout.floats.silence;
        layout.bytes.offset = layout.bytes.silence;
        // Outputs are bound upstream first. Connected inputs copy the corresponding read-only view.
        for (std::size_t node_index = 0; node_index < topology.nodes.size(); ++node_index)
        {
            const auto &entry = *topology.nodes[node_index];
            const auto &step = plan.steps[node_index];
            for (std::size_t port = 0; port < step.output_count; ++port)
            {
                const auto spec = entry.outputs[port];
                const auto frames = spec.max_frames(period);
                auto &pool = layout.for_format(spec.format);
                plan.outputs[step.first_output + port] = plan.block(spec, frames, pool.offset);
                pool.offset += spec.channels * pool_units_per_channel(spec.format, frames);
            }
            for (std::size_t port = 0; port < step.input_count; ++port)
            {
                const auto input_index = step.first_input + port;
                const auto link = topology.links[input_index];
                if (link.node_index != Link::unconnected)
                {
                    const auto source = plan.steps[link.node_index].first_output + link.port_index;
                    plan.inputs[input_index] = plan.outputs[source];
                    plan.input_sources[input_index] = source + 1;
                }
                else
                {
                    const auto spec = entry.inputs[port];
                    plan.inputs[input_index] = plan.block(spec, spec.max_frames(period), 0);
                }
            }
        }
    }

    std::unique_ptr<Plan> current_;
    std::atomic<Plan *> submitted_{nullptr};
    // Only the clock thread accesses this while running.
    Plan *rendering_ = nullptr;
};

Engine::Engine(BlockPeriod period)
    : period_(checked_period(period)), graph_(std::make_unique<Graph>()), runner_(std::make_unique<Runner>()),
      clock_(std::make_unique<Clock>())
{
}

// The clock is destroyed first and joins before the runner or graph is destroyed.
Engine::~Engine() = default;

void Engine::require_control_thread() const
{
    if (clock_engine == this)
        throw Error(ErrorCode::Busy);
}

bool Engine::tick() noexcept
{
    const auto tick = position_.load(std::memory_order_relaxed);
    const bool swapped = runner_->render(period_, tick);
    position_.store(tick + 1, std::memory_order_release);
    return swapped;
}

NodeId Engine::add_node(std::unique_ptr<Node> instance)
{
    require_control_thread();
    return insert_node(std::move(instance));
}

NodeId Engine::insert_node(std::unique_ptr<Node> instance)
{
    return graph_->add_node(std::move(instance));
}

void Engine::remove_node(NodeId id)
{
    require_control_thread();
    graph_->remove_node(id);
}

void Engine::replace_node(NodeId id, std::unique_ptr<Node> replacement)
{
    require_control_thread();
    graph_->replace_node(id, std::move(replacement));
}

void Engine::set_paused(NodeId id, bool paused)
{
    require_control_thread();
    graph_->set_paused(id, paused);
}

Node *Engine::node(NodeId id)
{
    require_control_thread();
    return graph_->node(id);
}

ConnId Engine::connect(OPort from, IPort to)
{
    require_control_thread();
    return graph_->connect(from, to);
}

void Engine::disconnect(ConnId id)
{
    require_control_thread();
    graph_->disconnect(id);
}

void Engine::commit()
{
    require_control_thread();
    if (!graph_->dirty())
        return;

    runner_->commit(graph_->resolve(), period_, state_ == State::Running ? clock_.get() : nullptr);
    ++plan_generation_;
    graph_->mark_clean();
    if (state_ == State::Unprepared)
        state_ = State::Ready;
}

void Engine::start()
{
    require_control_thread();
    if (state_ == State::Running)
        throw Error(ErrorCode::Busy);
    if (state_ == State::Unprepared)
        throw Error(ErrorCode::NotReady);
    clock_->start(*this);
    state_ = State::Running;
}

void Engine::stop()
{
    require_control_thread();
    if (state_ == State::Running)
    {
        clock_->stop();
        state_ = State::Ready;
    }
}

void Engine::reset()
{
    require_control_thread();
    if (state_ == State::Running)
        throw Error(ErrorCode::Busy);
    runner_->reset(*graph_);
    graph_->reset_nodes();
    position_.store(0, std::memory_order_release);
    missed_periods_.store(0, std::memory_order_relaxed);
    last_tick_us_.store(0, std::memory_order_relaxed);
}

Engine::State Engine::state() const
{
    require_control_thread();
    return state_;
}

bool Engine::dirty() const
{
    require_control_thread();
    return graph_->dirty();
}

std::uint64_t Engine::plan_generation() const
{
    require_control_thread();
    return plan_generation_;
}

View<const NodeId> Engine::plan_nodes() const
{
    require_control_thread();
    return runner_->nodes();
}

View<const Connection> Engine::plan_connections() const
{
    require_control_thread();
    return runner_->connections();
}

const Node *Engine::plan_node(NodeId node_id) const
{
    require_control_thread();
    return runner_->node(node_id);
}

std::uint64_t Engine::tick_position() const noexcept
{
    return position_.load(std::memory_order_acquire);
}

} // namespace aereborn
