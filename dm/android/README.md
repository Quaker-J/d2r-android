# Android build and deployment

The maintained step-by-step instructions are in [README.zh-CN.md](README.zh-CN.md). See the repository [status](../../README.md) and [snapshot boundaries](../../docs/ANDROID_RELEASE_SNAPSHOT.zh-CN.md).

Build on Linux x86_64 using `tools/bootstrap.py` (NDK r28c, API35, JDK), then install Python dependencies in a virtual environment: `lz4 capstone unicorn numpy pillow zstandard nsz`. Tools default to `~/dm-work/tools/android`; override with `DM_ANDROID_TOOLS`.

Prepare your own RomFS, rewritten `main`, `sdk` and `subsdk0` modules, and a matching SPIR-V cache. The repository provides `extract_packages.py`, `patch_nso.py` with `DM_TARGET=android`, and `build_shaders.py`; it does not distribute game data, keys or the generated artifacts. Shader translation additionally requires a compatible .NET installation.

```bash
python dm/android/tools/build.py --game-run \
  --game-source ~/dm-work/build/android/game \
  --shader-cache ~/dm-work/build/android/shaders/spirv
```

Output: `dm/android/build/D2R-Android-port.apk`. It includes locally derived game code and is not part of the source publication. Install using `adb -s DEVICE install -r`, and deploy RomFS and shaders once into `/sdcard/Android/data/org.d2r.android.port/files/{romfs,shaders}`. Start `org.d2r.android.port/.D2RActivity`. Updating the APK preserves app data; do not overwrite saves in private `files/save`.

Touch controls, gamepads, keyboard input, native name entry, AAudio and a performance overlay are implemented. The display preserves the game's aspect ratio. Logs are readable with `adb -s DEVICE shell run-as org.d2r.android.port tail -100 files/game.log`.

The current source includes private `files/pipeline-cache` persistence, with the first write verified but restart loading/benefits still unverified. Diagnostic/test sources remain available; no new tests were added or run during publication preparation.
