# Android Mali：2026-10-21 特效区逐绘制固定成本修复

**结论先说**：特效场景掉帧的主因不是多了几个 render pass，而是**多了几百个 draw**。营地火焰初始视野 1231 draw/帧、移开后 794 draw/帧，pass 只差 3（54 vs 51），GPU 只差约 0.7–1.0 ms，差的是每 draw 11–12 µs 的 CPU 固定成本。本轮针对这笔固定成本改了五处（UBO 读取、描述符/采样器查询、贴图脏检查、依赖预检、静态状态缓存），并修掉复核中发现的四个正确性问题。真机同路径 A/B：火焰初始位置 `draw` 相位 **13.98 → 12.17 ms**（同为 1231 draw）、`ubo` **4.04 → 3.26 ms**、`bind` **7.45 → 6.46 ms**；移开后 `draw` **9.78 → 8.97 ms**、`ubo` **2.86 → 2.11 ms**。移开后 5×60 帧窗 **45.24 FPS / p50 21.20 ms / over33 0/120**（第一轮基线 43.3–44.0 FPS / 22.65 ms / 0.3–0.6）。

## 1. 归因：特效放大的是 draw 数，不是 pass 数

第一轮已用同路径受控 A/B 定量（[ANDROID_MALI_FPS_20261020.zh-CN.md](ANDROID_MALI_FPS_20261020.zh-CN.md)）：

| 场景 | draw/帧 | pass/帧 | FPS | GPU |
| --- | ---: | ---: | ---: | ---: |
| 火焰初始位置（5709,4494） | 1231 | 54 | 34.9–35.2 | 15.2 ms |
| 移开后（5765,4546） | 794 | 51 | 43.3–44.0 | 14.2 ms |

多出的 draw 分散在 7 组 shader，其中带 28/29 个 FS 贴图绑定的重材质组合占约 300 draw/帧。每 draw 的翻译层成本约 11–12 µs，逐项拆开就是本轮要削的对象：

```text
draw 13.98 = bind 7.45（⊃ resolve 6.47 ⊃ ubo 4.04）+ dep 1.41 + draw 自耗 4.1 + layout 0.1
```

翻译层无法把这些游戏 draw 合批，只能降低每 draw、每绑定、每次 UBO 读取的固定开销。技能特效没有可用的真机日志，本轮先处理与 draw 数/绑定数成正比的通用路径；技能场景仍需单独录制取证。

## 2. 发现的问题

### 2.1 UBO 每次读取都多一次 scratch 复制

`uniform_read_cached_read()`（`dm/android/native/nvn_vulkan_binding_cache.inc`）每次调用都先把客体内存复制到 `uniform_read_scratch`，再与上一次的 arena 版本 memcmp。营地稳态 **6485 次读取/帧、约 0.62 µs/次、3.9 MB/帧**，其中 74% 内容未变——命中时这次 scratch 复制完全是多余的。

### 2.2 每个贴图绑定要进两次全局描述符锁

`descriptor_texture()`（`nvn_vulkan_shader.inc`）和 `sampler_get_for_type()`（`nvn_vulkan_sampler.inc`）各取一次 `descriptor_lock`；同一个绑定在 `bind_shader` 里查一次、在 `dependencies_shader_unknown` 里又查一次。约 **2.3 万次 lock/unlock + 164 B 描述符拷贝每帧**，且重材质 shader 的 28/29 个绑定会把成本线性放大。

### 2.3 采样贴图的脏检查会整图扫页

`image_upload()`（`nvn_vulkan_tex.inc`）的稳态快路径先 `nvn_dirty_tracked()`（对最多 4096 个跟踪区间线性 `find()`），再在全局写代号变化时 `nvn_dirty_since()` 对整张贴图覆盖的每一页做 atomic load。全局写代号每帧前进约 350–380 次（贴图流式写入），因此大多数绑定都会走到逐页扫描；大贴图（如 2048² RGBA = 4096 页）每次绑定都要扫整图。

### 2.4 依赖预检对每个贴图元素重复查 pending 树

`dependencies_shader_unknown()`（`nvn_vulkan_dependencies.inc`）为每个贴图元素做 `descriptor_texture` + `dependencies_texture_unknown`（image 查找 + pending range 查询）。重材质 draw 一次要查几十个元素，实测 `dep=1.41 ms/帧`（1231 draw）。

