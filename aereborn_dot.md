# AEReborn 诊断实现核对与 DOT 可视化重构计划

本文档分两部分：一是核对当前已落地的诊断实现与原设计方案的一致性并收口 2 处边界隐患；二是对 `diag/inspect.cpp` 的 Graphviz DOT 输出进行彻底重构，淘汰晦涩的几何符号与扁平文本框，改为面向排障的 **Patchbay 卡片式可视化方案**。

---

## 1. 已落地实现核对与边界收口

对照原计划 `AudioEngine_Diagnostics_Plan.md`，当前 `core/`、`plugins/`、`backend/` 与 `diag/` 的整体架构完全符合“无冗余 Snapshot 结构体、仅查生效计划、后端方向由 `type_name()` 表达、零热路径开销”的原则，且多处落地细节更为严谨：

| 模块 | 超出原计划的优秀落地细节 |
| --- | --- |
| `core/fring.h`、`plugins/endpoint.h` | 引入 `FrameRing::estimate(std::size_t&)` 与 `estimate_queued_frames()`，自动过滤并发采样双游标时偶发的越界撕裂值。 |
| `core/engine.h/.cpp` | 更名为语义更精确的 `missed_periods()` 与 `uint64_t last_tick_us()`；`state()`、`plan_generation()` 统一补充 `require_control_thread()` 守卫。 |
| `Graph::resolve()` | 保持 `DraftNode::inputs` 结构不变，直接按 `node_index` 过滤生效连接，改动面更小。 |
| `backend/backend.h/.cpp` | `ASRC::correction` 直接原子化保持单一事实来源；`Backend::type_name()` 提供默认实现，降低测试桩负担。 |
| `diag/inspect.cpp` | 全部 `std::ostringstream` 显式设置 `std::locale::classic()`，避免受系统 locale 影响小数格式。 |

### 需收口的 2 处边界隐患

#### 隐患 1：`Capture::close()` / `Playback::close()` 下线后未复位 `primed_`

* **现状**：`Capture::close()` 和 `Playback::close()` 仅关闭了 `graph_open` 并清空 `ring_`，未将 `primed_` 重置为 `false`。后端 `stop()` 后，`online()` 返回 `false`，但直接查询 `endpoint.steady()` 仍返回 `true`（`diag/inspect.cpp` 目前被迫写为 `online && endpoint.steady()` 兜底）。
* **修复方案**：在 `Capture::close()` 和 `Playback::close()` 清空 `ring_` 时同步复位 `primed_.store(false, std::memory_order_release);`。
* **关键约束**：**严禁在 `close_graph_gate()` 或 `Playback::close_graph()` 中复位 `primed_`**。同步播放停机排空（`ActiveBackend::stop()` $\to$ `close_playback_graph()` $\to$ `drain_playback()`）期间，I/O 线程仍需调用 `Playback::read()` 排空剩余帧；若提前清零 `primed_`，队列低于 50% 水位时会被预填充门控重新拦截导致排空失败。

#### 隐患 2：`diag/inspect.cpp` 中 `Convert` 标签遗漏格式转换

* **现状**：当前 DOT 生成使用 `if (input.sample_rate != output.sample_rate) ... else if (input.format != output.format) ...`，当 `Convert` 同时改变采样率和样本格式时（如 `48k F32P` 转 `16k S16LE`），`else if` 会静默丢弃格式转换信息。
* **修复方案**：并入本轮 DOT 重构，同时比对 `sample_rate` 与 `format` 并按实际变化完整展示。

---

## 2. 当前 DOT 图的核心痛点

1. **符号表意不明且易乱码**：使用 `●`、`◐`、`○` 表示在线/预填充/离线，`▰▱` 拼装进度条，`Σ`、`↔`、`×` 修饰插件，`!` 混用表示 XRUN 与后端故障。既需要对照图例猜测含义，又容易在常规无衬线字体下出现字宽错位或回退乱码。
2. **关键排障信息隐藏在 `tooltip`**：静态导出（PNG/PDF）或快速浏览时无法直接看到连线的音频规格、端口号、`Mixer` 静音通道、`Backend` 的具体错误码以及 `Engine` 的 `dirty` / `late` 告警。
3. **信息主次倒置**：正常节点堆砌装饰符号，而真正影响“为何无声/卡顿”的核心状态（`Mixer` 静音路、`Matrix` 声道映射与零增益、`Backend` 的 `Draining`/`Stopped` 与错误码）在主图缺失。
4. **扁平文本框缺结构层次**：普通 `shape=box` 靠换行堆叠文本，多总线、多分支场景下难以分辨端口映射与硬件边界。

