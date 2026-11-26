# AEReborn：音频引擎框架草案

项目使用 C++14。Engine 由内置软件时钟线程推进。设备、文件和网络后端沿用本稿定义的运行边界。
命名空间统一为 `aereborn`。命名规则见 [编程约束](CODING_GUIDELINES.md#1-命名)。
图处理和虚拟 PCM 使用同一套渲染逻辑。API、运行约定和构建方法见第 7 节。

## 1. 参考与取舍

下面区分成熟引擎已有的设计，以及本项目据此作出的选择。参考设计思路，不引入这些引擎作为依赖。

| 官方参考 | 可借鉴的设计 | 本项目的选择 |
| --- | --- | --- |
| [JUCE AudioProcessorGraph](https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html) | 处理器组成节点图，通过通道连接；提供图重建机制 | 统一 Node/Port/Connection，将图描述编译为执行计划 |
| [JACK 实时处理回调](https://jackaudio.org/api/group__ClientCallbacks.html) | 处理回调禁止内存分配及可能阻塞的 I/O、锁和等待 | 音频执行路径只处理已准备的数据，外部工作通过有界队列交换 |
| [PipeWire 图运行](https://docs.pipewire.org/devel/page_running.html)、[调度](https://docs.pipewire.org/page_scheduling.html)、[时钟](https://docs.pipewire.org/page_man_pipewire-props_7.html) | 调度组选择 driver；配置与数据处理分离；区分设备时钟并适配 | 每个 Engine 由一条软件时钟线程驱动，设备通过边界适配 |
| [FMOD 混音与连接](https://www.fmod.com/docs/2.04/api/mixing-and-routing-in-the-core-api.html)、[DSPConnection](https://www.fmod.com/docs/2.04/api/core-api-dspconnection.html) | 总线分组、子混音、效果处理；连接持有增益和声道矩阵 | Connection 只读共享同规格端口的数据；Mixer 混音，Matrix 处理声道矩阵与增益，Convert 转换格式与采样率 |

第一版不做多进程共享图、节点并行调度、插件宿主或完整 DAW 自动化系统。先实现能运行、能恢复、能验证的单进程引擎。

## 2. 核心概念与节点实现

Engine 只处理 Node、Port、Connection 三个核心概念。它拥有节点和连接，校验图、准备缓冲并按依赖顺序执行节点；不需要识别节点的具体业务角色。

```text
Node A 的输出端口 ── Connection ──> Node B 的输入端口
```

| 对象 | 定位 |
| --- | --- |
| Node | 具有输入/输出端口的执行单元 |
| Port | 节点上的多声道音频入口或出口，固定声明采样率、声道数和格式 |
| Connection | 将上游输出零拷贝地借给同规格的下游输入，只读 |

使用 `engine.connect(source, mix[1])` 将源节点的第一个输出连到混音器的第二个输入。`connect(from, to)` 分别按输出和输入方向校验下标，无效端口抛出 `Error(RetCode::EPORT)`。`PortSpec` 固定声明采样率、声道数和 `SampleFormat`。连接两端的规格必须完全一致，规格冲突抛出 `Error(RetCode::INVFMT)`。需要转换时显式插入 Convert。输入端口已由其他来源占用时抛出 `Error(RetCode::INPUTBUSY)`，多路求和使用 Mixer 的不同输入。没有连接的输入得到该格式的零值静音。
一个 Port 表示一组声道，声道间的选择、复制和合并由 Matrix 表达。
`NodeId` 的高 48 位保存节点编号，低 16 位保存从零开始的端口下标。节点句柄等同于第 0 号端口。`0xFFFF` 标识越界端口，每个方向最多可声明 65535 个端口。删除、替换、暂停和节点查询接收下标为零的句柄，下标非零时抛出 `Error(RetCode::EPORT)`。`base()` 可从端口句柄取得节点句柄。替换节点实例时编号继续有效。`ConnId` 标识一次连接，重新连接相同端口会生成新的 ID。Node 不持有其他 Node 的共享所有权，也不各自维护连接列表或调度循环。

Source、Processor、Mixer、Sink 是具体节点的用途。Convert 负责格式与采样率转换。Mixer、Matrix 和 Tone 使用 planar float，算法节点可以声明自己需要的 PCM 格式。I/O 测试经 Capture、Playback 与实际后端交换音频，也可使用 Tone 生成可预测的信号。

| 用途 | 职责 | 当前实现 |
| --- | --- | --- |
| Source | 向图提供音频 | `Capture`、`Tone` |
| Processor | 处理输入并产生输出 | `Convert`、`Matrix`、用户算法节点 |
| Mixer | 显式合并多个相同规格的输入 | `Mixer` |
| Sink | 消费音频并交给设备 | `Playback` |

PCM 适配器和固定端口数量的自定义节点通常继承 `NodeT<I, O>`，由它保存端口描述。需要按周期准备资源的节点重写 `prepare(BlockPeriod)`，默认实现无需准备资源。节点在 `process()` 中从上下文读取周期，数据规格来自自身端口。端口数量由运行时决定的节点可以直接继承 `Node`，通过 `inputs()/outputs()` 提供固定的端口描述。引擎持有节点期间借用这些描述，节点负责保持描述及其存储有效。
后续设备、文件、网络、滤波等节点遵循同一边界。Bus 表示 Mixer 与效果节点构成的应用层总线，不是新的核心基类。

### 从输出创建输入：直接复用端口

任何输出端口都能继续连接到其他节点。Bus 可提供 PreEffects、PostEffects、PostFader 等命名输出位置，第一版先提供最终输出。
需要取得总线音频时，从相应输出连接普通 Sink；需要其他数据规格时连接 Convert，再由算法节点或 Playback 消费结果。图核心不提供独立的端口订阅机制。
编译器保证一个节点每个渲染块只执行一次，同一输出供多个下游只读使用；需要修改数据的下游使用自己的工作缓冲。

这支持“输出作为另一个输入”，并且不依赖物理设备是否在线。系统全局回环录音则作为独立 CaptureSource 能力接入。
第一版拒绝所有图内反馈环，校验同时覆盖音频与侧链依赖。

## 3. 引擎骨架

`core/` 提供 Engine、Node、音频缓冲、PCM 编解码、F32P 重采样器和内部控制指令队列 CommandQueue。`plugins/` 提供 Convert、Mixer、Matrix、Capture 和 Playback。两个端点负责经由 F32P 队列与绑定的 `Backend` 交接帧。`backend/` 的 `Backend` 统一管理设备、线程、转换工作区及漂移补偿。绑定 `Capture` 或 `Playback` 决定运行方向。依赖方向为 `backend → plugins → core`。`tests/support/` 提供缓冲夹具、测试后端和信号源 Tone。

Engine 持有编辑图和渲染计划。`commit()` 按依赖排序，从无输出的 Sink 遍历可达节点；未连接到 Sink 的分支不执行。每个可达节点每周期只执行一次。

引擎按各端口的最大周期帧数分配两块连续缓冲池，分别存放 F32P 的 float 对象和交错 PCM 的字节。连接输入只读借用上游输出；未连接的输入按存储类型共享缓冲池开头的只读零区间。所有缓冲和块视图由 Engine 管理。

`ProcessContext` 提供 `block_period`、`tick_position`、`inputs` 和 `outputs`。`IBBlock` 和 `OBBlock` 分别是只读和可写的 `ByteBlock`，携带缓冲地址、端口规格和本周期实际帧数。`byte_view()` 按原格式借用字节，适用于所有格式。`f32p_view()` 用于 F32P 端口，返回 `FloatBlock` 的只读或可写形式 `IFBlock` / `OFBlock`，声道步长为本周期的实际帧数。`FloatBlock` 也可借用插件工作区的分声道浮点样本，此时声道步长可以大于实际帧数。端口提供的视图只在 `process()` 调用内有效，节点必须写满自己的输出。

`ProcessContext::has_input(port)` 返回该输入在本周期是否需要作为活跃块处理。非活跃块保证以零字节填满，未连接输入和来自暂停节点的输入均为非活跃。源交付的全零样本和 DSP 尾音可以是活跃块。引擎在调用节点之前初始化输出标志。无输入端口的源节点默认活跃，其他节点的每个输出默认取所有输入标志的逻辑或。节点在所有输入均非活跃时若仍产生音频，包括有状态 DSP 的尾音，必须调用 `set_output_active(port, true)` 标记相应输出。能保证某个输出已填满零字节的节点，可调用 `set_output_active(port, false)` 让下游跳过样本计算。标志视图只在本次调用内有效。直接用四个字段构造 `ProcessContext` 时，`has_input()` 返回 true，设置输出标志无效果。

Capture 读到至少一帧即标为活跃，短读补零。队列读空表示当前周期缺数据，网络欠载和正常结束都可能出现这一情况。Convert 在 FIR 历史冲刷完毕后才将空输入标为非活跃。Matrix 在淡出之后的稳态零增益块上标记非活跃。Mixer 只累加活跃且未静音的输入。Playback 继续逐拍入队，包括非活跃的静音块，以保持录音和设备输出的时间线。

渲染计划用连续数组保存端口视图、活动标志和各输入对应的上游输出下标。引擎按拓扑顺序读取上游输出标志，每个节点只访问自身端口范围。包含分数帧端口的计划在每个 tick 更新实际帧数，全整数帧计划使用预先绑定的帧数。缓冲和节点引用由计划统一持有。

控制侧 `commit()` 完成分配和节点准备。运行时，时钟线程在块边界从原子槽接管计划，控制侧等待接管后同步释放旧计划及仅由旧计划持有的节点。沿用的节点实例保留 DSP 状态。

## 4. 执行与时钟

每个运行中的 Engine 由一条内置软件时钟线程串行执行渲染。Capture 和 Playback 在图侧按每个 tick 的实际帧数交接，后端线程通过 SPSC 帧队列交接可变长度的包。队列容量由最大图块和设备单包预算决定。异步硬件以半满为预填充和漂移目标。同步文件与 Null 后端立即上线，由队列背压决定推进速度。

`Engine(BlockPeriod)` 在构造时验证推进周期。可选值为 `BlockPeriod::INR_02MS`、`INR_05MS`、`INR_10MS`、`INR_20MS` 和 `INR_40MS`。调用方可用 `block_period()` 查询周期，节点可从 `ProcessContext` 读取周期。`tick_position()` 返回已完成的周期数，停止后重启继续计数，`reset()` 归零。每个 `process()` 收到本周期开始时的 `tick_position`。

端口以 `BandWidth` 表示采样率，单位为 Hz。当前支持 8000、11025、16000、22050、24000、32000、44100、48000、88200 和 96000，例如 `BandWidth::CDQuality` 为 44100 Hz，`BandWidth::Full` 为 48000 Hz。`SampleFormat::F32P` 是原生平面 float32。`S16LE`、`S24_3LE`、`S24LE`、`S32LE` 和 `F32LE` 是交错小端 PCM。连接匹配包含采样率、声道数、样本格式及其布局。

每个端口按累计时间的向上取整差值计算本周期实际帧数。2 ms 下，48 kHz 每块固定 96 帧，44.1 kHz 按 89、88、88、88、88 帧循环。44.1 kHz / 5 ms 为 221、220 帧交替。`PortSpec::max_frames()` 用于容量，`frames()` 和 `frame_position()` 用于实际块与该端口的样本时间。不同采样率端口仍由同一个基础周期驱动。Convert 将分数相位对齐到共享 tick，运行中新增或替换节点不会另起采样时钟。

内置时钟用 steady_clock 和 condition_variable::wait_until 调度，包括空图在内的每个 tick 都执行一个完整周期。延误时跳过过期的唤醒期限，逻辑时间按实际执行的 tick 前进。

独立的设备时钟由后端流式 SincResampler 补偿。消费者完成预填充后，设备侧 PI 根据队列水位在 ±2000 ppm 内修正采样步长，同标称采样率也使用这条路径。同步后端按固定比例转换，同采样率直接编解码。

### 控制与实时处理的边界

调用方负责串行执行 Engine 和后端的控制操作。仓库内的组件可通过内部头文件 `core/command_queue.h` 使用 CommandQueue，在同一工作线程执行 `sync()`、`post()` 和基于单调时钟的定时任务。每批即时任务后检查一次最早到期的定时器。周期任务从当次执行结束起重新计时。

`stop(on_stop)` 关闭提交和定时器取出，等待在途任务结束，排空已接收的即时任务，再由工作线程释放剩余定时回调并执行清理。定时回调在持锁取出时进入在途状态。取消和停机允许已取出的当次执行完毕。

后端工作线程可异步提交控制命令。Engine 时钟和后端工作线程均应避免同步等待控制线程，因为控制操作可能正在等待这些线程退出当前处理。`post()` 可能竞争锁或分配内存，实时 `process()` 应使用预分配的事件交接。

设备打开/关闭、文件访问、编解码和应用回调都在非渲染路径完成。设备必要的实时缓冲交付由平台后端封装。
音频路径只读写已准备的缓冲，避免堆操作、锁等待、日志和应用回调。

```text
应用串行控制执行器 ──> 计划/参数 ──> 渲染路径
从设备 / 网络 / 文件 ──> 有界队列 ──> 音频入口节点
音频出口节点 ──> 有界队列 ──> 从设备 / 网络 / 文件
音频出口节点 ──> 有界队列 ──> 用户回调执行器
```

队列预先明确单/多生产者归属、容量和满队列行为。活跃数据流不允许无限堆积；慢回调只影响自身输出分支，录音写入跟不上时由后端报告数据缺口。
采集和播放端点使用 `core/fring.h` 的有界 F32P SPSC 帧环，推入和回调后端可复用该组件。
Engine 停止会等待图处理退出。后端停止时唤醒阻塞的异步 I/O，或先完成同步播放排空，再下线并清空队列。同步采集正常读到 EOF 后排出 FIR 尾部，并等待图消费队列。图暂停或停止期间，调用 `stop()` 或 `unbind()` 可取消这段等待。

## 5. 外部资源与恢复

逻辑端点可跨设备切换复用。后端独占借用已经准备的端点，并持有设备句柄。设备枚举、选择策略与自动恢复可由后续的 DeviceService 负责。
选择可以是固定设备、跟随系统默认、优先某类设备；不在收到任意插入事件后立即抢占当前设备。
设备身份使用平台稳定标识及匹配规则，不依赖枚举序号永久不变。

```text
逻辑意图：Running / Paused / Stopped
资源状态：WaitingDevice -> Opening -> Ready -> Recovering / Failed
```

两组状态分别维护：恢复完成仍要服从用户当前意图。
掉线期间保留图、音量和连接；缺失采集数据补静音并由后端记录缺口，输出设备不可用时丢弃该设备数据，总线及其他下游继续处理。
可就地恢复的欠载由后端处理；需重开、等待设备、不可恢复的配置错误由统一恢复策略区分。
重试带退避和取消，主动定期重开也是同一策略的选项。会话代际编号屏蔽过期打开结果与旧设备数据。

一个逻辑端点最多由一个外部参与方读写队列。播放侧在最终设备端点之前显式混音。图侧端点保持既定格式，后端在打开设备时独立协商实际格式。
设备协商格式与逻辑音频格式分离，恢复不改变应用看到的端口布局；适配不了时报告端点失败。

网络与文件复用相同入口/出口节点机制：

- NetworkSource：会话识别、乱序整理、按播放期限解码/FEC/PLC，再交付音频。NetworkSink：排队、编码、发送。先 PCM，后 Opus；协议独立版本化。
- WavCapture 在后端线程读文件，可循环播放。WavPlayback 接收同步播放队列，解绑时回填 RIFF/WAVE 文件长度。
- `PushSource`：应用推入固定规格的交错 PCM，接口返回已接收的输入帧数。帧时间和连续性信息由上层流处理。
- `PullSink`：应用从 `Playback` 拉取固定规格的交错 PCM，返回实际输出帧数。
- `UsrPlayback`：同步播放后端在工作线程按固定批次回调交错 PCM，停止时排空队列和 FIR 尾部。回调中的块只在当次调用内有效。

## 6. 混音策略归属

Mixer 显式求和，Matrix 负责声道映射与增益。两者都只支持 F32P，采样率由构造参数固定，并不要求整张图使用同一采样率。Mixer 每个输入与输出的规格相同；Matrix 的输入、输出声道数可不同，采样率保持一致。

Matrix 的行是目标声道，列是源声道，系数可用于选取、复制、合并和相位反转。全局增益与矩阵在同一遍采样处理中完成，增益在下一个有帧的处理块内线性过渡到目标值。

Mixer 支持 1 到 64 路输入，每路默认未静音、为普通优先级。任一未静音的高优先级输入活跃时，普通输入在一个块内淡出至零。全部高优先级输入失去活跃状态或被静音后，普通输入在一个块内恢复至原音量。多路活跃的高优先级输入直接求和。刻意交付的全零音频仍会触发压制。普通输入即使暂时无数据也会跟随当前压制状态，在压制期间恢复供帧时从静音开始。静音、稳态压制和斜坡精确到达零增益时跳过相应源样本。

总线可以由 Mixer、Matrix 和效果节点组成。输入优先级由应用设置，Mixer 根据输入活动标志进行裁决。

## 7. 时钟运行 API

应用创建 Engine，加入节点、连接端口、调用 `commit()`，再用 `start()` 启动时钟。示例演示 Tone → Mixer → Matrix → 电平 Sink，以及运行中修改 Matrix 增益。[时钟运行示例](examples/clocked_graph.cpp)

### 固定能力与连接检查

插件通过不可变的 inputs()/outputs() 声明实际端口规格。构造端口规格时显式传入 `PortSpec(bandwidth, channels, format)` 的三个参数。Mixer、Matrix、Tone 显式声明 F32P 端口。特殊算法声明其支持的固定 PCM 规格，Engine 在 `connect()` 时检查两端，节点在准备阶段验证资源条件。本版不增加动态能力列表或格式协商协议。

每个输入最多一个来源；同一输出可供多个下游只读使用。需要多路混音必须显式创建 Mixer 的多个输入。连接不包含增益、静音或矩阵，也不增加转换器。

下面的链路可以直接用 connect() 表达：

~~~text
Tone / BUS（48k F32P）
    → Convert（48k F32P → 16k S16LE）
    → 算法（固定 16k S16LE）
    → Convert（16k S16LE → 48k F32P）
    → 其他节点 / Playback
~~~

### 插件接口

| 插件 | 端口 | 配置与职责 |
| --- | --- | --- |
| Convert(input_spec, output_spec) | (1,1) | 支持上述全部格式的转换和图内固定比例重采样，两端声道数须相同 |
| Mixer(rate, channels, inputs) | (N,1)，1 ≤ N ≤ 64 | 相同 F32P 规格求和。用 `set_muted(index, bool)` 控制静音，用 `set_high_priority(index, bool)` 设置优先级 |
| Matrix(rate, input_channels, output_channels, coefficients, gain) | (1,1) | F32P 声道映射与增益；行对应输出、列对应输入，系数固定；set_gain() 更新增益 |
| Matrix(rate, channels, gain) | (1,1) | 使用恒等声道映射，仅控制增益 |
| Capture(rate, channels, io_period) | (0,1) | 接收外部写入的 F32P 帧，按 tick 向图交付并补足静音 |
| Playback(rate, channels, io_period) | (1,0) | 逐 tick 将图帧写入 F32P 队列，供外部消费者读取 |
| Tone(rate, channels, frequency, amplitude) | (0,1) | 按端口样本时间生成 F32P 正弦波 |

控制接口通过 `aereborn::Error` 报告失败，调用方可用 `code()` 获取 `RetCode::Code`。内置插件在构造时验证固定端口与参数，规格错误使用 `RetCode::EPARAM`，容量超限使用 `RetCode::TOOLARGE`。节点用 `prepare(BlockPeriod)` 准备工作区和处理状态。标准库的分配和线程创建异常，以及自定义节点异常，直接传播给调用方。

图内 Convert 与后端共用 core 的 PCM 编解码和 SincResampler 实现，各自持有处理状态。固定同采样率下，相同规格按字节直通，F32P 与交错 PCM 互转直接读写端口缓冲，两条路径的准备都无需堆分配。同采样率的不同交错 PCM 格式互转共用一块解码缓冲。重采样准备滤波器和历史样本，仅在输入为交错 PCM 时分配解码缓冲，仅在输出为交错 PCM 时分配重采样输出缓冲。

需要 PCM 声道映射时，先 Convert 到 F32P，经过 Matrix，再按需要 Convert 回 PCM。后端在图外完成交错 PCM 编解码和流式重采样，协商出的声道数须与图端口一致。

### 运行控制与缓冲交接

- 图编辑修改草稿。`commit()` 准备并提交新计划。停机时立即生效，运行时等待时钟在块边界接管。旧计划由调用线程释放，此时首个新块的处理可能仍在进行。草稿无变动时直接返回，准备失败时保留当前计划。
- `replace_node()` 要求输入、输出的数量及完整规格相同，布局不同时抛出 `Error(RetCode::INVFMT)`。设备格式变化时重新准备设备侧转换。新包长超出原周期预算时，用同端口规格的新节点替换并提交，保留 NodeId 和图连接。逻辑端口规格变化时修改相应连线。
- `set_paused()` 通过计划移除或恢复节点及相关连接，未改动实例保留状态。暂停导致某个输入失去来源时，该输入得到静音。需要平滑暂停时，先将 Matrix 增益淡出至零，再提交暂停。
- `Matrix::set_gain()`、`Mixer::set_muted()` 和 `Mixer::set_high_priority()` 可与 `process()` 并发调用。每个有帧的块各读取一次 Mixer 的静音和优先级掩码。可用 `muted()` 和 `high_priority()` 查询设置，用 `suppressed()` 查询最近一个有帧的块中哪些活跃且未静音的输入处于淡出或稳态压制。Mixer 的 `reset()` 清除处理状态和压制报告，保留静音和优先级设置。控制侧从 `Engine::node()` 借用草稿节点指针，须在节点被移除或替换前结束使用，最长有效期到 Engine 析构。
- 图提交并准备端点后，`ActiveBackend::bind(endpoint)` 协商 PCM 规格和单包预算 `io_period()`，`start()` 在工作线程上初始化。`ActiveBackend` 和 `PassiveBackend` 共用 `Backend` 的端点占用与 PCM 转换，并可通过 `Backend&` 统一启停与查询。端点已经被占用时抛出 `Error(RetCode::BUSY)`。绑定失败释放资源和占用。设备启动失败时先回收工作线程，再向控制调用方抛出设备错误。
- `PushSource` 绑定 `Capture`，由应用串行执行 `push(IBBlock)`、控制接口、状态查询和析构。可用单消费者指令队列执行这些操作。PCM 缓冲至少保留到 `push()` 返回。同一流的分片按顺序投递，停止接收旧流数据后再执行停机或重启。`push()` 返回本次接收的输入帧数，后续从该偏移续推。队列空位不足时可能返回零，图侧缺帧时补静音。`stop()` 丢弃未消费的帧并复位重采样器。
- `PullSink` 同样由应用串行调用，`pull(OBBlock)` 读取 `Playback` 中当前可用的 PCM。`UsrPlayback` 由同步播放工作线程消费 `Playback` 队列。分数秒预算在构造 `Playback` 时一次向上取整为微秒，回调使用固定帧数，交付由数据就绪驱动。
- `stop()` 下线并复位 DSP 和漂移，保留绑定和已分配工作区。同步播放先关闭图侧写入，写完已排队帧及 FIR 尾部。同步 I/O 钩子须自行完成操作。切换设备时依次调用旧后端的 `unbind()` 和新后端的 `bind(endpoint)`、`start()`。
- 异步播放补齐设备批次的静音，后端通过 `last_error()` 返回 I/O 终止原因。异步等待和数据交接共用以最后一次 I/O 进展为起点的截止时间。主动停止可唤醒设备等待。测试通过真实的 I/O 工作线程验证设备交接。
- `Engine::stop()` 唤醒时钟并等待在途图处理退出。先停止绑定的后端，再调用 `Engine::reset()`。在移除、替换端点或销毁 Engine 前解绑其后端。派生驱动析构时先调用 `unbind()`。
- 调用方在控制侧串行执行控制接口和析构。同一 Engine 的时钟线程调用控制接口时抛出 `Error(RetCode::BUSY)`。节点需要请求修改引擎时，通过预分配的事件交接通知控制侧，并直接返回。实例有效期间可并发读取 `block_period()` 与 `tick_position()`。
- 处理路径尽量避免锁等待，禁止堆分配、释放和调用普通应用回调。异步使用块数据必须在 process() 返回前复制到后端自有缓冲。

### 运行态诊断

仓库内的目标链接 `aereborn::aereborn`，并通过私有头路径包含 `diag/inspect.h`，即可在串行控制侧调用 `dump_text()` 或 `dump_dot()`。两者都支持返回字符串或写入输出流。传入需要一起检查的后端，报告会按端点关联生效节点，并列出未进入生效 Plan 的后端。队列水位为近似值，`underrun` 和 `overflow` 分别累计图侧采集缺帧和播放丢帧。`late` 是时钟跳过的周期数，`tick_us` 是最近一拍含时钟收尾的估计耗时。

用 `dot -Tsvg graph.dot -o graph.svg` 渲染信号流图。卡片标题显示端点状态和后端错误短码，橙色标记预填充、排空、跳拍或队列缺帧。多端口节点按插槽接线，Mixer 的静音端口和入边置灰。启用优先级时，文本报告显示 `high` 和 `suppressed`，DOT 用 `[HIGH]` 标记高优端口，用 `[SUPP]` 和琥珀色虚线标记被压制的普通端口及其入边。卡片和连线直接显示关键规格，SVG 悬停可查看完整诊断详情。`io.diagnostic_reports` 和 `io.mixer_priority_diagnostics` 检查文本和 DOT 输出，`io.endpoint_diagnostics` 检查端点计数、重启及短批次排空。

### AudioCenter

应用通过 `include/audio_interface.h` 使用 `AudioCenter` 创建 WAV 输入、WAV 输出、Mixer 和 Matrix。门面将操作同步派发至控制线程，由 `app/graph_service.cpp` 管理图和后端。调用会阻塞。所有调用线程须在实例析构前结束对实例的访问。控制方法返回 `RetCode`，创建失败返回空句柄，诊断失败返回空字符串。`AudioCenter::last_error()` 返回调用线程最近一次控制或诊断操作的状态。

- `connect(from, to)` 可直接连接同规格端口，规格变化时自动插入 Matrix 或 Convert。降声道先映射，升声道先重采样。单声道复制到各输出，合并为单声道时取平均，其他情况按相同下标映射并将额外输出置零。
- 首次 `start()` 和草稿有变化时会自动提交。运行中 `commit()` 的图准备失败会保留当前计划。发布新计划后，门面先按旧计划的逆执行顺序停止离场后端，再按新计划的执行顺序启动入场后端。离场的录音后端可以在计划切换后排空队列。留在计划里的后端延续原有状态，包括自然结束和 I/O 错误。若有后端启动失败，门面停止后端和时钟并保留新计划，修复后可重新 `start()`。移除 WAV 端点会立即解绑，旧计划里仍存在的采集端点输出静音，播放端点停止接收数据。
- 未暂停的 WAV 输出在断开上游后继续录制静音，以保留时间轴。暂停输出节点并提交会停止该后端。单文件输出在创建节点时独占打开，目标已存在时返回 `BUSY`。编号分段跳过已有文件。`stop()` 排空录音队列并保留打开的文件，移除输出或销毁实例会回填最终的 WAV 文件长度。

### 构建与验证

项目使用 C++14。CMake 目标 `aereborn::aereborn` 链接静态库 `aereborn`，其中包含内核、插件、后端、诊断模块和应用门面（`app/`）。目标公开 `include/` 头路径。应用通过 `#include "audio_interface.h"` 使用 `AudioCenter`，也可通过 `#include "audio_types.h"` 单独使用基础类型。库链接 `Threads::Threads`。Linux 在查找到 ALSA 开发库时编译 ALSA 后端并定义 `AEREBORN_HAS_ALSA`，Windows 编译 WASAPI 后端。只构建库可设置 `-DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF`。

~~~sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --config Debug --parallel 4
ctest --test-dir build/debug -C Debug --output-on-failure
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --config Release --parallel 4
ctest --test-dir build/release -C Release --output-on-failure
ctest --test-dir build/release -C Release -L bus --output-on-failure
~~~

测试目录和 CTest 标签使用 `core`、`dsp`、`bus`、`io`、`public`、`regression`。用 `ctest --test-dir build/release -C Release -L public --output-on-failure` 验证公共门面。单独运行四声道场景可使用 `ctest --test-dir build/release -C Release -R '^bus.four_channel_song_archive$' --output-on-failure`。引擎测试使用内置时钟，通过测试节点在块边界与测试控制侧同步。混音、PCM 转换和重采样使用独立字节或数值参照。

`tests/data/` 的两份 48 kHz 双声道 WAV 短片来自《渡口》第 30 秒起的 0.5 秒和 CaliforniaHotel 第 40 秒起的 0.75 秒。四声道测试将两段音乐交错到同一个输入，生成两份 48 kHz 拆分归档和一份 44.1 kHz 混音归档。录音和分段文件在构建目录内创建，正常退出时清理。

`io` 包含后端绑定、设备超时、WAV 文件和时钟漂移检查。Linux 有 ALSA 时运行两种 `plug:null` 采集和播放配置。Windows 默认验证 WASAPI 绑定失败和 Null 接管。设置 `AEREBORN_WASAPI_CAPTURE_ID` 或 `AEREBORN_WASAPI_PLAYBACK_ID` 后，可用 `ctest --test-dir build/release -C Release -L hardware --output-on-failure` 运行对应实机用例。ALSA 后端的设计背景见 [ALSA 实施记录](docs/archive/aereborn_alsa.md)。

MSVC Debug 的迭代器代理会在 `noexcept` 构造中分配内存，`regression` 中的分配注入和零分配检查在 Release 执行。示例由默认开启的 `BUILD_EXAMPLES` 控制。

## 8. 实现顺序

1. **离线图核心**：Node、Port、Connection、音频块及 Engine 的准备与执行。用插件节点验证多对多、处理器串联、总线互连、输出分发、环路拒绝。
2. **运行内核**包括内置软件时钟、块边界计划切换、同步提交、插件参数、显式 Mixer/Matrix 和停止屏障。验证调音量时复用图计划，修改连接时保留未变节点的状态。
3. **设备边界与平台闭环。** 用虚拟设备验证 Capture、Playback、重启和漂移补偿。平台驱动已接入，后续实机验收检查短读写、拔插和跨设备恢复。
4. **文件、推入和混音策略**：预读/录制、PlaybackHandle、PushSource、DSP 链、ducking。验证多实例、循环、过载报告和增益包络。
5. **网络**：先 PCM，再 Opus、乱序/抖动缓冲与丢包恢复；验证本地与远程源进入同一 Bus，断网后有界退化并恢复。

各阶段复用同一张图与同一套节点执行机制。

第一步的验收包括 `core.graph_constraints`、`core.commit_failure_and_retry`、`bus.subgroup_and_monitor` 和 `bus.parallel_pcm_algorithm`。路由走实际的 Convert、Mixer 和 Matrix，算法由声明固定 PCM 格式的测试节点提供。

第二步的验收覆盖 `core.clock_boundary_barriers`、`bus.live_input_switch` 和 `regression.render_and_passive_io_without_heap`。可运行 `clocked_graph` 示例观察运行期间的音量更新。
