#ifndef AEREBORN_TESTS_TEST_BACKEND_H
#define AEREBORN_TESTS_TEST_BACKEND_H

#include "backend/backend.h"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace aereborn
{
namespace test
{

struct BackendCalls
{
    SampleRate rate{};
    std::size_t channels = 0;
    std::chrono::microseconds period{};
    unsigned opens = 0;
    unsigned starts = 0;
    unsigned stops = 0;
    unsigned cancels = 0;
    std::thread::id start_thread;
    std::thread::id stop_thread;
    unsigned closes = 0;
};

/// @brief Scriptable device fixture. Configure its behavior while stopped.
class TestBackend final : public ActiveBackend
{
  public:
    TestBackend(PortSpec spec, std::size_t frames, BackendCalls *calls = nullptr, bool async = true)
        : ActiveBackend(async), spec_(spec), frames_(frames), calls_(calls)
    {
    }

    ~TestBackend() override { unbind(); }

    Result<PortSpec> on_open(bool, PortSpec logical, std::chrono::microseconds period) override
    {
        if (calls_)
        {
            calls_->rate = logical.sample_rate;
            calls_->channels = logical.channels;
            calls_->period = period;
            ++calls_->opens;
        }
        if (throw_on_open)
            throw std::bad_alloc();
        if (!open_result)
            return open_result;
        set_device_max_frames(frames_);
        return spec_;
    }

    void on_close() noexcept override
    {
        if (calls_)
            ++calls_->closes;
    }

    RetCode on_start() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (calls_)
        {
            ++calls_->starts;
            calls_->start_thread = std::this_thread::get_id();
        }
        if (!start_result)
            return start_result;
        cancelled_ = false;
        entered_ = 0;
        released_ = 0;
        finished_ = false;
        if (measure_io)
            measure_io(true);
        return {};
    }

    void on_stop() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (calls_)
        {
            ++calls_->stops;
            calls_->stop_thread = std::this_thread::get_id();
        }
        if (measure_io)
            measure_io(false);
        finished_ = true;
        changed_.notify_all();
    }

    void on_cancel() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (calls_)
            ++calls_->cancels;
        cancelled_ = true;
        changed_.notify_all();
    }

    bool wait_for_io(unsigned calls = 1)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_ >= calls; });
    }

    /// @brief Release one batch on a gated async worker and wait for the next gate.
    /// @return True when the next gate is reached, false if the worker exits or the wait expires.
    /// @note At the gate the controller can inspect the completed transfer and update the next batch.
    bool run_batch()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto next = ++released_ + 1;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_ >= next || finished_; });
        return entered_ >= next;
    }

    RetCode open_result;
    bool throw_on_open = false;
    RetCode start_result;
    RetCode io_result;
    std::chrono::microseconds cadence{0};
    unsigned slow_batch = 0;
    std::chrono::microseconds slow_delay{0};
    bool gated = false;
    bool block = false;
    void (*measure_io)(bool) = nullptr;
    bool pace_write = false;

    Result<std::size_t> on_read(OBBlock output) noexcept override
    {
        const auto batch = pace();
        if (!batch)
            return batch.ret_code();
        if (batch.value() >= eof_batch)
            return ErrorCode::EndOfStream;
        if (batch.value() < first_data_batch || batch.value() > last_data_batch)
            return std::size_t{0};
        const auto frames = read_frames == std::numeric_limits<std::size_t>::max()
                                ? (input.frames() ? input.frames() : output.frames())
                                : read_frames;
        if (!frames || frames > output.frames())
            return frames;
        const auto width = output.spec().channels * sample_bytes(output.spec().format);
        auto *bytes = output.byte_view().data();
        const auto copied = std::min(frames, input.frames()) * width;
        if (copied)
            std::memcpy(bytes, input.byte_view().data(), copied);
        std::memset(bytes + copied, 0, frames * width - copied);
        return frames;
    }

    IBBlock input;
    std::size_t read_frames = std::numeric_limits<std::size_t>::max();
    unsigned first_data_batch = 1;
    unsigned eof_batch = std::numeric_limits<unsigned>::max();
    unsigned last_data_batch = std::numeric_limits<unsigned>::max();

    Result<std::size_t> on_wait_writable() noexcept override
    {
        if (!pace_write)
        {
            const auto batch = pace();
            if (!batch)
                return batch.ret_code();
        }
        return batch_frames == std::numeric_limits<std::size_t>::max() ? device_max_frames() : batch_frames;
    }

    RetCode on_write(IBBlock input) noexcept override
    {
        if (pace_write)
        {
            const auto batch = pace();
            if (!batch)
                return batch.ret_code();
        }
        written = input.frames();
        if (recorded)
        {
            const auto bytes = input.byte_view();
            if (bytes.size() > recorded->capacity() - recorded->size())
                return ErrorCode::TooLarge;
            recorded->insert(recorded->end(), bytes.begin(), bytes.end());
        }
        if (output.frames() && input.byte_view().size() <= output.byte_view().size())
            std::memcpy(output.byte_view().data(), input.byte_view().data(), input.byte_view().size());
        return write_result;
    }

    std::vector<std::uint8_t> *recorded = nullptr;
    OBBlock output;
    std::size_t batch_frames = std::numeric_limits<std::size_t>::max();
    std::size_t written = 0;
    RetCode write_result;

  private:
    Result<unsigned> pace() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto batch = ++entered_;
        changed_.notify_all();
        if (gated)
            changed_.wait(lock, [&] { return batch <= released_ || cancelled_; });
        else if (block)
            changed_.wait(lock, [&] { return cancelled_; });
        else
            changed_.wait_for(lock, batch == slow_batch ? slow_delay : cadence, [&] { return cancelled_; });
        if (cancelled_)
            return ErrorCode::Cancelled;
        if (!io_result)
            return io_result;
        return batch;
    }

    PortSpec spec_;
    std::size_t frames_;
    BackendCalls *calls_;
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned entered_ = 0;
    unsigned released_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace test
} // namespace aereborn

#endif
