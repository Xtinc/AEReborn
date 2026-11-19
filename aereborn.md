# AEReborn 架构审查与改进方案（第二阶段迭代版）

本文档对照最新一版 `AudioEngine_Outline.md`、`CODING_GUIDELINES.md` 及当前源码，评估本轮架构演进的合理性，并按优先级列出待改进的设计点、具体 C++14 改造方法与验证路线图。

## 1. 本轮演进评估

相比上一版本，当前实现在以下核心架构点上取得了显著简化：

* **设备格式切换收口为统一的图生命周期**：移除了 Adapter 内部动态重配置的 `configure_pcm()`，改为构造期固定规格，通过 `set_paused()` + `replace_node()` + `prepare()` 在块边界统一切换，消除了运行中修改节点内部转换状态的复杂性。
* **消除每订阅独立线程**：将原先每订阅一个独立线程和 5 ms 轮询的结构，收敛为共享的 30 Hz 后台调度 + 双线程 `ObserverWorkers` 线程池，并移除了 `render_epoch` 自旋等待。
* **热路径基础纪律保持良好**：音频渲染路径仍维持零堆分配与平铺式内存布局，`ChMatrix::mix_ramp_add()` 已把末帧判断移出主循环。

以下按优先级整理当前实现中仍存在隐患、冗余或阻碍第 3~5 阶段演进的问题，并给出具体落地方法。

---

## 2. P0：高优先级运行时与并发隐患

### 2.1 `ObserverDelivery` 回归无锁 SPSC 并补全排空循环

#### 现状与问题
* **涉及文件**：`core/runtime.cpp`、`core/runtime.h`、`core/engine.h`
* **问题 1（锁竞争导致的正常负载假丢帧）**：当前 `ObserverDelivery` 使用 `buffer_mutex_` 保护 `PreparedFormat`，音频线程 `write()` 采用 `try_to_lock`，抢锁失败即直接丢块；工作线程 `deliver()` 在持锁期间执行整批 `memcpy` 拼包。由于 30 Hz 后台调度与 5 ms 音频周期相位持续漂移，只要 `read()` 拷贝与 tick 重叠，即便队列远未写满也会产生假丢帧。
* **问题 2（默认容量与轮询周期余量过窄）**：`ObserverOptions::queue_blocks` 默认值为 8，在 5 ms 周期下总容量仅 40 ms，扣除 33.33 ms 的 30 Hz 轮询周期后，留给定时器抖动与线程池调度的余量只剩约 6.7 ms（约 1.3 个块）。
* **问题 3（缺口后的级联溢出）**：`PreparedFormat::read()` 遇到帧号不连续时只取第一段连续批次便返回，而 `deliver()` 未循环排空队列。剩余块需再等待 33.33 ms 才能被下次调度取走，极易触发二次溢出。

#### 改进方法
利用 `Engine` 已建立的生命周期不变量（`configure()` 与 `clear_when_idle()` 仅在写入端已脱离当前计划且回调结束后调用，`unsubscribe()` 仅置状态而不立即销毁 `buffer_`），可将 `PreparedFormat` 改回彻底无锁的 SPSC 环形缓冲：

1. **移除 `buffer_mutex_`，在 `PreparedFormat` 中改用带缓存行隔离的无锁读写游标**：
   * 分配 `capacity + 1` 个环槽（多 1 槽用于区分满与空，复用 `SpscRing` 思路），后半段仍保持 `capacity * block_units` 的连续回调视图区。
   * 写入端（音频线程）与读取端（Worker 线程）各独占一个游标，使用 `alignas(64)` 避免伪共享：

