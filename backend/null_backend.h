#ifndef AEREBORN_BACKEND_NULL_BACKEND_H
#define AEREBORN_BACKEND_NULL_BACKEND_H

#include "backend.h"

namespace aereborn
{

/// @brief Graph-clocked source of silence or sink that discards PCM frames.
class NullBackend final : public Backend
{
  public:
    /// @brief Construct a null backend whose direction is selected at bind time.
    /// @param format Interleaved PCM format used by the device side.
    /// @throws Error If the format is not interleaved PCM.
    explicit NullBackend(SampleFormat format);
    ~NullBackend() override { unbind(); }

  private:
    Result<PortSpec> on_open(bool capture, PortSpec logical, std::chrono::microseconds period) override;
    Result<std::size_t> on_read(OBBlock output) noexcept override;
    RetCode on_write(IBBlock input) noexcept override;

    const SampleFormat format_;
};

} // namespace aereborn

#endif
