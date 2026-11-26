#ifndef AEREBORN_BACKEND_PULL_SINK_H
#define AEREBORN_BACKEND_PULL_SINK_H

#include "backend.h"

namespace aereborn
{

/// @brief Caller-driven playback in a fixed interleaved PCM format.
/// @note Serialize all calls, including queries and destruction, on the calling side.
class PullSink final : public PassiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "PullSink"; }

    /// @throws Error RetCode::EPARAM if output_spec is invalid or is not interleaved PCM.
    explicit PullSink(PortSpec output_spec);

    /// @brief Bind to a prepared Playback endpoint and allocate conversion buffers.
    void bind(Playback &endpoint);

    /// @brief Pull available PCM without waiting for the graph to fill the queue.
    /// @param output Borrowed writable PCM storage, valid until the call returns.
    /// @return Output frames written. The unwritten suffix is left intact.
    /// @throws Error RetCode::INVFMT for a different PortSpec, or RetCode::EPARAM for a nonempty null buffer.
    std::size_t pull(OBBlock output);

  private:
    const PortSpec output_spec_;
};

} // namespace aereborn

#endif
