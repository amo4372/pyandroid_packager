"""pa 命令行入口：初始化 / 构建 / 诊断 / 信息。

后端类型（--backend）：
  auto    —— 运行时按 main.py 的 import 自动检测（默认）
  sdl2    —— SDL2 + pygame-ce（游戏/SDL 渲染）
  gles    —— OpenGL ES（PyOpenGL GLES2/EGL、ModernGL、Ursina/Panda3D）
  mixed   —— 混合：SDL 窗口 + GLES context（pygame 与 OpenGL 项目并存）
  console —— 控制台终端（无窗口，stdout 走 App 界面）

已识别的引擎（auto 检测）：
  pygame        -> sdl2
  moderngl/OpenGL -> gles
  ursina/panda3d  -> gles（需要 panda3d android runtime 放入 site-packages）
  pygame + OpenGL  -> mixed
  其他          -> console
"""
import argparse
import ast
import os
import shutil
import subprocess
import sys

from . import __version__, template

BACKENDS = ("auto", "sdl2", "gles", "mixed", "console")

# 引擎识别表：子串 -> (backend, 说明)
ENGINE_RULES = (
    # 优先 OpenGL 系
    ("import ursina", "gles", "Ursina（基于 Panda3D，需 panda3d android runtime）"),
    ("from ursina", "gles", "Ursina（基于 Panda3D，需 panda3d android runtime）"),
    ("import panda3d", "gles", "Panda3D（需 panda3d android runtime）"),
    ("from panda3d", "gles", "Panda3D（需 panda3d android runtime）"),
    ("import moderngl", "gles", "ModernGL"),
    ("from moderngl", "gles", "ModernGL"),
    ("import OpenGL", "gles", "PyOpenGL"),
    ("from OpenGL", "gles", "PyOpenGL"),
    ("import pygame", "sdl2", "pygame / SDL2"),
    ("from pygame", "sdl2", "pygame / SDL2"),
)


def log(msg: str) -> None:
    print("[pa] " + msg)


def log_err(msg: str) -> None:
    print("[pa][错误] " + msg)


def detect_backend(source_text: str) -> tuple:
    """检测项目源码文本 -> (backend, 说明列表)。mixed 检测：
    pygame 与 OpenGL 系同时出现时返回 mixed。"""
    notes = []
    has_pygame = False
    has_gl = False
    gl_note = ""
    for substr, backend, note in ENGINE_RULES:
        if substr in source_text:
            if backend == "sdl2":
                has_pygame = True
                notes.append(note)
            else:
                has_gl = True
                gl_note = note
    if has_gl and has_pygame:
        return "mixed", notes + [gl_note]
    if has_gl:
        return "gles", notes + [gl_note]
    if has_pygame:
        return "sdl2", notes
    return "console", notes


def check_project_source(src: str) -> tuple:
    """校验用户项目源码目录。返回 (ok, backend, notes)。"""
    if not os.path.isdir(src):
        return False, None, ["项目目录不存在：%s" % src]
    main_py = os.path.join(src, "main.py")
    if not os.path.isfile(main_py):
        return False, None, ["缺少 main.py（入口文件必须命名为 main.py）"]
    try:
        with open(main_py, "r", encoding="utf-8", errors="replace") as f:
            text = f.read(65536)
        compile(text, main_py, "exec")
    except SyntaxError as e:
        return False, None, ["main.py 语法错误：%s (line %s)" % (e.msg, e.lineno)]
    backend, notes = detect_backend(text)
    return True, backend, notes


# ---------------------------------------------------------------------------
# pa init
# ---------------------------------------------------------------------------
def cmd_init(args) -> int:
    target = os.path.abspath(args.target)
    if os.path.exists(target) and os.listdir(target):
        log_err("目标目录非空，请使用空目录：%s" % target)
        return 1

    # 打包器语义：必须提供项目源码（无内置示例）
    if not args.project:
        log_err("pyandroid-packager 是纯打包器，不内置示例项目。")
        log_err("请用 --project 指定你的 Python 项目源码目录（含 main.py）。")
        log_err("示例：pa init %s --project /path/to/your_project [--backend auto]" % target)
        return 1

    # 校验项目源码
    ok, backend, notes = check_project_source(args.project)
    if not ok:
        for n in notes:
            log_err(n)
        return 1
    if args.backend == "auto":
        log("检测到项目类型：%s（%s）" % (backend, "；".join(notes) if notes else "未知"))
    elif backend == "console" and args.backend in ("sdl2", "gles", "mixed"):
        log("提示：项目看起来是纯脚本，但你选择了 %s 后端，仍会创建 SDL 窗口。" % args.backend)

    log("生成工程模板 -> %s" % target)
    try:
        template.materialize(target, with_runtime=not args.no_runtime)
    except Exception as e:
        log_err("模板生成失败：%r" % e)
        return 1

    # 项目源码复制
    project_dst = os.path.join(target, "app", "src", "main", "assets", "project")
    log("复制项目源码 %s -> assets/project" % args.project)
    if os.path.isdir(project_dst):
        shutil.rmtree(project_dst)
    try:
        shutil.copytree(args.project, project_dst)
    except Exception as e:
        log_err("复制项目失败：%r" % e)
        return 1

    backend = args.backend if args.backend else "auto"
    if not write_backend(target, backend):
        log_err("写入后端配置失败")
        return 1
    log("后端类型: %s" % backend)
    log("完成。用 Android Studio 打开 %s 直接编译，或运行：pa build %s" % (target, target))
    log("提示：先运行 pa doctor %s 做环境自检" % target)
    return 0


