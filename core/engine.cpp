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

template <class Block>
View<const Block> port_range(const std::vector<Block> &blocks, std::size_t first, std::size_t count) noexcept
{
    if (count == 0)
        return {};
    return {blocks.data() + first, count};
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
};
} // namespace

class Engine::Graph
{
  public:
    Result<NodeId> add_node(std::unique_ptr<Node> instance)
    {
        if (!instance)
            return ErrorCode::InvalidArg;
        const auto inputs = instance->inputs();
        const auto outputs = instance->outputs();
        if (inputs.empty() && outputs.empty())
            return ErrorCode::InvalidArg;
        for (const auto ports : {inputs, outputs})
            for (const auto &port : ports)
                if (!port.valid())
                    return ErrorCode::InvalidArg;

        std::vector<OPort> links(inputs.size());
        const NodeId id{next_identifier()};
        auto entry = std::make_shared<NodeEntry>(id, std::move(instance), inputs, outputs);
        nodes_.emplace(id, DraftNode{std::move(entry), std::move(links)});
        dirty_ = true;
        return id;
    }

    RetCode remove_node(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            return ErrorCode::UnknownNode;
        for (auto it = connections_.begin(); it != connections_.end();)
            if (it->second.from.node == id || it->second.to.node == id)
                it = erase_connection(it);
            else
                ++it;
        dirty_ = true;
        nodes_.erase(found);
        return {};
    }

    RetCode replace_node(NodeId id, std::unique_ptr<Node> replacement)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            return ErrorCode::UnknownNode;
        if (!replacement)
            return ErrorCode::InvalidArg;
        const auto inputs = replacement->inputs();
        const auto outputs = replacement->outputs();
        auto &current = found->second.entry;
        if (!same_ports(current->inputs, inputs) || !same_ports(current->outputs, outputs))
            return ErrorCode::FormatMismatch;
        auto entry = std::make_shared<NodeEntry>(id, std::move(replacement), inputs, outputs);
        dirty_ = true;
        current = std::move(entry);
        return {};
    }

    RetCode set_paused(NodeId id, bool paused)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            return ErrorCode::UnknownNode;
        dirty_ |= found->second.paused != paused;
        found->second.paused = paused;
        return {};
    }

    Result<Node *> node(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            return ErrorCode::UnknownNode;
        return found->second.entry->node.get();
    }

    Result<ConnId> connect(OPort from, IPort to)
    {
        const auto source = port_owner(from);
        if (!source)
            return source.ret_code();
        const auto destination = port_owner(to);
        if (!destination)
            return destination.ret_code();
        auto &draft = *destination.value();
        if (source.value()->entry->outputs[from.index] != draft.entry->inputs[to.index])
            return ErrorCode::FormatMismatch;
        auto &input = draft.inputs[to.index];
        if (input.node)
            return input == from ? ErrorCode::DuplicateConn : ErrorCode::InputConnected;
        if (would_cycle(from.node, to.node))
            return ErrorCode::Cycle;
        const ConnId id{next_identifier()};
        connections_.emplace(id, Connection{from, to});
        input = from;
        dirty_ = true;
        return id;
    }

    RetCode disconnect(ConnId id) noexcept
    {
        const auto found = connections_.find(id);
        if (found == connections_.end())
            return ErrorCode::UnknownConn;
        erase_connection(found);
        dirty_ = true;
        return {};
    }

    Topology resolve() const
    {
        Topology topology;
        NodeIndex node_index;
        for (const auto &item : nodes_)
            if (!item.second.paused && item.second.entry->outputs.empty())
                visit(item.second, topology, node_index);

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
    struct Connection
    {
        OPort from;
        IPort to;
    };
    using Connections = std::map<ConnId, Connection>;
    using NodeIndex = std::map<NodeId, std::size_t>;
    template <class Port> Result<DraftNode *> port_owner(Port port) noexcept
    {
        const auto found = nodes_.find(port.node);
        if (found == nodes_.end())
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

    std::map<NodeId, DraftNode> nodes_;
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
                const bool swapped = engine.tick();
                lock.lock();
                if (swapped)
                    applied_.notify_one();
                const auto lateness = std::chrono::steady_clock::now() - deadline;
                deadline += interval * (lateness >= interval ? lateness / interval + 1 : 1);
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
    RetCode commit(Topology topology, BlockPeriod period, Clock *live_clock)
    {
        auto plan = std::make_unique<Plan>();
        const auto code = build_plan(*plan, topology, period);
        if (!code)
            return code;

        plan->owners = std::move(topology.nodes);
        if (live_clock)
            live_clock->submit_and_wait(submitted_, plan.get());
        else
            rendering_ = plan.get();
        current_ = std::move(plan);
        return {};
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

            for (const auto &step : steps)
                step.node->process({period, tick, port_range(inputs, step.first_input, step.input_count),
                                    port_range(outputs, step.first_output, step.output_count)});
        }

        bool has_fractional_ports = false;
        // The zero-filled prefix of each pool is shared by unconnected inputs.
        std::vector<float> float_pool;
        std::vector<std::uint8_t> byte_pool;
        std::vector<IBBlock> inputs;
        std::vector<OBBlock> outputs;
        std::vector<Step> steps;
        std::vector<std::shared_ptr<NodeEntry>> owners;
    };

    /// @brief Prepare nodes and build their execution steps and buffers.
    /// @param plan Empty plan to populate.
    /// @param topology Reachable nodes in dependency order and their input links.
    /// @param period Validated block period shared by the plan and its nodes.
    /// @return Success or a node preparation or capacity error.
    static RetCode build_plan(Plan &plan, const Topology &topology, BlockPeriod period)
    {
        for (const auto &entry : topology.nodes)
            if (!entry->prepared)
            {
                const auto code = entry->node->prepare(period);
                if (!code)
                    return code;
                entry->prepared = true;
            }

        PoolLayout layout{{plan.float_pool.max_size()}, {plan.byte_pool.max_size()}};
        const auto code = measure_layout(plan, topology, period, layout);
        if (!code)
            return code;
        bind_blocks(plan, topology, period, layout);
        return {};
    }

    /// @brief Measure buffer capacity and record each node's port offsets.
    /// @param plan Plan receiving the execution steps and fractional-frame flag.
    /// @param topology Node order and input links used by those steps.
    /// @param period Block duration used to size each port at its maximum frame count.
    /// @param layout Pool limits and accumulated port and storage counts.
    /// @return Success or `TooLarge` when a port table or buffer pool would exceed capacity.
    static RetCode measure_layout(Plan &plan, const Topology &topology, BlockPeriod period, PoolLayout &layout)
    {
        const auto period_ms = static_cast<std::uint32_t>(period);
        const auto fractional = [period_ms](PortSpec spec) {
            return (static_cast<std::uint32_t>(spec.sample_rate) * period_ms) % 1000 != 0;
        };
        plan.steps.reserve(topology.nodes.size());
        for (const auto &entry : topology.nodes)
        {
            const auto inputs = entry->inputs;
            const auto outputs = entry->outputs;
            if (inputs.size() > plan.inputs.max_size() - layout.input_count ||
                outputs.size() > plan.outputs.max_size() - layout.output_count)
                return ErrorCode::TooLarge;
            plan.steps.push_back(
                {entry->node.get(), layout.input_count, inputs.size(), layout.output_count, outputs.size()});

            for (const auto spec : outputs)
            {
                if (!layout.for_format(spec.format).count(spec, period, false))
                    return ErrorCode::TooLarge;
                plan.has_fractional_ports |= fractional(spec);
            }
            for (std::size_t port = 0; port < inputs.size(); ++port)
            {
                if (topology.links[layout.input_count + port].node_index != Link::unconnected)
                    continue;
                const auto spec = inputs[port];
                if (!layout.for_format(spec.format).count(spec, period, true))
                    return ErrorCode::TooLarge;
                plan.has_fractional_ports |= fractional(spec);
            }
            layout.input_count += inputs.size();
            layout.output_count += outputs.size();
        }
        return {};
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
                    plan.inputs[input_index] = plan.outputs[plan.steps[link.node_index].first_output + link.port_index];
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
    : period_(period), graph_(std::make_unique<Graph>()), runner_(std::make_unique<Runner>()),
      clock_(std::make_unique<Clock>())
{
}

// The clock is destroyed first and joins before the runner or graph is destroyed.
Engine::~Engine() = default;

bool Engine::on_clock_thread() const noexcept
{
    return clock_engine == this;
}

bool Engine::tick() noexcept
{
    const auto tick = position_.load(std::memory_order_relaxed);
    const bool swapped = runner_->render(period_, tick);
    position_.store(tick + 1, std::memory_order_release);
    return swapped;
}

Result<NodeId> Engine::add_node(std::unique_ptr<Node> instance)
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return insert_node(std::move(instance));
}

Result<NodeId> Engine::insert_node(std::unique_ptr<Node> instance)
{
    return graph_->add_node(std::move(instance));
}

RetCode Engine::remove_node(NodeId id) noexcept
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->remove_node(id);
}

