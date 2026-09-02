# trm-lite — tie 平台的 Go 式静态内置运行时

> 状态：**阶段 2 进行中**（简单形态内置原语已落地；复杂形态静态链接外壳已起，work-stealing/并发三色 GC/可迁移栈 依 p.6.5 切片推进）

trm-lite 为 tie 引入 **Go 式静态内置 runtime** 形态，对标 Go 把调度器/GC 以库形式**静态链接**进单一零依赖二进制。与 trm（字节码 VM，路线 B）**并行开发、互不干扰**。

分两小级：

- **简单 M:N + GC**：作 **tiec 内置原语**（`spawn`/`yield`/`collect` + 简单 stop-the-world mark-sweep），原生供 actor 使用，零配置、零 import。
- **复杂 M:N + GC**：作 **trm-lite runtime 库**（work-stealing 调度 + 并发三色 GC + 可迁移栈），`import trm-lite` 触发静态链接进单一二进制。

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
- `ctx_collect() -> i64`：本轮回收对象数（占位恒 0，p.6.5.3 并发三色接管）
- `ctx_stolen() -> i64` / `ctx_completed() -> i64`：跨轮累计窃取/完成任务数（验收观察量）
- `ctx_version() -> string`

限制（文档明示）：任务为 `fn() -> i64` 原子执行体，运行中不可被抢占；
抢占体现为调度级（任务边界让出 + 窃取均衡），时间片硬抢占待 p.6.5.5 可迁移栈。
跨任务可见状态请用全局/独立槽位（局部捕获是 env 副本）。

验收载体：`tests/s65_ctx/ctx_shell_demo.tie`（正向 exit 0）；`tie-main/tests/_p651_probe/ctx_mix_neg.tie`（复杂 import + 内置 spawn → 编译期报错）。

## 工程结构

```
trm-lite/
├── core/
│   ├── gc/              #  GC（简单：stop-the-world；复杂：并发三色）
│   ├── mnn/             #  M:N 调度（简单：协作；复杂：work-stealing）
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
