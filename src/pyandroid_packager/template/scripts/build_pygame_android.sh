#!/usr/bin/env bash
# =============================================================================
# 一键构建 SDL2 + Pygame-CE for Android，并组装进工程 assets
#
# 为什么需要手动构建：PyPI / GitHub 都没有 pygame-ce 的安卓预编译 wheel，
# 安卓上标准做法（python-for-android 配方）就是从源码交叉编译。
#
# 产物布局（ABI 专属，避免 site-packages 共享目录混入 .so）：
#   app/src/main/assets/python/<abi>/lib/python3.14/site-packages/pygame/
#       ├── pygame 包（.py + cpython-314-<abi>-linux-android.so）
#       └── libSDL2.so / libSDL2_image.so / libSDL2_ttf.so / libSDL2_mixer.so
#           + libpng16.so / libjpeg.so / libfreetype.so（运行时动态加载依赖）
#
# 用法:
#   export ANDROID_NDK_HOME=/path/to/android-ndk-r27   # 可选，缺省自动下载
#   ./scripts/build_pygame_android.sh
#
# 依赖: cmake(>=3.22), python3, curl, unzip
# 耗时: 首次约 20~40 分钟（含 NDK 下载）；产物已随工程交付，正常无需重跑。
# =============================================================================
set -e
cd "$(dirname "$0")/.."   # 工程根目录
ROOT="$(pwd)"
WORK="$ROOT/third_party/pygame-build"
mkdir -p "$WORK"

PYGAME_VERSION="2.5.8"
SDL2_VERSION="2.30.11"
SDL2_IMAGE_VERSION="2.8.4"
SDL2_TTF_VERSION="2.24.0"
SDL2_MIXER_VERSION="2.8.0"
FREETYPE_VERSION="2.13.3"
LIBPNG_VERSION="1.6.46"
LIBJPEG_VERSION="3.0.4"
ABIS="arm64-v8a x86_64"

