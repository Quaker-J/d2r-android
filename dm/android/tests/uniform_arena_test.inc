// 64 KiB 描述符的零 padding、批内版本和 fence 后复用是真实 uniform 契约。
// Zero padding across the 64 KiB descriptor, per-draw versions and reuse after fences are real uniform contracts.
static uint8_t *uniform_test_mapping(VkDescriptorBufferInfo info) {
    for(uniform_chunk *c=uniform_chunks;c;c=c->next)if(c->buffer.buffer==info.buffer){assert(info.offset+info.range<=c->buffer.size);assert(!(info.offset%uniform_stride));return (uint8_t*)c->buffer.mapped+info.offset;}
    assert(0);return NULL;
}
static void uniform_arena_reference_test(void) {
    uint8_t *bytes=malloc(65536);assert(bytes);for(unsigned i=0;i<65536;i++)bytes[i]=(uint8_t)(i*37+13);
    shader_entry fragment={.stage=1},compute_shader={.stage=5};shader_binding guest={.kind=1,.slot=3},driver={.kind=1,.slot=0},handles={.kind=1,.slot=2};
    memset(&state,0,sizeof state);state.ubo[1][0]=(p_bind_buf){.addr=(uintptr_t)bytes,.size=65536};
    mtl_sub_begin();VkDescriptorBufferInfo full=uniform_buffer(&fragment,&guest);assert(full.range==65536);assert(!memcmp(uniform_test_mapping(full),bytes,65536));
    state.ubo[1][0].size=16;VkDescriptorBufferInfo small=uniform_buffer(&fragment,&guest);assert(full.buffer!=small.buffer||full.offset!=small.offset);
    uint8_t *mapped=uniform_test_mapping(small);assert(!memcmp(mapped,bytes,16));for(unsigned i=16;i<65536;i++)assert(!mapped[i]);assert(!memcmp(uniform_test_mapping(full),bytes,65536));
    VkDescriptorBufferInfo previous[20];for(unsigned i=0;i<20;i++){previous[i]=uniform_buffer(&fragment,&guest);assert(!memcmp(uniform_test_mapping(previous[i]),bytes,16));
        for(unsigned j=0;j<i;j++)assert(previous[i].buffer!=previous[j].buffer||previous[i].offset!=previous[j].offset);}
    uint64_t hits=uniform_driver_hits;VkDescriptorBufferInfo a=uniform_buffer(&compute_shader,&driver),b=uniform_buffer(&compute_shader,&driver);assert(a.buffer==b.buffer&&a.offset==b.offset&&uniform_driver_hits==hits+1);
    state.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)bytes,.size=16};VkDescriptorBufferInfo c=uniform_buffer(&compute_shader,&driver);assert(c.buffer!=a.buffer||c.offset!=a.offset);
    assert(!memcmp(uniform_test_mapping(c)+0x210,&state.ubo[5][0].addr,8));for(unsigned i=0x210;i<0x220;i++)assert(!uniform_test_mapping(a)[i]);
    a=uniform_buffer(&fragment,&handles);state.textures[1][0]=123;b=uniform_buffer(&fragment,&handles);assert(a.buffer!=b.buffer||a.offset!=b.offset);
    uint32_t value;memcpy(&value,uniform_test_mapping(b)+0x168,4);assert(value==123);memcpy(&value,uniform_test_mapping(a)+0x168,4);assert(value==0);mtl_sub_commit();
    // 前一轮写满的页重用为未绑定槽时，全部 64 KiB 必须回到零。
    // A page written in full in the previous batch must become entirely zero when reused for an unbound slot.
    memset(&state,0,sizeof state);for(unsigned i=1;i<DM_VK_BATCH_SLOTS;i++){mtl_sub_begin();mtl_sub_commit();}mtl_sub_begin();VkDescriptorBufferInfo zero=uniform_allocate(0,&mapped);assert(zero.buffer==full.buffer&&zero.offset==full.offset);
    mapped=uniform_test_mapping(zero);for(unsigned i=0;i<65536;i++)assert(!mapped[i]);mtl_sub_commit();
    // 独立计算 shader 把 c0 的 UBO 地址/大小读到 SSBO；不是只验证 CPU 构造函数。
    // An independent compute shader copies c0's UBO address/size to an SSBO, exercising GPU consumption too.
    uint32_t result[4]={0};uint8_t control[0x880]={10};prog_t program={.nstage=1,.sd={{.control=control}}};
    state.program=&program;state.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)bytes,.size=12};state.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)result,.size=sizeof result};
    mtl_sub_begin();p_dispatch one={1,1,1,0};dispatch_compute(&one);mtl_sub_commit();uint64_t address;memcpy(&address,result,8);assert(address==(uintptr_t)bytes&&result[2]==12&&result[3]==0);
    free(bytes);printf("Vulkan uniform arena: zero padding, independent versions, driver reuse, compute UBO table passed; arena=%llu KiB\n",(unsigned long long)(uniform_arena_bytes/1024));
}
