#ifndef AEREBORN_CORE_FRING_H
#define AEREBORN_CORE_FRING_H

#include "block.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace aereborn
{
/// @brief SPSC storage for planar float frames.
/// @note Call write() from one producer and read() from one consumer.
/// @note Prepare and clear with both sides stopped. Query fill from either owning side.
class FrameRing
{
  public:
    /// @brief Allocate storage and clear the queue on success.
    void prepare(std::size_t channels, std::size_t capacity)
    {
        // Buffer's float storage limit also bounds the cursor arithmetic below.
        storage_.resize(channels, capacity);
        capacity_ = capacity;
        clear();
    }

    void clear() noexcept
    {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    std::size_t available() const noexcept
    {
        const auto read = read_.load(std::memory_order_acquire);
        return queued(write_.load(std::memory_order_acquire), read);
    }

    /// @brief Sample queue occupancy from outside the producer and consumer.
    /// @return False if the independently sampled cursors yield an out-of-range result.
    /// @pre Serialize with prepare().
    bool estimate(std::size_t &frames) const noexcept
    {
        frames = available();
        return frames <= capacity_;
    }

    /// @pre The ring is prepared and the input matches its layout.
    std::size_t write(IFBlock input) noexcept
    {
        assert(capacity_ && input.valid() && input.channels() == storage_.block(0).channels());
        const auto write = write_.load(std::memory_order_relaxed);
        const auto frames = std::min(input.frames(), capacity_ - queued(write, read_.load(std::memory_order_acquire)));
        if (frames)
        {
            copy(input, storage_.block(capacity_), write, frames, true);
            write_.store(advance(write, frames), std::memory_order_release);
        }
        return frames;
    }

    /// @pre The ring is prepared and the output matches its layout.
    std::size_t read(OFBlock output) noexcept
    {
        assert(capacity_ && output.valid() && output.channels() == storage_.block(0).channels());
        const auto read = read_.load(std::memory_order_relaxed);
        const auto frames = std::min(output.frames(), queued(write_.load(std::memory_order_acquire), read));
        if (frames)
        {
            copy(storage_.block(capacity_), output, read, frames, false);
            read_.store(advance(read, frames), std::memory_order_release);
        }
        return frames;
    }

  private:
    void copy(IFBlock input, OFBlock output, std::size_t cursor, std::size_t frames, bool writing) noexcept
    {
        // Cursors wrap at twice the capacity.
        const auto offset = cursor < capacity_ ? cursor : cursor - capacity_;
        const auto first = std::min(frames, capacity_ - offset);
        const auto second = frames - first;
        const auto src_first = writing ? std::size_t{0} : offset;
        const auto dst_first = writing ? offset : std::size_t{0};
        const auto src_second = writing ? first : std::size_t{0};
        const auto dst_second = writing ? std::size_t{0} : first;
        for (std::size_t ch = 0; ch < input.channels(); ++ch)
        {
            const auto *src = input.channel(ch);
            auto *dst = output.channel(ch);
            std::memcpy(dst + dst_first, src + src_first, first * sizeof(float));
            if (second)
                std::memcpy(dst + dst_second, src + src_second, second * sizeof(float));
        }
    }

    std::size_t queued(std::size_t write, std::size_t read) const noexcept
    {
        return write >= read ? write - read : 2 * capacity_ + write - read;
    }

    std::size_t advance(std::size_t cursor, std::size_t frames) const noexcept
    {
        const auto next = cursor + frames;
        return next < 2 * capacity_ ? next : next - 2 * capacity_;
    }

    Buffer storage_;
    std::size_t capacity_ = 0;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
};

} // namespace aereborn

#endif
