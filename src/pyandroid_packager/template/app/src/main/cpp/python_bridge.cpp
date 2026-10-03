/*
 * python_bridge.cpp
 *
 * Python 3.14（Android 官方预编译包）的嵌入桥接层：
 *   - Java 传入 PYTHONHOME（解压后的标准库目录）与项目目录
 *   - 用官方推荐的 PyConfig 方式初始化解释器（行为等同于 `python <脚本>`）
 *   - 将 Python 的 sys.stdout / sys.stderr 重定向到 Android logcat + App 界面
 *   - 通过 runpy 运行项目入口脚本（sys.argv / __main__ 语义与桌面 Python 一致）
 *
 * 参考：python.org 官方文档 docs.python.org/3/using/android.html
 *      及 CPython 源码 Android/testbed/app/src/main/c/main_activity.c
 */
#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Python.h>

#define LOG_TAG "python_bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM *g_jvm = NULL;
static volatile int g_initialized = 0;   /* 解释器初始化完成标记 */
static char g_last_error[512] = "";      /* 最近一次初始化失败的原因（供界面显示） */

/* writeLog 的全局 class/method 引用（JNI_OnLoad 里预缓存，运行时零 FindClass） */
static jclass    g_write_class = NULL;
static jmethodID g_write_mid   = NULL;

static void set_last_error(const char *msg) {
    if (msg && *msg) {
        strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
        g_last_error[sizeof(g_last_error) - 1] = '\0';
    } else {
        g_last_error[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* JNI_OnLoad：缓存 JavaVM + 用 RegisterNatives 动态注册 native 方法。
 *
 * 为什么动态注册：JVM 默认按“导出符号名”查找 native 实现，若 .so 因
 * 构建缓存/链接可见性异常导致符号缺失，会出现 UnsatisfiedLinkError。
 * RegisterNatives 在库加载时直接绑定函数指针，不再依赖符号名解析，
 * 只要 .so 是当前源码编译的，绑定必然成功。
 * ------------------------------------------------------------------ */

/* 函数前向声明（RegisterNatives 需要函数指针） */
JNIEXPORT jint JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeInit(
    JNIEnv *env, jclass clazz,
    jstring jPythonHome, jstring jProjectDir, jstring jNativeLibDir);
JNIEXPORT jint JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeRunProject(
    JNIEnv *env, jclass clazz,
    jstring jProjectDir, jstring jScriptName, jobjectArray jArgs);
JNIEXPORT jstring JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeGetLastError(
    JNIEnv *env, jclass clazz);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    JNIEnv *env = NULL;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass cls = env->FindClass("com/example/pythonandroid/PythonBridge");
    if (!cls) {
        /* 本 .so 可能被 SDL 模式（PythonSDLActivity）加载，此时没有
           PythonBridge 类；不注册也不报错，stdout 桥接只走 logcat。 */
        env->ExceptionClear();
        return JNI_VERSION_1_6;
    }
    static const JNINativeMethod methods[] = {
        {"nativeInit",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
         (void *)Java_com_example_pythonandroid_PythonBridge_nativeInit},
        {"nativeRunProject",
         "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I",
         (void *)Java_com_example_pythonandroid_PythonBridge_nativeRunProject},
        {"nativeGetLastError",
         "()Ljava/lang/String;",
         (void *)Java_com_example_pythonandroid_PythonBridge_nativeGetLastError},
    };
    if (env->RegisterNatives(cls, methods, 3) != JNI_OK) {
        env->DeleteLocalRef(cls);
        return JNI_ERR;
    }
    /* 预缓存 writeLog 的全局 class/method 引用：
       JNI_OnLoad 运行在具有 Java 调用上下文的线程上，FindClass 必然可用。
       此后任何线程（含 Python threading 新线程）的 print 都直接用全局
       引用调用，绝不在运行时 FindClass —— 规避 ART 的
       AssertNoPendingException 断言崩溃。 */
    g_write_class = (jclass)env->NewGlobalRef(cls);
    g_write_mid = env->GetStaticMethodID(
        g_write_class, "writeLog", "(Ljava/lang/String;Z)V");
    if (!g_write_mid) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_WARN, "python_bridge",
                            "writeLog method not found, stdout->logcat only");
    }
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}

/* ------------------------------------------------------------------ */
/* _pybridge：Python 模块，write() 转发到 logcat + Java 界面           */
/* ------------------------------------------------------------------ */

