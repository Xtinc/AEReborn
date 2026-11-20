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
/// Control calls from this engine's clock thread return `Busy`.
/// Node callbacks must defer control requests and return without waiting for them.
/// block_period() and tick_position() may run concurrently while the Engine is alive.
class Engine
{
  public:
    /// @brief Construct an engine with an empty draft graph.
    /// @param block_period Fixed duration of each tick, validated by commit().
    explicit Engine(BlockPeriod block_period);

    /// @brief Stop the clock and release all plans and nodes.
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// @brief Add a node to the draft graph.
    /// @param instance Node to transfer to the engine. The call consumes it on success or failure.
    /// @return The new node ID, or `InvalidArg` for a null node or invalid port layout.
    Result<NodeId> add_node(std::unique_ptr<Node> instance);

    /// @brief Construct a node and add it to the draft graph.
    /// @tparam T Concrete node type derived from Node.
    /// @tparam Args Constructor argument types.
    /// @param args Arguments forwarded to the node constructor.
    /// @return The new node ID or one of the errors documented for add_node().
    template <class T, class... Args> Result<NodeId> emplace_node(Args &&...args)
    {
        if (on_clock_thread())
            return ErrorCode::Busy;
        return insert_node(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// @brief Remove a node and its connections from the draft graph.
    /// @param node_id ID of the node to remove.
    /// @return Success on removal or `UnknownNode` if the draft has no such node.
    RetCode remove_node(NodeId node_id) noexcept;

    /// @brief Replace a draft node while preserving its ID and connections.
    /// @param node_id ID of the node to replace.
    /// @param replacement New instance with identical port specifications in the same order.
    /// The call consumes it on success or failure.
    /// @return Success on replacement, or `UnknownNode`, `InvalidArg`, or `FormatMismatch`.
    RetCode replace_node(NodeId node_id, std::unique_ptr<Node> replacement);

    /// @brief Stage a local pause or resume for the next submitted plan.
    /// @param node_id ID of the node to pause or resume.
    /// @param paused True to exclude the node from the next plan, false to make it eligible again.
    /// @return Success if the node exists in the draft, otherwise `UnknownNode`.
    RetCode set_paused(NodeId node_id, bool paused);

    /// @brief Look up a node in the draft graph.
    /// @param node_id ID of the node to look up.
    /// @return A borrowed node pointer, or `UnknownNode` if the draft has no such node.
    /// @note The pointer is valid until removal, replacement, or Engine destruction.
    /// Follow the node's own contract for concurrent access during rendering.
    Result<Node *> node(NodeId node_id) noexcept;

    /// @brief Look up a draft node as a requested type.
    /// @tparam T Node type to cast the result to.
    /// @param node_id ID of the node to look up.
    /// @return A borrowed pointer, a lookup error, or `InvalidType` if the cast fails.
    /// @note The pointer follows the same lifetime and access rules as node(NodeId).
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

    /// @brief Connect an output to an input in the draft graph.
    /// @param from Source output port.
    /// @param to Destination input port, which can have only one source.
    /// @return The new connection ID or a graph validation error.
    /// @note Port specifications must match, and the connection must leave the graph acyclic.
    Result<ConnId> connect(OPort from, IPort to);

    /// @brief Remove a connection from the draft graph.
    /// @param conn_id ID returned by connect().
    /// @return Success on removal or `UnknownConn` if the connection is absent.
    RetCode disconnect(ConnId conn_id) noexcept;

    /// @brief Prepare the draft and make it the current render plan.
    /// @return Success if the plan is installed or the draft is unchanged.
    /// On validation or preparation failure, returns the error and keeps the current plan.
    /// @note While running, wait for block-boundary takeover and release the previous
    /// plan on the calling thread.
    RetCode commit();

    /// @brief Start the software clock with the current plan and tick position.
    /// @return Success, `NotPrepared` before the first successful commit, or `Busy` if already running.
    /// @note Thread creation exceptions propagate and leave the engine ready to retry.
    RetCode start();

    /// @brief Stop the clock and wait for any in-flight block.
    /// @return Success after the clock stops, including when it was already stopped.
    RetCode stop() noexcept;

    /// @brief Reset nodes and the tick position while stopped.
    /// @return Success when stopped, or `Busy` while running.
    /// @note Resets each node instance held by the draft or current plan once and sets the next tick to zero.
    RetCode reset() noexcept;

    /// @brief Query the fixed block period.
    /// @return The duration supplied to the constructor.
    BlockPeriod block_period() const noexcept
    {
        return period_;
    }

    /// @brief Query the current tick position.
    /// @return The zero-based position of the next tick, advanced after each completed block.
    std::uint64_t tick_position() const noexcept;

  private:
    class Graph;
    class Runner;
    class Clock;
    enum class State
    {
        Unprepared,
        Ready,
        Running
    };

    bool on_clock_thread() const noexcept;
    Result<NodeId> insert_node(std::unique_ptr<Node> instance);
    bool tick() noexcept;

    const BlockPeriod period_;
    State state_ = State::Unprepared;
    std::atomic<std::uint64_t> position_{0};
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<Runner> runner_;
    std::unique_ptr<Clock> clock_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
