# AEReborn Engine 重构修改计划（阻塞 commit 版）

本计划基于以下已敲定的架构决策整理，面向 `core/engine.h`、`core/engine.cpp` 以及配套示例、文档与单元测试：

1. **阻塞式 `commit()`**：将 `Engine::prepare()` 更名为 `Engine::commit()`；停机态立即生效，运行态阻塞等待时钟线程接管新计划并同步析构旧计划，移除 `Engine::reclaim()`。
2. **仅使用内置软件时钟**：不引入外部时钟接口，保留 `Clock` 对 `Engine` 的直接协作关系。
3. **方案 B（零轮询条件变量唤醒）**：`tick()` 返回本周期是否完成计划切换，`Clock` 复用原有 `mutex_` 触发条件变量通知，时钟线程复用块末的加锁操作，控制线程通过条件变量等待。
4. **补齐时钟线程防重入保护**：阻止节点 `process()` 回调同步调用 `Engine` 控制接口导致死锁或自 `join` 崩溃。

---

## 1. 变更总览

| 维度 | 当前实现 | 改造后 |
| :--- | :--- | :--- |
| **公开接口** | `prepare()`（非阻塞，槽满返 `BufferFull`）+ `reclaim()` | `commit()`（停机态立即生效，运行态阻塞至切换完成并同步析构旧计划）；删除 `reclaim()` |
| **`Runner` 状态** | `current_` + `pending_` + `submitted_` + `rendering_` + `reclaim()` + `drain_when_stopped()` | 仅保留 `current_` + `submitted_` + `rendering_`；删去 `pending_`、`has_pending()`、`reclaim()`、`drain_when_stopped()` |
| **运行态切换同步** | 异步提交，调用方无法可靠确认生效时机 | **方案 B**：`tick()` 返回 `bool swapped`，`Clock` 复用现有锁通知 `applied_.notify_one()` |
| **防重入保护** | 无运行时检查，`process()` 内误调 `commit()` 会死锁、调 `stop()` 会自 `join` 崩溃 | `Clock` 维护 `thread_local` 标记，`Engine` 所有控制入口检测时钟线程并返回 `ErrorCode::Busy` |
| **组件持有方式** | `Graph`、`Runner`、`Clock` 通过 `unique_ptr` 持有 | 继续以 `unique_ptr` 持有 |

---

## 2. `core/engine.h` 修改计划

### 2.1 公开接口与 Doxygen 注释调整

* `RetCode prepare();` 改名为 `RetCode commit();`：
  * 停机态（`Unprepared` / `Ready`）立即构建并生效，不阻塞；
  * 运行态（`Running`）构建并提交后阻塞至下一个块边界接管，随后在控制线程同步析构旧计划及仅由其持有的节点。首个新块的处理可能仍在进行。
  * 草稿未改动时直接返回成功；不再因提交槽占用返回 `ErrorCode::BufferFull`。
* 删除 `void reclaim() noexcept;`。
* 更新 `Engine` 类及各控制方法注释：明确说明从节点 `process()` 回调中同步调用控制接口将返回 `ErrorCode::Busy`（仅 `block_period()` 与 `tick_position()` 允许并发读取）。

### 2.2 内部嵌套类精简接口

* **`Engine` 私有方法**：`bool tick() noexcept;`（返回本周期是否完成了新计划接管）。
* **`Graph`**：保持现有草稿编辑与只读接口，移除冗余外部依赖。
* **`Runner`**：
  * 公开方法精简为 `RetCode commit(Topology topology, BlockPeriod period, Clock *live_clock);`、`void reset(const Graph &draft) noexcept;` 和 `bool render(BlockPeriod period, std::uint64_t tick) noexcept;`。
  * `Runner::reset()` 在停机时重置当前计划持有且草稿已移除或替换的节点。草稿中的节点由 `Graph::reset_nodes()` 重置。
  * 私有成员精简为：`std::unique_ptr<Plan> current_;`、`std::atomic<Plan *> submitted_{nullptr};`、`Plan *rendering_ = nullptr;`。
* **`Clock`**：
  * 公开方法：`~Clock() { stop(); }`、`void start(Engine &engine);`、`void stop() noexcept;`、`submit_and_wait(submitted, plan)`。
  * 私有成员：`std::mutex mutex_;`、`std::condition_variable changed_;`、`std::condition_variable applied_;`、`bool stopping_ = false;`、`std::thread thread_;`。

