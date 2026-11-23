# AEReborn 错误处理统一与错误码精简改造计划

本计划旨在消除当前代码库中“异常与错误码在同一条控制链上混用、热路径多处冗余错误传递、内部反复 `try/catch` 转错误码”的复杂度，将全库收敛为**两条极简边界、全库零 `try/catch` 胶水、单一异常类型 `aereborn::Error`** 的清晰模型。

---

## 1. 核心取舍：两条边界替代混合双轨

综合评估控制面复杂度与 C++14 语言约束（无 `[[nodiscard]]`），最简单的方案是**将 `Backend::bind()` 与 `Backend::start()` 也纳入统一异常控制面**，使全库只剩两条清晰规则：

| 执行边界 | 覆盖范围 | 错误表达方式 | 为什么最简单 |
| :--- | :--- | :--- | :--- |
| **控制线程公开接口** | `Engine` 全部控制接口、`Node::prepare`、插件构造与参数接口、`Buffer::resize`、`FrameRing::prepare`、`SincResampler::prepare` / `max_*_frames`、`Backend::bind` / `start` | **统一抛 `aereborn::Error`**（继承 `std::runtime_error`，携带 `ErrorCode`） | 1. 建图、提交、绑定、启动全流程直线书写，彻底消灭 `checked(...)` 与 `Result<T>` 拆包。<br>2. 避免 C++14 忘记检查 `bind()` / `start()` 返回值导致静默吞错。<br>3. `Backend::bind()` 内部与 `SincResampler` / `Buffer` 零阻抗失配，单靠 `ScopeExit` 自动回滚，零 `try/catch`。 |
| **驱动钩子与异步工作线程** | `Backend` 派生钩子（`on_open`、`on_start`、`on_read`、`on_wait_writable`、`on_write`）、`Backend::frames_in_period`、`Backend::last_error()` | **保留 `RetCode` / `Result<std::size_t>`**（I/O 钩子全部标 `noexcept`） | 1. 驱动直接映射 ALSA `-errno` / WASAPI `HRESULT` / 文件 EOF，无需在驱动层写 `throw`。<br>2. 异常永不跨线程传播；`Backend::bind()` 与 `Backend::start()` 在基类边界通过 `.or_throw()` 一行完成桥接。<br>3. `on_read` / `on_wait_writable` / `on_write` 标记 `noexcept` 后，彻底删除 `backend.cpp` 内全部 `try/catch (...)`。 |
| **实时渲染、热路径 DSP 与停机清理** | `Node::process` / `reset`、`FrameRing::read` / `write`、`decode_pcm` / `encode_pcm`、`SincResampler::process_tick` / `required_input` / `process`、`Backend::stop` / `unbind` | **`noexcept` 返回裸值或 `void`**（前置条件由 `assert` 守卫） | 零展开开销，删光 `Backend` 热路径中全部不可能触发的重采样错误判断分支。 |

---

## 2. `ErrorCode` 命名整理与对称规范

遵循 `CODING_GUIDELINES.md` 的命名约定（最短清晰写法、成对词缀对齐、避免词根重复），删除全库未引用的 `BufferFull`，将错误码精简为 **18 个、5 组**，并修正 3 处不对称或别扭的命名：

| 分组 | 原名称 | 新名称 | 调整说明与语义 |
| :--- | :--- | :--- | :--- |
| **成功** | `Ok` | `Ok` | 操作成功 |
| **参数与边界** | `InvalidArg` | `InvalidArg` | 非法参数（空指针、无效采样率/格式/周期、NaN/Inf、维度不匹配） |
| | `InvalidPort` | `InvalidPort` | 端口索引越界（与 `InvalidArg` / `InvalidType` 保持 `Invalid*` 前缀对齐） |
| | `InvalidType` | `InvalidType` | 节点动态类型转换失败 |
| | `TooLarge` | `TooLarge` | 缓冲容量、帧数、文件长度或矩阵尺寸超出上限 |
| **状态与并发** | `NotPrepared` | `NotReady` | **改名**：统一覆盖未 `prepare`、未 `commit`、未 `bind` 三种未就绪态，与 `Busy` 形成简洁对偶 |
| | `Busy` | `Busy` | 状态冲突（运行中禁止修改、时钟线程重入、端点或后端已被占用） |
| | `BufferFull` | *(删除)* | 全库无任何引用，直接清除 |
| **图与连接** | `UnknownNode` | `UnknownNode` | 节点不存在或已移除（与 `UnknownConn` 等长对称） |
| | `UnknownConn` | `UnknownConn` | 连接不存在 |
| | `DuplicateConn` | `DuplicateConn` | 相同 `(from, to)` 已连接（与 `UnknownConn` 共享 `*Conn` 后缀） |
| | `InputConnected` | `InputBusy` | **改名**：输入端口已被其他来源占用，消除原名“Connected”偏正向的歧义，并与 `Busy` 风格对齐 |
| | `Cycle` | `Cycle` | 连接会引入反馈环 |
| | `FormatMismatch` | `FormatMismatch` | 端口规格、节点替换布局或设备格式/声道不匹配 |
| **I/O 与设备** | `IoError` | `IoFailed` | **改名**：消除 `ErrorCode::IoError` 中 `Error` 词根重复，与 `DeviceLost` 分词形态一致 |
| | `DeviceLost` | `DeviceLost` | 音频设备拔出、失联或不存在 |
| | `EndOfStream` | `EndOfStream` | 有限流正常读至末尾（如非循环 WAV 文件） |
| | `Cancelled` | `Cancelled` | 阻塞 I/O 被主动 `stop()` 唤醒取消 |
| | `Timeout` | `Timeout` | 异步设备超出无进展截止时间 |

