# D2R Android

基于 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的 Android ARM64 移植工作快照，包含原始项目源码及 Android 宿主、Vulkan 渲染、音频、触控和构建工具。

English: [README.md](README.md)

## 当前状态（2026-10-16）

- 原游戏 ARM64 程序运行于 Android 宿主，图形通过 NVN → Vulkan 在 GPU 执行。
- 已具备 AAudio 声音、系统键盘命名、虚拟摇杆与按键、手柄输入、FPS/CPU/GPU 调试浮层。
- Android 16 / OPD2409 / Adreno 830：营地平均约 60 FPS。营地段（含走动）151 个 120 帧窗口里，142 个窗口一帧都没超过 33 毫秒；其余 9 个分别对应进营地读盘（最差 200.9 毫秒）、一次提交队列等待（92.6 毫秒）和首次踏入新区域（102.6 / 101.2 毫秒）。
- 跨启动管线预热已实现并真机验证（`dm/android/native/nvn_vulkan_warm.inc`）：每次真实创建图形管线都记入应用私有清单，下次启动在后台重建。实测后台建完 127 条用时 447.7 毫秒，驱动管线缓存重启加载成功（`load=accepted bytes=1739369`），菜单处 30 条管线创建从修复前的 826 毫秒降到 1.1 毫秒。
- 仍存在的缺口：**第一次踏入没去过的地面**时，该处用到的着色器组合仍会现编（实测单次 85.1 / 85.3 毫秒）；同样一段路原路返回不超过 29.4 毫秒。计算管线尚未纳入录制，种子清单尚未随包发布。
- Android 16 / vivo V2505A / Adreno 840：已实际进入野外，短窗口约 59 FPS；已修复底图旋转和手机触控尺寸。不能据此宣称所有场景长期稳定 60 FPS；当前提交尚未在该机安装验证。

性能开发仍在继续：营地已接近稳定 60 FPS，缺口集中在“首次踏入新区域时的一次性编译”和偶发长帧。本仓库是持续更新的源码快照，不是完成全流程验证的正式发行版。

## 使用与构建

见 [Android 构建与部署](docs/ANDROID_BUILD.zh-CN.md)。需要 Linux x86_64 构建环境、Android ARM64 设备，以及自行准备的游戏资源和派生模块。

仓库不包含游戏程序、RomFS、密钥、存档、翻译后的游戏着色器、预编译驱动或 APK。真实游戏 APK 会打包本机派生的游戏模块，因此不作为本次发布附件。

## 阅读入口

- [Android 移植进度（继续开发的入口）](docs/ANDROID_PORT_PROGRESS.zh-CN.md)
- [冻结状态、实测范围与已知问题](docs/ANDROID_RELEASE_SNAPSHOT.zh-CN.md)
- [代码来源与借鉴](docs/ANDROID_SOURCE_PROVENANCE.zh-CN.md)
- [Android 构建说明](docs/ANDROID_BUILD.zh-CN.md)
- [性能开发记录](docs/ANDROID_FPS_OPTIMIZATION_20261016.zh-CN.md)
- [上游项目原始说明](docs/UPSTREAM_README.zh-CN.md)

历史开发记录含本地证据位置；这些日志、截图和外部研究目录不包含在本源码仓库中。当前状态以[移植进度](docs/ANDROID_PORT_PROGRESS.zh-CN.md)为准，历史记录中的旧问题/旧成绩不代表现状。

## 来源与许可

保留原项目 [MIT 许可证](LICENSE) 及第三方许可。上游基线为 `2eb5758771f110cd53082b999d88d242766610da`。Android 优化参考了 [dodola/d2r-mobile](https://github.com/dodola/d2r-mobile) 的固定提交 `642480b7606cf4adda30bcf009eb56f87745c908`，以及 Eden / LASRecomp 的设计；具体采用范围见来源说明，不以共用代码行数推算贡献比例。
