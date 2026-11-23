# AEReborn 后端抽象基类与 ALSA 修改实施计划

本计划基于当前仓库代码现状，聚焦第一阶段后端重构：**清理 `detail` 命名空间与 `backend/pcm.h`，将双基类收敛为单一抽象基类 `Backend`，并完成 Linux `AlsaBackend` 的单类改造与细节修正**。`core/` 全量代码及 `plugins/convert.*`、`plugins/endpoint.cpp` 主体逻辑保持不变；其余后端（`WasapiBackend`、`NullBackend`、`WavCapture` / `WavPlayback`、`TestBackend`）按第 3 节顺序逐个迁移。

---

## 1. 现状差异与核心精简清单

对照当前代码与目标架构，本阶段完成以下 7 项精确调整：

| 维度 | 当前代码现状 | 本阶段改造目标 |
| :--- | :--- | :--- |
| **抽象基类** | `CaptureBackend` / `PlaybackBackend` 两套平行类，11 个重复成员 + 7～9 参数模板转发 | 合并为单一基类 `Backend`，通过 `bind(Capture&)` / `bind(Playback&)` 编译期绑定方向 |
| **`detail` 与 `pcm.h`** | 存在 `detail::EndpointBinding`、`detail::BackendWorkspace`、`detail::AlsaStream` 与 `backend/pcm.h` | 彻底删除 `detail` 命名空间与 `backend/pcm.h`：`endpoint.h` 改为 `friend class Backend;`；`frames_in_period()` 收为 `Backend` 受保护静态方法 |
| **公有生命周期** | `set_online()` / `step()` 暴露在 `public`，与实机驱动契约冲突 | 降为 `protected`，公有动词收敛为 `bind` / `start` / `stop` / `unbind`；`TestBackend` 通过 `using` 暴露 |
| **启停钩子与 `start()`** | 4 个钩子（`on_start` / `on_worker_start` / `on_worker_stop` / `on_stop`），`start()` 建线程即返回 `Ok` | 合并为 3 个钩子（`on_start` / `on_stop` / `on_cancel`）；`on_start()` 在工作线程执行，由 `start()` 阻塞握手并同步返回真实错误码 |
| **状态机与超时** | 双端竞争 `request_stop()`；保留 `PeriodMonitor` 且 ALSA `poll(-1)` 无限阻塞 | 收敛为 4 态机（`kStopped` / `kRunning` / `kDraining` / `kStopping`），`on_cancel()` 仅由控制侧 `stop()` 触发；删除 `PeriodMonitor`，统一使用 `io_timeout()` 下传 `poll()` 并在异步循环做 4 行无数据超时判定 |
| **ALSA 类形态与命名** | `detail::AlsaStream` + `AlsaCapture` + `AlsaPlayback` 三类，裸透传设备名 | 合并为单类 `AlsaBackend`，支持 `hw:CARD=<card>,DEV=<device>` 设备名规整 |
| **ALSA 参数与恢复** | 已实现 `configure_pcm()` 回退重跑与 `SND_PCM_STATE_DISCONNECTED` 判断；缺少 `periods = 2` 与 `silence_size`，且 `start_threshold` 误用 `period` | **保留** `configure_pcm()` 干净重跑与 `SND_PCM_STATE_DISCONNECTED` 兜底；**补齐** `periods = 2`、`silence_threshold = 0` / `silence_size = boundary` 与可取消 `-ESTRPIPE` 恢复；**修正** `start_threshold = frames`；保持默认 `stop_threshold` |

---

## 2. 逐文件修改方案

### 2.1 `plugins/endpoint.h`（清理 `detail` 友元）

保持 `plugins/endpoint.cpp` 全部队列与屏障逻辑不动，仅在 `plugins/endpoint.h` 中做两处替换：

1. 将顶部的 `namespace detail { struct EndpointBinding; }` 替换为前向声明 `class Backend;`。
2. 将 `Capture` 与 `Playback` 私有区的 `friend struct detail::EndpointBinding;` 替换为 `friend class Backend;`。

这样可以继续把 `backend_bound_` 封装为私有成员，同时彻底消除 `plugins/` 中的 `detail` 命名空间。

---

### 2.2 删除 `backend/pcm.h`

删除仅含单个函数的 `backend/pcm.h`，将 `frames_in_period(SampleRate rate, std::chrono::microseconds period)` 移入 `Backend` 的 `protected static` 方法中，供 `Backend` 与所有具体驱动直接调用。

