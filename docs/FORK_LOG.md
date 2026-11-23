# 引擎分叉日志（Strata）

本仓库是 [Niko1221/Strata](https://github.com/Niko1221/Strata) 的下游分叉，用于 Tectonic 的层切分分布式执行。本文件记录**我们改了什么、为什么改、能否回到上游**——不是调查过程的逐步记录；已被后续条目推翻或取代的中间假设、具体调试命令、来回论证，不长期保留，只留最终结论和指向具体 commit/文件的指针。

## 起源与归属（2026-11-17）

`main` 从当时的 `origin/main`（`c7757280`）重新开始，把早期可行性验证分支 `l0-stage-split`（分叉点 `504b1433`，含加了又删的 ring 代码）相对分叉点的**净 diff**（不是逐提交重放）应用上去，排除一次性 DSV41 实验和旧版本的这份日志。旧分支归档为 `archive/l0-stage-split-20261117`，没有删除。

`git diff 504b1433 HEAD` 核实过：`boundary_wire`/`boundary_socket`、`--agent`/`stage_worker`（后改名 `--tectonic`/`stage_worker`）这整块网络/跨进程机制，upstream 完全没有对应概念，100% 是这个 fork 自己写的——改名、重构、整体推翻都不存在"动了 upstream 东西"的顾虑。真正有 rebase 风险的是对 upstream 既有文件（`generate.cpp`、`metal/CMakeLists.txt` 等）的修改量。

## 命名

| 旧名字 | 新名字 | 理由 |
|---|---|---|
| `--agent HOST:PORT` | `--tectonic` + `--worker-data-endpoint HOST:PORT` | 原来一个 flag 兼职"选模式"和"带参数"；`--agent` 本身是 Tectonic 侧已退役的旧术语，且与"AI agent"易混淆 |
| （新增） | `--tectonic` | 纯模式开关（无值，和 `--serve` 同形）。不用"有没有传 endpoint"隐式判断模式，避免漏传时悄悄滚回默认模式 |
| （新增） | `--worker-signal-endpoint HOST:PORT` | 第二条连接，专载 `cancel`/`shutdown`。叫"信号"不叫"控制"，因为 daemon 另有一个 HTTP 管理端口叫 `control` |
| （新增） | `--instance-id ID` | 防止端口残留的上一个进程被误认 |
| `--stage-begin`/`--stage-end` | 删除 | 这两个 flag 在分叉点和当前 upstream 都不存在，是这个 fork 自己引入、仅用于单进程离线调试的死代码；`--layers LO,HI` 是唯一入口，避免"漏传一个边界"在语法上可表达 |
| `--batch N` | 不改 | 已是好名字，作为 `engine.args` 的调优参数传，不进契约必需参数 |
| `--stage-input` | 删除 | 旧的文件回放式离线调试输入，`--tectonic` 网络驱动后失去意义，三处消费代码一并删除，不留死分支 |
| `--max-requests`、`--stage-output` | 删除 | 确认过是 ring 专用死代码 |
| `--max-new`、`--tokens` | 保留 | 通用 `Options` 字段，默认模式和 `--serve` 都要用，不是 `--tectonic` 专属 |

## 设计基线（day 0 定下，此后未变）

- **并发可独立于 `--serve` 实现**：`run_slot_rows`（`include/strata/core/verify.hpp`）设计时已考虑切分场景，`--tectonic` 只需要在 `stage_worker_run` 里新写一个比 `--serve` 的 `batch_step` 小得多的调度层（无 MTP、无自适应分组），不需要重构 `--serve` 共享调度核心。并发数走已有的 `--batch N`，超限时用明确的 `error{code:"batch_full"}` 拒绝，不静默排队。
- **取消检查点**：对齐 `--serve` 的逐 token 粒度（每个产出 token 之后），不是每个 batch window 一次；取消是 fire-and-forget，结果体现在该请求自己的终态（`done`/`error`）里。
- **会话持久化（SAVE/RESTORE）**：`--serve` 的拒绝检查（`--layer-split`/`--batch`/`--peer-device`）针对的是 upstream 自己的单进程多 GPU 切分，`--tectonic` 完全绕开 `--serve` 的命令层，不受影响；真要做引擎侧跨机会话持久化，需新写代码直接调用 `session_file_write`/`session_file_read`，且只能保存"这台机器自己负责的那段 KV"——这是独立于并发改造的工程项，未排期。

## 仍然开放的事项

- [ ] Tectonic 侧 `qwen.go`：确认 `--tectonic` 模式下不再传 `--max-new`/`--tokens`（Tectonic 仓库改动，不在这里）。
- [ ] `kBoundaryEnd` 同时承担"数据帧+完成标记"与"控制广播"，是否拆成 `End`/`Seal`——不做也能运行，清晰度问题，暂不处理。
- [ ] `--serve`（非 split）在 `(split || n >= --short-read)` 条件下同样会走 `prompt_chunk`；2026-11-22 修复的 scratch 清零 bug 理论上同样影响它（见下面"2026-11-22"一节），尚未专门验证，也还没有一个断言"第二个请求开始时 scratch 已清零"的回归测试。

## 2026-11-18：wire v4（`request_id`）与 entry bridge

架构问题——"`gen`/`delta`/`done` 和跨 stage 的 `BoundaryFrame` 残差交换是什么关系"——确认为**方案 1**：`gen`/`delta`/`done` 只是 entry stage 自己的请求生命周期外壳，stage 间残差交换继续走 `BoundaryFrame` 不变（完整记录见 Tectonic 仓库 `docs/decisions/0005-wire-v4-request-id.md`）。

做了什么：`BoundaryFrame` 加 `uint64_t request_id`（`kBoundaryVersion` 3→4），任何 stage 转发时原样带过去；`stage_worker_run` 的单一共享状态换成按 `request_id` 查的 map；新增 `include/strata/core/control_wire.{hpp,cpp}`（`gen`/`delta`/`done`/`error` 的 JSON 读写，字段对应 Tectonic Go 侧 `internal/engine/protocol.go`）；新增 `include/strata/core/entry_bridge.hpp` + `.cpp`（`EntryBridge` 类，维护 Tectonic 字符串 `request_id` ↔ 本进程 wire id 的映射、准入上限、`max_new` 计数，只在 entry stage 启用）。

当时记录的三个已知缺口，**均已在后续条目解决**：并发准入 `batch_full` 曾因自喂循环优先级问题永远不触发（2026-11-19 的读线程改动一并修好，见下）；EOS 检测未实现（2026-11-20 修好）；真正跨机 split 下的环闭合判据未实现（2026-11-19 续五修好）。

验证：`tools/test_tier1_concurrency.cpp`，真实 TCP 连接对，45 checks；过程中发现并修了一个真 bug（`current_request_id` 只在 `pos0==0` 时更新，导致取消一个请求实际打中了另一个）。

## 2026-11-19：`batch` 个常驻 slot，交替单步计算（ADR 0005 补充决定，"Tier 1.5"）

目标：多个 stage 顺序处理同一请求的 decode 时，一个 stage 等待请求 A 的帧在途的空闲时间，应该能用来算请求 B 的下一步——这件事（"多份独立常驻 session、交替单步、不合批"）被确认可以和 `run_slot_rows` 的 kernel 级合批（Tier 2，ADR 0005 已搁置的部分）分开做：`run_slot_rows` 契约本身保证"未列出的 slot 不受影响"，`--serve` 自己的架构也早就是"prefill 走 solo 路径，之后入队跨 slot 交替 decode"的形状，不是凭空发明。

做了什么：`StageWorkerCompute`/`Reset`/`Prefill` 回调加 `slot` 参数；单一请求表换成 `batch` 个固定槽位 + `request_id → slot` 映射；新增一个专门读数据连接的后台线程，把字节推进队列，和主循环彻底解耦——没有这个，一个常驻 slot 的自喂循环会无限期堵住任何新请求的准入，这是写测试时发现的，不是预判到的。这个改动顺带也修好了 2026-11-18 记录的"`gen` 排队而不是在繁忙时被拒绝"的缺口。

验证：`tools/test_tier1_concurrency.cpp` 新增 Section D，两个真实 `stage_worker_run` 实例、按时间戳证明真实墙钟重叠（连续三次运行 16 个传输窗口里 13-15 个重叠）；64 checks。

## 2026-11-19（续—续三）：接入真实 `Verifier`，根因是 `&ss` 在 `set_of_ss_` 里重复注册

把 Tier 1.5 的回调接到真实 `generate.cpp` 的 `Verifier`/`SessionState`/`Prefill` 后，过程中发现并修了四个真问题（`metal/CMakeLists.txt` 漏列 `entry_bridge.cpp`；`--batch` 的校验从未对 `--tectonic` 放行；`generate.cpp` 从未真正构造 `EntryBridge`；Metal 的 batch slot 是 `Window` 内部独立 carve 的 GPU buffer，和传给 `init_slots` 的 `SessionState*` 指针身份无关，需要显式 `window_slot_copy_main` 才能把内容搬进 slot），单请求验证完全正确（`done.tokens` 字节级匹配 ground truth）。

但**第二个请求（不论是否并发、不论 slot 编号）持续出错**，经过两轮诊断（逐 position 对比显示一个"predict(pos) == token_at(pos-2)"的精确偏移规律；排查并排除了 `hp_`/`ghist_` 双 parity 陈旧、`pooled_` 未清零、K/V 未清零几个假设——过程中确实发现并修了一个真实独立缺口：Metal 的 `reset()` 漏了 `pooled_` 清零，CUDA/HIP 侧有对应的 `cudaMemsetAsync`，但这不是本次 bug 的根因）才找到：

**根因**：`Window::slots_init` 把 solo session 的指针 `&ss` 同时也塞进了 `slot_sessions[0]` 传给 `init_slots`，导致 `set_of_ss_[&ss]` 被覆盖成 `1`——此后 `window_reset(&ss)` 激活的是 slot 0 自己的一套独立缓冲区，不是 solo/live 那一套；`Window::reset()` 确实清零了"当前激活"的那套，只是清错了一套，真正读写的 solo/live 缓冲区从未被清过。第一次永远对（全新进程两套缓冲区都是零），第二次必错（`reset_stage` 清的始终是错的那个 set）——完全解释了之前两轮的全部现象。

**修法**：`ss` 不再出现在传给 `init_slots` 的 slot 列表里，`n_slots` 个逻辑 slot 全部用全新 carve 的 `SessionState`。验证：顺序三会话（A→B→C）、并发交替（`--batch 2`）、单请求（`--batch 1`）全部字节级正确，两套既有假 compute 测试（64+191 checks）不受影响。`pooled_` 清零修复保留（独立正确），`TECTONIC_DEBUG_COMPUTE`/`_BUFFERS`/`_ZERO_KV` 三个调试钩子保留，默认关闭。

## 2026-11-19（续四）：`tectonicd` 真实连通，`--tokens` 豁免补上

第一次让 Tectonic 的 `internal/node.StartWorker` 真正启动、构造完整命令行（此前都是独立测试程序直接驱动）。发现：`generate.cpp` 对 `--tokens` 的硬性检查只对 `o.serve` 做了豁免，`--tectonic` 同样是"prompt 由 `gen` 消息带来、不经过这个 CLI 参数"的情形，但从未被加进豁免——和之前的 `--batch` gate bug 是同一个模式（`--serve` 踩过的坑，`--tectonic` 接入时没有对照补上）。修法：条件改成 `o.serve || o.tectonic`，复用 `--serve` 已验证的占位 token。验证：真实握手、真实 48 层模型加载、真实一轮 chat completion 跑通。

## 2026-11-19（续五）：首次真实两节点部署，发现并修好四个 split 专属问题

架构待决（"`gen`/`delta`/`done` 和 `BoundaryFrame` 的关系"）确认为选项 1：daemon 只做字节透传，entry 自己照旧通过 `BoundaryFrame` 和下一跳交换残差。第一次真实起两个 `tectonicd`（entry [0,24) 带 model-frontend、tail [24,48)）接同一个二进制：

1. **Metal 的 prompt 调度阈值把短 prompt 甩进不支持 split 的回退路径**：`prompt_run_rows` 判断"是否值得走 GEMM 路径"的阈值没把"是不是 split stage"算进去。修法：split stage 无条件走 GEMM 路径。
2. **entry 从未发送 `kBoundaryRequest` 头帧**：tail 累积 prompt 分片、凑够声明行数才真正 prefill 的逻辑早就写好但是死代码，因为没有发送端用过它。修法：`prefill_stage` 先发一帧 `kBoundaryRequest`。
3. **一条只该本机消费的旧信号帧被字节盲转发污染了 entry 自身状态**：`kBoundaryReady` 这条非 mesh 架构下的本机确认帧被 mesh 原样转发回 entry，被误判成"新请求开始"，清空了 entry 自己活跃的 KV。修法：这条帧没有消费者，直接不再发送。
4. **entry 没有识别"下游真实回环"的机制**：`entry_bridge.hpp` 记录过的 scope cut（环闭合判据对 split entry 不成立）从未实现对应判据。修法：用输入帧的 `in.layer_begin > 0`（entry 自己合成的帧不设这个字段）区分本地合成帧与下游回环帧。同时修了两个子问题：entry 正常结束/取消时都需要显式通知下游释放/关闭，否则 tail 的常驻 slot 永久占用、或客户端连接永久挂起。

验证：两节点输出和单节点参考字节级一致；连续请求验证 slot 正确释放；解码中途取消验证客户端收到干净的 `error{code:cancelled}`；两套既有测试（195+64 项）不受影响。`STRATA_DEBUG_STAGE_WIRE`（逐帧 SEND/RECV/DROPPED）新增为常驻调试钩子。

## 2026-11-20：EOS 检测补上

`entry_bridge.hpp` 最后一个 scope cut——只认 `max_new`、不检测 EOS——修好了。`--serve` 早就有 `o.stop_eos`/`o.eos_ids` 机制，2026-11-19 修 `--tokens` 豁免时已经顺带把 `o.stop_eos` 对 `--tectonic` 打开了，真正缺的只是 `stage_worker.hpp` 的两处写回合从未检查这两个值。修法：`EntryBridge` 加只读的 `tokens_so_far`（EOS 本身不计入返回内容），`stage_worker.hpp` 加 `StageWorkerIsEOS` 回调（空值=不检测，向后兼容现有假 compute 测试），`generate.cpp` 一行把已解析的 `o.eos_ids`/`o.stop_eos` 接上。不新增 wire 字段、不新增 CLI flag、Go 侧零改动——`finish_reason` 判断逻辑本就正确，只是没被真正触发过。验证：单节点和两节点 split 对同一请求跑出字节级一致的 `finish_reason:"stop"` 结果。

## 2026-11-21：HIP 构建打通——三个 Metal-only 真问题

PC（HIP 后端协作者）首次尝试在真实 HIP 环境编译 `--tectonic` 路径，依次发现并修好三个"Metal 专属改动漏了 HIP/CUDA 对应处理"的问题：`cmake/strata_gpu_engine.cmake` 同样漏列 `entry_bridge.cpp`（Metal 侧 CMake 当时已经补过，HIP 侧没有）；`compute_stage` 的 Tier 1.5 `!primed` 分支调用 `window_slot_copy_main` 没有套 `#if defined(STRATA_GPU_METAL)`（HIP/CUDA 没有等价机制，这次让非 Metal 分支永远走 solo 路径，配合新增的"非 Metal 下 `--batch > 1` 直接报错"校验，用 `batch=1` 当隔离前提）；`generate.cpp` 里 `stage_chunk = stage_is_split ? 1 : 2048` 错把 Metal 专属限制（`StagePlan::window` 当时的"split 每次最多一行"）当成了通用规则，HIP/CUDA 的 `Prefill` 完全可以一次扛住整个 chunk，这个 `1` 实测让一个 55-token prompt 被拆成 54 次 wire 往返，墙钟 26 秒几乎全耗在网络上；修法同样是 `#if defined(STRATA_GPU_METAL)` 分流。

随后 PC 用同机两个 HIP 进程（entry+tail）真实跑通，结果和 Metal 参考字节级一致；紧接着用真实跨机（Mac Metal entry + PC HIP tail，局域网）跑通两个连续真实请求，第一次验证了异构双机场景。两条运维发现记在这里供下次排障参考：mesh 的"只 accept 一次"设计下，entry 重新拨号要求 tail 人工重启，但 `worker.state` 仍显示 `ready`，看不出失效；不要用端口探测验证 mesh 可达，探测本身会耗掉唯一的 accept 名额。

## 2026-11-21（续三）：显式取消在真实跨机下仍然失败

客户端**显式**调用 `/cancel`（不是断连）时，tail 收到取消帧但没有真正发 `kBoundaryEnd` 释放 slot，下一个请求撞上"no free slot"。根因：`stage_worker.hpp` 的 signal 线程只把 `cancelled` 标志置位，真正收尾的逻辑只在这个请求自己的下一帧到达时才被检查——显式取消后上游不再发任何帧，这个检查永远不会发生。之前"跨机取消成功"的记录纯粹是时序运气（断连时手上恰好还有别的帧在流转）。

修法：给主循环等待下一帧的地方加一个 50ms 超时空闲轮询，检查"被标记取消但不会再等到下一帧"的 slot 并主动收尾（`drain_cancelled_slots`），真实计算路径不受影响。这次改动顺带暴露并修正了一处断言"旧的、有缺陷行为"的既有测试。真实两机复测三条取消路径（prefill 期间取消、decode 中途且上游已停止发帧、客户端直接断连）全部通过。

## 2026-11-22：split-stage 第二次对话污染 bug——发现、排查、根因、修复、提交上游

**T=1 decode 限制解除**：`StagePlan::window()` 此前对任何 split 阶段的 T=1 decode 无条件拒绝（更早期可行性验证遗留，从未因 Tier 1.5 落地重新评估），放开后 `finish_rows()` 也补上了 `finish()` 早就有的 boundary-export 逻辑。这让 split-stage 第一次能跑单次 decode，但紧接着暴露了一个全新 bug：**同一个长驻 tail 进程上，第二个请求（`B`）的输出变成重复乱码或语义连贯但完全无关的文本，100% 确定性复现**（同一二进制连续 5 次完整重启，B 的输出字节级完全相同——不是时序竞态；一次"sleep 修好了"的单次观测经复测证伪，不构成任何方向的证据）。

**排查过程依次排除的候选**（均有真实 Metal 计算/checksum 证据，不是代码审查猜测）：slot 自己的 Metal buffer 残留、solo/live session 的 K/V 残留、`--batch`/`bigs_ok_` 路径本身、Tectonic/model-frontend 请求拼接、A/B 内容是否相同、expert 缓存 hit/miss 两条路径数据不等价（17/17 checksum 一致）、`hold_region`/`region_slots_` 清理缺口（`where_` 全表扫描证明该机制在复现配置下根本未激活）、`cancel_slot` 遗留 `urgent_q_` job（代码同步结构分析证明不可达）、跨层专家权重混用。逐 cell 诊断确认 B 的分叉从**第一个 cell**就开始，不是渐进式污染。曾用 `STRATA_METAL_ARENA_GB=10`（默认约 3.4GB/1728 slot 调到 10GB/5395 slot）验证为可靠缓解方案（3/3 复测正确）——这掩盖了问题，不是修好了问题，但确认了"驱逐/未命中频率"是关键变量。

**根因**：`Window::prompt_gemm_init` 的一次性初始化闸门（`if (pg_) return true`）让 `PromptGemm` 的全部 scratch buffer（`R`/`qfull`/`kcur`/`vcur` 等，几乎整条 prompt 前向路径）在**整个进程生命周期内只分配一次**，被所有后续 prompt 复用。当 arena 太小、`ExpertStream::region()` 退回匿名 mmap 的 `standalone_region()` 时（本次所有复现配置），`region_bytes_` 被硬编码成 `0`（注释"fresh zero pages"只对第一次调用成立，但代码把它当成了永久前提），而 `hold_region(true)` 清零 scratch 的唯一动作 `memset(region_, 0, region_bytes_)` 用的正是这个 0——从第二个请求起，这个 `memset` 永远是空操作。这一条根因不需要叠加任何第二个机制就解释了全部观测（cell 0 即错、A 越长 B 越差、大 arena 下走 `region()` 真实非零值因而正确、100% 确定性复现、专家权重侧全部干净）。

**修复**（`metal/src/engine/stream.cpp`）：`standalone_region()` 的 `region_bytes_ = sz`（函数内已算好的页对齐分配大小）。加固：`stream.h` 补上 `region_bytes_` 的真实契约注释（"要清零的字节数"，不是"借用量"——两者在 `region()` 路径碰巧相同，在 `standalone_region()` 路径不同，这正是 bug 根源），`hold_region(true)` 的 memset 前加 `assert(!region_ || region_bytes_ != 0)`（这类 bug 没有任何可检出状态，断言把它变成启动即炸）。同时删除了一个此前"测过无效"的不完整诊断（`TECTONIC_DEBUG_PGZERO`，只清了 QSA 相关 scratch，漏了 GDN 的那一半，所以测不出效果、一度把排查方向带偏到专家权重侧）。

验证：真实两节点部署（default/小 arena，确切复现配置），连续三个不同对话全部正确；既有回归测试不受影响。

**方法论教训**：和更早的 `&ss` 重复注册 bug 属于同一族——"某个一次性初始化建立的前提，在第二次使用时已不成立，但代码仍然相信它"。排查这类 bug，看到"只做一次"的初始化逻辑要下意识问"这个假设在第二次使用这份状态时还成立吗"。

**上游状态**：确认这个 bug 在 `Niko1221/Strata`（`origin/main`，`c6f6427d9`，目录已重组为 `engine/metal/`）原封不动存在，社区从未报过（只有 #6446、#3527 两个处理"内存没还回去"而非"没清零"的相邻改动）。已基于 `origin/main` 开干净单 commit 分支，推到 `myrepo`，提交上游 PR：[Niko1221/Strata#6722](https://github.com/Niko1221/Strata/pull/6722)。

**待办**：`(split || n >= --short-read默认64)` 这个 `prompt_gemm_init` 的进入条件和 split 无关——纯 `--serve` 处理 ≥64 token 的 prompt 且 arena 小到走 `standalone_region` 时理论上同样受影响，之前 `--serve` 验证一直用的短 prompt 没有跨过这个阈值，尚未专门验证；也还没有一个不依赖模型输出的专门回归测试（断言第二个请求开始时 `R`/`qfull`/`kcur` 的 checksum 为零）。

## 2026-11-23：合并上游 1284 个提交（含 `metal/` → `engine/metal/` 整体重构）

两个真实修复（split-stage T=1 decode、standalone 清零根因）先提交到本地 `main`，再 `git merge origin/main`（不是 rebase——fork 已推到 `myrepo` 且有自己的提交历史，rebase 需要强推且冲突会在每个提交上重演一遍，merge 保留两边历史、只需解决一次冲突）。

冲突范围：12 个 fork 独有的文件（wire 协议层整个家族）被 git 标成"file location"冲突，因为上游把它们所在的目录整体搬到了 `engine/` 下——逐个核对确认内容已被正确原样搬运，无真实冲突，直接 `git add` 确认。真正需要手工解决的 3 个文件：`engine/metal/CMakeLists.txt`（两边各自改了 include 目录/版本号，合并为保留我们的 `third_party/strata-llama/vendor` include + 采用上游版本号升级）、`engine/src/core/verify.cpp`（三处——两边各自的新增 `#include` 都保留；我们的 `window_set_stage` 调用和上游一条 `#879` bug 修复的 `le_` 默认值注释合并，不是真正重复；上游新增的 Metal batch-slot 非有限值扫描逻辑直接采用）、`engine/metal/src/engine/window.cpp`（我们给 `ple_on_` 加的 split-stage 专属条件和上游同时做的 PLE 权重加载泛化叠加，去掉了我们那行被上游泛化逻辑取代的多余类型判断）。

构建系统也跟着重组：从仓库根目录配置变成从 `engine/metal` 单独配置（`cmake -S engine/metal -B build-metal`，和 `.github/workflows/metal-build.yml` 的 CI 调用一致）；`tools/` 没有移动，`tests/core/` 搬到了 `engine/tests/core/`。

合并后用 CI 的确切命令重新编译出完整二进制（80/80 步骤无新增错误），接入真实两节点部署重跑 A→B→C 三个连续对话全部正确，三套既有回归测试全部重新跑过干净通过。合并提交 `90a4b2cb2`，工作区干净。

## 维护规则

1. **产品逻辑不进引擎。** runner、orch、编排、测试框架全部放 Tectonic。哪怕代码要贴着引擎写，
   宁可多一层接口。
2. **保持加法为主。** 一旦开始大段重写上游代码，合并成本会失控。
3. **定期 merge 上游**，merge 后跑现有回归。
4. **不要为了"整洁"重构不该重构的地方。** 先看块级重叠，再决定是否动。
5. **`#if defined(STRATA_GPU_METAL)` 是隔离带**，不是堆砌分支的地方。改 `verify.cpp` 时先确认
   改动落在哪个后端之下。
6. **旧注释不做扫描式清理。** 只清理这次改动直接触及的局部；散落在别处、这次够不到的旧注释留给
   下次真正touch那段代码的人顺手处理，不为了注释去碰不相关的代码。
