/* r.1.6.11 Link-layer POSIX regex bridge — tie_regex_* five primitives over glibc.
 *
 * tie 的五正则内置（regex_match/find/find_all/group/replace）在【运行期 pattern】时由
 * irgen_regex.tie 的 rex_bridge_* 回退到符号 tie_regex_*（g_used_interp=true）。Linux 目标
 * 无 tie_interp.lib，故由 trm_lite_linux.a 的**本成员**提供同名五桥的 C 实现（内部用 glibc
 * <regex.h> regcomp/regexec，POSIX ERE）。irgen 侧零改动；r.1.6.16 主代理在 is_libc_sym
 * 登记这五个符号后 g_used_interp 不再置位。
 *
 * ABI 对齐（从 llvmgen_str.tie declare 与 irgen_regex.tie rex_bridge_* 调用核实）：
 *   i8  @tie_regex_match(ptr,ptr,ptr)   ; 前两参串数据指针(UTF-8,NUL 结尾),第三参=出参槽(写只读)
 *   ptr @tie_regex_find(ptr,ptr)        ; 返回串数据指针(NUL 结尾)；无匹配返回 "" 
 *   ptr @tie_regex_find_all(ptr,ptr)    ; 返回【表句柄】非字符串(is_table_bridge 禁补头)
 *   ptr @tie_regex_group(ptr,ptr,i64)   ; 组号 i64；返回组子串数据指针
 *   ptr @tie_regex_replace(ptr,ptr,ptr) ; repl 支持 $N/$0；返回替换结果数据指针
 *
 * tie 串内存模型（compiler/backend 既约）：
 *   串块 = [8B 长度头][数据][\0][32 尾部填充]，数据指针 = 块基址+8；长度在数据指针-8。
 *   短串(≤31B)通常走 SSO 静态池永不释放；长串走 malloc 由 @tie_str_free_if_heap(数据指针-8)
 *   释放（仅 len>31）。读侧只需数据指针(NUL 结尾)即可 strlen/strcmp；本 shim 用自持静态
 *   bump 池提供短/中标量串（镜像 SSO：永不释放、进程生命周期、零 malloc 泄漏）。
 *
 * find_all 返回的表句柄 = tl_tbl 布局 48B {cap@0,len@8,data@16,esz@24,lock@32,refcount@40}，
 * 元素 = 串数据指针(i64)。句柄**不自拼**：先调用 tl_tbl$tbl_new(8) 取得合法句柄（lock 已
 * InitializeCriticalSection@32、refcount=1、esz=8），再就地覆盖 cap/len/data 采纳自建缓冲
 * ——与编译期 s21_tbl_adopt 完全同构，消费方 tl_tbl$tbl_at 持锁读/表释放回收均安全。
 *
 * regex_t 布局脆弱 → 不向 IR 暴露句柄：五桥各自"收到 pattern 后查内部缓存（无则 regcomp 存）"，
 * 用后不 regfree（模块生命周期缓存）。编译一次、循环复用；find_all/replace 用 C 侧 realloc
 * 块增长缓冲，禁逐字符拼接；空匹配(rm_so==rm_eo)前进 1 字节防死循环。
 *
 * 无头文件写法：交叉编译（clang --target=x86_64-unknown-linux-gnu -c）无 Linux sysroot。<regex.h>
 * 手写 ABI：regcomp/regexec/regfree/malloc/free/memcpy/memset/strlen/realloc 全 extern 声明；
 * regmatch_t = {int rm_so; int rm_eo;}（glibc regoff_t 即 int，两项 8 字节）；regex_t 以
 * 足够大的字节缓冲承载（opaque，仅透传指针）。
 */

typedef long long int64_t;
typedef unsigned long size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif

