#ifndef AEREBORN_BACKEND_USR_PLAYBACK_H
#define AEREBORN_BACKEND_USR_PLAYBACK_H

#include "backend.h"
#include <functional>

namespace aereborn
{

/// @brief Deliver fixed interleaved PCM batches on a worker thread as graph data becomes available.
/// @note The callback borrows the block for the duration of the call and must return without throwing.
/// @note Serialize control operations outside the callback. Stop waits for the callback and drains queued audio.
class UsrPlayback final : public ActiveBackend
{
  public:
    /// @copydoc Backend::type_name
    const char *type_name() const noexcept override { return "UsrPlayback"; }

    using Callback = std::function<void(IBBlock)>;

    /// @throws Error RetCode::EPARAM if spec is not valid interleaved PCM or callback is empty.
    UsrPlayback(PortSpec spec, Callback callback);
    ~UsrPlayback() override { unbind(); }

    using ActiveBackend::bind;
    void bind(Capture &) = delete;

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds target_period) override;
    RetCode on_write(IBBlock input) noexcept override;

    const PortSpec spec_;
    const Callback callback_;
};

} // namespace aereborn

#endif
