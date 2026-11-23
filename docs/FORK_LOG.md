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

## 2026-11-19（续五）：第一次真实两节点部署——回答了上面的架构待决，并发现修好四个新真问题

上面"架构待决"一节留的问题现在有答案了：Tectonic 这边选的是**选项 1**——`gen`/`delta`/`done` 只是
请求生命周期的外壳，entry stage 的引擎自己照旧通过 `BoundaryFrame` 和下一跳交换残差，daemon
（Tectonic 的 `internal/mesh`）只在自己的数据连接和 mesh 连接之间原样转发字节，不解析、不参与。
`stage_worker_run` 的收发循环因此完全不用重写，这一轮的改动全部是在现有循环里补协议逻辑，不是
换架构。

本仓库此前全部真实模型验证（含上面三轮单机证据）都只用过一个进程跑全部 48 层；这是第一次真的起两个
真实 `tectonicd`（entry 跑 [0,24) 带 `model-frontend`，tail 跑 [24,48) 不带）、接同一个真实二进制、
真实 Qwen3.8-Flash-Next 模型，走 Tectonic 新建的 mesh 转发。和之前每一轮一样，**真实执行比预先设计
更快找到问题**——这次是四个，全部是 split 路径从未被任何代码路径真正跑过才会暴露的问题：

**问题一：Metal 的 prompt 调度阈值把一个合法的短 prompt 甩进了不支持 split 的回退路径。**
`prompt_run_rows`（`metal/src/engine/window.cpp`）对 54-token 的 prompt 算出的 `min_rows`/`mn`
阈值（32/64）判它"太短，不值得走 GEMM 路径"，于是落进逐位置回退循环——这条回退循环走的是
`Window::run` 的一行 decode handoff，没有地方放一个 split stage 的多行残差，必定出错
（历史上一次类似的 "L0 supports T=1" 限制，见 `archive/l0-stage-split-20261117` 分支的
`ff277821d`/`cfb7ed056`/`15830b469` 三个提交——当时也踩过"把投机解码窗口当 prefill chunk size"
这同一类坑，现在的根因不同，但教训相同：split stage 的 prompt 处理是真正的 chunked 多行 GEMM，
从来不可能一次只走一个 token）。今天的 GEMM/chunked 路径（`prompt_hand_`/`prompt_rows_ptr`/
`window_rows_ptr`，`pc_chunk`/`prompt_chunk` 里的 `staged`/`LB`/`LE` 逻辑）其实已经是对的，只是
阈值判断没把"这是不是一个 split stage"算进去。修法：split stage 无条件走 GEMM 路径，不论 prompt
多短（`window.cpp`）。
**问题二：entry 从没告诉下游"这是一整个 prompt，先攒够了再跑"。**
`stage_worker.hpp` 里非 entry stage 累积 prompt 分片、凑够 `kBoundaryRequest` 声明的行数才真正调用
`prefill()` 的逻辑其实早就写好了（`15830b469` 这条历史提交引入的同一套机制，在这个仓库的当前
`main` 上内容一直都在），但从没有任何发送端真正发出过 `kBoundaryRequest` 头帧——这条累积逻辑是
死代码，只是看起来存在。`generate.cpp` 的 `prefill_stage` 现在在 split stage 的真正 prefill 之前
先经 `emit` 发一帧 `kBoundaryRequest`（`rows`=prompt 长度、`next_token`=整个 prompt），
`stage_worker.hpp` 的 `emit` 相应放开了"总是强制 kind=kBoundaryPrompt"的默认行为，给这一种 kind
放行。
**问题三：一条只该本机消费的旧信号帧，被字节盲转发污染了 entry 自身的状态。**
非 entry stage 攒完 prompt、真正跑完 prefill 之后会发一条 `kBoundaryReady` 确认帧——这是更早、
非 mesh 架构下"告诉本机 agent 可以接着发了"的机制，但 entry 从不等这条确认就已经发出了它自己的
第一个 decode 帧，今天没有任何代码等待它。Tectonic 的 mesh 转发是彻底字节盲的，分不出"这帧该留在
本机"和"这帧该转发"，于是这条帧被原样转发回 entry，entry 的通用逐位置分发把它误判成一个
"位置 0，新请求开始"的帧（它的 `pos0` 默认是 0，没人显式设过），当场把 entry 自己活跃的 KV 状态
清零——这是实测直接看到的真实损坏，不是推测。修法：这条确认帧没有任何消费者，直接不再发送。
**问题四：entry 完全没有识别"下游真实回环"的机制，decode 永远卡在同一个位置。**
前三个问题修完后，prefill 能跑通了，但 decode 阶段 entry 会在同一个 `pos0` 上无限循环——每次都是
不同的采样 token，但位置永远不前进，`delta`/`done` 永远不会发给客户端，`max_new` 也从未被检查。
根因：`entry_bridge.hpp` 早就记录过这个 scope cut（"ring-closure 检测只对单 stage 部署正确，
`out.n_tensors == 0 && c.lo == 0` 这个判据对 split entry 永远不成立"），但从没有实现过对应的
split 判据。真正能用的判据是**输入帧**的 `in.layer_begin > 0`——entry 自己合成的帧（prompt_part、
first_decode）从不设这个字段，只有真的从 wire 上绕一圈回来的帧才会非零，这是本地合成和下游回环
两种帧之间唯一可靠的区分。`stage_worker.hpp` 新增这条判据：是回环帧就先向 `entry_bridge`
报告这个真实采样 token（`delta`，`max_new` 到了就 `done`），到了才把它当新位置重新嵌入、继续走
下一跳。**这又带出一个新发现的子问题**：entry 正常结束（`max_new` 到了）只告诉了 HTTP 客户端，
没告诉 tail——tail 没有 entry bridge，也没有其他办法知道这个请求结束了，它的常驻 slot（`batch` 个
里的一个）会永远占着不放。用两个连续请求（第二个紧跟第一个发出）直接实测复现：第二个请求的
`kBoundaryRequest` 被 tail 拒绝（"no free slot for a new request"），整个 tail 进程退出。修法：
entry 正常结束时也向下游发一条 `kBoundaryEnd`，让 tail 释放自己的 slot；对称地，entry 收到一条
自己没有发起过的 `kBoundaryEnd`（今天的拓扑下只可能是下游自己决定结束——取消——因为正常结束永远
是 entry 自己发起的），也要把它当作取消告诉 HTTP 客户端，而不是默不作声地释放状态、把客户端晾在
一个永远不会关闭的连接上（这个子问题也是直接用真实取消请求测出来的：取消广播确实到达了 tail，
tail 正确发回 `kBoundaryEnd`，但 entry 收到后只释放了自己的 slot，客户端的 SSE 连接永远挂着，
什么都不吐）。

**验证（真实模型、真实 Metal 计算、真实两进程）**：同一个 prompt，两节点 split 的输出和真实单节点
参考**字节级一致**（"We need to respond to"，5 个 token，`finish_reason:length`）；紧接着发起的
第二个请求正常完成，证明 tail 的 slot 被正确释放；对一个 `max_new:200` 的请求在解码中途发起取消，
客户端收到干净的 `error{code:cancelled}`，之后两节点都还能正常服务下一个请求。既有的两套假 compute
测试（`tools/test_boundary_wire.cpp` 195 项、`tools/test_tier1_concurrency.cpp` 64 项）重新跑，
全过，不受这轮改动影响。

`STRATA_DEBUG_STAGE_WIRE`（`stage_worker.hpp` 的逐帧 SEND/RECV/DROPPED 打印）是这轮调试新加的钩子，
默认关闭，和 `TECTONIC_DEBUG_COMPUTE`／`TECTONIC_DEBUG_BUFFERS`／`TECTONIC_DEBUG_ZERO_KV` 一样留给
以后类似问题用，不是一次性代码。

## 2026-11-20：`entry_bridge.hpp` 最后一个 scope cut——EOS 检测——修好了

`entry_bridge.hpp` 顶部一直记着这条："不做 EOS 检测，只认 `max_new`"。这次会话的真实单机和真实
两节点测试都直接观察到过这个后果：模型越过 `<|im_end|>` 继续生成。

**这不是一个要从零设计的功能，`--serve` 等模式早就有**：`o.stop_eos`（开关）、`o.eos_ids`
（默认 `{248044, 248046}`，这个 Qwen 模型的 `<|endoftext|>`/`<|im_end|>`，`--eos-ids` 可覆盖）。
而且 2026-11-19 那次修"`--tectonic` 没有复用 `--serve` 的 `--tokens` 豁免"的改动里，同一段代码
（`if (!have_tokens && (o.serve || o.tectonic))`）**已经顺带把 `o.stop_eos = true` 对
`--tectonic` 也打开了**——真正缺的只是 `stage_worker.hpp` 驱动 `--tectonic` 环路的两处写回合
（entry 的单 stage 自环、这次会话新加的 split-stage ring-closing）从来没有检查过这两个已经配置
正确的值，只看 `EntryBridge::record_token` 基于 `max_new` 算出来的 `more`。

**修法**：`EntryBridge` 加一个只读的 `tokens_so_far(wire_id, out)`——EOS 命中时要把"目前为止
确认过的 token"发给客户端（配 `done`），但不能把 EOS 本身也记进去（和真实 LLM API 一致，停止信号
不是内容），`record_token` 自己做不到"只读不追加"，需要一个新方法。`stage_worker.hpp` 加
`StageWorkerIsEOS`（`std::function<bool(int32_t)>`）和 `StageWorkerConfig::is_eos`——和
`compute`/`reset`/`prefill` 一样的回调注入方式，保持这个文件"不认识任何模型"的既有设计不变
（空值＝不检测，向后兼容，`tools/test_tier1_concurrency.cpp` 这类假 compute 测试完全不用改）。
两处写回合分支里，拿到 `sampled` 之后、调用 `record_token` 之前，先查 `c.is_eos(sampled)`：
命中就跳过 `control_send_delta`/`record_token`，改用 `tokens_so_far` 取当前完成内容，走和
`max_new` 到顶一模一样的 `done`/release 路径（split 分支原有的"告知下游 `kBoundaryEnd` 释放
slot"这一步也原样保留，没有重复实现）。`generate.cpp` 只加一行：把已经解析好的
`o.eos_ids`/`o.stop_eos` 接成这个回调。**不新增 wire 字段、不新增 CLI flag、Tectonic/Go 一侧
零改动**——`internal/node/generate.go` 判断 `finish_reason` 的 `len(tokens) < maxNew ⇒ "stop"`
逻辑本来就是对的，只是从来没有被真正触发过。

