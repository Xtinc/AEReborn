# AEReborn 架构审查与改进方案（多采样率图 + 插件化重构版）

本文档基于最新一版 `AudioEngine_Outline.md`、`CODING_GUIDELINES.md` 及重构后的 `core/` 与 `plugins/` 源码，围绕以下三个核心问题展开系统检阅：

1. 当前去中心化采样率 + 插件化设计是否更合理，有无进一步优雅化与简化的空间；
2. 代码与设计文档中是否存在可以移除的冗余流程；
3. 如何围绕“引擎 + 插件”体系进一步提升整体设计一致性。

---

## 1. 总体评估：新架构是否更合理？

**结论：这一轮架构重构的方向非常正确，相比前两版实现了质的飞跃。**

* **核心引擎彻底瘦身且职责纯粹**：`Engine` 不再绑定全局采样率，不再持有隐式混音、增益斜坡、声道矩阵、参数命令队列和 Observer 线程池，回归为纯粹的“固定周期（`BlockPeriod`）驱动 + 强类型端口匹配 + DAG 拓扑编译与执行”内核。`core/` 源码量与认知负担大幅下降，彻底消除了上一版中 `is_pcm()` 与 `pcm_block()` 向核心图编译渗透的问题。
* **零拷贝只读共享与显式拓扑**：输入端口严格 1 对 1，直接只读借用上游 `OPortBlock`，不再做隐式累加和清零；多路混音（`Mixer`）、声道映射与增益（`Matrix`）、格式与采样率转换（`Convert`）、设备收发（`Capture`/`Playback`）全部显式化为插件，数据流与算力开销完全透明。
* **多采样率分数帧时序数学严密**：`PortSpec::frames()`、`frame_position()` 与 `Convert::State::resample()` 中基于共享 `tick` 累计取整的相位对齐公式在代数上严格无截断误差（对任意 $1 \sim 96000\text{ Hz}$ 整数采样率均满足 `(rounding(dst_fs) * src_fs - rounding(src_fs) * dst_fs) % 1000 == 0`），使得多采样率节点无需独立时钟即可精确同步。

在此扎实骨架上，仍有若干可以做得**更简洁、更快、更一致**的改进空间。

---

## 2. 设计优雅度与性能提升空间（问题 1）

### 2.1 `RenderPlan` 缓冲池合并与共享零缓冲

#### 现状与问题
* **涉及文件**：`core/engine.cpp`
* `RenderPlan::buffers` 目前是 `std::vector<PortBuffer>`，而每个 `PortBuffer` 同时持有两个 `std::vector`（`audio` 与 `pcm`，其中总有一个为空）。这意味着：
  1. 每个输出端口都会产生一次独立堆分配，`prepare()` 构图与旧计划回收产生 $O(P)$ 次零散分配/释放；
  2. 每个未连接的输入端口都会单独调用 `plan.add_buffer()` 分配一份独立的零值缓冲（例如一个 16 路 `Mixer` 只接了 2 路输入时，会白白分配 14 份内容完全相同的零缓冲）。

#### 改进方法
恢复前两版“整块连续缓冲池”的优雅布局，同时适配多格式存储：
1. 在 `RenderPlan` 中改为仅持有两块连续内存池：`std::vector<float> audio_pool` 与 `std::vector<std::uint8_t> pcm_pool`。
2. `build_plan()` 分两遍扫描：
   * 第一遍统计所有 `F32P` 输出总浮点容量、所有 PCM 输出总字节容量，以及所有未连接 `F32P` / PCM 输入分别所需的**最大单端口容量**；
   * 一次性 `resize` 两块池内存（未连接输入共用各自池首部的一段只读零区间），第二遍直接绑定所有 `OPortBlock` / `IPortBlock` 视图指针。
3. **收益**：将 `RenderPlan` 的堆分配次数从 $O(\text{端口数})$ 压缩为常数 $O(1)$（至多 2 次），彻底消除未连接输入的重复零缓冲内存浪费，并进一步提升音频热路径的 CPU 缓存局部性。