---

### 2.3 重写 `backend/backend.h` & `backend/backend.cpp`

#### 2.3.1 `backend/backend.h` 接口定义

```cpp
#ifndef AEREBORN_BACKEND_BACKEND_H
#define AEREBORN_BACKEND_BACKEND_H

#include "core/resampler.h"
#include "drift.h"
#include "plugins/endpoint.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace aereborn
{

/// @brief Common base for interleaved PCM backends bound to a Capture or Playback node.
/// @note Borrows the bound endpoint; unbind or destroy before removing the node or destroying Engine.
/// @note Serialize bind(), start(), stop(), unbind(), and destruction on the control side.
/// @note CRITICAL: Concrete derived destructors MUST call unbind() as their first statement.
class Backend
{
  public:
    virtual ~Backend();

    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    /// @brief Open the source via on_open(), prepare DSP buffers, and bind to a prepared Capture node.
    RetCode bind(Capture &endpoint);
    /// @brief Open the sink via on_open(), prepare DSP buffers, and bind to a prepared Playback node.
    RetCode bind(Playback &endpoint);

    /// @brief Launch the I/O thread, wait for on_start() to finish on it, and bring the endpoint online.
    RetCode start();
    /// @brief Stop I/O, join the thread, take the endpoint offline, and reset DSP/drift state.
    /// @note Sync playback closes graph writes and drains queued frames and the FIR tail before stopping.
    /// @note Keeps the endpoint bound and prepared buffers intact for fast restart.
    void stop() noexcept;
    /// @brief Stop I/O, release resources via on_close(), reset workspace, and detach the endpoint.
    void unbind() noexcept;

    bool async() const noexcept { return async_; }
    bool bound() const noexcept { return capture_ != nullptr || playback_ != nullptr; }
    bool is_capture() const noexcept { return capture_ != nullptr; }
    bool running() const noexcept;
    RetCode last_error() const noexcept { return last_error_.load(std::memory_order_acquire); }
    double correction() const noexcept { return correction_.load(std::memory_order_relaxed); }
    PortSpec device_spec() const noexcept { return bound() ? workspace_.device_spec : PortSpec{}; }
    std::size_t device_max_frames() const noexcept { return bound() ? workspace_.max_device_frames : 0; }

  protected:
    /// @param async True for external-clock hardware (ASRC + PI + timeout); false for graph-clock backends.
    explicit Backend(bool async) noexcept : async_(async) {}

    /// @brief Deterministic threadless stepping controls for test subclasses (Busy while running).
    RetCode set_online(bool online) noexcept;
    Result<std::size_t> step() noexcept;

    /// @brief Compute frames contained in one period at the given sample rate.
    static Result<std::size_t> frames_in_period(SampleRate rate, std::chrono::microseconds period) noexcept;

    /// @brief Inactivity timeout shared by async loop monitoring and blocking platform waits.
    std::chrono::milliseconds io_timeout() const noexcept;

    /// @brief Open underlying device/file and report negotiated interleaved PCM format and max batch size.
    /// @note Runs on the control thread. Leave concrete resources closed on failure or exception.
    virtual RetCode on_open(bool capture, SampleRate preferred_rate, std::size_t channels,
                            std::chrono::microseconds target_period,
                            PortSpec &out_spec, std::size_t &out_max_frames) = 0;

    /// @brief Start the stream and acquire any thread-affine resources on the I/O worker thread.
    /// @note Synchronized with start(); on failure, roll back before returning the error.
    virtual RetCode on_start() noexcept { return {}; }

    /// @brief Stop the stream and release worker-thread resources after a successful on_start().
    virtual void on_stop() noexcept {}

    /// @brief Wake blocked I/O hooks promptly so they return Cancelled. Idempotent and non-blocking.
    /// @note Called only from control-side stop() while bound; must remain safe until on_close().
    virtual void on_cancel() noexcept {}

    /// @brief Release handles acquired in on_open() on the control thread after the worker has joined.
    virtual void on_close() noexcept {}

    /// @brief Read one batch of interleaved PCM frames (capture backends override this).
    virtual Result<std::size_t> on_read(OBBlock output);

    /// @brief Wait until writable and return next batch size in [1, device_max_frames()].
    virtual Result<std::size_t> on_wait_writable() { return device_max_frames(); }

    /// @brief Write one batch of interleaved PCM frames (playback backends override this).
    virtual RetCode on_write(IBBlock input);

  private:
    struct Workspace
    {
        PortSpec device_spec;
        std::size_t max_device_frames = 0;
        std::size_t logical_frames = 0;
        SincResampler resampler;
        Buffer device_planar;  ///< Allocated only when resampler.needs_fir() is true.
        Buffer logical_planar;
        std::vector<std::uint8_t> device_bytes;
    };

    struct StartSync
    {
        RetCode code;
        bool done = false;
    };

    template <class Endpoint> RetCode bind_endpoint(Endpoint &endpoint, Endpoint *&slot, bool capture);
    void set_endpoint_online(bool online) noexcept;
    bool endpoint_online() const noexcept;
    std::chrono::microseconds endpoint_period() const noexcept;
    void wake_waiter() noexcept;
    void wait_backpressure() noexcept;
    void go_offline(RetCode error = {}) noexcept;
    void update_drift(std::size_t before, std::size_t device_frames) noexcept;

    Result<std::size_t> playback_input_frames(std::size_t device_frames, double scale) const noexcept;
    Result<std::size_t> emit_capture(IFBlock input, double scale) noexcept;
    Result<std::size_t> transfer_capture(std::size_t &device_frames, double scale) noexcept;
    Result<std::size_t> next_playback_batch() noexcept;
    Result<std::size_t> transfer_playback(std::size_t device_frames, std::size_t requested,
                                          std::size_t *tail = nullptr) noexcept;

    void run(StartSync &startup) noexcept;
    RetCode run_async() noexcept;
    RetCode run_sync() noexcept;
    RetCode drain_capture() noexcept;
    RetCode drain_playback() noexcept;

    const bool async_;
    Capture *capture_ = nullptr;
    Playback *playback_ = nullptr;
    Workspace workspace_;
    DriftController drift_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool wake_ = false;
    bool has_input_ = false;
    std::thread worker_;
    std::atomic<int> worker_state_{0};
    std::atomic<RetCode> last_error_{RetCode{}};
    std::atomic<double> correction_{0.0};
};

} // namespace aereborn

#endif
```

