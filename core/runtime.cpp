#include "clock.h"
#include "runtime.h"

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <thread>

namespace aereborn
{

namespace
{
// Scheduling and timer allocation are outside the Engine tick.
class TimerDriver final : public ClockDriver
{
  public:
    ~TimerDriver() override { stop(); }

    RetCode start(BlockPeriod period, Tick tick, void *context) override
    {
        period_ = std::chrono::milliseconds(static_cast<std::uint32_t>(period));
        deadline_ = std::chrono::steady_clock::now() + period_;
        schedule(tick, context);
        thread_ = std::thread([this] { context_.run(); });
        return {};
    }
    void stop() noexcept override
    {
        if (thread_.joinable())
        {
            context_.stop();
            thread_.join();
        }
    }

  private:
    void schedule(Tick tick, void *context)
    {
        timer_.expires_at(deadline_);
        timer_.async_wait([this, tick, context](const asio::error_code &error) {
            if (error || context_.stopped())
            {
                return;
            }
            tick(context);
            if (!context_.stopped())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto lateness = now - deadline_;
                deadline_ += period_ * (lateness >= period_ ? lateness / period_ + 1 : 1);
                schedule(tick, context);
            }
        });
    }

    asio::io_context context_;
    asio::steady_timer timer_{context_};
    std::chrono::steady_clock::duration period_{};
    std::chrono::steady_clock::time_point deadline_;
    std::thread thread_;
};
} // namespace

std::unique_ptr<ClockDriver> make_timer_driver()
{
    return std::make_unique<TimerDriver>();
}

} // namespace aereborn
