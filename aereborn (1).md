# AEReborn 诊断与可观测性设计方案

本方案在不引入冗余 `Snapshot` / `Status` 包装结构体的前提下，聚焦于**已生效的音频运行态**，打通 AEReborn 从 **生效渲染计划（Active Plan）→ 端点队列（Capture / Playback）→ 外部后端（Backend）** 的全链路诊断能力，并为后续外层的指令队列与事件管理预留平滑接口。

## 1. 问题本质与设计参考

当前引擎内部闭环已经完备，但缺乏可观测性，根本原因在于三层运行事实对外不可见：

1. **生效拓扑不可见**：`Engine::Runner` 运行上一次 `commit()` 编译出的不可变 `Plan`（自动按依赖排序并裁剪未连到无输出 Sink 的分支与暂停节点），但 `Engine` 对外只暴露了 `block_period()` 和 `tick_position()`，外部无法得知当前实际在跑哪些节点、它们如何连接。
2. **图内端点与图外后端割裂**：`Capture` / `Playback` 的门控、预填充与水位均为私有；`Backend` 无法直接关联回图内的 `NodeId`。
3. **运行健康度不可见**：软件时钟逾期、单拍渲染耗时、队列 XRUN（欠载/溢出）以及异步设备 ASRC 漂移闭环状态目前不可查询。

### 业界参考与取舍

| 参考项目 | 借鉴的设计 | 本项目的精巧取舍 |
| --- | --- | --- |
| **PipeWire**（`pw-top` / `pw-dump`） | 慢变拓扑按需查询，快变指标由音频线程只写原子计数 | 完全采用；控制侧直接按需读取，音频热路径零锁、零分配、零日志 |
| **GStreamer**（`.dot` 导出） | 用 Graphviz DOT 可视化管线拓扑、格式与运行状态 | 放在内核外的独立只读诊断工具中实现，不污染 `core/`、`plugins/`、`backend/` |
| **JACK / FMOD** | 聚焦实际渲染执行顺序与节点类型自描述 | `Engine` 只暴露生效计划的零拷贝 `View`，不暴露未生效的草稿细节 |

## 2. 核心设计原则（零 Snapshot 冗余）

1. **只查生效计划，不查草稿图**：未 `commit()` 的草稿尚未生效，不对外暴露草稿节点/连线/暂停表；`Engine` 仅保留 1 个 `dirty()` 布尔标志提示“存在未提交改动”，拓扑查询全部面向当前生效的 `Plan`。
2. **拒绝结构体膨胀**：不新增 `EngineSnapshot`、`EndpointStatus`、`BackendStatus` 等镜像打包结构体，直接复用现有类型（`NodeId`、`ConnId`、`IPort`、`OPort`、`PortSpec`、`View<T>`、`RetCode`）和 `const` 只读查询。
3. **全库仅增 1 个 3 字段 POD 类型**：将 `core/engine.cpp` 中原本私有的 `Connection` 提升到 `core/types.h`（含 `ConnId id; OPort from; IPort to;`），即可完整表达生效计划中的所有连接。
4. **严守单向依赖与热路径零成本**：保持 `diag（只读工具） → backend → plugins → core`。`Engine::tick()` 与 `Node::process()` 保持 `noexcept`、无锁、无堆分配。

## 3. 分层接口设计

### 3.1 `core/`：唯一连接值类型、节点自描述与生效计划查询

#### (1) `core/types.h`：提升 `Connection`

```cpp
/// @brief Read-only description of one directed connection in the active render plan.
struct Connection
{
    ConnId id;
    OPort from;
    IPort to;
};
```

不携带冗余的 `PortSpec`（端口规格可由 `engine.plan_node(from.node)->outputs()[from.index]` 直接零拷贝取得）。

#### (2) `core/graph.h`：节点零分配类型标识

在 `Node` 增加带默认实现的虚函数，不依赖 RTTI，无字符串分配：

```cpp
/// @brief Return a static diagnostic name for this node type.
virtual const char *type_name() const noexcept
{
    return "Node";
}
```

> 业务层自定义标签（如 `"主麦克风"`、`"背景音乐"`）由上层管理模块按 `NodeId` 映射维护，不侵入 `Node`。

