# D2R Android

基于 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的 Android ARM64 移植工作快照，包含原始项目源码及 Android 宿主、Vulkan 渲染、音频、触控和构建工具。

English: [README.md](README.md)

## 当前状态（2026-10-16）

- 原游戏 ARM64 程序运行于 Android 宿主，图形通过 NVN → Vulkan 在 GPU 执行。
- 已具备 AAudio 声音、系统键盘命名、虚拟摇杆与按键、手柄输入、FPS/CPU/GPU 调试浮层。
- Android 16 / OPD2409 / Adreno 830：营地静止实测平均约 60 FPS；1800 帧中没有超过 33 毫秒的帧。移动到新场景仍可能因首次管线编译卡顿。
- Android 16 / vivo V2505A / Adreno 840：已实际进入野外，短窗口约 59 FPS；已修复底图旋转和手机触控尺寸。不能据此宣称所有场景长期稳定 60 FPS。
- 最新源码含跨启动 Vulkan 管线缓存：正常构建通过，首次运行已写出缓存；**重启加载及性能收益尚未完成验证**。

性能开发仍在继续：营地已接近稳定 60 FPS，当前主要缺口是首次管线编译卡顿。本仓库是持续更新的源码快照，不是完成全流程验证的正式发行版。

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
