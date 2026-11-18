# AEReborn：音频引擎框架草案

采用 C++14；后续异步控制与 I/O 使用 standalone Asio，当前离线核心不依赖 Asio。本稿确定核心对象与运行边界，暂不展开网络协议、队列算法和恢复参数。
命名空间统一为 `aereborn`，方向类型采用 `IPort/OPort`、`IBlock/OBlock` 的对称命名。
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

Port 按方向使用 `IPort/OPort` 引用，节点只声明每个端口的声道数。所有输入端口都可以接多条连接，引擎在每个周期应用各连接的增益和矩阵后求和。没有连接的输入得到静音。
一个 Port 表示一组声道，声道间的选择、复制、合并由 Connection 的矩阵表达。
`NodeId/ConnId` 是类型化句柄，原型采用进程内不复用的唯一 ID。Node 不持有其他 Node 的共享所有权，也不各自维护连接列表或调度循环。

Source、Processor、Mixer、Sink 是具体节点的用途。`plugins/` 提供 PCM 适配器和虚拟收发节点，图测试的其他辅助节点放在 `tests/nodes/`。

| 用途 | 职责 | 当前实现 |
| --- | --- | --- |
| Source | 产生音频 | `IAdapter`、`VirtualSource`、`BufferSourceNode` |
| Processor | 处理输入并产生输出 | `GainNode` |
| Mixer | 将输入端口的混音结果转发到一个可供下游复用的输出 | `MixerNode` |
| Sink | 消费输入音频 | `OAdapter`、`VirtualSink`、`BufferSinkNode` |

PCM 适配器和固定端口数量的自定义节点通常继承 `NodeT<I, O, Rate, Period>`，由它保存端口声道数和块视图。自定义节点可以是普通类，基类的 `Rate` 和 `Period` 与所用引擎一致。需要在构造时决定端口数量的节点可以直接继承 `Node<Rate, Period>`，自行提供 `inputs()/outputs()` 和 `iblocks()/oblocks()` 所需的稳定存储，端口布局在加入引擎后保持固定。
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
├── types.h                    采样率、周期、PCM 格式、句柄、声道矩阵和处理上下文
├── graph.h                    Node、NodeT 与共享 ID 分配
├── block.h                    非持有块视图与持有采样内存的 Buffer
├── retcode.h                  错误码、错误消息与带值结果
└── engine.h                   Engine 模板：图编辑、校验、准备、执行与生命周期
plugins/
├── adapter.h / adapter.cpp    PCM 输入和输出适配
└── virtual.h                  正弦波发端与 PCM 校验收端
tests/
├── nodes/                     缓冲、增益和混音测试节点
└── nodes.h                    汇总测试节点头文件
```

`core/types.h` 集中定义调用方配置音频和建立连接所用的类型。`core/graph.h` 提供 `Node` 和 `NodeT`。`aereborn::core` 是纯头文件的 CMake 接口目标，传递头文件路径和 C++14 要求。`aereborn::plugins` 提供 PCM 适配和虚拟收发节点，依赖核心接口。
Engine 直接拥有 Node、Connection 和图缓冲池。`prepare()` 完成依赖排序、预分配缓冲和端口绑定；执行计划只保存排好顺序的节点记录指针，块视图和连接区间直接保存在节点记录中，`process()` 直接遍历这些记录。
固定端口数量通过 `NodeT<I, O, Rate, Period>` 确定，端口声道数和块视图使用 `std::array`。采样数据集中在图缓冲池，节点执行顺序和连接索引使用可复用的连续数组。
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
引擎周期是运行时的基础节拍。设备每次收发覆盖整数个引擎周期。设备驱动按基础周期计数，在各设备对应的边界安排读写。

| 场景 | ClockDriver |
| --- | --- |
| 主设备周期等于引擎周期 | 该设备的音频回调或设备 I/O 线程可提供基础节拍 |
| 设备周期覆盖多个引擎周期 | 由基础周期驱动器推进图，设备在各自的周期边界收发 |
| 主设备掉线、纯网络或无物理设备 | 独立的 TimerDriver，保持逻辑时间推进 |
| 离线验证或导出 | 调用方直接推进，不等待真实时间 |

主设备由显式配置或确定的选择策略指定，可以是采集或播放设备，不把某一方向写死为主时钟。
主设备能提供基础节拍时，可以直接在其回调或设备线程执行渲染。
主驱动切换须先确认旧驱动退出，再接管执行权；帧序号继续递增，并报告时钟切换或数据不连续。

内部统一使用 planar float32。`Engine<Rate, Period>` 显式接收两个枚举模板参数，同一引擎内的节点采用相同模板参数，端口声道数可以不同。PCM 适配器模板的默认规格为 `SampleRate::Hz48000` 和 `BlockPeriod::Ms10`，例如 `IAdapter<>`。
`SampleRate` 提供 8000、11025、16000、22050、24000、32000、44100、48000、88200、96000 Hz，最高 96 kHz；`BlockPeriod` 提供 `Ms05`、`Ms10`、`Ms20`、`Ms40`。
每周期帧数精确计算为 `采样率 × 周期毫秒 / 1000`，由内部 constexpr 函数统一计算；不能组成整数帧的组合在实例化时编译失败，不进行四舍五入。例如 44.1 kHz/5 ms 不允许，44.1 kHz/10 ms 为 441 帧。11.025 kHz 仅允许 40 ms，22.05 kHz 允许 20/40 ms，44.1 kHz 允许 10/20/40 ms，其余列出的采样率允许四种周期。
`block_frames()` 可用于常量表达式；不再提供配置结构体、运行时配置构造参数或 `config()`。节点 `prepare()` 直接使用模板规格，`ProcContext` 仅传递帧位置和输入输出块视图，不重复携带采样率与帧数。
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
以下连接虚拟发端和收端。带总线和逐样本校验的测试见 [tests/virtual_routing.cpp](tests/virtual_routing.cpp)。

```cpp
#include "core/engine.h"
#include "plugins/virtual.h"