---

## 3. 新版 DOT 可视化设计：Patchbay 卡片式架构

保持 `diag/inspect.h` 公开 API 与 Graphviz 工具链完全不变，在 `diag/inspect.cpp` 内部改用 Graphviz 原生 **HTML-Like Table 卡片（`shape=plain`）**，通过以下四项规则实现“一眼定位拓扑与故障”：

### 3.1 核心表达规则

1. **零神秘符号，统一使用短英文状态词 + 语义色板**
   * 不再需要顶部符号图例，文字自解释：
     * 引擎状态：`UNPREPARED`、`READY`、`RUNNING`
     * 端点状态：`ONLINE`、`PREFILL`、`OFFLINE`
     * 后端状态：`RUN`、`DRAIN`、`STOPPING`、`STOP`、`ERR: <ErrorCode>`（使用短枚举名，如 `ERR: DeviceLost`、`ERR: Timeout`、`ERR: EndOfStream`）
2. **正常静默、异常高亮**
   * `dirty == false`、`missed_periods == 0`、`underrun == 0`、`overflow == 0` 时不占用卡片版面。
   * 一旦出现异常（未提交草稿、时钟逾期、队列 XRUN、后端报错），立即弹出高对比度琥珀/红色徽章或告警行。
3. **显式端口插槽与静音连线可视化**
   * `Mixer` 及任意多端口节点（`inputs.size() > 1 || outputs.size() > 1`）在卡片内逐行绘制左入右出端口插槽（`PORT="i0"`、`PORT="o0"`），连线经 `:e` / `:w` 锚点精确对接。
   * `Mixer` 已静音的输入槽标注 `[MUTE]` 并置灰；连向该输入槽的边自动渲染为**灰色虚线**并附 `(muted)`，阻断点一目了然。
4. **主图信息凝练，完整细节保留在 `tooltip`**
   * 节点卡片与边标签直接展示最关键的拓扑与运行事实；完整的端口明细、未截断的 `RetCode::message()` 及 Windows WASAPI 原生 `HRESULT` 仍保留在 SVG `tooltip` 中供悬停深查。

### 3.2 语义色板规范

| 角色 / 状态 | 标题栏底色 | 边框色 | 标题 / 徽章文字色 | 适用场景 |
| --- | --- | --- | --- | --- |
| **端点（正常）** | `#DFF4F0` | `#2E8B7D` | `#0F172A` / `#15803D` | `Capture`、`Playback` 稳态在线 |
| **图内 DSP 节点** | `#E8EEF7` | `#4A6FA5` | `#0F172A` | `Mixer`、`Matrix`、`Convert` 及自定义节点 |
| **外部后端（正常）** | `#F1F5F9` | `#64748B` | `#0F172A` / `#15803D` | `Alsa*`、`Wasapi*`、`Wav*`、`Null*`、`PushSource`、`PullSink`、`UsrPlayback` |
| **告警 / 过渡态** | `#FEF3C7` | `#D97706` | `#92400E` / `#B45309` | `PREFILL`、`DRAIN`、`STOPPING`、`DIRTY`、`LATE`、存在 XRUN |
| **错误态** | `#FEE2E2` | `#DC2626` | `#991B1B` / `#DC2626` | `!backend.last_error()` |
| **静默 / 离线态** | `#F3F4F6` | `#9CA3AF` | `#64748B` / `#9CA3AF` | `OFFLINE`、`STOP`、`[MUTE]` 槽位、静音入边、`Detached` 后端 |

### 3.3 各类元素的卡片与连线规范

#### (1) 顶部引擎状态横幅（Graph Label）

单行无边框表格，紧凑呈现核心遥测：