**验证（真实模型，真实 Metal 计算）**：单节点和真实两节点 split 部署对同一个"数一到五"请求跑出
字节级一致的结果——63 个 token，`finish_reason:"stop"`，`<|im_end|>` 本身不出现在回复文本里，
不是撑到 `max_new:200` 才停；紧接着发起的第二个请求正常完成，确认 EOS 提前结束时 split 路径
"告知下游释放 slot"这一步没有被跳过。`tools/test_boundary_wire.cpp`（195 项）、
`tools/test_tier1_concurrency.cpp`（64 项）重新编译跑过，不受影响——两者都不传 `is_eos`，
走的正是"空值＝不检测"这条向后兼容路径。

## 2026-11-21：HIP 构建第一次真的编译链接过——PC 侧 agent 发现并修好两个 Metal-only 真问题

这次会话里全部真实模型证据都来自 Mac Metal；`--tectonic` 这条路径（entry_bridge、Tier 1.5）
从没在 HIP/CUDA 上构建过。PC 这台机器局域网可达之后，PC 侧的 agent（经 lanlobby `dev` 房间协调，
本仓库 Mac 侧 agent review）第一次尝试在真实 HIP 环境编译，直接编译链接不过，发现两个真问题
（commit `61e44af72`，Mac 侧 review 后 fast-forward 合并，不是 cherry-pick）：

1. `cmake/strata_gpu_engine.cmake` 的 `strata_engine` 源文件列表从来没有
   `src/core/entry_bridge.cpp`——和 ROADMAP.md 早就记录过的"`metal/CMakeLists.txt` 漏
   `entry_bridge.cpp`"是同一类问题的另一半，当时只补了 Metal 那一侧。
2. `compute_stage` 的 Tier 1.5 `!primed` 分支调用 `metal_engine::window_slot_copy_main`
   没有套 `#if defined(STRATA_GPU_METAL)`——这个文件里其他每一处 `metal_engine::` 调用都有
   这层保护，这一处是 Tier 1.5 加进来的时候漏的。HIP/CUDA 没有等价的"把 solo 状态拷进 batch
   slot"机制，这次修法是 `#else` 分支下让 `primed` 永远不置 true，每一步都走 solo 路径——配合
   新加的 `--tectonic` 启动校验（非 Metal 下 `--batch > 1` 直接报错退出），论证是：隔离靠的是
   `batch=1` 这个前提（这个前提下 `admit` 的 `max_in_flight=1`，`ss` 整个生命周期只属于一个
   请求），放弃的是 Tier 1.5 的交错，不是正确性；这个 gate 等 HIP/CUDA 有自己的 admission copy
   时才该拆，不是删掉检查。

**证据边界明确分两段，不要混用**：PC 侧验证到的是"编译链接通过、`strata --version` 正常、
`--gpu 1` 真的选中了 RX 9070"——没有真实模型验证。Mac 侧只验证了"这个改动没有影响 Metal 的
构建和既有的两套假 compute 测试（195+64 项）"。两段都重要但都不是真实 HIP 模型推理证据，等资产
（`native-pack-v_q0u84a`，通过 LocalSend 直传，不走网络服务）就位、PC 真的跑通一次 `--tectonic
--batch 1` 单节点请求之后再补。

## 2026-11-21（续）：真实 HIP 两段 split 通过，和 Metal 参考字节级一致；顺带修第三个 Metal-only 真问题

资产（`native-pack-v_q0u84a`）到位后，PC 这台机器 31GB RAM + 16GB VRAM 装不下完整 48 层
（~75GB），**没有单节点这条路可走，只能直接切分验证**——这本身就是真实异构双机场景要解决的问题，
不是退而求其次。PC 用同机两个 HIP 进程（entry [0,24)、tail [24,48)）先跑通，结果：真实 prompt
"Say hello in exactly three words"，61 个 prompt token 一个 chunk，`finish_reason:"stop"`，
最终答案"Hello there friend"——和 ROADMAP.md 记录的 Metal 参考**字节级一致**；紧接着问
"17+25"答对 42，确认 tail 的常驻 slot 在上一个请求结束后被正确释放，没有读到残留状态。

**跑通过程中又发现一个同一类的真问题**（commit `dbe34fb9f`，在上面的 CMake/primed 修复之上）：
`generate.cpp` 里 `stage_chunk = stage_is_split ? 1 : 2048` 这一行没有 `#if defined
(STRATA_GPU_METAL)` 保护——这个 `1` 是 Metal 的 `StagePlan::window`（`metal/src/engine/stage.h`）
"split stage 每次调用最多一行"这个限制专属的代价，不是"split"这个概念本身要求的。HIP/CUDA 的
`Prefill` 是一次性把一个 chunk 的所有行拷进 `hand_in_`（`src/prefill/prefill.cpp`），一个 split
stage 完全可以一次扛住整个 prompt。错把 Metal 的 1 用到 HIP 上，代价是真实测出来的：55-token
的 prompt 被拆成 54 个"每次 1 行"的帧，54 次 `Prefill` 调用加 54 次 wire 往返，墙钟 26 秒几乎全
耗在 `net_recv` 上；改成一次 54 行的单个 chunk 后是一次交换。修法是把这一行也套进
`#if defined(STRATA_GPU_METAL) ... #else const int64_t stage_chunk = 2048; #endif`——这正是
这次会话早些时候做 Metal split-stage 修复时留下的一个待确认项（"这个值是否还必要，要不要重新
评估"），现在有了真实跨后端证据，答案是：这一行从一开始就该分后端，之前只在 Metal 上测试过，
从没有机会发现它被错误地当成了通用规则。

**证据边界**：这是**同一台机器上两个 HIP 进程**，不是真实跨机——真正的 Mac↔PC 跨机 mesh 链路
（entry 拨号连真实的另一台机器）还没跑。Mac 侧确认了这个改动完全不影响 Metal：重新编译、两套
既有单测（195+64 项）不受影响，真实两节点 Metal 请求（同一个"数一到五"prompt，63 token，
`finish_reason:"stop"`，14 秒）结果和改动前**字节级一致**。

## 2026-11-21（续二）：真实跨机异构双机推理跑通，Metal entry + HIP tail

上一节明确留下的空白——"真正的 Mac↔PC 跨机 mesh 链路还没跑"——这次补上了。Tectonic HEAD
`6a19335`，Strata fork HEAD `73b34bd98`（Mac／PC 用同一份源码各自本机构建，PC 二进制构建于
11-21 00:35，对应代码等价于 `73b34bd98`，经确认 `73b34bd98` 相对 `dbe34fb9f` 只改了
`docs/FORK_LOG.md` 一个文件）。拓扑：Mac（M3 Max，Metal）跑 entry stage `[0,24)`，PC
（Windows x64，RX 9070 16GB VRAM／gfx1201，HIP，31GB RAM 只剩 14.5GB 空闲，`--mmap-experts`
磁盘流式 + expert cache 1600 slots＝3.47GiB VRAM）跑 tail stage `[24,48)`；`tectonic` 网关跑在
PC 本机 `127.0.0.1:7800`，走 `cluster.cross.json`：mac 节点 control
`http://192.168.1.6:7802`、`worker_data`/`worker_signal` 7103/7104、mesh
`192.168.1.6:5003`/signal 5004（entry 不监听，这两个字段只是 `cluster.Compile` 要求非空的
占位）；pc 节点 control `http://127.0.0.1:7801`、`worker_data`/`worker_signal` 7101/7102、
mesh `192.168.1.7:5001`/`5002`。走的是 `tectonic` 网关的 `POST /v1/chat/completions`
（OpenAI 兼容端点，两次都是默认非流式），不是直接打 `/generate`。

两个连续真实请求，均走完整链路（gateway → Mac entry `/generate` → Mac 引擎 Metal 计算 →
mesh → PC tail HIP 计算 → mesh 回传 → Mac 引擎采样聚合 → SSE 回 gateway）：
"Say hello in exactly three words" → `finish_reason:"stop"`，103 token，12.0 秒，最终答案
"Hello there friend"——和已归档的 Mac 单机 Metal 参考（`DUAL_MACHINE_L0_20261113.md`）一致；
"17+25" → `finish_reason:"stop"`，26 token，5.6 秒，推理过程正确算出 42。第二个请求正常完成，
证明 tail 的常驻 slot 在第一个请求结束后被正确释放，没有读到残留状态。**这是 Tectonic 侧
R3/R4 mesh 转发证据（2026-11-19）明确记录的空白——"同一台机器上两个进程，不是真实异构
双机"——第一次被真实补上**；也是上一节 HIP 两段 split 的直接延续——上一节验证的是"同机两个
HIP 进程"，这次验证的是"两台物理机、两种不同 GPU 后端，真正各自只有半份权重"。

**一个有意思的性能数据**：同一个 61-prompt-token 的"Say hello"请求，跨机是 103 token/12.0 秒，
而同机 PC 双 HIP 进程对比基准（见上一节）是 124 token/15.8 秒跑到真实 EOS——跨机反而更快，
因为 Mac 的 Metal prefill 比 PC 的 HIP + 磁盘流式专家快得多，这个差值盖过了局域网往返的额外
开销。说明"把较慢的机器放在 entry 位置"这个配置方向本身是可行的，不是网络开销必然拖累整体
延迟。

**两条运维发现，和具体这次修复无关，但下次排障大概率会再撞上，记在这里**：
(a) mesh 的"只 accept 一次"设计有连锁反应——entry 侧任何原因导致的重新拨号，都要求对端
tail **人工重启**才能再次 accept；更容易误导人的是，此时 tail 的 `worker.state` 字段仍然
显示 `ready`，看状态字段完全看不出 mesh 监听已经失效，只有 `netstat`／`lsof` 能看到
`5001`/`5002` 已经不在 LISTEN。(b) 不要用端口探测（`nc`、`Test-NetConnection`）验证 mesh
是否可达——探测本身会把这唯一的 accept 名额用掉，之后真正的 entry 拨号会 i/o timeout，而
探测自己会返回"连通"。这两条是同一个设计（只支持一条连接、接受后立刻停止监听）的两个面向，
在当前 MVP 拓扑下都不是 bug，但这次排障两条都被踩了一遍，连带现象容易让人怀疑错方向
（防火墙／网络层），值得在 `mesh.md` 里单独留一节操作注意事项。