#### 2.3.2 `backend/backend.cpp` 关键实现约束

1. **绑定校验与独占回滚（`bind_endpoint`）**：
   * 依次校验 `!bound()`（`Busy`）、`endpoint.capacity() > 0`（`NotPrepared`）与 `endpoint.backend_bound_.exchange(true, acq_rel)`（已被占用返回 `Busy`）。
   * 挂局部 `ScopeExit` 守卫，调用 `on_open(...)` 后校验：合法交错 PCM、声道数与端点一致、$1 \le \text{frames} \le \text{frames\_in\_period}(\text{device\_rate}, \text{io\_period})$、重采样逻辑帧上限 $\le \text{target\_frames}()$。
   * 仅在 `resampler.needs_fir()` 为真时分配 `device_planar`；任一步失败或抛异常自动调用 `on_close()`（若已打开）并重置 `endpoint.backend_bound_` 为 `false`。
2. **同步启动握手（`start` + `run`）**：
   * `start()` 先校验 `bound()` 与 `!running()`，清理可能残留的已退出线程或手动在线状态，清零 `last_error_` 与 `wake_`，启动工作线程并在 `wait_mutex_` / `wait_cv_` 上等待 `startup.done`。
   * 工作线程入口：若 `async_` 为真则调 `configure_audio_thread(is_capture() ? "ae-capture" : "ae-playback")`，随后执行 `on_start()`。
   * 若 `on_start()` 失败，直接通知 `startup` 失败并退出线程，`start()` 执行 `worker_.join()` 后同步返回真实错误码（端点全程不上报在线）。
   * 若 `on_start()` 成功，先 `set_endpoint_online(true)`、置 `worker_state_ = kRunning`，再唤醒 `start()` 返回 `Ok`。
3. **4 态状态机与停机收尾（`stop` + `run` 尾部）**：
   * 仅保留 `kStopped = 0`、`kRunning`、`kDraining`、`kStopping` 四态，去掉 `kStarting` 与双端竞争的 `cancel_io()`。
   * `stop()` 在 `worker_.joinable()` 时：
     * 同步播放（`!async_ && playback_ != nullptr`）：先调 `playback_->close_graph()` 切断图侧写入，再执行 `CAS(kRunning -> kDraining)` 并 `wake_waiter()`，等待工作线程排空队列与 FIR 尾部；
     * 其余情形：执行 `CAS(kRunning -> kStopping)`，成功后调用 `wake_waiter()` 与 `on_cancel()` 唤醒阻塞 I/O。
     * `worker_.join()` 后若端点仍在线，兜底调用 `go_offline()`。
   * 工作线程退出循环后：依次调用 `on_stop()` $\to$ 读取 `worker_state_ == kStopping`（控制侧主动取消时清零错误，自退或同步排空写盘失败时保留错误码） $\to$ `go_offline(...)` $\to$ `worker_state_.store(kStopped, release)`。
