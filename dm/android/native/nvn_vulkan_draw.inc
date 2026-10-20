// 图形管线与绘制；与描述符/计算绑定分开以便独立优化。
// Graphics pipelines and draws, separated from descriptor/compute binding for independent optimization.
static VkBlendFactor blend_factor(unsigned factor) {
    switch(factor){case 1:return VK_BLEND_FACTOR_ZERO;case 2:return VK_BLEND_FACTOR_ONE;case 3:return VK_BLEND_FACTOR_SRC_COLOR;case 4:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;case 5:return VK_BLEND_FACTOR_SRC_ALPHA;case 6:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_DST_ALPHA;case 8:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;case 9:return VK_BLEND_FACTOR_DST_COLOR;case 10:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;case 11:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;case 0x61:return VK_BLEND_FACTOR_CONSTANT_COLOR;case 0x62:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;case 0x63:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case 0x64:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;default:return VK_BLEND_FACTOR_ONE;}
}
static VkBlendOp blend_operation(unsigned op){return op>=1&&op<=5?op-1:VK_BLEND_OP_ADD;}
static VkPrimitiveTopology topology(unsigned primitive){switch(primitive){case 0:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 1:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case 6:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;default:die("Vulkan primitive %u pending",primitive);return 0;}}
static VkImageView attachment_view(image_entry *e,const view_t *v) {
    return image_view_get(e,VK_IMAGE_VIEW_TYPE_2D,texture_range(e,v));
}
// 附件描述的记忆表：一帧约 1270 次绘制里绝大多数用的还是完全没变的渲染目标，重建出来的描述、视图与尺寸
// 都一样。键覆盖下面这段真正读到的输入——state.rt 的全部字节，加上每个附件当时的客体 serial 与 populated
// （serial 挡住游戏原地重建纹理对象，populated 决定 loadOp）。贴图释放时由 graphics_cache_forget_image 清空，
// 所以 image_entry 指针被复用的别名也不会留下过期 view。
// Memo for the attachment descriptions: of ~1270 draws a frame, most still target unchanged attachments and
// rebuild identical descriptions, views and extents. The key covers exactly what that block reads -- every byte of
// state.rt plus each attachment's guest serial at the time (which catches the game reinitializing a texture object
// in place) and its populated flag (it decides loadOp). graphics_cache_forget_image drops the memo when a texture
// is released, so a recycled image_entry address cannot leave a stale view behind either.
typedef struct {
    uint8_t rt[sizeof(state.rt)];
    image_entry *images[9];const tex_t *source[9];VkImageView views[9];VkAttachmentDescription attachments[9];
    VkAttachmentReference colors[8],depth;int populated[9];
    VkImageLayout slot_of[9];unsigned pin_bits;
    int n;unsigned na;uint32_t width,height;int valid;
} attachment_memo;
static attachment_memo attach_memo;
// 记忆表默认关闭：它按构造应当与逐绘制重建完全同结果，但今天没有设备能证实，所以基线保持原行为，
// 设了 DM_VK_MEMO 才启用，一次装机就能把三项改动分别 A/B。
// The memo is off by default: it is supposed to be equivalent to rebuilding every draw, but nothing can prove that
// on a device today, so the baseline keeps the original behaviour and DM_VK_MEMO turns it on. One install can then
// A/B each change separately.
static int vk_memo_mode=-1;
// 默认开（任务 #13 实测：命中率 0.96、sample_attachment_conflicts=0、画面无异常；同会话 CPU 方向性下降
// 0.4–0.6 ms，落在噪声边缘但方向一致）；DM_VK_MEMO=0 关。记忆只缓存附件描述，loadOp 仍逐绘制按 populated 决定。
// On by default (measured in task #13: 0.96 hit rate, zero sampled-attachment conflicts, no visual
// difference; CPU moved down 0.4-0.6 ms in the same session, at the noise edge but consistent in direction).
// DM_VK_MEMO=0 disables it. The memo caches only the attachment descriptions; loadOp is still decided per draw
// from populated.
static int vk_memo_enabled(void) { if(vk_memo_mode<0){const char *e=getenv("DM_VK_MEMO");vk_memo_mode=!(e&&!strcmp(e,"0"));} return vk_memo_mode; }
static uint64_t attachment_memo_hits,attachment_memo_misses;   // 命中率决定这段记忆到底省了多少：没有它，下一轮无法判断改动是否生效
// The hit rate is what proves the memo does anything at all; without it the next round cannot tell the change from a no-op.
static void attachment_memo_invalidate(void) { attach_memo.valid=0; }
// 同一绘制内既被采样又被当附件写，或与另一附件槽同图：这两种情况下没有任何"角色布局"能同时满足
// 描述符与通道转换，只有 GENERAL 对读写都合法，所以钉在 GENERAL。真机实测这种冲突约 8.6 次/帧，是常态。
// An image sampled and written as an attachment in the same draw, or sharing two attachment slots, has no role
// layout that satisfies both the descriptor and the pass transition; GENERAL is the only layout legal for read and
// write, so pin those. The device measured ~8.6 such conflicts per frame, i.e. this is routine, not an edge case.
static VkImageLayout attachment_slot_layout(const image_entry *e,const image_entry *const *assigned,unsigned assigned_count,VkAccessFlags role) {
    if(draw_sampled_contains(e))return VK_IMAGE_LAYOUT_GENERAL;
    for(unsigned i=0;i<assigned_count;i++)if(assigned[i]==e)return VK_IMAGE_LAYOUT_GENERAL;
    return image_target_layout(e,role);
}
#include "nvn_vulkan_pass.inc"

