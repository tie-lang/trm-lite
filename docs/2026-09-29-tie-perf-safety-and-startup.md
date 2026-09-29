# tie 性能：安全优化路径 + 启动时间构成 / tie Performance: Safe Optimization Path & Startup Cost

* 日期 / Date: 2026-09-29
* 性质 / Nature: `2026-09-29-tie-dataplane-perf-report.md` 的**续篇 + 更正**
  (follow-up and correction to the data-plane report)
* 方法 / Method: 诊断变体实测（去锁/加自旋重建运行时）+ 稳健 A/B（多轮取最小总时间，减同形 noop）

---

## 零、先给结论 / Answers First

**问 1「如何在保证安全的前提下大幅优化性能？」**

锁是唯一的大头，且**可以在不改并发语义的前提下拿掉大半**。按安全性排序：

| 方案 / approach | 安全性 / safety | 预期 / expected | 建议 / |
| --- | --- | --- | --- |
| ① **批量原语** `tbl_append(h,src,n)` / 块读 | **零并发语义改变**（只是把 N 次加锁变 1 次） | 批量路径 **> 10×**，配合批量编码 > 100× | **先做** |
| ② ~~**归属快路径** `rc==1 → 免锁`~~ | **已实测证伪并回滚**（§3.3：运行期逃逸标志无法检测全局共享） | — | **不做**；替代=编译期逃逸分析 |
| ③ **字节负载改字符串/StringBuilder** | 纯新增，无并发影响 | **已落地**：内存 8×→1×（峰值 7.2× 更低）；构造 8.25×、扫描 6.7×、CRC 7.4×、文件读 **10.7×**、zd 帧 **14.9×** | **tdb r.1.6.7** |
| ④ **编译器表字面量批量 codegen** | 纯 codegen，语义不变 | 定宽编码 **≈10×** | 独立推进 |

**问 2「tie 程序启动时间构成？」**

**≈90% 是本机进程创建 + 杀软成本（≈610–660ms 地板），不是 tie 的成本。** tie 相对同规格
C 程序的边际开销实测 **+23 ~ +83ms**（且随系统负载漂移，无法稳定分离）。已用证据排除：
`main` 体（空程序 `ret i32 0`）、全局构造器（一个都没有）、二进制体积（+20KB ≈ +0.3ms，
实测 14ms/MB）。**结论：这是平台地板，tie 侧无可观优化空间；正确策略是摊销（长驻进程/
减少进程启动次数），不是提速。**

---

## 一、锁到底占多少：诊断变体实测 / How much of the cost is the lock?

**方法**：把 `tl_tbl` 的 `lock_enter`/`lock_leave` 置空，重建 `trm_lite.a`（**仅测量用，
不提交**），跑同一探针。这是唯一能把「锁」与「字段访问 + 调用」分开的直接手段。

| 操作 / op | 基线 / baseline | 去锁变体 / lock removed | **锁的份额 / lock share** |
| --- | ---: | ---: | ---: |
| `t[i]`（`tbl_at`） | **27–28 ns** | **8 ns** | 20 ns = **71%** |
| `table_push`（`tbl_push`） | **60 ns** | **26 ns** | 34 ns = **57%** |

⇒ **`t[i]` = 8 ns 有效工作 + 20 ns 锁。去掉锁就是 3.5×。**

复现 / reproduce（`tests/ab2.sh`，多轮取最小总时间、减同形 noop）：

```bash
cd trm-lite && cp -r . /tmp/tlexp && cd /tmp/tlexp
# 把 core/tbl/tl_tbl.tie 的 lock_enter/lock_leave 体清空
tiec core/runtime/tl_runtime.tie --no-cache --no-warn -o /tmp/tlexp/rt_nolock.a
cd tdb && TIE_TRM_LITE_LIB=/tmp/tlexp/rt_nolock.a \
  ../tiec/compiler/tiec.exe tests/probe_zd_perf.tie --no-cache --no-warn -o /tmp/perf_nolock.exe
tests/ab2.sh /tmp/perf_base.exe /tmp/perf_nolock.exe readloop 100000 100 8
```

---

## 二、安全的大幅优化 / Safe Large Optimizations

### 2.1 批量原语（首选：零并发语义改变）——**运行时侧已落地（r.1.6.6）**

