# Android Mali 帧率：2026-10-20 每帧一次整帧排空

**结论先说**：Mali 营地只有 20 多 FPS 的串行点，是一条**只在「声明 64 位 `timestampValidBits` 的设备」上被关闭的快路**——NVN timestamp counter 的 GPU 编码能力判据把这类设备整体挡在外面，于是每帧一次 `commit → wait(UINT64_MAX) → begin` 整帧排空，帧时变成 CPU 与 GPU 相加。放宽判据并把它挪到启动期预建后，同机同设置营地静止 **27.0–28.1 → 38.3–41.0 FPS**，`over33ms` 由 **81–116/120 降到 2–3/120**。

## 前提：README 的 60 FPS 与 Mali 不同源

- 60 FPS 是 OPD2409 / Adreno 830 + 随包 Turnip（Mesa 的 Adreno 驱动）的实测；文档**从未**测过 Mali，且明文「不能直接套用另一台设备的测量结果」（[冻结快照](ANDROID_RELEASE_SNAPSHOT.zh-CN.md)）。
- 同一份 APK 在 Adreno 手机上改走系统驱动，营地也只有 20.5 FPS / 48.8 ms（[驱动说明](ANDROID_GPU_DRIVER.zh-CN.md)）。也就是说「20 出头」正是这条移植**没有 Turnip 时的历史水位**，Mali 本次修前测到的 25–28 并不异常。

## 排查：先分账，再逐个否证

设备 vivo V2417A / MT6989 / Mali-G720-Immortalis MC12，系统驱动 `driver_id=9`，`BC=0 ASTC=1`，`subgroup=16`，主机档 1600×900，浮层开。

| 假设 | 判据字段 | 实测 | 结论 |
| --- | --- | --- | --- |
| ASTC→RGBA8 转码是热路径 | `astc_to_rgba_copy` | 只有零星 64–256 小图 | 否证 |
| 着色器/管线编译 | `pipeline-cache load=accepted`、长帧时新建管线计数 | `accepted`、长帧时新建 0 条 | 否证 |
| present 处 `UINT64_MAX` 等待（`nvn_vulkan.c:930`） | `[vulkan-frame-stability] prev_acquire`、`wait_prior` | 两者恒 0.000 ms | 否证 |
| 数据包/驱动配置漏做 | 包 `manifest.json`、`lib/` | ASTC 补充档与 subgroup16 齐、profile matches、Mali 本就不该有 `vulkan.purple.so` | 排除 |
| **帧内依赖排空** | `[vulkan-dependency-time] reason=ssbo` | **calls=1.000/帧、wait=14.63 ms/帧** | **命中** |

明细行直接点名了资源：`[vulkan-dependency-detail] reason=ssbo shader=16479cc6d75c2b28 stage=5 binding=1 bytes=64 forward=host_effect host_kind=2` —— 一个 compute 的 64 字节 SSBO，与一条 16 字节的 counter 写回重叠。

## 根因链

1. `mtl_sub_counter()`（`nvn_vulkan.c:780`）为 NVN timestamp counter 登记 16 字节写回：`post_new(2)` → `pending_host_sync=1`，范围按 `DM_PENDING_HOST_EFFECT` 入索引（`nvn_vulkan_async.inc:112`）。
2. 若这 16 字节已在 GPU 上编好（`counter_gpu_encoded=1`），组合转发会把它叠到消费者的基线缓冲上（`nvn_vulkan_forward_compose.inc:14`），**不需要**等批次完成。
3. 能力判据原文是 `gpu_timestamp_bits+26<=64+shift`。Mali 声明 64 位、`timestampPeriod=76.923080 ns`（float32 指数 133 → `shift=15`）→ `90<=79` 为假 → `supported=0`。
4. 于是每帧 4 个 counter 全走 CPU 回退（批次完成后才把值 `memcpy` 进客体内存）→ compose 对未编码的 kind==2 返回 -1 → `dependencies_gpu_unknown_reason` 判定不可转发 → `dependencies_flush` 整帧排空（`nvn_vulkan_dependencies.inc:83-91`）。
5. 帧时 = CPU(15–19) + GPU(20) ≈ 35–39 ms。Adreno 是 48 位 / 52.0833 ns（换算系数恰为 1.0），旧判据天然通过——**这条快路一直是 Adreno 独占**。

