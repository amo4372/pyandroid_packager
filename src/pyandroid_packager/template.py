"""模板物化：把内置工程模板复制到目标目录。"""
import os
import shutil


def _template_root() -> str:
    return os.path.join(os.path.dirname(__file__), "template")


def materialize(target: str, with_runtime: bool = True) -> None:
    """复制模板到 target。with_runtime=False 时跳过超大运行时目录。"""
    root = _template_root()
    if with_runtime:
        shutil.copytree(root, target)
        return

    # 瘦身模式：跳过 assets/python 运行时（用户自行补充）
    skip = {
        os.path.join("app", "src", "main", "assets", "python"),
        os.path.join("app", "src", "main", "assets", "site-packages"),
        os.path.join("app", "src", "main", "assets", "wheels"),
        os.path.join("app", "src", "main", "jniLibs"),
    }
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        # 剪枝：跳过整个被排除目录
        dirnames[:] = [d for d in dirnames
                       if os.path.join(rel, d) not in skip and not _under(rel, skip)]
        dst_dir = os.path.join(target, rel)
        os.makedirs(dst_dir, exist_ok=True)
        for fn in filenames:
            shutil.copy2(os.path.join(dirpath, fn), os.path.join(dst_dir, fn))


def _under(rel: str, skip) -> bool:
    for s in skip:
        if rel.startswith(s + os.sep):
            return True
    return False
