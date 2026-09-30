# tie 运行时数据面性能问题报告 / tie Runtime Data-Plane Performance Report

* 日期 / Date: 2026-09-29
* 范围 / Scope: 表容器运行时（trm-lite `tl_tbl`）、编译器表构造 codegen、语言原语能力、内存占用
* 证据来源 / Evidence: 平台实测（斜率法差值计时）+ LLVM IR + `llvm-objdump` 反汇编
* 相关文档 / Related:
  * **续篇（必读 / must read）**: `2026-09-29-tie-perf-safety-and-startup.md`
    —— 锁占比实测（诊断变体）、安全优化路径、启动时间构成，**并更正本报告 P2/P3/P7 与路线图**
  * `tdb/docs/perf-zd.md`（zd 侧已落地优化与基准工具）

---

## 一、摘要 / Executive Summary

**中：** tie 的运行时数据面存在一个**单一主导瓶颈**——表容器 `tl_tbl` 的**每一次元素访问**都要
进出一次 CRITICAL_SECTION 锁，并对句柄字段做**逐字节**读写。实测表元素读取 **25–40ns**，而
同样是「取一个字节」的字符串访问只需 **6ns**（无锁）——**约 4–7 倍差距**，且随每次访问的
固定开销完全不随数据规模摊销。凡以表承载数据的代码路径（编解码、解析、序列化、批量处理）都
受此下限约束。第二个问题是**内存**：`table<i64>` 每元素占 8 字节，承载字节流时相对净荷
**膨胀 8 倍**（1 MB 载荷 → 8 MB 常驻），与「低内存」目标直接冲突。

**EN:** tie's runtime data plane has a single dominant bottleneck — the table container
(`tl_tbl`) takes a CRITICAL_SECTION lock on **every element access** and reads/writes its
handle fields **byte by byte**. Measured element read is **25–40 ns**, while reading a byte
from a string costs **6 ns** (unlocked) — a **4–7× gap** that never amortizes. Every
table-backed path (codec, parsing, serialization, batch processing) is bounded by it. The
second issue is **memory**: `table<i64>` uses 8 bytes per element, so carrying a byte stream
**bloats it 8×** (1 MB payload → 8 MB resident), directly conflicting with the low-memory goal.

---

## 二、测量方法 / Measurement Method

tie 只有**秒级** `time_now()`，且**最小程序启动成本实测 ≈ 680 ms**
（`func main() { println("hi") }`，含运行时初始化）。多数测项自身成本远小于此，直接计时
会被启动成本淹没，且秒级粒度无法区分。故：

1. **外部计时**：`date +%s%N` 取前 16 位（微秒精度）；
2. **斜率法差值**：进程内重复 `R` 次测项，取 `unit = (T(R) − T(R/2)) / (R/2)`，`R = 100–200`；
   固定启动成本在斜率中被抵消，测项成本被放大 2 个数量级、压过 ±90 ms 抖动；
3. **防优化**：测项结果一次性打印到 `sink` 防 DCE；索引依赖上一轮累加值防循环折叠。

tie only offers **second-granular** `time_now()` and a **≈ 680 ms** minimum program startup,
so measurements use external microsecond timestamps plus a **slope method**
(`unit = (T(R) − T(R/2)) / (R/2)`), with DCE/loop-folding guards.

工具 / Tooling: `tdb/tests/probe_zd_perf.tie`（20 个测项）+ `tdb/tests/bench.sh`（运行器）。

---

## 三、实测数据 / Measured Numbers

单次操作成本（ns/op），n = 100000–200000，R = 200（多次取样区间）：

| 操作 / operation | 路径 / path | ns/op | 备注 / note |
| --- | --- | ---: | --- |
| **`str_byte(s, i)`** | 字符串字节直读 | **6** | `(ptr,len)`，**无锁**，单指令 |
| **`t[i]` 表元素读** | `tl_tbl$tbl_at` | **25–40** | **加锁** + 逐字节字段 + 外部调用 |
| **`table_push(t, v)`** | `tl_tbl$tbl_push` | **40–85** | 加锁 + ensure + memcpy + set_len；波动来自扩容摊销（取样窗口内触发 realloc 的次数不同） |
| `byte_concat` / `str_sub_bytes` | 原生 memcpy | ≈ 0.1 | 批量原语参照上限 |
| `len(t)` | 句柄 @8 读 | ≈ 0 | 循环不变量，被 LLVM 提出循环 |
| zdb `pool_index`（旧，2000 条池） | 线性扫 + 逐条解码 | 99 904 | ← 已修（见 §五） |
| `zd.encode_bytes`（旧，200KB） | 逐元素 `table_push` | 33.4 ms/次 | ← 已修（见 §五） |

