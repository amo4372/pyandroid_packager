# pyandroid-packager

把任意 Python 项目打包成 Android APK 的**通用套壳**工具（可发布到 PyPI 的 CLI）。

内置：
- 官方 CPython 3.14 嵌入式运行时（arm64-v8a / x86_64 预编译）
- SDL2 + SDL2_image / SDL2_mixer / SDL2_ttf + freetype（pygame-ce 全链路）
- PyOpenGL（GLES2 / EGL 路径）+ **ModernGL（NDK 交叉编译内置，两个 ABI）**
- OpenGL ES 系统库预加载（libGLESv2 / libEGL / libGLESv1_CM）
- 丧尸幸存者示例项目（assets/project，可替换）
- 无 adb 的内置崩溃捕获 + 诊断界面（DEBUG 包）/ 精简提示（RELEASE 包）

## 安装

```bash
pip install pyandroid-packager
```

## 使用

```bash
# 1. 环境自检（可选，推荐先跑）
pa doctor

# 2. 生成工程（默认内置运行时 + 示例项目，含 Python 运行时约 240MB）
pa init my_app

# 3. 指定你自己的 Python 项目 + 后端类型（自动检测引擎）
pa init my_app --project /path/to/your_python_project --backend auto

# 4. 编译（需要 gradle + JAVA_HOME + ANDROID_HOME）
pa build my_app --release
# 或直接用 Android Studio 打开 my_app 目录编译（一键）
```

## 后端类型

| backend | 说明 | 适用项目 |
|---|---|---|
| `sdl2` | SDL2 窗口 + pygame-ce | pygame / SDL 游戏 |
| `gles` | SDL 窗口 + GLES3 context | PyOpenGL(GLES2/EGL)、ModernGL、Ursina/Panda3D |
| `mixed` | SDL 窗口 + GLES3 context 并存 | pygame 与 OpenGL 混合（pygame UI + moderngl 渲染） |
| `console` | 无窗口终端，输出走 App 界面 | 脚本 / CLI 工具 |
| `auto` | 运行时读 main.py 自动检测（默认） | 通用 |

### auto 引擎识别规则

读 `project/main.py` 前 64KB 的 import：

| 检测到 | 后端 |
|---|---|
| `import pygame` 且无 GL | `sdl2` |
| `import moderngl` / `OpenGL` / `ursina` / `panda3d` | `gles` |
| `import pygame` + 任一 GL | `mixed` |
| 其他 | `console` |

> 注意：Android 只有 OpenGL ES，没有桌面 libGL。PyOpenGL 项目需使用
> `OpenGL.GLES2` / `OpenGL.EGL` 编写；桌面 OpenGL API（glBegin 等）不可用。
> ModernGL 的 `create_context()` 在 Android 上已自动放宽 require 到 GLES3
> （version_code 300~320），standalone 模式强制复用 SDL 已建好的 context。

## Ursina / Panda3D 项目

Ursina 基于 Panda3D（独立 C++ 渲染引擎，非 SDL/GLES 简单封装）。本工具**自动识别**
`import ursina` / `import panda3d` 并切换 GLES 后端 + 预建 GLES3 context，
但**运行时（panda3d 的 .so）需要你自行放入 site-packages**：

```bash
# 1. 用 python-for-android 构建 panda3d（耗时数小时），或用 p3d 官方 android 产物
# 2. 把 panda3d 包目录 + libp3*.so 放入：
#    app/src/main/assets/python/<abi>/lib/python3.14/site-packages/
# 3. 重新编译
```

Ursina 项目若只用少量 API（Entity / 基础输入），也可改用 pygame-ce 或
ModernGL 重写渲染层后直接用 `--backend gles` 打包（不需要 panda3d）。

## 工具命令

| 命令 | 说明 |
|---|---|
| `pa init <dir> [--project SRC] [--backend X] [--no-runtime]` | 生成工程；`--project` 自动校验 main.py 并检测引擎 |
| `pa build [dir] [--backend X] [--release] [--force]` | 编译 APK；构建前自动做工程完整性检查 |
| `pa doctor [dir]` | 环境自检（JAVA_HOME/ANDROID_HOME/gradle/工程完整性/运行时） |
| `pa info` | 显示工具与模板信息 |

## 工程结构

```
my_app/
├── settings.gradle / build.gradle / gradlew ...
└── app/
    ├── build.gradle        # -PpaBackend 控制后端（也可 pa build --backend 覆盖）
    └── src/main/
        ├── assets/
        │   ├── python/          # CPython 3.14 运行时（common + <abi>）
        │   ├── site-packages/   # pygame-ce、PyOpenGL、sitecustomize（可再放第三方库）
        │   ├── project/         # 你的 Python 项目（替换即可）
        │   └── wheels/          # pip 离线引导
        ├── jniLibs/             # SDL2 / Python / libc++_shared 预编译 .so
        ├── java/                # 套壳（MainActivity / SDLActivity / 解压 / 诊断）
        └── cpp/                 # python_bridge / SDL_main / crash_handler
```

## 瘦身模式

不需要内置运行时时可生成无运行时工程（后续自行补充 assets/python）：

```bash
pa init my_app --no-runtime
```

## 构建

```bash
pip install .
pa info
```

Linux 上编译（工具链需自行安装 NDK/SDK/Gradle，或用 Android Studio）：

```bash
pa build my_app
```
