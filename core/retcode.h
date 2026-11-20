#ifndef AEREBORN_CORE_RETCODE_H
#define AEREBORN_CORE_RETCODE_H

#include <cassert>
#include <type_traits>
#include <utility>

namespace aereborn
{

enum class ErrorCode
{
    Ok = 0,
    InvalidArg,
    UnknownNode,
    UnknownConn,
    InvalidPort,
    InvalidType,
    FormatMismatch,
    DuplicateConn,
    InputConnected,
    Cycle,
    NotPrepared,
    Busy,
    BufferFull,
    TooLarge
};

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
        case ErrorCode::UnknownNode:
            return "Node does not belong to this engine or was removed";
        case ErrorCode::UnknownConn:
            return "Connection does not exist";
        case ErrorCode::InvalidPort:
            return "Port index is out of range";
        case ErrorCode::InvalidType:
            return "Node has a different type";
        case ErrorCode::FormatMismatch:
            return "Port formats do not match; connect a converter";
        case ErrorCode::DuplicateConn:
            return "These ports are already connected";
        case ErrorCode::InputConnected:
            return "Input already has a source; use a mixer for multiple sources";
        case ErrorCode::Cycle:
            return "Connection would create a feedback cycle";
        case ErrorCode::NotPrepared:
            return "Preparation is required";
        case ErrorCode::Busy:
            return "The current execution context or state prevents this operation";
        case ErrorCode::BufferFull:
            return "A buffer is full or still in use";
        case ErrorCode::TooLarge:
            return "Requested capacity exceeds the supported size";
        }
        return "Unknown error code";
    }

  private:
    ErrorCode code_;
};

// For graph results with default-constructible values. Check the status before reading the value.
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

  private:
    RetCode ret_code_;
    T value_{};
};

} // namespace aereborn

#endif // AEREBORN_CORE_RETCODE_H
