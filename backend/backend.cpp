#include "backend.h"

#include "audio_types.h"
#include "core/retcode.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

#if LINUX_OS_ENVIRONMENT || defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#elif WINDOWS_OS_ENVIRONMENT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aereborn
{
namespace
{

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

constexpr double ppm_per_unit = 1000000.0;
constexpr double max_asrc_correction = max_clock_drift_ppm / ppm_per_unit;

/// @brief Name the calling worker and request the platform's audio scheduling priority.
/// @param name Null-terminated UTF-8 thread name of at most 15 bytes.
void configure_audio_thread(const char *name) noexcept
{
#if LINUX_OS_ENVIRONMENT || defined(__APPLE__)
    const auto thread = pthread_self();
#if LINUX_OS_ENVIRONMENT
    pthread_setname_np(thread, name);
    constexpr auto policy = SCHED_RR;
    const auto priority = sched_get_priority_max(policy) - 1;
#else
    pthread_setname_np(name);
    constexpr auto policy = SCHED_FIFO;
    const auto priority = sched_get_priority_min(policy);
#endif
    if (priority >= 0)
    {
        sched_param params{};
        params.sched_priority = priority;
        pthread_setschedparam(thread, policy, &params);
    }
#elif WINDOWS_OS_ENVIRONMENT
    const auto thread = GetCurrentThread();
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    const auto kernel = GetModuleHandleW(L"Kernel32.dll");
    if (kernel)
    {
        if (const auto set_name = reinterpret_cast<SetDescription>(GetProcAddress(kernel, "SetThreadDescription")))
        {
            constexpr int capacity = 16;
            wchar_t wide_name[capacity];
            if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, capacity))
                set_name(thread, wide_name);
        }
    }
#else
    (void)name;
#endif
}

} // namespace

Backend::~Backend()
{
    assert(!capture_ && !playback_ && "Backend must release its endpoint before destruction");
}

void Backend::claim(Capture &endpoint)
{
    claim_endpoint(endpoint, capture_);
}

void Backend::claim(Playback &endpoint)
{
    claim_endpoint(endpoint, playback_);
}

template <class Endpoint> void Backend::claim_endpoint(Endpoint &endpoint, Endpoint *&slot)
{
    if (capture_ || playback_)
        throw Error(RetCode::BUSY);
    if (!endpoint.capacity())
        throw Error(RetCode::ESTATE);
    if (endpoint.backend_bound_.exchange(true, std::memory_order_acq_rel))
        throw Error(RetCode::BUSY);
    slot = &endpoint;
}

void Backend::validate_channels(std::size_t logical_channels, std::size_t device_channels)
{
    if (device_channels != logical_channels)
        throw Error(RetCode::INVFMT);
}

void Backend::prepare(PortSpec logical, BandWidth device_rate, double max_rate_offset)
{
    const auto frames = device_max_frames_;
    const auto capture = is_capture();
    const auto src_fs = capture ? device_rate : logical.bandwidth;
    const auto dst_fs = capture ? logical.bandwidth : device_rate;
    const auto logical_frames = capture ? SincResampler::max_output_frames(src_fs, dst_fs, frames, max_rate_offset)
                                        : SincResampler::max_input_frames(src_fs, dst_fs, frames, max_rate_offset);
    if (logical_frames > target_frames())
        throw Error(RetCode::TOOLARGE);

    resampler_.prepare(logical.channels, src_fs, dst_fs, max_rate_offset);
    if (resampler_.needs_fir())
        device_planar_.resize(logical.channels, frames);
    logical_planar_.resize(logical.channels, logical_frames);
    logical_frames_ = logical_frames;
}

void Backend::reset_workspace() noexcept
{
    device_spec_ = {};
    device_max_frames_ = 0;
    logical_frames_ = 0;
    resampler_ = {};
    device_planar_ = {};
    logical_planar_ = {};
}

void Backend::release_endpoint() noexcept
{
    assert(!bound() && (capture_ || playback_));
    auto &occupied = capture_ ? capture_->backend_bound_ : playback_->backend_bound_;
    occupied.store(false, std::memory_order_release);
    capture_ = nullptr;
    playback_ = nullptr;
}

void Backend::release() noexcept
{
    reset_workspace();
    release_endpoint();
}

void Backend::open_queue(bool prefill) noexcept
{
    if (capture_)
        capture_->open(prefill);
    else
        playback_->open(prefill);
}

void Backend::close_queue() noexcept
{
    if (capture_)
        capture_->close();
    else
        playback_->close();
    resampler_.reset();
}

void Backend::close_playback_graph() noexcept
{
    playback_->close_graph();
}

const Node *Backend::endpoint_node() const noexcept
{
    if (!bound())
        return nullptr;
    if (capture_)
        return capture_;
    return playback_;
}

