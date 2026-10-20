# Android Mali：2026-10-20 附件描述记忆与三处渲染修复

**结论先说**：这批改动（提交 `4a851d1`）除了单独成文的 counter 排空修复（见 [ANDROID_MALI_FPS_20261020.zh-CN.md](ANDROID_MALI_FPS_20261020.zh-CN.md)），还处理了四个问题：每绘制重复重建附件描述、布局模式下"同一绘制内既采样又当附件写"的未定义状态、预热线程对稀疏渲染目标的越界读、以及 PSO 清单表头随键布局变化后预热静默失效；另把每帧追加的异常帧日志改为每次启动截断。附件描述记忆在营地稳态命中 **0.96**、`sample_attachment_conflicts=0`，同会话 CPU 方向性下降 0.4–0.6 ms（噪声边缘，不作为独立提速结论）。

## 1. 每绘制重建附件描述（附件记忆表）

**问题**：`draw_graphics_inner` 每次绘制都重算 9 个 `VkAttachmentDescription`、视图与通道/帧查找。营地稳态约 1260–1270 绘制/帧，这 1000 多次重建里绝大多数输入与前一次完全相同——渲染目标（`state.rt`）只在 `OP_SET_RT` 时变。

**修法**：一张按 `state.rt` 字节 + 当前通道数做键的记忆表，命中前逐附件再比三样会在两次绘制之间自己变的东西：

- `serial`：`state.rt` 存的是客体纹理指针，游戏原地重建纹理时指针字节不变而 serial 已变，只比指针会把过期视图当命中；
- `populated`：决定 `loadOp`，上传或清除后会变；
- 冲突钉位 `pin_bits`：同一绘制内该图是否被采样，决定缓存里的 `slot_of` 还成不成立。

命中时直接复用缓存的附件描述、视图、颜色引用与 `slot_of`；未命中才构建并整表写回。

**踩过的坑（审查挡下的正确性事故）**：`populated` 必须在 `image_gpu_written(e)` **之前**快照。写回函数会把 `populated` 置 1，若拿它之后的值入表，首次绘制的 `DONT_CARE` 描述会被标成"已填充"，之后每次命中都重放 `DONT_CARE`、丢掉上一通道写好的内容。代码里用 `populated_now[]` 单独保存描述真正编码的那个值（`nvn_vulkan_draw.inc`）。

**失效路径**：图像销毁必须同时作废记忆表——`graphics_cache_forget_image` 调 `attachment_memo_invalidate()`，否则缓存里的 images/views 指针会指向已销毁的图。

**实测**（营地 区域1/5709,4494，诊断版同源码）：

| 指标 | 值 |
| --- | --- |
| 命中率 | **0.96**（4,962,314 命中 / 197,379 未命中，约 1064 次查找、41 次未命中每帧） |
| 未命中位置 | 与通道切换数吻合（≈54 通道/帧） |
| `sampled_attachment_conflicts` | **0** |
| 画面 | 静止 10 分钟与走位到新区域均无错绘 |
| CPU（同会话、诊断版对比） | `draw` 14.11 vs 14.45/14.68、`thread_cpu` 20.7 vs 21.1，方向一致但在噪声边缘 |

**开关**：默认开，`DM_VK_MEMO=0` 关闭。注意提交版**不打印命中率**——那行 `[vulkan-passes] attachment_memo=…` 属于当轮未提交的诊断改动，记忆本身仍在运行；需要时补一行报告即可。

## 2. 布局冲突：同一绘制内既采样又当附件写

**问题**：这里说的"布局模式"指把所有图像从硬编码 `VK_IMAGE_LAYOUT_GENERAL` 改成按用途选真实布局的改动（`DM_VK_LAYOUT`，默认关）。它有一个正确性洞：同一绘制里一张图既被当采样源、又被当附件写时，描述符里记录的布局与通道开始前那次转换实际选出的布局可能不一致，属未定义行为。

**实测规模**：开启布局模式的一次运行里累计 **31394 次**（≈8.6 次/帧）——是常态，不是边角。截图未见错绘，但不能把"看起来正常"当安全依据。

**修法**：`attachment_slot_layout()` 把两类附件槽钉回 `GENERAL`（唯一同时允许读写与采样的布局）：本绘制内被采样的图、与其它槽指向同一张图的槽。描述符记录布局与通道前转换改为取同一个函数结果（`image_access_layout()`），并把这个结果并入附件记忆表的键（`slot_of[9]`、`pin_bits`）。

**现状**：Mali-G720 上布局模式的实测收益≈0（布局开 GPU 14.55 vs 关 14.99/15.41，差值在噪声带内），因此**默认关、不作为优化方向**；代码保留给其它 GPU 试，冲突钉位是它能被安全开启的前提。

## 3. 预热线程读越界：稀疏渲染目标

