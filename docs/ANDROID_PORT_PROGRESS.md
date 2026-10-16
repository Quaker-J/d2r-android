# Android port progress

Updated: 2026-10-16. This file is the entry point for resuming interrupted work.

中文：[ANDROID_PORT_PROGRESS.zh-CN.md](ANDROID_PORT_PROGRESS.zh-CN.md)

## Current state (read first when resuming)

The Android port enters Act I camp and moves; the stable phase approaches 60 FPS, while first-time pipeline compilation still causes hitches and full-scene stable 60 FPS is not reached yet. Detection, build and install evidence: `~/dm-work/research/android-latest-891f596/`.

- Private repository: [sydeEvans/d2r-android](https://github.com/sydeEvans/d2r-android). The upstream license and attribution are kept; no game assets, keys or APKs.
- Latest pulled, built and installed commit: `891f596172042a7d2db702e66fb425ca0b6d6102` (branch `main`, "Reduce Android Vulkan frame-time overhead").
- Main working directory: `~/d2r` (moved back from `~/dm-work/publish/d2r-android` on 2026-10-16; the earlier development site is archived at `~/dm-work/archive/d2r-old-site-20261016`). Source lives only in `~/d2r`; `~/dm-work` holds tools, resources and run artifacts only. Confirm directory and branch before continuing.
- Implemented: Vulkan rendering, AAudio sound, virtual stick and buttons, system keyboard name entry, landscape and display fit, FPS and CPU/GPU overlay.
- Observed on the tablet (OPPO OPD2409): stationary camp screenshot 60.0 FPS (CPU 7.7 ms, GPU 7.4 ms; two consecutive 120-frame windows with no frame over 33 ms); camp movement screenshot 58.2 FPS, ten 60-frame windows at 59.38-60.69 FPS. One first-time pipeline compile took 111.987 ms (frame 109872); another 91.010 ms long frame is not yet explained.
- Rebuildability (verified 2026-10-16): a fresh clone at `891f596` rebuilt end to end via `docs/ANDROID_BUILD.md`; the three generated tables and the rewritten `libdmguest_main/sdk/subsdk0.so` are byte-identical to the existing artifacts, and the production APK builds successfully (about 4 min 47 s). Evidence: `~/dm-work/research/android-repro-verify/`.
- Next steps: reduce first-time pipeline compile hitches (saved pipeline descriptions / load-stage prewarm), observe movement, new areas and combat frame intervals, deploy the same commit to the second phone, and keep this file current.

The Chinese file is the authoritative per-round record; the sections below are older English notes and history that are not maintained.

## Objective and user decisions

- Port the Switch ARM64 native execution layer to Android and attempt builds and on-device execution.
- Use `$HOME/Downloads/dd.zip`; do not repeat game package integrity/version checks.
- Use the local `ProdKeys.zip` for extraction. Never log or commit key contents.
- Wireless ADB pairing and connection were requested and completed.
- Keep key decisions, progress, validation, and resume commands in Markdown.

## Workspace and device

- Repository: `<LOCAL_WORKTREE>`; baseline `main`, commit `2eb5758`.
- Host: Linux x86_64; the home directory is mounted at both `/home/<user>` and `/var/home/<user>`.
- Device: `OPD2409`, SoC `SM8750P`, arm64-v8a, Android 16 / API 36, 4 KB pages, approximately 12 GB RAM.
- ADB: `~/dm-work/tools/android/platform-tools/adb`.
- Connected serial: `<设备序列号>`.
- Current connection: `<设备地址>`. Ports can change; use `adb devices -l` and `adb mdns services`.
- The temporary pairing code is not stored.
- Vulkan reported Adreno 830 / API 1.3, with a 3392×2400 window; adapt to actual large-screen dimensions.

## Local artifacts outside git

- Toolchain: `~/dm-work/tools/android`.
- Python environment: `~/dm-work/tools/android-venv`, with lz4, capstone, unicorn, zstandard, and nsz.
- Keys: `~/dm-work/data/private/{prod.keys,title.keys}`, directory 0700 and files 0600.
- Base NSZ, update NSP, and Warlock DLC NSZ extracted to `~/dm-work/data/android-input/`.
- hactool built from upstream at `~/dm-work/tools/hactool/hactool`.
- Tool installation log: `~/dm-work/tools/android-bootstrap.log`.
- Complete RomFS: 46 files / 8,973,547,258 bytes deployed to `/sdcard/Android/data/org.d2r.android.port/files/romfs/`.
- Device boot probe: `/data/local/tmp/d2r-port/`, with `game/`, `temp/`, and isolated `probe-save/`.
- Real host executable: `dm/android/build/host/dmhost-android`, built with `tools/host_link.py`.
- Logs: `~/dm-work/tools/android-{build,tests,host-boot,romfs-push}.log`.

## Implemented and validated

- `dm/android/` with generated outputs and local settings ignored.
- `tools/bootstrap.py`: external installation of NDK r28c, API 35 platform, build-tools 35, ADB, and JDK 17 using ranged downloads. Official tool archive checksums are checked; game checksums are not.
- `patch_nso.py`: opt-in `DM_TARGET=android` obtains dynamic TLS context through a full-register-preserving entry, retaining the default Apple route.
- Opt-in `DM_ALLOW_PREPATCHED=1` accepts exactly the new bytes of the same registered patch; unknown bytes still fail.
- `tools/guest_elf.py`: 16 KB-aligned ELF libraries preserving the rewritten module layout and exporting `dm_guest_image`.
- `native/runtime.[ch]`: dynamic TLS context, module loading/key pages, and four-thread isolation test.
- NativeActivity lifecycle and multitouch wired to the existing pure C touch state machine.
- Actual Vulkan diagnostic renderer and swapchain, not yet game NVN playback.
- Real game module loading and dynamic symbol/relocation inspection, without calling the game entry.
- Reproducible APK build/alignment/signing/install script without Gradle.
- Unicorn equivalence: eight instruction categories with four inputs each, all passing.
- Device isolation test: four threads with 10000 iterations each, passing.
- Device and APK sandbox: rewritten ARM64 instructions executed 1000 times on each of four threads. x18, TLS/TLR, svc, timers, host TP, FP, and NZCV passed.
- Device Vulkan: Adreno 830, API 1.3, 3392×2400 swapchain; more than 120 frames presented.
- Touch input: nonzero stick axes observed, returning to zero on release.
- Actual APK-loaded game modules: main 55,170,816 bytes, sdk 15,578,476 bytes, subsdk0 8,563,932 bytes; 38837 symbols and 116034 relocation requirements.
- The APK inspector found 14 imports without guest definitions among 1326 imports. This does not mean only 14 services remain: SDK-defined platform services also require host replacements.
- All **71/71** upstream host C files compile with Android adapters. The real host, adapters, display/driver glue and decompressor link successfully into an Android executable.
- Under device shell, the real host applied **116034 relocations**, with **264 forced service replacements all referenced**.
- SDK completed nnosInitialize, nninitInitializeSdkModule and nninitStartup; approximately 3720 MB virtual heap configured. This is not a measured resident RAM requirement.
- Global constructors ran: 852 main, 115 sdk, 44 subsdk0. The program entered actual `nnMain`.
- Current boundary: `nvnDeviceGetProcAddress` was requested; the headless probe explicitly aborts with `Android game Vulkan backend is pending`, exit 134. There is no game frame.
- Original audio HLE passed device tests: 48 kHz PCM16, 6-to-2 mixing matrix, actual silent callbacks, released-buffer events, pause/resume and close.
- Original lifecycle HLE passed SIGTERM ordering and its 1.5-second stalled-shutdown fallback.
- An actual write-protection fault validated Linux ucontext PC/SP, ESR WnR and resumed execution.
- Portable MD5/SHA1/SHA256 passed 36 reference comparisons against Python hashlib.
- `tools/test.py` is the unified repeatable test entry point.

The initial APK is installed and running. The game is not running; the screen explicitly says GAME BACKEND PENDING.

## Decisions

- Reuse game features, asset tools, pure C touch state machine, and NVN command records; add Android platform support and Vulkan execution.
- Avoid Bionic private pthread storage. Use dynamic ELF TLS with a full-register-preserving context entry. The original initial-exec proposal was discarded because JNI/dlopen libraries cannot rely on surplus static TLS. Key pages hold offsets within the context plus its entry address.
- Rewrite game code on the computer and package it into ELF libraries inside the APK; never modify guest instructions at runtime.
- Feed existing SPIR-V and reflection into Vulkan; the Metal execution layer cannot be reused directly.
- Keep keys and game-generated artifacts outside git. Report unsupported services explicitly instead of claiming fake game startup.

## In progress and next steps

1. Tool installation and game extraction are complete. Game input is at `~/dm-work/data/switch/3.3.94012/Update/{ExeFS,RomFS}`.
2. All three Android rewritten modules and ELF libraries are in `~/dm-work/build/android/game/`.
3. APK: `dm/android/build/D2R-Android-port.apk`, package `org.d2r.android.port`.
4. **Current priority: complete NVN-to-Vulkan playback**, reusing hle_nvn.c/nvn_obj.h and replacing the approximately 5900-line Metal execution layer. frontend_headless.c deliberately aborts at nvnBootstrapLoader and is not a game backend.
5. Prepare .NET 10 shader tools and SPIR-V caches; game shaders and Android texture adaptation are not prepared yet.
6. Integrate the real host into Activity, replacing diagnostic rendering and connecting resources, actual game input, UI and overlays. The APK currently inspects modules/tests subsystems; actual SDK boot remains a separate shell probe.
7. Validate game frames, menus, gameplay, real saves, background recovery and sustained performance.

## Resolved issues

- Restore NDK archive symlinks; plain-text extracted symlinks broke clang.
- Add valid ELF section headers required by Android's linker.
- Correct virtual x18 use in guest TLS writes and handle reads targeting x18.
- Expand fixed br-x18 landing-pad space for the longer Android context entry.
- Treat hactool's explicit errors as extraction failures even when it returns zero.
- Use a local minimal `hactool.keys` to avoid old hactool rejecting unrelated newer key fields.
- Pass encrypted ticket title keys to hactool; it performs the decryption itself.
- Extraction now automatically writes a minimal private keyset and checks that outputs were actually produced; no game version/digest validation.
- HLE registry uses retained ELF sections. Device registration passed with --gc-sections enabled.
- dm_ucontext.h handles the distinct Linux signal context layout; actual fault validation passed.
- An initial probe with incomplete resources and missing display/driver glue returned from nnMain. After adding gfx/hle_nv.c/hle_vi.c and deploying complete resources, the program explicitly reached NVN. Do not interpret the initial return as successful gameplay.

## Resume

```bash
cd <LOCAL_WORKTREE>
git status --short
tail -30 ~/dm-work/tools/android-bootstrap.log
python3 dm/android/tools/bootstrap.py
~/dm-work/tools/android/platform-tools/adb devices -l
~/dm-work/tools/android/platform-tools/adb mdns services
source ~/dm-work/tools/android-venv/bin/activate
python dm/android/tests/test_rewriter.py
python dm/android/tools/test.py
python dm/android/tools/host_compile.py --compat
python dm/android/tools/host_link.py
python dm/android/tools/build.py --game-source ~/dm-work/build/android/game --install
~/dm-work/tools/android/platform-tools/adb logcat -d -s D2RAndroid:I AndroidRuntime:E
```

Actual boot probe after deployment; the current graphics-boundary failure is expected:

```bash
~/dm-work/tools/android/platform-tools/adb shell \
  'DM_ROMFS=/sdcard/Android/data/org.d2r.android.port/files/romfs DM_SAVE=/data/local/tmp/d2r-port/probe-save DM_TEMP=/data/local/tmp/d2r-port/temp DM_BOOTPROF=1 DM_TACT_PREFETCH=0 DM_DBG=1 timeout 30 /data/local/tmp/d2r-port/dmhost-android /data/local/tmp/d2r-port/game main sdk subsdk0'
```

## Risks and limits

- Approximately 5900 lines of Metal execution code; Vulkan memory, barriers, synchronization, and textures dominate the work.
- Existing rewriting uses below-stack scratch storage in some paths; Android signal/call boundaries need actual validation.
- Full-register context entry is tested but expensive; actual game performance is unknown.
- The 4 KB device passed. ELF 16 KB alignment is configured, but there is no 16 KB device result.
- Runtime/code execution, save transactions and safe memory reads passed inside the APK sandbox. Actual SDK boot has not yet been tested inside the APK.
- Cross-thread suspend/register diagnostics explicitly return unsupported. Audio route disconnect/recreation is untested.
- Changes are uncommitted in the worktree; inspect git status before continuing and preserve existing work.
- No game execution, frame-rate, or stability result. Pairing, compiling, and clearing a Vulkan surface do not prove game execution.

## Phase 2: NVN → Vulkan (in progress)

- Continued at the user's request. Removed the deliberate headless `nvnBootstrapLoader` abort and connected the original 523 NVN thunks, ELF implementation registry and command recorder.
- Added `dm/android/native/nvn_vulkan.c`: real Vulkan device/queue, pool bounds, GPU buffer copies, completion-side events/timestamps, fence waits, CALL/JUMP/update/barrier replay. Initial synchronous commits favor correctness. Unsupported draw commands still fail explicitly.
- The original host plus NVN interface and Vulkan executor links successfully with the NDK.
- `dm/android/tests/vulkan_test.c` passed on desktop Mesa llvmpipe software Vulkan: deferred data/event visibility, overlapping write/read dependencies, nested commands, fences and pool bounds. This is not Adreno device validation.
- The original wireless-debugging endpoint refuses connections; no ADB device is connected. Requested its current endpoint and continued implementation locally.
- Installing .NET 10 for the existing Maxwell → SPIR-V translator. Texture, clear, graphics and compute pipeline implementation is continuing.

### First-frame priority

The device reconnected on mDNS port 34951. Buffer/event/fence tests now pass on Adreno 830; BC and ASTC are supported. All 6927 shader jobs from 162 CASC packages plus embedded graphics shaders were translated to SPIR-V, with JSON and binary metadata, and deployed to `/data/local/tmp/d2r-port/shaders`. Optimizer failures retried successfully with DebugMode.

The Vulkan executor now includes texture creation/deswizzling/uploads/readback, clears, buffer-to-texture copies, descriptor binding, compute and graphics pipelines, indexed draws, depth-only passes and occlusion queries. Stencil state is being connected. The real game executes skin preintegration and several graphics draws but has not yet presented a complete frame. Per-draw synchronous recycling favors initial correctness over performance.

An accurately symbolized startup crash was in `clone_node -> mount_ex -> MountSave`: the recursive clone carried a 64 KB buffer on each frame, overflowing small guest worker stacks. Buffers and child paths were moved to the heap, and subsequent device boots passed that point. The earlier driver-initialization diagnosis was a hypothesis, not the actual cause of this crash.

Use `DM_VK_SHADERS=/data/local/tmp/d2r-port/shaders DM_VK_CAPTURE=/data/local/tmp/d2r-port/captures` on the existing boot command. Log: `~/dm-work/tools/android-first-draw.log`. Android NativeWindow presentation and pixel-level regression checks remain outstanding. The installed APK still runs the earlier diagnostic frontend.

### First real game image displayed in the Android app

`frontend_activity.c` now launches the original host and nnMain. The installed APK uses the real game runner, with Vulkan Android surface/swapchain, acquisition/presentation semaphores, layouts and aspect-preserving game-texture blits. A negative Vulkan viewport corrected the Metal/Vulkan clip-Y difference: prior captures were black; subsequent frames show the original flaming D2R loading logo. Actual screen: `dm/android/build/device-game-live.png`; GPU readback: `dm/android/build/game-first-frame.png`. These are rendered game frames, not a screenshot substituted for execution. The main menu has not yet been reached and gameplay is not established.

All 6931 shaders, including four embedded counter compute kernels, are translated. Adreno pixel tests passed texture deswizzling/sampling, exact triangle RGBA, 256 occlusion samples, compute image writes and readback. The phase-two regression suite, including a 64 KB guest-stack save-copy regression, passed (`~/dm-work/tools/android-tests-phase2.log`). Subsequent copy/dispatch/draw synchronization changes require another GPU check.

Bindings currently snapshot CPU memory. Synchronous boundaries before draws/dispatches and after dispatches publish earlier uniform updates and SSBO writes; persistent Vulkan buffers/asynchronous scheduling are not implemented. APK: `dm/android/build/D2R-Android-port.apk`, about 81 MiB, locally includes game code and remains ignored by git. Build/install with `android-venv/bin/python dm/android/tools/build.py --game-run --game-source ~/dm-work/build/android/game --install`.

App log is private `files/game.log`, readable with `adb shell run-as org.d2r.android.port tail -100 files/game.log`. First-frame controls support tap-to-confirm, native touch and directional/confirm keys. Full virtual controls and soft keyboard remain pending. Window destruction gracefully exits; process-preserving surface recreation is not implemented.

### Hero-class selection reached (latest acceptance point)

The real game naturally closed `MainMenuLoadingPanel`, created `MainMenuButtonRibbon`, and accepted A input to enter `CharacterCreatePanel` / `Hover Barbarian`. No loading-overlay hiding or pre-rendered screenshot was used. Actual image: `dm/android/build/device-hero-select.png`, showing Chinese class-selection text, description, A/B prompts and 3D silhouettes. The user's immediate first-picture request is achieved.

Startup loading takes roughly 165–400 seconds and needs further diagnosis. INTERNET permission was added for existing game network calls; no account data was submitted manually. The initial class-scene `missing vertex buffer 4` failure was caused by binding stale attributes unused by the current shader. SPIR-V Input/Location reflection now selects actual inputs, and the class scene passed that point.

Driver pipeline cache and fixed-state graphics-pipeline reuse were added, with dynamic viewport/scissor/blend constants/stencil masks/reference. Latest source was built and installed. All phase-two regressions passed (`~/dm-work/tools/android-tests-phase2-final.log`). The previous version ran loading at ~5.6 FPS and the class scene at ~4.3 seconds/frame. Full scene performance with the latest reuse cache remains unmeasured. Lighting is dark; full gameplay, name-entry keyboard, full touch controls, surface recreation and remaining GPU operations are not complete.

No user character was created/deleted, original game archives were not changed, and keys were not logged or packaged. All source changes are uncommitted; artifacts and game caches remain ignored. Resume from `git status`, this section, and `adb shell run-as org.d2r.android.port tail -100 files/game.log`. Use `~/dm-work/tools/android-venv/bin/python`; system Python lacks lz4 for probe/rewrite scripts.

### Phase three: device performance work (in progress)

The user requested continued porting. ADB shell verified OPD2409 / Android 16 online at the current mDNS port `<设备地址>`. Before optimization, 300 class-scene frames had p50 4339 ms (~0.23 FPS); screenshot `dm/android/build/device-before-opt.png`.

Completed GPU buffers are now recycled within a 64 MiB cache. Draws batch up to 32 draws or 32 MiB of temporary buffers. Known copy inputs are forwarded to later CPU snapshots while guest memory is published only after GPU completion. Unknown SSBO writes and events/counters retain completion boundaries. Occlusion resets are queued in command order; empty draws no longer allocate zero-length index buffers.

All device regressions passed (`~/dm-work/tools/android-tests-batching-final.log`). Added exact red/green uniform pixels, deferred copy visibility, copy-to-texture sampling, consecutive SSBO dispatch visibility and 128-sample reset/report checks. 96 draws required 3 submissions and about 28 ms. The new APK is installed. Loading's first 300 frames had p50 32.68 ms (~30.6 FPS); steady complex class-scene performance remains to be measured. Repeated vertex snapshot copying and lighting are under investigation.

First-picture APK and backend sources are backed up under `~/dm-work/build/android/checkpoints/` (ignored/outside git). Test shader deployment now pushes `source/.` into `destination/`, fixing nested directories on repeated runs.

The batched version's full class scene measured p50 1005.42 ms across 300 frames (~0.99 FPS), about 4.3 times faster than the original 4339 ms. That interval read ~192 GB of snapshots, identifying repeated vertex copies as the next bottleneck. A 128 MiB read-only vertex snapshot cache now checks actual dirty pages/pending copies and purges on pool destruction; retired buffers wait for GPU completion. Device vertex-pixel, dirty-invalidation and pool-destruction tests passed (`~/dm-work/tools/android-tests-vertex-cache.log`). The cache version is being deployed for steady-scene measurements.

### Native text entry and further resource reuse (in progress)

ADB recovered at port `<设备地址>`; shell commands work. `D2RActivity.java` bridges the original swkbd to a system text dialog with stock localized OK/Cancel, game/existing dm_ui text, Unicode conversion, length/name-byte/numeric validation. Java/dex build passes without Gradle. Real-game component is now `org.d2r.android.port/.D2RActivity`; diagnostic mode retains NativeActivity.

The first real naming request aborted ART's `FindStackTop > GetStackEnd` check: the guest stack falls outside its host pthread's registered stack. JNI now runs on a separate standard pthread and the guest waits for the result. This is being deployed for validation; that crash was not a normal background exit.

Read-only SSBO/texel resources now share dirty-page snapshot caching (256 MiB cap). Writable texel buffers publish real GPU readback. All new device pixel/SSBO/texel-to-uniform checks pass (`~/dm-work/tools/android-tests-read-cache-final.log`). Complex-scene snapshot traffic dropped from ~900 MiB/frame to ~32 MiB/frame, but the previous version still takes ~1 second/frame. Persistent image views and per-batch reusable descriptor pools also passed device pixel regressions and are being deployed for evaluation.

The Chinese name dialog and system IME actually appeared (`dm/android/build/device-native-keyboard.png`); entering one `A` disables confirmation, verified in UI XML. The first confirmation hit UnsatisfiedLinkError because NativeActivity's dlopen does not register the library with the Java class loader. ANativeActivity_onCreate now explicitly RegisterNatives; the updated APK is installed, confirmation/cancellation remain under verification, and no character has yet been successfully created.

Occlusion queries now use a reusable batch pool and one result read, retaining ordered resets/reports; on-device regression is running. Coherent CPU buffers prefer HOST_CACHED memory (Adreno exposes type 6 with flags 0xf); all prior GPU pixel/visibility checks passed with this choice.

### Act I camp actually entered (latest acceptance point)

Explicit JNI registration passed confirmation and returned the ASCII name `AndroidProbe`. The original game created its .key/.map/.ma4 and shared stash data, closed its original LoadScreenPanel, and entered area 1. No modified game logic fabricated entry. Screenshot `dm/android/build/device-first-town.png` shows real camp/character geometry, life 55/55, mana 10/10 and skill/potion UI. Lighting remains too dark and frames take ~0.5 seconds (~2 FPS); smooth/full gameplay is not established.

The independent character lives in private `files/save/AndroidProbe/`; no existing character was overwritten. Log: `~/dm-work/tools/android-game-first-town.log`. ASCII keyboard/confirmation and rejection of a one-letter name are verified. Cancellation, Chinese/numeric entry and save reload remain to verify. First-town APK backup: `~/dm-work/build/android/checkpoints/first-town.apk`. Source has further reusable occlusion queries and immutable support/c1 buffers under device regression, still needing build/install.

### Requested on-screen diagnostics (under verification)

The user requested FPS and further debug information. Added bilingual table keys/generated header for smoothed FPS/frame time, actual NVN-submit CPU time, draws/submits/snapshot traffic per frame, buffer capacity, process RSS and original game's stage/area/cell coordinates/input samples. Mutex snapshots protect cross-thread reads; no estimated GPU time is shown. A normal host pthread updates Java UI every 0.5 seconds. Vulkan overwrote ordinary addContentView text, so the overlay now uses a non-touchable/non-focusable app PopupWindow; visibility is being checked on the device.

Added WASD left-stick movement, all controller buttons/four axes, retained key chords and background input clearing. Complete virtual touch overlays remain missing. Reusable occlusion pools and immutable support/c1 buffers passed all device regressions (`~/dm-work/tools/android-tests-debug-hud.log`).

### CPU sampling identifies cache lookup cost

A 12-second on-device simpleperf run collected 11030 samples with no loss (`~/dm-work/tools/android-cpu-profile.txt`; device data `/data/local/tmp/d2r-port/android-cpu-profile.data`). Use shell `simpleperf record --app ... -o /data/local/tmp/...`; direct run-as cannot set perf properties and the wrapper cannot initially open private-app output. User-space cycles: snapshot_buffer 39.89%, image_get 18.03%, dynamic context gate 14.31%, sha256_block 6.58%.

Read snapshots now use an 8192-bucket hash plus doubly linked LRU; images use 4096 buckets; shader resolution caches program address and real ProgramSetShaders serial, handling reused control addresses correctly. Dirty writes, copy overlays, pool destruction and shader replacement still invalidate correctly. Device collision/dirty/pool/program-serial tests are running before deployment/performance comparison.

The debug PopupWindow is actually visible (`device-debug-popup.png`, `device-debug-current.png`) and non-touchable. AndroidProbe appears after APK updates; save reload/movement/lighting validation continues.

### Cache indexing deployed and measured

All regressions passed, including real GPU pixels/visibility, hash collisions, dirty writes/pool destruction and program-serial reuse (`~/dm-work/tools/android-tests-cache-index.log`). Build/install succeeded (`~/dm-work/tools/android-install-cache-index.log`); `git diff --check` passed.

In the existing AndroidProbe character-selection scene, the earlier HUD showed ~1.61 FPS / 631 ms / submit-thread CPU 472 ms. The indexed version sustained ~2.50–2.64 FPS across multiple 60-frame reports; 300-frame p50 was 396.76 ms and average submit CPU 235.899 ms. Screenshot `dm/android/build/device-debug-cache-index.png` shows smoothed 2.71 FPS / 376.7 ms / CPU 216.4 ms. This is not a same-scene camp comparison or evidence of smooth gameplay. Per frame: ~1021 draws, 37 submissions, 24.3 MiB snapshots. GPU time remains unmeasured; wall-time minus CPU includes scheduling/waits and is not a GPU measurement.

A second 11.965-second simpleperf capture collected 11189 samples without loss (`~/dm-work/tools/android-cpu-profile-index.txt`). snapshot_buffer fell from 39.89% to 3.27%; image_get/sha256 are below the report's 1% threshold. Current context entry is 36.76%, memset 7.13%, bind_shader 5.14%, image_upload 4.74%. These are shares of sampled user-space CPU cycles, not wall-clock frame percentages. Current game log: `~/dm-work/tools/android-game-cache-index.log`.

Next performance targets are context-gate/TLS call frequency and remaining draw/submission costs; full register preservation, Android x18 and thread-context correctness must remain intact. Lighting, complete touch controls and actual save reload followed by movement still need work.

### Parallel original-project/Eden review and first implementation

The user requested latest Eden source and parallel agents. Three agents studied the original Metal/NVN backend, Eden Android/Vulkan, and context entry. Consolidated report: [Android GPU comparison (Chinese)](ANDROID_GPU_REVIEW.zh-CN.md). Eden is cloned at `~/dm-work/research/eden`, master `a594c8d60ef79280adc046760561912207e16e17`, verified against the remote. The original project was fetched to `origin/main=685d3ddef8bf75768fd4c51228566f2e8d2a5182` for read-only comparison, without merging/resetting the modified port.

Vulkan GPU rendering/compute/copies are already active. Useful reference patterns are asynchronous completion ticks, stream/staging buffers, pass/object reuse and explicit GPU/CPU data ownership; Eden's full emulator backend cannot directly replace this NVN interceptor.

Implemented a hidden ELF TLSDESC context entry (25 instructions, 64-byte wrapper stack), preserving all registers/state except output x16 and leaving Android x18/TP intact. The full syscall/observation dm_gate remains unchanged. Compute pipeline/layout now persist for shader lifetime. Corrected SSBO Write bit to 1 and fixtures to actual flags=1. Separated unbound writable images from sampled dummy textures; the new pixel test failed before the fix and passed afterward on host Vulkan and Adreno. Dirty snapshots now protect before generation/read, matching the original backend.

Added genuine GPU timestamps/fence-wait metrics: the device exposes 48 valid bits and 52.083332 ns/tick. GPU spans sum batch intervals, exclude presentation/vsync and are not utilization; CPU/GPU/wait cannot be added. Complete device regression passed (`~/dm-work/tools/android-tests-eden-final.log`), including dlopen cold/warm dynamic TLS, four threads x 1001 full-register/isolation checks, GPU visibility/dummy isolation/timestamps. Build/install succeeded (`~/dm-work/tools/android-install-eden-review.log`); diff whitespace check passed.

The first launch after install encountered an asleep/locked device and orderly window-destroy shutdown; sent Wakeup/system dismiss-keyguard and restarted. This was not classified as a new context crash. Complex-scene FPS and lighting on this version still need live measurement; entry instruction count is not an FPS prediction. Full notes remain under `~/dm-work/research/`. Eden APK and Turnip were not installed, and no artifacts were published.

### Final device measurement and conditional FP-state restore

The new runner actually rendered AndroidProbe selection again. Its first version sustained ~2.38–2.44 FPS, ~121 ms GPU batch spans and ~150 ms fence waits per frame. A 12-second profile still assigned 30.93% user-space cycles to context entry; sample addresses were verified against its exact 100-byte ELF range. Most entry cost was in FPCR/FPSR restoration, rather than a misattributed following NVN function.

The entry now compares FPCR/FPSR and restores only changed values, always restoring NZCV. It has 31 static instructions, typically 29 executed, with the same 64-byte wrapper stack. Twenty emulated resolver/thread scenarios verify both unchanged and individually changed FP state and actual write traces. Added a deterministic guest write immediately after snapshot read; an external old-order backend failed its next-cache-hit assertion, while the corrected version passed on host Vulkan and Adreno.

Final complete regression `~/dm-work/tools/android-tests-context-conditional.log` passed; build/install `~/dm-work/tools/android-install-context-conditional.log` succeeded and whitespace checks passed. Screenshot `dm/android/build/device-debug-context-conditional.png`: 2.51 FPS / frame 403.7 ms / CPU 238.3 ms / GPU 122.8 ms / wait 151.1 ms. Its 60-frame mean is 2.43 FPS, GPU 122.84 ms, wait 151.70 ms. There is no clear overall FPS improvement yet; lighting remains dark.

Final CPU profile (11.971 seconds, 8915 samples, none lost) reduces entry share to 11.79% (`~/dm-work/tools/android-cpu-profile-conditional.txt`). This does not predict frame-wall-time improvement. Current game log is saved as `~/dm-work/tools/android-game-context-conditional.log`; previous iteration is preserved in `android-game-eden-review.log`. GPU is genuinely active. Next targets are graphics object/parameter reuse, compatible pass merging and asynchronous retirement based on real GPU completion.

### Explicit real-device 60 FPS target: pass/parameter restructuring

The user now requires 60 FPS (16.7 ms/frame) in the real game. Three agents implemented pass/object reuse, uniform arena/descriptor batching, and visibility redesign analysis. Added draw/pass/uniform includes. Same-attachment draws retain a renderpass; layouts/renderpasses/framebuffers persist; graphics pipelines use a hash with full attachment-slot mapping. Queries accumulate per real pass with reset/report boundaries. Uniform arenas retain independent 64 KiB versions/zero padding, clearing only written prefixes; c0/c2 reuse and per-stage descriptor updates reduce CPU work. Compute c0 UBO pointer tables are verified by a GPU fixture.

Image hazards use actual stage/access; consecutive reads avoid redundant full barriers. Draw cap increased from 32 to 256, retaining 32 MiB temporary-memory and unknown-write/event/counter boundaries. Pending copies have a separate list. Known CPU-copy input snapshots publish only after actual GPU completion, without redundant temporary GPU buffer copies.

Complete device regression (`~/dm-work/tools/android-tests-passes-arena.log`) passed: 96 draws use one submit/pass/query, 257 use two batches with object reuse, and all pixel/query/dirty-window/SSBO/arena-padding/version checks pass. Build/install succeeded (`android-install-passes-arena.log`). Previous APK is backed up at `~/dm-work/build/android/checkpoints/2.4fps-before-passes.apk`.

Same character-selection scene now sustains ~6.97–7.18 FPS, 300-frame p50 141.81 ms, average submit CPU 82.537 ms; GPU ~32.1–32.8 ms, fence waits ~39.8–40.6 ms/frame. Still 1021 draws, but ~51 passes/154 image barriers/12 submits per frame; arena peak 149 MiB. This is ~3x faster, far below 60 FPS; lighting is still dark and the task continues. Log: `~/dm-work/tools/android-game-passes-arena.log`; screenshot: `dm/android/build/device-debug-passes-arena.png`.