---

## 3. 核心错误原语设计（`core/retcode.h`）

保留 `core/retcode.h` 文件路径以减少头文件变更面，新增 `aereborn::Error` 异常类及 `RetCode::or_throw()` / `Result<T>::value_or_throw()` 桥接方法：

```cpp
#ifndef AEREBORN_CORE_RETCODE_H
#define AEREBORN_CORE_RETCODE_H

#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace aereborn
{

enum class ErrorCode
{
    Ok = 0,

    // Validation & capacity
    InvalidArg,
    InvalidPort,
    InvalidType,
    TooLarge,

    // Lifecycle & concurrency
    NotReady,
    Busy,

    // Graph topology & format
    UnknownNode,
    UnknownConn,
    DuplicateConn,
    InputBusy,
    Cycle,
    FormatMismatch,

    // External I/O & device status
    IoFailed,
    DeviceLost,
    EndOfStream,
    Cancelled,
    Timeout
};

class Error;

class RetCode
{
  public:
    constexpr RetCode(ErrorCode code = ErrorCode::Ok) noexcept : code_(code) {}
    constexpr explicit operator bool() const noexcept { return code_ == ErrorCode::Ok; }
    constexpr ErrorCode code() const noexcept { return code_; }

    friend constexpr bool operator==(RetCode lhs, RetCode rhs) noexcept { return lhs.code_ == rhs.code_; }
    friend constexpr bool operator!=(RetCode lhs, RetCode rhs) noexcept { return lhs.code_ != rhs.code_; }

    const char *message() const noexcept
    {
        switch (code_)
        {
        case ErrorCode::Ok:             return "Success";
        case ErrorCode::InvalidArg:     return "Invalid argument";
        case ErrorCode::InvalidPort:    return "Port index is out of range";
        case ErrorCode::InvalidType:    return "Node has a different type";
        case ErrorCode::TooLarge:       return "Requested capacity exceeds the supported size";
        case ErrorCode::NotReady:       return "Object is not prepared, committed, or bound";
        case ErrorCode::Busy:           return "Current execution context or state prevents this operation";
        case ErrorCode::UnknownNode:    return "Node does not belong to this engine or was removed";
        case ErrorCode::UnknownConn:    return "Connection does not exist";
        case ErrorCode::DuplicateConn:  return "These ports are already connected";
        case ErrorCode::InputBusy:      return "Input already has a source; use a mixer for multiple sources";
        case ErrorCode::Cycle:          return "Connection would create a feedback cycle";
        case ErrorCode::FormatMismatch: return "Formats or port layouts do not match";
        case ErrorCode::IoFailed:       return "I/O operation failed";
        case ErrorCode::DeviceLost:     return "Audio device is no longer available";
        case ErrorCode::EndOfStream:    return "End of stream";
        case ErrorCode::Cancelled:      return "I/O operation was cancelled";
        case ErrorCode::Timeout:        return "I/O operation timed out";
        }
        return "Unknown error code";
    }

    /// @brief Throw aereborn::Error if this code represents a failure.
    void or_throw() const;

  private:
    ErrorCode code_;
};

/// @brief Unified exception thrown by all control-plane AEReborn APIs.
class Error : public std::runtime_error
{
  public:
    explicit Error(ErrorCode code) : std::runtime_error(RetCode(code).message()), code_(code) {}
    Error(ErrorCode code, const char *detail) : std::runtime_error(detail), code_(code) {}
    Error(ErrorCode code, const std::string &detail) : std::runtime_error(detail), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

inline void RetCode::or_throw() const
{
    if (!*this)
        throw Error(code_);
}

/// @brief Status-or-value return type used by non-throwing backend I/O hooks.
template <class T> class Result
{
  public:
    Result(T value) noexcept(std::is_nothrow_move_constructible<T>::value) : value_(std::move(value)) {}
    Result(RetCode code) noexcept(std::is_nothrow_default_constructible<T>::value) : ret_code_(code) { assert(!code); }
    Result(ErrorCode code) noexcept(std::is_nothrow_default_constructible<T>::value) : Result(RetCode(code)) {}

    RetCode ret_code() const noexcept { return ret_code_; }
    explicit operator bool() const noexcept { return static_cast<bool>(ret_code_); }
    T &value() & noexcept { assert(ret_code_); return value_; }
    const T &value() const & noexcept { assert(ret_code_); return value_; }
    T value() && noexcept(std::is_nothrow_move_constructible<T>::value) { assert(ret_code_); return std::move(value_); }

    T value_or_throw() &&
    {
        ret_code_.or_throw();
        return std::move(value_);
    }

  private:
    RetCode ret_code_;
    T value_{};
};

} // namespace aereborn

#endif // AEREBORN_CORE_RETCODE_H
```

