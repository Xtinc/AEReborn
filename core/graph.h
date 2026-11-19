#ifndef AEREBORN_CORE_GRAPH_H
#define AEREBORN_CORE_GRAPH_H

#include "types.h"
#include <array>

namespace aereborn
{

// Execution unit with immutable input and output layouts.
class Node
{
  public:
    virtual ~Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    // The engine borrows these descriptors for as long as it owns the node.
    virtual View<const PortSpec> inputs() const noexcept = 0;
    virtual View<const PortSpec> outputs() const noexcept = 0;
    // Validate configuration and prepare resources. The engine may retry a failed preparation.
    virtual RetCode prepare(BlockPeriod)
    {
        return {};
    }
    // Context views are valid during this call. Fill every sample of each output using its declared format.
    virtual void process(const ProcContext &context) noexcept = 0;
    virtual void reset() noexcept
    {
    }

  protected:
    Node() = default;
};

// Port counts are compile-time constants. Descriptors are stored inline.
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
