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
    // Each entry is the channel count of one port.
    virtual View<const std::size_t> inputs() const noexcept = 0;
    virtual View<const std::size_t> outputs() const noexcept = 0;
    // Validate configuration and prepare resources. The engine may retry a failed preparation.
    virtual RetCode prepare(const EngineSpec &)
    {
        return {};
    }
    // Use context's block views during this call and fill every output sample.
    virtual void process(const ProcContext &context) noexcept = 0;
    virtual void reset() noexcept
    {
    }

  protected:
    Node() = default;
};

// Port counts are compile-time constants. Channel counts are stored inline.
template <std::size_t I, std::size_t O> class NodeT : public Node
{
    static_assert(I + O > 0, "Node needs at least one port");

  public:
    NodeT(std::array<std::size_t, I> input_channels, std::array<std::size_t, O> output_channels)
        : input_channels_(input_channels), output_channels_(output_channels)
    {
    }
    View<const std::size_t> inputs() const noexcept final
    {
        return {input_channels_.data(), I};
    }
    View<const std::size_t> outputs() const noexcept final
    {
        return {output_channels_.data(), O};
    }

  private:
    std::array<std::size_t, I> input_channels_;
    std::array<std::size_t, O> output_channels_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_GRAPH_H