#### (3) `core/engine.h` & `core/engine.cpp`：状态遥测 + 生效计划零拷贝视图

将 `Engine::State` 从 `private` 提升为 `public`，并在 `Engine` 上直接提供两组精简查询：

```cpp
class Engine
{
  public:
    enum class State
    {
        Unprepared,
        Ready,
        Running
    };

    // --- 1. 引擎状态与实时遥测 ---

    /// @brief Return the current engine lifecycle state.
    /// @note Serialize with control calls.
    State state() const noexcept { return state_; }

    /// @brief Report whether the draft has uncommitted changes.
    /// @throws Error Busy if called from this engine's clock thread.
    bool dirty() const;

    /// @brief Return the monotonically increasing count of successful commits.
    std::uint64_t plan_generation() const noexcept;

    /// @brief Return the number of software-clock ticks that exceeded one block period.
    std::uint64_t late_ticks() const noexcept;

    /// @brief Return the duration of the most recently completed tick, in microseconds.
    std::uint32_t last_tick_us() const noexcept;

    // --- 2. 生效渲染计划查询（控制线程调用，零拷贝借用） ---

    /// @brief Borrow the active plan's node IDs in actual execution order.
    /// @throws Error Busy if called from this engine's clock thread.
    /// @note Returns empty before the first commit. Valid until the next commit() or Engine destruction.
    View<const NodeId> plan_nodes() const;

    /// @brief Borrow the connections active in the current render plan.
    /// @throws Error Busy if called from this engine's clock thread.
    /// @note Returns empty before the first commit. Valid until the next commit() or Engine destruction.
    View<const Connection> plan_connections() const;

    /// @brief Borrow a read-only pointer to a node in the active render plan.
    /// @throws Error Busy on the clock thread, or UnknownNode if not in the active plan.
    /// @note Valid until the next commit() or Engine destruction.
    const Node *plan_node(NodeId node_id) const;
};
```

**内部实现要点（极低改动量）**：

* **`Graph::DraftNode`**：将 `std::vector<OPort> inputs;` 改为记录 `{ConnId id; OPort from;}`，在 `Graph::visit()` 解析可达拓扑时顺手填充 `Topology::connections`（天然只包含未被裁剪、未被 `paused` 阻断的生效连接）。
* **`Runner::Plan`**：由于 `Plan` 一经 `commit()` 构建即完全不可变，在 `Plan` 内新增 `std::vector<NodeId> node_ids;` 和 `std::vector<Connection> connections;`，构建时一次性填好；`plan_nodes()` / `plan_connections()` 直接返回 `View`，零堆分配。`plan_node(NodeId)` 直接在 `current_->owners` 中查找，即使草稿已做未提交的替换或删除，依然严格指向正在渲染的节点实例，无悬垂风险。每次 `commit()` 成功后 `plan_generation_` 递增 1。
* **`Clock` 遥测**：`Clock::start()` 线程循环中在 `engine.tick()` 前后读取 `steady_clock::now()`（尾部原本就有一次 `now()` 用于计算 `lateness`），写入 `std::atomic<std::uint32_t> last_tick_us_`；若 `lateness >= interval`，`late_ticks_.fetch_add(1, std::memory_order_relaxed)`。`Engine::reset()` 停机复位时同步清零 `late_ticks_` 和 `last_tick_us_`。

### 3.2 `plugins/`：插件自描述与端点直接查询

#### (1) 内置插件覆写 `type_name()`

* `Convert` → `"Convert"`
* `Mixer` → `"Mixer"`
* `Matrix` → `"Matrix"`
* `Capture` → `"Capture"`
* `Playback` → `"Playback"`
* （`tests/support/tone.h` 中的 `Tone` → `"Tone"`）

> `Matrix::gain()` 和 `Mixer::muted(index)` 已是线程安全的公开只读方法，无需改动。

#### (2) `plugins/endpoint.h` & `plugins/endpoint.cpp`：开放现有状态 + 单项 XRUN 计数

