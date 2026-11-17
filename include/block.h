#ifndef AEREBORN_BLOCK_H
#define AEREBORN_BLOCK_H

#include <cassert>
#include <cstddef>
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

// Non-owning planar views. Only the first frames() samples of each channel are valid.
class IBlock
{
  public:
    IBlock() noexcept = default;
    IBlock(const float *data, std::size_t channels, std::size_t frames, std::size_t stride) noexcept
        : data_(data), channels_(channels), frames_(frames), stride_(stride)
    {
    }
    const float *channel(std::size_t index) const noexcept
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

  private:
    const float *data_ = nullptr;
    std::size_t channels_ = 0;
    std::size_t frames_ = 0;
    std::size_t stride_ = 0;
};

class OBlock
{
  public:
    OBlock() noexcept = default;
    OBlock(float *data, std::size_t channels, std::size_t frames, std::size_t stride) noexcept
        : data_(data), channels_(channels), frames_(frames), stride_(stride)
    {
    }
    float *channel(std::size_t index) const noexcept
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
    void clear() const noexcept;
    operator IBlock() const noexcept
    {
        return {data_, channels_, frames_, stride_};
    }

  private:
    float *data_ = nullptr;
    std::size_t channels_ = 0;
    std::size_t frames_ = 0;
    std::size_t stride_ = 0;
};

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

#endif // AEREBORN_BLOCK_H