aereborn::RetCode render_one_block()
{
    using namespace aereborn;
    constexpr auto rate = SampleRate::Hz48000;
    constexpr auto period = BlockPeriod::Ms05;
    Engine<rate, period> engine;

    const auto source = engine.emplace_node<VirtualSource<rate, period>>(2, 440.0, 0.25f, PCMFormat::S16LE);
    if (!source)
    {
        return source.ret_code();
    }
    const auto sink = engine.emplace_node<VirtualSink<rate, period>>(2, PCMFormat::S16LE);
    if (!sink)
    {
        return sink.ret_code();
    }
    const auto connection = engine.connect({source.value(), 0}, {sink.value(), 0});
    if (!connection)
    {
        return connection.ret_code();
    }
    const auto status = engine.prepare();
    if (!status)
    {
        return status;
    }
    return engine.process(); // 一周期 240 帧。
}
```

调用由应用同步串行。拓扑改变后调用 `prepare()`，连接增益和静音在下一块生效。应用可查询引擎每周期帧数和当前帧位置。Engine 直接持有节点、连接、缓冲池与执行计划。
后续接入后台执行时，再增加异步图提交和块边界切换，区分编辑失败、计划已应用与端点已就绪，不改变单一 Node 模型。
第一版不提供外部插件 ABI，不依赖 C++17/20 特性。

### 错误处理

可能失败的操作返回 `RetCode`，同时返回节点指针、句柄或矩阵时用 `Result<T>`。两者都可用 `if (!result)` 判断失败。`Result<T>::ret_code()` 返回 `RetCode`，调用方按 `code()` 分支，可在控制侧用 `message()` 读取固定错误文本。检查成功后才调用 `value()`。

引擎在加入节点时检查端口声道数。节点在 `prepare()` 校验自身配置并准备处理资源，失败时可以重试，已准备成功的节点保留状态。`Node::process()` 是 `void noexcept`，引擎未准备好时 `Engine::process()` 返回 `NotPrepared`。

需要分配的接口将分配失败映射为 `NoMemory`，容量超限返回 `TooLarge`。自定义节点构造或准备期间抛出的其他异常转换为 `NodeFailed`。低层块视图的索引和跨度由调用者保证。

### 当前原型的运行约定

- `prepare()` 从所有无输出端口的 Sink 反向收集依赖。节点首次进入执行计划时准备资源，并在计划中按依赖顺序处理。
- 拓扑改变后 `process()` 返回 `NotPrepared`，直到重新准备成功。已准备的节点保留暂存音频和 DSP 状态。
- 连接的增益和静音参数同步修改，下一块生效。
- 自定义节点通过 `context.inputs[i]`、`context.outputs[i]` 访问块，通过 `.size()` 查询端口数量；引擎清零输入，节点必须写满本次处理跨度内的全部输出。`ProcContext` 提供块起始帧位置；节点算法直接使用对应模板规格的固定帧数。

### 外部 PCM 输入和输出

`plugins/adapter.h` 提供两个独立的节点 `IAdapter<Rate, Period>` 和 `OAdapter<Rate, Period>`，都接收 `core/types.h` 中的 `PCMSpec{sample_rate, format, channels, periods}`。`periods` 是设备周期覆盖的引擎周期数，默认为 1。外部使用交错 PCM，声道数与节点端口一致。格式包括 `S16LE`、`S24_3LE`、`S24LE`、`S32LE` 和 `F32LE`。`pcm_sample_bits()` 查询样本位深，`pcm_sample_bytes()` 查询存储字节数。`S24_3LE` 每样本占三字节，`S24LE` 每样本占四字节，低 24 位有效，写出的高字节为零。

- `pcm_frames()` 在准备后返回一个设备周期的帧数。设备采样率限于 `SampleRate` 列出的值，周期配置必须能得到整数 PCM 帧数。以 48 kHz / 5 ms 的图连接 44.1 kHz / 10 ms 设备为例，设备周期覆盖两次引擎推进，一块 441 帧的 PCM 在图内按 221 帧和 220 帧处理。
- 调用方在各设备周期开始前用 `bind_pcm()` 绑定对应的整块缓冲，并保证它在接下来 `periods` 次引擎推进期间有效。适配器逐周期读写对应片段，设备周期结束后自动解除绑定，驱动可提交输出。上一块仍在使用时重新绑定返回 `BufferFull`。缺少输入时图得到静音，缺少输出缓冲时图继续推进。
- 两个适配器公开继承 `NodeT`，私有继承 `PCMConverter`。适配器管理设备缓冲绑定和周期计数，转换器计算每个引擎周期对应的 PCM 帧。引擎固定使用按声道存放的 IEEE float32。`F32LE` 保持采样的位表示，小端单声道可以整块复制，多声道按布局重排。整数 PCM 在设备边界做归一化或量化。异采样率时，输入先读入 float32 再重采样，输出先重采样再写出。转换器维护相位和滤波历史，滤波延迟为 48 个源端帧。`reset()` 解除绑定并清除相位和滤波历史，供设备流中断或重新开始时调用。

### 虚拟收发节点

`plugins/virtual.h` 提供用于测试的 `VirtualSource` 和 `VirtualSink`。发端按引擎帧位置生成配置频率和振幅的正弦波，再按指定 PCM 格式量化。收端将收到的音频编码为指定格式。两端的 `pcm()` 提供一个引擎周期的交错 PCM，下一块会覆盖其中的数据。

`VirtualSink::matches(expected, tolerance)` 在处理后调用。它将最近收到的 PCM 解码后，与同声道数、同帧数的参照块逐样本比较，`tolerance` 是归一化采样的绝对误差。测试可根据已知正弦波、连接增益和声道矩阵独立计算混音参照。

构建与验证（Visual Studio 多配置生成器）：

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

使用 PCM 适配器或虚拟收发节点的应用链接 `aereborn::plugins`。自定义节点可以只链接 `aereborn::core`，实现可参考 [tests/core_tests.cpp](tests/core_tests.cpp) 中的 `CopyNode`。
`aereborn_core_tests` 验证自定义节点、多路混音和 ID 唯一性。`aereborn_tests` 验证块视图、声道矩阵、图的准备与执行及错误处理。`aereborn_io_adapter_tests` 验证 PCM 转换、设备周期和重采样。`virtual_routing` 校验五种 PCM 格式和总线的混音、分发、静音、增益及重连。

## 8. 实现顺序

1. **离线图核心**：Node、Port、Connection、音频块及 Engine 的准备与执行。用插件节点验证多对多、处理器串联、总线互连、输出分发、环路拒绝。
2. **运行内核**：ClockDriver、块边界计划切换、参数更新、有界队列、Observer、停止屏障。验证调音量不重建图，修改连接不重置未变节点的状态。
3. **一个平台的设备闭环**：主设备驱动、多设备边界适配、DeviceSession、热插拔与恢复。验证同一张图只被一个驱动执行，拔插不破坏其他连接；再接另一个平台。
4. **文件、推入和混音策略**：预读/录制、PlaybackHandle、PushSource、DSP 链、ducking。验证多实例、循环、过载报告和增益包络。
5. **网络**：先 PCM，再 Opus、乱序/抖动缓冲与丢包恢复；验证本地与远程源进入同一 Bus，断网后有界退化并恢复。

第一步先写少量核心类型和离线执行，不接系统设备，不启动 Asio 线程。每一步复用同一张图与同一套节点执行机制。
