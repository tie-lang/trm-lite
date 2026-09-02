# trm-lite 变更日志

## preview.2 — 2026-09-02

- **p.6.5.3 并发三色 GC**（复杂形态托管堆：登记占位 → 真并发回收）：
  - 扁平托管对象图（对象/边/根表）+ 三色标记栈；Dijkstra 写屏障（set_ref
    黑→白 置灰压栈）+ 根写屏障（cycle 中 add_root 置灰，防晚到根误收）；
    后台回收器线程随 drain 与 worker 真并发推进（bg_step 轮次状态机），
    sweep 仅在无任务窗口执行；gc_collect_sync 提供同步收集供断言。
  - 同步原语抽宿主 `core/mnn/tl_sync.tie`（tie extern 声明按文件作用域且跨文件
    不可重名：未声明→未定义、双声明→重复定义；集中单一声明源）。
  - 全局命名 g_c_ 前缀规避旧 trm_lite.a 归档全局（如 g_roots）的链接期撞名。
  - 验收：`tie-main/tests/_p651_probe/ctx_gc_demo.tie`（live=16/freed=8 精确、
    steps=16 后台推进、黑→白 交错改写后同步收集零误回收，PASS exit 0）；
    回归 ctx_ws_demo / ctx_shell_demo / spawn_demo 零回归。

- **p.6.5.2 work-stealing 调度器**（复杂形态执行层：协作 FIFO → 真实多线程）：
  - 每线程双端队列：任务注册表 `g_regs`（table<fn() -> i64>，regid）+ 承载表
    `g_items`（P×SEG_CAP 预分配，段固定不重叠）——owner 段尾 LIFO 自取、他人
    段头 FIFO 窃取、段满溢出全局 `g_ovf`。
  - 多 OS 线程池：每轮 drain 经 CreateThread 起 P worker、完成后 join 回收；
    锁外执行 `g_regs[regid]()`；单 CRITICAL_SECTION + CONDITION_VARIABLE（50ms
    轮询兜底）；终止协议「无可取 && pending==0 && active==0」原子一致无竞态。
  - 抢占：任务为 fn() 原子执行体不可中断，抢占体现为调度级；时间片硬抢占待
    p.6.5.5 可迁移栈（文档明示）。
  - 已知限制：tie 标量全局变量初值被静默丢弃（`= 4` 实际 0，表全局 `= []` 却
    生效）——sched_ws 在 ensure_state 强制默认池大小规避 `% 0`；`to_string`
    (bool) 输出 -1 待查。
  - 验收：`tie-main/tests/_p651_probe/ctx_ws_demo.tie`（P=4 执行线程去重=4、
    结果全对、子任务 16、burst 1300 溢出窃取 stolen>0、并行快于串行，PASS
    exit 0）；回归 ctx_shell_demo / spawn_demo / actor 零回归。

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
