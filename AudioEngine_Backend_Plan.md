# 设备后端实施说明

## 层次与设备类型

`core/` 提供块视图、PCM 编解码、重采样器和 SPSC 帧环。`plugins/` 的 Capture 与 Playback 只负责图侧 F32P 队列。`backend/` 的单一 `Backend` 管理设备、I/O 线程、PCM 工作区和漂移控制。`aereborn::backend` 依赖 `aereborn::plugins` 和 `aereborn::core`。

`AlsaBackend`、`WasapiBackend` 和 `NullBackend` 根据 `bind(Capture&)` 或 `bind(Playback&)` 选择运行方向。`WavCapture` 和 `WavPlayback` 只接受各自方向。Null 与 WAV 跟随引擎时钟，ALSA 与 WASAPI 服务有独立时钟的设备。测试后端通过真实的绑定和启停流程运行，在测试桩内控制设备批次的就绪时机。

## 绑定和启停

- 先准备图端点，再在控制侧调用 `bind()`。同一端点只接受一个后端。绑定时协商 PCM 和帧数并准备工作区。`bind()` 通过 `aereborn::Error` 报告预期失败。打开或准备失败时，作用域清理会调用 `on_close()` 并释放端点占用。`on_close()` 必须能清理部分初始化的资源。标准库分配异常直接传播。
- `start()` 先回收已自行退出的工作线程，再启动新线程并等待 `on_start()` 的结果。初始化成功后上线端点。初始化失败时回收线程，再将设备错误以 `Error` 抛给调用方。线程创建异常直接传播，绑定可用于重试。
- `on_open()` 在控制线程接收逻辑端口的 `PortSpec` 和 I/O 周期，通过 `Result<DeviceFormat>` 返回协商后的 PCM 规格和批次上限或设备错误。资源准备异常可以传播。工作线程的启动、读取、等待可写和写入钩子使用 `noexcept`，通过 `RetCode` 或 `Result<std::size_t>` 报告故障。应用通过 `last_error()` 读取运行期终止原因。
- `stop()` 取消 I/O 或排空同步播放队列，再等待线程退出。`on_cancel()` 由控制侧触发，`on_start()` 和 `on_stop()` 在 I/O 线程上成对执行。正常退出、设备故障和主动取消共用线程收尾流程。
- `stop()` 保留绑定和工作区以便重启。`unbind()` 停机后关闭资源并释放端点。后端必须在端点销毁前解绑，具体驱动的析构函数先调用 `unbind()`。

异步播放、同步播放和同步排空分别控制读取时机，共用重采样、PCM 编码和写入逻辑。异步播放不足一个设备批次时补静音，升采样有时可仅凭 FIR 历史输出。ASRC 根据队列水位调整采样速率，在后端离线时复位。

同步播放停止时关闭图侧写入，写完队列和 FIR 尾部。同步采集遇到 EOF 后冲刷 FIR，让图消费剩余帧，主动停止可取消排空。

## Null 和 WAV

`NullBackend` 在构造时校验交错 PCM 格式，非法格式报 `InvalidArg`。`WavPlayback` 同样在构造时校验，只接受 S16LE、S24_3LE、S32LE 和 F32LE，非法格式报 `FormatMismatch`。

WAV 文件读写集中在 `WavFile`。读写和定位以帧为单位，读取兼容 RIFF 的附加块及奇数字节填充。16 位、24 位、32 位整数 PCM 和 32 位浮点保留样本字节，8 位无符号 PCM 和 64 位浮点以 F32LE 交付。

WAV 写入使用标准 I/O 缓冲。播放停止时在排空后将缓冲提交给操作系统，可通过 `last_error()` 查询刷新失败。分卷和解绑时关闭当前文件，回填 RIFF、数据和帧数信息，并保留已经发生的写入错误。

WAV 采集可读一遍、无限循环，或通过 `WavCapture::Repeat` 指定额外读取次数。重启从文件游标续读，重新绑定从头开始。WAV 播放写入单个文件，或按显式指定的分卷时长切分录音。分卷采用从 1 开始的编号，如 `recording-0000000001.wav`，并跳过已有编号。

## 异步超时

异步设备从最近一次非空设备批次算起，使用 `max(500 ms, 10 × io_period())` 的无进展期限。毫秒等待向上取整并限制在平台等待参数可表达的范围。ALSA `poll()` 和 WASAPI `WaitForMultipleObjects()` 使用剩余期限，播放的就绪等待与分段写入共享同一截止时间。主动停止通过独立信号唤醒等待。只有实际的 I/O 进展才刷新期限。

## ALSA

- 空名称和 `default` 使用系统默认 PCM。裸声卡名和设备号构成 `hw:CARD=<card>,DEV=<device>`。完整 ALSA PCM 名称可直接传入，例如 `hw:0,0`、`hw:0` 或 `plug:null`。
- 构造参数 `use_mmap` 为 `true` 时优先协商交错 MMAP，规格不匹配时重新协商普通交错读写。设为 `false` 时直接使用普通交错读写。
- 优先协商两周期硬件缓冲，必要时按两倍周期帧数协商缓冲大小。报告给上层的设备批次受端点预算限制。播放起播和就绪使用实际批次帧数。
- 播放启用 ALSA 的静音预填，停止阈值沿用默认值以保留 XRUN 检测。XRUN 重新准备 PCM。系统挂起时尝试恢复，重试间隔由取消事件和同一 I/O 截止时间约束。
- 设备断开返回 `DeviceLost`，无进展返回 `Timeout`，主动取消返回 `Cancelled`。虚拟设备可用于验证 I/O 和故障注入，物理时延和拔插表现需实机验证。

## WASAPI

WASAPI 在绑定时校验显式 Endpoint ID 的方向，并使用设备原生混音格式。COM 流和服务在同一个 I/O 线程上创建和释放。采集取得零帧时直接等待下一次就绪事件，按 API 约定，零长度包可以省略 `ReleaseBuffer(0)`。播放在启动前填满一包静音，运行时整包静音沿用 WASAPI 静音标志，部分欠载时补齐尾部。控制侧可在工作线程停下后用 `last_failure()` 查询失败操作和原始 HRESULT。 [采集缓冲契约](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) · [播放启动契约](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-start)

Windows 构建包含 WASAPI，SDK 目标要求 `_WIN32_WINNT >= 0x0600`。Linux 在找到 ALSA 开发库时包含 ALSA。
