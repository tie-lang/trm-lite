# trm-lite 变更日志

## preview.2 — 2026-09-02

- **p.6.5.1 复杂形态静态链接外壳**（«Go 式静态内置 runtime»复杂形态骨架——import 即选择）：
  - 独立命名空间三件骨架（与简单形态 trm_lite_sched/trm_lite_gc 物理隔离，避免与
    trm_lite.a 符号重复）：`core/mnn/sched_ws.tie`（`trm_lite_ws` 函数值任务协作
    FIFO 队列 `spawn/queued_count/task_at/dequeue/drain`，p.6.5.2 在此升级
    work-stealing）、`core/gc/gc_tri.tie`（`trm_lite_tgc` 分配登记占位
    `alloc/alloc_count/collect`，p.6.5.3 并发三色接管）、
    `core/runtime/tl_runtime_ctx.tie`（`tl_runtime_ctx` 复杂形态汇总库，语言层入口
    `ctx_ensure/ctx_spawn/ctx_queued/ctx_drain/ctx_collect/ctx_version`）。
  - tiec 侧（tie-main）：内置 spawn/yield/collect 与 import trm-lite 的冲突检测扩展到
    复杂形态汇总库（`tl_runtime_ctx::ctx_*`）——复杂 import 与内置混用编译期报错。
  - 已知限制：全局 `table<fn() -> i64>` 惰性 `= []` 重赋值缺陷（fn 元素路径崩溃），
    须声明时初始化；局部捕获是环境副本（跨任务可见状态须用全局，与简单形态一致）。
  - 验收：`tests/s65_ctx/ctx_shell_demo.tie`（3 任务排队 → drain → collect，exit 0）；
    负例 `tie-main/tests/_p651_probe/ctx_mix_neg.tie`（复杂 import + 内置 spawn → 编译期报错）；
    简单形态 spawn_demo 零回归；tiec 二阶自举 IR 不动点一致。

## preview.1 — 2026-08-27

- **阶段 1 块 2：spawn/yield/collect 成为 tiec 完整内置原语**（不依赖 import，静态链接 trm_lite.a）。
  - tiec 注册三处全落地：`is_builtin_name()`（backend/irgen_expr.tie）、`builtin_call()`（frontend/sbuiltin.tie）、`ensure_builtins()`（middle/data.tie，主表 73→76 有序）。
  - `builtin_expr` 内置 codegen：`spawn(f)` 提取闭包 env/entry 入队；`yield()` 排空就绪任务队列（drain 按 retcode 分派 call_indirect，支持 i64/void 返回）；`collect()` 委托 mark-sweep GC。
  - 静态链接：新标志 `g_used_trmlite` → `link_exe` 追加 `trm_lite.a`（`find_trmlite_lib`：`TIE_TRM_LITE_LIB` 环境变量 → 相对默认路径）；`tig_trmlite_call` 直发 extern_call 不置 tie_interp。
  - trm-lite 侧：sched 新增闭包任务队列 `spawn_task/queued_count/queued_entry/queued_env/queued_retcode/queued_dequeue`；gc 新增扁平 `collect()->i64`；表全局惰性初始化 `tl_sched_ensure/tl_gc_ensure`（.a 静态链接时 main 不初始化库内表全局）。
  - 混合检测：内置与 import 是替代运行时路径，同模块既 import trm-lite 又用内置 → 编译期清晰报错。
  - 新增 `core/runtime/tl_runtime.tie` 汇总库（import sched+gc 编译为 trm_lite.a）。
  - 验收：`tests/s10_exec/spawn_demo.tie`（3 任务 FIFO + collect，exit 0）、`spawn_void_demo.tie`（void 返回路径）；既有 `exec_demo.tie` 零回归。
  - 已知限制：闭包字面量（`func() -> i64 { … }`）解析在 var 初始化/实参位置有历史缺陷，spawn 暂以命名函数引用（`spawn(task)`）承载；tiec 二阶自举（tiec 编译自身）通过。

## preview.0 — 2026-08-26

- 初始化 trm-lite 独立仓库骨架（core/{gc,mnn}、lib、tests、docs、tie.pkg、main.tie 占位）。
- 设计定稿：Go 式静态内置 runtime，分两小级——简单 M:N/GC 作 tiec 内置原语（供 actor），复杂 M:N/GC 作 trm-lite runtime 库（`import` 静态链接）。与 trm 并行、互不干扰。
- 设计依据：`tie-main/docs/superpowers/specs/2026-08-26-trm-lite-design.md`。
