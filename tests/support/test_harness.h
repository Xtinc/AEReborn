#ifndef AEREBORN_TESTS_TEST_HARNESS_H
#define AEREBORN_TESTS_TEST_HARNESS_H

#include "core/retcode.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace aereborn
{
namespace test
{

inline void check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <class Fn> void expect_error(ErrorCode expected, Fn &&fn)
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

struct TestCase
{
    const char *name;
    void (*run)();
};

inline int run_tests(int argc, char **argv, std::initializer_list<TestCase> cases)
{
    bool matched = false;
    for (const auto &test : cases)
    {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
        {
            continue;
        }
        matched = true;
        try
        {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        }
        catch (const std::exception &error)
        {
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return matched ? 0 : 1;
}

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_TEST_HARNESS_H
