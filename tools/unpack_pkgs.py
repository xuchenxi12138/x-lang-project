"""
从 Arch 风格的 .pkg.tar.zst 包中解出文件到指定根目录。

用法:
    python unpack_pkgs.py <目标根目录> <包1.pkg.tar.zst> [包2 ...]

Arch 包的内部路径形如 `mingw64/bin/gcc.exe`，直接按原路径展开到目标根目录，
最终得到 <目标根目录>/mingw64/bin/gcc.exe。
"""
import io
import os
import sys
import tarfile

import zstandard


def extract(pkg_path: str, dest_root: str) -> int:
    count = 0
    with open(pkg_path, "rb") as fh:
        dctx = zstandard.ZstdDecompressor()
        with dctx.stream_reader(fh) as reader:
            # tarfile 用 stream 模式读，因为 stream_reader 不支持 seek
            with tarfile.open(fileobj=reader, mode="r|") as tf:
                for member in tf:
                    name = member.name
                    # 只取以 mingw64/ 开头的条目，其它元数据跳过
                    if not (name.startswith("mingw64/") or name == "mingw64"):
                        continue
                    target = os.path.join(dest_root, name)
                    if member.isdir():
                        os.makedirs(target, exist_ok=True)
                        continue
                    if not member.isfile():
                        continue
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    src = tf.extractfile(member)
                    if src is None:
                        continue
                    with open(target, "wb") as out:
                        while True:
                            chunk = src.read(1 << 20)
                            if not chunk:
                                break
                            out.write(chunk)
                    # 保留可执行位（Windows 上无意义，交叉到 Linux 时有用）
                    try:
                        os.chmod(target, member.mode | 0o200)
                    except OSError:
                        pass
                    count += 1
    return count


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dest_root = sys.argv[1]
    os.makedirs(dest_root, exist_ok=True)
    total = 0
    for pkg in sys.argv[2:]:
        n = extract(pkg, dest_root)
        print(f"{os.path.basename(pkg):60s} {n:6d} files")
        total += n
    print(f"total {total} files -> {dest_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())