4. **无数据超时模型（替代 `PeriodMonitor`）**：
   * `io_timeout()` 返回 $\max(500\text{ ms},\; 10 \times \text{endpoint\_period}())$。
   * `run_async()` 中记录 `last_active = steady_clock::now()`，每拍成功后判断 `now - last_active > timeout` 即返回 `ErrorCode::Timeout`；仅当 `device_frames > 0` 时更新 `last_active`。
5. **同步播放去重计算与假欠载防护**：
   * 抽取 `playback_input_frames(device_frames, scale)`，供 `run_sync()` 背压等待与 `transfer_playback()` 复用，不再重复调用 `required_input()`。
   * 同步模式从队列读取量上限取 $\min(\text{requested},\; \text{playback\_->available}())$，防止排空最后残余块触发假的 `underrun_` 计数。
6. **解绑与析构守卫**：
   * `unbind()` 依次执行 `stop()` $\to$ `on_close()` $\to$ 释放 `backend_bound_` $\to$ 清空端点指针与 `workspace_`。
   * `~Backend()` 内置 `assert(!bound() && "Derived backend destructor must call unbind()");`。

---

### 2.4 重写 `backend/alsa_backend.h` & `backend/alsa_backend.cpp`

#### 2.4.1 `backend/alsa_backend.h` 接口定义

```cpp
#ifndef AEREBORN_BACKEND_ALSA_BACKEND_H
#define AEREBORN_BACKEND_ALSA_BACKEND_H

#include "backend.h"

#ifdef AEREBORN_HAS_ALSA
#include <poll.h>
#include <string>
#include <vector>

struct _snd_pcm;

namespace aereborn
{

/// @brief External-clock Linux ALSA backend supporting both Capture and Playback.
/// @note Device naming: empty or "default" -> "default"; "hw:...,..." is kept as-is;
///       otherwise normalized to "hw:CARD=<card>,DEV=<device>".
/// @note Prefers MMAP_INTERLEAVED when use_mmap is true and falls back to RW_INTERLEAVED.
class AlsaBackend final : public Backend
{
  public:
    explicit AlsaBackend(std::string card = "default", unsigned int device = 0, bool use_mmap = true);
    ~AlsaBackend() override { unbind(); }

  private:
    RetCode on_open(bool capture, SampleRate preferred_rate, std::size_t channels,
                    std::chrono::microseconds period, PortSpec &spec, std::size_t &frames) override;
    RetCode on_start() noexcept override;
    void on_stop() noexcept override;
    void on_cancel() noexcept override;
    void on_close() noexcept override;

    Result<std::size_t> on_read(OBBlock output) override;
    Result<std::size_t> on_wait_writable() override;
    RetCode on_write(IBBlock input) override;

    RetCode wait_ready() noexcept;
    RetCode recover(int error) noexcept;

    std::string device_;
    const bool use_mmap_;
    bool mmap_ = false;
    bool active_ = false;
    _snd_pcm *pcm_ = nullptr;
    std::vector<pollfd> poll_;
};

} // namespace aereborn

#endif
#endif
```

#### 2.4.2 `backend/alsa_backend.cpp` 实现规范（融合现有改进与修复项）

1. **设备名规整（匿名命名空间 `normalize_device_name`）**：
   * `card.empty() || card == "default"` $\to$ `"default"`；
   * `card.find("hw:") == 0 && card.find(',') != std::string::npos` $\to$ 原样保留 `card`；
   * 其余情况 $\to$ `"hw:CARD=" + card + ",DEV=" + std::to_string(device)`。