* 固定项：`AEReborn` + 引擎状态徽章（`RUNNING` 绿 / `READY` 蓝 / `UNPREPARED` 灰）+ `period | Plan #gen | tick N | M us`
* 按需告警徽章：
  * `engine.dirty() == true` 时追加琥珀徽章 `DIRTY`
  * `engine.missed_periods() > 0` 时追加红/琥珀徽章 `LATE <count>`

#### (2) `Capture` / `Playback` 端点卡片

* 边框采用 `<TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED">` + `<HR/>` 分隔标题栏与正文（兼顾圆角轮廓与清晰分隔线）。
* 标题栏：`Capture #id`（或 `Playback #id`）+ 状态徽章：
  * `online() && steady()` $\to$ `ONLINE`（绿）
  * `online() && !steady()` $\to$ `PREFILL`（琥珀）
  * `!online()` $\to$ `OFFLINE`（灰）
* 正文行：
  * 队列水位：`Queue 960/1920 (50%)`（当 `estimate_queued_frames()` 不可用时显示 `Queue -/1920`）。
  * 异常行（仅在 `lost > 0` 时显示）：`Underrun: <N>f` 或 `Overflow: <N>f`（琥珀加粗，同时卡片边框与标题栏切至告警色）。

#### (3) `Mixer` 与多端口节点卡片

* 只要节点满足 `inputs.size() > 1 || outputs.size() > 1`，使用两列端口表（标题栏 `COLSPAN="2"`，下方用 `<HR/>` 分隔，左右列用 `<VR/>` 分隔）：
  * 左列逐行渲染 `in0`、`in1`…（`PORT="i0"`、`PORT="i1"`…）；若为 `Mixer` 且 `muted(i) == true`，该单元格底色 `#F3F4F6`、字色 `#9CA3AF`，文本为 `in<i> [MUTE]`。
  * 右列逐行渲染 `out0`…（`PORT="o0"`…）；当左右端口数不等（如 `Mixer` 为 $N$ 入 1 出）时，较少一侧最后一行用 `ROWSPAN` 纵向合并对齐。

#### (4) `Matrix`、`Convert` 与单端口节点卡片

* **`Matrix`**：
  * 声道数变化时显示 `<in_ch>ch -> <out_ch>ch | `，否则省略声道映射前缀。
  * 增益显示 `Gain 0.50`；当 `gain() == 0.0f` 时显示 `Gain 0.00 [MUTE]`（标题与边框可保持 DSP 色，文字标灰提示零输出）。
* **`Convert`**：
  * 同时检查 `sample_rate` 与 `format`：
    * 两者均变化：`48k F32P -> 16k S16LE`
    * 仅采样率变化：`48k -> 44.1k`
    * 仅格式变化：`F32P -> S16LE`
    * 规格相同：`passthrough`
* **其他单端口自定义节点（如 `Tone`、`Meter` 或算法节点）**：
  * 标题栏显示 `type_name() #id`，正文按端口规格显示紧凑概要（如 `48k 1ch F32P`）。

#### (5) `Backend` 外部驱动卡片与 `Detached` 分组

* 标题栏：`backend.type_name()` + 状态徽章：
  * `!error` $\to$ `ERR: <ShortErrorCode>`（红，卡片整体切换错误色）
  * `ActiveBackend`: `Running` $\to$ `RUN`、`Draining` $\to$ `DRAIN`（琥珀）、`Stopping` $\to$ `STOPPING`（琥珀）、`Stopped` $\to$ `STOP`（灰）
  * `PassiveBackend`: `running()` $\to$ `RUN` / `STOP`
* 正文两行：
  * 第 1 行（已绑定时）：设备原生规格 + 批次上限，例如 `48k 2ch S32LE (max 240f)`；未绑定时显示 `unbound`。
  * 第 2 行：时钟与处理摘要：
    * `ActiveBackend` 异步：`async · FIR · drift +12.4 ppm`（仅在 `uses_resampler()` 时含 `FIR`，异步始终含 `drift`）
    * `ActiveBackend` 同步：`sync`（若 `uses_resampler()` 则 `sync · FIR`）
    * `PassiveBackend`：`passive`（若 `uses_resampler()` 则 `passive · FIR`）
* **Detached Backends**：未关联到任何生效 Plan 节点的后端归入 `subgraph cluster_detached`，虚线灰框标注 `Detached Backends`。

