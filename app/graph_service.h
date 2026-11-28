#ifndef AEREBORN_APP_GRAPH_SERVICE_H
#define AEREBORN_APP_GRAPH_SERVICE_H

#include "core/engine.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace aereborn
{

class Backend;
class CommandQueue;
struct DeviceStatus;

struct NodeBinding
{
    virtual ~NodeBinding() = default;
    virtual const Backend *backend() const noexcept = 0;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;
    virtual void unbind() noexcept = 0;
};

/// @brief Own the graph, its public handles, and its backends on the control thread.
class GraphService
{
  public:
    using DeviceStatusFn = std::function<DeviceStatus(NodeId)>;
    using BindingMatcher = bool (*)(const NodeBinding &);

    GraphService(CommandQueue &commands, BlockPeriod period);
    /// @brief Unbind backends and stop the engine on the control thread.
    ~GraphService();

    NodeId add_node(std::unique_ptr<Node> node);
    /// @brief Prepare, bind, and publish an endpoint on the control thread.
    /// @param bind Synchronous operation borrowing the supplied endpoint and binding.
    NodeId add_endpoint(std::unique_ptr<Node> endpoint, std::shared_ptr<NodeBinding> binding,
                        const std::function<void()> &bind);
    NodeId create_mixer(BandWidth bandwidth, std::size_t channels, std::size_t inputs);
    NodeId create_matrix(BandWidth bandwidth, std::size_t input_channels, std::size_t output_channels,
                         View<const float> coefficients, float gain);
    NodeId create_convert(PortSpec input, PortSpec output);
    PortSpec port_spec(NodeId port, bool output);

    void connect(NodeId from, NodeId to);
    void disconnect(NodeId from, NodeId to);
    void disconnect(NodeId to);
    void remove(NodeId node);

    void commit();
    void start();
    void stop() noexcept;

    void set_paused(NodeId node, bool paused);
    void set_gain(NodeId matrix, float gain);
    void set_muted(NodeId mixer_port, bool muted);
    void set_high_priority(NodeId mixer_port, bool high);

    /// @brief Match an endpoint under the table lock and retain it for a caller-thread transfer.
    /// @pre matches is a non-throwing predicate.
    Result<std::shared_ptr<NodeBinding>> find_binding(NodeId node, BindingMatcher matches) noexcept;

    std::string dump_json(const DeviceStatusFn &device_status = {});

  private:
    struct NodeEntry
    {
        std::shared_ptr<NodeBinding> endpoint;
        bool published;
        bool paused = false; // Control thread only.
    };
    using NodeMap = std::map<NodeId, NodeEntry>;

    NodeMap::iterator insert_node(std::unique_ptr<Node> node, bool published);
    std::shared_ptr<NodeBinding> detach_node(NodeMap::iterator entry);
    NodeMap::iterator find_node(NodeId node);
    NodeBinding *find_endpoint(NodeId node) noexcept;
    void disconnect_input(NodeId to, const NodeId *from);

    CommandQueue &commands_;
    Engine engine_;
    std::mutex nodes_mutex_;
    NodeMap nodes_;
    std::map<NodeId, Connection> links_; // Keyed by destination input port.
};

} // namespace aereborn

#endif // AEREBORN_APP_GRAPH_SERVICE_H
