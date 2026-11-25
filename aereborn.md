# AEReborn 静音标识与空块早退优化计划

本方案参考 PipeWire（`SPA_CHUNK_FLAG_EMPTY`）与 GStreamer（`GST_BUFFER_FLAG_GAP`）的成熟设计，在不污染 `ByteBlock` 图外 I/O 视图、不引入业务逻辑到 `core/` 的前提下，为 AEReborn 音频图增加 **1 字节数据存在位（Active Flag）自动传播机制**，并基于该标识在内置插件上落地 **空块早退优化（Empty-Block Early Exit）**。

---

## 1. 背景与优化目标

在固定周期软件时钟引擎中，`Engine::Clock` 会按 2--40 ms 的固定节拍持续驱动整张生效渲染计划（`Plan`），导致当前存在两类问题：

1. **语义混淆（阻塞自动优先级闪避）**：当上游 `Capture` 处于离线、预填充、队列读空（如 `PushSource` 停推、WAV 播完）或端口未连接/暂停时，引擎与端点会向缓冲填零静音。下游节点面对全 `0.0f` 样本，无法区分“**通话中对方静音但仍在推全零包**”与“**通话已挂断/文件已播完**”。
2. **空闲算力浪费与次正规数隐患**：待机中的多路网络接收槽位、静音支路或未推流的 `PushSource` 若串联了跨采样率 `Convert`（96 抽头多相 Sinc 卷积）或多路 `Mixer`，每拍仍会对全零数据执行完整编解码、卷积与浮点乘加；对于未来的 IIR / 混响节点，持续处理全零输入还容易触发 IEEE 754 次正规浮点数（Denormal）性能退化。

### 业界参考与 AEReborn 取舍

| 维度 | GStreamer (`GST_BUFFER_FLAG_GAP`) | PipeWire (`SPA_CHUNK_FLAG_EMPTY`) | AEReborn 本方案设计 |
| --- | --- | --- | --- |
| **标志载体** | `GstBuffer::flags` | `spa_chunk::flags` | `ProcessContext` 中的 `input_active` / `output_active` 视图 |
| **为何不放进 `ByteBlock`** | — | 块头与图强绑定 | `ByteBlock` 被 `WavFile`、`PushSource`、ALSA/WASAPI 等图外模块广泛复用，放入 `ProcessContext` 可保证图外接口零污染，且不破坏 `View<const OBBlock>` 的只读容器契约 |
| **默认透传方式** | 框架基类自动复制 metadata | 端口共享 `spa_chunk` + `audioadapter` 转发 | `Engine::Runner::Plan::render()` 在每步 `process()` 前集中接力并写入默认值，**用户自定义节点一行不改即自动透传** |
| **多输入汇聚规则** | 全 GAP 才输出 GAP | 全 EMPTY 才输出 EMPTY | 任意未静音输入为活跃，则输出活跃（逻辑或） |

---

## 2. 五条核心不变式（设计红线）

落地过程中必须严格守住以下五条契约，避免引入隐蔽的音频瑕疵或回归：

1. **存在位语义与样本值彻底解耦**
   * `active == 1`：上游本拍交付了真实音频流（**即使所有样本恰好都是 `0.0f`**，例如通话对方静音或 WAV 文件内的静音段）。
   * `active == 0`：本拍无真实数据流（端口未连接、上游被 `set_paused(true)` 排除、`Capture` 离线/预填充/读空、`Matrix` 稳态零增益关断、或 `Mixer` 全部未静音输入均为空）。
2. **静音内存安全兜底（Silence Memory Invariant）**
   * 即使某节点将 `output_active` 置为 `0` 并走早退路径，**仍必须将输出块清零（`clear()` 或 `std::memset(0)`）**。
   * 原因：`float_pool` 与 `byte_pool` 在每个 tick 间复用；不关心存在位的用户自定义节点或 `Playback` 会直接读取样本区，若跳过清零会导致上一拍残余样本循环泄漏。对单拍数 KB 的 L1 缓存块执行一次 `memset` 耗时仅纳秒级，换来绝对安全的内存不变式。
3. **热路径零堆分配、零锁、`noexcept`**
   * `input_active`、`output_active` 与 `input_sources` 全部在 `commit()` 阶段随 `Plan` 一次性分配完毕；`Plan::render()` 仅做顺序数组下标访问。
