# PythonAndroid —— 内嵌 Python 3.14 官方安卓运行时，启动即运行你的 Python 项目

一个"开箱即用"的 Android Studio 工程，目标只有一个：**把 Python 项目装进 APK，打开 App 就运行**。

- **内嵌 Python 3.14.7**（python.org 官方发布的 `*-linux-android.tar.gz` 预编译包，arm64-v8a + x86_64），解释器行为与桌面 `python` 完全一致；
- **启动即运行你的 Python 项目**：把项目整体丢进 `app/src/main/assets/project/`，App 启动后自动 `cd 项目目录 && python main.py`，文件原样进入 APK，不做任何改写；
- **项目目录可写**（落盘在 App 私有目录 `filesDir/project`），`os.getcwd()` 就是项目目录；
- **stdout/stderr 实时显示**在 App 界面控制台上；
- **pip 可引导**：首次启动自动把 assets 里的 pip/setuptools 装进 site-packages，之后可 `python -m pip install <包>`（需网络）；
- **极简**：无 SDL3 演示、无 AndroidX、无 pygame（如需 pygame/SDL2 可运行 `scripts/build_pygame_android.sh` 加回，见文末）。

---

## 目录结构

```
PythonAndroid/
├── app/
│   ├── build.gradle              # AGP 8.7.3 / compileSdk 35 / minSdk 26
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── assets/
│       │   ├── python/common/            # 标准库纯 Python 部分（两 ABI 共享）
│       │   ├── python/<abi>/lib-dynload/ # 标准库 C 扩展（按 ABI 分开）
│       │   ├── site-packages/            # 你的 Python 库（共享，空目录+说明）
│       │   ├── wheels/                   # pip / setuptools 离线 wheel
│       │   └── project/                  # ★你的 Python 项目（启动即运行 main.py）
│       ├── cpp/
│       │   ├── python_bridge.cpp         # JNI 桥：PyConfig 初始化 + 运行项目（RegisterNatives 动态注册）
│       │   └── CMakeLists.txt
│       ├── java/com/example/pythonandroid/  # MainActivity / PythonBridge / AssetExtractor
│       └── res/                          # 布局、主题、矢量图标
├── scripts/
│   ├── prepare.py                # 一键下载/组装 Python 运行时（幂等，含缓存）
│   └── build_pygame_android.sh   # 可选：交叉编译 SDL2 + Pygame-CE（默认不启用）
├── third_party/                  # 下载缓存（gitignore）
└── README.md
```

## 快速开始

1. 用 Android Studio（Koala+，内置 JDK 17）**打开本目录**（不要点 "New Project"）。
2. 连接 arm64 手机或 x86_64 模拟器，直接 `Run`。
3. 启动后自动运行 `assets/project/main.py`，输出实时显示在控制台。

> 本仓库已把 Python 运行时组装好（jniLibs + assets/python），不需要再跑脚本。
> `scripts/prepare.py` 用于**重新生成 / 换版本**。

## 把 Python 项目换成你自己的（例如"丧尸幸存者"）

```text
app/src/main/assets/project/
├── main.py            ← 入口（保持这个名字，或改 MainActivity.java 里的 "main.py"）
├── VERSION            ← 更新项目后把这里的数字 +1（触发手机端重新解压）
└── ...你的其他文件/包
```

把你项目的所有文件/目录整体拷进 `assets/project/`，在 `main.py` 里调用你的启动函数：

```python
# assets/project/main.py
from zombie_survivor import main as game   # 例子：换成你的项目
game.run()
```

规则与桌面完全一致：入口脚本的 `sys.path[0]`、`__main__`、相对导入都按
`python main.py` 的语义工作；`os.getcwd()` 就是项目目录；项目目录可写。
APK 里的项目文件与你在 assets 里放的一模一样。

## 这个 Android 里的 Python 与 Windows 安装的 Python 对应关系