> **状态 / Status**：运行时原语**已实现并回归通过**（`trm-lite` r.1.6.6）：
> * 新增 `tbl_append(h, src, n)`（一次锁追加 n 个连续元素，与连续 n 次 `tbl_push`
>   **逐元素等价**，探针有专门等价性断言）与 `tbl_reserve(h, need)`（一次锁预留容量）。
> * 顺带做掉热路径冗余读：`tbl_push`/`tbl_at`/`tbl_set` 原先每次访问白读 16 次句柄字节
>   （`lock_enter` + `lock_leave` 各调一次 `lock_of`），`ensure_locked` 又重读调用方刚读过的
>   4 个字段（32 次字节访问）。**实测**（稳健 A/B）：`table_push` **43 → 32 ns（−26%）**、
>   `t[i]` **26 → 24 ns（−8%）**。
> * 验证：`tl_tbl_probe`/`tl_tbl_rc_probe` 输出与期望逐字一致 + 新增
>   `tl_tbl_append_probe`（12 项）+ tdb 8 个 zd 探针全绿 + 三阶不动点。
> * **编译器接线仍待做**（把 `table_append` 暴露为标准内建 + 表字面量批量 codegen），
>   完成后批量路径才能被 tie 源码直接使用；届时逐元素循环可整体切到批量原语。


**做什么**：给表容器加批量接口——`tbl_append(h, src_bytes, n)`（一次加锁 + 一次 `ensure`
+ 一次 `memcpy`）、`tbl_reserve(h, n)`（预分配）、`tbl_read_block(h, i, out, n)`。

**为什么安全**：**不改变任何并发语义**。原来「N 次 `tbl_push` = N 次加锁」变成「1 次加锁
做 N 个元素」，等价于把 N 次操作放进同一个临界区。既有的单元素接口一字不动，调用方按需
选择。这是唯一**不需要任何并发论证**的大幅优化。

**收益**：批量路径从「逐元素 60ns」降到「memcpy 级 ≈0.1ns/元素 + 一次锁」。zd 侧的
`encode_bytes` 已经是这个思路的**用户态版**（`byte_concat(头, 载荷)` → **>100×**），
本项把它下沉到运行时、让所有 tie 代码都能享受。

**风险**：低（新增 API + 一个新外部符号 + 编译器接线）。**建议先做这个。**

### 2.2 归属快路径：`rc == 1 → 免锁`（大头）——**已实测证伪并回滚（r.1.6.7）**

> **结论 / Verdict**：本方案**不可行**，已实现→实测→回滚。证伪过程与根因见
> §3.3。核心原因：**表可经全局变量 / 堆槽被多线程共享而不触发任何 retain**，
> 运行期逃逸标志（无论叫 esc 还是 `rc==1 && owner==me`）**无法覆盖该场景**，
> 会在真实并发下漏锁。下面保留原始设计与论证以便追溯，但**不要按其实现**。


**做什么**：句柄已有 `refcount @40`。在读/写/追加的入口加一条快路径：

```
if refcount == 1 && (owner_tid == my_tid) { 免锁访问 }
else { 走原加锁路径 }
```

**为什么安全（论证 / soundness argument）**：
* `refcount == 1` 意味着**全程序只有一份引用**。其他线程要访问这张表，必须先拿到句柄，
  而拿到句柄必然使编译器插桩的 `tbl_retain` 把 `refcount` 增到 ≥2。
* 因此「`rc==1` 时不会有第二个线程访问它」——免锁访问**没有竞争对象**。
* 任何一条不成立（`rc>1`、`owner` 不是本线程、还没来得及记录 owner）→ 退回原加锁路径，
  **语义与今天完全一致**。
* 快路径的判断是「宁可保守」的：判错只会变慢，不会变不安全。

**已知窗口 / caveat**：A 线程读取 `rc==1` 之后、自己把表**移交**给 B 之前后仍在用表，可能与
B 的加锁访问竞争。缓解：**在 `tbl_retain` 里清掉快路径标志**，或让 `owner_tid` 在首次加锁
获取时更新（移交后 B 拿到的是新 owner）。需要在实现时明确这一点并写进注释。

**实现要件（已确认可行 / verified available）**：
* `GetCurrentThreadId` 编译器**已有桥接**（`llvmgen_str.tie:365`），无需新增 extern；
* 但它是 kernel32 调用（约 5–15 ns），**必须把本线程 id 缓存在全局/TLS 里**（单线程程序
  只需一次），否则快路径省下的 20 ns 会被它吃掉；
* 句柄从 48 字节扩到 56 字节加 `owner_tid`。

**收益**：`t[i]` 28 → ≈10–12 ns（**≈2.5–2.8×**），`table_push` 60 → ≈28 ns（≈2.1×）。
这是**唯一能让「逐元素」写法本身变快**的手段。

**风险**：中。必须做并发评审 + 压力测试（多线程迁移/移交场景）。