4. **对既有代码与直接单测 100% 向后兼容**
   * `ProcessContext` 保持 C++14 聚合类型（Aggregate），不引入用户自定义构造函数。既有单测直接构造 4 字段 `ProcessContext{period, tick, inputs, outputs}` 时，末尾两个 `View` 自动值初始化为空视图；`has_input()` 对空视图默认返回 `true`，`set_output_active()` 对空视图安全空操作。
5. **汇端 `Playback` 必须照常消费静音块**
   * `Playback::process()` **禁止**因 `has_input(0) == false` 而跳过写入 `FrameRing`。
   * 原因：录音文件（`WavPlayback`）与周期回调（`UsrPlayback`）需要连续时间线，异步声卡（ALSA / WASAPI）的 ASRC PI 闭环依赖稳定入队维持 $50\%$ 队列水位。

---

## 3. 内核改造设计（`core/`）

### 3.1 `core/types.h`：扩展 `ProcessContext`

在 `ProcessContext` 末尾追加两个轻量只读/可写字节视图，并提供语义清晰的访问方法：

```cpp
/// @brief Timing, port blocks, and per-port active flags passed to a node for one process call.
/// @note All views and backing buffers are borrowed for the duration of that call.
struct ProcessContext
{
    BlockPeriod block_period;             ///< Duration of the current engine tick.
    std::uint64_t tick_position;          ///< Zero-based position of the current tick.
    View<const IBBlock> inputs;           ///< Read-only input blocks in port index order.
    View<const OBBlock> outputs;          ///< Output blocks with writable samples in port index order.
    View<const std::uint8_t> input_active;  ///< Nonzero when an input carries real stream data.
    View<std::uint8_t> output_active;       ///< Output active flags initialized to engine defaults.

    /// @brief Check whether an input port carries real stream data on this tick.
    /// @note Returns true when input_active is omitted (for direct standalone node calls).
    bool has_input(std::size_t index) const noexcept
    {
        return index < input_active.size() ? input_active[index] != 0 : true;
    }

    /// @brief Override whether an output port carries real stream data on this tick.
    /// @note Even when set to false, the node must still write silence to the output block.
    void set_output_active(std::size_t index, bool active) const noexcept
    {
        if (index < output_active.size())
            output_active[index] = active ? 1 : 0;
    }
};
```

### 3.2 `core/engine.cpp`：编译期索引与每拍自动传播

#### (1) `port_range()` 支持可写视图重载

在 `core/engine.cpp` 匿名命名空间补充非常量版本，供 `output_active` 构造可写 `View<std::uint8_t>`：

```cpp
template <class T>
View<T> mutable_port_range(std::vector<T> &items, std::size_t first, std::size_t count) noexcept
{
    if (count == 0)
        return {};
    return {items.data() + first, count};
}
```

#### (2) `Engine::Runner::Plan` 新增三个扁平数组

与现有 `inputs`、`outputs` 一一对齐，全部由 `commit()` 一次性构建：

```cpp
struct Plan
{
    // 原有成员保持不变 ...
    std::vector<IBBlock> inputs;
    std::vector<OBBlock> outputs;

    // 新增成员：每端口 1 字节存在位及上游输出下标映射
    std::vector<std::uint8_t> input_active;
    std::vector<std::uint8_t> output_active;
    std::vector<std::size_t> input_sources; // Link::unconnected 表示未连接或上游已暂停
};
```

#### (3) `bind_blocks()` 绑定阶段记录上游下标

在 `Engine::Runner::bind_blocks()` 中同步分配数组并填充 `input_sources`：

```cpp
plan.input_active.assign(layout.input_count, 0);
plan.output_active.assign(layout.output_count, 0);
plan.input_sources.assign(layout.input_count, Link::unconnected);
```

在输入端口绑定循环内补充：

```cpp
if (link.node_index != Link::unconnected)
{
    const auto source_output = plan.steps[link.node_index].first_output + link.port_index;
    plan.inputs[input_index] = plan.outputs[source_output];
    plan.input_sources[input_index] = source_output;
}
else
{
    const auto spec = entry.inputs[port];
    plan.inputs[input_index] = plan.block(spec, spec.max_frames(period), 0);
}
```

> 容量边界安全性：`layout.input_count` 与 `layout.output_count` 已在 `measure_layout()` 中分别受 `plan.inputs.max_size()` 与 `plan.outputs.max_size()` 约束；由于 $\text{sizeof}(\text{IBBlock}) \ge \text{sizeof}(\text{size\_t}) > \text{sizeof}(\text{uint8\_t})$，新增三个数组不会发生容量溢出。

