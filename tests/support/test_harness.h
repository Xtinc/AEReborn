#ifndef AEREBORN_TESTS_TEST_HARNESS_H
#define AEREBORN_TESTS_TEST_HARNESS_H

#include "test_common.h"
#include "core/retcode.h"
#include <utility>

namespace aereborn
{
namespace test
{

template <class Fn> void expect_error(RetCode::Code expected, Fn &&fn)
{
    try
    {
        std::forward<Fn>(fn)();
    }
    catch (const Error &error)
    {
        check(error.code() == expected, error.what());
        return;
    }
    check(false, "Expected aereborn::Error was not thrown");
}

template <class Exception, class Operation> void expect_exception(Operation operation)
{
    try
    {
        operation();
    }
    catch (const Exception &)
    {
        return;
    }
    check(false, "Expected exception was not thrown");
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_TEST_HARNESS_H