---

### 2.2 `Mixer` 与 `Matrix` 音频热路径的算法与访存优化

#### 现状与问题
* **涉及文件**：`plugins/mixer.cpp`、`plugins/matrix.cpp`
* **问题 1（`Mixer` 内层循环跨声道访存，破坏缓存与 SIMD）**：
  当前 `Mixer::process()` 的循环顺序是外层 `frame`、内层 `ch`：
  ```cpp
  for (std::size_t frame = 0; frame < output.frames(); ++frame) {
      ...
      for (std::size_t ch = 0; ch < output.channels(); ++ch)
          output.channel(ch)[frame] += input.channel(ch)[frame] * gain;
  }
  ```
  对于 planar float32（`F32P`）布局，内层遍历声道会导致每次迭代跨步跳跃整个声道步长，完全阻断编译器自动向量化（SIMD）。
* **问题 2（稳态路径每帧执行无意义的 `double` 斜坡计算与分支）**：
  在 99% 以上的常规周期里，增益和静音目标并未变化（`gains_[i] == target` 或 `gain_ == target`）。当前 `Mixer` 与 `Matrix` 仍在每个采样点上做 `frame + 1 == frames ? target : static_cast<float>(...)` 的浮点乘加和零值分支判断；且 `Mixer` 对持续静音的输入（`gains_[i] == 0 && target == 0`）仍会遍历全部帧。
* **问题 3（零帧周期导致斜坡丢失）**：
  当端口处于极低采样率（$< 200\text{ Hz}$ 且 `Ms05`）出现 `frames == 0` 的周期时，`Matrix` 与 `Mixer` 在末尾仍执行 `gain_ = target`，导致增益跳过平滑斜坡直接阶跃。
* **问题 4（`Matrix` 恒等/静音路径的多余写操作）**：
  `Matrix` 每次都先 `output.clear()` 再累加；在纯增益模式（`coefficients_.empty()`）下可直接覆盖写入，避免“清零 + 读改写”两遍内存带宽。

#### 改进方法
1. **统一采用 `ch` 外层、`frame` 内层**的 planar 友好循环顺序。
2. **拆分稳态与斜坡快速路径**：
   * 若 `frames == 0`，直接 `return`，保留当前 `gain_` 待有帧时再平滑；
   * `Mixer`：当 `gains_[i] == 0.0f && target == 0.0f` 时直接 `continue` 跳过整路输入；当 `gains_[i] == 1.0f && target == 1.0f` 时直接做无分支 `dst[f] += src[f]`；仅在 `gains_[i] != target` 时走斜坡循环（且将末帧提到循环外）；
   * `Matrix`：当 `gain_ == 0.0f && target == 0.0f` 时只需 `output.clear()` 后直接 `return`；当 `gain_ == target` 时走无分支常量增益循环；当 `coefficients_.empty()` 时省去 `output.clear()` 改为直接赋值 `dst[f] = src[f] * gain`。

---

### 2.3 明确 `PortBlock::audio()` 的紧凑 stride 契约

#### 现状与问题
* **涉及文件**：`core/types.h`、`core/engine.cpp`
* `PortBuffer::prepare()` 按 `max_frames(period)` 分配 `channels * max_frames` 空间，而 `RenderPlan::render()` 按本周期实际 `frames` 刷新 `PortBlock`，`PortBlock::audio()` 构造的 `Block` 将 `channel_stride` 设为 `frames_`（如 44.1 kHz / 5 ms 在奇数 tick 为 220，偶数 tick 为 221）。
* 由于所有节点和连接均在同一 tick 内按相同的 `frames_` 读写，当前不会越界，但这种“同一块缓冲在不同 tick 上声道步长动态变化”的行为属于隐式约定。

#### 改进方法
* 在 `PortBlock` 注释中显式写明：`F32P` 块视图按本周期实际 `frames()` 紧凑排布（`channel_stride == frames()`），下游不得假定跨 tick 的内存偏移不变；
* 若后续需要支持视图切片（sub-block view）或零拷贝对齐步长（如 SIMD 32 字节对齐每声道起点），可为 `PortBlock` 增加 `channel_stride_` 字段（默认等于 `max_frames`），一劳永逸消除步长随 tick 漂移的问题。

