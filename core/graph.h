#ifndef AEREBORN_CORE_GRAPH_H
#define AEREBORN_CORE_GRAPH_H

#include "types.h"
#include <array>

namespace aereborn
{

/// @brief Graph execution unit with immutable input and output port layouts.
///
/// Keep the port descriptors and their storage valid and unchanged for as long as
/// the engine owns the node.
/// Defer any requests to control the owning engine and return without waiting for them.
class Node
{
  public:
    virtual ~Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    /// @brief Return a static diagnostic type name. May be called during processing.
    virtual const char *type_name() const noexcept { return "Node"; }

    /// @brief Describe the input ports.
    /// @return A read-only view of the input port specifications in port index order.
    virtual View<const PortSpec> inputs() const noexcept = 0;

    /// @brief Describe the output ports.
    /// @return A read-only view of the output port specifications in port index order.
    virtual View<const PortSpec> outputs() const noexcept = 0;

    /// @brief Prepare the node's processing resources on the control thread.
    /// @param period Block period validated by the engine.
    /// @throws Error If preparation fails.
    /// @note The default implementation succeeds without preparing any resources.
    /// @note The engine may retry after a failure.
    virtual void prepare(BlockPeriod /*period*/) {}

    /// @brief Process one block and write every output sample in its declared format.
    /// @param context Current period, tick position, port blocks, and per-port activity flags.
    /// @pre Preparation has succeeded.
    /// @note Sources default to active. Other nodes' outputs default to the logical OR of their inputs.
    /// Mark an output active if it produces audio with all inputs inactive. Inactive outputs must be filled with zero bytes.
    /// @note The context and its views are valid only during this call. Keep processing
    /// free of heap allocation and deallocation, blocking I/O, and logging.
    /// Avoid lock waits where possible.
    virtual void process(const ProcessContext &context) noexcept = 0;

    /// @brief Reset the node's processing state.
    /// @pre Reset must be serialized with processing.
    /// @note The engine may reset a node before its first preparation.
    virtual void reset() noexcept
    {
    }

  protected:
    Node() = default;
};

/// @brief Node base with compile-time port counts and inline port specifications.
/// @tparam I Number of input ports.
/// @tparam O Number of output ports.
template <std::size_t I, std::size_t O> class NodeT : public Node
{
    static_assert(I + O > 0, "Node needs at least one port");

  public:
    NodeT(std::array<PortSpec, I> inputs, std::array<PortSpec, O> outputs) : inputs_(inputs), outputs_(outputs)
    {
    }
    View<const PortSpec> inputs() const noexcept final
    {
        return {inputs_.data(), I};
    }
    View<const PortSpec> outputs() const noexcept final
    {
        return {outputs_.data(), O};
    }

  private:
    const std::array<PortSpec, I> inputs_;
    const std::array<PortSpec, O> outputs_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_GRAPH_H