**问题**：后台预热线程用通道键里的 `layout[]` 给附件引用填布局，而稀疏渲染目标（未使用的颜色/深度槽）在键里是 `VK_ATTACHMENT_UNUSED`（`0xFFFFFFFF`），被直接当成数组下标 → 越界读 → **预热线程段错误**。与任何门控无关，默认构建也会触发。

**修法**：颜色与深度两处都先判 `index < key->attachments`，否则退回 `GENERAL`。同时把"表头尺寸"写进 PSO 清单（见下）。

## 4. PSO 清单表头失效 → 预热静默关闭

**问题**：`pso_vk_user.tsv` 的表头记录了 `sizeof(graphics_pass_key)` 与 `sizeof(pipeline_key)`。本轮通道键加了 `layout[9]`，结构体变大；读取端按尺寸拒绝不匹配的记录，而表头只在文件为空时写过，于是旧文件**永久读不出任何记录**，新记录还继续往旧头后面追加——结果是预热无声失效，没有任何日志提示。

**修法**：用 `pread` 读表头并与当前结构体尺寸比对，不一致就 `ftruncate(fd,0)` 整份作废、重写新表头并打一行日志。旧记录本身已不可用，留着只会静默关掉预热。

**影响**：只在键布局变化时触发，但一旦触发比崩溃更难发现（功能悄悄消失）。

## 5. 异常帧日志每帧追加

**问题**：`abnormal-frames.log` 原来用追加模式。这台设备几乎帧帧超过 20 ms 阈值，单次运行涨到 **59 MB**；而写这份文件的正是被测线程，体积本身变成取证成本并污染测量。

**修法**：每次启动截断重写，与 `game-startup.log` 同一约定。

## 验证与边界

- **构建**：干净树（提交 `4a851d1`）`build.py --game-run --no-turnip --no-gpu-extras` exit 0，`nvn_vulkan`/`hle_nvn`/`hle_thread` 三个 TU 0 error；本提交的索引另在临时 worktree 中单独编译验证过（exit 0、`nvn_vulkan.log` 0 字节）。
- **真机**：vivo V2417A / MT6989 / Mali-G720，营地 区域1/5709,4494。提交版复跑：35.0 FPS（浮层）、日志窗 `interval 28.85` / `thread_cpu 21.62` / `GPU 12.90` / `fps 33.08`，0 条 `【红】/VUID/Fatal`，`[vulkan-counter-gpu] CPU_fallback=0`（counter 快路正常）。
- **长稳与走位**（诊断版同源码）：22200 帧 / 10 分钟，进程未重启、0 错误、命中率恒 0.96、buffer 缓存封顶 256 MB、RSS ≈2.6 GB 稳定；摇杆走进新区域（5709,4494 → 5722,4595）通道对象 79/26/49 → 87/28/51，命中率仍 0.96、冲突 0、无错绘。
- **验证边界**：单台 Mali、营地静止与短距离走位；未覆盖战斗、传送、首次进入新区域，也未做温度匹配的跨运行 A/B。附件记忆的 CPU 收益在噪声边缘，**不作为独立提速结论**。

## 改动文件

- `nvn_vulkan_draw.inc`：附件记忆表、冲突钉位、`slot_of`/`pin_bits` 键。
- `nvn_vulkan_pass.inc`：通道键 `layout[9]`（布局模式）、图像销毁时作废记忆表。
- `nvn_vulkan_tex.inc`、`nvn_vulkan_shader.inc`、`nvn_vulkan_probe.inc`、`nvn_vulkan_warm.inc`：布局模式（默认关）、描述符按图像真实布局记录、调试抓图用真实布局、预热通道带布局。
- `nvn_vulkan_cpu_phases.inc`：异常帧日志改为启动时截断。
- `nvn_vulkan.c`、`nvn_vulkan_counter_gpu.inc`、`docs/ANDROID_MALI_FPS_20261020.zh-CN.md`：counter 修复，详见该文。

## 复现

- 构建：`PYTHONUTF8=1 python dm/android/tools/build.py --game-run --no-turnip --no-gpu-extras --keystore dm-work/build/android/debug.keystore --version dev`
- 装机：`python dm/android/tools/install_apk.py`（同签名覆盖，数据包与存档保留，装完核对双端 md5）
- 进营地：「▸ 启动游戏」→ 约 15 s 到英雄选择（须看到角色）→ 点屏幕中心（触屏全部映射为手柄确认）→ 约 15 s 进营地
- 取日志：`adb exec-out "run-as org.d2r.android.port cat files/game.log"`
- 看哪些字段：`[vulkan-passes] passes/frame`、`[vulkan-frame-stability] interval/thread_cpu`、`[vulkan-counter-gpu] CPU_fallback`；附件记忆命中率需另加报告行（提交版未打印）。
