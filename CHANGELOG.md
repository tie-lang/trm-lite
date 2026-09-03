# trm-lite 变更日志

## preview.3 — 2026-09-03

- **p.6.7.7 简单形态窃取队列（S-deque）**：
  - `core/mnn/sched.tie`：S-pool 单全局队列升级为 per-worker 双端队列——owner 段尾
    LIFO 自取、他人段头 FIFO 窃取、段满溢出全局（复刻 sched_ws 算法、独立实现不依赖
    复杂形态）；任务寄存器表（任务 id 即下标）+ 轮转分发（g_sdq_spi % P）；新增窃取
    计数观察量 `stolen_count` + 顶层符号 shim `stolen_count_s`（纯内置探针经
    unsafe extern 直调）；worker 序号经 CreateThread arg3 传入（编译器
    tig_s_pool_start 传 cnt，gen_s_pool_worker_eager 转发 param → pop_task(me,…)）；
    文件级全局统一 `g_sdq_*` 前缀（tiec 裸名导出全局与 sched_ws 内联的
    g_items/g_h/g_t/g_spi 链接期 LNK2005 多重定义根治）；`pool_idle_wait` 改
    「无可取才等」防空队忙轮询热循环。
  - 验收：`s_deque_probe`（失衡负载——制造者任务内 spawn 600 异构子任务刺激窃取 +
    300 池复用批次）stolen=272~368、tid_set=4、len=900 无重复 PASS×3；p.6.7 全套
    + m6_actor + spawn_demo 零回归；自举 tiec74→tiec75 字节不动点。

- **p.6.7.6 简单形态真并行（S-pool 常驻线程池）**：
  - `core/mnn/sched.tie`：spawn_task 锁内入队 + pending++ + 广播唤醒（防轻任务
    单 worker 独吞）；`pop_task` 锁内原子摘头 + out 槽写回（wp_i64）；`task_done`
    pend-- 归零广播；`yield_wait` 同步点；`pool_idle_wait` 空队 CV 限时；启动闸
    `pool_try_start/pool_workers`；池常驻不随 drain 终止。
  - 新增 `core/mnn/tl_k32.tie` **纯声明源**（kernel32 extern 零函数）——tl_sync /
    tl_tbl / sched 共用，杜绝同单元 extern 重名且不产 `tl_sync$*` 符号（.o 成员
    打包零泄漏，p.6.7.2 链接冲突根治面再收窄）。
  - 编译器侧（tie-main）：惰性合成 `tie_s_pool_worker` + `tig_s_pool_start`
    （CreateThread×P 常驻）；yield 内置重定义 = 同步点；actor 投递补池启动。
  - 验收：`s_pool_par_probe`（纯内置 8 任务真并行 tid_set≥2、共享表 800 元素
    无重复）PASS×5；m6_actor 7 正向探针 PASS；spawn_demo PASS；自举 fixpoint
    字节一致。

- **p.6.7.5 生命周期确定性（tie-main 编译器侧）**：str_cat 对直连链上临时释放
  （`@tie_str_free_if_heap`，长串 >31B 才 free，短串池只读）；严格零外引用判定防
  UAF；`chain_leak_probe`（8×2M 轮 6 段链）PASS + 二阶自举字节不动点。

- **p.6.7.4 输出与运行时余项并发安全**（审计性验收，spec §8.4）：
  `println` 单次 vararg printf（CRT 逐调用加锁）行级原子不撕裂；`to_string` 全内联
  每调用新缓冲；SSO 原子 bump；表/通道/调度/GC 内部表全锁内。探针
  `println_par_probe` + `println_checker`（400 行逐字节精确）PASS。

- **p.6.7.3 全局读写原子化 + race 指南**：
  - `tl_tbl` 锁原语自持 kernel32 CS extern（不 import tl_sync）→ `tl_runtime.o`
    不再捆绑 `tl_sync$*`；`trm_lite.a` 重建为双成员（`tl_runtime.o` 仅含
    sched/gc/tl_tbl，`tl_chan_lib.o` 含 chan+tl_sync），链接器按需提取——复杂形态
    探针（内联 tl_sync）与简单形态内置（spawn/ch/表混用）同链零符号冲突。
  - 通道句柄分配并发安全：`ch_open` 的 `g_ch_next++` + 平行数组追加复合临界区
    由全局分配锁 `g_alloc_cs` 串行化（裸自增并发打开丢句柄/数组错位竞态根治）。
  - tiec 编译器侧：`atomic<T>` 方法生成修复全局原子槽地址操作数 kind（kind 0 →
    kind 3，`@g_sum` 直接引用）；此前全局 atomic 生成的 `atomicrmw/load atomic`
    引用未定义 `%N`（闭包内尤甚）。store atomic relaxed 序非法问题探针侧规避
    （Release/SeqCst）。
  - 探针（tie-main/tests/_p67_probe/，全 PASS）：
    `atomic_sum_probe`（8 任务 × 1000 fetch_add = 8000 精确 + 独立槽位 8000 +
    CAS 换值）；`chan_open_par_probe`（8 任务并发开通道：句柄 1..8 各一次、通道
    互不串扰）；回归 tbl_par/sso_par/mix_simple 零回归。

- **p.6.7.1 SSO 短串池原子 bump**（分配器并发安全，tie-main 编译器侧）：
  简单/复杂形态共用字符串短串分配从 load→add→store 改为 atomicrmw fetch_add（seq_cst），
  多线程并发字符串构建拿到唯一不重叠块偏移、零 malloc 回退正确；
  探针 `tie-main/tests/_p67_probe/sso_par_probe.tie`（P=4 · 8 任务 · fails=0 ·
  dist_tid=4 · malloc=0）PASS；sso_probe / ctx_ws_demo / parity_chan_demo 零回归。

