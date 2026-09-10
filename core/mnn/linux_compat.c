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
 */

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