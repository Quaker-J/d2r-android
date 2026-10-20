#!/usr/bin/env python3
"""从真机 game.log 取营地稳态的六个对照指标，可单跑也可 A/B 对比。

Field names come straight from the source that prints them, so a renamed counter shows up as "缺行"
instead of silently reading as zero:
  [vulkan-perf]           fps=                                   nvn_vulkan.c
  [vulkan-frame-stability] over33ms=N samples=120, interval=mean/p50/p95/max
  [vulkan-time]           GPU=.. fence_wait=.. ms/frame
  [vulkan-dependency-time] reason=total calls=/frame wait=..   （reason=ssbo 的行只计数，不混进均值）

Overlay numbers on the screen are not evidence; the pulled log is.
"""
import argparse
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PACKAGE = "org.d2r.android.port"
LOG_FILE = "files/game.log"
METRICS = [
    ("fps", r"\[vulkan-perf\].*?fps=([\d.]+)", "每 60 帧窗均值"),
    ("interval p50", r"\[vulkan-frame-stability\].*?interval=[\d.]+/([\d.]+)", "120 帧窗中位帧间隔 ms"),
    ("over33ms/120", r"\[vulkan-frame-stability\] frame=\d+ samples=(\d+) over33ms=(\d+)", "超 33ms 的帧数/窗口"),
    ("GPU=", r"\[vulkan-time\].*?GPU=([-\d.]+) ms/frame", "GPU 批跨度 ms/帧"),
    ("fence_wait=", r"\[vulkan-time\].*?fence_wait=([-\d.]+) ms/frame", "等 GPU 完成 ms/帧"),
    ("dep calls/wait", r"\[vulkan-dependency-time\].*?reason=total calls=([\d.]+)/frame.*?wait=([\d.]+)", "合计依赖 次/帧 与 wait ms/帧"),]


