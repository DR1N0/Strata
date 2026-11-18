# 引擎分叉日志（Strata）

本仓库是 [Niko1221/Strata](https://github.com/Niko1221/Strata) 的下游分叉，用于 Tectonic 的层切分分布式执行。本文件记录**我们改了什么、为什么改、能否回到上游**。半年后没人记得某个 `if` 为什么存在，这份日志就是答案。

## 2026-11-17：历史重置——`main` 从 upstream HEAD 重新开始

`l0-stage-split`（分叉起点 `504b1433`，25 个提交）是早期可行性验证留下的探索历史，commit 记录了
一次"边做边改主意"的过程（含已经删除又在工作区被意外加回来的 ring 代码），不是值得长期背着的
历史包袱。它被**归档，不是删除**：分支本身保留，打了 `archive/l0-stage-split-20261117` 标签。

`main` 分支（以及这份文件）重新从当时的 `origin/main`（`c7757280`，比 `504b1433` 晚 946 个提交）
开始：

1. 把 `l0-stage-split` 相对 `504b1433` 的**净 diff**（不是逐提交 cherry-pick）应用到新的 `main` 上，
   排除 DSV41 实验（`src/dsv41/*`，一次性可行性验证，和通用引擎无关）和旧版本的这份日志。用净 diff
   而不是逐提交重放的原因：ring topology（`--listen`/`--downstream`/`--orch`）在那段历史里被加了又
   删，净 diff 自动把这段来回抵消掉，不需要再手动清理一遍。
2. 只有 `src/program/generate.cpp` 一处冲突，而且和这次改造完全无关——upstream 自己的
   `multi_gpu`/`GpuStage` 路径里，`dense.load()` 的调用签名在两边各自独立演化（我们加了
   `st.lb, st.le`，upstream 同时也在重排这段代码），用 upstream 当前版本的结构 + 我们的参数新增
   来解决，不是真正的功能冲突。
3. 在这个干净的基础上，**重新给旧的 `--agent` 路径改名、删掉不再需要的 flag**（见下面"命名"一节），
   而不是先原样照搬旧名字再改一遍——这样 `main` 的 commit 历史里不会出现"引入 --agent" →
   "`--agent` 改名"这种循环，直接就是目标状态。

### 归属核查：网络/跨进程这一整块，100% 是这个 fork 自己的

`git diff 504b1433 HEAD`（在旧分支上）核实过：`boundary_wire`/`boundary_socket`、
`--agent`/`stage_worker`、已删除的旧 ring，upstream 完全没有对应概念，都是这个 fork 从零写的。
**改名、重构、整体推翻，都不存在"动了 upstream 东西"的顾虑。** 真正有真实 rebase 风险的是对
upstream 文件的修改量——`generate.cpp` 的净改动是主挂载点，不是这些新概念本身。

## 命名：这一轮定下来，减少以后的反复改名成本

| 旧名字 | 新名字 | 理由 |
|---|---|---|
| `--agent HOST:PORT` | `--tectonic` + `--worker-data-endpoint HOST:PORT` | 原来一个 flag 兼职"选模式"和"带参数"两件事。`--agent` 本身的名字问题：是 Tectonic 侧已经退役的旧术语（历史上指"执行计算的进程"，现在叫 engine/worker），而且和"AI agent"在今天的语境下容易混淆。拆分之后，`--tectonic` 是纯模式开关（无值，和引擎已有的 `--serve` 同一种形状），`--worker-data-endpoint` 专职带参数，用的是术语表里现在活着的词（"worker"），和 Go 侧 `worker_data_port` 对称 |
| （不存在，新增） | `--tectonic` | 模式开关本身。不复用"有没有传 `--worker-data-endpoint`"来隐式判断模式——那样一旦这个 flag 写错或漏传，程序会悄悄滚回默认单次命令行模式，而不是报错。`--tectonic` 出现后，后面三个参数（`--worker-data-endpoint`/`--worker-signal-endpoint`/`--instance-id`）缺一个就是启动期硬错误，见 `generate.cpp` 里 `if (o.tectonic)` 开头的校验 |
| （不存在，新增） | `--worker-signal-endpoint HOST:PORT` | 新的第二条连接，专门承载 `cancel`/`shutdown`。叫"信号"不叫"控制"，是因为 daemon 另有一个叫 `control` 的 HTTP 管理端口（部署/预检/启停），两者不能共用一个词；"信号"贴合 `cancel`/`shutdown` 这类"打断"动作的语感（类似 Unix signal） |
| （不存在，新增） | `--instance-id ID` | 防止端口上残留上一次没回收干净的进程被误认 |
| `--stage-begin`/`--stage-end` | **彻底删除**（2026-11-17 第三次清理：上一版这份日志说"保留给 local multi-GPU 和单次命令行模式"，是错的） | 用 `git log -S"stage-begin"` 查过：这两个 flag 在分叉点 `504b1433` 和当前 `origin/main` 里都不存在，是这个 fork 自己的 `a85fbcc0` 引入的（和已删除的 `--stage-input` 同一个提交，同一个目的——单进程手动跑一段layer 做离线调试）。不是 upstream 的，也不是 local multi-GPU 真正依赖的东西（multi-GPU 走自己的 `split_at`/`split_devs`，不读 `o.layer_begin`/`o.layer_end`）。`--layers LO,HI` 现在是设置这对字段的唯一方式；合并成一个参数的理由不变（让"漏传一个边界"在语法上无法表达，R1 真的踩过这个坑） |
| `--batch N` | **不改**，原样沿用 | 已经是一个好名字，没理由发明新词；作为 `engine.args` 里的普通调优参数传，不进契约的必需参数 |
| `--stage-input` | **彻底删除**（2026-11-17 第二次清理：不是留作死代码） | 旧语义是"和 `--agent` 互斥，走文件输入模式"，读取另一个 stage 落盘的残差来做离线调试。`--tectonic` 整个被取代后这条互斥关系也一并失去意义；网络驱动的新路径有自己的输入来源（边界帧），不需要文件回放。struct 字段、两处文件读取逻辑块（`generate.cpp` 原 9971 行与 21987 行附近）、以及一处只能靠它才有意义的 `verify` hand-off 消费代码，三处全部删除或改写，不留 `if (!x.empty())` 式的永假死分支 |
| `--max-requests`、`--stage-output` | **删除**（第一轮清理时已确认） | 已确认是 ring 专用死代码（`StageRingConfig` 读取、`--agent` 从不读取），ring 删除后这两个参数只写不读 |
| ~~`--max-new`、`--tokens`~~ | **不删，保留** —— 2026-11-17 第二次清理时的更正 | 上一个版本的这份日志把这两个也列进了"删除"，是错的：它们是**通用**的 `Options` 字段，默认单次命令行模式和 `--serve` 都要用（分别表示"预先给的 prompt token"和"生成上限"），不是 `--tectonic` 专属。真正该改的是 **Tectonic 自己的 `qwen.go` 启动器不要在 `--tectonic` 模式下传这两个 flag**——新契约里"生成多少个 token"是每个 `gen` 请求自己带的字段，但这是 Tectonic 仓库那边的改动，不是这个引擎仓库要删 flag |

## 并发：day 0 就要支持，不是以后再加

确认 `run_slot_rows`（`include/strata/core/verify.hpp:253` 声明）是独立于 `--serve` 调度逻辑
（`batch_step`）的方法，设计时就考虑了切分场景（"split's stages each get their own sessions"），
`next_ == nullptr` 时就是算完自己这段就返回，天然适配"一个进程只管一段"的用法。**结论：不需要重构
`--serve` 把调度核心共享出来，也不需要在计算层重新发明——`--tectonic` 需要的是在 `stage_worker_run`
里新写一个比 `batch_step` 小得多的调度层**（无 MTP、无自适应分组、无 stdin 命令），攒够一批后调用
`run_slot_rows`，再按 `request_id` 把结果分发回数据连接。

并发数走 `--batch N`（已有，不新造参数），握手时把实际生效值自报给 daemon（Tectonic 侧 `hello` 的
`batch` 字段）。超出并发上限时要用明确的错误（`error`，`code: "batch_full"`）拒绝新请求，不能让
请求静默排队或挂起。

## 取消检查点：逐 token 粒度，对齐 `--serve` 的 `sl.stop`

`--serve` 里取消标志的检查点在**每个产出 token 之后**（`generate.cpp` 里 `keep[b]` 循环内部），
不是每个 batch window 一次。`--tectonic` 的新调度层要在 `run_slot_rows` 返回后、对每个 slot 的
产出逐 token 处理的循环里做同等粒度的检查，不能做成"每批一次"的粗粒度。取消本身应该是
fire-and-forget：不需要独立的"已收到取消"回执，结果体现在该请求自己的终态里（`done`/`error`，
对齐 `--serve` 的 `BDONE ... cancel` 模式）。

## 会话持久化（SAVE/RESTORE）：能做，但要绕开 `--serve` 的命令层

`SAVE`/`RESTORE` 拒绝 `--layer-split`/`--batch`/`--peer-device`，针对的是 upstream 自己的单进程
多 GPU 切分（`--split-devices`），和 `--tectonic` 完全是两套机制；`--tectonic` 根本不经过
`--serve` 的 stdin 命令解析器，这条拒绝检查对它"摸不到"，不存在"被拒绝"的问题。底层格式
（`conversation_file.hpp` 的 `layer_range`，读取时会校验）本来就支持部分层。**如果将来要做引擎侧的
会话持久化，是新写代码直接调用 `session_file_write`/`session_file_read`，绕开 `--serve` 的命令层，
而且仍然只是"保存这台机器自己负责的那段 KV"，不是跨机协调保存**——这是独立于并发改造的另一项
工程，R5 阶段再排期。

## 当前状态与待办

- [x] `main` 从 `origin/main` 重新开始，净 diff 应用（1 处无关冲突已解决）
- [x] flag 改名：`--agent` 拆成 `--tectonic`（模式开关）+ `--worker-data-endpoint`，新增
      `--worker-signal-endpoint`、`--instance-id`、`--layers`
- [x] `--stage-input` 彻底删除（不留死代码），`--max-requests`/`--stage-output` 删除
- [x] `hello`/`hello_ok`/`hello_reject` JSON 握手：新文件 `include/strata/core/control_wire.hpp` +
      `src/core/control_wire.cpp`，复用 `BoundarySocket::send_frame`/`recv_frame` 的既有 u32 小端
      长度前缀帧（和 `docs/design/engine.md` 的"帧编码"要求字节级一致，不是另起一套）。两条连接
      （数据、信号）各发一次同样形状的 `hello`，数据连接额外发一次 `ready`，信号连接没有。已用
      `clang++ -fsyntax-only -Wall -Wextra` 核实两个文件及接入点（`stage_worker.hpp`）独立语法
      正确；**没有在真实 daemon/假 daemon 之间跑通过**，见下面的"未验证"
- [x] 信号连接本身：`stage_worker_run` 里新开一条到 `worker_signal_endpoint` 的连接，握手后在
      独立线程里循环读 `cancel`/`shutdown`
- [x] 逐 token 取消检查点：数据循环每个位置处理前检查一次（2026-11-18 之前是
      `std::atomic<bool> cancel_requested`，现在是按 request_id 查表，见下面 2026-11-18 小节）。
      **缺口已部分补上**：entry stage 且被 entry bridge 认得的 request_id，取消现在回真正的
      `error{code:"cancelled"}`（JSON，不再是看起来和正常结束一样的 `kBoundaryEnd`）；非 entry
      stage 仍是老行为（`kBoundaryEnd`，和正常结束在 wire 上长得一样），因为它没有 bridge 可查
- [x] `stage_worker_run` 外层的并发——2026-11-18 的 Tier 1（按 request_id 隔离的交替处理，计算状态
      仍单一常驻）之后，2026-11-19 补了 Tier 1.5：`batch` 个独立常驻 slot，decode 单步计算跨 slot
      交替（`run_slot_rows(rows={slot}, S=1, ...)` 的形状，这次用假 compute 验证了调度逻辑，真正接
      `Verifier`/`SessionState` 仍是 `generate.cpp` 侧独立工程项）。`run_slot_rows` 的 kernel 级合批
      （Tier 2，多个 slot 合并进一次调用）仍未动手，理由不变：收益依赖专家是否 GPU 常驻，这轮刻意
      不做（Tectonic 侧 ADR 0005 的决定，不是这个仓库自己能下的判断）
- [x] 复核时补了一处遗漏：默认单次命令行模式下 `o.layer_begin > 0`（非 `--tectonic`、非
      `--split-devices`，现在只能通过 `--layers` 设置）过去唯一的输入来源是 `--stage-input`，
      删掉它之后这个组合会静默用全零的 hand-off buffer 跑出错误结果，而不是报错。现在在
      `--tectonic` 分支之后加了一个显式拒绝
      （`generate.cpp`，紧跟 `stage_worker_run` 调用之后），不让这个组合悄悄算出错的答案
- [ ] Tectonic 侧 `qwen.go`：`--tectonic` 模式下不要再传 `--max-new`/`--tokens`（这是 Tectonic
      仓库的改动，不在这里）
- [ ] `kBoundaryEnd` 同时承担"数据帧+完成标记"与"控制广播"，是否要拆成 `End`/`Seal`——不做也能
      运行，是清晰度问题，暂不处理
- [ ] 散落在各处注释里提到 `--agent`/ring 的旧描述，**不逐条全仓库扫描修复**，只在这次改动直接
      经过的地方顺手清理过（见下面"维护规则"）

## 2026-11-18：wire v4（`request_id`）、per-request 状态表、entry bridge——架构待决项已回到 Tectonic 侧确认

上一节的"架构待决"问题被拿回 Tectonic 设计讨论里确认了：**方案 1**（`gen`/`delta`/`done` 只是
entry stage 自己的请求生命周期外壳，stage 间的残差交换继续走 `BoundaryFrame`，不变）。方案 2（daemon
变成张量转发者）被排除——和"daemon 只透传不透明字节"这条已验证的设计矛盾，也完全没必要：真正驱动
这次决定的是 `R` 的实际形状（`[96,10240]`，96 行 prefill 块 fp32 约 3.9MB），塞进 JSON 不划算，二进制
格式本来就是对的选择。完整记录见 Tectonic 仓库的 `docs/decisions/0005-wire-v4-request-id.md`。

**做了什么**：

- `include/strata/core/boundary_wire.hpp` / `src/core/boundary_wire.cpp`：`BoundaryFrame` 加
  `uint64_t request_id`（紧跟 `model_fingerprint` 之后），`kBoundaryVersion` 3→4（50→58 字节头，
  `<IHHHQQqqqq`）。`request_id` 是不透明值，任何 stage 转发时原样带过去，不像 `layer_begin`/
  `layer_end` 那样被每个 stage 改写成自己的范围。`tools/test_boundary_wire.cpp`（CPU-only 的既有
  wire 测试）同步加了 request_id 的往返校验。Tectonic 仓库的 `tools/mesh_wire.py`（Python 镜像）
  和 `tools/src/tectonic_tools/probes/stage_probe.py`（诊断探针）也同步改了——这两个文件不在本仓库，
  但它们必须和这里的 wire 字节级一致，否则 v4 引擎直接拒绝它们发的 v3 帧。
- `src/program/stage_worker.hpp`：`stage_worker_run` 原来的单一共享变量（`pf_n`/`pf_rows`/
  `pf_tokens`/`cancel_requested`）换成 `std::unordered_map<uint64_t, PerRequestState>`，按帧的
  `request_id` 查表。信号线程只写 `cancelled`（`std::atomic<bool>`），`requests_mu` 只保护 map
  结构本身（insert/find/erase），不保护 prefill 累积缓冲区（只有主循环会碰它们）。**明确没做**：
  `reset()`（session_zero：R/GDN 递归态/QSA cache/PLE 历史）语义没碰——Tier 1 阶段这个引擎进程依然
  只有一份计算状态常驻，per-request map 解决的是"交替处理不串数据"，不是"真正并发计算"；
  `StageWorkerConfig::batch`/`HelloFields::batch` 的注释已经更新，明确这是协议层在途数量，不是计算
  状态独立的 slot 数。
- `include/strata/core/control_wire.hpp` / `.cpp`：新增 `gen`/`delta`/`done`/`error` 的 JSON 读写
  函数（`GenFields`、`control_send_gen`/`control_recv_gen`/`control_parse_gen`/`control_send_delta`/
  `control_send_done`/`control_send_error`），字段形状和 Tectonic Go 侧 `internal/engine/protocol.go`
  一一对应。文件头注释从"待决"改成"已解决"。
- 新文件 `include/strata/core/entry_bridge.hpp` + `src/core/entry_bridge.cpp`：`EntryBridge` 类，
  维护 Tectonic 字符串 `request_id` ↔ 本进程内 64 位 wire id 的映射、准入上限、每请求已生成 token 计数
  （`max_new` 比较）。只在 entry stage（`layer_begin == 0`）且被显式接上时启用。
- `stage_worker_run` 接入：数据连接现在可能收到两种字节——`BoundaryFrame`（原有，和邻居 stage 之间）
  和 JSON `gen`（和本机 daemon 之间）——靠首 4 字节是否等于 `kBoundaryMagic` 区分
  （`looks_like_boundary_frame`），同一套 u32 长度前缀框架天然支持两种 payload，不需要第二套分帧。
  收到 `gen` 后：按"prefill 处理 [0, n-1)，最后一个 prompt token 走 decode 路径"（`generate.cpp`
  自己的 plan v0.3 P5 注释）拆成 1-2 个合成帧塞进 `pending` 队列；每次 decode 产出的 token 翻译成
  `delta`，达到 `max_new` 就 `done`+释放，没达到就把采样 token 自己喂回去当下一个位置的输入——这是
  以前由 Python daemon 原型的回显循环做的事，现在 Go daemon 不说 `BoundaryFrame` 了，只能挪进引擎自己
  做。取消：信号连接的 `cancel` 命中时，entry bridge 能查到是哪个 Tectonic 请求，直接回真正的
  `error{code:"cancelled"}`，不再是"看起来和正常结束一样的 `kBoundaryEnd`"那个老缺口——但这只在
  entry stage 且 bridge 认得这个 request_id 时成立，非 entry stage 仍是老行为（没有变，也不可能变，
  见下面"明确没做"）。

**明确没做，而且是测试过程中才发现、不是一开始就知道要排除的**：

1. **真正的并发准入（`batch_full`）在这套接入方式下永远不会在 socket 层触发**。
   自喂循环（采样 token 没到 `max_new` 就塞回 `pending`，下一轮循环优先处理 `pending` 而不是读
   socket）意味着：只要一个请求还没走完，主循环根本不会再去读 socket，新的 `gen` 只是安静地躺在
   TCP 接收缓冲区里，等当前请求**完全结束、释放 bridge 名额之后**才会被读到——这时候已经不存在
   "超限"这件事了，新请求总是顺利准入。`EntryBridge::admit` 的容量检查代码本身是对的（有独立于
   socket 的单元测试验证，见下面"验证"），但这条集成路径永远走不到它的拒绝分支。想要在繁忙连接上
   真正触发 `batch_full`，主循环需要在还有 `pending` 积压时也去 poll 一下 socket，而不是把
   `pending` 排空当成唯一优先级——这是下一步要做的事，不是这次能顺手带上的。
2. **EOS 检测没有实现**：旧 Python daemon 回显循环靠外部把采样 token 和模型的 EOS id 比较来决定
   "正常结束 vs 继续"，`stage_worker.hpp` 一直是模型无关的，不知道 EOS id 是什么。本轮 entry bridge
   只按 `max_new` 计数停止，不检测 EOS——真实生成会越过模型自己该停的点，这是文本质量问题，不是
   wire 机制问题，但不能不提。
3. **真正切分（多机）下 entry bridge 不处理环闭合**：`out.n_tensors == 0 && c.lo == 0` 这个信号只对
   "entry 自己独占整个模型"的单 stage 部署成立；一个真正跨机的 split，环绕一圈回到 entry 的采样
   token 需要另一种判定方式（"这是我自己发出去的请求的环闭合"要和"邻居发来的其他流量"区分），
   本轮没有实现，也没有在这个方向上做任何假设性尝试。

以上三条都写在 `entry_bridge.hpp` 的文件头注释里，不是只记在这里。

**验证**（这次要求真正跑起来，不是 `-fsyntax-only`）：`tools/test_tier1_concurrency.cpp`，CPU-only，
两个真实 TCP 连接对（数据+信号），`stage_worker_run` 跑在独立线程里，和驱动它的"假 daemon"不共享
调用栈：

- Section A：一个非 entry-bridge 的 stage，两个 request_id（100、200）交替推进，验证 prefill/decode
  状态不串、取消 100 不影响 200（修复了一处真 bug：`current_request_id` 原来只在 `pos0==0` 时更新，
  导致请求 200 的后续位置会被误判成"当前"，取消请求 100 时实际打中了 200——测试第一次跑直接暴露了
  这个问题，不是靠代码审查发现的）。
- Section B：entry bridge 全链路，4-token prompt 验证 prefill 拿到的确实是前 n-1 个 token、
  decode 从最后一个 prompt token 开始、`done.tokens` 等于完整序列、一个 gen 在另一个处理期间发送后
  按顺序排队接纳（而不是当场拒绝，见上面的发现 1）、单 token prompt、取消翻译成
  `error{code:cancelled}`。
- Section C：`EntryBridge::admit` 的容量检查单独测（不经过 socket），因为 Section B 证明了集成路径
  碰不到这个分支——这是本节的做法：承认"写了但集成测不到"，用独立测试补上去，而不是假装集成测试
  覆盖了它。

45 checks, 0 failed（`clang++ -std=c++20 -O1`，这台 Mac arm64 上，CPU-only）。

## 2026-11-19：`batch` 个常驻 slot，交替单步计算（ADR 0005 补充决定，"Tier 1.5"）

Tectonic 侧提出一个具体场景：多个 stage 顺序处理同一个请求的 decode 时，任意时刻只有一个 stage
在真正计算，其余 N-1 个都在等这个请求自己的帧走完一圈环——这是自回归依赖本身决定的，单个请求无法
绕开。**能绕开的是**：如果环上同时能放得下不同请求，一个 stage 在等请求 A 的帧在途的时候，本来可以
用这段空闲时间算请求 B 的下一个位置。2026-11-18 的 Tier 1 只保证了"协议层不串数据"，计算状态仍然
是单一一份常驻——这次要确认的是，"多份独立常驻 session、交替单步计算（不合批）"这件事，是否能和
`run_slot_rows` 的合批（Tier 2，ADR 0005 明确搁置的部分）分开做。

### 可分离性：读了真代码，不是推断

- `include/strata/core/verify.hpp:248-258`（`run_slots`/`run_slot_rows` 的文档注释）："the slots not
  listed are not touched, so an idle slot keeps its state"——独立 per-slot 状态是写在契约里的，不是
  额外保证。
- `src/core/verify.cpp:4713-4840`（`run_slot_rows` 实现本身）：**没有单独的"solo"代码路径**。
  `run_slots(S,...)` 字面就是 `for t<S: rows[t]=t; return run_slot_rows(rows, S, ...)`。S=1 不是
  特殊分支，是同一个函数退化到一行。
- `src/program/generate.cpp:15916-15945`（`--serve` 自己的调度器）：把当下 `.active` 的 slot 收集
  进 `rows[0..S)`，调用一次 `run_slot_rows`——`if (S == 0) return true;` 本来就要处理，S 退化成 1
  是这套调度器自己日常就会走到的情况，不是新代码。
- `src/program/generate.cpp:21276-21297`（`admit_slot`/`copy_to_slot`）：**`--serve` 自己的架构早就
  把 prefill 和常驻 decode 分开**——一个请求先走 solo 路径把 prompt 读完，然后"入队"（`sl.active =
  true`）进一个 slot，之后才走批量/独立 decode。这正是这次要做的设计（prefill 串行、decode 跨 slot
  交替），不是凭空发明，是照抄已有架构。
- `include/strata/prefill/prefill.hpp:82-83`（`Prefill::init`）：吃的是单个 `SessionState&`，不像
  `Verifier` 那样是 slot 向量——`Prefill` 本身不是多 slot 的。这是唯一真正不确定的地方（两个 slot
  的 prefill 要不要可能同时跑），但这次刻意不解决：prefill 继续严格串行（永远不并发跑两个），只有
  decode 跨 slot 交替，和 `--serve` 自己的 admit 模式完全一致，这个不确定性不需要本轮回答。

**结论：decode 单步计算跨 slot 交替，和 `run_slot_rows` 的 kernel 级合批是可分离的，而且有真实代码
先例，不是臆测。**

### 做了什么

- `src/program/stage_worker.hpp`：
  - `StageWorkerCompute`/`StageWorkerReset`/`StageWorkerPrefill` 三个回调都加了 `slot` 参数（第一个
    参数）——调用方（`generate.cpp`，这次没有改，仍是下一步工程项）要把它绑定到
    `Verifier::run_slot_rows(&slot, 1, ...)`／对应 slot 的 `SessionState`。
  - `requests`/`PerRequestState`（2026-11-18 的单一 map）换成 `slot_state`（`batch` 个固定槽位）+
    `request_slot`（request_id → slot 下标）。`ensure_slot`/`find_slot`/`release_slot` 三个辅助函数
    取代了 `reset_request_state`。
  - **新增一个后台"读线程"**，专门读 `link`（数据连接），把字节推进一个带条件变量的队列
    （`incoming`），和主循环的计算/分发彻底解耦。没有这个，接纳一个新请求是不可能的：旧设计只有在
    "无事可做"时才读 socket，意味着一个已经常驻的 slot 的自喂循环会一直堵住任何新 `gen` 的准入，
    直到那个请求完全结束——这是写测试时发现的，不是预判到的（和上一轮 `current_request_id` 的 bug
    一样，靠跑测试找出来，不是代码审查）。主循环现在每一轮都先非阻塞地看一眼 `incoming`，有就优先
    处理，没有才去排 `pending`（自喂队列）——顺序反过来会让新准入排在已经常驻的 slot 后面挨饿。
  - `EntryBridge::admit` 的 `max_in_flight` 现在传 `c.batch`（之前硬编码 1）。
  - `entry_bridge.hpp` 的文件头注释更新：两条已知缺口（不检测 EOS、不处理真实跨机环闭合）原样保留，
    新增一条——这次加的调度机制对它们没有影响，仍然是独立、未解决的问题。

### 发现：原来 Tier 1 报告过的"gen 排队而不是拒绝"这个缺口，被这次的改动顺带修好了

上一轮（2026-11-18）的测试记录过："一个 gen 在另一个处理期间发送，会排队，不会被当场拒绝"，原因是
旧设计只有在空闲时才读 socket。这次加了"每轮都先看一眼 incoming"之后，重新测发现：**单 slot
(`batch=1`) 的场景下，繁忙时发来的 `gen` 现在真的会在另一个请求还没结束时就收到活的
`error{code:"batch_full"}`**，不再是悄悄排队——这是本轮改动的副作用，不是刻意修的，但确实修好了，
已经用测试验证（见下面）。同时也确认了一个真实、固有的边界：这个"每轮检查"不是"计算中途可抢占"，
如果一个请求的单步计算本身很快、自喂链条首尾相接没有间隙，第二个请求的 `gen` 字节有没有来得及被读
线程送进 `incoming`、赶在当前请求释放 slot 之前被检查到，是一个真实的时序竞争，不是哪一边的 bug——
写测试时第一次用很短的 `max_new`（2）复现了这个竞争并且输给了它，加长 `max_new` 给出足够的真实窗口
后才稳定复现"繁忙时被拒绝"。这个边界条件记在这里，不是本轮的待办。

### 验证：真实两段 stage 链路，带时间戳的重叠证据

`tools/test_tier1_concurrency.cpp` 新增 Section D：两个真实的 `stage_worker_run` 实例（stage A
`[0,1)`、stage B `[1,2)`，各 2 个常驻 slot），两个测试侧"driver"线程各自独立跑一个请求的完整环
（A → 人为的"传输延迟"(40ms) → B → 人为的"传输延迟"(40ms) → 回到 A，4 个位置），伪计算本身只睡
5ms。因为两个线程会并发读写同一对连接，且 `stage_worker_run` 不保证按发送顺序回复（这正是 wire v4
要解决的问题），测试侧自己也要一个按 request_id 分发的解复用器（`FrameDemux`）——不这么做的话测试
自己会引入一个看起来像被测代码 bug 的竞争。

记录每次"传输延迟"的起止时间戳和每次 compute() 的起止时间戳（按 token 值所在的区间给请求打标签，
因为 stage A 的 `compute()` 对 `lo==0` 收到的 `input` 是空指针，拿不到 request_id），然后找：
请求 X 的传输延迟区间，和请求 Y（另一个）在同一个 stage 的 compute 区间，是否有真实的墙钟重叠。

连续三次独立运行：16 个传输窗口里分别找到 13、14、15 个重叠——不是边界运气，是稳定可复现的真实
时间重叠，不是"先后完成看起来像并发"。64 checks, 0 failed（含 A/B/C 三节）。

## 2026-11-19（续）：`generate.cpp` 接真实 `Verifier`——真实单请求正确，真实双请求仍有未解决的 bug

上一节的 slot 调度逻辑一直是用假 compute 验证的；这次任务是把 `compute`/`reset`/`prefill` 回调接到
`generate.cpp` 的真实 `Verifier`/`SessionState`/`Prefill`，并且**要求真实模型、真实 decode 计算的
证据**，不能停在"编译通过"。过程中发现并修了四个真问题，第五个只修对了一半——如实记录，不夸大。

### 先确认：真实二进制当时编译不过

`f526d5005` 改了 `StageWorkerCompute`/`Reset`/`Prefill` 的签名（加 `slot` 参数），但没有同步改
`generate.cpp` 的 `--tectonic` 分支——`compute_stage`/`reset_stage`/`prefill_stage` 三个 lambda
还是旧签名。真实 `strata` 二进制（不是 `tools/test_tier1_concurrency.cpp` 那个不碰 `generate.cpp`
的独立测试）在这次之前编译不过。用 `clang++ -c`（真实对象文件，不是 `-fsyntax-only`）确认过。

### 做了什么，按发现顺序

1. **把三个回调的签名和调用点改成 slot-indexed**（本来就是这次任务要做的事）：`n_slots` 个独立
   carve 的 `SessionState`（`session_bytes`/`session_init`，和本文件已有的多 GPU "stages" 循环
   同一个模式，不是发明新机制）；`Verifier::init_slots(slot_sessions, err)`。
2. **发现并修：`metal/CMakeLists.txt` 没把 `entry_bridge.cpp` 列进 `STRATA_METAL_HOST_SOURCES`**。
   `ceee43bd9` 加了这个文件但没同步 CMake 源文件列表，这次用真的 `cmake -S metal -B ... -G Ninja`
   （装了 `cmake`/`ninja`，这台环境原来没有）配置+构建才链接报错发现——这是另一处预先存在的缺口，
   不是本轮引入的。已修：加一行。
3. **发现并修：`--batch` 这个 gate 从来没打算支持 `--tectonic`**。`generate.cpp` 里有一条校验
   `!o.serve ? "it needs --serve" : ...`，不是 `--serve` 就把 `o.batch` 静默清零成 0——这条校验写
   在 `--batch` 还只给 `--serve` 用的年代，`--tectonic` 读 `o.batch` 这件事（`wc.batch = n_slots`）
   从来没真正被这条校验放行过。真实跑 `--batch 2` 才发现"2 个 slot"从没真正到过 `init_slots`。
   已修：改成 `!o.serve && !o.tectonic`。
4. **发现并修：`generate.cpp` 从来没有构造 `EntryBridge` 并挂到 `wc.entry_bridge`**。
   `stage_worker_run` 收到非 `BoundaryFrame` 字节时，`c.entry_bridge == nullptr` 直接拒绝——
   `ceee43bd9` 加了 `EntryBridge` 类，但 `--tectonic` 分支从没构造过一个。真实跑才报错发现；修法是
   `layer_begin == 0` 时构造一个局部 `EntryBridge` 并赋给 `wc.entry_bridge`。
5. **发现但只修对了"单请求"：`SessionState*` 指针身份不等于 Metal window 的 batch slot 身份**。
   这是这次最深的坑，也是过程最长的部分：
   - 最初以为"每个 slot 一个独立 carve 的 `SessionState`，`Prefill`/`session_zero`/`run_slot_rows`
     都直接传这个指针"就够了——单 slot (`--batch 1`) 测试给出 `[16,15,1088,88396]`，不是真实 ground
     truth `[4,5,6,24218]`（prompt `[1,2,3]`，和本仓库 `docs/archive`/Tectonic 仓库
     `DUAL_MACHINE_L0_20261113.md` 记录的参考值一致）。
   - 先排除"是 entry bridge 拆帧错了"：把 `compute_stage` 临时改回最早的单会话 `stage_ver.run(1,...)`
     + `commit(1,...)`（完全不碰 slot），同一套 entry bridge 拆帧逻辑，结果就是对的
     （`[4,5,6,24218]`）——说明 entry bridge 本身没问题，问题在 `run_slot_rows` 这条路径。
   - 读 `metal/src/engine/window.cpp` 的 `Window::slots_init`/`run_slots`/`slot_copy`，加上
     `src/program/generate.cpp` 里 `--serve` 自己的 `copy_to_slot`（约 15687 行）才搞清楚：
     **Metal 后端的 batch slot 是 `Window` 内部单独 carve 的 GPU buffer 集合（`sets_[1..N]`），
     和传给 `init_slots` 的 `SessionState*` 指针身份无关**——`Prefill`/`session_zero`/单会话
     `run()` 实际操作的是 `Window` 的"live"（也就是 `sets_[0]`，`ss_` 别名）那一份缓冲区，不是
     按指针寻址到某个 slot 的缓冲区。要让一个 batch slot 真的有内容，必须显式
     `strata::metal_engine::window_slot_copy_main(true, slot, upto, false, err)`——`--serve`
     自己的 `copy_to_slot` 就是干这个的，不是"续接旧对话"专属机制，是"往 slot 里第一次写入内容"
     本身依赖的机制，这是我最初漏看的一点。
   - 改法：prefill 和每个请求的**第一次** decode 仍然跑在共享的 solo `ss`/`stage_prefill` 上（和
     改动前的原始单会话代码完全一样的路径），紧接着显式 `window_slot_copy_main(true, slot, pos+1,
     false, err)` 把结果搬进目标 slot，之后该 slot的后续 decode 步骤才用 `run_slot_rows`。每个
     slot 一个 `bool slot_primed` 标志位记这个状态，`reset_stage` 时清掉。
   - **验证：单请求现在完全正确**。`tier15_single_test.py`（真实 Qwen3.8-Flash-Next pack、真实
     Metal 计算、`--batch 1`）：`done.tokens = [4,5,6,24218]`，和 ground truth 字节级一致。

### 没能修完：第二个 slot 的结果仍然错，和"是否并发"无关

加了 solo-then-copy 之后，两个请求**真正并发交替**跑（`tier15_real_test.py`，`--batch 2`，prompt
`[1,2,3]` 和 `[10,20,30]`）：req-a 正确拿到第一个 token（4），之后错；req-b 从第一个 token 就错
（`[10,20,30,10]`，不是 ground truth `[198,10,20,30]`）。为了排除"是并发交织把状态搞乱了"，写了
`tier15_sequential_test.py`：**同一个进程里，A 完全跑完、释放 slot 之后再发 B**，结果一样——
`req-a` 正确，`req-b` 还是 `[10,20,30,10]`。

**这排除了"并发/交织"假设，把问题精确缩小到："这个进程生命周期里，第二个被用到的 slot，不管是不是
并发，结果都错；第一个永远对。"** 没有继续深挖下去——怀疑和 `Window` 内部某个全局的、跨 slot 共享
的生成计数/奇偶状态有关（读 `slot_copy` 时见过 `hp_`/`o.hp`，"history parity"，按注释是某种
ping-pong 缓冲的奇偶标记；`session_zero` 显然没有重置这个——但这是没有验证过的假设，不是结论）。
留了一个调试钩子在 `src/core/verify.cpp` 的 `slot_row_draw`（环境变量 `TECTONIC_DEBUG_SLOT_DRAW=1`
打印采样参数，排除了"第二个 slot 采样配置不对"这个曾经的假设——结果显示采样参数本身是对的，问题在
更早的计算阶段，不在 `slot_row_draw`），给下一轮继续查的人留个起点，不是本轮的待办项。

### 这一轮实际验证到的程度，不要读多也不要读少

- **真实构建**：`cmake -S metal -B ... && ninja`，真实 Metal 后端，真实 `strata` 二进制，链接成功、
  能跑。
- **真实模型、单请求、真实 decode compute**：`/Users/haiyuwang/models/qwen38/native-pack-v_q0u84a`
  （Qwen3.8-Flash-Next IQ3_XXS），`--batch 1` 和 `--batch 2`（仅用其中一个 slot）下，结果都和
  solo（非 `--tectonic`）跑同一个 prompt 的输出字节级一致，且和 Tectonic 仓库
  `DUAL_MACHINE_L0_20261113.md` 记录的参考值一致。**这是本轮达到的、有真实分量的正确性证据**。
- **真实模型、两个请求同时在途**：**没有验证通过**。第二个 slot 的结果错，和并发/串行无关，根因
  没找到。`tools/test_tier1_concurrency.cpp` 的假 compute 测试（64+191 checks）不受影响，仍然全过，
  但那测的是调度逻辑，不是这次新发现的真实计算 bug——**不能拿假 compute 测试通过当作"多 slot 真实
  计算正确"的证据，这两者是不同的问题**。
- 结论：Tier 1.5 的调度层（wire v4 的 request_id 分发、slot 准入、读线程）在真实引擎上被证明是对的
  （至少跑到了正确绑定真实 Verifier 这一步，没有在调度层面翻车）；真正的"多 slot 同时装着不同
  conversation 的真实计算"这件事，这一轮做到了单 slot 完全正确，多 slot 还有一个未解决的真 bug。

## 2026-11-19（续二）：diagnose 第二个 slot 的 bug——定位更精确了，根因仍未找到

上一节报告的 bug 这次重新测量后发现描述得不够准：不是"第二个 *slot 编号*"，是**"这个
`stage_ver`/`Window` 实例上，第二次跑 solo prime 流程（不管中间有没有发生过 `run_slot_rows`，
不管这次用的是不是同一个 slot 编号），永远错；第一次永远对"**。两个证据：

- 顺序测试（A 跑完整个会话、释放 slot，B 才开始）：加了 `TECTONIC_DEBUG_COMPUTE=1` 跟踪后，**A 和
  B 都被分到了 slot 0**（`ensure_slot` 找最小的空闲编号，A 释放后 B 自然又拿到 0）——这推翻了
  "slot 1 比 slot 0 有问题"这个假设。
- 并发测试：B 这次确实拿到了 slot 1（A 还占着 slot 0），但 B 的 unprimed（solo）第一步**在 A 的任何
  `run_slot_rows` 调用之前**就已经错了——这推翻了"是 batch 路径弄脏了后面的 solo 调用"这个假设，
  因果顺序对不上。

两次测试共同点：错的那次调用，之前这个 `stage_ver` 已经**至少完整跑过一次 solo prime
（`run(1,...)` + `commit(1,...)` + `window_slot_copy_main`）**，不管这次之后发生过 `run_slot_rows`
与否。

### 具体错成什么样：精确到 position 级别，不是"乱码"

`TECTONIC_DEBUG_COMPUTE=1` 完整单请求/双请求 trace（见下方，命令可复现）显示 B 的输出不是随机垃圾，
是一个精确的规律：

```
位置(pos)  真实喂入的 token   应该预测    实际预测
  2           30 (prompt最后一个)   198        10
  3           10 (上一步采样值)      10         20
  4           20                    20         30
  5           30                    30         10
```

把"实际预测"对齐到"这个 token 在原 prompt 里的位置"看：**`predict(pos) == token_at(pos − 2)`**——
每一步预测的都是"当前位置往前数 2 格"喂进去的那个 token，不是真正的模型推理结果，像是在读一个
没清干净、位置整体偏移了 2 的缓冲区。"2" 这个数字和 `--spec 2`（这个 pack 强制要求，决定
`Verifier::init` 的 `max_t=2` speculative 窗口大小）数值相同——这**是一个值得继续查的线索，不是
结论**，没有验证过两者的因果关系。

### 排查 `hp_`（"history parity"）：读懂了，但排除了它单独作为根因

`metal/src/engine/window.cpp:499` 声明 `int hp_ = 0;`——**这是 `Window` 一个进程/实例全局的字段，
不是每个 slot 自己的**（每个 `SeqSet` 另有自己的 `o.hp`，两者是两回事）。读了每一处用到它的函数：

- `Window::finish`（`window.cpp:2049`，solo 单序列路径用）：`if (Tn == 1) hp_ = 1 - hp_;`——**每次
  solo 单 token 推理都会把这个全局 parity 翻转一次**。
- `Window::finish_rows`（`window.cpp:1656`，batch/`run_slot_rows` 路径用）：只翻转
  `s.hp`（对应 slot 自己的字段），**完全不碰全局 `hp_`**。
- `Window::slot_copy`（`copy_to_slot`/`window_slot_copy_main` 背后的实现，`window.cpp:2295-2301`
  一带）：`const int hs = into_slot ? hp_ : o.hp;`——拷贝进 slot 时，用的是**当前全局 `hp_`** 决定
  从 `ghist_[hs]` 还是 `ghist_[1-hs]` 读；拷贝完顺手 `o.hp = hp_`，把这次的 parity 记到 slot 自己
  头上。

**`session_zero`（Metal 路径是 `window_reset`→`Window::reset()`，`window.cpp:2387`）不重置 `hp_`
本身**，但它确实把 `ghist_[0]` 和 `ghist_[1]` **两个 parity 的内容都清零**（`window.cpp:2393-2397`）。
这意味着：不管 `hp_` 当前指向 0 还是 1，`reset()` 之后两边都是干净的零——**这排除了"hp_ 读到另一个
会话留下的脏历史"这个最初的假设**，因为两边都被清过。

### 验证过、确认没用的两个尝试

1. **补 `Window::reset()` 漏掉的 `pooled_`（QSA indexer 的"pooled"块摘要）清零**（`window.cpp:2410`
   附近，已提交）。这是读 `src/core/layer.cpp:981` 的 CUDA/HIP 版 `qsa_state_zero` 时发现的真实、
   独立成立的缺口——CUDA/HIP 那边显式 `cudaMemsetAsync(st.idx_pooled, ...)`，Metal 的 `reset()`
   从来没有对应这一步（`tail_`/`dead_` 清了，`pooled_` 没清）。**这是一个真实的、值得保留的正确性
   修复，但单独加上去之后，`tier15_sequential_test.py` 的结果完全没变**——不是这次报告 bug 的根因，
   是另一个（更小、更隐蔽，可能从未被触发过，因为大多数真实场景下 pooled 的陈旧内容恰好不会被路由
   选中）独立缺口。保留这个修复，但不claim它解决了本节的问题。
2. **额外再清 K/V（`kq_`/`vq_`/`ksc_`/`vsc_`）**（`TECTONIC_DEBUG_ZERO_KV=1`，诊断用，默认关闭，
   未提交为默认行为）：同样测过，结果没变。这与"K/V 按位置读写，不需要清零"的原有假设一致，但既然
   `pooled_` 的假设也错了，这次是真测过，不是凭这条假设就不测。

### 第三条路：`--serve` 自己的准入流程，没有发现额外步骤

重新读了 `--serve` 的 `BGEN`/`admit_slot`/`copy_to_slot`（`generate.cpp:16784-16797`、`15687`
附近）：**它的准入机制和这次 `--tectonic` 用的完全一样**——先在 solo 路径（同一个 `ss`/`stage_ver`）
跑完 prompt 和第一个 token，调用方（`serve/server.py`）用 `BGEN <slot> <max_new>` 显式指定目标
slot，引擎侧调用的还是同一个 `window_slot_copy_main`。**没有找到"`--serve` 在 solo prime 和
copy-to-slot 之间多做了一步我们没做"的证据**——这要么说明这个 bug 在 `--serve` 的真实生产使用里
同样存在、只是没被这种"同一个 Verifier 连续接两个全新会话"的用法模式触发过，要么说明问题出在
`--tectonic` 这边更靠后、更隐蔽的地方，两种可能都没有被这一轮排除。

### 第四条路：上游的真实先例

`origin`（`Niko1221/Strata`，公开仓库，`gh issue list --repo Niko1221/Strata` 可查）上有一个直接
相关的真实案例，**同一个子系统，同一类缺陷形状，不同的具体字段**：

> **#4908**「Metal batch slots (0.2.9): a picture request decodes other text once another
> request joins (slot rows rotate without M-RoPE)」——`--vision` 场景下，一张图片请求单独跑是对的，
> 第二个请求一起进 batch slot 就错；原因是 `qsa_prep` 的 function constant 只带了 slot-rows 版本
> （30），没带 M-RoPE 版本（24），导致每个 slot row 按"纯 cell 位置"转而不是按"M-RoPE 位置"转。
> 报告者给出的修复方案：**每个 slot 自己留一份 M-RoPE 表，`slot_copy` 时把准入请求的表拷贝进去**——
> 和这次 `pooled_` 缺口的修复形状完全一样（"`slot_copy`/`reset` 漏了一块该搬/该清的状态"）。

这不是同一个 bug（我们没用 `--vision`/M-RoPE），但确认了一件事：**"Metal 的 batch slot 机制有
某块状态没有被 `slot_copy`/`reset` 正确搬运或清零"是这个代码库里真实发生过、有真实先例、有真实
修复模式的一类问题，不是本轮凭空怀疑**。没有搜到和本次文本场景（QSA pooled、无 vision）完全对应
的已知 issue。

### 复现命令（供下一轮直接跑，不用重新搭）

```
cd ~/projects/strata
cmake -S metal -B /tmp/strata-build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMETAL_CPP_DIR=/Users/haiyuwang/models/ds41/downloads/metal-cpp-26.4-unpacked.cqlcx4su/metal-cpp-c595afef4a5dc388f4047cd0c69f9e7f9468d9ed
ninja -C /tmp/strata-build-metal strata
TECTONIC_DEBUG_COMPUTE=1 python3 /tmp/tier15_sequential_test.py   # 若 /tmp 已清空，脚本内容见本节此前的 commit message / 对话记录
```

### 诚实的定性：这不是"快速修完"的问题

两个独立的、读代码读出来、看起来很有道理的假设（`hp_`/`ghist_` 陈旧，`pooled_`/K-V 没清零）都被
真实跑分真实模型**证伪**了，不是"还没来得及测"。剩下的"predict(pos) = token_at(pos−2)"这个精确
规律指向某个和 `--spec 2`/`max_t=2` 的 speculative 窗口捕获（graph capture/replay）或者 QSA
indexer 的路由逻辑有关的、更底层的状态复用问题，但这两个方向都需要继续深挖
（`capture_batch`/`record_window`/窗口图缓存键，或者 indexer 具体怎么决定"看哪些 cell"），不是
再加一行 `memset` 就能验证的假设了。上游 #4908 的先例说明这类 bug 修起来通常是"找到那一块多出来的
专属状态，让 slot_copy 搬一份"，但这次已经验证过的几块（pooled、K/V、ghist 双 parity）都不是，
说明还没找到那一块。**不建议现在就投入更多时间盲试下一个假设，比较值得的下一步是要么上 Metal 的
GPU 调试工具直接看缓冲区内容，要么把这个现象整理成一个新 issue 发给上游，参考 #4908 的格式**。

## 2026-11-19（续三）：找到真正的根因并修好了——`&ss` 在 `set_of_ss_` 里注册了两次

**`--spec 0` 这条路排除了**：这个 native (IQ) pack 的 CLI 校验（`generate.cpp:5479`，
`o.spec < 2` 直接拒绝）硬性要求 `--spec >= 2`，`--spec 0`／`--spec 1` 都在模型加载前就被拒绝，这个
具体模型没法测"完全不跑投机解码"这条路径。但 `--spec N` 的帮助文本（"the MTP drafter's verify
window: how many tokens it proposes per check"）和实测日志（`stream windows T=2` 确实出现过）说明
投机解码是真的在跑；不过这条路最后被证明和真正的根因无关（见下）。

**GPU 缓冲区读回，这次真的做了，不是只读日志**：在 `Window::commit()`（`window.cpp:2432`）结尾加了
一段调试代码（`TECTONIC_DEBUG_BUFFERS=1`），在每次 commit 后把 `gst_[0]`／`ghist_[0][0]`／
`ghist_[1][0]` 这几个 GDN 状态缓冲区（Apple Silicon 统一内存，`.ptr` 直接是 CPU 可读的指针，不需要
`MTLCaptureManager`）求和，对比 A（prompt `[1,2,3]`）和 B（prompt `[10,20,30]`）各自 prefill 之后
的数值。结果：

```
A: gst_[0] sum=107.736026 absum=1634.979140 | ghist_[0][0] sum=-3.527412 absum=41738.717532 | ghist_[1][0] sum=84.964309 absum=28237.515764
B: gst_[0] sum=107.736026 absum=1634.979140 | ghist_[0][0] sum=-3.527412 absum=41738.717532 | ghist_[1][0] sum=84.964309 absum=28237.515764
```

**两组数字逐位相同**——B 用完全不同的 prompt token，prefill 之后的 GDN 状态却和 A 的一模一样，
浮点数不可能"巧合"到这个精度。这不是"B 读到了 A 的陈旧状态"（那样应该是 A 的状态被 B 自己的新
token 继续推进后的某个中间值，不会恰好停在 A 自己的终值上），而是**B 的 prefill 根本没有真正执行
到它应该写入的那块缓冲区——它读写的是另一块从未被触碰过的缓冲区，而 `Window::run()`/`Prefill`
实际使用的那块缓冲区，从 A 用完之后就原封不动地留在那里**。

**读代码找到了为什么**：`Window::slots_init`（`window.cpp:2194-2211`）里有两行:

```cpp
v[0]->ss = ss_;
set_of_ss_[ss_] = 0;              // 这一行先把 solo session 登记成 set 0
...
for (size_t b = 0; b < slots.size(); ++b) {
    ...
    set_of_ss_[slots[b]] = (int) b + 1;   // 这一行把传进来的每个 slot session 登记成 set b+1
}
```

**这次改动自己的 bug**：`generate.cpp` 把 `&ss`（`Verifier::init` 已经绑定成 solo session、也是
`Prefill`/`Window::run` 真正读写的那份状态）**同时也塞进了 `slot_sessions[0]`** 传给
`init_slots`。于是上面第二个循环跑到 `b=0` 时，`slots[0] == &ss`，`set_of_ss_[&ss]` 被
**覆盖成 1**——`window_reset(&ss)`（`session_zero` 的 Metal 实现）此后查到的永远是 1，不是 0，
`activate(set_of(&ss)=1)` 激活的是**slot 0 自己的一套独立缓冲区，不是 solo/live 那一套**。
`Window::reset()` 确实把"当前激活"的那套清零了——只是清错了一套，`Prefill`/`Window::run` 真正
读写的 solo/live 缓冲区从来没被清过。

这解释了之前两轮的每一个现象：
- **第一次永远对**：全新进程里 solo/live 和 slot 0 的缓冲区都是刚分配的全零，清哪一套都一样，
  这个 bug 完全观察不到。
- **第二次永远错，和 slot 编号、并发/顺序无关**：只要是"这个 `Verifier` 第二次跑 solo prime"，
  无论这次 entry bridge 把它记成哪个逻辑 slot，`reset_stage` 清的都还是同一个（错的）内部 set，
  solo/live 缓冲区永远不会被真正清空。
- **`predict(pos) == token_at(pos-2)` 这个精确规律**：这次没有单独去复现这个子现象验证因果，但
  given 缓冲区从未被清空、也可能从未被 B 自己的 prefill 真正写入（见下一条），这类"整个计算链条读的
  是上一个会话原封不动的残留输入"的 bug，产出一个和上一个会话的某种确定性函数相关的输出，是完全
  说得通的，不需要再假设"2"和 `--spec 2` 有什么关系——那条线索看来是巧合。
- **`hp_`／`pooled_`／K-V 清零都测不出效果**：因为这几个修复都发生在**错误的那个 set**（slot 0）
  上，从来没有碰到真正在用的 solo/live 缓冲区，清不清都不影响结果。这也解释了为什么这几个方向
  "读代码很有道理，实测却都没用"——不是假设错了，是从一开始就在改错的内存。

**修法**：`ss` 不再出现在传给 `init_slots` 的 slot 列表里——它只属于 `Verifier::init`／
`Prefill::init`，专门服务于每个新会话的 solo priming 这一步（`compute_stage` 的 `!primed`
分支）。`n_slots` 个逻辑 slot（包括原来的"slot 0"）现在全部用**全新 carve** 的 `SessionState`，
一个都不借用 `&ss`。

**验证（真实模型，真实 Metal 计算，和之前两轮同一个 bar）**：

- 顺序测试（A→B→C 三个完全不同的会话，连续在同一个进程里跑）：`[4,5,6,24218]` /
  `[198,10,20,30]` / `[220,248046,871,606]`，**全部和各自独立 solo 跑出来的 ground truth 字节级
  一致**，三个会话都对，不只是前两个。
- 并发测试（A、B 真正交替在途，`--batch 2`）：两个请求都对，互不串流
  （`tools/test_tier1_concurrency.cpp` 风格的"真实 wall-clock 重叠"这次没有重新测，但结果本身已经
  用 token 序列证明了两个 slot 各自保有自己独立正确的状态）。
- 单请求（`--batch 1`）：仍然 `[4,5,6,24218]`，没有回归。
- 两套既有的假 compute 测试套件（`tools/test_tier1_concurrency.cpp` 64 checks、
  `tools/test_boundary_wire.cpp` 191 checks）重新跑，全过，不受影响（这次改动完全在
  `generate.cpp` 里，没碰 `stage_worker.hpp`）。

**这一次，Tier 1.5 在真实模型、真实 Metal 计算、真实并发场景下，第一次做到了完整正确**——不是
"单请求对、多会话还有问题"，是两条路径（顺序复用、并发交替）都对。前两轮加的 `pooled_` 清零修复
继续保留（它本身是独立、正确的修复，CUDA/HIP 的 `qsa_state_zero` 就是这么做的，只是不是这次这个
bug 的根因）；`TECTONIC_DEBUG_COMPUTE`／`TECTONIC_DEBUG_BUFFERS`／`TECTONIC_DEBUG_ZERO_KV` 三个
调试钩子都保留，默认关闭，留给以后类似问题用。

## 架构待决（已回到 Tectonic 侧确认，见上面 2026-11-18 小节）：`gen`/`delta`/`done` 和 BoundaryFrame 是什么关系

这是挡住并发调度层（`run_slot_rows` 接入）的真正问题，不是"不知道怎么写代码"，是"不知道这段
代码该做什么"，所以这次没有自己拍板，留给下一次有权衡依据时再定：

Tectonic 的 `engine.md` 契约里，`gen` 请求带 `tokens[]`/`max_new`/`sampling{}`，`delta` 只带
`request_id`/`token`（一个 token id，不带任何张量）。而这个引擎跨机传递的实际隐藏状态——层切分的
残差 `R`/`bo`/`inj`——只存在于 `BoundaryFrame`（二进制 wire）里，`gen`/`delta`/`done` 的字段里
完全没有地方放它们。两种可能，这次没有证据去选：

1. **`gen`/`delta`/`done` 只是请求生命周期的外壳，不是真正的数据面**：entry stage 的引擎收到
   `gen` 后，还是像今天一样通过 `BoundaryFrame` 和下一跳交换残差（可能是走同一条数据连接，也
   可能是另一条内部连接），`delta`/`done` 只是"这一步选出了哪个 token"的通知，不携带张量。
   —— 如果是这样，并发调度层要做的事情，是在**现有的 `receive`/`send` BoundaryFrame 循环外面**
   包一层 `request_id` 记账，`run_slot_rows` 替换掉现在的单 token `compute()` 调用，`gen`/
   `delta`/`done` 只是这层记账状态机对外广播的通知。
2. **`gen`/`delta`/`done` 彻底取代了逐位置的 `BoundaryFrame` 交换，对所有 stage 都一样**：
   entry stage 算完自己的层后，把结果**怎么**交给下一个 stage 是 daemon（mesh）的责任，不再是
   这个引擎进程自己去 `send()`/`receive()` 一条 `BoundaryFrame`。
   —— 如果是这样，`stage_worker_run` 的整个收发循环要重写成一个不知道"下一跳"存在的纯计算
   服务，边界转发完全移出这个文件，去 daemon 那一层。

两者对 `stage_worker_run` 要改的范围差了一个数量级，而且第 2 种和 `engine.md` 自己写的"engine
只连本机 daemon"、"worker 只连本机 daemon，不知道全网拓扑"这些话按字面看更吻合——但这几句话写
的时候，作者可能假定的就是情形 1（本机 daemon 负责转发，引擎还是要把残差交给它），不能单凭字面
推断。**这个问题需要回到 Tectonic 的设计讨论里确认，不是引擎这边能单方面决定的**，因为答案决定
的是 daemon 侧要不要再转发张量、mesh wire v3/v4 要不要继续存在。

## 2026-11-19（续四）：真实 `tectonicd` 第一次连上真实二进制，发现并修好第六个真问题

前几轮的验证都是用独立测试程序（`tools/test_tier1_concurrency.cpp`、临时的 `tier15_*.py`）直接驱动
`stage_worker_run`，从没有真的让 Tectonic 的 `internal/node.StartWorker` 启动这个二进制、走它自己
构造的完整命令行。这一轮把 `tectonicd`/`tectonic` 两个 Go 命令、这个仓库刚修好的真实二进制
（`09e21ed13`）、真实 Qwen3.8-Flash-Next 模型、真实转换出来的 tokenizer 全部接起来跑了一次。

**发现并修了第六个真问题**：`generate.cpp` 对`--tokens`有一条硬性检查——没传就报错退出
（"--tokens is required"），**这条检查只对 `o.serve` 做了豁免**（`!have_tokens && o.serve` 时填一个
占位 token 继续跑，因为 `--serve` 的 prompt 是每个 HTTP 请求自己带来的，不经过这个 CLI 参数）。
`--tectonic` 同样是"prompt 经 `EntryBridge` 的 `gen` 消息带来，不经过这个 CLI 参数"的情形，但从来
没有被加进这条豁免——和之前的 `--batch` gate bug（`60632197c`）是同一个模式：**`--serve` 已经踩过
的坑，`--tectonic` 接入时没有对照着补上同一个豁免**。真正跑 `internal/node.StartWorker` 构造的、
不带 `--tokens` 的命令行才触发，之前所有测试要么手写命令行带了占位值，要么走的是独立测试程序，
从没真的复现过这条路径。

修法和 `--batch` gate 那次一样：把条件从 `o.serve` 改成 `o.serve || o.tectonic`，复用 `--serve` 已经
验证过的占位值（`o.tokens = {248045}`，这个 pack 里是 `<|im_start|>`，一个安全的特殊 token）。

**验证**：真实握手、真实 48 层模型在 Apple M3 Max 上加载、真实一轮 chat completion 跑通，模型对
"Say hello in exactly three words" 给出的最终答案是"Hello there friend"（确实三个词，`</think>`
推理块之后）——`EntryBridge` 不做 EOS 检测这一条已知、刻意的缺口（见上面的 scope cut 记录）照常
出现：模型越过 `<|im_end|>` 继续编了一轮新对话，这是已经记录过的行为，不是这次发现的新问题。

这是 wire v4／Tier 1／Tier 1.5／真实 `Verifier` 绑定这条工作线第一次有"从 Tectonic 的两个 Go 命令
发起，到真实模型真实产出一个正确答案"的完整端到端证据，不只是某一段链路的真实计算证据。

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