extern int regcomp(void *preg, const char *pattern, int cflags);
extern int regexec(const void *preg, const char *string, size_t nmatch, void *pmatch, int eflags);
extern void regfree(void *preg);
extern void *malloc(size_t);
extern void free(void *);
extern void *realloc(void *, size_t);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern int bcmp(const void *, const void *, size_t);   /* clang 将 memcmp(x,y,..)==0 降级为 bcmp；glibc 提供 */
extern size_t strlen(const char *);

/* --- extern 链接到 trm_lite_linux.a 内的 tl_tbl 字节层表容器（tl_runtime.o 成员） --- */
extern int64_t tl_tbl$tbl_new(int64_t esz);          /* 返回 48B 句柄(i64=块指针)，lock 已 init，refcount=1 */

#define REG_EXTENDED 1    /* glibc POSIX 常量 */
#define REG_NOMATCH 1     /* glibc regexec 无匹配返回值 */

/* regmatch_t：glibc `typedef int regoff_t; struct { regoff_t rm_so; regoff_t rm_eo; }` */
typedef struct { int rm_so, rm_eo; } regmatch_t;

/* ============================================================
 * 1. pattern 预处理（tie 扩展速记 → 纯 POSIX ERE）
 * ============================================================
 * POSIX ERE 无 \d \w \s \t \n \r；glibc 会把 `\d` 当字面 `d` → 语义崩坏。故对运行期
 * pattern 做一次字节级改写再 regcomp（映射见审计报告 §5.1）。ERE 既有转义(\. \\ \* 等)
 * 原样保留。字符类内 [[:digit:]] 类表达式可并集（[\d]→[[:digit:]] 合法）。负类速记
 * \D \W \S 仅顶层展开；其含 `^` 无法在类内表达（文档化局限，探针不涉）。 */
static char *preprocess_pattern(const char *pat, int64_t n, int64_t *outlen) {
    int64_t cap = n * 4 + 16;
    char *out = (char *)malloc((size_t)cap);
    int64_t o = 0;
    for (int64_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pat[i];
        if (c == '\\' && i + 1 < n) {
            unsigned char e = (unsigned char)pat[i + 1];
            const char *ins = NULL;
            if (e == 'd') ins = "[[:digit:]]";
            else if (e == 'D') ins = "[^[:digit:]]";
            else if (e == 'w') ins = "[[:alnum:]_]";
            else if (e == 'W') ins = "[^[:alnum:]_]";
            else if (e == 's') ins = "[[:space:]]";
            else if (e == 'S') ins = "[^[:space:]]";
            if (ins) {
                int64_t il = (int64_t)strlen(ins);
                if (o + il >= cap - 1) {
                    cap = cap * 2 + il + 8;
                    out = (char *)realloc(out, (size_t)cap);
                }
                memcpy(out + o, ins, (size_t)il);
                o += il;
                i++; /* 消费反斜杠+速记符 */
                continue;
            }
            /* \t \n \r → 字面控制字节（tie rx_emit_cchar 同语义） */
            unsigned char lit = 0;
            if (e == 't') lit = 0x09;
            else if (e == 'n') lit = 0x0A;
            else if (e == 'r') lit = 0x0D;
            if (lit != 0 || e == 't' || e == 'n' || e == 'r') {
                /* 只有 \t\n\r 映射；其它 \X 走下方"保留转义"。lit!=0 覆盖 \t\n\r。 */
                if (lit != 0) {
                    if (o + 1 >= cap) { cap *= 2; out = (char *)realloc(out, (size_t)cap); }
                    out[o++] = (char)lit;
                    i++;
                    continue;
                }
            }
            /* 其它 \X：保留 `\X`（ERE 转义，\.\\\* 等 ERE 本义） */
            if (o + 2 >= cap - 1) { cap *= 2; out = (char *)realloc(out, (size_t)cap); }
            out[o++] = (char)c;
            out[o++] = (char)e;
            i++;
            continue;
        }
        if (o + 1 >= cap) { cap = cap * 2 + 16; out = (char *)realloc(out, (size_t)cap); }
        out[o++] = (char)c;
    }
    if (o >= cap) { cap *= 2; out = (char *)realloc(out, (size_t)cap); }
    out[o] = 0;
    *outlen = o;
    return out;
}

