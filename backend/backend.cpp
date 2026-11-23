#include "backend.h"

#include "core/os.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace aereborn
{
namespace
{

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

constexpr double max_asrc_correction = max_clock_drift_ppm / 1000000.0;

template <class Cleanup> class ScopeExit
{
  public:
    explicit ScopeExit(Cleanup cleanup) noexcept : cleanup_(std::move(cleanup))
    {
    }
    ~ScopeExit()
    {
        if (active_)
            cleanup_();
    }
    void dismiss() noexcept
    {
        active_ = false;
    }

  private:
    Cleanup cleanup_;
    bool active_ = true;
};

} // namespace

Backend::~Backend()
{
    assert(!bound() && "Derived backend must unbind before destruction");
}

Result<std::size_t> Backend::frames_in_period(SampleRate rate, Microseconds period) noexcept
{
    constexpr std::uint64_t micros_per_second = 1000000;
    const auto hz = static_cast<std::uint32_t>(rate);
    const auto micros = static_cast<std::uint64_t>(period.count());
    const auto frames = (micros / micros_per_second) * hz + (micros % micros_per_second) * hz / micros_per_second;
    if (frames > std::numeric_limits<std::size_t>::max())
        return ErrorCode::TooLarge;
    if (!frames)
        return ErrorCode::InvalidArg;
    return static_cast<std::size_t>(frames);
}

void Backend::bind(Capture &endpoint)
{
    bind_endpoint(endpoint, capture_);
}

void Backend::bind(Playback &endpoint)
{
    bind_endpoint(endpoint, playback_);
}

template <class Endpoint> void Backend::bind_endpoint(Endpoint &endpoint, Endpoint *&slot)
{
    if (bound())
        throw Error(ErrorCode::Busy);
    if (!endpoint.capacity())
        throw Error(ErrorCode::NotReady);
    if (endpoint.backend_bound_.exchange(true, std::memory_order_acq_rel))
        throw Error(ErrorCode::Busy);

    const auto cleanup = [&]() noexcept {
        on_close();
        endpoint.backend_bound_.store(false, std::memory_order_release);
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);

    constexpr bool capture = std::is_same<Endpoint, Capture>::value;
    Workspace next;
    const auto logical = endpoint.spec();
    const auto period = endpoint.io_period();
    next.device = on_open(capture, logical, period).value_or_throw();

    const auto &device = next.device.spec;
    const auto frames = next.device.max_frames;
    if (!is_sample_rate(device.sample_rate) || !device.channels || !is_interleaved_pcm(device.format) || !frames)
        throw Error(ErrorCode::InvalidArg);
    if (device.channels != logical.channels)
        throw Error(ErrorCode::FormatMismatch);
    if (frames > frames_in_period(device.sample_rate, period).value_or_throw())
        throw Error(ErrorCode::TooLarge);

    const auto src_fs = capture ? device.sample_rate : logical.sample_rate;
    const auto dst_fs = capture ? logical.sample_rate : device.sample_rate;
    const auto max_rate_offset = async_ ? max_asrc_correction : 0.0;
    next.logical_frames = capture ? SincResampler::max_output_frames(src_fs, dst_fs, frames, max_rate_offset)
                                  : SincResampler::max_input_frames(src_fs, dst_fs, frames, max_rate_offset);
    if (next.logical_frames > endpoint.target_frames())
        throw Error(ErrorCode::TooLarge);

    const auto width = sample_bytes(device.format);
    if (frames > next.device_bytes.max_size() / logical.channels / width)
        throw Error(ErrorCode::TooLarge);
    next.resampler.prepare(logical.channels, src_fs, dst_fs, max_rate_offset);
    if (next.resampler.needs_fir())
        next.device_planar.resize(logical.channels, frames);
    next.logical_planar.resize(logical.channels, next.logical_frames);
    next.device_bytes.resize(frames * logical.channels * width);

    workspace_ = std::move(next);
    slot = &endpoint;
    last_error_.store({}, std::memory_order_release);
    rollback.dismiss();
}

bool Backend::running() const noexcept
{
    return worker_state_.load(std::memory_order_acquire) != State::Stopped;
}

Microseconds Backend::endpoint_period() const noexcept
{
    return capture_ ? capture_->io_period() : playback_->io_period();
}

Milliseconds Backend::io_timeout() const noexcept
{
    // Ten periods, rounded up to the wait API's millisecond resolution.
    const auto us = endpoint_period().count();
    const auto ms = us / 100 + (us % 100 != 0);
    return Milliseconds(std::max<std::int64_t>(500, std::min<std::int64_t>(ms, std::numeric_limits<int>::max())));
}

int Backend::remaining_io_ms() const noexcept
{
    const auto remaining = io_deadline_ - Clock::now();
    if (remaining <= Clock::duration::zero())
        return 0;
    const auto ms = std::chrono::duration_cast<Milliseconds>(remaining);
    return static_cast<int>(ms.count() + (ms < remaining));
}

void Backend::start()
{
    if (!bound())
        throw Error(ErrorCode::NotReady);
    if (running())
        throw Error(ErrorCode::Busy);
    if (worker_.joinable())
        worker_.join();
    last_error_.store({}, std::memory_order_release);

    RetCode start_result;
    bool start_done = false;
    worker_ = std::thread([this, &start_result, &start_done]() noexcept {
        if (async_)
            configure_audio_thread(capture_ ? "ae-capture" : "ae-playback");

        const auto code = on_start();
        if (code)
        {
            if (capture_)
                capture_->open(async_);
            else
                playback_->open(async_);
            worker_state_.store(State::Running, std::memory_order_release);
        }
        {
            const std::lock_guard<std::mutex> lock(wait_mutex_);
            start_result = code;
            start_done = true;
        }
        wait_cv_.notify_one();
        if (code)
            run();
    });

    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait(lock, [&] { return start_done; });
    lock.unlock();
    if (!start_result)
        worker_.join();
    start_result.or_throw();
}

void Backend::stop() noexcept
{
    if (!worker_.joinable())
        return;
    if (running())
    {
        const auto drain = !async_ && playback_;
        if (drain)
            playback_->close_graph();

        bool stopping;
        {
            const std::lock_guard<std::mutex> lock(wait_mutex_);
            auto expected = State::Running;
            stopping = worker_state_.compare_exchange_strong(expected, drain ? State::Draining : State::Stopping,
                                                             std::memory_order_acq_rel);
        }
        if (stopping)
        {
            wait_cv_.notify_one();
            if (!drain)
                on_cancel();
        }
    }
    worker_.join();
}

void Backend::unbind() noexcept
{
    if (!bound())
        return;
    stop();
    on_close();
    auto &bound = capture_ ? capture_->backend_bound_ : playback_->backend_bound_;
    bound.store(false, std::memory_order_release);
    capture_ = nullptr;
    playback_ = nullptr;
    workspace_ = {};
}

void Backend::go_offline(RetCode error) noexcept
{
    if (capture_)
        capture_->close();
    else
        playback_->close();
    workspace_.resampler.reset();
    asrc_.reset();
    has_input_ = false;
    record_io_error(error);
}

void Backend::record_io_error(RetCode error) noexcept
{
    if (!error && error != ErrorCode::Cancelled)
        last_error_.store(error, std::memory_order_release);
}

Result<std::size_t> Backend::on_read(OBBlock) noexcept
{
    return ErrorCode::InvalidArg;
}

RetCode Backend::on_write(IBBlock) noexcept
{
    return ErrorCode::InvalidArg;
}

RetCode Backend::on_write_silence(OBBlock scratch) noexcept
{
    const auto bytes = scratch.byte_view();
    std::memset(bytes.data(), 0, bytes.size());
    return on_write(scratch);
}

void Backend::wait_backpressure() noexcept
{
    const auto wait = std::max(Microseconds(200), std::min(Microseconds(2000), endpoint_period() / 2));
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait_for(lock, wait, [this] { return worker_state_.load(std::memory_order_acquire) != State::Running; });
}

void Backend::ASRC::update(std::size_t queued_before, std::size_t queued_after, std::size_t target_frames,
                           SampleRate logical_rate, std::size_t device_frames, SampleRate device_rate) noexcept
{
    constexpr double filter_seconds = 0.25;
    constexpr double proportional_gain = 0.5;
    constexpr double integral_gain = 0.06;

    const auto batch_seconds = static_cast<double>(device_frames) / static_cast<std::uint32_t>(device_rate);
    const auto fill_frames = (static_cast<double>(queued_before) + static_cast<double>(queued_after)) * 0.5;
    const auto error_seconds =
        (fill_frames - static_cast<double>(target_frames)) / static_cast<std::uint32_t>(logical_rate);
    filtered_error += -std::expm1(-batch_seconds / filter_seconds) * (error_seconds - filtered_error);
    const auto proportional = proportional_gain * filtered_error;
    const auto next_integral = integral + integral_gain * filtered_error * batch_seconds;
    const auto requested_correction = proportional + next_integral;
    if (std::abs(requested_correction) <= max_asrc_correction || requested_correction * filtered_error < 0)
        integral = next_integral;
    correction = std::max(-max_asrc_correction, std::min(max_asrc_correction, proportional + integral));
}

OBBlock Backend::device_block(std::size_t frames) noexcept
{
    return {workspace_.device_bytes.data(), workspace_.device.spec, frames};
}

void Backend::emit_capture(IFBlock input, double scale) noexcept
{
    const auto output = workspace_.logical_planar.block(workspace_.logical_frames);
    const auto result = workspace_.resampler.process(input, output, scale);
    assert(result.consumed == input.frames());
    capture_->write(output.subblock(0, result.produced));
}

RetCode Backend::transfer_capture(std::size_t &device_frames) noexcept
{
    const auto batch = device_block(device_max_frames());
    const auto read = on_read(batch);
    if (!read)
        return read.ret_code();
    device_frames = read.value();
    if (device_frames > batch.frames())
        return ErrorCode::IoFailed;
    if (!device_frames)
        return {};

    has_input_ = true;
    const auto steady = async_ && capture_->steady();
    const auto queued_before = steady ? capture_->available() : 0;
    const auto fir = workspace_.resampler.needs_fir();
    const auto planar = (fir ? workspace_.device_planar : workspace_.logical_planar).block(device_frames);
    decode_pcm(device_block(device_frames), planar);
    if (fir)
        emit_capture(planar, asrc_.scale());
    else
        capture_->write(planar);
    if (steady)
        asrc_.update(queued_before, capture_->available(), capture_->target_frames(), capture_->spec().sample_rate,
                     device_frames, batch.spec().sample_rate);
    return {};
}

Result<std::size_t> Backend::next_playback_batch() noexcept
{
    const auto batch = on_wait_writable();
    if (!batch)
        return batch;
    const auto frames = batch.value();
    if (!frames || frames > device_max_frames())
        return ErrorCode::IoFailed;
    return batch;
}

RetCode Backend::emit_playback(IFBlock input, std::size_t device_frames, double scale) noexcept
{
    IFBlock output = input;
    if (workspace_.resampler.needs_fir())
    {
        const auto device = workspace_.device_planar.block(device_frames);
        const auto conversion = workspace_.resampler.process(input, device, scale);
        assert(conversion.consumed == input.frames());
        output = device.subblock(0, conversion.produced);
    }

    const auto produced = output.frames();
    if (!produced)
        return async_ ? on_write_silence(device_block(device_frames)) : RetCode{};

    const auto samples = device_block(produced);
    encode_pcm(output, samples);
    if (async_ && produced < device_frames)
    {
        const auto batch = device_block(device_frames);
        const auto bytes = batch.byte_view();
        const auto encoded = samples.byte_view().size();
        std::memset(bytes.data() + encoded, 0, bytes.size() - encoded);
        return on_write(batch);
    }
    return on_write(samples);
}

RetCode Backend::transfer_playback_async(std::size_t &device_frames) noexcept
{
    const auto batch = next_playback_batch();
    if (!batch)
        return batch.ret_code();
    device_frames = batch.value();
    const auto scale = asrc_.scale();
    const auto requested = workspace_.resampler.required_input(device_frames, scale);
    const auto steady = playback_->steady();
    const auto queued_before = steady ? playback_->available() : 0;
    const auto logical = workspace_.logical_planar.block(requested);
    const auto received = requested ? playback_->read(logical) : 0;
    const auto result = received || (!requested && steady)
                            ? emit_playback(logical.subblock(0, received), device_frames, scale)
                            : on_write_silence(device_block(device_frames));
    if (result && steady)
        asrc_.update(queued_before, playback_->available(), playback_->target_frames(), playback_->spec().sample_rate,
                     device_frames, workspace_.device.spec.sample_rate);
    return result;
}

RetCode Backend::run_sync_capture() noexcept
{
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        if (capture_->capacity() - capture_->available() < workspace_.logical_frames)
        {
            wait_backpressure();
            continue;
        }
        std::size_t frames = 0;
        const auto result = transfer_capture(frames);
        if (!result)
            return result == ErrorCode::EndOfStream ? drain_capture() : result;
        if (!frames)
            wait_backpressure();
    }
    return {};
}

