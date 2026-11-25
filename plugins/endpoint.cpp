#include "endpoint.h"

#include "core/resampler.h"
#include <algorithm>
#include <limits>

namespace aereborn
{
namespace
{
constexpr unsigned graph_open = 1u;
constexpr unsigned graph_active = 2u;

PortSpec endpoint_spec(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period)
{
    const PortSpec spec(rate, channels, SampleFormat::F32P);
    if (!spec.valid() || io_period.count() <= 0)
        throw Error(ErrorCode::InvalidArg, "Invalid endpoint port or I/O period");
    return spec;
}

void prepare_ring(FrameRing &ring, PortSpec spec, BlockPeriod period, std::chrono::microseconds io_period,
                  BlockPeriod &prepared_period, bool in_use)
{
    if (!is_block_period(period))
        throw Error(ErrorCode::InvalidArg);
    if (period == prepared_period)
        return;
    if (in_use)
        throw Error(ErrorCode::Busy);
    const auto graph_frames = spec.max_frames(period);
    const auto rate = static_cast<std::uint32_t>(spec.sample_rate);
    const auto us = static_cast<std::uint64_t>(io_period.count());
    if (us > std::numeric_limits<std::uint64_t>::max() / rate)
        throw Error(ErrorCode::TooLarge);
    // Round up at the slowest allowed device clock to cover the largest logical batch.
    constexpr auto effective_second = 1000000 - max_clock_drift_ppm;
    const auto units = us * rate;
    const auto device_frames = units / effective_second + (units % effective_second != 0);
    const auto capacity = std::max<std::uint64_t>(8 * graph_frames, 2 * device_frames + 4 * graph_frames);
    if (capacity > std::numeric_limits<std::size_t>::max())
        throw Error(ErrorCode::TooLarge);
    ring.prepare(spec.channels, static_cast<std::size_t>(capacity));
    prepared_period = period;
}

bool enter_graph(std::atomic<unsigned> &state) noexcept
{
    auto expected = graph_open;
    return state.compare_exchange_strong(expected, graph_open | graph_active, std::memory_order_acquire,
                                         std::memory_order_relaxed);
}

void close_graph_gate(std::atomic<unsigned> &state) noexcept
{
    state.fetch_and(~graph_open, std::memory_order_acq_rel);
    // Only the caller outside the graph waits, until the current queue access ends.
    while (state.load(std::memory_order_acquire) & graph_active)
    {
    }
}

void open_graph_gate(std::atomic<unsigned> &state, std::atomic<bool> &primed, bool prefill) noexcept
{
    primed.store(!prefill, std::memory_order_relaxed);
    state.store(graph_open, std::memory_order_release);
}

bool prefill_ready(const FrameRing &ring, std::atomic<bool> &primed, std::size_t target_frames) noexcept
{
    if (primed.load(std::memory_order_relaxed))
        return true;
    if (ring.available() < target_frames)
        return false;
    primed.store(true, std::memory_order_release);
    return true;
}
} // namespace

Capture::Capture(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period)
    : NodeT({}, {endpoint_spec(rate, channels, io_period)}), io_period_(io_period)
{
}

void Capture::prepare(BlockPeriod period)
{
    prepare_ring(ring_, spec(), period, io_period_, period_, bound());
}

void Capture::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    std::size_t frames = 0;
    if (enter_graph(graph_state_))
    {
        if (prefill_ready(ring_, primed_, target_frames()))
        {
            frames = ring_.read(output);
            if (frames < output.frames())
                underrun_frames_.fetch_add(output.frames() - frames, std::memory_order_relaxed);
        }
        graph_state_.fetch_and(~graph_active, std::memory_order_release);
    }
    output.subblock(frames, output.frames() - frames).clear();
    context.set_output_active(0, frames != 0);
}

bool Capture::online() const noexcept
{
    return (graph_state_.load(std::memory_order_acquire) & graph_open) != 0;
}

void Capture::open(bool prefill) noexcept
{
    underrun_frames_.store(0, std::memory_order_relaxed);
    open_graph_gate(graph_state_, primed_, prefill);
}

void Capture::close() noexcept
{
    close_graph_gate(graph_state_);
    ring_.clear();
    primed_.store(false, std::memory_order_release);
}

Playback::Playback(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period)
    : NodeT({endpoint_spec(rate, channels, io_period)}, {}), io_period_(io_period)
{
}

void Playback::prepare(BlockPeriod period)
{
    prepare_ring(ring_, spec(), period, io_period_, period_, bound());
}

void Playback::process(const ProcessContext &context) noexcept
{
    if (enter_graph(graph_state_))
    {
        const auto input = context.inputs[0].f32p_view();
        const auto written = ring_.write(input);
        if (written < input.frames())
            overflow_frames_.fetch_add(input.frames() - written, std::memory_order_relaxed);
        graph_state_.fetch_and(~graph_active, std::memory_order_release);
    }
}

bool Playback::online() const noexcept
{
    return (graph_state_.load(std::memory_order_acquire) & graph_open) != 0;
}

void Playback::open(bool prefill) noexcept
{
    overflow_frames_.store(0, std::memory_order_relaxed);
    open_graph_gate(graph_state_, primed_, prefill);
}

void Playback::close_graph() noexcept
{
    close_graph_gate(graph_state_);
}

void Playback::close() noexcept
{
    close_graph();
    ring_.clear();
    primed_.store(false, std::memory_order_release);
}

std::size_t Playback::read(OFBlock output) noexcept
{
    return prefill_ready(ring_, primed_, target_frames()) ? ring_.read(output) : 0;
}

} // namespace aereborn