#### (6) 连线（Edge）规范

* **图内连接（`plan_connections()`）**：
  * 锚点：若源/目标节点为多端口卡片，分别使用 `n<from>:o<idx>:e` 和 `n<to>:i<idx>:w`；单端口卡片使用 `n<from>:e` 和 `n<to>:w`。
  * 标签：直接显示紧凑格式（如 `48k 2ch F32P`），字体 9pt 浅灰 `#64748B`。
  * 静音检测：若目标节点为 `Mixer` 且 `mixer->muted(conn.to.index)`，边样式设为 `style=dashed, color="#9CA3AF", fontcolor="#9CA3AF"`，标签追加 `(muted)`。
* **后端与端点边界连线**：
  * 方向：`Capture` 为 `b<id>:e -> n<id>:w`，`Playback` 为 `n<id>:e -> b<id>:w`。
  * 样式：虚线 `style=dashed`；若 `!backend.last_error()` 线条标红 `#DC2626`，否则 `#64748B`。
  * 标签：按边界处理模式简洁标注——异步标 `ASRC`，同步/被动带重采样标 `FIR`，无重采样标 `PCM`。

---

## 4. 完整 DOT 输出示例

以下为典型多路总线场景（1 路 ALSA 正常采集 + 1 路 WAV 采集 EOF 欠载且在 Mixer 被静音 + Matrix 声道缩混 + Convert 采样率与格式双转 + WASAPI 异步播放）的目标 DOT 输出，可直接通过 `dot -Tsvg` 渲染验证：

