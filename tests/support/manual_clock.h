#ifndef AEREBORN_TESTS_MANUAL_CLOCK_H
#define AEREBORN_TESTS_MANUAL_CLOCK_H

#include "core/engine.h"

namespace aereborn
{
namespace test
{

class ManualDriver final : public ClockDriver
{
  public:
    RetCode start(EngineSpec, Tick tick, void *context) override
    {
        tick_ = tick;
        context_ = context;
        return {};
    }
    void stop() noexcept override
    {
        tick_ = nullptr;
    }
    void step() const noexcept
    {
        tick_(context_);
    }

  private:
    Tick tick_ = nullptr;
    void *context_ = nullptr;
};

class ManualClock
{
  public:
    explicit ManualClock(Engine &engine) noexcept : engine_(engine)
    {
    }
    ~ManualClock()
    {
        stop();
    }

    RetCode start()
    {
        auto driver = std::make_unique<ManualDriver>();
        auto *active = driver.get();
        const auto code = engine_.start(std::move(driver));
        if (code)
        {
            driver_ = active;
        }
        return code;
    }

    RetCode tick()
    {
        if (!driver_)
        {
            const auto code = start();
            if (!code)
            {
                return code;
            }
        }
        driver_->step();
        return {};
    }

    RetCode stop() noexcept
    {
        const auto code = engine_.stop();
        if (code)
        {
            driver_ = nullptr;
        }
        return code;
    }

    RetCode reset() noexcept
    {
        const auto code = stop();
        return code ? engine_.reset() : code;
    }

  private:
    Engine &engine_;
    ManualDriver *driver_ = nullptr;
};

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_MANUAL_CLOCK_H