| Windows 安装的 Python | 本 APK 内 |
|---|---|
| `python.exe` | 内嵌解释器（libpython3.14.so，JNI 启动） |
| `Lib\` | `assets/python/common/...` → `filesDir/python/lib/python3.14/` |
| `Lib\site-packages\` | `assets/site-packages/` → `filesDir/python/lib/python3.14/site-packages/` |
| `DLLs\` | `app/src/main/jniLibs/<abi>/`（libpython3.14.so 等） |
| `Scripts\pip.exe` | `python -m pip`（首次启动自动引导） |

## 第三方 Python 库怎么装

- **纯 Python 包 / 有安卓 wheel 的包**：PyPI 已支持 `cp314-cp314-android_24_arm64 / _x86_64` 标签的安卓 wheel。把 `.whl` 放进 `assets/wheels/` 离线装，或直接在设备上 `python -m pip install 包名`。
- **需要 C 编译的包**：优先找安卓 wheel，或自行交叉编译后放进 `site-packages`（注意命名 `cpython-314-<平台>.so`）。

## 版本与来源（全部官方发布）

| 组件 | 版本 | 来源 |
|---|---|---|
| CPython (Android) | 3.14.7 (aarch64 / x86_64) | https://www.python.org/downloads/android/ |
| pip / setuptools | 26.2.1 / 84.0.0 | PyPI |
| AGP / Gradle | 8.7.3 / 8.9 | google / gradle.org |

## 技术要点（遇到问题先看这里）

- **为什么能像桌面 Python**：JNI 层用 CPython 官方推荐的 `PyConfig.home + Py_InitializeFromConfig` 初始化，再用 `runpy.run_path` 运行入口脚本——与 `python main.py` 等价。
- **stdlib 为什么分两层**：纯 Python 部分两 ABI 共享（省体积），`lib-dynload` 里的 C 扩展 `.so` 按 ABI 分开。
- **`.so` 必须能按路径 dlopen**：`packagingOptions.jniLibs.useLegacyPackaging = true`，保证 libpython、扩展模块都能从磁盘加载。
- **JNI 动态注册**：`python_bridge.cpp` 的 `JNI_OnLoad` 用 `RegisterNatives` 绑定 native 方法，不依赖导出符号名解析（避免增量构建导致的 UnsatisfiedLinkError）。
- **SDK/NDK**：compileSdk 35 / targetSdk 35 / minSdk 26；NDK 由 Android Studio 自动装（官方 python 包即用它构建）。
- **常见问题**：
  - `CMake` 报找不到 `libpython3.14.so` / `Python.h` → 还没运行 `scripts/prepare.py`；
  - 更新了自己的项目但手机上还是旧的 → 把 `assets/project/VERSION` 数字 +1 重装；
  - **改了 native 代码后闪退/报 UnsatisfiedLinkError** → Android Studio 里 `Build > Clean Project`，删除 `app/.cxx` 和 `app/build` 后 `Rebuild`，并**卸载手机上的旧应用**再安装（不要覆盖安装）；
  - 想删掉 x86_64 只留 arm64 减小 APK → 改 `app/build.gradle` 的 `abiFilters`，并删除 `assets/python/x86_64/`。

## 已验证（交付前实测）

| 验证项 | 方法 | 结果 |
|---|---|---|
| Python 运行时组装 | `scripts/prepare.py` 幂等重跑：jniLibs（libpython3.14.so + libssl/libcrypto/libsqlite3_python）+ assets/python 双 ABI | 通过 |
| Java 源码 | JDK 17 `javac` 对真实 `android.jar`(API 35) 全量编译（MainActivity / PythonBridge / AssetExtractor + R） | 通过 |
| C/C++ 源码 | NDK r27 clang++ 语法检查（RegisterNatives 动态注册版 python_bridge.cpp） | 通过 |
| 项目入口逻辑 | 宿主 Python 模拟运行 `assets/project/main.py`（极简模板 + pip 引导） | 通过 |
| JNI 绑定 | `JNI_OnLoad` 用 RegisterNatives 注册 nativeInit/nativeRunProject/nativeGetLastError，函数指针直接绑定 | 通过（免疫符号名解析问题） |
| 解压完整性 | `.complete` 标记记录实际拷贝文件数，启动时与 assets 文件数比对，不符则强制重建 | 通过（根治残缺标准库导致的 encodings 初始化失败） |
| 解压目录树 | 修复 `copyAssetTree` 递归时未把子目录名写入目标路径的 bug（曾导致 encodings/、lib-dynload/ 等包目录被平铺，Python 报 `Failed to import encodings module`）；模拟解压验证 2462 个文件树全部落在正确位置 | 通过 |
| 错误可读 | nativeInit 失败时通过 nativeGetLastError 把 err_msg 直接显示到界面 | 通过 |
| 精简体积 | 删除 SDL3 演示 AAR（40MB）与 pygame/SDL2（54MB）后：assets 71MB + jniLibs 18MB | APK 显著变小 |
| 全量构建 | 本仓库已在 Linux 用 Gradle 8.9 + AGP 8.7.3 + NDK r27 跑通 `gradle assembleDebug`（双 ABI，37 个任务全过，产出 app-debug.apk 30MB） | 通过 |

> 说明：本环境无法运行 Android 模拟器做设备级冒烟测试；设备级验证以你的真机构建为准。

## 可选：加回 Pygame-CE / SDL2

本版默认不含 pygame（保持极简）。如果项目需要 `import pygame`：
```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r27   # 可选，缺省自动下载
./scripts/build_pygame_android.sh
```
脚本会交叉编译 SDL2 全家桶 + Pygame-CE 2.5.8（双 ABI），组装进
`assets/python/<abi>/lib/python3.14/site-packages/pygame/`，并让 `prepare.py`
生成 `sitecustomize.py`（启动时预加载 SDL2 动态库）。耗时约 20~40 分钟。

## 2026-09-26 丧尸幸存者 APK（演示成品，工程本身未绑定）
- 拉取 github.com/amo4372/zombie_survivor 最新源码放入 assets/project，打包出
  内嵌丧尸幸存者的 APK（129MB）。工程保持通用套壳：项目可随时替换。
- 新增 SDL 渲染模式（通用能力）：
  - org.libsdl.app.SDLActivity + libmain.so（SDL_main 入口），pygame.display.set_mode
    在 SDL 主线程用 GLSurfaceView 渲染，游戏画面全屏接管屏幕；
  - MainActivity 检测到 site-packages/pygame 存在时自动切换到 SDLActivity；
  - pygame-ce 2.5.8 双 ABI（arm64-v8a / x86_64）已装入 python/<abi>/lib/python3.14/site-packages；
  - SDL2 2.30.11（含 image/mixer/ttf，mixer 已补编 STB vorbis 支持 .ogg 音乐）。
- 热更新（updater.py）Android 适配：下载/解压/覆盖/回滚在 filesDir 完全可用；
  restart_game 的 os.execv 在嵌入式 Python 无可执行文件，sitecustomize 已替换为
  提示"请手动重启 App"（更新文件已应用，重启后生效）。https 已适配 Android 系统 CA。
