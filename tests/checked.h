#ifndef AEREBORN_TESTS_CHECKED_H
#define AEREBORN_TESTS_CHECKED_H

#include "core/retcode.h"
#include <stdexcept>

namespace aereborn
{
namespace test
{

inline void checked(RetCode status)
{
    if (!status)
    {
        throw std::runtime_error(status.message());
    }
}

template <class T> T checked(Result<T> result)
{
    checked(result.ret_code());
    return std::move(result).value();
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_CHECKED_H