## 判据为什么可以放宽（语义不变）

- 核函数 `shaders/counter_timestamp.comp` 真正算过的中间量：`tick(<2^bits) × 3M(<2^26) < 2^90`，留在 96 位乘积里；基 65536 长除 625 精确（中间值 ≤ 40,959,999）；随后右移 `shift` 取低 64 位。
- 旧判据多出来的一条是「最终值必须装得进 64 位」。但报告字段本身就是 64 位寄存器，CPU 回退用 `uint64` 算术同样回绕（`nvn_vulkan_async.inc:335`），两者语义一致；要触发回绕需要 2^64 个 52.083 ns 单位，即连续运行三万年量级。
- 新判据 `gpu_timestamp_bits+26<=96` 只保留「中间量无损」这一条真约束，不再拿声明位宽当溢出证据。
- **保持不变的安全边界**：`NVN_COUNTER_TYPE_SAMPLES_PASSED` 仍不进 GPU 转发（同文件 `:80-81`，计数必须由 CPU 按序发布）；`mtl_addr` 失败、写回地址未 8 对齐、单批 1024 项查询用满时，仍回到原来的完成等待——不存在「既不编码也不登记 pending」的第三态。

## 与 2026-10-16 Adreno 记录同源

这不是新发明的修法。[2026-10-16 帧率优化记录](ANDROID_FPS_OPTIMIZATION_20261016.zh-CN.md) 已把同一等待定位到「compute 读取 64B 时间戳报告、post kind2/type0、`forward=host_effect`」，其 GPU counter domain 版在 Adreno 上「已消除每帧中途依赖等待，4 报告/帧、CPU fallback 0」。当时实现按 Adreno 的 48 位 / 52.0833 ns 通过判据，**判据本身把 64 位 timestamp 设备整体挡在快路之外**，Mali 因此从未拿到那次修复的收益。

## 改动

- `dm/android/native/nvn_vulkan_counter_gpu.inc:25`：判据换成 `gpu_timestamp_bits+26<=96`，注释写明中间量与寄存器回绕两件事，LOG 增 `product_bits=96 report_field=64bit_wrap`。
- `dm/android/native/nvn_vulkan.c:424`：`counter_gpu_initialize()` 提到设备初始化期调用，与同处 `:413` 的 `astc_copy_init()` 同惯例。理由是审查发现的既存竞态：该函数的 `vkCreate*` 无锁，而 PSO 预热线程不持 `queue_lock` 却在创建同类对象；留着首次计数时初始化，还会把一次性创建塞进游戏某一帧。
- 影响面：Adreno 判据本就通过 → `supported` 与 counter 数值均不变，只少一次游戏中途创建。

## 实测对照（同机、主机档 1600×900、浮层开、Act I 营地静止）

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| fps | 27.0–28.1 | **38.3–41.0**（走动 34–35） |
| 帧间隔 p50 | 35.1–38.7 ms | **24.4–27.4 ms** |
| `over33ms` / 120 帧窗 | 81–116 | **2–3** |
| `over50ms` | 0–2 | 0 |
| GPU | 19.8–20.5 ms | 21.1–22.0 ms |
| `fence_wait` | 16.1–16.4 ms | **2.3–2.6 ms** |
| `reason=ssbo` 排空 | 1.000 次/帧、14.63 ms | **0.000 次/帧、0 ms** |
| `[vulkan-counter-gpu]` | `supported=0 CPU_fallback=4.000` | `supported=1 reports=4.000 CPU_fallback=0` |

- 编码后 GPU 微升 1.7 ms、`thread_cpu` 从 15.5 升到 19–21 ms：每帧多 4 次 dispatch/查询回读与组合拷贝，且 CPU 与 GPU 第一次真正重叠。净收益仍是 +12 FPS。
- `[vulkan-counter-published] ... domain=device GPU_forward=1`，值单调、TOP/BOTTOM 跨度量级合理（48 ms / 32 µs / 379 µs）。
- **验证边界**：单台 Mali、营地静止加短暂走动、无温度匹配 A/B、未跑战斗/传送/首次进入新区域。**不能据此宣称全场景 60 FPS，也不能宣称已验证游戏对 counter 值的全部用法**；帧尾等待本身不能删。

