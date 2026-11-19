#ifndef AEREBORN_CORE_RUNTIME_H
#define AEREBORN_CORE_RUNTIME_H

#include <functional>
#include <memory>

namespace aereborn
{

extern thread_local const void *engine_execution_context;

class EngineExecutionScope
{
  public:
    explicit EngineExecutionScope(const void *context) noexcept : previous_(engine_execution_context)
    {
        engine_execution_context = context;
    }
    ~EngineExecutionScope() { engine_execution_context = previous_; }

  private:
    const void *previous_;
};

class ClockDriver;
std::unique_ptr<ClockDriver> make_timer_driver();

class RuntimeTask
{
  public:
    virtual ~RuntimeTask() = default;
};

std::unique_ptr<RuntimeTask> start_background_task(std::function<void()> run);

} // namespace aereborn

#endif // AEREBORN_CORE_RUNTIME_H
