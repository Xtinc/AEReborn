#ifndef AEREBORN_TESTS_TEST_BACKEND_H
#define AEREBORN_TESTS_TEST_BACKEND_H

#include "backend/backend.h"
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace aereborn
{
namespace test
{

struct BackendCalls
{
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
    TestBackend(PortSpec spec, std::size_t frames, BackendCalls *calls = nullptr, bool async = true,
                bool fail_on_timeout = true)
        : ActiveBackend(async, fail_on_timeout), spec_(spec), frames_(frames), calls_(calls)
    {
    }

    ~TestBackend() override { unbind(); }

    Result<PortSpec> on_open(bool, PortSpec, std::chrono::microseconds) override
    {
        if (calls_)
            ++calls_->opens;
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

    bool wait_for_io()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(3), [&] { return entered_ != 0; });
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

    RetCode start_result;
    RetCode io_result;
    std::chrono::microseconds cadence{0};
    bool gated = false;
    bool block = false;
    bool empty_capture = false;
    void (*measure_io)(bool) = nullptr;
    IBBlock input;
    OBBlock output;

    Result<std::size_t> on_read(OBBlock output) noexcept override
    {
        const auto status = pace();
        if (!status)
            return status;
        if (empty_capture)
            return std::size_t{0};
        const auto destination = output.byte_view();
        if (input.frames())
        {
            const auto source = input.byte_view();
            if (source.size() <= destination.size())
                std::memcpy(destination.data(), source.data(), source.size());
            return input.frames();
        }
        if (!destination.empty())
            std::memset(destination.data(), 0, destination.size());
        return output.frames();
    }

    Result<std::size_t> on_wait_writable() noexcept override
    {
        const auto status = pace();
        if (!status)
            return status;
        return device_max_frames();
    }

    RetCode on_write(IBBlock input) noexcept override
    {
        const auto source = input.byte_view();
        const auto destination = output.byte_view();
        if (!source.empty() && source.size() <= destination.size())
            std::memcpy(destination.data(), source.data(), source.size());
        return {};
    }

  private:
    RetCode pace() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto batch = ++entered_;
        changed_.notify_all();
        if (gated)
            changed_.wait(lock, [&] { return batch <= released_ || cancelled_; });
        else if (block)
            changed_.wait(lock, [&] { return cancelled_; });
        else
            changed_.wait_for(lock, cadence, [&] { return cancelled_; });
        return cancelled_ ? RetCode{RetCode::CANCELLED} : io_result;
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