2. **硬件参数协商（保留 `configure_pcm()` 干净重试结构）**：
   * `snd_pcm_open(..., SND_PCM_NONBLOCK)` 打开句柄。
   * 将硬件参数设置封装在局部 `configure_pcm(handle, access, ...)`：当 `use_mmap_` 为真且 `SND_PCM_ACCESS_MMAP_INTERLEAVED` 返回 `FormatMismatch` 时，重新执行 `snd_pcm_hw_params_any()` 以 `SND_PCM_ACCESS_RW_INTERLEAVED` 完整重配一次，避免残留脏参数状态。
   * 声道数严格匹配（不符返回 `FormatMismatch`）；按 `F32LE` $\to$ `S32LE` $\to$ `S24_3LE` $\to$ `S24LE` $\to$ `S16LE` 探测格式；`snd_pcm_hw_params_set_rate_near()` 协商并校验结果为合法 `SampleRate`。
   * 按 `frames_in_period(rate, period)` 计算预算，以 `dir = -1` 调 `snd_pcm_hw_params_set_period_size_near()`；随后优先调 `snd_pcm_hw_params_set_periods_near(..., 2, ...)` 将硬件总缓冲收敛在 2 个周期（若驱动不支持再回退 `snd_pcm_hw_params_set_buffer_size_near(..., 2 * period_size)`），回报 `frames = std::min<std::size_t>(period_size, budget)`。
3. **软件参数配置（修复 `start_threshold` 错配 + 补静音保护）**：
   * 双向设 `snd_pcm_sw_params_set_avail_min(handle, sw, frames)`。
   * 播放方向设：
     * `snd_pcm_sw_params_set_start_threshold(handle, sw, frames)`（**修正现代码中误用 `period` 导致首包不满起播阈值的问题**）；
     * `snd_pcm_sw_params_set_silence_threshold(handle, sw, 0)`；
     * `snd_pcm_sw_params_get_boundary(sw, &boundary)` 与 `snd_pcm_sw_params_set_silence_size(handle, sw, boundary)`。
   * **`stop_threshold` 保持 ALSA 默认行为**，不设为 `ULONG_MAX` / `boundary`，确保欠载时由 ALSA 报告 `-EPIPE` 并通过 `recover()` 干净重对齐硬件指针。
4. **等待、有限超时与断连判定（`wait_ready`）**：
   * `on_open()` 收集 `snd_pcm_poll_descriptors` 并追加 `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)`。
   * `wait_ready()` 以 `static_cast<int>(io_timeout().count())` 调用 `::poll()`：
     * `rc == 0` $\to$ 返回 `ErrorCode::Timeout`；
     * `rc < 0` 且 `errno == EINTR` 重试，否则返回 `ErrorCode::IoError`；
     * `poll_.back().revents & POLLIN` $\to$ 返回 `ErrorCode::Cancelled`；
     * 调 `snd_pcm_poll_descriptors_revents` 后，**保留现有稳健判定顺序**：若 `(events & (POLLHUP | POLLNVAL))` 或 `((code < 0 || (events & POLLERR)) && snd_pcm_state(pcm_) == SND_PCM_STATE_DISCONNECTED)`，直接返回 `ErrorCode::DeviceLost`；再判 `code < 0` 返回 `alsa_error(code)`；最后 `events & (POLLIN | POLLOUT | POLLERR)` 返回就绪。
5. **工作线程启停、取消与恢复**：
   * `on_start()`（工作线程）：先排空 `eventfd` 残留信号，调用 `snd_pcm_prepare()`，采集流再调用 `snd_pcm_start()`（失败时 `snd_pcm_drop()` 回滚），成功置 `active_ = true`。
   * `on_stop()`（工作线程）：执行 `snd_pcm_drop()` 并置 `active_ = false`。
   * `on_cancel()`（控制线程）：向 `eventfd` 写 `std::uint64_t{1}`，仅依赖 `eventfd` 在 `on_close()` 前持续有效。
   * `on_close()`（控制线程）：关闭 `eventfd` 并 `snd_pcm_close(pcm_)`。
   * `recover()`：`-EPIPE` 直接 `snd_pcm_prepare()`（采集追加 `snd_pcm_start()`）；`-ESTRPIPE` 循环调 `snd_pcm_resume()` 最多 10 次，每次 `-EAGAIN` 用对 `eventfd` 的 50 ms `::poll()` 等待以便被 `stop()` 立即取消，若遇 `-ENODEV` / `-ENXIO` 直接返回 `DeviceLost`，仍未恢复则回退 `snd_pcm_prepare()`。