#### (4) `Plan::render()` 自动接力与默认传播

由于 `steps` 已按拓扑顺序排列（上游节点必先于下游节点执行），每拍循环如下：

```cpp
for (const auto &step : steps)
{
    std::uint8_t any_input_active = 0;
    for (std::size_t p = 0; p < step.input_count; ++p)
    {
        const auto input_index = step.first_input + p;
        const auto source = input_sources[input_index];
        const std::uint8_t active = source != Link::unconnected ? output_active[source] : 0;
        input_active[input_index] = active;
        any_input_active |= active;
    }

    // 源节点（无输入端口）默认输出活跃；处理节点默认按全部输入的逻辑或透传。
    const std::uint8_t default_output = step.input_count == 0 ? 1 : any_input_active;
    for (std::size_t p = 0; p < step.output_count; ++p)
        output_active[step.first_output + p] = default_output;

    step.node->process({period, tick,
                        port_range(inputs, step.first_input, step.input_count),
                        port_range(outputs, step.first_output, step.output_count),
                        port_range(input_active, step.first_input, step.input_count),
                        mutable_port_range(output_active, step.first_output, step.output_count)});
}
```

---

## 4. 内置插件的静音标识与早退优化（`plugins/`）

### 4.1 全节点行为总览

| 插件节点 | 存在位产生 / 改写规则 | 空块早退优化动作 | 算力收益 |
| --- | --- | --- | --- |
| **`Capture`** | `set_output_active(0, frames != 0)` | 离线或读空时原有逻辑已跳过队列读并清零 | 作为全图真实流源头，精确区分停推与全零包 |
| **`Playback`** | 无输出端口 | **不早退**，照常写入静音帧保时间线与 ASRC 水位 | 保障设备/录盘连续性 |
| **`Convert`** | 正常流 `1`；拖尾冲刷期强制 `1`；冲刷完毕走默认 `0` | `tail_remaining_ == 0` 且无输入时，跳过编解码与 96 抽头卷积，直接 `memset(0)` | **收益最高**（砍掉空闲分支全部重采样与编解码开销） |
| **`Matrix`** | 无输入走默认 `0`；稳态零增益显式写 `0` | 无输入时跳过矩阵乘加与斜坡计算，清零并同步 `gain_ = target` | 跳过多声道矩阵乘加与斜坡循环 |
| **`Mixer`** | 写未静音且活跃输入的逻辑或 | 跳过 `muted_[i] \|\| !has_input(i)` 的输入支路 | 空闲/静音路零样本读写开销 |
| **`Tone` / 自定义节点** | 无需改代码（引擎默认：`Tone` 为 `1`，处理节点自动透传） | 保持原逻辑；未来有状态插件可按需接入三态早退 | 零侵入兼容 |

---

### 4.2 `Capture`（`plugins/endpoint.cpp`）

在 `Capture::process()` 末尾补 1 行即可成为整张图的精确活跃源头：

```cpp
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
```

* 离线、预填充未就绪、队列读空（WAV EOF、停推、硬件断流）时 `frames == 0` $\to$ 输出 `0`。
* 只要从 `FrameRing` 读到了真实帧（含部分短读、或调用方刻意推送的全零静音包），`frames != 0` $\to$ 输出 `1`。

---

### 4.3 `Convert` 三态早退优化（`plugins/convert.h` & `plugins/convert.cpp`）

`Convert` 内部可能包含有状态的 96 抽头 `SincResampler`（历史缓冲 `history_` 保存最近 $96$ 帧输入样本）。为避免硬截断尾音与脏历史残留，采用**三态早退**策略：

1. **成员变量（`plugins/convert.h`）**：新增 `std::size_t tail_remaining_ = 0;`（单位：输入帧数）。
2. **生命周期清零**：
   * `Convert::prepare(BlockPeriod)`：仅在全部缓冲与 `resampler_.prepare()` 成功后，将 `tail_remaining_ = 0;` 与新缓冲一同提交（维持准备失败保留旧状态的强异常安全）。
   * `Convert::reset()`：在 `resampler_.reset();` 后同步执行 `tail_remaining_ = 0;`。
3. **`Convert::process()` 实现**：

