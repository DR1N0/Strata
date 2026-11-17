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
- [x] 逐 token 取消检查点：数据循环每个位置处理前检查一次（`std::atomic<bool> cancel_requested`，
      信号线程置位），不是每批一次。**已知缺口**：命中后用 `kBoundaryEnd` 回复——这是二进制 wire
      仅有的"请求结束"帧，不区分"正常结束"和"被取消"；对下游来说两者长得一样。真正的区分
      （契约里 `error` 带 `code: cancelled`）要靠 JSON 数据连接的 `error` 消息，而这个循环现在
      还不发 JSON 数据消息，见下面的架构待决项
- [ ] `stage_worker_run` 外层的并发调度层（攒批 + `run_slot_rows` + 按 `request_id` 分发）——
      **因为下面这条架构待决项没有答案，这次没有动手实现**，强行写会是在猜
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

## 架构待决：`gen`/`delta`/`done` 和 BoundaryFrame 是什么关系

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
