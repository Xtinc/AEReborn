#include "io.h"
#include "drift.h"
#include <stdexcept>
#include <thread>

namespace aereborn
{
namespace
{
constexpr unsigned open = 1u;
constexpr unsigned graph_active = 2u;
} // namespace

EndpointQueue::EndpointQueue(PortSpec spec, std::chrono::microseconds device_period)
    : spec_(spec), device_period_(device_period)
{
    if (!spec.valid() || device_period.count() <= 0)
        throw std::invalid_argument("Invalid endpoint port or device period");
}

RetCode EndpointQueue::prepare(BlockPeriod period)
{
    if (!is_block_period(period))
        return ErrorCode::InvalidArg;
    if (period_ == period)
        return {};
    if (online())
        return ErrorCode::Busy;

    const auto graph_frames = spec_.max_frames(period);
    const auto rate = static_cast<std::uint32_t>(spec_.sample_rate);
    const auto us = static_cast<std::uint64_t>(device_period_.count());
    const auto effective_second = 1000000 - DriftController::max_correction_ppm();
    if (us > std::numeric_limits<std::uint64_t>::max() / rate)
        return ErrorCode::TooLarge;
    const auto units = us * rate;
    const auto device_frames = units / effective_second + (units % effective_second != 0);
    const auto limit = std::numeric_limits<std::size_t>::max() / 4;
    const auto largest_transfer = std::max<std::uint64_t>(graph_frames, device_frames);
    if (8 * graph_frames > limit || largest_transfer > (limit - 4 * graph_frames) / 2)
        return ErrorCode::TooLarge;
    const auto capacity = std::max(8 * graph_frames, 2 * static_cast<std::size_t>(largest_transfer) + 4 * graph_frames);
    const auto code = ring_.prepare(spec_.channels, capacity);
    if (!code)
        return code;
    period_ = period;
    return {};
}

RetCode EndpointQueue::set_online(bool value) noexcept
{
    if (value)
    {
        if (!ring_.capacity())
            return ErrorCode::NotPrepared;
        state_.fetch_or(open, std::memory_order_release);
    }
    else
    {
        state_.fetch_and(~open, std::memory_order_acq_rel);
        while (state_.load(std::memory_order_acquire) & graph_active)
            std::this_thread::yield();
        ring_.clear();
        primed_.store(false, std::memory_order_release);
    }
    return {};
}

void EndpointQueue::reset() noexcept
{
    set_online(false);
    overflow_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);
}

bool EndpointQueue::enter_graph() noexcept
{
    auto expected = open;
    return state_.compare_exchange_strong(expected, open | graph_active, std::memory_order_acquire,
                                          std::memory_order_relaxed);
}

void EndpointQueue::leave_graph() noexcept
{
    state_.fetch_and(~graph_active, std::memory_order_release);
}

bool EndpointQueue::online() const noexcept
{
    return (state_.load(std::memory_order_acquire) & open) != 0;
}

bool EndpointQueue::steady() const noexcept
{
    return primed_.load(std::memory_order_acquire);
}

QueueStatus EndpointQueue::status() const noexcept
{
    return {ring_.capacity(), ring_.available(), overflow_.load(std::memory_order_relaxed),
            underrun_.load(std::memory_order_relaxed), target_frames()};
}

RetCode EndpointQueue::check(IFBlock input) const noexcept
{
    if (!ring_.capacity())
        return ErrorCode::NotPrepared;
    if (input.channels() != spec_.channels)
        return ErrorCode::FormatMismatch;
    return input.valid() ? RetCode{} : RetCode{ErrorCode::InvalidArg};
}

std::size_t EndpointQueue::write(IFBlock input) noexcept
{
    const bool primed = steady();
    const auto frames = ring_.write(input);
    if (primed && frames < input.frames())
        overflow_.fetch_add(input.frames() - frames, std::memory_order_relaxed);
    return frames;
}

std::size_t EndpointQueue::read(OFBlock output) noexcept
{
    if (!output.frames())
        return 0;
    if (!primed_.load(std::memory_order_relaxed))
    {
        if (ring_.available() < target_frames())
            return 0;
        primed_.store(true, std::memory_order_release);
    }

    const auto frames = ring_.read(output);
    if (frames < output.frames())
        underrun_.fetch_add(output.frames() - frames, std::memory_order_relaxed);
    return frames;
}

Capture::Capture(SampleRate rate, std::size_t channels, std::chrono::microseconds device_read_period)
    : NodeT({}, {{PortSpec(rate, channels, SampleFormat::F32P)}}), queue_(outputs()[0], device_read_period)
{
}

RetCode Capture::prepare(BlockPeriod period) { return queue_.prepare(period); }

void Capture::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    std::size_t frames = 0;
    if (queue_.enter_graph())
    {
        frames = queue_.read(output);
        queue_.leave_graph();
    }
    output.subblock(frames, output.frames() - frames).clear();
}

void Capture::reset() noexcept { queue_.reset(); }

Result<std::size_t> Capture::write(IFBlock input) noexcept
{
    const auto code = queue_.check(input);
    if (!code)
        return code;
    return input.frames() && online() ? queue_.write(input) : 0;
}

Playback::Playback(SampleRate rate, std::size_t channels, std::chrono::microseconds device_write_period)
    : NodeT({{PortSpec(rate, channels, SampleFormat::F32P)}}, {}), queue_(inputs()[0], device_write_period)
{
}

RetCode Playback::prepare(BlockPeriod period) { return queue_.prepare(period); }

void Playback::process(const ProcessContext &context) noexcept
{
    if (!queue_.enter_graph())
        return;
    queue_.write(context.inputs[0].f32p_view());
    queue_.leave_graph();
}

void Playback::reset() noexcept { queue_.reset(); }

Result<std::size_t> Playback::read(OFBlock output) noexcept
{
    const auto code = queue_.check(output);
    if (!code)
        return code;
    const auto frames = online() ? queue_.read(output) : 0;
    output.subblock(frames, output.frames() - frames).clear();
    return frames;
}

} // namespace aereborn
