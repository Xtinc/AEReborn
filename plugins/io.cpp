#include "io.h"
#include <cstring>
#include <limits>
namespace aereborn
{
namespace
{
RetCode check_binding(PortSpec spec, BlockPeriod period, std::size_t bytes, std::uint64_t tick, bool bound) noexcept
{
    if (!is_block_period(period))
        return ErrorCode::NotPrepared;
    if (bound)
        return ErrorCode::BufferFull;
    const auto frames = spec.frames(period, tick);
    const auto width = sample_bytes(spec.format);
    if (frames && spec.channels > std::numeric_limits<std::size_t>::max() / frames / width)
        return ErrorCode::TooLarge;
    if (bytes != frames * spec.channels * width)
        return ErrorCode::InvalidArg;
    return {};
}
} // namespace
RetCode Capture::prepare(BlockPeriod period)
{
    if (!is_block_period(period) || !outputs()[0].valid())
        return ErrorCode::InvalidArg;
    period_ = period;
    return {};
}
RetCode Capture::bind(View<const std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    if (bytes.size() && !bytes.data())
        return ErrorCode::InvalidArg;
    const auto code = check_binding(outputs()[0], period_, bytes.size(), tick, bound_);
    if (code)
    {
        bytes_ = bytes;
        tick_ = tick;
        bound_ = true;
    }
    return code;
}
void Capture::process(const ProcContext &context) noexcept
{
    const auto output = context.outputs[0].bytes();
    if (bound_ && tick_ == context.tick_position)
    {
        if (output.size())
            std::memcpy(output.data(), bytes_.data(), output.size());
    }
    else if (output.size())
        std::memset(output.data(), 0, output.size());
    if (bound_ && tick_ <= context.tick_position)
        reset();
}
void Capture::reset() noexcept
{
    bytes_ = {nullptr, 0};
    bound_ = false;
}
RetCode Playback::prepare(BlockPeriod period)
{
    if (!is_block_period(period) || !inputs()[0].valid())
        return ErrorCode::InvalidArg;
    period_ = period;
    return {};
}
RetCode Playback::bind(View<std::uint8_t> bytes, std::uint64_t tick) noexcept
{
    if (bytes.size() && !bytes.data())
        return ErrorCode::InvalidArg;
    const auto code = check_binding(inputs()[0], period_, bytes.size(), tick, bound_);
    if (code)
    {
        bytes_ = bytes;
        tick_ = tick;
        bound_ = true;
    }
    return code;
}
void Playback::process(const ProcContext &context) noexcept
{
    const auto input = context.inputs[0].bytes();
    if (bound_ && tick_ == context.tick_position && input.size())
        std::memcpy(bytes_.data(), input.data(), input.size());
    if (bound_ && tick_ <= context.tick_position)
        reset();
}
void Playback::reset() noexcept
{
    bytes_ = {nullptr, 0};
    bound_ = false;
}
} // namespace aereborn