不引入 `EndpointStatus` 结构体，直接将 `Capture` / `Playback` 已有的私有只读状态公开，并各自补充 1 个零开销原子 XRUN 计数：

```cpp
// Capture 与 Playback 共同公开的轻量只读查询：
std::chrono::microseconds io_period() const noexcept { return io_period_; }
bool bound() const noexcept { return backend_bound_.load(std::memory_order_acquire); }
bool online() const noexcept { return (graph_state_.load(std::memory_order_acquire) & graph_open) != 0; }
bool steady() const noexcept { return primed_.load(std::memory_order_acquire); }
std::size_t capacity() const noexcept { return ring_.capacity(); }
std::size_t target_frames() const noexcept { return ring_.capacity() / 2; }
/// @brief Approximate queued frames for diagnostics (may be momentarily imprecise under concurrent I/O).
std::size_t available() const noexcept { return ring_.available(); }
```

**XRUN 计数（各 1 个 `std::atomic<std::size_t>`）**：

* **`Capture::underrun_frames() const noexcept`**：在 `Capture::process()` 中，仅当 `primed_` 为 `true`（已过启动预填充期）且 `frames < output.frames()` 时，以 `memory_order_relaxed` 累加 `output.frames() - frames`。
* **`Playback::overflow_frames() const noexcept`**：在 `Playback::process()` 中，当 `graph_open` 开启且 `ring_.write()` 实际写入帧数小于 `input.frames()` 时，以 `memory_order_relaxed` 累加丢弃帧数（队满即溢出，无论是否已 `primed`）。
* **清零时机**：在 `open()` 重新上线时及覆写的 `reset()` 中清零，确保重启后诊断计数干净。

### 3.3 `backend/`：后端直接查询与零耦合节点关联

不引入 `BackendStatus` 结构体，也不引入 `Direction` 枚举（方向由 `type_name()` 直接表达），在 `Backend` 现有 `bound()`、`running()`、`last_error()`、`device_spec()`、`device_max_frames()` 基础上补齐 4 个轻量只读方法：

#### (1) `Backend` 基类（`backend/backend.h`）

```cpp
class Backend
{
  public:
    /// @brief Return a static diagnostic name for this backend (including direction when applicable).
    virtual const char *type_name() const noexcept = 0;

    /// @brief Return the bound graph endpoint as a Node pointer, or nullptr when unbound.
    /// @note Used by external diagnostics to correlate this backend with an Engine NodeId.
    const Node *endpoint_node() const noexcept;

    /// @brief Return the bound endpoint's logical F32P specification, or an empty spec when unbound.
    PortSpec logical_spec() const noexcept;

    /// @brief Report whether the active binding uses FIR resampling.
    bool uses_resampler() const noexcept;
};
```

**实现约定**：

* **`type_name()` 直接表达方向**：
  * 单向后端直接返回固定名：`"WavCapture"`、`"WavPlayback"`、`"PushSource"`、`"PullSink"`、`"UsrPlayback"`。
  * 双向后端按当前绑定方向返回清晰名称（未绑定时返回通用名）：
    * `AlsaBackend`：`bound() ? (is_capture() ? "AlsaCapture" : "AlsaPlayback") : "AlsaBackend"`
    * `WasapiBackend`：`bound() ? (is_capture() ? "WasapiCapture" : "WasapiPlayback") : "WasapiBackend"`
    * `NullBackend`：`bound() ? (is_capture() ? "NullCapture" : "NullPlayback") : "NullBackend"`
* **`endpoint_node()`**：返回 `bound() ? (capture_ ? static_cast<const Node *>(capture_) : static_cast<const Node *>(playback_)) : nullptr`。这是把图外 `Backend` 精确挂接到图内 `NodeId` 的**唯一桥梁**，零反向依赖。
* **`logical_spec()` 与 `uses_resampler()`**：提升为 `public` 时均以 `bound()` 守卫，未绑定时安全返回空 `PortSpec{}` 和 `false`。

#### (2) `ActiveBackend` 子类（`backend/backend.h`）

将私有 `enum class State { Stopped, Running, Draining, Stopping };` 提升为 `public`，补充 3 个单行只读查询：