### 2.5 每 draw 的静态状态重复获取/扫描

- `graphics_layout_get()` 每 draw 取一次 `graphics_object_lock` 做哈希查找；而 `(vs,fs)` 的布局在管线创建时就已确定。
- `graphics_shader_writes()`/`shader_host_writes()` 每 draw 重新遍历 shader 的全部 binding，只为回答两个加载后不变的布尔问题。

### 2.6 真机测试中发现的额外问题：UBO 快路被“全局存在已知复制”误关

第一版 UBO 快路只在**所有批次都没有任何已知 CPU 复制**时启用。实测移开后 99.97% 的 UBO 范围与复制不重叠，但一个无关的复制仍让快路命中率跌到约 5%（183 快比 / 3597 读）。这是测试中发现并当场修掉的过度保守判断。

## 3. 修复方法

### 3.1 UBO 免 scratch 直比（`nvn_vulkan_binding_cache.inc`、`hle_svc.c`、`nvn_vulkan_async.inc`）

- 新增 `dm_android_guest_heap_compare_owned()`：在堆权限读锁内做 guest 与 arena 版本的 `memcmp`，返回 1 相等 / 0 不等 / -1 不可用。
- 新增 `async_no_known_copy_overlap(address,size)`：按**请求范围**检查在途已知 CPU 复制，而不是看全局标志；范围不重叠才走快路。
- 命中：不复制，直接返回旧 arena 描述符；内容变化：直接复制到新 arena 槽；复制失败：先复制到 scratch，成功后再分配 arena 槽，避免租约竞争时白占 64 KiB 槽。
- 无 entry 的首次读取也走同一路径，省掉一次全量 scratch 复制。

### 3.2 描述符/采样器无锁读缓存（`nvn_vulkan_tex.inc`、`shader.inc`、`sampler.inc`）

- 在 `descriptor_lock` 外增加注册 seqlock `descriptor_read_epoch`：写者（`mtl_reg_tex`/`mtl_reg_samp`/`mtl_set_tex_pool_n`）在锁内先置奇再置偶；读者无锁取 `{id, epoch, descriptor}` 副本并复核 epoch。
- texture 缓存 256 项、sampler 缓存 256 项，按 id + epoch 命中；锁从“每次绑定两次”降到“每次真实注册才对读者可见”。
- 写者仍在 `descriptor_lock` 内，realloc/扩容期间的读者会被 epoch 或锁挡住；缓存只存值拷贝，不持有 `tex_t` 指针。

### 3.3 图像脏检查 O(1)（`dm/gfx/nvn/nvn_dirty.c/.h`、`nvn_vulkan_tex.inc`）

- 每个跟踪区间增加 `token`（incarnation + slot）和写纪元 `epoch`；内容写在 `unprotect_page`/snapshot 回滚/publish 路径用**奇偶 seqlock**包住（先奇、改 gen/state、再偶）。
- 区间登记/销毁再加一层表级 seqlock `g_range_table_epoch`，保证无锁读者看到的 `base/end/incarnation/token` 一致；读者遇奇数或前后纪元不等即拒绝。
- `image_upload` 命中“区间 token 未变 + 写纪元未变”就直接判 clean，跳过 `find()` 与整图扫页；池销毁后槽复用靠 token/incarnation 识别。
- 图像基线在 `nvn_dirty_protect()` 之前、读取之前采样 token/epoch，读完后用 `nvn_dirty_range_epoch_unchanged()` 验证未被打断；只有验证通过才置 `dirty_epoch_valid=1`，但 token 总是保留。上传开始时池尚未登记（token=0）的旧基线强制重传一次（GPU 渲染目标除外，避免用客体旧字节覆盖刚渲染的内容）。

### 3.4 依赖预检贴图 clean 记忆（`nvn_vulkan_dependencies.inc`、`async.inc`）