PortSpec Backend::logical_spec() const noexcept
{
    if (capture_)
        return capture_->spec();
    return playback_ ? playback_->spec() : PortSpec{};
}

std::chrono::microseconds Backend::endpoint_period() const noexcept
{
    return capture_ ? capture_->io_period() : playback_->io_period();
}

bool Backend::steady() const noexcept
{
    return capture_ ? capture_->steady() : playback_->steady();
}

std::size_t Backend::available() const noexcept
{
    return capture_ ? capture_->available() : playback_->available();
}

std::size_t Backend::target_frames() const noexcept
{
    return capture_ ? capture_->target_frames() : playback_->target_frames();
}

std::size_t Backend::capture_free_frames() const noexcept
{
    return capture_->capacity() - capture_->available();
}

void Backend::decode_to_capture(IBBlock input, double scale) noexcept
{
    const auto fir = resampler_.needs_fir();
    const auto planar = (fir ? device_planar_ : logical_planar_).block(input.frames());
    decode_pcm(input, planar);
    if (fir)
        emit_capture(planar, scale);
    else
        capture_->write(planar);
}

void Backend::flush_capture_tail(std::size_t frames) noexcept
{
    const auto input = device_planar_.block(frames);
    input.clear();
    emit_capture(input, 1.0);
}

void Backend::emit_capture(IFBlock input, double scale) noexcept
{
    const auto output = logical_block(logical_frames_);
    const auto result = resampler_.process(input, output, scale);
    assert(result.consumed == input.frames());
    capture_->write(output.subblock(0, result.produced));
}

std::size_t Backend::read_playback(OFBlock output) noexcept
{
    // A zero-frame batch must leave the playback prefill gate untouched.
    return output.frames() ? playback_->read(output) : 0;
}

OBBlock Backend::encode_from_playback(IFBlock input, OBBlock output, double scale) noexcept
{
    auto source = input;
    if (resampler_.needs_fir())
    {
        const auto device = device_planar_.block(output.frames());
        const auto result = resampler_.process(input, device, scale);
        assert(result.consumed == input.frames());
        source = device.subblock(0, result.produced);
    }

    assert(source.frames() <= output.frames());
    const OBBlock encoded(output.byte_view().data(), output.spec(), source.frames());
    if (encoded.frames())
        encode_pcm(source, encoded);
    return encoded;
}

Result<std::size_t> Backend::frames_in_period(BandWidth rate, std::chrono::microseconds period) noexcept
{
    constexpr std::uint64_t micros_per_second = 1000000;
    const auto hz = static_cast<std::uint32_t>(rate);
    const auto micros = static_cast<std::uint64_t>(period.count());
    const auto frames = (micros / micros_per_second) * hz + (micros % micros_per_second) * hz / micros_per_second;
    if (frames > std::numeric_limits<std::size_t>::max())
        return RetCode::TOOLARGE;
    if (!frames)
        return RetCode::EPARAM;
    return static_cast<std::size_t>(frames);
}

void PassiveBackend::start()
{
    if (!bound())
        throw Error(RetCode::ESTATE);
    if (running_)
        throw Error(RetCode::BUSY);
    open_queue(false);
    running_ = true;
}

void PassiveBackend::stop() noexcept
{
    if (!running_)
        return;
    close_queue();
    running_ = false;
}

void PassiveBackend::unbind() noexcept
{
    if (!bound())
        return;
    stop();
    release();
}

void ActiveBackend::bind(Capture &endpoint)
{
    bind_endpoint(endpoint);
}

void ActiveBackend::bind(Playback &endpoint)
{
    bind_endpoint(endpoint);
}

template <class Endpoint> void ActiveBackend::bind_endpoint(Endpoint &endpoint)
{
    claim(endpoint);
    const auto cleanup = [this]() noexcept {
        reset_workspace();
        on_close();
        release_endpoint();
    };
    ScopeExit<decltype(cleanup)> rollback(cleanup);

    const auto logical = endpoint.spec();
    const auto period = endpoint_period();
    const auto device = on_open(is_capture(), logical, period).value_or_throw();
    const auto frames = device_max_frames();
    if (!valid_pcm_spec(device) || !frames)
        throw Error(RetCode::EPARAM);
    validate_channels(logical.channels, device.channels);
    if (frames > frames_in_period(device.bandwidth, period).value_or_throw())
        throw Error(RetCode::TOOLARGE);

    prepare(logical, device.bandwidth, async_ ? max_asrc_correction : 0.0);
    const auto width = sample_bytes(device.format);
    if (frames > device_bytes_.max_size() / device.channels / width)
        throw Error(RetCode::TOOLARGE);
    std::vector<std::uint8_t> device_bytes(frames * device.channels * width);
    throw_if_error(on_bind_ready(device));
    device_bytes_.swap(device_bytes);
    complete_binding(device);
    last_error_.store({}, std::memory_order_release);
    rollback.dismiss();
}

