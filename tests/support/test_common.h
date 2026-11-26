#ifndef AEREBORN_TESTS_TEST_COMMON_H
#define AEREBORN_TESTS_TEST_COMMON_H

#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

template <class Ready> void wait_for(Ready ready, const char *message,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + timeout;
    while (!ready())
    {
        check(steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(milliseconds(1));
    }
}

class TempWave
{
  public:
    explicit TempWave(const char *name)
        : stem_(std::string("aereborn-") + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
    {
    }
    ~TempWave()
    {
        for (const auto &path : paths_)
            std::remove(path.c_str());
    }
    TempWave(const TempWave &) = delete;
    TempWave &operator=(const TempWave &) = delete;

    std::string path(const char *suffix = ".wav")
    {
        paths_.push_back(stem_ + suffix);
        return paths_.back();
    }

  private:
    const std::string stem_;
    std::vector<std::string> paths_;
};

} // namespace test
} // namespace aereborn

#endif // AEREBORN_TESTS_TEST_COMMON_H