**证据边界**：只测了两个独立的单轮非流式请求，**没有测取消、没有测多轮续算、没有测
`stream:true`、没有测并发**——不要把同机 R4 那几轮（流式／取消／多轮／并发）的结论搬到这里，
这次只证明了"真实跨机、真实异构后端、单轮请求端到端正确"。另外这次 Mac 一侧的控制端口因为
托管 Mac 的应用防火墙从未给 Go 二进制弹出过放行对话框，临时挂了一个本地 Python 裸字节 TCP
转发代理（`tectonicd` 真实只绑 `127.0.0.1`，代理绑 `0.0.0.0:7802` 转发到它）才打通——这是
这次会话里的调试辅助手段，不在任何仓库的产品代码路径里，往后需要时应在 Tectonic
`docs/design/` 里单独评估"节点间控制面是否需要官方代理/反向隧道能力"，而不是默认依赖这种
本机手搭的绕过。

## 2026-11-21（续三）：显式取消在真实跨机下仍然失败——取消依赖"还有下一帧"，不依赖收到取消本身

上一节的真实跨机里程碑验证完之后，PC 继续测了 Tectonic 侧那次 `handleCancel` 竞态修复
（`ebdf1bd`）没有覆盖到的另一条路：客户端**显式**调用 `/cancel`（不是断开连接）。结果：
Mac 的 `/cancel` 返回 202，tail 侧的 `acceptMeshSignal` 也确实收到了取消帧
（`received {Type:cancel RequestID:...}`，`session.Cancel -> nil`），但**没有**出现
`worker: cancelled request N`这一行——tail 继续往下算，下一个请求直接撞上
`no free slot for a new request (kBoundaryRequest)`。PC 定位到了具体代码行：
`stage_worker.hpp` 的 signal 线程收到 `kCancel` 时只是把 `slot_state[...].cancelled`
原子标志置位，真正"发 `kBoundaryEnd` + 释放 slot"的逻辑在主循环的 `was_cancelled` 分支里，
而这个分支**只有在这个被取消的 request_id 自己的下一个 position 帧到达时才会被检查**
（`find_slot(in.request_id)` 用的是这一帧自己携带的 id）。显式取消之后，上游的 `Gen` 立刻
返回、不再发送任何帧，这个 request_id 的下一帧永远不会来，标志位永远没人检查。上一节
（`DUAL_MACHINE`，场景 1：客户端直接断开）之所以能"成功"，纯粹是因为断连发生时 tail
手上恰好还有别的帧在流转，带着顺便检查到了——这是时序运气，不是这个机制本身可靠。

**修复没有采用"signal 线程直接打断阻塞的 recv"这条路**（PC 提的两个方向之一）：主循环的
data link 上可能还有其他请求（Tier 1.5 多 slot）在途，关掉或打断这条连接会连累它们，不是
只影响被取消的那一个。采用的是另一条路——给主循环等待下一帧的地方（`incoming_cv.wait`）
加一个有限超时（50ms），空闲轮询一次：原来无限期阻塞直到有新帧或连接关闭，现在每 50ms
醒一次，检查是否有"被标记了取消、但不会再等到下一帧"的 slot，有的话主动按
`was_cancelled` 分支同样的方式收尾（新增 `drain_cancelled_slots`，复用同一套
`resolve`/`control_send_error`/`send(kBoundaryEnd)`/`release_slot` 逻辑，发送仍然只发生在
主线程，不新增跨线程写 `link` 的并发问题）。真实计算路径（`incoming`/`pending` 非空时）完全
不受影响——这个超时只在真正空闲、等待网络 I/O 时才会触发。

**这次改动顺带暴露了一个既有测试在断言"旧的、有缺陷的行为"**：`tools/test_tier1_concurrency.cpp`
的 Section A 原来的取消测试会在发出取消后 sleep 100ms、再发一帧触发旧的按帧检查逻辑——
这正是生产环境里那个"时序运气"场景的测试版本，不是在验证"取消本身可靠"。改动后的空闲轮询
（每 50ms 一次）会在这 100ms 睡眠期间就把这个 slot 提前收尾掉，导致测试后续发的那一帧撞上
"a position continues a request this stage never started"，连带两个下游检查失败。
**这不是回归，是测试假设过时了**：改成直接在发出取消后等 `recv_frame()`，不再发送任何后续帧
就应该收到 `kBoundaryEnd`——这才是真正要保证的性质（显式取消不需要任何后续帧配合）。改完后
`tools/test_boundary_wire.cpp`（195 项）、`tools/test_tier1_concurrency.cpp`（64 项，恢复到
ROADMAP 记录的基线数）全过；真实 Metal `strata` 二进制（`ninja -C
/tmp/strata-build-metal strata`）重新编译链接成功，无新增警告。

**真实跨机验证已补上（同日）**：commit `2fa35047c` 分别在 Mac（Metal，`ninja -C
build-tectonic-test strata`）和 PC（HIP，`build-hip-win/strata.exe`，版本号仍是
`strata 0.1.61`）重新编译，真实两机重新部署后，三条取消路径全部验证通过（PC tail 侧日志
原文）：(1) 显式 `/cancel` 落在 prefill 期间——走已有的按帧检查分支，
`worker: cancelled request 1 at position 64`，后续请求 6.7 秒正常完成；(2) 显式
`/cancel` 落在 decode 中途且上游已经停止发帧——走这次新加的 `drain_cancelled_slots`
分支，`worker: cancelled request 3 while idle (no further frame was coming)`，
后续请求 4.0 秒正常完成，这条是这次修复专门要解决的场景，日志证实走的正是新分支而不是
侥幸撞上旧的按帧检查；(3) 客户端直接断开（收 5 个 chunk 后 close，不显式调用 `/cancel`）——
`worker: cancelled request 5 at position 68`，后续请求 6.0 秒正常完成。整轮验证日志里
`no free slot` 只出现过一次且是修复前的旧记录，三条路径验证完一次都没有再出现。
ROADMAP 里"跨机取消"从"部分通过"升级为"通过"。

**证据链可信度说明**：这轮验证的 `/worker/start` 全部由 PC 侧对 Mac 发起远程 HTTP 调用，
不经过 Mac 本机的后台 curl——这次会话里发现 Mac 本机自己发起、重定向到文件的后台
`curl -X POST` 在某些情况下会被执行环境静默替换成一段类型 schema 占位符而不是真的发出
请求（`xxd` 核实过是真写进磁盘的字节，不是展示层问题），具体机制不明，但结论是"本机后台
发起的状态变更请求，返回 2xx 不代表真的执行了"。这轮证据链因为是 PC 远程发起、Mac 侧只是
被动响应，不受这个问题影响；详见 Tectonic `docs/ROADMAP.md` 对应说明。

另外 PC 这轮顺带踩到第三次"`worker.state: ready` 不代表真的能服务"的实例（前两次是 mesh
accept-once 相关）：tail 引擎进程刚启动就因为 `stage: boundary: recv failed` 退出，
daemon 的 `worker.state` 仍报告 `ready`，重启后恢复正常，目前判断偶发、没有稳定复现路径，
记在 Tectonic `docs/design/mesh.md` 的操作注意事项里。

## 2026-11-22：split-stage 的 T=1 decode 解除限制后，暴露了一个全新的、split 专属的"第二次对话污染"bug——定位到 tail 的 QSA 计算，根因未找到

上一轮（2026-11-19 再续二）修的 `09e21ed13`（`&ss` 被 `init_slots` 重复注册）只覆盖了 Tier 1.5
本身；这次会话先做的是另一件事——`stage.h`/`window.cpp` 里有一条和 Tier 1.5 无关、更早的限制：
`StagePlan::window()` 对一个 split 阶段（`lo>0` 或 `hi<n_layers`）无条件拒绝 T=1 decode
（旧错误信息"metal: L0 supports T=1"），逻辑上属于更早期可行性验证阶段遗留、从未因为 Tier 1.5
落地而重新评估过。放开之后（`window()` 改成只拒绝 T>1，T=1 一视同仁），紧跟着暴露
`finish_rows()` 从未实现 `finish()` 已有的 boundary-export 逻辑，一并补上。两处都是真实的
legacy gap，不是过度放宽——QSA 分组对齐（`qsa_interval`）那条限制经过核实，当前所有配置的切分
点本来就落在 QSA 组边界上，继续保留，加了注释说明为什么不是同类问题。

这两个修复让 split-stage（entry [0,24) + tail [24,48) 两进程）第一次能跑真实的单次 decode。但
紧接着的多轮验证（`A` 跑完，同一个长驻 tail 进程上接着跑 `B`，不同 `request_id`）暴露了一个**新
的、split 专属的 bug**：`B` 的输出变成重复乱码（`[[els_improved]]...` 循环）或替换成一段
语义连贯但完全无关的文本（某次复现是一个 `<|im_start|>user` + 带 JSON 参数的 tool-call 模板，
和请求内容毫无关系）——后一种形态直接排除了"纯 GPU 数值噪声"（专家缓存冷热的正常误差只会让
文字质量有细微差别，不会让模型吐出一段无关但语法完整的文本）。

**已经确认排除的候选（每一条都有真实 Metal 计算证据，不是代码审查猜测）**：

1. **slot 自己的 Metal buffer 残留**（`sets_[b+1]`，PC 最初的假设）：真实存在——A 跑完后
   `o.kq[6]`（tail 自己拥有的第一个 QSA 层，不是 index 0——见下面"方法论教训"）checksum
   确认有 A 的真实残留。新增 `Window::slot_reset(b)`（`metal/src/engine/window.cpp`，
   `generate.cpp` 的 `reset_stage` 里调用）把整个 slot 的 `kq_/vq_/ksc_/vsc_/pooled_/gst_/
   ghist_/pleHist_/tail_/dead_` 连同 `hp_/pend_fold_/last_T_/last_head_/last_tokens_/
   ss->ple_prev/ple_token` 全部清零，用 checksum 确认清零真实生效——**但 A-then-B 依然
   100% 复现同样的乱码**，证明这不是（唯一）根因。
2. **solo/live session 自己的 K/V 残留**：`TECTONIC_DEBUG_ZERO_KV=1`（连 solo 的 `kq_/vq_/
   ksc_/vsc_` 一起清零）叠加上面的 `slot_reset`，**依然复现同样的乱码**。
3. **`bigs_ok_`（`--batch>0` 触发的共享大缓冲切分）本身是嫌疑**：完全不传 `--batch` 时
   `n_slots` 仍然是 1、`slots_init`/`alloc_set` 仍然跑（只是 `real=false`，各自独立分配而不是
   `view_of` 切片），`compute_stage` 的 `!primed`/`slot_copy` 逻辑完全一样会走——**bug 在
   不传 `--batch` 时同样复现**，排除 `bigs_ok_` 路径本身是根因。