6. **读写与部分写补齐**：
   * `on_read()` / `on_write()` 按 `mmap_` 分支调用 `snd_pcm_mmap_readi/writei` 或 `snd_pcm_readi/writei`。
   * `on_write()` 处理部分写入时循环调用 `on_wait_writable()`，并以 `std::min(next.value(), input.frames() - written)` 补齐剩余帧。

---

## 3. 过渡期构建说明与后续逐个迁移顺序

> **构建提示**：由于当前 `backend/CMakeLists.txt` 同时编译 `null_backend.cpp`、`wav_backend.cpp` 与（Windows 下的）`wasapi_backend.cpp`，在仅改完 `Backend` 与 `AlsaBackend` 的中间状态，尚未迁移的后端与测试桩仍引用旧的 `CaptureBackend` / `PlaybackBackend` 和 `backend/pcm.h`。建议第一阶段落地后可先单独编译 `backend.cpp` 与 `alsa_backend.cpp` 验证语法与 ALSA 单测，再按下列顺序完成其余迁移使全工程重新全绿：

1. **阶段 1（本计划）**：修改 `plugins/endpoint.h`，删除 `backend/pcm.h`，落地单基类 `backend/backend.*` 与单类 `backend/alsa_backend.*`。
2. **阶段 2（`WasapiBackend`）**：合并 `detail::WasapiStream` + `WasapiCapture` + `WasapiPlayback` 为单类 `WasapiBackend final : public Backend`；用 `io_timeout()` 替换 `WaitForMultipleObjects(..., INFINITE)`；工作线程 COM 生命周期直接放进 `on_start()` / `on_stop()`。
3. **阶段 3（`NullBackend` & `WavCapture` / `WavPlayback`）**：
   * `NullBackend` 合并为单类 `NullBackend final : public Backend`（`async = false`）。
   * `WavCapture` / `WavPlayback` 继承 `Backend(/*async=*/false)`，通过 `using Backend::bind; RetCode bind(Playback&) = delete;`（及对称删除）在编译期锁定方向；`WavPlayback` 去掉 `pubsetbuf(nullptr, 0)` 改为栈上拼装 44 / 58 字节单次写头；明确 `WavCapture::on_start()` 游标复位或续播语义。
4. **阶段 4（`TestBackend`、全量回归与文档清理）**：
   * 更新 `tests/support/test_backend.h`，继承 `Backend` 并通过 `using Backend::set_online; using Backend::step;` 暴露单步测试能力。
   * 在 Debug / Release 下运行全量 CTest（含 Release `allocation` 零堆分配检查）。
   * 删除根目录过时的 `aereborn.md` 与 `aereborn_alsa.md`，同步刷新 `AudioEngine_Backend_Plan.md` 与 `AudioEngine_Outline.md`。

---

## 4. 第一阶段验收清单

| 验收项 | 检查标准 |
| :--- | :--- |
| **结构洁净度** | `backend/` 与 `plugins/endpoint.h` 零 `detail` 命名空间；`backend/pcm.h` 已删除；`Backend` 公有控制方法仅 `bind` / `start` / `stop` / `unbind`，无 `PeriodMonitor`。 |
| **绑定与独占保护** | 未 `prepare` 返回 `NotPrepared`；已绑定或端点已被另一后端占用返回 `Busy`；`on_open()` 后任意校验失败均执行 `on_close()` 并释放 `backend_bound_`。 |
| **同步启动与停机** | `on_start()` 失败时 `start()` 同步返回真实错误码、端点未上线且 `running() == false`；`stop()` 仅由控制侧触发 `on_cancel()` 且 `Cancelled` 不写入 `last_error()`；同步播放 `stop()` 排空队列与 FIR 尾部且无假欠载；同步采集 EOS 冲刷 FIR 尾部且可被 `stop()` 取消。 |
| **异步无数据超时** | 硬件静默无事件时 ALSA `poll()` 在 `io_timeout()` 后返回 `Timeout`；驱动持续返回 0 帧时 `run_async()` 在 `io_timeout()` 后返回 `Timeout` 并自动下线。 |
| **ALSA 实机/虚拟设备** | 设备名规整覆盖 `""`、`"default"`、`"hw:0,0"`、`"hw:CARD=Loopback,DEV=1"` 与 `("Loopback", 1)`；MMAP 优先且 `configure_pcm()` 干净回退 RW；缓冲收敛在 2 个周期；播放 `start_threshold == frames`；热拔出/失联返回 `DeviceLost`；`-ESTRPIPE` 重试可被 `stop()` 及时取消。 |