#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "graph.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace aereborn
{

// Phase 1: synchronous offline engine. All calls must be serialized by the caller.
template <SampleRate Rate, BlockPeriod Period> class Engine
{
    static_assert(calc_block_frames<Rate, Period>() > 0, "Engine needs a nonzero block size");

  public:
    Engine() = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Result<NodeId> add_node(std::unique_ptr<Node<Rate, Period>> instance) noexcept
    try
    {
        if (!instance)
        {
            return ErrorCode::InvalidArg;
        }
        for (const auto ports : {instance->inputs(), instance->outputs()})
        {
            for (const auto channels : ports)
            {
                if (channels == 0)
                {
                    return ErrorCode::InvalidArg;
                }
            }
        }
        const NodeId node_id{get_next_identifier()};
        nodes_.emplace(node_id, NodeEntry{std::move(instance)});
        ready_ = false;
        return node_id;
    }
    catch (const std::bad_alloc &)
    {
        return ErrorCode::NoMemory;
    }
    template <class T, class... Args> Result<NodeId> emplace_node(Args &&...args) noexcept
    try
    {
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }
    catch (const std::bad_alloc &)
    {
        return ErrorCode::NoMemory;
    }
    catch (const std::length_error &)
    {
        return ErrorCode::TooLarge;
    }
    catch (...)
    {
        return ErrorCode::NodeFailed;
    }
    RetCode remove_node(NodeId node_id) noexcept
    {
        const auto node_it = nodes_.find(node_id);
        if (node_it == nodes_.end())
        {
            return ErrorCode::UnknownNode;
        }
        ready_ = false;
        for (auto conn_it = connections_.begin(); conn_it != connections_.end();)
        {
            const auto &connection = conn_it->second;
            if (connection.from.node == node_id || connection.to.node == node_id)
            {
                conn_it = connections_.erase(conn_it);
            }
            else
            {
                ++conn_it;
            }
        }
        nodes_.erase(node_it);
        return {};
    }
    Result<Node<Rate, Period> *> node(NodeId node_id) noexcept
    {
        const auto node_it = nodes_.find(node_id);
        if (node_it == nodes_.end())
        {
            return ErrorCode::UnknownNode;
        }
        return node_it->second.node.get();
    }
    Result<const Node<Rate, Period> *> node(NodeId node_id) const noexcept
    {
        const auto node_it = nodes_.find(node_id);
        if (node_it == nodes_.end())
        {
            return ErrorCode::UnknownNode;
        }
        return node_it->second.node.get();
    }
    template <class T> Result<T *> node(NodeId node_id) noexcept
    {
        const auto result = node(node_id);
        if (!result)
        {
            return result.ret_code();
        }
        auto *typed_node = dynamic_cast<T *>(result.value());
        if (!typed_node)
        {
            return ErrorCode::InvalidType;
        }
        return typed_node;
    }
    Result<ConnId> connect(OPort from, IPort to, const ConnOpts &options = {}) noexcept
    try
    {
        const auto src_channels = output_channels(from);
        if (!src_channels)
        {
            return src_channels.ret_code();
        }
        const auto dst_channels = input_channels(to);
        if (!dst_channels)
        {
            return dst_channels.ret_code();
        }
        if (!std::isfinite(options.gain))
        {
            return ErrorCode::InvalidArg;
        }
        const auto &matrix = options.matrix;
        const bool channels_match = matrix.empty() ? src_channels.value() == dst_channels.value()
                                                   : matrix.input_channels() == src_channels.value() &&
                                                         matrix.output_channels() == dst_channels.value();
        if (!channels_match)
        {
            return ErrorCode::ChannelMismatch;
        }
        for (const auto &item : connections_)
        {
            const auto &connection = item.second;
            if (connection.to.node == to.node && connection.to.index == to.index && connection.from.node == from.node &&
                connection.from.index == from.index)
            {
                return ErrorCode::DuplicateConn;
            }
        }
        if (would_cycle(from.node, to.node))
        {
            return ErrorCode::Cycle;
        }
        const ConnId conn_id{get_next_identifier()};
        connections_.emplace(conn_id, Connection{from, to, options});
        ready_ = false;
        return conn_id;
    }
    catch (const std::bad_alloc &)
    {
        return ErrorCode::NoMemory;
    }
    catch (const std::length_error &)
    {
        return ErrorCode::TooLarge;
    }
    RetCode disconnect(ConnId conn_id) noexcept
    {
        if (connections_.erase(conn_id) == 0)
        {
            return ErrorCode::UnknownConn;
        }
        ready_ = false;
        return {};
    }
    RetCode set_gain(ConnId conn_id, float linear_gain) noexcept
    {
        if (!std::isfinite(linear_gain))
        {
            return ErrorCode::InvalidArg;
        }
        const auto conn_it = connections_.find(conn_id);
        if (conn_it == connections_.end())
        {
            return ErrorCode::UnknownConn;
        }
        conn_it->second.options.gain = linear_gain;
        return {};
    }
    RetCode set_muted(ConnId conn_id, bool muted) noexcept
    {
        const auto conn_it = connections_.find(conn_id);
        if (conn_it == connections_.end())
        {
            return ErrorCode::UnknownConn;
        }
        conn_it->second.options.muted = muted;
        return {};
    }
    // Prepare after topology changes. Existing prepared nodes keep their state.
    RetCode prepare() noexcept
    try
    {
        if (ready_)
        {
            return {};
        }
        for (auto &item : nodes_)
        {
            item.second.visited = false;
            item.second.conn_count = 0;
        }

        // Index the existing connections by destination; processing reuses this index.
        conns_by_input_.clear();
        conns_by_input_.reserve(connections_.size());
        for (auto &item : connections_)
        {
            conns_by_input_.push_back(&item.second);
        }
        std::sort(conns_by_input_.begin(), conns_by_input_.end(), [](const Connection *lhs, const Connection *rhs) {
            return std::tie(lhs->to.node, lhs->to.index, lhs->from.node, lhs->from.index) <
                   std::tie(rhs->to.node, rhs->to.index, rhs->from.node, rhs->from.index);
        });
        // connect() validates endpoints and remove_node() erases incident connections.
        for (std::size_t conn_index = 0; conn_index < conns_by_input_.size(); ++conn_index)
        {
            auto &entry = nodes_.find(conns_by_input_[conn_index]->to.node)->second;
            if (entry.conn_count == 0)
            {
                entry.conn_offset = conn_index;
            }
            ++entry.conn_count;
        }

        plan_.clear();
        plan_.reserve(nodes_.size());
        std::size_t total_channels = 0;
        const auto max_channels = samples_.max_size() / block_frames();
        auto visit_node = [&](auto &&self, NodeEntry &entry) -> RetCode {
            if (entry.visited)
            {
                return {};
            }
            entry.visited = true;
            // connect() already guarantees an acyclic graph.
            for (std::size_t i = 0; i < entry.conn_count; ++i)
            {
                const auto src_id = conns_by_input_[entry.conn_offset + i]->from.node;
                const auto code = self(self, nodes_.find(src_id)->second);
                if (!code)
                {
                    return code;
                }
            }
            for (const auto ports : {entry.node->inputs(), entry.node->outputs()})
            {
                for (const auto channels : ports)
                {
                    if (channels > max_channels - total_channels)
                    {
                        return ErrorCode::TooLarge;
                    }
                    total_channels += channels;
                }
            }
            if (!entry.prepared)
            {
                const auto code = entry.node->prepare();
                if (!code)
                {
                    return code;
                }
                entry.prepared = true;
            }
            plan_.push_back(&entry);
            return {};
        };
        for (auto &item : nodes_)
        {
            auto &entry = item.second;
            if (entry.node->outputs().empty())
            {
                const auto code = visit_node(visit_node, entry);
                if (!code)
                {
                    return code;
                }
            }
        }
        samples_.resize(total_channels * block_frames());

        std::size_t sample_offset = 0;
        for (auto *entry : plan_)
        {
            auto &instance = *entry->node;
            entry->inputs = instance.iblocks();
            entry->outputs = instance.oblocks();
            const auto input_channels = instance.inputs();
            const auto output_channels = instance.outputs();
            assert(entry->inputs.size() == input_channels.size() && entry->outputs.size() == output_channels.size());
            const auto input_sample_offset = sample_offset;
            std::size_t conn_index = 0;
            for (std::size_t port_index = 0; port_index < entry->inputs.size(); ++port_index)
            {
                const auto channels = input_channels[port_index];
                const OBlock input_block(samples_.data() + sample_offset, channels, block_frames(), block_frames());
                entry->inputs[port_index] = input_block;
                sample_offset += channels * block_frames();
                // Incoming edges are sorted by input port; bind each destination once.
                while (conn_index < entry->conn_count)
                {
                    auto &connection = *conns_by_input_[entry->conn_offset + conn_index];
                    if (connection.to.index != port_index)
                    {
                        break;
                    }
                    const auto &src_entry = nodes_.find(connection.from.node)->second;
                    connection.src_block = &src_entry.outputs[connection.from.index];
                    connection.dst_block = input_block;
                    ++conn_index;
                }
            }
            entry->input_samples = {samples_.empty() ? nullptr : samples_.data() + input_sample_offset,
                                    sample_offset - input_sample_offset};
            for (std::size_t port_index = 0; port_index < entry->outputs.size(); ++port_index)
            {
                const auto channels = output_channels[port_index];
                entry->outputs[port_index] = {samples_.data() + sample_offset, channels, block_frames(),
                                              block_frames()};
                sample_offset += channels * block_frames();
            }
        }
        ready_ = true;
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return ErrorCode::NoMemory;
    }
    catch (const std::length_error &)
    {
        return ErrorCode::TooLarge;
    }
    catch (...)
    {
        return ErrorCode::NodeFailed;
    }
    // Processes one full period or reports that prepare() is required.
    RetCode process() noexcept
    {
        if (!ready_)
        {
            return ErrorCode::NotPrepared;
        }
        for (const auto *entry : plan_)
        {
            std::fill(entry->input_samples.begin(), entry->input_samples.end(), 0.0f);
            for (std::size_t i = 0; i < entry->conn_count; ++i)
            {
                const auto &connection = *conns_by_input_[entry->conn_offset + i];
                if (!connection.options.muted)
                {
                    detail::mix(*connection.src_block, connection.dst_block, connection.options.matrix,
                                connection.options.gain);
                }
            }
            entry->node->process({frame_position_, entry->inputs, entry->outputs});
        }
        frame_position_ += block_frames();
        return {};
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

  private:
    struct NodeEntry
    {
        std::unique_ptr<Node<Rate, Period>> node;
        bool prepared = false;
        bool visited = false;
        std::size_t conn_offset = 0;
        std::size_t conn_count = 0;
        View<IBlock> inputs{nullptr, 0};
        View<OBlock> outputs{nullptr, 0};
        View<float> input_samples{nullptr, 0};
    };

    // The graph editor and processing plan share the same connection records.
    struct Connection
    {
        OPort from;
        IPort to;
        ConnOpts options;
        const OBlock *src_block = nullptr;
        OBlock dst_block{};
    };

    Result<std::size_t> input_channels(IPort port) const noexcept
    {
        const auto result = node(port.node);
        if (!result)
        {
            return result.ret_code();
        }
        const auto ports = result.value()->inputs();
        if (port.index >= ports.size())
        {
            return ErrorCode::InvalidPort;
        }
        return ports[port.index];
    }
    Result<std::size_t> output_channels(OPort port) const noexcept
    {
        const auto result = node(port.node);
        if (!result)
        {
            return result.ret_code();
        }
        const auto ports = result.value()->outputs();
        if (port.index >= ports.size())
        {
            return ErrorCode::InvalidPort;
        }
        return ports[port.index];
    }
    bool would_cycle(NodeId from, NodeId to)
    {
        pending_node_ids_.clear();
        pending_node_ids_.reserve(nodes_.size());
        for (auto &item : nodes_)
        {
            item.second.visited = false;
        }
        nodes_.find(to)->second.visited = true;
        pending_node_ids_.push_back(to);
        while (!pending_node_ids_.empty())
        {
            const auto node_id = pending_node_ids_.back();
            pending_node_ids_.pop_back();
            if (node_id == from)
            {
                return true;
            }
            for (const auto &item : connections_)
            {
                const auto &connection = item.second;
                if (connection.from.node == node_id)
                {
                    const auto next_id = connection.to.node;
                    auto &entry = nodes_.find(next_id)->second;
                    if (!entry.visited)
                    {
                        entry.visited = true;
                        pending_node_ids_.push_back(next_id);
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
    std::vector<Connection *> conns_by_input_;
    std::vector<NodeId> pending_node_ids_;
    std::uint64_t frame_position_ = 0;
    bool ready_ = false;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