RetCode Backend::drain_capture() noexcept
{
    auto tail = has_input_ ? workspace_.resampler.tail_frames() : 0;
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        const auto queued = capture_->available();
        if (!tail && !queued)
            return ErrorCode::EndOfStream;
        if (tail && capture_->capacity() - queued >= workspace_.logical_frames)
        {
            const auto frames = std::min(tail, device_max_frames());
            const auto input = workspace_.device_planar.block(frames);
            input.clear();
            emit_capture(input, 1);
            tail -= frames;
        }
        else
            wait_backpressure();
    }
    return {};
}

RetCode Backend::run_sync_playback() noexcept
{
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        const auto batch = next_playback_batch();
        if (!batch)
            return batch.ret_code();
        const auto requested = workspace_.resampler.required_input(batch.value());

        while (playback_->available() < requested && worker_state_.load(std::memory_order_acquire) == State::Running)
            wait_backpressure();
        if (worker_state_.load(std::memory_order_acquire) != State::Running)
            break;

        const auto logical = workspace_.logical_planar.block(requested);
        const auto received = requested ? playback_->read(logical) : 0;
        assert(received == requested);
        has_input_ |= received != 0;
        const auto result = emit_playback(logical, batch.value(), 1.0);
        if (!result)
            return result;
    }
    return worker_state_.load(std::memory_order_acquire) == State::Draining ? drain_playback() : RetCode{};
}

