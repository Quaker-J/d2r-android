#!/usr/bin/env python3
"""Windows 入口：安卓 ASTC 覆盖层生成，实际实现是 dm/tools/casc_overlay.py。

共享实现按平台选链接方式：Windows 用硬链接，跨卷或文件系统不支持时退回复制；
macOS / Linux 仍用符号链接。用法与 casc_overlay.py 相同；这个入口只是给安卓打包流程一个明确的 Windows 命令名。

Windows entry point for the Android ASTC overlay step; the implementation is dm/tools/casc_overlay.py.
The shared tool picks hardlinks on Windows and falls back to copying across volumes or unsupported filesystems;
macOS / Linux keep using symlinks. Usage is the same as casc_overlay.py; this entry only gives the Android
packaging flow an explicit Windows command name.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import casc_overlay  # noqa: E402


if __name__ == "__main__":
    casc_overlay.main(sys.argv)