---

## 4. 逐模块改造清单

### 4.1 `core/`：控制路径全面去 `RetCode` / `Result<T>`

| 文件 | 改造前 | 改造后 | 关键细节 |
| :--- | :--- | :--- | :--- |
| **`core/block.h` & `core/types.cpp`** | `RetCode Buffer::resize(...)` | `void Buffer::resize(...)` | 零维度抛 `Error(InvalidArg)`，尺寸溢出抛 `Error(TooLarge)`，与 `std::bad_alloc` 同走异常路径。 |
| **`core/fring.h`** | `RetCode FrameRing::prepare(...)` | `void FrameRing::prepare(...)` | 容量超限抛 `Error(TooLarge)`，`storage_.resize()` 成功后才更新 `capacity_` 并 `clear()`，保持强异常安全。 |
| **`core/graph.h`** | `virtual RetCode prepare(BlockPeriod)` | `virtual void prepare(BlockPeriod)` | 自定义节点在 `prepare()` 中原本即可抛异常，统一后消除双轨检查。 |
| **`core/resampler.h` & `.cpp`** | `prepare` 返 `RetCode`；`max_*_frames`、`required_input`、`process` 返 `Result<...>` | 控制侧：`void prepare(...)`、`std::size_t max_output_frames(...)`、`std::size_t max_input_frames(...)` 抛 `Error`；<br>热路径：`std::size_t required_input(...) const noexcept`、`Transfer process(...) noexcept` 返回裸值 | `prepare` 内部局部构建 `history`/`kernel` 完成后再 `std::move`，失败保留旧状态；热路径 `required_input` / `process` 改用 `assert` 守卫前置条件，与 `decode_pcm` / `encode_pcm` / `process_tick` 保持一致。 |
| **`core/engine.h` & `.cpp`** | 全部控制接口返回 `RetCode` / `Result<T>`；`period_` 延迟到 `commit()` 校验 | `Engine(BlockPeriod)` 构造即校验 `is_block_period` 并抛 `Error(InvalidArg)`；<br>控制接口直接返回裸值：`NodeId add_node(...)`、`NodeId emplace_node<T>(...)`、`void remove_node(...)`、`void replace_node(...)`、`void set_paused(...)`、`Node *node(...)`、`T *node<T>(...)`、`ConnId connect(...)`、`void disconnect(...)`、`void commit()`、`void start()`、`void stop()`、`void reset()` | 1. `Engine::Graph` 与 `Engine::Runner` 内部同步移除所有 `RetCode`/`Result<T>` 包装。<br>2. `commit()` 先在局部 `Plan` 准备并测算，异常时原 `current_` 计划毫发无损。<br>3. `stop()` 仅在时钟线程重入时抛 `Error(Busy)`，控制线程永不抛；`~Engine()` 由 `Clock` 析构直接 `clock_->stop()` 保持 `noexcept`。 |

### 4.2 `plugins/`：统一抛 `aereborn::Error`，彻底零 `RetCode` / `Result<T>`

