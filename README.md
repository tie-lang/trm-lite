# trm-lite — tie 平台的 Go 式静态内置运行时

> 状态：**preview.2**（p.6.5 完成：复杂形态 work-stealing 调度 + 并发三色 GC（分代/整理）+ 可迁移栈 + channel 语言原语 + actor mailbox 咬合）

trm-lite 为 tie 引入 **Go 式静态内置 runtime** 形态，对标 Go 把调度器/GC 以库形式**静态链接**进单一零依赖二进制。与 trm（字节码 VM，路线 B）**并行开发、互不干扰**。

分两小级：

- **简单 M:N + GC**：作 **tiec 内置原语**（`spawn`/`yield`/`collect` + `ch_open`/`ch_send`/`ch_recv`/`ch_close` + 简单 stop-the-world mark-sweep），原生供 actor 使用，零配置、零 import。
- **复杂 M:N + GC**：作 **trm-lite runtime 库**（work-stealing 调度 + 并发三色 GC + 分代/整理 + 可迁移语义 + mailbox），`import trm-lite` 触发静态链接进单一二进制。

哲学：**简单形态走纯编译零依赖；复杂形态以库静态内置。** 同源一套 tie 源码，`import` 即选择。**注意：内置与 import 是替代运行时路径，同程序不可混用**（tiec 编译期会明确报错）。

## 快速开始（阶段 1 块 2）

前提：tiec 已支持 `spawn`/`yield`/`collect` 内置（compiler 后端已注册），并已构建 `trm_lite.a`：

```powershell
# 构建静态链接运行时库（一次）
tiec core/runtime/tl_runtime.tie -o trm_lite.a
```

用户程序直接调用内置（无需 import），tiec 检测到内置时自动静态链接 `trm_lite.a`：

```tie
type tie<logic>
var g_done: i64 = 0

func task_a() -> i64 {
    g_done = g_done + 1
    return 0
}

func main() {
    var t0 = spawn(func() -> i64 { g_done = g_done + 1; return 0 })  // 闭包字面量直接入队
    var t1 = spawn(task_a)
    yield()                     // 协作式让出：排空就绪任务队列（每个任务一次跑完）
    collect()                   // 简单 mark-sweep GC（返回本轮回收对象数）
    yield()                     // 排空就绪任务队列（每个任务一次跑完）
    println("done=" + to_string(g_done))
}
```

编译运行（需指定库位置）：

```powershell
$env:TIE_TRM_LITE_LIB = "F:\Projects\tie-repo\trm-lite\trm_lite.a"
tiec spawn_demo.tie -o spawn_demo.exe
./spawn_demo.exe
```

内置签名：

- `spawn(f: fn() -> i64 / fn() -> void) -> i64`：入队一个轻量执行体（任务），返回任务 id。
- `yield() -> void`：协作式让出，排空就绪任务队列。
- `collect() -> i64`：跑一次 mark-sweep GC，返回本轮回收对象数。
- `ch_open() -> i64`：分配新 channel（mailbox），返回句柄（1 起）。p.6.5.7。
- `ch_send(ch, v) -> i64`：入队消息；**0**=成功，**1**=失败（通道关闭或队列满）。
- `ch_recv(ch) -> i64`：取队首消息；**v**=消息值，**0**=空(未关)，**-1**=已关闭且排空。
- `ch_close(ch) -> void`：置关闭位（幂等），唤醒等待者。

验收载体：`tests/s10_exec/`（`exec_demo` 阶段 1 块 1 内核验收；`spawn_demo`/`spawn_void_demo` 块 2 内置验收）。

## 快速开始（阶段 2 起：复杂形态 import 即选择）

复杂形态以**独立命名空间**（`trm_lite_ws` / `trm_lite_tgc` / `tl_runtime_ctx`）静态内置，
与简单形态内置原语互为替代路径（同程序混用 → tiec 编译期报错）。用户程序 `import`
复杂形态汇总库即选择复杂运行时：

```tie
type tie<logic>
import "../../trm-lite/core/runtime/tl_runtime_ctx.tie"

var g_cnt: i64 = 0

func main() {
    tl_runtime_ctx.ctx_ensure()
    tl_runtime_ctx.ctx_spawn(func() -> i64 { g_cnt = g_cnt + 1; return g_cnt })
    tl_runtime_ctx.ctx_spawn(func() -> i64 { g_cnt = g_cnt + 2; return g_cnt })
    tl_runtime_ctx.ctx_drain()     // 协作排空就绪任务队列
    tl_runtime_ctx.ctx_collect()   // 回收（p.6.5.3 起真实回收）
    println("cnt=" + to_string(g_cnt))
}
```

复杂形态语言层入口（`namespace tl_runtime_ctx`）：

- `ctx_ensure()`：复杂运行时惰性初始化（幂等）
- `ctx_set_workers(p)` / `ctx_workers()`：调度池大小（默认 4，须在首个 drain 前设置）
- `ctx_spawn(f: fn() -> i64) -> i64`：注册并入队函数值执行体，返回任务 id（p.6.5.2 起
  多线程 work-stealing 承托：每 worker 独立双端队列 + 窃取 + 溢出队列）
- `ctx_queued() -> i64`：未完成任务数（排队 + 运行中）
- `ctx_drain() -> i64`：并行 run——起 P 个 worker（kernel32 真线程）并发执行全部
  就绪任务（含任务内再 spawn 的子任务），全部完成后回收池；返回本轮执行数
