package com.example.pythonandroid;

import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * SDL 渲染模式入口（pygame / SDL 项目专用）。
 *
 * 与 MainActivity（纯控制台模式）的区别：
 *  - 本 Activity 由 org.libsdl.app.SDLActivity 承载：SDL 创建 GLSurfaceView
 *    原生窗口，pygame.display.set_mode() 直接在其上渲染，游戏画面接管屏幕。
 *  - Python 解释器在 SDL_main（libmain.so，SDL 主线程）里初始化并运行
 *    project/main.py；控制台输出走 logcat（_pybridge 桥接）。
 *
 * 使用条件（套壳自动判定，见 MainActivity）：
 *  - 已解压的 site-packages 里存在 pygame → 本项目需要图形窗口 → 走本入口。
 */
public class PythonSDLActivity extends SDLActivity {

    private static final String TAG = "PythonSDLActivity";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 锁横屏：丧尸幸存者为横屏游戏。必须在 super.onCreate(SDL 初始化) 之前
        // 锁定，否则设备方向与游戏方向不一致时，GLSurfaceView 会在方向变化时
        // 重建 Surface，而 pygame 项目不处理 VIDEORESIZE，直接闪退。
        try {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        } catch (Throwable t) {
            Log.w(TAG, "setRequestedOrientation failed: " + t);
        }

        // 确保 assets 已解压（MainActivity 已解压过，这里是幂等校验；若用户
        // 直接以本 Activity 为启动项也能正常工作，只是首次解压较慢）
        try {
            String abi = Build.SUPPORTED_ABIS[0];
            AssetExtractor.extractPython(this, abi);
            AssetExtractor.extractProject(this);
            Log.i(TAG, "assets 就绪，启动 SDL 渲染");
        } catch (Throwable t) {
            Log.e(TAG, "解压失败: " + t, t);
        }

        super.onCreate(savedInstanceState); // SDL 初始化 -> SDL 主线程 -> SDL_main()
    }
}
