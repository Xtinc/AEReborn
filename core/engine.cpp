#include "engine.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <tuple>

namespace aereborn
{
static std::uint64_t get_next_identifier() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

Engine::Engine(SampleRate sample_rate, BlockPeriod block_period) : spec_(sample_rate, block_period)
{
}

Engine::~Engine() = default;

Result<NodeId> Engine::add_node(std::unique_ptr<Node> instance)
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

RetCode Engine::remove_node(NodeId node_id) noexcept
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

Result<Node *> Engine::node(NodeId node_id) noexcept
{
    const auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end())
    {
        return ErrorCode::UnknownNode;
    }
    return node_it->second.node.get();
}

Result<const Node *> Engine::node(NodeId node_id) const noexcept
{
    const auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end())
    {
        return ErrorCode::UnknownNode;
    }
    return node_it->second.node.get();
}

static Result<std::size_t> port_channels(const Result<Node *> &node, std::size_t port_index,
                                         View<const std::size_t> (Node::*get_ports)() const) noexcept
{
    if (!node)
    {
        return node.ret_code();
    }
    const auto ports = (node.value()->*get_ports)();
    if (port_index >= ports.size())
    {
        return ErrorCode::InvalidPort;
    }
    return ports[port_index];
}

Result<ConnId> Engine::connect(OPort from, IPort to, const ConnOpts &options)
{
    const auto src_channels = port_channels(node(from.node), from.index, &Node::outputs);
    if (!src_channels)
    {
        return src_channels.ret_code();
    }
    const auto dst_channels = port_channels(node(to.node), to.index, &Node::inputs);
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

RetCode Engine::disconnect(ConnId conn_id) noexcept
{
    if (connections_.erase(conn_id) == 0)
    {
        return ErrorCode::UnknownConn;
    }
    ready_ = false;
    return {};
}

RetCode Engine::set_gain(ConnId conn_id, float linear_gain) noexcept
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

RetCode Engine::set_muted(ConnId conn_id, bool muted) noexcept
{
    const auto conn_it = connections_.find(conn_id);
    if (conn_it == connections_.end())
    {
        return ErrorCode::UnknownConn;
    }
    conn_it->second.options.muted = muted;
    return {};
}

RetCode Engine::prepare()
{
    if (ready_)
    {
        return {};
    }
    const auto frames = spec_.block_frames();
    if (frames == 0)
    {
        return ErrorCode::InvalidArg;
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
    const auto max_channels = samples_.max_size() / frames;
    for (auto &item : nodes_)
    {
        auto &entry = item.second;
        if (entry.node->outputs().empty())
        {
            const auto code = prepare_node(entry, max_channels, total_channels);
            if (!code)
            {
                return code;
            }
        }
    }
    samples_.resize(total_channels * frames);

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
            const OBlock input_block(samples_.data() + sample_offset, channels, frames, frames);
            entry->inputs[port_index] = input_block;
            sample_offset += channels * frames;
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
            entry->outputs[port_index] = {samples_.data() + sample_offset, channels, frames, frames};
            sample_offset += channels * frames;
        }
    }
    ready_ = true;
    return {};
}

RetCode Engine::prepare_node(NodeEntry &entry, std::size_t max_channels, std::size_t &total_channels)
{
    if (entry.visited)
    {
        return {};
    }
    entry.visited = true;
    // connect() guarantees an acyclic graph.
    for (std::size_t i = 0; i < entry.conn_count; ++i)
    {
        const auto src_id = conns_by_input_[entry.conn_offset + i]->from.node;
        const auto code = prepare_node(nodes_.find(src_id)->second, max_channels, total_channels);
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
        const auto code = entry.node->prepare(spec_);
        if (!code)
        {
            return code;
        }
        entry.prepared = true;
    }
    plan_.push_back(&entry);
    return {};
}

RetCode Engine::process() noexcept
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
            const auto &options = connection.options;
            if (!options.muted)
            {
                options.matrix.mix_add(*connection.src_block, connection.dst_block, options.gain);
            }
        }
        entry->node->process({frame_position_, entry->inputs, entry->outputs});
    }
    frame_position_ += spec_.block_frames();
    return {};
}

void Engine::reset() noexcept
{
    for (auto &item : nodes_)
    {
        item.second.node->reset();
    }
    frame_position_ = 0;
}

EngineSpec Engine::spec() const noexcept
{
    return spec_;
}

std::uint64_t Engine::frame_position() const noexcept
{
    return frame_position_;
}

bool Engine::would_cycle(NodeId from, NodeId to)
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

} // namespace aereborn
