#ifndef AEREBORN_CORE_GRAPH_H
#define AEREBORN_CORE_GRAPH_H

#include "types.h"
#include <array>
#include <atomic>

namespace aereborn
{

// Shared by all engine specializations and translation units.
inline std::uint64_t get_next_identifier()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Execution unit with immutable input and output layouts.
template <SampleRate Rate, BlockPeriod Period> class Node
{
    static_assert(calc_block_frames<Rate, Period>() > 0, "Node needs a nonzero block size");

  public:
    virtual ~Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    // Each entry is the channel count of one port.
    virtual View<const std::size_t> inputs() const noexcept = 0;
    virtual View<const std::size_t> outputs() const noexcept = 0;
    // Engine binds one block view per declared port. Processing receives them through ProcContext.
    virtual View<IBlock> iblocks() noexcept = 0;
    virtual View<OBlock> oblocks() noexcept = 0;
    // Validate configuration and prepare resources. The engine may retry a failed preparation.
    virtual RetCode prepare()
    {
        return {};
    }
    // Fill every output sample in the current processing span.
    virtual void process(const ProcContext &context) noexcept = 0;
    virtual void reset() noexcept
    {
    }

  protected:
    Node() = default;
};

// Port counts are compile-time constants; channel counts and block views are stored inline.
template <std::size_t I, std::size_t O, SampleRate Rate, BlockPeriod Period> class NodeT : public Node<Rate, Period>
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
    View<IBlock> iblocks() noexcept final
    {
        return {iblocks_.data(), I};
    }
    View<OBlock> oblocks() noexcept final
    {
        return {oblocks_.data(), O};
    }

    std::array<std::size_t, I> input_channels_;
    std::array<std::size_t, O> output_channels_;
    std::array<IBlock, I> iblocks_;
    std::array<OBlock, O> oblocks_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_GRAPH_H