---

## 3. `core/engine.cpp` 修改计划

### 3.1 方案 B：零轮询阻塞切换协议（`Clock` + `Runner`）

1. **`Runner::render()` 返回切换标记**：

~~~cpp
bool Engine::Runner::render(BlockPeriod period, std::uint64_t tick) noexcept
{
    bool swapped = false;
    if (auto *plan = submitted_.load(std::memory_order_acquire))
    {
        rendering_ = plan;
        // 时钟线程清零后，旧计划不再被任何执行路径访问
        submitted_.store(nullptr, std::memory_order_release);
        swapped = true;
    }
    rendering_->render(period, tick);
    return swapped;
}
~~~

2. **`Clock` 复用块末的加锁点发出条件变量通知，并补齐幂等 `stop()` 与析构保护**。

~~~cpp
void Engine::Clock::start(Engine &engine)
{
    stopping_ = false;
    const auto interval = std::chrono::milliseconds(static_cast<std::uint32_t>(engine.period_));
    const auto first_deadline = std::chrono::steady_clock::now() + interval;
    thread_ = std::thread([this, &engine, interval, deadline = first_deadline]() mutable {
        clock_engine = &engine;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!changed_.wait_until(lock, deadline, [this] { return stopping_; }))
        {
            lock.unlock();
            const bool swapped = engine.tick();
            lock.lock();
            if (swapped)
                applied_.notify_one();
            const auto lateness = std::chrono::steady_clock::now() - deadline;
            deadline += interval * (lateness >= interval ? lateness / interval + 1 : 1);
        }
    });
}

void Engine::Clock::stop() noexcept
{
    if (!thread_.joinable())
        return;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    thread_.join();
}

template <class Plan> void Engine::Clock::submit_and_wait(std::atomic<Plan *> &submitted, Plan *plan)
{
    std::unique_lock<std::mutex> lock(mutex_);
    submitted.store(plan, std::memory_order_release);
    applied_.wait(lock, [&submitted]() noexcept { return submitted.load(std::memory_order_acquire) == nullptr; });
}
~~~

* **无丢唤醒说明**：
  * 控制线程先获取互斥锁再发布计划。加锁失败时，新计划仍由本地指针持有，尚未发布给时钟。
  * 如果时钟已接管，控制线程检查谓词时直接返回。如果谓词为假并释放锁等待，时钟线程随后持同一把锁发出通知。控制线程离开等待后再销毁旧计划。

3. **`Runner::commit()` 同步生效并就地析构旧计划**：

~~~cpp
RetCode Engine::Runner::commit(Topology topology, BlockPeriod period, Clock *live_clock)
{
    auto plan = std::make_unique<Plan>();
    const auto code = build_plan(*plan, topology, period);
    if (!code)
        return code;

    plan->owners = std::move(topology.nodes);
    if (live_clock)
        live_clock->submit_and_wait(submitted_, plan.get());
    else
        rendering_ = plan.get();
    current_ = std::move(plan);
    return {};
}
~~~

### 3.2 时钟线程防重入守卫

`core/engine.cpp` 匿名命名空间声明线程局部标记。时钟使用新线程，线程入口设置一次，标记随线程退出销毁。

~~~cpp
thread_local const Engine *clock_engine = nullptr;
~~~

* `Engine::on_clock_thread() const noexcept` 直接比较 `clock_engine == this`。
* 在 `Engine` 的全部控制入口（`emplace_node`、`add_node`、`remove_node`、`replace_node`、`set_paused`、`node`、`connect`、`disconnect`、`commit`、`start`、`stop`、`reset`）开头统一校验 `if (on_clock_thread()) return ErrorCode::Busy;`。`emplace_node()` 在构造节点前检查。销毁 Engine 仍由控制侧串行执行。

### 3.3 `Engine` 编排方法精简

* `Engine` 继续通过 `unique_ptr` 持有组件。析构时先销毁 `Clock`，等线程退出后再销毁 `Runner` 和 `Graph`。
* `Engine::tick()`：

~~~cpp
bool Engine::tick() noexcept
{
    const auto tick = position_.load(std::memory_order_relaxed);
    const bool swapped = runner_->render(period_, tick);
    position_.store(tick + 1, std::memory_order_release);
    return swapped;
}
~~~