~~~dot
digraph AEReborn {
  graph [
    rankdir=LR, splines=true, bgcolor="#FFFFFF",
    pad=0.32, nodesep=0.42, ranksep=0.72, labelloc=t,
    fontname="Helvetica,Arial,sans-serif",
    label=<
      <TABLE BORDER="0" CELLBORDER="0" CELLSPACING="2" CELLPADDING="4">
        <TR>
          <TD><B><FONT POINT-SIZE="15" COLOR="#0F172A">AEReborn</FONT></B></TD>
          <TD BGCOLOR="#DCFCE7"><B><FONT POINT-SIZE="10" COLOR="#166534"> RUNNING </FONT></B></TD>
          <TD><FONT POINT-SIZE="10" COLOR="#475569">5ms | Plan #3 | tick 12480 | 138 us</FONT></TD>
          <TD BGCOLOR="#FEF3C7"><B><FONT POINT-SIZE="10" COLOR="#B45309"> DIRTY </FONT></B></TD>
        </TR>
      </TABLE>
    >
  ];
  node [shape=plain, fontname="Helvetica,Arial,sans-serif"];
  edge [fontname="Helvetica,Arial,sans-serif", fontsize=9, fontcolor="#64748B", color="#475569", arrowsize=0.7, penwidth=1.4];

  b1 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#64748B" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#F1F5F9" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">AlsaCapture</FONT></B>  <FONT POINT-SIZE="9" COLOR="#15803D"><B>RUN</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">48k 2ch S32LE (max 240f)<BR ALIGN="LEFT"/>async | FIR | drift +12.4 ppm</FONT></TD></TR>
    </TABLE>
  >];

  n1 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#2E8B7D" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#DFF4F0" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Capture</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#1</FONT>  <FONT POINT-SIZE="9" COLOR="#15803D"><B>ONLINE</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">Queue 960/1920 (50%)</FONT></TD></TR>
    </TABLE>
  >];

  b2 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#DC2626" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#FEE2E2" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#991B1B">WavCapture</FONT></B>  <FONT POINT-SIZE="9" COLOR="#DC2626"><B>ERR: EndOfStream</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#7F1D1D">44.1k 2ch S16LE (max 220f)<BR ALIGN="LEFT"/>sync | FIR</FONT></TD></TR>
    </TABLE>
  >];

  n2 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#D97706" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#FEF3C7" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Capture</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#2</FONT>  <FONT POINT-SIZE="9" COLOR="#64748B"><B>OFFLINE</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">Queue 0/1920 (0%)<BR ALIGN="LEFT"/></FONT><FONT POINT-SIZE="9" COLOR="#B45309"><B>Underrun: 480f</B></FONT></TD></TR>
    </TABLE>
  >];

  n3 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="4" STYLE="ROUNDED" COLOR="#4A6FA5" BGCOLOR="#FFFFFF">
      <TR><TD COLSPAN="2" BGCOLOR="#E8EEF7" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Mixer</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#3</FONT></TD></TR>
      <HR/>
      <TR>
        <TD PORT="i0" ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">in0</FONT></TD>
        <VR/>
        <TD PORT="o0" ROWSPAN="2" ALIGN="RIGHT"><FONT POINT-SIZE="9" COLOR="#334155">out0</FONT></TD>
      </TR>
      <TR>
        <TD PORT="i1" BGCOLOR="#F3F4F6" ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#9CA3AF">in1 [MUTE]</FONT></TD>
      </TR>
    </TABLE>
  >];

  n4 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#4A6FA5" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#E8EEF7" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Matrix</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#4</FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">2ch -&gt; 1ch | Gain 0.50</FONT></TD></TR>
    </TABLE>
  >];

  n5 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#4A6FA5" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#E8EEF7" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Convert</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#5</FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">48k F32P -&gt; 16k S16LE</FONT></TD></TR>
    </TABLE>
  >];

  n6 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#2E8B7D" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#DFF4F0" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">Playback</FONT></B> <FONT POINT-SIZE="9" COLOR="#64748B">#6</FONT>  <FONT POINT-SIZE="9" COLOR="#15803D"><B>ONLINE</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">Queue 952/1920 (50%)</FONT></TD></TR>
    </TABLE>
  >];

  b6 [label=<
    <TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="5" STYLE="ROUNDED" COLOR="#64748B" BGCOLOR="#FFFFFF">
      <TR><TD BGCOLOR="#F1F5F9" ALIGN="LEFT"><B><FONT POINT-SIZE="11" COLOR="#0F172A">WasapiPlayback</FONT></B>  <FONT POINT-SIZE="9" COLOR="#15803D"><B>RUN</B></FONT></TD></TR>
      <HR/>
      <TR><TD ALIGN="LEFT"><FONT POINT-SIZE="9" COLOR="#334155">48k 2ch F32LE (max 480f)<BR ALIGN="LEFT"/>async | FIR | drift -8.1 ppm</FONT></TD></TR>
    </TABLE>
  >];

  b1:e -> n1:w [style=dashed, color="#64748B", label=" ASRC "];
  b2:e -> n2:w [style=dashed, color="#DC2626", label=" FIR "];
  n1:e -> n3:i0:w [label=" 48k 2ch F32P "];
  n2:e -> n3:i1:w [style=dashed, color="#9CA3AF", fontcolor="#9CA3AF", label=" 48k 2ch F32P (muted) "];
  n3:o0:e -> n4:w [label=" 48k 2ch F32P "];
  n4:e -> n5:w [label=" 48k 1ch F32P "];
  n3:o0:e -> n6:w [label=" 48k 2ch F32P "];
  n6:e -> b6:w [style=dashed, color="#64748B", label=" ASRC "];
}
~~~

---

## 5. 逐文件改造清单与实现细节

### 5.1 `plugins/endpoint.cpp`（2 行修复）

* 在 `Capture::close()` 和 `Playback::close()` 中，于 `ring_.clear()` 旁补充 `primed_.store(false, std::memory_order_release);`。
* 保持 `close_graph_gate()` 与 `Playback::close_graph()` 不变，确保同步播放排空流程不受影响。

### 5.2 `diag/inspect.cpp`（集中重构 `dump_dot()`）

`diag/inspect.h` 与 `dump_text()` 均无需改动。`diag/inspect.cpp` 内部调整如下：