RetCode Backend::drain_playback() noexcept
{
    auto tail = has_input_ || playback_->available() ? workspace_.resampler.tail_frames() : 0;
    for (;;)
    {
        if (!tail && !playback_->available())
        {
            // Upsampling can still have output at the current phase after consuming the last input frame.
            if (workspace_.resampler.required_input(1))
                return {};
        }
        const auto batch = next_playback_batch();
        if (!batch)
            return batch.ret_code();
        const auto requested = workspace_.resampler.required_input(batch.value());
        const auto logical = workspace_.logical_planar.block(requested);
        const auto received = requested ? playback_->read(logical) : 0;
        const auto padding = std::min(tail, requested - received);
        if (padding)
        {
            logical.subblock(received, padding).clear();
            tail -= padding;
        }
        const auto result = emit_playback(logical.subblock(0, received + padding), batch.value(), 1.0);
        if (!result)
            return result;
    }
}

RetCode Backend::run_async() noexcept
{
    const auto timeout = io_timeout();
    io_deadline_ = Clock::now() + timeout;
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        std::size_t frames = 0;
        const auto result = capture_ ? transfer_capture(frames) : transfer_playback_async(frames);
        if (!result)
            return result;

        const auto now = Clock::now();
        if (now >= io_deadline_)
            return ErrorCode::Timeout;
        if (frames)
            io_deadline_ = now + timeout;
    }
    return {};
}

void Backend::run() noexcept
{
    auto error = async_ ? run_async() : (capture_ ? run_sync_capture() : run_sync_playback());
    on_stop();
    if (worker_state_.load(std::memory_order_acquire) == State::Stopping)
        error = {};
    go_offline(error);
    worker_state_.store(State::Stopped, std::memory_order_release);
}

} // namespace aereborn