~~~cpp
alignas(64) std::atomic<std::size_t> head_{0}; // 仅音频线程写，Worker/调度线程 acquire 读
alignas(64) std::atomic<std::size_t> tail_{0}; // 仅 Worker 线程写，音频线程 acquire 读
~~~

   * `write()` 逻辑：先 `relaxed` 读 `head`，`acquire` 读 `tail`；若 `next(head) == tail` 直接返回（真正满队列才丢弃）；写入数据和 `blocks[head]` 后，`head_.store(next(head), std::memory_order_release)`。
   * `has_data()` 逻辑：`head_.load(std::memory_order_acquire) != tail_.load(std::memory_order_acquire)`。
   * `read()` 逻辑：先 `relaxed` 读 `tail`，`acquire` 读 `head`；统计从 `tail` 开始的连续帧批次拷贝至回调视图区后，`tail_.store(new_tail, std::memory_order_release)`。

2. **在 `deliver()` 中循环排空，直到队列无数据或状态非 `Running`**：

~~~cpp
void ObserverDelivery::deliver()
{
    while (state_.load(std::memory_order_acquire) == State::Running)
    {
        Observation batch{};
        const auto frames = buffer_->read(batch);
        if (frames == 0)
        {
            break;
        }
        if (state_.load(std::memory_order_acquire) != State::Running)
        {
            break;
        }
        const auto previous_end =
            delivered_end_ == no_frame ? first_frame_.load(std::memory_order_acquire) : delivered_end_;
        batch.dropped_frames = batch.frame_position - previous_end;
        try
        {
            callback_(batch);
            delivered_end_ = batch.frame_position + frames;
        }
        catch (...)
        {
            disable();
            break;
        }
    }
}
~~~

3. **放宽默认缓冲余量**：
   * 在保持 30 Hz 共享定时器的前提下，建议将 `ObserverOptions::queue_blocks` 默认值提高至至少 16（5 ms 下为 80 ms），或支持传 0 表示由引擎按 `ceil(100ms / block_period)` 自动计算安全容量。

---

### 2.2 加固 `ObserverWorkers` 的任务停止屏障

#### 现状与问题
* **涉及文件**：`core/runtime.cpp`、`core/runtime.h`
* **问题**：`ObserverWorkers::schedule()` 通过 `scheduled_.exchange(true)` 占位后，使用 `asio::post()` 将任务投递到 2 线程的 `asio::thread_pool`。当两个 worker 都被慢回调占用时，新投递的任务会滞留在线程池队列中，此时尚未获取 `callback_mutex_`。
* 若此时调用 `unsubscribe()` -> `cancel()` -> `wait_for_callback()`，或局部暂停后准备恢复时调用 `wait_for_callback()`，仅靠锁一次 `callback_mutex_` 会立刻穿透返回，无法等待队列中“已投递但尚未启动”的旧任务退出。这会导致：
  * `unsubscribe()` 返回后，旧 lambda 仍持有 `shared_ptr<ObserverDelivery>` 并在后台延迟析构回调捕获对象；
  * 恢复期旧任务持续占据 `scheduled_ == true`，阻塞后续正常调度。

#### 改进方法
用显式的 `task_mutex_ + task_cv_ + task_pending_` 替代 `scheduled_` 原子变量与 `callback_mutex_` 的分离管理（调度与取消均在非音频线程执行，使用条件变量不影响实时性）：

1. **在 `ObserverDelivery` 中维护投递态**：

~~~cpp
std::mutex task_mutex_;
std::condition_variable task_cv_;
bool task_pending_ = false;
~~~

2. **在 `ObserverWorkers::schedule()` 中加锁检查并标记 `task_pending_`**（防止 `cancel()` 置 `Disabled` 后仍有新任务入队）：

~~~cpp
void ObserverWorkers::schedule(const std::shared_ptr<ObserverDelivery> &observer, const void *context) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(observer->task_mutex_);
        if (observer->state_.load(std::memory_order_acquire) != ObserverDelivery::State::Running ||
            observer->task_pending_ || !observer->buffer_->has_data())
        {
            return;
        }
        observer->task_pending_ = true;
    }
    try
    {
        asio::post(pool_->workers, [observer, context] {
            const EngineExecutionScope scope(context);
            observer->deliver();
            {
                const std::lock_guard<std::mutex> lock(observer->task_mutex_);
                observer->task_pending_ = false;
            }
            observer->task_cv_.notify_all();
        });
    }
    catch (...)
    {
        {
            const std::lock_guard<std::mutex> lock(observer->task_mutex_);
            observer->task_pending_ = false;
        }
        observer->task_cv_.notify_all();
    }
}
~~~