## preview.2 — 2026-09-02

- **p.6.5.11 收尾**：preview.2 发布——README 全景更新（复杂形态入口全集 + 已知限制
  清单）；自举核验：新 tiec 编译自身字节一致（tiec2 4032512 字节），简单 spawn/demo
  零回归；channel 构件 `tl_chan_lib.tie` 独立切片并入 `trm_lite.a` 的构建约定文档化。

- **p.6.5.10 回归与对比**：m6_actor 15 正向探针零回归（9 显式 PASS + 5 语义打印
  exit 0 + panic_raise 预期非零）+ 10 负例编译期拒绝；简单 vs 复杂行为一致——
  同一「生产者×channel→消费者」逻辑内置（parity_chan sum=804）与复杂
  （combo_demo ctx_ch sum=804）逐字节一致。

- **p.6.5.9 双形态验收 demo**：`combo_demo`（复杂：work-stealing 池 + mailbox 8
  消息取回 + 16 任务分配 96 槽/32 活/64 垃圾后台并发回收 + 执行器分布）；`
  combo_simple_demo`（简单：actor mailbox FIFO + 内置 channel + spawn 闭包
  10/45/190 + collect），两形态 exit 0 + 计数精确。

- **p.6.5.8 actor × 复杂形态咬合**：
  - actor 消息入口改经 **channel mailbox**：`actor_task` 从 record@56 的通道句柄
    `ch_recv` 取 method_id（替代直读单槽），发送方 `ch_send` 入队后 spawn 任务；
    单消息槽串行护栏保留（async FIFO 精确，actor_a4_async count=7 佐证）。
  - `#[unsafe.trm]` actor 级接入门接受（通用 `#[]` 属性通道白名单，语法零改动）；
    探针 actor_trm_demo：async 多参 FIFO total=96 PASS。
- **p.6.5.7 channel 语言原语**：
  - `core/chan/tl_chan.tie`：环形缓冲 mailbox（互斥 + 条件变量），FIFO、关闭标志、
    满/空语义（send 0=成功 1=失败；recv 值/0=空/-1=关闭空）。
  - tiec 三处注册 `ch_open/ch_send/ch_recv/ch_close` 内置（is_builtin_name /
    builtin_call / ensure_builtins + codegen 静态链接 trm_lite_chan$* + 冲突检测）。
  - 验收：chan_demo（FIFO/空/关闭/满容量 64）PASS；自举二阶段字节一致。
- **p.6.5.6 精确根拍板**：「任务 env 即根」——闭包 env 引用根集合由 add_root +
  写屏障维护，sweep 仅在无任务窗口执行；root_protect_demo（运行中对象不回收、
  结束后收敛 0 无泄漏）PASS。设计 §9 待决项 3 定案写入任务书文档。
- **p.6.5.5 可迁移栈语义落位**：任务与创建 worker 解耦（任意 worker 可执行），
  执行 worker 登记 `g_exec_w` + 迁移计数 `g_migrated`（worker≠创建者即迁移）；
  ctx_migrated/ctx_task_exec_w 观察量；mig_demo（24 任务分布 4 worker）PASS。
- **p.6.5.4 分代 + mark-compact 回收**（新生代/老年代 minor + 老年代整理回收）：
  - 年龄表 `g_o_age` + 晋升阈值 `TG_AGE_T`：存活 minor 轮 ≥2 → 老年代。
  - minor（半代）：young 全白 + 老年代预黑；种子 = 全根 + 记忆集 `g_c_rs`；
    sweep 只回收 young 白，存活 young age++（晋升）；老垃圾对 minor 免疫。
  - 记忆集：写屏障「老→新」入 rs（去重）+ **晋升屏障**（minor sweep 后扫描
    全部边，存活 老→新 补入 rs——覆盖晋升对象的陈旧历史出边）；rs 持久累积
    不清空（清空会抹掉两轮之间的屏障条目），陈旧条目由种子时 alive/WHITE 守卫。
  - major：全量三色 + **mark-compact**——存活黑对象按 id 升序重排到表前段，
    old→new 映射 `g_remap`，边表/根表重写，槽位压缩回收（`remapped()` 查询）。
  - **修复标记栈缺陷**：tie `table_push` 只追加不删除；旧实现 `g_stack[g_sp-1]`
    pop + 计数，表内新旧元素错位 → 每轮重复 pop 旧根、多级对象停留 gray（标记
    不收敛、老垃圾逃过 sweep）。改为队列式头游标（`mark_reset/mark_push/
    mark_peek/mark_advance/mark_empty`，BFS 遍历每个灰点恰好取一次）。
  - ctx 观察量扩展：`ctx_minor_runs/ctx_major_runs/ctx_minor_freed/ctx_compacted/
    ctx_gc_minor/ctx_gc_major/ctx_remapped/ctx_obj_age`；`ctx_collect` 汇总
    minor+major 累计回收；`root_survivors()` 供探针断言 compact 后根全存活。
  - 验收：`tie-main/tests/_p651_probe/gc_gen_demo.tie`（10 根晋升 10/10、rs 隔离
    存活、老垃圾免疫 minor、major 回收 21 + compact 重映射边正确，PASS exit 0）；
    既有 ctx_gc_demo（同步收断言修复：垃圾回收 >0 且 16 根全存活）与
    ctx_ws_demo / ctx_shell_demo / spawn_demo / m6_actor 零回归。

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
