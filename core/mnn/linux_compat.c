/* r.1.6.6 Linux 链接层 POSIX shim（并入 trm_lite_linux.a 成员）
 *
 * tie 编译器自身（semantic/llvmgen 计时、irgen_expr 浮点格式化）引用 Windows
 * CRT 专用符号 `GetTickCount`/`_gcvt`；glibc 无同名实现。Linux 程序链接时由
 * trm_lite_linux.a 提供本 shim（与 core/mnn/tl_linux_shim.tie 同构——Win32
 * 同名符号以 POSIX 对等物实现）：
 *   GetTickCount() -> 毫秒（CLOCK_MONOTONIC，单调语义对齐 Windows）；
 *   _gcvt(v,d,b)   -> 有效数字格式化（glibc 仅 gcvt，下划线变体不存在）。
 *
 * 无头文件写法：交叉编译（clang --target=x86_64-unknown-linux-gnu -c）时
 * 不需要 Linux sysroot 头文件；struct timespec 布局手排（x86_64：tv_sec=i64、
 * tv_nsec=i64，glibc long 即 64 位）。
 *
 * r.1.6.8：新增 `BCryptGenRandom`（std/csprng.tie `csrnd.strong_bytes` 的唯一
 * extern，Windows 为 bcrypt.dll 原生符号）的同名 POSIX 实现。Linux 无此符号，
 * 由 trm_lite_linux.a 的 compat 成员提供，tie 侧零改动。实现用 getrandom(2)
 * 直接 syscall（x86_64 rax=318，glibc 无标准封装也无需头文件）一次成块填充。
 */

#include <stddef.h>   /* size_t（freestanding 头，交叉编译可用） */

typedef long long int64_t;
typedef int64_t time_t_64;

struct compat_timespec {
    time_t_64 tv_sec;   /* glibc: time_t（x86_64 = long = 8 字节） */
    long tv_nsec;       /* glibc: long（8 字节） */
};

extern int clock_gettime(int clockid, struct compat_timespec *tp);
extern char *gcvt(double value, int ndigit, char *buf);

int64_t GetTickCount(void) {
    struct compat_timespec ts;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

char *_gcvt(double value, int ndigit, char *buf) {
    return gcvt(value, ndigit, buf);
}

/* ---- r.1.6.8：BCryptGenRandom POSIX shim（getrandom syscall rax=318）---- */
/* EINTR=4（x86_64 Linux errno 表第一项 = EAGAIN/EINTR，无需 errno.h） */
enum { SYS_GETRANDOM_X86_64 = 318, EINTR_LINUX = 4 };

/* getrandom(2) 直接 syscall：rax=318，rdi=buf，rsi=buflen，rdx=flags。
 * 返回写入字节数；错误返回 -errno。无头文件写法：syscall 寄存器约束手排，
 * rcx/r11 被内核乱写，声明为 clobber。 */
static long compat_sys_getrandom(void *buf, size_t buflen, unsigned int flags) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_GETRANDOM_X86_64), "D"(buf), "S"(buflen),
                       "d"((unsigned long)flags)
                     : "rcx", "r11", "memory");
    return ret;
}

int BCryptGenRandom(void *hAlgorithm, unsigned char *pbBuffer, unsigned int cbBuffer, unsigned int dwFlags) {
    (void)hAlgorithm;   /* NULL 句柄 + 0x2 flag 用系统首选 RNG，忽略句柄 */
    if (!(dwFlags & 2u)) /* BCRYPT_USE_SYSTEM_PREFERRED_RNG=0x2 */
        return -1;      /* 非 2 → 失败（STATUS_INVALID_PARAMETER 类比，tie 侧 !=0 判定） */

    /* 性能纪律：一次成块直接填充请求缓冲（flags=0 内核阻塞直至足额）；
     * 循环仅处理 EINTR 与理论短读补齐，禁止逐字节。 */
    unsigned char *p = pbBuffer;
    size_t left = (size_t)cbBuffer;
    while (left > 0) {
        long n = compat_sys_getrandom(p, left, 0u);
        if (n > 0) {
            p += (size_t)n;
            left -= (size_t)n;
        } else if (n == -EINTR_LINUX) {
            /* 被信号打断：重试——flags=0 保证再次阻塞至足额 */
            continue;
        } else {
            return -1;  /* 其余错误（-errno）→ 失败 */
        }
    }
    return 0;           /* STATUS_SUCCESS */
}