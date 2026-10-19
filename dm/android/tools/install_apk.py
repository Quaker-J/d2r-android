import subprocess
import time
import re
import hashlib
import xml.etree.ElementTree as ET
import sys
from pathlib import Path

# 默认装 build.py 的产物；也可以把别的 APK 路径作为参数传入。
# By default install build.py's output; pass another APK path as the argument if needed.
DEFAULT_APK = str(Path(__file__).resolve().parents[1] / "build" / "D2R-Android-port.apk")
DEVICE_APK = "/data/local/tmp/d2r.apk"
PACKAGE = "org.d2r.android.port"

def adb(cmd):
    return subprocess.run(f"adb {cmd}", shell=True, capture_output=True, text=True, encoding="utf-8", errors="ignore")

def ensure_screen_awake_and_unlocked():
    print("[install] 检查屏幕状态...")
    p = adb("shell dumpsys power")
    if "mWakefulness=Awake" not in p.stdout:
        print("[install] 屏幕未亮起，正在唤醒屏幕...")
        adb("shell input keyevent 26")
        time.sleep(1)

    # 保持屏幕常亮
    adb("shell svc power stayon true")

    # 检查是否在锁屏界面
    w = adb("shell dumpsys window")
    if "Keyguard" in w.stdout or "NotificationShade" in w.stdout:
        print("[install] 屏幕处于锁屏状态，正在滑动解锁...")
        adb("shell input keyevent 82")
        adb("shell input swipe 540 2000 540 300 150")
        time.sleep(1)

def parse_bounds(bounds_str):
    m = re.findall(r'\[(\d+),(\d+)\]', bounds_str)
    if len(m) == 2:
        x1, y1 = int(m[0][0]), int(m[0][1])
        x2, y2 = int(m[1][0]), int(m[1][1])
        return (x1 + x2) // 2, (y1 + y2) // 2, x1, y1, x2, y2
    return None

def monitor_and_click(timeout=45):
    start = time.time()
    checkbox_checked = False
    print("[install] 开始循环监测安装对话框中的 CheckBox 与继续安装按钮...")

    while time.time() - start < timeout:
        # 直接走 exec-out，不在设备上留 ui.xml | use exec-out so no ui.xml is left on the device
        xml_content = adb("exec-out uiautomator dump /dev/tty").stdout.strip()

        if not xml_content.startswith("<?xml") and "<hierarchy" not in xml_content:
            time.sleep(1)
            continue

        try:
            root = ET.fromstring(xml_content)
        except Exception:
            time.sleep(1)
            continue

        cb_node = None
        btn_node = None

        for node in root.iter("node"):
            cls = node.attrib.get("class", "")
            rid = node.attrib.get("resource-id", "")
            text = node.attrib.get("text", "")
            desc = node.attrib.get("content-desc", "")

            if "CheckBox" in cls or "deleted_file_state_cb" in rid or "已了解应用的风险检测结果" in text or "已了解" in desc:
                cb_node = node
            if "button1" in rid or "继续安装" in text or "继续安装" in desc or "Install" in text or "允许" in text:
                btn_node = node

        if cb_node is not None:
            is_checked = cb_node.attrib.get("checked", "false").lower() == "true"
            b_info = parse_bounds(cb_node.attrib.get("bounds", ""))
            if b_info:
                cx, cy, x1, y1, x2, y2 = b_info
                if not is_checked and not checkbox_checked:
                    # 点击复选框左侧方框位置
                    tap_x = x1 + 30
                    tap_y = cy
                    print(f"[install] 发现未选中的风险提示复选框，点击位置: ({tap_x}, {tap_y})")
                    adb(f"shell input tap {tap_x} {tap_y}")
                    checkbox_checked = True
                    time.sleep(1)
                    continue

        if btn_node is not None:
            b_info = parse_bounds(btn_node.attrib.get("bounds", ""))
            if b_info:
                cx, cy, _, _, _, _ = b_info
                print(f"[install] 发现继续安装按钮，点击位置: ({cx}, {cy})")
                adb(f"shell input tap {cx} {cy}")
                time.sleep(1.5)
                # 再次确认是否需要重试或安装已结束
                break

        time.sleep(1)

def local_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def verify_md5(local_apk):
    """双端 md5 比对：lastUpdateTime 只能证明「发生过更新」，装到旧包也会刷新。
    Two-sided md5 compare: lastUpdateTime only proves "an update happened" and also refreshes when an old package got installed."""
    p = adb(f"shell pm path {PACKAGE}")
    dev = p.stdout.strip().split("package:", 1)[-1].strip()
    if not dev:
        print("[install] 【红】pm path 取不到设备路径，跳过 md5 核对")
        return
    r = adb(f"shell md5sum '{dev}'")
    dev_md5 = r.stdout.split()[0] if r.stdout.strip() else "?"
    loc_md5 = local_md5(local_apk)
    ok = dev_md5 == loc_md5
    print(f"[install] md5 设备={dev_md5} 本地={loc_md5} {'一致' if ok else '【红】不一致（设备上不是本次构建）'}")
    lt = adb(f"shell dumpsys package {PACKAGE} | grep -m1 lastUpdateTime")
    print(f"[install] {lt.stdout.strip()}")

def install_apk(apk_path=DEFAULT_APK):
    ensure_screen_awake_and_unlocked()
    dev_path = apk_path
    local = not apk_path.startswith("/")
    if local:
        # 本机 APK：先推到设备（pm install 读的是设备上的文件） | local APK: push to the device first (pm install reads a device file)
        print(f"[install] 推送 {apk_path} → {DEVICE_APK} ...")
        r = adb(f'push "{apk_path}" {DEVICE_APK}')
        out = (r.stdout + r.stderr).strip()
        print(f"[install] push 结果: {out}")
        if "pushed" not in out:
            print("[install] 【红】push 失败，中止")
            return False
        dev_path = DEVICE_APK
    print(f"[install] 正在启动后台安装进程: pm install -r -d {dev_path} ...")
    proc = subprocess.Popen(f"adb shell pm install -r -d {dev_path}", shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    # 开始监控点击
    monitor_and_click()

    try:
        stdout, stderr = proc.communicate(timeout=90)
        print(f"[install] pm install 结果:\n{stdout}\n{stderr}")
        ok = "Success" in stdout
        if ok and local:
            verify_md5(apk_path)
        return ok
    except subprocess.TimeoutExpired:
        proc.kill()
        print("[install] 安装等待超时！")
        return False
    finally:
        if local:
            adb(f"shell rm -f {DEVICE_APK}")

if __name__ == "__main__":
    apk = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_APK
    success = install_apk(apk)
    sys.exit(0 if success else 1)
