#ifndef AEREBORN_PLUGINS_VIRTUAL_H
#define AEREBORN_PLUGINS_VIRTUAL_H

#include "core/types.h"
#include <vector>

namespace aereborn
{

// Graph-external PCM generator. Feed pcm() to IAdapter after each generate().
class VirtualSource
{
  public:
    VirtualSource(PCMSpec spec, BlockPeriod period, double frequency_hz = 440.0, float amplitude = 0.25f) noexcept;
    RetCode prepare() noexcept;
    RetCode generate() noexcept;
    void reset() noexcept;
    std::size_t pcm_frames() const noexcept;
    // Borrowed until the next generate(), reset() or destruction.
    View<const std::uint8_t> pcm() const noexcept;

  private:
    const PCMSpec spec_;
    const BlockPeriod period_;
    const double frequency_hz_;
    const float amplitude_;
    std::vector<std::uint8_t> pcm_;
    std::uint64_t frame_position_ = 0;
    bool generated_ = false;
};

// Graph-external PCM receiver. Copies a complete OAdapter transfer for inspection.
class VirtualSink
{
  public:
    VirtualSink(PCMSpec spec, BlockPeriod period) noexcept;
    RetCode prepare() noexcept;
    RetCode receive(View<const std::uint8_t> pcm) noexcept;
    void reset() noexcept;
    View<const std::uint8_t> pcm() const noexcept;
    bool matches(View<const std::uint8_t> expected) const noexcept;

  private:
    const PCMSpec spec_;
    const BlockPeriod period_;
    std::vector<std::uint8_t> pcm_;
    bool received_ = false;
};

} // namespace aereborn

#endif // AEREBORN_PLUGINS_VIRTUAL_H
