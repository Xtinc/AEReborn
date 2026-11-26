#ifndef AEREBORN_CORE_RETCODE_H
#define AEREBORN_CORE_RETCODE_H

#include "audio_types.h"
#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace aereborn
{

/// @brief Exception for AEReborn control-side validation and operation failures.
class Error : public std::runtime_error
{
  public:
    explicit Error(RetCode code) : std::runtime_error(code.what()), code_(code) {}
    Error(RetCode code, const char *detail) : std::runtime_error(detail), code_(code) {}
    Error(RetCode code, const std::string &detail) : std::runtime_error(detail), code_(code) {}

    RetCode::Code code() const noexcept { return code_.code(); }

  private:
    RetCode code_;
};

/// @brief Throw Error if the operation did not return OK.
inline void throw_if_error(RetCode code)
{
    if (!code)
        throw Error(code);
}

/// @brief Status or value for backend operations. Check the status before reading the value.
template <class T> class Result
{
  public:
    Result(T value) noexcept(std::is_nothrow_move_constructible<T>::value) : value_(std::move(value))
    {
    }
    Result(RetCode code) noexcept(std::is_nothrow_default_constructible<T>::value) : ret_code_(code)
    {
        assert(!code);
    }
    Result(RetCode::Code code) noexcept(std::is_nothrow_default_constructible<T>::value) : Result(RetCode(code))
    {
    }
    RetCode ret_code() const noexcept
    {
        return ret_code_;
    }
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(ret_code_);
    }
    T &value() & noexcept
    {
        assert(ret_code_);
        return value_;
    }
    const T &value() const & noexcept
    {
        assert(ret_code_);
        return value_;
    }
    T value() && noexcept(std::is_nothrow_move_constructible<T>::value)
    {
        assert(ret_code_);
        return std::move(value_);
    }

    /// @brief Move out the result or throw Error on failure.
    T value_or_throw() &&
    {
        throw_if_error(ret_code_);
        return std::move(value_);
    }

  private:
    RetCode ret_code_;
    T value_{};
};

/// @brief Run a non-throwing cleanup when leaving scope unless dismissed.
template <class Cleanup> class ScopeExit
{
  public:
    explicit ScopeExit(Cleanup cleanup) noexcept(std::is_nothrow_move_constructible<Cleanup>::value)
        : cleanup_(std::move(cleanup))
    {
    }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

    ~ScopeExit()
    {
        if (active_)
            cleanup_();
    }

    void dismiss() noexcept { active_ = false; }

  private:
    Cleanup cleanup_;
    bool active_ = true;
};

} // namespace aereborn

#endif // AEREBORN_CORE_RETCODE_H