/* nsub = 未转义左圆括号数（POSIX ERE 无非捕获组，re_nsub 可靠）。类内 [[:...:]] 无括号。 */
static int count_groups(const char *ere, int64_t n) {
    int g = 0;
    for (int64_t i = 0; i < n; i++) {
        if (ere[i] == '\\') { i++; continue; }
        if (ere[i] == '(') g++;
    }
    return g;
}

/* ============================================================
 * 2. 编译缓存（模块生命周期，编译一次；结果永不放，不向 IR 暴露 regex_t 布局）
 * ============================================================ */
#define REGEX_T_BUF 512   /* glibc regex_t 远小于此；opaque 透传，含指针需 max_align（malloc 提供） */

typedef struct RC {
    char *key;             /* 预处理后 pattern（拥有） */
    int64_t keylen;
    unsigned char re[REGEX_T_BUF];  /* regex_t 承载 */
    int nsub;
    int ok;                /* regcomp 成功与否 */
    int live;
} RC;

static RC *g_cache = NULL;
static int g_cnt = 0, g_cap = 0;

/* 按 pattern 取得已编译缓存项（无则预处理+regcomp 入缓存）。pattern 为 tie 串数据指针。 */
static RC *rc_get(const char *pat) {
    int64_t plen = (int64_t)strlen(pat);
    int64_t ere_len = 0;
    char *ere = preprocess_pattern(pat, plen, &ere_len);
    for (int i = 0; i < g_cnt; i++) {
        if (g_cache[i].live && g_cache[i].keylen == ere_len &&
            memcmp(g_cache[i].key, ere, (size_t)ere_len) == 0) {
            free(ere);
            return &g_cache[i];
        }
    }
    if (g_cnt == g_cap) {
        int nc = g_cap ? g_cap * 2 : 8;
        g_cache = (RC *)realloc(g_cache, (size_t)nc * sizeof(RC));
        g_cap = nc;
    }
    RC *rc = &g_cache[g_cnt++];
    rc->key = ere;
    rc->keylen = ere_len;
    rc->nsub = count_groups(ere, ere_len);
    int ret = regcomp(rc->re, ere, REG_EXTENDED);
    rc->ok = (ret == 0);
    rc->live = 1;
    return rc;
}

/* ============================================================
 * 3. 自持静态 bump 池串（镜像 SSO：永不释放、进程生命周期）
 * ============================================================ */
#define REGEX_POOL (512 * 1024)
static char g_pool[REGEX_POOL];
static int64_t g_pool_off = 0;

/* 取 C 串（NUL 结尾）副本入池；池满回退 malloc（大结果 leak，探针不触边界）。 */
static char *pool_chars(const char *src, int64_t n) {
    int64_t need = n + 1;
    if (g_pool_off + need <= REGEX_POOL) {
        char *d = g_pool + g_pool_off;
        if (n) memcpy(d, src, (size_t)n);
        d[n] = 0;
        g_pool_off += need;
        return d;
    }
    char *d = (char *)malloc((size_t)need);
    if (n) memcpy(d, src, (size_t)n);
    d[n] = 0;
    return d;
}

/* 建 tie 格式串（[8B 长度头][数据][\0]），返回【数据指针】（供表元素/读方使用）。 */
static char *mk_tie_str(const char *base, int64_t so, int64_t eo) {
    int64_t n = (eo > so) ? (eo - so) : 0;
    int64_t need = 8 + n + 1 + 32;   /* 头 + 数据 + \0 + 尾部填充(容纳向量化宽读漏洞B修) */
    char *block;
    if (g_pool_off + need <= REGEX_POOL) {
        block = g_pool + g_pool_off;
        g_pool_off += need;
    } else {
        block = (char *)malloc((size_t)need);   /* 大串回退(理论不达探针边界) */
    }
    *(int64_t *)block = n;
    if (n) memcpy(block + 8, base + so, (size_t)n);
    block[8 + n] = 0;
    return block + 8;
}

