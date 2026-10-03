把你的 Python 项目放进这个目录（至少包含 main.py）。
main.py 是入口文件，应用启动后会运行它。

示例结构：
  project/
  ├── main.py
  ├── 其他模块.py / 资源目录...

后端类型（gradle -PpaBackend= 或 pa init --backend）：
  sdl2   —— pygame / SDL 渲染（import pygame）
  gles   —— OpenGL ES（import moderngl / OpenGL / ursina / panda3d）
  mixed  —— pygame + OpenGL 混合
  console—— 纯终端脚本
  auto   —— 运行时按 main.py 的 import 自动检测（默认）