**对照结论 / key contrast：`str_byte` 6 ns vs `t[i]` 25–40 ns —— 两者都是「取一个字节」，
差别即锁与句柄访问方式。**

---

## 四、问题清单 / Findings（按影响排序）

### P1 — 表元素访问每次加锁 / Per-access locking (critical)

* **现象 / Symptom**：`t[i]` 25–40 ns、`table_push` 40–85 ns，且**不随规模摊销**（每次访问都是完整固定开销）。
* **根因 / Root cause**：`core/tbl/tl_tbl.tie` 的 `tbl_at` / `tbl_set` / `tbl_push` 全部以
  `EnterCriticalSection` + `LeaveCriticalSection` 包裹（p.6.7.2「表追加单步原子」的设计后果）。
  两次 kernel32 调用约 20 ns，是单次访问成本的主体。设计注释自陈对标 Go 的
  「容器并发不安全，append 由运行时锁保护」——但 Go 的 slice *元素* 访问 **无锁**，
  tie 连读都加锁。
* **影响 / Impact**：全部表承载路径。解码侧尤甚（解码热路径几乎全是「读一个字节 + 判断 + 取值」）。
* **建议 / Proposal**：
  1. **归属快路径 `rc==1 → 免锁`**（原写「同线程重入快路径」，**已更正**——同线程重入只在
     嵌套调用时有效，对平铺的单次访问无用）：句柄增 `owner_tid`；入口判
     `refcount == 1 && owner_tid == my_tid` 则免锁访问，否则走原加锁路径。`rc==1` 意味全程序
     只有一份引用、无第二线程可访问它 ⇒ 免锁无竞争对象（完整论证与已知移交窗口见续篇 §2.2）。
     预期 `t[i]` **28 → ≈10–12 ns（≈2.5–2.8×）**，需并发评审。
  2. 读路径 `tbl_at` 可考虑**免锁**（读与扩容 realloc 的竞态才是加锁动机；可在扩容时用
     「换址后旧缓冲延迟释放」或原子交换指针消除读侧竞态）。风险中等，需并发评审。

### P2 — 句柄字段逐字节读写 / Byte-wise handle field access

* **现象 / Symptom**：`tbl_at` 一次调用含约 **16 次 `movzbl`**（0x20–0x27 连续字节各读一次再移位或运算）。
* **根因 / Root cause**：`r_i64` / `w_i64` 实现为 8 次迭代的字节循环
  （`deref` / `deref_write` 对 `ptr<u8>` 是单字节存取）。**LLVM 未能把循环合并成单条
  8 字节 load/store**——已用 `llvm-objdump -d trm_lite.a` 反汇编确认（含 SIMD 打包序列）。
* **影响 / Impact**：每次字段访问放大 8–16 倍；`ensure_locked` 每次重新读 cap/len/data/esz
  （调用方刚读过，纯重复）。
* **严重性更正 / severity corrected（2026-09-29 续篇实测）**：本项**单项收益有限**——
  去锁诊断变体测得 `tbl_at` 的**有效工作仅 8 ns**（含调用、232 B 栈帧、全部字段读取与
  memcpy），而锁占 **20 ns**。**不应绕过 P1 单独改 P2。**
* **建议 / Proposal**：语言层提供**宽指针 deref**（下条 P3）后，`r_i64`/`w_i64` 各降为
  一条 load/store；在此之前可先用 `memset`/`memcpy` 桥或 `@llvm` 内建做整字访问。

### P3 — 缺少宽指针 deref（语言能力缺口）/ No wide-pointer deref

* **现象 / Symptom**：`var p: ptr<i64> = int_to_ptr(x)` 报 E00377——`int_to_ptr` 恒返回
  `ptr<u8>` 且**无转换形式**，因此 tie 层的所有结构体字段访问只能逐字节。
* **根因 / Root cause**：语言/编译器未开放宽指针类型转换（既有 `unsafe` 体系只覆盖 `ptr<u8>`）。
* **影响 / Impact**：这是 **P2 无法在 tie 层修复**的唯一原因，也是所有指针密集运行时代码
  （表容器、通道、GC、调度器）的共同上限。
* **严重性更正 / severity corrected**：P2 的有效工作总量只有 8 ns，**该项在 P1 未解前收益
  上限约 3–4 ns**。优先级**下调**——P1（锁）才是主战场。见续篇 §2.2/§5。
* **建议 / Proposal**：开放 `ptr_cast`/`int_to_ptr_typed` 或允许 `ptr<T>` 声明带显式转换
  （仍在 `unsafe` 内）。一处能力，全部运行时代码受益；但**排期应在 P1 之后**。

### P4 — `table<i64>` 承载字节 = 8× 内存膨胀 / 8× memory bloat

