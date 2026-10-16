#!/usr/bin/env python3
"""把派生好的游戏数据打成一个可导入的数据包（格式见 docs/ANDROID_PACKAGE.zh-CN.md）。
Build a single importable package from already derived game data (format: docs/ANDROID_PACKAGE.zh-CN.md).

同一个模块既当命令行工具用，也被 pack_server.py 的网页后端调用；两条路产生完全一样的包。

用法:
  build_package.py assemble <输出.zip> --game <目录> --romfs <目录> --shaders <目录> [--subgroup 64]
  build_package.py inspect  <某个.zip>        只读地打印包内容与清单，不改动任何东西
"""
import argparse
import datetime
import json
import os
import sys
import zipfile
from pathlib import Path

FORMAT_VERSION = 1
MANIFEST_NAME = "manifest.json"
# 包内所有条目用同一个固定时间戳：包里带的是内容而不是"什么时候打包的"，同一份输入应产出同样的 zip。
# Every entry gets the same fixed timestamp: the zip carries content, not packaging time, so identical inputs
# produce an identical archive. The actual creation time lives in the manifest.
FIXED_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
# 包里必须齐全的东西：缺任何一项都不能算一个可用的包。
GAME_MODULES = ("libdmguest_main.so", "libdmguest_sdk.so", "libdmguest_subsdk0.so")
GAME_LAYOUTS = tuple(f"{name}.{kind}.tsv" for name in ("main", "sdk", "subsdk0") for kind in ("fixups", "gates"))
ROMFS_EXTENSIONS = (".data", ".psv")


class PackageError(Exception):
    pass


def require_directory(path, label):
    if not path.is_dir():
        raise PackageError(f"{label} directory does not exist: {path}")
    return path


def collect_game(directory):
    """模块与布局文件必须齐全，缺一个就报错——半个模块的包在手机上只会崩。"""
    require_directory(directory, "game")
    for name in GAME_MODULES + GAME_LAYOUTS:
        if not (directory / name).is_file():
            raise PackageError(f"game directory is missing {name}")
    return sorted(GAME_MODULES + GAME_LAYOUTS)


def collect_romfs(directory):
    """RomFS 只需要 data/ 下的资源档案；其它东西游戏不读，放进包里只是白白变大。"""
    require_directory(directory, "romfs")
    data = directory / "data"
    if not data.is_dir():
        raise PackageError(f"romfs directory has no data/ subdirectory: {directory}")
    entries = sorted(p for p in data.iterdir() if p.is_file() and p.suffix in ROMFS_EXTENSIONS)
    if not entries:
        raise PackageError(f"romfs data/ has no {'/'.join(ROMFS_EXTENSIONS)} files: {data}")
    return entries


def collect_shaders(directory):
    """着色器目录必须带分组档位文件：档位与 GPU 策略不一致时运行时会直接终止，不能靠猜。"""
    require_directory(directory, "shaders")
    profile = directory / "host-subgroup-size.txt"
    if not profile.is_file():
        raise PackageError(f"shaders directory has no host-subgroup-size.txt: {directory}")
    try:
        subgroup = int(profile.read_text().strip())
    except ValueError:
        raise PackageError("host-subgroup-size.txt is not a plain integer")
    entries = sorted(p for p in directory.iterdir() if p.is_file() and p.name != profile.name)
    if not entries:
        raise PackageError(f"shaders directory has no modules: {directory}")
    return subgroup, entries


def write_entry(archive, source, name, progress):
    """逐个写入并按 zip 的 STORED 方式存：这些文件本身就是压缩格式，再压一遍只浪费 CPU。
    大文件分块写；progress 收到的是本次新增的字节数，不是累计值。"""
    info = zipfile.ZipInfo(name, date_time=FIXED_TIMESTAMP)
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    written = 0
    with source.open("rb") as src, archive.open(info, "w") as destination:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            destination.write(chunk)
            written += len(chunk)
            progress(len(chunk))
    return written