```cpp
void Convert::process(const ProcessContext &context) noexcept
{
    const auto input = context.inputs[0];
    const auto output = context.outputs[0];

    if (context.has_input(0))
    {
        tail_remaining_ = resampler_.tail_frames(); // 同采样率为 0，跨采样率为 96
    }
    else if (tail_remaining_ > 0)
    {
        // 冲刷 FIR 拖尾：继续消费上游保证为零的输入块，并维持输出活跃位为 1
        tail_remaining_ -= std::min(tail_remaining_, input.frames());
        context.set_output_active(0, true);
    }
    else
    {
        // 空块早退：history_ 已完全归零，直接按字节清零输出并跳过编解码与卷积
        const auto bytes = output.byte_view();
        if (!bytes.empty())
            std::memset(bytes.data(), 0, bytes.size());
        return;
    }

    // 原有 same_spec_ 直通 / decode_pcm / process_tick / encode_pcm 逻辑保持不变
    ...
}
```

#### 数值等价性与零相位漂移证明

* **比特级等价**：`SincResampler` 的滤波窗口长度 `taps` $= 96$ 帧，且 `resampler_.tail_frames()` 返回的正是 `taps`（同采样率时返回 $0$）。在输入变为 `0` 后，冲刷阶段累计喂入并经 `remember()` 移入 `history_` 的零输入帧数 $\ge 96$，故进入早退态时 `history_` 内每个浮点样本严格等于 `0.0f`。后续全零历史与全零输入的卷积结果与 `std::memset(0)` **比特级完全一致**（AEReborn 支持的全部 6 种样本格式均以全零字节表示静音）。
* **零相位漂移**：`SincResampler::process_tick()` 每拍的分数相位完全由 `(context.block_period, context.tick_position)` 无状态直接算出，不依赖跨拍相位累加器。因此空闲期间跳过 `process_tick()` 不会产生任何相位偏移，数据恢复时首拍即无缝衔接。

---

### 4.4 `Matrix` 空块与零增益早退（`plugins/matrix.cpp`）

在 `Matrix::process()` 中补齐两处极简逻辑：

```cpp
void Matrix::process(const ProcessContext &context) noexcept
{
    const auto &output_block = context.outputs[0];
    const auto output = output_block.f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;

    const auto target = target_.load(std::memory_order_acquire);
    if (!context.has_input(0))
    {
        output.clear();
        gain_ = target;
        return;
    }

    const bool steady = gain_ == target;
    if (steady && target == 0)
    {
        output.clear();
        context.set_output_active(0, false);
        return;
    }
    ...
```

* **无输入早退**：跳过全部声道矩阵乘加和斜坡计算，同时将 `gain_` 同步到 `target`。这样在静音期间修改目标增益，待音频恢复时直接按新增益生效，不会出现意外的瞬态斜坡。
* **稳态零增益关断**：增益已平滑淡出并稳定在 `0.0f` 时，主动将 `output_active` 设为 `0`（淡出过渡拍仍保持 `1`，确保淡出尾音完整传播），下游混音器或未来的 `PriorityMixer` 可立即感知支路已关断。

---

### 4.5 `Mixer` 跳过空闲支路与汇聚传播（`plugins/mixer.cpp`）

```cpp
void Mixer::process(const ProcessContext &context) noexcept
{
    const auto output = context.outputs[0].f32p_view();
    const auto frames = output.frames();
    if (frames == 0)
        return;

    output.clear();
    bool active = false;
    for (std::size_t i = 0; i < inputs_.size(); ++i)
    {
        if (muted_[i].load(std::memory_order_acquire) || !context.has_input(i))
            continue;
        active = true;
        const auto input = context.inputs[i].f32p_view();
        for (std::size_t ch = 0; ch < output.channels(); ++ch)
        {
            auto *dst = output.channel(ch);
            const auto *src = input.channel(ch);
            for (std::size_t frame = 0; frame < frames; ++frame)
                dst[frame] += src[frame];
        }
    }
    context.set_output_active(0, active);
}
```

* 当混音器挂接了 8--16 路待机槽位（例如路线 A 动态网络/剪辑总线）而仅 1--2 路活跃时，其余空槽在外层循环直接跳过，彻底消除无效内存读取与浮点加法。

---

### 4.6 对未来插件的承接与规范

1. **`PriorityMixer`（优先级自动闪避）**：
   * 默认直接消费 `context.has_input(i)` + `hold_ms` 判定各路输入是否活跃：用户推流/播文件期间自动压低低优先级（含全零静音段，绝不误恢复），停推或 WAV EOF 排空过 `hold_ms` 后自动平滑恢复，彻底免除手动状态切换。
