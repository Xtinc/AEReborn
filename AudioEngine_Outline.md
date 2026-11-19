# AEReborn：音频引擎框架草案

采用 C++14；后续异步控制与 I/O 使用 standalone Asio，当前离线核心不依赖 Asio。本稿确定核心对象与运行边界，暂不展开网络协议、队列算法和恢复参数。
命名空间统一为 `aereborn`，方向类型采用 `IPort/OPort`、`IBlock/OBlock` 的对称命名。
第一阶段离线图与虚拟 PCM 收发原型已落地，实际 API、运行约定及构建方法见第 7 节。本稿中的设备、网络和异步控制仍是后续设计。

## 1. 参考与取舍

下面区分成熟引擎已有的设计，以及本项目据此作出的选择。参考设计思路，不引入这些引擎作为依赖。

| 官方参考 | 可借鉴的设计 | 本项目的选择 |
| --- | --- | --- |
| [JUCE AudioProcessorGraph](https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html) | 处理器组成节点图，通过通道连接；提供图重建机制 | 统一 Node/Port/Connection，将图描述编译为执行计划 |
| [JACK 实时处理回调](https://jackaudio.org/api/group__ClientCallbacks.html) | 处理回调禁止内存分配及可能阻塞的 I/O、锁和等待 | 音频执行路径只处理已准备的数据，外部工作通过有界队列交换 |
| [PipeWire 图运行](https://docs.pipewire.org/devel/page_running.html)、[调度](https://docs.pipewire.org/page_scheduling.html)、[时钟](https://docs.pipewire.org/page_man_pipewire-props_7.html) | 调度组选择 driver；配置与数据处理分离；区分设备时钟并适配 | 第一版只做一个渲染执行域、一个 ClockDriver，预留边界时钟适配 |
| [FMOD 混音与连接](https://www.fmod.com/docs/2.04/api/mixing-and-routing-in-the-core-api.html)、[DSPConnection](https://www.fmod.com/docs/2.04/api/core-api-dspconnection.html) | 总线分组、子混音、效果处理；连接持有增益和声道矩阵 | Bus 可接 Bus；Connection 持有增益、静音和声道矩阵，处理器保持单一职责 |

第一版不做多进程共享图、节点并行调度、插件宿主或完整 DAW 自动化系统。先实现能运行、能恢复、能验证的单进程引擎。

## 2. 核心概念与节点实现

Engine 只处理 Node、Port、Connection 三个核心概念。它拥有节点和连接，校验图、准备缓冲并按依赖顺序执行节点；不需要识别节点的具体业务角色。

```text
Node A 的 OPort ── Connection ──> Node B 的 IPort
```

| 对象 | 定位 |
| --- | --- |
| Node | 具有输入/输出端口的执行单元 |
| Port | 节点上的多声道音频入口或出口，带声道布局 |
| Connection | 输出端口到输入端口的有向连接，独立持有增益、静音和声道矩阵 |

Port 按方向使用 `IPort/OPort` 引用，节点只声明每个端口的声道数。所有输入端口都可以接多条连接，引擎在每个周期应用各连接的增益和矩阵后求和。没有连接的输入得到静音。
一个 Port 表示一组声道，声道间的选择、复制、合并由 Connection 的矩阵表达。
`NodeId/ConnId` 是类型化句柄，原型采用进程内不复用的唯一 ID。Node 不持有其他 Node 的共享所有权，也不各自维护连接列表或调度循环。

Source、Processor、Mixer、Sink 是具体节点的用途。`plugins/` 中的 `IAdapter/OAdapter` 是图节点；`VirtualSource/VirtualSink` 模拟图外设备，只产生和接收指定格式的 PCM，由 Adapter 完成与引擎音频块之间的转换。

| 用途 | 职责 | 当前实现 |
| --- | --- | --- |
| Source | 向图提供音频 | `IAdapter` |
| Processor | 处理输入并产生输出 | 后续插件 |
| Mixer | 将输入端口的混音结果转发到一个可供下游复用的输出 | 后续插件；当前输入端口已支持多连接求和 |
| Sink | 消费图内音频 | `OAdapter` |

PCM 适配器和固定端口数量的自定义节点通常继承 `NodeT<I, O>`，由它保存端口声道数和块视图。节点在 `prepare(const EngineSpec &)` 中取得引擎规格。端口数量由运行时决定的节点可以直接继承 `Node`，自行提供 `inputs()/outputs()` 和 `iblocks()/oblocks()` 所需的稳定存储，端口布局在加入引擎后保持固定。
后续设备、文件、网络、滤波等节点遵循同一边界。Bus 表示 Mixer 与效果节点构成的应用层总线，不是新的核心基类。

### 从输出创建输入：直接复用端口

任何 OPort 都能继续连接到其他节点，也能被订阅。Bus 可提供 PreEffects、PostEffects、PostFader 等命名输出位置，第一版先提供最终输出。
监听总线输出只返回端口引用，不再引入 BusTapSource 类、虚拟设备或额外定时循环。
编译器保证一个节点每个渲染块只执行一次，同一输出供多个下游只读使用；需要修改数据的下游使用自己的工作缓冲。

这支持“输出作为另一个输入”，并且不依赖物理设备是否在线。系统全局回环录音则作为独立 CaptureSource 能力接入。
第一版拒绝所有图内反馈环，校验同时覆盖音频与侧链依赖。

## 3. 引擎骨架

```text
core/
├── types.h / types.cpp        公共规格、图句柄、声道矩阵和基础类型实现
├── block.h                    块视图与 Buffer 接口
├── retcode.h                  错误码与带值结果接口
├── graph.h                    Node、节点默认行为和固定端口数的 NodeT
└── engine.h / engine.cpp      图编辑、准备与执行
plugins/
├── adapter.h / adapter.cpp    PCM 输入和输出适配
└── virtual.h / virtual.cpp    图外正弦 PCM 发端与原始 PCM 收端
tests/
├── engine_graph_tests.cpp        块、矩阵、图校验、准备重试和句柄
├── graph_routing_tests.cpp       使用虚拟端点和测试节点验证离线路由
├── pcm_adapter_tests.cpp         PCM 格式、绑定、配置校验与声道映射
├── pcm_resampling_tests.cpp      相位连续性、采样率比例和抗混叠
├── pcm_reconfiguration_tests.cpp  虚拟设备切换和逻辑端口稳定性
├── virtual_pcm_tests.cpp         虚拟 PCM 发端与收端的独立行为
├── allocation_failure_tests.cpp  缓冲扩容和 PCM 配置的分配失败恢复
└── support/                      用例入口、独立 PCM 参照和路由夹具
examples/
└── virtual_routing.cpp        虚拟设备通过 Adapter 接入引擎
```

`core/types.cpp` 集中实现缓冲、错误消息、图句柄和声道矩阵。`core/engine.cpp` 实现引擎生命周期。节点的默认行为、模板和 `constexpr` 函数定义放在头文件中。`aereborn::core` 是 CMake 静态库目标，提供核心实现和头文件路径。`aereborn::plugins` 提供 PCM 适配器和图外虚拟收发端点，并链接核心库。
Engine 直接拥有 Node、Connection 和图缓冲池。`prepare()` 完成依赖排序、预分配缓冲和端口绑定；执行计划只保存排好顺序的节点记录指针，块视图和连接区间直接保存在节点记录中，`process()` 直接遍历这些记录。
固定端口数量通过 `NodeT<I, O>` 确定，端口声道数和块视图使用 `std::array`。采样数据集中在图缓冲池，节点执行顺序和连接索引使用可复用的连续数组。
`IBlock/OBlock` 分别是 `Block<const float>/Block<float>`，共享布局与寻址实现，只允许可写块转换为只读块。`ProcContext` 通过 `View` 传入块数组，端口数量直接从视图取得。
`iblocks()/oblocks()` 供 Engine 准备缓冲时绑定块视图。`NodeT` 已实现这两个接口，节点处理音频统一通过 `ProcContext`。
`Connection` 是 Engine 的内部记录，持有端口引用、参数和由 `prepare()` 写入的缓冲绑定。编辑与执行共享这些记录，节点通过按输入端口排序的指针索引访问它们。
每个节点记录保存输入缓冲的连续采样区间；渲染阶段一次清零该区间，无需恢复声道数、逐声道清零或重新累加端口偏移。
节点统一使用准备、处理、重置生命周期。后续设备、网络、文件的后台服务负责外部资源，其音频入口与出口由插件节点接入；服务不继承 Node，也不使用全局后台线程单例。

### 图描述与音频执行分离

```text
编辑节点与连接 -> 校验并准备 -> 执行计划 -> 块边界切换
参数更新 ---------------------------------> 块边界应用并平滑
```

执行计划是内部节点执行顺序与连接索引，不是新的图模型。当前原型同步串行准备和执行；后台运行接入后，再增加块边界计划切换、参数通道及以下状态交接约束。
音频侧使用已解析端口和分配好的缓冲，不遍历可变容器查找目标，也不现场建立连接。
运行中的 DSP 状态仅由渲染侧修改。图更新时按稳定节点身份保留未改变的状态，避免每次连线都重置滤波器、播放位置和增益包络。
旧计划及已移除状态在确认音频侧不再使用后，由非音频线程销毁。

修改节点、连接和布局才重建计划。音量、静音、暂停、抑制目标等通过有界参数通道更新；增益在音频侧平滑，避免频繁重建和突变。
参数绑定句柄及计划代际，过期更新不会误操作被复用的对象。第一版按块应用参数，不做逐采样自动化时间轴。

## 4. 执行与时钟

第一版只有一个串行渲染执行域：任意时刻只有一个执行上下文可以调用 render。
选定的 ClockDriver 驱动整张图，其他设备仅通过边界适配器交换数据，不分别执行同一张图。
引擎周期是运行时的基础节拍。当前 Adapter 的每块 PCM 覆盖整数个引擎周期；这是图边界的交付约定，不要求硬件周期也满足这个比例。后端负责将实际设备收发整理为该交付粒度，驱动按基础周期推进图。

| 场景 | ClockDriver |
| --- | --- |
| 主设备周期等于引擎周期 | 该设备的音频回调或设备 I/O 线程可提供基础节拍 |
| 设备周期覆盖多个引擎周期 | 由基础周期驱动器推进图，设备在各自的周期边界收发 |
| 主设备掉线、纯网络或无物理设备 | 独立的 TimerDriver，保持逻辑时间推进 |
| 离线验证或导出 | 调用方直接推进，不等待真实时间 |

主设备由显式配置或确定的选择策略指定，可以是采集或播放设备，不把某一方向写死为主时钟。
主设备能提供基础节拍时，可以直接在其回调或设备线程执行渲染。
主驱动切换须先确认旧驱动退出，再接管执行权；帧序号继续递增，并报告时钟切换或数据不连续。

内部统一使用 planar float32。`Engine(sample_rate, block_period)` 构造时固定图规格，通过 `spec()` 按值返回 `EngineSpec`。创建 PCM 适配器时传入这份规格和图内端口声道数。
`SampleRate` 提供 8000、11025、16000、22050、24000、32000、44100、48000、88200、96000 Hz 的枚举值。图采样率范围为 1 至 96000 Hz，`BlockPeriod` 支持 `Ms05`、`Ms10`、`Ms20` 和 `Ms40`。
每周期帧数按 `采样率 × 周期毫秒 / 1000` 精确计算，通过 `engine.spec().block_frames()` 查询。例如 44.1 kHz / 10 ms 为 441 帧。44.1 kHz / 5 ms 无法得到整数帧，规格中的帧数为零，`Engine::prepare()` 返回 `InvalidArg`。
引擎将规格传给节点的 `prepare(const EngineSpec &)`，节点可以据此准备算法资源。`ProcContext` 提供当前帧位置及输入输出块视图，块视图提供实际帧数。
`RetCode process() noexcept` 是唯一处理入口，每次固定处理一周期；短输入在源节点补零，输出和时间推进仍为完整周期。图尚未准备时返回 `ErrorCode::NotPrepared`，不执行节点、不消费输入、不推进时间；准备成功的空图返回 `ErrorCode::Ok` 并推进一周期。
当前原型由调用方推进。`IAdapter` 和 `OAdapter` 在每个引擎周期处理已绑定设备缓冲的一段 PCM。设备驱动接入后提供实际的设备周期及收发缓冲。

从设备与引擎时钟不一致时，在边界做缓冲与自适应重采样。即使标称采样率相同，也不能默认两个设备共享时钟。

### Asio 与实时处理的边界

Asio 负责控制命令、UDP I/O、设备事件、恢复定时及非实时任务协调。
设备打开/关闭、文件访问、编解码和应用回调都在非渲染路径完成。设备必要的实时缓冲交付由平台后端封装。
音频路径不分配/释放堆内存，不等待互斥锁，不打印日志，不调用普通应用回调，也不依赖一次 asio::post 在截止时间前完成。

```text
Asio 控制执行器 ──> 计划/参数 ──> 渲染路径
从设备 / 网络 / 文件 ──> 有界队列 ──> 音频入口节点
音频出口节点 ──> 有界队列 ──> 从设备 / 网络 / 文件
观测点 ──> 有界订阅队列 ──> 用户回调执行器
```

队列预先明确单/多生产者归属、容量和满队列行为。活跃数据流不允许无限堆积；慢订阅只影响自身，录音写入跟不上时报告可观测的数据缺口。
调用停止或取消订阅的完成通知代表相关处理已经退出；不能仅取消定时器就释放仍在使用的对象。

## 5. 外部资源与恢复

逻辑端点长期存在，物理句柄归 DeviceSession。DeviceService 分开维护设备清单、选择策略与运行会话。
选择可以是固定设备、跟随系统默认、优先某类设备；不在收到任意插入事件后立即抢占当前设备。
设备身份使用平台稳定标识及匹配规则，不依赖枚举序号永久不变。

```text
逻辑意图：Running / Paused / Stopped
资源状态：WaitingDevice -> Opening -> Ready -> Recovering / Failed
```

两组状态分别维护：恢复完成仍要服从用户当前意图。
掉线期间保留图、音量、订阅和连接；缺失采集数据补静音并标记，输出设备不可用时丢弃该设备数据，总线及其他下游继续处理。
可就地恢复的欠载由后端处理；需重开、等待设备、不可恢复的配置错误由统一恢复策略区分。
重试带退避和取消，主动定期重开也是同一策略的选项。会话代际编号屏蔽过期打开结果与旧设备数据。

同一设备的兼容逻辑端点共享 DeviceSession。播放侧明确汇入该设备的最终混音节点，再交给唯一设备写入者；独占或格式冲突明确报错。
设备协商格式与逻辑音频格式分离，恢复不改变应用看到的端口布局；适配不了时报告端点失败。

网络与文件复用相同入口/出口节点机制：

- NetworkSource：会话识别、乱序整理、按播放期限解码/FEC/PLC，再交付音频。NetworkSink：排队、编码、发送。先 PCM，后 Opus；协议独立版本化。
- FileSource：工作线程预读，独立 PlaybackHandle 管理循环、暂停和停止。FileSink：排队写入，支持分段保存。
- PushSource：应用显式推入带格式、帧时间和连续性信息的音频，接口报告接收数量或队列满。
- Observer：订阅现有输出端口，指定格式和交付帧数，回调携带帧时间及缺口标记。它不参与驱动音频图。

## 6. 混音策略归属

Bus 负责混音与分组控制，Connection 描述输入角色；抑制策略根据 Voice、Music、Alert 等角色决定目标增益。
策略最终产生独立的增益包络处理，不把角色判断散落到设备、网络或所有节点的 process 中。

```text
Connection 增益/矩阵 -> Bus 混音 -> 效果链 -> 总线音量 -> 最终输出
                         ↑
                  MixPolicy / 侧链检测
```

普通相加是默认策略，ducking 是可配置策略，具有 attack、hold、release。
策略明确按“有效帧”“能量”还是“语音活动”触发；补出的静音、暂停或断流不算活动。
同一 Source 在不同 Connection 中可以有不同角色。同一处理器接口可表达限幅、滤波及带参考输入的自适应滤波，不为每种算法扩展 Engine API。

## 7. 第一阶段 API 使用形态

应用直接操作节点句柄，统一连接 OPort 到 IPort，不再区分 connect、attach 和创建监听输入。
链路为 `VirtualSource → IAdapter → Engine → OAdapter → VirtualSink`，只有 Adapter 加入图。下面展示成功路径；完整错误处理及异采样率、多周期收发见 [examples/virtual_routing.cpp](examples/virtual_routing.cpp)。其中 `checked()` 在失败时报告错误，成功时取出结果值。

```cpp
#include "core/engine.h"
#include "plugins/adapter.h"
#include "plugins/virtual.h"

using namespace aereborn;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
const PCMSpec spec{rate, PCMFormat::S16LE, 2, 1};
VirtualSource source(spec, period, 440.0, 0.25f);
VirtualSink sink(spec, period);
checked(source.prepare());
checked(sink.prepare());

Engine engine(rate, period);
const auto input_id = checked(engine.emplace_node<IAdapter>(2, engine.spec()));
const auto output_id = checked(engine.emplace_node<OAdapter>(2, engine.spec()));
checked(engine.connect({input_id}, {output_id}));
checked(engine.prepare());
auto *input = checked(engine.node<IAdapter>(input_id));
auto *output = checked(engine.node<OAdapter>(output_id));
checked(input->configure_pcm(spec));
checked(output->configure_pcm(spec));
std::vector<std::uint8_t> pcm(output->pcm_bytes());

checked(source.generate());
checked(input->bind_pcm(source.pcm()));
checked(output->bind_pcm({pcm.data(), pcm.size()}));
checked(engine.process()); // 一周期 240 帧；本例设备周期也是 5 ms。
checked(sink.receive({pcm.data(), pcm.size()}));
```

调用由应用同步串行。拓扑改变后调用 `prepare()`，连接增益和静音在下一块生效。应用可查询引擎每周期帧数和当前帧位置。Engine 直接持有节点、连接、缓冲池与执行计划。
后续接入后台执行时，再增加异步图提交和块边界切换，区分编辑失败、计划已应用与端点已就绪，不改变单一 Node 模型。
第一版不提供外部插件 ABI，不依赖 C++17/20 特性。

### 错误处理

参数、状态和容量校验等可预期的操作失败通过 `RetCode` 返回，同时返回节点指针或句柄时用 `Result<T>`。两者都可用 `if (!result)` 判断失败。`Result<T>::ret_code()` 返回 `RetCode`，调用方按 `code()` 分支，可在控制侧用 `message()` 读取固定错误文本。检查成功后才调用 `value()`。

引擎在加入节点时检查端口声道数，在 `prepare()` 校验图的采样率与周期。节点通过 `prepare(const EngineSpec &)` 校验自身配置并准备处理资源，失败时可以重试，已准备成功的节点保留状态。Adapter 的设备资源由独立的 `configure_pcm()` 准备，图可以先准备和运行，再接入设备。`Node::process()` 是 `void noexcept`，引擎未准备好时 `Engine::process()` 返回 `NotPrepared`。

控制侧分配失败以及自定义节点在构造或准备时抛出的异常会传给调用方。`ChMatrix` 直接构造，参数非法时抛 `std::invalid_argument`，尺寸超出存储容量时抛 `std::length_error`。低层块视图的索引和跨度由调用者保证。

### 当前原型的运行约定

- `prepare()` 从所有无输出端口的 Sink 反向收集依赖。节点首次进入执行计划时准备资源，并在计划中按依赖顺序处理。
- 拓扑改变后 `process()` 返回 `NotPrepared`，直到重新准备成功。已准备的节点保留暂存音频和 DSP 状态。
- 连接的增益和静音参数同步修改，下一块生效。
- 自定义节点通过 `context.inputs[i]`、`context.outputs[i]` 访问块，通过 `.size()` 查询端口数量。引擎清零输入，节点必须写满块视图 `frames()` 指定的输出跨度。`ProcContext` 提供块的起始帧位置。

### 外部 PCM 输入和输出

`plugins/adapter.h` 提供两个独立的节点 `IAdapter` 和 `OAdapter`，构造时接收固定的图内端口声道数和 `EngineSpec`。通过 `configure_pcm(PCMSpec spec, const ChMatrix &matrix = {})` 配置或替换设备侧转换状态。`PCMSpec{sample_rate, format, channels, periods}` 全部描述设备侧 PCM，设备声道数可以与图内声道数不同。`periods` 是一块 PCM 覆盖的引擎周期数，默认为 1。当前虚拟设备的收发周期与该交付块一致。

外部使用交错 PCM，格式包括 `S16LE`、`S24_3LE`、`S24LE`、`S32LE` 和 `F32LE`。`pcm_sample_bits()` 查询样本位深，`pcm_sample_bytes()` 查询存储字节数。`S24_3LE` 每样本占三字节，`S24LE` 每样本占四字节，低 24 位有效，写出的高字节为零。

- 图内端口声道数在加入引擎时校验。Adapter 在首次进入执行计划时核对其构造规格与当前图一致。未配置设备时输入产生静音，输出丢弃数据，图照常推进，此时 `bind_pcm()` 返回 `NotPrepared`。独立配置时也会校验图规格和逻辑声道数。
- `configure_pcm()` 在临时转换器中完成校验、缓冲分配及滤波准备，成功后替换旧转换器、解除旧缓冲绑定并从新流的第一个周期开始。无论返回错误码还是抛出异常，旧配置、缓冲绑定、周期索引和滤波历史都保持有效。配置可以在图准备前或准备后进行，无需再次调用 `Engine::prepare()`，节点身份、图内端口、连接及引擎帧位置保持不变。
- `ChMatrix` 的行是目标声道，列是源声道。输入适配器的矩阵方向是设备→图，输出适配器是图→设备。相同声道数时空矩阵表示直通；不同声道数必须提供显式矩阵，否则返回 `ChannelMismatch`。不自动复制单声道，也不自动混合多声道；相同声道数也可用显式矩阵选择、交换或混合声道。
- `pcm_frames()` 和 `pcm_bytes()` 在配置成功后返回一块设备 PCM 的帧数和字节数，未配置时返回零。设备采样率限于 `SampleRate` 列出的值，周期配置必须能得到整数 PCM 帧数。以 48 kHz / 5 ms 的图连接 44.1 kHz / 10 ms 设备为例，一块 441 帧的 PCM 在图内按 221 帧和 220 帧处理。
- 调用方在各设备周期开始前用 `bind_pcm()` 绑定对应的整块缓冲，并保证它在接下来 `periods` 次引擎推进期间有效。适配器逐周期读写对应片段，设备周期结束后自动解除绑定，驱动可提交输出。上一块仍在使用时重新绑定返回 `BufferFull`。缺少输入时图得到静音，缺少输出缓冲时图继续推进。
- 两个适配器公开继承 `NodeT`，各自保存借用的设备缓冲和一个 `PCMConverter`。适配器共用配置和绑定校验，转换器负责计算 PCM 切片和推进周期。输入先解码为设备声道的 float32，重采样后映射到图内声道。输出先映射到设备声道，再重采样和编码。Adapter 和图连接都用 `ChMatrix::mix_add()` 累加样本，尺寸在各自的配置入口校验。同采样率、空矩阵的 `F32LE` 保持样本位表示，小端单声道可以整块复制，多声道按布局重排。整数 PCM 在设备边界做归一化或量化。
- **不新增缓冲延迟。** 解码、声道映射和编码在当前 `process()` 内完成，工作缓冲不跨周期排队。即使绑定了多个周期的 PCM，也逐周期直接写入输出，不等待整块计算结束。只有既有 sinc 重采样器带来 48 个源端帧的滤波延迟；本次声道映射和配置切换不叠加额外周期或帧延迟。
- `reset()` 解除绑定、将周期索引归零并清除相位和滤波历史，保留设备配置。拔出设备时重置对应 Adapter，停止绑定数据；重新接入后配置新格式并绑定新缓冲。不调用全局 `Engine::reset()`，其他节点和引擎时间继续运行。
- `configure_pcm()` 会分配和释放资源，必须在非音频线程执行，且与 `prepare()/bind_pcm()/reset()/process()` 串行。本阶段不提供并发配置或无停顿的后台状态交接。

例如图内始终为 48 kHz、2ch，采集设备改为 16 kHz、1ch 时，调用方显式决定复制规则：

```cpp
const ChMatrix capture_matrix(2, 1, {1, 1});
checked(input->configure_pcm({SampleRate::Hz16000, PCMFormat::S16LE, 1}, capture_matrix));
// 下一次绑定使用 input->pcm_bytes() 对应的新规格；现有图连接保持不变。
```

### 后续设备后端的接入边界

当前 Adapter 支持固定逻辑端口和串行设备格式切换。设备会话、I/O 和队列与图节点分离，可以继续用于 ALSA、文件或网络后端；接入真实设备还需要补齐以下能力。

- **硬件收发与图周期解耦。** 例如 48 kHz 的 256 帧硬件周期不是 5 ms 图周期的整数倍。后端通过预分配的有界缓冲拼接和拆分数据，处理短读写，再向 Adapter 交付完整 PCM 块。`PCMSpec::periods` 表示这块数据的图周期数，不应直接等同于 ALSA 的硬件 period。ALSA 还支持非交错和 mmap 布局；当前接口只接收连续交错 PCM，其他布局由后端整理，后续有实际需求再扩展视图。[ALSA PCM 接口](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)与[读写接口说明](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html)描述了这些布局和实际传输帧数。
- **设备时钟适配。** 当前重采样按固定标称采样率转换，没有队列水位反馈，也不会修正两个同采样率设备的时钟漂移。真实多设备接入时，需要在边界支持根据缓冲水位微调转换比例，并报告欠载、溢出和数据缺口，不能只依赖现有固定比例转换器。
- **恢复和连续性。** 当前可串行调用 `configure_pcm()` 切换设备规格，后续运行内核还需要支持后台准备、块边界接管和非音频线程回收旧转换状态。当前缺少绑定时不会推进转换器的滤波历史；流中断后重新绑定前应调用该 Adapter 的 `reset()`，或成功配置一份新转换状态。时间戳、缺口标记与会话代际尚未实现。
- **缓冲所有权。** 当前绑定借用调用方内存，并要求与渲染串行操作。异步设备线程不能直接与 `process()` 并发调用 `bind_pcm()` 或 `reset()`；后续由后端队列完成所有权交接，再由渲染侧绑定。完成通知和停止屏障由运行内核提供。

上述扩展继续放在设备边界，不要求 Engine 识别 ALSA 或其他具体后端。当前实现尚不能直接充当完整的异步设备适配层。

### 图外虚拟 PCM 端点

`plugins/virtual.h` 提供非模板类 `VirtualSource` 和 `VirtualSink`，两者都不继承 `Node` 或 `PCMConverter`。它们接收 `PCMSpec` 和引擎周期，用设备采样率、格式、声道数和跨越的引擎周期数确定一包交错 PCM 的布局。

`VirtualSource::generate()` 按自身连续的采样位置直接生成指定格式的正弦 PCM，通过 `pcm()` 借出一整包数据，不依赖引擎的帧位置。调用方将这包数据绑定到 `IAdapter`，在其消耗完之前不再生成下一包。源端 `reset()` 才会重启正弦波时间位置，单独重置引擎不影响源端。

`OAdapter` 将图内音频转换到调用方绑定的 PCM 缓冲，完成 `periods` 次引擎推进后，调用方将整包交给 `VirtualSink::receive()` 保存。收端不解码、不重采样；`matches(expected)` 逐字节比较最近收到的 PCM。需要验证混音或重采样时，测试独立计算采样参照与误差，避免使用待验证的转换器生成参照。

构建与验证（Visual Studio 多配置生成器）：

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

使用 PCM 适配器或虚拟端点的应用链接 `aereborn::plugins`，自定义节点可以只链接 `aereborn::core`。

测试源文件按验证对象命名，统一使用小写下划线和 `_tests.cpp` 后缀。共用的断言、PCM 参照和路由夹具放在 `tests/support/`。每个用例独立注册为 CTest 的 `组名.场景名`，并使用组名作为 label。

| 分组 | 源文件 | 验证边界 |
| --- | --- | --- |
| `core` | `engine_graph_tests.cpp` | 只链接核心库，覆盖块视图、矩阵构造、运行时图规格、句柄、端口、反馈拒绝、准备重试和异常传播 |
| `routing` | `graph_routing_tests.cpp` | 经 Adapter 连接图外 `VirtualSource/VirtualSink`，验证路由、拓扑重建和跨设备周期 |
| `adapter` | `pcm_adapter_tests.cpp` | 独立字节参照、PCM 量化与格式、缓冲绑定、图规格匹配、声道映射和脉冲延迟 |
| `resampling` | `pcm_resampling_tests.cpp` | 正弦相位参照、分数周期、8–96 kHz 极端比例、滤波历史重置和抗混叠 |
| `device` | `pcm_reconfiguration_tests.cpp` | 模拟采集和播放切换、旧缓冲解绑、稳定的节点身份和逻辑端口 |
| `virtual` | `virtual_pcm_tests.cpp` | 发端 PCM 字节和独立时间线、收端报文所有权、端点配置校验 |
| `allocation` | `allocation_failure_tests.cpp` | 缓冲扩容和 PCM 配置分配失败后的异常传播与状态恢复 |
| `example` | `examples/virtual_routing.cpp` | 构建并运行可独立使用的示例 |

`routing` 覆盖一对一、一对多、多对一、多对多、处理器串联、Bus 串联、效果前后输出监听、多输入输出端口、声道矩阵、缺失端点和不同设备周期。输出与独立计算的正弦叠加、增益和矩阵结果比较，并检查共享节点每周期只执行一次。虚拟处理节点只放在测试目录，不作为正式 DSP 插件；系统回环、真实异步 Observer、设备线程和热插拔服务仍属于后续运行内核。

MSVC Debug 的标准容器会在 `noexcept` 构造内分配迭代器代理，因此分配失败注入在该配置下显式标记为跳过，在 Release 中完整执行。无效参数、容量超限和失败状态保留在两种配置下均验证。

可以单独执行一个分组或场景：

```powershell
ctest --test-dir build -C Debug -L routing --output-on-failure
ctest --test-dir build -C Debug -R '^core.feedback_rejection$' --output-on-failure
ctest --test-dir build -C Release -L allocation --output-on-failure
```

示例由 `BUILD_EXAMPLES` 控制，默认开启，也会作为 `example.virtual_routing` 加入 CTest。关闭测试后仍可独立构建和运行示例：

```powershell
cmake -S . -B build/examples-only -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=ON
cmake --build build/examples-only --config Debug --parallel
./build/examples-only/examples/Debug/virtual_routing.exe
```

## 8. 实现顺序

1. **离线图核心**：Node、Port、Connection、音频块及 Engine 的准备与执行。用插件节点验证多对多、处理器串联、总线互连、输出分发、环路拒绝。
2. **运行内核**：ClockDriver、块边界计划切换、参数更新、有界队列、Observer、停止屏障。验证调音量不重建图，修改连接不重置未变节点的状态。
3. **一个平台的设备闭环**：主设备驱动、多设备边界适配、DeviceSession、热插拔与恢复。验证同一张图只被一个驱动执行，拔插不破坏其他连接；再接另一个平台。
4. **文件、推入和混音策略**：预读/录制、PlaybackHandle、PushSource、DSP 链、ducking。验证多实例、循环、过载报告和增益包络。
5. **网络**：先 PCM，再 Opus、乱序/抖动缓冲与丢包恢复；验证本地与远程源进入同一 Bus，断网后有界退化并恢复。

第一步先写少量核心类型和离线执行，不接系统设备，不启动 Asio 线程。每一步复用同一张图与同一套节点执行机制。

第一步的验收用例包括 `core.audio_block_layout`、`core.prepare_retry`、`routing.many_to_many`、`routing.processor_chain`、`routing.bus_chain_and_taps`、`routing.one_to_many` 和 `core.feedback_rejection`。路由走实际的 PCM Adapter，处理器和总线由测试节点提供。
