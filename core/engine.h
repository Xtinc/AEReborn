#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "clock.h"
#include "graph.h"
#include <memory>
#include <utility>

namespace aereborn
{

using PlanRevision = std::uint64_t;

// Serialize control calls. tick_position() and applied_revision() may be read concurrently.
class Engine
{
  public:
    explicit Engine(BlockPeriod block_period);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Result<NodeId> add_node(std::unique_ptr<Node> instance);
    template <class T, class... Args> Result<NodeId> emplace_node(Args &&...args)
    {
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }
    RetCode remove_node(NodeId node_id) noexcept;
    // Stage a node with the same fixed input and output specifications. Existing connections follow its ID.
    RetCode replace_node(NodeId node_id, std::unique_ptr<Node> replacement);
    // Stage a local pause or resume. prepare() applies it with the graph at a block boundary.
    RetCode set_paused(NodeId node_id, bool paused);
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
    Result<ConnId> connect(OPort from, IPort to);
    RetCode disconnect(ConnId conn_id) noexcept;
    // Prepare and submit the draft for application at a block boundary.
    Result<PlanRevision> prepare();
    RetCode start(std::unique_ptr<ClockDriver> driver = {});
    RetCode stop() noexcept;
    RetCode reset() noexcept;
    BlockPeriod block_period() const noexcept;
    std::uint64_t tick_position() const noexcept;
    PlanRevision applied_revision() const noexcept;

  private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
