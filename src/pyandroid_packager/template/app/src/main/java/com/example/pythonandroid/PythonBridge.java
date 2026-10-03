package com.example.pythonandroid;

/**
 * Python 嵌入桥接层的 Java 侧声明。
 *
 * nativeInit / nativeRunProject 必须在同一个“Python 线程”上按顺序调用；
 * writeLog 由 C 层（_pybridge 模块）从 Python 线程回调，用于把
 * sys.stdout / sys.stderr 送到 App 界面。
 */
public final class PythonBridge {

    public interface LogListener {
        /** @param text 一行输出（可能为空串） @param isError true=stderr */
        void onLog(String text, boolean isError);
    }

    private static volatile LogListener sListener = null;

    private PythonBridge() {
    }

    public static void setLogListener(LogListener listener) {
        sListener = listener;
    }

    /** 由 native 层调用（静态方法，见 python_bridge.cpp call_java_write）。 */
    @SuppressWarnings("unused")
    public static void writeLog(String text, boolean isError) {
        LogListener l = sListener;
        if (l != null) {
            l.onLog(text != null ? text : "", isError);
        }
    }

    /** 初始化解释器（必须在 Python 线程调用）。返回 0=成功；非 0=失败原因见 logcat。 */
    public static native int nativeInit(String pythonHome, String projectDir,
                                        String nativeLibDir);

    /** 运行项目入口脚本，等价于 `python main.py [args...]`。返回退出码。 */
    public static native int nativeRunProject(
            String projectDir, String scriptName, String[] args);

    /** 返回最近一次 nativeInit 失败的详细原因（便于界面直接显示）。 */
    public static native String nativeGetLastError();

    static {
        // 必须先加载 preload：用 RTLD_GLOBAL 预加载 libpython3.14.so / SDL，
        // 否则 pygame 扩展 dlopen 时解析不到 Python API 符号而崩溃
        System.loadLibrary("preload");
        System.loadLibrary("python_bridge");
    }
}
