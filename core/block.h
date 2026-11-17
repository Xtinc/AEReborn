#ifndef AEREBORN_CORE_BLOCK_H
#define AEREBORN_CORE_BLOCK_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace aereborn
{
// C++14 non-owning view. Fixed-size node arrays remain owned by their node.
template <class T> class View
{
  public:
    View(T *data, std::size_t size) noexcept : data_(data), size_(size)
    {
    }
    template <class U, typename std::enable_if<std::is_convertible<U (*)[], T (*)[]>::value, int>::type = 0>
    View(View<U> other) noexcept : data_(other.data()), size_(other.size())
    {
    }
    T *data() const noexcept
    {
        return data_;
    }
    std::size_t size() const noexcept
    {
        return size_;
    }
    bool empty() const noexcept
    {
        return size_ == 0;
    }
    T *begin() const noexcept
    {
        return data_;
    }
    T *end() const noexcept
    {
        return size_ == 0 ? data_ : data_ + size_;
    }
    T &operator[](std::size_t index) const noexcept
    {
        assert(index < size_);
        return data_[index];
    }

  private:
    T *data_;
    std::size_t size_;
};

// Non-owning planar views. frames() is the processing span, including any padded tail.
template <class T> class Block
{
    static_assert(std::is_same<typename std::remove_const<T>::type, float>::value, "Audio samples must be float");
    template <class> friend class Block;

  public:
    Block() noexcept = default;
    Block(T *data, std::size_t channels, std::size_t frames, std::size_t stride) noexcept
        : data_(data), channels_(channels), frames_(frames), stride_(stride)
    {
        assert(frames <= stride);
    }
    template <class U, typename std::enable_if<std::is_convertible<U *, T *>::value, int>::type = 0>
    Block(Block<U> other) noexcept
        : data_(other.data_), channels_(other.channels_), frames_(other.frames_), stride_(other.stride_)
    {
    }
    T *channel(std::size_t index) const noexcept
    {
        assert(index < channels_);
        return data_ + index * stride_;
    }
    std::size_t channels() const noexcept
    {
        return channels_;
    }
    std::size_t frames() const noexcept
    {
        return frames_;
    }
    void set_frames(std::size_t frames) noexcept
    {
        assert(frames <= stride_);
        frames_ = frames;
    }

    template <class U = T, typename std::enable_if<!std::is_const<U>::value, int>::type = 0> void clear() const noexcept
    {
        for (std::size_t ch = 0; ch < channels_; ++ch)
        {
            std::fill_n(channel(ch), frames_, 0.0f);
        }
    }

  private:
    T *data_ = nullptr;
    std::size_t channels_ = 0;
    std::size_t frames_ = 0;
    std::size_t stride_ = 0;
};

using IBlock = Block<const float>;
using OBlock = Block<float>;

// Allocation belongs to construction/preparation, never to block processing.
class Buffer
{
  public:
    Buffer() = default;
    Buffer(std::size_t channels, std::size_t capacity_frames);
    OBlock block(std::size_t frames) noexcept;
    IBlock block(std::size_t frames) const noexcept;
    std::size_t channels() const noexcept
    {
        return channels_;
    }
    std::size_t capacity_frames() const noexcept
    {
        return capacity_frames_;
    }

  private:
    std::vector<float> samples_;
    std::size_t channels_ = 0;
    std::size_t capacity_frames_ = 0;
};

} // namespace aereborn

#endif // AEREBORN_CORE_BLOCK_H
