# D2R Android

Android ARM64 port snapshot based on [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), including the upstream sources and the Android host, Vulkan backend, audio, touch controls, and build tools.

中文及完整状态：[README.zh-CN.md](README.zh-CN.md)

## Status — 2026-10-16

The game runs with Vulkan rendering, AAudio, native text entry and touch controls. Adreno 830 camp measurements average approximately 60 FPS: of 151 consecutive 120-frame windows covering standing and walking, 142 had no frame over 33 ms. The other 9 line up with loading the save (worst 200.9 ms), one submission-queue wait (92.6 ms) and first visits to new ground (102.6 / 101.2 ms). An Adreno 840 phone has reached actual outdoor gameplay at approximately 59 FPS in a short observation, with landscape rendering and scaled controls verified; the current commit has not been installed on that phone. These observations do not establish sustained 60 FPS across all gameplay.

Cross-launch pipeline warm-up is implemented and verified on device (`dm/android/native/nvn_vulkan_warm.inc`): every genuine graphics pipeline creation is recorded to a private manifest and rebuilt on a background thread on the next launch. All 127 records were rebuilt in 447.7 ms, the driver pipeline cache reloaded successfully (`load=accepted bytes=1739369`), and the 30 menu pipeline creations fell from 826 ms before the fix to 1.1 ms.

The remaining gap is first use in ground the player has not visited before, where the shader combinations used there still compile on demand (85.1 / 85.3 ms measured for single creations); the same route walked back stayed under 29.4 ms. Compute pipelines are not recorded yet and no seed manifest ships with the build.

This is a source snapshot, not a fully validated release. No game executables, RomFS, keys, saves, translated game shaders, drivers or APKs are distributed here.

- [Android port progress (resume entry point, maintained)](docs/ANDROID_PORT_PROGRESS.zh-CN.md) — the English [ANDROID_PORT_PROGRESS.md](docs/ANDROID_PORT_PROGRESS.md) holds the current-state summary plus older, unmaintained history.
- [Android build and deployment](docs/ANDROID_BUILD.md)
- [Snapshot status and limitations](docs/ANDROID_RELEASE_SNAPSHOT.zh-CN.md)
- [Source provenance](docs/ANDROID_SOURCE_PROVENANCE.zh-CN.md)
- [Original upstream README](docs/UPSTREAM_README.md)
- [MIT license](LICENSE); retain third-party notices in their respective directories.

Upstream baseline: `2eb5758771f110cd53082b999d88d242766610da`. Optimization references include dodola/d2r-mobile at `642480b7606cf4adda30bcf009eb56f87745c908`; this port retains its own Android backend. Historical development notes refer to local evidence not shipped with this source archive.
