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
Node A 的 OPort ── Connection ──> Node B 的 IPort
```

| 对象 | 定位 |
| --- | --- |
| Node | 具有输入/输出端口的执行单元 |
| Port | 节点上的多声道音频入口或出口，固定声明采样率、声道数和格式 |
| Connection | 将上游输出零拷贝地借给同规格的下游输入，只读 |

`IPort` 和 `OPort` 用节点 ID 和从零开始的下标定位端口。`PortSpec` 固定声明采样率、声道数和 `SampleFormat`。连接两端必须完全一致，否则返回 `FormatMismatch`；需要转换时显式插入 Convert。一个输入端口只能接一个来源，重复占用返回 `InputConnected`，多路求和使用 Mixer 的不同输入。没有连接的输入得到该格式的零值静音。
一个 Port 表示一组声道，声道间的选择、复制和合并由 Matrix 表达。
`NodeId` 标识图中的节点，替换节点实例时 ID 继续有效。`ConnId` 标识一次连接，重新连接相同端口会生成新的 ID。原型采用进程内不复用的唯一 ID。Node 不持有其他 Node 的共享所有权，也不各自维护连接列表或调度循环。

Source、Processor、Mixer、Sink 是具体节点的用途。`plugins/` 中的 Capture、Playback 是设备缓冲交接节点，Convert 是格式与采样率转换节点。Mixer、Matrix 和 Tone 是只支持 planar float 的普通插件；算法节点可以声明自己需要的 PCM 格式。

| 用途 | 职责 | 当前实现 |
| --- | --- | --- |
| Source | 向图提供音频 | `Capture`、`Tone` |
| Processor | 处理输入并产生输出 | `Convert`、`Matrix`、用户算法节点 |
| Mixer | 显式合并多个相同规格的输入 | `Mixer` |
| Sink | 消费音频并交给设备 | `Playback` |

PCM 适配器和固定端口数量的自定义节点通常继承 `NodeT<I, O>`，由它保存端口描述。具体节点实现 `prepare(BlockPeriod)`，按周期准备资源，无需准备资源时直接返回成功。节点在 `process()` 中从上下文读取周期，数据规格来自自身端口。端口数量由运行时决定的节点可以直接继承 `Node`，通过 `inputs()/outputs()` 提供固定的端口描述。引擎持有节点期间借用这些描述，节点负责保持描述及其存储有效。
后续设备、文件、网络、滤波等节点遵循同一边界。Bus 表示 Mixer 与效果节点构成的应用层总线，不是新的核心基类。

### 从输出创建输入：直接复用端口

任何 `OPort` 都能继续连接到其他节点。Bus 可提供 PreEffects、PostEffects、PostFader 等命名输出位置，第一版先提供最终输出。
需要取得总线音频时，从相应输出连接普通 Sink；需要其他数据规格时连接 Convert，再由算法节点或 Playback 消费结果。图核心不提供独立的端口订阅机制。
编译器保证一个节点每个渲染块只执行一次，同一输出供多个下游只读使用；需要修改数据的下游使用自己的工作缓冲。

这支持“输出作为另一个输入”，并且不依赖物理设备是否在线。系统全局回环录音则作为独立 CaptureSource 能力接入。
第一版拒绝所有图内反馈环，校验同时覆盖音频与侧链依赖。

## 3. 引擎骨架

core 提供 Node、端口描述、只读/可写块视图和 Engine。plugins 提供按功能命名的 Convert、Mixer、Matrix、Capture、Playback、Tone。

Engine 持有编辑图和渲染计划。`commit()` 按依赖排序，从无输出的 Sink 遍历可达节点；未连接到 Sink 的分支不执行。每个可达节点每周期只执行一次。

引擎按各端口的最大周期帧数分配两块连续缓冲池，分别存放 F32P 的 float 对象和交错 PCM 的字节。连接输入只读借用上游输出；未连接的输入按存储类型共享缓冲池开头的只读零区间。所有缓冲和块视图由 Engine 管理。

`ProcessContext` 提供 `block_period`、`tick_position`、`inputs` 和 `outputs`。`IPortBlock` 只读，`OPortBlock` 可写，两者都携带缓冲地址、端口规格和本周期实际帧数。`byte_view()` 按原格式借用字节，适用于所有格式。`f32p_view()` 用于 F32P 端口，返回 `IFBlock` 或 `OFBlock`，声道步长为本周期的实际帧数。`FloatBlock` 也可借用插件工作区的分声道浮点样本，此时声道步长可以大于实际帧数。端口提供的视图只在 `process()` 调用内有效，节点必须写满自己的输出。

渲染计划用两个连续数组保存已绑定的输入、输出视图，每个节点按端口范围访问。包含分数帧端口的计划在每个 tick 更新实际帧数，全整数帧计划使用预先绑定的帧数。缓冲和节点引用由计划统一持有。

控制侧 `commit()` 完成分配和节点准备。运行时，时钟线程在块边界从原子槽接管计划，控制侧等待接管后同步释放旧计划及仅由旧计划持有的节点。沿用的节点实例保留 DSP 状态。

## 4. 执行与时钟

每个运行中的 Engine 由一条内置软件时钟线程串行执行渲染。设备通过边界适配器交换数据。当前 Capture / Playback 每次交接一个基础周期的数据，后端负责将实际硬件收发整理为这个粒度。跨周期拼拆包留待后续实现。

Engine(BlockPeriod) 固定推进周期。调用方可用 block_period() 查询周期，节点可从 ProcessContext 读取周期。tick_position() 返回已完成的周期数，停止后重启继续计数，reset() 归零。每个 process() 收到本周期开始时的 tick_position。

端口只支持 `SampleRate` 枚举中的采样率，单位为 Hz。当前为 8000、11025、16000、22050、24000、32000、44100、48000、88200 和 96000。SampleFormat::F32P 是原生 planar float32；S16LE、S24_3LE、S24LE、S32LE、F32LE 是交错小端 PCM。连接匹配包含采样率、声道数、样本格式及其布局，不做自动协商或隐式转换。

每个端口按累计时间的向上取整差值计算本周期实际帧数。44.1 kHz / 5 ms 为 221、220 帧交替，11.025 kHz 等规格也不逐周期独立取整；PortSpec::max_frames() 用于容量，frames() 和 frame_position() 用于实际块与该端口的样本时间。不同采样率端口仍由同一个基础周期驱动。Convert 将分数相位对齐到共享 tick，运行中新增或替换节点不会另起采样时钟。

内置时钟用 steady_clock 和 condition_variable::wait_until 调度，包括空图在内的每个 tick 都执行一个完整周期。延误时跳过过期的唤醒期限，逻辑时间按实际执行的 tick 前进。

从设备与引擎时钟不一致时，在边界做缓冲与自适应重采样。即使标称采样率相同，也不能默认两个设备共享时钟。

### 控制与实时处理的边界

应用负责串行执行 Engine 的控制操作。应用可以用 Asio strand 承载控制命令，也可以独立使用 Asio 协调网络 I/O、设备事件和恢复定时。
设备打开/关闭、文件访问、编解码和应用回调都在非渲染路径完成。设备必要的实时缓冲交付由平台后端封装。
音频路径只读写已准备的缓冲，避免堆操作、锁等待、日志和应用回调。

```text
应用串行控制执行器 ──> 计划/参数 ──> 渲染路径
从设备 / 网络 / 文件 ──> 有界队列 ──> 音频入口节点
音频出口节点 ──> 有界队列 ──> 从设备 / 网络 / 文件
音频出口节点 ──> 有界队列 ──> 用户回调执行器
```

队列预先明确单/多生产者归属、容量和满队列行为。活跃数据流不允许无限堆积；慢回调只影响自身输出分支，录音写入跟不上时由后端报告数据缺口。
第 3 阶段在插件层实现可复用的有界无锁 SPSC 队列和字节流环形缓冲，供设备、推入和回调后端共用。
Engine 停止会等待图处理退出。后端负责等待自己的 I/O 与应用回调退出；不能仅取消定时器就释放仍在使用的对象。

## 5. 外部资源与恢复

逻辑端点长期存在，物理句柄归 DeviceSession。DeviceService 分开维护设备清单、选择策略与运行会话。
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

同一设备的兼容逻辑端点共享 DeviceSession。播放侧明确汇入该设备的最终混音节点，再交给唯一设备写入者；独占或格式冲突明确报错。
设备协商格式与逻辑音频格式分离，恢复不改变应用看到的端口布局；适配不了时报告端点失败。

网络与文件复用相同入口/出口节点机制：

- NetworkSource：会话识别、乱序整理、按播放期限解码/FEC/PLC，再交付音频。NetworkSink：排队、编码、发送。先 PCM，后 Opus；协议独立版本化。
- FileSource：工作线程预读，独立 PlaybackHandle 管理循环、暂停和停止。FileSink：排队写入，支持分段保存。
- PushSource：应用显式推入带格式、帧时间和连续性信息的音频，接口报告接收数量或队列满。
- 回调输出：普通 Sink 接收图内 float，或由 Convert 转换为所需 PCM。后端通过有界队列异步调用用户函数，负责数据所有权、缺口报告和停止屏障。

## 6. 混音策略归属

Mixer 显式求和，Matrix 负责声道映射与增益。两者都只支持 F32P，采样率由构造参数固定，并不要求整张图使用同一采样率。Mixer 每个输入与输出的规格相同；Matrix 的输入、输出声道数可不同，采样率保持一致。

Matrix 的行是目标声道，列是源声道，系数可用于选取、复制、合并和相位反转。全局增益与矩阵在同一遍采样处理中完成。Mixer 对每个输入独立静音。增益和静音在下一个有帧的处理块读取，并在该块内线性过渡到目标值，零增益或零系数跳过源样本，避免未使用的 NaN / Inf 污染输出。

总线可以由 Mixer、Matrix 和效果节点组成，不增加核心基类。Voice、Music、Alert 等抑制策略由后续插件产生增益目标，Engine 不识别角色或策略。

## 7. 时钟运行 API

应用创建 Engine，加入节点、连接端口、调用 `commit()`，再用 `start()` 启动时钟。示例演示 Tone → Mixer → Matrix → 电平 Sink，以及运行中修改 Matrix 增益。[时钟运行示例](examples/clocked_graph.cpp)

### 固定能力与连接检查

插件通过不可变的 inputs()/outputs() 声明实际端口规格。Mixer、Matrix、Tone 构造函数只生成 F32P 端口；普通插件直接编写 float 算法。特殊算法声明其支持的固定 PCM 规格，Engine 在 connect() 时检查两端，准备阶段验证时序和资源。本版不增加动态能力列表或格式协商协议。

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
| Convert(input_spec, output_spec) | (1,1) | 支持上述全部格式的转换和固定比例重采样，要求两端声道数相同 |
| Mixer(rate, channels, inputs) | (N,1) | 相同 F32P 规格显式求和；set_muted(index, bool) 控制单个输入 |
| Matrix(rate, input_channels, output_channels, coefficients, gain) | (1,1) | F32P 声道映射与增益；行对应输出、列对应输入，系数固定；set_gain() 更新增益 |
| Matrix(rate, channels, gain) | (1,1) | 使用恒等声道映射，仅控制增益 |
| Capture(spec) | (0,1) | 将后端绑定的一周期字节复制进 Engine 输出缓冲；缺少数据时输出静音 |
| Playback(spec) | (1,0) | 将输入复制进后端绑定的一周期缓冲；无缓冲时丢弃数据 |
| Tone(rate, channels, frequency, amplitude) | (0,1) | 按端口样本时间生成 F32P 正弦波 |

内置插件在构造时校验固定端口和参数，规格错误抛出 `std::invalid_argument`，静态配置超出容量抛出 `std::length_error`。`Engine::commit()` 先校验周期，再开始构建计划。节点的 `prepare(BlockPeriod)` 接收已校验的周期，检查自身的平台和资源条件，并准备运行状态及工作区。应用也可通过 `Engine::add_node()` 加入自定义节点，引擎继续校验端口声明。

相同规格的 Convert 按字节直通，无需转换状态或工作区；同采样率 PCM → F32P 直接写入 Engine 输出，不经中间 float 缓冲。其余路径只准备实际转换需要的工作区。

需要 PCM 声道映射时，先 Convert 到 F32P，经过 Matrix，再按需要 Convert 回 PCM。Convert 不负责声道矩阵，Capture / Playback 不负责转换。

### 运行控制与缓冲交接

- 图编辑修改草稿。`commit()` 返回 `RetCode`。停机时新计划立即生效。运行时，成功返回表示时钟已在块边界接管新计划，旧计划已由调用线程释放。首个新块的处理可能仍在进行。草稿未改动时直接返回成功，准备失败保留当前计划。
- replace_node() 要求输入、输出的数量及完整规格相同，否则返回 FormatMismatch。改变设备格式时，显式替换相应设备节点与 Convert 并重建连接，可用一次 `commit()` 共同提交；稳定的下游 float 节点继续运行。
- set_paused() 通过计划移除或恢复节点及相关连接，未改动实例保留状态。暂停导致某个输入失去来源时，该输入得到静音。需要平滑暂停时，先淡出 Matrix 增益或静音 Mixer 输入，再提交暂停。
- `Matrix::set_gain()` 和 `Mixer::set_muted()` 使用原子目标，可与 `process()` 并发调用。处理块读取最新目标。控制侧从 `Engine::node()` 借用草稿节点指针，须在节点被移除或替换前结束使用，最长有效期到 Engine 析构。
- Capture / Playback 在节点准备完成后接受 bind(bytes, tick_position)，字节数必须恰好等于该 tick 的端口帧数乘声道数和样本宽度。已有绑定时返回 BufferFull；绑定和 reset 与处理串行，借用内存在指定 tick 结束前保持有效。未来 tick 的绑定保留到该 tick，过期数据不再输出。重新准备不同周期时释放已有绑定。
- 控制侧可在启动之前绑定单周期缓冲。运行时的跨线程供给由后端队列负责。
- `stop()` 唤醒时钟并等待在途处理退出。`reset()` 在停机时归零 tick 并重置节点。
- 调用方在控制侧串行执行控制接口和析构。同一 Engine 的时钟线程同步调用控制接口时返回 `Busy`。节点需要请求修改引擎时，将操作延后投递到应用的串行执行器并直接返回，例如使用 `asio::post`。实例有效期间可并发读取 `block_period()` 与 `tick_position()`。
- 处理路径尽量避免锁等待，禁止堆分配、释放和调用普通应用回调。异步使用块数据必须在 process() 返回前复制到后端自有缓冲。

### 构建与验证

项目使用 C++14，core 通过 CMake 链接 Threads::Threads。应用链接 aereborn::plugins，单独使用核心可链接 aereborn::core。

~~~powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel 4
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
~~~

测试按 core、runtime、routing、convert、resampling、mix、device、tone、allocation、example 分组。引擎测试使用内置时钟，通过测试节点在块边界与测试控制侧同步。混音、格式转换和重采样使用独立字节或数值参照；完整算法链验证不同格式节点直接连线。覆盖分数帧、格式拒绝、禁止隐式混音、只读共享、参数斜坡、计划切换、停止屏障、失败恢复和实时路径无堆操作。

MSVC Debug 的迭代器代理会在 noexcept 构造中分配内存，allocation 用例按项目规则跳过，在 Release 完整执行。示例由默认开启的 BUILD_EXAMPLES 控制。

后续真实设备接入仍需后端处理短读写、硬件缓冲、不同设备时钟漂移、会话代际和热插拔；这些职责不进入 Engine 的格式校验与图调度。

## 8. 实现顺序

1. **离线图核心**：Node、Port、Connection、音频块及 Engine 的准备与执行。用插件节点验证多对多、处理器串联、总线互连、输出分发、环路拒绝。
2. **运行内核**包括内置软件时钟、块边界计划切换、同步提交、插件参数、显式 Mixer/Matrix 和停止屏障。验证调音量时复用图计划，修改连接时保留未变节点的状态。
3. **一个平台的设备闭环**包括多设备边界适配、DeviceSession、热插拔与恢复。验证通过缓冲交付音频，拔插期间其他连接继续处理，再接入另一个平台。
4. **文件、推入和混音策略**：预读/录制、PlaybackHandle、PushSource、DSP 链、ducking。验证多实例、循环、过载报告和增益包络。
5. **网络**：先 PCM，再 Opus、乱序/抖动缓冲与丢包恢复；验证本地与远程源进入同一 Bus，断网后有界退化并恢复。

各阶段复用同一张图与同一套节点执行机制。

第一步的验收用例包括 `core.audio_block_layout`、`core.commit_retry`、`routing.explicit_mix_and_fanout`、`routing.algorithm_chain`、`routing.silence_and_pruning` 和 `core.feedback_rejection`。路由走实际的 Convert、Mixer 和 Matrix，算法由声明固定 PCM 格式的测试节点提供。

第二步的验收覆盖 `runtime` 用例、`allocation.render_uses_prepared_buffers` 和 `example.clocked_graph`。
