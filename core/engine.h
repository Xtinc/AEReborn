#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "clock.h"
#include "graph.h"
#include <memory>
#include <utility>

namespace aereborn
{

using PlanRevision = std::uint64_t;
class Observer;

// Serialize control calls. frame_position() and applied_revision() may be read concurrently.
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
    // Stage a node with compatible graph ports. Existing connections and observers follow its ID.
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
    Result<ConnId> connect(OPort from, IPort to, const ConnOpts &options = {});
    RetCode disconnect(ConnId conn_id) noexcept;
    RetCode set_gain(ConnId conn_id, float linear_gain) noexcept;
    RetCode set_muted(ConnId conn_id, bool muted) noexcept;
    // Prepare and submit the draft. Changing a running node's port format requires an applied pause first.
    Result<PlanRevision> prepare();
    RetCode start(std::unique_ptr<ClockDriver> driver = {});
    RetCode stop() noexcept;
    RetCode reset() noexcept;
    // Describe a port in the draft graph. Observers report the format of the data they deliver.
    Result<PortSpec> port_spec(ObservationPort port) const noexcept;
    // Stage an observer for the next prepared plan. Its queue is prepared with the port format.
    Result<ObserverId> add_observer(std::unique_ptr<Observer> observer);
    // Cancel delivery and wait for any active callback. The old plan retains the queue until retirement.
    RetCode remove_observer(ObserverId observer) noexcept;
    EngineSpec spec() const noexcept;
    std::uint64_t frame_position() const noexcept;
    PlanRevision applied_revision() const noexcept;

  private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