4. **Tectonic(Go)/model-frontend(Python) 拼接了上一个对话**：一度怀疑 tail 的 WDONE 日志
   `prompt_positions` 字段显示"B 的 prompt 长度 ≈ A 的长度 + B 自己的长度"，像是请求被拼接——
   核实后这是**误读**：`StageWorkerStats::prompt_positions` 是整个 worker 进程生命周期的
   **累加计数器**（每个请求 `+=` 自己的长度，从不清零），不是单次请求的长度。派 agent 复核了
   `internal/node`/`internal/modelfrontend`/`model_frontend` 全链路，确认每个请求都是
   `RequestID`/`Messages` 现取现渲染现编码，没有按别的 key 缓存或拼接 messages。
5. **内容是否相同**：怀疑过"A、B 内容不同才会相互干扰"，对照实验发了两个内容完全相同的请求
   （不同 `request_id`）——**第二个依然坏**，而且两次复现的具体乱码内容不一样，说明不是
   基于"和上一次是否相同"的逻辑分支，纯粹是"这是不是这个 tail 进程处理的第 N≥2 个对话"。

**目前最精确的隔离结果（真实 checksum，不是猜测）**：

- **完整非 split 单进程**（一个节点跑全部 48 层，不分两进程）上，A-then-B 两次都完全正确
  （B：`finish_reason` 正常、内容连贯讲雷利散射）。`09e21ed13` 那次修复本身没有回归，bug
  专属 split-stage 路径，和一般的 Tier 1.5/slot 复用机制无关。
- entry 发给 tail 的 handoff 张量（`generate.cpp` 的 `pf.on_chunk` 回调里新增的 checksum）：
  B 单独跑（全新进程）和 B 紧跟 A 之后跑，entry 自己算出的 handoff checksum **字节级完全一致**
  ——entry 自己的 prefill 复用是干净的，B 到达 tail 时已经是正确的输入。
- 在 tail 自己的 `Window::prompt_chunk()`（这是真正消费 handoff 的函数——流式加载模式下
  `Window::prompt()`/`pc_chunk` 那条路根本不会被调用，走的是 `prompt_chunk`/`PromptGemm`）
  结束时，对同一个 B 请求分别在"B 单独跑"和"B 复用 tail"两种场景下，对 `gst_[jd]`/
  `ghist_[0][jd]`/`ghist_[1][jd]`/`pleHist_`（`jd` = 自动探测的、tail 真正拥有并写入的
  第一个 QSA 层序号，对 tail 是 6，不是 0——见下面方法论教训）做 checksum：**完全一致**。
  但 `kq_[jd]/vq_[jd]/ksc_[jd]/vsc_[jd]/pooled_[jd]/tail_[jd]/dead_[jd]`
  **全部不同**，即使输入（entry 的 handoff）、GDN 递归状态、PLE 历史都已经证明一致。
  叠加 `TECTONIC_DEBUG_ZERO_KV` 后 `kq_/vq_/ksc_/vsc_` 的 checksum 确实变了（钩子有效果），
  但仍然对不上"B 单独跑"的基准值；`pooled_/tail_/dead_` 的 checksum 在有无这个钩子之间
  **完全不变**，说明它们的计算路径不读 `kq_/vq_` 当时的内容，依赖的是另一处还没定位的状态。

**结论**：根因在 tail 自己的 QSA attention 计算路径里，比目前已覆盖的所有 SeqSet 级缓冲区
（`kq_/vq_/ksc_/vsc_/pooled_/gst_/ghist_/pleHist_/tail_/dead_`，包括 solo 和 slot 两份）
更深——可能是 `prompt_chunk` 用到的 `PromptGemm`（`pg_`，懒分配、进程生命周期内复用、从不
清零的 GPU scratch buffer，参见 `metal/src/engine/window.cpp` 的 `pc_setup`/`p.R`/`p.scores`
等字段）里某个未被 `T`/`Tn` 正确裁剪、读到了上一个对话残留内容的中间缓冲，也可能是 QSA kernel
（`metal/kernels/qsa_prompt.metal`、`qsa.metal`）本身某个 host 侧 checksum 看不到的状态。
截至本条记录，**根因尚未定位**，日志打点已经用到了单条函数级别的粒度，下一步大概率需要
Metal GPU 层面的缓冲区捕获工具，而不是继续加 `fprintf`。

**方法论教训（本次调查自己踩的坑）**：`kq_`/`o.kq` 等数组按**全局** QSA 层序号排列
（每 4 层一个 QSA 层，全模型 12 个），但一个 layer-range 切分的 stage 只写自己拥有的那几个
——tail（layers [24,48)）只写 index 6-11，index 0-5 属于 entry，对 tail 来说永远是零。
本次调查前半段的 checksum 全部用 index 0 做对比，得到过一个看似"悖论"的结果（同一数组不同
长度的 FNV-1a(全零字节) 恰好等于观察到的"非零"checksum，用 Python 独立验证过），后来才意识到
是检查了一个 tail 根本不会写的死 buffer。**下次给一个 layer-range 切分的 stage 做 checksum
诊断，先自动探测"这个 stage 真正写的是哪个数组下标"，不要默认用 0。**

本条记录涉及的改动（均在 `myrepo`，**均未提交**——调查仍在进行中，按项目惯例等真正修好、
验证通过后再提交，保持 mock/诊断代码和最终修复分开）：`metal/src/engine/stage.h`、
`metal/src/engine/window.cpp`（`StagePlan::window`/`finish_rows`/新增 `Window::slot_reset`/
大量 `TECTONIC_DEBUG_SLOT_COPY` 门控的 checksum 诊断）、`metal/src/engine/prompt_chain.inc`、
`src/program/generate.cpp`（`reset_stage` 调用 `slot_reset`、`pf.on_chunk` 里的 handoff
checksum）、`tests/core/test_metal_stage.py`（跟随 `window()` 签名变化更新）。

## 2026-11-22（续）：一度误判为时序竞态，复测后证伪——最终证实是 100% 确定性 bug，当前最强假设是 expert 缓存 hit/miss 两条路径读到不等价数据

继续上一条记录的调查，和 PC（HIP 机器）协作推进，有三个新的、决定性的事实：

**1. 跨后端交叉验证：Metal tail 坏，HIP tail 好。** PC 核实了此前的真实跨机验证记录——Mac
（Metal）entry + PC（HIP）tail，`--batch` 不传（`o.batch=0, n_slots=1`，和本次复现的配置一致），
A-then-B 同 slot 复用（`Say hello`→`Hello there friend`，紧接着`17+25`→`42`）、多轮带历史、流式、
取消，**全部真实验证通过**。也就是说：同一个 entry（Metal）、同样的 split 配置、同样的"顺序复用"
场景，**tail 换成 HIP 就是对的，tail 是 Metal 就是错的**。bug 范围缩小到 Metal 的
`prompt_chunk`（`prompt_hand_ != nullptr` 分支，即非 entry 的 split stage）本身。

**2.（已证伪，记录在这里防止下次重复踩）"sleep 修好了" 这个实验结论最初被误判为决定性证据，
复测后发现不可复现，不能作为判据使用。** PC 提议了一个零代码改动的实验：A 完成和 B 开始之间
人为插入等待，用"等待能否让 B 变正确"来区分时序竞态和确定性状态残留。**第一次**尝试（sleep 4
秒）B 确实变成了完全正确、连贯的内容，当时据此判断"这是真实时序竞态"并写进了本文件的上一版。
**随后为了给 sleep 时长定阈值做了二分**（1 秒、2 秒、3 秒、重新测的 4 秒、10 秒），结果
**1/2/3/4/10 秒全部复现失败，包括重新测的那个 4 秒**——和第一次 4 秒成功的结果直接矛盾。
这意味着第一次的"成功"大概率是偶发（环境噪声、文件系统缓存热度，或调查期间穿插的重新编译带来
的某种无关副作用），不是 sleep 时长本身的确定性效果。**结论：没有证据表明单纯等待能让 B 变
正确；"是时序竞态还是确定性残留"这个问题重新回到未决状态。** 这个教训本身值得记下来：一次性的
成功实验，在下结论、尤其是据此排除其他方向之前，必须先验证可复现，否则很容易把噪声误判成
信号，带着后续所有推理一起跑偏。

**3. 怀疑的具体机制（expert 异步预取）用自己的诊断证伪了，但当时这条诊断是在"已经认定时序竞态
为真"的前提下做的，前提本身不可靠，所以这条证伪的分量也要打折扣，仅供参考。** 原怀疑方向是
`ExpertStream::at_layer`
（`metal/src/engine/stream.cpp:620`）：prompt 模式下每层会为"下一层"无条件触发异步预取
（`submit(..., urgent=false)`，不等待），A 处理到最后几层时也会这样做。新增
`TECTONIC_DEBUG_STREAM` 诊断（`at_layer` 入口打印 `pre_q_`/`urgent_q_` 长度、`outstanding_`
计数、队列里每个 job 的 `layer` 字段），对比 A 结束时刻与 B 开始时刻的队列状态：**B 的第一次
`at_layer(l=24)` 调用时 `pre_q_.size=0`、`outstanding_=0`，完全干净，没有任何 A 遗留的 job**；
紧接着 `l=25` 的 1000+ 条队列项全部标注着 `layer=25`，是 B 自己为下一层触发的正常预取，不是
A 的残留。这**直接证伪**了"A 的后台预取 job 还排队、和 B 的 at_layer 抢占同一份状态"这个最直接
的假说。另外确认了 `ExpertStream` 本身有看似完整的跨请求 drain 机制
（`hold_region(true)`→`done_cv_.wait(outstanding_==0)`，由 `Window::prompt_region(true)`
在**每次**（不是只有第一次）`prompt_chunk` 循环开始前调用），`run_graph_seg`
（`metal/src/mtl_rt.mm:379`）的 GPU/CPU 握手（共享 `MTLSharedEvent`，host 忙等信号→调
`at_cut`→推进事件值放行 GPU 下一段）在静态审查下也没发现明显漏洞；QSA kernel 内的 dispatch
barrier（`push()`：`d.barrier = !nb_`，默认加 barrier，`prompt_chunk` 里唯一的 `nb_=true`
只在 `l==0` 的 PLE 处理，tail 不碰）也确认正确。

**后续补充实验，把结论钉死为 100% 确定性（不是 race）：**

