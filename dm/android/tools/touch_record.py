#!/usr/bin/env python3
"""Record and replay in-game input for the Android port without root.

The app records the touch state machine's per-frame output (stick + gamepad buttons) to
files/dm-touch-record.txt; replay feeds the same samples back to the game. Raw /dev/input
injection is unavailable on this device (SELinux denies sendevent to the shell user).

Commands:
  record [--duration N] [--name NAME]   start recording, stop on Enter/Ctrl+C or after N seconds
  replay [--name NAME] [--timeout N]    replay the newest (or a named) recording
  stop                                  remove the control flags (a recording is written out)
  status                                show app state and device-side recording files
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

PKG = "org.d2r.android.port"
ROOT = Path(__file__).resolve().parents[3]
LOGS = ROOT / "dm-work" / "build" / "logs"
SERIAL: str | None = None


def adb(*args: str, capture: bool = False, stdin=None, stdout=None):
    command = ["adb"] + (["-s", SERIAL] if SERIAL else []) + list(args)
    return subprocess.run(
        command,
        capture_output=capture,
        text=capture,
        encoding="utf-8" if capture else None,
        errors="replace" if capture else None,
        stdin=stdin,
        stdout=stdout,
        check=False,
    )


def runas(*args: str, capture: bool = False):
    return adb("shell", "run-as", PKG, *args, capture=capture)


def device_exists(path: str) -> bool:
    return runas("ls", path, capture=True).returncode == 0


def app_running() -> bool:
    result = adb("shell", "pidof", PKG, capture=True)
    return result.returncode == 0 and bool(result.stdout.strip())


def remove_flags() -> None:
    runas("rm", "-f", "files/dm-touch-record", "files/dm-touch-replay", "files/dm-touch-replay.done")


def wait_file(path: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if device_exists(path):
            return True
        time.sleep(0.25)
    return False


def pull_recording(destination: Path) -> bool:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as out:
        result = adb("exec-out", "run-as", PKG, "cat", "files/dm-touch-record.txt", stdout=out)
    return result.returncode == 0 and destination.stat().st_size > 0


def push_recording(source: Path) -> bool:
    # The redirection must run inside the app's run-as shell, otherwise the device shell writes it.
    command = f"run-as {PKG} sh -c 'cat > files/dm-touch-record.txt'"
    with source.open("rb") as data:
        result = adb("shell", command, stdin=data)
    return result.returncode == 0


def require_app() -> bool:
    if app_running():
        return True
    print("游戏进程不在前台/未运行；先启动并进入游戏再执行。", file=sys.stderr)
    return False


def cmd_record(args: argparse.Namespace) -> int:
    if not require_app():
        return 2
    remove_flags()
    # 旧档留着会让 stop 后的“文件已存在”立刻成立，把上一轮内容当成新录制拉回来。
    # A stale file would make the post-stop existence check pass immediately and pull the previous run.
    runas("rm", "-f", "files/dm-touch-record.txt", "files/dm-touch-record.tmp")
    if runas("touch", "files/dm-touch-record").returncode != 0:
        print("无法创建录制标志文件。", file=sys.stderr)
        return 3
    print("录制已开始。现在在手机上操作到掉帧位置；结束后回电脑按回车（Ctrl+C 也可）。")
    try:
        if args.until_stopped:
            # 另一个终端/调用方执行 stop 移除旗标后，这里再取回文件；便于后台长录。
            # Another terminal/caller removes the flag with stop, then this side pulls the file; suits a long background capture.
            while runas("ls", "files/dm-touch-record", capture=True).returncode == 0:
                time.sleep(0.5)
        elif args.duration:
            time.sleep(args.duration)
        else:
            input()
    except (KeyboardInterrupt, EOFError):
        pass
    runas("rm", "-f", "files/dm-touch-record")
    if not wait_file("files/dm-touch-record.txt", 10):
        print("录制标志已移除，但设备上没有写出录制文件（游戏可能已退出）。", file=sys.stderr)
        return 4
    name = args.name or time.strftime("%Y%m%d-%H%M%S")
    destination = LOGS / f"touch-{name}.txt"
    if not pull_recording(destination):
        print("取回录制失败。", file=sys.stderr)
        return 5
    lines = sum(1 for _ in destination.open(encoding="utf-8", errors="replace"))
    print(f"录制完成：{destination}（{lines} 帧，设备端保留 files/dm-touch-record.txt）")
    return 0


def cmd_replay(args: argparse.Namespace) -> int:
    if not require_app():
        return 2
    if args.name:
        source = LOGS / f"touch-{args.name}.txt"
        if not source.exists():
            print(f"找不到本地录制 {source}", file=sys.stderr)
            return 3
        if not push_recording(source):
            print("把本地录制放回设备失败。", file=sys.stderr)
            return 4
    if not device_exists("files/dm-touch-record.txt"):
        print("设备上没有录制文件；先执行 record。", file=sys.stderr)
        return 3
    remove_flags()
    if runas("touch", "files/dm-touch-replay").returncode != 0:
        print("无法创建回放标志文件。", file=sys.stderr)
        return 5
    print("回放已开始，请不要触摸屏幕。")
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if device_exists("files/dm-touch-replay.done"):
            break
        time.sleep(0.5)
    else:
        print("等待回放结束超时；已请求停止。", file=sys.stderr)
    runas("rm", "-f", "files/dm-touch-replay", "files/dm-touch-replay.done")
    print("回放结束。")
    return 0


def cmd_stop(_: argparse.Namespace) -> int:
    runas("rm", "-f", "files/dm-touch-record", "files/dm-touch-replay")
    print("已移除控制标志；正在录制的内容会在 0.25 秒内写盘。")
    return 0


def cmd_status(_: argparse.Namespace) -> int:
    print("app_running:", app_running())
    listing = runas("ls", "-l", "files", capture=True)
    if listing.stdout:
        print(listing.stdout.rstrip())
    count = runas("wc", "-l", "files/dm-touch-record.txt", capture=True)
    if count.returncode == 0 and count.stdout.strip():
        print("recording:", count.stdout.strip())
    return 0


def main() -> int:
    global SERIAL
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", help="adb serial when several devices are attached")
    sub = parser.add_subparsers(dest="command", required=True)

    record = sub.add_parser("record", help="record in-game input")
    record.add_argument("--duration", type=float, help="stop automatically after N seconds")
    record.add_argument("--until-stopped", action="store_true", help="record until another invocation runs stop")
    record.add_argument("--name", help="local file suffix (default: timestamp)")
    record.set_defaults(func=cmd_record)

    replay = sub.add_parser("replay", help="replay a recording")
    replay.add_argument("--name", help="replay dm-work/build/logs/touch-NAME.txt (default: device file)")
    replay.add_argument("--timeout", type=float, default=900, help="maximum replay wait in seconds")
    replay.set_defaults(func=cmd_replay)

    stop = sub.add_parser("stop", help="clear the control flags")
    stop.set_defaults(func=cmd_stop)

    status = sub.add_parser("status", help="show app/files state")
    status.set_defaults(func=cmd_status)

    args = parser.parse_args()
    SERIAL = args.serial
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