---

### 2.4 抽离 `SincResampler` 以衔接第 3 阶段时钟漂移补偿

#### 现状与问题
* **涉及文件**：`plugins/convert.cpp`
* `Convert::State` 目前同时负责 PCM 编解码编排与固定整数比的多相 Sinc 重采样。第 3 阶段接入真实多设备时，必然需要面对“标称同采样率但硬件晶振漂移”的自适应微调重采样。

#### 改进方法
* 将 `history`、`kernel`、`sinc_kernel()`、`calc_coffs()`、`interpolate()` 与 `resample()` 抽离为一个独立的内部组件（如 `plugins/detail/resampler.h` 或 `SincResampler` 类），使 `Convert` 只做“解码 -> 重采样 -> 编码”的薄编排层，后续可在不改动 `Convert` 外部接口的前提下，复用或扩展带微调比例的异步重采样器。

---

## 3. 可移除或简化的冗余流程（问题 2）

### 3.1 移除 `Engine::Runtime::maintenance` 的 30 Hz 常驻后台轮询线程

#### 现状与问题
* **涉及文件**：`core/engine.cpp`、`core/runtime.h`、`core/runtime.cpp`
* 在上一版本中，30 Hz 后台任务同时承担 `ObserverWorkers` 调度和旧 `RenderPlan` 回收两项职责。**当前版本已完全移除了 Observer 机制**，但 `Engine::start()` 仍会启动一条独立的操作系统线程（独占 `asio::io_context` + `asio::steady_timer`），以 **30 Hz 频率永久空转唤醒**，每次加锁 `plans_mutex` 检查 `pending` 是否已被消费。
* 在常规音频播放期间，图拓扑几乎不变，`pending` 99.99% 的时间为 `nullptr`。为极低频的计划回收维持一条 30 Hz 轮询线程属于典型的历史遗留冗余；同时 `Engine::prepare()` 在整个 `build_plan()`（包含节点 `prepare()` 与 Sinc 滤波器构建）期间一直持有 `plans_mutex`，进一步放大了不必要的锁耦合。

#### 改进方法
根据对“旧节点析构实时性”的要求，二选一简化：
* **方案 A（最简，彻底删除后台线程与 `plans_mutex`）**：
  * 如果允许旧计划在下一次控制调用（`prepare()`、`stop()` 或新增轻量接口 `reclaim()`）时回收，可以直接删除 `maintenance`、`start_background_task()`、`RuntimeTask` 和 `plans_mutex`！整个 `core/runtime.cpp` 只剩下 `TimerDriver`，`Engine` 少一条常驻线程且完全零互斥锁。
* **方案 B（按需唤醒，零空转）**：
  * 如果确实希望 `remove_node()` + `prepare()` 后即使控制侧不再调用任何 API，旧节点也能在块切换后尽快析构，则保留回收逻辑，但**取消 30 Hz 周期轮询**：改为仅在 `prepare()` 提交 `pending` 时通知一个阻塞在 `condition_variable` 上的惰性回收线程（或向按需启停的单次 timer 投递检查），待 `collect()` 完成后立即重新休眠，稳态下 **0 次唤醒**。同时将 `build_plan()` 移出 `plans_mutex` 锁外，仅在检查状态与发布 `pending` 时短持锁。

---

### 3.2 清理 `AudioEngine_Outline.md` 中残留的旧版矛盾描述

#### 现状与问题
* **涉及文件**：`AudioEngine_Outline.md`
* 文档第 3、6、7 节已更新为最新设计，但**第 1 节与第 2 节仍保留着已被删除的旧设计描述**：
  * 第 1 节表格：“`Connection 持有增益、静音和声道矩阵`”
  * 第 2 节表格：“`Connection | 输出端口到输入端口的有向连接，独立持有增益、静音和声道矩阵`”
  * 第 2 节正文：“`一个 Port 表示一组声道，声道间的选择、复制、合并由 Connection 的矩阵表达。`”
