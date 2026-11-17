# AEReborn：音频引擎框架草案

采用 C++14；后续异步控制与 I/O 使用 standalone Asio，当前离线核心不依赖 Asio。本稿确定核心对象与运行边界，暂不展开网络协议、队列算法和恢复参数。
命名空间统一为 `aereborn`，方向类型采用 `ISpec/OSpec`、`IPort/OPort`、`IBlock/OBlock` 的对称命名。
第一阶段虚拟节点原型已落地，实际 API、运行约定及构建方法见第 7 节。本稿中的设备、网络和异步控制仍是后续设计。

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

Port 按方向使用 `IPort/OPort` 引用，使用 `ISpec/OSpec` 声明规格。`IMode::Single` 只接受一条连接，`IMode::Mix` 允许多条连接并由引擎求和；这是端口能力，任何自定义 Node 都能使用。
一个 Port 表示一组声道，声道间的选择、复制、合并由 Connection 的矩阵表达。
`NodeId/ConnId` 是类型化句柄，原型采用进程内不复用的唯一 ID。Node 不持有其他 Node 的共享所有权，也不各自维护连接列表或调度循环。

Source、Processor、Mixer、Sink 是具体节点实现的用途分类，统一放在 `plugins/`，不再作为核心对象或额外基类：

| 用途 | 职责 | 当前实现 |
| --- | --- | --- |
| Source | 产生音频 | `BufferSourceNode`、`SineNode` |
| Processor | 处理输入并产生输出 | `GainNode` |
| Mixer | 以允许多条连接的输入汇合音频，并提供输出 | `MixerNode` |
| Sink | 消费输入音频 | `BufferSinkNode` |