```cpp
class ActiveBackend : public Backend
{
  public:
    enum class State
    {
        Stopped,
        Running,
        Draining,
        Stopping
    };

    /// @brief Return the detailed worker state (distinguishes Running, Draining, and Stopping).
    State state() const noexcept { return worker_state_.load(std::memory_order_acquire); }

    /// @brief Report whether this backend uses an independent hardware clock with ASRC.
    bool is_async() const noexcept { return async_; }

    /// @brief Return the latest ASRC clock drift correction in ppm (zero for sync backends).
    float drift_ppm() const noexcept { return drift_ppm_.load(std::memory_order_relaxed); }
};
```

* 在 `ASRC::update()` 末尾以 `memory_order_relaxed` 将 `static_cast<float>(correction * 1e6)` 写入 `drift_ppm_`（与 `Matrix::target_` 保持相同的 `std::atomic<float>` 风格）；在 `go_offline()` 时重置为 `0.0f`，便于实时观测 PI 闭环是否收敛在 $\pm 2000\text{ ppm}$ 范围内。
* `WasapiBackend::last_failure()` 保持不变，供诊断工具按需补充输出原生 HRESULT。

## 4. 端到端链路诊断工具（`diag/`）

在内核库之外新增一个轻量只读工具模块（`diag/inspect.h` & `diag/inspect.cpp`，独立静态库 `aereborn::diag`，依赖 `aereborn::backend`），不持有任何运行态，只负责把三层直接查询拼装为紧凑文本或 Graphviz DOT。

### 4.1 极简接口

```cpp
namespace aereborn
{

/// @brief Format a compact human-readable report of engine state, active render plan, and backends.
/// @pre Call on the engine's control thread, serialized with backend bind/unbind operations.
void dump_text(std::ostream &out, const Engine &engine, View<const Backend *const> backends = {});
std::string dump_text(const Engine &engine, View<const Backend *const> backends = {});

/// @brief Export the active render plan and attached backends in Graphviz DOT format.
/// @pre Call on the engine's control thread, serialized with backend bind/unbind operations.
void dump_dot(std::ostream &out, const Engine &engine, View<const Backend *const> backends = {});
std::string dump_dot(const Engine &engine, View<const Backend *const> backends = {});

} // namespace aereborn
```

### 4.2 零冗余关联逻辑

因为只查生效计划，工具实现极其直白：

1. 遍历传入的 `backends`，用 `backend->endpoint_node()` 建一张 `std::map<const Node *, const Backend *>`。
2. 按 `engine.plan_nodes()` 的执行顺序输出每个生效节点（通过 `engine.plan_node(id)` 拿到 `const Node *`，读出 `type_name()`、端口 `PortSpec`、端点水位与 XRUN、`Matrix::gain()`、`Mixer::muted()` 及关联的 `Backend` 详情）。
3. 遍历 `engine.plan_connections()` 直接输出全部生效连线。
4. 任何未命中生效计划节点的后端（例如尚未绑定、或绑定到了被暂停/未连通至 Sink/未提交的端点）统一列在 `Detached Backends` 中，一眼发现“后端已启动但未进入渲染图”的异常。

### 4.3 输出视图示例

#### 视图 A：紧凑文本转储（日志 / CLI / 测试失败自动打印）

```text
Engine: Running | period=5ms | tick=12480 | plan_gen=3 | dirty=false | late=0 | tick_us=138
Plan Order: [1] -> [2] -> [3] -> [4]

Nodes:
  [1] Capture   out0=48000/2ch/F32P | bound=1 online=1 steady=1 fill=960/1920 underrun=0
      └─ AlsaCapture (async) state=Running dev=48000/2ch/S32LE max=240 fir=1 drift=+12.4ppm err=Ok
  [2] Mixer     in0..1=48000/2ch/F32P out0=48000/2ch/F32P | muted=[0, 1]
  [3] Matrix    in0=48000/2ch/F32P out0=48000/2ch/F32P | gain=0.50
  [4] Playback  in0=48000/2ch/F32P | bound=1 online=1 steady=1 fill=952/1920 overflow=0
      └─ WasapiPlayback (async) state=Running dev=48000/2ch/F32LE max=480 fir=1 drift=-8.1ppm err=Ok

Connections:
  #10: [1]:0 -> [2]:0 (48000/2ch/F32P)
  #11: [2]:0 -> [3]:0 (48000/2ch/F32P)
  #12: [3]:0 -> [4]:0 (48000/2ch/F32P)
```

