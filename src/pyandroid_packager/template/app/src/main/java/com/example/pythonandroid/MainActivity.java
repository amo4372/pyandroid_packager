package com.example.pythonandroid;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;

/**
 * 主界面：启动即解压资源 -> 初始化 Python -> 运行 assets/project 中的项目，
 * 控制台实时显示 Python 的 stdout / stderr。
 */
public class MainActivity extends Activity {

    private TextView console;
    private volatile boolean pythonStarted = false;
    private long launchTimeMs = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        launchTimeMs = System.currentTimeMillis();

        console = findViewById(R.id.console);

        Button btnClear = findViewById(R.id.btn_clear);
        btnClear.setOnClickListener(v -> console.setText(""));
        if (!BuildConfig.DEBUG) {
            // RELEASE 包：删除日志界面 —— 隐藏“清空日志”，保留“重启游戏”
            btnClear.setVisibility(android.view.View.GONE);
        }

        Button btnRestart = findViewById(R.id.btn_restart);
        btnRestart.setOnClickListener(v -> restartGame());

        // Python 的 stdout/stderr 回调（来自 Python 线程）
        PythonBridge.setLogListener((text, isError) ->
                runOnUiThread(() -> appendConsole(text)));

        // TMPDIR：Android 只在 API 33+ 自动设置，Python 需要它（官方 testbed 同款处理）
        try {
            android.system.Os.setenv("TMPDIR", getCacheDir().toString(), false);
        } catch (android.system.ErrnoException e) {
            appendConsole("[java] 设置 TMPDIR 失败（忽略）: " + e);
        }