bool ActiveBackend::running() const noexcept
{
    return state() != State::Stopped;
}

float ActiveBackend::drift_ppm() const noexcept
{
    return static_cast<float>(asrc_.correction.load(std::memory_order_relaxed) * ppm_per_unit);
}

Milliseconds ActiveBackend::io_timeout() const noexcept
{
    // Ten periods, rounded up to the wait API's millisecond resolution.
    const auto us = endpoint_period().count();
    const auto ms = us / 100 + (us % 100 != 0);
    return Milliseconds(std::max<std::int64_t>(500, std::min<std::int64_t>(ms, std::numeric_limits<int>::max())));
}

int ActiveBackend::remaining_io_ms() const noexcept
{
    const auto remaining = io_deadline_ - Clock::now();
    if (remaining <= Clock::duration::zero())
        return 0;
    const auto ms = std::chrono::duration_cast<Milliseconds>(remaining);
    return static_cast<int>(ms.count() + (ms < remaining));
}

void ActiveBackend::start()
{
    if (!bound())
        throw Error(RetCode::ESTATE);
    if (running())
        throw Error(RetCode::BUSY);
    if (worker_.joinable())
        worker_.join();
    last_error_.store({}, std::memory_order_release);

    RetCode start_result;
    bool start_done = false;
    worker_ = std::thread([this, &start_result, &start_done]() noexcept {
        if (async_)
            configure_audio_thread(is_capture() ? "ae-capture" : "ae-playback");

        const auto code = on_start();
        if (code)
        {
            open_queue(async_);
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
    throw_if_error(start_result);
}

void ActiveBackend::stop() noexcept
{
    if (!worker_.joinable())
        return;
    if (running())
    {
        const auto drain = !async_ && !is_capture();
        if (drain)
            close_playback_graph();

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

void ActiveBackend::unbind() noexcept
{
    if (!bound())
        return;
    stop();
    on_close();
    std::vector<std::uint8_t>().swap(device_bytes_);
    release();
}

void ActiveBackend::go_offline(RetCode error) noexcept
{
    close_queue();
    asrc_.reset();
    has_input_ = false;
    record_io_error(error);
}

void ActiveBackend::record_io_error(RetCode error) noexcept
{
    if (!error && error != RetCode::CANCELLED)
        last_error_.store(error, std::memory_order_release);
}

Result<std::size_t> ActiveBackend::on_read(OBBlock) noexcept
{
    return RetCode::EPARAM;
}

RetCode ActiveBackend::on_write(IBBlock) noexcept
{
    return RetCode::EPARAM;
}

RetCode ActiveBackend::on_write_silence(OBBlock scratch) noexcept
{
    const auto bytes = scratch.byte_view();
    std::memset(bytes.data(), 0, bytes.size());
    return on_write(scratch);
}

void ActiveBackend::wait_backpressure() noexcept
{
    const auto wait = std::max(Microseconds(200), std::min(Microseconds(2000), endpoint_period() / 2));
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait_for(lock, wait, [this] { return worker_state_.load(std::memory_order_acquire) != State::Running; });
}

void ActiveBackend::ASRC::reset() noexcept
{
    filtered_error = 0;
    integral = 0;
    correction.store(0.0, std::memory_order_relaxed);
}

void ActiveBackend::ASRC::update(std::size_t queued_before, std::size_t queued_after, std::size_t target_frames,
                           BandWidth logical_rate, std::size_t device_frames, BandWidth device_rate) noexcept
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
    const auto bounded = std::max(-max_asrc_correction, std::min(max_asrc_correction, proportional + integral));
    correction.store(bounded, std::memory_order_relaxed);
}

OBBlock ActiveBackend::device_block(std::size_t frames) noexcept
{
    return {device_bytes_.data(), device_spec(), frames};
}

RetCode ActiveBackend::transfer_capture(std::size_t &device_frames) noexcept
{
    const auto batch = device_block(device_max_frames());
    const auto read = on_read(batch);
    if (!read)
        return read.ret_code();
    device_frames = read.value();
    if (device_frames > batch.frames())
        return RetCode::IOFAILED;
    if (!device_frames)
        return {};

    has_input_ = true;
    const auto primed = async_ && steady();
    const auto queued_before = primed ? available() : 0;
    decode_to_capture(device_block(device_frames), asrc_.scale());
    if (primed)
        asrc_.update(queued_before, available(), target_frames(), logical_spec().bandwidth,
                     device_frames, batch.spec().bandwidth);
    return {};
}

Result<std::size_t> ActiveBackend::next_playback_batch() noexcept
{
    const auto batch = on_wait_writable();
    if (!batch)
        return batch;
    const auto frames = batch.value();
    if (!frames || frames > device_max_frames())
        return RetCode::IOFAILED;
    return batch;
}

RetCode ActiveBackend::emit_playback(IFBlock input, std::size_t device_frames, double scale) noexcept
{
    const auto batch = device_block(device_frames);
    const auto samples = encode_from_playback(input, batch, scale);
    if (!samples.frames())
        return async_ ? on_write_silence(batch) : RetCode{};

    if (async_ && samples.frames() < device_frames)
    {
        const auto bytes = batch.byte_view();
        const auto encoded = samples.byte_view().size();
        std::memset(bytes.data() + encoded, 0, bytes.size() - encoded);
        return on_write(batch);
    }
    return on_write(samples);
}

RetCode ActiveBackend::transfer_playback_async(std::size_t &device_frames) noexcept
{
    const auto batch = next_playback_batch();
    if (!batch)
        return batch.ret_code();
    device_frames = batch.value();
    const auto scale = asrc_.scale();
    const auto requested = required_input(device_frames, scale);
    const auto primed = steady();
    const auto queued_before = primed ? available() : 0;
    const auto logical = logical_block(requested);
    const auto received = read_playback(logical);
    const auto result = received || (!requested && primed)
                            ? emit_playback(logical.subblock(0, received), device_frames, scale)
                            : on_write_silence(device_block(device_frames));
    if (result && primed)
        asrc_.update(queued_before, available(), target_frames(), logical_spec().bandwidth,
                     device_frames, device_spec().bandwidth);
    return result;
}

RetCode ActiveBackend::run_sync_capture() noexcept
{
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        if (capture_free_frames() < logical_frames())
        {
            wait_backpressure();
            continue;
        }
        std::size_t frames = 0;
        const auto result = transfer_capture(frames);
        if (!result)
            return result == RetCode::EOS ? drain_capture() : result;
        if (!frames)
            wait_backpressure();
    }
    return {};
}

RetCode ActiveBackend::drain_capture() noexcept
{
    auto tail = has_input_ ? tail_frames() : 0;
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        if (!tail && !available())
            return RetCode::EOS;
        if (tail && capture_free_frames() >= logical_frames())
        {
            const auto frames = std::min(tail, device_max_frames());
            flush_capture_tail(frames);
            tail -= frames;
        }
        else
            wait_backpressure();
    }
    return {};
}

RetCode ActiveBackend::run_sync_playback() noexcept
{
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        const auto batch = next_playback_batch();
        if (!batch)
            return batch.ret_code();
        const auto requested = required_input(batch.value());

        while (available() < requested && worker_state_.load(std::memory_order_acquire) == State::Running)
            wait_backpressure();
        if (worker_state_.load(std::memory_order_acquire) != State::Running)
            break;

        const auto logical = logical_block(requested);
        const auto received = read_playback(logical);
        assert(received == requested);
        has_input_ |= received != 0;
        const auto result = emit_playback(logical, batch.value(), 1.0);
        if (!result)
            return result;
    }
    return worker_state_.load(std::memory_order_acquire) == State::Draining ? drain_playback() : RetCode{};
}

RetCode ActiveBackend::drain_playback() noexcept
{
    auto tail = has_input_ || available() ? tail_frames() : 0;
    for (;;)
    {
        if (!tail && !available())
        {
            // Upsampling can still have output at the current phase after consuming the last input frame.
            if (required_input(1))
                return {};
        }
        const auto batch = next_playback_batch();
        if (!batch)
            return batch.ret_code();
        const auto requested = required_input(batch.value());
        const auto logical = logical_block(requested);
        const auto received = read_playback(logical);
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

RetCode ActiveBackend::run_async() noexcept
{
    const auto timeout = io_timeout();
    io_deadline_ = Clock::now() + timeout;
    while (worker_state_.load(std::memory_order_acquire) == State::Running)
    {
        std::size_t frames = 0;
        const auto result = is_capture() ? transfer_capture(frames) : transfer_playback_async(frames);
        if (!result)
            return result;

        const auto now = Clock::now();
        if (now >= io_deadline_)
            return RetCode::TIMEOUT;
        if (frames)
            io_deadline_ = now + timeout;
    }
    return {};
}

void ActiveBackend::run() noexcept
{
    auto error = async_ ? run_async() : (is_capture() ? run_sync_capture() : run_sync_playback());
    on_stop();
    if (worker_state_.load(std::memory_order_acquire) == State::Stopping)
        error = {};
    go_offline(error);
    worker_state_.store(State::Stopped, std::memory_order_release);
}

} // namespace aereborn
