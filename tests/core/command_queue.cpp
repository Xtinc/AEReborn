#include "core/command_queue.h"
#include "core/engine.h"
#include "plugins/matrix.h"
#include "tests/support/test_harness.h"
#include "tests/support/tone.h"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace aereborn;
using namespace aereborn::test;
using namespace std::chrono_literals;
constexpr auto timeout = 3s;

class Gate
{
  public:
    void enter()
    {
        entered_.set_value();
        release_signal_.wait_for(timeout);
    }

    void wait() { check(entered_signal_.wait_for(timeout) == std::future_status::ready, "The worker enters the gate"); }
    void open() { release_.set_value(); }

  private:
    std::promise<void> entered_;
    std::promise<void> release_;
    std::future<void> entered_signal_ = entered_.get_future();
    std::future<void> release_signal_ = release_.get_future();
};

void command_queue_sync_and_post()
{
    struct Counter
    {
        Counter() = default;
        Counter(const Counter &) = delete;
        int &operator()()
        {
            ++value;
            return value;
        }
        int value = 0;
    } counter;

    std::vector<int> order;
    CommandQueue queue;
    check(!queue.on_thread(), "The submitting thread is not the worker");
    check(!queue.post({}), "An empty task is rejected");
    check(queue.sync([] { return NodeId::make(7); }) == NodeId::make(7), "sync returns a node ID");
    check(queue.sync([] { return std::string("ready"); }) == "ready", "sync returns a string");
    const auto item = queue.sync([value = std::make_unique<int>(42)]() mutable { return std::move(value); });
    check(item && *item == 42, "sync accepts a move-only callable and returns its result");

    struct Value
    {
        explicit Value(int number) : number(number) {}
        int number;
    };
    check(queue.sync([] { return Value(5); }).number == 5, "A result needs no default constructor");
    auto &result = queue.sync(counter);
    check(&result == &counter.value && result == 1, "External sync calls the original object by reference");
    queue.sync([&] {
        check(queue.on_thread(), "The worker identifies itself");
        order.push_back(1);
        check(queue.post([&] { order.push_back(4); }), "The worker can submit asynchronous tasks");
        auto &nested = queue.sync(counter);
        check(&nested == &counter.value && nested == 2, "Reentrant sync calls the same object");
        queue.sync([&] { order.push_back(2); });
        order.push_back(3);
    });
    check(queue.post([&] { order.push_back(5); }), "Submit a second asynchronous task");
    queue.sync([&] {
        check(order == std::vector<int>({1, 2, 3, 4, 5}), "Nested sync is inline and immediate tasks follow FIFO");
    });
    expect_error(RetCode::EPARAM, [&] { queue.sync([] { throw Error(RetCode::EPARAM); }); });
    struct CommandFailure : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    expect_exception<CommandFailure>([&] { queue.sync([] { throw CommandFailure("command failed"); }); });
}

void timer_order_and_cancel()
{
    Gate gate;
    std::promise<void> finished;
    auto complete = finished.get_future();
    std::vector<int> order;
    CommandQueue queue;
    queue.post([&] { gate.enter(); });
    gate.wait();
    const auto longest = CommandQueue::Duration::max();
    check(queue.post_after(longest, [&] { order.push_back(-1); }) != 0 &&
              queue.post_every(longest, [&] { order.push_back(-2); }) != 0,
          "Register long timers without overflowing the clock");
    check(queue.post_after(100ms, [&] { order.push_back(2); }) != 0, "Register the later timer first");
    const auto cancelled = queue.post_after(100ms, [&] { order.push_back(3); });
    check(cancelled != 0, "Register a timer to cancel");
    check(queue.post_after(-1ms, [&] {
              order.push_back(1);
              queue.cancel(cancelled);
          }) != 0,
          "Clamp negative delays to zero");
    check(queue.post_after(100ms, [&] { finished.set_value(); }) != 0, "Register the last timer");
    std::this_thread::sleep_until(CommandQueue::Clock::now() + 100ms);
    gate.open();
    check(complete.wait_for(timeout) == std::future_status::ready, "The last timer fires");
    queue.stop();
    check(order == std::vector<int>({1, 2}), "Overdue timers follow deadlines and long delays remain pending");
}