3. **`wait_for_callback()` 与 `cancel()` 完整等待在途与排队任务归零**：

~~~cpp
void ObserverDelivery::disable() noexcept
{
    state_.store(State::Disabled, std::memory_order_release);
}

void ObserverDelivery::wait_for_callback() noexcept
{
    std::unique_lock<std::mutex> lock(task_mutex_);
    task_cv_.wait(lock, [this] { return !task_pending_; });
}

void ObserverDelivery::cancel() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(task_mutex_);
        state_.store(State::Disabled, std::memory_order_release);
    }
    wait_for_callback();
}
~~~

* **死锁检查**：`deliver()` 异常路径调用的 `disable()` 不获取 `task_mutex_`；`Engine::prepare()` 在调用 `wait_for_callback()` 前已释放 `plans_mutex`；`unsubscribe()` 不持有 `plans_mutex`。不存在锁序反转风险。

---

## 3. P1：架构简化与边界收口

### 3.1 移除 `IAdapter` / `OAdapter` 构造函数的冗余 `EngineSpec` 参数

#### 现状与问题
* **涉及文件**：`plugins/adapter.h`、`plugins/adapter.cpp`
* **问题**：移除 `configure_pcm()` 后，所有转换器初始化均已统一由 `Node::prepare(const EngineSpec &graph_spec)` 触发。构造函数中保留的 `EngineSpec graph_spec_` 目前仅用于在 `prepare()` 时核对 `expected == graph`，既增加了构造样板，也违背 `CODING_GUIDELINES.md` 中“一项规格只保留一个定义来源”的约束。

#### 改进方法
* 将构造函数简化为：

~~~cpp
IAdapter(std::size_t channels, PCMSpec pcm, ChMatrix matrix = {});
OAdapter(std::size_t channels, PCMSpec pcm, ChMatrix matrix = {});
~~~

* 删除成员变量 `graph_spec_` 以及 `prepare_converter()` 中的 `expected` 参数，直接以 `Engine::prepare()` 传入的 `graph_spec` 作为唯一图规格来源。

---

### 3.2 消除 `IAdapter::process()` 的冗余拷贝与 `PCMConverter` 的引用隐患

#### 现状与问题
* **涉及文件**：`plugins/adapter.cpp`
* **问题 1（输入路径多一次 `memcpy` 与多余缓冲）**：`IAdapter::process()` 每周期先把借用的 `pcm_` 切片拷贝到 `PCMConverter::bytes_` 中再解码，目的仅仅是为了让 `pcm_block()` 在 tick 末尾返回该切片。但调用方借出的 `pcm_` 在整个 tick 期间本身就是有效的，无需额外分配和拷贝。
* **问题 2（隐蔽的悬空引用风险）**：`PCMConverter` 中 `spec_` 按值存储，而 `const ChMatrix &matrix_` 以常引用指向宿主 Adapter 的成员，容易在后续重构对象生命周期时埋下隐患。

#### 改进方法
1. **输入零拷贝**：
   * `PCMConverter` 仅在编码（`OAdapter`）方向需要内部输出字节缓冲 `bytes_`；`IAdapter` 直接在类内保存 `View<const std::uint8_t> last_slice_{nullptr, 0}`。
   * `IAdapter::process()` 仅计算本周期切片帧数与字节数，直接对 `pcm_.data()` 调用 `decode()`，同时记录 `last_slice_ = {pcm_.data(), size}` 供 `pcm_block()` 返回；缺输入时置 `last_slice_ = {nullptr, 0}`。
2. **统一 `matrix_` 传递方式**：
   * 将 `PCMConverter` 的 `const ChMatrix &matrix_` 改为在 `prepare_pcm()` / `decode()` / `encode()` 调用时显式传入，或改为非空指针 `const ChMatrix *matrix_` 并注明生命周期绑定于所属 Adapter。

---

