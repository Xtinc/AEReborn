#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "graph.h"
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace aereborn
{

// Phase 1: synchronous offline engine. All calls must be serialized by the caller.
class Engine
{
  public:
    Engine(SampleRate sample_rate, BlockPeriod block_period);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Result<NodeId> add_node(std::unique_ptr<Node> instance);
    template <class T, class... Args> Result<NodeId> emplace_node(Args &&...args)
    {
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }
    RetCode remove_node(NodeId node_id) noexcept;
    Result<Node *> node(NodeId node_id) noexcept;
    Result<const Node *> node(NodeId node_id) const noexcept;
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
    Result<ConnId> connect(OPort from, IPort to, const ConnOpts &options = {});
    RetCode disconnect(ConnId conn_id) noexcept;
    RetCode set_gain(ConnId conn_id, float linear_gain) noexcept;
    RetCode set_muted(ConnId conn_id, bool muted) noexcept;
    // Prepare after topology changes. Existing prepared nodes keep their state.
    RetCode prepare();
    // Processes one full period or reports that prepare() is required.
    RetCode process() noexcept;
    void reset() noexcept;
    EngineSpec spec() const noexcept;
    std::uint64_t frame_position() const noexcept;

  private:
    struct NodeEntry
    {
        std::unique_ptr<Node> node;
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

    bool would_cycle(NodeId from, NodeId to);
    RetCode prepare_node(NodeEntry &entry, std::size_t max_channels, std::size_t &total_channels);

    const EngineSpec spec_;
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
