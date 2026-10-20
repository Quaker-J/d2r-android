#!/usr/bin/env python3
"""启动 d2r-android 并从启动界面一路点进营地：唤醒解锁 -> LauncherActivity -> 「启动游戏」-> 选人 -> 屏幕中心（手柄确认）-> 营地。

Every tap is taken from a live uiautomator dump, never from a remembered coordinate, so a changed
layout cannot make this click the wrong row. Screenshots are saved for the caller to look at: this
script can prove a tap landed, not that the right character was chosen.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PACKAGE = "org.d2r.android.port"
LAUNCHER = f"{PACKAGE}/.LauncherActivity"


def adb(*args, binary=False):
    return subprocess.run(["adb", *args], capture_output=True,
                          text=not binary, encoding="utf-8", errors="replace")


def wake_and_unlock():
    power = adb("shell", "dumpsys", "power").stdout
    if "mWakefulness=Awake" not in power:
        print("[launch] 屏幕未亮 -> KEYCODE_WAKEUP(224)")
        adb("shell", "input", "keyevent", "224")
        time.sleep(1)
    adb("shell", "svc", "power", "stayon", "true")
    window = adb("shell", "dumpsys", "window").stdout
    if "mDreamingLockscreen=true" in window or "Keyguard" in window:
        print("[launch] 仍在锁屏 -> MENU(82) + 上滑")
        adb("shell", "input", "keyevent", "82")
        adb("shell", "input", "swipe", "540", "2000", "540", "300", "150")
        time.sleep(1)
    if "mDreamingLockscreen=true" in adb("shell", "dumpsys", "window").stdout:
        sys.exit("[launch] 【红】解锁失败：安装/注入会被系统判成「用户拒绝」")


def dump_rows():
    """Return (rows, root_size). rows = (text, clickable, cx, cy); root_size = (w, h) of the screen.

    uiautomator appends a status line after </hierarchy>, so the document is cut before parsing."""
    out = adb("exec-out", "uiautomator", "dump", "/dev/tty").stdout
    start, end = out.find("<?xml"), out.rfind("</hierarchy>")
    if start < 0 or end < 0:
        return None, None
    xml = out[start:end + len("</hierarchy>")]
    rows, largest, root = [], 0, None
    for node in re.finditer(r"<node\b[^>]*>", xml):
        tag = node.group(0)
        bounds = re.findall(r"\[(\d+),(\d+)\]", tag)
        if len(bounds) != 2:
            continue
        (x1, y1), (x2, y2) = (int(a) for a in bounds[0]), (int(a) for a in bounds[1])
        area = (x2 - x1) * (y2 - y1)
        if area > largest:
            largest, root = area, (x2 - x1, y2 - y1)
        text = re.search(r'\btext="([^"]*)"', tag)
        text = text.group(1) if text else ""
        if text.strip():
            rows.append((text, "clickable=\"true\"" in tag, (x1 + x2) // 2, (y1 + y2) // 2))
    return rows, root


def show(rows):
    for text, clickable, cx, cy in rows:
        print(f"    {'[点]' if clickable else '[ ]'} {text!r} -> {cx},{cy}")


def tap(cx, cy, what):
    adb("shell", "input", "tap", str(cx), str(cy))
    print(f"[launch] 已点：{what} ({cx},{cy})")


def front():
    activity = re.search(r"topResumedActivity=.*?(\S+/\S+)", adb("shell", "dumpsys", "activity", "activities").stdout)
    return (activity.group(1) if activity else "(无)"), adb("shell", "pidof", PACKAGE).stdout.strip()


def alive_or_die(stage):
    activity, pid = front()
    print(f"[launch] {stage}: pid={pid or '(空)'} activity={activity}")
    if not pid:
        sys.exit(f"[launch] 【红】{stage} 时进程不在了 —— 闪退，先取 game.log 与 logcat -b crash，别再等")
    return activity, pid


def screenshot(directory, name):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    with open(path, "wb") as handle:
        subprocess.run(["adb", "exec-out", "screencap", "-p"], stdout=handle, stderr=subprocess.DEVNULL)
    print(f"[launch] 截图 {path} ({path.stat().st_size} 字节) —— 用 Read 看它，脚本判断不了画面内容")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wait-launch", type=int, default=15, help="点「启动游戏」后到选人界面的等待秒数")
    parser.add_argument("--wait-enter", type=int, default=15, help="点屏幕中心后到进营地的等待秒数")
    parser.add_argument("--settle", type=int, default=0, help="进营地后再静置多少秒（采数用 30 足够，不必更长）")
    parser.add_argument("--stop-at-select", action="store_true", help="停在选人界面，不点屏幕中心")
    parser.add_argument("--out", type=Path, default=Path("dm-work/build/logs"), help="截图落盘目录（Windows 下 python 看不见 /tmp）")
    parser.add_argument("--tag", default=time.strftime("%H%M"), help="截图文件名后缀，避免 A/B 两轮互相覆盖")
    args = parser.parse_args()

    wake_and_unlock()
    adb("shell", "am", "start", "-n", LAUNCHER)
    time.sleep(5)
    activity, _ = alive_or_die("启动 5 秒后")
    if "LauncherActivity" not in activity:
        print(f"[launch] 【注意】前台不是启动界面（{activity}）：可能已经进了游戏，也可能崩在了启动阶段。先确认再继续。")

    rows, screen = dump_rows()
    if not rows:
        sys.exit("[launch] 【红】uiautomator 没拿到界面 —— 屏幕是否亮着？是否在锁屏？")
    show(rows)
    target = next((r for r in rows if "启动游戏" in r[0] and r[1]), None)
    if not target:
        sys.exit("[launch] 【红】找不到可点的「启动游戏」行。若上面出现「导入数据包」相关提示，说明数据包没导入，别硬点。")
    tap(target[2], target[3], "启动游戏")

    time.sleep(args.wait_launch)
    alive_or_die(f"等 {args.wait_launch} 秒（选人）后")
    screenshot(args.out, f"camp-a-select-{args.tag}.png")
    if args.stop_at_select:
        print("[launch] --stop-at-select：停在选人界面")
        return

    if not screen:
        sys.exit("[launch] 【红】dump 里没有屏幕尺寸，无法算中心点")
    tap(screen[0] // 2, screen[1] // 2, "屏幕中心（触屏全部映射为手柄确认 = 进入游戏）")

    time.sleep(args.wait_enter)
    activity, pid = alive_or_die(f"等 {args.wait_enter} 秒（进营地）后")
    if "D2RActivity" not in activity:
        print(f"[launch] 【注意】前台活动是 {activity}，不像是游戏本体 —— 截图确认是否真的进了营地")
    path = screenshot(args.out, f"camp-b-camp-{args.tag}.png")
    if args.settle:
        print(f"[launch] 营地静置 {args.settle} 秒取稳态…")
        time.sleep(args.settle)
        alive_or_die("静置结束")
    print(f"[launch] 完成。pid={pid} 截图={path.name}；接着跑 pull_camp_metrics.py 取日志（叠加层只能当一眼确认）")


if __name__ == "__main__":
    main()
