# 引擎分叉日志（Strata）

本仓库是 [Niko1221/Strata](https://github.com/Niko1221/Strata) 的下游分叉，用于 Tectonic 的层切分分布式执行。本文件记录**我们改了什么、为什么改、能否回到上游**。半年后没人记得某个 `if` 为什么存在，这份日志就是答案。

## 分叉概况

数据取自 2026-11-13，基准 `504b1433`（分叉起点）：

| 项目 | 数值 |
|---|---|
| 我们的提交 | 6 |
| 我们的改动 | 22 文件，+2775 / −37 |
| 上游同期改动 | 310 文件，+12403 / −825（159 提交） |
| 双方都改过的文件 | **3 个** |

重叠的 3 个文件是未来冲突的唯一来源，改它们时要格外小心：

| 文件 | 我们的改动 | 性质 |
|---|---|---|
| `src/program/generate.cpp` | +540 | 最大单点，ring 入口与 CLI |
| `src/prefill/prefill.cpp` | 6 行 | 放宽中段必须有进程内后继的断言 |
| `cmake/strata_gpu_engine.cmake` | 2 行 | 链接 ring 传输源文件 |

`src/core/verify.cpp` 我们改了 118 行，但**上游同期没碰这个文件**，冲突风险低。

## 逐提交记录

### `d1b71162` feat(dsv41): add opt-in cut20 Metal boundary replay

- **改动**：`src/dsv41/ds4_model.cu`（+59）、`ds4_model.hpp`（+6）等 3 文件 +154/−2，加入可选的 cut20 边界回放能力。
- **为什么**：DeepSeek-V4.1 阶段的早期可行性实验，验证 Metal 侧切点边界能否复现。
- **可上游**：否。这是 DSV41 专用的一次性实验代码，对通用引擎无价值。
- **状态**：保留在分支内，不推动上游。若清理 DSV41 支持，此项可一并删除。
- **注**：DSV41 已不在当前阶段适配范围（单机 2 tok/s 的瓶颈是驻留专家太少与权重过大，双机翻倍后仍不达使用标准），但模型本身仍在路线图内。

### `4f25ec2a` fix(metal): report batch dispatch evidence and completion progress

- **改动**：`src/core/verify.cpp`（+5）、`tests/core/test_metal_batch_progress.py`（+126）等 3 文件 +173/−1。让 Metal 侧上报 batch dispatch 证据与完成进度。
- **为什么**：Metal 的批量调度此前无法从外部确认是否真的发生了，导致"看起来在跑"却无法验证的观测盲区。
- **可上游**：**可能可以**。这是通用的可观测性改进，与切分无关。若上游有兴趣，是一个干净的 PR 候选。
- **状态**：暂留本地，等 M0 指标接口稳定后再评估是否提 PR。

### `677157af` feat(core): session_loop walks the session's layer range

- **改动**：`src/core/session.cpp` 15 行。让 `session_loop` 按会话的层范围推进，而不是永远走全部层。
- **为什么**：区间执行的必要前提。没有它，任何 stage 都会试图算完整模型。
- **可上游**：**可能可以**。这是一个通用能力（"只跑一部分层"），实现是加法且小。上游若有分阶段加载或流水线需求会需要它。
- **状态**：与 `a85fbcc0` 绑定，作为一组提 PR 更合理。

### `a85fbcc0` feat(generate): --stage-begin/--stage-end/--stage-input

- **改动**：`src/program/generate.cpp` +46/−3。新增三个 CLI：层区间起止与外部边界输入。
- **为什么**：让**单个进程只计算模型的一段**，并且能从上一次的边界继续。这是切分的入口，也是最容易理解的一块。
- **可上游**：**可能可以**。参数是纯加法，无行为变更（默认 `0..n_layers`）。
- **状态**：与 `677157af` 一组。

### `9eeeb837` Implement typed multi-stage ring and orchestrator-ready execution

- **改动**：14 文件 +2123/−14。核心提交，包含：
  - `include/strata/core/boundary_wire.hpp` + `src/core/boundary_wire.cpp`（新）：边界帧序列化，wire v3，50 字节头 `<IHHHQqqqq`，多张量按名寻址，预留 `next_token` 槽。
  - `include/strata/core/boundary_socket.hpp` + `src/core/boundary_socket.cpp`（新）：TCP 收发，Windows `SO_EXCLUSIVEADDRUSE`，超时与优雅关闭。
  - `src/program/stage_ring.hpp`（新）：单一显式状态机——READY 绕环、REQUEST 走 head、TOKEN 回 orch。
  - `src/core/verify.cpp`：`boundary_capture` / `boundary_apply`，携带 pending `R/bo/inj`。
  - `src/prefill/prefill.cpp`：放宽中段断言。
  - `tools/test_boundary_wire.cpp`（353 行）、`tools/test_stage_ring.cpp`（62 行）：187 项检查。
- **为什么**：这是整个分布式执行的地基。三个关键设计：
  1. **边界是三张量而非一张**。Verifier 用融合跨层计算，pending FFN 的 `bo` 与 `inj` 必须随 `R` 一起移交；只传 `R` 会算错且不易察觉。
  2. **帧按名寻址**。早期实现按偏移量传递，`rebind()` 用全零缓存偏移覆盖了张量指针，导致 `bo`/`inj` 都指向 `R`，而旧测试只比对存储区字节因而漏检。现在按名 + 精确偏移测试。
  3. **显式状态机而非散落的阻塞调用**。多节点最怕死锁，状态机让每个阶段的阻塞点唯一可读。
- **可上游**：**部分可以，但价值存疑**。协议、socket、状态机都是通用的，上游若无多机需求不会接受这么大一块。建议**不主动提**，作为分叉的核心资产维护。
- **状态**：长期保留。

### `ff277821` feat(metal): support T=1 layer-range ring stages

- **改动**：`metal/CMakeLists.txt`、`metal/src/engine/stage.h`（新 44 行）、`metal/src/engine/window.cpp`（+102/−14）、`src/core/verify.cpp`（+32）、`tests/core/test_metal_stage.py`（新 100 行）。
- **为什么**：Mac 端要能作为 ring 的一环。把 stage 归属传入 Metal window，保留 pending 移交，把专家流式裁剪映射到全局层号；默认仍是全模型，并显式拒绝不支持的 staged batching 与 MTP。
- **可上游**：**可能可以**，与 `677157af`/`a85fbcc0` 同属"只跑一部分层"的能力。
- **状态**：保留。注意 `src/core/verify.cpp` 的新增 32 行**全部位于 `#if defined(STRATA_GPU_METAL)` 内**，HIP/CPU 构建的预处理结果不变——这是刻意的隔离，改这个文件时不要破坏它。

## 维护规则

1. **产品逻辑不进引擎。** runner、orch、编排、测试框架全部放 Tectonic。这条纪律让分叉保持在 22 个文件。哪怕代码要贴着引擎写，宁可多一层接口。
2. **保持加法为主。** 目前 +2775/−37，删除极少。一旦开始大段重写上游代码，合并成本会失控。
3. **每两周 merge 一次上游**，别拖。现在落后 159 提交，越拖冲突越大。merge 后跑现有回归（187 项检查）。
4. **优先缩小热点文件。** `generate.cpp` 的 +540 是最大单点，尽量把逻辑抽到独立头文件，让 `generate.cpp` 只剩参数解析与一次调用（`stage_ring.hpp` 已抽出 147 行，可以继续）。
5. **`#if defined(STRATA_GPU_METAL)` 是隔离带**，不是堆砌分支的地方。改 `verify.cpp` 时先确认改动落在哪个后端之下。
6. **"分开"发生在发布层，不在代码层。** 打 tag、出独立构建产物（`tectonic-engine-vX`），但代码始终是同一 git 历史的 fork。这样既吃得上下游的量化 kernel 修复，又能让产品版本可控。物理复制引擎代码是最后手段——一旦复制，上游的 bug fix 就永远丢失。

## 待办

- [ ] `4f25ec2a` / `677157af` / `a85fbcc0` / `ff277821` 评估合并成一个"layer-range execution"上游 PR 的可行性。
- [ ] 把 `generate.cpp` 里的 ring 入口逻辑进一步外移。
- [ ] 记录下一次与上游 merge 的日期与冲突文件。