void timer_between_batches()
{
    Gate gate;
    std::promise<int> fired;
    auto at_fire = fired.get_future();
    int count = 0;
    CommandQueue::Task refill;
    CommandQueue queue;
    refill = [&] {
        if (++count < 20)
            queue.post(refill);
    };
    queue.post([&] {
        refill();
        gate.enter();
    });
    gate.wait();
    check(queue.post_after(0ms, [&] { fired.set_value(count); }) != 0, "Register an already-due timer");
    gate.open();
    check(at_fire.wait_for(timeout) == std::future_status::ready && at_fire.get() == 1,
          "An overdue timer gets a turn after one immediate batch even while the queue is refilled");
}

void periodic_cancellation()
{
    Gate running;
    std::promise<void> self_cancelled, repeated;
    auto cancelled = self_cancelled.get_future();
    auto unexpected = repeated.get_future();
    std::vector<int> values;
    bool completed = false;
    int calls = 0;
    CommandQueue::TimerId id = 0;
    CommandQueue queue;
    check(queue.post_after(0ms, {}) == 0 && queue.post_every(1ms, {}) == 0 &&
              queue.post_every(0ms, [] {}) == 0 && queue.post_every(-1ms, [] {}) == 0,
          "Reject empty timer tasks and nonpositive intervals");
    queue.sync([&] {
        id = queue.post_every(1ms, [&, count = 0]() mutable {
            values.push_back(++count);
            if (count == 3)
            {
                queue.cancel(id);
                self_cancelled.set_value();
            }
        });
        check(id != 0, "Register a self-cancelling timer");
    });
    check(cancelled.wait_for(timeout) == std::future_status::ready, "The mutable callback reaches its third firing");

    const auto external = queue.post_every(1ms, [&] {
        if (++calls == 1)
        {
            running.enter();
            completed = true;
        }
        else if (calls == 2)
            repeated.set_value();
    });
    check(external != 0, "Register a timer to cancel from another thread");
    running.wait();
    queue.cancel(external);
    running.open();
    queue.sync([&] { check(completed, "Cancellation lets the in-flight callback finish"); });
    check(unexpected.wait_for(40ms) == std::future_status::timeout, "External cancellation prevents rearming");
    queue.stop();
    check(values == std::vector<int>({1, 2, 3}), "The same callback state survives and self-cancellation stops it");
}

void pending_timer_cleanup()
{
    std::atomic<bool> reentered{false};
    CommandQueue queue;
    const auto release = [&]() noexcept {
        queue.cancel(1);
        queue.post([&] { reentered = true; });
    };
    auto guard = std::make_shared<ScopeExit<decltype(release)>>(release);
    const auto id = queue.post_after(1h, [guard = std::move(guard)] { (void)guard; });
    check(id != 0, "Register a delayed callback with an owned capture");
    queue.cancel(id);
    queue.sync([] {});
    check(reentered, "Releasing a pending timer can reenter cancel and post");
}

void command_queue_timers()
{
    timer_order_and_cancel();
    timer_between_batches();
    periodic_cancellation();
    pending_timer_cleanup();
}