- 每个 shader 的每个贴图元素保存 `{handle, pending_epoch, image_epoch, descriptor_epoch, clean}`；三个纪元**逐项比较**，不再合并成一个可能相互抵消的异或值。
- `pending_unknown_epoch` 在新 unknown/host 范围登记时推进；已知 CPU 复制登记也推进。`image_state_epoch` 在图像创建、实际上传、GPU 首次写、释放、dummy clear 时推进；`descriptor_epoch` 来自注册 seqlock。
- 命中时跳过 `descriptor_texture` + `dependencies_texture_unknown`；只缓存 clean，dirty 仍立即 flush 且不写缓存。
- `image_gpu_written` 只在 `populated/stamp` 真的变化时推进 `image_state_epoch`——附件每 draw 都会调用它，无条件推进会永久关闭记忆。

### 3.5 静态状态预计算/缓存（`nvn_vulkan_shader.inc`、`pass.inc`、`draw.inc`）

- `shader_entry` 加载时预计算 `writes`/`host_writes`，保留 `flags_valid` 兼容测试/手工构造的 shader。
- `pipeline_entry` 缓存 `(vs,fs)` 对应的 `VkPipelineLayout`；只在真正创建管线时调用一次 `graphics_layout_get()`，命中路径不再取全局锁。

### 3.6 per-shader 描述符集绑定 memo（第三轮，`nvn_vulkan_shader.inc`、`nvn_vulkan_binding_cache.inc`）

- `shader_entry` 保存上次绑定解析出的真实值（`buffer/offset/range`、`sampler/imageView/imageLayout`、texel view）。解析循环仍完整执行——UBO 内容比较、`image_upload`、`image_access`、SSBO 转发、CS 探针都不跳过；只有这些值全部相同才复用上次的 `VkDescriptorSet`。
- 命中跳过 payload 打包、FNV 哈希、桶查找与 memcmp；未命中回退原 `descriptor_cache_find/allocate/update` 并刷新 memo。动态偏移不进集合，每次仍照常填写。
- `binding_cache_reset()` 递增 `binding_memo_epoch`：描述符池 reset 或批资源切换时所有 shader 的 memo 一起失效。memo 与既有 `descriptor_cache` 一样比较裸句柄、随池重置失效，不新增句柄复用风险。

## 4. 复核中拦下的正确性问题

两轮并发/生命周期与图形/依赖语义子 agent 复核，修掉：

1. **上传期间写被新基线吞掉（会永久旧图）**：基线原本在整次上传读完后才采样区间纪元；读中发生的写会被记成“上传后基线”，之后快路恒 clean。改为在 protect 之前、读取之前采样，读完验证纪元未变。
2. **`publish_unprotect_run` 纪元净增 3 次**：奇偶对多了一次，池纪元永久为奇，快路与池复用判断全部失效。改为入口 +1、gen/state 落地后 +1。
3. **依赖纪元 XOR 抵消**：pending +2、descriptor +2 可能互相抵消。改为三个纪元逐项比较。
4. **池槽复用缺口**：基线在没有 token 时把新区间全零 gen 判成 clean。改为 token 即使纪元不稳定也保留；区间表加 seqlock；`unpinned` 时强制重传一次。
5. **UBO 快路白占 arena 槽**：copy 失败时已分配的槽不会回滚。改为先复制到 scratch，成功后再分配。
6. **附件写无条件推进 image epoch**：会让依赖记忆每 draw 失效。改为只在状态真变化时推进。

残留低风险：池加载/销毁期间区间表 seqlock 为奇，读者保守回退，可能多一次整图重传；无锁读字段与既有 `find()` 同级的理论内存序问题。

## 5. 真机结果

设备 vivo V2417A / MT6989 / Mali-G720，主机档 1600×900，营地同路径（5709,4494 → 5765,4546）。`[vulkan-cpu-phases]` 是每帧分段，阶段嵌套，只能按各自 calls 看自耗。

| 指标（ms/帧） | 初始 1231 draw | 第一轮基线 | 移开 799 draw | 第一轮基线 |
| --- | ---: | ---: | ---: | ---: |
| `draw` | **12.17** | 13.98 | **8.97** | 9.78 |
| `bind` | **6.46** | 7.45 | **4.02** | 4.85 |
| `ubo` | **3.26** | 4.04 | **2.11** | 2.86 |
| `dep` | **1.35** | 1.41 | **1.06** | 1.18 |
| `resolve` | **5.51** | 6.47 | **3.54** | 4.35 |
| `descset` | 1.08 | 1.06 | 0.74 | 0.74 |
| fps / interval p50 | 38.91 / 27.05 ms | 34.9–35.2 / 28.4 | 45.24 / 21.20 ms | 43.3–44.0 / 22.65 |
| `over33ms` | 5.6/120 | 11–15/120 | **0/120** | 0.3–0.6/120 |