* **现象 / Symptom**：承载 n 字节净荷的表实占 **8n 字节**（每元素 8 字节），另加 48 字节
  句柄 + 每表 64 字节 CRITICAL_SECTION 缓冲。**1 MB 载荷 → 8 MB 常驻**。
* **根因 / Root cause**：tie 的字节约定即 `table<i64>`（元素 0..255），而表元素宽度固定 8 字节。
* **对照 / Contrast**：tie **字符串**是 `(ptr, len)`，字节密度 **1×**，且具备 memcpy 级能力
  （`str_byte` O(1)、`str_sub_bytes` 单次 memcpy 切片、`sb_append` 摊销 O(1)）——
  既省 8 倍内存，又快 5 倍。
* **影响 / Impact**：一切 blob / 缓冲 / 序列化中间产物；zd、td、网络帧、存档、图标像素。
* **建议 / Proposal**：新增**原始字节缓冲抽象**（字符串承载 + `StringBuilder` 组装，或
  「缓冲 + 偏移/长度」视图类型），zd/td 等上层仅保留**边界转换**到 `table<i64>`。
  无需编译器改动即可落地，是**零风险高收益**项。

### P5 — 表字面量无批量 codegen / No bulk table-literal codegen

* **现象 / Symptom**：`var out: table<i64> = [a, b, c, d, e, f, g, h]` 编译为
  `tl_tbl$tbl_new` + **8 次 `tl_tbl$tbl_push`**（IR 实证），仍是 8 次加锁追加。
* **根因 / Root cause**：表字面量按「逐元素追加」降级，未做「一次性分配 + 常量长度直写」。
* **影响 / Impact**：所有以字面量构造定宽编码（大端 16/32/64 位写、标签+载荷）的地方，
  每次多付 7 次加锁；配合 P1/P2 叠加放大。
* **建议 / Proposal**：编译器识别**元素数与静态元素宽度都已知**的表字面量
  （或新增 `table_lit_fill` 内建），生成 `tbl_new` + 一次 `ensure(n)` + n 次直写。
  纯 codegen 改动、语义不变，预期定宽编码路径 **≈10×**。

### P6 — 计时原语只有秒级 / Second-granular timing only

* **现象 / Symptom**：`time_now()` 返回整秒（无亚秒原语），`std/time` 的 `now_ms`/`tick_ms`
  只是 `* 1000` 换算，实际精度仍是 1 秒。
* **影响 / Impact**：可测性——任何亚秒级性能回归**无法在 tie 内检测**，必须依赖外部计时 +
  斜率法（本报告的方法），把简单测量变成专门工程。
* **建议 / Proposal**：补 `time_now_ns()`（或 `time_monotonic_us()`）原语；顺带让
  `std/time` 的 `tick_ms`/`elapsed_ms` 名副其实。

### P7 — 最小程序启动成本 ≈ 680 ms（**已更正 / corrected**）

* **现象 / Symptom**：`func main() { println("hi") }` 实测 **≈ 600–850 ms**（多次取样）。
* **已更正 / corrected（2026-09-29 续篇实测）**：该数字的**绝大部分不是 tie 的成本**——
  `cmd /c exit`（纯 Windows 进程创建）即 **612–660 ms**，同规格 C 空程序 **616–679 ms**。
  即 **≈90% 是本机进程创建 + 杀软地板**；tie 相对 C 的边际仅 **+23 ~ +83 ms**（随负载漂移）。
  已用证据排除：`main` 体（空程序为 `ret i32 0`）、全局构造器（无）、体积（+20 KB ≈ +0.3 ms，
  实测 14 ms/MB）、额外 DLL（只导入 KERNEL32 的标准 CRT 集）。
* **影响 / Impact（更正后）**：对**长驻进程无影响**（启动只付一次）；对 CLI 是**地板问题**，
  tie 侧可优化空间 ≤80 ms。**正确方向是摊销（减少进程启动次数），不是提速。**
* **建议 / Proposal**：见续篇 `2026-09-29-tie-perf-safety-and-startup.md` §4。若确要压那 ≤80 ms，
  只能从杀软/PE 布局（`.fptable` 非标准段）入手，需专项。

### P8 — `ensure_locked` 重复读字段 / Redundant field reads

* **现象 / Symptom**：`tbl_push` → `ensure_locked` 内部重新读取 cap/len/data/esz，而调用方
  刚刚读过同类字段。配合 P2 的逐字节读，纯重复开销。
* **建议 / Proposal**：把已读值作为参数传入，或让 `ensure_locked` 返回「新 data + 是否换址」。
  改动局部、收益随 P2 放大。

---

## 五、本次已修复 / Fixed in This Pass

以下为**已落地并回归通过**的项（zd 侧，见 `tdb/docs/perf-zd.md` 与提交 `8aa0576`）：

