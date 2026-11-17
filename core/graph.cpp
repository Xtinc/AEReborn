#include "graph.h"

#include <atomic>
#include <cmath>
#include <limits>

namespace aereborn
{
namespace
{
std::size_t matrix_size(std::size_t outputs, std::size_t inputs)
{
    if (inputs == 0 || outputs == 0 || outputs > std::numeric_limits<std::size_t>::max() / inputs)
    {
        throw GraphError(ErrorCode::InvalidArgument, "Invalid channel matrix dimensions");
    }
    return outputs * inputs;
}
} // namespace

ChMatrix::ChMatrix(std::size_t output_channels, std::size_t input_channels, std::vector<float> coefficients)
    : input_channels_(input_channels), output_channels_(output_channels), coefficients_(std::move(coefficients))
{
    if (coefficients_.size() != matrix_size(output_channels, input_channels))
    {
        throw GraphError(ErrorCode::InvalidArgument, "Channel matrix coefficient count is incorrect");
    }
    for (const auto coefficient : coefficients_)
    {
        if (!std::isfinite(coefficient))
        {
            throw GraphError(ErrorCode::InvalidArgument, "Channel matrix coefficients must be finite");
        }
    }
}

ChMatrix ChMatrix::identity(std::size_t channels)
{
    std::vector<float> coefficients(matrix_size(channels, channels), 0.0f);
    for (std::size_t ch = 0; ch < channels; ++ch)
    {
        coefficients[ch * channels + ch] = 1.0f;
    }
    return {channels, channels, std::move(coefficients)};
}

std::uint64_t get_next_identifier()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aereborn