RetCode Engine::replace_node(NodeId id, std::unique_ptr<Node> replacement)
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->replace_node(id, std::move(replacement));
}

RetCode Engine::set_paused(NodeId id, bool paused)
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->set_paused(id, paused);
}

Result<Node *> Engine::node(NodeId id) noexcept
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->node(id);
}

Result<ConnId> Engine::connect(OPort from, IPort to)
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->connect(from, to);
}

RetCode Engine::disconnect(ConnId id) noexcept
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    return graph_->disconnect(id);
}

RetCode Engine::commit()
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    if (!is_block_period(period_))
        return ErrorCode::InvalidArg;
    if (!graph_->dirty())
        return {};

    const auto code = runner_->commit(graph_->resolve(), period_, state_ == State::Running ? clock_.get() : nullptr);
    if (code)
    {
        graph_->mark_clean();
        if (state_ == State::Unprepared)
            state_ = State::Ready;
    }
    return code;
}

RetCode Engine::start()
{
    if (on_clock_thread() || state_ == State::Running)
        return ErrorCode::Busy;
    if (state_ == State::Unprepared)
        return ErrorCode::NotPrepared;
    clock_->start(*this);
    state_ = State::Running;
    return {};
}

RetCode Engine::stop() noexcept
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    if (state_ == State::Running)
    {
        clock_->stop();
        state_ = State::Ready;
    }
    return {};
}

RetCode Engine::reset() noexcept
{
    if (on_clock_thread() || state_ == State::Running)
        return ErrorCode::Busy;
    runner_->reset(*graph_);
    graph_->reset_nodes();
    position_.store(0, std::memory_order_release);
    return {};
}

std::uint64_t Engine::tick_position() const noexcept
{
    return position_.load(std::memory_order_acquire);
}

} // namespace aereborn