2. **未来有状态 DSP 插件（IIR 滤波、混响、LMS 滤波等）**：
   * 遵循与 `Convert` 相同的三态早退模式：在输入变空后等待尾音能量衰减至噪声底以下（或固定冲刷拍数）时，将内部递归状态清零并放行 `output_active = 0`，既节省空闲 CPU，又彻底杜绝次正规浮点数（如 $10^{-39}$ 级状态）引发的 CPU 飙升。

---

## 5. 逐文件改动清单与分阶段实施顺序

| 阶段 | 涉及文件 | 具体改动 |
| --- | --- | --- |
| **阶段 1：核心传播骨架** | `core/types.h`、`core/engine.cpp` | 1. `ProcessContext` 增加 `input_active`、`output_active`、`has_input()`、`set_output_active()`；<br>2. `Plan` 增加 3 个预分配扁平数组，`bind_blocks()` 绑定 `input_sources`，`Plan::render()` 实现自动接力与默认传播 |
| **阶段 2：插件标识与早退** | `plugins/endpoint.cpp`、`plugins/convert.h/.cpp`、`plugins/matrix.cpp`、`plugins/mixer.cpp` | 1. `Capture::process()` 按 `frames != 0` 写入活跃位；<br>2. `Convert` 实现 `tail_remaining_` 三态早退并在 `prepare()`/`reset()` 复位；<br>3. `Matrix` 补无输入早退与稳态零增益置 `0`；<br>4. `Mixer` 跳过空闲输入并汇聚写输出活跃位 |
| **阶段 3：测试与文档同步** | `tests/`（核心、DSP、路由、回归用例）、`AudioEngine_Outline.md` | 1. 新增活跃位传播、拖尾冲刷、比特级等价与 `Playback` 连续性专项测试；<br>2. 确认 Debug & Release 全量 CTest（含 `regression` 零堆分配检查）全绿；<br>3. 在 `AudioEngine_Outline.md` 第 3 节补充 `ProcessContext` 活跃位契约说明 |

---

## 6. 验收测试矩阵

| 测试项 | 场景与断言要点 |
| --- | --- |
| **1. 核心传播与默认透传** | ① 未连接端口、被 `set_paused(true)` 排除的上游端口到达下游时 `has_input() == false`；<br>② 源节点（`Tone`）默认输出 `1`，不改代码的自定义透传 `NodeT<1, 1>` 自动把上游 `0`/`1` 传给下游；<br>③ 既有直接构造 4 字段 `ProcessContext` 的单测编译与行为完全无回归 |
| **2. `Capture` 真静音 vs. 无数据区分** | ① `Capture` 离线、预填充中、队列读空时 `output_active == 0`；<br>② 经 `PushSource` 推入全 `0.0f` PCM 包时，消费该包的 tick `output_active == 1`，队列播空后的下一拍立刻变为 `0`；<br>③ `WavCapture` 播放期间持续为 `1`，EOF 排空下线后自动变为 `0` |
| **3. `Convert` 拖尾冲刷与比特级等价** | ① 48 kHz $\to$ 16 kHz（与 16 kHz $\to$ 48 kHz）链路，喂入脉冲/正弦信号后令输入变 `0`：验证拖尾冲刷期间（累计输入零帧达到 96 前）`output_active` 保持 `1` 且输出非零尾音；冲刷结束后立刻切为 `0` 且输出全零；<br>② 对比“开启早退”与“持续喂零强制全算”的输出序列，逐样本比特级一致；<br>③ 早退若干拍后恢复信号输入，验证输出相位与连续参考信号严丝合缝，无跳变或历史残留 |
| **4. `Matrix` 与 `Mixer` 早退传播** | ① `Matrix` 输入为空时跳过处理并同步 `gain_ = target`；`set_gain(0.0f)` 淡出拍保持 `1`，下一稳态零增益拍输出 `0`；<br>② `Mixer` 两路输入（一路活跃一路空）：静音活跃路后输出立刻变 `0`，取消静音变回 `1` |
| **5. `Playback` 连续性回归** | 上游全为空块（`active == 0`）时，绑定 `WavPlayback` 持续录制若干 tick，验证落盘帧数与时间线完全连续、未少写帧 |
| **6. 零堆分配与回归验收** | Debug & Release 全量 CTest 通过，特别是 `regression.render_and_passive_io_without_heap` 确认新增 3 个扁平数组与早退路径在运行期零堆分配 |