void command_queue_stop_and_concurrency()
{
    Gate running;
    std::atomic<int> calls{0};
    std::atomic<int> sync_calls{0};
    std::atomic<bool> all_on_worker{true};
    std::vector<int> shutdown;
    shutdown.reserve(4);
    int periodic_calls = 0;
    CommandQueue queue;
    const auto command = [&] {
        if (!queue.on_thread())
            all_on_worker = false;
        ++calls;
    };
    std::vector<std::future<void>> producers;
    for (int i = 0; i < 4; ++i)
    {
        producers.push_back(std::async(std::launch::async, [&] {
            for (int n = 0; n < 100; ++n)
            {
                queue.sync(command);
                check(queue.post(command), "Concurrent post succeeds while the queue is open");
            }
        }));
    }
    for (auto &producer : producers)
        producer.get();
    queue.sync([] {});
    check(all_on_worker && calls == 800, "All producers' commands run on the queue worker");

    check(queue.post_every(1ms, [&] {
              if (++periodic_calls == 1)
                  running.enter();
          }) != 0,
          "Start a periodic callback to hold the worker during shutdown");
    running.wait();
    check(queue.post([&] { shutdown.push_back(1); }), "An immediate task is accepted before shutdown");
    const auto release = [&]() noexcept {
        const bool on_worker = queue.on_thread();
        const bool rejected = !queue.post([] {});
        shutdown.push_back(on_worker && rejected ? 2 : -2);
    };
    auto guard = std::make_shared<ScopeExit<decltype(release)>>(release);
    check(queue.post_after(0ms, [&, guard = std::move(guard)] {
              (void)guard;
              shutdown.push_back(-1);
          }) != 0,
          "An overdue timer is pending during shutdown");

    auto syncing = std::async(std::launch::async, [&] {
        try
        {
            queue.sync([&] { ++sync_calls; });
            return true;
        }
        catch (const Error &error)
        {
            check(error.code() == RetCode::ESTATE, "The racing sync gets the shutdown error");
            return false;
        }
    });
    auto stopping = std::async(std::launch::async, [&] {
        queue.stop([&] { shutdown.push_back(queue.sync([&] { return queue.on_thread() ? 3 : -3; })); });
    });
    const auto deadline = CommandQueue::Clock::now() + timeout;
    bool closed = false;
    while (CommandQueue::Clock::now() < deadline)
    {
        if (!queue.post([] {}))
        {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool waited = stopping.wait_for(0ms) == std::future_status::timeout;
    running.open();
    stopping.get();
    check(closed && waited && periodic_calls == 1, "Stop waits for the in-flight callback and prevents rearming");
    check(sync_calls == (syncing.get() ? 1 : 0), "A sync racing stop runs exactly once if accepted");
    check(shutdown == std::vector<int>({1, 2, 3}), "Stop drains, drops pending timers, then runs the cleanup");

    const auto verify_closed = [&] {
        check(!queue.on_thread(), "A stopped worker has no thread identity");
        expect_error(RetCode::ESTATE, [&] { queue.sync([] {}); });
        check(!queue.post([] {}) && queue.post_after(0ms, [] {}) == 0 && queue.post_every(1ms, [] {}) == 0,
              "A stopped queue rejects all external submissions");
    };
    verify_closed();
    std::async(std::launch::async, verify_closed).get();
    queue.stop([&] { shutdown.push_back(4); });
    check(shutdown.size() == 3, "Repeated stop ignores a new cleanup");
}

class Meter final : public NodeT<1, 0>
{
  public:
    Meter() : NodeT({PortSpec(BandWidth::Full, 1, SampleFormat::F32P)}, {}) {}
    void process(const ProcessContext &) noexcept override { ++blocks; }
    std::size_t blocks = 0;
};

void command_queue_engine_control()
{
    constexpr auto rate = BandWidth::Full;
    Engine engine(BlockPeriod::INR_05MS);
    NodeId matrix{}, meter{};
    ConnId output{};
    CommandQueue queue;
    queue.sync([&] {
        const auto tone = engine.emplace_node<Tone>(rate, 1);
        matrix = engine.emplace_node<Matrix>(rate, 1);
        meter = engine.emplace_node<Meter>();
        engine.connect(tone, matrix);
        output = engine.connect(matrix, meter);
        engine.commit();
        engine.start();
    });

    constexpr int edits = 8;
    std::vector<std::future<void>> producers;
    for (int i = 0; i < 4; ++i)
    {
        producers.push_back(std::async(std::launch::async, [&, i] {
            for (int n = 0; n < edits; ++n)
            {
                queue.sync([&] {
                    const auto generation = engine.plan_generation();
                    engine.node<Matrix>(matrix)->set_gain(0.25f * (i + 1));
                    engine.disconnect(output);
                    output = engine.connect(matrix, meter);
                    engine.commit();
                    check(engine.plan_generation() == generation + 1, "A graph edit publishes one plan");
                });
            }
        }));
    }
    for (auto &producer : producers)
        producer.get();

    queue.stop([&] { engine.stop(); });
    check(engine.plan_generation() == 1 + producers.size() * edits, "Every edit was committed");
    check(engine.tick_position() > 0 && engine.node<Meter>(meter)->blocks > 0, "The clock processed the graph");
    check(engine.state() == Engine::State::Ready, "The queue cleanup stopped the clock");
}
} // namespace

int main(int argc, char **argv)
{
    return aereborn::test::run_tests(argc, argv, {{"command_queue_sync_and_post", command_queue_sync_and_post},
                                                {"command_queue_timers", command_queue_timers},
                                                {"command_queue_stop_and_concurrency", command_queue_stop_and_concurrency},
                                                {"command_queue_engine_control", command_queue_engine_control}});
}