/* ============================================================
 * 4. 五桥实现
 * ============================================================ */
int tie_regex_match(const char *s, const char *pat, char *out) {
    RC *rc = rc_get(pat);
    int r = 0;
    if (rc->ok) {
        regmatch_t pm[1];
        r = regexec(rc->re, s, 1, pm, 0);
    }
    int res = (rc->ok && r == 0) ? 1 : 0;
    if (out) *out = (char)res;   /* 只读出参槽（结果以返回值 i8 为准） */
    return res;
}

char *tie_regex_find(const char *s, const char *pat) {
    RC *rc = rc_get(pat);
    if (!rc->ok) return "";
    int nm = rc->nsub + 1;
    regmatch_t *pm = (regmatch_t *)malloc((size_t)nm * sizeof(regmatch_t));
    regmatch_t *p0 = pm;
    if (regexec(rc->re, s, (size_t)nm, pm, 0) != 0) {
        free(p0);
        return "";
    }
    int64_t so = pm[0].rm_so, eo = pm[0].rm_eo;
    char *res = pool_chars(s + so, eo - so);
    free(p0);
    return res;
}

char *tie_regex_group(const char *s, const char *pat, int64_t k) {
    RC *rc = rc_get(pat);
    if (!rc->ok) return "";
    int nm = rc->nsub + 1;
    regmatch_t *pm = (regmatch_t *)malloc((size_t)nm * sizeof(regmatch_t));
    regmatch_t *p0 = pm;
    if (regexec(rc->re, s, (size_t)nm, pm, 0) != 0) {
        free(p0);
        return "";
    }
    char *res = "";
    if (k == 0) {
        res = pool_chars(s + (int64_t)pm[0].rm_so, (int64_t)pm[0].rm_eo - (int64_t)pm[0].rm_so);
    } else if (k > 0 && k < (int64_t)nm) {
        if (pm[k].rm_so != -1) {   /* 该组未参与匹配 → "" */
            res = pool_chars(s + (int64_t)pm[k].rm_so, (int64_t)pm[k].rm_eo - (int64_t)pm[k].rm_so);
        }
    }
    free(p0);
    return res;
}

/* find_all：返回表句柄（tl_tbl 48B 布局，元素=串数据指针）。空匹配前进 1 防死循环。 */
void *tie_regex_find_all(const char *s, const char *pat) {
    RC *rc = rc_get(pat);
    int64_t h = tl_tbl$tbl_new(8);   /* 合法句柄：lock@32 init、refcount=1、esz=8 */
    if (!rc->ok) return (void *)h;
    int nm = rc->nsub + 1;
    regmatch_t *pm = (regmatch_t *)malloc((size_t)nm * sizeof(regmatch_t));
    regmatch_t *p0 = pm;
    int64_t slen = (int64_t)strlen(s);
    int64_t cap = 16, n = 0;
    int64_t *spans = (int64_t *)malloc((size_t)cap * 2 * 8);
    int64_t start = 0;
    while (start <= slen) {
        if (regexec(rc->re, s + start, (size_t)nm, pm, 0) != 0) break;
        int64_t so = (int64_t)pm[0].rm_so;
        int64_t eo = (int64_t)pm[0].rm_eo;
        if (n * 2 + 2 > cap * 2) {
            cap *= 2;
            spans = (int64_t *)realloc(spans, (size_t)(cap * 2 * 8));
        }
        spans[n * 2] = start + so;
        spans[n * 2 + 1] = start + eo;
        n++;
        if (eo == so) start += 1;      /* 空匹配 → 前进 1 字节 */
        else start += eo;
    }
    int64_t dcap = n > 8 ? n : 8;
    char **data = (char **)malloc((size_t)(dcap * 8));
    for (int64_t i = 0; i < n; i++) data[i] = mk_tie_str(s, spans[i * 2], spans[i * 2 + 1]);
    free(spans);
    free(p0);
    /* s21_tbl_adopt：就地采纳 cap/len/data（lock/esz/refcount 已由 tbl_new 备妥） */
    *(int64_t *)(h + 0) = dcap;
    *(int64_t *)(h + 8) = n;
    *(int64_t *)(h + 16) = (int64_t)data;
    return (void *)h;
}

