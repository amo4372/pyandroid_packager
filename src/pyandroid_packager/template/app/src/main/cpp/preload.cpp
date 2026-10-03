/*
 * preload.cpp —— 全局符号预加载库
 *
 * 为什么需要它：
 *   python_bridge / main 通过 DT_NEEDED 链接 libpython3.14.so，Android linker
 *   默认以 RTLD_LOCAL 加载依赖库，导致 pygame 等扩展（dlopen 方式加载，
 *   DT_NEEDED 里没有 libpython）在解析 Python API 符号（PyArg_ParseTuple 等）
 *   时失败 —— 表现为 import pygame 时崩溃/失败。
 *
 * 解法：在 python_bridge / SDL 库加载【之前】先加载本库，用 dlopen(RTLD_GLOBAL)
 * 把 libpython3.14.so 和 SDL 全家放入全局符号表，后续库与扩展即可解析其符号。
 * （与 python-for-android 的 RTLD_GLOBAL 加载策略一致）
 */
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#define LOG_TAG "preload"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void load_global(const char *name) {
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        LOGI("RTLD_GLOBAL ok: %s", name);
    } else {
        LOGE("RTLD_GLOBAL FAILED: %s (%s)", name, dlerror());
    }
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm;
    (void)reserved;

    /* 必须最先加载 libpython3.14.so：pygame 扩展依赖其导出的 Python API 符号 */
    load_global("libpython3.14.so");

    /* SDL 全家：pygame 扩展（SDL2_image/mixer/ttf 的扩展）链接它们 */
    load_global("libSDL2.so");
    load_global("libSDL2_image.so");
    load_global("libSDL2_mixer.so");
    load_global("libSDL2_ttf.so");
    load_global("libfreetype.so");

    /* Python 自带 ssl 依赖的 openssl（改名 _python 后缀避免冲突） */
    load_global("libcrypto_python.so");
    load_global("libssl_python.so");

    return JNI_VERSION_1_6;
}