/* 全局缓存：writeLog 的 class/method 引用（定义见文件顶部，JNI_OnLoad 预缓存）。
   任何线程（含 Python threading 新线程）的 print 都直接用全局引用，
   绝不在运行时 FindClass —— 否则无 Java 栈帧的线程会用系统 classloader
   找不到 app 类，或踩中已存在的 pending exception，触发 ART 的
   AssertNoPendingException 断言（SIGABRT）。 */
static void call_java_write(const char *text, int is_err) {
    if (!g_jvm) return;
    JNIEnv *env = NULL;
    /* NDK 的 C++ 包装签名是 AttachCurrentThread(JNIEnv **p_env, ...)，
       直接传 &env，不要用 C 风格的 (void **) 强转 */
    if (g_jvm->AttachCurrentThread(&env, NULL) != JNI_OK) {
        return;
    }
    if (!g_write_class || !g_write_mid) {
        /* 缓存未建立（非 SDL 场景），只走 logcat，不碰 JNI */
        return;
    }
    jstring js = env->NewStringUTF(text);
    if (!js) {
        env->ExceptionClear();
        return;
    }
    env->CallStaticVoidMethod(g_write_class, g_write_mid, js, is_err ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(js);
    /* 注意：这里【绝不】调用 DetachCurrentThread！
       SDL 模式下 Python 运行在 native SDLThread（pthread）上，ART 会检查
       "detach 时线程是否仍在运行代码"，每次 write 都 Detach 会触发
       "attempting to detach while still running code" abort（SIGABRT）。
       attached 状态由 ART 在进程/线程退出时自动清理。 */
}

static PyObject *pybridge_write(PyObject *self, PyObject *args) {
    const char *text;
    int is_err = 0;
    if (!PyArg_ParseTuple(args, "s|i", &text, &is_err)) {
        return NULL;
    }
    __android_log_write(
        is_err ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "python",
        text ? text : "");
    call_java_write(text ? text : "", is_err);
    Py_RETURN_NONE;
}

static PyMethodDef pybridge_methods[] = {
    {"write", pybridge_write, METH_VARARGS, "write(text, is_err)"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef pybridge_module = {
    PyModuleDef_HEAD_INIT,
    "_pybridge",
    "Android stdout/stderr bridge",
    -1,
    pybridge_methods,
};

static PyObject *PyInit__pybridge(void) {
    return PyModule_Create(&pybridge_module);
}

/* ------------------------------------------------------------------ */
/* 信号初始化（照搬官方 testbed）                                       */
/* ------------------------------------------------------------------ */
static void init_signals(void) {
    sigset_t set;
    if (sigemptyset(&set) || sigaddset(&set, SIGUSR1)) return;
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

/* ------------------------------------------------------------------ */
/* 公共初始化/运行逻辑（JNI 包装与 SDL_main 共用）                      */
/* ------------------------------------------------------------------ */

/* 阶段标记（native 崩溃时进程被杀，标记文件保留最后一步）：
   filesDir 从环境变量 PYTHON_ANDROID_FILES 读取（SDL_main 已 setenv） */
extern "C" void bridge_mark_stage(const char *stage) {
    const char *fd = getenv("PYTHON_ANDROID_FILES");
    if (fd && fd[0]) {
        char path[1400];
        snprintf(path, sizeof(path), "%s/sdl_stage.txt", fd);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", stage);
            fclose(f);
        }
    }
    LOGI("stage: %s", stage);
}

extern "C" int bridge_init(const char *python_home, const char *project_dir,
                           const char *native_lib_dir) {
    LOGI("PYTHONHOME=%s", python_home);
    LOGI("projectDir=%s", project_dir);
    LOGI("nativeLibDir=%s", native_lib_dir);

    /* 工作目录切换到项目目录，与桌面 `cd 项目 && python main.py` 一致 */
    if (chdir(project_dir) != 0) {
        LOGE("chdir(%s) failed: %s", project_dir, strerror(errno));
    }

    bridge_mark_stage("python init: chdir/signals");
    init_signals();

    /* 注册 _pybridge 模块必须在 Py_Initialize 之前 */
    PyImport_AppendInittab("_pybridge", PyInit__pybridge);

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;     /* 不解析命令行参数 */
    config.run_filename = 0;   /* 初始化后不立即运行脚本（保持解释器存活） */

    PyStatus status = PyConfig_SetBytesString(&config, &config.home, python_home);
    if (PyStatus_Exception(status)) {
        LOGE("PyConfig_SetBytesString(home) failed: %s",
             status.err_msg ? status.err_msg : "unknown");
        set_last_error(status.err_msg ? status.err_msg : "PyConfig_SetBytesString(home) failed");
        PyConfig_Clear(&config);
        return 1;
    }

    bridge_mark_stage("python init: Py_InitializeFromConfig");
    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        LOGE("Py_InitializeFromConfig failed: %s",
             status.err_msg ? status.err_msg : "unknown");
        set_last_error(status.err_msg ? status.err_msg : "Py_InitializeFromConfig failed (unknown)");
        PyConfig_Clear(&config);
        return 2;
    }
    PyConfig_Clear(&config);
    bridge_mark_stage("python init: Py_Initialize done (import site/sitecustomize OK)");

    /* Android 系统 CA：嵌入式 CPython 编译时默认 CA 路径无效，
       ssl 模块会读取 SSL_CERT_DIR/SSL_CERT_FILE 环境变量作为默认
       验证路径。把默认路径指向系统 CA 目录，urllib/requests 的
       create_default_context 即可正常验证 https 证书。 */
    setenv("SSL_CERT_DIR", "/system/etc/security/cacerts", 1);
    LOGE("SSL_CERT_DIR set to /system/etc/security/cacerts");

    /* 启动引导：把项目目录加入 sys.path，并替换 sys.stdout/sys.stderr；
       同时把输出落盘到 filesDir/crash.txt（SDL 模式崩溃诊断） */
    char bootstrap[10240];
    snprintf(bootstrap, sizeof(bootstrap),
        "import sys\n"
        "import os\n"
        "os.environ['ANDROID_NATIVE_LIB_DIR'] = '%s'\n"
        "sys.path.insert(0, '%s')\n"
        "try:\n"
        "    os.chdir('%s')\n"
        "    print('[bootstrap] CWD=' + os.getcwd(), file=sys.stderr)\n"
        "except Exception as e:\n"
        "    print('[bootstrap] chdir 失败: %%r' %% (e,), file=sys.stderr)\n"
        /* ---- 关键：RTLD_GLOBAL 预加载 libpython + SDL 全家 ----
           pygame 扩展 DT_NEEDED 不含 libpython，靠全局符号表解析
           PyExc_RuntimeError 等 Python API。必须在 import pygame 前
           以完整路径 + RTLD_GLOBAL 加载（bionic namespace 下相对名
           可能找不到）。此处直接在 bootstrap 执行，不依赖 sitecustomize
           的自动加载（它在 filesDir/site-packages，不在默认 sys.path）。 */
        "try:\n"
        "    import ctypes\n"
        "    _nlib = os.environ.get('ANDROID_NATIVE_LIB_DIR', '')\n"
        "    for _ln in ['libpython3.14.so','libSDL2.so','libSDL2_image.so',\n"
        "                'libSDL2_mixer.so','libSDL2_ttf.so','libfreetype.so',\n"
        "                'libcrypto_python.so','libssl_python.so','libc++_shared.so']:\n"
        "        _lp = os.path.join(_nlib, _ln) if _nlib else _ln\n"
        "        try:\n"
        "            ctypes.CDLL(_lp, mode=ctypes.RTLD_GLOBAL)\n"
        "            print('[bootstrap] RTLD_GLOBAL ok: ' + _ln, file=sys.stderr)\n"
        "        except Exception as _e:\n"
        "            print('[bootstrap] RTLD_GLOBAL FAIL: ' + _ln + ' ' + repr(_e), file=sys.stderr)\n"
        "    # OpenGL ES：PyOpenGL(GLES2)/ModernGL 的 ctypes 需要全局可见系统 GLES 库\n"
        "    # （libGLESv2/libEGL/libGLESv1_CM 在 /system/lib64 或 /vendor/lib64，\n"
        "    #  dlopen 默认搜索系统路径即可，无需 app 内复制）\n"
        "    for _ln in ['libGLESv2.so','libEGL.so','libGLESv1_CM.so']:\n"
        "        try:\n"
        "            ctypes.CDLL(_ln, mode=ctypes.RTLD_GLOBAL)\n"
        "            print('[bootstrap] RTLD_GLOBAL ok: ' + _ln, file=sys.stderr)\n"
        "        except Exception as _e:\n"
        "            print('[bootstrap] RTLD_GLOBAL skip: ' + _ln + ' ' + repr(_e), file=sys.stderr)\n"
        "except Exception as _e:\n"
        "    print('[bootstrap] preload error: ' + repr(_e), file=sys.stderr)\n"
        /* ---- 手动加载 sitecustomize（filesDir/site-packages）---- */
        "try:\n"
        "    _sp = os.path.join(os.environ.get('PYTHON_ANDROID_FILES',''), 'site-packages')\n"
        "    if os.path.isdir(_sp) and _sp not in sys.path:\n"
        "        sys.path.insert(0, _sp)\n"
        "    import sitecustomize\n"
        "    print('[bootstrap] sitecustomize loaded', file=sys.stderr)\n"
        "except Exception as _e:\n"
        "    print('[bootstrap] sitecustomize skip: ' + repr(_e), file=sys.stderr)\n"
        "import _pybridge\n"
        "_crash_fh = None\n"
        "def _open_crash_sink():\n"
        "    global _crash_fh\n"
        "    try:\n"
        "        base = os.environ.get('PYTHON_ANDROID_FILES')\n"
        "        if base:\n"
        "            _crash_fh = open(os.path.join(base, 'crash.txt'), 'a', encoding='utf-8', errors='replace')\n"
        "    except Exception:\n"
        "        _crash_fh = None\n"
        "_open_crash_sink()\n"
        "def _sink(s):\n"
        "    try:\n"
        "        if _crash_fh:\n"
        "            _crash_fh.write(s)\n"
        "            _crash_fh.flush()\n"
        "    except Exception:\n"
        "        pass\n"
        "class _AndroidStream:\n"
        "    def __init__(self, is_err=False):\n"
        "        self.is_err = is_err\n"
        "    def write(self, s):\n"
        "        if s:\n"
        "            _pybridge.write(s, self.is_err)\n"
        "            _sink(s)\n"
        "    def flush(self):\n"
        "        pass\n"
        "    def writelines(self, lines):\n"
        "        for line in lines:\n"
        "            self.write(line)\n"
        "    def isatty(self):\n"
        "        return False\n"
        "    def fileno(self):\n"
        "        return 2 if self.is_err else 1\n"
        "    def readable(self):\n"
        "        return False\n"
        "    def writable(self):\n"
        "        return True\n"
        "    def seekable(self):\n"
        "        return False\n"
        "    def __enter__(self):\n"
        "        return self\n"
        "    def __exit__(self, *a):\n"
        "        pass\n"
        "sys.stdout = _AndroidStream(False)\n"
        "sys.stderr = _AndroidStream(True)\n"
        "def _android_excepthook(tp, val, tb):\n"
        "    import traceback\n"
        "    s = ''.join(traceback.format_exception(tp, val, tb))\n"
        "    print('[python] 未捕获异常:\\n' + s, file=sys.stderr)\n"
        "sys.excepthook = _android_excepthook\n",
        native_lib_dir, project_dir, project_dir);

    if (PyRun_SimpleStringFlags(bootstrap, NULL) != 0) {
        LOGE("bootstrap failed");
        PyErr_Print();
        return 3;
    }
    bridge_mark_stage("python init: done");
    LOGI("Python %s initialized", Py_GetVersion());
    g_initialized = 1;
    return 0;
}

/* runpy 运行入口脚本（等价 `python main.py`），返回退出码 */
extern "C" int bridge_run(const char *project_dir, const char *script_name) {
    if (!g_initialized) {
        LOGE("bridge_run called before successful init");
        return 2;
    }

    char script_path[4096];
    snprintf(script_path, sizeof(script_path), "%s/%s", project_dir, script_name);

    /* sys.argv = [script_path] */
    PyObject *argv = PyList_New(1);
    if (!argv) return 1;
    PyList_SET_ITEM(argv, 0, PyUnicode_FromString(script_path));
    PyObject *sys = PyImport_ImportModule("sys");
    if (sys) {
        PyObject_SetAttrString(sys, "argv", argv);
        Py_DECREF(sys);
    }
    Py_DECREF(argv);

    int rc = 0;
    PyObject *runpy = PyImport_ImportModule("runpy");
    if (!runpy) {
        PyErr_Print();
        rc = 1;
    } else {
        /* 运行边界标记：确认 main.py 是否真的开始执行（进 crash.txt/logcat） */
        PyObject *sys_stderr = PySys_GetObject("stderr");
        if (sys_stderr) {
            PyObject_CallMethod(sys_stderr, "write", "s",
                                "[bridge] >>> 开始运行项目 main.py\n");
        }
        PyObject *res = PyObject_CallMethod(runpy, "run_path", "(s)", script_path);
        if (!res) {
            PyErr_Print();   /* 输出到重定向后的 stderr -> logcat + 界面 */
            rc = 1;
        } else {
            Py_DECREF(res);
        }
        if (sys_stderr) {
            char msg[128];
            snprintf(msg, sizeof(msg), "[bridge] <<< main.py 结束，返回码 %d\n", rc);
            PyObject_CallMethod(sys_stderr, "write", "s", msg);
        }
        Py_DECREF(runpy);
    }
    return rc;
}

/*
 * nativeInit(pythonHome, projectDir, nativeLibDir)
 * 必须在“Python 线程”上调用一次：初始化解释器并安装 stdout 桥接。
 * 返回 0=成功；1=home 设置失败；2=Py_InitializeFromConfig 失败；3=引导脚本失败。
 */
JNIEXPORT jint JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeInit(
    JNIEnv *env, jclass clazz,
    jstring jPythonHome, jstring jProjectDir, jstring jNativeLibDir) {

    const char *python_home = env->GetStringUTFChars(jPythonHome, NULL);
    const char *project_dir = env->GetStringUTFChars(jProjectDir, NULL);
    const char *native_lib_dir = env->GetStringUTFChars(jNativeLibDir, NULL);

    int rc = bridge_init(python_home, project_dir, native_lib_dir);

    env->ReleaseStringUTFChars(jPythonHome, python_home);
    env->ReleaseStringUTFChars(jProjectDir, project_dir);
    env->ReleaseStringUTFChars(jNativeLibDir, native_lib_dir);
    return rc;
}

/*
 * nativeGetLastError()
 * 返回最近一次 nativeInit 失败的详细原因（Py_InitializeFromConfig 的 err_msg），
 * 供界面直接显示，避免只能从 logcat 抓。
 */
JNIEXPORT jstring JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeGetLastError(JNIEnv *env, jclass clazz) {
    return env->NewStringUTF(g_last_error[0] ? g_last_error : "(no error)");
}

/*
 * nativeRunProject(projectDir, scriptName, args)
 * 必须在 nativeInit 之后、仍位于“Python 线程”上调用。
 * 等价于桌面： cd projectDir && python main.py [args...]
 * 返回进程级退出码（0 = 正常）。
 */
JNIEXPORT jint JNICALL
Java_com_example_pythonandroid_PythonBridge_nativeRunProject(
    JNIEnv *env, jclass clazz,
    jstring jProjectDir, jstring jScriptName, jobjectArray jArgs) {

    const char *project_dir = env->GetStringUTFChars(jProjectDir, NULL);
    const char *script_name = env->GetStringUTFChars(jScriptName, NULL);

    if (!g_initialized) {
        LOGE("nativeRunProject called before successful nativeInit");
        env->ReleaseStringUTFChars(jProjectDir, project_dir);
        env->ReleaseStringUTFChars(jScriptName, script_name);
        return 2;
    }

    char script_path[4096];
    snprintf(script_path, sizeof(script_path), "%s/%s", project_dir, script_name);

    /* sys.argv = [script_path, ...extra args] */
    jsize argc = env->GetArrayLength(jArgs);
    PyObject *argv = PyList_New(argc + 1);
    if (!argv) return 1;
    PyList_SET_ITEM(argv, 0, PyUnicode_FromString(script_path));
    for (jsize i = 0; i < argc; i++) {
        jstring js = (jstring)env->GetObjectArrayElement(jArgs, i);
        const char *utf = env->GetStringUTFChars(js, NULL);
        PyList_SET_ITEM(argv, i + 1, PyUnicode_FromString(utf ? utf : ""));
        env->ReleaseStringUTFChars(js, utf);
    }
    PyObject *sys = PyImport_ImportModule("sys");
    if (sys) {
        PyObject_SetAttrString(sys, "argv", argv);
        Py_DECREF(sys);
    }
    Py_DECREF(argv);

    /* runpy.run_path(path, run_name="__main__")：
       与 `python main.py` 的 __main__ / 相对导入语义一致 */
    int rc = 0;
    PyObject *runpy = PyImport_ImportModule("runpy");
    if (!runpy) {
        PyErr_Print();
        rc = 1;
    } else {
        PyObject *res = PyObject_CallMethod(runpy, "run_path", "(s)", script_path);
        if (!res) {
            PyErr_Print();   /* 输出到重定向后的 stderr -> logcat + 界面 */
            rc = 1;
        } else {
            Py_DECREF(res);
        }
        Py_DECREF(runpy);
    }

    env->ReleaseStringUTFChars(jProjectDir, project_dir);
    env->ReleaseStringUTFChars(jScriptName, script_name);
    return rc;
}
