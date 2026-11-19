"""
补齐 Arch 包中被 Windows 解包丢失的符号链接（以实体文件副本代替）。

在 Windows 上 tarfile 提取符号链接需要管理员权限，之前的 unpack_pkgs.py 直接跳过了
非普通文件条目，导致 mingw64/bin/gcc.exe 之类的符号链接缺失。本脚本重新扫一遍包，
把所有符号链接按目标内容复制为实体文件。硬链接同样处理。

用法:
    python fix_symlinks.py <目标根目录> <包1.pkg.tar.zst> [包2 ...]
"""
import os
import shutil
import sys
import tarfile

import zstandard


def collect(pkg_path: str):
    """返回 (links, files)：links 是 [(name, target)]，files 是 {name: bytes} 的惰性替代。

    这里为避免把 50MB 包全读进内存，采用两遍扫描：
    第一遍只收集链接关系，第二遍再在需要时取数据。
    """
    links = []
    with open(pkg_path, "rb") as fh:
        reader = zstandard.ZstdDecompressor().stream_reader(fh)
        with tarfile.open(fileobj=reader, mode="r|") as tf:
            for m in tf:
                if m.issym() or m.islnk():
                    links.append((m.name, m.linkname))
    return links


def fix(dest_root: str, pkg_path: str) -> int:
    links = collect(pkg_path)
    fixed = 0
    for name, target in links:
        dst = os.path.join(dest_root, name)
        # linkname 是包内绝对路径形式（mingw64/bin/xx.exe），直接拼根目录
        src = os.path.join(dest_root, target)
        if not os.path.exists(src):
            print(f"  ! missing target for {name} -> {target}")
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst):
            os.remove(dst)
        shutil.copyfile(src, dst)
        fixed += 1
    return fixed


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dest_root = sys.argv[1]
    total = 0
    for pkg in sys.argv[2:]:
        n = fix(dest_root, pkg)
        if n:
            print(f"{os.path.basename(pkg):60s} {n:4d} links materialized")
        total += n
    print(f"total {total} links -> {dest_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())