static int max_group(const char *repl) {
    int m = 0;
    size_t L = strlen(repl);
    for (size_t i = 0; i < L; i++) {
        if (repl[i] == '$' && i + 1 < L && repl[i + 1] >= '0' && repl[i + 1] <= '9') {
            int v = 0;
            size_t j = i + 1;
            while (j < L && repl[j] >= '0' && repl[j] <= '9') { v = v * 10 + (repl[j] - '0'); j++; }
            if (v > m) m = v;
            i = j - 1;
        }
    }
    return m;
}

/* replace：全部替换；repl 支持 $N/$0。先 regcomp 一次，循环复用；结果 realloc 块增长拼接。 */
char *tie_regex_replace(const char *s, const char *pat, const char *repl) {
    RC *rc = rc_get(pat);
    if (!rc->ok) return s ? (char *)s : "";
    int nm = rc->nsub + 1;
    int mg = max_group(repl);
    if (mg + 1 > nm) nm = mg + 1;      /* 防 $N 越界读 pm */
    regmatch_t *pm = (regmatch_t *)malloc((size_t)nm * sizeof(regmatch_t));
    regmatch_t *p0 = pm;
    int64_t slen = (int64_t)strlen(s);

    int64_t ocap = (slen < 16 ? 16 : slen) + 16;
    char *out = (char *)malloc((size_t)ocap);
    int64_t o = 0;
#define OUT_APPEND(p,ln) do { if (o + (ln) >= ocap) { ocap = (ocap * 2 + (ln) + 8); out = (char*)realloc(out,(size_t)ocap); } memcpy(out+o,(p),(size_t)(ln)); o += (ln); } while(0)

    int64_t last = 0, start = 0;
    while (start <= slen) {
        if (regexec(rc->re, s + start, (size_t)nm, pm, 0) != 0) break;
        int64_t so = (int64_t)pm[0].rm_so;
        int64_t eo = (int64_t)pm[0].rm_eo;
        int64_t abs_so = start + so, abs_eo = start + eo;
        OUT_APPEND(s + last, abs_so - last);            /* 前缀/间隔 */
        /* 展开 repl 的 $N/$0 */
        size_t R = strlen(repl);
        for (size_t i = 0; i < R; i++) {
            if (repl[i] == '$' && i + 1 < R && repl[i + 1] >= '0' && repl[i + 1] <= '9') {
                int v = 0; size_t j = i + 1;
                while (j < R && repl[j] >= '0' && repl[j] <= '9') { v = v * 10 + (repl[j] - '0'); j++; }
                if (v == 0) {
                    OUT_APPEND(s + abs_so, abs_eo - abs_so);   /* $0 = 整体匹配 */
                } else if (v < nm && pm[v].rm_so != -1) {
                    /* pm 偏移相对 s+start：组子串绝对区间 = start+rm_so..start+rm_eo */
                    int64_t gso = start + (int64_t)pm[v].rm_so;
                    int64_t geo = start + (int64_t)pm[v].rm_eo;
                    OUT_APPEND(s + gso, geo - gso);
                }
                i = j - 1;
            } else {
                OUT_APPEND(&repl[i], 1);   /* 字面字符（$ 后无数字 → 字面 $） */
            }
        }
        last = abs_eo;
        if (eo == so) start += 1;
        else start += eo;
        if (start > slen) break;
    }
    OUT_APPEND(s + last, slen - last);   /* 尾缀 */
#undef OUT_APPEND
    free(p0);
    char *res = pool_chars(out, o);
    free(out);
    return res;
}