### 3.3 收敛 `PortSpec` 与 `pcm_block()` 向 `core/` 的渗透

#### 现状与问题
* **涉及文件**：`core/types.h`、`core/graph.h`、`core/engine.cpp`
* **问题**：
  1. `PortSpec` 直接内嵌 `PCMSpec`，导致图内 `PlanarFloat` 端口带有无意义的 `periods = 1` 与未初始化的 `SampleRate{}`（0 Hz），必须经过 `Engine::resolve_port_format()` 事后修补。
  2. `IAdapter` / `OAdapter` 继承 `NodeT<1, 1>`，但 `build_plan()` 跳过 PCM 端口的缓冲分配，使 `ProcContext` 中混入空的占位 `Block`（`IAdapter` 的 `inputs[0]` 与 `OAdapter` 的 `outputs[0]` 为空）。
  3. `Node::pcm_block()` 与 `port.is_pcm()` 逻辑散落在 `core/engine.cpp` 的 5 处核心路径中（找根、算通道、建缓冲、连线校验、写订阅）。后续第 4/5 阶段引入文件或 Opus 编码网络节点时，单一的 `is_pcm()` 枚举会难以表达非 PCM 外部数据流。

#### 改进方法（提供两种渐进方案）
* **方案 A（轻量收口，适合当前阶段立即实施）**：
  * 明确约定 `PortSpec` 中 `sample_rate` 与 `periods` 仅在 `is_pcm()` 时有效，并把 `Engine` 内判定“是否为依赖遍历根”的逻辑抽为 `Node` 虚函数（例如 `virtual bool is_sink() const noexcept`，默认返回 `outputs().empty()`），消除 `build_plan()` 中对 `is_pcm()` 的硬编码推断。
* **方案 B（彻底解耦，适合第 3~4 阶段前一次性重构）**：
  * 保持图内端口（`inputs()/outputs()`）只描述 `PlanarFloat` 声道数，确保 `ProcContext` 中每个 `IBlock/OBlock` 永远有效、无空洞；
  * 将可观测的外部边界流抽象为独立的观测点/外部端口（例如 `Node::external_ports()` 或专门的 `Tap` 描述），使 `Engine` 核心图编译完全不感知 PCM 格式，同时自然兼容未来的 Opus / 文件字节流观测。

---

## 4. P2：稳健性加固与前瞻重构（含首轮遗留项）

### 4.1 消除 `Command` 中的 `PlanConnection *` 裸指针
* **涉及文件**：`core/engine.cpp`
* **现状**：`Command` 直接保存 `PlanConnection *connection` 裸指针，依赖“单生产者 + 最多 1 个 pending 计划”的隐式不变量保证不悬空。
* **改进方法**：落实 `AudioEngine_Outline.md` 第 3 节的“句柄 + 计划代际”设计：

~~~cpp
struct Command
{
    RenderPlan *plan = nullptr;
    PlanRevision revision = 0;
    std::size_t conn_index = 0;
    float gain = 0.0f;
};
~~~

* `set_parameter()` 写入目标计划的 `plan->revision` 与 `found->second`（连接索引）；
* 音频线程 `apply_commands()` 仅在 `active && active->revision == command->revision` 时更新 `active->connections[command->conn_index]`，彻底杜绝跨线程悬空指针风险。

### 4.2 `SpscRing` 消除伪共享
* **涉及文件**：`core/runtime.h`
* **改进方法**：为 `head_` 与 `tail_` 分别添加 `alignas(64)`，消除多核环境下控制/音频线程并发访问同一缓存行导致的无效化开销。

### 4.3 `ObserverDelivery` 边界校验与热路径早退
* **涉及文件**：`core/runtime.cpp`
* **改进点 1**：`ObserverDelivery::prepare_format()` 开头补充校验 `if (block_frames == 0) return ErrorCode::InvalidArg;`，防止无效引擎规格（如 44.1 kHz / 5 ms）在 `subscribe()` 时绕过检查创建零长度缓冲。
* **改进点 2**：`ObserverDelivery::write()` 将 `state_.load(std::memory_order_acquire) != State::Running` 判断提前到函数最开头，避免非运行态订阅提前写入 `first_frame_` 或触碰缓冲。

