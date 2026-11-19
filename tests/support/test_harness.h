#ifndef AEREBORN_TESTS_TEST_HARNESS_H
#define AEREBORN_TESTS_TEST_HARNESS_H

#include "core/retcode.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

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

inline void checked(RetCode status)
{
    check(static_cast<bool>(status), status.message());
}

template <class T> T checked(Result<T> result)
{
    checked(result.ret_code());
    return std::move(result).value();
}

inline void expect_error(ErrorCode expected, RetCode code)
{
    check(code.code() == expected, code.message());
}

template <class T> void expect_error(ErrorCode expected, const Result<T> &result)
{
    expect_error(expected, result.ret_code());
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
