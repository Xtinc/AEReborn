#ifndef AEREBORN_CORE_RETCODE_H
#define AEREBORN_CORE_RETCODE_H

#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace aereborn
{

enum class ErrorCode
{
    Ok = 0,

    // Validation and capacity
    InvalidArg,
    InvalidPort,
    InvalidType,
    TooLarge,

    // Lifecycle and concurrency
    NotReady,
    Busy,

    // Graph topology and format
    UnknownNode,
    UnknownConn,
    DuplicateConn,
    InputBusy,
    Cycle,
    FormatMismatch,

    // External I/O and device status
    IoFailed,
    DeviceLost,
    EndOfStream,
    Cancelled,
    Timeout
};

/// @brief Non-throwing status used by backend operations.
class RetCode
{
  public:
    constexpr RetCode(ErrorCode code = ErrorCode::Ok) noexcept : code_(code)
    {
    }
    constexpr explicit operator bool() const noexcept
    {
        return code_ == ErrorCode::Ok;
    }
    constexpr ErrorCode code() const noexcept
    {
        return code_;
    }
    friend constexpr bool operator==(RetCode lhs, RetCode rhs) noexcept
    {
        return lhs.code_ == rhs.code_;
    }
    friend constexpr bool operator!=(RetCode lhs, RetCode rhs) noexcept
    {
        return lhs.code_ != rhs.code_;
    }
    const char *message() const noexcept
    {
        switch (code_)
        {
        case ErrorCode::Ok:
            return "Success";
        case ErrorCode::InvalidArg:
            return "Invalid argument";
        case ErrorCode::InvalidPort:
            return "Port index is out of range";
        case ErrorCode::InvalidType:
            return "Node has a different type";
        case ErrorCode::TooLarge:
            return "Requested capacity exceeds the supported size";
        case ErrorCode::NotReady:
            return "Object is not prepared, committed, or bound";
        case ErrorCode::Busy:
            return "The current execution context or state prevents this operation";
        case ErrorCode::UnknownNode:
            return "Node does not belong to this engine or was removed";
        case ErrorCode::UnknownConn:
            return "Connection does not exist";
        case ErrorCode::DuplicateConn:
            return "These ports are already connected";
        case ErrorCode::InputBusy:
            return "Input already has a source (use a mixer for multiple sources)";
        case ErrorCode::Cycle:
            return "Connection would create a feedback cycle";
        case ErrorCode::FormatMismatch:
            return "Formats or port layouts do not match";
        case ErrorCode::IoFailed:
            return "I/O operation failed";
        case ErrorCode::DeviceLost:
            return "Audio device is no longer available";
        case ErrorCode::EndOfStream:
            return "End of stream";
        case ErrorCode::Cancelled:
            return "I/O operation was cancelled";
        case ErrorCode::Timeout:
            return "I/O operation timed out";
        }
        return "Unknown error code";
    }

    /// @brief Throw Error if this code represents a failure.
    void or_throw() const;

  private:
    ErrorCode code_;
};

/// @brief Exception for AEReborn control-side validation and operation failures.
class Error : public std::runtime_error
{
  public:
    explicit Error(ErrorCode code) : std::runtime_error(RetCode(code).message()), code_(code) {}
    Error(ErrorCode code, const char *detail) : std::runtime_error(detail), code_(code) {}
    Error(ErrorCode code, const std::string &detail) : std::runtime_error(detail), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

inline void RetCode::or_throw() const
{
    if (!*this)
        throw Error(code_);
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
    Result(ErrorCode code) noexcept(std::is_nothrow_default_constructible<T>::value) : Result(RetCode(code))
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
        ret_code_.or_throw();
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
