/* Part of vk_draw.c (included there, shares its statics): pipeline keys and creation, vertex input from D3D9
 * vertex declarations, and r_draw. Kept apart so each file stays readable.
 */

typedef struct {
    void *vs, *ps;
    VkRenderPass rp;
    uint32_t topo, ncol;
    uint32_t decl_hash, strides[16];
    uint32_t rs[24];               /* the render states that are baked into a pipeline */
} PipeKey;

typedef struct { PipeKey k; VkPipeline p; int used; } PipeEnt;
#define NPIPE 8192
static PipeEnt pipes[NPIPE];

static uint32_t fnv(const void *d, size_t n) {
    const uint8_t *p = d;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static const uint8_t baked_rs[24] = {7, 8, 14, 19, 20, 22, 23, 27, 52, 53, 54, 55, 56, 58, 59, 168, 171, 175, 185,
                                     195, 206, 207, 208, 209};

static VkBlendFactor blend(uint32_t b) {
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 12: return VK_BLEND_FACTOR_SRC_ALPHA; case 13: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blendop(uint32_t o) {
    switch (o) {
    case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD;
    }
}
static VkCompareOp cmp(uint32_t c) { return c >= 1 && c <= 8 ? (VkCompareOp)(c - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp sop(uint32_t o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }

static VkFormat decl_format(uint32_t t) {
    switch (t) {
    case 0: return VK_FORMAT_R32_SFLOAT; case 1: return VK_FORMAT_R32G32_SFLOAT; case 2: return VK_FORMAT_R32G32B32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT; case 4: return VK_FORMAT_B8G8R8A8_UNORM;
    case 5: return VK_FORMAT_R8G8B8A8_USCALED; case 6: return VK_FORMAT_R16G16_SSCALED; case 7: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 8: return VK_FORMAT_R8G8B8A8_UNORM; case 9: return VK_FORMAT_R16G16_SNORM; case 10: return VK_FORMAT_R16G16B16A16_SNORM;
    case 11: return VK_FORMAT_R16G16_UNORM; case 12: return VK_FORMAT_R16G16B16A16_UNORM;
    case 13: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 14: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 15: return VK_FORMAT_R16G16_SFLOAT; case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* A widened element (vk.vtx_widen) is appended to its vertex as 4 floats: its index among the stream's widened
 * elements in declaration order, or -1. */
static int widen_index(D3DObj *decl, uint32_t upto) {
    uint32_t el = decl->code + 8 * upto, st = R16(el);
    if (!(vk.vtx_widen & (1u << R8(el + 4)))) return -1;
    int k = 0;
    for (uint32_t e = 0; e < upto; e++) {
        uint32_t x = decl->code + 8 * e;
        if (R16(x) == st && (vk.vtx_widen & (1u << R8(x + 4)))) k++;
    }
    return k;
}
static uint32_t widen_count(D3DObj *decl, uint32_t st) {
    uint32_t n = 0;
    for (uint32_t e = 0; e < decl->code_size / 8; e++) {
        uint32_t x = decl->code + 8 * e;
        if (R16(x) == 0xFF || R16(x) >= 16) break;
        if (R16(x) == st && (vk.vtx_widen & (1u << R8(x + 4)))) n++;
    }
    return n;
}
static void widen_value(uint32_t type, const uint8_t *p, float *o) {
    int16_t s[4] = {0, 0, 0, 1};
    switch (type) {
    case 5: for (int i = 0; i < 4; i++) o[i] = p[i]; return;                                   /* UBYTE4 */
    case 6: memcpy(s, p, 4); o[0] = s[0]; o[1] = s[1]; o[2] = 0; o[3] = 1; return;             /* SHORT2 */
    case 7: memcpy(s, p, 8); for (int i = 0; i < 4; i++) o[i] = s[i]; return;                 /* SHORT4 */
    case 13: { uint32_t v; memcpy(&v, p, 4); o[0] = (float)(v & 1023); o[1] = (float)((v >> 10) & 1023);
               o[2] = (float)((v >> 20) & 1023); o[3] = (float)(v >> 30); return; }            /* UDEC3 */
    case 4: o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; return;   /* D3DCOLOR */
    case 8: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; return;                          /* UBYTE4N */
    case 9: case 10: { memcpy(s, p, type == 9 ? 4 : 8); if (type == 9) { s[2] = 0; s[3] = 32767; }
               for (int i = 0; i < 4; i++) { float f = s[i] / 32767.0f; o[i] = f < -1 ? -1 : f; } return; }   /* SHORTxN */
    case 11: case 12: { uint16_t u[4] = {0, 0, 0, 65535}; memcpy(u, p, type == 11 ? 4 : 8);
               for (int i = 0; i < 4; i++) o[i] = u[i] / 65535.0f; return; }                   /* USHORTxN */
    case 14: { uint32_t v; memcpy(&v, p, 4); for (int i = 0; i < 3; i++) { int x = (int)((v >> (10 * i)) & 1023); if (x > 511) x -= 1024;
               float f = x / 511.0f; o[i] = f < -1 ? -1 : f; } o[3] = 1; return; }            /* DEC3N */
    case 15: case 16: { uint16_t h[4] = {0, 0, 0, 0x3C00}; memcpy(h, p, type == 15 ? 4 : 8);
               for (int i = 0; i < 4; i++) { uint32_t e = (h[i] >> 10) & 31, m = h[i] & 1023, sg = (uint32_t)(h[i] >> 15) << 31, b;
                   if (!e) b = sg | (m ? 0 : 0); else if (e == 31) b = sg | 0x7F800000u | (m << 13);
                   else b = sg | ((e + 112) << 23) | (m << 13);
                   if (!e && m) { float f = m / 16777216.0f; o[i] = sg ? -f : f; continue; }
                   memcpy(&o[i], &b, 4); } return; }                                            /* FLOAT16 */
    default: o[0] = o[1] = o[2] = 0; o[3] = 1;
    }
}

static VkPipeline pipeline(GpuShader *vs, GpuShader *ps, D3DObj *decl, uint32_t topo, const uint32_t *strides) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs; k.ps = ps; k.rp = cur.rp; k.topo = topo; k.ncol = cur.ncol;
    k.decl_hash = fnv(HOST(decl->code), decl->code_size);
    for (int i = 0; i < 16; i++) k.strides[i] = strides[i];
    for (int i = 0; i < 24; i++) k.rs[i] = g_dev.rs[baked_rs[i]];
    if (!g_dev.rs[27]) { k.rs[3] = k.rs[4] = k.rs[16] = k.rs[20] = k.rs[21] = k.rs[22] = k.rs[23] = 0; }   /* no blend */
    if (!g_dev.rs[52]) for (int i = 9; i < 15; i++) k.rs[i] = 0;                                            /* no stencil */
    uint32_t h = fnv(&k, sizeof k), i = h % NPIPE;
    for (uint32_t probe = 0; probe < NPIPE; probe++, i = (i + 1) % NPIPE) {
        if (!pipes[i].used) break;
        if (!memcmp(&pipes[i].k, &k, sizeof k)) return pipes[i].p;
    }
    /* vertex input */
    VkVertexInputBindingDescription bind[17];
    VkVertexInputAttributeDescription attr[16];
    uint32_t nb = 0, na = 0, have = 0, used_streams = 0;
    for (uint32_t e = 0; e < decl->code_size / 8; e++) {
        uint32_t el = decl->code + 8 * e;
        uint32_t stream = R16(el), off = R16(el + 2), type = R8(el + 4), usage = R8(el + 6), idx = R8(el + 7);
        if (stream == 0xFF || stream >= 16) break;
        int loc = sm3_attr_location(usage, idx);
        if (loc < 0 || !(vs->info.inputs_mask & (1u << loc)) || (have & (1u << loc))) continue;
        VkFormat f = decl_format(type);
        if (f == VK_FORMAT_UNDEFINED) continue;
        int wk = widen_index(decl, e);
        if (wk >= 0) {                                          /* appended as 4 floats after the original vertex */
            f = VK_FORMAT_R32G32B32A32_SFLOAT;
            off = strides[stream] - 16u * widen_count(decl, stream) + 16u * (uint32_t)wk;
        }
        attr[na++] = (VkVertexInputAttributeDescription){(uint32_t)loc, stream, f, off};
        have |= 1u << loc;
        used_streams |= 1u << stream;
    }
    for (uint32_t s = 0; s < 16; s++)
        if (used_streams & (1u << s)) bind[nb++] = (VkVertexInputBindingDescription){s, strides[s], VK_VERTEX_INPUT_RATE_VERTEX};
    uint32_t missing = vs->info.inputs_mask & ~have;
    if (missing) {
        bind[nb++] = (VkVertexInputBindingDescription){16, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        for (uint32_t l = 0; l < 16; l++)
            if (missing & (1u << l)) attr[na++] = (VkVertexInputAttributeDescription){l, 16, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb; vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = na; vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    static const VkPrimitiveTopology tp[7] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    ia.topology = tp[topo < 7 ? topo : 4];
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = g_dev.rs[8] == 2 && vk.feats.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = g_dev.rs[22] == 2 ? VK_CULL_MODE_FRONT_BIT : g_dev.rs[22] == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    float dbias, slope;
    memcpy(&dbias, &g_dev.rs[195], 4);
    memcpy(&slope, &g_dev.rs[175], 4);
    if (dbias != 0.0f || slope != 0.0f) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = dbias * (cur.f[4] == VK_FORMAT_D16_UNORM ? 65535.0f : 16777215.0f);
        rs.depthBiasSlopeFactor = slope;
    }
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (cur.has_depth) {
        ds.depthTestEnable = g_dev.rs[7] != 0;
        ds.depthWriteEnable = g_dev.rs[7] != 0 && g_dev.rs[14] != 0;
        ds.depthCompareOp = cmp(g_dev.rs[23]);
        ds.stencilTestEnable = g_dev.rs[52] != 0 && cur.f[4] != VK_FORMAT_D16_UNORM;
        ds.front = (VkStencilOpState){sop(g_dev.rs[53]), sop(g_dev.rs[55]), sop(g_dev.rs[54]), cmp(g_dev.rs[56]),
                                      g_dev.rs[58], g_dev.rs[59], 0};
        ds.back = ds.front;
        if (g_dev.rs[185])
            ds.back = (VkStencilOpState){sop(g_dev.rs[186]), sop(g_dev.rs[188]), sop(g_dev.rs[187]), cmp(g_dev.rs[189]),
                                         g_dev.rs[58], g_dev.rs[59], 0};
    }
    VkPipelineColorBlendAttachmentState cb[4];
    for (uint32_t i = 0; i < 4; i++) {
        VkPipelineColorBlendAttachmentState *b = &cb[i];
        memset(b, 0, sizeof *b);
        uint32_t mask = i == 0 || !vk.feats.independentBlend ? g_dev.rs[168] : g_dev.rs[189 + i];
        b->colorWriteMask = mask & 0xF;
        if (g_dev.rs[27]) {
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = blend(g_dev.rs[19]);
            b->dstColorBlendFactor = g_dev.rs[19] == 12 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA :
                                     g_dev.rs[19] == 13 ? VK_BLEND_FACTOR_SRC_ALPHA : blend(g_dev.rs[20]);
            b->colorBlendOp = blendop(g_dev.rs[171]);
            int sep = g_dev.rs[206] != 0;
            b->srcAlphaBlendFactor = sep ? blend(g_dev.rs[207]) : b->srcColorBlendFactor;
            b->dstAlphaBlendFactor = sep ? blend(g_dev.rs[208]) : b->dstColorBlendFactor;
            b->alphaBlendOp = sep ? blendop(g_dev.rs[209]) : b->colorBlendOp;
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = cur.ncol;
    cbs.pAttachments = cb;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = ps->mod; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cbs; ci.pDynamicState = &dy;
    ci.layout = playout; ci.renderPass = cur.rp;
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) {                                       /* log it once, cache the failure, skip such draws */
        fprintf(stderr, "usf: pipeline failed (%d): vs %08x ps %08x topo %u attrs", r, d3d_shader_hash(g_dev.vs), d3d_shader_hash(g_dev.ps), topo);
        for (uint32_t a = 0; a < na; a++) fprintf(stderr, " [loc %u fmt %d off %u b%u]", attr[a].location, attr[a].format, attr[a].offset, attr[a].binding);
        fprintf(stderr, "\n");
        p = VK_NULL_HANDLE;
    }
    if (!pipes[i].used) { pipes[i].k = k; pipes[i].p = p; pipes[i].used = 1; }
    return p;
}

/* ---- draws ---- */
static uint32_t vcount(uint32_t topo, uint32_t prims) {
    switch (topo) {
    case 1: return prims; case 2: return 2 * prims; case 3: return prims + 1; case 4: return 3 * prims;
    default: return prims + 2;
    }
}

static uint32_t constants(int ps) {
    uint32_t nc = ps ? 224 : 256, size = nc * 16 + 16 * 16 + 16 + 16;
    uint8_t *p;
    uint32_t off = vk_ring_alloc(size, (uint32_t)vk.props.limits.minUniformBufferOffsetAlignment, (void **)&p);
    memcpy(p, ps ? (void *)g_dev.psc : (void *)g_dev.vsc, nc * 16);
    memcpy(p + nc * 16, ps ? (void *)g_dev.psi : (void *)g_dev.vsi, 256);
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) if ((ps ? g_dev.psb : g_dev.vsb)[i]) bits |= 1u << i;
    memset(p + nc * 16 + 256, 0, 32);
    memcpy(p + nc * 16 + 256, &bits, 4);
    float *misc = (float *)(p + nc * 16 + 272);
    if (ps) {
        misc[0] = g_dev.rs[15] ? (float)(g_dev.rs[24] & 255) / 255.0f : 0.0f;
        misc[1] = g_dev.rs[15] ? (float)g_dev.rs[25] : 0.0f;
        misc[2] = g_dev.rs[194] ? 1.0f : 0.0f;
    } else {
        float vw = g_dev.vp[2] ? (float)g_dev.vp[2] : (float)cur.w, vh = g_dev.vp[3] ? (float)g_dev.vp[3] : (float)cur.h;
        misc[0] = 1.0f / vw;                                   /* D3D9 pixel centres are at integers: +0.5 px right */
        misc[1] = -1.0f / vh;                                  /* and +0.5 px down (NDC y points up) */
        misc[2] = as_x; misc[3] = as_y;                        /* ultrawide: clip-space scale about the centre */
    }
    return off;
}

static void bind_sampler(VkWriteDescriptorSet *w, VkDescriptorImageInfo *ii, VkDescriptorSet set, uint32_t binding,
                         uint32_t stage, uint32_t type) {
    D3DObj *o = d3d_obj(g_dev.tex[stage]);
    GpuImage *g = vk_texture(o);
    int want = type == 3 ? OBJ_CUBE : type == 4 ? OBJ_VOLUME : OBJ_TEXTURE;
    if (!g || o->kind != want) g = vk_dummy_texture(type == 3 ? 3 : type == 4 ? 4 : 2);
    ii->sampler = vk_sampler(g_dev.ss[stage]);
    ii->imageView = g->view;
    ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(w, 0, sizeof *w);
    w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w->dstSet = set;
    w->dstBinding = binding;
    w->descriptorCount = 1;
    w->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w->pImageInfo = ii;
}

void r_draw(int indexed, uint32_t prim, uint32_t base_vertex, uint32_t min_index, uint32_t num_vertices,
            uint32_t start, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t up_indices, uint32_t up_ifmt) {
    g_dev.draws++;
    if (!vk.dev || !count) return;
    if (!g_dev.vs || !g_dev.ps || !g_dev.decl) { skipped_ff++; return; }
    GpuShader *vs = shader(d3d_obj(g_dev.vs)), *ps = shader(d3d_obj(g_dev.ps));
    D3DObj *decl = d3d_obj(g_dev.decl);
    if (!vs || !ps || vs->bad || ps->bad || !decl) { skipped_bad++; return; }
    if (getenv("USF_SKIP_VS") && R32(g_dev.vs + 8) == (uint32_t)atoi(getenv("USF_SKIP_VS"))) return;   /* debugging */
    vk_ring_reserve(8u << 20);
    /* textures first (uploads end the render pass and may flush, which resets the descriptor pool) */
    for (uint32_t s = 0; s < 16; s++) if (ps->info.sampler_type[s]) vk_texture(d3d_obj(g_dev.tex[s]));
    for (uint32_t s = 0; s < 4; s++) if (vs->info.sampler_type[s]) vk_texture(d3d_obj(g_dev.tex[12 + s]));
    for (int t = 2; t <= 4; t++) vk_dummy_texture(t);
    if (!ensure_pass()) return;
    VkDescriptorImageInfo ii[20];
    VkWriteDescriptorSet wr[22];
    uint32_t nw = 0;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = vk.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(vk.dev, &ai, &set) != VK_SUCCESS) { vk_flush(); VKC(vkAllocateDescriptorSets(vk.dev, &ai, &set)); }
    for (uint32_t s = 0; s < 16; s++)
        if (ps->info.sampler_type[s]) { bind_sampler(&wr[nw], &ii[nw], set, 2 + s, s, ps->info.sampler_type[s]); nw++; }
    for (uint32_t s = 0; s < 4; s++)
        if (vs->info.sampler_type[s]) { bind_sampler(&wr[nw], &ii[nw], set, 18 + s, 12 + s, vs->info.sampler_type[s]); nw++; }
    uint32_t nverts = vcount(prim, count);
    if (getenv("USF_DRAWLOG") && g_dev.frames == (uint32_t)atoi(getenv("USF_DRAWLOG")))
        fprintf(stderr, "draw rt %08x(%ux%u) ds %08x vs %08x ps %08x prim %u n %u idx %d blend %u %u/%u z %u/%u cull %u cw %x vp %u,%u %ux%u tex0 %08x fmt %u %ux%u usage %x st %u ref %u func %u pass %u mask %x wmask %x hasds %d\n",
                g_dev.rt[0], cur.w, cur.h, g_dev.ds, d3d_shader_hash(g_dev.vs), d3d_shader_hash(g_dev.ps), prim, count, indexed, g_dev.rs[27],
                g_dev.rs[19], g_dev.rs[20], g_dev.rs[7], g_dev.rs[14], g_dev.rs[22], g_dev.rs[168], g_dev.vp[0], g_dev.vp[1],
                g_dev.vp[2], g_dev.vp[3], g_dev.tex[0], d3d_obj(g_dev.tex[0]) ? d3d_obj(g_dev.tex[0])->format : 0,
                d3d_obj(g_dev.tex[0]) ? d3d_obj(g_dev.tex[0])->width : 0, d3d_obj(g_dev.tex[0]) ? d3d_obj(g_dev.tex[0])->height : 0,
                d3d_obj(g_dev.tex[0]) ? d3d_obj(g_dev.tex[0])->usage : 0, g_dev.rs[52], g_dev.rs[57], g_dev.rs[56], g_dev.rs[55],
                g_dev.rs[58], g_dev.rs[59], cur.has_depth);
    /* USF_TEXDBG=WxH (with USF_DRAWLOG and USF_SHOT_DIR): texture 0 of that size, as the game's memory holds it
     * (tex_<guest>.raw, the level-0 bytes) and as the GPU holds it (gputex_<frame>_<w>x<h>.bgra, written with the shot) */
    if (getenv("USF_DRAWLOG") && g_dev.frames == (uint32_t)atoi(getenv("USF_DRAWLOG")) && getenv("USF_TEXDBG") &&
        getenv("USF_SHOT_DIR") && d3d_obj(g_dev.tex[0])) {
        D3DObj *t = d3d_obj(g_dev.tex[0]);
        unsigned dw = 0, dh = 0;
        if (sscanf(getenv("USF_TEXDBG"), "%ux%u", &dw, &dh) == 2 && t->width == dw && t->height == dh) {
            extern void vk_debug_texture(GpuImage *g);
            if (!(t->usage & 3u)) vk_debug_texture(vk_texture(t));
            char fn[600];
            snprintf(fn, sizeof fn, "%s/tex_%08x.raw", getenv("USF_SHOT_DIR"), g_dev.tex[0]);
            FILE *f = fopen(fn, "wb");
            if (f) { fwrite(HOST(t->mem[0][0]), 1, t->size[0], f); fclose(f); }
        }
    }
    if (getenv("USF_DRAWLOG") && g_dev.frames == (uint32_t)atoi(getenv("USF_DRAWLOG")) && prim == 6) {
        D3DObj *rt = d3d_obj(g_dev.rt[0]), *tx = d3d_obj(g_dev.tex[0]);
        fprintf(stderr, "   backbuffer %08x rt kind %d parent %08x usage %x; tex kind %d usage %x fmt %u %ux%u; ps c2 %.3f %.3f %.3f %.3f c3 %.3f %.3f %.3f %.3f\n",
                g_dev.backbuffer, rt ? rt->kind : -1, rt ? rt->parent : 0, rt ? rt->usage : 0, tx ? tx->kind : -1,
                tx ? tx->usage : 0, tx ? tx->format : 0, tx ? tx->width : 0, tx ? tx->height : 0,
                g_dev.psc[2][0], g_dev.psc[2][1], g_dev.psc[2][2], g_dev.psc[2][3], g_dev.psc[3][0], g_dev.psc[3][1], g_dev.psc[3][2], g_dev.psc[3][3]);
    }
    /* vertices: the used range of every stream the declaration reads */
    uint32_t strides[16] = {0};
    VkBuffer bufs[17];
    VkDeviceSize offs[17];
    uint32_t first = indexed ? base_vertex + min_index : start, nv = indexed ? num_vertices : nverts;
    for (uint32_t e = 0; e < decl->code_size / 8; e++) {
        uint32_t s = R16(decl->code + 8 * e);
        if (s >= 16 || strides[s]) continue;
        uint32_t stride = up_data ? up_stride : g_dev.stream_stride[s];
        strides[s] = stride ? stride : 1;
        uint32_t src;
        if (up_data) src = up_data + (indexed ? min_index : 0) * stride;
        else {
            D3DObj *vb = d3d_obj(g_dev.stream[s]);
            if (!vb) { skipped_bad++; return; }
            src = vb->mem[0][0] + g_dev.stream_off[s] + first * stride;
        }
        uint8_t *dst;
        uint32_t nw = vk.vtx_widen ? widen_count(decl, s) : 0;
        if (!nw) {
            uint32_t bytes = nv * stride + 16;
            offs[s] = vk_ring_alloc(bytes, 16, (void **)&dst);
            memcpy(dst, HOST(src), bytes - 16);
        } else {                                                /* each vertex + its widened elements as floats */
            uint32_t ns = stride + 16u * nw;
            offs[s] = vk_ring_alloc(nv * ns + 16, 16, (void **)&dst);
            for (uint32_t v = 0; v < nv; v++) {
                const uint8_t *in = HOST(src + v * stride);
                uint8_t *out = dst + (size_t)v * ns;
                memcpy(out, in, stride);
                uint32_t k = 0;
                for (uint32_t e = 0; e < decl->code_size / 8 && k < nw; e++) {
                    uint32_t x = decl->code + 8 * e;
                    if (R16(x) == 0xFF || R16(x) >= 16) break;
                    if (R16(x) != s || !(vk.vtx_widen & (1u << R8(x + 4)))) continue;
                    widen_value(R8(x + 4), in + R16(x + 2), (float *)(out + stride + 16u * k));
                    k++;
                }
            }
            strides[s] = ns;
        }
        bufs[s] = vk.ring;
    }
    bufs[16] = defaults_buf;
    offs[16] = 0;
    VkPipeline p = pipeline(vs, ps, decl, prim, strides);
    if (!p) { skipped_bad++; return; }
    vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    for (uint32_t s = 0; s < 17; s++) if (s == 16 || strides[s]) vkCmdBindVertexBuffers(vk.cmd, s, 1, &bufs[s], &offs[s]);
    {   /* the positions, for the ultrawide cover test (vk_draw.c aspect_scale) */
        av.n = 0;
        for (uint32_t e = 0; e < decl->code_size / 8; e++) {
            uint32_t el = decl->code + 8 * e, st = R16(el);
            if (st >= 16 || R8(el + 6) != 0 || R8(el + 7) != 0 || R8(el + 4) > 3) continue;   /* POSITION0, FLOAT1..4 */
            uint32_t stride = up_data ? up_stride : g_dev.stream_stride[st];
            D3DObj *vb = up_data ? NULL : d3d_obj(g_dev.stream[st]);
            if (!up_data && !vb) break;
            av.base = (up_data ? up_data + (indexed ? min_index : 0) * stride : vb->mem[0][0] + g_dev.stream_off[st] + first * stride)
                      + R16(el + 2);
            av.stride = stride; av.n = nv; av.comps = R8(el + 4) + 1;
            break;
        }
        aspect_compute();
    }
    uint32_t dyn[2] = {constants(0), constants(1)};
    VkDescriptorBufferInfo bi[2] = {{vk.ring, 0, 256 * 16 + 288}, {vk.ring, 0, 224 * 16 + 288}};
    for (int k = 0; k < 2; k++) {
        memset(&wr[nw], 0, sizeof wr[nw]);
        wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[nw].dstSet = set;
        wr[nw].dstBinding = (uint32_t)k;
        wr[nw].descriptorCount = 1;
        wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[nw].pBufferInfo = &bi[k];
        nw++;
    }
    vkUpdateDescriptorSets(vk.dev, nw, wr, 0, NULL);
    vkCmdBindDescriptorSets(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &set, 2, dyn);
    set_viewport_scissor();
    if (indexed) {
        int i32;
        uint32_t isrc;
        if (up_data) { i32 = up_ifmt == 102; isrc = up_indices; }
        else {
            D3DObj *ib = d3d_obj(g_dev.ib);
            if (!ib) return;
            i32 = ib->format == 102;
            isrc = ib->mem[0][0] + start * (i32 ? 4u : 2u);
        }
        uint8_t *dst;
        uint32_t ib_off = vk_ring_alloc(nverts * (i32 ? 4u : 2u), 16, (void **)&dst);
        memcpy(dst, HOST(isrc), nverts * (i32 ? 4u : 2u));
        vkCmdBindIndexBuffer(vk.cmd, vk.ring, ib_off, i32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(vk.cmd, nverts, 1, 0, -(int32_t)min_index, 0);
    } else {
        vkCmdDraw(vk.cmd, nverts, 1, 0, 0);
    }
}

void vk_draw_frame_start(void) {
    vk.in_pass = 0;
    static uint32_t last_ff, last_bad;
    if ((skipped_ff != last_ff || skipped_bad != last_bad) && vk.frame % 300 == 0) {
        fprintf(stderr, "usf: draws skipped so far: %u without shaders (fixed function), %u with untranslated shaders\n",
                skipped_ff, skipped_bad);
        last_ff = skipped_ff; last_bad = skipped_bad;
    }
}

void vk_draw_init(void) {
    sc = shaderc_compiler_initialize();
    VkDescriptorSetLayoutBinding b[22];
    for (uint32_t i = 0; i < 22; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){i, i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              1, 0, NULL};
        b[i].stageFlags = i == 0 || i >= 18 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 22;
    dci.pBindings = b;
    VKC(vkCreateDescriptorSetLayout(vk.dev, &dci, NULL, &dsl));
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &dsl;
    VKC(vkCreatePipelineLayout(vk.dev, &pci, NULL, &playout));
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 16;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VKC(vkCreateBuffer(vk.dev, &bci, NULL, &defaults_buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, defaults_buf, &req);
    void *map;
    VkDeviceMemory m = vk_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &map);
    VKC(vkBindBufferMemory(vk.dev, defaults_buf, m, 0));
    float d[4] = {0, 0, 0, 1};
    memcpy(map, d, 16);
}
