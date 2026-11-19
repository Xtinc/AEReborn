#include "clock.h"
#include "runtime.h"

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <thread>

namespace aereborn
{

thread_local const void *engine_execution_context = nullptr;

namespace
{
// Scheduling and timer allocation are outside the Engine tick.
class PeriodicTask final : public RuntimeTask
{
  public:
    PeriodicTask(std::chrono::steady_clock::duration period, std::function<void()> run)
        : period_(period),
          deadline_(std::chrono::steady_clock::now() + period_), run_(std::move(run))
    {
        schedule();
        thread_ = std::thread([this] { context_.run(); });
    }
    ~PeriodicTask() override
    {
        context_.stop();
        thread_.join();
    }

  private:
    void schedule()
    {
        timer_.expires_at(deadline_);
        timer_.async_wait([this](const asio::error_code &error) {
            if (error || context_.stopped())
            {
                return;
            }
            run_();
            if (!context_.stopped())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto lateness = now - deadline_;
                deadline_ += period_ * (lateness >= period_ ? lateness / period_ + 1 : 1);
                schedule();
            }
        });
    }

    asio::io_context context_;
    asio::steady_timer timer_{context_};
    const std::chrono::steady_clock::duration period_;
    std::chrono::steady_clock::time_point deadline_;
    std::function<void()> run_;
    std::thread thread_;
};

class TimerDriver final : public ClockDriver
{
  public:
    RetCode start(EngineSpec spec, Tick tick, void *context) override
    {
        const auto period = std::chrono::milliseconds(static_cast<std::uint32_t>(spec.block_period()));
        task_ = std::make_unique<PeriodicTask>(period, [tick, context] { tick(context); });
        return {};
    }
    void stop() noexcept override
    {
        task_.reset();
    }

  private:
    std::unique_ptr<RuntimeTask> task_;
};
} // namespace

std::unique_ptr<ClockDriver> make_timer_driver()
{
    return std::make_unique<TimerDriver>();
}

std::unique_ptr<RuntimeTask> start_background_task(std::function<void()> run)
{
    const std::chrono::duration<double> period(1.0 / 30.0);
    return std::make_unique<PeriodicTask>(std::chrono::duration_cast<std::chrono::steady_clock::duration>(period),
                                          std::move(run));
}

} // namespace aereborn