| 项 / item | 前 / before | 后 / after | 增益 / gain |
| --- | ---: | ---: | ---: |
| `zd.encode_bytes`：逐元素 push → `byte_concat(头, 载荷)` | 33.4 ms / 200 KB | **< 0.3 ms** | **> 100×** |
| `zd_extra` 池查询：线性扫池 → **哈希索引**（FNV-1a 32 开放寻址） | 99 904 ns | **4 222 ns** | **23.7×** |
| `zd.read_f64_be`：数学逆分解 → `bitcast_i64_f64` | 1672 ns | 616 ns | 2.7× |
| `zd.encode_f64` / `write_f64_be`：数学分解 → `bitcast_f64_i64` | 1543 ns | 940 ns | 1.6× |
| 定宽写（u16/u32/u64）+ `encode_i64` 定宽分支：N 次 push → 单次分配 | 8 push ≈ 312 ns | 1 次 ≈ 50 ns | ≈ 6× |

两点附带收益 / side benefits：

* f64 改 bitcast **语义更强**：旧数学分解把次正规数简化为 ±0（丢信息），bitcast 逐位精确
  （含次正规 / NaN / ±inf / ±0）。
* 池哈希索引修复了旧线性实现的**算法级退化**（O(池条目数 × 串长) → O(1) 均摊）。

---

## 六、建议路线图 / Recommended Roadmap

按「收益 ÷ 风险」排序（1 最优）：

| 序 / # | 动作 / action | 归属 / owner | 预期 / expected | 风险 / risk |
| ---: | --- | --- | --- | --- |
| 1 | 原始字节缓冲抽象（字符串/StringBuilder 承载） | 语言/std/zd | **内存 8× → 1×**；访问 ≈30 → 6 ns | **低**（纯新增） |
| 2 | ~~运行期归属快路径~~ **已证伪回滚**（续篇 §3.3）；**替代＝编译期独占分析，已落地**（续篇 §2.2b）| tiec（irgen） | 单次访问 ≈16 → ≈3 ns（**≈5×**）；整模式 1.3–1.7× | **r.1.6.7 已完成** |
| 3 | 编译器表字面量批量 codegen | tiec | 定宽编码 ≈ 10× | 低（纯 codegen） |
| 4 | 补 `time_now_ns` 原语 | 编译器/std | 可测性 | 低 |
| 5 | 语言宽指针 deref（`ptr<T>` 转换） | tiec | 字段访问 ≈ 4×，全运行时代码受益 | 中 |
| 6 | 批量追加原语 `tbl_append(h, src, n)` | trm-lite + tiec | 逐元素 → memcpy 级（> 100×） | 中 |
| 7 | 启动成本剖析（≈680 ms） | trm-lite + tiec | 未量化 | 需专项 |

**优先建议（2026-09-29 续篇修订 + 实测回填）**：① **批量原语**（已落地 r.1.6.6，零并发语义改变）→
② ~~归属快路径免锁~~ **已证伪回滚**（§3.3），替代方向 = 编译期逃逸分析 → ③ 字节缓冲承载（内存 1/8）→
④ **表字面量批量 codegen（已落地，实测 ≈10×）**。原始「同线程重入快路径」表述有误（对平铺访问无效，见续篇 §2.2 注）。
实测结论：**锁占 `t[i]` 的 71%、`table_push` 的 57%**，是唯一的大头；宽指针 deref（第 5 项）
在锁未解前收益上限仅 3–4 ns，**优先级下调**。

**EN:** Priorities 1–3 are independent, low-risk, and together reduce the most common
byte-stream-over-table scenario to **1/8 memory and ≈10× faster access**. Item 5 (wide-pointer
deref) is the gate for all further runtime optimization and deserves its own work item.

---

## 七、复现 / Reproduce

```bash
cd tie-repo/tdb
../tiec/compiler/tiec.exe tests/probe_zd_perf.tie --no-cache --no-warn -o /tmp/perf.exe

tests/bench.sh /tmp/perf.exe strread   200000 200   # 字符串字节读 / string byte read
tests/bench.sh /tmp/perf.exe readloop  200000 200   # 表元素读 / table element read
tests/bench.sh /tmp/perf.exe pushloop  200000 200   # 表追加 / table push
tests/bench.sh /tmp/perf.exe encbytes  200000 200   # 批量编码 / bulk encode
tests/bench.sh /tmp/perf.exe poolhash  20000  100   # 哈希池查询 / hashed pool lookup
```

测项全表见 `probe_zd_perf.tie` 头部注释。
反汇编证据 / disassembly evidence：`llvm-objdump -d trm-lite/trm_lite.a`（看 `tl_tbl$tbl_at`）。
