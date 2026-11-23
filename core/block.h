#ifndef AEREBORN_CORE_BLOCK_H
#define AEREBORN_CORE_BLOCK_H

#include "retcode.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

namespace aereborn
{
/// @brief Borrow a contiguous range of elements.
/// @tparam T Type of the elements, including their const qualification.
/// @note Keep the backing storage valid while the view is in use.
template <class T> class View
{
  public:
    /// @brief Construct an empty view with a null data pointer.
    View() noexcept = default;

    /// @brief Borrow a contiguous range.
    /// @param data Address of the range.
    /// @param size Number of elements in the range.
    /// @pre A nonempty range contains at least size valid elements.
    View(T *data, std::size_t size) noexcept : data_(data), size_(size)
    {
    }

    /// @brief Borrow the same range with a compatible element type.
    /// @param other Source view.
    template <class U, typename std::enable_if<std::is_convertible<U (*)[], T (*)[]>::value, int>::type = 0>
    View(View<U> other) noexcept : data_(other.data()), size_(other.size())
    {
    }

    /// @brief Return the stored address of the range.
    T *data() const noexcept
    {
        return data_;
    }

    /// @brief Return the number of elements.
    std::size_t size() const noexcept
    {
        return size_;
    }

    /// @brief Check whether the range is empty.
    bool empty() const noexcept
    {
        return size_ == 0;
    }

    /// @brief Return the start of the range.
    T *begin() const noexcept
    {
        return data_;
    }

    /// @brief Return the end of the range. For an empty range it equals begin().
    T *end() const noexcept
    {
        return size_ == 0 ? data_ : data_ + size_;
    }

    /// @brief Access an element by its zero-based index.
    /// @pre index is less than size().
    T &operator[](std::size_t index) const noexcept
    {
        assert(index < size_);
        return data_[index];
    }

  private:
    T *data_ = nullptr;
    std::size_t size_ = 0;
};

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "Audio samples require IEEE float32");

/// @brief Borrow a block of planar float samples.
/// @tparam T Use float for writable samples or const float for read-only samples.
/// @note The channel stride may exceed the number of frames in the view.
template <class T> class FloatBlock
{
    static_assert(std::is_same<typename std::remove_const<T>::type, float>::value, "Audio samples must be float");
    template <class> friend class FloatBlock;

  public:
    /// @brief Construct an empty float block.
    FloatBlock() noexcept = default;

    /// @brief Borrow planar samples with an explicit channel stride.
    /// @param data Address of the first channel.
    /// @param channels Number of channels.
    /// @param frames Number of frames in the processing span.
    /// @param channel_stride Distance between channel starts, in samples.
    /// @pre frames is at most channel_stride. The owner keeps the exposed samples valid.
    FloatBlock(T *data, std::size_t channels, std::size_t frames, std::size_t channel_stride) noexcept
        : data_(data), channels_(channels), frames_(frames), channel_stride_(channel_stride)
    {
        assert(frames <= channel_stride);
    }

    /// @brief Borrow the same samples with compatible access.
    /// @param other Source block.
    template <class U, typename std::enable_if<std::is_convertible<U *, T *>::value, int>::type = 0>
    FloatBlock(FloatBlock<U> other) noexcept
        : data_(other.data_), channels_(other.channels_), frames_(other.frames_), channel_stride_(other.channel_stride_)
    {
    }

    /// @brief Return a pointer to the start of a channel.
    /// @param ch Zero-based channel index.
    /// @pre ch is less than channels().
    T *channel(std::size_t ch) const noexcept
    {
        assert(ch < channels_);
        return data_ + ch * channel_stride_;
    }

    /// @brief Return the number of channels.
    std::size_t channels() const noexcept
    {
        return channels_;
    }

    /// @brief Return the number of frames in the processing span.
    std::size_t frames() const noexcept
    {
        return frames_;
    }

    /// @brief Check the view geometry. The caller owns the backing storage.
    bool valid() const noexcept
    {
        constexpr auto limit = std::numeric_limits<std::size_t>::max() / sizeof(float);
        return frames_ == 0 || (data_ && channels_ && frames_ <= channel_stride_ && frames_ <= limit &&
                               channels_ - 1 <= (limit - frames_) / channel_stride_);
    }

    /// @brief Borrow a frame range with the original channel stride.
    /// @pre The requested range lies in this block.
    FloatBlock subblock(std::size_t offset, std::size_t frames) const noexcept
    {
        assert(offset <= frames_ && frames <= frames_ - offset);
        return {data_ ? data_ + offset : nullptr, channels_, frames, channel_stride_};
    }

    /// @brief Zero the processing span of each channel in a writable block.
    template <class U = T, typename std::enable_if<!std::is_const<U>::value, int>::type = 0> void clear() const noexcept
    {
        if (!frames_)
            return;
        for (std::size_t ch = 0; ch < channels_; ++ch)
        {
            std::fill_n(channel(ch), frames_, 0.0f);
        }
    }

  private:
    T *data_ = nullptr;
    std::size_t channels_ = 0;
    std::size_t frames_ = 0;
    std::size_t channel_stride_ = 0;
};

using IFBlock = FloatBlock<const float>;
using OFBlock = FloatBlock<float>;

/// @brief Own a planar float workspace prepared for reuse during processing.
class Buffer
{
  public:
    /// @brief Set the workspace dimensions and zero its samples.
    /// @param channels Number of channels.
    /// @param capacity_frames Frame capacity per channel.
    /// @throws Error InvalidArg for a zero dimension, or TooLarge if storage would exceed its limit.
    /// @note Allocate in prepare(). Allocation failure propagates as an exception.
    void resize(std::size_t channels, std::size_t capacity_frames);

    /// @brief Borrow a writable block using the workspace capacity as the channel stride.
    /// @param frames Number of frames in the processing span.
    /// @pre frames fits the workspace capacity.
    /// @note Borrow a new block after a successful resize. Keep the backing storage alive while using a view.
    OFBlock block(std::size_t frames) noexcept;

    /// @brief Zero all samples in the workspace.
    void clear() noexcept;

  private:
    std::vector<float> samples_;
    std::size_t channels_ = 0;
    std::size_t capacity_frames_ = 0;
};

} // namespace aereborn

#endif // AEREBORN_CORE_BLOCK_H