快路命中（`[vulkan-bindings]`，每个 60 帧窗）：

| 计数（次/帧） | 初始 | 移开 |
| --- | ---: | ---: |
| `ubo_compare_fast` / `ubo_direct` | 4814.5 / 1472.8（约 97%） | 2653 / 958（100%） |
| `tex_read_fast` / `samp_read_fast` | 8939 / 6523 | 4486 / 3112 |
| `image_clean_fast` | 1365 | 676 |
| `dep_tex_memo` | 5457 | 2568 |

**口径**：最终轮最高核 2.6 GHz，第一轮基线 2.6–2.8 GHz；GPU 14.58/13.38 vs 15.2/14.17，帧率提升含设备状态成分。**CPU 分段是主要可归因证据**，不能把 FPS 差值全部归给本轮改动。两个位置 `uploads/gpu_overwrite` 均为 0（基线 2/帧），但样本不足以单独归因，暂只记录。

证据：`dm-work/build/logs/game-log-20261021-0145-before2.log` / `-after2.log`、`camp-b-camp-before2.png`、`camp-after-move2.png`；无 FATAL/STOP，测试后已 force-stop 并息屏。最终 APK md5 `5e2bdf505a57ea9aa103d6f0bb341d9e`。

### 5.1 提交子集验证（变体 A/B）

为了回答“哪些改动真正有收益”，用临时 worktree 构建了一个只保留 UBO 快路 + layout/写标记的变体（8 文件），与全量包（15 文件）相邻安装、同一设备、同一营地初始位置测量；按本线程核频分组比较，避免跨轮频率混淆。

| 同核 3.4 GHz | `draw` | `dep` | interval |
| --- | ---: | ---: | ---: |
| 全量（15 文件） | **12.23** | **1.13** | 26.30 |
| 变体（去掉三项） | 12.68 | 1.45 | 27.33 |

同核 2.85 GHz：全量 `11.65 / 0.95`，变体 `12.31 / 1.32`。变体去掉描述符/采样器缓存、脏页区间纪元、依赖 clean 记忆后，`dep` 高 0.2–0.37 ms、`draw` 高 0.35–0.66 ms；说明这三项并非无收益，尤其依赖记忆。**结论：不删除这三项，提交全量 15 文件。**

5×60 帧窗：变体 36.04 FPS / p50 27.27，全量 37.37 / 26.49，方向一致；设备电量/热状态仍有影响，故以同核分段为主。变体与全量 APK 均做了双端 md5 核对；临时 worktree 与分支已清理。

### 5.2 P1 memo 同机相邻 A/B（2026-10-21 14:08–14:14）

同一设备先跑现装基线包（3097fce，md5 `5e2bdf505a57ea9aa103d6f0bb341d9e`），再构建并安装 P1 新包，各测营地初始与移开两个位置；绘制数 1272→1276.8、800→801.1 calls，属同场景。

| ms/帧 | 初始基线 | 初始 P1 | 移开基线 | 移开 P1 |
| --- | ---: | ---: | ---: | ---: |
| `descset` | 1.131 | **0.847** | 0.743 | **0.575** |
| `desc`（描述符查找内部） | 0.505 | **0.279** | 0.301 | **0.173** |
| `draw` | 13.101 | 13.209 | 8.035 | 8.571 |
| `binding_memo` 命中/未命中 | — | 1698.6 / 861.1（66.4%） | — | 1038.9 / 565.4（64.8%） |

- `descset` 两个位置均降约 23–25%，是本轮唯一可直接归因的收益（约 0.17–0.28 ms/帧，低于预估的 0.5–1.5 ms）。memo 接管后 `descriptor hits/miss` 从 1768/782 降到 76.9/784.2。
- `draw` 顶层相位被 ±0.5–1.3 ms 的窗口波动掩盖：初始 +0.1、移开 +0.5 ms；同轮 `bind` 6.775→6.329 / 4.396→3.791、`ubo` 3.311→3.113 / 2.360→2.071、`resolve` 5.903→5.647 / 4.089→3.477。FPS 34.85→35.05 与 44.56→45.72，GPU 13.55→14.52 / 13.00→13.45（新包热态更差），因此不把 FPS 差值当作证据。
- 截图与基线一致；四份日志无 FATAL/STOP/native crash；新包 md5 `ea8477d2986f00972afd3907c611ae57`，装机双端一致。