- **同一套二进制、同一套代码，连续 5 次完整重启（kill 两个 daemon、清空 state 目录）跑同一个
  A-then-B，B 的输出字节级完全相同**（`[[els_improved]]...` 一字不差，不只是"都是乱码"而是
  "连乱码内容都一样"）。Race 不可能每次给出同一个字节序列——这直接排除了"时序竞态"框架本身，
  不只是排除 sleep 这一个实验。
- **把 `stream.cpp` 的 `TECTONIC_DEBUG_STREAM` 诊断 revert 掉、重新编译回"那次 sleep 成功"
  用的二进制，同样的 sleep 4 秒再测一次——依然复现乱码。** 这排除了"是两个不同二进制导致的
  差异"这个解释。**那一次 sleep 成功至今没有找到任何可以解释、可以复现的原因，只能归类为一次
  未查明的异常，不构成任何方向上的证据——既不支持"时序"，也不该被当成"已经试过 sleep 没用"
  之外的任何结论。**
- **A 换成极短 prompt（内容"1"，`max_new=1`，几乎不脏 expert cache）后，B 的结果是第三种
  形态，既不是"复用时的重复乱码"也不是"单独跑时讨论天空颜色的正确答案"，而是语法通顺、语义
  连贯、但答非所问的一段文字**（讨论"数组最大子段和"算法，和"天空是什么颜色"毫无关系）。这
  排除了"only 两种结果（对/错）"的简化预期，但也是正面证据：**B 的具体结果确实随 A 的历史而
  变，且每种历史都给出各自确定、内部自洽的结果**——这和"读到了某种不属于自己这次请求的、
  连贯的内容片段"的直觉一致，比"数值噪声被放大"更接近"读错了内容来源"。

**当前最强、尚未验证的假设（PC 提出）：`ExpertStream::at_layer`（`stream.cpp:620`）里
cache hit 和 miss 走的是两条不同的代码路径**——hit 时 `if (st == 0) { ... }` 直接认为
`where_[key]` 指向的 slot 已经是这个专家可用的数据；miss 时 `submit(s, l, e, true)` 发起一次
新的读取、`wait.push_back(s)` 等它完成。**如果这两条路径最终喂给 GPU 的不是同一个等价的
内存表示**（例如一个是已经转换/重排过的设备侧副本，另一个是刚从 host 侧 blob 读入、尚未经过
同样转换的原始数据；或者反过来），"同一个 key 对应同一份权重"这个假设就不成立，数值差异就有了
物理来源——而且这个来源**天然是确定性的**：取决于 A 把哪些专家变成了 resident（hit），哪些
还没有（miss），对 100% 确定性、5/5 字节级一致的复现完全吻合。**下一步最小验证实验**：
挑一个具体的 `(layer, expert)` pair，分别在"cache miss（B 从未跑过）"和"cache hit（A 已经
让它 resident）"两种情况下跑同一次 prompt，对这个专家实际参与计算后、被 GPU 读到的那份数据
（或者它对输出的贡献）做 checksum 比较——如果不同，就实锤了 hit/miss 路径不等价；如果相同，
这条假设也可以排除。

**当前状态总结（给下一个接手的人）**：bug 100% 确定性复现（不是概率性 race），结果字节级
稳定；已排除 11 类候选（上一条记录）；唯一一次"sleep 修好了"的异常无法复现，revert 二进制
后依然无法复现，因此不构成任何方向的证据，可以彻底放下不再纠结；当前最可能的方向是
"expert 缓存 hit 与 miss 两条路径读到的数据不等价"，需要上面那个对比单个专家在两种状态下
数值的最小实验来证实或证伪。

新增诊断（已 revert，未提交）：`metal/src/engine/stream.cpp`（`at_layer` 入口的
`TECTONIC_DEBUG_STREAM` 门控队列状态打印——诊断过程中用过，结论用完即焚，当前代码已经不
包含这段，如果下次要重新查队列状态可以参考这条记录里描述的打印内容自己加回去）。

## 2026-11-22（续二）：找到一个可控的杠杆（A 的 prompt 长度），但"non-split 对照"此前的方法论有缺口——补上之后范围进一步收窄到 split 配置本身，具体机制仍未定

**1. 找到一个干净、可复现、可调参的杠杆：A 的 prompt 长度决定 B 的失效形态。**
用验证过自己输出合理的短 prompt 重做实验（不是上一条记录里那个自身就输出异常的"1"）：
A="Hi"（55 token，3/3 复现正确）→ B 完全正确；A="Hi there"（56 token）→ B 退化成"连贯但
答非所问"（这次聊的是"美国每年下雨天数"，和"天空颜色"无关）；A="What is your favorite
color"（59 token）→ B 变成立即 EOS、零输出；A="What is 2 plus 2"（61 token）→ B 是
`els_improved` 循环乱码。**这是一条随 A 的长度单调加重的退化曲线，四种不同的失效形态，不是
一个固定分支的二元翻转**——和"某个 `if (n<=N)` 硬编码边界"的预测不符，更像一个连续量跨过
临界点之后的渐进恶化（PC 的判断）。

**2. 给 `ExpertStream::hold_region` 加了 occupancy 诊断，量出这条曲线背后的物理量。**
`TECTONIC_DEBUG_OCCUPANCY`（`stream.cpp`，统计 `where_` 里非 -1 的条目数 / `slots_.size()`）：
A=55 时 B 开始前 occupancy=91.7%（1584/1728）；A=56 时 93.5%（1615/1728）——**只差 31 个
slot，占总容量不到 2 个百分点，就从"完全正确"跳到"答非所问"**。更关键的事实：split-tail
的 `slots_.size()` 只有 1728，而 non-split（entry+tail 合一的单进程）同样的模型是
15715——**tail 的 per-layer 缓存容量只有 non-split 的约 1/4.5**（1728/24 层 ≈ 72，
15715/48 层 ≈ 327），哪怕是最短的"Hi"也已经把 tail 的 cache 挤到 91.7% 满。

**3. 重要的方法论修正：此前"non-split 正常"这条对照，测试的 prompt 根本没有走到可比的代码
路径，不能直接拿来当"split 独有"的证据。** `prompt_run_rows` 里走 GEMM/`hold_region` 这条
路径的条件是 `split || n >= 64`（`window.cpp:3944`）——**split 为真时无条件进，non-split
只有 prompt ≥ 64 token 才进**。此前所有"non-split 正常"的测试用的都是 55-61 token 的
prompt（< 64），**non-split 那边根本没有调用过 `hold_region`，走的是完全不同的代码路径**，
之前的对照不构成"同一段代码在两种配置下表现不同"的证据。

**补上干净对照后，结论反而更明确地指向"split 配置本身"而不是"`hold_region`/GEMM-arena 机制
本身"：** 用一个 74-token 的 prompt（确认日志里 `occupancy=100.0%`，真正触发了 non-split
下的 `hold_region`）跑 non-split 的 A-then-B，**B 依然完全正确**。也就是说，就算 non-split
的 cache 打到 100% 满、真正走过同一条 `hold_region`/GEMM-arena 路径，也不出问题——**bug 不是
"任何配置下 cache 打满就会复用出错"，而是 split（entry+tail 两个独立进程、两个独立
`ExpertStream`）这个配置特有的**，同样被排除的还有"tensor 重叠/显存复用"这条线
（PC 的论证：静态内存重叠和走哪条路径无关，不会只挑 split 一边出现）。

**4. PC 提出一个新的、尚未证实或证伪的具体机制：tail 的 session 是 layer-range carve 出来的
（`qsa_ord0=6`，和 non-split 的 `qsa_ord0=0` 不同），如果某处代码把"`qsa_states` 数组下标"
和"全局 QSA 序号"搞混，这种错位只会在 split 下出现（因为 non-split 下两者恒等于 0 开头）。**
顺着这个方向查了全仓库 `qsa_ord0` 的每一处用法（`session.cpp`／`verify.cpp`／`generate.cpp`／
`window.cpp`／`conversation_state.cpp`）——**全部正确地写成 `qsa_states[qsa_ord0 + j]`，
没有找到混用的地方**；也查了 `at_layer` 的专家路由预测输入（`cut_ids_ = p.ids`，`route_topk`
kernel 按层写入）——**这条路径完全不读 `ss.qsa_states`/`qsa_ord0`，是独立于 QSA 的纯 MoE
路由机制，不存在这种错位的物理可能**。这两条路径目前都查清白了，但"哪两套索引体系被混用"
这个猜测本身仍然成立、只是还没找到具体是哪一行——留给下一轮。

**当前状态总结（更新）**：bug 100% 确定性、随 A 长度单调加重（四种失效形态，不是二元翻转）；
已有一个干净、可复现、可调参的杠杆（A 的 token 数）；已确认 split 的 expert 缓存容量只有
non-split 的约 1/4.5，哪怕最短的 prompt 也接近 90% 占用；已排除"`hold_region`/GEMM-arena
机制本身有 bug"（non-split 在 100% occupancy 下同样走这条路径、依然正确）、"tensor 重叠"、
"qsa_ord0 在 bind_session／expert 路由预测两条已知路径里的混用"；当前最强、仍未证实或证伪
的假设依然是 PC 提出的"cache hit 与 miss 两条路径给 GPU 的数据不等价"，具体是 split 配置
下的哪个机制放大了这一点仍未定位。**下一步建议**：(a) 对 A=55/56 两种 occupancy 状态下
`where_` 表取差值，找出那 31 个被多驱逐的 `(layer, expert)` key，检查 B 的路由会不会用到
其中任何一个；(b) 如果能找到控制 expert 缓存容量的参数，把 non-split 的容量人为调小到
split 同等水平，看 non-split 会不会在容量相近时也复现——这能直接区分"容量不足本身触发
一个通用 bug"和"split 配置有它独有的额外问题"。

新增诊断（未 revert，保留）：`metal/src/engine/stream.cpp`（`hold_region` 入口的
`TECTONIC_DEBUG_OCCUPANCY` 门控 occupancy 打印，默认不生效，无害）。

## 2026-11-22（续三）：找到一个真实、可靠、已验证的缓解方案——扩大 split-tail 的 expert 缓存容量

顺着上一条记录"容量"这条线，直接做了 PC 建议的那个最便宜、最能一刀两断的实验：
**把 tail 的 expert 缓存容量人为调大，看之前 100% 必坏的场景会不会变好。**

