#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
prepare.py —— 一键下载并组装 PythonAndroid 工程所需的全部二进制资源。

负责把以下内容装进工程（幂等，可重复运行；下载有缓存，存放在 third_party/）：
  1. Python 3.14 官方 Android 预编译包（python.org 发布，aarch64 + x86_64）
       -> app/src/main/jniLibs/<abi>/  (libpython3.14.so + lib*_python.so)
       -> app/src/main/assets/python/   (标准库：纯 Python 部分共享，
                                          lib-dynload 按 ABI 分层)
       -> app/src/main/cpp/python_include/  (编译 python_bridge.cpp 所需的头文件)
  2. SDL3 系列官方预编译 AAR（libsdl-org 发布）
       -> app/libs/*.aar
  3. pip / setuptools 纯 Python wheel（PyPI 发布）
       -> app/src/main/assets/wheels/
  4. Gradle wrapper（gradlew / gradlew.bat / gradle-wrapper.jar）

用法：
    python3 scripts/prepare.py            # 全部执行
    python3 scripts/prepare.py --abi-only # 只重新组装已下载的资源

依赖：python3.8+（标准库即可）。
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THIRD_PARTY = os.path.join(ROOT, "third_party")
DOWNLOADS = os.path.join(THIRD_PARTY, "downloads")
APP = os.path.join(ROOT, "app", "src", "main")
JNI_LIBS = os.path.join(APP, "jniLibs")
ASSETS = os.path.join(APP, "assets")
CPP = os.path.join(ROOT, "app", "src", "main", "cpp")
PY_INCLUDE = os.path.join(CPP, "python_include")
APP_LIBS = os.path.join(ROOT, "app", "libs")

PY_VERSION = "3.14"
PY_TAG = "3.14.7"
PY_BASE = f"https://www.python.org/ftp/python/{PY_TAG}/"
PY_ABIS = {
    "arm64-v8a": f"python-{PY_TAG}-aarch64-linux-android.tar.gz",
    "x86_64": f"python-{PY_TAG}-x86_64-linux-android.tar.gz",
}

# SDL3 系列官方发布（tag + 资产名）
SDL_PACKAGES = [
    ("SDL3", "release-3.4.16", "SDL3-devel-3.4.16-android.zip"),
    ("SDL3_image", "release-3.4.6", "SDL3_image-devel-3.4.6-android.zip"),
    ("SDL3_mixer", "release-3.2.4", "SDL3_mixer-devel-3.2.4-android.zip"),
    ("SDL3_ttf", "release-3.2.2", "SDL3_ttf-devel-3.2.2-android.zip"),
    ("SDL3_net", "release-3.2.0", "SDL3_net-devel-3.2.0-android.zip"),
]
SDL_URL = ("https://github.com/libsdl-org/{repo}/releases/download/{tag}/{asset}")

GRADLE_VER = "8.9.0"
GRADLE_BASE = f"https://raw.githubusercontent.com/gradle/gradle/v{GRADLE_VER}/"


def log(msg):
    print(f"[prepare] {msg}")


def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        log(f"已缓存 {os.path.basename(dest)}")
        return
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    log(f"下载 {url}")
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1024 * 256)
    os.replace(tmp, dest)


def pypi_wheel(pkg):
    with urllib.request.urlopen(f"https://pypi.org/pypi/{pkg}/json", timeout=60) as r:
        data = json.load(r)
    for u in data["urls"]:
        if u["packagetype"] == "bdist_wheel" and "py3-none-any" in u["filename"]:
            return u["filename"], u["url"]
    raise RuntimeError(f"{pkg}: 找不到 py3-none-any wheel")


def extract_tar(tarball, dest):
    if os.path.isdir(dest):
        return
    os.makedirs(dest, exist_ok=True)
    log(f"解压 {os.path.basename(tarball)}")
    with tarfile.open(tarball) as t:
        t.extractall(dest)


def need_rename(name):
    """官方 testbed 的 .gz 规避：文件名以 .gz 或 - 结尾时追加一个 '-'。"""
    return name.endswith(".gz") or name.endswith("-")


def copy_tree(src, dst, rename_gz=False):
    """拷贝目录；rename_gz=True 时对满足条件的文件追加 '-'。"""
    for root, dirs, files in os.walk(src):
        # 排除 __pycache__
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        rel = os.path.relpath(root, src)
        target = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(target, exist_ok=True)
        for f in files:
            s = os.path.join(root, f)
            n = f
            if rename_gz and need_rename(n):
                n = n + "-"
            t = os.path.join(target, n)
            shutil.copy2(s, t)


def assemble_python():
    """组装 Python：jniLibs + assets/python + python_include。"""
    os.makedirs(JNI_LIBS, exist_ok=True)
    os.makedirs(ASSETS, exist_ok=True)

    staged = {}
    for abi, tname in PY_ABIS.items():
        tarball = os.path.join(DOWNLOADS, tname)
        download(f"{PY_BASE}{tname}", tarball)
        stage = os.path.join(THIRD_PARTY, "staged", abi)
        extract_tar(tarball, stage)
        staged[abi] = os.path.join(stage, "prefix")

    # ---- 1) jniLibs：libpython + 外部库(lib*_python.so) ----
    for abi, prefix in staged.items():
        lib = os.path.join(prefix, "lib")
        out = os.path.join(JNI_LIBS, abi)
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(lib):
            if (name.startswith(f"libpython{PY_VERSION}") and name.endswith(".so")) or \
               (name.endswith("_python.so")):
                shutil.copy2(os.path.join(lib, name), os.path.join(out, name))
                log(f"jniLibs/{abi}/{name}")
        # libpython3.so 是符号链接，跳过（链接名不影响加载）

    # ---- 2) assets/python：纯 Python 部分共享，lib-dynload 按 ABI ----
    py_lib_name = f"python{PY_VERSION}"
    common_src = os.path.join(staged["arm64-v8a"], "lib", py_lib_name)
    common_dst = os.path.join(ASSETS, "python", "common", "lib", py_lib_name)
    # 先拷全部，再从每个 ABI 里把 lib-dynload 单独拷出（排除 __pycache__）
    log("组装 assets/python/common（纯 Python 标准库，两 ABI 共享）")
    copy_tree(common_src, common_dst, rename_gz=True)
    dynload_rel = os.path.join("lib", py_lib_name, "lib-dynload")
    for abi, prefix in staged.items():
        dyn_src = os.path.join(prefix, dynload_rel)
        dyn_dst = os.path.join(ASSETS, "python", abi, dynload_rel)
        log(f"组装 assets/python/{abi}/lib-dynload（C 扩展，按 ABI 分开）")
        copy_tree(dyn_src, dyn_dst, rename_gz=True)
        # 删掉 common 里的 lib-dynload（避免重复）
        shutil.rmtree(os.path.join(common_dst, "lib-dynload"), ignore_errors=True)

    # ---- 3) python_include：编译 python_bridge.cpp 需要的头文件 ----
    inc_src = os.path.join(staged["arm64-v8a"], "include", py_lib_name)
    shutil.rmtree(PY_INCLUDE, ignore_errors=True)
    shutil.copytree(inc_src, PY_INCLUDE)
    log(f"python_include/ ({len(os.listdir(PY_INCLUDE))} 个头文件)")

    # ---- 4) site-packages（共享，用户 Python 库放这里） ----
    os.makedirs(os.path.join(ASSETS, "site-packages"), exist_ok=True)
    readme = os.path.join(ASSETS, "site-packages", "README.txt")
    if not os.path.exists(readme):
        with open(readme, "w", encoding="utf-8") as f:
            f.write("此目录内容会解压到 filesDir/python/lib/python3.14/site-packages，"
                    "所有 ABI 共享。可放入纯 Python 库或安卓 wheel 的包。\n")


def assemble_sdl():
    """解压 SDL3 系列官方 devel-android.zip，把 AAR 放到 app/libs。"""
    os.makedirs(APP_LIBS, exist_ok=True)
    for repo, tag, asset in SDL_PACKAGES:
        zpath = os.path.join(DOWNLOADS, asset)
        download(SDL_URL.format(repo=repo, tag=tag, asset=asset), zpath)
        with zipfile.ZipFile(zpath) as z:
            aar_names = [n for n in z.namelist() if n.endswith(".aar")]
            if not aar_names:
                raise RuntimeError(f"{asset} 里没有 AAR")
            aar = aar_names[0]
            dst = os.path.join(APP_LIBS, os.path.basename(aar))
            if not os.path.exists(dst):
                with z.open(aar) as src, open(dst, "wb") as out:
                    shutil.copyfileobj(src, out)
            log(f"app/libs/{os.path.basename(aar)}")


def assemble_wheels():
    """离线 wheel 目录：仅 pip/setuptools（首次启动由 main.py 引导）。"""
    os.makedirs(os.path.join(ASSETS, "wheels"), exist_ok=True)
    for pkg in ("pip", "setuptools"):
        try:
            name, url = pypi_wheel(pkg)
        except RuntimeError as e:
            print("  [WARN]", e)
            continue
        dest = os.path.join(ASSETS, "wheels", name)
        download(url, dest)
        log(f"assets/wheels/{name}")


def assemble_wrapper():
    wrapper_dir = os.path.join(ROOT, "gradle", "wrapper")
    os.makedirs(wrapper_dir, exist_ok=True)
    for fname, base in (("gradlew", GRADLE_BASE), ("gradlew.bat", GRADLE_BASE),
                        ("gradle-wrapper.jar", GRADLE_BASE + "gradle/wrapper/")):
        dest = os.path.join(ROOT if fname != "gradle-wrapper.jar" else wrapper_dir, fname)
        download(base + fname, dest)
    os.chmod(os.path.join(ROOT, "gradlew"), 0o755)




def summary():
    total = 0
    print("\n===== 组装完成，资源汇总 =====")
    for root, dirs, files in os.walk(APP):
        for f in files:
            p = os.path.join(root, f)
            total += os.path.getsize(p)
    for f in sorted(os.listdir(APP_LIBS)):
        p = os.path.join(APP_LIBS, f)
        print(f"  app/libs/{f}  {os.path.getsize(p)/1024/1024:.1f} MB")
    print(f"  assets + jniLibs 合计: {total/1024/1024:.1f} MB（APK 中为压缩后体积，通常约 1/3）")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--abi-only", action="store_true", help="只重新组装已下载的资源")
    args = ap.parse_args()

    if args.abi_only:
        pass
    else:
        assemble_wrapper()
        assemble_wheels()
    assemble_python()
    summary()


if __name__ == "__main__":
    main()