## 6. 改动文件

- `dm/android/native/nvn_vulkan_binding_cache.inc`：UBO 快路、范围化 known-copy 判断、计数、binding memo 纪元与计数。
- `dm/android/native/nvn_vulkan_async.inc`：`async_no_known_copy_overlap`、`pending_unknown_epoch`。
- `dm/android/native/nvn_vulkan_tex.inc`：描述符注册 seqlock、texture/sampler 读缓存、图像区间纪元与基线、`image_state_epoch`。
- `dm/android/native/nvn_vulkan_shader.inc`：`descriptor_texture` 读缓存、shader 写标记、`dep_textures` 数组、per-shader 描述符集 memo。
- `dm/android/native/nvn_vulkan_sampler.inc`：sampler 无锁读缓存。
- `dm/android/native/nvn_vulkan_dependencies.inc`：贴图 clean 记忆与三纪元失效。
- `dm/android/native/nvn_vulkan_draw.inc`、`nvn_vulkan_pass.inc`：pipeline layout 缓存、写标记使用。
- `dm/android/native/nvn_vulkan.c`：新快路日志字段（含 `binding_memo hits/miss`）、弱符号声明。
- `dm/gfx/nvn/nvn_dirty.c/.h`：区间 token/写纪元、奇偶 seqlock、表级 seqlock、新 API。
- `dm/host/hle_svc.c`、`hle.h`：`dm_android_guest_heap_compare_owned`。
- `dm/android/tests/vulkan_test.c`、`owned_snapshot_test.c`：新 API 桩。

## 7. 复现

```bash
# 构建（PYTHONUTF8=1 不能省）
PYTHONUTF8=1 python dm/android/tools/build.py --game-run --no-turnip --no-gpu-extras \
  --keystore dm-work/build/android/debug.keystore --version dev

# 装机（双端 md5 必须一致）
python dm/android/tools/install_apk.py

# 进营地并静置 30 s
python dm/android/tools/launch_to_camp.py --settle 30 --tag before2

# 采初始位置
python dm/android/tools/pull_camp_metrics.py --label before2 --abnormal

# 回放 6.64 s 录像移开（dm-work/build/logs/touch-user1.txt）
python dm/android/tools/touch_record.py replay --name user1 --timeout 60
sleep 30
python dm/android/tools/pull_camp_metrics.py --label after2 --abnormal

# 收尾
adb shell am force-stop org.d2r.android.port
adb shell input keyevent 223
```

看哪些字段：`[vulkan-cpu-phases]` 的 `draw/bind/ubo/dep/resolve/descset`、`[vulkan-bindings]` 的 `ubo_compare_fast/direct`、`tex/samp_read_fast`、`image_clean_fast`、`dep_tex_memo`、`binding_memo hits/miss`、`[vulkan-frame-stability]` 的 interval/over33ms。

## 8. 剩余候选

- **脏页快路覆盖率（P2）**：`image_clean_fast` 约占图像绑定的 18–19%；当前按整个内存池的区间纪元失效，同池任意写会让整池贴图回退慢路。先加 miss 原因计数，再评估 per-image/per-block 写纪元。
- **剩余 draw 自耗（P4）**：`draw` 顶层仍约 13.2 / 8.6 ms；附件 memo 的约 600 B 拷贝、400 B `pipeline_key` memcmp、顶点/索引快照与 `vkCmdDraw` 发射尚未拆开，需要临时子相位/采样，不盲改。
- **技能特效场景取证（P3）**：目前只有营地环境火焰；技能特效需要用户录制路线后，用同一 A/B 流程确认是否还有 GPU overdraw 或额外 pass 放大。
- **遮挡查询去 PRECISE（P5）**：会改变可观察结果（可能影响遮蔽剔除），需用户单独批准；不列入纯性能改动。
- **`mtl_tex_release` 延迟销毁（P5）**：只在场景切换/卸载时出现（营地稳态 `tex_releases=0/帧`），不是特效稳态掉帧的原因；需要数据再决定。
