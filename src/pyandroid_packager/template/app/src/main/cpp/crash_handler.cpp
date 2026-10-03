/*
 * crash_handler.cpp —— 内置 native 崩溃捕获（无 adb 环境专用）
 *
 * 在 SDL 主线程初始化后安装 SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE 处理器，
 * 崩溃瞬间把信号信息 + backtrace（PC 地址 + dladdr 解析的符号）写入
 * filesDir/crash_native.txt。App 下次启动回到诊断界面即可直接看到
 * native 崩溃栈，无需 logcat / adb。
 *
 * 用法：extern "C" void install_crash_handler(const char* files_dir);
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

#include <android/log.h>

#define LOG_TAG "crash_handler"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static char g_log_path[512] = {0};
static int  g_log_fd = -1;

/* 信号名（不用 strsignal，避免静态缓冲非 async-safe） */
static const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "SIG?";
    }
}

static void log_write(const char* s) {
    size_t n = strlen(s);
    if (g_log_fd >= 0) {
        (void)write(g_log_fd, s, n);
    } else if (g_log_path[0]) {
        int fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            (void)write(fd, s, n);
            close(fd);
        }
    }
    LOGE("%s", s);
}

static void log_writef(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    log_write(buf);
}

struct bt_ctx {
    void* frames[64];
    int count;
};

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context* ctx, void* arg) {
    struct bt_ctx* b = (struct bt_ctx*)arg;
    if (b->count >= 64) return _URC_END_OF_STACK;
    b->frames[b->count++] = (void*)_Unwind_GetIP(ctx);
    return _URC_NO_REASON;
}

static void crash_handler(int sig, siginfo_t* info, void* uc) {
    (void)uc;
    log_writef("\n=== NATIVE CRASH signal %d (%s) ===\n", sig, sig_name(sig));
    log_writef("si_code=%d si_addr=%p\n",
               info ? info->si_code : -1,
               info ? info->si_addr : NULL);

    struct bt_ctx bt;
    memset(&bt, 0, sizeof(bt));
    _Unwind_Backtrace(bt_cb, &bt);

    for (int i = 0; i < bt.count; i++) {
        void* pc = bt.frames[i];
        Dl_info di;
        if (dladdr(pc, &di) && di.dli_fname) {
            const char* sym = di.dli_sname ? di.dli_sname : "?";
            uintptr_t base = (uintptr_t)di.dli_fbase;
            uintptr_t off = (uintptr_t)pc - base;
            log_writef("  #%02d pc 0x%lx  %s (%s)\n", i, off, di.dli_fname, sym);
        } else {
            log_writef("  #%02d pc %p (unknown)\n", i, pc);
        }
    }
    log_writef("=== end ===\n");

    /* 恢复默认行为并退出，避免 handler 重入死循环 */
    signal(sig, SIG_DFL);
    _exit(128 + sig);
}

extern "C" void install_crash_handler(const char* files_dir) {
    if (files_dir && files_dir[0]) {
        snprintf(g_log_path, sizeof(g_log_path), "%s/crash_native.txt", files_dir);
        g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (g_log_fd >= 0) {
            log_writef("=== crash handler installed (pid=%d) ===\n", getpid());
        } else {
            g_log_fd = -1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    LOGE("crash handler installed");
}