### 2.3 字节负载改字符串/StringBuilder 承载 —— **已落地（tdb r.1.6.7）**

> **状态 / Status**：抽象与首个真实消费方**已实现并实测**（tdb 仓）：
> * 新增 `tdb/src/zbuf.tie`（namespace `zbuf`）：字符串载体的读写原语
>   （u8/u16/u32/u64/f64 的 BE+LE、varint、zigzag、长度前缀字节串、hex）
>   + 与 `table<i64>` 的边界互换 + 1× 文件 I/O（`file_load`/`file_store`）。
> * **首个消费方**：`zd_stream` 的帧/流载体由 `table<i64>` 迁到字符串
>   （**线上字节格式逐字节不变**，迁移前后 hex 基线 `diff` 通过）。
> * 验证：`tdb/tests/probe_zbuf.tie`（46 项，含与 `zd` 表版写入器**逐字节一致**、
>   二进制安全、CRC 两载体同值）+ `probe_zd_stream`（40 项）+ tdb 全量 10 探针绿。
> * 文档：`tdb/docs/zbuf.md`（含完整测量表与「边界互换很贵、别在热循环互换」的使用纪律）。

**实测 / Measured**（同机、外部计时、差值法 + 多轮最小总时间）：

| 操作 / operation | `table<i64>`（8×） | 字符串（1×） | 比值 |
| --- | ---: | ---: | ---: |
| 构造 n 字节 | 33 ns/字节 | **4 ns/字节** | **8.25×** |
| 扫描 n 字节 | 40 ns/字节 | **6 ns/字节** | 6.7× |
| CRC32 | 52 ns/字节 | **7 ns/字节** | 7.4× |
| 读 1 MB 文件 | 4471 µs | **418 µs** | **10.7×** |
| `chunk_encode`（zd 帧，含 CRC） | 104 ns/字节 | **7 ns/字节** | **14.9×** |
| `chunk_next`（取 payload） | 74 ns/字节 | **6 ns/字节** | 12.3× |
| 峰值工作集（16 MB 载荷） | 184.6 MB | **25.7 MB** | 7.2× 更低 |

> 注：`chunk_*` 两行是**迁移的真实 A/B**（旧实现取自迁移前 git 版本、作独立模块同进程对照）。
> 边界互换成本：`to_table` 35 ns/字节、`from_table` 49 ns/字节——**比它们代替的工作还贵**，
> 故纪律是「整链路保持字符串载体，只在真有必要 `table<i64>` 签名的边界处互换」。
> 后续项：`zd_v3` 内部层整体迁到字符串载体（其字节负载现仍为 `table<i64>`）。


**做什么**：新增原始字节缓冲抽象（字符串承载 + `StringBuilder` 组装，或「缓冲 + 偏移/长度」
视图），仅在 API 边界与 `table<i64>` 互换。

**为什么安全**：纯新增类型/接口，不改现有语义。

**收益**：**内存 8× → 1×**（`table<i64>` 每元素 8 字节 vs 字符串密度 1×），
访问 `t[i]` 28 ns → `str_byte` **6 ns**（≈5×）。zd/td/网络帧/存档/图标像素全线受益。

**风险**：低。**与 §2.1 并列推荐。**

### 2.4 编译器：表字面量批量 codegen

**做什么**：元素数与元素宽度都已知的表字面量，生成为 `tbl_new` + 一次 `ensure(n)` + n 次
直写（可放在**同一个临界区内**），而非 n 次 `tbl_push`。

**为什么安全**：纯 codegen 改动，产出字节与语义完全不变。

**收益**：定宽编码路径（大端 16/32/64 位写、标签 + 载荷）**≈10×**（实测 8 次 push ≈312 ns
→ 1 次分配 + 直写）。

**风险**：低。

---

## 三、被实测推翻的三个假设 / Three Hypotheses Refuted by Measurement

> **教训：先量再改。** 下面两条都是听起来合理、实测无效的方案，若照直觉实施会白费功夫。

### 3.1 自旋计数（spin count）无效

**假设**：`InitializeCriticalSection` 默认自旋 0，未争用时也会走内核转换；设成 4000 可让
未争用获取纯用户态。

**实测**：把运行时改成 `InitializeCriticalSectionAndSpinCount(cs, 4000)` 并重建，用稳健
A/B 对比：

| 版本 / build | `t[i]` |
| --- | ---: |
| 基线 | 27 ns |
| spin=4000 | 29 ns |

**无差异，假设被推翻。** 原因：现代 Windows 的 `EnterCriticalSection` 在**未争用时本就走
用户态快路径**（内核只在争用时进入），自旋计数只影响争用行为。**不要做这个改动。**