- `ctx_collect() -> i64`：并发三色 GC 已回收对象总数（p.6.5.3；后台回收器随
  drain 与 worker 真并发推进，sweep 在无任务窗口执行）
- `ctx_live_objs()` / `ctx_gc_steps()` / `ctx_gc_rounds()`：GC 观察量（存活数 / 标记推进步数 / 回收轮数）
- `ctx_minor_runs()` / `ctx_major_runs()` / `ctx_minor_freed()` / `ctx_compacted()`：
  分代观察量（p.6.5.4）；`ctx_gc_minor()` / `ctx_gc_major()` 同步触发代际回收
- `ctx_remapped(old)` / `ctx_obj_alive(id)` / `ctx_obj_age(id)`：mark-compact 后查询（p.6.5.4）
- `ctx_migrated()` / `ctx_task_exec_w(id)`：可迁移栈观察量（p.6.5.5）
- `ctx_stolen() -> i64` / `ctx_completed() -> i64`：跨轮累计窃取/完成任务数（验收观察量）
- `ctx_ch_open()` / `ctx_ch_send(ch,v)` / `ctx_ch_recv(ch)` / `ctx_ch_close(ch)` /
  `ctx_ch_len(ch)` / `ctx_ch_count()`：channel/mailbox 入口（p.6.5.7，复杂形态）
- `ctx_version() -> string`

托管堆（p.6.5.3/6.5.4）：`trm_lite_tgc` 提供 `alloc(size) -> id` / `set_ref(from,k,to)`
（写屏障：Dijkstra 黑→白 置灰 + 老→新记 remembered set）/ `drop_ref` /
`add_root|drop_root`（保守根）`gc_minor_sync()` / `gc_collect_sync()`（同步收集，断言用）。
分代：新生代/老年代（`TG_AGE_T=2` 晋升阈值）；minor 只收 young（老年代预黑保护）；
major = 全量三色 + mark-compact（存活重排前段 + 边/根重写 + `remapped()` 查询）。
精确根定义（p.6.5.6 拍板）：任务闭包 env 引用根集合（`add_root` + 写屏障维护），
运行时 sweep 仅在「无任务窗口」（pending==0 && active==0）执行。

## 已知限制（preview.2）

- 任务为 `fn() -> i64` 原子执行体，运行中不可抢占；抢占体现为调度级（任务边界
  让出 + 窃取均衡），时间片硬中断待语言级支持。「可迁移栈」语义落位为任务与
  创建 worker 解耦（任意 worker 可执行 + 迁移计数），非 OS 级协程栈迁移。
- 精确根为「任务 env 即根」保守口径：任务运行期未显式登根/未建引用边的局部临时
  对象依赖「无任务窗口」而非逐帧栈图（精度损失已文档化，root_protect_demo 佐证）。
- `ch_recv` 空队列（未关）返回 0（非阻塞降级），需调用方轮询/配合消息已达性约定；
  `ch_send` 队满返回 1（非阻塞降级）。Go 的阻塞 send/recv 语义需语言级挂起能力，
  当前以返回值协商替代（复杂形态任务内可用 cv 等待，见 `ch_recv` 内部实现）。
- channel 消息为标量 i64；对象/表消息需经 root 登记承载（p.6.5.8 actor 消息经
  单消息槽 + mailbox 令牌）。
- actor 由 trm-lite 承载（内置 spawn/collect 路径）：`#[unsafe.trm]` 标注作为
  显式接入确认（语法零改动，p.6.5.8）；actor 与 import trm-lite 混用 → 编译期报错。
- tie 相关既有缺陷（沿用，非本次引入）：标量全局初值静默丢弃（`=4` 实际 0）、
  `to_string(bool)` 输出 `-1`（true）/`0`（false）、顶层 `table<T>` 全局须带 `= []`
  初始化、单行块语句须以 `;` 结尾。
- 复杂形态汇总库（import tl_runtime_ctx 或多模块聚合 import）的重编译受 LLVM opt
  严格 declare+define 同模块冲突约束：channel 切片以 `tl_chan_lib.tie` 独立构件
  并入 `trm_lite.a`（构建方法见仓库）。

## 工程结构

```
trm-lite/
├── core/
│   ├── gc/              #  GC（简单：stop-the-world；复杂：并发三色 + 分代 + mark-compact）
│   ├── mnn/             #  M:N 调度（简单：协作；复杂：work-stealing + 可迁移语义）
│   ├── chan/            #  channel/mailbox（环形缓冲 + 互斥/条件变量，p.6.5.7）
│   └── runtime/         #  静态链接汇总库（import sched+gc → trm_lite.a）
├── lib/                 # 库层业务能力
├── tests/               # 验收 / 回归
├── docs/                 # 设计文档（独立副本）
├── tie.pkg              # 包清单
└── main.tie             # 入口（占位）
```

## 权威设计

架构与验收口径见 `tie-main/docs/superpowers/specs/2026-08-26-trm-lite-design.md`。

## License

本仓库按 tie-lang 组织自创的宽松许可证 **TIE-LANG Open Source License v1.0** 授权发布（全文见 [LICENSE](LICENSE)）：你可自由使用、修改并分发本软件源码，包括用于商业产品，仅需保留版权声明并附本许可证；而用该语言开发的自有软件完全归你所有，不附带任何署名义务。

This repository is released under the **TIE-LANG Open Source License v1.0** (full text in [LICENSE](LICENSE)): you may freely use, modify, and redistribute the source code, including in commercial products, provided you retain the copyright notice and a copy of the license; programs you write in the language are entirely your own, with no attribution obligation.
