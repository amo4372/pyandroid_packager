/*
 * sdl_main.cpp —— SDL 模式入口（pygame / SDL 项目专用）
 *
 * 运行方式（Android）：
 *   1) Java 侧 PythonSDLActivity（继承 org.libsdl.app.SDLActivity）先完成
 *      assets 解压，再进入 SDL 初始化；libSDL2.so 的 nativeInit JNI 会
 *      创建 SDL 主线程并调用本文件导出的 SDL_main()（dlsym 查找）。
 *   2) SDL_main 里通过 SDL_AndroidGetActivity() 拿到 Activity，
 *      取其 getFilesDir()，得到 filesDir/python、filesDir/project。
 *   3) 复用 python_bridge 的公共初始化（bridge_init / bridge_run），
 *      启动嵌入式 Python 并运行 project/main.py。
 *   4) pygame.display.set_mode() 在 SDL 主线程内使用 SDLActivity 创建的
 *      GLSurfaceView 渲染，游戏画面即“接管”屏幕。
 */
#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* SDL2 公共接口（libSDL2.so 已在 jniLibs，链接见 CMakeLists） */
#include <SDL.h>

#define LOG_TAG "sdl_main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* python_bridge.cpp 提供的公共函数 */
extern "C" int bridge_init(const char *python_home, const char *project_dir,
                           const char *native_lib_dir);
extern "C" int bridge_run(const char *project_dir, const char *script_name);
extern "C" void bridge_mark_stage(const char *stage);

/* crash_handler.cpp：native 崩溃捕获（写 filesDir/crash_native.txt） */
extern "C" void install_crash_handler(const char *files_dir);

/* 阶段标记统一走 bridge_mark_stage（从环境变量取 filesDir，写 sdl_stage.txt） */
static void mark_stage(const char *stage) {
    bridge_mark_stage(stage);
}

/* 通过 JNI 读取 Activity.getFilesDir() 的绝对路径（如 /data/user/0/.../files） */
static int get_files_dir(JNIEnv *env, jobject activity, char *out, size_t outlen) {
    jclass cls = env->GetObjectClass(activity);
    if (!cls) return -1;
    jmethodID mid = env->GetMethodID(cls, "getFilesDir", "()Ljava/io/File;");
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return -1;
    }
    jobject file = env->CallObjectMethod(activity, mid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return -1;
    }
    jclass fcls = env->GetObjectClass(file);
    jmethodID pathMid = env->GetMethodID(fcls, "getAbsolutePath", "()Ljava/lang/String;");
    jstring js = (jstring)env->CallObjectMethod(file, pathMid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(file);
        return -1;
    }
    const char *utf = env->GetStringUTFChars(js, NULL);
    if (!utf) {
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(js);
        return -1;
    }
    snprintf(out, outlen, "%s", utf);
    env->ReleaseStringUTFChars(js, utf);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(file);
    env->DeleteLocalRef(js);
    return 0;
}

/* 读取 Activity.getApplicationInfo().nativeLibraryDir */
static int get_native_lib_dir(JNIEnv *env, jobject activity, char *out, size_t outlen) {
    jclass cls = env->GetObjectClass(activity);
    if (!cls) return -1;
    jmethodID mid = env->GetMethodID(cls, "getApplicationInfo",
                                     "()Landroid/content/pm/ApplicationInfo;");
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return -1;
    }
    jobject ai = env->CallObjectMethod(activity, mid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        return -1;
    }
    jclass acls = env->GetObjectClass(ai);
    jfieldID fid = env->GetFieldID(acls, "nativeLibraryDir", "Ljava/lang/String;");
    jstring js = (jstring)env->GetObjectField(ai, fid);
    if (env->ExceptionCheck() || !js) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(ai);
        return -1;
    }
    const char *utf = env->GetStringUTFChars(js, NULL);
    if (utf) {
        snprintf(out, outlen, "%s", utf);
        env->ReleaseStringUTFChars(js, utf);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(ai);
    env->DeleteLocalRef(js);
    return utf ? 0 : -1;
}

/* 读取 backend 配置：filesDir/backend.txt（Java 侧写 BuildConfig.PA_BACKEND）。
   取值：auto / sdl2 / gles / console */
static void read_backend_config(const char *files_dir, char *out, size_t outlen) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/backend.txt", files_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(out, (int)outlen, f)) {
            out[strcspn(out, "\r\n")] = '\0';
        }
        fclose(f);
        return;
    }
    snprintf(out, outlen, "auto");
}

/* auto 检测：读 project/main.py 前 64KB，按 import 判断项目类型。
   规则（优先级）：
     moderngl / OpenGL / ursina / panda3d      -> gles
     pygame 且同时有 OpenGL 系                  -> mixed（SDL 窗口 + GLES context）
     pygame                                     -> sdl2
     其他                                       -> console */