| 文件 | 改造内容 |
| :--- | :--- |
| **`plugins/convert.h` & `.cpp`** | 构造非法规格改抛 `Error(ErrorCode::InvalidArg, ...)`（替换 `std::invalid_argument`）；`void prepare(BlockPeriod)` 校验失败抛 `Error(InvalidArg)`，局部缓冲准备完毕后再 `std::move` 保持强异常安全。 |
| **`plugins/endpoint.h` & `.cpp`** | `Capture` / `Playback` 构造非法规格或非正 `io_period` 改抛 `Error(ErrorCode::InvalidArg, ...)`；`void prepare(BlockPeriod)` 按场景抛 `Error(InvalidArg)` / `Error(Busy)` / `Error(TooLarge)`；`process()` 保持 `noexcept`。 |
| **`plugins/matrix.h` & `.cpp`** | 构造非法参数/非有限系数改抛 `Error(ErrorCode::InvalidArg, ...)`，尺寸超限改抛 `Error(ErrorCode::TooLarge, ...)`（替换 `std::length_error`）；`void set_gain(float)` 在 `!std::isfinite(gain)` 时抛 `Error(InvalidArg)`；`void prepare(BlockPeriod)`；`process()` / `reset()` 保持 `noexcept`。 |
| **`plugins/mixer.h` & `.cpp`** | 构造改抛 `Error(ErrorCode::InvalidArg, ...)`；`void set_muted(std::size_t, bool)` 与 `bool muted(std::size_t) const` 在索引越界时抛 `Error(ErrorCode::InvalidPort)`（消灭 `Result<bool>`）。 |

### 4.3 `backend/`：统一控制面异常 + `noexcept` I/O 钩子 + 全库零 `try/catch`

| 文件 | 改造内容 |
| :--- | :--- |
| **`backend/backend.h` & `.cpp`** | 1. **控制接口统一抛异常**：`void bind(Capture&)`、`void bind(Playback&)`、`void start()` 全部返回 `void`，失败抛 `aereborn::Error`（未就绪抛 `NotReady`，已绑定/被占用/运行中抛 `Busy`，驱动 `on_open()` / `on_start()` 错误经 `.or_throw()` 透传）。<br>2. **`bind_endpoint()` 零 `try/catch`**：调用 `on_open(...).or_throw()`、`frames_in_period(...).value_or_throw()`、`SincResampler::max_*_frames(...)`、`resampler.prepare(...)`、`Buffer::resize(...)`，任何异常由局部 `ScopeExit` 自动执行 `on_close()` 与释放 `backend_bound_`。<br>3. **`start()` 零 `try/catch`**：去掉 `catch (const std::system_error &)`，与 `Engine::start()` 统一；工作线程 `on_start()` 失败时先 `worker_.join()` 再调 `start_result.or_throw()`。<br>4. **I/O 钩子全部 `noexcept`**：将 `on_read(OBBlock) noexcept`、`on_wait_writable() noexcept`、`on_write(IBBlock) noexcept` 标为 `noexcept`，删除 `transfer_capture`、`next_playback_batch`、`transfer_playback` 中全部 **3 处** `try/catch (...)`。<br>5. **热路径去冗余判断**：因 `SincResampler::required_input` 与 `process` 返回裸值，`emit_capture` 简化为 `void noexcept`，`playback_input_frames` 简化为 `std::size_t noexcept`，删去 `transfer_playback`、`transfer`、`run_sync_playback`、`drain_playback` 中全部 **5 处** 冗余错误检查分支。<br>6. 同步替换重命名后的错误码：`NotPrepared -> NotReady`、`IoError -> IoFailed`。 |
| **`backend/alsa_backend.h` & `.cpp`** | `on_read` / `on_wait_writable` / `on_write` 补齐 `noexcept`；`IoError` 全局替换为 `IoFailed`。 |
| **`backend/wasapi_backend.h` & `.cpp`** | `on_read` / `on_wait_writable` / `on_write` 补齐 `noexcept`；`IoError` 替换为 `IoFailed`；**顺手修复上轮核对发现的两处细节**：`on_open()` 拷贝前执行 `format_.fill(0)` 防止复绑残留旧字节；`on_read()` 在 `GetBuffer` 返回 `frames == 0` 时直接 `ReleaseBuffer(0)` 返回 `0`，避免 `memcpy` 空指针未定义行为。 |
| **`backend/wav_backend.h` & `.cpp`** | `on_read` / `on_write` 补齐 `noexcept`（标准库默认 `exceptions()` 掩码为 `goodbit`，不抛异常）；`IoError` 替换为 `IoFailed`。 |
| **`backend/null_backend.h` & `.cpp`** | `on_read` / `on_write` 补齐 `noexcept`。 |