* `Engine::commit()`：

~~~cpp
RetCode Engine::commit()
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    if (!is_block_period(period_))
        return ErrorCode::InvalidArg;
    if (!graph_->dirty())
        return {};

    const auto code = runner_->commit(graph_->resolve(), period_, state_ == State::Running ? clock_.get() : nullptr);
    if (code)
    {
        graph_->mark_clean();
        if (state_ == State::Unprepared)
            state_ = State::Ready;
    }
    return code;
}
~~~

* `Engine::stop()`（因不存在未消费 `pending_`，移除 `drain_when_stopped()`）：

~~~cpp
RetCode Engine::stop() noexcept
{
    if (on_clock_thread())
        return ErrorCode::Busy;
    if (state_ == State::Running)
    {
        clock_->stop();
        state_ = State::Ready;
    }
    return {};
}
~~~

### 3.4 `Graph` 与 `Runner` 已完成的局部清理

1. **`Graph::resolve()` 合并为单遍遍历**：在 `visit()` 递归处理完 `draft.inputs`、向 `node_index` 和 `topology.nodes` 写入当前节点后，立即遍历 `draft.inputs` 填充 `topology.links`（未暂停的上游必然已在 `node_index` 中，暂停或未连接的上游查不到即写入 `Link{}`）。删除 `resolve()` 中的第二遍循环与重复的 `nodes_.find()`。
2. **`Graph::add_node()` / `replace_node()` 先校验后分配**：先检验输入和输出至少一侧非空，所有端口均通过 `valid()`（或 `same_ports()`），通过后再调用 `next_identifier()` 与 `std::make_shared<NodeEntry>`，失败路径零浪费。
3. **计划构建拆分**。`build_plan()` 惰性准备节点，再由 `measure_layout()` 计算容量和端口偏移，最后由 `bind_blocks()` 分配双池并绑定端口视图。输入、输出计数在每个节点处理完后累加。
4. **直接处理未连接端口**。拓扑解析为未连接端口写入空连线，跳过节点索引查找。

---

## 4. 端口块帧数

移除 `PortBlock::set_frames()`。含分数帧端口的计划用原缓冲地址、原端口规格和当前 tick 的帧数原地重建端口视图。

## 5. 外围同步与验证清单

### 5.1 示例与设计文档同步

* **`examples/clocked_graph.cpp`**：将 `checked(engine.prepare());` 改为 `checked(engine.commit());`。
* **`AudioEngine_Outline.md`**：
  * 将涉及 `Engine::prepare()` 的表述统一更新为 `Engine::commit()`（保留 `Node::prepare(BlockPeriod)` 不变）；
  * 删除 `reclaim()` 与运行态提交返回 `BufferFull` 的旧描述，明确 `commit()` 在运行态阻塞至下一块边界接管并同步析构旧计划；
  * 更新节点回调重入说明：从 `process()` 中同步调用 `Engine` 控制接口返回 `ErrorCode::Busy`。

### 5.2 单元测试同步与补充 (`tests/`)

* 批量将 `engine.prepare()` 替换为 `engine.commit()`，把回收用例改为验证同步提交。阻塞提交期间放行测试栅栏，验证后续块时再停在块边界。
* **核心回归用例**：
  1. **阻塞生效屏障与即时析构**：运行中替换或移除节点后调用 `commit()`，验证返回前旧实例已在控制侧析构。暂停场景验证停止处理及恢复时保留节点状态。
  2. **时钟线程防重入保护**：用测试节点在 `process()` 内依次调用 `engine.commit()`、`engine.stop()`、`engine.add_node()`，验证全部立即返回 `ErrorCode::Busy`，无死锁、无自 `join` 崩溃。
  3. **运行态 `commit()` 失败恢复**：运行中插入一个 `Node::prepare()` 失败的新节点并调用 `commit()`，验证立刻返回错误码、不阻塞卡死，且原有计划继续正常渲染。

---

## 6. 建议实施顺序

1. **重构 `core/engine.h` 与 `core/engine.cpp`**。改用阻塞式 `commit()`，实现方案 B 条件变量唤醒与时钟线程防重入守卫。
2. **同步外围代码与文档**。更新 `examples/clocked_graph.cpp`、`tests/` 及 `AudioEngine_Outline.md`。
3. **全量构建与回归**。在 Debug 和 Release 配置下执行完整 CTest。
