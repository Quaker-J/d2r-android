#!/usr/bin/env python3
"""Android SDK/NDK/JDK 工具查找：同时接受旧版扁平工具根和 Windows/Linux SDK 布局。

Locate Android SDK/NDK/JDK tools for the build scripts. Accepts both the legacy flat tool root
(ndk/, jdk/, platform/, build-tools/, platform-tools/ directly under DM_ANDROID_TOOLS) and a
normal SDK root (ndk/<version>/, platforms/android-*/ , build-tools/<version>/).
"""
from functools import lru_cache
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HOST_TAG = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
API = 29


def _version_key(path):
    return tuple(int(part) for part in re.findall(r"\d+", path.name)) or (-1,)


def _tool(directory, name):
    directory = Path(directory)
    suffixes = (".exe", ".bat", ".cmd", "") if os.name == "nt" else ("",)
    for suffix in suffixes:
        candidate = directory / (name + suffix)
        if candidate.is_file():
            return candidate
    return None


@lru_cache
def sdk_root():
    for variable in ("DM_ANDROID_TOOLS", "ANDROID_SDK_ROOT", "ANDROID_HOME"):
        value = os.environ.get(variable)
        if value and Path(value).is_dir():
            return Path(value)
    adb = shutil.which("adb")
    if adb:
        return Path(adb).resolve().parent.parent
    candidates = (Path.home() / "dm-work/tools/android",
                  Path.home() / "AppData/Local/Android/Sdk",
                  Path("D:/Android/SDK"))
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    sys.exit("Android SDK not found; set DM_ANDROID_TOOLS, ANDROID_SDK_ROOT or ANDROID_HOME")


@lru_cache
def ndk_root():
    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "DM_NDK"):
        value = os.environ.get(variable)
        if value and (Path(value) / "toolchains").is_dir():
            return Path(value)
    root = sdk_root() / "ndk"
    if (root / "toolchains").is_dir():
        return root
    if root.is_dir():
        versions = [path for path in root.iterdir() if (path / "toolchains").is_dir()]
        if versions:
            return max(versions, key=_version_key)
    sys.exit("Android NDK not found; set ANDROID_NDK_HOME or install it under the SDK")


@lru_cache
def ndk_bin():
    path = ndk_root() / "toolchains/llvm/prebuilt" / HOST_TAG / "bin"
    if not path.is_dir():
        sys.exit("Android NDK host toolchain not found: " + str(path))
    return path


@lru_cache
def sysroot():
    return ndk_bin().parent / "sysroot"


@lru_cache
def clang_argv():
    binary = ndk_bin()
    if os.name == "nt":
        compiler = binary / "clang.exe"
        flags = ["--target=aarch64-linux-android" + str(API), "--sysroot", str(sysroot()), "-fno-emulated-tls"]
    else:
        compiler = binary / ("aarch64-linux-android" + str(API) + "-clang")
        flags = []
        if not compiler.is_file():
            compiler = binary / "clang"
            flags = ["--target=aarch64-linux-android" + str(API), "--sysroot", str(sysroot())]
    if not compiler.is_file():
        sys.exit("Android clang not found: " + str(compiler))
    return tuple([str(compiler), *flags])


def llvm_tool(name):
    suffix = ".exe" if os.name == "nt" else ""
    path = ndk_bin() / (name + suffix)
    if not path.is_file():
        sys.exit("Android NDK tool not found: " + str(path))
    return path


def _shader_tool(name):
    suffix = ".exe" if os.name == "nt" else ""
    for directory in (ndk_root() / "shader-tools" / HOST_TAG, ndk_bin()):
        path = directory / (name + suffix)
        if path.is_file():
            return path
    sys.exit("Android shader tool not found: " + name)


def glslc():
    return _shader_tool("glslc")


def spirv_val():
    return _shader_tool("spirv-val")


@lru_cache
def build_tools():
    root = sdk_root() / "build-tools"
    if _tool(root, "aapt2"):
        return root
    if root.is_dir():
        versions = [path for path in root.iterdir() if _tool(path, "aapt2")]
        if versions:
            return max(versions, key=_version_key)
    sys.exit("Android build-tools not found under " + str(root))


def build_tool(name):
    path = _tool(build_tools(), name)
    if not path:
        sys.exit("Android build tool not found: " + name + " in " + str(build_tools()))
    return path


@lru_cache
def android_jar():
    value = os.environ.get("DM_ANDROID_JAR")
    if value and Path(value).is_file():
        return Path(value)
    flat = sdk_root() / "platform/android.jar"
    if flat.is_file():
        return flat
    platforms = sdk_root() / "platforms"
    if platforms.is_dir():
        jars = [path / "android.jar" for path in platforms.iterdir() if (path / "android.jar").is_file()]
        if jars:
            return max(jars, key=lambda path: _version_key(path.parent))
    sys.exit("android.jar not found; set DM_ANDROID_JAR or install an SDK platform")


@lru_cache
def platform_tools():
    path = sdk_root() / "platform-tools"
    if path.is_dir():
        return path
    sys.exit("Android platform-tools not found: " + str(path))


def adb():
    path = _tool(platform_tools(), "adb")
    if not path:
        sys.exit("adb not found in " + str(platform_tools()))
    return path


@lru_cache
def _jdk_major(path):
    javac = _tool(Path(path) / "bin", "javac")
    if not javac:
        return 0
    result = subprocess.run([str(javac), "-version"], capture_output=True, text=True)
    match = re.search(r"javac (\d+)", (result.stderr or "") + (result.stdout or ""))
    return int(match.group(1)) if match else 0


def _valid_jdk(path):
    path = Path(path) if path else None
    return path if path and _jdk_major(path) >= 9 else None


@lru_cache
def jdk_home():
    if _valid_jdk(os.environ.get("DM_JDK")):
        return Path(os.environ["DM_JDK"])
    if _valid_jdk(os.environ.get("JAVA_HOME")):
        return Path(os.environ["JAVA_HOME"])
    candidates = [sdk_root() / "jdk", Path.home() / "dm-work/tools/android/jdk"]
    if os.name == "nt":
        for root in (Path("C:/Program Files/Java"), Path("C:/Program Files/Eclipse Adoptium")):
            if root.is_dir():
                candidates += [path for path in root.iterdir() if path.is_dir()]
    else:
        candidates += [path for path in Path("/usr/lib/jvm").glob("*") if path.is_dir()]
    valid = [path for path in candidates if _valid_jdk(path)]
    if valid:
        return max(valid, key=lambda path: (_jdk_major(path), _version_key(path)))
    sys.exit("JDK 9+ not found; set DM_JDK or JAVA_HOME to a JDK 11/17 installation")


def jdk_tool(name):
    path = _tool(jdk_home() / "bin", name)
    if not path:
        sys.exit("JDK tool not found: " + name + " in " + str(jdk_home()))
    return path


def java():
    return jdk_tool("java")


def javac():
    return jdk_tool("javac")


def keytool():
    return jdk_tool("keytool")


def d8():
    return build_tool("d8")


def aapt2():
    return build_tool("aapt2")


def zipalign():
    return build_tool("zipalign")


def apksigner():
    return build_tool("apksigner")
