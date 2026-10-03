@echo off
rem ============================================================
rem  PythonAndroid 一键清理旧版 SDL3 演示残留
rem  适用：从旧版(v1~v4)升级到精简版(v5+)后构建报错：
rem      "程序包org.libsdl.app不存在 / SDLDemoActivity"
rem  用法：解压后，双击本文件，或在此目录命令行运行 cleanup.bat
rem ============================================================
chcp 65001 >nul
echo.
echo 正在清理旧版 SDL3 演示残留文件...
echo.

set DELETED=0

rem 1) 旧版 SDL 演示 Activity（Java）
if exist "app\src\main\java\com\example\pythonandroid\SDLDemoActivity.java" (
    del /q "app\src\main\java\com\example\pythonandroid\SDLDemoActivity.java"
    echo   [已删除] app\src\main\java\com\example\pythonandroid\SDLDemoActivity.java
    set DELETED=1
)

rem 2) 旧版 SDL 演示 C++ 源码
if exist "app\src\main\cpp\sdl_demo.cpp" (
    del /q "app\src\main\cpp\sdl_demo.cpp"
    echo   [已删除] app\src\main\cpp\sdl_demo.cpp
    set DELETED=1
)

rem 3) 旧版 SDL3 AAR 库（40MB，精简版不再需要）
if exist "app\libs\*.aar" (
    del /q "app\libs\*.aar"
    echo   [已删除] app\libs\*.aar
    set DELETED=1
)

rem 4) 布局/字符串中残留的 SDL 按钮引用（若旧文件覆盖过）
findstr /c:"btn_sdl" "app\src\main\res\layout\activity_main.xml" >nul 2>&1
if not errorlevel 1 (
    echo   [警告] activity_main.xml 仍含 btn_sdl，请确认已用精简版覆盖本工程
    set DELETED=1
)

rem 5) 确认精简版入口文件都在
if not exist "app\src\main\cpp\python_bridge.cpp" (
    echo   [警告] 未找到 python_bridge.cpp，请确认解压的是精简版 zip
)

echo.
if %DELETED%==0 (
    echo 未发现 SDL 残留（工程已是精简版），无需清理。
) else (
    echo 清理完成。
)
echo.
echo 下一步：Android Studio 中 Build - Clean Project，
echo         删除 app\.cxx 与 app\build 目录，然后 Rebuild。
echo         手机上卸载旧应用后重新安装。
echo.
pause
