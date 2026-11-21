#ifndef AEREBORN_PLUGINS_DEVICE_STREAM_H
#define AEREBORN_PLUGINS_DEVICE_STREAM_H

#include "convert.h"
#include "drift.h"
#include "io.h"
#include <memory>

namespace aereborn
{

/// @brief Prepared conversion and scratch storage shared by the two device directions.
class DeviceStreamBuffers
{
  public:
    RetCode prepare(PortSpec input, PortSpec output, std::size_t max_frames, std::size_t queue_target,
                    std::chrono::microseconds budget);
    void reset() noexcept;
    OFBlock logical_block() noexcept;
    double rate_scale() const noexcept { return 1 + clock.correction(); }
    void update(QueueStatus before, QueueStatus after, std::size_t device_frames) noexcept;

    std::unique_ptr<Convert> convert;
    DriftController clock;

  private:
    PortSpec logical_;
    PortSpec device_;
    std::size_t device_capacity_ = 0;
    std::size_t logical_frames_ = 0;
    Buffer scratch_;
};

/// @brief Convert device capture packets and enqueue logical frames on the calling I/O thread.
/// @note The node must outlive this stream. Serialize all calls with endpoint device control.
class CaptureStream
{
  public:
    /// @brief Attach conversion to an existing capture node.
    explicit CaptureStream(Capture &node) : node_(node) {}
    /// @brief Prepare an interleaved device format and maximum packet size after the node has been prepared.
    /// @note Successful preparation resets conversion and drift state for the next start.
    RetCode prepare(PortSpec device, std::size_t max_frames);
    /// @brief Reset conversion and drift, keeping the prepared format, limits and storage.
    /// @pre Stop device I/O and take the endpoint offline before resetting for a same-device restart.
    void reset() noexcept { buffers_.reset(); }
    /// @brief Convert one valid range. Return the number of logical frames enqueued.
    /// @note Valid input received while the endpoint is offline is discarded without advancing conversion.
    Result<std::size_t> transfer(IBBlock input) noexcept;
    /// @brief Return the current fractional clock correction.
    double correction() const noexcept { return buffers_.clock.correction(); }

  private:
    Capture &node_;
    DeviceStreamBuffers buffers_;
};

/// @brief Read logical audio and fill a device playback buffer on the calling I/O thread.
/// @note The node must outlive this stream. Serialize all calls with endpoint device control.
class PlaybackStream
{
  public:
    /// @brief Attach conversion to an existing playback node.
    explicit PlaybackStream(Playback &node) : node_(node) {}
    /// @brief Prepare an interleaved device format and maximum request size after the node has been prepared.
    /// @note Successful preparation resets conversion and drift state for the next start.
    RetCode prepare(PortSpec device, std::size_t max_frames);
    /// @copydoc CaptureStream::reset
    void reset() noexcept { buffers_.reset(); }
    /// @brief Fill one device range. Return frames generated from live data, excluding silence padding.
    Result<std::size_t> transfer(OBBlock output) noexcept;
    /// @brief Return the current fractional clock correction.
    double correction() const noexcept { return buffers_.clock.correction(); }

  private:
    Playback &node_;
    DeviceStreamBuffers buffers_;
};

} // namespace aereborn

#endif