#### 视图 B：Graphviz DOT 导出（复杂多总线排障）

* **图节点**：`shape=record` 显示 `[NodeId] TypeName` 与端口规格及关键参数（增益、静音、队列水位）。
* **外部后端**：`shape=box, style="rounded,filled"` 挂载在对应 `Capture` / `Playback` 外侧，展示 `type_name()`、设备原生格式、运行状态、`drift_ppm` 与 `last_error`（非 `Ok` 时标红）；未命中生效计划的后端以灰色虚线框列在独立分组中。
* **连线**：直接绘制 `engine.plan_connections()`，标注连接 ID 与端口格式。

## 5. 与未来“指令队列 & 事件管理”的自然衔接

1. **Pull 模式（对接指令队列）**：
   * 外层单消费者指令队列本身就是 `Engine` 和 `Backend` 的唯一串行控制线程。
   * 新增诊断指令（如 `InspectEngineCmd`），在指令回调内直接调用 `dump_text()` / `dump_dot()` 或读取标量接口并返回结果——**天然零额外锁、零线程安全隐患**。
2. **Push 模式（对接事件管理）**：
   * 枚举类型单一来源：`Engine::State`、`ActiveBackend::State`、`RetCode`、`plan_generation()` 直接作为外层事件载荷，无需维护两套状态枚举。
   * 控制面状态变化（`commit` / `start` / `stop` / `bind` / `unbind`）由指令队列在控制线程直接派发事件。
   * 异步后端异常离线（`DeviceLost` / `Timeout` / `EndOfStream`）：后续只需在 `ActiveBackend` 增加一个可选的轻量通知回调，在 `ActiveBackend::run()` 完成 `go_offline(error)` 并将状态置为 `State::Stopped` 之后向**外层事件队列投递**（严禁在 I/O 线程同步执行复杂逻辑），上层即可立刻感知异常并自动触发诊断转储或设备恢复。

## 6. 改动清单与分阶段实施顺序

| 阶段 | 涉及文件 | 核心改动 | 验收重点 |
| --- | --- | --- | --- |
| **阶段 1：核心与插件只读原语** | `core/types.h`、`core/graph.h`、`core/engine.h/.cpp`、`plugins/convert.h`、`plugins/matrix.h`、`plugins/mixer.h`、`plugins/endpoint.h/.cpp` | 新增 `Connection`、`Node::type_name()`、`Engine` 状态/遥测及 `plan_nodes()` / `plan_connections()` / `plan_node()`；公开 `Capture`/`Playback` 状态与 XRUN 计数 | 验证 `plan_nodes` 执行顺序与 `plan_connections` 准确性、未提交修改时 `plan_node()` 仍安全指向旧实例、`plan_generation` 递增、XRUN 计数准确性及 Release 零堆分配无回归 |
| **阶段 2：后端只读原语** | `backend/backend.h/.cpp`、各具体后端头文件（ALSA / WASAPI / WAV / Null / PushSource / PullSink / UsrPlayback） | 新增 `Backend::type_name()`（含方向语义）、`endpoint_node()`、`logical_spec()`、`uses_resampler()`；公开 `ActiveBackend::State`、`is_async()`、`drift_ppm()` | 验证双向/单向后端 `type_name()` 准确、解绑后 `endpoint_node()` 为空、`Draining`/`Running`/`Stopped` 状态区分及异步 `drift_ppm()` 数值 |
| **阶段 3：端到端诊断输出** | `diag/inspect.h/.cpp`、`CMakeLists.txt`、新增诊断测试 | 实现 `dump_text()` 与 `dump_dot()`；在 `bus` / `io` 场景测试中验证多路混音、Convert、孤立后端及后端报错的文本与 DOT 输出 | Debug & Release 全量 CTest 通过，确认文本与 DOT 输出清晰无越界 |