def assemble(output, game, romfs, shaders, note=None, quiet=False):
    game_files = collect_game(game)
    romfs_files = collect_romfs(romfs)
    subgroup, shader_files = collect_shaders(shaders)

    total_romfs = sum(p.stat().st_size for p in romfs_files)
    total_shaders = sum(p.stat().st_size for p in shader_files)
    total_game = sum((game / n).stat().st_size for n in game_files)

    if not quiet:
        print(f"模块 {len(game_files)} 个 {total_game/1048576:.1f} MiB", flush=True)
        print(f"RomFS {len(romfs_files)} 个档案 {total_romfs/1073741824:.2f} GiB", flush=True)
        print(f"着色器 {len(shader_files)} 个 {total_shaders/1048576:.1f} MiB（分组档位 {subgroup}）", flush=True)

    manifest = {
        "format": FORMAT_VERSION,
        "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": note or "user-supplied game files",
        "subgroup_size": subgroup,
        "counts": {"game": len(game_files), "romfs": len(romfs_files), "shaders": len(shader_files)},
        "bytes": {"game": total_game, "romfs": total_romfs, "shaders": total_shaders},
    }

    written_bytes = 0
    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode()
    expected_bytes = len(manifest_bytes) + total_game + total_romfs + total_shaders
    last_report = [0]

    def progress(count):
        nonlocal written_bytes
        written_bytes += count
        if quiet:
            return
        percent = written_bytes * 100 // max(1, expected_bytes)
        if percent >= last_report[0] + 5:
            last_report[0] = percent - percent % 5
            print(f"  打包 {percent}% ({written_bytes/1073741824:.2f}/{expected_bytes/1073741824:.2f} GiB)", flush=True)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".part")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_STORED, allowZip64=True) as archive:
        manifest_info = zipfile.ZipInfo(MANIFEST_NAME, date_time=FIXED_TIMESTAMP)
        manifest_info.compress_type = zipfile.ZIP_STORED
        manifest_info.external_attr = 0o644 << 16
        archive.writestr(manifest_info, manifest_bytes)
        written_bytes += len(manifest_bytes)
        for name in game_files:
            write_entry(archive, game / name, f"game/{name}", progress)
        for path in romfs_files:
            write_entry(archive, path, f"romfs/data/{path.name}", progress)
        for path in shader_files:
            write_entry(archive, path, f"shaders/{path.name}", progress)
    # 全部写完才改名：中断留下的 .part 不会被误当成可用包。
    temporary.replace(output)
    if not quiet:
        print(f"完成：{output} ({output.stat().st_size/1073741824:.2f} GiB)", flush=True)
    return manifest


def inspect(archive_path):
    """只读检查：手机端导入前，先在这里确认结构对不对。"""
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        if MANIFEST_NAME not in names:
            raise PackageError(f"{archive_path} has no {MANIFEST_NAME}")
        manifest = json.loads(archive.read(MANIFEST_NAME))
        if manifest.get("format") != FORMAT_VERSION:
            raise PackageError(f"unsupported package format {manifest.get('format')}; this build understands {FORMAT_VERSION}")
        groups = {}
        for name in names:
            top = name.split("/", 1)[0] if "/" in name else name
            groups[top] = groups.get(top, 0) + 1
        return manifest, groups


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    assemble_parser = subparsers.add_parser("assemble", help="assemble a package from already derived data")
    assemble_parser.add_argument("output", type=Path)
    assemble_parser.add_argument("--game", type=Path, required=True)
    assemble_parser.add_argument("--romfs", type=Path, required=True)
    assemble_parser.add_argument("--shaders", type=Path, required=True)
    assemble_parser.add_argument("--note", default=None)

    inspect_parser = subparsers.add_parser("inspect", help="print a package's manifest without touching it")
    inspect_parser.add_argument("archive", type=Path)

    args = parser.parse_args()
    try:
        if args.command == "assemble":
            assemble(args.output, args.game, args.romfs, args.shaders, note=args.note)
        else:
            manifest, groups = inspect(args.archive)
            print(json.dumps(manifest, indent=2, ensure_ascii=False))
            print("顶层条目:", groups)
    except PackageError as error:
        sys.exit(f"package error: {error}")


if __name__ == "__main__":
    main()