### 4.4 `examples/clocked_graph.cpp` 简化效果

删除文件顶部的两个 `checked(...)` 包装模板，主流程直接直线书写：

```cpp
int main()
{
    try
    {
        std::atomic<float> level{0};
        Engine engine(BlockPeriod::Ms05);
        const auto tone   = engine.emplace_node<Tone>(SampleRate::Hz48000, 1);
        const auto mix    = engine.emplace_node<Mixer>(SampleRate::Hz48000, 1, 1);
        const auto matrix = engine.emplace_node<Matrix>(SampleRate::Hz48000, 1);
        const auto sink   = engine.emplace_node<Meter>(level);
        engine.connect(OPORT(tone, 0), IPORT(mix, 0));
        engine.connect(OPORT(mix, 0), IPORT(matrix, 0));
        engine.connect(OPORT(matrix, 0), IPORT(sink, 0));
        auto *gain = engine.node<Matrix>(matrix);
        engine.commit();
        engine.start();
        // ...
        gain->set_gain(0.5f);
        // ...
        engine.stop();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

---

## 5. 分阶段实施顺序与验收清单

### 实施步骤

1. **阶段 1：核心原语与内核/插件迁移**
   * 更新 `core/retcode.h`（整理 18 个 `ErrorCode`、新增 `aereborn::Error`、`RetCode::code()` / `or_throw()`、`Result<T>::value_or_throw()`）。
   * 改造 `core/block.h`、`core/types.cpp`、`core/fring.h`、`core/graph.h`、`core/resampler.h/.cpp`、`core/engine.h/.cpp`。
   * 改造 `plugins/convert.*`、`plugins/endpoint.*`、`plugins/matrix.*`、`plugins/mixer.*`（构造统一改抛 `aereborn::Error`）。
   * 清理 `examples/clocked_graph.cpp`。
2. **阶段 2：后端控制面统一与 I/O 热路径去 `try/catch`**
   * 改造 `backend/backend.h/.cpp`（`bind`/`start` 改抛 `Error`，`on_read`/`on_wait_writable`/`on_write` 标 `noexcept`，删除全部 `try/catch` 与 5 处重采样错误检查）。
   * 同步更新 `alsa_backend.*`、`wasapi_backend.*`（含两处边界修复）、`wav_backend.*`、`null_backend.*`。
3. **阶段 3：测试迁移与文档收口**
   * 在测试支撑头中新增极简断言工具 `expect_error(ErrorCode expected, Fn &&fn)`，统一替代原有错误码失败分支检查；正常路径直接去掉所有 `Result` 拆包样板。
   * 更新 `tests/support/test_backend.h`（I/O 钩子标 `noexcept`，错误码名同步为 `NotReady` / `InputBusy` / `IoFailed`）。
   * 同步更新 `CODING_GUIDELINES.md` 第 3 节（错误处理准则）、`AudioEngine_Outline.md` 第 7 节（修正异常描述并清理重复跳转句与旧词“周期偏离”）及 `AudioEngine_Backend_Plan.md`。

### 验收清单

| 验收维度 | 验收标准 |
| :--- | :--- |
| **错误原语洁净度** | `ErrorCode` 共 18 项，无未使用的 `BufferFull`，完成 `NotReady` / `InputBusy` / `IoFailed` 命名对称化；全库零 `std::invalid_argument` / `std::length_error` 抛出点，控制面全部统一为 `aereborn::Error`。 |
| **零胶水转换** | `core/`、`plugins/`、`backend/` 源码中 **0 处 `try/catch`**（仅示例 `examples/` 与测试 `tests/` 捕获校验异常）；`Backend` 全部 I/O 钩子均带 `noexcept`。 |
| **强异常安全与回滚** | `Engine::commit()` 失败保留旧渲染计划；`Convert::prepare()` / `SincResampler::prepare()` 失败保留旧缓冲；`Backend::bind()` 任一步抛异常（含 `on_open` 报错或 `std::bad_alloc`）均自动执行 `on_close()` 并释放端点占用；`Backend::start()` 在 `on_start()` 失败时安全 `join()` 线程并抛出对应 `ErrorCode`。 |
| **全量回归与实时性** | Debug 与 Release 下全量 CTest 通过；Release `allocation` 用例继续验证图渲染、设备热路径与重启工作区复用的零堆分配。 |