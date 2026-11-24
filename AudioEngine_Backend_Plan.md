# 设备后端实施说明

## 层次与设备类型

`core/` 提供块视图、PCM 编解码、重采样器和 SPSC 帧环。`plugins/` 的 Capture 与 Playback 负责图侧 F32P 队列。`Backend` 统一管理端点占用、PCM 转换和平面工作区，并提供多态启停与状态查询。`PassiveBackend` 由调用方驱动，`ActiveBackend` 管理工作线程、设备字节缓冲和异步漂移控制。`aereborn::backend` 依赖 `aereborn::plugins` 和 `aereborn::core`。

`AlsaBackend`、`WasapiBackend` 和 `NullBackend` 根据 `bind(Capture&)` 或 `bind(Playback&)` 选择运行方向。`WavCapture` 和 `WavPlayback` 只接受各自方向。Null 与 WAV 跟随引擎时钟，ALSA 与 WASAPI 服务有独立时钟的设备。测试后端通过真实的绑定和启停流程运行，在测试桩内控制设备批次的就绪时机。

`PushSource` 和 `PullSink` 继承 `PassiveBackend`，分别绑定 `Capture` 和 `Playback`。`UsrPlayback` 继承 `ActiveBackend`，在工作线程交付图侧输出。设备的绑定入口位于 `ActiveBackend`，调用方可用 `Backend&` 统一启停和查询状态。

## 绑定和启停

- 先准备图端点，再在控制侧调用 `bind()`。同一端点只接受一个外部驱动。`Backend` 预占端点，准备完毕后才发布绑定。`bind()` 通过 `aereborn::Error` 报告预期失败。工作线程驱动打开或准备失败时先清空已协商状态，再由 `on_close()` 清理设备资源，最后释放端点占用。标准库分配异常直接传播。
- `ActiveBackend::start()` 先回收已退出的工作线程，再启动新线程并等待 `on_start()` 的结果。初始化成功后上线端点。初始化失败时回收线程，再以 `Error` 报告设备错误。线程创建异常直接传播，绑定可用于重试。
- `on_open()` 在控制线程接收逻辑端口的 `PortSpec` 和 I/O 周期，通过 `Result<PortSpec>` 返回协商后的 PCM 规格或设备错误。驱动通过 `set_device_max_frames()` 将批次上限写入工作区，应用可用 `device_max_frames()` 查询。资源准备异常可以传播。工作线程的启动、读取、等待可写和写入钩子使用 `noexcept`，通过 `RetCode` 或 `Result<std::size_t>` 报告故障。应用通过 `last_error()` 读取运行期终止原因。
- `ActiveBackend::stop()` 取消 I/O 或排空同步播放队列，再等待线程退出。`on_cancel()` 由控制侧触发。`on_start()` 成功后，`on_stop()` 在同一 I/O 线程执行。正常退出、设备故障和主动取消共用线程收尾流程。
- `stop()` 保留绑定和工作区以便重启。`unbind()` 停机后关闭资源并释放端点。后端必须在端点销毁前解绑，具体驱动的析构函数先调用 `unbind()`。

异步播放、同步播放和同步排空分别控制读取时机，共用重采样、PCM 编码和写入逻辑。异步播放不足一个设备批次时补静音，升采样有时可仅凭 FIR 历史输出。ASRC 根据队列水位调整采样速率，在后端离线时复位。

同步播放停止时关闭图侧写入，写完队列和 FIR 尾部。同步采集遇到 EOF 后冲刷 FIR，让图消费剩余帧，主动停止可取消排空。

## PushSource

构造时指定固定的交错 PCM `PortSpec`，声道数须与 `Capture` 一致。绑定失败时释放端点占用及准备中的工作区。内部切片时长取端点 I/O 周期与 10 ms 的较小值。推入的数据直接解码为 F32P，采样率不同时经过固定比率的 FIR 重采样，再写入图侧队列。推流路径使用绑定时准备的缓冲，输入 PCM 直接从调用方缓冲读取。

`start()` 在调用线程开放图侧读取，无需启动预填充。`stop()` 等待在途队列读取结束，丢弃未消费的帧并复位重采样器。FIR 尾音随复位丢弃。绑定和工作区保留到 `unbind()`，可以直接重启。

`push(IBBlock)` 返回实际接收的输入帧数，短写后从该帧偏移继续推送。有效输入在后端未运行或帧数为零时返回 `0`。启用 FIR 时，每个切片按输出帧数上界预留空间，因此队列尚有空位时也可能返回 `0`。图侧缺帧时补静音，输入间隔不触发 I/O 超时。

同一实例的操作、状态查询和析构由调用方串行执行，例如使用单消费者指令队列。调用方按顺序投递同一流的分片，切换流时先停止接收旧流数据。`IBBlock` 借用的 PCM 缓冲至少保留到对应的 `push()` 返回。

多路输入各自绑定独立 `Capture`，然后连接到 `Mixer` 的不同输入。单声道 `Capture` 接入立体声混音时，可用 `engine.emplace_node<Matrix>(rate, 1, 2, std::initializer_list<float>{1.0f, 1.0f})` 将同一声道送到左右两侧。

## PullSink

`PullSink` 绑定 `Playback`。调用方提供固定规格的交错 PCM 可写缓冲，`pull(OBBlock)` 返回实际写入的输出帧数，未写入的区间保留原样。拉取立即返回可用帧，无需等待图补数据或由后端填静音。跨采样率时按当前 FIR 相位从队列取数据，后续拉取接着上次相位处理。调用方串行执行拉取、查询和生命周期操作。停机时丢弃未拉取的帧并复位重采样器，绑定保留到 `unbind()`。

## UsrPlayback

`UsrPlayback` 绑定 `Playback`，在独立工作线程按固定批次交付交错 PCM。正常批次向下取整为 `采样率 × io_period` 的整帧数。若应用用分数秒配置周期，在创建 `Playback` 时一次性向上取整为微秒，例如 `1/30 s` 转为 `33334 µs`，对应 44.1 kHz 每批 1470 帧、48 kHz 每批 1600 帧。回调由队列就绪驱动，与图 tick 独立。44.1 kHz 下图每 5 ms 交替写入 221、220 帧；端点周期为 5 ms 时回调每批取 220 帧，其余帧留在队列。

回调借用的 `IBBlock` 只在当次调用内有效。回调需返回且不得抛异常，启停和解绑由控制侧串行执行。`stop()` 关闭图侧写入，排空已入队的音频和 FIR 尾部后回收工作线程，排空期间可能交付短包。慢回调可能填满自己的队列并丢弃无法入队的新帧，也会延长停机。其他分支各自消费独立队列。

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
