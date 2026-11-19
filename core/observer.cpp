#include "observer.h"
#include "runtime.h"

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>
#include <cstring>
#include <utility>
#include <vector>

namespace aereborn
{

class Observer::PreparedFormat
{
  public:
    struct BlockInfo
    {
        std::uint64_t frame;
        std::size_t size;
    };

    PreparedFormat(PortSpec format, std::size_t block_frames, std::size_t block_units, std::size_t capacity)
        : format(format), block_frames(block_frames), block_units(block_units), blocks(capacity + 1)
    {
        // One spare ring slot distinguishes full from empty. The rest is callback storage.
        const auto units = (blocks.size() + capacity) * block_units;
        if (format.is_pcm())
        {
            pcm.resize(units);
        }
        else
        {
            audio.resize(units);
        }
    }

    void write(IBlock samples, View<const std::uint8_t> bytes, std::uint64_t frame) noexcept
    {
        const auto index = head.load(std::memory_order_relaxed);
        const auto next = index + 1 == blocks.size() ? 0 : index + 1;
        if (next == tail.load(std::memory_order_acquire))
        {
            return;
        }
        const auto offset = index * block_units;
        if (format.is_pcm())
        {
            if (bytes.empty() || bytes.size() > block_units)
            {
                return;
            }
            std::memcpy(pcm.data() + offset, bytes.data(), bytes.size());
            blocks[index].size = bytes.size();
        }
        else
        {
            for (std::size_t ch = 0; ch < samples.channels(); ++ch)
            {
                std::memcpy(audio.data() + offset + ch * block_frames, samples.channel(ch),
                            block_frames * sizeof(float));
            }
        }
        blocks[index].frame = frame;
        head.store(next, std::memory_order_release);
    }

    bool lock_free() const noexcept
    {
        return head.is_lock_free() && tail.is_lock_free();
    }

    std::size_t published() const noexcept
    {
        return head.load(std::memory_order_acquire);
    }

    bool has_data() const noexcept
    {
        const auto consumed = tail.load(std::memory_order_acquire);
        return consumed != published();
    }