def adb_exec(remote):
    return subprocess.run(["adb", "exec-out", *remote], capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


def pull(directory, label):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"game-log-{time.strftime('%Y%m%d-%H%M')}-{label}.log"
    text = adb_exec(["run-as", PACKAGE, "cat", LOG_FILE])
    if not text.strip():
        sys.exit(f"[metrics] 【红】取回空日志：{LOG_FILE} 不存在或包不可 run-as（非 debuggable？）")
    path.write_text(text, encoding="utf-8", errors="replace")
    print(f"[metrics] 取回 {path} 行数={len(text.splitlines())}")
    return path




def parse(path, windows):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    results, missing = {}, []
    for name, pattern, note in METRICS:
        found = [re.search(pattern, line) for line in lines]
        hits = [m.groups() for m in found if m]
        if not hits:
            missing.append(name)
            continue
        tail = hits[-windows:]
        if name == "over33ms/120":
            samples = {int(s) for s, _ in tail}
            values = [int(o) for _, o in tail]
            results[name] = (f"{statistics.mean(values):.2f}/{sorted(samples)[0]}", note)
        elif name == "dep calls/wait":
            calls = [float(c) for c, _ in tail]
            waits = [float(w) for _, w in tail]
            results[name] = (f"{statistics.mean(calls):.3f}/{statistics.mean(waits):.4f}", note)
        else:
            values = [float(v[0]) for v in tail]
            results[name] = (f"{statistics.mean(values):.2f}", note)
    fatal = sum("ANDROID_FATAL" in line or "] STOP " in line for line in lines)
    ssbo = sum("reason=ssbo" in line for line in lines)
    frames = [int(m.group(1)) for line in lines for m in [re.search(r"\[vulkan-perf\] frame=(\d+)", line)] if m]
    integrity = (f"末帧={max(frames) if frames else '?'} 致命行={fatal} ssbo行={ssbo} 报告行={len(lines)}")
    return results, missing, integrity


def render(runs, windows):
    labels = [label for label, _ in runs]
    print(f"\n=== 营地对照（各取末尾 {windows} 个 60 帧窗）===")
    print(f"{'指标':<18}" + "".join(f"{l:>16}" for l in labels) + (" " * 2 + "Δ" if len(labels) > 1 else ""))
    for name, _, note in METRICS:
        cells = [results.get(name, ("缺行",))[0] for _, (results, _, _) in runs]
        line = f"{name:<18}" + "".join(f"{c:>16}" for c in cells)
        if len(labels) > 1:
            try:
                values = [float(re.match(r"[-\d.]+", cell).group()) for cell in cells[:2]]
                line += f"  {values[1] - values[0]:+.2f}"
            except (AttributeError, ValueError):
                line += "  (不可算)"
        print(line + f"    # {note}")
    for label, (_, _, integrity) in runs:
        print(f"[{label}] {integrity}")
    for label, (_, missing, _) in runs:
        if missing:
            print(f"[{label}] 【注意】缺这些行，说明该诊断没出数据或标签改名了：{'、'.join(missing)}")
    print("提示：interval p50 换算 FPS = 1000/p50；对照成立的前提是同一坐标、同相位、同一档位。")


def abnormal_listing():
    """Return [(size, name)] for every abnormal-frames.log generation on the device.

    No `sh -c` here on purpose: adb joins argv with spaces and lets the device shell re-parse it, so
    `sh -c "ls -l files/x"` reaches the device as plain `ls`. Listing `files` and filtering locally
    avoids that, and `.1` / `.2` only exist once the open-time rotation lands in the APK."""
    rows = []
    for line in adb_exec(["run-as", PACKAGE, "ls", "-l", "files"]).splitlines():
        parts = line.split()
        if len(parts) < 5 or not parts[-1].startswith("abnormal-frames.log"):
            continue
        try:
            rows.append((int(parts[4]), parts[-1]))
        except ValueError:
            print(f"[metrics] 【注意】ls 行认不出大小，原样贴出来：{line}")
    return rows


def pull_abnormal(directory, stem, max_mb):
    rows = abnormal_listing()
    if not rows:
        print("[metrics] 设备上没有任何 abnormal-frames.log*：这版没写出异常帧，或者装的是旧包")
        return
    for size, name in sorted(rows):
        suffix = name[len("abnormal-frames.log"):]
        if suffix not in ("", ".1", ".2"):
            print(f"[metrics] 【跳】认不出的代次后缀：{name}")
            continue
        tag = {"": "当前", ".1": "上一次", ".2": "上上次"}[suffix]
        if size > max_mb * 1048576:
            print(f"[metrics] 【跳】{name}（{tag}）{size / 1048576:.1f} MB > --max-mb {max_mb}：抬上限或先在设备上清")
            continue
        text = adb_exec(["run-as", PACKAGE, "cat", f"files/{name}"])
        target = directory / f"{stem}-abnormal.log{suffix}"
        target.write_text(text, encoding="utf-8", errors="replace")
        print(f"[metrics] 异常帧日志 {name}（{tag}）{size / 1048576:.1f} MB -> {target.name} ({len(text.splitlines())} 行)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*", type=Path, help="已有本地日志（省略则从设备取回）；给两个就是 A/B")
    parser.add_argument("--labels", default="A,B", help="对比时的列名，逗号分隔")
    parser.add_argument("--windows", type=int, default=5, help="每份日志取末尾几个 60 帧窗（默认 5 = 300 帧）")
    parser.add_argument("--out", type=Path, default=Path("dm-work/build/logs"), help="取回落盘目录")
    parser.add_argument("--abnormal", action="store_true",
                        help="把设备上的 abnormal-frames.log 各代（当前 / .1 / .2）一起拉回，文件名跟着 game.log")
    parser.add_argument("--max-mb", type=int, default=40, help="--abnormal 单个文件的上限，超过只跳过并报大小")
    args = parser.parse_args()

    runs, labels = [], [l.strip() for l in args.labels.split(",")]
    if args.files:
        for i, path in enumerate(args.files):
            runs.append((labels[i] if i < len(labels) else path.stem, parse(path, args.windows)))
        render(runs, args.windows)
        return

    path = pull(args.out, labels[0])
    runs.append((labels[0], parse(path, args.windows)))
    render(runs, args.windows)
    if args.abnormal:
        pull_abnormal(args.out, path.stem, args.max_mb)
    else:
        rows = abnormal_listing()
        sizes = "、".join(f"{name} {size / 1048576:.1f} MB" for size, name in rows) or "无"
        print(f"[metrics] 异常帧日志（只报大小，要拉回加 --abnormal）：{sizes}")


if __name__ == "__main__":
    main()
