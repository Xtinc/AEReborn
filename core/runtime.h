#ifndef AEREBORN_CORE_RUNTIME_H
#define AEREBORN_CORE_RUNTIME_H

#include <memory>

namespace aereborn
{

class ClockDriver;
std::unique_ptr<ClockDriver> make_timer_driver();

} // namespace aereborn

#endif // AEREBORN_CORE_RUNTIME_H
