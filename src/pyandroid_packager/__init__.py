"""pyandroid-packager —— Python Android 通用套壳打包器。

内置官方 CPython 3.14 嵌入式运行时 + SDL2/pygame-ce + OpenGL ES 支持，
一键生成可编译的 Android Studio 工程，把任意 Python 项目打包成 APK。

后端类型（pa init --backend）：
  sdl2    —— SDL2 + pygame-ce（游戏/SDL 渲染）
  gles    —— OpenGL ES（PyOpenGL GLES2 / EGL，或 ModernGL）
  console —— 控制台终端（无窗口，stdout 走 App 界面）
  auto    —— 运行时按 main.py 的 import 自动检测（默认）
"""

__version__ = "0.1.0"