static const char *detect_project_backend(const char *project_dir) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/main.py", project_dir);
    FILE *f = fopen(path, "r");
    if (!f) return "console";
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    int has_pygame = (strstr(buf, "import pygame") || strstr(buf, "from pygame")) ? 1 : 0;
    int has_gl = (strstr(buf, "import moderngl") || strstr(buf, "from moderngl") ||
                  strstr(buf, "import OpenGL") || strstr(buf, "from OpenGL") ||
                  strstr(buf, "import ursina") || strstr(buf, "from ursina") ||
                  strstr(buf, "import panda3d") || strstr(buf, "from panda3d")) ? 1 : 0;
    if (has_gl && has_pygame) return "mixed";
    if (has_gl) return "gles";
    if (has_pygame) return "sdl2";
    return "console";
}

/* SDLActivity 创建的窗口 ID 不固定（一般 0 或 1），枚举取第一个 */
static SDL_Window *get_main_window() {
    for (int i = 0; i < 8; i++) {
        SDL_Window *w = SDL_GetWindowFromID(i);
        if (w) return w;
    }
    return NULL;
}

extern "C" int SDL_main(int argc, char *argv[]) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) {
        LOGE("cannot get JNI env / activity");
        return 1;
    }

    char files_dir[1024] = "";
    if (get_files_dir(env, activity, files_dir, sizeof(files_dir)) != 0) {
        LOGE("getFilesDir failed");
        return 1;
    }
    LOGI("filesDir=%s", files_dir);

    char native_lib_dir[1024] = "";
    get_native_lib_dir(env, activity, native_lib_dir, sizeof(native_lib_dir));
    LOGI("nativeLibDir=%s", native_lib_dir);

    char python_home[2048];
    char project_dir[2048];
    snprintf(python_home, sizeof(python_home), "%s/python", files_dir);
    snprintf(project_dir, sizeof(project_dir), "%s/project", files_dir);

    /* 后端类型：auto 时按 main.py 的 import 自动检测 */
    char backend[16];
    read_backend_config(files_dir, backend, sizeof(backend));
    if (strcmp(backend, "auto") == 0 || backend[0] == '\0') {
        snprintf(backend, sizeof(backend), "%s", detect_project_backend(project_dir));
    }
    LOGI("backend=%s", backend);
    mark_stage(backend);

    /* gles / mixed 后端：显式创建 GLES3 context（SDL2 Android 走 EGL），
       供 PyOpenGL(GLES2) / ModernGL / Ursina(Panda3D) 使用；
       sdl2(pygame) 由 pygame 自己建。mixed 下 pygame 与 GL 并存。 */
    if (strcmp(backend, "gles") == 0 || strcmp(backend, "mixed") == 0) {
        SDL_Window *win = get_main_window();
        if (win) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GLContext gles_ctx = SDL_GL_CreateContext(win);
            if (gles_ctx) {
                SDL_GL_MakeCurrent(win, gles_ctx);
                LOGI("gles context created (EGL/GLES3)");
            } else {
                LOGE("gles context FAIL: %s", SDL_GetError());
            }
        }
    }

    /* 供 Python 侧（bootstrap/sitecustomize）落盘崩溃日志 + bridge 阶段标记 */
    setenv("PYTHON_ANDROID_FILES", files_dir, 1);

    /* 内置 native 崩溃捕获：SIGSEGV/SIGABRT 等崩溃瞬间写 backtrace 到
       filesDir/crash_native.txt，无 adb 也能在下次启动的诊断界面看到栈。 */
    install_crash_handler(files_dir);

    LOGI("PYTHONHOME=%s", python_home);
    mark_stage("python init: starting");

    int rc = bridge_init(python_home, project_dir, native_lib_dir);
    if (rc != 0) {
        LOGE("bridge_init failed rc=%d", rc);
        mark_stage("FAILED python init");
        return rc;
    }
    mark_stage("python ready, running project");

    /* 关键：把工作目录切到项目目录 —— 游戏的 game_log.txt/存档/配置
       都是相对路径，不 chdir 的话写到 CWD(/)，全部静默失败（日志丢失）。 */
    if (chdir(project_dir) != 0) {
        LOGE("chdir(%s) failed: %s", project_dir, strerror(errno));
    } else {
        LOGI("chdir -> %s", project_dir);
    }

    LOGI("running project main.py ...");
    int prc = bridge_run(project_dir, "main.py");
    LOGI("project exited rc=%d", prc);
    mark_stage(prc == 0 ? "project exited 0" : "project exited NONZERO");

    /* 正常退出：返回给 SDL，Activity 结束 */
    return prc;
}
