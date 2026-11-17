#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "graph.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

namespace aereborn
{

namespace detail
{
inline void validate_gain(float gain)
{
    if (!std::isfinite(gain))
    {
        throw GraphError(ErrorCode::InvalidArgument, "Connection gain must be finite");
    }
}

template <std::size_t Frames> void mix(IBlock source, OBlock destination, const ConnOpts &options) noexcept
{
    if (options.muted || options.gain == 0.0f)
    {
        return;
    }
    auto add_channel = [&](std::size_t in, std::size_t out, float gain) {
        if (gain == 0.0f)
        {
            return;
        }
        const auto *input = source.channel(in);
        auto *output = destination.channel(out);
        for (std::size_t frame = 0; frame < Frames; ++frame)
        {
            output[frame] += input[frame] * gain;
        }
    };
    if (options.matrix.empty())
    {
        for (std::size_t ch = 0; ch < destination.channels(); ++ch)
        {
            add_channel(ch, ch, options.gain);
        }
        return;
    }
    const auto &coefficients = options.matrix.coefficients();
    for (std::size_t out = 0; out < destination.channels(); ++out)
    {
        for (std::size_t in = 0; in < source.channels(); ++in)
        {
            add_channel(in, out, coefficients[out * source.channels() + in] * options.gain);
        }
    }
}

} // namespace detail

// Phase 1: synchronous offline engine. All calls must be serialized by the caller.
template <SampleRate Rate, BlockPeriod Period> class Engine
{
    static_assert(calc_block_frames<Rate, Period>() > 0, "Engine needs a nonzero block size");

  public:
    Engine() = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    NodeId add_node(std::unique_ptr<Node<Rate, Period>> node)
    {
        if (!instance)
        {
            throw GraphError(ErrorCode::InvalidArgument, "Cannot add a null node");
        }
        const NodeId id{get_next_identifier()};
        nodes_.emplace(id, NodeEntry(std::move(instance)));
        ready_ = false;
        return id;
    }
    template <class T, class... Args> NodeId emplace_node(Args &&...args)
    {
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }
    void remove_node(NodeId id)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
        {
            throw GraphError(ErrorCode::UnknownNode, "Node does not belong to this engine or was removed");
        }
        ready_ = false;
        for (auto it = connections_.begin(); it != connections_.end();)
        {
            if (it->second.from.node == id || it->second.to.node == id)
            {
                it = connections_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        nodes_.erase(found);
    }
    Node<Rate, Period> &node(NodeId id)
    {
        return *entry(id).node;
    }
    const Node<Rate, Period> &node(NodeId id) const
    {
        return *entry(id).node;
    }
    template <class T> T &node(NodeId id)
    {
        auto *typed = dynamic_cast<T *>(&node(id));
        if (!typed)
        {
            throw GraphError(ErrorCode::InvalidNodeType, "Node has a different type");
        }
        return *typed;
    }
    IPort input(NodeId id, std::size_t port = 0) const
    {
        const IPort result{id, port};
        input_spec(result);
        return result;
    }
    OPort output(NodeId id, std::size_t port = 0) const
    {
        const OPort result{id, port};
        output_spec(result);
        return result;
    }
    ConnId connect(OPort from, IPort to, ConnOpts options = {})
    {
        const auto source_channels = output_spec(from).channels;
        const auto &destination = input_spec(to);
        detail::validate_gain(options.gain);
        if (options.matrix.empty())
        {
            if (source_channels != destination.channels)
            {
                throw GraphError(ErrorCode::ChannelMismatch, "Different channel counts require an explicit matrix");
            }
        }
        else if (options.matrix.input_channels() != source_channels ||
                 options.matrix.output_channels() != destination.channels)
        {
            throw GraphError(ErrorCode::ChannelMismatch, "Channel matrix does not match the ports");
        }
        for (const auto &item : connections_)
        {
            const auto &existing = item.second;
            if (existing.to.node != to.node || existing.to.index != to.index)
            {
                continue;
            }
            if (existing.from.node == from.node && existing.from.index == from.index)
            {
                throw GraphError(ErrorCode::DuplicateConnection, "These ports are already connected");
            }
            if (destination.mode == IMode::Single)
            {
                throw GraphError(ErrorCode::InputAlreadyConnected, "Input port accepts only one connection");
            }
        }
        if (would_cycle(from.node, to.node))
        {
            throw GraphError(ErrorCode::Cycle, "Connection would create a feedback cycle");
        }
        const ConnId id{get_next_identifier()};
        connections_.emplace(id, Connection{from, to, std::move(options)});
        ready_ = false;
        return id;
    }
    void disconnect(ConnId id)
    {
        if (connections_.erase(id) == 0)
        {
            throw GraphError(ErrorCode::UnknownConnection, "Connection does not exist");
        }
        ready_ = false;
    }
    void set_gain(ConnId id, float linear_gain)
    {
        detail::validate_gain(linear_gain);
        connection(id).options.gain = linear_gain;
    }
    void set_muted(ConnId id, bool muted)
    {
        connection(id).options.muted = muted;
    }
    // Prepare after topology changes. Existing prepared nodes keep their state.
    void prepare()
    {
        if (ready_)
        {
            return;
        }
        for (auto &item : nodes_)
        {
            auto &target = item.second;
            if (!target.prepared)
            {
                target.node->prepare();
                target.prepared = true;
            }
            target.visited = false;
            target.connection_count = 0;
        }

        // Index the existing connections by destination; processing reuses this index.
        connections_by_input_.clear();
        connections_by_input_.reserve(connections_.size());
        for (auto &item : connections_)
        {
            connections_by_input_.push_back(&item.second);
        }
        std::sort(connections_by_input_.begin(), connections_by_input_.end(),
                  [](const Connection *a, const Connection *b) {
                      return std::tie(a->to.node, a->to.index, a->from.node, a->from.index) <
                             std::tie(b->to.node, b->to.index, b->from.node, b->from.index);
                  });
        // Connection endpoints are valid: connect() checks them and remove_node() erases incident edges.
        for (std::size_t i = 0; i < connections_by_input_.size(); ++i)
        {
            auto &target = nodes_.find(connections_by_input_[i]->to.node)->second;
            if (target.connection_count == 0)
            {
                target.connection_begin = i;
            }
            ++target.connection_count;
        }

        plan_.clear();
        plan_.reserve(nodes_.size());
        std::size_t channels = 0;
        const auto channel_limit = samples_.max_size() / block_frames();
        auto count_channels = [&](std::size_t count) {
            if (count > channel_limit - channels)
            {
                throw std::length_error("Graph sample pool is too large");
            }
            channels += count;
        };
        auto visit = [&](auto &&self, NodeEntry &target) -> void {
            if (target.visited)
            {
                return;
            }
            target.visited = true;
            // connect() already guarantees an acyclic graph.
            for (std::size_t i = 0; i < target.connection_count; ++i)
            {
                const auto source = connections_by_input_[target.connection_begin + i]->from.node;
                self(self, nodes_.find(source)->second);
            }
            for (const auto &port : target.node->inputs())
            {
                count_channels(port.channels);
            }
            for (const auto &port : target.node->outputs())
            {
                count_channels(port.channels);
            }
            plan_.push_back(&target);
        };
        for (auto &item : nodes_)
        {
            if (item.second.node->outputs().empty())
            {
                visit(visit, item.second);
            }
        }

        samples_.resize(channels * block_frames());

        std::size_t offset = 0;
        for (auto *target : plan_)
        {
            auto &instance = *target->node;
            target->inputs = instance.iblocks();
            target->outputs = instance.oblocks();
            const auto inputs = target->inputs;
            const auto outputs = target->outputs;
            const auto ispecs = instance.inputs();
            const auto ospecs = instance.outputs();
            const auto input_offset = offset;
            std::size_t connection_index = 0;
            for (std::size_t i = 0; i < inputs.size(); ++i)
            {
                const OBlock destination(samples_.data() + offset, ispecs[i].channels, block_frames(), block_frames());
                inputs[i] = destination;
                offset += ispecs[i].channels * block_frames();
                // Incoming edges are sorted by input port; bind each destination once.
                while (connection_index < target->connection_count)
                {
                    auto &connection = *connections_by_input_[target->connection_begin + connection_index];
                    if (connection.to.index != i)
                    {
                        break;
                    }
                    const auto &source = nodes_.find(connection.from.node)->second;
                    connection.source_ = &source.outputs[connection.from.index];
                    connection.destination_ = destination;
                    ++connection_index;
                }
            }
            target->input_buffer = {samples_.data() + input_offset, offset - input_offset};
            for (std::size_t i = 0; i < outputs.size(); ++i)
            {
                outputs[i] = OBlock(samples_.data() + offset, ospecs[i].channels, block_frames(), block_frames());
                offset += ospecs[i].channels * block_frames();
            }
        }
        ready_ = true;
    }
    // Processes one full period; false means prepare() is required.
    bool process() noexcept
    {
        if (!ready_)
        {
            return false;
        }
        for (const auto *target : plan_)
        {
            const auto &step = *target;
            std::fill(step.input_buffer.begin(), step.input_buffer.end(), 0.0f);
            for (std::size_t i = 0; i < step.connection_count; ++i)
            {
                const auto &connection = *connections_by_input_[step.connection_begin + i];
                detail::mix<block_frames()>(*connection.source_, connection.destination_, connection.options);
            }
            step.node->process({frame_position_, step.inputs, step.outputs});
        }
        frame_position_ += block_frames();
        return true;
    }
    void reset() noexcept
    {
        for (auto &item : nodes_)
        {
            item.second.node->reset();
        }
        frame_position_ = 0;
    }
    static constexpr std::size_t block_frames() noexcept
    {
        return calc_block_frames<Rate, Period>();
    }
    std::uint64_t frame_position() const noexcept
    {
        return frame_position_;
    }
    std::size_t node_count() const noexcept
    {
        return nodes_.size();
    }
    std::size_t connection_count() const noexcept
    {
        return connections_.size();
    }

  private:
    struct NodeEntry
    {
        explicit NodeEntry(std::unique_ptr<Node<Rate, Period>> instance) : node(std::move(instance))
        {
        }
        std::unique_ptr<Node<Rate, Period>> node;
        bool prepared = false;
        bool visited = false;
        std::size_t connection_begin = 0;
        std::size_t connection_count = 0;
        View<IBlock> inputs{nullptr, 0};
        View<OBlock> outputs{nullptr, 0};
        View<float> input_buffer{nullptr, 0};
    };

    const NodeEntry &entry(NodeId id) const
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
        {
            throw GraphError(ErrorCode::UnknownNode, "Node does not belong to this engine or was removed");
        }
        return found->second;
    }
    Connection &connection(ConnId id)
    {
        const auto found = connections_.find(id);
        if (found == connections_.end())
        {
            throw GraphError(ErrorCode::UnknownConnection, "Connection does not exist");
        }
        return found->second;
    }
    const ISpec &input_spec(IPort port) const
    {
        const auto &ports = node(port.node).inputs();
        if (port.index >= ports.size())
        {
            throw GraphError(ErrorCode::InvalidPort, "Input port index is out of range");
        }
        return ports[port.index];
    }
    const OSpec &output_spec(OPort port) const
    {
        const auto &ports = node(port.node).outputs();
        if (port.index >= ports.size())
        {
            throw GraphError(ErrorCode::InvalidPort, "Output port index is out of range");
        }
        return ports[port.index];
    }
    bool would_cycle(NodeId from, NodeId to)
    {
        work_.clear();
        work_.reserve(nodes_.size());
        for (auto &item : nodes_)
        {
            item.second.visited = false;
        }
        nodes_.find(to)->second.visited = true;
        work_.push_back(to);
        while (!work_.empty())
        {
            const auto current = work_.back();
            work_.pop_back();
            if (current == from)
            {
                return true;
            }
            for (const auto &item : connections_)
            {
                if (item.second.from.node == current)
                {
                    auto &target = nodes_.find(item.second.to.node)->second;
                    if (!target.visited)
                    {
                        target.visited = true;
                        work_.push_back(item.second.to.node);
                    }
                }
            }
        }
        return false;
    }

  private:
    std::map<NodeId, NodeEntry> nodes_;
    std::map<ConnId, Connection> connections_;
    std::vector<NodeEntry *> plan_;
    std::vector<float> samples_;
    // Shared connection index for graph preparation and per-step processing.
    std::vector<Connection *> connections_by_input_;
    // Reused control-side scratch storage.
    std::vector<NodeId> work_;
    std::uint64_t frame_position_ = 0;
    bool ready_ = false;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
