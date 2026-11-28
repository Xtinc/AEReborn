#ifndef AEREBORN_APP_DEVICE_SERVICE_H
#define AEREBORN_APP_DEVICE_SERVICE_H

#include "core/command_queue.h"
#include <map>

namespace aereborn
{

class GraphService;
struct DeviceStatus;
struct DevConfig;
enum class DevFlags : std::uint32_t;

/// @brief Manage hardware binding recovery on the graph's control thread.
/// @note Destroy the graph before the service so all bindings can unregister.
class DeviceService
{
  public:
    DeviceService(CommandQueue &commands, GraphService &graph);
    ~DeviceService();

    NodeId create_idev(const DevConfig &config, DevFlags flags);
    NodeId create_odev(const DevConfig &config, DevFlags flags);

    /// @brief Query hardware status or return an empty status for other nodes.
    DeviceStatus status_of(NodeId node) const noexcept;
    /// @brief Cancel the shared timer before graph teardown.
    void cancel_poll() noexcept;

  private:
    struct DeviceBinding;
    template <class Endpoint> NodeId create_device(const DevConfig &config, DevFlags flags);

    void on_device_started();
    void on_device_stopped() noexcept;
    void unregister(NodeId id) noexcept;
    void poll_devices() noexcept;

    CommandQueue &commands_;
    GraphService &graph_;
    CommandQueue::TimerId poll_timer_ = 0;
    std::map<NodeId, DeviceBinding *> devices_;
};

} // namespace aereborn

#endif // AEREBORN_APP_DEVICE_SERVICE_H
