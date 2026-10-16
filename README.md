# D2R Android

Android ARM64 port snapshot based on [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), including the upstream sources and the Android host, Vulkan backend, audio, touch controls, and build tools.

中文及完整状态：[README.zh-CN.md](README.zh-CN.md)

## Status — 2026-10-16

The game runs with Vulkan rendering, AAudio, native text entry and touch controls. Adreno 830 camp measurements average approximately 60 FPS; exploration can still stall on first-time pipeline compilation. An Adreno 840 phone has reached actual outdoor gameplay at approximately 59 FPS in a short observation, with landscape rendering and scaled controls verified. These observations do not establish sustained 60 FPS across all gameplay.

The current source includes persistent Vulkan pipeline caching. The build and first cache save succeeded; restart loading and performance benefits remain unverified. Performance development continues; the known gap is first-time pipeline compilation hitching.

This is a source snapshot, not a fully validated release. No game executables, RomFS, keys, saves, translated game shaders, drivers or APKs are distributed here.

- [Android port progress (resume entry point)](docs/ANDROID_PORT_PROGRESS.md)
- [Android build and deployment](docs/ANDROID_BUILD.md)
- [Snapshot status and limitations](docs/ANDROID_RELEASE_SNAPSHOT.zh-CN.md)
- [Source provenance](docs/ANDROID_SOURCE_PROVENANCE.zh-CN.md)
- [Original upstream README](docs/UPSTREAM_README.md)
- [MIT license](LICENSE); retain third-party notices in their respective directories.

Upstream baseline: `2eb5758771f110cd53082b999d88d242766610da`. Optimization references include dodola/d2r-mobile at `642480b7606cf4adda30bcf009eb56f87745c908`; this port retains its own Android backend. Historical development notes refer to local evidence not shipped with this source archive.