这些内置插件与应用自定义节点都实现同一个 Node 接口。`NodeT<I, O, Rate, Period>` 只帮助声明固定数量的端口，不区分业务角色。当前插件采用头文件模板；增加具体节点无需为 Engine 增加专用 API、插件注册表或动态加载机制。
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
├── graph.h / graph.cpp        音频规格、Node、Port、Connection、句柄、矩阵及共享 ID 分配
├── block.h / block.cpp        非持有块视图与持有采样内存的 Buffer
└── engine.h                   Engine 模板：图编辑、校验、准备、执行与生命周期
plugins/
└── nodes.h                    Source、Processor、Mixer、Sink 的具体 Node 模板
tests/                        核心独立验证、插件回归与使用示例
```

`core/graph.h` 集中声明最核心的抽象，没有额外的 Graph 包装对象。`aereborn::core` 只编译核心，`aereborn::plugins` 提供内置节点模板并传递核心依赖；核心不依赖插件。应用可以只链接核心并提供自己的 Node 实现。
Engine 直接拥有 Node、Connection 和图缓冲池。`prepare()` 完成依赖排序、预分配缓冲和端口绑定；执行计划只保存排好顺序的节点记录指针，块视图和连接区间直接保存在节点记录中，`process()` 直接遍历这些记录。
固定端口数量通过 `NodeT<I, O, Rate, Period>` 确定，规格与块视图使用 `std::array`；采样数据集中在图缓冲池，节点执行顺序和连接索引使用可复用的连续数组。
`IBlock/OBlock` 分别是 `Block<const float>/Block<float>`，共享布局与寻址实现，只允许可写块转换为只读块。`ProcessContext` 通过 `View` 传入块数组，端口数量直接从视图取得。
`Node` 与 `NodeT` 的 `iblocks()/oblocks()` 是私有绑定接口，仅供 Engine 准备缓冲；节点处理音频统一通过 `ProcessContext`，公共接口提供端口规格、生命周期与诊断输出。
Connection 从编辑到执行始终是同一个对象，持有端口引用、参数以及由 `prepare()` 写入的私有缓冲绑定。节点记录通过按输入端口排序的指针索引访问已有 Connection，不再复制出另一套连接执行结构。
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

| 场景 | ClockDriver |
| --- | --- |
| 有指定主设备 | 该设备的音频回调或设备 I/O 线程提供处理节拍 |
| 主设备掉线、纯网络或无物理设备 | 独立的 TimerDriver，保持逻辑时间推进 |
| 离线验证或导出 | 调用方直接推进，不等待真实时间 |

主设备由显式配置或确定的选择策略指定，可以是采集或播放设备，不把某一方向写死为主时钟。
第一版可以直接在主设备回调/设备线程执行渲染；不要求另起一条永远由定时器唤醒的音频线程。
主驱动切换须先确认旧驱动退出，再接管执行权；帧序号继续递增，并报告时钟切换或数据不连续。

内部统一使用 planar float32。`Engine<Rate, Period>` 直接使用两个枚举作模板参数，默认 `SampleRate::Hz48000`、`BlockPeriod::Ms10`；同一引擎内的 Node 和内置插件采用相同模板参数，仅端口声道数可以不同。C++14 下默认实例写作 `Engine<>`、`GainNode<>`。
`SampleRate` 提供 8000、11025、16000、22050、24000、32000、44100、48000、88200、96000 Hz，最高 96 kHz；`BlockPeriod` 提供 `Ms05`、`Ms10`、`Ms20`、`Ms40`。
每周期帧数精确计算为 `采样率 × 周期毫秒 / 1000`，由内部 constexpr 函数统一计算；不能组成整数帧的组合在实例化时编译失败，不进行四舍五入。例如 44.1 kHz/5 ms 不允许，44.1 kHz/10 ms 为 441 帧。11.025 kHz 仅允许 40 ms，22.05 kHz 允许 20/40 ms，44.1 kHz 允许 10/20/40 ms，其余列出的采样率允许四种周期。
`block_frames()` 可用于常量表达式；不再提供配置结构体、运行时配置构造参数或 `config()`。节点 `prepare()` 直接使用模板规格，`ProcessContext` 仅传递帧位置和输入输出块视图，不重复携带采样率与帧数。
`bool process() noexcept` 是唯一处理入口，每次固定处理一周期；短输入在源节点补零，输出和时间推进仍为完整周期。图尚未准备时返回 false，不执行节点、不消费输入、不推进时间；准备成功的空图返回 true 并推进一周期。
当前原型由调用方推进；设备和时钟驱动接入后负责真实时间调度以及平台回调块长的适配。本次不实现重采样，推入和读取的样本均按引擎采样率解释。

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
以下使用已经实现的虚拟节点；完整可运行程序见 [tests/virtual_routing.cpp](tests/virtual_routing.cpp)。

```cpp
#include "core/engine.h"
#include "plugins/nodes.h"

using namespace aereborn;
constexpr auto rate = SampleRate::Hz48000;
constexpr auto period = BlockPeriod::Ms05;
Engine<rate, period> engine; // 一周期 240 帧。
auto source  = engine.emplace_node<BufferSourceNode<rate, period>>(2);
auto mix     = engine.emplace_node<MixerNode<rate, period>>(2);
auto output  = engine.emplace_node<BufferSinkNode<rate, period>>(2);
auto monitor = engine.emplace_node<MixerNode<rate, period>>(2);
auto capture = engine.emplace_node<BufferSinkNode<rate, period>>(2);

