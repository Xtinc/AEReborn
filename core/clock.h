#ifndef AEREBORN_CORE_CLOCK_H
#define AEREBORN_CORE_CLOCK_H

#include "types.h"

namespace aereborn
{

// A driver invokes one tick at a time. stop() is idempotent and safe before start()
// or after a failed or throwing start(). It prevents new ticks and waits for any in-flight tick.
class ClockDriver
{
  public:
    using Tick = void (*)(void *);
    virtual ~ClockDriver() = default;
    virtual RetCode start(BlockPeriod period, Tick tick, void *context) = 0;
    virtual void stop() noexcept = 0;
};

} // namespace aereborn

#endif // AEREBORN_CORE_CLOCK_H