## 审查记录

两路只读子 agent（图形正确性 / 崩溃与资源生命周期）均判「可交付装机实测」，采纳其中一条：初始化期预建，消掉无锁 `vkCreate*` 与预热线程的同类创建竞争。图形审查另指出：CPU 回退先除后乘、核函数先乘后除，同一时刻可差 1 个计数单位——两条路径不会同时生效，本轮未改。生命周期审查确认 `post->output` 由 `forward_hold` 借引用到消费者批次发布才放，无 UAF、无双自由。

## 剩余瓶颈（下一步候选）

帧时已回到 `max(CPU, GPU) + ε ≈ 24 ms`，要进 16 ms 得两边同时降：

- **GPU 21.7 ms**：所有图像固定 `VK_IMAGE_LAYOUT_GENERAL`（Mali 丢 AFBC 快速通路，`nvn_vulkan_tex.inc:62/:110`）；52 通道/帧各带一条 PRECISE 遮挡查询（`nvn_vulkan_pass.inc:97`）+ `VK_QUERY_RESULT_WAIT_BIT` 回读（`nvn_vulkan_async.inc:423`，本次编码又新增 4 次/帧）；无 BC 时残留 BC1/4/5/6H 走 CPU bcdec 解成 RGBA8/RGBA16F（`nvn_bcdec_vk.c:23-33`，带宽 2–8×）；末端每帧对面板原生 2400×1080 整屏 clear + LINEAR 放大（`nvn_vulkan.c:951/958`）。
- **CPU 19–21 ms**：约 930 draw/帧的逐绑定解析（`draw 11.9–12.8`、`bind 5.9–6.4`、`resolve 5.5–6.0`、`ubo 3.7–4.0 ms`），Mali 用户态驱动 per-draw 更贵；vivo 把渲染线程压在 1.6–2.2 GHz、X4@3.4 GHz 全程未用，ADPF 只绑 1 个 tid（`performance_hint.c:29`），Android 上 mach 时限策略为 `KERN_NOT_SUPPORTED`。
- **分辨率**：Android 上动态降档恒失效——`mtl_presented_time()` 恒返回 0（`nvn_vulkan.c:1075`）→ `dm_3d_scale_feed(0)` → 控制器 `iv_ms<=0` 直接 return（`dm/host/hle_oe.c:183`），画质四档里的「自动」永远不会降。零改码可试：掌机档 1280×720（像素 1/1.5625）、性能 0.70、`DM_3D_SCALE`（`dm/host/hle_oe.c:239`，0.5–1.0）。

## 复现与验证

- **看哪些字段**：`[vulkan-dependency-time] reason=* calls=/frame wait=ms/frame`（排空）、`[vulkan-bindings] dependency_wait`、`[vulkan-counter-gpu] supported/reports/CPU_fallback`、`[vulkan-frame-stability] interval/over33ms`。`prev_acquire` 只反映 present 处那个等待，历史上恒 0，别拿它当串行点证据。
- **构建**：`PYTHONUTF8=1 python dm/android/tools/build.py --game-run --no-turnip --no-gpu-extras --keystore dm-work/build/android/debug.keystore --version dev`。`build.py:68` 的 `read_text()` 没有显式编码，Windows 上会按 GBK 去读 UTF-8 的 `counter_timestamp_spv.h` 而抛 `UnicodeDecodeError`（既存工具缺陷，本轮未改，用 `PYTHONUTF8=1` 绕开）。
- **装机**：`dm/android/tools/install_apk.py`（同签名覆盖安装，9.9 GB 数据包与存档保留；装完双端 md5 核对）。
- **进营地路径**：启动界面「▸ 启动游戏」→ 黑屏约 15 s 到英雄选择（能看到角色）→ 点屏幕中心（触屏全部映射为手柄确认，即「进入游戏」）→ 黑屏约 15 s 进营地。
- **取日志**：native 的 `stderr` 不进 logcat，用 `adb exec-out "run-as org.d2r.android.port cat files/game.log"`。本轮两份证据留在 `dm-work/build/logs/android-port-game-log-run20261020-003631.log`（修复前）与 `...-013500-final.log`（修复后）。