engine.connect(engine.output(source), engine.input(mix));
engine.connect(engine.output(mix), engine.input(output));
// “从输出创建输入”直接连接现有输出端口。
engine.connect(engine.output(mix), engine.input(monitor));
engine.connect(engine.output(monitor), engine.input(capture));
engine.prepare();
// 推入数据后，由调用方 engine.process() 推进一周期。
```

原型所有调用同步串行，拓扑改变后显式 prepare；参数修改无需重建图。保留帧数、帧位置、节点数与连接数查询，缓冲池用量和容量属于内部实现，不提供公开查询。Engine 直接持有节点、连接与执行计划，不采用 Impl 间接封装。
后续接入后台执行时，再增加异步图提交和块边界切换，区分编辑失败、计划已应用与端点已就绪，不改变单一 Node 模型。
第一版不提供外部插件 ABI，不依赖 C++17/20 特性。

### 节点诊断输出

`Node` 要求具体子类实现纯虚接口 `void attributes(std::ostream&) const`，输出具体类型及当前属性，末尾不换行。例如 `GainNode` 输出当前增益，`BufferSourceNode` 输出暂存容量及待消费帧数；没有额外参数的 `MixerNode` 只输出类型。
`operator<<(std::ostream&, const Node<Rate, Period>&)` 调用该接口，再统一输出已有端口规格：方向、索引、声道数及输入模式。属性直接读取当前成员，不另建属性表；缓冲绑定接口保持私有。

```cpp
#include <iostream>
std::cout << "Node[" << mix.value << "] " << engine.node(mix) << '\n';
```

诊断输出可以在准备前后调用，遵循与引擎其他操作相同的串行约定；它属于非实时路径，不在 `process()` 中调用，也不推进音频处理。流格式由调用方设置，输出失败按标准流规则处理。

### 当前原型的运行约定

- `prepare()` 从所有无输出端口的 Sink 反向收集依赖，仅执行能到达这些 Sink 的节点。未连接到 Sink 的节点仍会准备资源，但不推进其处理状态。
- 拓扑改变后 `process()` 返回 false，直到重新准备成功；已有节点不会重复 `prepare()`，因此保留暂存音频和 DSP 状态。重复调用 `prepare()` 无副作用。
- `BufferSourceNode` 只有一周期的暂存空间，未消费完时不能再次推入非空数据；每次处理消费全部暂存帧，缺少的帧补静音。它还不是跨线程队列。
- `BufferSinkNode::block()` 借用图缓冲，不复制样本；如需长期保存，调用方应在下一次处理、准备、重置或拓扑编辑前复制。
- 连接和 `GainNode` 的增益、静音参数同步立即修改，下一块生效；有界参数通道、增益平滑和异步计划切换尚未实现。
- 自定义节点通过 `context.inputs[i]`、`context.outputs[i]` 访问块，通过 `.size()` 查询端口数量；引擎清零输入，节点必须写满本次处理跨度内的全部输出。`ProcessContext` 提供块起始帧位置；节点算法直接使用对应模板规格的固定帧数。

构建与验证（Visual Studio 多配置生成器）：

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

使用内置节点的应用链接 `aereborn::plugins`，仅使用自定义节点的应用链接 `aereborn::core`。对应头文件分别通过 `plugins/`、`core/` 路径包含。
`aereborn_core_tests` 仅链接核心，验证自定义 Node、多连接输入、重新绑定以及跨规格和编译单元的 ID 唯一性；`aereborn_tests` 校验块视图、节点与端口诊断输出、混音与分发、多端口与矩阵、短输入补零、固定周期推进、拓扑更新状态保留、错误拒绝、准备失败后重试及信号生成；`virtual_routing_example` 单独演示总线与监听连接。

## 8. 实现顺序

1. **离线图核心**：Node、Port、Connection、音频块及 Engine 的准备与执行。用插件节点验证多对多、处理器串联、总线互连、输出分发、环路拒绝。
2. **运行内核**：ClockDriver、块边界计划切换、参数更新、有界队列、Observer、停止屏障。验证调音量不重建图，修改连接不重置未变节点的状态。
3. **一个平台的设备闭环**：主设备驱动、多设备边界适配、DeviceSession、热插拔与恢复。验证同一张图只被一个驱动执行，拔插不破坏其他连接；再接另一个平台。
4. **文件、推入和混音策略**：预读/录制、PlaybackHandle、PushSource、DSP 链、ducking。验证多实例、循环、过载报告和增益包络。
5. **网络**：先 PCM，再 Opus、乱序/抖动缓冲与丢包恢复；验证本地与远程源进入同一 Bus，断网后有界退化并恢复。

第一步先写少量核心类型和离线执行，不接系统设备，不启动 Asio 线程。每一步复用同一张图与同一套节点执行机制。