def write_backend(target: str, backend: str) -> bool:
    """把后端写入模板 build.gradle（-PpaBackend 为运行时覆盖）。"""
    path = os.path.join(target, "app", "build.gradle")
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        if backend not in BACKENDS:
            return False
        text = text.replace("def paBackend = project.findProperty('paBackend') ?: 'auto'",
                            "def paBackend = project.findProperty('paBackend') ?: '%s'" % backend)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# pa build
# ---------------------------------------------------------------------------
def check_project_integrity(target: str) -> list:
    """构建前完整性检查，返回问题列表（空=通过）。"""
    problems = []
    base = os.path.join(target, "app", "src", "main")
    runtime = os.path.join(base, "assets", "python")
    site = os.path.join(base, "assets", "site-packages")
    jni = os.path.join(base, "jniLibs")
    project = os.path.join(base, "assets", "project")

    if not os.path.isdir(base):
        problems.append("不是有效的 pa 工程（缺少 app/src/main）")
    if not os.path.isdir(runtime):
        problems.append("缺少 Python 运行时（assets/python）。用 pa init 重新生成，或从完整模板复制。")
    else:
        for abi in ("arm64-v8a", "x86_64"):
            if not os.path.isdir(os.path.join(runtime, abi, "lib")):
                problems.append("运行时缺少 %s 库目录" % abi)
    if not os.path.isdir(site):
        problems.append("缺少共享 site-packages（assets/site-packages）")
    if not os.path.isdir(jni):
        problems.append("缺少 jniLibs（SDL2 预编译库）")
    elif not os.listdir(jni):
        problems.append("jniLibs 为空")
    if not os.path.isfile(os.path.join(project, "main.py")):
        problems.append("缺少项目入口 assets/project/main.py")
    if not os.path.isfile(os.path.join(target, "build.gradle")):
        problems.append("缺少根 build.gradle")
    return problems


def cmd_build(args) -> int:
    target = os.path.abspath(args.target or ".")
    if not os.path.isdir(target):
        log_err("工程目录不存在：%s" % target)
        return 1

    # 完整性检查
    problems = check_project_integrity(target)
    if problems:
        log_err("工程完整性检查未通过：")
        for p in problems:
            log_err("  - " + p)
        log("可用 pa init 重新生成，或用 --force 跳过检查")
        if not args.force:
            return 1

    gradle = find_gradle(target)
    if not gradle:
        log_err("未找到 gradle。请先安装 gradle 并设置 JAVA_HOME/ANDROID_HOME，")
        log_err("或直接用 Android Studio 打开 %s 编译。" % target)
        return 1
    if args.backend:
        if args.backend not in BACKENDS:
            log_err("无效后端：%s（可选：%s）" % (args.backend, ", ".join(BACKENDS)))
            return 1
        if not write_backend(target, args.backend):
            log_err("写入后端配置失败")
            return 1
    task = "assembleRelease" if args.release else "assembleDebug"
    extra = ["-PpaBackend=%s" % args.backend] if args.backend else []
    log("执行: %s %s" % (gradle, task))
    try:
        return subprocess.call([gradle, task, "--console=plain"] + extra, cwd=target)
    except OSError as e:
        log_err("执行 gradle 失败：%r" % e)
        return 1


def find_gradle(target: str):
    for cand in (
        os.path.join(target, "gradlew"),
        os.path.join(target, "gradlew.bat"),
        shutil.which("gradle"),
    ):
        if cand and os.path.exists(cand):
            return cand
    return None