* 这与第 3 节（“原有连接参数、隐式矩阵、增益斜坡、输入累加及参数 FIFO 已删除”）和第 7 节（“连接不包含增益、静音或矩阵”）直接冲突。

#### 改进方法
* 同步修订 `AudioEngine_Outline.md` 第 1、2 节，统一口径为：`Connection` 仅表示同规格端口之间的零拷贝只读连接；混音由 `Mixer` 承担，声道矩阵与增益由 `Matrix` 承担，格式与采样率转换由 `Convert` 承担。

---

### 3.3 清理小体量历史残留

* **`core/retcode.h`**：`Cycle = 9` 留有历史跳过值 `8`（原注释 `// Preserve existing error values.`）。若项目尚未冻结二进制 ABI（当前版本 `0.1.0`），可顺手整理枚举连续性并按模块分组排序（图校验 / 运行状态 / 资源容量）。
* **`core/block.h` 中的 `Buffer` 归属**：当前 `core/` 自身已不再使用 `Buffer`，仅 `plugins/convert.cpp` 使用它。它作为平面浮点工作区工具类对后续 DSP 插件很有价值，建议明确注释其定位为“供插件在 `prepare()` 中预分配的平面浮点辅助缓冲”，避免被误认为是残留死代码。

---

## 4. 围绕“引擎 + 插件”体系提升整体一致性（问题 3）

当前架构已确立了“**极简核心引擎 + 功能完备的插件体系**”的定位。为了让核心与所有内置/自定义插件对外呈现高度统一的直觉和契约，建议收口以下 5 处不一致性：

### 4.1 统一插件参数校验时机与错误处理策略

#### 现状与不一致性
目前 5 个内置插件在“构造期校验抛异常”还是“`prepare()` 返回 `RetCode`”上分成两派：

| 插件 | 无效 `PortSpec` | 无效配置（如声道/频率/系数） | 表现行为 |
| :--- | :--- | :--- | :--- |
| `Matrix` | 构造期 `throw` | 构造期 `throw` | 立即抛异常 |
| `Mixer` | 构造期 `throw` | 构造期 `throw` | 立即抛异常 |
| `Convert` | 延迟到 `add_node` / `prepare` | 声道不匹配延迟到 `prepare` 返回 `ChannelMismatch` | 构造成功，运行前报错 |
| `Tone` | 延迟到 `add_node` / `prepare` | 频率/幅值非法延迟到 `prepare` 返回 `InvalidArg` | 构造成功，运行前报错 |
| `Capture` / `Playback` | 延迟到 `add_node` / `prepare` | — | 构造成功，入图/准备时报错 |

这导致调用方使用 `engine.emplace_node<T>(...)` 时行为极不统一：有的直接抛异常，有的入图成功却在 `prepare()` 才失败。

#### 统一方案
严格落实 `CODING_GUIDELINES.md` 第 3 节的原则——**“构造函数维护对象不变量，`prepare(BlockPeriod)` 仅验证周期依赖与分配运行资源”**：
* 所有插件（`Convert`、`Tone`、`Capture`、`Playback`、`Matrix`、`Mixer`）在构造函数中统一校验**与 `BlockPeriod` 无关的固有不变量**（端口 `valid()`、`Convert` 两端声道数相等、`Tone` 频率在 $[0, \text{fs}/2)$ 且幅值有限、`Matrix` 维度与系数有限、`Mixer` 输入数 $> 0$），不合法即抛出 `std::invalid_argument` / `std::length_error`；
* `prepare(BlockPeriod)` 统一只负责三件事：校验 `is_block_period(period)`、校验平台 `is_lock_free()`、按 `period` 分配内部工作区（`RetCode` 仅返回 `InvalidArg` / `Unsupported` / `TooLarge` 等周期与资源错误）。
* 如果更偏好全程无异常风格，也可以反过来统一为“私有构造 + 静态工厂 `Result<std::unique_ptr<T>> create(...)`”，核心是**全部插件遵循同一套规则**。

