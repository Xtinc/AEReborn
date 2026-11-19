#include "io.h"
#include <cstring>
#include <limits>
#include <stdexcept>
namespace aereborn
{
namespace detail
{
template <class Byte> RetCode TickBuffer<Byte>::prepare(BlockPeriod period) noexcept
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (period_ != period)
        reset();
    period_ = period;
    return {};
}

template <class Byte>
RetCode TickBuffer<Byte>::bind(PortSpec spec, View<Byte> bytes, std::uint64_t tick) noexcept
{
    if (!bytes.empty() && !bytes.data())
        return ErrorCode::InvalidArg;
    if (period_ == BlockPeriod{})
        return ErrorCode::NotPrepared;
    if (!bytes_.empty())
        return ErrorCode::BufferFull;
    const auto bytes_per_channel = spec.frames(period_, tick) * sample_bytes(spec.format);
    // Every supported rate and period produces a nonempty block.
    if (spec.channels > std::numeric_limits<std::size_t>::max() / bytes_per_channel)
        return ErrorCode::TooLarge;
    if (bytes.size() != spec.channels * bytes_per_channel)
        return ErrorCode::InvalidArg;
    bytes_ = bytes;
    tick_ = tick;
    return {};
}

template <class Byte> View<Byte> TickBuffer<Byte>::take(std::uint64_t tick) noexcept
{
    if (bytes_.empty() || tick < tick_)
        return {nullptr, 0};
    const auto bytes = bytes_;
    reset();
    return tick == tick_ ? bytes : View<Byte>{nullptr, 0};
}
} // namespace detail

Capture::Capture(PortSpec spec) : NodeT({}, {{spec}})
{
    if (!spec.valid())
        throw std::invalid_argument("Invalid capture port specification");
}
RetCode Capture::prepare(BlockPeriod period)
{
    return buffer_.prepare(period);
}
RetCode Capture::bind(View<const std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    return buffer_.bind(outputs()[0], bytes, tick);
}
void Capture::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].bytes();
    const auto bytes = buffer_.take(context.tick_position);
    if (output.empty())
        return;
    if (bytes.empty())
        std::memset(output.data(), 0, output.size());
    else
        std::memcpy(output.data(), bytes.data(), output.size());
}
void Capture::reset() noexcept
{
    buffer_.reset();
}
Playback::Playback(PortSpec spec) : NodeT({{spec}}, {})
{
    if (!spec.valid())
        throw std::invalid_argument("Invalid playback port specification");
}
RetCode Playback::prepare(BlockPeriod period)
{
    return buffer_.prepare(period);
}
RetCode Playback::bind(View<std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    return buffer_.bind(inputs()[0], bytes, tick);
}
void Playback::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0].bytes();
    const auto bytes = buffer_.take(context.tick_position);
    if (!bytes.empty() && !input.empty())
        std::memcpy(bytes.data(), input.data(), input.size());
}
void Playback::reset() noexcept
{
    buffer_.reset();
}
} // namespace aereborn
