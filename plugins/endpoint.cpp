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

std::size_t read_queue(FrameRing &ring, OFBlock output, std::atomic<bool> &primed,
                       std::size_t target_frames) noexcept
{
    if (!primed.load(std::memory_order_relaxed))
    {
        if (ring.available() < target_frames)
            return 0;
        primed.store(true, std::memory_order_release);
    }
    return ring.read(output);
}
} // namespace

Capture::Capture(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period)
    : NodeT({}, {endpoint_spec(rate, channels, io_period)}), io_period_(io_period)
{
}

void Capture::prepare(BlockPeriod period)
{
    prepare_ring(ring_, spec(), period, io_period_, period_, backend_bound_.load(std::memory_order_acquire));
}

void Capture::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    std::size_t frames = 0;
    if (enter_graph(graph_state_))
    {
        frames = read_queue(ring_, output, primed_, target_frames());
        graph_state_.fetch_and(~graph_active, std::memory_order_release);
    }
    output.subblock(frames, output.frames() - frames).clear();
}

void Capture::open(bool prefill) noexcept
{
    open_graph_gate(graph_state_, primed_, prefill);
}

void Capture::close() noexcept
{
    close_graph_gate(graph_state_);
    ring_.clear();
}

Playback::Playback(SampleRate rate, std::size_t channels, std::chrono::microseconds io_period)
    : NodeT({endpoint_spec(rate, channels, io_period)}, {}), io_period_(io_period)
{
}

void Playback::prepare(BlockPeriod period)
{
    prepare_ring(ring_, spec(), period, io_period_, period_, backend_bound_.load(std::memory_order_acquire));
}

void Playback::process(const ProcessContext &context) noexcept
{
    if (enter_graph(graph_state_))
    {
        ring_.write(context.inputs[0].f32p_view());
        graph_state_.fetch_and(~graph_active, std::memory_order_release);
    }
}

void Playback::open(bool prefill) noexcept
{
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
}

std::size_t Playback::read(OFBlock output) noexcept
{
    return read_queue(ring_, output, primed_, target_frames());
}

} // namespace aereborn