---

### 4.2 统一 `SampleRate` 的类型表达与合法范围

#### 现状与不一致性
* `AudioEngine_Outline.md` 第 4 节与 `PortSpec::valid()` 均允许 **$1 \sim 96000\text{ Hz}$ 的任意整数采样率**；
* 但 `core/types.h` 将 `SampleRate` 定义为仅含 10 个枚举值的 `enum class SampleRate : std::uint32_t`。调用方若要使用非列表采样率（或从外部设备读取整数采样率构造端口），必须到处写 `static_cast<SampleRate>(rate)`，破坏了类型系统原本表达的意图。

#### 统一方案（二选一）
* **若明确支持任意整数采样率（$1 \sim 96000\text{ Hz}$）**：将 `SampleRate` 改为 `using SampleRate = std::uint32_t;`（或轻量强类型包装），并将常用采样率提供为 `constexpr` 常量（如 `SampleRate::Hz48000` 或 `rates::Hz48000`），避免强制转换；
* **若仅支持标准采样率集合**：恢复 `is_sample_rate(SampleRate)` 校验并在 `PortSpec::valid()` 中启用，同时同步更新文档口径。

---

### 4.3 明确插件控制接口的线程安全分级与生命周期边界

#### 现状与不一致性
将参数控制下放到插件后，所有插件句柄都统一通过 `engine.node<T>(id)` 获取，但不同插件公开方法的线程安全等级截然不同：
* `Matrix::set_gain()`、`Mixer::set_muted()`：内部基于无锁原子变量，**允许在引擎运行期间由控制线程并发调用**；
* `Capture::bind()`、`Playback::bind()`：内部操作非原子成员（`bytes_`、`tick_`、`bound_`），**严禁与 `process()` 并发调用**，必须由调用方在渲染执行域内串行调用；
* 同时，调用方持有的裸指针 `T *` 在 `remove_node()` / `replace_node()` + `prepare()` 之后存在悬空风险。

#### 统一方案
1. **在头文件与文档中建立清晰的两级接口标记**：
   * 明确区分 **Concurrent Control API**（如 `Matrix::set_gain`、`Mixer::set_muted`，标注 `// Thread-safe with process()`）与 **Render-Thread Serialized API**（如 `Capture::bind`、`Playback::bind`、`Node::reset`，标注 `// Must be serialized with process()`）。
2. **收口 `Capture` / `Playback` 的同步边界定位**：
   * 在 `plugins/io.h` 与文档中明确标注：`Capture`/`Playback` 是单 tick 同步借用节点（适合测试驱动或直接在主设备回调线程中绑定）；第 3 阶段接入异步设备/线程时，由独立的队列化后端节点（内部持无锁 SPSC 缓冲）向控制/设备线程暴露并发推送/拉取接口，不让调用方在多线程下直接碰 `bind()`。
3. **控制侧指针使用约定**：在 `Engine::node<T>()` 注释中明确指针有效期仅持续到该节点被 `remove_node()` / `replace_node()` 之前（或与 `Engine` 同生命周期）。

---

### 4.4 让 `ProcContext` 携带 `BlockPeriod`，简化所有时间敏感插件

#### 现状与不一致性
* `Tone`、`Convert`、`Capture`、`Playback` 四个插件目前都必须在 `prepare(BlockPeriod)` 里把 `period` 缓存为私有成员 `BlockPeriod period_{}`，仅仅是为了在 `process(context)` 里能调用 `spec.frame_position(period_, context.tick_position)`。
* 这不仅造成样板代码重复，还使得 `Tone` 这种本可“无状态（stateless）”的纯函数节点被迫变成有状态对象，并且强依赖 `prepare()` 必须先于 `process()` 被调用。