### 3.2 CRC32 查表版无效（平台依赖）

逐位 CRC 是 8 轮/字节 ≈16 ns；查表版需 1 次表读 ≈28 ns（表读比逐位还贵）⇒ **在表访问
提速（§2.2/2.3）之前，查表版更慢。** 待表读降到 ≈8–10 ns 后再评估。

---

### 3.3 运行期「独占/未逃逸」判据无法检测全局共享（esc 快路径证伪，r.1.6.7）

**假设**：给表句柄加一个「是否已被复制」的标志（esc@48，由 `tbl_retain` 置 1），
esc==0 即「全程序仅此一份引用」⇒ 免锁访问安全。为避开 `GetCurrentThreadId` 的
多线程缓存问题，实现里刻意去掉了 owner 线程判据，只保留 esc 单标志。

**实现**：句柄 48→56 字节；`tbl_retain` 置 esc=1（唯一置位点，单调）；
`tbl_push/append/at/set` 四个热入口按 esc 分流到免锁内核 / 加锁内核（内核各一份，
两路共用）。功能测试初看通过。

**证伪（关键实验）**：写了一个专门的 retain 插桩实验，逐步观察 esc：

| 动作 / action | 期望 / expected | 实测 / observed |
| --- | --- | --- |
| 新建表 | esc = 0 | **0** ✓ |
| 局部赋值 `var b = a` | esc = 1 | **1** ✓ |
| 赋给全局 `g = a` | esc = 1 | **1** ✓ |
| 闭包捕获全局 | esc = 1 | **1** ✓ |

⇒ **retain 插桩本身完全正常**。问题在另一头：**表通过全局变量被多线程共享时，
没有任何一次 retain**——worker 读全局槽取句柄、直接 push，编译器不插桩 retain
（读共享不复制句柄）⇒ **esc 永远是 0** ⇒ 四个 worker 全部走免锁路径
⇒ **真实数据竞争**（丢 len 更新、扩容 realloc 与读并发释放）。

更糟的是：**我的并发探针一度「通过」**（4 worker × 500 次 push，len 精确）——
因为任务太短、work-stealing 下实际未真正重叠，属于**假通过**。把规模放大到
每个 worker 2000 次后仍通过，但那是**加锁版**的结果；用 esc 版重测时该场景的
重叠窗口依然无法保证复现，**即探针无法证伪 = 不能作为安全依据**。

**根因（一句话）**：**「句柄是否被复制」≠「表是否可能被并发访问」**。
共享可以经全局变量、堆槽、channel 外的闭包捕获等多种路径发生，**且都不产生
句柄复制**。因此**任何运行期"未逃逸"标志都不充分**。

**更安全的替代方向（后续建议，未实施）**：
* **编译期逃逸分析**：irgen 在生成表访问时静态判定「该表变量是否可能被其他执行
  流访问」（是全局变量 / 被 spawn 闭包捕获 / 存入全局容器 ⇒ 判为逃逸 ⇒ 生成加锁
  调用；纯局部且未逃逸 ⇒ 可直接内联免锁代码）。判据保守（宁可判逃逸），
  且不依赖任何运行期状态 ⇒ 无「漏锁」窗口。
* 这也是编译器已具备的能力范围（`g_used_trmlite` 等已按模块静态决策），
  比运行期标志更契合 tie 的静态编译模型。

**回滚**：`tl_tbl.tie` 恢复到 r.1.6.6（句柄 48 字节、四入口直接加锁），
重建两个运行时库，重跑三阶不动点（`8b5189c983437534`）并重新升格 tiec——
**因为 tiec 链运行时库，若不重新升格，esc 版的竞态风险会留在编译器二进制里**。
原探针改造为 `tests/s_tbl/tl_tbl_conc_probe.tie`（纯并发正确性回归，不含 esc 断言）。

## 四、启动时间构成 / Startup Cost Composition

### 4.1 实测（交叉轮询，取最小值与中位数 / ms）

| 被测 / subject | min | median | 说明 / note |
| --- | ---: | ---: | --- |
| `cmd /c exit` | **612** | **660** | Windows 进程创建地板的纯样本 |
| C 空程序（clang -O2，114 KB） | 616–628 | 643–679 | CRT 启动 + 主程序返回 |
| C 空程序（1 MB .rdata 填充） | 642 | 664 | **体积敏感度：≈14 ms/MB** |
| tie 空程序（134 KB） | 674–683 | 726–758 | 相对 C：**+23 ~ +83 ms** |