### 4.4 局部暂停/恢复时的防爆音（Click-Free）策略
* **涉及文件**：`core/engine.cpp`、`AudioEngine_Outline.md`
* **现状**：`set_paused(id, true)` 会把相关连接直接从 `RenderPlan` 中移除；恢复时因旧计划中已无该连接（`previous == no_connection`），增益会直接从 `0` 阶跃跳变至 `options.gain`。
* **改进方法**：
  * **短期约定**：在文档与上层服务中约定，有听感要求的局部暂停先 `set_muted(conn, true)` 等待 1 个块边界完成斜坡淡出后再暂停；
  * **引擎增强（可选）**：对新加入计划的连接（`previous == no_connection` 且当前处于运行态平滑切换 `smooth == true`），可将初始 `gain` 设为 `0.0f`、`target` 设为 `effective_gain(...)`，使其在恢复首块自动完成一次块内线性淡入。

### 4.5 抽离 `SincResampler` 并替换 `RenderPlan` 内的 `std::map`
* **涉及文件**：`plugins/adapter.cpp`、`core/engine.cpp`
* **抽离 `SincResampler`（面向第 3 阶段）**：当前 `PCMConverter` 将切片、编解码和固定整数比重采样强耦合在一起。建议将 `history_`、`kernel_`、`phase_`、`resample()` 拆分为独立内部类 `SincResampler`，并将步进改为高精度小数/定点步长，预留 `set_ratio_trim(double ppm)` 接口，为第 3 阶段真实多设备时钟漂移自适应补偿做好铺垫。
* **替换 `RenderPlan` 内的 `std::map`**：`node_index` 和 `connection_index` 每次 `prepare()` 和回收都会产生大量离散红黑树节点堆分配。改为有序 `std::vector<std::pair<Id, std::size_t>>`（构建完 `std::sort`，查找用 `std::lower_bound`），可将索引表压缩为单次连续分配。

---

## 5. 重构执行路线图

| 优先级 | 改动项 | 涉及文件 | 建议验证方式 |
| :--- | :--- | :--- | :--- |
| **P0** | `ObserverDelivery` 回归无锁 SPSC + 排空循环 + 默认队列余量调整 | `core/runtime.h`, `core/runtime.cpp`, `core/engine.h` | 在 `clocked_engine_tests.cpp` 增加高频写入 + 慢/不均匀消费 + 缺口后的连续性压测，验证无过载时零丢帧 |
| **P0** | `ObserverWorkers` 用 `task_pending_ + condition_variable` 加固停止屏障 | `core/runtime.h`, `core/runtime.cpp` | 增加 3 个以上订阅且前 2 个阻塞在慢回调时的 `unsubscribe()` 与暂停恢复测试，验证无任务遗留与死锁 |
| **P1** | 移除 `IAdapter`/`OAdapter` 构造期冗余 `EngineSpec`；消除 `IAdapter::process()` 输入 `memcpy` | `plugins/adapter.h`, `plugins/adapter.cpp` | 复用现有 `adapter`、`routing`、`device` 测试组并检查零拷贝切片正确性 |
| **P1** | 收敛 `PortSpec` 与 `is_pcm()` 在 `core/` 中的散落判断 | `core/types.h`, `core/graph.h`, `core/engine.cpp` | 复用 `core` 与 `runtime` 测试组 |
| **P2** | `Command` 代际索引化、`SpscRing` `alignas(64)`、`prepare_format`/`write` 边界修补 | `core/engine.cpp`, `core/runtime.h`, `core/runtime.cpp` | 补充无效 `EngineSpec` 下直接 `subscribe()` 返回 `InvalidArg` 的用例 |
| **P2** | 抽离 `SincResampler`、替换 `RenderPlan` 内的 `std::map`、恢复首块淡入评估 | `plugins/adapter.cpp`, `core/engine.cpp` | 复用 `resampling` 与 `allocation` 测试组 |