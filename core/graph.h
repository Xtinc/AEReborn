#ifndef AEREBORN_CORE_GRAPH_H
#define AEREBORN_CORE_GRAPH_H

#include "block.h"
#include <array>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace aereborn
{

enum class SampleRate : std::uint32_t
{
    Hz8000 = 8000,
    Hz11025 = 11025,
    Hz16000 = 16000,
    Hz22050 = 22050,
    Hz24000 = 24000,
    Hz32000 = 32000,
    Hz44100 = 44100,
    Hz48000 = 48000,
    Hz88200 = 88200,
    Hz96000 = 96000
};

enum class BlockPeriod : std::uint32_t
{
    Ms05 = 5,
    Ms10 = 10,
    Ms20 = 20,
    Ms40 = 40
};

template <SampleRate Rate, BlockPeriod Period> constexpr std::size_t calc_block_frames() noexcept
{
    constexpr auto rate = static_cast<std::uint32_t>(Rate);
    constexpr auto period = static_cast<std::uint32_t>(Period);
    static_assert(rate > 0 && rate <= 96000, "Sample rate must be positive and at most 96 kHz");
    static_assert(period == 5 || period == 10 || period == 20 || period == 40, "Unsupported block period");
    constexpr auto product = static_cast<std::uint64_t>(rate) * period;
    static_assert(product % 1000 == 0, "Sample rate and block period must produce an integer frame count");
    return static_cast<std::size_t>(product / 1000);
}

// Shared by all engine specializations and translation units.
std::uint64_t get_next_identifier();

enum class ErrorCode
{
    InvalidArgument,
    UnknownNode,
    UnknownConnection,
    InvalidPort,
    InvalidNodeType,
    ChannelMismatch,
    DuplicateConnection,
    InputAlreadyConnected,
    Cycle,
    NotPrepared,
    BufferFull
};

// Control operations can allocate and throw. Engine::process() reports readiness with bool.
class GraphError : public std::runtime_error
{
  public:
    GraphError(ErrorCode code, const char *message) : std::runtime_error(message), code_(code)
    {
    }
    ErrorCode code() const noexcept
    {
        return code_;
    }

  private:
    ErrorCode code_;
};

// IDs are never reused, including across Engine instances in this process.
struct NodeId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept
    {
        return value != 0;
    }
    bool operator==(NodeId other) const noexcept
    {
        return value == other.value;
    }
    bool operator!=(NodeId other) const noexcept
    {
        return !(*this == other);
    }
    bool operator<(NodeId other) const noexcept
    {
        return value < other.value;
    }
};

struct ConnId
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept
    {
        return value != 0;
    }
    bool operator==(ConnId other) const noexcept
    {
        return value == other.value;
    }
    bool operator!=(ConnId other) const noexcept
    {
        return !(*this == other);
    }
    bool operator<(ConnId other) const noexcept
    {
        return value < other.value;
    }
};

struct IPort
{
    NodeId node;
    std::size_t index = 0;
};

struct OPort
{
    NodeId node;
    std::size_t index = 0;
};

enum class IMode
{
    Single,
    Mix // Engine sums all connections into this input.
};

struct ISpec
{
    std::size_t channels;
    IMode mode = IMode::Single;
};

struct OSpec
{
    std::size_t channels;
};

// Rows are destination channels; columns are source channels.
class ChMatrix
{
  public:
    ChMatrix() = default; // Empty means identity, requiring equal channel counts.
    ChMatrix(std::size_t output_channels, std::size_t input_channels, std::vector<float> coefficients);
    static ChMatrix identity(std::size_t channels);
    bool empty() const noexcept
    {
        return coefficients_.empty();
    }
    std::size_t input_channels() const noexcept
    {
        return input_channels_;
    }
    std::size_t output_channels() const noexcept
    {
        return output_channels_;
    }
    const std::vector<float> &coefficients() const noexcept
    {
        return coefficients_;
    }

  private:
    std::size_t input_channels_ = 0;
    std::size_t output_channels_ = 0;
    std::vector<float> coefficients_;
};

struct ConnOpts
{
    float gain = 1.0f; // Linear gain; float mixing is not clipped.
    bool muted = false;
    ChMatrix matrix;
};

// One connection throughout editing, preparation and processing.
struct Connection
{
    Connection(OPort from, IPort to, ConnOpts options = {}) : from(from), to(to), options(std::move(options))
    {
    }
    OPort from;
    IPort to;
    ConnOpts options;
    const OBlock *source_ = nullptr;
    OBlock destination_;
};

struct ProcContext
{
    std::uint64_t frame_position;
    View<const IBlock> inputs;
    View<const OBlock> outputs;
};

// Execution unit with immutable input and output layouts.
template <SampleRate Rate, BlockPeriod Period> class Node
{
    static_assert(calc_block_frames<Rate, Period>() > 0, "Node needs a nonzero block size");

  public:
    virtual ~Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    virtual View<const ISpec> inputs() const noexcept = 0;
    virtual View<const OSpec> outputs() const noexcept = 0;
    // Engine binds the views; processing receives them through ProcessContext.
    virtual View<IBlock> iblocks() noexcept = 0;
    virtual View<OBlock> oblocks() noexcept = 0;
    // Non-realtime diagnostics: write the concrete type and its current properties, without a trailing newline.
    // Serialize with processing and control operations, as with all other node access.
    virtual void attributes(std::ostream &stream) const = 0;
    virtual void prepare()
    {
    }
    // Fill every output sample in the current processing span.
    virtual void process(const ProcContext &context) noexcept = 0;
    virtual void reset() noexcept
    {
    }

  protected:
    Node() = default;
    void validate_ports() const
    {
        for (const auto &port : inputs())
        {
            if (port.channels == 0)
            {
                throw GraphError(ErrorCode::InvalidArgument, "Invalid input port specification");
            }
        }
        for (const auto &port : outputs())
        {
            if (port.channels == 0)
            {
                throw GraphError(ErrorCode::InvalidArgument, "Invalid output port specification");
            }
        }
    }
};

// Prints subclass properties and the existing port specifications. Does not expose buffer bindings.
template <SampleRate Rate, BlockPeriod Period>
std::ostream &operator<<(std::ostream &stream, const Node<Rate, Period> &node)
{
    node.attributes(stream);
    const auto inputs = node.inputs();
    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        stream << "\n  IPort[" << i << "] channels=" << inputs[i].channels
               << " mode=" << (inputs[i].mode == IMode::Mix ? "Mix" : "Single");
    }
    const auto outputs = node.outputs();
    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        stream << "\n  OPort[" << i << "] channels=" << outputs[i].channels;
    }
    return stream;
}

// Port counts are compile-time constants; specifications and views never allocate.
template <std::size_t I, std::size_t O, SampleRate Rate, BlockPeriod Period> class NodeT : public Node<Rate, Period>
{
    static_assert(I + O > 0, "Node needs at least one port");

  public:
    NodeT(std::array<ISpec, I> inputs, std::array<OSpec, O> outputs) : ispecs_(inputs), ospecs_(outputs)
    {
        this->validate_ports();
    }
    View<const ISpec> inputs() const noexcept final
    {
        return {ispecs_.data(), I};
    }
    View<const OSpec> outputs() const noexcept final
    {
        return {ospecs_.data(), O};
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

    std::array<ISpec, I> ispecs_;
    std::array<OSpec, O> ospecs_;
    std::array<IBlock, I> iblocks_;
    std::array<OBlock, O> oblocks_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_GRAPH_H