    void clear() noexcept
    {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

    std::size_t read(std::size_t end, Observation &result) noexcept
    {
        const auto start = tail.load(std::memory_order_relaxed);
        if (start == end)
        {
            return 0;
        }
        result.frame_position = blocks[start].frame;
        result.port = format;

        const auto queued = end >= start ? end - start : blocks.size() - start + end;
        std::size_t batch = 1;
        while (batch < queued && blocks[(start + batch) % blocks.size()].frame ==
                                    result.frame_position + batch * block_frames)
        {
            ++batch;
        }
        const auto callback_offset = blocks.size() * block_units;
        if (format.is_pcm())
        {
            auto *destination = pcm.data() + callback_offset;
            std::size_t size = 0;
            for (std::size_t i = 0; i < batch; ++i)
            {
                const auto index = (start + i) % blocks.size();
                const auto bytes = blocks[index].size;
                std::memcpy(destination + size, pcm.data() + index * block_units, bytes);
                size += bytes;
            }
            result.pcm = {destination, size};
        }
        else
        {
            auto *destination = audio.data() + callback_offset;
            const auto channels = format.spec.channels;
            const auto frames = batch * block_frames;
            for (std::size_t i = 0; i < batch; ++i)
            {
                const auto *source = audio.data() + ((start + i) % blocks.size()) * block_units;
                for (std::size_t ch = 0; ch < channels; ++ch)
                {
                    std::memcpy(destination + ch * frames + i * block_frames, source + ch * block_frames,
                                block_frames * sizeof(float));
                }
            }
            result.audio = {destination, channels, frames, frames};
        }
        tail.store((start + batch) % blocks.size(), std::memory_order_release);
        return batch * block_frames;
    }

    const PortSpec format;

  private:
    const std::size_t block_frames;
    const std::size_t block_units;
    std::vector<BlockInfo> blocks;
    std::vector<float> audio;
    std::vector<std::uint8_t> pcm;
    // The producer publishes head after filling a slot. The consumer releases tail after copying out.
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

void Observer::DestroyFormat::operator()(PreparedFormat *format) const noexcept
{
    delete format;
}

Observer::Observer(ObservationPort port, ObserverCallback callback, ObserverOptions options)
    : port_(port), queue_blocks_(options.queue_blocks), callback_(std::move(callback))
{
}

Result<Observer::Format> Observer::prepare_format(EngineSpec spec, PortSpec format) const
{
    if (buffer_ && buffer_->format == format)
    {
        return Format{};
    }
    if (!callback_ || queue_blocks_ == 0)
    {
        return ErrorCode::InvalidArg;
    }
    const auto &pcm = format.spec;
    const auto block_frames = spec.block_frames();
    std::size_t frames = block_frames;
    std::size_t units_per_sample = 1;
    auto max_units = std::vector<float>().max_size();
    if (format.is_pcm())
    {
        const auto period = static_cast<std::uint32_t>(spec.block_period());
        frames = (std::uint64_t{static_cast<std::uint32_t>(pcm.sample_rate)} * period + 999) / 1000;
        units_per_sample = pcm_sample_bytes(pcm.format);
        max_units = std::vector<std::uint8_t>().max_size();
        if (!is_pcm_sample_rate(pcm.sample_rate) || units_per_sample == 0 || pcm.periods == 0)
        {
            return ErrorCode::InvalidArg;
        }
    }
    if (queue_blocks_ >= std::vector<PreparedFormat::BlockInfo>().max_size() ||
        pcm.channels > max_units / (2 * queue_blocks_ + 1) / frames / units_per_sample)
    {
        return ErrorCode::TooLarge;
    }

    Format prepared{new PreparedFormat(format, block_frames, frames * pcm.channels * units_per_sample, queue_blocks_)};
    if (!state_.is_lock_free() || !first_frame_.is_lock_free() || !prepared->lock_free())
    {
        return ErrorCode::Unsupported;
    }
    return prepared;
}

void Observer::configure(Format format) noexcept
{
    std::unique_lock<std::mutex> lock(task_mutex_);
    wait_for_delivery(lock);
    if (format)
    {
        buffer_.swap(format);
    }
    else
    {
        buffer_->clear();
    }
}

void Observer::write(IBlock samples, std::uint64_t frame) noexcept
{
    write(samples, {nullptr, 0}, frame);
}

void Observer::write(View<const std::uint8_t> bytes, std::uint64_t frame) noexcept
{
    write({}, bytes, frame);
}

void Observer::start() noexcept
{
    auto idle = State::Idle;
    state_.compare_exchange_strong(idle, State::Running);
}

void Observer::request_stop() noexcept
{
    auto running = State::Running;
    state_.compare_exchange_strong(running, State::Idle);
}

void Observer::wait_for_delivery(std::unique_lock<std::mutex> &lock) noexcept
{
    // Queued jobs can be behind unrelated callbacks. Invalidate them without waiting for a worker.
    if (task_ == Task::Queued)
    {
        task_ = Task::Stale;
    }
    delivery_finished_.wait(lock, [this] { return task_ != Task::Delivering; });
}

void Observer::clear_when_idle() noexcept
{
    if (buffer_)
    {
        buffer_->clear();
    }
}

void Observer::cancel() noexcept
{
    std::unique_lock<std::mutex> lock(task_mutex_);
    state_.store(State::Disabled);
    wait_for_delivery(lock);
}

void Observer::reset_when_idle() noexcept
{
    first_frame_.store(no_frame, std::memory_order_relaxed);
    delivered_end_ = no_frame;
}

void Observer::write(IBlock samples, View<const std::uint8_t> bytes, std::uint64_t frame) noexcept
{
    if (state_.load(std::memory_order_acquire) != State::Running)
    {
        return;
    }
    if (first_frame_.load(std::memory_order_relaxed) == no_frame)
    {
        first_frame_.store(frame, std::memory_order_relaxed);
    }
    buffer_->write(samples, bytes, frame);
}

void Observer::run_task() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(task_mutex_);
        if (task_ != Task::Queued || state_.load() != State::Running)
        {
            task_ = Task::Idle;
            return;
        }
        task_ = Task::Delivering;
    }
    deliver();
    {
        const std::lock_guard<std::mutex> lock(task_mutex_);
        task_ = Task::Idle;
    }
    delivery_finished_.notify_one();
}

void Observer::deliver() noexcept
{
    const auto end = buffer_->published();
    while (state_.load() == State::Running)
    {
        Observation batch{};
        const auto frames = buffer_->read(end, batch);
        if (frames == 0 || state_.load() != State::Running)
        {
            break;
        }
        const auto previous_end = delivered_end_ == no_frame ? first_frame_.load(std::memory_order_relaxed)
                                                           : delivered_end_;
        batch.dropped_frames = batch.frame_position - previous_end;
        try
        {
            callback_(batch);
            delivered_end_ = batch.frame_position + frames;
        }
        catch (...)
        {
            state_.store(State::Disabled);
            break;
        }
    }
}

struct ObserverWorkers::Pool
{
    asio::thread_pool workers{2};
};

ObserverWorkers::ObserverWorkers() : pool_(std::make_unique<Pool>())
{
}

ObserverWorkers::~ObserverWorkers()
{
    pool_->workers.join();
}

void ObserverWorkers::schedule(const std::shared_ptr<Observer> &observer, const void *context) noexcept
{
    using Task = Observer::Task;
    const std::lock_guard<std::mutex> lock(observer->task_mutex_);
    auto &task = observer->task_;
    if (observer->state_.load() != Observer::State::Running || task == Task::Queued ||
        task == Task::Delivering || !observer->buffer_->has_data())
    {
        return;
    }
    const bool already_posted = task == Task::Stale;
    task = Task::Queued;
    if (already_posted)
    {
        return; // The next delivery can reuse the handler left in the pool by reconfiguration.
    }
    try
    {
        asio::post(pool_->workers, [weak = std::weak_ptr<Observer>(observer), context] {
            if (const auto observer = weak.lock())
            {
                const EngineExecutionScope scope(context);
                observer->run_task();
            }
        });
    }
    catch (...)
    {
        task = Task::Idle;
    }
}

} // namespace aereborn