// 管线按真正的固定状态复用；视口、剪裁、颜色常量和模板掩码/参考值动态设置。
// Reuse pipelines by actual fixed state; viewport, scissor, blend constants and stencil masks/reference are dynamic.
typedef struct {
    shader_entry *vs,*fs; VkFormat formats[9]; uint32_t colors,primitive,attachments,depth_attachment,color_indices[8],required_subgroup_size;
    va_st attributes[16]; vs_st streams[16]; color_st color; chmask_st mask;
    blend_st blend[8]; poly_st polygon; ds_st depth; ms_st multisample;
} pipeline_key;
typedef struct pipeline_entry { pipeline_key key; VkPipeline pipeline; struct pipeline_entry *next; } pipeline_entry;
static pipeline_entry *graphics_pipelines[2048];
static uint64_t pipeline_creations, pipeline_reuses;
static pipeline_entry *graphics_last_pipeline;
static uint64_t pipeline_last_hits,pipeline_hash_calls;
static void graphics_pipeline_report(void) {
    static uint64_t frame,last_hits,hash_calls;
    if(!presents||presents%60||presents==frame)return;
    resource_create_report();
    uint64_t span=presents-frame,hits=pipeline_last_hits-last_hits,hashes=pipeline_hash_calls-hash_calls;
    LOG("[vulkan-pipeline-last] frame=%llu span=%llu key_bytes=%zu last_hits/frame=%.2f hash_calls/frame=%.2f hash_bytes_avoided/frame=%.0f full_key_compare=1",(unsigned long long)presents,(unsigned long long)span,sizeof(pipeline_key),hits/(double)span,hashes/(double)span,(double)hits*sizeof(pipeline_key)/span);
    frame=presents;last_hits=pipeline_last_hits;hash_calls=pipeline_hash_calls;
}
#include "nvn_vulkan_cmd_state.inc"
// 无片元着色器：全程序共用一份空描述符布局；运行期与预热共享同一对象，管线布局缓存键一致。
// No fragment shader: one empty descriptor-set layout for every program, one object shared by runtime and warm-up so layout cache keys agree.
static shader_entry *graphics_empty_shader(void) {
    static shader_entry empty={.stage=1,.set=1};
    if(!empty.layout){pthread_mutex_lock(&graphics_object_lock);
        if(!empty.layout){VkDescriptorSetLayoutCreateInfo empty_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};VK_CHECK(vkCreateDescriptorSetLayout(device,&empty_info,NULL,&empty.layout));}
        pthread_mutex_unlock(&graphics_object_lock);}
    return &empty;
}
// 逐字段从 key 派生图形管线创建状态，运行期与预热共用；动态值（视口/剪裁/混合常量/深度偏置/模板掩码与参考）
// 保持零或占位，真正取值由每次绘制的动态状态提交，创建结果只依赖固定状态，两处调用因此字节级一致。
// Derive graphics pipeline creation state from the key, shared by runtime and warm-up; dynamic values (viewport, scissor,
// blend constants, depth bias, stencil masks/reference) stay zero/placeholder and are applied per draw as dynamic state,
// so creation results depend on fixed state only and both callers agree byte for byte.
static VkResult graphics_pipeline_create(const pipeline_key *key,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout,VkPipeline *pipeline,int fatal) {
    if(key->colors>8){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan color attachments %u pending",key->colors);}
    // key->primitive 已是拓扑转换后的 VkPrimitiveTopology；运行期只可能产出点/线/三角形六种，其余一律拒绝。
    // key->primitive is already the converted VkPrimitiveTopology; the runtime only ever produces the six point/line/triangle kinds, anything else is rejected.
    if(key->primitive>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan primitive %u pending",key->primitive);}
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned nattr=0,nstream=0;uint8_t stream_used[16]={0};
    for(unsigned i=0;i<16;i++){va_st a=key->attributes[i];if(!a.fmt||!vs->attributes[i])continue;VkFormat format=vk_format(a.fmt);
        if(format==VK_FORMAT_UNDEFINED){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex format %#x pending",a.fmt);}
        if(a.stream>15){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex stream %u pending",a.stream);}
        attrs[nattr++]=(VkVertexInputAttributeDescription){i,a.stream,format,a.offset};stream_used[a.stream]=1;}
    for(unsigned i=0;i<16;i++)if(stream_used[i]){const vs_st *s=&key->streams[i];
        if(s->divisor>1){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex divisor %u pending",s->divisor);}
        streams[nstream++]=(VkVertexInputBindingDescription){i,s->stride,s->divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};}
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=nstream,.pVertexBindingDescriptions=streams,.vertexAttributeDescriptionCount=nattr,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=key->primitive};
    VkViewport viewport={0,0,1,1,0,1};VkRect2D scissor={{0,0},{1,1}};
    VkPipelineViewportStateCreateInfo view={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=key->polygon.cull,.frontFace=key->polygon.front?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.depthBiasEnable=!!key->polygon.offset_en,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo multi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT,.alphaToCoverageEnable=key->multisample.a2c};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=key->depth.dt,.depthWriteEnable=key->depth.dw,.depthCompareOp=key->depth.df?key->depth.df-1:VK_COMPARE_OP_ALWAYS,.stencilTestEnable=key->depth.st};
    ds.front.failOp=key->depth.fail_f?key->depth.fail_f-1:VK_STENCIL_OP_KEEP;ds.front.passOp=key->depth.zpass_f?key->depth.zpass_f-1:VK_STENCIL_OP_KEEP;ds.front.depthFailOp=key->depth.zfail_f?key->depth.zfail_f-1:VK_STENCIL_OP_KEEP;ds.front.compareOp=key->depth.sf_f?key->depth.sf_f-1:VK_COMPARE_OP_ALWAYS;
    ds.back.failOp=key->depth.fail_b?key->depth.fail_b-1:VK_STENCIL_OP_KEEP;ds.back.passOp=key->depth.zpass_b?key->depth.zpass_b-1:VK_STENCIL_OP_KEEP;ds.back.depthFailOp=key->depth.zfail_b?key->depth.zfail_b-1:VK_STENCIL_OP_KEEP;ds.back.compareOp=key->depth.sf_b?key->depth.sf_b-1:VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState blend[8];for(unsigned i=0;i<key->colors;i++){blend_st b=key->blend[i];blend[i]=(VkPipelineColorBlendAttachmentState){.blendEnable=(key->color.blend_en>>i)&1,.srcColorBlendFactor=blend_factor(b.src_rgb),.dstColorBlendFactor=blend_factor(b.dst_rgb),.colorBlendOp=blend_operation(b.eq_rgb),.srcAlphaBlendFactor=blend_factor(b.src_a),.dstAlphaBlendFactor=blend_factor(b.dst_a),.alphaBlendOp=blend_operation(b.eq_a),.colorWriteMask=(key->mask.mask>>(4*i))&15};}
    VkPipelineColorBlendStateCreateInfo color={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=key->colors,.pAttachments=blend};
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs->module,.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs->module,.pName="main"}};
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=sizeof dynamic_states/sizeof *dynamic_states,.pDynamicStates=dynamic_states};
#ifdef __ANDROID__
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required[2];
    for(unsigned i=0;i<(fs->module?2u:1u);i++)
        if(!dm_vk_driver_prepare_shader_stage(&vk_api,&stages[i],&required[i])){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan graphics subgroup policy: %s",dm_vk_driver_error(&vk_api));}
#endif
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=fs->module?2:1,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&view,.pRasterizationState=&raster,.pMultisampleState=&multi,.pDepthStencilState=&ds,.pColorBlendState=&color,.pDynamicState=&dynamic,.layout=layout,.renderPass=pass->pass};
    return pipeline_create_graphics(&pi,pipeline);
}
// 真实创建才记录（缓存命中不写）；清单在 nvn_vulkan_warm.inc。
// Record genuine creations only (cache hits write nothing); the manifest lives in nvn_vulkan_warm.inc.
static void pso_record_graphics(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key);
static void draw_graphics_inner(const p_draw *draw,const p_draw_idx *indexed);
static void draw_graphics(const p_draw *draw,const p_draw_idx *indexed) {
    uint64_t phase=dm_cpu_phase_begin();draw_graphics_inner(draw,indexed);dm_cpu_phase_end(DM_CPU_PHASE_DRAW,phase);
}
static void draw_graphics_inner(const p_draw *draw,const p_draw_idx *indexed) {
    // 零顶点/零实例的 NVN 绘制不访问索引和资源，也不增加遮挡样本。
    // NVN draws with zero vertices/instances access no indices/resources and add no occlusion samples.
    if ((draw && draw->count <= 0) || (indexed && (indexed->count <= 0 || !indexed->instances))) return;
    snapshot_prepare();
    if(!state.program||(!state.rt.n&&!state.rt.depth))die("Vulkan draw without program/targets");shader_entry *vs=NULL,*fs=NULL;
    for(unsigned i=0;i<state.program->nstage;i++){shader_entry *s=program_shader(state.program,i);if(s->stage==0)vs=s;else if(s->stage==1)fs=s;else die("Vulkan graphics stage %u pending",s->stage);}
    if(!vs)die("Vulkan draw missing vertex stage");
    {uint64_t phase=dm_cpu_phase_begin();dependencies_prepare_draw(vs,fs,draw,indexed);dm_cpu_phase_end(DM_CPU_PHASE_DEPENDENCY,phase);}
    // 只在已有通道自然结束后提前提交；绑定和资源版本获取尚未开始。
    // Submit early only after the previous pass naturally ended, before binding or acquiring resource versions.
    if(!graphics_active_frame)async_chunk_boundary_locked();
    if(!fs)fs=graphics_empty_shader();
    post_op *const *output_scope=post_copy_tail;forward_scope_marker=output_scope;
    // 初始化列表的求值顺序在 C 里未定义；第二个集合的动态偏移接在第一个之后，必须按序调用。
    // Initializer-list evaluation order is unspecified in C; the second set's dynamic offsets follow the first,
    // so the two binds must be ordered statements.
    uint32_t dynamic_offsets[256];unsigned dynamic_count=0;
    draw_sampled_count=0;   // 本帧采样过的图与被重复绑定的 UBO 都从这一句起重新记
    uint64_t bind_phase=dm_cpu_phase_begin();
    VkDescriptorSet sets[2];
    sets[0]=bind_shader(vs,NULL,dynamic_offsets,&dynamic_count);
    sets[1]=bind_shader(fs,NULL,dynamic_offsets,&dynamic_count);
    dm_cpu_phase_end(DM_CPU_PHASE_BIND,bind_phase);
    uint64_t layout_phase=dm_cpu_phase_begin();VkPipelineLayout layout=graphics_layout_get(vs,fs);dm_cpu_phase_end(DM_CPU_PHASE_LAYOUT,layout_phase);
    // 附件的 initial/final 用"角色常量"而不是图像此刻的布局：所有转换都交给通道开始前那次 image_access。
    // 这样通道对象与帧缓冲不随图像历史变化，通道键不必扩字段，预热与运行期仍然同键。
    // Attachment initial/final use the role's constant layout rather than the image's current one, with every
    // transition done by the image_access that precedes the pass. Render passes and framebuffers therefore stay
    // independent of image history, the pass key needs no new field, and warm-up keeps matching runtime keys.
    // 角色常量与图像无关：颜色附件访问在任何 aspect 判断之前就返回 COLOR_ATTACHMENT_OPTIMAL，深度访问既不匹配
    // 颜色也不匹配传输/采样，落到 GENERAL，和 image_access(实际 e, 同一 access) 必然同值。
    // The role constants are entry-independent: color-attachment access returns COLOR_ATTACHMENT_OPTIMAL before any
    // aspect test, and depth access matches neither color nor transfer nor sample, so it lands on GENERAL -- always
    // the same value image_access(the real entry, same access) computes.
    const VkImageLayout color_layout=image_target_layout(NULL,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        depth_layout=image_target_layout(NULL,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkImageView views[9]={0};image_entry *images[9]={0};const tex_t *sources[9]={0};int populated_now[9]={0};VkImageLayout slot_of[9]={0};VkAttachmentDescription attachments[9]={0};VkAttachmentReference colors[8]={0},depth={VK_ATTACHMENT_UNUSED,depth_layout};unsigned na=0;uint32_t width=UINT32_MAX,height=UINT32_MAX;
    int memo=vk_memo_enabled()&&attach_memo.valid&&attach_memo.n==state.rt.n&&!memcmp(attach_memo.rt,&state.rt,sizeof attach_memo.rt);
    // 命中前还要逐附件比两样会在两次绘制之间自己变的东西：populated 决定 loadOp（上传或清除后会变），
    // serial 决定这个客体对象现在到底是哪张图。两步都只用已缓存的指针，不再走 image_get、texture_range
    // 和视图缓存查找。
    // Before a hit, compare the two inputs that can change between draws on their own: populated decides loadOp
    // (it changes after an upload or clear) and the serial says which texture that guest object is right now. Both
    // use only the cached pointers, skipping image_get, texture_range and the view cache lookup.
    if(memo)for(unsigned i=0;i<attach_memo.na;i++){
        // state.rt 里存的是客体纹理对象的指针，游戏原地重建它时字节不变而 serial 已经变了：
        // 只比指针会把过期 view 当成命中，所以还要确认每个缓存条目仍然对应当前 serial。
        // state.rt holds pointers to guest texture objects; when the game reinitializes one in place the bytes are
        // unchanged but the serial is not, so comparing pointers alone would accept a stale view. Each cached entry
        // must still correspond to the current serial.
        const tex_t *t=attach_memo.source[i];
        // 钉位（冲突图）逐绘制变化，它决定缓存里的 slot_of 还成不成立，所以和 serial/populated 一起比。
        // The pin set changes per draw and determines whether the cached slot_of still holds, so compare it
        // alongside serial and populated.
        unsigned cached_pin=attach_memo.pin_bits&(1u<<i),now_pin=draw_sampled_contains(attach_memo.images[i])?(1u<<i):0u;
        if(!t||t->serial!=attach_memo.images[i]->serial||attach_memo.images[i]->populated!=attach_memo.populated[i]||now_pin!=cached_pin){memo=0;break;}
    }
    if(memo)attachment_memo_hits++;
    if(memo){
        na=attach_memo.na;width=attach_memo.width;height=attach_memo.height;depth=attach_memo.depth;
        memcpy(slot_of,attach_memo.slot_of,sizeof slot_of);
        memcpy(images,attach_memo.images,sizeof images);memcpy(views,attach_memo.views,sizeof views);
        memcpy(attachments,attach_memo.attachments,sizeof attachments);memcpy(colors,attach_memo.colors,sizeof colors);
        for(unsigned i=0;i<na;i++)image_gpu_written(images[i]);
    } else {
    attachment_memo_misses++;
    for(int i=0;i<state.rt.n;i++){
        colors[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,color_layout};if(!state.rt.color[i])continue;image_entry *e=image_get(state.rt.color[i]);const view_t *v=state.rt.has_cview[i]?&state.rt.cview[i]:NULL;VkImageSubresourceRange range=texture_range(e,v);uint32_t w=dimension(e->texture.d.width,range.baseMipLevel),h=dimension(e->texture.d.height,range.baseMipLevel);if(w<width)width=w;if(h<height)height=h;
        const VkImageLayout slot=attachment_slot_layout(e,(const image_entry *const *)images,na,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);slot_of[na]=slot;
        images[na]=e;sources[na]=state.rt.color[i];views[na]=attachment_view(e,v);attachments[na]=(VkAttachmentDescription){.format=e->format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=slot,.finalLayout=slot};populated_now[na]=e->populated;colors[i]=(VkAttachmentReference){na,slot};na++;image_gpu_written(e);
    }
    if(state.rt.depth){image_entry *e=image_get(state.rt.depth);const view_t *v=state.rt.has_dview?&state.rt.dview:NULL;VkImageSubresourceRange range=texture_range(e,v);uint32_t w=dimension(e->texture.d.width,range.baseMipLevel),h=dimension(e->texture.d.height,range.baseMipLevel);if(w<width)width=w;if(h<height)height=h;const VkImageLayout dslot=attachment_slot_layout(e,(const image_entry *const *)images,na,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);slot_of[na]=dslot;images[na]=e;sources[na]=state.rt.depth;views[na]=attachment_view(e,v);attachments[na]=(VkAttachmentDescription){.format=e->format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=dslot,.finalLayout=dslot};populated_now[na]=e->populated;depth=(VkAttachmentReference){na,dslot};na++;image_gpu_written(e);}
    if(!na)die("Vulkan draw has no attachments");
    // 记下这段真正读到的输入，供下一个用同样渲染目标的绘制直接取用。命中分支不会再有 na==0：
    // 只有活下来的描述才会被记进表里。
    // Record the inputs this block actually read so the next draw with the same targets can take them directly.
    // A hit can never have na==0: only descriptions that survived the check above get stored.
    attach_memo.valid=1;attach_memo.n=state.rt.n;attach_memo.na=na;attach_memo.width=width;attach_memo.height=height;
    attach_memo.depth=depth;memcpy(attach_memo.rt,&state.rt,sizeof attach_memo.rt);
    memcpy(attach_memo.images,images,sizeof attach_memo.images);memcpy(attach_memo.source,sources,sizeof attach_memo.source);memcpy(attach_memo.views,views,sizeof attach_memo.views);
    memcpy(attach_memo.attachments,attachments,sizeof attach_memo.attachments);memcpy(attach_memo.colors,colors,sizeof attach_memo.colors);
    // 存进表里的必须是"描述里编码的那个 populated"，不是 image_gpu_written 已经改成 1 之后的值：
    // 后者会让首绘的 DONT_CARE 描述被标成已填充，之后每次命中都重放 DONT_CARE 丢掉上一通道写的内容。
    // Store the populated value the description actually encoded, not the value after image_gpu_written flipped it
    // to 1: otherwise a first-draw DONT_CARE description gets labelled as filled and every later hit replays it,
    // discarding what the previous pass wrote.
    for(unsigned i=0;i<na;i++)attach_memo.populated[i]=populated_now[i];
    memcpy(attach_memo.slot_of,slot_of,sizeof attach_memo.slot_of);
    attach_memo.pin_bits=0;for(unsigned i=0;i<na;i++)if(draw_sampled_contains(images[i]))attach_memo.pin_bits|=1u<<i;
    }
    graphics_pass_key pass_key=graphics_pass_key_make(na,state.rt.n,attachments,colors,depth);graphics_frame_entry *frame;
    if(graphics_pass_matches(&pass_key,views,width,height))frame=graphics_active_frame;
    else {
        graphics_pass_end();
        for(unsigned i=0;i<na;i++)if(draw_sampled_contains(images[i])){
            // 只在布局启用时才会非零：真机上如果一直是 0，这条风险就不用防护；一旦有，就把冲突图退回 GENERAL。
            // Only non-zero while layouts are enabled: if the device run keeps it at 0 the risk needs no guard, and
            // if it fires, the conflicting images get pinned to GENERAL.
            if(!(layout_sampled_attachment_conflicts%256))LOG("[vulkan-layout] 同一绘制内图像既被采样又被当附件写 serial=%llu 累计 %llu 次（该槽已钉回 GENERAL）",(unsigned long long)images[i]->serial,(unsigned long long)layout_sampled_attachment_conflicts);
            layout_sampled_attachment_conflicts++;
        }
        for(unsigned i=0;i<na;i++)if(i==depth.attachment)image_access_layout(images[i],VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,slot_of[i]);
            else image_access_layout(images[i],VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,slot_of[i]);
        graphics_pass_entry *pass=graphics_pass_get(pass_key,attachments,colors,depth,1);frame=graphics_frame_get(pass,views,images,width,height);
    }
    // 只复制索引真正可访问的顶点前缀，避免每次绘制读取整段大型绑定池。
    // Copy only the vertex prefix reachable by the actual indices, rather than the entire large bound pool for every draw.
    VkBuffer index_buffer=VK_NULL_HANDLE;VkIndexType index_type=VK_INDEX_TYPE_UINT16;
    int64_t highest=draw?(int64_t)draw->first+draw->count-1:0;
    if(indexed){unsigned size=indexed->itype==0?1:indexed->itype==1?2:4;uint64_t bytes=(uint64_t)indexed->count*size;
        if(bytes>UINTPTR_MAX-indexed->ibuf)die("Vulkan draw index range overflow");
        // 这里在可能发生的 vertex dependency flush 后重新取得快照，不保留 preflight 的 handle。
        // Reacquire after any vertex-dependency flush rather than retaining the preflight handle.
        vk_buffer raw=snapshot_buffer(indexed->ibuf,bytes),indices=raw;uint32_t maximum=index_buffer_maximum(raw,size);
        if(size==1){post_op *p=post_new(3);p->source=buffer_create((uint64_t)indexed->count*2);for(int i=0;i<indexed->count;i++)((uint16_t*)p->source.mapped)[i]=((const uint8_t*)raw.mapped)[i];indices=p->source;}
        highest=(int64_t)maximum+indexed->base_vertex;index_buffer=indices.buffer;index_type=size==4?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16;
    }
    if(highest<0)die("Vulkan negative vertex fetch");
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned nattr=0,nstream=0;uint8_t stream_used[16]={0};uint32_t attribute_end[16]={0};VkBuffer vertex_buffers[16]={0};VkDeviceSize offsets[16]={0};
    for(unsigned i=0;i<16;i++){va_st a=state.attributes[i];if(!a.fmt||!vs->attributes[i])continue;VkFormat format=vk_format(a.fmt);if(format==VK_FORMAT_UNDEFINED)die("Vulkan vertex format %#x pending",a.fmt);attrs[nattr++]=(VkVertexInputAttributeDescription){i,a.stream,format,a.offset};stream_used[a.stream]=1;int bw,bh;uint32_t end=a.offset+nvn_fmt_bpb(a.fmt,&bw,&bh);if(end>attribute_end[a.stream])attribute_end[a.stream]=end;}
    for(unsigned i=0;i<16;i++)if(stream_used[i]){const vs_st *s=&state.streams[i];const NVNbufferRange *v=&state.vertices[i];if(s->divisor>1)die("Vulkan vertex divisor %u pending",s->divisor);if(!v->address||!v->size)die("Vulkan missing vertex buffer %u",i);streams[nstream++]=(VkVertexInputBindingDescription){i,s->stride,s->divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};uint64_t element=s->divisor?(indexed?(uint64_t)indexed->base_instance+indexed->instances-1:0):(uint64_t)highest;uint64_t needed=element*s->stride+attribute_end[i];if(needed>v->size)needed=v->size;VkDescriptorBufferInfo fetched=forward_readonly_buffer(v->address,needed,1,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);vertex_buffers[i]=fetched.buffer;offsets[i]=fetched.offset;}
    VkPrimitiveTopology primitive=topology(draw?draw->prim:indexed->prim);
    // NVN 范围可反向或为常值，Vulkan 同样允许；默认 0..1 由状态初始化提供。
    // NVN permits reversed or constant ranges, as does Vulkan; state initialization supplies the 0..1 default.
    float *v=state.viewports.v[0];VkViewport viewport={v[0],v[1],v[2]>0?v[2]:width,v[3]>0?v[3]:height,state.depth_range.n,state.depth_range.f};
    // 限量记录实际范围供真机定位；最多 12 种，不执行读回或改变渲染。
    // Bound real-range diagnostics to 12 distinct pairs, without readbacks or rendering changes.
    static uint64_t depth_ranges_seen[12];static unsigned depth_ranges_count;uint64_t depth_range_bits;
    if(depth_ranges_count<12){memcpy(&depth_range_bits,&state.depth_range,sizeof depth_range_bits);unsigned i=0;while(i<depth_ranges_count&&depth_ranges_seen[i]!=depth_range_bits)i++;
        if(i==depth_ranges_count){depth_ranges_seen[depth_ranges_count++]=depth_range_bits;LOG("[vulkan-depth] range=%g,%g test=%u write=%u compare=%u target=%s",viewport.minDepth,viewport.maxDepth,(unsigned)state.depth.dt,(unsigned)state.depth.dw,(unsigned)state.depth.df,state.rt.depth?"depth":"color-only");}}
    int x=state.scissor.x>0?state.scissor.x:0,y=state.scissor.y>0?state.scissor.y:0;int sw=state.scissor.w>0?state.scissor.w:width,sh=state.scissor.h>0?state.scissor.h:height;if(x+sw>(int)width)sw=width-x;if(y+sh>(int)height)sh=height-y;if(sw<0||sh<0)die("Vulkan scissor outside target");VkRect2D scissor={{x,y},{sw,sh}};
    // Metal 的裁剪空间 Y 向上，Vulkan 的正高度视口 Y 向下；用负高度保持 NVN 画面方向。
    // Metal clip Y points up while Vulkan's positive viewport points down; a negative viewport preserves NVN orientation.
    viewport.y += viewport.height; viewport.height = -viewport.height;
    pipeline_key key={.vs=vs,.fs=fs,.colors=state.rt.n,.primitive=primitive,.color=state.color,.mask=state.mask,.polygon=state.polygon,.depth=depth.attachment!=VK_ATTACHMENT_UNUSED?state.depth:(ds_st){0},.multisample=state.multisample,.attachments=na,.depth_attachment=depth.attachment};
#ifdef __ANDROID__
    key.required_subgroup_size=vk_api.requested_subgroup_size;
#endif
    for(int i=0;i<state.rt.n;i++){key.color_indices[i]=colors[i].attachment;if(colors[i].attachment!=VK_ATTACHMENT_UNUSED)key.formats[i]=attachments[colors[i].attachment].format;if((state.color.blend_en>>i)&1)key.blend[i]=state.blend[i];}
    if(depth.attachment!=VK_ATTACHMENT_UNUSED)key.formats[8]=attachments[depth.attachment].format;
    for(unsigned i=0;i<16;i++){if(state.attributes[i].fmt&&vs->attributes[i])key.attributes[i]=state.attributes[i];if(stream_used[i])key.streams[i]=state.streams[i];}
    // 持久 pipeline 链只追加，queue_lock 串行访问；完整 key 相同才绕过哈希查找。
    // Persistent pipelines are append-only and accessed under queue_lock; only a full-key match bypasses hashing.
    unsigned pipeline_bucket=0;pipeline_entry *cached=graphics_last_pipeline;
    if(cached&&!memcmp(&cached->key,&key,sizeof key))pipeline_last_hits++;
    else{pipeline_hash_calls++;pipeline_bucket=graphics_hash(&key,sizeof key)&2047;cached=graphics_pipelines[pipeline_bucket];while(cached&&memcmp(&cached->key,&key,sizeof key))cached=cached->next;}
    if(!cached){resource_create_start creation=resource_create_begin();cached=calloc(1,sizeof *cached);if(!cached)die("Vulkan pipeline cache allocation");cached->key=key;
        // 只有未命中才派生创建管线的状态；每 draw 的动态状态仍在下面独立提交。创建与预热共用 graphics_pipeline_create，驱动缓存键一致。
        // Derive pipeline-creation state only on a miss; per-draw dynamic state is applied separately below. Creation shares graphics_pipeline_create with warm-up so driver cache keys agree.
        VK_CHECK(graphics_pipeline_create(&key,vs,fs,frame->key.pass,layout,&cached->pipeline,1));
        cached->next=graphics_pipelines[pipeline_bucket];graphics_pipelines[pipeline_bucket]=cached;pipeline_creations++;resource_create_end(1,creation,vs->key,fs->module?fs->key:NULL);
        pso_record_graphics(vs,fs,&frame->key.pass->key,&key);}else pipeline_reuses++;
    graphics_last_pipeline=cached;
    graphics_pass_begin(frame);
    graphics_command_apply(cached->pipeline,layout,sets,dynamic_offsets,dynamic_count,&viewport,&scissor,state.blend_color.rgba,state.polygon_offset.units,state.polygon_offset.factor,state.stencil_faces);
    for(unsigned i=0;i<16;i++)if(vertex_buffers[i])graphics_command_vertices(i,vertex_buffers[i],offsets[i]);
    graphics_probe_draw(draw,indexed);
    if(indexed){graphics_command_index(index_buffer,0,index_type);vkCmdDrawIndexed(command,indexed->count,indexed->instances,0,indexed->base_vertex,indexed->base_instance);}else vkCmdDraw(command,draw->count,1,draw->first,0);
    gpu_snapshot_consumer_encoded();
    forward_record_outputs(output_scope,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    draw_count++;
    // 无 shader 输出时保留 tile 内附件；可写缓冲/图像必须先结束通道再保证主机可见性。
    // Keep attachment tiles for shaders without outputs; writable buffers/images end the pass before host visibility.
    if(graphics_shader_writes(vs)||graphics_shader_writes(fs)){
        graphics_pass_end();if(shader_host_writes(vs)||shader_host_writes(fs)){VkMemoryBarrier writes={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&writes,0,NULL,0,NULL);}
    }
    batch_draws++;
    if (draw_count <= 12 && getenv("DM_VK_TRACE")) LOG("[vulkan] draw=%llu %s/%s rt=%ux%u vp=%g,%g %gx%g sc=%d,%d %dx%d mask=%#x cull=%u front=%u samples=%llu", (unsigned long long)draw_count, vs->key, fs->key, width, height, viewport.x, viewport.y, viewport.width, viewport.height, x, y, sw, sh, state.mask.mask, (unsigned)state.polygon.cull, (unsigned)state.polygon.front, (unsigned long long)samples_passed);
}
#include "nvn_vulkan_warm.inc"
