#ifndef AEREBORN_APP_GRAPH_SERVICE_H
#define AEREBORN_APP_GRAPH_SERVICE_H

#include "core/engine.h"
#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace aereborn
{

class ActiveBackend;
class Backend;
class CommandQueue;

/// @brief Own the graph, its public handles, and its WAV backends on the control thread.
class GraphService
{
  public:
    GraphService(CommandQueue &commands, BlockPeriod period);
    /// @brief Unbind backends and stop the engine on the control thread.
    ~GraphService();

    NodeId create_iwav(const std::string &path, int repeat, std::chrono::microseconds io_period);
    NodeId create_owav(const std::string &path, PortSpec spec, std::chrono::seconds segment_duration,
                       std::chrono::microseconds io_period);
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

    std::string dump_json();

  private:
    struct NodeBinding
    {
        std::unique_ptr<Backend> backend;
        bool paused = false;
    };
    using NodeMap = std::map<NodeId, NodeBinding>;

    NodeMap::iterator add_node(std::unique_ptr<Node> node);
    template <class Endpoint>
    NodeId add_endpoint(std::unique_ptr<Endpoint> endpoint, std::unique_ptr<ActiveBackend> backend);
    NodeMap::iterator find_node(NodeId node);
    Backend *backend_of(NodeId node) noexcept;
    void disconnect_input(NodeId to, const NodeId *from);

    CommandQueue &commands_;
    Engine engine_;
    NodeMap nodes_;
    std::map<NodeId, Connection> links_; // Keyed by destination input port.
};

} // namespace aereborn

#endif // AEREBORN_APP_GRAPH_SERVICE_H
