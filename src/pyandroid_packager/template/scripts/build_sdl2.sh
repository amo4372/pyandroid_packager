#!/usr/bin/env bash
# =============================================================================
# build_sdl2_android.sh —— 可选：为坚持用 SDL2 的用户从源码编译 libSDL2.so
#
# 背景：SDL2 官方【不】发布安卓预编译二进制（只有源码），SDL3 才提供官方
# 预编译 AAR（本项目默认采用 SDL3，开箱即用）。本脚本让需要 SDL2 的开发者
# 用 NDK 自行编译出 arm64-v8a / x86_64 的 libSDL2.so。
#
# 用法：
#   export ANDROID_HOME=/path/to/android-sdk      # 必须
#   ./scripts/build_sdl2_android.sh [SDL2版本]      # 默认 2.30.12
#
# 产物：third_party/sdl2-build/<abi>/libSDL2.so
#
# 切换到 SDL2 的工程改动（README 有说明）：
#   1. 把 libSDL2.so 拷到 app/src/main/jniLibs/<abi>/
#   2. 用 SDL2 的 java glue（SDL2 源码 android-project/app/src/main/java/
#      org/libsdl/app/）替换 org.libsdl.app 相关依赖，并把 sdl_demo.cpp 改成
#      SDL2 API（SDL_Init/SDL_CreateWindow/SDL_RenderGeometry 等前缀相同，
#      主要是头文件路径 SDL2/SDL.h 与部分函数名）
# =============================================================================
set -euo pipefail

ANDROID_HOME="${ANDROID_HOME:?请设置 ANDROID_HOME}"
VERSION="${1:-2.30.12}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/sdl2-build"
WORK="$ROOT/third_party/sdl2-src"

ABIS=("arm64-v8a:x86_64" )
PLATFORM=24

if ! command -v cmake >/dev/null; then
    echo "需要 cmake（Android Studio 自带，或 apt/brew 安装）" >&2
    exit 1
fi

# 找到 NDK（优先 android-env 同款版本，否则取已安装的最新）
NDK_DIR="$ANDROID_HOME/ndk"
if [ ! -d "$NDK_DIR" ]; then
    echo "未找到 $NDK_DIR，请先在 SDK Manager 安装 NDK（推荐 r27.3+）" >&2
    exit 1
fi
NDK="$(ls -1d "$NDK_DIR"/* 2>/dev/null | sort -V | tail -1)"
echo "使用 NDK: $NDK"

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || { echo "缺少 $TOOLCHAIN" >&2; exit 1; }

# 下载 SDL2 源码
mkdir -p "$WORK" "$OUT"
TARBALL="$ROOT/third_party/downloads/SDL2-$VERSION.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "下载 SDL2-$VERSION 源码..."
    curl -L -o "$TARBALL" "https://www.libsdl.org/release/SDL2-$VERSION.tar.gz"
fi
SRC="$WORK/SDL2-$VERSION"
[ -d "$SRC" ] || tar xzf "$TARBALL" -C "$WORK"

for SPEC in "${ABIS[@]}"; do
    ABI="${SPEC%%:*}"
    BUILD="$OUT/$ABI"
    echo "==> 编译 $ABI"
    cmake -S "$SRC" -B "$BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$PLATFORM" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --config Release -j"$(nproc)"
done

echo
echo "完成。产物："
find "$OUT" -name "libSDL2.so"
echo "提示：把对应 ABI 的 libSDL2.so 拷到 app/src/main/jniLibs/<abi>/ 即可打包进 APK；"
echo "同时需按 README 切换 SDL2 的 java glue 与 sdl_demo 源码。"