#### 统一方案
* 在 `ProcContext` 中增加 4 字节的 `BlockPeriod block_period;` 字段：
  ```cpp
  struct ProcContext
  {
      BlockPeriod block_period;
      std::uint64_t tick_position;
      View<const IPortBlock> inputs;
      View<const OPortBlock> outputs;
  };
  ```
* 如此一来，`Tone` 和任意自定义时间相关节点无需缓存 `period_` 成员，直接从 `context` 即可计算当前样本时间；`prepare(BlockPeriod)` 回归纯粹的“资源准备与容量校验”职责。

---

### 4.5 为第 3~5 阶段保留统一的无锁队列基元，补齐“回调输出”插件范式

#### 现状与前瞻
* 移除 `core/` 内置 `Observer` 后，应用获取图内音频的标准路径变为“连接一个自定义 Sink 插件，通过有界队列异步交付给用户回调”。
* 但目前 `SpscRing` 已随 `Observer` 一起从代码库删除，`examples/clocked_graph.cpp` 只演示了单个原子峰值 `Meter`。随着第 3 阶段（设备后端）、第 4 阶段（文件/推入/回调输出）推进，多个插件都会重复需要“单生产/单消费有界无锁队列”和“跨周期拼拆包”能力。

#### 改进建议
* 保持 `core/` 零依赖与极简不变，在进入第 3 阶段时于 `plugins/`（或独立公共工具头如 `plugins/detail/ring_buffer.h`）沉淀经过验证、带 `alignas(64)` 防伪共享的无锁 `SpscRing` 与字节流环缓基元，供未来的 `DeviceCapture`、`DevicePlayback`、`PushSource`、`CallbackSink` 等插件统一复用，避免各后端各写一套并发队列导致质量参差。

---

## 5. 优化优先级与落地路线图

| 优先级 | 类别 | 改进项 | 涉及文件 | 预期收益 |
| :--- | :--- | :--- | :--- | :--- |
| **P0** | 冗余移除 | 移除或按需化 `Engine::Runtime::maintenance` 的 30 Hz 常驻轮询线程，并缩小 `plans_mutex` 锁区间 | `core/engine.cpp`, `core/runtime.h`, `core/runtime.cpp` | 消除稳态下 30 Hz 空转唤醒与多余后台线程 |
| **P0** | 热路径性能 | 重构 `Mixer`/`Matrix` 循环顺序为 `ch` 外 `frame` 内；拆出稳态与静音快速路径；修复 `frames == 0` 提前结束斜坡的问题 | `plugins/mixer.cpp`, `plugins/matrix.cpp` | 恢复 SIMD 向量化友好性，大幅降低稳态 CPU 开销 |
| **P1** | 设计一致性 | 统一 5 个插件的构造期校验与 `prepare()` 错误返回契约；统一 `SampleRate` 表达 | `plugins/*.h`, `plugins/*.cpp`, `core/types.h` | 消除 `emplace_node` 有的抛异常、有的延迟报错的割裂感 |
| **P1** | 内存优雅度 | `RenderPlan` 合并为 `audio_pool`/`pcm_pool` 两块连续池，所有未连接输入共享只读零区间 | `core/engine.cpp` | 堆分配从 $O(P)$ 降至 $O(1)$，消除空输入内存浪费 |
| **P1** | 文档一致性 | 清理 `AudioEngine_Outline.md` 第 1、2 节中关于 Connection 持有矩阵/增益的旧描述 | `AudioEngine_Outline.md` | 消除规范文档内部前后矛盾 |
| **P2** | 接口整洁度 | `ProcContext` 补充 `block_period` 字段；明确插件并发控制与串行绑定的线程安全分级；明确 `PortBlock` stride 契约 | `core/types.h`, `plugins/tone.cpp`, `plugins/io.h` | 消除插件冗余 `period_` 成员，降低误用风险 |
| **P2** | 阶段演进 | 从 `Convert` 抽离 `SincResampler`；在 `plugins/` 沉淀防伪共享无锁队列基元 | `plugins/convert.cpp` | 平滑过渡到第 3 阶段多设备时钟漂移适配与异步后端 |