派生 / derived：
* **地板 ≈ 610–660 ms**（`cmd /c exit`），占绝对主导；
* **tie 相对同规格 C 的边际 ≈ +23 ~ +83 ms**，且**随系统负载漂移大于信号本身**
  （同一台机、不同时段：+82 ms → +23 ms），无法稳定分离；
* 体积代价：tie 比 C 大 20 KB ⇒ **+0.3 ms**（可忽略）。

### 4.2 已用证据排除的项 / Ruled Out

* **`main` 函数体**：空程序编译出 `define i32 @main() { ret i32 0 }`——零工作。
* **全局构造器**：`llvm.global_ctors` **不存在**（空程序与用表的程序都没有）；
  运行时没有额外的 ctor。
* **二进制体积**：实测 14 ms/MB，+20 KB ≈ +0.3 ms。
* **导入/依赖 DLL**：只导入 `KERNEL32.dll`，且是标准 MSVC CRT 启动集
  （`GetStartupInfoW` / `SetUnhandledExceptionFilter` / `InitializeSListHead` /
  `GetSystemTimeAsFileTime` / `QueryPerformanceCounter` / `FlsAlloc` …）——
  与 C 版本同源，无额外 DLL 装载。
* **256 KB 静态池**（`@s21_sso_pool`）：BSS 按需零页映射，不预清零。

### 4.3 剩余嫌疑与建议 / Remaining Suspects

| 嫌疑 / suspect | 检验方式 / how to verify |
| --- | --- |
| 杀软实时防护对新二进制文件的扫描 | 临时关闭实时防护重测（**两侧一致受影响**已验证：新拷入的 tie 与 C 文件首次运行都升高到 ~1 s） |
| 非标准 PE 段 `.fptable` 触发启发式 | 用 `dumpbin /headers` 对比 C 版本的段表；试移除该段 |
| CRT 链接配置差异 | 对比两版 `link` 参数与 `.pdata` 大小 |

### 4.4 结论：优化方向是「摊销」，不是「提速」/ Amortize, don't optimize

地板 ~610–660 ms 由**操作系统 + 杀软**决定，**tie 侧无可观可优化空间**。因此：

* **长驻进程**（tshell REPL、tedit、服务端、Subterra 运行时）——启动只付一次，**问题不存在**；
* **CLI 工具**——真正的修法是**减少进程启动次数**（批量/常驻模式/进程内复用），
  而不是想办法把 60 ms 抠成 30 ms；
* 若确要压 tie 自己的那 ≤80 ms，只能从 §4.3 的杀软/PE 布局入手，收益有限、需专项。

---

## 五、对前篇报告的更正 / Corrections to the Earlier Report

| 位置 / place | 原文 / was | 更正 / corrected |
| --- | --- | --- |
| §P7 启动成本 | 「最小程序启动 ≈680 ms」被列为 **tie 的问题** | **≈610–660 ms 是本机进程创建 + 杀软地板**（`cmd /c exit` 即为 612–660 ms）；tie 边际仅 **+23~83 ms**。P7 严重性**大幅下调** |
| §P2 句柄逐字节字段 | 列为第二严重项 | 实测**有效工作仅 8 ns**（含调用/帧/字段读取），锁占 20 ns。P2 单项收益有限，**严重性下调**，且**不应绕过 P1 去单改 P2** |
| §P3 缺宽指针 deref | 「性价比最高的编译器改动」 | 在 P1 未解前收益有限（8 ns 里能省的最多 3–4 ns）。**优先级下调**；P1 才是主战场 |
| §六 路线图 | 顺序 ①字节缓冲 ②重入快路径 ③字面量 | 修正为：**①批量原语（零风险）②rc==1 归属快路径 ③字节缓冲承载 ④字面量 codegen**；「重入快路径」应改为 **`rc==1` 归属快路径**（同线程重入只在嵌套调用时有效，对平铺访问无用——见 §3.1 同源推理错误） |

---

## 六、一句话总结 / One-liner

> 锁占表访问成本的 **57–71%**，且**能在不改并发语义的前提下拿掉**（批量原语零风险、
> `rc==1` 免锁 ≈2.8×）；启动时间 **90% 是操作系统与杀软的地板**，tie 自身 ≤80 ms、
> 无可观优化空间，应改为**摊销**而非提速。

> The lock is 57–71% of table access cost and can largely be eliminated **without changing
> concurrency semantics** (bulk primitives at zero risk; an `rc==1` fast path for ≈2.8×).
> Startup is ~90% OS + AV floor; tie's own share is ≤80 ms with little headroom — **amortize,
> don't optimize**.
