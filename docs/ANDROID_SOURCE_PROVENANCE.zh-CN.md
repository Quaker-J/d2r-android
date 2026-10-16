# dodola 参考代码溯源清单

2026-10-16，只读核对当前生产源码与本地参考仓库；未改生产、未运行测试或ADB。不用相同行数推算贡献比例。

## 固定参考及结论边界

参考仓库 `<LOCAL_WORK>/research/dodola-d2r-mobile`，remote `https://github.com/dodola/d2r-mobile.git`，实际HEAD为 `642480b7606cf4adda30bcf009eb56f87745c908`，标题 `Add Android Vulkan runtime controls and measured game A/B records`。本文行号指该固定版本。

本仓库源自上游 `https://github.com/jhaizhou-ops/d2r-mobile.git`（私有发布为 `sydeEvans/d2r-android`，起点为其提交 `2eb5758`）。当前Android渲染实现仍是 `dm/android/native/nvn_vulkan.c` 及其拆分include，未替换为dodola的 `dm/gfx/nvn/nvn_vk.c`。已经采用的是数项具体算法和生命周期设计，再按当前资源模型改写；“使用了百分之多少dodola代码”没有可审计的单一数字。

## 已落地：可定位的借鉴与改写

| 项目 | dodola固定源码证据 | 当前实际入口及区别 | 归类 |
| --- | --- | --- | --- |
| 提前分段提交 | `nvn_vk.c:2224 sub_chunk_flush`，pass/query结束后无fence提前submit，多command buffer以最终fence退役 | `nvn_vulkan_async.inc:50 async_chunk_boundary_locked`，自然pass边界检查draw数和录制wall预算；本地async batch及post/资源直到真实final fence，尾段才发screen信号。不照搬48draw常数 | 算法/生命周期借鉴后改写，已落地 |
| 异步呈现 | `nvn_vk.c:3063 mtl_present`，无capture时异步提交；`:3216 mtl_present_wait`仅检查票据上限 | 本地 `nvn_vulkan.c:807 mtl_present`返回真实GPU提交票，`:918 mtl_present_wait`真实等待GPU完成及CPU发布；仍等上一acquire消费保证binary semaphore复用。另参考共同上游Metal和WindowAcquire契约，不能单归于dodola | 多来源设计审查后改写，已落地 |
| driver c0/c2构表前复用 | `nvn_vk.c:1881 driver_table`按epoch及ssbo/texture revision先命中，`:1845 driver_table_build`未命中才合成 | `nvn_vulkan_uniform.inc:17 uniform_driver_current`，shader构表前调用；`nvn_vulkan.c:740/746`按实际输入变化失效state_valid；arena reset强制失效。不复制完整revision/registry系统 | 算法借鉴后改写，已落地 |
| 最近PSO入口 | `nvn_vk.c:807`完整PsoKey比较最近永久槽，再回退hash缓存 | `nvn_vulkan_draw.inc:102`完整pipeline_key memcmp `graphics_last_pipeline`，未命中才hash；本地永久entry生命周期单独核对，保留所有状态 | 算法借鉴后改写，已落地 |
| 真GPU counter timestamp | `nvn_vk.c:950`在完成后CPU按真实query tick及timestampPeriod转换19.2MHz | 本地 `nvn_vulkan_counter_gpu.inc`真实timestamp→query copy→纯32位精确compute转换→同GPU报告backing；`nvn_vulkan_forward_compose.inc`按FIFO拼装消费范围，type1仍CPU真实samples且不提前forward | 参考真实query语义，但GPU转换及合成是本地进一步实现，不是复制其CPU发布路径 |

## 已研究但未整套移植

- **cached writeback staging**：dodola `nvn_vk.c:867 wb_stage_has_cached_type`及`:874`独立缓存CPU读回staging。本地buffer_create优先同时HOST_VISIBLE/COHERENT/CACHED内存，但没有因此等同于移植其独立wb_stage与一次invalidate体系；不能把共享内存属性偏好说成整套方案落地。
- **writeback coalesce/prepare**：dodola `nvn_vk_writeback_plan.h`及`:907`范围规划；本地未直接引入该header及其完整规划路径。
- **binding plan / command state cache / registry revision**：dodola专用 `nvn_vk_binding_plan.h`、`nvn_vk_command_state.h`等未整文件纳入当前Android native目录。本地已有自己的descriptor、hazard及forward系统，功能名称相近不证明同一来源。
- **renderpass load/store与precise hazard**：读过参考设计，但本次清单没有足够逐提交证据把当前整套pass/dependency实现归为dodola移植。需具体变更记录，不能用结果相似补齐来源。
- **跨启动VkPipelineCache**：dodola `nvn_vk.c:469–474`明确创建当前device空cache，不加载跨设备数据。本地新增 `nvn_vulkan_pipeline_cache.inc` 的磁盘持久化、身份校验和worker是本地实现；可以说都使用Vulkan公开pipeline cache API，不能说磁盘方案复制自此fork。

## 共同上游与直接复用的区分

两仓都有原项目的NVN命令协议、HLE及shader工具体系；例如dodola的 `hle_nvn.c` 历史包含 `5924a97 Merge upstream v1.0.17`及上游版本提交。其共有文件/接口不应自动计为dodola贡献。当前同路径的 `hle_nvn.c`、`dm.h`、`hle_lifecycle.c`内容已经不同，字节比较也不能追溯历史来源。

本次做了一个**有限范围**整文件核对：参考目录 `dm/gfx/nvn/nvn_vk*` 全部文件的SHA256，与当前 `dm/android/native` 直接子文件比较，未发现整文件完全相同的模块；当前目录也没有对应 `nvn_vk_*.h` 文件。这个结果仅说明“此范围没有完全相同的整文件”，**不证明整个仓库没有直接复制过代码片段或其他文件**。未做全仓历史blame/patch来源审计，不给“直接复制零行”的断言。

对外可准确表述：目前明确吸收了分段提交、异步呈现、driver表提前复用、最近PSO缓存等设计，并依照本地Android架构实现；真实counter路径也参考其query语义。不是把dodola整个渲染后端搬进来，行数百分比未知。其README的iOS60FPS不能当作这个Android版本的实测依据。
