#ifndef AEREBORN_BACKEND_USR_CAPTURE_H
#define AEREBORN_BACKEND_USR_CAPTURE_H

#include "backend.h"
#include <functional>

namespace aereborn
{

/// @brief Request fixed interleaved PCM batches on a worker thread as queue space becomes available.
/// @note The callback borrows the writable block for the duration of the call and must return without throwing.
/// @note Serialize control operations outside the callback. Stop waits for the callback and joins the worker.
class UsrCapture final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "UsrCapture"; }

    using Callback = std::function<void(OBBlock)>;

    /// @throws Error RetCode::EPARAM if spec is not valid interleaved PCM or callback is empty.
    UsrCapture(PortSpec spec, Callback callback);
    ~UsrCapture() override { unbind(); }

    using ActiveBackend::bind;
    void bind(Playback &) = delete;

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds target_period) override;
    Result<std::size_t> on_read(OBBlock output) noexcept override;

    const PortSpec spec_;
    const Callback callback_;
};

} // namespace aereborn

#endif