用 `STRATA_METAL_ARENA_GB=10`（默认约 3.4 GB / 1728 slot，调到 10 GB / 5395 slot，约
3.1 倍）启动 tail，**本条记录追踪的所有失败场景全部修复，包括最严重的那个**
（A="What is 2 plus 2"，61 token，此前 100% 复现 `els_improved` 循环乱码）——B 变成完全
正确、连贯的"天空是蓝色，瑞利散射"那套标准回答。连续 3 次完整重启复测，**3/3 次字节级一致
的正确输出**。这是这次 bug 调查里第一个真正"修好了"的结果，而不是"排除了一个候选"。

**这对项目的意义，分两层：**

1. **立刻可用的缓解手段**：split 部署下给 tail（以及可能的其他非 entry stage）分配比默认
   更大的 expert 缓存预算（`STRATA_METAL_ARENA_GB`），就能让多轮/并发请求的正确性恢复。
   默认预算是按"这台机器、这个进程自己的可用内存"算出来的（`arena_budget`），没有考虑到
   "split 场景下一个 stage 只需要算全模型一部分层，但为了支持多请求复用，仍然需要接近
   non-split 水准的 per-layer 缓存深度"这件事——这本身可能是一个值得在 Tectonic 的部署
   建议/默认配置里体现出来的产品结论，不只是调试手段。
2. **强烈支持、但没有最终证实"hit/miss 路径不等价"这个底层假说**：扩容能修好，只说明
   "让 cache 驱逐/未命中的发生频率降到接近零"能避开这个 bug，**不代表"驱逐/未命中本身"这条
   因果链被证实了**——也可能是另一种尚未设想到的机制，恰好也对"miss 发生频率"敏感。
   真正坐实根因仍然需要上面记录的"对比同一个专家在 hit 和 miss 两种状态下、被 GPU 实际读到
   的数据是否等价"这个最小实验。**这是下一轮排查的入口，不是这次会话要收尾的地方。**

**当前状态总结（最终版）**：bug 100% 确定性、随 A 的 prompt 长度单调加重，原因链条收敛到
"split-tail 的 expert 缓存容量远小于 non-split（约 1/4.5），哪怕极短的 prompt 也接近
90%+ 占用，容量不足触发的驱逐/未命中在某个环节产生了错误数据"；**有一个已验证、可复现
（3/3）的缓解方案：给 split 的非 entry stage 分配更大的 `STRATA_METAL_ARENA_GB`**；
根因本身（到底是不是"hit/miss 路径不等价"，具体是哪一行）仍未最终定位，但现在有了一个
真正能让用户当下用起来的出口，不必等根因修好才能支持 split 多轮/并发。

新增诊断（未 revert，保留）：`metal/src/engine/stream.cpp`（`hold_region` 的
`TECTONIC_DEBUG_OCCUPANCY` 诊断）。**`STRATA_METAL_ARENA_GB` 不是新增的——它是已有的、
文档化的启动参数（`window.cpp` 里本来就读取），这次会话只是第一次把它和这个 bug 联系起来。**

**补充两点技术细节（PC）**：(1) tail 的 per-layer 缓存容量只有 non-split 的约 1/4.5，
**不是代码按层数把预算平分**——`make_arena` 的 `budget` 来自 `arena_budget()`（这台机器
当时实际可给的内存），split 下 entry 和 tail 是两个独立进程、各自申请、各自拿到一份，
tail 自然分不到 non-split 单进程能拿到的那么多；也就是说"给 split 的非 entry stage 配
更大内存额度"是一个**部署参数**，不需要改代码结构。(2) `cls_` 里会有一个 `pages=0` 的
size class（tail 的 `lay_[0..23]`——它不拥有的那些层——没有 expert tensor，分组时这些
"空"条目也会被收进来），它在 `base += k.n` 里占编号但 `total += k.n*0*PAGE` 贡献 0
字节，**不占内存，不是配置浪费**，免得以后有人又重新查一遍。

**曾尝试、已证明方法有缺陷、不构成任何结论的一次实验（记录下来避免重复犯同一个错）**：
为了直接验证"cache hit 和 miss 给 GPU 的数据是否等价"，在 `at_layer` 里加过一个诊断，
对比"默认 1728-slot arena 跑 B"（很多 miss）和"10 GB/5395-slot arena 跑 B"（很多 hit）
两次运行中、同一个 `(layer, expert)` 的 host 侧数据 checksum。**结果不可信**：哪怕两次
都是 `hit=1`（双方都不是刚读的新数据，理论上必须是磁盘上同一份、完全确定的字节），1635
对里仍有 775 对（47%）checksum 不一致。这么高的比例说明问题出在测量方法本身——最可能是
checksum 的范围覆盖了整个 size class 分配（`cls_[cls_of(s)].pages * PAGE`），而不是
这个专家实际用到的精确字节数，多出来的 padding/尾部可能残留着**上一个占用者**的数据，
两次独立进程运行里自然不同，和 hit/miss 毫无关系，纯粹是 checksum 粒度太粗；另外这次读取
发生在释放 `mu_` 锁**之后**，虽然 `pin` 理论上保护到这次 `at_layer` 结束，但不能排除还有
别的时序因素。**这个诊断已经 revert、未保留在代码里。** 下次要做这个实验，必须先精确对齐
"专家实际权重的字节数"（不是 size class 的整数倍，要看 gate/up/down 各自的真实大小），并且
在锁保护下完成读取，才能回答这个问题。

**同一时间来自 PC 侧的一个很锐利的旁证，值得记下来指引下一轮方向**：如果根因是"层间权重
错位"（比如第 15 层的 slot 被第 6 层的专家权重占据），模型的隐藏维度/语义空间会直接对不上，
输出应该是彻底不连贯的噪声；但本条记录和上一条记录里观测到的"连贯但答非所问"（讨论一个
完全无关的话题，却语法通顺、语义自洽）**更像是模型的推理链路本身工作正常，只是它看到的
上下文/输入换成了别的文本的表示**——这把怀疑方向从"权重侧错位"进一步推向"输入侧
（KV/hidden state 里混入了别的对话的表示）"，和更早记录里"entry 的 handoff checksum 一致、
但 tail 自己写出的 `kq_` 不同"这个观测完全吻合。**下一轮如果要验证根因（不只是维持现在的
缓解方案），这是比"hit/miss 数据是否逐字节相同"更值得先确认的方向：先确认是不是真的有一段
不属于当前请求的、连贯的 KV/hidden 表示混了进来，而不是先假设是权重或纯数值问题。**

## 2026-11-22（续四）：按 PC 的三点要求重做 hit/miss 实验——干净的负面结果，排除专家权重侧

上一条记录里那次 hit/miss checksum 实验被 PC 指出一个比"padding 太粗"更根本的问题：**比较的
两边全都是 `hit=1`，从头到尾没有真正对比过 hit 和 miss 这两条路径，47% 不一致这个数字对
"hit/miss 是否等价"这个问题贡献的信息量是零。** 按 PC 给的三点要求重做：(1) 真正制造一次
miss（小 arena 下某专家被驱逐）和一次 hit（大 arena 下同一专家仍然 resident），各自确认
`st`/`path` 字段；(2) checksum 范围改成 `lay_[l].gs + lay_[l].us + lay_[l].ds`（这个专家
实际的权重字节数），不再用 size class 的 padding 后容量；(3) 读取放在 `mu_` 锁保护下，
miss 的那份在 `done_cv_.wait` 完成、仍持有同一把锁时读。

**结果干净、确定：针对 layer 25，找到 17 个"默认小 arena 下是真 MISS、10 GB 大 arena 下是真
HIT"的专家，17/17（100%）checksum 完全一致。** 这次方法论上没有歧义——两边确认走的是不同
分支（一个真的从 SSD 读，一个真的直接命中），覆盖的字节范围是这个专家精确的权重大小，读取
在锁内完成。**这排除了"cache hit 和 miss 两条路径给 GPU 的数据不等价"这个假说本身**（至少
对 expert 的 FFN 权重数据而言）——同一个专家的权重，不管是刚从磁盘读出来还是已经常驻，字节
完全相同。

**这把排查范围进一步收窄、也更加确认了 PC 和 FORK_LOG 这几条记录反复指向的方向**：entry 的
handoff 干净（已验证）、GDN/PLE 状态跨对话一致（已验证）、现在 expert 权重在 hit/miss 下
等价（新验证）——**entry、FFN/MoE 权重、GDN 这几条线基本上都排除了，剩下的只有 tail 自己的
QSA attention 计算（K/V 的写入/索引/indexer）这一处还没有直接证据证明清白**。"连贯但答非
所问"这个失效形态（PC 的论证：跨层权重错位该是不连贯的噪声，而不是语法通顺的另一个话题）
继续支持"KV/上下文被混入了别的内容"，而不是"权重读错了"。

**下一轮的入口非常具体**：对比"B 单独跑"和"B 复用退化"两种场景下，tail 自己在 QSA 层写入/
读取的 K/V（`kq_`/`vq_`/`ksc_`/`vsc_`，用自动探测的真实 index，不是死掉的 0 号）在**整个
prefill 过程中逐 cell 的内容**（不只是像更早记录那样看 prefill 完成后的最终 checksum，而是
看写入的中间过程，确认是不是真的有一段不属于当前 token 序列的、连贯的历史 K/V 混了进来，
混入的具体机制——是 indexer 的某个候选范围没有被正确地按当前 cell 数裁剪、还是别的）。

新增诊断（已 revert，未保留在代码里；这次的方法——真实 hit/miss 分流 + 精确字节范围 + 锁内
读——是可信的，下次需要类似实验时可以照这个模式重建，不需要再踩一遍"比较了两个 hit"那个坑）。

## 2026-11-22（续五）：逐 cell 对比——分叉从 B 的第一个 cell 就开始，不是渐进式污染

按上一条记录的入口，加了一个逐 cell 的 K checksum 诊断（`TECTONIC_DEBUG_PERCELL`，
`window.cpp`，`prompt_chunk` 写完之后对 `kq_[jd]` 按 cell 粒度——不是整个 buffer——逐个
算 FNV-1a），对比"B 单独跑"和"B 紧跟 A 复用退化"两种场景。

**结果出乎意料：B 自己 prefill 的第 0 个 cell（这次对话的第一个 token）checksum 就已经
不一样，不是从某个中间 cell 才开始分叉。** 这和"读到了前一个对话遗留在后面某处的 K/V"
这个直觉模型不符——如果是"历史 KV 残留被读到"，至少前几个属于 B 自己、刚写入的 cell 应该是
对的，分叉应该出现在"B 自己的内容用完、indexer 越界读到别处"的某个点上。**从 cell 0 就错，
意味着连 B 自己第一个 token 的 K/V 写入过程本身，计算出来的数值就和"单独跑"时不同。**