echo "== [1/6] 准备 NDK =="
if [ -z "$ANDROID_NDK_HOME" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    NDK="$WORK/android-ndk-r27"
    if [ ! -d "$NDK" ]; then
        echo "   下载 NDK r27（约 660MB）..."
        curl -sL -o "$WORK/ndk-r27.zip" "https://dl.google.com/android/repository/android-ndk-r27-linux.zip"
        (cd "$WORK" && unzip -oq ndk-r27.zip)
    fi
else
    NDK="$ANDROID_NDK_HOME"
fi
echo "   NDK: $NDK"

echo "== [2/6] 下载源码 =="
cd "$WORK"
mkdir -p src && cd src
[ -d sdl2 ] || { curl -sL -o s.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "SDL2-$SDL2_VERSION" sdl2 && rm s.tar.gz; }
[ -d sdl2_image ] || { curl -sL -o s.tar.gz "https://github.com/libsdl-org/SDL_image/releases/download/release-$SDL2_IMAGE_VERSION/SDL2_image-$SDL2_IMAGE_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "SDL2_image-$SDL2_IMAGE_VERSION" sdl2_image && rm s.tar.gz; }
[ -d sdl2_ttf ] || { curl -sL -o s.tar.gz "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$SDL2_TTF_VERSION/SDL2_ttf-$SDL2_TTF_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "SDL2_ttf-$SDL2_TTF_VERSION" sdl2_ttf && rm s.tar.gz; }
[ -d sdl2_mixer ] || { curl -sL -o s.tar.gz "https://github.com/libsdl-org/SDL_mixer/releases/download/release-$SDL2_MIXER_VERSION/SDL2_mixer-$SDL2_MIXER_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "SDL2_mixer-$SDL2_MIXER_VERSION" sdl2_mixer && rm s.tar.gz; }
[ -d freetype ] || { curl -sL -o s.tar.gz "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "freetype-$FREETYPE_VERSION" freetype && rm s.tar.gz; }
[ -d libpng ] || { curl -sL -o s.tar.gz "https://github.com/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "libpng-$LIBPNG_VERSION" libpng && rm s.tar.gz; }
[ -d libjpeg ] || { curl -sL -o s.tar.gz "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$LIBJPEG_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "libjpeg-turbo-$LIBJPEG_VERSION" libjpeg && rm s.tar.gz; }
[ -d pygame ] || { curl -sL -o s.tar.gz "https://files.pythonhosted.org/packages/source/p/pygame-ce/pygame_ce-$PYGAME_VERSION.tar.gz" && tar -xzf s.tar.gz && mv "pygame_ce-$PYGAME_VERSION" pygame && rm s.tar.gz; }
echo "   源码就绪"

echo "== [3/6] 宿主 Python 3.14 + Cython =="
PY="$WORK/venv/bin/python"
if [ ! -x "$PY" ]; then
    curl -sL -o uv.tar.gz "https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-unknown-linux-gnu.tar.gz"
    tar -xzf uv.tar.gz
    ./uv-x86_64-unknown-linux-gnu/uv venv --python 3.14 "$WORK/venv"
    ./uv-x86_64-unknown-linux-gnu/uv pip install --python "$WORK/venv/bin/python" cython setuptools wheel pip
fi

echo "== [4/6] 交叉编译原生库（双 ABI 串行，避免 build 目录冲突） =="
HOST_TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
for ABI in $ABIS; do
    PREFIX="$WORK/prefix/$ABI"
    C="$HOST_TC/bin"
    CFLAGS="-O2 -fPIC -DANDROID --sysroot=$HOST_TC/sysroot"
    export CC="$C/clang" CXX="$C/clang++" AR="$C/llvm-ar" RANLIB="$C/llvm-ranlib"
    echo "--- [$ABI] freetype / libpng / libjpeg / SDL2 ---"
    for name in freetype libpng libjpeg sdl2; do
        cmake -S "$WORK/src/$name" -B "$WORK/build/$ABI/$name" \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" \
            -DBUILD_SHARED_LIBS=ON -DCMAKE_C_FLAGS="$CFLAGS" \
            -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON \
            -DPNG_SHARED=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
            -DENABLE_SHARED=ON -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=OFF \
            -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
        cmake --build "$WORK/build/$ABI/$name" -j$(nproc) >/dev/null
        cmake --install "$WORK/build/$ABI/$name" >/dev/null
    done
    echo "--- [$ABI] SDL2_image / ttf / mixer ---"
    SDL_LIBS="-DSDL2_LIBRARY=$PREFIX/lib/libSDL2.so -DSDL2_INCLUDE_DIR=$PREFIX/include/SDL2 -DSDL2_INCLUDE_DIRS=$PREFIX/include/SDL2"
    for name in sdl2_image sdl2_ttf sdl2_mixer; do
        cmake -S "$WORK/src/$name" -B "$WORK/build/$ABI/$name" \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
            -DCMAKE_INSTALL_PREFIX="$PREFIX" \
            -DCMAKE_C_FLAGS="$CFLAGS" \
            $SDL_LIBS \
            -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_JPG=ON -DSDL2IMAGE_PNG=ON \
            -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_HARFBUZZ=OFF \
            -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_MP3=OFF -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_OGG=OFF \
            -DSDL2MIXER_VORBIS=OFF -DSDL2MIXER_FLAC=OFF -DSDL2MIXER_MOD=OFF -DSDL2MIXER_MIDI=OFF >/dev/null
        cmake --build "$WORK/build/$ABI/$name" -j$(nproc) >/dev/null
        cmake --install "$WORK/build/$ABI/$name" >/dev/null
    done
done
echo "   原生库完成"

echo "== [5/6] 交叉编译 Pygame-CE（双 ABI 串行） =="
for ABI in $ABIS; do
    if [ "$ABI" = "arm64-v8a" ]; then TRIPLE=aarch64-linux-android; else TRIPLE=x86_64-linux-android; fi
    PREFIX="$WORK/prefix/$ABI"
    TCC="$HOST_TC/bin/${TRIPLE}26-clang"
    export CC="$TCC" CXX="$HOST_TC/bin/${TRIPLE}26-clang++" \
        AR="$HOST_TC/bin/llvm-ar" RANLIB="$HOST_TC/bin/llvm-ranlib" STRIP="$HOST_TC/bin/llvm-strip" \
        LDSHARED="$TCC -shared -fPIC" MAINCC="$TCC" \
        CFLAGS="-O2 -fPIC -DANDROID --sysroot=$HOST_TC/sysroot -I$PREFIX/include -I$PREFIX/include/SDL2" \
        LDFLAGS="--sysroot=$HOST_TC/sysroot -L$PREFIX/lib -L$HOST_TC/sysroot/usr/lib/$TRIPLE" \
        PYGAME_ANDROID=1
    python3 - "$WORK/src/pygame/buildconfig/Setup.Android.SDL2.in" "$PREFIX" > "$WORK/src/pygame/Setup" <<'PYEOF'
import sys
tpl = open(sys.argv[1]).read()
p = sys.argv[2]
sdl = f" -I{p}/include/SDL2 -L{p}/lib"
print(tpl.format(sdl_includes=sdl, sdl_ttf_includes=sdl, sdl_image_includes=sdl,
                 sdl_mixer_includes=sdl, jpeg_includes=f" -I{p}/include",
                 png_includes=f" -I{p}/include", freetype_includes=f" -I{p}/include"))
PYEOF
    (cd "$WORK/src/pygame" && rm -rf build && "$WORK/venv/bin/python" setup.py build_ext >/dev/null)
    BUILD_LIB=$(ls -d "$WORK/src/pygame/build"/lib.*-cpython-314* | head -1)
    find "$BUILD_LIB/pygame" -name "*.so" | while read f; do
        n=$(basename "$f" | sed "s/\.cpython-314-[a-z0-9_]*-linux-gnu\.so$/.cpython-314-${TRIPLE}.so/")
        [ "$n" != "$(basename "$f")" ] && mv "$f" "$(dirname "$f")/$n"
    done
    find "$BUILD_LIB/pygame" -name "*cpython-314-cpython-314*" | while read f; do
        mv "$f" "$(dirname "$f")/$(basename "$f" | sed 's/cpython-314-cpython-314/cpython-314/')"
    done
    for lib in libSDL2 libSDL2_image libSDL2_ttf libSDL2_mixer libpng libfreetype libjpeg; do
        for f in "$PREFIX/lib/$lib"*.so*; do [ -f "$f" ] && cp -L "$f" "$BUILD_LIB/pygame/" 2>/dev/null || true; done
    done
    cp -r "$WORK/src/pygame/src_py/." "$BUILD_LIB/pygame/"
    find "$BUILD_LIB/pygame" -name "*.so" -exec "$HOST_TC/bin/llvm-strip" {} \;
    DEST="$ROOT/app/src/main/assets/python/$ABI/lib/python3.14/site-packages/pygame"
    rm -rf "$DEST" && mkdir -p "$(dirname "$DEST")" && cp -r "$BUILD_LIB/pygame" "$DEST"
    echo "   [$ABI] -> $DEST ($(du -sh "$DEST" | cut -f1))"
done

echo "== [6/6] 完成。SDK 侧无需任何改动，重新构建 APK 即可 =="
