#ifndef AEREBORN_BACKEND_PUSH_SOURCE_H
#define AEREBORN_BACKEND_PUSH_SOURCE_H

#include "backend.h"

namespace aereborn
{

/// @brief Caller-driven capture of interleaved PCM into the audio graph.
/// @note Serialize all calls, including queries and destruction, for each instance on the calling side.
/// @note Borrows the Capture. Unbind or destroy this source before removing the node or destroying Engine.
class PushSource final : public PassiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "PushSource"; }

    /// @throws Error InvalidArg if input_spec is invalid or is not interleaved PCM.
    explicit PushSource(PortSpec input_spec);
    /// @brief Prepare DSP buffers and bind to a prepared Capture node.
    /// @throws Error If already bound, the endpoint is unprepared or occupied, or format validation fails.
    void bind(Capture &endpoint);

    /// @brief Push interleaved PCM without waiting for queue space.
    /// @param input Borrowed PCM storage, valid until the call returns.
    /// @return Input frames accepted. Resume a short write at this frame offset. Zero means no frames were accepted,
    ///         including when stopped or when the queue has less space than the FIR output bound.
    /// @throws Error FormatMismatch for a different PortSpec, or InvalidArg for nonzero frames with a null pointer.
    std::size_t push(IBBlock input);

  private:
    const PortSpec input_spec_;
};

} // namespace aereborn

#endif