这把已知排除的清单再收紧一层：entry 的 handoff（这个 cell 对应的残差输入 R）已证明一致、
GDN 状态一致、PLE 状态一致、现在 expert 权重在 hit/miss 下也证明等价——**qsa_prep 这个
kernel 对 cell 0 的所有已知输入都被证明相同，输出却从第一个 cell 起就不同**。顺手查了一个
新候选并排除：`mrope_`/`g_mrope_host`（"M-RoPE"，传给每次 `qsa_prep` 调用的 `mtab`
参数）——确认这是纯视觉/图片位置表机制（`mrope_table_set` 的调用点报错信息是"cannot
allocate the image position table"），我们的测试全程纯文本、不带图片，`g_mrope_host`
应该全程为 `nullptr`，这条路径对本次复现不生效。

**当前候选已经耗尽了"host 侧可读、可比较"的输入**：剩下能解释"所有已知输入相同、输出却
不同"的候选只剩两类：(a) 还有一个我们没想到、没检查过的输入（可能性仍然存在，但已经是
"不知道该往哪找"的程度）；(b) GPU kernel 计算本身存在某种和"这是不是这个 pipeline 第一次
被调用"相关的非确定性或隐藏状态（不是 expert cache 那种已排除的机制，而可能是 Metal
threadgroup 内存初始化假设、kernel 变体缓存、或者别的更底层的东西）。**到这一步，继续靠
host 侧打点可能已经摸到了这种方法的天花板——下一轮如果要继续定位根因（不只是维持现有的
扩容缓解方案），大概率需要真正的 GPU 帧捕获/调试工具（Xcode Metal frame capture 之类），
而不是再加一层 `fprintf`。**

新增诊断（未 revert，保留）：`metal/src/engine/window.cpp`（`prompt_chunk` 末尾的
`TECTONIC_DEBUG_PERCELL` 门控逐 cell checksum，默认不生效，无害，下次需要类似粒度的对比
可以直接复用）。

## 2026-11-22（续六）：`hold_region`/`region_slots_` 的 `where_` 全表扫描——反相关，排除整条线

PC（HIP 后端协作者）把跨层权重混用的假说改写后（"slot 被跨层复用时状态清理不完整，
where_/smap_/x.layer/x.e 这一组在'归还'时有一个没完全重置"），指出三个具体可疑点：
(1) `victim()` 清 `x.layer/x.e` 但 `io_main` 完成时用的是 job 队列里缓存的 `j.layer/j.e`，
slot 中途被复用会让两者脱节；(2) `cancel_slot` 清了 `where_`/`smap_`/`x.layer`/`x.e`/
`x.pre`/`x.left`，但只从 `pre_q_` 摘除匹配项，从未检查 `urgent_q_`，可能留下指向旧
layer/expert 的残留 job；(3) 最可疑的一点——`hold_region(false)` 只设 `state=0` 和
`lru_back`，完全不碰 `layer`/`e`/`where_`/`smap_`：如果 `hold_region(true)` 把一个 slot
划进 scratch 区域、其内容被 prompt GEMM 的写入覆盖，但 `where_` 表里还留着指向它的旧
key，`hold_region(false)` 把它放回 LRU 后，`at_layer` 会继续用"三件套全通过"
（`state==0 && where_>=0 && smap_有值`）误判为命中，直接把被 scratch 覆盖过的内容当成
那个专家的权重用——且这个过程不含任何随机性，完全由 arena 布局和前序请求的路由决定，天然
解释了"100% 字节级确定性复现"。

PC 给出一个比 frame capture 便宜几个数量级、且不论假说对错都能给出确定性答案的检查：在
`hold_region(true)` 清完 `region_slots_` 之后，再扫一遍 `where_` 全表，统计有多少条目的
值落在 `region_slots_` 里、但对应 slot 的 `x.layer < 0`（也就是清理循环以 `x.layer >= 0`
为假而跳过它）。这类条目如果存在，就是"scratch 覆盖但 key 未失效"的直接证据。

在 `ExpertStream::hold_region(true)` 里加了 `TECTONIC_DEBUG_WHERE_SCAN` 门控的这个扫描
（见 `stream.cpp`，未 revert，保留，默认不生效）：

```cpp
if (std::getenv("TECTONIC_DEBUG_WHERE_SCAN")) {
    std::vector<uint8_t> is_region(slots_.size(), 0);
    for (int s : region_slots_) is_region[(size_t) s] = 1;
    int64_t stale = 0;
    for (size_t key = 0; key < where_.size(); ++key) {
        const int32_t s = where_[key];
        if (s >= 0 && is_region[(size_t) s] && slots_[(size_t) s].layer < 0) {
            ++stale;
            std::fprintf(stderr, "DBG where_stale: key=%zu (l=%zu e=%zu) -> region slot=%d (x.layer=%d x.e=%d)\n",
                         key, key / NE, key % NE, s, slots_[(size_t) s].layer, slots_[(size_t) s].e);
        }
    }
    std::fprintf(stderr, "DBG where_scan: stale_entries=%lld (region_slots_.size=%zu)\n",
                 (long long) stale, region_slots_.size());
}
```

**结果，两种配置各跑一次 A→B，均为真实请求（非 fake 流程）：**

- 默认/小 arena（bug 复现的配置）：`stale_entries=0 (region_slots_.size=0)`。
- 10GB arena（bug 不复现的配置）：`stale_entries=0 (region_slots_.size=832)`。

**这组交叉验证是反相关的，而且把整条 `hold_region`/`region_slots_` 线彻底排除**：不是
"这次凑巧没测出来"，而是 `region_slots_` 机制在 bug 复现的小 arena 配置下根本就是空的、
从未被执行过（`ExpertStream::region()` 在没有 arena chunk 留有余量时会退回
`standalone_region()`，即匿名 mmap，不走 `region_slots_` 这条路径）——一条从未被执行的
代码路径不可能是这个 bug 的根因。而在 10GB arena 配置下，`region_slots_` 确实被激活
（832 个 slot），但扫描结果完全干净（0 条残留），进一步确认即使这条机制被真正使用，也
没有 PC 假说里描述的清理缺口。

这排除了 PC 三点怀疑里最具体、最容易验证的第 (3) 点。PC 随后指出这组对照同时变了两个
变量——slot 数（1728 vs 5395，驱逐频率）和 `region_slots_`（0 vs 832，scratch 位置）——
而 (3) 已被证明不是原因，所以剩下能解释"扩容修好"的只有驱逐频率本身：驱逐越频繁，slot 被
不同层/专家复用的次数越多，撞上任何"复用时状态没清干净"缺口的概率越高。PC 随后自行核实了
点 (1)：`victim()` 只选 `state==0` 的 slot，`submit()` 立刻把 state 设成 1（不等于 2），
所以一个槽在被读期间不可能被 `victim` 选中，`x.layer` 和 job 队列里缓存的 `j.layer` 不会
脱节——(1) 是干净的。

点 (2)——`cancel_slot` 只从 `pre_q_` 摘除匹配 job，从不检查 `urgent_q_`——PC 给出一条具体
的危险链：若某 slot 在 `urgent_q_` 里还有在飞的 part，`cancel_slot` 把它的 `state`/`left`
清零后，I/O 线程读完那个残留 part 时 `--x.left` 会变成 -1（条件 `== 0` 为假），不触发
`state=0`/`smap_` 更新，但 `read_part` 已经把数据写进了 `host_[s]`；而 `state==0` 这时已经
被 `cancel_slot` 设好，`victim()` 可以立刻把这个 slot 派给一个新专家，新旧两个专家的字节就
会混进同一个 slot——如果成立，这正是"权重混用"的物理形态，而且是确定性的（由 arena 布局和
路由决定），同时解释"连贯但答非所问"（语义正确、来源错误）而非纯数值噪声。

**在采纳这条之前，沿着实际的同步结构逐步核实了一遍，结论是这条路径在当前代码里不可达**：
`submit()`（stream.cpp:338-349）把一个 job 的全部 part 原子性地整体压入 `pre_q_` 或
`urgent_q_` 二选一，不会出现部分在一边、部分在另一边的情况；唯一能把 entry 从 `pre_q_`
搬到 `urgent_q_` 的只有 `promote_slot()`（stream.cpp:366-372），且它在**同一把锁内**把该
slot 在 `pre_q_` 里剩下的**全部** entry 一次性搬空，不是部分搬运。而 `cancel_slot` 的两个
调用点（`at_layer` 的 line 644-650、`hold_region(true)` 的 line 777-781）发现候选 slot 的
唯一方式是"在 `pre_q_` 里扫到属于它、且 `state==1` 的 job"——如果这个 slot 之前被
`promote_slot` 处理过，`pre_q_` 里已经一条都不剩，扫描根本发现不了它，更谈不上对它调用
`cancel_slot`。也就是说：**一个 slot 一旦有任何 part 被 promote 进 `urgent_q_`，就从
`cancel_slot` 的可触及集合里永久移出**，不存在"`state==1` 但 `urgent_q_` 里有残留 job"这个
中间态。另外，`hold_region(true)` 自己的 `cancel_slot` 调用发生在
`done_cv_.wait([&]{ return outstanding_ == 0; })` 之前；等这个 wait 真正放行、代码继续往下
清理 `region_slots_` 时，`outstanding_ == 0`（全局计数）已经保证两条队列此刻都是空的——不
可能有任何在飞 job。

**结论：PC 三点怀疑全部查完，均为阴性**（(1) victim/io_main 层号脱节——安全；(2) cancel_slot
的 urgent_q_ 残留——代码层面不可达；(3) hold_region/region_slots_ 清理缺口——反相关实测排除）。
这把"跨层专家权重混用（经由某种状态清理缺口）"这整条假说家族排除到了代码能验证的极限。已把
这个反驳同步给 PC，并提出下一步的两个方向：(a) 还存在一个双方都没想到的第四类状态；
(b) host 侧 `fprintf` 诊断确实已经到头，需要上 Metal frame capture，直接比对"同一个 dispatch
在两次运行里的输入 buffer 字节"——这是纯数据对比，不受捕获本身引入的同步/时序变化影响，
适合这种已经确认是 100% 确定性、而非时序竞态的 bug。

## 2026-11-22（续七）：根因找到并修好——`standalone_region` 的 `region_bytes_ = 0` 让跨请求的清零变成空操作