        // 后台线程执行：解压 -> init -> 运行项目
        new Thread(this::startPython, "PythonThread").start();
    }

    /** 重启游戏：清掉诊断标记，重新进入 SDL 渲染模式。 */
    private void restartGame() {
        appendConsole("==== 手动重启游戏 ====");
        File filesDir = getFilesDir();
        for (String name : new String[]{"crash.txt", "sdl_stage.txt", "import_trace.txt"}) {
            File f = new File(filesDir, name);
            if (f.exists()) {
                f.delete();
            }
        }
        launchTimeMs = System.currentTimeMillis();
        android.content.Intent intent =
                new android.content.Intent(this, PythonSDLActivity.class);
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 从 SDL 渲染模式返回（游戏异常退出/用户返回）后，把上次的崩溃日志
        // 直接显示在控制台界面，无需 adb 也能看到原因。
        checkCrashLog();
    }

    /** 诊断信息显示：崩溃循环时也能看到历史日志（不依赖 mtime 条件）。
     *  RELEASE 包不显示详细日志，仅提示“有错误（可尝试无视运行）”。 */
    private void checkCrashLog() {
        if (!BuildConfig.DEBUG) {
            // release：不输出任何内部路径/日志，仅给用户一句提示
            appendConsole("游戏遇到错误，可尝试点击“重启游戏”继续运行。");
            return;
        }
        try {
            File crash = new File(getFilesDir(), "crash.txt");
            if (crash.exists()) {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                String content = new String(java.nio.file.Files.readAllBytes(crash.toPath()), utf8);
                appendConsole("==== 上次运行日志（filesDir/crash.txt）====");
                appendConsole(content.trim().isEmpty() ? "(日志为空)" : content);
                appendConsole("=============================================");
            }
            // native 崩溃捕获（内置 crash handler 写入）：无 adb 时也能看崩溃栈
            File nativeCrash = new File(getFilesDir(), "crash_native.txt");
            if (nativeCrash.exists()) {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                String nc = new String(java.nio.file.Files.readAllBytes(nativeCrash.toPath()), utf8).trim();
                if (!nc.isEmpty() && nc.contains("NATIVE CRASH")) {
                    appendConsole("==== native 崩溃（filesDir/crash_native.txt）====");
                    appendConsole(nc);
                    appendConsole("（注意：显示到 ===== end ==== 之前的最后一行才是崩溃点）");
                }
            }
            // 无 Python 日志但有 native 阶段标记（进程被杀前的最后一步）
            File stage = new File(getFilesDir(), "sdl_stage.txt");
            if (stage.exists()) {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                String s = new String(java.nio.file.Files.readAllBytes(stage.toPath()), utf8).trim();
                appendConsole("==== 上次 SDL 阶段标记 ====");
                appendConsole("最后一步: " + (s.isEmpty() ? "(空)" : s));
                if (s.startsWith("python ready")) {
                    appendConsole("（解释器 OK，崩溃/退出发生在游戏代码或 SDL 渲染阶段）");
                } else if (s.equals("project exited 0")) {
                    appendConsole("（注意：SDL_main 正常返回 0 = 游戏是秒退而非崩溃，看下方 import/调用追踪）");
                }
            }
            // Python 侧 import 追踪：显示最后一个成功 import 的模块
            File trace = new File(getFilesDir(), "import_trace.txt");
            if (trace.exists()) {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                String t = new String(java.nio.file.Files.readAllBytes(trace.toPath()), utf8).trim();
                if (!t.isEmpty()) {
                    String[] lines = t.split("\n");
                    appendConsole("==== import 追踪（最后 15 个模块）====");
                    for (int i = Math.max(0, lines.length - 15); i < lines.length; i++) {
                        appendConsole("  " + lines[i]);
                    }
                    appendConsole("（崩溃点可能在最后一行之后）");
                }
            }
            // 游戏自己的日志 game_log.txt（chdir 后写到 filesDir/project/ 下）
            File gameLog = new File(getFilesDir(), "project/game_log.txt");
            if (gameLog.exists()) {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                String lg = new String(java.nio.file.Files.readAllBytes(gameLog.toPath()), utf8);
                String[] lines = lg.split("\n");
                appendConsole("==== 游戏日志 game_log.txt（最后 25 行）====");
                for (int i = Math.max(0, lines.length - 25); i < lines.length; i++) {
                    appendConsole(lines[i]);
                }
            } else {
                // 项目目录文件列表：判断游戏是否运行过（是否产生过任何文件）
                File project = new File(getFilesDir(), "project");
                if (project.isDirectory()) {
                    File[] files = project.listFiles();
                    appendConsole("==== filesDir/project 目录（前 15 个）====");
                    if (files == null || files.length == 0) {
                        appendConsole("(空目录)");
                    } else {
                        java.util.Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
                        for (int i = 0; i < Math.min(15, files.length); i++) {
                            appendConsole("  " + files[i].getName() +
                                    (files[i].isDirectory() ? "/" : "  " + files[i].length() + "B"));
                        }
                    }
                }
            }
        } catch (Throwable t) {
            // 读不到就忽略，不影响主流程
        }
    }

    private void startPython() {
        try {
            String abi = Build.SUPPORTED_ABIS[0];   // 如 arm64-v8a / x86_64
            appendConsole("ABI: " + abi);

            File pythonHome = AssetExtractor.extractPython(this, abi);
            File projectDir = AssetExtractor.extractProject(this);

            appendConsole("PYTHONHOME: " + pythonHome);
            appendConsole("PROJECT:    " + projectDir);

            // 后端类型：gradle -PpaBackend=sdl2|gles|console|auto 注入 BuildConfig；
            // auto 时按 project/main.py 的 import 自动检测。
            String backend = BuildConfig.PA_BACKEND;
            if (backend == null || backend.equals("auto") || backend.isEmpty()) {
                backend = detectBackend(projectDir);
            }
            writeBackendConfig(backend);
            appendConsole("[python] 后端: " + backend);

            // SDL 渲染后端（pygame=SDL2 / OpenGL/GLES / 混合）：走 PythonSDLActivity
            if (backend.equals("sdl2") || backend.equals("gles") || backend.equals("mixed")) {
                // 上次运行是否正常结束（project exited 0）。若上次崩溃/异常
                // 或没有结束标记，则停在本诊断界面，防止反复自动重启循环。
                File stage = new File(getFilesDir(), "sdl_stage.txt");
                boolean lastOk = stage.exists();
                if (lastOk) {
                    java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                    String s = new String(java.nio.file.Files.readAllBytes(stage.toPath()), utf8).trim();
                    lastOk = s.equals("project exited 0");
                }
                if (lastOk) {
                    appendConsole("[python] 切换到 SDL 渲染模式...");
                    android.content.Intent intent =
                            new android.content.Intent(this, PythonSDLActivity.class);
                    intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(intent);
                    // 注意：不 finish —— SDL 模式异常返回后回到本界面显示崩溃日志
                    return;
                }
                appendConsole("[python] 上次运行未正常结束，");
                appendConsole("[python] 已停留在诊断界面（查看上方日志；确认后点“重启游戏”继续）");
                return;
            }

            // 控制台后端：直接在本界面运行（不创建 SDL 窗口）
            int rc = PythonBridge.nativeInit(pythonHome.getAbsolutePath(),
                                             projectDir.getAbsolutePath(),
                                             getApplicationInfo().nativeLibraryDir);
            if (rc != 0) {
                String err = PythonBridge.nativeGetLastError();
                appendConsole("[python] 解释器初始化失败(rc=" + rc + "): " + err);
                appendConsole("[python] 详细原因请查看 logcat 的 python_bridge 标签");
                return;
            }
            pythonStarted = true;

            int prc = PythonBridge.nativeRunProject(
                    projectDir.getAbsolutePath(), "main.py", new String[0]);
            appendConsole("[python] 项目结束，退出码 " + prc);
        } catch (Throwable t) {
            // 用 Throwable 而非 Exception：OutOfMemoryError / LinkageError 也必须落盘
            appendConsole("[java] 启动失败: " + t);
            t.printStackTrace();
            saveCrash(t);
        }
    }

    /** 按 project/main.py 的 import 自动判断后端：pygame=SDL2 / OpenGL=GLES / 其他=console */
    /** 按 project/main.py 的 import 自动判断后端：
        pygame=SDL2 / moderngl|OpenGL|ursina|panda3d=GLES /
        pygame+GL=mixed / 其他=console */
    private String detectBackend(File projectDir) {
        try {
            File main = new File(projectDir, "main.py");
            byte[] buf = new byte[65536];
            int n;
            try (java.io.FileInputStream fis = new java.io.FileInputStream(main)) {
                n = fis.read(buf);
            }
            String s = new String(buf, 0, Math.max(n, 0), java.nio.charset.StandardCharsets.UTF_8);
            boolean hasPygame = s.contains("import pygame") || s.contains("from pygame");
            boolean hasGl = s.contains("import moderngl") || s.contains("from moderngl") ||
                    s.contains("import OpenGL") || s.contains("from OpenGL") ||
                    s.contains("import ursina") || s.contains("from ursina") ||
                    s.contains("import panda3d") || s.contains("from panda3d");
            if (hasGl && hasPygame) return "mixed";
            if (hasGl) return "gles";
            if (hasPygame) return "sdl2";
        } catch (Throwable ignore) {
            // main.py 读不到时按控制台处理
        }
        return "console";
    }

    /** 把后端类型写入 filesDir/backend.txt，SDL_main（native）启动时读取。 */
    private void writeBackendConfig(String backend) {
        try {
            java.io.File f = new java.io.File(getFilesDir(), "backend.txt");
            java.io.PrintWriter pw = new java.io.PrintWriter(new java.io.FileWriter(f, false));
            pw.print(backend);
            pw.flush();
            pw.close();
        } catch (Throwable ignore) {
        }
    }

    /** 把启动异常写入 filesDir/crash.txt，便于无 logcat 时排查。 */
    private void saveCrash(Throwable t) {
        try {
            java.io.File f = new java.io.File(getFilesDir(), "crash.txt");
            java.io.PrintWriter pw = new java.io.PrintWriter(new java.io.FileWriter(f, true));
            pw.println("==== " + new java.util.Date() + " ====");
            t.printStackTrace(pw);
            pw.flush();
            pw.close();
            appendConsole("[java] 崩溃详情已写入 " + f.getAbsolutePath());
        } catch (Throwable ignore) {
            // 写不了就算了，logcat 里 printStackTrace 还在
        }
    }

    /** 线程安全的控制台追加：任何线程调用都会切到 UI 线程执行。 */
    private void appendConsole(String line) {
        runOnUiThread(() -> {
            if (console == null) return;
            console.append(line);
            console.append("\n");
            android.widget.ScrollView sv = findViewById(R.id.scroll);
            sv.post(() -> sv.fullScroll(android.view.View.FOCUS_DOWN));
        });
    }
}
