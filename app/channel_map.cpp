#include "audio_interface.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace aereborn
{
namespace
{

RetCode check_dimensions(std::size_t inputs, std::size_t outputs, std::size_t capacity) noexcept
{
    if (!inputs || !outputs)
        return RetCode::EPARAM;
    if (outputs > capacity / inputs)
        return RetCode::TOOLARGE;
    return RetCode::OK;
}

} // namespace

ChannelMap::ChannelMap(std::size_t input_channels, std::size_t output_channels) noexcept
    : status_(check_dimensions(input_channels, output_channels, coefficients_.max_size()))
{
    if (!status_)
        return;

    try
    {
        coefficients_.resize(input_channels * output_channels);
        input_channels_ = input_channels;
        output_channels_ = output_channels;
    }
    catch (const std::bad_alloc &)
    {
        status_ = RetCode::TOOLARGE;
    }
}

ChannelMap ChannelMap::identity(std::size_t channels) noexcept
{
    ChannelMap map(channels, channels);
    if (map.valid())
        for (std::size_t channel = 0; channel < channels; ++channel)
            map.coefficients_[channel * channels + channel] = 1.0f;
    return map;
}

ChannelMap ChannelMap::select(std::size_t input_channels, const std::vector<std::size_t> &sources) noexcept
{
    return select(input_channels, sources.data(), sources.size());
}

ChannelMap ChannelMap::select(std::size_t input_channels, std::initializer_list<std::size_t> sources) noexcept
{
    return select(input_channels, sources.begin(), sources.size());
}

ChannelMap ChannelMap::select(std::size_t input_channels, const std::size_t *sources, std::size_t count) noexcept
{
    for (std::size_t output = 0; output < count; ++output)
        if (sources[output] >= input_channels)
            return {};

    ChannelMap map(input_channels, count);
    if (map.valid())
        for (std::size_t output = 0; output < count; ++output)
            map.coefficients_[output * input_channels + sources[output]] = 1.0f;
    return map;
}

ChannelMap ChannelMap::slice(std::size_t input_channels, std::size_t first, std::size_t count) noexcept
{
    if (first > input_channels || count > input_channels - first)
        return {};

    ChannelMap map(input_channels, count);
    if (map.valid())
        for (std::size_t output = 0; output < count; ++output)
            map.coefficients_[output * input_channels + first + output] = 1.0f;
    return map;
}

ChannelMap ChannelMap::to_mono(std::size_t input_channels) noexcept
{
    ChannelMap map(input_channels, 1);
    if (map.valid())
        std::fill(map.coefficients_.begin(), map.coefficients_.end(), 1.0f / static_cast<float>(input_channels));
    return map;
}

ChannelMap ChannelMap::from_mono(std::size_t output_channels) noexcept
{
    ChannelMap map(1, output_channels);
    std::fill(map.coefficients_.begin(), map.coefficients_.end(), 1.0f);
    return map;
}

ChannelMap ChannelMap::dense(std::size_t input_channels, std::size_t output_channels,
                             const std::vector<float> &coefficients) noexcept
{
    return dense(input_channels, output_channels, coefficients.data(), coefficients.size());
}

ChannelMap ChannelMap::dense(std::size_t input_channels, std::size_t output_channels,
                             std::initializer_list<float> coefficients) noexcept
{
    return dense(input_channels, output_channels, coefficients.begin(), coefficients.size());
}

ChannelMap ChannelMap::dense(std::size_t input_channels, std::size_t output_channels, const float *coefficients,
                             std::size_t count) noexcept
{
    ChannelMap map;
    map.status_ = check_dimensions(input_channels, output_channels, map.coefficients_.max_size());
    if (!map.status_)
        return map;
    if (count != input_channels * output_channels ||
        !std::all_of(coefficients, coefficients + count, [](float weight) { return std::isfinite(weight); }))
        return {};

    try
    {
        map.coefficients_.assign(coefficients, coefficients + count);
        map.input_channels_ = input_channels;
        map.output_channels_ = output_channels;
    }
    catch (const std::bad_alloc &)
    {
        map.status_ = RetCode::TOOLARGE;
    }
    return map;
}

ChannelMap &ChannelMap::route(std::size_t from, std::size_t to, float weight) noexcept
{
    if (auto *row = output_row(to))
    {
        if (from < input_channels_ && std::isfinite(weight))
            row[from] = weight;
        else
            status_ = RetCode::EPARAM;
    }
    return *this;
}

ChannelMap &ChannelMap::mix(const std::vector<std::size_t> &from, std::size_t to, float gain) noexcept
{
    return mix(from.data(), from.size(), to, gain);
}

ChannelMap &ChannelMap::mix(std::initializer_list<std::size_t> from, std::size_t to, float gain) noexcept
{
    return mix(from.begin(), from.size(), to, gain);
}

ChannelMap &ChannelMap::mix(const std::size_t *from, std::size_t count, std::size_t to, float gain) noexcept
{
    auto *row = output_row(to);
    if (!row)
        return *this;
    if (!count || count > input_channels_ || !std::isfinite(gain))
    {
        status_ = RetCode::EPARAM;
        return *this;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        if (from[i] >= input_channels_ || std::find(from, from + i, from[i]) != from + i)
        {
            status_ = RetCode::EPARAM;
            return *this;
        }
    }

    const auto weight = gain / static_cast<float>(count);
    std::fill_n(row, input_channels_, 0.0f);
    for (std::size_t i = 0; i < count; ++i)
        row[from[i]] = weight;
    return *this;
}

ChannelMap &ChannelMap::scale_output(std::size_t to, float factor) noexcept
{
    auto *row = output_row(to);
    if (!row)
        return *this;
    const auto end = row + input_channels_;
    if (!std::isfinite(factor) ||
        !std::all_of(row, end, [factor](float weight) { return std::isfinite(weight * factor); }))
    {
        status_ = RetCode::EPARAM;
        return *this;
    }
    for (auto *weight = row; weight != end; ++weight)
        *weight *= factor;
    return *this;
}

bool ChannelMap::valid() const noexcept
{
    return static_cast<bool>(status());
}

RetCode ChannelMap::status() const noexcept
{
    if (status_ && (!input_channels_ || !output_channels_ || coefficients_.size() != input_channels_ * output_channels_))
        return RetCode::EPARAM;
    return status_;
}

float ChannelMap::weight(std::size_t from, std::size_t to) const noexcept
{
    if (!valid() || from >= input_channels_ || to >= output_channels_)
        return 0.0f;
    return coefficients_[to * input_channels_ + from];
}

float *ChannelMap::output_row(std::size_t to) noexcept
{
    if (!valid())
        return nullptr;
    if (to >= output_channels_)
    {
        status_ = RetCode::EPARAM;
        return nullptr;
    }
    return coefficients_.data() + to * input_channels_;
}

} // namespace aereborn