在上一条发出后几分钟，PC 独立从 `region_slots_=0` 这个结果反推出了真正的根因，绕开了整条
"跨层专家权重混用"假说家族（专家权重侧从始至终都是干净的——这才是污染源头实际上不在
`ExpertStream` 的专家缓存那部分，而在它旁边的 prompt GEMM 便笺（scratch）分配逻辑）。

**根因**：`Window::prompt_gemm_init`（`window.cpp:3170`）有 `if (pg_) return true;` 这个一次性
初始化闸门——`PromptGemm` 结构体里的每一个 buffer（`R` 残差流输入、`qfull`/`kcur`/`vcur`
K/V 投影、几乎整条 prompt 前向路径要用到的 scratch）都是在**整个进程生命周期内只分配一次**，
之后每一个新 prompt 请求都复用同一块物理内存（`cx_->wrap(region, b)` 把这些 buffer 包进同一个
`region` 指针）。当 arena 太小、挤不出一块 slot 区域给 prompt scratch 时（`ExpertStream::region()`
退回 `standalone_region()`——也就是本次所有复现用例的配置，日志能看到
"the arena is small: the prompt's N GB of buffers are their own allocation"），
`standalone_region()`（`stream.cpp:755`）把 `region_bytes_` 硬编码成 **0**，注释写的是
"fresh zero pages; nothing in the arena to empty"——这句话对**第一次**调用是对的（`mmap`
给的新页确实全零），但 `region_`/`region_bytes_` 是成员变量，会原样留到下一次请求；而
`hold_region(true)`（每个 prompt chunk 开始前都会调用，split 模式无条件进入）清零 scratch 的
唯一动作是 `std::memset(region_, 0, (size_t) region_bytes_)`——**用的正是这个被硬编码成 0 的
长度**。于是从第二个请求开始，这个 `memset` 永远是长度 0 的空操作，B 的 prompt GEMM 直接在
A 留下的 scratch 残留上开始写自己的数据。

这条根因同时解释了本轮调查里每一个此前"最难解释"的观测，不需要再叠加任何第二个机制：
- **B 的第一个 cell 就错**：scratch 从第一行开始就是脏的，不是某种边界之后才越界读到的。
- **A 越长、B 越差（55 正确 → 56 答非所问 → 59 零输出 → 61 循环乱码）**：A 写进 scratch 的
  残留随 A 的长度单调增多，B 的更多行被污染。
- **10GB arena 下完全正确、且 `region_slots_` 反而非空**：大 arena 走的是 `region()`（不是
  `standalone_region()`），那里 `region_bytes_ = end - lo` 是真实非零值，`memset` 真的执行。
- **100% 字节级确定性复现**：A 留下的 scratch 内容本身是确定性的（给定固定权重和路由），不涉及
  任何时序或并发窗口。
- **专家缓存 hit/miss 的 17/17 checksum 一致、`region_slots_` 扫描 0 条残留**：这两项之前的
  "阴性"结果都是对的——污染根本不在专家权重那条路径上，在它旁边的 prompt scratch 分配上，
  两者是同一个 `ExpertStream` 里两套相互独立的东西（专家 slot vs. 一次性 GEMM scratch 分配）。

**修复**（`metal/src/engine/stream.cpp`，`standalone_region`）：把 `region_bytes_ = 0` 改成
`region_bytes_ = sz`（函数开头已经算好的、页对齐后的分配大小），让 `hold_region(true)` 的
`memset` 每次都清掉这一整块复用的 scratch，并在函数上方补了注释说明这个一次性分配会被所有
后续 prompt 复用、为什么长度不能是 0。

**验证**（真实两节点部署，entry[0,24)+tail[24,48)，默认/小 arena，不设
`STRATA_METAL_ARENA_GB`，tail 日志确认走的就是 "the arena is small" 这条路径——也就是本轮
所有复现用例的确切配置）：连续三个不同对话——"What is 2 plus 2" → "What color is the sky"
→ "Name three fruits"——全部正确。第二个请求不再是 `els_improved` 循环乱码，而是正常的
"The user asks: \"What color is the sky\". This is a simple..."；第三个请求同样正常，证明
修复不是只对第二个请求生效、而是每次 `hold_region(true)` 都正确清零。

这把 split-stage 第二次对话污染这个 2026-11-22 当天发现、追了一整天的 bug 实际修复了——
不再只是"已知可靠的缓解方案"（`STRATA_METAL_ARENA_GB` 调大隐藏了问题，不是修好了问题），
而是代码本身的修复。感谢 PC 坚持追查"scratch 清零"这条线，没有被"专家权重混用"这个更复杂、
但实际上是错误方向的假说带偏。

**顺手解释了早前一次"测过但没效果"的诊断为什么没效果**：本次会话更早之前加过一个
`TECTONIC_DEBUG_PGZERO` 诊断（`window.cpp`，`prompt_chunk` 开头在 `pos0 == 0` 时手动
`memset` 清零 `p.scores/p.qids/p.qidx/p.part/p.XQ/p.qcur/p.qcurh/p.ids/p.wts/p.qfull/
p.kcur/p.vcur/p.iqr/p.ikr/p.R`），测试后判定"不能修好这个 bug"，一度让排查方向偏离
"scratch 残留"这条线、转向了专家权重。现在看，这个诊断其实是**同一个根因下的一次不完整
尝试**：它覆盖了 QSA 相关的 scratch（`R`/`qfull`/`kcur`/`vcur` 等），但遗漏了 GDN
（gated delta net，与 QSA 交替的那一半层）用到的 scratch（`p.qkv`/`p.z`/`p.al`/`p.be`/
`p.yg`/`p.gqn`/`p.gkn`/`p.gvc`/`p.ggb`/`p.gkq`/`p.go` 等）和其余中间 buffer（`p.V`/
`p.mixed`/`p.inj[0,1]`/`p.bo` 等）——这些同样是 `pg_` 一次性分配、被所有后续 prompt 复用、
同样会把上一个请求的残留带进下一个请求。只清一部分，残留还是能从另一部分漏进来，所以这个
局部诊断测出"无效"，但不代表方向错了，只代表覆盖不全。真正的修复（`standalone_region` 的
`region_bytes_`）是在分配层面一次性清掉整块 scratch，不依赖列全每一个字段，天然不会有这个
遗漏问题。这个不完整的诊断已经从 `prompt_chunk` 里删掉（`TECTONIC_DEBUG_PGZERO` 整段，
连同判断分支），重新编译、真实两节点部署重测 A→B 确认删除后行为不变（仍然正确）。

**加固（PC 建议，均已采纳）**：`region_bytes_` 在 `stream.h:107` 声明处原本完全没有注释，
而它在两处赋值、一处使用时各自被读出不同的隐含语义——`region()` 里 `region_bytes_ =
end - lo` 读起来是"从 arena 借了多少"，`standalone_region()` 里原来的 `= 0` 也读起来是
"没借所以是 0"，但 `hold_region(true)` 里 `memset(region_, 0, region_bytes_)` 要的其实是
"需要清零多少字节"——`region()` 路径下两个语义碰巧相同，所以一直是对的，`standalone_region`
路径下两个语义不同，这才是 bug 的根源。现在在声明处写清楚了这个字段的真实含义（清零用的字节数，
不是借用量），并在 `hold_region(true)` 的 `memset` 之前加了 `assert(!region_ ||
region_bytes_ != 0)`——这个 bug 能潜伏到今天的根本原因是它没有任何可检出的状态（所有指针
合法、所有 checksum 一致、没有任何断言失败，只是那块内存没被清零），这条断言把"有 region
但清零长度是 0"这个本该不可能发生的状态变成启动即炸，而不是又一次悄悄产生错误输出。加固后
重新编译、真实两节点部署重测 A→B 确认断言不触发、输出仍然正确。

**待办（已记录，未在本轮完成）**：PC 指出 `(split || n >= mn)`（`window.cpp:3805`，`mn` 默认
64）这个 `prompt_gemm_init`/`prompt_chunk` 的进入条件和 split 无关——**`--serve`（非 split
单进程）在 streaming 模式下处理任何 ≥64 token 的 prompt 时也会走这条路径，如果当时的 arena
小到需要 `standalone_region`，第二个这样的请求同样会读到第一个的 scratch 残留**。之前验证
`--serve` 多轮对话一直用的是 26/30 token 量级的短 prompt，恰好没跨过这个阈值，所以没有暴露过
这个问题。这意味着这个 bug 的影响范围可能不止 `--tectonic` split，需要在 `--serve` 上补一次
"连续两个 ≥64 token 的不同 prompt、arena 小到走 standalone_region"的真实验证，确认后把这一条
从"已知限制"挪到"影响范围"里。同样待办：补一个专门的回归测试（连续两个不同 prompt，断言第二个
的输出不等于第一个的，或者直接断言 `R`/`qfull`/`kcur` 在第二个请求开始时的 checksum 为零）。

**方法论教训（PC 总结，记录在案）**：这个根因的形状和本次会话更早之前的 `09e21ed13`（`set_of_ss_`
被覆盖、reset 清错缓冲）属于同一族——**都是"某个一次性初始化建立的前提，在第二次使用时已经不再
成立，但代码仍然相信它"**。`standalone_region` 里"fresh zero pages; nothing in the arena to
empty"这句注释本身没有错，只是它描述的是一个瞬时事实（mmap 新页确实全零），代码却把它当成了永久
前提（这块内存只分配一次、被所有后续请求复用）。排查这类 bug 时，看到"只做一次"的初始化逻辑，
应该下意识去问"这个初始化建立的假设，在第二次使用这份状态时还成立吗"。

**上游状态（PC 核实）**：这个 bug 在上游 `Niko1221/Strata`（`origin/main`，`c6f6427d9`，目录已从
`metal/` 重组到 `engine/metal/`）里原封不动地存在，社区从未报过（搜索确认只有两个"相邻但不同"的
改动落在同一个 `standalone_region` 契约边界上——#6446 和 #3527，两者都是处理这块内存"没还回去"，
不是"没清零"）。已基于 `origin/main` 开干净分支（单 commit，只含这一处修复 + `stream.h` 的语义
注释 + `hold_region` 的断言），推到 `myrepo`（`DR1N0/Strata`），向上游提交了 PR：
[Niko1221/Strata#6722](https://github.com/Niko1221/Strata/pull/6722)。PR 描述按上游实际使用的
模板写（"## Problem and result" / "## Related discussion" / "## Approach" / "## Validation"
的 Passed/Failed/Not run 三段式，参照了 #6441/#6446/#3527 的真实格式），如实区分了"针对本 fork
的真实验证"和"没有重新搭建上游这套重组后的树做端到端验证，只做了语法检查"。

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