# ---------------------------------------------------------------------------
# pa doctor —— 环境自检
# ---------------------------------------------------------------------------
def cmd_doctor(args) -> int:
    target = os.path.abspath(args.target or ".")
    ok = True

    def check(label, cond, hint=""):
        nonlocal ok
        status = "OK " if cond else "缺失"
        print("  [%s] %s%s" % (status, label, ("  -> " + hint) if hint and not cond else ""))
        if not cond:
            ok = False

    print("== 环境 ==")
    check("Python", sys.version_info >= (3, 8), "需要 Python 3.8+，当前 %d.%d" % sys.version_info[:2])
    check("JAVA_HOME", os.environ.get("JAVA_HOME"), "设置 JAVA_HOME（Android Studio 自带 JDK17）")
    check("ANDROID_HOME", os.environ.get("ANDROID_HOME"), "设置 ANDROID_HOME（Android SDK 目录）")
    check("gradle", shutil.which("gradle"), "安装 gradle，或用 Android Studio 编译")

    print("== 工程（%s）==" % (target if os.path.isdir(target) else "未生成"))
    if os.path.isdir(target):
        problems = check_project_integrity(target)
        if problems:
            for p in problems:
                check(p, False)
        else:
            check("工程完整性", True)
        bg = os.path.join(target, "app", "build.gradle")
        if os.path.isfile(bg):
            with open(bg, "r", encoding="utf-8") as f:
                text = f.read()
            import re
            m = re.search(r"findProperty\('paBackend'\) \?: '(\w+)'", text)
            print("  后端: %s" % (m.group(1) if m else "(未知)"))
    else:
        check("工程目录", False, "先运行 pa init %s" % target)

    print("== 模板运行时 ==")
    tpl = os.path.join(os.path.dirname(__file__), "template")
    if os.path.isdir(tpl):
        py = os.path.join(tpl, "app", "src", "main", "assets", "python")
        check("模板 Python 运行时", os.path.isdir(py))
        mg = os.path.join(tpl, "app", "src", "main", "assets", "python",
                          "arm64-v8a", "lib", "python3.14", "site-packages", "moderngl")
        check("ModernGL(arm64)", os.path.isdir(mg))
    else:
        check("模板", False, "模板目录缺失（安装损坏）")

    print()
    if ok:
        print("环境检查通过。")
        return 0
    print("存在缺失项，见上方提示。")
    return 1


# ---------------------------------------------------------------------------
# pa info
# ---------------------------------------------------------------------------
def cmd_info(args) -> int:
    print("pyandroid-packager %s" % __version__)
    tpl = os.path.join(os.path.dirname(__file__), "template")
    runtime = os.path.join(tpl, "app", "src", "main", "assets", "python")
    print("模板: %s" % tpl)
    print("内置运行时: %s (%s)" % (runtime, "有" if os.path.isdir(runtime) else "无(使用 --no-runtime 生成)"))
    print("后端类型: " + ", ".join(BACKENDS))
    print("引擎识别: pygame=SDL2 | moderngl/OpenGL/ursina/panda3d=GLES | pygame+OpenGL=mixed | 其他=console")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="pa", description="Python Android 套壳打包器")
    parser.add_argument("--version", action="version", version=__version__)
    sub = parser.add_subparsers(dest="cmd")

    p_init = sub.add_parser("init", help="生成 Android Studio 套壳工程")
    p_init.add_argument("target", help="目标工程目录（需为空）")
    p_init.add_argument("--backend", choices=BACKENDS, default="auto",
                        help="项目后端类型（默认 auto 运行时检测）")
    p_init.add_argument("--project", required=True,
                        help="你的 Python 项目源码目录（必须，含 main.py；自动检测引擎类型）")
    p_init.add_argument("--no-runtime", action="store_true",
                        help="不复制内置 Python 运行时（工程更小，需自行补充 assets/python）")
    p_init.set_defaults(func=cmd_init)

    p_build = sub.add_parser("build", help="编译 APK（需要 gradle/JAVA_HOME/ANDROID_HOME）")
    p_build.add_argument("target", nargs="?", default=".", help="工程目录（默认当前目录）")
    p_build.add_argument("--backend", choices=BACKENDS, help="覆盖后端类型")
    p_build.add_argument("--release", action="store_true", help="编译 release（精简日志界面）")
    p_build.add_argument("--force", action="store_true", help="跳过工程完整性检查")
    p_build.set_defaults(func=cmd_build)

    p_doctor = sub.add_parser("doctor", help="环境与工程自检")
    p_doctor.add_argument("target", nargs="?", default=".", help="工程目录（默认当前目录）")
    p_doctor.set_defaults(func=cmd_doctor)

    p_info = sub.add_parser("info", help="显示工具信息")
    p_info.set_defaults(func=cmd_info)

    args = parser.parse_args(argv)
    if not getattr(args, "cmd", None):
        parser.print_help()
        return 0
    try:
        return args.func(args)
    except KeyboardInterrupt:
        log("已取消")
        return 130
    except Exception as e:
        log_err("未预期错误：%r" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
