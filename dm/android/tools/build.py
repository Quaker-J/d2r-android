#!/usr/bin/env python3
"""构建独立 ARM64 NativeActivity APK 与运行时测试，不依赖 Gradle。
Build a standalone ARM64 NativeActivity APK and runtime tests without Gradle.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("DM_ANDROID_TOOLS", str(Path.home() / "dm-work/tools/android")))
BIN = TOOLS / "ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"

GAME_ACTIVITY = "org.d2r.android.port.D2RActivity"
LAUNCHER_NAME = "org.d2r.android.port.LauncherActivity"
# 正式构建把仓库清单里的单个 NativeActivity 换成这两条：启动界面当入口，游戏本体降级成它启动的第二个页面。
# 探针构建走不到这里：它没有 dex，清单里不能出现 Java 页面。
# The release build swaps the repository manifest's single NativeActivity for these two: the launcher is the
# entry point and the game becomes the second page it starts. The probe build never reaches this branch —
# it has no dex, so its manifest must not declare a Java activity.
LAUNCHER_ACTIVITY_BLOCK = (
    '<activity android:name="' + LAUNCHER_NAME + '" android:exported="true"'
    ' android:screenOrientation="landscape" android:configChanges="orientation|screenSize|keyboardHidden"'
    ' android:theme="@android:style/Theme.Material.NoActionBar">\n'
    '            <intent-filter>\n'
    '                <action android:name="android.intent.action.MAIN" />\n'
    '                <category android:name="android.intent.category.LAUNCHER" />\n'
    '            </intent-filter>\n'
    '        </activity>\n'
    '        ')
GAME_ACTIVITY_BLOCK = (
    '<activity android:name="' + GAME_ACTIVITY + '" android:exported="false"'
    ' android:screenOrientation="landscape" android:configChanges="orientation|screenSize|keyboardHidden"'
    ' android:theme="@android:style/Theme.Material.NoActionBar">\n'
    '            <meta-data android:name="android.app.lib_name" android:value="dmandroid" />\n'
    '        </activity>')


def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def build_counter_timestamp(build):
    # 生产辅助着色器每次构建从GLSL生成，独立于游戏缓存。
    # Generate the production helper from GLSL independently of the game cache.
    output = build / "counter_timestamp.spv"
    run([TOOLS / "ndk/shader-tools/linux-x86_64/glslc", "--target-env=vulkan1.1", "-O",
         ROOT / "native/shaders/counter_timestamp.comp", "-o", output])
    data = output.read_bytes()
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    text = "// 从生产GLSL自动生成。| Generated from production GLSL.\n#pragma once\n#include <stdint.h>\n"
    text += "static const uint32_t dm_vk_counter_timestamp_spv[] = {\n"
    text += "".join("    " + ",".join(f"0x{word:08x}u" for word in words[i:i+8]) + ",\n" for i in range(0,len(words),8))
    text += "};\n#define DM_VK_COUNTER_TIMESTAMP_SPV_BYTES (sizeof dm_vk_counter_timestamp_spv)\n"
    header = ROOT / "native/counter_timestamp_spv.h"
    if not header.exists() or header.read_text() != text:
        header.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--game-source", type=Path, help="Local rewritten Android game modules (includes game code in APK)")
    parser.add_argument("--game-run", action="store_true", help="Run the real game host with NVN/Vulkan instead of the diagnostic frontend")
    parser.add_argument("--shader-cache", type=Path, default=Path.home() / "dm-work/build/android/shaders/spirv")
    args = parser.parse_args()
    if args.game_run and not args.game_source: parser.error("--game-run requires --game-source")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    if args.game_run: build_counter_timestamp(build)
    for required in [BIN / "aarch64-linux-android29-clang", TOOLS / "jdk/bin/java", TOOLS / "platform/android.jar"]:
        if not required.exists(): sys.exit("Missing tool: " + str(required) + "; run tools/bootstrap.py")
    env = dict(os.environ, JAVA_HOME=str(TOOLS / "jdk"), PATH=str(TOOLS / "jdk/bin") + os.pathsep + os.environ["PATH"])
    entry = build / "entry_elf.S"
    run([sys.executable, ROOT / "tools/generate_entry.py", entry])
    run([sys.executable, ROOT / "tools/strings.py", build])
    run([sys.executable, ROOT / "tools/make_probe.py", build / "probe", BIN])
    run([sys.executable, ROOT / "tools/build_gpu_driver.py"])
    run([sys.executable, ROOT / "tools/build_gpu_fixtures.py"])
    compiler = BIN / "aarch64-linux-android29-clang"
    common = [compiler, "-O1", "-g", "-Wall", "-Wextra", "-I", ROOT / "native", "-I", build, "-I", build / "gpu-driver/include", ROOT / "native/runtime.c", ROOT / "native/probe.c", entry]
    run(common + ["-ldl", "-o", build / "runtime-smoke"])
    if args.game_run:
        run([sys.executable, ROOT / "tools/host_link.py", "--activity"])
        shutil.copyfile(build / "game-host/libdmandroid.so", build / "libdmandroid.so")
    else:
        run(common + ["-fPIC", "-shared", "-DDM_NATIVE_APP", "-Wl,-z,max-page-size=16384", "-Wl,-z,defs",
                  ROOT / "native/activity.c", ROOT / "native/app_probe.c", ROOT / "native/vulkan_front.c",
                  ROOT / "native/game_modules.c", ROOT / "native/gpu_driver.c",
                  ROOT / "native/host_posix.c", ROOT / "tests/host_posix_test.c",
                  ROOT.parent / "ios/Sources/dm_touch_controls.c", "-landroid", "-llog", "-ldl", "-lm", "-o", build / "libdmandroid.so"])
    assets = build / "assets"
    assets.mkdir(exist_ok=True)
    licenses = assets / "licenses"
    licenses.mkdir(exist_ok=True)
    for license_file in (build / "gpu-driver/licenses").glob("*.txt"):
        shutil.copyfile(license_file, licenses / license_file.name)
    # 启动画面背景图随源码发布（dm/android/artwork/），新电脑 clone 后能重建出同样的 APK。
    # dm/android/local/ 是可选覆盖（已 gitignore）：想换成自己的图，把同名文件放那儿即可。
    # The launcher backdrop ships with the source (dm/android/artwork/) so a fresh clone rebuilds the same APK.
    # dm/android/local/ is an optional gitignored override: drop a same-named file there to use your own image.
    for artwork in sorted((ROOT / "artwork").glob("launcher-background.*")):
        shutil.copyfile(artwork, assets / artwork.name)
    for override in sorted((ROOT / "local").glob("launcher-background.*")):
        shutil.copyfile(override, assets / override.name)
    # 诊断GLSL缓存独立于游戏的离线Maxwell fullshader缓存，不覆盖游戏资源。
    # Keep diagnostic GLSL shaders separate from the game's offline Maxwell fullshader cache.
    shutil.copytree(build / "gpu-test-shaders", assets / "gpu-test-shaders", dirs_exist_ok=True)
    for name in ["probe.fixups.tsv", "probe.gates.tsv"]:
        shutil.copyfile(build / "probe" / name, assets / name)
    if args.game_source:
        run([sys.executable, ROOT / "tools/guest_elf.py", args.game_source, args.game_source, "main", "sdk", "subsdk0"])
        for name in ["main", "sdk", "subsdk0"]:
            shutil.copyfile(args.game_source / (name + ".fixups.tsv"), assets / (name + ".fixups.tsv"))
            if args.game_run: shutil.copyfile(args.game_source / (name + ".gates.tsv"), assets / (name + ".gates.tsv"))
    else:
        for name in ["main", "sdk", "subsdk0"]:
            (assets / (name + ".fixups.tsv")).unlink(missing_ok=True)
    unsigned = build / "unsigned.apk"
    activity = "android.app.NativeActivity"
    manifest = ROOT / "AndroidManifest.xml"
    if args.game_run:
        classes, dex = build / "java-classes", build / "dex"
        if classes.exists(): shutil.rmtree(classes)
        classes.mkdir(); dex.mkdir(exist_ok=True)
        run([TOOLS / "jdk/bin/javac", "--release", "8", "-classpath", TOOLS / "platform/android.jar", "-d", classes,
             *sorted((ROOT / "java").rglob("*.java"))])
        run([TOOLS / "build-tools/d8", "--min-api", "29", "--lib", TOOLS / "platform/android.jar", "--output", dex,
             *sorted(classes.rglob("*.class"))], env=env)
        activity = LAUNCHER_NAME
        manifest = build / "game-AndroidManifest.xml"
        # 正式游戏声明游戏类别；探针应用仍保持原类别。
        # Declare the game category only for the playable application, not the probe.
        source = (ROOT / "AndroidManifest.xml").read_text()
        source = source.replace('android:hasCode="false"', 'android:hasCode="true" android:appCategory="game"')
        source = source.replace('0.1-runtime-probe', '0.2-android-port')
        source, replaced = re.subn(r'<activity android:name="android\.app\.NativeActivity".*?</activity>',
                                   LAUNCHER_ACTIVITY_BLOCK + GAME_ACTIVITY_BLOCK, source, count=1, flags=re.S)
        if replaced != 1: sys.exit("game manifest: expected exactly one NativeActivity activity block")
        manifest.write_text(source)
    run([TOOLS / "build-tools/aapt2", "compile", "--dir", build / "res", "-o", build / "resources.zip"])
    run([TOOLS / "build-tools/aapt2", "link", "--manifest", manifest,
         "-I", TOOLS / "platform/android.jar", "-A", assets, "-o", unsigned, build / "resources.zip"])
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_STORED) as z:
        if args.game_run: z.write(dex / "classes.dex", "classes.dex")
        z.write(build / "libdmandroid.so", "lib/arm64-v8a/libdmandroid.so")
        z.write(build / "probe/libdmguest_probe.so", "lib/arm64-v8a/libdmguest_probe.so")
        for library in sorted((build / "gpu-driver").glob("*.so")):
            z.write(library,"lib/arm64-v8a/" + library.name)
        for fixture in sorted((build / "gpu-fixtures").glob("*.so")):
            z.write(fixture,"lib/arm64-v8a/" + fixture.name)
        if args.game_source:
            for name in ["main", "sdk", "subsdk0"]:
                z.write(args.game_source / ("libdmguest_" + name + ".so"), "lib/arm64-v8a/libdmguest_" + name + ".so")
    aligned = build / "aligned.apk"
    run([TOOLS / "build-tools/zipalign", "-f", "-P", "16", "4", unsigned, aligned])
    keystore = TOOLS / "debug.keystore"
    if not keystore.exists():
        run([TOOLS / "jdk/bin/keytool", "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass", "android",
             "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
             "-dname", "CN=Android Debug,O=Android,C=US"], env=env)
    apk = build / "D2R-Android-port.apk"
    run([TOOLS / "build-tools/apksigner", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--out", apk, aligned], env=env)
    run([TOOLS / "build-tools/apksigner", "verify", apk], env=env)
    print("APK:", apk)
    if args.install:
        run([TOOLS / "platform-tools/adb", "install", "-r", apk])
        if args.game_run:
            if not args.shader_cache.is_dir(): sys.exit("Missing shader cache: " + str(args.shader_cache))
            run([TOOLS / "platform-tools/adb", "shell", "mkdir", "-p", "/sdcard/Android/data/org.d2r.android.port/files/shaders"])
            run([TOOLS / "platform-tools/adb", "push", "--sync", str(args.shader_cache) + "/.", "/sdcard/Android/data/org.d2r.android.port/files/shaders/"])
        run([TOOLS / "platform-tools/adb", "shell", "am", "start", "-n", "org.d2r.android.port/" + activity])


if __name__ == "__main__":
    main()