1. **移除旧 Unicode 常量**：删除 `online_symbol`、`prefill_symbol`、`offline_symbol`、`▰`/`▱`、`Σ`/`↔`/`×` 等符号拼接逻辑。
2. **新增 3 个纯辅助函数（匿名命名空间）**：
   * `write_html_escaped(std::ostream &, const std::string &)`：将 `&`、`<`、`>`、`"` 分别转义为 `&amp;`、`&lt;`、`&gt;`、`&quot;`，确保自定义节点名或平台错误串不会破坏 HTML-Like Label 语法。
   * `error_code_name(ErrorCode)`：覆盖 `core/retcode.h` 全部 18 个枚举值，返回紧凑短名（如 `"DeviceLost"`、`"EndOfStream"`、`"Timeout"`），用于后端卡片红标；`tooltip` 继续输出完整 `RetCode::message()` 与 WASAPI `HRESULT`。
   * `write_short_rate(std::ostream &, SampleRate)`：纯整数格式化采样率（`48000` $\to$ `48k`，`44100` $\to$ `44.1k`，`11025` $\to$ `11.025k`），不修改 `std::ostream` 的浮点格式标志位。
3. **重写卡片与边生成函数**：
   * `is_multiport_node(const Node &)`：判断 `inputs().size() > 1 || outputs().size() > 1`，统一驱动多端口插槽渲染与连线端口锚点（`:o<idx>:e` / `:i<idx>:w`）。
   * `write_dot_endpoint(...)`：渲染 `Capture`/`Playback` 卡片，含 `ONLINE`/`PREFILL`/`OFFLINE` 徽章、`Queue frames/cap (pct%)` 与按需 XRUN 告警行。
   * `write_dot_node(...)`：分发端点卡片、多端口插槽卡片（`Mixer` 标记 `[MUTE]`）、以及 `Matrix`（声道变化 + `Gain` / `[MUTE]`）、`Convert`（同时比较 `sample_rate` 与 `format`）、其他单端口节点卡片。
   * `write_dot_backend(...)`：渲染外部驱动卡片，区分 `RUN` / `DRAIN` / `STOPPING` / `STOP` / `ERR: <ShortCode>`，展示设备规格、批次上限、`async`/`sync`/`passive`、`FIR` 与 `drift`。
   * 边循环：为图内连线标注紧凑规格、识别 `Mixer` 静音输入并灰虚化连线；为后端↔端点虚线标注 `ASRC` / `FIR` / `PCM`。
   * 全部节点、后端、连线及根图仍保留 `tooltip` 输出完整诊断信息。

### 5.3 文档与测试同步

| 文件 | 同步内容 |
| --- | --- |
| `AudioEngine_Outline.md` | 更新第 7 节“运行态诊断”：删去 `●、◐、○`、条形刻度和 `!` 的旧描述，改为文字状态徽章、端口插槽与静音连线说明。 |
| `tests/`（`io.endpoint_diagnostics`） | 补充验证：后端 `stop()` 后 `Capture::steady()` 与 `Playback::steady()` 归回 `false`；同时确认同步播放停机排空（`WavPlayback` / `UsrPlayback`）回归无破坏。 |
| `tests/`（`io.diagnostic_reports`） | 更新 DOT 断言：校验 HTML 卡片与文字状态（`RUNNING`/`ONLINE`/`OFFLINE`/`ERR: ...`）、`Mixer` 端口插槽与静音虚线、`Convert` 同变采样率与格式输出、`Matrix` 声道映射与增益、`DIRTY` 徽章以及 `Detached` 分组。 |

---

## 6. 验收清单

1. **视觉与语法正确性**：将测试生成的 DOT 输出经 `dot -Tsvg` 渲染，零语法告警；图内无豆腐块/错位 Unicode 符号，节点层次、端口对接、静音支路、错误高亮与孤立后端一眼可辨。
2. **信息完备性**：
   * `Convert(48k F32P -> 16k S16LE)` 同时完整展示采样率与格式变化。
   * `Mixer` 静音输入在卡片槽位与对应入边上均有清晰灰显标记。
   * 后端报错卡片直接显示具体短错误码（如 `ERR: DeviceLost` / `ERR: EndOfStream`），而非单个 `!`。
3. **转义安全性**：自定义节点 `type_name()` 返回含 `<`、`>`、`&`、`"` 的字符串时，生成的 DOT 仍为合法语法。
4. **生命周期与并发回归**：`Capture::close()` / `Playback::close()` 复位 `primed_` 后，`steady()` 语义自洽，且同步播放排空（含队列剩余帧小于 50% 水位时）全部通过；Debug 与 Release 全量 CTest（含 `regression` 零堆分配检查）全绿。