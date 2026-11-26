#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "graph.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace aereborn
{

/// @brief Owns an audio graph and advances it on an internal software clock.
/// @note Serialize control calls, including node lookup, and destruction on the control side.
/// Control calls from this engine's clock thread throw Error with code RetCode::BUSY.
/// Node callbacks must defer control requests and return without waiting for them.
/// block_period() and tick_position() may run concurrently while the Engine is alive.
class Engine
{
  public:
    /// @brief Lifecycle state, as observed on the control side.
    enum class State
    {
        Unprepared,
        Ready,
        Running
    };

    /// @brief Construct an engine with an empty draft graph.
    /// @param block_period Fixed duration of each tick.
    /// @throws Error RetCode::EPARAM if the period is unsupported.
    explicit Engine(BlockPeriod block_period);

    /// @brief Stop the clock and release all plans and nodes.
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// @brief Add a node to the draft graph.
    /// @param instance Node to transfer to the engine. The call consumes it on success or failure.
    /// @return The new node ID.
    /// @throws Error RetCode::EPARAM for a null node or invalid port layout, or RetCode::TOOLARGE
    /// if the port count or identifier space exceeds capacity.
    NodeId add_node(std::unique_ptr<Node> instance);

    /// @brief Construct a node and add it to the draft graph.
    /// @tparam T Concrete node type derived from Node.
    /// @tparam Args Constructor argument types.
    /// @param args Arguments forwarded to the node constructor.
    /// @return The new node ID.
    /// @throws Error As documented for add_node().
    template <class T, class... Args> NodeId emplace_node(Args &&...args)
    {
        require_control_thread();
        return insert_node(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// @brief Remove a node and its connections from the draft graph.
    /// @param node_id ID of the node to remove.
    /// @throws Error RetCode::EPORT for a nonzero port, or RetCode::ENODE for an absent node.
    void remove_node(NodeId node_id);

    /// @brief Replace a draft node while preserving its ID and connections.
    /// @param node_id ID of the node to replace.
    /// @param replacement New instance with identical port specifications in the same order.
    /// The call consumes it on success or failure.
    /// @throws Error RetCode::EPORT for a nonzero port, RetCode::ENODE for an absent node,
    /// RetCode::EPARAM for a null replacement,
    /// or RetCode::INVFMT for a different port layout.
    void replace_node(NodeId node_id, std::unique_ptr<Node> replacement);

    /// @brief Stage a local pause or resume for the next submitted plan.
    /// @param node_id ID of the node to pause or resume.
    /// @param paused True to exclude the node from the next plan, false to make it eligible again.
    /// @throws Error RetCode::EPORT for a nonzero port, or RetCode::ENODE for an absent node.
    void set_paused(NodeId node_id, bool paused);

    /// @brief Look up a node in the draft graph.
    /// @param node_id ID of the node to look up.
    /// @return A borrowed node pointer.
    /// @throws Error RetCode::EPORT for a nonzero port, or RetCode::ENODE for an absent node.
    /// @note The pointer is valid until removal, replacement, or Engine destruction.
    /// Follow the node's own contract for concurrent access during rendering.
    Node *node(NodeId node_id);

    /// @brief Look up a draft node as a requested type.
    /// @tparam T Node type to cast the result to.
    /// @param node_id ID of the node to look up.
    /// @return A borrowed pointer.
    /// @throws Error As documented for node(NodeId), or RetCode::ETYPE if the cast fails.
    /// @note The pointer follows the same lifetime and access rules as node(NodeId).
    template <class T> T *node(NodeId node_id)
    {
        auto *typed_node = dynamic_cast<T *>(node(node_id));
        if (!typed_node)
            throw Error(RetCode::ETYPE);
        return typed_node;
    }

    /// @brief Connect an output to an input in the draft graph.
    /// @param from Source output port.
    /// @param to Destination input port, which can have only one source.
    /// @return The new connection ID.
    /// @throws Error RetCode::EPORT if either port is unavailable in the requested direction,
    /// or another status if graph validation fails.
    /// @note Port specifications must match, and the connection must leave the graph acyclic.
    ConnId connect(NodeId from, NodeId to);

    /// @brief Remove a connection from the draft graph.
    /// @param conn_id ID returned by connect().
    /// @throws Error RetCode::ECONN if the connection is absent.
    void disconnect(ConnId conn_id);

    /// @brief Prepare the draft and make it the current render plan.
    /// @note Skips an unchanged draft. Validation or preparation failure leaves the current plan in place.
    /// @note While running, wait for block-boundary takeover and release the previous
    /// plan on the calling thread.
    void commit();

    /// @brief Start the software clock with the current plan and tick position.
    /// @throws Error RetCode::ESTATE before the first successful commit, or RetCode::BUSY if already running.
    /// @note Thread creation exceptions propagate and leave the engine ready to retry.
    void start();

    /// @brief Stop the clock and wait for any in-flight block.
    /// @throws Error RetCode::BUSY if called from this engine's clock thread.
    void stop();

    /// @brief Reset nodes and the tick position while stopped.
    /// @throws Error RetCode::BUSY while running.
    /// @note Resets each node instance held by the draft or current plan once and sets the next tick to zero.
    void reset();

    /// @brief Query the fixed block period.
    /// @return The duration supplied to the constructor.
    BlockPeriod block_period() const noexcept
    {
        return period_;
    }

    /// @brief Query the current tick position.
    /// @return The zero-based position of the next tick, advanced after each completed block.
    std::uint64_t tick_position() const noexcept;

    /// @brief Return the current lifecycle state on the serialized control side.
    State state() const;

    /// @brief Report whether the draft needs a commit, including its initial preparation.
    bool dirty() const;

    /// @brief Count published render plans. Failed and unchanged commits leave this unchanged.
    std::uint64_t plan_generation() const;

    /// @brief Count software-clock periods skipped since construction or reset.
    /// @note May be read concurrently with rendering.
    std::uint64_t missed_periods() const noexcept { return missed_periods_.load(std::memory_order_relaxed); }

    /// @brief Estimate the most recent tick time in microseconds, including clock bookkeeping.
    /// @note May be read concurrently with rendering. Zero until sampled after construction or reset.
    std::uint64_t last_tick_us() const noexcept { return last_tick_us_.load(std::memory_order_relaxed); }

    /// @brief Borrow committed node IDs in execution order, or an empty view before commit.
    /// @note Use on the serialized control side until the next plan publication or destruction.
    View<const NodeId> plan_nodes() const;

    /// @brief Borrow the committed node at the given position in plan_nodes().
    /// @pre The index is less than plan_nodes().size().
    const Node &plan_node_at(std::size_t index) const;

    /// @brief Borrow committed connections, with the same lifetime as plan_nodes().
    View<const Connection> plan_connections() const;

    /// @brief Borrow a committed node instance, with the same lifetime as plan_nodes().
    /// @throws Error RetCode::EPORT for a nonzero port, or RetCode::ENODE for a node outside the plan.
    /// @note Follow the node's own contract for access during rendering.
    const Node *plan_node(NodeId node_id) const;

  private:
    class Graph;
    class Runner;
    class Clock;
    void require_control_thread() const;
    NodeId insert_node(std::unique_ptr<Node> instance);
    bool tick() noexcept;

    const BlockPeriod period_;
    State state_ = State::Unprepared;
    std::atomic<std::uint64_t> position_{0};
    std::uint64_t plan_generation_ = 0;
    std::atomic<std::uint64_t> missed_periods_{0};
    std::atomic<std::uint64_t> last_tick_us_{0};
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<Runner> runner_;
    std::unique_ptr<Clock> clock_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
