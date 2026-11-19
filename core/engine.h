#ifndef AEREBORN_CORE_ENGINE_H
#define AEREBORN_CORE_ENGINE_H

#include "graph.h"
#include <functional>
#include <memory>
#include <utility>

namespace aereborn
{

// A driver invokes one tick at a time. stop() must wait for any in-flight tick.
class ClockDriver
{
  public:
    using Tick = void (*)(void *);
    virtual ~ClockDriver() = default;
    virtual RetCode start(EngineSpec spec, Tick tick, void *context) = 0;
    virtual void stop() noexcept = 0;
};

using PlanRevision = std::uint64_t;
using ObserverId = std::uint64_t;

struct ObserverOptions
{
    std::size_t frames;
    std::size_t queue_blocks = 8;
};

struct Observation
{
    IBlock audio;
    std::uint64_t frame_position;
    std::uint64_t dropped_frames;
};

using ObserverCallback = std::function<void(const Observation &)>;

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
    // Prepare and submit draft topology. A running clock applies it at a block boundary.
    Result<PlanRevision> prepare();
    RetCode start(std::unique_ptr<ClockDriver> driver = {});
    RetCode stop() noexcept;
    RetCode reset() noexcept;
    // Audio views are borrowed for the duration of the callback.
    Result<ObserverId> subscribe(OPort port, ObserverOptions options, ObserverCallback callback);
    RetCode unsubscribe(ObserverId observer) noexcept;
    EngineSpec spec() const noexcept;
    std::uint64_t frame_position() const noexcept;
    PlanRevision applied_revision() const noexcept;

  private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace aereborn

#endif // AEREBORN_CORE_ENGINE_H
