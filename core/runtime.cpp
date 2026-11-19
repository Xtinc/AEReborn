#include "runtime.h"

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <algorithm>
#include <chrono>
#include <thread>

namespace aereborn
{

thread_local const void *engine_execution_context = nullptr;

namespace
{
// Scheduling and timer allocation are outside the Engine tick.
class PeriodicTask final : public RuntimeTask
{
  public:
    PeriodicTask(EngineSpec spec, std::function<void()> run)
        : period_(static_cast<std::uint32_t>(spec.block_period())),
          deadline_(std::chrono::steady_clock::now() + period_), run_(std::move(run))
    {
        schedule();
        thread_ = std::thread([this] { context_.run(); });
    }
    ~PeriodicTask() override
    {
        context_.stop();
        thread_.join();
    }

  private:
    void schedule()
    {
        timer_.expires_at(deadline_);
        timer_.async_wait([this](const asio::error_code &error) {
            if (error || context_.stopped())
            {
                return;
            }
            run_();
            if (!context_.stopped())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto lateness = now - deadline_;
                deadline_ += period_ * (lateness >= period_ ? lateness / period_ + 1 : 1);
                schedule();
            }
        });
    }

    asio::io_context context_;
    asio::steady_timer timer_{context_};
    const std::chrono::milliseconds period_;
    std::chrono::steady_clock::time_point deadline_;
    std::function<void()> run_;
    std::thread thread_;
};

class TimerDriver final : public ClockDriver
{
  public:
    RetCode start(EngineSpec spec, Tick tick, void *context) override
    {
        task_ = start_runtime_task(spec, [tick, context] { tick(context); });
        return {};
    }
    void stop() noexcept override
    {
        task_.reset();
    }

  private:
    std::unique_ptr<RuntimeTask> task_;
};
} // namespace

std::unique_ptr<ClockDriver> make_timer_driver()
{
    return std::make_unique<TimerDriver>();
}

std::unique_ptr<RuntimeTask> start_runtime_task(EngineSpec spec, std::function<void()> run)
{
    return std::make_unique<PeriodicTask>(spec, std::move(run));
}

ObserverDelivery::ObserverDelivery(EngineSpec spec, std::size_t channels, ObserverOptions options,
                                   ObserverCallback callback)
    : spec_(spec), channels_(channels), frames_(options.frames), callback_(std::move(callback)),
      queue_(options.queue_blocks), scratch_(channels * spec.block_frames()), delivery_(channels * options.frames)
{
    for (auto &slot : queue_.slots())
    {
        slot.samples.resize(scratch_.size());
    }
}

ObserverDelivery::~ObserverDelivery()
{
    stop();
}

bool ObserverDelivery::fits(std::size_t channels, std::size_t block_frames, ObserverOptions options)
{
    const auto max_frames = std::vector<float>().max_size() / channels;
    return options.frames <= max_frames && block_frames <= max_frames &&
           options.queue_blocks < std::vector<Packet>().max_size();
}

bool ObserverDelivery::lock_free() const noexcept
{
    return queue_.lock_free() && enabled_.is_lock_free() && first_frame_.is_lock_free();
}

void ObserverDelivery::start(const void *context)
{
    if (!enabled_.load())
    {
        return;
    }
    worker_ = start_runtime_task(spec_, [this, context] {
        const EngineExecutionScope scope(context);
        deliver();
    });
    running_.store(true);
}

void ObserverDelivery::request_stop() noexcept
{
    running_.store(false);
}

void ObserverDelivery::stop() noexcept
{
    request_stop();
    worker_.reset();
    queue_.clear_when_idle();
    filled_ = 0;
    expected_frame_ = no_frame;
}

void ObserverDelivery::disable() noexcept
{
    enabled_.store(false);
}

void ObserverDelivery::reset_when_idle() noexcept
{
    first_frame_.store(no_frame, std::memory_order_relaxed);
    delivered_end_ = no_frame;
}

void ObserverDelivery::publish(IBlock block, std::uint64_t frame) noexcept
{
    if (!enabled_.load())
    {
        return;
    }
    if (first_frame_.load(std::memory_order_relaxed) == no_frame)
    {
        first_frame_.store(frame, std::memory_order_relaxed);
    }
    if (auto *slot = queue_.write_slot())
    {
        slot->frame = frame;
        for (std::size_t ch = 0; ch < channels_; ++ch)
        {
            std::copy_n(block.channel(ch), block.frames(), slot->samples.data() + ch * block.frames());
        }
        queue_.publish();
    }
}

void ObserverDelivery::deliver()
{
    const auto end = queue_.published();
    const auto block_frames = spec_.block_frames();
    while (const auto *slot = queue_.read_slot(end))
    {
        if (!running_.load() || !enabled_.load())
        {
            return;
        }
        const auto frame = slot->frame;
        std::copy(slot->samples.begin(), slot->samples.end(), scratch_.begin());
        queue_.consume();
        if (frame != expected_frame_)
        {
            filled_ = 0;
        }
        expected_frame_ = frame + block_frames;
        for (std::size_t offset = 0; offset < block_frames;)
        {
            if (filled_ == 0)
            {
                delivery_start_ = frame + offset;
            }
            const auto count = std::min(frames_ - filled_, block_frames - offset);
            for (std::size_t ch = 0; ch < channels_; ++ch)
            {
                std::copy_n(scratch_.data() + ch * block_frames + offset, count,
                            delivery_.data() + ch * frames_ + filled_);
            }
            offset += count;
            filled_ += count;
            if (filled_ != frames_)
            {
                continue;
            }
            if (!running_.load() || !enabled_.load())
            {
                return;
            }
            const auto previous_end =
                delivered_end_ == no_frame ? first_frame_.load(std::memory_order_relaxed) : delivered_end_;
            const Observation packet{{delivery_.data(), channels_, frames_, frames_},
                                     delivery_start_, delivery_start_ - previous_end};
            try
            {
                callback_(packet);
            }
            catch (...)
            {
                disable();
                return;
            }
            delivered_end_ = delivery_start_ + frames_;
            filled_ = 0;
        }
    }
}

} // namespace aereborn
