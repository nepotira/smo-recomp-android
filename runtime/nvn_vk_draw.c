/* NVN on Vulkan, the drawing part (session 2; design: plan\phases\V1_vulkan.md). Included at the end of nvn_vk.c: it uses
 * that file's device (V), loader functions and error helpers. Render thread only.
 *
 * What is here: device features, a block allocator, the stream ring (guest memory -> one mapped buffer, at most one copy
 * of a range per batch), images and views made at first use from the front end's texture source, samplers, translated
 * stages -> SPIR-V (smo_glslang.dll, cached in ..\cache\vk\spv) -> pipelines made synchronously, state tracking with
 * dynamic state, dynamic rendering, clears, draws, buffer->texture and texture->texture copies, present by blit, the
 * screenshot of the window texture, fences. Anything else stops with its name (nvn_vk_missing).
 *
 * Debug: SMO_VK_DEBUG=1 turns on the checks that stand in for the validation layer that is not installed (image used
 * after destroy, barrier inside a pass, a view of an image in an unexpected layout) and prints every new pipeline. */
#include <stdarg.h>
#include <math.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#include "tex_decode.h"
#include "smo_ui.h"                                    /* UiMenuJob: the amiibo menu's pictures (vk_menu) */
static int g_split_bar, g_cs_debt;                     /* 0.1.7, image_barrier */
static int have_targets_live(void);

#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdBindVertexBuffers2) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateEvent) X(vkCmdSetEvent) X(vkGetEventStatus) X(vkResetEvent) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkResetQueryPool) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache)
#define X(n) static PFN_##n n;
VKD(X)
#undef X
static PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)

static struct {
    VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids; VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc; VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc;
    const char *exts[16]; uint32_t next;
    int has_vids, has_cbc, has_pv, has_iu8, has_plr, has_vad, has_push, has_eds3, debug;
    int has_gpl, ext_gpl, ext_so;
    int has_dcc;                                   /* SMO: VK_EXT_depth_clip_control (NVN depth mode NEAR_IS_MINUS_W) */
    uint32_t subgroup;                             /* SMO 0.1.2: VkPhysicalDeviceSubgroupProperties.subgroupSize */
    int dyn0;                                      /* Android port: without push descriptors, up to dyn0 buffers of set 0 are dynamic (dyn0_is); 0 = none */
    VkPhysicalDeviceMemoryProperties mp;
} D;
/* SMO: NVN's depth mode is honoured, read when a pipeline is made (the game sets it on the device after nvnDeviceInitialize, which
 * may be after this backend starts). 0 = NEAR_IS_MINUS_W (OpenGL clip z -w..w, NVN's default) needs VK_EXT_depth_clip_control
 * negativeOneToOne; 1 = NEAR_IS_ZERO is Vulkan's own. The window origin must be upper left (the viewport flip assumes it). */
static const void *vk_vp_next(void) {
    static VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, VK_TRUE};
    static int seen = -1;
    int m = nvn_front_clip_modes();
    if (m != seen) {
        seen = m;
        printf("nvn_vk: window origin mode %d, depth mode %d (depth clip control %s)\n", m & 0xff, m >> 8, D.has_dcc ? "yes" : "no"); fflush(stdout);
        if ((m & 0xff) != 1) nvn_vk_missing("a window origin other than upper left (nvnDeviceSetWindowOriginMode)");
        if ((m >> 8) == 0 && !D.has_dcc) printf("nvn_vk: depth mode NEAR_IS_MINUS_W without VK_EXT_depth_clip_control: the vertex stages remap z (module_from_glsl)\n");
        if ((m >> 8) != 0 && !D.has_dcc) nvn_vk_missing("NVN depth mode NEAR_IS_ZERO on a GPU without VK_EXT_depth_clip_control (the vertex stages remap z as for NEAR_IS_MINUS_W)");
    }
    return (m >> 8) == 0 && D.has_dcc ? &dcc : NULL;
}
static int64_t g_refused_draws;                    /* Android port: draws left out, their pipeline refused by the driver (pipe_build) */
static void vkd_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("nvn_vk: FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); plat_exit((int)6);
}
#define DBG_ASSERT(c, ...) do { if (D.debug && !(c)) vkd_fail(__VA_ARGS__); } while (0)
/* Session 11: the pipeline-library mode. SMO_VK_GPL wins (0 = whole pipelines, the path before session 9; 1 = a library per
 * stage pair, a measurement mode; 2 = a library per stage + preparation + barrier); without it VK_GPL_DEFAULT. */
#ifndef VK_GPL_DEFAULT
#ifdef __ANDROID__
/* Android port: whole pipelines, made when first needed and cached (Eden's way). The stage-library path compiles every
 * variation of the game's shader containers ahead: with Turnip on a Galaxy S24 Ultra its loading barrier held each frame
 * for the 120,000 jobs of a first start and the libraries ran the driver out of memory. */
#define VK_GPL_DEFAULT 0
#else
#define VK_GPL_DEFAULT 2                           /* session 11: on by default where the driver has the extension (validation run s11v3 and the first-run table of V1_vulkan.md "Session 11"); -DVK_GPL_DEFAULT=0 or SMO_VK_GPL=0 for the old path */
#endif
#endif
static int vk_gpl_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SMO_VK_GPL"); m = e && e[0] ? atoi(e) : VK_GPL_DEFAULT; } return m; }

/* ---------------------------------------------------------------- device features */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext) {
    void **tail;
    vkGetPhysicalDeviceFeatures2 = vk_sym("vkGetPhysicalDeviceFeatures2"); vkGetPhysicalDeviceProperties2 = vk_sym("vkGetPhysicalDeviceProperties2");
    vkGetPhysicalDeviceFormatProperties = vk_sym("vkGetPhysicalDeviceFormatProperties");
    vkGetPhysicalDeviceMemoryProperties = vk_sym("vkGetPhysicalDeviceMemoryProperties");
    vkGetDeviceProcAddr = vk_sym("vkGetDeviceProcAddr");
    D.debug = getenv("SMO_VK_DEBUG") && atoi(getenv("SMO_VK_DEBUG"));
    D.f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; D.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    D.f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; D.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    D.f2.pNext = &D.f11; D.f11.pNext = &D.f12; D.f12.pNext = &D.f13; tail = &D.f13.pNext;
    D.exts[D.next++] = "VK_KHR_swapchain";
#define EXT(flag, name, st, stype) if (has_name(dext, ndext, name)) { D.flag = 1; D.exts[D.next++] = name; D.st.sType = stype; *tail = &D.st; tail = &D.st.pNext; }
    EXT(has_vids, "VK_EXT_vertex_input_dynamic_state", vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT)
    EXT(has_cbc, "VK_EXT_custom_border_color", cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT)
    EXT(has_pv, "VK_EXT_provoking_vertex", pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT)
    EXT(has_iu8, "VK_EXT_index_type_uint8", iu8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT)
    EXT(has_plr, "VK_EXT_primitive_topology_list_restart", plr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT)
    EXT(has_vad, "VK_EXT_vertex_attribute_divisor", vad, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
    EXT(has_dcc, "VK_EXT_depth_clip_control", dcc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT)
    if (!(getenv("SMO_VK_EDS3") && !atoi(getenv("SMO_VK_EDS3")))) { EXT(has_eds3, "VK_EXT_extended_dynamic_state3", eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) }
    /* session 9: graphics pipeline libraries (nvn_vk_m4.c "graphics pipeline libraries"): the stage pair is compiled at loading
     * without the pass formats, a draw only LINKS. SMO_VK_GPL=1 turns it on (needs extended dynamic state 3 as well). */
    D.ext_gpl = has_name(dext, ndext, "VK_EXT_graphics_pipeline_library") && has_name(dext, ndext, "VK_KHR_pipeline_library"); D.ext_so = has_name(dext, ndext, "VK_EXT_shader_object");
    if (D.ext_gpl && vk_gpl_mode()) { D.exts[D.next++] = "VK_KHR_pipeline_library"; EXT(has_gpl, "VK_EXT_graphics_pipeline_library", gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT) }
#undef EXT
    if (has_name(dext, ndext, "VK_KHR_push_descriptor") && !(getenv("SMO_VK_PUSH") && !atoi(getenv("SMO_VK_PUSH")))) { D.has_push = 1; D.exts[D.next++] = "VK_KHR_push_descriptor"; }
    vkGetPhysicalDeviceFeatures2(V.gpu, &D.f2);                    /* every supported feature of these structures is enabled */
    {   /* SMO 0.1.2: the subgroup the translated warp operations run on (mx_translate.c, MX_LANE / mx_shfl / mx_ballot) */
        VkPhysicalDeviceSubgroupProperties sp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceFloatControlsProperties fc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, NULL};
        VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &fc};
        sp.pNext = &dp;
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sp};
        if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(V.gpu, &p2);
        D.subgroup = sp.subgroupSize; mx_subgroup_wide = sp.subgroupSize >= 32;
        printf("nvn_vk: driver %s %s; clip distances: feature %d, up to %u; 32-bit floats: keeps NaN/inf/-0 %d, keeps denormals %d, flushes denormals %d; 16-bit floats: keeps NaN/inf/-0 %d\n",
               dp.driverName, dp.driverInfo, (int)D.f2.features.shaderClipDistance, (unsigned)V.props.limits.maxClipDistances,
               (int)fc.shaderSignedZeroInfNanPreserveFloat32, (int)fc.shaderDenormPreserveFloat32, (int)fc.shaderDenormFlushToZeroFloat32, (int)fc.shaderSignedZeroInfNanPreserveFloat16);
        {   uint32_t mi = V.props.limits.maxComputeWorkGroupInvocations, mx = V.props.limits.maxComputeWorkGroupSize[0]; if (mx < mi) mi = mx; if (mi >= 64) mx_max_invocations = mi; }      /* mx_split_x */
        printf("nvn_vk: subgroup of %u lanes (the Switch GPU's warp: 32; %s), operations 0x%x in stages 0x%x; compute groups up to %u invocations (%u x %u x %u)\n", sp.subgroupSize,
               sp.subgroupSize >= 32 ? "warp operations exact" : "values exchanged between lanes (shuffle, ballot) reach only the lanes of the same subgroup",
               (unsigned)sp.supportedOperations, (unsigned)sp.supportedStages, V.props.limits.maxComputeWorkGroupInvocations,
               V.props.limits.maxComputeWorkGroupSize[0], V.props.limits.maxComputeWorkGroupSize[1], V.props.limits.maxComputeWorkGroupSize[2]);
    }
    D.has_vids = D.has_vids && D.vids.vertexInputDynamicState; D.has_cbc = D.has_cbc && D.cbc.customBorderColors && D.cbc.customBorderColorWithoutFormat;
    D.has_pv = D.has_pv && D.pv.provokingVertexLast; D.has_iu8 = D.has_iu8 && D.iu8.indexTypeUint8; D.has_plr = D.has_plr && D.plr.primitiveTopologyListRestart;
    D.has_dcc = D.has_dcc && D.dcc.depthClipControl;
    if (getenv("SMO_VK_NO_DCC")) D.has_dcc = 0;      /* tests: a GPU that has the extension takes the path of one that has not (Mali): z is remapped in the shaders */
    {   /* 0.1.8, Arm GPUs: math guard and strict position math on, as in the one start of 0.1.6 whose captures came back clean from a Mali
         * tester (with the format swap, fmt_sub, off). Optimiser and split barriers (0.1.7) stay off there until that state is confirmed: the optimiser
         * held a first start on the Joy-Con screen for minutes. SMO_VK_GUARD, SMO_VK_PRECISE, SMO_VK_OPT_FS, SMO_VK_SPLIT_BARRIER = 0/1 choose. */
        const char *eg = getenv("SMO_VK_GUARD"), *ep = getenv("SMO_VK_PRECISE"), *eo = getenv("SMO_VK_OPT_FS"), *es = getenv("SMO_VK_SPLIT_BARRIER");
        int arm = V.props.vendorID == 0x13B5;
        mx_guard = eg ? atoi(eg) != 0 : arm;
        mx_precise = ep ? atoi(ep) != 0 : arm;
        mx_opt_fs = eo && atoi(eo) != 0;
        g_split_bar = es && atoi(es) != 0;
        printf("nvn_vk: math guard %s, strict vertex math %s, shader optimiser %s, split pass barriers %s\n", mx_guard ? "on" : "off", mx_precise ? "on" : "off", mx_opt_fs ? "on" : "off", g_split_bar ? "on" : "off");
    }
    /* session 4: blend / mask / polygon / multisample state out of the pipeline key, only when the GPU has ALL of these (else all stay in the key) */
    D.has_eds3 = D.has_eds3 && D.eds3.extendedDynamicState3ColorBlendEnable && D.eds3.extendedDynamicState3ColorBlendEquation && D.eds3.extendedDynamicState3ColorWriteMask && D.eds3.extendedDynamicState3PolygonMode &&
                 D.eds3.extendedDynamicState3AlphaToCoverageEnable && D.eds3.extendedDynamicState3SampleMask && (D.eds3.extendedDynamicState3DepthClampEnable || !D.f2.features.depthClamp);
    printf("nvn_vk: extended dynamic state 3 (blend enable / equation / write mask, polygon mode, alpha to coverage, sample mask, depth clamp dynamic = not in the pipeline key): %s\n", D.has_eds3 ? "yes" : "no");
    D.has_gpl = D.has_gpl && D.gpl.graphicsPipelineLibrary && D.has_eds3 && D.has_vids;
    printf("nvn_vk: driver support for pipelines without a hold: VK_EXT_graphics_pipeline_library %s, VK_EXT_shader_object %s, extended dynamic state 3 (all seven states) %s, vertex input dynamic state %s | pipeline libraries in use: %s\n",
           D.ext_gpl ? "yes" : "NO", D.ext_so ? "yes" : "NO", D.has_eds3 ? "yes" : "NO", D.has_vids ? "yes" : "NO", D.has_gpl ? "YES" : "no");
    /* session 11: ONE line that says which path draws and why */
    {   const char *e = getenv("SMO_VK_GPL");
        int m = vk_gpl_mode();
        printf("nvn_vk_path: %s: %s (SMO_VK_GPL %s; default %d; SMO_VK_GPL=0 is the switch back to whole pipelines)\n",
               D.has_gpl && m >= 2 ? "STAGE LIBRARIES + preparation + loading barrier (a draw links, nothing is compiled in a fight)" : D.has_gpl ? "PAIR LIBRARIES (SMO_VK_GPL=1, a measurement mode)" : "WHOLE PIPELINES (made by the workers from the list of earlier runs; a first use can hold a frame)",
               !m ? (e ? "turned off by SMO_VK_GPL=0" : "off by default in this build") : D.has_gpl ? (e ? "asked for by SMO_VK_GPL" : "the default: the driver has VK_EXT_graphics_pipeline_library, extended dynamic state 3 and dynamic vertex input") :
               !D.ext_gpl ? "the driver has NO VK_EXT_graphics_pipeline_library: fallen back" : "the driver lacks extended dynamic state 3 or dynamic vertex input: fallen back", e ? e : "not set", VK_GPL_DEFAULT);
    }
    if (!D.f13.dynamicRendering || !D.f13.synchronization2) vkd_fail("the device lacks dynamicRendering / synchronization2");
    if (!D.has_vids) printf("nvn_vk: no VK_EXT_vertex_input_dynamic_state: the vertex layout is part of the pipeline key (strides stay dynamic)\n");
    if (!D.f2.features.textureCompressionBC) nvn_vk_missing("a GPU without BC texture compression");
    di->enabledExtensionCount = D.next; di->ppEnabledExtensionNames = D.exts; di->pNext = &D.f2;
    printf("nvn_vk: features: vertex input dynamic %d, custom border colour %d, provoking vertex last %d, 8-bit indices %d, list restart %d, ASTC %d, depth clamp %d, wide lines %d, robust buffers %d\n",
           D.has_vids, D.has_cbc, D.has_pv, D.has_iu8, D.has_plr, (int)D.f2.features.textureCompressionASTC_LDR, (int)D.f2.features.depthClamp, (int)D.f2.features.wideLines, (int)D.f2.features.robustBufferAccess);
}

/* ---------------------------------------------------------------- memory: blocks per memory type and class (64 MB, host-visible 16 MB), first fit */
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
typedef struct { VkDeviceMemory mem; uint64_t size; void *map; uint32_t type; int cls, nfree, used; struct { uint64_t off, size; } fr[512]; } VBlock;
static VBlock *g_blk[2048];
static int g_nblk;
static int64_t g_mem_total, g_mem_leak;
static VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls) {
    VMem m = {-1, 0, 0, NULL, NULL};
    uint32_t type = 32;
    for (uint32_t i = 0; i < D.mp.memoryTypeCount && type == 32; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == 32) vkd_fail("no memory type for flags 0x%x (bits 0x%x)", (unsigned)want, (unsigned)rq.memoryTypeBits);
    uint64_t al = rq.alignment < 256 ? 256 : rq.alignment;
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < g_nblk; b++) {
            VBlock *k = g_blk[b];
            if (!k || k->type != type || k->cls != cls) continue;
            for (int i = 0; i < k->nfree; i++) {
                uint64_t start = (k->fr[i].off + al - 1) / al * al, end = k->fr[i].off + k->fr[i].size;
                if (start + rq.size > end) continue;
                uint64_t head = start - k->fr[i].off, tail = end - (start + rq.size);
                if (head && tail) { if (k->nfree == 512) continue; memmove(&k->fr[i + 2], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree++; k->fr[i].size = head; k->fr[i + 1].off = start + rq.size; k->fr[i + 1].size = tail; }
                else if (head) k->fr[i].size = head;
                else if (tail) { k->fr[i].off = start + rq.size; k->fr[i].size = tail; }
                else { memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; }
                m.block = b; m.off = start; m.size = rq.size; m.mem = k->mem; m.map = k->map ? (uint8_t *)k->map + start : NULL; k->used++;
                return m;
            }
        }
        if (pass) break;
        int b = 0;
        while (b < g_nblk && g_blk[b]) b++;
        if (b == 2048) vkd_fail("out of memory blocks");
        VBlock *k = calloc(1, sizeof *k);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        /* large: a block of its own. Host-visible memory is system memory the driver keeps resident for the GPU: its blocks are
         * 16 MB (the small buffers, compute copy-backs and readbacks of a stage used under 1 MB of two 64 MB blocks) */
        uint64_t bsz = (D.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? (16u << 20) : (64u << 20);
        k->size = rq.size > (16u << 20) ? rq.size : bsz; k->type = type; k->cls = rq.size > (16u << 20) ? 100 + b : cls;
        ai.allocationSize = k->size; ai.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(V.device, &ai, NULL, &k->mem);
        if (r != VK_SUCCESS) vkd_fail("vkAllocateMemory of %llu MB (type %u): %s; %lld MB allocated so far", (unsigned long long)(k->size >> 20), type, vk_result_name(r), (long long)(g_mem_total >> 20));
        if (D.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(V.device, k->mem, 0, VK_WHOLE_SIZE, 0, &k->map));
        {   static int log = -1;
            if (log < 0) log = getenv("SMO_VK_MEMLOG") && atoi(getenv("SMO_VK_MEMLOG"));
            if (log) printf("nvn_vk_mem: block %d: %llu MB, type %u (flags 0x%x, heap %u), class %d, mapped at %p\n", b, (unsigned long long)(k->size >> 20), type,
                            (unsigned)D.mp.memoryTypes[type].propertyFlags, (unsigned)D.mp.memoryTypes[type].heapIndex, k->cls, k->map);
        }
        k->nfree = 1; k->fr[0].off = 0; k->fr[0].size = k->size;
        g_blk[b] = k; if (b == g_nblk) g_nblk++;
        g_mem_total += (int64_t)k->size;
        cls = k->cls;
    }
    vkd_fail("allocation of %llu bytes failed in a fresh block", (unsigned long long)rq.size);
    return m;
}
static void vmem_free(VMem m) {
    if (m.block < 0) return;
    VBlock *k = g_blk[m.block];
    if (--k->used == 0 && k->cls >= 100) { vkFreeMemory(V.device, k->mem, NULL); g_mem_total -= (int64_t)k->size; free(k); g_blk[m.block] = NULL; return; }
    int i = 0;
    while (i < k->nfree && k->fr[i].off < m.off) i++;
    if (i > 0 && k->fr[i - 1].off + k->fr[i - 1].size == m.off) { k->fr[i - 1].size += m.size; if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i - 1].size += k->fr[i].size; memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; } return; }
    if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i].off = m.off; k->fr[i].size += m.size; return; }
    if (k->nfree == 512) { g_mem_leak += (int64_t)m.size; return; }
    memmove(&k->fr[i + 1], &k->fr[i], sizeof k->fr[0] * (size_t)(k->nfree - i)); k->nfree++;
    k->fr[i].off = m.off; k->fr[i].size = m.size;
}
static VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkBuffer b;
    VkMemoryRequirements rq;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(V.device, &bi, NULL, &b));
    vkGetBufferMemoryRequirements(V.device, b, &rq);
    *mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VKC(vkBindBufferMemory(V.device, b, mem->mem, mem->off));
    return b;
}

/* ---------------------------------------------------------------- command buffers in flight */
#ifdef __ANDROID__
#define NSLOT 16                                   /* Android port: more, smaller submissions a frame (submit_chunk) */
#else
#define NSLOT 8
#endif
typedef struct { VkCommandBuffer cmd; VkCommandPool cpool; VkFence fence; VkEvent ev; VkDescriptorPool pool[16]; int npool, ipool, pending; uint64_t serial; int64_t ts_frame; uint64_t rlap0, rpos0, rlap1, rpos1; } Slot;      /* ev: set by the GPU at the end of the command buffer (slot_finished) */      /* rlap/rpos: the stream ring (lap, position) when it was begun and submitted */
typedef struct VTex VTex;
static struct {
    Slot slot[NSLOT]; int cur, recording; uint64_t serial, done;      /* serial: number of the last submission; done: every submission up to here has finished */
    VkCommandBuffer cmd;
    /* stream ring */
    VkBuffer ring; VMem ring_mem; uint64_t ring_pos, ring_size, ring_lap; uint32_t gen;
    struct { uint64_t addr, size, off; uint32_t gen, batch, wpos; } acache[8192];      /* wpos: host writes logged up to here were checked (ring_put) */
    VkBuffer zero; VMem zero_mem;
    /* pass */
    int in_pass, pass_w, pass_h;
    int pass_rs; float pass_sx, pass_sy;           /* track O: the pass draws into scaled images (pass_w, pass_h are the images'; sx, sy = image / game) */
    /* NVN state as left by the commands executed so far */
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_tex[6][32], c2[6][112], nvn_img[6][8];      /* nvn_img: nvnCommandBufferBindImage (NVN image handle = texture pool index) */
    uint32_t tex_int[6], tex_sint[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds; uint32_t chanmask, sample_mask;
    int sref[2], svmask[2], swmask[2];
    float alpha_ref; int alpha_func;
    uint8_t blend[8][8], blend_on, logic, poly[4], ms[24];
    float viewport[4], drange[2], blendc[4], point_size, line_width, polyoff[3];
    int scissor[4], have_viewport, have_scissor, rast_discard, depth_clamp, restart, render_off;
    int ncol; TargetRef col[8], depth; int have_targets;
    int64_t row_stride, image_stride;
    int dyn_dirty, vi_dirty, dyn3_dirty, dyn3_samples;      /* dyn3: the extended-dynamic-state-3 group (blend, masks, polygon mode, multisample) */
    void *pipe_bound;
    int64_t frame_draws, frame_skipped, frame_clears, pipelines, modules, uploads, upload_bytes, images, images_live, views, ring_wraps, feedback, flushes, sets;
    int64_t sets_reused, tset_collisions, dummy_kind;
    int64_t dispatches, cb_bytes, cb_syncs, cb_draws, cb_made, cb_waits, cb_wait_ticks, cb_guest_words, cb_conflict_words, readbacks;
} R;

static void pass_shot(void);
static void cmd_begin(void);
/* SMO debug aid SMO_VK_FRAMELOG=<first>-<last>: one line per frame of that range: draws, draws left out (and how many of
 * them because rendering was switched off), conditional render switches (and how many switched rendering off), render
 * passes, and a hash of the conditional results in order, to find frames that differ from their neighbours. */
static int64_t g_fr_cond, g_fr_condoff, g_fr_offskip, g_fr_passes, g_fr_texbar, g_fr_wraps, g_fr_ring0;
static uint64_t g_fr_condhash = 1469598103934665603ull;
/* SMO debug aid SMO_VK_PASSSHOT=<frame> or <first>-<last>[/<step>]: the frames whose render passes pass_shot saves and
 * whose draws are logged (a range finds a defect that shows in some frames only; its files carry the frame number). */
static long long g_ps_lo = -2, g_ps_hi, g_ps_step = 1;
static int g_diag_low;                                          /* 0.1.6: a round of a screen with few draws */
static int g_diag_mode, g_diag_busy;                            /* SMO 0.1.3 (diag_present): the frame being rendered is captured, 1 at its end, 2 after each pass */
static void passshot_parse(void) {
    const char *e = getenv("SMO_VK_PASSSHOT"), *d;
    g_ps_lo = -1;
    if (!e) return;
    g_ps_lo = g_ps_hi = atoll(e);
    if ((d = strchr(e, '-')) != NULL) { g_ps_hi = atoll(d + 1); if ((d = strchr(d, '/')) != NULL && atoll(d + 1) > 0) g_ps_step = atoll(d + 1); }
}
static int passshot_env(void) {
    if (g_ps_lo == -2) passshot_parse();
    long long f = nvn_front_frame_next();
    return g_ps_lo >= 0 && f >= g_ps_lo && f <= g_ps_hi && (f - g_ps_lo) % g_ps_step == 0;
}
static int passshot_frame(void) { return g_diag_mode == 2 || passshot_env(); }
static int passshot_range(void) { if (g_ps_lo == -2) passshot_parse(); return g_ps_hi > g_ps_lo; }
/* SMO port: NVN counter type 1, SAMPLES_PASSED (occlusion). Between nvnCommandBufferResetCounter and ReportCounter the
 * samples of every draw are counted. A Vulkan occlusion query begun inside a render pass must end in it, so the count is
 * made of segments: one query from the first draw of a pass while counting until the pass (or the counting) ends.
 * Segments are numbered on without end and use a ring of OCC_MAX queries. A report does not wait for the GPU: it is
 * queued with the segments recorded since the entry before it, and its {count, timestamp} (the sum since the last reset,
 * as nvn_gl.c's counter_stop / acc) is written into guest memory once the submission holding them has finished. Finished
 * submissions are looked for at each batch and while the render thread is idle; the GPU is waited for where a result is
 * needed: a conditional render that compares it, the game's SyncWait or Finish, and the end of a present (the game reads
 * the counts of one frame in the next: agl::cull::OcclusionQuery::getZPassCount). SMO_VK_OCC_SYNC=1 waits at each report
 * as before; SMO_VK_OCC_PRESENT=0 does not wait at the present. */
#define OCC_MAX 65536
#define OCC_Q 4096
typedef struct { uint64_t lo, hi, dst, serial, seq; int64_t frame; int kind, log; } OccEntry;     /* seq: its number among all entries queued (occ_hold) */     /* segments [lo, hi); kind 0 report (to dst), 1 reset (the segments are dropped), 2 fold (added up, nothing written) */
static VkQueryPool g_occ_pool;
static int g_occ_on, g_occ_open;                   /* counting; a segment is open */
static uint64_t g_occ_head, g_occ_tail, g_occ_mark, g_occ_seg_serial;   /* next segment; oldest segment in use; first segment of no entry yet; submission of the newest segment */
static OccEntry g_occ_q[OCC_Q];
static int g_occ_qh, g_occ_qn;                     /* oldest entry, entries queued */
static uint64_t g_occ_total;                       /* samples of the count the resolved entries belong to */
static int g_occ_sync = -1, g_occ_present = -1;
static int64_t g_occ_reports, g_occ_waits, g_occ_wait_ticks, g_occ_folds, g_occ_cond, g_occ_cond_waits, g_occ_present_waits, g_occ_late;
static int64_t g_occ_cond_late, g_occ_cond_kept;   /* conditional renders decided on the counts of the frame before; on the last decision for that address */
static void occ_end(void) { if (g_occ_open) { vkCmdEndQuery(R.cmd, g_occ_pool, (uint32_t)((g_occ_head - 1) % OCC_MAX)); g_occ_open = 0; } }
/* Android port, SMO_VK_WAITWHO=1: who ends the render passes (return address), printed with the 300-frame figures */
static struct { void *ra; int64_t n; } g_pe[24];
static int64_t g_pe_total, g_targets_kept;
static int g_pe_on = -1;
/* Android port, LOAD-CLEARS (SMO_VK_LOADCLEAR=0 turns them off): a clear of a whole attachment that comes before the pass
 * has begun is not recorded as vkCmdClearAttachments; the pass begins with LOAD_OP_CLEAR for it (pass_begin). On a tiled
 * mobile GPU that saves loading the attachment from memory only to overwrite it. If the pass would end without having
 * begun (the targets change, a copy, the present), it is begun and ended at once, which performs the clears. */
/* Android port, SMO_VK_PASSTIME=<frame>: the GPU time of every render pass of that frame. A timestamp at the bottom of the
 * pipe (everything before it finished) before each pass begins and after it ends: a pass's time is its own work, a
 * gap is what ran between two passes (barriers, copies, compute). Printed after the frame ("nvn_vk_passtime:"). */
#define PT_MAX 400
static int g_pt_frame = -2, g_pt_on, g_pt_n;
static VkQueryPool g_pt_pool;
static struct { int w, h, ncol, cfmt, dfmt; int64_t d0, d1; unsigned load, clear, disc, why; } g_pt[PT_MAX];     /* load / clear: attachments begun with LOAD / CLEAR; disc: discarded while the pass ran (bits 0..7 colour, 8 depth-stencil); why: what ended it */
static int g_pt_dout;                              /* discards outside a pass in that frame */
static void pt_mark(int end) {
    if (!g_pt_on || g_pt_n >= PT_MAX) return;
    vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt_pool, (uint32_t)(g_pt_n * 2 + end));
    if (end) { g_pt[g_pt_n].d1 = R.frame_draws; g_pt_n++; }
}
static unsigned g_lc_mask;                          /* bits 0..7 colour attachments, 8 depth, 9 stencil */
static VkClearColorValue g_lc_col[8];
static VkClearDepthStencilValue g_lc_ds;
static int g_lc_busy;
static int64_t g_lc_done, g_lc_flushed;
static int pass_begin(void);
static int loadclear_on(void) { static int on = -1; if (on < 0) on = !(getenv("SMO_VK_LOADCLEAR") && !atoi(getenv("SMO_VK_LOADCLEAR"))); return on; }
__attribute__((noinline)) static void pass_end(void) {
    if (!R.in_pass && g_lc_mask && !g_lc_busy) { g_lc_busy = 1; g_lc_flushed++; pass_begin(); g_lc_busy = 0; g_lc_mask = 0; }      /* pending clears: performed by an empty pass (dropped if it cannot begin) */
    if (!R.in_pass) return;
    if (g_pe_on < 0) g_pe_on = getenv("SMO_VK_WAITWHO") && atoi(getenv("SMO_VK_WAITWHO"));
    if (g_pe_on > 0) {
        void *ra = __builtin_return_address(0);
        g_pe_total++;
        for (int i = 0; i < 24; i++) if (g_pe[i].ra == ra || !g_pe[i].ra) { g_pe[i].ra = ra; g_pe[i].n++; break; }
    }
    occ_end(); vkCmdEndRendering(R.cmd); R.in_pass = 0; pt_mark(1); pass_shot();
    if (!R.recording) cmd_begin();                 /* SMO 0.1.3: a capture read the targets, which submits: callers (cmd_submit itself) go on with the command buffer being recorded */
}
static int64_t g_tm[12];                          /* ticks: 0 wait for a submission, 1 copy-back, 2 dispatch, 3 batch replay, 4 present, 5 pipelines, 6 modules, 7 copy-back wait at batch start, 8 draw preparation */
static int64_t tk(void) { plat_li t; ((t).QuadPart = plat_qpc()); return t.QuadPart; }
/* SMO port: report timestamps, the clock nvn_gl.c's now_ns uses (performance counter in nanoseconds) */
static uint64_t vk_now_ns(void) {
    static plat_li f;
    plat_li t;
    if (!f.QuadPart) ((f).QuadPart = plat_qpf());
    ((t).QuadPart = plat_qpc());
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}
/* Session 5, SMO_VK_GPUTIME=1: two timestamp queries per command buffer (written at its first and after its last command),
 * read when the buffer has finished; "nvn_vk_gpu:" line every 300 frames. The span of a buffer includes the time the GPU
 * gave to other processes while it ran. */
static int g_ts_on = -1;
static VkQueryPool g_ts_pool;
static double g_ts_ns;
static int64_t g_ts_n;
/* Session 9: the same per FRAME (the frame a buffer was recorded for), with the time between two presents and whether the frame
 * was a battle frame (compute dispatches and more than 120 draws): median / p99 of the battle frames at exit ("nvn_vk_gpu_fight:"). */
#define FRSTAT_N 40000
static float g_fr_gpu[FRSTAT_N], g_fr_wall[FRSTAT_N];
static uint8_t g_fr_fight[FRSTAT_N];
static int64_t g_cur_frame, g_battle_frames;
void nvn_tl(int ev, int arg);                     /* nvn_gl.c: SMO_FRAMETL (13/14: a wait for the GPU begins / ends) */
/* Android port, SMO_VK_WAITWHO=1: which callers of slot_wait the render thread waits in (return address, time), printed
 * with the 300-frame figures (tools: llvm-symbolizer on the unstripped libsmo.so) */
static struct { void *ra; int64_t tk, n; } g_ww[16];
static int g_ww_on = -1;
__attribute__((noinline)) static void ww_note(void *ra, int64_t t) {
    for (int i = 0; i < 16; i++) if (g_ww[i].ra == ra || !g_ww[i].ra) { g_ww[i].ra = ra; g_ww[i].tk += t; g_ww[i].n++; return; }
}
static void ww_print(void) {
    plat_li fq; ((fq).QuadPart = plat_qpf());
    for (int i = 0; i < 16 && g_ww[i].ra; i++) {
        if (g_ww[i].n) printf("nvn_vk_waitwho: libsmo.so+0x%llx: %lld waits, %.1f ms in 300 frames\n", (unsigned long long)((uintptr_t)g_ww[i].ra - plat_module_base((void *)ww_print)), (long long)g_ww[i].n, (double)g_ww[i].tk * 1000.0 / (double)fq.QuadPart);
        g_ww[i].tk = g_ww[i].n = 0;
    }
}
/* 0.1.7, THE GAME'S GPU TIMERS (nvnCommandBufferReportCounter, timestamp type, a pair a frame). The game reads them for its quality setting, which
 * caps the 3D size. They held the host clock in nanoseconds at replay time. Two things wrong, both found by Power Moon Recomp (MIT, docs/licenses/):
 * replaying commands on the CPU counted as GPU time, and the unit is the Switch GPU's ticks (614.4 MHz), so every time read 1.63x too long.
 * Each report now also writes a GPU timestamp; when its command buffer is done the GPU's time replaces the provisional host one, in the
 * host clock's terms (offset: a timestamp cannot be later than the moment its buffer is seen done). SMO_VK_COUNTER=cpu: host clock in
 * ticks; =legacy: as before 0.1.7. */
#define CQ_MAX 16
static VkQueryPool g_cq_pool;
static int g_cq_mode = -1, g_cq_n[NSLOT], g_cq_cal;  /* mode: 0 host clock, 1 GPU, 2 legacy */
static uint64_t g_cq_addr[NSLOT][CQ_MAX];
static int64_t g_cq_off;
static void cq_resolve(Slot *s);
static void slot_wait(Slot *s) {
    if (!s->pending) return;
    int64_t t0 = tk();
    if (g_ww_on < 0) g_ww_on = getenv("SMO_VK_WAITWHO") && atoi(getenv("SMO_VK_WAITWHO"));
    nvn_tl(13, (int)(s - R.slot));
    VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, 20000000000ull);
    nvn_tl(14, (int)(s - R.slot));
    g_tm[0] += tk() - t0;
    if (g_ww_on > 0) ww_note(__builtin_return_address(0), tk() - t0);
    if (r != VK_SUCCESS) vkd_fail("a submitted command buffer did not finish within 20 s (%s): the GPU is stuck or the device is lost", vk_result_name(r));
    VKC(vkResetFences(V.device, 1, &s->fence));
    if (s->ev) VKC(vkResetEvent(V.device, s->ev));
    if (g_ts_on > 0) {                             /* SMO_VK_GPUTIME=1: the GPU's own clock at the first and the last command of this buffer */
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(V.device, g_ts_pool, (uint32_t)(s - R.slot) * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) { double ns = (double)(ts[1] - ts[0]) * (double)V.props.limits.timestampPeriod; g_ts_ns += ns; g_ts_n++; if (s->ts_frame >= 0 && s->ts_frame < FRSTAT_N) g_fr_gpu[s->ts_frame] += (float)(ns / 1e6); }
    }
    cq_resolve(s);
    s->pending = 0;
    if (s->serial > R.done) R.done = s->serial;
}
static void garbage_collect(void);
static uint32_t g_tset_gen = 1;                   /* texture descriptor sets live as long as the command buffer whose pools they come from */
static int g_tset_used, g_tset_list_used;
static int g_bset_used, g_bset_list_used;      /* the same for the buffer sets of devices without push descriptors (draw_prepare, dyn0_pick) */
static int64_t g_bset_hits, g_bset_made;
static int64_t g_sub_draws0, g_chunk_submits;    /* Android port: the frame's draw count when this command buffer began (submit_chunk) */
static void cmd_begin(void) {
    if (R.recording) return;
    g_sub_draws0 = R.frame_draws;
    g_tset_gen++; g_tset_used = 0; g_tset_list_used = 0; g_bset_used = 0; g_bset_list_used = 0;
    R.cur = (R.cur + 1) % NSLOT;
    Slot *s = &R.slot[R.cur];
    slot_wait(s);
    for (int i = 0; i < s->npool; i++) VKC(vkResetDescriptorPool(V.device, s->pool[i], 0));
    s->ipool = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandPool(V.device, s->cpool, 0));      /* the slot's own pool, kept memory and all (vkResetCommandBuffer freed and */
    VKC(vkBeginCommandBuffer(s->cmd, &bi));              /* reallocated it: ~1.2 ms a frame on Qualcomm's driver, 2026-11-03) */
    s->rlap0 = R.ring_lap; s->rpos0 = R.ring_pos;
    R.cmd = s->cmd; R.recording = 1; R.in_pass = 0; R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1;
    if (g_ts_on < 0) {
        g_ts_on = getenv("SMO_VK_GPUTIME") && atoi(getenv("SMO_VK_GPUTIME")) && V.props.limits.timestampComputeAndGraphics;
        if (g_ts_on) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * 2; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_ts_pool)); }
    }
    s->ts_frame = g_cur_frame;
    if (g_cq_mode < 0) {
        const char *e = getenv("SMO_VK_COUNTER");
        extern int nvn_counter_ticks;
        g_cq_mode = e && !strcmp(e, "legacy") ? 2 : e && !strcmp(e, "cpu") ? 0 : V.props.limits.timestampComputeAndGraphics ? 1 : 0;
        nvn_counter_ticks = g_cq_mode != 2;
        if (g_cq_mode == 1) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * CQ_MAX; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_cq_pool)); }
        printf("nvn_vk: the game's GPU timers: %s\n", g_cq_mode == 1 ? "GPU timestamps, in ticks" : g_cq_mode == 0 ? "host clock, in ticks" : "host clock, in nanoseconds (as before 0.1.7)");
    }
    if (g_cq_pool) { g_cq_n[R.cur] = 0; vkCmdResetQueryPool(s->cmd, g_cq_pool, (uint32_t)R.cur * CQ_MAX, CQ_MAX); }
    if (g_ts_on > 0) { vkCmdResetQueryPool(s->cmd, g_ts_pool, (uint32_t)R.cur * 2, 2); vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2); }
    garbage_collect();
}
/* Ends the command buffer being recorded and gives it to the queue. Returns its number (the last one when nothing was recorded). */
/* Android port, POLLING WITHOUT THE KERNEL: vkGetFenceStatus of Turnip (msm kgsl) is a wait ioctl in the kernel, and the
 * polls that must never wait (cb_poll, occ_resolve(0): compute results and occlusion counts that have landed) spent much
 * of the render thread's time there: a profile on a Galaxy S24 Ultra with off-CPU time had them as the largest wait,
 * ending only when the GPU finished. A command buffer now sets an event at its end (after everything in it, without
 * holding later work as a barrier would); an event's status is read from memory. SMO_VK_EVENT_POLL=0: fences, as before. */
static int slot_finished(Slot *s) {
    if (s->ev) return vkGetEventStatus(V.device, s->ev) == VK_EVENT_SET;
    return vkGetFenceStatus(V.device, s->fence) == VK_SUCCESS;
}
static uint64_t cmd_submit(VkSemaphore wait, VkSemaphore signal) {
    if (!R.recording) { if (!wait && !signal) return R.serial; cmd_begin(); }
    pass_end();
    Slot *s = &R.slot[R.cur];
    if (g_ts_on > 0) vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2 + 1);
    if (s->ev) vkCmdSetEvent(s->cmd, s->ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VKC(vkEndCommandBuffer(s->cmd));
    VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait) { sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &wait; sb.pWaitDstStageMask = &stage; }
    if (signal) { sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &signal; }
    sb.commandBufferCount = 1; sb.pCommandBuffers = &s->cmd;
    nvn_tl(15, (int)R.frame_draws);               /* SMO_FRAMETL: a submission, with the draws of the frame so far */
    VKC(vkQueueSubmit(V.queue, 1, &sb, s->fence));
    s->pending = 1; s->serial = ++R.serial;
    s->rlap1 = R.ring_lap; s->rpos1 = R.ring_pos;
    R.recording = 0; R.cmd = NULL; R.flushes++;
    return R.serial;
}
/* Android port, CHUNKED SUBMISSIONS: a batch of the game's commands was recorded whole and then submitted, and the GPU
 * waited for it: measured on a Galaxy S24 Ultra (SMO_VK_PASSTIME), one gap of 8.2 ms in a 24 ms frame, 15.7 ms of GPU
 * work in all. Between two passes, once the command buffer holds SMO_VK_SUBMIT_DRAWS draws (0 = off), it is submitted and the
 * next one begins: the GPU works on the start of a batch while its end is recorded. Default on Android 150; 0.1.8, Adreno: 15. With a
 * frame split that finely the S24's governor held 310 MHz in every run (at 150 it often sat at 231 MHz, 45 against 57 fps). Not measured on Mali. */
static void submit_chunk(void) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("SMO_VK_SUBMIT_DRAWS");
#ifdef __ANDROID__
        every = e ? atoi(e) : V.props.vendorID == 0x5143 ? 15 : 150;
#else
        every = e ? atoi(e) : 0;
#endif
    }
    if (every > 0 && R.recording && !R.in_pass && R.frame_draws - g_sub_draws0 >= every) { cmd_submit(NULL, NULL); g_chunk_submits++; }
}
static void cmd_wait_all(void) { cmd_submit(NULL, NULL); for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]); R.done = R.serial; }

/* ---------------------------------------------------------------- stream ring */
#define RING_ALIGN 256
static void cb_ring_wrap(void);
/* SMO port: at a wrap the ring used to wait for the GPU to finish everything (cmd_wait_all), about every 13 frames in
 * Cascade Kingdom (256 MB, about 20 MB streamed a frame): a frame of 20+ ms each time. What is written from position 0 on
 * was written in the lap before by submissions long finished, so now a submission still running is waited for only when
 * what it read from the lap before overlaps the bytes about to be written. The compute write masks stay where they are
 * until the copy-back reads them (cb_land, at the next batch at the latest), long before the ring comes round to them
 * again. SMO_VK_RING_DRAIN=1: wait for everything at a wrap, as before. */
static int g_ring_drain = -1;
static int64_t g_ring_waits, g_ring_bytes;
/* The range caches (the ring's copies of guest ranges, the largest index of an index range) hold for one generation,
 * R.gen: a new batch starts one (the game writes its memory between batches), and so does a ring wrap. The host's own
 * writes into guest memory during a batch (UpdateUniformBuffer, reports and timestamps, occlusion results, buffer copies
 * and clears, compute results copied back) used to start one as well, about 60 times a frame in Cascade Kingdom (52 of
 * them UpdateUniformBuffer), and 9 of the 22 MB the ring copied each frame were ranges copied again within their batch.
 * Now such a write is logged with its range, and a cached range is checked against the writes logged since it was copied
 * or last checked: only a range a write overlaps is copied (or scanned) again. A full log starts a new generation.
 * Counted for the perf line every 300 frames (why: 0 batch, 1 ring wrap, 2 copy-back of compute results, 3 occlusion
 * results, 4 UpdateUniformBuffer, 5 report value or timestamp, 6 buffer copy or clear, 7 texture read into a buffer,
 * 8 write log full, 9 a logged write overlaps the index range of a draw: index_max keeps its cache by generation only). */
enum { GEN_BATCH, GEN_WRAP, GEN_CB, GEN_OCC, GEN_UBO, GEN_REPORT, GEN_BUF, GEN_TEXBUF, GEN_LOGFULL, GEN_INDEXW, GEN_N };
static int64_t g_gen_why[GEN_N], g_wlog_why[GEN_N];
#define WLOG_N 1024
static struct { uint64_t lo, hi; } g_wlog[WLOG_N];
static uint32_t g_wlog_n;                          /* host writes logged in this generation */
#define GEN_BUMP(why) (R.gen++, g_wlog_n = 0, g_gen_why[why]++)
static void host_wrote(uint64_t addr, uint64_t size, int why) {
    if (!size) return;
    if (g_wlog_n == WLOG_N) { GEN_BUMP(GEN_LOGFULL); return; }
    g_wlog[g_wlog_n].lo = addr; g_wlog[g_wlog_n].hi = addr + size; g_wlog_n++;
    g_wlog_why[why]++;
}
static int wlog_overlap(uint32_t from, uint64_t lo, uint64_t hi) {      /* a write logged since `from` overlaps [lo, hi) */
    for (uint32_t w = from; w < g_wlog_n; w++) if (g_wlog[w].lo < hi && lo < g_wlog[w].hi) return 1;
    return 0;
}
static int64_t g_rput_calls, g_rput_hits, g_rput_again, g_rput_again_bytes;      /* ring_put: calls, cache hits, copies of a range already copied in this batch */
static uint32_t g_batch_no;
static void ring_reuse(uint64_t lo, uint64_t hi) {   /* [lo, hi) of this lap is about to be written: whoever read it in the lap before has finished */
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending) continue;                 /* finished, or the command buffer being recorded (pending only once submitted) */
        uint64_t a, b;                             /* what the submission wrote in the lap before this one */
        if (s->rlap0 + 1 == R.ring_lap) { a = s->rpos0; b = s->rlap1 + 1 == R.ring_lap ? s->rpos1 : R.ring_size; }
        else continue;                             /* begun in this lap (behind the write position) or two laps ago (finished: slots are reused in turn) */
        if (a < hi && lo < b) { slot_wait(s); g_ring_waits++; }
    }
}
static uint64_t ring_raw(const void *p, uint64_t n) {
    R.ring_pos = (R.ring_pos + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1);
    if (n > R.ring_size / 4) vkd_fail("a range of %llu bytes does not fit the stream ring (%llu MB)", (unsigned long long)n, (unsigned long long)(R.ring_size >> 20));
    if (g_ring_drain < 0) g_ring_drain = getenv("SMO_VK_RING_DRAIN") && atoi(getenv("SMO_VK_RING_DRAIN"));
    if (R.ring_pos + n > R.ring_size) {
        if (g_ring_drain) { cmd_wait_all(); cb_ring_wrap(); }      /* everything that reads the ring has finished */
        R.ring_pos = 0; R.ring_lap++; GEN_BUMP(GEN_WRAP); R.ring_wraps++; g_fr_wraps++;
    }
    if (!g_ring_drain && R.ring_lap) ring_reuse(R.ring_pos, R.ring_pos + n);
    uint64_t off = R.ring_pos;
    if (p) memcpy((uint8_t *)R.ring_mem.map + off, p, (size_t)n);
    R.ring_pos += n; g_ring_bytes += (int64_t)n;
    return off;
}
static uint64_t ring_put(uint64_t addr, uint64_t size) {
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    g_rput_calls++;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (R.acache[i].gen == R.gen && R.acache[i].addr == addr && R.acache[i].size == size) {
            if (R.acache[i].wpos == g_wlog_n || !wlog_overlap(R.acache[i].wpos, addr, addr + size)) { R.acache[i].wpos = g_wlog_n; g_rput_hits++; return R.acache[i].off; }
            h = i; break;                          /* the host wrote into the range since it was copied: copied again, same entry */
        }
        if (R.acache[i].gen != R.gen) { h = i; break; }
    }
    h &= 8191;
    if (R.acache[h].addr == addr && R.acache[h].size == size && R.acache[h].batch == g_batch_no) { g_rput_again++; g_rput_again_bytes += (int64_t)size; }
    uint64_t off = ring_raw(P(addr), size);
    R.acache[h].addr = addr; R.acache[h].size = size; R.acache[h].off = off; R.acache[h].gen = R.gen; R.acache[h].batch = g_batch_no; R.acache[h].wpos = g_wlog_n;      /* after ring_raw: a wrap starts a generation */
    return off;
}
/* Constant buffer 2 of a stage (R.c2: the texture sizes the translated shaders read) in the ring: copied again only when
 * a texture binding changed it or a new generation began (it used to be copied at every draw: 2.2 MB a frame). */
static struct { uint64_t off; uint32_t gen; int valid; } g_c2r[6];
static uint64_t c2_ring(int s) {
    if (g_c2r[s].valid && g_c2r[s].gen == R.gen) return g_c2r[s].off;
    uint64_t off = ring_raw(R.c2[s], sizeof R.c2[s]);
    g_c2r[s].off = off; g_c2r[s].gen = R.gen; g_c2r[s].valid = 1;
    return off;
}

/* ---------------------------------------------------------------- formats */
static VkFormat comp_fmt(int comps, int i) {
    static const int b8[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}, b16[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
                     b32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const struct { uint8_t w, o; } m[12] = {{8, 0}, {8, 1}, {8, 4}, {8, 5}, {16, 6}, {16, 0}, {16, 1}, {16, 4}, {16, 5}, {32, 2}, {32, 0}, {32, 1}};      /* offsets: UNORM 0 SNORM 1 USCALED 2 SSCALED 3 unsigned 4 SINT 5 SFLOAT 6; 32-bit: unsigned 0 SINT 1 SFLOAT 2 */
    return (VkFormat)((m[i].w == 8 ? b8 : m[i].w == 16 ? b16 : b32)[comps - 1] + m[i].o);
}
/* The optimal-tiling features of a format. SMO_VK_SIM_FMT=<vulkan format>:<letters>,... takes features away, to see on
 * a GPU that has them what another GPU does without (b blending, l linear filtering, c colour target, s storage), e.g.
 * Mali-G615: SMO_VK_SIM_FMT=100:b,109:b,78:l */
static VkFormatFeatureFlags fmt_feats(VkFormat f) {
    static VkFormatFeatureFlags cache[256]; static uint8_t have[256];
    static char sim[256]; static int sim_init;
    if (!sim_init) {
        sim_init = 1;
        for (const char *e = getenv("SMO_VK_SIM_FMT"); e && *e; ) {
            int n = atoi(e); const char *c = strchr(e, ':');
            if (!c) break;
            for (c++; *c && *c != ','; c++) if (n > 0 && n < 256) sim[n] |= *c == 'b' ? 1 : *c == 'l' ? 2 : *c == 'c' ? 4 : *c == 's' ? 8 : 0;
            e = *c ? c + 1 : c;
        }
    }
    if ((unsigned)f < 256 && have[f]) return cache[f];
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(V.gpu, f, &p);
    VkFormatFeatureFlags r = p.optimalTilingFeatures;
    if ((unsigned)f < 256) {
        if (sim[f] & 1) r &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (sim[f] & 2) r &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (sim[f] & 4) r &= ~(VkFormatFeatureFlags)(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        if (sim[f] & 8) r &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        cache[f] = r; have[f] = 1;
    }
    return r;
}
static int fmt_supported(VkFormat f, VkFlags need) { return (fmt_feats(f) & need) == need; }
/* SMO 0.1.3, off since 0.1.8 and no longer an option (SMO_VK_FMT_SUB=1, for tests): a Mali tester's visual errors went away with it off, at a small cost in fps. On a GPU that cannot filter or blend what the game
 * does with these formats, the render target is made in a format that can. R16G16_SNORM (filtered) and R32_SFLOAT (blended) as
 * R16G16_SFLOAT, RGBA32F (blended) as RGBA16F; the precision is less, the sampled values are the same. Not for textures with
 * data from the game's files (vt_get2 keeps those). */
static int g_fmt_sub = -1;
static VkFormat fmt_sub(int nf) {
    if (g_fmt_sub < 0) { const char *e = getenv("SMO_VK_FMT_SUB"); g_fmt_sub = e && atoi(e) != 0; }
    if (!g_fmt_sub) return VK_FORMAT_UNDEFINED;
    if (nf == 0x13 && !fmt_supported(VK_FORMAT_R16G16_SNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return VK_FORMAT_R16G16_SFLOAT;
    if (nf == 0x0a && !fmt_supported(VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) return VK_FORMAT_R16G16_SFLOAT;
    if (nf == 0x2e && !fmt_supported(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) return VK_FORMAT_R16G16B16A16_SFLOAT;
    return VK_FORMAT_UNDEFINED;
}
/* NVN texture format -> Vulkan. *expand = 1 + component kind when a 3-component format is stored with 4; *decode = 1 when the texels are decoded to RGBA8 on the CPU. */
static VkFormat tex_vkfmt(int nf, int *expand, int *decode) {
    static int d24 = -1, d24s8, astc;
    if (d24 < 0) { d24 = fmt_supported(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); d24s8 = fmt_supported(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); astc = D.f2.features.textureCompressionASTC_LDR != 0; }
    *expand = 0; *decode = 0;
    { VkFormat sf = fmt_sub(nf); if (sf) return sf; }
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (comps == 3) { *expand = 1 + i; comps = 4; }
        return comp_fmt(comps, i);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        if (astc) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * ((nf - 0x79) % 14) + (nf >= 0x87));
        *decode = 1;
        return nf >= 0x87 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    if (nf >= 0x41 && nf <= 0x50) {
        /* Android port: a GPU without BC texture compression (Mali, PowerVR; SMO_VK_NO_BC=1 forces it, to test this on a GPU
         * that has it): the blocks are decoded on the CPU when the texture is read (nvn_front_tex_read, tex_decode_to) into
         * the smallest format that keeps what the GPU would have sampled */
        static int bc = -1;
        if (bc < 0) {
            bc = D.f2.features.textureCompressionBC && !(getenv("SMO_VK_NO_BC") && atoi(getenv("SMO_VK_NO_BC")));
            printf("nvn_vk: BC textures: %s\n", bc ? "the GPU reads them" : "decoded on the CPU (BC1-3/7 to RGBA8, BC4 to R8, BC5 to RG8, BC6H to RGBA16F)");
        }
        if (!bc) {
            if (nf <= 0x44 || nf == 0x4D) { *decode = TEX_DEC_RGBA8; return VK_FORMAT_R8G8B8A8_UNORM; }
            if (nf <= 0x48 || nf == 0x4E) { *decode = TEX_DEC_RGBA8; return VK_FORMAT_R8G8B8A8_SRGB; }
            switch (nf) {
            case 0x49: *decode = TEX_DEC_R8; return VK_FORMAT_R8_UNORM; case 0x4A: *decode = TEX_DEC_R8_SNORM; return VK_FORMAT_R8_SNORM;
            case 0x4B: *decode = TEX_DEC_RG8; return VK_FORMAT_R8G8_UNORM; case 0x4C: *decode = TEX_DEC_RG8_SNORM; return VK_FORMAT_R8G8_SNORM;
            default: *decode = TEX_DEC_RGBA16F; return VK_FORMAT_R16G16B16A16_SFLOAT;    /* 0x4F, 0x50: BC6H */
            }
        }
    }
    switch (nf) {
    case 0x31: return VK_FORMAT_S8_UINT; case 0x32: return VK_FORMAT_D16_UNORM; case 0x33: return d24 ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT; case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT; case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB; case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 0x3A: case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK; case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK; case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK; case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK; case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK; case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK; case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK; case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK; case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM; case 0x9B: case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}
static VkFlags fmt_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkFormat vertex_vkfmt(int nf) {
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) return comp_fmt(nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12);
    if (nf >= 0x51 && nf <= 0x68) {
        int comps = (nf - 0x51) / 6 + 1, t = (nf - 0x51) % 6;
        if (t >= 4) return VK_FORMAT_UNDEFINED;                       /* 32-bit scaled: Vulkan has none */
        return (VkFormat)(comp_fmt(comps, t < 2 ? 0 : 5) + 2 + (t & 1));
    }
    switch (nf) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

/* ---------------------------------------------------------------- images */
typedef struct VView { EView e; int vtype, attach; VkImageView view; struct VView *next; } VView;
static int64_t g_storage_tex;                       /* SMO: textures whose image has storage use (vt_get2) */
struct VTex {
    uint32_t magic; Tex *t; VkImage img; VMem mem; VkFormat fmt; VkImageLayout layout; VkFlags aspect;
    int w, h, d, levels, layers, samples, expand, decode, is3d, feedback, bpb, bw, bh;
    int rs; float sx, sy;                          /* track O: rs = 1 when the image is smaller than the game's texture (w, h are the image's; sx, sy = image / game) */
    int sub;                                       /* SMO 0.1.3: fmt_sub chose the format */
    int storage;                                   /* SMO: made with VK_IMAGE_USAGE_STORAGE_BIT (NVN_TEXTURE_FLAGS_IMAGE): it can be bound as an image */
    const void *gmem;                              /* SMO 0.1.4: the game's memory of the texture, when what the GPU draws here should outlive the texture object (the note above vk_drop); else NULL */
    int carried, kept; int64_t stamp;              /* its contents came from the image before it / it waits in g_keep, its texture gone (stamp: the frame its texture was last used) */
    VView *views; VTex *next, *prev;
};
/* ---- Track O: internal resolution scale (graphics.ini resolution_scale; SMO_VK_SCALE wins; plan\phases\O1_options.md, session 7).
 * A texture of the screen class (2D, one layer, attachable, the game's size exactly 1920x1080 >> k, k = 0..6) gets an image
 * of round(size * scale); everything the game says in ITS pixels about such an image is converted where it is used:
 * viewport, scissor, clear rectangles, copy / blit / resolve / downsample regions, uploads and read-backs (through a
 * temporary image of the game's size), the crop of the present. The size table of the shaders (c2) and the regions the game
 * reads keep the game's sizes. Fragment stages are translated with MxIn.screen_scale (another hash: ssrc_hb). At 1.0 none
 * of this runs: every branch is behind RS_ON or VTex.rs / R.pass_rs, which are then 0. */
static float g_rs = -1.0f;
static int64_t g_rs_images, g_rs_stages, g_rs_fcs, g_rs_tf, g_rs_up, g_rs_down, g_rs_blits;
static float rs_scale(void) {
    if (g_rs < 0) {
        const char *e = getenv("SMO_VK_SCALE");
        float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
        if (v <= 0) v = gfx_options()->handheld ? 1.0f : 1.2f;      /* Android port, resolution_scale = auto: docked 1920x1080 (SMO draws 1600x900), handheld 1280x720 */
        if (v < 0.25f) v = 0.25f;
        if (v > 2.0f) v = 2.0f;                    /* Android port: up to 2.0, drawing above the game's size as an emulator's resolution multiplier does */
        g_rs = v;
    }
    return g_rs;
}
#define RS_ON (rs_scale() != 1.0f)
/* Android port: SMO's screen sizes too. Its docked picture is 1600x900 with its 3D at 800x450, handheld 1280x720: with
 * 1920x1080 only (ssbu's size) no texture of SMO was ever scaled and resolution_scale did nothing. A size that halves to
 * an odd number rounds down, as the game's own mip-style chains do (1600x900 >> 3 = 200x112). */
static int rs_screen_class(int w, int h) {
    static const int base[3][2] = {{1920, 1080}, {1600, 900}, {1280, 720}};
    for (int b = 0; b < 3; b++) for (int k = 0; k < 7; k++) if (w == base[b][0] >> k && h == base[b][1] >> k) return 1;
    return 0;
}
/* A coordinate of the game's grid on a scaled image (nearest image texel edge, inside 0..max). */
static int rs_edge(int v, float s, int max) { int r = (int)floorf((float)v * s + 0.5f); return r < 0 ? 0 : r > max ? max : r; }
/* Session 9: the textures of the game's window (nvn_gl.c win_initialize keeps the list; also noted here at every present).
 * SMO_VK_UI_NATIVE=1 (resolution scale, experiment): a window texture is NOT scaled, so what the game draws straight into it
 * (its last full-screen pass and the 2D interface after it) is drawn at the game's 1920x1080. */
int nvn_front_tex_is_window(const Tex *t);
static Tex *g_wintex[8];
static int64_t g_rs_mixed;
static int wintex_is(const Tex *t) { if (nvn_front_tex_is_window(t)) return 1; for (int i = 0; i < 8; i++) if (g_wintex[i] == t) return 1; return 0; }
static void wintex_note(Tex *t) { if (!t || wintex_is(t)) return; for (int i = 0; i < 8; i++) if (!g_wintex[i]) { g_wintex[i] = t; return; } }
static int rs_ui_native(void) { static int on = -1; if (on < 0) on = getenv("SMO_VK_UI_NATIVE") && atoi(getenv("SMO_VK_UI_NATIVE")); return on; }
static VTex *g_vtex_all;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap;
static VkFlags layout_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT; case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT; case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}
/* 0.1.7 (g_split_bar, Arm GPUs, SMO_VK_SPLIT_BARRIER=0/1). On a tiler an ALL_COMMANDS -> ALL_COMMANDS barrier at each pass empties the GPU: the next
 * pass's geometry waits for the last fragment of the one before. A target going to be sampled, or back to being a target, only orders the
 * stages that touch it. LAS Recomp measured 14.8 to 13.65 ms a frame on a Mali-G610; Arm's best practices say the same. Two assumptions:
 *  - no vertex stage samples a render target: bind turns this off for the session, and says so, if one does;
 *  - compute reads what fragments wrote only behind a barrier of its own: g_cs_debt, paid in compute_dispatch_run.
 * Copies keep ALL_COMMANDS: their layout change comes through the path below. */
static void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    DBG_ASSERT(!R.in_pass, "debug: an image layout change inside a render pass");
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = layout_access(from); b.dstAccessMask = layout_access(to); b.oldLayout = from; b.newLayout = to;
    VkFlags att = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    int from_att = from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || from == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, to_att = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || to == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (g_split_bar && from_att && to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        vkCmdPipelineBarrier(R.cmd, att, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        g_cs_debt = 1;
    } else if (g_split_bar && from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && to_att)
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, att, 0, 0, NULL, 0, NULL, 1, &b);
    else
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void vt_layout(VTex *vt, VkImageLayout to) {
    if (vt->layout == to) return;
    pass_end();
    cmd_begin();
    image_barrier(vt->img, vt->aspect, vt->layout, to);
    vt->layout = to;
}
/* SMO port: nvnCommandBufferBarrier with ORDER_FRAGMENTS (2) / ORDER_FRAGMENTS_TILED (4): the draws after it read what
 * the draws before it rendered (a texture that is also a render target of the pass, or was one of the pass before
 * without a change of layout, e.g. GENERAL). The OpenGL path answers with glTextureBarrier; here the pass is ended and
 * every attachment / shader / copy write so far is made visible to every read after it (the next draw begins the pass
 * again, its targets loaded). Between passes the layout changes already order what one pass writes and the next reads;
 * the case left is a pass that samples its own target (GENERAL layout), where without a barrier the texture cache may
 * still hold what was there before. SMO_VK_NO_TEXBARRIER=1: as before (no barrier). */
static int64_t g_texbar, g_texbar_split, g_texbar_skip;
/* SMO port: the vertices a draw really reads. nvnCommandBufferBindVertexBuffer names a range, and some of SMO's draws read
 * past it: the composite passes of its half-size buffers (programs #12026 and #23331 in Cascade Kingdom, and 15 more at
 * the title screen) draw 9 vertices from a range of 36 bytes, 3 vertices of 12; the other 6 follow in the game's memory,
 * which is what the console reads. The copy into the stream ring stopped at the range, so the GPU took those 6 vertices
 * from whatever the ring held after it: stray triangles in the half-size buffer, upscaled into a quarter or a half of
 * the screen for a frame (the flicker: single frames with a white or pale wedge, or half the screen white; in Cascade
 * Kingdom 2 to 9 frames in 361 while the camera moved, and 0 since). Each stream is now copied as far as the draw reads:
 * the highest vertex (plain draws: first + count; indexed draws: the largest index, found once per index range and batch)
 * and, for per-instance streams, the highest instance; only inside the game's heap, and by 16 MB at most (else left as
 * bound, counted). SMO_VK_VTXGROW=0: as before; SMO_VK_VTXCHECK=1 logs each kind of draw that reads past its range. */
static int g_vtxcheck = -1, g_vtxgrow = -1;
static int64_t g_vtx_over, g_fr_vtxover, g_vtx_capped, g_imax_scans;
static uint64_t g_vneed[16];                       /* bytes of each vertex stream the next draw reads (0: not known) */
int smo_heap_section_offset(uint64_t addr, uint64_t size, void **section, uint64_t *offset);      /* smo_nn.c: inside the game's committed heap? */
static unsigned vattr_size(int nf) {               /* bytes of one vertex attribute of NVN format nf (as vertex_vkfmt maps it) */
    static const uint8_t w[12] = {8, 8, 8, 8, 16, 16, 16, 16, 16, 32, 32, 32};
    if (nf >= 0x01 && nf <= 0x30) return (unsigned)((nf - 1) / 12 + 1) * w[(nf - 1) % 12] / 8;
    if (nf >= 0x6D && nf <= 0x78) return 4u * w[nf - 0x6D] / 8;
    if (nf >= 0x51 && nf <= 0x68) { int t = (nf - 0x51) % 6; return (unsigned)((nf - 0x51) / 6 + 1) * (t < 2 ? 1u : t < 4 ? 2u : 4u); }
    return 4;
}
static void vtx_switches(void) {
    if (g_vtxgrow >= 0) return;
    g_vtxgrow = !(getenv("SMO_VK_VTXGROW") && !atoi(getenv("SMO_VK_VTXGROW")));
    g_vtxcheck = getenv("SMO_VK_VTXCHECK") && atoi(getenv("SMO_VK_VTXCHECK"));
}
/* The largest index of [addr, addr + count) of index type t (0 8-bit, 1 16-bit, 2 32-bit), restart indices left out;
 * remembered for the batch (R.gen: guest memory may change between batches). 16 bytes at a time (SSE2: the host code is
 * built at -O1, which does not vectorize): one index at a time it was 7% of the render thread in Cascade Kingdom, about
 * 1.2 ms a frame. Unsigned maxima: bytes directly, 16- and 32-bit indices with the top bit flipped compared as signed;
 * a restart index counts as 0. */
static uint32_t index_max(uint64_t addr, int count, unsigned t) {
    static struct { uint64_t addr; uint32_t gen, maxi; int n; uint8_t t, rs; } c[2048];
    unsigned h = (unsigned)(((addr >> 1) ^ (uint64_t)count * 0x9E3779B1u) * 2654435761u) >> 21;
    if (c[h].addr == addr && c[h].n == count && c[h].t == (uint8_t)t && c[h].gen == R.gen && c[h].rs == (uint8_t)R.restart) return c[h].maxi;
    const uint8_t *src = P(addr);
    uint32_t mx = 0;
    int k = 0, per = 16 >> t;                      /* indices in 16 bytes */
#if defined(__aarch64__)
    /* Android port: NEON has unsigned maxima of every width (no sign flip) and a horizontal maximum */
    if (t == 0) {
        uint8x16_t m = vdupq_n_u8(0), rs = vdupq_n_u8(R.restart ? 0xFF : 0);
        for (; k + 16 <= count; k += 16) { uint8x16_t v = vld1q_u8(src + k); m = vmaxq_u8(m, vbicq_u8(v, vandq_u8(vceqq_u8(v, vdupq_n_u8(0xFF)), rs))); }
        mx = vmaxvq_u8(m);
    } else if (t == 1) {
        uint16x8_t m = vdupq_n_u16(0), rs = vdupq_n_u16(R.restart ? 0xFFFF : 0);
        for (; k + 8 <= count; k += 8) { uint16x8_t v = vld1q_u16((const uint16_t *)(src + 2 * (size_t)k)); m = vmaxq_u16(m, vbicq_u16(v, vandq_u16(vceqq_u16(v, vdupq_n_u16(0xFFFF)), rs))); }
        mx = vmaxvq_u16(m);
    } else {
        uint32x4_t m = vdupq_n_u32(0), rs = vdupq_n_u32(R.restart ? 0xFFFFFFFFu : 0);
        for (; k + 4 <= count; k += 4) { uint32x4_t v = vld1q_u32((const uint32_t *)(src + 4 * (size_t)k)); m = vmaxq_u32(m, vbicq_u32(v, vandq_u32(vceqq_u32(v, vdupq_n_u32(0xFFFFFFFFu)), rs))); }
        mx = vmaxvq_u32(m);
    }
#elif defined(__SSE2__) || defined(_M_X64)
    __m128i ones = _mm_set1_epi8(-1), rs = R.restart ? ones : _mm_setzero_si128();
    if (t == 0) {
        __m128i m = _mm_setzero_si128();
        for (; k + 16 <= count; k += 16) { __m128i v = _mm_loadu_si128((const __m128i *)(src + k)); m = _mm_max_epu8(m, _mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi8(v, ones), rs), v)); }
        uint8_t b[16]; _mm_storeu_si128((__m128i *)b, m);
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    } else if (t == 1) {
        __m128i flip = _mm_set1_epi16((short)0x8000), m = flip;
        for (; k + 8 <= count; k += 8) { __m128i v = _mm_loadu_si128((const __m128i *)(src + 2 * (size_t)k)); m = _mm_max_epi16(m, _mm_xor_si128(_mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi16(v, ones), rs), v), flip)); }
        uint16_t b[8]; _mm_storeu_si128((__m128i *)b, _mm_xor_si128(m, flip));
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    } else {
        __m128i flip = _mm_set1_epi32((int)0x80000000u), m = flip;
        for (; k + 4 <= count; k += 4) {
            __m128i v = _mm_loadu_si128((const __m128i *)(src + 4 * (size_t)k));
            v = _mm_xor_si128(_mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi32(v, ones), rs), v), flip);
            __m128i gt = _mm_cmpgt_epi32(v, m);
            m = _mm_or_si128(_mm_and_si128(gt, v), _mm_andnot_si128(gt, m));
        }
        uint32_t b[4]; _mm_storeu_si128((__m128i *)b, _mm_xor_si128(m, flip));
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    }
#endif
    if (t == 0) { for (; k < count; k++) { uint32_t v = src[k]; if (v > mx && !(R.restart && v == 0xFFu)) mx = v; } }
    else if (t == 1) { const uint16_t *p16 = (const uint16_t *)src; for (; k < count; k++) { uint32_t v = p16[k]; if (v > mx && !(R.restart && v == 0xFFFFu)) mx = v; } }
    else { const uint32_t *p32 = (const uint32_t *)src; for (; k < count; k++) { uint32_t v = p32[k]; if (v > mx && !(R.restart && v == 0xFFFFFFFFu)) mx = v; } }
    c[h].addr = addr; c[h].n = count; c[h].t = (uint8_t)t; c[h].gen = R.gen; c[h].rs = (uint8_t)R.restart; c[h].maxi = mx;
    g_imax_scans++;
    return mx;
}
/* maxv: the highest vertex the draw reads; maxi: the highest instance. Fills g_vneed for the streams that read past
 * their range (only those: the others keep the game's range and its copy). */
static void vtx_need(uint64_t maxv, uint64_t maxi, int indexed) {
    static struct { int vs, fs, st; uint64_t size; } seen[256];
    static int nseen;
    memset(g_vneed, 0, sizeof g_vneed);
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        uint32_t a = R.vattr[i];
        if (!(a & 0xff)) continue;
        int st = (int)((a >> 8) & 31);
        if (st >= 16 || !R.vbuf[st].addr) continue;
        uint64_t last = R.vstream[st].divisor ? maxi / R.vstream[st].divisor : maxv;
        uint64_t need = last * R.vstream[st].stride + (a >> 14) + vattr_size((int)(a & 0xff));
        if (need <= R.vbuf[st].size || need <= g_vneed[st]) continue;
        if (need > R.vbuf[st].size + (16u << 20)) { g_vtx_capped++; continue; }
        { void *hs; uint64_t ho; if (!smo_heap_section_offset(R.vbuf[st].addr, need, &hs, &ho)) { g_vtx_capped++; continue; } }      /* the bytes past the range must be the game's memory (smo_nn.c) */
        g_vneed[st] = need;
    }
    for (int st = 0; st < 16; st++) {
        if (!g_vneed[st]) continue;
        g_vtx_over++; g_fr_vtxover++;
        if (!g_vtxcheck) break;
        int vs = R.prog[0] ? R.prog[0]->serial : -1, fs = R.prog[1] ? R.prog[1]->serial : -1, k;
        for (k = 0; k < nseen && !(seen[k].vs == vs && seen[k].fs == fs && seen[k].st == st && seen[k].size == R.vbuf[st].size); k++) {}
        if (k == nseen && nseen < 256) {
            seen[nseen].vs = vs; seen[nseen].fs = fs; seen[nseen].st = st; seen[nseen].size = R.vbuf[st].size; nseen++;
            printf("nvn_vk_vtx: frame %lld: %s draw of programs #%d/#%d reads stream %d (stride %u%s) to byte %llu, the game bound %llu%s\n",
                   nvn_front_frame_next(), indexed ? "indexed" : "plain", vs, fs, st, R.vstream[st].stride, R.vstream[st].divisor ? ", per instance" : "",
                   (unsigned long long)g_vneed[st], (unsigned long long)R.vbuf[st].size, g_vtxgrow ? " (copied that far)" : "");
        }
        break;
    }
    if (!g_vtxgrow) memset(g_vneed, 0, sizeof g_vneed);
}
static void tex_barrier(void) {
    static int off = -1;
    if (off < 0) off = getenv("SMO_VK_NO_TEXBARRIER") && atoi(getenv("SMO_VK_NO_TEXBARRIER"));
    if (off) return;
    if (R.in_pass) {                               /* SMO issues one at almost every pass (about 60 a frame): only a pass that samples its own target needs it */
        int fb = R.depth.tex && R.depth.tex->vk && ((VTex *)R.depth.tex->vk)->feedback;
        for (int i = 0; i < R.ncol && !fb; i++) fb = R.col[i].tex && R.col[i].tex->vk && ((VTex *)R.col[i].tex->vk)->feedback;
        if (!fb) { g_texbar_skip++; return; }      /* the layout change after the pass carries the barrier for what reads it later */
        pass_end(); g_texbar_split++;
    }
    cmd_begin();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    if (g_split_bar) {                             /* 0.1.7: two halves, what fragments wrote reaches the fragment stages only (image_barrier) */
        VkFlags att = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        VkFlags sw = mb.srcAccessMask;
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(R.cmd, att, att | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        g_cs_debt = 1;
        mb.srcAccessMask = sw & ~(VkFlags)(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    vkCmdPipelineBarrier(R.cmd, (g_split_bar ? 0 : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    g_texbar++; g_fr_texbar++;
}
static void vt_check(VTex *vt) { if (vt->magic != 0x56544558) vkd_fail("an image is used after it was destroyed (texture record %p)", (void *)vt); }
static void vt_destroy(VTex *vt) {
    for (VView *v = vt->views, *n; v; v = n) { n = v->next; vkDestroyImageView(V.device, v->view, NULL); free(v); }
    vkDestroyImage(V.device, vt->img, NULL);
    vmem_free(vt->mem);
    if (vt->prev) vt->prev->next = vt->next; else g_vtex_all = vt->next;
    if (vt->next) vt->next->prev = vt->prev;
    vt->magic = 0xDEADDEAD; R.images_live--;
    free(vt);
}
static void garbage_collect(void) {
    int n = 0;
    int64_t t0 = g_ngarbage ? tk() : 0;
    for (int i = 0; i < g_ngarbage; i++) if (g_garbage[i].serial <= R.done) vt_destroy(g_garbage[i].vt); else g_garbage[n++] = g_garbage[i];
    if (t0 && g_ngarbage - n >= 50) {              /* session 8: what destroying many images at once costs (the end of a screen) */
        plat_li fq; ((fq).QuadPart = plat_qpf());
        double ms = (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart;
        static int nl;
        if (ms > 10.0 && nl++ < 40) printf("nvn_vk_tex: frame %lld: %d images destroyed in %.1f ms\n", (long long)nvn_front_frame_next(), g_ngarbage - n, ms);
    }
    g_ngarbage = n;
}
static void garbage_add(VTex *vt) {
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;       /* the command buffer being recorded may still name it */
}
/* SMO 0.1.4: what the GPU drew outlives the texture object. On the console a texture's contents are its pool memory, so an
 * object made again over the same memory, with the same shape, still holds them, and the game relies on that: when Mario
 * respawns it makes new objects for its 128x128 cube maps of ambient light and draws with them for thousands of frames
 * without rendering them again (only the pause menu does). Here what the GPU drew lives in the image of the old Tex, so the
 * new one read as zero: every lit surface lost its ambient light until the game paused (S24, capture of the same view with
 * and without: same draws, the G-buffer's colour target black, 10 cube maps "written 0", at the addresses of the old ones).
 * So the image of a texture the GPU wrote waits in g_keep when the texture goes, and vt_carry copies it into the next image
 * at that memory. Not the screen-sized targets (redrawn every frame), and at most KEEP_IMAGES images and KEEP_BYTES bytes:
 * the oldest goes first. SMO_VK_KEEPTEX=0: as before. */
enum { KEEP_IMAGES = 128 };
#define KEEP_BYTES ((VkDeviceSize)64 << 20)
#define KEEP_ONE_BYTES ((VkDeviceSize)4 << 20)
static VTex *g_keep[KEEP_IMAGES];
static int g_keep_n;
static VkDeviceSize g_keep_bytes;
static int64_t g_keep_carried;
static int keep_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("SMO_VK_KEEPTEX"); on = !(e && !atoi(e)); } return on; }
static int vt_same_shape(const VTex *a, const VTex *b) {
    return a->fmt == b->fmt && a->w == b->w && a->h == b->h && a->d == b->d && a->levels == b->levels && a->layers == b->layers && a->is3d == b->is3d;
}
static void keep_remove(int i) {
    g_keep[i]->kept = 0;
    g_keep_bytes -= g_keep[i]->mem.size;
    memmove(&g_keep[i], &g_keep[i + 1], sizeof g_keep[0] * (size_t)(g_keep_n - i - 1));
    g_keep_n--;
}
static void keep_add(VTex *vt) {
    for (int i = 0; i < g_keep_n; i++) if (g_keep[i]->gmem == vt->gmem && vt_same_shape(g_keep[i], vt)) { VTex *o = g_keep[i]; keep_remove(i); garbage_add(o); break; }
    while (g_keep_n && (g_keep_n == KEEP_IMAGES || g_keep_bytes + vt->mem.size > KEEP_BYTES)) { VTex *o = g_keep[0]; keep_remove(0); garbage_add(o); }
    vt->kept = 1;
    g_keep[g_keep_n++] = vt;
    g_keep_bytes += vt->mem.size;
}
static void vk_drop(int kind, void *host) {
    if (kind != K_TEX) return;
    Tex *t = host;
    VTex *vt = t->vk;
    if (!vt) return;
    int written = t->gpu_written || vt->carried;
    int64_t last = t->last_use;
    t->vk = NULL; vt->t = NULL;                    /* the Tex is about to be freed */
    for (int i = 0; i < 8; i++) if (R.col[i].tex == t) { pass_end(); R.col[i].tex = NULL; }
    if (R.depth.tex == t) { pass_end(); R.depth.tex = NULL; }
    if (written && vt->gmem && vt->mem.size <= KEEP_ONE_BYTES) { vt->stamp = last; keep_add(vt); }
    else garbage_add(vt);
}
/* Textures nothing in the game names any more (nvn_gl.c, the note above pool_ent_release: agl gave up every pool slot
 * of the texture and the object that made it holds another one). Their images go once they have not been used for
 * SMO_VK_TEXFREE frames (default 8; 0 = never, as before): a command list recorded before agl let go may still be
 * submitted (RevengeBossMagmaStage, 4,700 images freed: the last use was at most 1 frame after the release). The
 * texture is not dead: a later use makes its image again (counted and logged as "made again"; what the GPU had drawn
 * into it comes back through g_keep while the image still waits there). One still bound as a render target, or a window
 * texture, waits. */
static Tex **g_rc;                                 /* the textures waiting (each has Tex.reclaim = 1) */
static int g_rc_n, g_rc_cap;
static int64_t g_rc_freed, g_rc_freed_mb, g_rc_again, g_rc_late;
static int texfree_frames(void) { static int n = -2; if (n == -2) { const char *e = getenv("SMO_VK_TEXFREE"); n = e ? atoi(e) : 8; } return n; }
static int texlog_vk(void) { static int on = -1; if (on < 0) on = getenv("SMO_TEXLOG") != NULL; return on; }
static void rc_take(Tex *t) {
    if (g_rc_n == g_rc_cap) { g_rc_cap = g_rc_cap ? g_rc_cap * 2 : 256; g_rc = realloc(g_rc, sizeof *g_rc * (size_t)g_rc_cap); }
    g_rc[g_rc_n++] = t;
}
static void rc_ignore(Tex *t) { (void)t; }        /* SMO_VK_TEXFREE=0: taken and left (Tex.reclaim stays 1, so it is not offered again) */
static void tex_reclaim(int64_t frame) {
    int idle = texfree_frames(), n = 0;
    nvn_front_take_unref(idle > 0 ? rc_take : rc_ignore);
    for (int i = 0; i < g_rc_n; i++) {
        Tex *t = g_rc[i];
        if (t->dead) continue;                     /* finalized by the object that made it: vk_drop has its image */
        if (t->nslots > 0) {                       /* registered again: offered again when agl lets go of it once more */
            plat_atomic_xchg(&t->reclaim, 0);
            if (t->nslots > 0 || plat_atomic_cas(&t->reclaim, 1, 0) != 0) continue;     /* (and if that happened just now, it stays) */
        }
        int bound = R.depth.tex == t || wintex_is(t);
        for (int k = 0; k < 8 && !bound; k++) bound = R.col[k].tex == t;
        if (bound || frame - t->last_use < idle || !nvn_front_tex_unref(t)) { g_rc[n++] = t; continue; }
        VTex *vt = t->vk;
        if (vt) {
            g_rc_freed++; g_rc_freed_mb += (int64_t)(vt->mem.size >> 20);
            if (texlog_vk()) printf("texlog: image freed host %p %dx%d fmt 0x%x (%llu KB) frame %lld, last used in frame %lld\n", (void *)t, t->b.width, t->b.height, t->b.format, (unsigned long long)(vt->mem.size >> 10), (long long)frame, (long long)t->last_use);
            vk_drop(K_TEX, t);
            t->uploaded = 0; t->checked = 0;       /* a texture the CPU wrote is read from its memory again if it is used again */
            t->image_freed = 1;
        } else t->image_freed = 2;
        plat_atomic_xchg(&t->reclaim, 0);
    }
    g_rc_n = n;
}
static int lvl_(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Track O. A temporary image of w x h (one level, one layer, the format of `like`) for an upload into, or a read-back from,
 * an image that is smaller than the game's texture. The command buffer is being recorded; the image goes when it has finished. */
static VTex *rs_temp(VTex *like, int w, int h) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = like->fmt; vt->aspect = like->aspect; vt->w = w; vt->h = h; vt->d = 1; vt->levels = 1; vt->layers = 1; vt->samples = 1;
    vt->bpb = like->bpb; vt->bw = like->bw; vt->bh = like->bh;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;
    return vt;
}
static VkFilter rs_filter(VTex *vt) { return vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
/* Texels of the game's grid (w x h at `off` of the stream ring, for the rectangle x, y of one level) into a scaled image:
 * through a temporary image of the game's size and a filtered blit. The command buffer is being recorded, no pass is open. */
static void rs_upload(VTex *vt, int level, int layer, uint64_t off, uint32_t rowlen, uint32_t rows, int x, int y, int w, int h) {
    int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
    VkBufferImageCopy c;
    VkImageBlit b;
    if (w <= 0 || h <= 0) return;
    if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) { nvn_vk_missing("resolution scale: an upload into a scaled texture whose format cannot be blitted"); return; }
    VTex *tmp = rs_temp(vt, w, h);
    memset(&c, 0, sizeof c); memset(&b, 0, sizeof b);
    c.bufferOffset = off; c.bufferRowLength = rowlen; c.bufferImageHeight = rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(R.cmd, R.ring, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.mipLevel = (uint32_t)level; b.dstSubresource.baseArrayLayer = (uint32_t)layer; b.dstSubresource.layerCount = 1;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = rs_edge(x, vt->sx, lw); b.dstOffsets[0].y = rs_edge(y, vt->sy, lh); b.dstOffsets[1].x = rs_edge(x + w, vt->sx, lw); b.dstOffsets[1].y = rs_edge(y + h, vt->sy, lh); b.dstOffsets[1].z = 1;
    if (b.dstOffsets[1].x <= b.dstOffsets[0].x || b.dstOffsets[1].y <= b.dstOffsets[0].y) return;
    vkCmdBlitImage(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
    g_rs_up++;
}
static VTex *g_put_vt;
static uint32_t g_fmt_hist[256];                   /* images made per NVN format (printed with the totals) */
static void vt_put(Tex *t, int level, int layer, int w, int h, int d, const void *data, size_t size, int rgba8, int64_t row_stride) {
    VTex *vt = g_put_vt;
    uint8_t *tmp = NULL;
    (void)t;
    if (level >= vt->levels) return;
    if (vt->expand && !rgba8) {                    /* 3 components -> 4, the fourth is one */
        static const uint8_t csize[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
        static const uint32_t one[12] = {255, 127, 1, 1, 0x3C00, 65535, 32767, 1, 1, 0x3F800000, 1, 1};
        int cs = csize[vt->expand - 1];
        size_t n = (size_t)w * h * d, stride = row_stride ? (size_t)row_stride : (size_t)w * 3 * cs;
        tmp = malloc(n * 4 * cs);
        for (size_t y = 0; y < (size_t)h * d; y++)
            for (size_t x = 0; x < (size_t)w; x++) { uint8_t *o = tmp + (y * w + x) * 4 * cs; memcpy(o, (const uint8_t *)data + y * stride + x * 3 * cs, 3 * (size_t)cs); memcpy(o + 3 * cs, &one[vt->expand - 1], (size_t)cs); }
        data = tmp; size = n * 4 * cs; row_stride = 0;
    }
    uint64_t off = ring_raw(data, size);           /* may submit what was recorded so far */
    free(tmp);
    pass_end(); cmd_begin();
    if (vt->rs) {                                  /* track O: the texture's own memory holds the game's size */
        if (!vt->is3d && (uint32_t)layer < (uint32_t)vt->layers) rs_upload(vt, level, layer, off, row_stride && vt->bpb ? (uint32_t)(row_stride / vt->bpb) : 0, 0, 0, 0, w, h);
        R.upload_bytes += (int64_t)size;
        return;
    }
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    if (row_stride && vt->bpb) c.bufferRowLength = (uint32_t)(row_stride / vt->bpb);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = (uint32_t)(vt->is3d ? d : 1);
    if (!vt->is3d && (uint32_t)layer >= (uint32_t)vt->layers) return;
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    R.upload_bytes += (int64_t)size;
}
/* The image of a texture: made at first use, and holding what the guest memory holds (unless the GPU wrote it). */
/* Session 8: textures made and read AHEAD of their first draw (tex_ahead, below), and what is still made at a draw. */
static int64_t g_ta_n, g_ta_bytes, g_ta_tk, g_ta_max_tk, g_late_n, g_late_tk, g_late_fight_n, g_late_fight_tk, g_up_fr_tk, g_up_fr_bytes;
static int g_up_fr_n, g_ta_inside;
static volatile int32_t g_in_fight;                   /* nvn_vk_m4.c (included further down) */
/* SMO 0.1.4 (the note above vk_drop): a new image takes what the GPU drew at the same memory, from a kept image or from a
 * texture still alive over that memory (the newest used of them). The game's memory is noted as seen, not read over the
 * copy; if the CPU writes it afterwards, nvn_front_tex_changed sees that and the upload wins, as for any other texture.
 * A range the game flushed before this first use (Tex.dirty) holds new texels, so nothing is carried. */
static void vt_carry(VTex *vt, Tex *t) {
    VTex *from = NULL;
    int64_t newest = -1;
    if (!vt->gmem || t->dirty) return;
    for (VTex *o = g_vtex_all; o; o = o->next) {
        int64_t s;
        if (o == vt || o->gmem != vt->gmem || !vt_same_shape(o, vt)) continue;
        if (o->kept) s = o->stamp;
        else if (o->t && (o->t->gpu_written || o->carried)) s = o->t->last_use;
        else continue;
        if (s > newest) { from = o; newest = s; }
    }
    if (!from) return;
    VkImageLayout was = from->layout;
    VkImageCopy rg[16];
    uint32_t n = 0;
    for (int l = 0; l < vt->levels && l < 16; l++, n++) {
        memset(&rg[n], 0, sizeof rg[n]);
        rg[n].srcSubresource.aspectMask = rg[n].dstSubresource.aspectMask = vt->aspect;
        rg[n].srcSubresource.mipLevel = rg[n].dstSubresource.mipLevel = (uint32_t)l;
        rg[n].srcSubresource.layerCount = rg[n].dstSubresource.layerCount = (uint32_t)(vt->is3d ? 1 : vt->layers);
        rg[n].extent.width = (uint32_t)lvl_(vt->w, l); rg[n].extent.height = (uint32_t)lvl_(vt->h, l); rg[n].extent.depth = (uint32_t)(vt->is3d ? lvl_(vt->d, l) : 1);
    }
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vt_layout(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImage(R.cmd, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, rg);
    t->checked = 0;
    (void)nvn_front_tex_changed(t);
    t->uploaded = 1;
    vt->carried = 1;
    g_keep_carried++;
    {   static int nl;
        if (nl++ < 60) printf("nvn_vk_tex: frame %lld: texture %dx%d format 0x%x (%d layers, %d levels) at memory %p: contents carried over from the %s image there (%lld so far, %d images kept)\n", (long long)g_cur_frame, t->b.width, t->b.height, (unsigned)t->b.format, vt->layers, vt->levels, vt->gmem, from->kept ? "kept" : "live", (long long)g_keep_carried, g_keep_n);
    }
    if (from->kept) {
        for (int i = 0; i < g_keep_n; i++) if (g_keep[i] == from) { keep_remove(i); break; }
        garbage_add(from);
    } else if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(from, was);      /* a draw being prepared may have it bound */
}
static VTex *vt_get2(Tex *t, int sync) {
    VTex *vt = t->vk;
    int64_t tu0 = 0, ub0 = R.upload_bytes;
    t->last_use = g_cur_frame;
    if (!vt && t->image_freed) {                   /* tex_reclaim freed it (or let it go before it had an image): it is used after all */
        static int nl;
        if (t->image_freed == 1) g_rc_again++; else g_rc_late++;
        if (nl++ < 40) printf("nvn_vk_tex: frame %lld: texture %dx%d format 0x%x %s after nothing named it (written by the GPU %d, pool entries %d)\n", (long long)g_cur_frame, t->b.width, t->b.height, t->b.format,
                              t->image_freed == 1 ? "used again: its image is made again (empty if the GPU wrote it)" : "used for the first time", t->gpu_written, t->nslots);
        t->image_freed = 0;
        if (t->nslots <= 0 && plat_atomic_cas(&t->reclaim, 1, 0) == 0) rc_take(t);      /* still nothing names it: it may go again */
    }
    if (!vt) {
        tu0 = tk();
        TexBuilder *b = &t->b;
        Fmt f;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        vt = calloc(1, sizeof *vt);
        vt->magic = 0x56544558; vt->t = t;
        {   int sub0 = g_fmt_sub, is_sub;
            is_sub = fmt_sub(b->format) != VK_FORMAT_UNDEFINED;
            if (is_sub && (b->packaged || !(b->flags & (8 | 0x40)))) { sub0 = g_fmt_sub;      /* only render targets (compressible or NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET): the game writes texels into small R32F textures */
                                                          g_fmt_sub = 0; vt->fmt = tex_vkfmt(b->format, &vt->expand, &vt->decode); g_fmt_sub = sub0; is_sub = 0; }
            else vt->fmt = tex_vkfmt(b->format, &vt->expand, &vt->decode);
            vt->sub = is_sub;
            if (is_sub) printf("nvn_vk_fmt: format fix: texture %dx%d, NVN format 0x%x, made as Vulkan format %d (flags 0x%x)\n", b->width, b->height, (unsigned)b->format, (int)vt->fmt, (unsigned)b->flags);
        }
        if (!vt->fmt || !nvn_front_fmt(b->format, &f)) { static char msg[80]; snprintf(msg, sizeof msg, "texture format 0x%x", b->format); nvn_vk_missing(msg); vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; nvn_front_fmt(0x25, &f); }
        vt->bpb = vt->decode ? tex_decode_bytes_per_texel(vt->decode) : vt->expand ? f.bpb / 3 * 4 : f.bpb; if (vt->sub && b->format == 0x2e) vt->bpb = 8; vt->bw = vt->decode ? 1 : f.bw; vt->bh = vt->decode ? 1 : f.bh;
        vt->aspect = fmt_aspect(vt->fmt);
        vt->w = b->width > 0 ? b->width : 1; vt->h = b->height > 0 ? b->height : 1; vt->d = b->depth > 0 ? b->depth : 1;
        vt->levels = nvn_front_tex_levels(t); vt->layers = nvn_front_tex_layers(t); vt->samples = b->samples > 1 ? b->samples : 1; vt->is3d = b->target == 2;
        if (b->target == 0 || b->target == 3) { ci.imageType = VK_IMAGE_TYPE_1D; vt->h = 1; if (b->target == 3) vt->layers = b->height > 0 && b->depth <= 1 ? b->height : vt->layers; }
        else ci.imageType = vt->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        if (RS_ON && (b->target == 1 || b->target == 5 || b->target == 4) && vt->layers == 1 &&      /* Android port: 4 (2D array) of one layer too: SMO's 1600x900 HDR target is one; left at the game's size beside its scaled partners it drew a second, smaller picture */ !vt->decode && rs_screen_class(vt->w, vt->h) && !(rs_ui_native() && wintex_is(t)) &&
            !(b->flags & 4) &&                     /* Android port: not an image of compute stages (NVN_TEXTURE_FLAGS_IMAGE): a dispatch addresses it in the game's texels (SMO's post effects); it keeps the game's size */
            (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (f.kind != 4 && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)))) {      /* track O: a screen-class target */
            int gw = vt->w, gh = vt->h, lv = 1;
            vt->w = (int)((float)gw * rs_scale() + 0.5f); vt->h = (int)((float)gh * rs_scale() + 0.5f);
            if (vt->w < 1) vt->w = 1; if (vt->h < 1) vt->h = 1;
            vt->rs = 1; vt->sx = (float)vt->w / (float)gw; vt->sy = (float)vt->h / (float)gh;
            while ((vt->w >> lv) || (vt->h >> lv)) lv++;
            if (vt->levels > lv) vt->levels = lv;
            g_rs_images++;
        }
        ci.format = vt->fmt; ci.extent.width = (uint32_t)vt->w; ci.extent.height = (uint32_t)vt->h; ci.extent.depth = (uint32_t)(vt->is3d ? vt->d : 1);
        ci.mipLevels = (uint32_t)vt->levels; ci.arrayLayers = (uint32_t)(vt->is3d ? 1 : vt->layers); ci.samples = (VkSampleCountFlagBits)vt->samples; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        int can_color = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && f.kind != 4 && !vt->decode && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        if (can_color) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if ((b->flags & 4) && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand && vt->samples == 1 && !vt->rs && vt->bw == 1 && vt->bh == 1 &&
            (fmt_supported(vt->fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB)) {      /* SMO: NVN_TEXTURE_FLAGS_IMAGE (0x4; the game sets it on most textures): compute programs may bind it as an image; not on block-compressed formats, which no storage view can have */
            ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT; vt->storage = 1; g_storage_tex++;
            if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ci.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;      /* bound through a view of another format (sRGB, r32ui on r32f) */
        }
        if (ci.imageType == VK_IMAGE_TYPE_2D && vt->w == vt->h && vt->layers >= 6 && vt->samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (vt->is3d && (ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;      /* SMO: its slices can be render targets */
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(V.device, &ci, NULL, &vt->img);
        if (r != VK_SUCCESS) vkd_fail("vkCreateImage (NVN format 0x%x -> %d, target %d, %dx%dx%d, %d levels, %d layers, %d samples): %s", b->format, (int)vt->fmt, b->target, vt->w, vt->h, vt->d, vt->levels, vt->layers, vt->samples, vk_result_name(r));
        {   /* Android port (2026-11-04): a format the GPU cannot filter, blend or draw into is used anyway (the game's samplers and
             * blend states do not know the GPU), so the picture can differ from one GPU to the next. One line per format, for
             * reports from phones without the tester's GPU (Mali: the fog and glow broken there). */
            static int seen[64], nseen;
            int k = 0;
            while (k < nseen && seen[k] != (int)vt->fmt) k++;
            if (k == nseen && nseen < 64 && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                VkFlags have = fmt_feats(vt->fmt);
                seen[nseen++] = (int)vt->fmt;
                if (!(have & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) || (f.kind != 4 && !vt->decode && !(have & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)))
                    printf("nvn_vk_fmt: NVN format 0x%x (Vulkan %d)%s: linear filtering %s, colour target %s, blending %s, storage %s\n", b->format, (int)vt->fmt, vt->decode ? " (decoded on the CPU)" : "",
                           have & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "yes" : "NO", have & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "yes" : "NO",
                           have & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? "yes" : "NO", have & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "yes" : "NO");
            }
        }
        VkMemoryRequirements rq;
        vkGetImageMemoryRequirements(V.device, vt->img, &rq);
        vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
        vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
        t->vk = vt;
        R.images++; R.images_live++; nvn_front_count(5, 1);
        g_fmt_hist[b->format & 255]++;
        if (can_color || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {      /* an image nothing was written to reads as zero (the OpenGL driver's behaviour, which the comparison saw) */
            VkImageSubresourceRange sr = {vt->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (vt->samples == 1) {
                if (can_color) { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
                else { VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
            }
        }
        if (keep_on() && can_color && vt->samples == 1 && !vt->rs && t->mem && !b->packaged && !rs_screen_class(b->width, b->height)) { vt->gmem = t->mem; vt_carry(vt, t); }
    }
    vt_check(vt);
    if (sync && nvn_front_tex_changed(t)) {
        if (!tu0) tu0 = tk();
        vt->carried = 0;                           /* the CPU wrote it after all */
        g_put_vt = vt;
        cmd_begin();
        if (!nvn_front_tex_read(t, vt->decode, vt_put)) { static int n; if (n++ < 20) printf("nvn_vk: texture %dx%d format 0x%x: its memory is not readable; not uploaded\n", t->b.width, t->b.height, t->b.format); }
        R.uploads++;
    }
    if (tu0 && !g_ta_inside) {                     /* made or read again at its use (a draw, a copy, a target): the frame pays */
        int64_t d = tk() - tu0;
        g_up_fr_tk += d; g_up_fr_n++; g_up_fr_bytes += R.upload_bytes - ub0;
        g_late_n++; g_late_tk += d;
        if (g_in_fight) { g_late_fight_n++; g_late_fight_tk += d; }
    }
    return vt;
}
static VTex *vt_get(Tex *t) { return vt_get2(t, 1); }
/* Session 8, render thread, after every present. A texture used to get its image, and its texels read out of the game's
 * memory (hash, de-swizzle, copy into the stream ring), at the first draw that sampled it: the first frame of a fight paid
 * 37-50 ms for the stage's and the fighters' textures (runs s7fn2 / s7gn2, frames 1804-1823). Here every block-compressed
 * texture the game has made (nvnTextureInitialize) and not yet drawn with is given its image two frames after the render
 * thread first saw it, under a time budget per frame (4 ms outside a fight, 1 ms in one; one texture is always taken, so a
 * very large one can exceed it): the loading screen absorbs the work. Only compressed formats: they cannot be render
 * targets, so nothing the GPU is about to write is touched; if the game fills the memory later, the draw sees the change
 * (nvn_front_tex_changed) and reads it again, as before. SMO_VK_TEXAHEAD=0 = as before (everything at the first draw). */
static int fight_recent(void);                     /* nvn_vk_m4.c */
static void present_progress(float frac);          /* session 11: the held loading screen presented again with the progress bar (defined above vk_present) */
static int64_t g_ovl_presents, g_ovl_frames;        /* extra presents of a held loading screen; normal frames drawn with the thin bar */
static int holdlog_on(void);
static void tex_ahead(int64_t frame) {
    static int on = -1;
    static Tex *done_head;
    static int64_t fq;
    if (on < 0) { const char *e = getenv("SMO_VK_TEXAHEAD"); plat_li f; ((f).QuadPart = plat_qpf()); fq = f.QuadPart; on = !(e && !atoi(e)); }
    if (!on) return;
    Tex *head = nvn_front_tex_first();
    if (head == done_head) return;
    int fr = fight_recent();
    int64_t t0 = tk(), budget = fq * (fr ? 1 : 4) / 1000;
    int pending = 0, taken = 0;
    Tex *oldest = NULL;                            /* the oldest texture still pending: the next walk ends after it */
    g_ta_inside = 1;
    for (Tex *t = head; t && t != done_head; t = t->all_next) {
        Fmt f;
        if (t->ahead_seen < 0) continue;
        if (t->vk || t->dead || t->image_freed) { t->ahead_seen = -1; continue; }
        if (!t->ahead_seen) { t->ahead_seen = frame + 1; pending++; oldest = t; continue; }
        if (frame + 1 - t->ahead_seen < 2) { pending++; oldest = t; continue; }
        if (t->gpu_written || (!t->mem && !t->b.packaged) || t->b.samples > 1 || t->b.width <= 0 || t->b.height <= 0 || !nvn_front_fmt(t->b.format, &f) || f.kind != 4) { t->ahead_seen = -1; continue; }
        if (taken && tk() - t0 > budget) { pending++; oldest = t; continue; }
        if (fr) {
            /* in and around a fight (a frame of the last 600 had fighters) only what is certainly cheap: at most 512 KB, no
             * decoding on the CPU, no wrap of the stream ring (a wrap waits for the GPU). Measured without this rule (runs s8b
             * frame 3549, s8d 4297, s8f 3840: the result screen's textures arriving while the fight ends, in frames without
             * a dispatch): 15-22 ms after one present and a frame of 34-35 ms. The others wait for a frame far from a fight,
             * or are made at their use as before. SMO port: SMO dispatches compute in every frame, so for it every frame is
             * "in a fight": a texture over 512 KB or decoded on the CPU is made at its use (it stayed pending for ever, and the
             * walk above, which ends after the oldest pending texture, went over every texture made since the start in every
             * frame: 24% of the render thread after two minutes in Cascade Kingdom). */
            VTex probe;
            if (t->data_size > (512u << 10)) { t->ahead_seen = -1; continue; }
            if (R.ring_pos + 2 * (uint64_t)t->data_size + (1u << 20) > R.ring_size) { pending++; oldest = t; continue; }
            tex_vkfmt(t->b.format, &probe.expand, &probe.decode);
            if (probe.decode) { t->ahead_seen = -1; continue; }
        }
        int64_t b0 = R.upload_bytes;
        vt_get(t);
        t->ahead_seen = -1; taken++;
        g_ta_n++; g_ta_bytes += R.upload_bytes - b0;
    }
    g_ta_inside = 0;
    int64_t d = tk() - t0;
    g_ta_tk += d; if (d > g_ta_max_tk) g_ta_max_tk = d;
    if (d * 1000 > 6 * fq && holdlog_on()) { static int nl; if (nl++ < 100) printf("nvn_vk_tex: frame %lld (%s): %d textures made ahead after the present in %.1f ms\n", (long long)frame, fr ? "in or near a fight" : "far from a fight", taken, (double)d * 1000.0 / (double)fq); }
    done_head = pending ? oldest->all_next : head;     /* everything older than the oldest pending texture is done (before: only when nothing was
                                                    * pending, and the game makes textures every frame, so every frame walked all of them) */
}
/* tex_view_obj's rules: the effective view of `t` through `v`. */
static void eview_of(Tex *t, VTex *vt, const View *v, EView *e) {
    EView base;
    memset(&base, 0, sizeof base);
    base.target = t->b.target; base.format = t->b.format; base.nlvl = vt->levels; base.nlay = vt->is3d ? vt->d : nvn_front_tex_layers(t); base.dsmode = t->b.dsmode;   /* SMO: a 3D texture's view layers are its depth slices (a render target view picks one) */
    memcpy(base.swz, t->b.swz, 4);
    *e = base;
    if (!v || !(v->set & 0x80)) return;
    if (v->set & 1) { e->lvl0 = v->lvl0; e->nlvl = v->nlvl; }
    if (v->set & 2) { e->lay0 = v->lay0; e->nlay = v->nlay; }
    if ((v->set & 4) && v->format) e->format = v->format;
    if (v->set & 8) memcpy(e->swz, v->swz, 4);
    if (v->set & 16) e->dsmode = v->dsmode;
    if (v->set & 32) e->target = v->target;
    if (e->lvl0 < 0 || e->lvl0 >= base.nlvl) e->lvl0 = 0;
    if (e->nlvl <= 0 || e->lvl0 + e->nlvl > base.nlvl) e->nlvl = base.nlvl - e->lvl0;
    if (e->lay0 < 0 || e->lay0 >= base.nlay) e->lay0 = 0;
    if (e->nlay <= 0 || e->lay0 + e->nlay > base.nlay) e->nlay = base.nlay - e->lay0;
    if (t->b.target == 2) e->target = base.target;      /* SMO: a 3D texture keeps the view's slice (lay0); before, every slice became slice 0 (the colour-grading LUT is drawn as 8 attachments = 8 slices: blue frames) */
    else if (t->b.samples > 1) { e->target = base.target; e->lay0 = 0; e->nlay = base.nlay; }
    if ((e->target == 1 || e->target == 7 || e->target == 0) && e->nlay > 1) e->nlay = 1;
    if (e->target == 8 && e->nlay != 6) e->target = e->nlay == 1 ? 1 : 4;
    if (e->target == 9 && e->nlay % 6) e->target = 4;
}
enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1DA, DIM_2DA, DIM_CUBEA };
static const VkImageViewType k_vtype[7] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
static int target_dim(int target) { static const int m[10] = {DIM_1D, DIM_2D, DIM_3D, DIM_1DA, DIM_2DA, DIM_2D, DIM_2DA, DIM_2D, DIM_CUBE, DIM_CUBEA}; return target >= 0 && target < 10 ? m[target] : DIM_2D; }
/* dim < 0: an attachment view (one level, one layer, both aspects, no swizzle). */
static VkImageView vt_view(VTex *vt, const EView *e, int dim, int attach) {
    for (VView *n = vt->views; n; n = n->next) if (n->vtype == dim && n->attach == attach && !memcmp(&n->e, e, sizeof *e)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    int ex, dec;
    n->e = *e; n->vtype = dim; n->attach = attach;
    ci.image = vt->img; ci.format = vt->fmt;
    if (e->format != vt->t->b.format && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand) { VkFormat f = tex_vkfmt(e->format, &ex, &dec); if (f && !ex && !dec && fmt_aspect(f) == VK_IMAGE_ASPECT_COLOR_BIT) ci.format = f; }
    if (attach) {
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.subresourceRange.aspectMask = vt->aspect;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1; ci.subresourceRange.baseArrayLayer = (uint32_t)e->lay0; ci.subresourceRange.layerCount = 1;
        if (vt->is3d) {   /* SMO: one depth slice of a 3D texture, through a 2D view (image made 2D_ARRAY_COMPATIBLE); the slice is the view's first layer */
            int dl = vt->d >> e->lvl0; if (dl < 1) dl = 1;
            if (e->lay0 >= dl) ci.subresourceRange.baseArrayLayer = (uint32_t)(dl - 1);
            if (e->nlay > 1) { static int64_t n; if (n++ < 5) printf("nvn_vk: a 3D texture bound as render target with %d slices: only slice %d is drawn (layered rendering into a 3D texture is not implemented)\n", e->nlay, e->lay0); }
        }
    } else {
        int lay = e->nlay;
        if (dim == DIM_1D || dim == DIM_2D || dim == DIM_3D) lay = 1; else if (dim == DIM_CUBE) lay = 6; else if (dim == DIM_CUBEA) lay -= lay % 6;
        ci.viewType = k_vtype[dim];
        ci.subresourceRange.aspectMask = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (e->dsmode == 1 || vt->aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = (uint32_t)e->nlvl; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = (uint32_t)(vt->is3d ? 1 : lay);
        ci.components.r = sw[e->swz[0] < 6 ? e->swz[0] : 0]; ci.components.g = sw[e->swz[1] < 6 ? e->swz[1] : 0]; ci.components.b = sw[e->swz[2] < 6 ? e->swz[2] : 0]; ci.components.a = sw[e->swz[3] < 6 ? e->swz[3] : 0];
        if (vt->sub && vt->t->b.format == 0x0a && ci.components.g == VK_COMPONENT_SWIZZLE_IDENTITY) ci.components.g = VK_COMPONENT_SWIZZLE_ZERO;      /* fmt_sub: a one-channel format reads 0 in green */
    }
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (type %d, format %d of image format %d, levels %d+%d, layers %d+%d of %d, attach %d): %s", (int)ci.viewType, (int)ci.format, (int)vt->fmt, e->lvl0, e->nlvl, e->lay0, e->nlay, vt->layers, attach, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++;
    return n->view;
}

/* ---------------------------------------------------------------- samplers (shared by value, never deleted before shutdown) */
static struct { SamplerParams p; VkSampler s; } g_smp[2048];
static int g_nsmp;
static VkSampler g_smp_default;
static VkSampler sampler_get(const SamplerParams *p) {
    static const VkSamplerAddressMode wrap[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int i = 0; i < g_nsmp; i++) if (!memcmp(&g_smp[i].p, p, sizeof *p)) return g_smp[i].s;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    int minf = p->min < 6 ? p->min : 1;          /* nearest, linear, nearest-mip-nearest, linear-mip-nearest, nearest-mip-linear, linear-mip-linear */
    ci.minFilter = (minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.magFilter = p->mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap[p->ws & 7]; ci.addressModeV = wrap[p->wt & 7]; ci.addressModeW = wrap[p->wr & 7];
    ci.mipLodBias = p->bias; ci.minLod = p->lodmin < 0 ? 0 : p->lodmin; ci.maxLod = p->lodmax;
    if (minf < 2) { ci.minLod = 0; ci.maxLod = 0.25f; }      /* no mip maps: level 0 only (the Vulkan specification's own mapping of GL_NEAREST / GL_LINEAR) */
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    float aniso = p->aniso;
    {   /* track O: anisotropic_filtering in graphics.ini = at least that on samplers that filter linearly between mip levels (not shadow-compare samplers) */
        static int64_t raised;
        float want = (float)gfx_options()->aniso;
        if (want > aniso && minf >= 4 && p->cmode != 1) { aniso = want; raised++; if (raised == 1 || raised % 100 == 0) printf("nvn_vk: anisotropic_filtering=%d: %lld samplers raised so far (this one from %.0f)\n", gfx_options()->aniso, (long long)raised, p->aniso); }
    }
    if (aniso > 1.0f && D.f2.features.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = aniso > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : aniso; }
    if (p->cmode == 1) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cfunc ? (p->cfunc - 1) & 7 : 7); }
    {   /* the OpenGL path can only use four border colours (bindless handles: sampler_gl); the same rounding here */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]}, mx = b[0] > b[1] ? b[0] : b[1];
        int rgb, al;
        if (b[2] > mx) mx = b[2];
        rgb = mx >= 0.5f; al = b[3] >= 0.5f;
        if (p->border_kind) { int32_t bi[4]; memcpy(bi, p->border, 16); rgb = bi[0] || bi[1] || bi[2]; al = bi[3] != 0; }
        if (!rgb) ci.borderColor = p->border_kind ? (al ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK) : (al ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        else if (al) ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else {
            int uses = (p->ws & 7) == 5 || (p->wt & 7) == 5 || (p->wr & 7) == 5;
            if (uses && !D.has_cbc) nvn_vk_missing("a sampler border colour (1, 1, 1, 0) on a GPU without VK_EXT_custom_border_color");
            if (D.has_cbc) {
                ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                if (p->border_kind) { cb.customBorderColor.int32[0] = cb.customBorderColor.int32[1] = cb.customBorderColor.int32[2] = 1; }
                else { cb.customBorderColor.float32[0] = cb.customBorderColor.float32[1] = cb.customBorderColor.float32[2] = 1.0f; }
                cb.pNext = ci.pNext; ci.pNext = &cb;
            }
        }
    }
    if (p->reduction == 1 || p->reduction == 2) {
        if (!D.f12.samplerFilterMinmax) nvn_vk_missing("a min / max reduction sampler on a GPU without samplerFilterMinmax");
        else { red.reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX; red.pNext = ci.pNext; ci.pNext = &red; }
    }
    if (p->lod_snap != 0.0f) nvn_vk_missing("a sampler with LOD snap");
    VkSampler s;
    VKC(vkCreateSampler(V.device, &ci, NULL, &s));
    if (g_nsmp == 2048) vkd_fail("more than 2048 distinct samplers");
    g_smp[g_nsmp].p = *p; g_smp[g_nsmp++].s = s;
    return s;
}

/* GLSL -> SPIR-V: smo_glslang.dll on Windows (vk_shaderc_shim.c, loaded at the first use); linked in elsewhere
 * (vk_shaderc_android.c, shaderc built with the NDK). Both pointers stay NULL when the compiler is not there. */
typedef int (*SmoGlslFn)(int, const char *, uint32_t **, size_t *, char *, size_t);
typedef void (*SmoSpirvFreeFn)(uint32_t *);
#ifdef _WIN32
static void smo_glsl_bind(void *to, void *fr) {
    HMODULE m = LoadLibraryA("smo_glslang.dll");
    *(SmoGlslFn *)to = m ? (SmoGlslFn)GetProcAddress(m, "smo_glsl_to_spirv") : NULL;
    *(SmoSpirvFreeFn *)fr = m ? (SmoSpirvFreeFn)GetProcAddress(m, "smo_spirv_free") : NULL;
}
#else
int smo_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size);
void smo_spirv_free(uint32_t *words);
static void smo_glsl_bind(void *to, void *fr) { *(SmoGlslFn *)to = smo_glsl_to_spirv; *(SmoSpirvFreeFn *)fr = smo_spirv_free; }
#endif
#include "nvn_vk_m4.c"                             /* session 4: stages, pipelines, workers (M4) */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's stages and pipelines made at the draw); kept only until someone deletes the block */
/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h; int stage, code_len, ok; uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp;
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next;
} VStage;
/* M4 (design section 5): what a stage needs to become a module again without the translator and glslang, in
 * ..\cache\vk\stage\<stage>_<hash of machine code, constants, variant, translator version>.st */
#define MX_VK_VERSION 9u                           /* part of the stage hash: change it when mx_translate's Vulkan text changes */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp; VSmp smp[64]; } StageRec;
static plat_mutex g_sh_lock;                 /* the stage and pipeline tables while the start-up workers run */
static VkPipelineCache g_pcache;
static int64_t g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
static VStage *g_vstage[4096];
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static uint64_t g_spv_unused;
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
    }
    free(words); fclose(fp);
    return mod;
}
static void stage_rec_path(char *path, size_t n, int s, uint64_t h) { snprintf(path, n, "../cache/vk/stage/%d_%016llx.st", s, (unsigned long long)h); }
/* A stage from its record on disk (the caller holds g_sh_lock when workers run). NULL when there is none. */
static VStage *vstage_from_rec(int s, uint64_t h) {
    char path[128];
    StageRec r;
    stage_rec_path(path, sizeof path, s, h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t got = fread(&r, 1, sizeof r, fp);
    fclose(fp);
    if (got != sizeof r || r.magic != 0x52545356 + MX_VK_VERSION || r.stage != s || r.nsmp < 0 || r.nsmp > 64) return NULL;
    VkShaderModule mod = module_from_spv(r.spv);
    if (!mod) return NULL;
    VStage *v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = r.code_len; memcpy(v->key, r.key, 16); v->fo = r.fo; v->id = ++g_vstage_ids;
    v->cbufs = r.cbufs; v->ssbos = r.ssbos; v->ssbo_w = r.ssbo_w; v->images = r.images; v->dep = r.dep; v->tex_units = r.tex_units; v->in_attr = r.in_attr; v->out_attr = r.out_attr;
    v->uses_alpha = r.uses_alpha; v->nsmp = r.nsmp; memcpy(v->smp, r.smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    plat_atomic_inc(&R.modules);
    return v;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash) {
    {   /* SMO port, debug aid SMO_VK_GLSLDUMP=<folder>: every freshly translated stage as <what>.glsl (cached stages are
         * not translated again: move build/cache/vk/stage aside to dump everything) */
        const char *dir = getenv("SMO_VK_GLSLDUMP");
        if (dir) {
            static int32_t n;
            char path[300]; snprintf(path, sizeof path, "%s/%s.glsl", dir, what); for (char *q = path + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '#') *q = '_';
            FILE *f = fopen(path, "wb");
            if (f) { fputs(glsl, f); fclose(f); }
            if (plat_atomic_inc(&n) <= 2) printf("nvn_vk: GLSL dump %s: %s\n", path, f ? "written" : "NOT written");
        }
    }
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    int from_dll = 0;
    VkShaderModule mod = NULL;
    int64_t t_mod = tk();
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n > 20 && n % 4 == 0) { words = malloc((size_t)n); count = (size_t)n / 4; if (fread(words, 1, (size_t)n, fp) != (size_t)n || words[0] != 0x07230203) { free(words); words = NULL; } }
        fclose(fp);
    }
    if (!words) {
        if (!g_glsl_to_spirv) {
            smo_glsl_bind(&g_glsl_to_spirv, &g_spirv_free);
            if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("smo_glslang.dll cannot be loaded (build_glslang.bat makes it)");
        }
        if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
        from_dll = 1;
        plat_mkdirs("../cache/vk/spv");
        fp = fopen(path, "wb");
        if (fp) { fwrite(words, 4, count, fp); fclose(fp); }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    if (from_dll) g_spirv_free(words); else free(words);
    plat_atomic_inc(&R.modules);
    g_tm[6] += tk() - t_mod;
    return mod;
}
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo) {
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    if (!st->code) return NULL;
    uint32_t dep_a = v && s == 0 ? v->dep : 0;
    if (v && ((key[0] ^ v->key[0]) & dep_a) == 0 && ((key[1] ^ v->key[1]) & dep_a) == 0 && ((key[2] ^ v->key[2]) & v->tex_units) == 0 && ((key[3] ^ v->key[3]) & v->tex_units) == 0 && v->fo == fo) return v->ok ? v : NULL;
    const uint8_t *cdata = st->ctl[3] && st->ctl[4] ? st->code - (s == 5 ? 0 : 0x30) + st->ctl[4] : NULL;       /* a compute stage has no program header */
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
    if (s == 5) { h = fnv(h, st->local, sizeof st->local); h = fnv(h, &st->smem_bytes, 4); h = fnv(h, &st->lmem_bytes, 4); }
    if (cdata) h = fnv(h ^ st->ctl[3], cdata, st->ctl[3]);
    h = fnv(h, key, 16); h = fnv(h, &fo, 4);
    { uint32_t ver = MX_VK_VERSION; h = fnv(h, &ver, 4); }
    for (v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s && v->code_len == st->code_len && v->fo == fo && !memcmp(v->key, key, 16)) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v->ok ? v : NULL; }
    {   int64_t t_mod = tk();
        v = vstage_from_rec(s, h);                 /* met in an earlier run: no translator, no glslang */
        g_tm[6] += tk() - t_mod;
        if (v) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v; }
    }
    MxIn in;
    MxOut out;
    char what[64];
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = s; in.code = st->code; in.code_len = st->code_len; in.cdata = cdata; in.cdata_size = cdata ? st->ctl[3] : 0;
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3]; in.force_out = fo;
    in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
    if (s == 5) {                                  /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
        in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
        memcpy(in.local_size, st->local, sizeof in.local_size); in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    }
    v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = st->code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    st->vk = v;
    snprintf(what, sizeof what, "program #%d %s stage", p->serial, s == 5 ? "compute" : s ? "fragment" : "vertex");
    if (!mx_translate(&in, &out)) { printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout); return NULL; }
    uint64_t spv_hash = 0;
    v->mod = module_from_glsl(s, out.glsl, what, &spv_hash);
    memcpy(v->local, st->local, sizeof v->local);
    v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr; v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->nsmp = out.nsmp;
    if (s == 0) v->dep = out.int_dep; else v->tex_units = out.tex_units | out.int_dep;
    v->tex_units = out.tex_units;
    for (int i = 0; i < out.nsmp && i < 64; i++) {
        const char *ty = out.smp[i].type;
        int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
        if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
        v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
    }
    free(out.glsl);
    v->ok = v->mod != NULL;
    if (v->ok) {
        char path[128];
        StageRec r;
        memset(&r, 0, sizeof r);
        r.magic = 0x52545356 + MX_VK_VERSION; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash;
        r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
        r.uses_alpha = v->uses_alpha; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
        plat_mkdirs("../cache/vk"); plat_mkdirs("../cache/vk/stage");
        stage_rec_path(path, sizeof path, s, v->h);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
    }
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines (made at the draw that first needs them: session 2; workers are M4) */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; } PKey;
typedef struct Pipe { PKey k; VkPipeline p; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; struct Pipe *next; } Pipe;
static Pipe *g_pipes[1024];
static void pkey_targets(PKey *k);
static VkBlendFactor blend_factor(unsigned f) {
    static const uint8_t m[9] = {VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                 VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
                 case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                 case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned e) { static const uint8_t m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX}; return (VkBlendOp)m[e < 6 ? e : 1]; }
static void layout_bindings(VStage *v, int s, VkDescriptorSetLayoutBinding *b0, int *n0, VkDescriptorSetLayoutBinding *b1, int *n1) {
    VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    for (unsigned n = 1; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[(*n1)++] = b; }
}
/* Makes the pipeline of a key (any thread: the device calls are thread-safe, the pipeline cache is synchronised by the driver). */
static Pipe *pipe_make(const PKey *k, VStage *vs, VStage *fs, double *took) {
    Pipe *p = calloc(1, sizeof *p);
    plat_li t0, t1, fq;
    ((t0).QuadPart = plat_qpc());
    p->k = *k; p->vs = vs; p->fs = fs;
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &p->nb[0], b1, &p->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &p->nb[0], b1, &p->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)p->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && p->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; p->push0 = 1; }      /* 32: the smallest maxPushDescriptors a device may have */
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &p->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = p->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &p->layout));

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? fs->mod : NULL; st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1; vp.pNext = vk_vp_next();
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof *dyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
    ((t1).QuadPart = plat_qpc()); ((fq).QuadPart = plat_qpf());
    *took = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    plat_atomic_inc(&R.pipelines);
    return p;
}
/* ..\cache\vk\pipes.bin: every pipeline key this PC ever needed, with its stages named by their hashes (design section 5:
 * "pipes.txt"). Read at start by the workers; a key missing at a draw is made there and appended (once per key per PC). */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    plat_mkdirs("../cache/vk");
    FILE *fp = fopen("../cache/vk/pipes.bin", "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, k, sizeof *k);
    for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p;
    double ms = 0;
    int64_t t0 = tk();
    Pipe *p = pipe_make(k, vs, fs, &ms);           /* never met on this PC: made here, the frame is late once (counted) */
    g_tm[5] += tk() - t0;
    g_pipe_miss++;
    if (D.debug) printf("nvn_vk: pipeline miss %lld: stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    piperec_append(k, vs, fs, 0);
    return p;
}

#endif
/* ---------------------------------------------------------------- passes */
static int target_level(const TargetRef *r) { return (r->view.set & 0x81) == 0x81 ? r->view.lvl0 : 0; }
static VkImageView target_view(const TargetRef *r, VTex *vt) {
    EView e;
    eview_of(r->tex, vt, &r->view, &e);
    memset(e.swz, 0, 4); e.nlvl = 1; e.nlay = 1; e.format = r->tex->b.format; e.target = 1; e.dsmode = 0;
    if ((r->view.set & 0x84) == 0x84 && r->view.format) e.format = r->view.format;
    return vt_view(vt, &e, -1, 1);
}
static int pass_begin(void) {
    VkRenderingAttachmentInfo ca[8], da;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    int w = 0, h = 0;
    if (R.in_pass) return 1;
    cmd_begin();
    R.pass_rs = 0;
    memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da);
    for (int i = 0; i < R.ncol; i++) {
        Tex *t = R.col[i].tex;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (g_lc_mask >> i & 1) { ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].clearValue.color = g_lc_col[i]; g_lc_done++; }
        if (!t || t->dead) continue;
        VTex *vt = vt_get(t);
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("a depth format bound as colour target"); continue; }
        if (vt->is3d && ((t->b.format == 0x3f && vt->d <= 8) || getenv("SMO_VK_3DRT_ALL"))) { static int n; if (n++ < 40) printf("nvn_vk_3drt: pass %lld: 3D texture %dx%dx%d fmt 0x%x as colour %d, view set 0x%x layers %d+%d level %d\n", (long long)nvn_front_frame_next(), vt->w, vt->h, vt->d, t->b.format, i, R.col[i].view.set, R.col[i].view.lay0, R.col[i].view.nlay, target_level(&R.col[i])); }
        t->gpu_written = 1;
        vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca[i].imageView = target_view(&R.col[i], vt); ca[i].imageLayout = vt->layout;
        if (!w) { w = lvl_(vt->w, target_level(&R.col[i])); h = lvl_(vt->h, target_level(&R.col[i])); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
    }
    if (R.depth.tex && !R.depth.tex->dead) {
        VTex *vt = vt_get(R.depth.tex);
        R.depth.tex->gpu_written = 1;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) nvn_vk_missing("a colour format bound as depth target");
        else if (RS_ON && rs_ui_native() && vt->rs && !R.pass_rs && w && (lvl_(vt->w, target_level(&R.depth)) < w || lvl_(vt->h, target_level(&R.depth)) < h)) g_rs_mixed++;      /* session 9 (SMO_VK_UI_NATIVE): an unscaled colour target with a scaled depth target: the pass runs WITHOUT depth (counted, reported at exit) */
        else if (w && (lvl_(vt->w, target_level(&R.depth)) < w || lvl_(vt->h, target_level(&R.depth)) < h)) {
            /* SMO port: a depth target SMALLER than the colour targets. NVN allows targets of different sizes (each surface
             * drops what falls outside it); Vulkan needs every attachment to cover the render area, and NVIDIA then drew
             * nothing at all. SMO binds a 2x2 dummy depth texture to full-screen passes with depth test and write off
             * (title screen, boot105 pass 7: Mario's layer stayed empty and the projected shadow became black speckles).
             * The pass runs without depth; the dropped surface could only have held its own few texels. */
            static int64_t n; if (n++ < 5) printf("nvn_vk: depth target %dx%d smaller than the colour targets %dx%d: pass without depth (%lld so far)\n", lvl_(vt->w, target_level(&R.depth)), lvl_(vt->h, target_level(&R.depth)), w, h, (long long)n);
        }
        else {
            vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.imageView = target_view(&R.depth, vt); da.imageLayout = vt->layout;
            if (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
            if (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
            {   /* a load-clear of depth / stencil: one attachment info serves both aspects, so it applies only when every
                 * aspect the format has is cleared */
                unsigned need = ((vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? 0x100u : 0) | ((vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? 0x200u : 0);
                if (need && (g_lc_mask & need) == need) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.clearValue.depthStencil = g_lc_ds; g_lc_done++; }
            }
            if (!w) { w = lvl_(vt->w, target_level(&R.depth)); h = lvl_(vt->h, target_level(&R.depth)); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
        }
    }
    if (!w) return 0;
    if (RS_ON) {   /* Android port, diagnostic: a pass whose targets are not all scaled alike (the first decides the pass's size and scale) */
        int nrs = 0, nun = 0;
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && !R.col[i].tex->dead && R.col[i].tex->vk) { if (((VTex *)R.col[i].tex->vk)->rs) nrs++; else nun++; }
        if (R.depth.tex && !R.depth.tex->dead && R.depth.tex->vk) { if (((VTex *)R.depth.tex->vk)->rs) nrs++; else nun++; }
        static int nlog;
        if (nrs && nun && nlog < 30) {
            nlog++;
            printf("nvn_vk_scale: MIXED pass in frame %lld (pass scaled %d, %dx%d):", nvn_front_frame_next(), R.pass_rs, w, h);
            for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) { VTex *v = R.col[i].tex->vk; printf(" col%d game %dx%d image %dx%d fmt 0x%x flags 0x%x target %d layers %d rs %d;", i, R.col[i].tex->b.width, R.col[i].tex->b.height, v->w, v->h, R.col[i].tex->b.format, R.col[i].tex->b.flags, R.col[i].tex->b.target, v->layers, v->rs); }
            if (R.depth.tex && R.depth.tex->vk) { VTex *v = R.depth.tex->vk; printf(" depth game %dx%d image %dx%d fmt 0x%x flags 0x%x rs %d", R.depth.tex->b.width, R.depth.tex->b.height, v->w, v->h, R.depth.tex->b.format, R.depth.tex->b.flags, v->rs); }
            printf("\n");
        }
    }
    {   /* session 9, SMO_VK_UILOG=<n>: every pass begun in the n-th battle frame ("nvn_vk_ui:" lines): its targets, whether the
         * colour target is a texture of the window, and the draws of the frame so far (the next line's number minus this one = its draws) */
        static int uil = -2, np;
        if (uil == -2) uil = getenv("SMO_VK_UILOG") ? atoi(getenv("SMO_VK_UILOG")) : -1;
        if (uil > 0 && g_battle_frames == uil) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL, *dt = R.depth.tex && !R.depth.tex->dead ? R.depth.tex : NULL;
            printf("nvn_vk_ui: pass %d: %d colour, first %dx%d format 0x%x level %d%s%s | depth %dx%d format 0x%x%s | image size of the pass %dx%d | %lld draws so far, %lld dispatches so far\n", np++, R.ncol,
                   c0 ? c0->b.width : 0, c0 ? c0->b.height : 0, c0 ? (int)c0->b.format : 0, c0 ? target_level(&R.col[0]) : 0, c0 && wintex_is(c0) ? " WINDOW TEXTURE" : "", c0 && c0->vk && ((VTex *)c0->vk)->rs ? " (scaled)" : "",
                   dt ? dt->b.width : 0, dt ? dt->b.height : 0, dt ? (int)dt->b.format : 0, dt && dt->vk && ((VTex *)dt->vk)->rs ? " (scaled)" : "", w, h, (long long)R.frame_draws, (long long)R.dispatches);
        }
    }
    {   /* SMO_VK_RTLOG=1 (track O, resolution scale survey): every distinct render-target size a pass is begun with, with
         * the colour / depth formats and how many passes used it (printed at exit: "nvn_vk_rt:" lines) */
        static int on = -1;
        if (on < 0) on = getenv("SMO_VK_RTLOG") && atoi(getenv("SMO_VK_RTLOG"));
        if (on) {
            int cf = R.ncol && R.col[0].tex ? (int)R.col[0].tex->b.format : 0, df = R.depth.tex ? (int)R.depth.tex->b.format : 0, lv = R.ncol && R.col[0].tex ? target_level(&R.col[0]) : R.depth.tex ? target_level(&R.depth) : 0, i;
            for (i = 0; i < g_rtlog_n; i++) if (g_rtlog[i].w == w && g_rtlog[i].h == h && g_rtlog[i].ncol == R.ncol && g_rtlog[i].cf == cf && g_rtlog[i].df == df && g_rtlog[i].lv == lv) break;
            if (i == g_rtlog_n && g_rtlog_n < 256) { g_rtlog[i].w = w; g_rtlog[i].h = h; g_rtlog[i].ncol = R.ncol; g_rtlog[i].cf = cf; g_rtlog[i].df = df; g_rtlog[i].lv = lv; g_rtlog[i].first = nvn_front_frame_next(); g_rtlog_n++; }
            if (i < 256) { g_rtlog[i].n++; if (g_in_fight) g_rtlog[i].fight++; }
        }
    }
    cmd_begin();                                   /* vt_get may have submitted (ring wrap) */
    g_lc_mask = 0;                                 /* the load-clears are in this pass's attachment infos */
    if (g_pt_on && g_pt_n < PT_MAX) {
        g_pt[g_pt_n].w = w; g_pt[g_pt_n].h = h; g_pt[g_pt_n].ncol = R.ncol; g_pt[g_pt_n].d0 = R.frame_draws;
        g_pt[g_pt_n].cfmt = R.ncol && R.col[0].tex ? (int)R.col[0].tex->b.format : 0; g_pt[g_pt_n].dfmt = R.depth.tex ? (int)R.depth.tex->b.format : 0;
        g_pt[g_pt_n].load = g_pt[g_pt_n].clear = g_pt[g_pt_n].disc = 0;
        for (int i = 0; i < R.ncol; i++) if (ca[i].imageView) { if (ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) g_pt[g_pt_n].load |= 1u << i; else g_pt[g_pt_n].clear |= 1u << i; }
        if (ri.pDepthAttachment || ri.pStencilAttachment) { if (da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) g_pt[g_pt_n].load |= 0x100; else g_pt[g_pt_n].clear |= 0x100; }
        pt_mark(0);
    }
    ri.renderArea.extent.width = (uint32_t)w; ri.renderArea.extent.height = (uint32_t)h; ri.layerCount = 1; ri.colorAttachmentCount = (uint32_t)R.ncol; ri.pColorAttachments = ca;
    vkCmdBeginRendering(R.cmd, &ri);
    g_fr_passes++;
    R.in_pass = 1; R.pass_w = w; R.pass_h = h;
    return 1;
}

/* ---------------------------------------------------------------- draws */
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    Slot *s = &R.slot[R.cur];
    VkDescriptorSet set;
    for (;;) {
        if (s->ipool < s->npool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s->pool[s->ipool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            VkResult r = vkAllocateDescriptorSets(V.device, &ai, &set);
            if (r == VK_SUCCESS) { R.sets++; return set; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) vk_fatal("vkAllocateDescriptorSets", r);
            s->ipool++;
            continue;
        }
        if (s->npool == 16) vkd_fail("more than 16 descriptor pools in one command buffer");
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 16384}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 8192; pi.poolSizeCount = D.dyn0 ? 3 : 2; pi.pPoolSizes = ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &s->pool[s->npool++]));
    }
}
static struct { uint64_t key; VkDescriptorSet set; uint32_t gen; VkDescriptorSetLayout dl; int n, off; } g_tset[4096];      /* texture sets of the command buffer being recorded */
static struct { VTex *d2, *d1, *d3; VkImageView v[7]; } g_dummy;
static struct { VkImageView view; VkSampler smp; VkImageLayout layout; } g_dtex[200], g_tset_list[32768];      /* g_tset_list: the lists of the cached sets, for the full comparison */
static int g_ndtex;
static struct { uint64_t key; VkDescriptorSet set; uint32_t gen; VkDescriptorSetLayout dl; int n, off; } g_bset[4096];      /* buffer sets (set 0) of the command buffer being recorded, without push descriptors */
static VkDescriptorBufferInfo g_bset_list[32768];  /* their lists (dynamic ones at offset 0), for the full comparison */
/* The image view and sampler of one declared sampler of stage s. May end the pass (upload, layout change). */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride);
/* SMO, 1.4.1: a sampler whose handle the game keeps in one of its uniform buffers (mx_translate.c bindless_unit: unit
 * 32 + 8 * hardware buffer + 64-bit word; hardware buffer n >= 3 is the stage's uniform buffer slot n - 3), read at the draw.
 * 0 when the buffer is not bound or too short (the shader of 1.4.1's primitive drawer then draws untextured itself). */
static uint64_t uniform_handle(int s, unsigned u) {
    unsigned cb = (u - 32) / 8, off = (u - 32) % 8 * 8;
    if (u < 32 || cb < 3 || cb >= 19) return 0;
    const Range *b = &R.ubo[s][cb - 3];
    if (!b->addr || b->size < off + 8) return 0;
    uint64_t h;
    memcpy(&h, (const void *)(uintptr_t)(b->addr + off), 8);
    return h;
}
static void resolve_sampler(int s, VStage *v, int i) {
    unsigned u = v->smp[i].unit;
    Tex *t = NULL;
    View view;
    SamplerParams sp;
    uint64_t handle = u < 32 ? R.nvn_tex[s][u] : uniform_handle(s, u);
    int r = u < 32 || handle ? nvn_front_handle(R.tpool, R.spool, handle, &t, &view, &sp) : 0;
    if (u >= 32) {      /* the first draw of each program that takes a texture this way: what it got (says where the game uses it) */
        static int said[64], nsaid;
        int serial = R.prog[s] ? R.prog[s]->serial : -1, k;
        for (k = 0; k < nsaid && said[k] != serial; k++) {}
        if (k == nsaid && nsaid < 64) {
            said[nsaid++] = serial;
            printf("nvn_vk: program #%d, %s stage: texture handle %llx from uniform buffer %u + %u (first draw, frame %lld) -> %s",
                   serial, s ? "fragment" : "vertex", (unsigned long long)handle, (u - 32) / 8 - 3, (u - 32) % 8 * 8,
                   (long long)nvn_front_frame_next(), (r & 1) ? "" : "no texture (an untextured draw)\n");
            if (r & 1) printf("texture format 0x%x, %d x %d\n", t->b.format, t->b.width, t->b.height);
        }
    }
    {   /* SMO port, with SMO_VK_PASSSHOT: what each draw of that frame samples (pass_shot's frame) */
        static int want = -2;
        want = passshot_frame() ? 1 : -1;
        if (want >= 0) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL;
            printf("nvn_vk_bind: stage %d unit %u handle %llx -> %s tex %p fmt 0x%x %dx%d mem %p gpu_written %d | target tex %p fmt 0x%x mem %p\n",
                   s, u, (unsigned long long)handle, (r & 1) ? "live" : "NO TEXTURE", (void *)((r & 1) ? t : NULL),
                   (r & 1) ? (unsigned)t->b.format : 0u, (r & 1) ? t->b.width : 0, (r & 1) ? t->b.height : 0, (void *)((r & 1) ? t->mem : NULL),
                   (r & 1) ? t->gpu_written : -1, (void *)c0, c0 ? (unsigned)c0->b.format : 0u, (void *)(c0 ? c0->mem : NULL));
            if (!(r & 1) && handle) printf("nvn_vk_bind:   why: %s\n", nvn_front_handle_why(R.tpool, handle));
            if ((r & 1) && t->b.target == 2 && t->b.format == 0x3f && t->b.width * t->b.height * t->b.depth <= 4096) {      /* SMO debug aid: a small R11G11B10F volume (colour-grading LUT): its contents */
                static uint32_t px[4096];
                int rr[6] = {0, 0, 0, t->b.width, t->b.height, t->b.depth}, n = t->b.width * t->b.height * t->b.depth;
                if (read_region(t, NULL, rr, px, 0, 0)) {
                    double m[3] = {0, 0, 0};
                    float first[4][3];
                    for (int k = 0; k < n; k++) {
                        uint32_t w = px[k], bits[3] = {w & 0x7ff, (w >> 11) & 0x7ff, w >> 22}, mb[3] = {6, 6, 5};
                        for (int ch = 0; ch < 3; ch++) {        /* unsigned float: 5-bit exponent, 6/6/5-bit mantissa */
                            uint32_t ex = bits[ch] >> mb[ch], man = bits[ch] & ((1u << mb[ch]) - 1);
                            float f = ex == 0 ? ldexpf((float)man, -14 - (int)mb[ch]) : ex == 31 ? 65504.0f : ldexpf(1.0f + (float)man / (float)(1u << mb[ch]), (int)ex - 15);
                            m[ch] += f; if (k < 4) first[k][ch] = f;
                        }
                    }
                    printf("nvn_vk_lut: %dx%dx%d R11G11B10F mean %.3f %.3f %.3f | texel 0: %.3f %.3f %.3f  1: %.3f %.3f %.3f  2: %.3f %.3f %.3f  3: %.3f %.3f %.3f | last raw %08x\n",
                           t->b.width, t->b.height, t->b.depth, m[0] / n, m[1] / n, m[2] / n, first[0][0], first[0][1], first[0][2], first[1][0], first[1][1], first[1][2],
                           first[2][0], first[2][1], first[2][2], first[3][0], first[3][1], first[3][2], px[n - 1]);
                }
            }
        }
    }
    g_dtex[g_ndtex].view = g_dummy.v[v->smp[i].dim]; g_dtex[g_ndtex].smp = g_smp_default; g_dtex[g_ndtex].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (r & 1) {
        VTex *vt = vt_get(t);
        EView e;
        if (g_split_bar && (s == 0 || s == 2) && t->gpu_written) {      /* 0.1.7: the assumption of image_barrier does not hold */
            g_split_bar = 0;
            printf("nvn_vk: stage %d of program #%d samples a render target (%dx%d): split pass barriers off from here on\n", s, R.prog[s] ? R.prog[s]->serial : -1, t->b.width, t->b.height);
        }
        int attached = R.depth.tex == t, dim = v->smp[i].dim;
        for (int k = 0; k < R.ncol; k++) attached |= R.col[k].tex == t;
        eview_of(t, vt, &view, &e);
        int have = target_dim(e.target);
        int ok = have == dim || (dim == DIM_2D && (have == DIM_2DA || have == DIM_CUBE || have == DIM_CUBEA)) || (dim == DIM_2DA && (have == DIM_2D || have == DIM_CUBE || have == DIM_CUBEA)) ||
                 (dim == DIM_CUBE && have == DIM_CUBEA) || (dim == DIM_CUBEA && have == DIM_CUBE) || (dim == DIM_1D && have == DIM_1DA) || (dim == DIM_1DA && have == DIM_1D) ||
                 ((dim == DIM_CUBE || dim == DIM_CUBEA) && (have == DIM_2DA) && e.nlay >= 6 && vt->w == vt->h);
        {   static int want2 = -2;
            want2 = passshot_frame() ? 1 : -1;
            if (want2 >= 0)
                printf("nvn_vk_bind:   shader wants dim %d, view gives dim %d (view target %d, %d layers, levels %d+%d), samples %d, attached %d -> %s\n",
                       dim, have, e.target, e.nlay, e.lvl0, e.nlvl, vt->samples, attached, ok && vt->samples == 1 ? "BOUND" : "DUMMY (black)");
            if (want2 >= 0 && (r & 2))
                printf("nvn_vk_bind:   sampler min %d mag %d lod %g..%g bias %g aniso %g | stage spv %016llx\n", sp.min, sp.mag, sp.lodmin, sp.lodmax, sp.bias, sp.aniso, (unsigned long long)v->spv);
        }
        if (ok && vt->samples == 1) {
            if (attached && have_targets_live()) { if (!vt->feedback) { vt->feedback = 1; R.feedback++; pass_end(); } vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL); }
            else vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_dtex[g_ndtex].view = vt_view(vt, &e, dim, 0); g_dtex[g_ndtex].layout = vt->layout;
        } else { static int n; R.dummy_kind++;
            if (n < 3) printf("nvn_vk: (detail of the next line) frame %lld, program #%d, texture format 0x%x, %d x %d x %d, %d levels, draws so far with a dummy for a view-kind mismatch: %lld\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.format, t->b.width, t->b.height, t->b.depth, nvn_front_tex_levels(t), (long long)R.dummy_kind);
            if (n++ < 20) printf("nvn_vk: texture unit %u of the %s stage: the shader declares view kind %d, the texture is kind %d (target %d, %d samples): a dummy is bound\n", u, s ? "fragment" : "vertex", dim, have, e.target, vt->samples); }
        if (r & 2) g_dtex[g_ndtex].smp = sampler_get(&sp);
        else {  /* a handle whose sampler ID was never registered (the game's sampler manager answers ID 0 when its pool is full) */
            static int64_t ns; if (ns++ % 20000 == 0) printf("nvn_vk: frame %lld: program #%d binds texture %dx%d with sampler ID %u, which is not registered: the default sampler (%lld such binds)\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.width, t->b.height, (unsigned)(handle >> 20) & 0xfff, (long long)ns);
        }
        if ((r & 2) && (sp.mag || (sp.min < 6 ? (sp.min & 1) || sp.min >= 4 : 1)) && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            static int on = -1;                    /* 0.1.2: SMO_VK_UNSUP_NEAREST=1 filters such a texture as nearest; by default it stays linear (what the game asks; the GPU's result) and is only named in the log */
            if (on < 0) on = getenv("SMO_VK_UNSUP_NEAREST") && atoi(getenv("SMO_VK_UNSUP_NEAREST")) ? 1 : 0;
            static int only = -2;                  /* debug aid SMO_VK_SIM_NEAREST_STAGE=<stage>: the nearest fallback in that stage only (0 vertex, 1 fragment) */
            if (only == -2) only = getenv("SMO_VK_SIM_NEAREST_STAGE") ? atoi(getenv("SMO_VK_SIM_NEAREST_STAGE")) : -1;
            static int only_u = -2;                /* and SMO_VK_SIM_NEAREST_UNIT=<unit>: that texture unit only */
            if (only_u == -2) only_u = getenv("SMO_VK_SIM_NEAREST_UNIT") ? atoi(getenv("SMO_VK_SIM_NEAREST_UNIT")) : -1;
            SamplerParams sn = sp;                 /* undefined on such a GPU: nearest instead (mip levels nearest as well) */
            sn.mag = 0; sn.min = sp.min >= 2 && sp.min < 6 ? 2 : 0;
            if (on && (only < 0 || only == s) && (only_u < 0 || (int)u == only_u)) g_dtex[g_ndtex].smp = sampler_get(&sn);
            static struct { int prog; int fmt; } said[64]; static int nsaid;      /* Mali: a linear sampler on a format the GPU cannot filter is undefined (once per program and format) */
            int serial = R.prog[s] ? R.prog[s]->serial : -1, k;
            for (k = 0; k < nsaid && (said[k].prog != serial || said[k].fmt != (int)vt->fmt); k++) {}
            if (k == nsaid && nsaid < 64) {
                said[nsaid].prog = serial; said[nsaid].fmt = (int)vt->fmt; nsaid++;
                printf("nvn_vk_fmt: frame %lld: program #%d (%s stage, unit %u) samples a %dx%d texture of NVN format 0x%x (Vulkan %d) with a LINEAR filter, which this GPU cannot do for that format: %s\n",
                       (long long)nvn_front_frame_next(), serial, s == 2 ? "geometry" : s ? "fragment" : "vertex", u, t->b.width, t->b.height, t->b.format, (int)vt->fmt, on ? "nearest instead (SMO_VK_UNSUP_NEAREST=1)" : "left linear");
            }
        }
        /* Depth textures read with a linear filter (SMO's sun shadow mask reads its 1024x1024 DEPTH32F map so). Filtering depth is
         * optional in Vulkan and the check above is colour only. Named once per format; SMO_VK_SIM_DEPTH_NEAREST=1 reads such
         * textures nearest on any GPU, to see what a GPU without the feature would show. */
        if ((r & 2) && (sp.mag || (sp.min < 6 ? (sp.min & 1) || sp.min >= 4 : 1)) && vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            static int sim = -1, said[8], nsaid;
            int k, can = fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
            if (sim < 0) sim = getenv("SMO_VK_SIM_DEPTH_NEAREST") && atoi(getenv("SMO_VK_SIM_DEPTH_NEAREST")) ? 1 : 0;
            for (k = 0; k < nsaid && said[k] != (int)vt->fmt; k++) {}
            if (k == nsaid && nsaid < 8) {
                said[nsaid++] = (int)vt->fmt;
                printf("nvn_vk_fmt: frame %lld: program #%d samples a %dx%d depth texture, NVN format 0x%x (Vulkan %d), with a LINEAR filter: this GPU %s%s\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.width, t->b.height, t->b.format, (int)vt->fmt, can ? "can filter it" : "CANNOT filter it", sim ? " (read nearest: SMO_VK_SIM_DEPTH_NEAREST=1)" : "");
            }
            if (sim) { SamplerParams sn = sp; sn.mag = 0; sn.min = sp.min >= 2 && sp.min < 6 ? 2 : 0; g_dtex[g_ndtex].smp = sampler_get(&sn); }
        }
    }
    g_ndtex++;
}
static int have_targets_live(void);
/* Track O: a rectangle of the game's pixels (scissor, clear) on the scaled images of the pass: every image pixel the
 * rectangle touches (floor of the first edge, ceiling of the last). */
static void rs_rect(int *x, int *y, int *w, int *h) {
    int x0 = (int)floorf((float)*x * R.pass_sx), y0 = (int)floorf((float)*y * R.pass_sy);
    int x1 = (int)ceilf((float)(*x + *w) * R.pass_sx - 0.001f), y1 = (int)ceilf((float)(*y + *h) * R.pass_sy - 0.001f);
    *x = x0; *y = y0; *w = x1 > x0 ? x1 - x0 : 0; *h = y1 > y0 ? y1 - y0 : 0;
}
static void set_dynamic_state(void) {
    VkViewport vp;
    VkRect2D sc;
    uint64_t d = R.ds;
    float x = R.have_viewport ? R.viewport[0] : 0, y = R.have_viewport ? R.viewport[1] : 0, w = R.have_viewport ? R.viewport[2] : (float)R.pass_w, h = R.have_viewport ? R.viewport[3] : (float)R.pass_h;
    if (R.pass_rs && R.have_viewport) { x *= R.pass_sx; w *= R.pass_sx; y *= R.pass_sy; h *= R.pass_sy; }      /* track O: the game's rectangle on the scaled image */
    vp.x = x; vp.y = y + h; vp.width = w; vp.height = -h; vp.minDepth = R.drange[0]; vp.maxDepth = R.drange[1];      /* NVN upper-left origin: clip y = +1 is row y (design, section 3) */
    if (vp.minDepth < 0) vp.minDepth = 0; if (vp.maxDepth > 1) vp.maxDepth = 1;
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
    if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
    if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
    if (sx > R.pass_w) sx = R.pass_w; if (sy > R.pass_h) sy = R.pass_h;
    if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
    sc.offset.x = sx; sc.offset.y = sy; sc.extent.width = (uint32_t)(sw > 0 ? sw : 0); sc.extent.height = (uint32_t)(sh > 0 ? sh : 0);
    vkCmdSetScissor(R.cmd, 0, 1, &sc);
    if (R.pass_rs && D.f2.features.wideLines) { float lw = R.line_width * R.pass_sx; vkCmdSetLineWidth(R.cmd, lw < V.props.limits.lineWidthRange[0] ? V.props.limits.lineWidthRange[0] : lw); } else
    vkCmdSetLineWidth(R.cmd, D.f2.features.wideLines ? R.line_width : 1.0f);
    vkCmdSetDepthBias(R.cmd, R.polyoff[1], D.f2.features.depthBiasClamp ? R.polyoff[2] : 0.0f, R.polyoff[0]);
    vkCmdSetBlendConstants(R.cmd, R.blendc);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.svmask[0]); vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.svmask[1]);
    vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.swmask[0]); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.swmask[1]);
    vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.sref[0]); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.sref[1]);
    { static int nocull = -1; if (nocull < 0) nocull = getenv("SMO_VK_DIAG_NOCULL") != NULL;      /* SMO port: diagnosis only */
      vkCmdSetCullMode(R.cmd, nocull ? 0 : (R.poly[0] & 3)); }
    vkCmdSetFrontFace(R.cmd, R.poly[1] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    { static int nodepth = -1; if (nodepth < 0) nodepth = getenv("SMO_VK_DIAG_NODEPTH") != NULL;      /* SMO port: diagnosis only */
      vkCmdSetDepthTestEnable(R.cmd, nodepth ? 0 : (d & 1)); vkCmdSetDepthWriteEnable(R.cmd, (d >> 1) & 1); }
    { unsigned f = (unsigned)(d >> 4) & 15; vkCmdSetDepthCompareOp(R.cmd, (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 7)); }
    vkCmdSetStencilTestEnable(R.cmd, (d >> 2) & 1);
    {   static const uint8_t op[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
#define SOP(sh) ((VkStencilOp)op[((d >> (sh)) & 15) < 9 ? ((d >> (sh)) & 15) : 1])
#define SFN(sh) ((VkCompareOp)((((d >> (sh)) & 15) >= 1 && ((d >> (sh)) & 15) <= 8) ? ((d >> (sh)) & 15) - 1 : 7))
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_BIT, SOP(16), SOP(24), SOP(20), SFN(8));       /* (fail, pass, depth fail, compare); NVN order: stencil fail, depth fail, depth pass */
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_BACK_BIT, SOP(28), SOP(36), SOP(32), SFN(12));
#undef SOP
#undef SFN
    }
    vkCmdSetDepthBiasEnable(R.cmd, (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1);          /* offset enable bits: point, line, fill; the polygon mode picks one */
    vkCmdSetRasterizerDiscardEnable(R.cmd, R.rast_discard);
}
static int have_targets_live(void) { return R.in_pass || R.have_targets; }
/* Session 4: the state that left the pipeline key (VK_EXT_extended_dynamic_state3). Set after a pipeline is bound, whenever
 * one of the values changed, the pass changed, or a pipeline of our own (static state) was bound in between. */
static void set_dynamic_state3(int samples) {
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
    for (int i = 0; i < 8; i++) {
        const uint8_t *b = R.blend[i];
        en[i] = R.blend_on >> i & 1; wm[i] = (R.chanmask >> (i * 4)) & 15;
        eq[i].srcColorBlendFactor = blend_factor(b[0]); eq[i].dstColorBlendFactor = blend_factor(b[1]); eq[i].colorBlendOp = blend_op(b[4]);
        eq[i].srcAlphaBlendFactor = blend_factor(b[2]); eq[i].dstAlphaBlendFactor = blend_factor(b[3]); eq[i].alphaBlendOp = blend_op(b[5]);
    }
    vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en);
    vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq);
    vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
    vkCmdSetPolygonModeEXT(R.cmd, R.poly[2] == 0 ? VK_POLYGON_MODE_POINT : R.poly[2] == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetAlphaToCoverageEnableEXT(R.cmd, R.ms[1] != 0);
    { VkSampleMask m = R.sample_mask; vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(samples ? samples : 1), &m); }
    if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, R.depth_clamp != 0);
    R.dyn3_samples = samples;
}

/* ---------------------------------------------------------------- compute (M3; design section 3, "DispatchCompute" and "Storage buffers written by compute")
 * A storage buffer a compute stage may write gets a Vulkan buffer of its own (CB: host-visible memory, filled from guest
 * memory at its first dispatch of the batch). Later commands of the SAME batch whose range lies inside it bind that buffer
 * (the fighter's vertex buffers: no round trip). At the end of the batch the command buffer is submitted and the CBs become
 * "pending"; when that submission has finished the ranges a dispatch REALLY wrote (mx_wm, one word per dispatch in the
 * stream ring) are copied back to guest memory (cb_land): at the latest before the next batch reads guest memory, earlier
 * when the render thread is idle or a sync is waited for. The OpenGL path copies back before the next draw; what guest
 * memory holds once the batch is done is the same. Anything that reads guest memory through a range that only partly lies
 * in a CB forces the copy-back at once (cb_sync: submit, wait; counted). */
typedef struct { uint64_t addr, size, cap, busy; VkBuffer buf; VMem mem; int state, dirty; uint8_t *snap; uint64_t snap_cap; } CB;     /* state: 0 free, 1 live in this batch, 2 pending (batch submitted); snap: guest memory as it was when the CB was filled */
static int g_cb_merge = -1;                        /* SMO_VK_CBMERGE=0: the whole range is copied back as before session 5 */
typedef struct { uint64_t wm_off; uint32_t written; int16_t cb[16]; } CDisp;
/* SMO: 256 were not enough for a Cascade Kingdom room (WindBlowExStage: "more than 256 storage buffers written by compute
 * in one batch" at its first camera turn, stage stress test, tests/stress_stages.py); its many small skinned objects each
 * have a written range of their own. */
#define CB_MAX 4096
static CB g_cb[CB_MAX];
static int g_cb_used;                              /* slots taken into use so far (the walks over g_cb stop there) */
static CDisp g_cdisp[2048];
static int g_ncdisp, g_cb_live, g_cb_pending, g_cb_cached = -1;
static uint64_t g_cb_serial;                       /* the submission the pending CBs wait for */
static void cb_masks(void) {                       /* every recorded dispatch has run: which buffers did each one write? */
    for (int i = 0; i < g_ncdisp; i++) {
        uint32_t wm;
        memcpy(&wm, (uint8_t *)R.ring_mem.map + g_cdisp[i].wm_off, 4);
        wm &= g_cdisp[i].written;
        for (int k = 0; k < 16; k++) if ((wm >> k & 1) && g_cdisp[i].cb[k] >= 0) g_cb[g_cdisp[i].cb[k]].dirty = 1;
    }
    g_ncdisp = 0;
}
static uint32_t g_cb_epoch;
/* midbatch: the command being prepared may still name a freed CB's buffer (it is recorded into the next submission), so
 * that buffer is not given out again before that submission has finished. */
static void rq_flush(void);                         /* below: the report values that waited for compute results */
static void cb_land(int midbatch) {                /* the GPU has finished everything that wrote a CB */
    int64_t t0 = tk();
    cb_masks();
    for (int i = 0; i < g_cb_used; i++) {
        CB *c = &g_cb[i];
        if (!c->state) continue;
        if (c->dirty) host_wrote(c->addr, c->size, GEN_CB);      /* guest memory changes: ranges the ring copied from it are copied again */
        if (c->dirty && !c->snap) { memcpy(P(c->addr), c->mem.map, (size_t)c->size); R.cb_bytes += (int64_t)c->size; }
        else if (c->dirty) {
            /* Session 5: the copy-back is DEFERRED (up to the next batch), so the guest may have written into the range since
             * the CB was filled. Only what compute changed goes back: a word the guest changed meanwhile (guest != snapshot)
             * is kept and counted; when compute changed that word too it is a real conflict (counted apart; the guest's later
             * write stays, as on the OpenGL path where the copy-back comes before it). */
            uint64_t *g = P(c->addr);
            const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)c->mem.map;
            size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
            int64_t gw = 0, cf = 0;
            for (size_t k = 0; k < nw; k++) {
                uint64_t sv = s[k], mv = m[k];
                if (mv == sv) { if (g[k] != sv) gw++; continue; }
                if (g[k] == sv) g[k] = mv; else { gw++; cf++; }
            }
            if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
            R.cb_bytes += (int64_t)c->size; R.cb_guest_words += gw; R.cb_conflict_words += cf;
            if (gw) { static int n; if (n++ < 10) printf("nvn_vk: copy-back of %llx+%llu: the guest wrote %lld words of the range while it was pending (%lld of them also written by compute); merged\n", (unsigned long long)c->addr, (unsigned long long)c->size, (long long)gw, (long long)cf); }
        }
        c->state = c->dirty = 0;
        c->busy = midbatch ? R.serial + 1 : 0;
    }
    g_cb_live = g_cb_pending = 0;
    nvn_front_ccmp_landed();                       /* SMO_COMPUTE_CMP: what the dispatches wrote is in guest memory now */
    g_cb_epoch++;
    g_tm[1] += tk() - t0;
    rq_flush();                                    /* every compute result is in guest memory now: the reports queued behind them */
}
static void cb_ring_wrap(void) { if (g_cb_pending) cb_land(0); else cb_masks(); }      /* after cmd_wait_all: the mask words in the ring are about to be reused */
static void cb_poll(void) {                        /* pending CBs whose submission has finished are copied back; never waits */
    if (!g_cb_pending) return;
    if (R.done < g_cb_serial) for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && slot_finished(&R.slot[i])) slot_wait(&R.slot[i]);
    if (R.done >= g_cb_serial) cb_land(0);
}
__attribute__((noinline)) static void cb_sync(void) {      /* guest memory must hold what compute wrote, now */
    int mid = g_cb_live != 0;
    if (!g_cb_live && !g_cb_pending) return;
    if (g_cb_live) { g_cb_serial = cmd_submit(NULL, NULL); R.cb_syncs++; }
    if (R.done < g_cb_serial) {
        plat_li t0, t1;
        ((t0).QuadPart = plat_qpc());
        for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && R.slot[i].serial <= g_cb_serial) slot_wait(&R.slot[i]);
        if (R.done < g_cb_serial) R.done = g_cb_serial;
        ((t1).QuadPart = plat_qpc());
        R.cb_waits++; R.cb_wait_ticks += t1.QuadPart - t0.QuadPart;
        if (g_ww_on > 0) ww_note(__builtin_return_address(0), t1.QuadPart - t0.QuadPart);      /* SMO_VK_WAITWHO: which caller of cb_sync waited */
    }
    cb_land(mid);
}
/* Android port, REPORT VALUES (nvnCommandBufferReportValue, the bottom-of-pipe timestamp counter). The GPU writes them
 * when it gets there: what came before, compute results included, is done. They were written here at replay time after
 * cb_sync, which submitted the batch and waited for the GPU to finish it: measured on a Galaxy S24 Ultra in the opening
 * cutscene, 196 waits per 300 frames, ~13 ms each (the render thread runs a frame ahead, so each wait emptied the GPU).
 * Now, while compute results are outstanding, the report is QUEUED, in order, and written (with the time of writing)
 * when they land (cb_land: a later dispatch, a read of their addresses, or the render thread running out of work,
 * nvn_vk_idle: a game thread that waits for the value is never left waiting). SMO_VK_REPORT_DEFER=0: as before. */
typedef struct { uint64_t addr; uint32_t value, counter; uint64_t ts; } RepQ;      /* ts: 0.1.7, the GPU's time came in first (cq_resolve) */
static RepQ g_rq[256];
static int g_rq_n;
static int64_t g_rq_queued, g_rq_max;
static int rq_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SMO_VK_REPORT_DEFER");
#ifdef __ANDROID__
        on = e ? atoi(e) != 0 : 1;
#else
        on = e ? atoi(e) != 0 : 0;
#endif
    }
    return on;
}
static uint64_t counter_ticks(uint64_t ns) { return g_cq_mode == 2 ? ns : (uint64_t)((unsigned __int128)ns * 384 / 625); }      /* Switch GPU ticks, 614.4 MHz */
static void rq_write(uint64_t addr, uint32_t value, int counter) {
    uint64_t d[2] = {counter ? 0 : (uint64_t)value, counter_ticks(vk_now_ns())};
    memcpy(P(addr), d, 16);
    host_wrote(addr, 16, GEN_REPORT);
}
static void cq_note(uint64_t addr) {               /* OP_COUNTER_REPORT, timestamp type */
    if (g_cq_mode != 1) return;
    cmd_begin();
    int sl = R.cur;
    if (g_cq_n[sl] >= CQ_MAX) return;
    vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_cq_pool, (uint32_t)sl * CQ_MAX + (uint32_t)g_cq_n[sl]);
    g_cq_addr[sl][g_cq_n[sl]++] = addr;
}
static void cq_resolve(Slot *s) {                  /* its command buffer is done */
    int sl = (int)(s - R.slot), n = g_cq_n[sl];
    uint64_t t[CQ_MAX];
    if (!n || vkGetQueryPoolResults(V.device, g_cq_pool, (uint32_t)sl * CQ_MAX, (uint32_t)n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    double per = V.props.limits.timestampPeriod;
    int64_t cand = (int64_t)vk_now_ns() - (int64_t)((double)t[n - 1] * per);
    if (!g_cq_cal || cand < g_cq_off) { g_cq_off = cand; g_cq_cal = 1; }
    for (int k = 0; k < n; k++) {
        uint64_t addr = g_cq_addr[sl][k], v = counter_ticks((uint64_t)((int64_t)((double)t[k] * per) + g_cq_off));
        memcpy((uint8_t *)P(addr) + 8, &v, 8);
        host_wrote(addr, 16, GEN_REPORT);
        for (int i = 0; i < g_rq_n; i++) if (g_rq[i].addr == addr) g_rq[i].ts = v;
    }
    g_cq_n[sl] = 0;
}
static void cq_poll(void) { for (int i = 0; i < NSLOT; i++) if (g_cq_n[i] && R.slot[i].pending && slot_finished(&R.slot[i])) cq_resolve(&R.slot[i]); }
static void rq_flush(void) {
    for (int i = 0; i < g_rq_n; i++) { rq_write(g_rq[i].addr, g_rq[i].value, (int)g_rq[i].counter); if (g_rq[i].ts) { memcpy((uint8_t *)P(g_rq[i].addr) + 8, &g_rq[i].ts, 8); host_wrote(g_rq[i].addr, 16, GEN_REPORT); } }
    g_rq_n = 0;
}
static void report_write(uint64_t addr, uint32_t value, int counter) {
    if (rq_on() && (g_cb_live || g_cb_pending || g_rq_n) && g_rq_n < 256) {
        g_rq[g_rq_n].addr = addr; g_rq[g_rq_n].value = value; g_rq[g_rq_n].counter = (uint32_t)counter; g_rq[g_rq_n].ts = 0; g_rq_n++;
        g_rq_queued++; if (g_rq_n > g_rq_max) g_rq_max = g_rq_n;
        return;
    }
    cb_sync();                                     /* nothing outstanding: no wait (or the queue is full: everything lands, in order) */
    rq_write(addr, value, counter);
}
/* nvn_gl.c render_loop: the render thread has no job left. Queued reports are written now (their compute results are
 * waited for): a game thread may be waiting for one before it hands over more work. Otherwise results the GPU has
 * finished land without a wait. */
/* Android port, SMO_VK_IDLE_POLL (default 1 on Android): the idle step does not wait for the GPU. With waiting, a frame's
 * compute results were waited for here once the frame was replayed (5-12 ms a frame on a Galaxy S24 Ultra, SMO_VK_WAITWHO)
 * and the next frame's commands, handed over meanwhile, waited behind it. Now what has landed is taken (cb_poll, which
 * writes the queued reports) and 1 is returned while reports still wait: the render loop then looks again after 1 ms
 * or as soon as a job comes, so a game thread waiting for a report is still served. */
int nvn_vk_idle(void) {
    static int poll = -1;
    if (!V.alive) return 0;
    if (poll < 0) {
        const char *e = getenv("SMO_VK_IDLE_POLL");
#ifdef __ANDROID__
        poll = e ? atoi(e) != 0 : 1;
#else
        poll = e ? atoi(e) != 0 : 0;
#endif
    }
    if (!poll || g_cb_live) { if (g_rq_n) cb_sync(); else cb_poll(); return 0; }
    cb_poll();
    if (g_rq_n && !g_cb_pending) rq_flush();       /* nothing outstanding any more */
    return g_rq_n != 0;
}
static void occ_pool(void) {
    if (g_occ_pool) return;
    if (!D.f12.hostQueryReset) nvn_vk_missing("occlusion counters on a GPU without hostQueryReset");
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = OCC_MAX;
    VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_occ_pool));
    vkResetQueryPool(V.device, g_occ_pool, 0, OCC_MAX);
    g_occ_sync = getenv("SMO_VK_OCC_SYNC") && atoi(getenv("SMO_VK_OCC_SYNC"));
    /* Android port: off by default. Waiting at the present for the frame's counts stopped the render thread until the GPU
     * had finished the frame (7 ms a frame on a Galaxy S24 Ultra in Cap Kingdom, 23-24 fps against 28-32 without); the
     * counts land when the GPU has them (cb_poll / occ_resolve without a wait), and the game reads them a frame later. */
#ifdef __ANDROID__
    g_occ_present = getenv("SMO_VK_OCC_PRESENT") && atoi(getenv("SMO_VK_OCC_PRESENT"));
#else
    g_occ_present = !getenv("SMO_VK_OCC_PRESENT") || atoi(getenv("SMO_VK_OCC_PRESENT"));
#endif
}
/* SMO_VK_GS_DEBUG=1 or 3 (gs_dbg_mode), draws with a geometry stage. (a) The pipeline statistics of 16 of them (every 25th),
 * printed once the GPU has run them (gsq_poll at every present): primitives in, vertex and geometry invocations, primitives
 * the geometry stage put out, primitives into and out of clipping, fragment invocations. (b) gs_dbg_read: the colour target
 * of every 20th of the first 400, read just before and just after the draw: how many texels the draw brightened and by how
 * much (R11G11B10F targets only). */
static VkQueryPool g_gsq_pool;
static int g_gsq_n, g_gsq_read, g_gsq_open;
static void gsq_begin(void) {
    if (g_gsq_n >= 16 || g_gs_draws % 25 != 1 || (gs_dbg_mode() != 1 && gs_dbg_mode() != 3) || !D.f2.features.pipelineStatisticsQuery) return;
    if (!g_gsq_pool) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = 16; qi.pipelineStatistics = 0xFE;      /* everything but input vertices */
        VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_gsq_pool));
        vkResetQueryPool(V.device, g_gsq_pool, 0, 16);
    }
    vkCmdBeginQuery(R.cmd, g_gsq_pool, (uint32_t)g_gsq_n, 0); g_gsq_open = 1;
}
static void gsq_end(void) { if (g_gsq_open) { vkCmdEndQuery(R.cmd, g_gsq_pool, (uint32_t)g_gsq_n++); g_gsq_open = 0; } }
/* 0.1.5, a measurement, not a fix: does a depth test of EQUAL miss by a few rounding steps on this GPU? SMO draws models twice,
 * a depth pre-pass and then a colour pass that tests EQUAL against it, with two programs. If the two do not compute exactly the
 * same z, the colour pass loses whole triangles and what lies behind them shows (Mali: gaps in Mario and Cappy on the title screen).
 * On five frames in a row the same EQUAL draws run as (0) EQUAL, then LEQUAL with z pulled nearer by some depth bias units and with
 * z pushed as far away, for two sizes of window: (1, 2) 16 units and (3, 4) 256, each with an occlusion query. A fragment passes the
 * first of a pair when z <= stored + units and the second when z <= stored - units, so the difference counts exactly the fragments
 * within that window of the stored depth: cut-out holes (far nearer) pass both and hidden surfaces (far behind) pass neither, so
 * neither is counted. Window minus exact is the near misses: about 0 on a GPU where EQUAL holds. If they are about 0 and the picture
 * still has gaps, the triangles are missing for another reason than this test. Depth writes are off for the test frames, which can
 * look a little wrong, one frame each round. On by default on Arm GPUs and with SMO_VK_EQTEST=1; SMO_VK_EQTEST=0 turns it off.
 * A colour draw that tests LEQUAL and writes depth is measured too: the title screen draws Mario and Cappy that way after a depth-only
 * pass, with no EQUAL at all, and it fails the same way when the colour z lands a little behind the stored one. Frame 0, 2 and 4 run
 * it as it is (2 and 4 say how much the counts move by themselves), frame 1 with z pulled 16 units nearer and frame 3 with 256, depth
 * writes left on, so that later draws still meet what they normally meet. Near misses: what frame 3 passes more than frame 0.
 * SMO_VK_EQTEST_CTRL=1 (tests of the measurement itself): frame 0 runs with z pushed 4 units away, so that draws over a depth-only
 * pass must fail: near misses of about 100%. */
#define EQ_MAX 9000
#define EQ_KEYS 512
#define EQ_FRAMES 5
#define EQ_ROUNDS 24
static VkQueryPool g_eq_pool;
static int g_eq_on = -1, g_eq_ctrl, g_eq_round, g_eq_n, g_eq_open;
static int64_t g_eq_next = -1;
static uint64_t g_eq_serial, g_eq_hist[16];
static struct { short key, variant; } g_eq_q[EQ_MAX];
static struct { int vs, fs, w, h, le; uint32_t count, draws[EQ_FRAMES]; uint64_t n[EQ_FRAMES]; } g_eq_k[EQ_KEYS];
static int g_eq_nk;
static int eq_begin(uint32_t count) {              /* after draw_prepare, right before the draw; 1: the query is open and the caller skips occ_draw */
    if (g_eq_on < 0) {
        const char *e = getenv("SMO_VK_EQTEST");
        g_eq_on = e ? atoi(e) != 0 : (V.props.vendorID == 0x13B5 && !gfx_options()->diag_off);
        if (g_eq_on && (!D.f12.hostQueryReset || !D.f2.features.occlusionQueryPrecise)) {
            printf("nvn_vk_eq: cannot measure on this GPU (host query reset %d, precise occlusion queries %d)\n", (int)D.f12.hostQueryReset, (int)D.f2.features.occlusionQueryPrecise);
            g_eq_on = 0;
        }
        g_eq_ctrl = g_eq_on && getenv("SMO_VK_EQTEST_CTRL") != NULL;
        if (g_eq_on) printf("nvn_vk_eq: measuring EQUAL and LEQUAL colour draws for near misses of up to 16 and 256 depth bias units%s\n", g_eq_ctrl ? " (control run: EQUAL is broken on purpose)" : "");
    }
    if (g_eq_on) g_eq_hist[R.ds & 1 ? (R.ds >> 4) & 15 : 0]++;
    int le = ((R.ds >> 4) & 15) == 4 && R.chanmask && (R.ds & 2);
    if (!g_eq_on || g_eq_round >= EQ_ROUNDS || !R.in_pass || !(R.ds & 1) || (((R.ds >> 4) & 15) != 3 && !le) || g_eq_n >= EQ_MAX) return 0;
    int64_t f = (int64_t)nvn_front_frame_next();
    if (g_eq_next < 0) g_eq_next = f + 60;
    int v = (int)(f - g_eq_next);
    if (v < 0 || v >= EQ_FRAMES) return 0;
    int vs = R.prog[0] ? R.prog[0]->serial : -1, fs = R.prog[1] ? R.prog[1]->serial : -1, k;
    for (k = 0; k < g_eq_nk; k++) if (g_eq_k[k].vs == vs && g_eq_k[k].fs == fs && g_eq_k[k].le == le && g_eq_k[k].count == count && g_eq_k[k].w == R.pass_w && g_eq_k[k].h == R.pass_h) break;
    if (k == g_eq_nk) {
        if (k == EQ_KEYS) return 0;
        memset(&g_eq_k[k], 0, sizeof g_eq_k[k]);
        g_eq_k[k].vs = vs; g_eq_k[k].fs = fs; g_eq_k[k].le = le; g_eq_k[k].count = count; g_eq_k[k].w = R.pass_w; g_eq_k[k].h = R.pass_h;
        g_eq_nk++;
    }
    if (!g_eq_pool) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = EQ_MAX;
        VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_eq_pool));
        vkResetQueryPool(V.device, g_eq_pool, 0, EQ_MAX);
    }
    if (le && (v & 1)) {
        int on = (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1;
        vkCmdSetDepthBiasEnable(R.cmd, VK_TRUE);
        vkCmdSetDepthBias(R.cmd, (on ? R.polyoff[1] : 0.0f) - (v == 1 ? 16.0f : 256.0f), 0.0f, on ? R.polyoff[0] : 0.0f);
        R.dyn_dirty = 1;
    } else if (v && !le) {
        int on = (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1;
        vkCmdSetDepthCompareOp(R.cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
        vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE);
        vkCmdSetDepthBiasEnable(R.cmd, VK_TRUE);
        vkCmdSetDepthBias(R.cmd, (on ? R.polyoff[1] : 0.0f) + (v <= 2 ? 16.0f : 256.0f) * (v & 1 ? -1.0f : 1.0f), 0.0f, on ? R.polyoff[0] : 0.0f);
        R.dyn_dirty = 1;                           /* the next draw sets the game's state again */
    } else if (!v && g_eq_ctrl) {
        vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE);
        vkCmdSetDepthBiasEnable(R.cmd, VK_TRUE);
        vkCmdSetDepthBias(R.cmd, 4.0f, 0.0f, 0.0f);
        R.dyn_dirty = 1;
    }
    occ_end();                                     /* queries of a type do not nest; the game's count, if one runs, leaves this draw out */
    g_eq_q[g_eq_n].key = (short)k; g_eq_q[g_eq_n].variant = (short)v;
    g_eq_k[k].draws[v]++;
    vkCmdBeginQuery(R.cmd, g_eq_pool, (uint32_t)g_eq_n, VK_QUERY_CONTROL_PRECISE_BIT);
    g_eq_open = 1;
    return 1;
}
static void eq_end(void) {
    if (!g_eq_open) return;
    vkCmdEndQuery(R.cmd, g_eq_pool, (uint32_t)g_eq_n++);
    g_eq_serial = R.serial + 1;                    /* the number the command buffer being recorded will get */
    g_eq_open = 0;
}
static int64_t eq_window(int k, int pair) { return (int64_t)g_eq_k[k].n[1 + pair * 2] - (int64_t)g_eq_k[k].n[2 + pair * 2]; }
static int64_t eq_miss(int k) { return g_eq_k[k].le ? (int64_t)g_eq_k[k].n[3] - (int64_t)g_eq_k[k].n[0] : eq_window(k, 1) - (int64_t)g_eq_k[k].n[0]; }
static int eq_cmp(const void *a, const void *b) {
    int64_t x = eq_miss(*(const int *)a), y = eq_miss(*(const int *)b);
    return y > x ? 1 : y < x ? -1 : 0;
}
static int occ_done(uint64_t serial, int wait);
static void eq_poll(void) {                        /* at every present */
    if (g_eq_on > 0) {                             /* what the screen of the moment uses, EQUAL or not: every 600 frames, the first 60 times */
        static int lines;
        int64_t f = (int64_t)nvn_front_frame_next();
        if (f % 600 == 0 && lines < 60) {
            lines++;
            printf("nvn_vk_eq: frames up to %lld, draws by depth test: none %llu, less %llu, equal %llu, lequal %llu, greater %llu, gequal %llu, other %llu\n", (long long)f,
                   (unsigned long long)g_eq_hist[0], (unsigned long long)g_eq_hist[2], (unsigned long long)g_eq_hist[3], (unsigned long long)g_eq_hist[4], (unsigned long long)g_eq_hist[5], (unsigned long long)g_eq_hist[7],
                   (unsigned long long)(g_eq_hist[1] + g_eq_hist[6] + g_eq_hist[8]));
            memset(g_eq_hist, 0, sizeof g_eq_hist);
        }
    }
    if (g_eq_next < 0 || g_eq_round >= EQ_ROUNDS || (int64_t)nvn_front_frame_next() < g_eq_next + EQ_FRAMES) return;
    int64_t at = g_eq_next;
    g_eq_next += g_eq_round < 8 ? 300 : 1800;      /* the title screen first, then now and then for as long as the rounds last */
    if (!g_eq_n) return;                           /* no EQUAL draw on those frames (a loading screen): that round is not counted */
    static uint64_t res[EQ_MAX];
    (void)occ_done(g_eq_serial, 1);
    VKC(vkGetQueryPoolResults(V.device, g_eq_pool, 0, (uint32_t)g_eq_n, (size_t)g_eq_n * 8, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    vkResetQueryPool(V.device, g_eq_pool, 0, (uint32_t)g_eq_n);
    for (int i = 0; i < g_eq_n; i++) g_eq_k[g_eq_q[i].key].n[g_eq_q[i].variant] += res[i];
    int64_t exact = 0, w16 = 0, w256 = 0, used = 0, skipped = 0, le0 = 0, le16 = 0, le256 = 0, lemove = 0, leused = 0;
    int idx[EQ_KEYS], nidx = 0;
    for (int k = 0; k < g_eq_nk; k++) {
        int same = 1;
        for (int v = 1; v < EQ_FRAMES; v++) if (g_eq_k[k].draws[v] != g_eq_k[k].draws[0]) same = 0;
        if (!same) { skipped++; continue; }        /* not the same draws on all the frames: nothing to compare */
        if (g_eq_k[k].le) {
            int64_t n0 = (int64_t)g_eq_k[k].n[0], m2 = (int64_t)g_eq_k[k].n[2] - n0, m4 = (int64_t)g_eq_k[k].n[4] - n0;
            le0 += n0; le16 += (int64_t)g_eq_k[k].n[1]; le256 += (int64_t)g_eq_k[k].n[3]; leused += g_eq_k[k].draws[0];
            lemove += (m2 < 0 ? -m2 : m2) > (m4 < 0 ? -m4 : m4) ? (m2 < 0 ? -m2 : m2) : (m4 < 0 ? -m4 : m4);
        } else {
            exact += (int64_t)g_eq_k[k].n[0]; w16 += eq_window(k, 0); w256 += eq_window(k, 1); used += g_eq_k[k].draws[0];
        }
        idx[nidx++] = k;
    }
    qsort(idx, (size_t)nidx, sizeof idx[0], eq_cmp);
    printf("nvn_vk_eq: round %d, frames %lld..%lld: %lld EQUAL draws compared (%lld kinds left out): %lld samples pass EQUAL, within 16 units %lld, within 256 units %lld, near misses %lld (%.2f%%)\n",
           g_eq_round + 1, (long long)at, (long long)at + EQ_FRAMES - 1, (long long)used, (long long)skipped, (long long)exact, (long long)w16, (long long)w256, (long long)(w256 - exact), w256 > 0 ? 100.0 * (double)(w256 - exact) / (double)w256 : 0.0);
    printf("nvn_vk_eq: round %d, %lld LEQUAL colour draws: %lld samples pass as they are, %lld with z 16 units nearer, %lld with 256, near misses %lld (%.2f%%); the counts move by %lld by themselves\n",
           g_eq_round + 1, (long long)leused, (long long)le0, (long long)le16, (long long)le256, (long long)(le256 - le0), le256 > 0 ? 100.0 * (double)(le256 - le0) / (double)le256 : 0.0, (long long)lemove);
    for (int i = 0; i < nidx && i < 6; i++) {
        int k = idx[i];
        int64_t m = eq_miss(k), w = g_eq_k[k].le ? (int64_t)g_eq_k[k].n[3] : eq_window(k, 1);
        if (m <= 0) break;
        printf("nvn_vk_eq:   %s vs #%d fs #%d %dx%d, %u indices, %u draws: as they are %llu, with the window %lld, near misses %lld (%.1f%%)\n", g_eq_k[k].le ? "LEQUAL" : "EQUAL", g_eq_k[k].vs, g_eq_k[k].fs, g_eq_k[k].w, g_eq_k[k].h, g_eq_k[k].count, g_eq_k[k].draws[0], (unsigned long long)g_eq_k[k].n[0], (long long)w, (long long)m, w > 0 ? 100.0 * (double)m / (double)w : 0.0);
    }
    fflush(stdout);
    g_eq_n = 0; g_eq_nk = 0; g_eq_round++;
}
static void gsq_poll(void) {
    while (g_gsq_pool && g_gsq_read < g_gsq_n) {
        uint64_t r[8];
        if (vkGetQueryPoolResults(V.device, g_gsq_pool, (uint32_t)g_gsq_read, 1, sizeof r, r, sizeof r, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !r[7]) return;
        printf("nvn_vk_gs: statistics of draw %d: primitives in %llu, vertex invocations %llu, geometry invocations %llu, geometry primitives out %llu, clipping in %llu, clipping out %llu, fragment invocations %llu\n",
               1 + 25 * g_gsq_read, (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6]);
        g_gsq_read++;
    }
}
static uint32_t *g_gsr_before;
static Tex *g_gsr_t;
static float r11g11b10_sum(uint32_t v) {          /* the three channels of an R11G11B10F texel, added */
    uint32_t c[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22}, mb[3] = {6, 6, 5};
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) { uint32_t e = c[i] >> mb[i], m = c[i] & ((1u << mb[i]) - 1); sum += e ? ldexpf(1.0f + (float)m / (float)(1u << mb[i]), (int)e - 15) : ldexpf((float)m, -14 - (int)mb[i]); }
    return sum;
}
static void gs_dbg_read(int after) {
    Tex *t = R.col[0].tex;
    if (!t || t->b.format != 0x3f) return;
    int w = t->b.width, h = t->b.height, rr[6] = {0, 0, 0, w, h, 1};
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px || !read_region(t, NULL, rr, px, 0, 0)) { free(px); return; }
    if (!after) { free(g_gsr_before); g_gsr_before = px; g_gsr_t = t; return; }
    if (g_gsr_before && g_gsr_t == t) {
        int n = 0, dbl = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
        double sum = 0.0, base = 0.0, mx = 0.0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t k = (size_t)y * w + x;
                if (px[k] == g_gsr_before[k]) continue;
                float b0 = r11g11b10_sum(g_gsr_before[k]), d = r11g11b10_sum(px[k]) - b0;
                if (d <= 0.001f) continue;
                n++; sum += d; base += b0; if (d > mx) mx = d; if (d > b0) dbl++;
                if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y;
            }
        printf("nvn_vk_gs: draw %lld (frame %lld): target %dx%d: %d texels brightened, by %.3f on average (max %.3f) over %.3f before; %d more than doubled; box %d,%d .. %d,%d\n",
               (long long)g_gs_draws, (long long)nvn_front_frame_next(), w, h, n, n ? sum / n : 0.0, mx, n ? base / n : 0.0, dbl, x0, y0, x1, y1);
    }
    free(px); free(g_gsr_before); g_gsr_before = NULL;
}
static void occ_draw(void) {                       /* after draw_prepare, inside a pass */
    if (!g_occ_on || g_occ_open || !R.in_pass) return;
    if (g_occ_head - g_occ_tail >= OCC_MAX) {      /* full within one batch (occ_fold makes room between batches) */
        nvn_vk_missing("more than 65536 occlusion count segments waiting for the GPU within one batch");
        return;
    }
    vkCmdBeginQuery(R.cmd, g_occ_pool, (uint32_t)(g_occ_head % OCC_MAX), D.f2.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);   /* NVN counts samples */
    g_occ_head++;
    g_occ_seg_serial = R.serial + 1;               /* the number the command buffer being recorded will get */
    g_occ_open = 1;
}
/* Has submission `serial` finished? With wait it is waited for (submitted first while it is still being recorded). */
static int occ_done(uint64_t serial, int wait) {
    if (serial <= R.done) return 1;
    int64_t t0 = wait ? tk() : 0;
    if (wait && serial > R.serial) cmd_submit(NULL, NULL);
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (s->pending && s->serial <= serial && (wait || slot_finished(s))) slot_wait(s);
    }
    if (wait) { if (R.done < serial) R.done = serial; g_occ_waits++; g_occ_wait_ticks += tk() - t0; }
    return serial <= R.done;
}
/* Segments [lo, hi) have run: their samples are added to *sum (none when sum is NULL) and their queries are free again. */
static void occ_take(uint64_t lo, uint64_t hi, uint64_t *sum) {
    static uint64_t res[OCC_MAX];
    while (lo < hi) {
        uint32_t first = (uint32_t)(lo % OCC_MAX);
        uint32_t n = hi - lo < (uint64_t)(OCC_MAX - first) ? (uint32_t)(hi - lo) : OCC_MAX - first;
        if (sum) {
            VKC(vkGetQueryPoolResults(V.device, g_occ_pool, first, n, (size_t)n * 8, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            for (uint32_t i = 0; i < n; i++) *sum += res[i];
        }
        vkResetQueryPool(V.device, g_occ_pool, first, n);
        lo += n;
    }
}
/* Android port: conditional renders decided one frame late (OP_RENDER_ENABLE). An entry per compared address: the
 * decision of the last frame whose two counts were both in guest memory; pending: a conditional render asked for it
 * while one count was in and the other not, and it is decided when the other lands (cond_landed, from occ_resolve). */
#define COND_N 4096
static struct { uint64_t addr; int mode, off, valid, pending; } g_cond[COND_N];
static int cond_slot(uint64_t addr, int make) {
    unsigned h = (unsigned)((addr >> 4) * 2654435761u) >> 20;
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + (unsigned)i) & (COND_N - 1);
        if (g_cond[k].addr == addr) return (int)k;
        if (!g_cond[k].addr) { if (!make) return -1; g_cond[k].addr = addr; return (int)k; }
    }
    return -1;
}
/* Android port, A FRAME'S COUNTS TOGETHER (SMO_VK_OCC_HOLD, default 1 on Android): without the wait at the present
 * (g_occ_present) the game read a frame's counts while some had landed and some not, and light glows decided by them
 * flickered (seen on a Galaxy S24 Ultra in Cap Kingdom). Counts that land are held (g_occ_st) and written to guest memory
 * when every entry queued before a present has landed: the game sees all of a frame's counts or none of them (then
 * those of the frame before), and nothing waits for the GPU. */
static int g_occ_hold = -1;
static uint64_t g_occ_pushed, g_occ_landed;        /* entries queued, entries taken (all kinds), ever */
static struct { uint64_t seq, dst, d[2]; } g_occ_st[OCC_Q];
static int g_occ_sth, g_occ_stn;                   /* counts landed and held */
static uint64_t g_occ_bnd[64];
static int g_occ_bh, g_occ_bn;                     /* frame ends: the entries before g_occ_bnd[] belong to frames presented */
static int occ_queued_at(uint64_t addr) {          /* a report not yet written targets [addr, addr + 16) */
    for (int i = 0; i < g_occ_qn; i++) { const OccEntry *e = &g_occ_q[(g_occ_qh + i) % OCC_Q]; if (e->kind == 0 && e->dst < addr + 16 && e->dst + 16 > addr) return 1; }
    for (int i = 0; i < g_occ_stn; i++) { uint64_t d = g_occ_st[(g_occ_sth + i) % OCC_Q].dst; if (d < addr + 16 && d + 16 > addr) return 1; }
    return 0;
}
static void cond_decide(int k) {
    int eq = rd64(g_cond[k].addr) == rd64(g_cond[k].addr + 16);
    g_cond[k].off = g_cond[k].mode == 1 ? !eq : eq; g_cond[k].valid = 1; g_cond[k].pending = 0;
}
static void cond_landed(uint64_t dst) {            /* a count was written to dst: a pending decision on it is settled when its other count is in too */
    for (int j = 0; j < 2; j++) {
        uint64_t addr = dst - 16 * (uint64_t)j;
        int k = cond_slot(addr, 0);
        if (k >= 0 && g_cond[k].pending && !occ_queued_at(addr) && !occ_queued_at(addr + 16)) cond_decide(k);
    }
}
static void occ_write(uint64_t dst, const uint64_t d[2]) {
    memcpy(P(dst), d, 16);
    host_wrote(dst, 16, GEN_OCC);                  /* ranges the ring copied from there are copied again */
    cond_landed(dst);
}
static void occ_publish(uint64_t upto) {           /* the held counts of entries before upto, in order */
    while (g_occ_stn && g_occ_st[g_occ_sth].seq < upto) {
        uint64_t dst = g_occ_st[g_occ_sth].dst, d[2] = {g_occ_st[g_occ_sth].d[0], g_occ_st[g_occ_sth].d[1]};
        g_occ_sth = (g_occ_sth + 1) % OCC_Q; g_occ_stn--;
        occ_write(dst, d);
    }
}
static void occ_publish_ready(void) {              /* every frame whose entries have all landed */
    while (g_occ_bn && g_occ_landed >= g_occ_bnd[g_occ_bh]) { occ_publish(g_occ_bnd[g_occ_bh]); g_occ_bh = (g_occ_bh + 1) % 64; g_occ_bn--; }
}
static void occ_frame_end(void) {                  /* at a present: the entries queued so far belong to presented frames */
    if (g_occ_hold < 0) {
        const char *e = getenv("SMO_VK_OCC_HOLD");
#ifdef __ANDROID__
        g_occ_hold = e ? atoi(e) != 0 : 1;
#else
        g_occ_hold = e ? atoi(e) != 0 : 0;
#endif
    }
    if (!g_occ_hold) return;
    if (g_occ_bn && g_occ_bnd[(g_occ_bh + g_occ_bn - 1) % 64] == g_occ_pushed) return;      /* nothing queued since the last */
    if (g_occ_bn == 64) g_occ_bnd[(g_occ_bh + 63) % 64] = g_occ_pushed;      /* full: the newest frame end moves on (coarser, never partial) */
    else { g_occ_bnd[(g_occ_bh + g_occ_bn) % 64] = g_occ_pushed; g_occ_bn++; }
    occ_publish_ready();
}
/* The queued entries in their order: the first `need` of them are waited for where the GPU has not finished them yet,
 * the ones after those as long as their submission has finished. */
static void occ_resolve(int need) {
    while (g_occ_qn) {
        OccEntry *e = &g_occ_q[g_occ_qh];
        if (!occ_done(e->serial, need > 0)) break;
        uint64_t sum = 0;
        occ_take(e->lo, e->hi, e->kind == 1 ? NULL : &sum);
        g_occ_tail = e->hi;
        if (e->kind == 1) g_occ_total = 0;
        else g_occ_total += sum;
        uint64_t d[2] = {g_occ_total, counter_ticks(vk_now_ns())};
        if (e->kind == 0 && e->dst) {
            if (nvn_front_frame_next() > e->frame) g_occ_late++;
            if (e->log) printf("nvn_vk_occ: frame %lld report %llu samples to %llx (segments %llu..%llu), written in frame %lld\n", (long long)e->frame, (unsigned long long)g_occ_total,
                               (unsigned long long)e->dst, (unsigned long long)e->lo, (unsigned long long)e->hi, nvn_front_frame_next());      /* SMO debug aid (SMO_VK_PASSSHOT) */
        }
        uint64_t landed = e->kind == 0 ? e->dst : 0, seq = e->seq;
        g_occ_qh = (g_occ_qh + 1) % OCC_Q; g_occ_qn--; need--;
        g_occ_landed = seq + 1;
        if (landed) {                              /* after the entry has left the queue (occ_queued_at) */
            if (g_occ_hold > 0) {
                if (g_occ_stn == OCC_Q) occ_publish(UINT64_MAX);
                int k = (g_occ_sth + g_occ_stn) % OCC_Q;
                g_occ_st[k].seq = seq; g_occ_st[k].dst = landed; g_occ_st[k].d[0] = d[0]; g_occ_st[k].d[1] = d[1]; g_occ_stn++;
            } else occ_write(landed, d);
        }
    }
    if (g_occ_hold > 0) occ_publish_ready();
}
static void occ_push(int kind, uint64_t dst) {     /* queues an entry for the segments since the last one */
    occ_end();
    if (g_occ_qn == OCC_Q) occ_resolve(OCC_Q / 2);
    OccEntry *e = &g_occ_q[(g_occ_qh + g_occ_qn) % OCC_Q];
    e->kind = kind; e->dst = dst; e->lo = g_occ_mark; e->hi = g_occ_head;
    e->serial = e->hi > e->lo ? g_occ_seg_serial : 0;
    e->frame = nvn_front_frame_next(); e->log = 0; e->seq = g_occ_pushed++;
    g_occ_mark = g_occ_head;
    g_occ_qn++;
}
/* A count that stays open over many passes (SMO's Top-Hat Tower stage: about 136 segments a frame): at the end of a
 * batch, when half of the ring is in use, its segments so far are added up into the count (a fold entry) and the GPU is
 * waited for. Segments of a count that was reset without a report are dropped by the reset's entry. */
static void occ_fold(void) {
    if (!g_occ_pool || g_occ_head - g_occ_tail < OCC_MAX / 2) return;
    occ_resolve(0);
    if (g_occ_head - g_occ_tail < OCC_MAX / 2) return;
    if (g_occ_head > g_occ_mark) { occ_push(2, 0); g_occ_folds++; }
    occ_resolve(g_occ_qn);
}
static void occ_reset(void) {
    occ_pool();
    occ_end();
    if (!g_occ_qn && g_occ_head == g_occ_mark) g_occ_total = 0;      /* nothing queued: the count starts again at once */
    else occ_push(1, 0);
    g_occ_on = 1;
}
static void occ_report(uint64_t dst) {
    occ_pool();
    occ_push(0, dst);
    g_occ_reports++;
    if (passshot_frame()) g_occ_q[(g_occ_qh + g_occ_qn - 1) % OCC_Q].log = 1;
    if (g_occ_sync) occ_resolve(g_occ_qn);
}
static void cb_batch_end(void) {
    if (!g_cb_live) return;
    g_cb_serial = cmd_submit(NULL, NULL);
    for (int i = 0; i < g_cb_used; i++) if (g_cb[i].state == 1) g_cb[i].state = 2;
    g_cb_pending = g_cb_live; g_cb_live = 0;
}
/* The CBs of this batch (state 1), in the order they were made: the first g_cb_live entries (every change of a CB out of
 * state 1 sets g_cb_live to 0). Every range a draw reads is looked up here while compute results are live, about 10,000
 * a frame: walking the 256 slots for the dozen CBs of a frame cost 0.5-0.7 ms a frame once they stayed live longer. */
static int16_t g_cb_live_list[CB_MAX];
static CB *cb_inside(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_cb_live; k++) { CB *c = &g_cb[g_cb_live_list[k]]; if (c->state == 1 && addr >= c->addr && addr + size <= c->addr + c->size) return c; }
    return NULL;
}
static int cb_overlap(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_cb_live; k++) { const CB *c = &g_cb[g_cb_live_list[k]]; if (c->state == 1 && addr < c->addr + c->size && c->addr < addr + size) return 1; }
    return 0;
}
static CB *cb_new(uint64_t addr, uint64_t size) {
    CB *c = NULL, *empty = NULL, *victim = NULL;
    for (int i = 0; i < g_cb_used && !c; i++) {
        CB *k = &g_cb[i];
        if (k->state || k->busy > R.done) continue;
        if (!k->buf) { if (!empty) empty = k; }
        else if (k->cap >= size && k->cap <= size * 2 + 4096) c = k;
        else if (!victim) victim = k;
    }
    if (!c && !empty && g_cb_used < CB_MAX) empty = &g_cb[g_cb_used++];      /* slots are taken into use as needed: the walks above stay short */
    if (!c) {
        c = empty ? empty : victim;
        if (!c) vkd_fail("more than %d storage buffers written by compute in one batch", CB_MAX);
        if (c->buf) { vkDestroyBuffer(V.device, c->buf, NULL); vmem_free(c->mem); c->buf = NULL; }      /* free and not busy: nothing in flight names it */
        if (g_cb_cached < 0) {                     /* read back by the CPU: cached host memory when the device has it */
            g_cb_cached = 0;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((D.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) g_cb_cached = 1;
        }
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements rq;
        c->cap = (size + 4095) & ~(uint64_t)4095;
        bi.size = c->cap; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device, &bi, NULL, &c->buf));
        vkGetBufferMemoryRequirements(V.device, c->buf, &rq);
        VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (g_cb_cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        int found = 0;
        for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) found = 1;
        if (!found) want &= ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        c->mem = vmem_alloc(rq, want, 2);
        VKC(vkBindBufferMemory(V.device, c->buf, c->mem.mem, c->mem.off));
        R.cb_made++;
    }
    c->addr = addr; c->size = size; c->state = 1; c->dirty = 0;
    if (g_cb_merge < 0) g_cb_merge = !(getenv("SMO_VK_CBMERGE") && !atoi(getenv("SMO_VK_CBMERGE")));
    if (g_cb_merge) {                              /* the snapshot the copy-back is compared with (cb_land) */
        if (c->snap_cap < size) { free(c->snap); c->snap_cap = (size + 4095) & ~(uint64_t)4095; c->snap = malloc((size_t)c->snap_cap); }
        memcpy(c->snap, P(addr), (size_t)size);
        memcpy(c->mem.map, c->snap, (size_t)size);
    } else
    memcpy(c->mem.map, P(addr), (size_t)size);
    g_cb_live_list[g_cb_live++] = (int16_t)(c - g_cb);
    return c;
}
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
/* Where a command reads the guest range from: a CB of this batch that contains it, else the stream ring. align: the offset
 * alignment the use needs (storage descriptors), 1 = none. Returns 1 when a CB is bound. */
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b) {
    b->zero = 0; b->size = size;
    if (g_cb_live) {
        CB *c = cb_inside(addr, size);
        if (c && (addr - c->addr) % align == 0) { b->buf = c->buf; b->off = addr - c->addr; return 1; }
        if (cb_overlap(addr, size)) cb_sync();
    }
    b->off = ring_put(addr, size); b->buf = R.ring;      /* in this order: a ring wrap submits */
    return 0;
}
static void compute_pipe_make(VStage *v);
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz);
static void compute_dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    Prog *p = R.prog[5];
    static int nlog;
    R.dispatches++;
    if (!p) { if (nlog++ < 10) printf("nvn_vk: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    if (!st->code) { nvn_vk_missing("nvnCommandBufferDispatchCompute of a program that has no machine code (assembly text only)"); return; }
    uint32_t key[4] = {0, 0, R.tex_int[5], R.tex_sint[5]};
    VStage *v = vstage_get(p, 5, key, 0, 0);
    if (!v) { nvn_vk_missing("nvnCommandBufferDispatchCompute: the compute stage could not be translated or compiled (the log names the program)"); return; }
    if (v->nsmp > 32) { nvn_vk_missing("a compute program with more than 32 samplers"); return; }
    if (!gx || !gy || !gz) return;
    int cp_how = 0;
    if (v->cp_state != 2) { int64_t t0 = tk(); cp_how = compute_pipe_ensure(v); if (cp_how) { hold_note(cp_how == 1 ? H_PIPE_DRAW : H_PIPE_WAIT, tk() - t0); g_tm[5] += tk() - t0; } }
    if (!v->cp_recorded) { v->cp_recorded = 1; if (!v->images) piperec_append(NULL, v, NULL, 1); }      /* SMO: a stage with images has no record to be made from at start */
    if (cp_how == 1) {
        g_pipe_miss++;
        printf("nvn_vk: compute program #%d: pipeline made at its first dispatch (%d bytes of machine code, work group %u x %u x %u, constant buffers 0x%x, storage 0x%x written 0x%x, %d samplers)\n", p->serial, st->code_len,
               st->local[0], st->local[1], st->local[2], v->cbufs, v->ssbos, v->ssbo_w, v->nsmp);
    }
    {   static VStage *seen[256]; static int nseen;          /* 0.1.2: which compute stages a run dispatches (once each), by a hash of their machine code */
        int k; for (k = 0; k < nseen && seen[k] != v; k++) {}
        if (k == nseen && nseen < 256) { seen[nseen++] = v;
            printf("nvn_vk_cs: frame %lld: compute program #%d first dispatched: %d bytes of machine code (fnv %016llx), work group %u x %u x %u, shared %u bytes, %u x %u x %u groups\n",
                   (long long)nvn_front_frame_next(), p->serial, st->code_len, (unsigned long long)fnv(0xcbf29ce484222325ULL, st->code, (size_t)st->code_len), st->local[0], st->local[1], st->local[2], st->smem_bytes, gx, gy, gz);
            if (v->warp) printf("nvn_vk_cs: compute program #%d %s (subgroup of %u lanes): %s\n", p->serial, v->warp & 2 ? "exchanges values between the lanes of a warp (shuffle / ballot)" : "reads its lane number",
                                D.subgroup, !(v->warp & 2) ? "exact (the lane is the Switch GPU's)" : D.subgroup >= 32 ? "exact" : "NOT exact on this GPU (lanes of the other half of each warp are out of reach)"); }
    }
    compute_dispatch_run(p, st, v, gx, gy, gz);
}
/* SMO: images of compute programs (nvnCommandBufferBindImage; 14 compute stages of the game: 13 of agl's post effects,
 * one of alComputeShader). The NVN image handle is the texture's index in the texture pool; the texture must have been made with
 * NVN_TEXTURE_FLAGS_IMAGE (its image then has VK_IMAGE_USAGE_STORAGE_BIT, vt_get2). A storage view has one level and the
 * identity swizzle; its format is the one the shader declares (typed accesses, atomics) or the texture view's own. */
static int64_t g_img_disp, g_img_views;
static VkFormat gl_image_vkfmt(uint16_t gl) {      /* the image formats of mx_translate.c img_qualifier */
    static const struct { uint16_t gl; VkFormat vk; } m[] = {
        {0x8814, VK_FORMAT_R32G32B32A32_SFLOAT}, {0x881A, VK_FORMAT_R16G16B16A16_SFLOAT}, {0x8230, VK_FORMAT_R32G32_SFLOAT}, {0x822F, VK_FORMAT_R16G16_SFLOAT},
        {0x8C3A, VK_FORMAT_B10G11R11_UFLOAT_PACK32}, {0x822E, VK_FORMAT_R32_SFLOAT}, {0x822D, VK_FORMAT_R16_SFLOAT}, {0x805B, VK_FORMAT_R16G16B16A16_UNORM},
        {0x8059, VK_FORMAT_A2B10G10R10_UNORM_PACK32}, {0x8058, VK_FORMAT_R8G8B8A8_UNORM}, {0x822C, VK_FORMAT_R16G16_UNORM}, {0x822B, VK_FORMAT_R8G8_UNORM},
        {0x822A, VK_FORMAT_R16_UNORM}, {0x8229, VK_FORMAT_R8_UNORM}, {0x8F9B, VK_FORMAT_R16G16B16A16_SNORM}, {0x8F97, VK_FORMAT_R8G8B8A8_SNORM},
        {0x8F99, VK_FORMAT_R16G16_SNORM}, {0x8F95, VK_FORMAT_R8G8_SNORM}, {0x8F98, VK_FORMAT_R16_SNORM}, {0x8F94, VK_FORMAT_R8_SNORM},
        {0x8D70, VK_FORMAT_R32G32B32A32_UINT}, {0x8D76, VK_FORMAT_R16G16B16A16_UINT}, {0x906F, VK_FORMAT_A2B10G10R10_UINT_PACK32}, {0x8D7C, VK_FORMAT_R8G8B8A8_UINT},
        {0x823C, VK_FORMAT_R32G32_UINT}, {0x823A, VK_FORMAT_R16G16_UINT}, {0x8238, VK_FORMAT_R8G8_UINT}, {0x8236, VK_FORMAT_R32_UINT}, {0x8234, VK_FORMAT_R16_UINT},
        {0x8232, VK_FORMAT_R8_UINT}, {0x8D82, VK_FORMAT_R32G32B32A32_SINT}, {0x8D88, VK_FORMAT_R16G16B16A16_SINT}, {0x8D8E, VK_FORMAT_R8G8B8A8_SINT},
        {0x823B, VK_FORMAT_R32G32_SINT}, {0x8239, VK_FORMAT_R16G16_SINT}, {0x8237, VK_FORMAT_R8G8_SINT}, {0x8235, VK_FORMAT_R32_SINT}, {0x8233, VK_FORMAT_R16_SINT},
        {0x8231, VK_FORMAT_R8_SINT}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].gl == gl) return m[i].vk;
    return VK_FORMAT_UNDEFINED;
}
static VkImageView vt_storage_view(VTex *vt, const EView *e, int ty, VkFormat fmt) {      /* cached with the other views (attach 2) */
    static const VkImageViewType vtype[6] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D};
    EView key = *e;
    memset(key.swz, 0, sizeof key.swz); key.nlvl = 1; key.format = (int)fmt; key.dsmode = 0;
    if (ty != 2 && ty != 4) key.nlay = 1;
    for (VView *n = vt->views; n; n = n->next) if (n->attach == 2 && n->vtype == ty && !memcmp(&n->e, &key, sizeof key)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    n->e = key; n->vtype = ty; n->attach = 2;
    ci.image = vt->img; ci.format = fmt; ci.viewType = vtype[ty];
    ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = vt->is3d ? 0 : (uint32_t)e->lay0; ci.subresourceRange.layerCount = vt->is3d ? 1 : (uint32_t)key.nlay;
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (storage, type %d, format %d of image format %d, level %d, layers %d+%d): %s", (int)ci.viewType, (int)fmt, (int)vt->fmt, e->lvl0, e->lay0, key.nlay, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++; g_img_views++;
    return n->view;
}
/* The images a compute stage uses: a storage view of each, in the GENERAL layout. 0 (the dispatch is left out, said once per
 * reason) when one is not bound or cannot be bound. */
static int compute_images(Prog *p, VStage *v, VkImageView *iview, Tex **itex) {
    for (unsigned k = 0; k < 8; k++) {
        Tex *t = NULL;
        View view;
        SamplerParams sp;
        iview[k] = NULL; itex[k] = NULL;
        if (!(v->images >> k & 1)) continue;
        if (!(nvn_front_handle(R.tpool, NULL, R.nvn_img[5][k] & 0xfffff, &t, &view, &sp) & 1)) {
            static int n; if (n++ < 10) printf("nvn_vk: compute program #%d uses image %u but none is bound (handle %llx): dispatch left out\n", p->serial, k, (unsigned long long)R.nvn_img[5][k]);
            return 0;
        }
        VTex *vt = vt_get(t);
        int ty = v->img_type[k];
        EView e;
        eview_of(t, vt, &view, &e);
        VkFormat f = v->img_fmt[k] ? gl_image_vkfmt(v->img_fmt[k]) : VK_FORMAT_UNDEFINED;
        if (!f) { int ex, dec; f = tex_vkfmt(e.format, &ex, &dec); if (!f || ex || dec) f = vt->fmt; }
        if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_R8G8B8A8_UNORM; else if (f == VK_FORMAT_B8G8R8A8_SRGB) f = VK_FORMAT_B8G8R8A8_UNORM;      /* no sRGB storage */
        const char *why = !vt->storage ? "the texture was not made with NVN_TEXTURE_FLAGS_IMAGE" : ty == 1 ? "an image buffer (texel buffer) is not supported" :
                          !fmt_supported(f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "its format has no storage support on this GPU" :
                          (ty == 5 ? 2 : ty >= 3 ? 1 : 0) != (vt->is3d ? 2 : (t->b.target == 0 || t->b.target == 3) ? 0 : 1) ? "the shader's image type (1D, 2D or 3D) does not fit the texture" : NULL;
        if (why) {
            static int n; if (n++ < 10) printf("nvn_vk: compute program #%d, image %u (texture format 0x%x, %dx%dx%d, flags 0x%x, view format %d, type %d): %s: dispatch left out\n", p->serial, k, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.flags, (int)f, ty, why);
            nvn_vk_missing("a compute image that cannot be bound (the log says why)");
            return 0;
        }
        {   static int n; if (n++ < 20) printf("nvn_vk: compute program #%d binds image %u: texture format 0x%x %dx%dx%d flags 0x%x, level %d, layers %d+%d, view format %d, type %d, %s\n", p->serial, k, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.flags, e.lvl0, e.lay0, e.nlay, (int)f, ty, (v->images_w >> k & 1) ? "written" : "read only"); }
        vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL);
        iview[k] = vt_storage_view(vt, &e, ty, f); itex[k] = t;
    }
    return 1;
}
static void compute_pipe_make(VStage *v) {         /* any thread */
    {
        VkDescriptorSetLayoutBinding b0[40], b1[64];
        int n0 = 0, n1 = 0;
        for (unsigned n = 0; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        if (v->ssbo_w) { VkDescriptorSetLayoutBinding b = {38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }
        for (unsigned k = 0; k < 8; k++) if (v->images >> k & 1) { VkDescriptorSetLayoutBinding b = {32 + k, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }      /* SMO: MxIn.image_base 32 */
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &v->cdl[i]));
        }
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = v->cdl;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &v->cl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = stage_mod_get(v); ci.stage.pName = "main"; ci.layout = v->cl;
        VkPipeline cp = NULL;
        printf("nvn_vk_cs: compute stage %u (warp %d): pipeline being made\n", v->id, v->warp);
        VkResult r = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, &cp);
        if (r != VK_SUCCESS) vkd_fail("vkCreateComputePipelines (compute stage %u): %s", v->id, vk_result_name(r));
        v->cp = cp;
        stage_mod_releasable(v);
        stage_mod_put(v);
        plat_atomic_inc(&R.pipelines);
    }
}
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz) {
    static BufRef bufs[40];
    int nbuf = 0;
    int16_t wcb[16];
    uint64_t al = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    pass_end();
    if (g_cs_debt) {                               /* 0.1.7, image_barrier: what fragments wrote since the last dispatch */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        g_cs_debt = 0;
        cmd_begin();
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    /* textures, then buffers (both may submit: upload, ring wrap, copy-back), then the mask word, then the commands */
    g_ndtex = 0;
    for (int i = 0; i < v->nsmp; i++) resolve_sampler(5, v, i);
    VkImageView iview[8];
    Tex *itex[8];
    if (v->images && !compute_images(p, v, iview, itex)) return;
    if (g_ncdisp == 2048 || g_cb_pending) cb_sync();
    uint64_t wm_off = 0;
    for (int attempt = 0; ; attempt++) {           /* a copy-back in here (cb_sync) frees the CBs chosen before it: everything is then chosen again */
        uint32_t epoch = g_cb_epoch;
        nbuf = 0;
        memset(wcb, 0xff, sizeof wcb);
        for (unsigned n = 0; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = c2_ring(5); b->size = sizeof R.c2[5]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[5][n - 3].addr && R.ubo[5][n - 3].size) buf_ref(R.ubo[5][n - 3].addr, R.ubo[5][n - 3].size, al, b);
            else b->zero = 1;                      /* c0 outside the storage descriptors, or nothing bound: zeros (OpenGL: nothing bound) */
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            Range r = R.ssbo[5][k];
            b->zero = !(r.addr && r.size); b->buf = R.ring;
            if (b->zero) continue;
            if (!(v->ssbo_w >> k & 1)) { buf_ref(r.addr, r.size, al, b); continue; }
            CB *c = g_cb_live ? cb_inside(r.addr, r.size) : NULL;
            if (c && (r.addr - c->addr) % al) c = NULL;
            if (!c) { if (g_cb_live && cb_overlap(r.addr, r.size)) cb_sync(); c = cb_new(r.addr, r.size); }
            b->buf = c->buf; b->off = r.addr - c->addr; b->size = r.size; wcb[k] = (int16_t)(c - g_cb);
        }
        if (v->ssbo_w) { static const uint32_t zero[4]; wm_off = ring_raw(zero, 16); }
        if (epoch == g_cb_epoch) break;
        if (attempt == 3) vkd_fail("compute program #%d: its buffers cannot be bound without a copy-back in between", p->serial);
    }
    nvn_front_ccmp_dispatch(R.ubo[5], R.ssbo[5], v->ssbo_w, st->code, st->code_len, gx, gy, gz);      /* SMO_COMPUTE_CMP */
    cmd_begin();
    static VkWriteDescriptorSet w[90];
    static VkDescriptorBufferInfo bi[40];
    static VkDescriptorImageInfo ii[72];
    VkDescriptorSet sets[2];
    int nw = 0, ib = 0;
    sets[0] = set_alloc(v->cdl[0]); sets[1] = set_alloc(v->cdl[1]);
    memset(w, 0, sizeof w);
    for (unsigned n = 0; n < 34; n++) {
        unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
        if (!bit) continue;
        BufRef *b = &bufs[ib];
        bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = n < 18 ? n : 20 + n - 18; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    if (v->ssbo_w) {
        bi[ib].buffer = R.ring; bi[ib].offset = wm_off; bi[ib].range = 16;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 38; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    for (int i = 0; i < v->nsmp; i++) {
        ii[i].sampler = g_dtex[i].smp; ii[i].imageView = g_dtex[i].view; ii[i].imageLayout = g_dtex[i].layout;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = v->smp[i].binding; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i]; nw++;
    }
    for (unsigned k = 0, j = 64; k < 8; k++) {     /* SMO: the images, in the GENERAL layout (compute_images) */
        if (!(v->images >> k & 1)) continue;
        ii[j].sampler = NULL; ii[j].imageView = iview[k]; ii[j].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = 32 + k; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[nw].pImageInfo = &ii[j]; nw++; j++;
    }
    if (nw) vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cp);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cl, 0, 2, sets, 0, NULL);
    {   unsigned k = mx_split_x(st->local, st->smem_bytes);      /* SMO: a work group over 1024 invocations runs as k groups along x (mx_translate.h) */
        if (!k) { nvn_vk_missing("a compute work group over 1024 invocations that uses shared memory"); return; }
        vkCmdDispatch(R.cmd, gx * k, gy, gz);
    }
    if (v->images_w) {                             /* SMO: what the dispatch wrote into images is read by later dispatches and draws (a layout change orders the rest) */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        for (unsigned k = 0; k < 8; k++) if ((v->images_w >> k & 1) && itex[k]) { itex[k]->gpu_written = 1; itex[k]->uploaded = 1; }
        g_img_disp++;
    }
    if (v->ssbo_w) {                               /* what the dispatch wrote is read by later dispatches and draws of this command buffer */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CDisp *d = &g_cdisp[g_ncdisp++];
        d->wm_off = wm_off; d->written = v->ssbo_w; memcpy(d->cb, wcb, sizeof wcb);
    }
}
/* Everything of a draw up to the draw call. topo: NVN primitive. Returns 0 when the draw cannot be done (counted as skipped). */
static int draw_prepare(unsigned prim) {
    {   /* SMO port, with SMO_VK_PASSSHOT: the state of each draw of that frame (every draw kind passes here) */
            static int want = -2;
            want = passshot_frame() ? 1 : -1;
            if (want >= 0)
                printf("nvn_vk_draw: prim %u programs vs #%d fs #%d gs #%d target %p | viewport %s %.0f %.0f %.0f %.0f | scissor %s %d %d %d %d | pass %dx%d in_pass %d | cull %u discard %d render_off %d | chanmask %08x blend_on %x blend0 %u %u %u %u %u %u | ds %llx\n",
                       prim, R.prog[0] ? R.prog[0]->serial : -1, R.prog[1] ? R.prog[1]->serial : -1, R.prog[2] ? R.prog[2]->serial : -1, (void *)R.col[0].tex, R.have_viewport ? "set" : "default", R.viewport[0], R.viewport[1], R.viewport[2], R.viewport[3],
                       R.have_scissor ? "set" : "default", R.scissor[0], R.scissor[1], R.scissor[2], R.scissor[3], R.pass_w, R.pass_h, R.in_pass,
                       (unsigned)(R.poly[0] & 3), R.rast_discard, R.render_off, R.chanmask, (unsigned)R.blend_on,
                       R.blend[0][0], R.blend[0][1], R.blend[0][2], R.blend[0][3], R.blend[0][4], R.blend[0][5], (unsigned long long)R.ds);
                if (want >= 0) printf("nvn_vk_draw:   depth target %p fmt 0x%x %dx%d drange %g..%g polyoff %g %g %g poly %u %u %u %u\n", (void *)R.depth.tex, R.depth.tex ? R.depth.tex->b.format : 0, R.depth.tex ? R.depth.tex->b.width : 0, R.depth.tex ? R.depth.tex->b.height : 0, R.drange[0], R.drange[1], R.polyoff[0], R.polyoff[1], R.polyoff[2], R.poly[0], R.poly[1], R.poly[2], R.poly[3]);
            if (want >= 0) {
                float ar; memcpy(&ar, &R.alpha_ref, 4);
                printf("nvn_vk_draw:   alpha func %u ref %g | vertex ubo0 (c3) addr %llx size %llx:", (unsigned)R.alpha_func, ar,
                       (unsigned long long)R.ubo[0][0].addr, (unsigned long long)R.ubo[0][0].size);
                if (R.ubo[0][0].addr && R.ubo[0][0].size >= 80) for (int k = 4; k < 20; k++) { float fv; memcpy(&fv, (const void *)(uintptr_t)(R.ubo[0][0].addr + 4u * k), 4); printf(" %g", fv); }
                printf("\n");
                for (int u = 0; u < 3; u++) if (R.ubo[1][u].addr && R.ubo[1][u].size >= 32) {     /* the fragment stage's first uniform values (the white-out hunt) */
                    printf("nvn_vk_draw:   fragment ubo%d (c%d) addr %llx size %llx:", u, u + 3, (unsigned long long)R.ubo[1][u].addr, (unsigned long long)R.ubo[1][u].size);
                    for (int k = 0; k < 8; k++) { float fv; memcpy(&fv, (const void *)(uintptr_t)(R.ubo[1][u].addr + 4u * k), 4); printf(" %g", fv); }
                    printf("\n");
                }
            }
        }
    static const int8_t topo[7] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    if (!R.have_targets || R.render_off) { if (R.render_off) g_fr_offskip++; return 0; }
    if (prim > 6 || topo[prim] < 0) { static char msg[64]; snprintf(msg, sizeof msg, "a draw with NVN primitive type %u", prim); nvn_vk_missing(msg); return 0; }
    if (!R.prog[0]) return 0;
    if (R.prog[3] || R.prog[4]) { nvn_vk_missing("a draw with a tessellation stage"); return 0; }      /* the game has no tessellation stage (tests/check_all_shaders.py) */
    if (R.prog[2] && !D.f2.features.geometryShader) { nvn_vk_missing("a draw with a geometry stage on a GPU without geometry shaders"); return 0; }
    if (R.prog[2] && gs_dbg_mode() == 2) return 0;      /* SMO_VK_GS_DEBUG=2: left out */
    uint32_t ints = 0, sints = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        int nf = (int)(R.vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { ints |= 1u << i; if (k == 3 || k == 8 || k == 11) sints |= 1u << i; }
        if (nf == 0x3E) ints |= 1u << i;
        if (nf == 0x6A) { ints |= 1u << i; sints |= 1u << i; }
    }
    uint32_t key0[4] = {ints, sints, R.tex_int[0], R.tex_sint[0]}, key1[4] = {0, 0, R.tex_int[1], R.tex_sint[1]};
    int depth_only = !R.prog[1];
    /* fragment inputs the vertex code never writes are declared by a vertex variant of their own, left at (0, 0, 0, 1). Session 4:
     * the variant the program used last is asked for first (session 3 asked for the plain one at every draw of such a pair,
     * which hashed the machine code each time). */
    VStage *cur = R.prog[0]->st[0].vk;
    VStage *fs = depth_only ? NULL : vstage_get(R.prog[1], 1, key1, 0, 0), *vs, *gs = NULL;
    if (R.prog[2]) {                               /* SMO: a geometry stage (the lightning effects), translated for the draw's primitive class; it writes the fragment inputs, the vertex stage its inputs */
        uint32_t keyg[4] = {0, 0, R.tex_int[2], R.tex_sint[2]};
        VStage *gcur = R.prog[2]->st[2].vk;
        gs = vstage_get(R.prog[2], 2, keyg, gcur ? gcur->fo : 0, tclass[prim]);
        if (gs) { uint32_t need = fs ? fs->in_attr & ~(gs->out_attr & ~gs->fo) : 0; if (need != gs->fo) gs = vstage_get(R.prog[2], 2, keyg, need, tclass[prim]); }
    }
    vs = vstage_get(R.prog[0], 0, key0, cur ? cur->fo : 0, R.prog[2] != NULL);      /* a vertex stage before a geometry stage is a translation of its own (module_from_glsl_ex) */
    if (vs) { uint32_t want = R.prog[2] ? (gs ? gs->in_attr : 0) : fs ? fs->in_attr : 0, need = want & ~(vs->out_attr & ~vs->fo); if (need != vs->fo) vs = vstage_get(R.prog[0], 0, key0, need, R.prog[2] != NULL); }
    if (g_skip_flag) { g_skip_flag = 0; if (!vs || (!depth_only && !fs) || (R.prog[2] && !gs)) return 0; }      /* shader_wait = skip: the draw is left out (counted in vstage_get) */
    if (!vs || (!depth_only && !fs) || (R.prog[2] && !gs)) { nvn_vk_missing("a draw whose vertex, fragment or geometry stage could not be translated or compiled (the log names the stage)"); return 0; }
    VStage *sv[3] = {vs, fs, gs};                  /* by NVN stage: the bindings of stage s are 40 * s + ... (mx_translate.h) */

    /* 1. textures (may upload, change layouts, end the pass) */
    g_ndtex = 0;
    for (int s = 0; s < 3; s++) if (sv[s]) for (int i = 0; i < sv[s]->nsmp; i++) resolve_sampler(s, sv[s], i);
    /* 2. buffers: every range the draw reads, into the ring (may wrap the ring = submit) */
    static BufRef bufs[120];
    int nbuf = 0;
    uint64_t sal = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    for (int s = 0; s < 3; s++) {
        VStage *v = sv[s];
        if (!v) continue;
        Stage *st = &R.prog[s]->st[s];
        for (unsigned n = 1; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = c2_ring(s); b->size = sizeof R.c2[s]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[s][n - 3].addr && R.ubo[s][n - 3].size) buf_ref(R.ubo[s][n - 3].addr, R.ubo[s][n - 3].size, sal, b);
            else b->zero = 1;
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = !(R.ssbo[s][k].addr && R.ssbo[s][k].size); b->buf = R.ring;
            if (!b->zero) buf_ref(R.ssbo[s][k].addr, R.ssbo[s][k].size, sal, b);
        }
    }
    /* vertex streams: only the ones an attribute names (a stale binding of another mesh is not read, and is not copied) */
    uint64_t voff[16];
    VkBuffer vbufs[16];
    uint32_t streams = 0;
    int from_cb = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) if (R.vattr[i] & 0xff) streams |= 1u << ((R.vattr[i] >> 8) & 31);
    for (int i = 0; i < 16; i++) {
        BufRef b;
        voff[i] = ~0ull; vbufs[i] = R.ring;
        if (!(streams >> i & 1) || !R.vbuf[i].addr || !R.vbuf[i].size) continue;
        from_cb |= buf_ref(R.vbuf[i].addr, g_vtxgrow && g_vneed[i] > R.vbuf[i].size ? g_vneed[i] : R.vbuf[i].size, 1, &b);
        voff[i] = b.off; vbufs[i] = b.buf;
    }
    R.cb_draws += from_cb;
    {   /* SMO debug aid, with SMO_VK_PASSSHOT: each attribute of the draw (format, stream, offset, stride) and the first two
         * vertices of attribute 0 as floats, read from guest memory */
        static int want = -2;
        want = passshot_frame() ? 1 : -1;
        if (want >= 0) {
            for (int i = 0; i < 4 && i < R.nattr; i++) {
                uint32_t a = R.vattr[i]; int st = (int)((a >> 8) & 31);
                if (!(a & 0xff)) continue;
                printf("nvn_vk_vtx:   attr %d fmt 0x%x stream %d offset %u stride %u addr %llx size %llx", i, a & 0xff, st, a >> 14, st < 32 ? R.vstream[st].stride : 0, (unsigned long long)(st < 16 ? R.vbuf[st].addr : 0), (unsigned long long)(st < 16 ? R.vbuf[st].size : 0));
                if (i == 0 && st < 16 && R.vbuf[st].addr && (a & 0xff) >= 0x1f && (a & 0xff) <= 0x24 + 12) {
                    const float *fp = (const float *)P(R.vbuf[st].addr + (a >> 14));
                    uint32_t sd = R.vstream[st].stride ? R.vstream[st].stride / 4 : 4;
                    printf(" | v0 %g %g %g  v1 %g %g %g", fp[0], fp[1], fp[2], fp[sd], fp[sd + 1], fp[sd + 2]);
                }
                printf("\n");
            }
        }
    }
    /* 3. pass and pipeline */
    if (!pass_begin()) return 0;
    PKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.fs = fs ? fs->id : 0; k.topo_class = tclass[prim]; k.logic = R.logic; k.sshade = fs ? (uint8_t)R.prog[1]->sample_shading : 0;
    if (!D.has_eds3) {                             /* without extended dynamic state 3 this state is part of the key */
        k.chanmask = R.chanmask; k.sample_mask = R.sample_mask; k.poly = R.poly[2] < 3 ? R.poly[2] : 2;
        k.a2c = R.ms[1] != 0; k.dclamp = R.depth_clamp && D.f2.features.depthClamp; k.blend_on = R.blend_on;
        memcpy(k.blend, R.blend, sizeof k.blend);
    }
    /* 4. vertex input: described before the pipeline is looked up, because without VK_EXT_vertex_input_dynamic_state the
     * layout is part of the key (Android port; PKey v* in nvn_vk_m4.c) */
    VkVertexInputBindingDescription2EXT bd[17];
    VkVertexInputAttributeDescription2EXT ad[16];
    VkBuffer vb[17];
    VkDeviceSize vo[17], vstride[17];
    uint32_t nb = 0, na = 0;
    {
        uint32_t bmap[32], need_const = 0;
        memset(bmap, 0xff, sizeof bmap);
        for (int i = 0; i < 16; i++) {
            uint32_t a = i < R.nattr ? R.vattr[i] : 0;
            int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
            VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
            if (nf && !f) { static char msg[64]; snprintf(msg, sizeof msg, "vertex attribute format 0x%x", nf); nvn_vk_missing(msg); }
            memset(&ad[na], 0, sizeof ad[na]);
            ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
            if (f && stream < 16 && voff[stream] != ~0ull) {
                if (bmap[stream] == 0xffffffffu) {
                    bmap[stream] = nb;
                    memset(&bd[nb], 0, sizeof bd[nb]);
                    bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = R.vstream[stream].stride;
                    bd[nb].inputRate = R.vstream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = R.vstream[stream].divisor ? R.vstream[stream].divisor : 1;
                    vb[nb] = vbufs[stream]; vo[nb] = voff[stream]; nb++;
                }
                ad[na].binding = bmap[stream]; ad[na].format = f; ad[na].offset = a >> 14; na++;
            } else if (vs->dep >> i & 1 || 1) {       /* not supplied: (0, 0, 0, 1), as OpenGL's current attribute value; integer inputs take the integer copy */
                ad[na].binding = 16; ad[na].format = (ints >> i & 1) ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = (ints >> i & 1) ? 16 : 0;
                need_const = 1; na++;
            }
        }
        if (need_const) {
            memset(&bd[nb], 0, sizeof bd[nb]);
            bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
            for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
            vb[nb] = R.zero; vo[nb] = 65536; nb++;
        }
    }
    if (!D.has_vids) {
        k.vnb = (uint8_t)nb; k.vna = (uint8_t)na;
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t dv = bd[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? bd[i].divisor : 0;
            if (dv > 255 || (dv > 1 && !D.has_vad)) { nvn_vk_missing("a per-instance vertex divisor above 1 (or above 255) on a GPU without dynamic vertex input"); dv = 1; }
            k.vrate[i] = (uint8_t)dv; vstride[i] = bd[i].stride;
        }
        for (uint32_t i = 0; i < na; i++) {
            if ((uint32_t)ad[i].format > 255 || ad[i].offset > 0xffff) nvn_vk_missing("a vertex attribute format or offset that does not fit the pipeline key");
            k.vbind[i] = (uint8_t)ad[i].binding; k.vfmt[i] = (uint8_t)ad[i].format; k.voff[i] = (uint16_t)ad[i].offset;
        }
    }
    pkey_targets(&k);
    Pipe *p = gs ? gs_pipe_get(&k, vs, gs, fs) : pipe_get(&k, vs, fs);
    if (!p) return 0;                              /* shader_wait = skip: the pipeline is being made by a worker, the draw is left out (counted in pipe_get) */
    if (!p->p) { g_refused_draws++; return 0; }    /* Android port: the driver refused this pipeline (pipe_build): the draw is left out */
    if (gs) g_gs_draws++;
    if (!D.has_eds3 && k.blend_on) {               /* Mali: what a draw that blends into a format the GPU cannot blend draws into (once per program) */
        static int said[32], nsaid;
        for (int i = 0; i < R.ncol && i < 8; i++) {
            if (!(k.blend_on >> i & 1) || !k.cfmt[i] || fmt_supported((VkFormat)k.cfmt[i], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) || !R.col[i].tex) continue;
            int serial = R.prog[1] ? R.prog[1]->serial : -1, j;
            for (j = 0; j < nsaid && said[j] != serial; j++) {}
            if (j < nsaid || nsaid >= 32) break;
            said[nsaid++] = serial;
            printf("nvn_vk_fmt: frame %lld: program #%d blends into colour %d, a %dx%d texture of NVN format 0x%x (level %d), pass %dx%d\n", (long long)nvn_front_frame_next(), serial, i,
                   R.col[i].tex->b.width, R.col[i].tex->b.height, R.col[i].tex->b.format, target_level(&R.col[i]), R.pass_w, R.pass_h);
            break;
        }
    }
    if (!R.pipe_bound) R.dyn3_dirty = 1;          /* a new command buffer, or a pipeline of our own with static state was bound */
    if (R.pipe_bound != p) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p); R.pipe_bound = p; }
    if (R.dyn_dirty) { set_dynamic_state(); R.dyn_dirty = 0; }
    if (D.has_eds3 && (R.dyn3_dirty || R.dyn3_samples != k.samples)) { set_dynamic_state3(k.samples); R.dyn3_dirty = 0; }
    if (gs && g_gs_dbg_fs) { vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); R.dyn_dirty = 1; }      /* SMO_VK_GS_DEBUG=3 (gs_pipe_get) */
    vkCmdSetPrimitiveTopology(R.cmd, (VkPrimitiveTopology)topo[prim]);
    vkCmdSetPrimitiveRestartEnable(R.cmd, R.restart && (D.has_plr || prim == 3 || prim == 5 || prim == 6));
    if (D.has_vids) {
        vkCmdSetVertexInputEXT(R.cmd, nb, bd, na, ad);
        if (nb) vkCmdBindVertexBuffers(R.cmd, 0, nb, vb, vo);
    } else if (nb) vkCmdBindVertexBuffers2(R.cmd, 0, nb, vb, vo, NULL, vstride);      /* the strides: dynamic state of the pipeline */
    /* 5. descriptors */
    {
        static VkWriteDescriptorSet w[320];
        static VkDescriptorBufferInfo bi[120];
        static VkDescriptorImageInfo ii[200];
        /* M4: buffers change with every draw: pushed into the command buffer (no set, no pool) when the device can; the
         * textures of a draw repeat: one set per distinct list (layout, views, samplers, image layouts) per command buffer */
        VkDescriptorSet sets[2] = {NULL, NULL};
        int nw = 0, ib = 0, it = 0, nd = 0;
        uint32_t doff[64];
        memset(w, 0, sizeof w[0] * (size_t)(p->nb[0] + p->nb[1]));
        for (int s = 0; s < 3; s++) {
            VStage *v = sv[s];
            if (!v) continue;
            for (unsigned n = 1; n < 34; n++) {
                unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
                if (!bit) continue;
                BufRef *b = &bufs[ib];
                bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
                w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 40u * (unsigned)s + (n < 18 ? n : 20 + n - 18); w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib];
                if (nw < 128 && (p->dynm[nw >> 6] >> (nw & 63) & 1)) {      /* dynamic (dyn0_pick: the writes are the layout's bindings in its order): the offset goes to the bind */
                    w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; doff[nd++] = (uint32_t)bi[ib].offset; bi[ib].offset = 0;
                }
                nw++; ib++;
            }
        }
        if (p->push0) { if (nw) vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, (uint32_t)nw, w); }
        else if (p->nb[0]) {                       /* without push descriptors: one set per distinct list (layout, buffers, offsets of the others, ranges) per command buffer;
                                                    * not for the one layout of every library pipeline (g_udl), whose lists hold other bindings */
            uint64_t key = fnv(fnv(0xcbf29ce484222325ULL, &p->dl[0], sizeof p->dl[0]), bi, sizeof bi[0] * (size_t)nw);
            unsigned slot;
            if (!key) key = 1;
            for (slot = (unsigned)key & 4095; g_bset[slot].gen == g_tset_gen && g_bset[slot].key != key; slot = (slot + 1) & 4095) {}
            int hit = g_bset[slot].gen == g_tset_gen && g_bset[slot].key == key;
            if (p->dl[0] == g_udl[0]) { hit = 0; slot = 4096; }
            if (hit && (g_bset[slot].dl != p->dl[0] || g_bset[slot].n != nw || memcmp(&g_bset_list[g_bset[slot].off], bi, sizeof bi[0] * (size_t)nw))) { hit = 0; R.tset_collisions++; slot = 4096; }
            if (hit) { sets[0] = g_bset[slot].set; R.sets_reused++; g_bset_hits++; }
            else {
                sets[0] = set_alloc(p->dl[0]); g_bset_made++;
                for (int i = 0; i < nw; i++) w[i].dstSet = sets[0];
                vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL);
                if (slot < 4096 && g_bset_used < 3000 && g_bset_list_used + nw <= (int)(sizeof g_bset_list / sizeof g_bset_list[0])) {
                    g_bset[slot].key = key; g_bset[slot].set = sets[0]; g_bset[slot].gen = g_tset_gen; g_bset[slot].dl = p->dl[0]; g_bset[slot].n = nw; g_bset[slot].off = g_bset_list_used;
                    memcpy(&g_bset_list[g_bset_list_used], bi, sizeof bi[0] * (size_t)nw);
                    g_bset_list_used += nw; g_bset_used++;
                }
            }
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &sets[0], (uint32_t)nd, doff);
        }
        if (p->nb[1]) {
            uint64_t key = fnv(0xcbf29ce484222325ULL, &p->dl[1], sizeof p->dl[1]);
            unsigned slot;
            if (p->dl[1] == g_udl[1]) for (int s = 0; s < 3; s++) { VStage *v = sv[s]; if (v) for (int i = 0; i < v->nsmp; i++) key = fnv(key, &v->smp[i].binding, sizeof v->smp[i].binding); }      /* session 9: one layout for every pipeline: the binding numbers tell the lists apart */
            for (int i = 0; i < g_ndtex; i++) { key = fnv(key, &g_dtex[i].view, sizeof g_dtex[i].view); key = fnv(key, &g_dtex[i].smp, sizeof g_dtex[i].smp); key = fnv(key, &g_dtex[i].layout, sizeof g_dtex[i].layout); }
            if (!key) key = 1;
            for (slot = (unsigned)key & 4095; g_tset[slot].gen == g_tset_gen && g_tset[slot].key != key; slot = (slot + 1) & 4095) {}
            /* session 4: a hash hit is confirmed by comparing the whole list (layout, views, samplers, image layouts); a
             * different list with the same hash gets a set of its own and is counted */
            int hit = g_tset[slot].gen == g_tset_gen && g_tset[slot].key == key;
            if (hit && (g_tset[slot].dl != p->dl[1] || g_tset[slot].n != g_ndtex || memcmp(&g_tset_list[g_tset[slot].off], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex))) { hit = 0; R.tset_collisions++; slot = 4096; }
            if (hit) { sets[1] = g_tset[slot].set; R.sets_reused++; }
            else {
                int n1 = 0;
                sets[1] = set_alloc(p->dl[1]);
                for (int s = 0; s < 3; s++) {
                    VStage *v = sv[s];
                    if (!v) continue;
                    for (int i = 0; i < v->nsmp; i++) {
                        ii[it].sampler = g_dtex[it].smp; ii[it].imageView = g_dtex[it].view; ii[it].imageLayout = g_dtex[it].layout;
                        memset(&w[n1], 0, sizeof w[n1]);
                        w[n1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n1].dstSet = sets[1]; w[n1].dstBinding = v->smp[i].binding; w[n1].descriptorCount = 1;
                        w[n1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n1].pImageInfo = &ii[it]; n1++; it++;
                    }
                }
                vkUpdateDescriptorSets(V.device, (uint32_t)n1, w, 0, NULL);
                if (slot < 4096 && g_tset_used < 3000 && g_tset_list_used + g_ndtex <= (int)(sizeof g_tset_list / sizeof g_tset_list[0])) {
                    g_tset[slot].key = key; g_tset[slot].set = sets[1]; g_tset[slot].gen = g_tset_gen; g_tset[slot].dl = p->dl[1]; g_tset[slot].n = g_ndtex; g_tset[slot].off = g_tset_list_used;
                    memcpy(&g_tset_list[g_tset_list_used], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex);
                    g_tset_list_used += g_ndtex; g_tset_used++;
                }
            }
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &sets[1], 0, NULL);
        }
    }
    {   uint32_t pc[4] = {(uint32_t)R.alpha_func, 0, 0, 0};
        memcpy(&pc[1], &R.alpha_ref, 4); memcpy(&pc[2], &R.point_size, 4);
        if (RS_ON) {                               /* track O: mx_fcs = game pixels per image pixel of this pass (fragment stages translated with screen_scale); points shrink with the image */
            float fcs = R.pass_rs ? 1.0f / R.pass_sx : 1.0f, ps = R.pass_rs ? R.point_size * R.pass_sx : R.point_size;
            memcpy(&pc[3], &fcs, 4); memcpy(&pc[2], &ps, 4);
        }
        vkCmdPushConstants(R.cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    }
    return 1;
}

/* The pass formats of a target set as the pipeline key holds them (also used by the look-ahead of nvn_vk_m4.c on targets
 * that are not bound yet). Returns 0 when one of the images does not exist yet. */
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth) {
    int all = 1;
    k->ncolor = (uint8_t)ncol; k->samples = 1;
    for (int i = 0; i < ncol; i++) {
        if (!col[i].tex) continue;
        VTex *vt = col[i].tex->vk;
        int ex, dec;
        if (!vt) { all = 0; continue; }
        VkFormat f = vt->fmt;
        if ((col[i].view.set & 0x84) == 0x84 && col[i].view.format && col[i].view.format != col[i].tex->b.format) { VkFormat f2 = tex_vkfmt(col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; }
        k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples;
    }
    if (depth->tex) { VTex *vt = depth->tex->vk; if (!vt) all = 0; else { k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; } }
    return all;
}
static void pkey_targets(PKey *k) { pkey_targets_of(k, R.ncol, R.col, &R.depth); }
#if 0   /* session 3's form */
static void pkey_targets(PKey *k) {
    k->ncolor = (uint8_t)R.ncol; k->samples = 1;
    for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) { VTex *vt = R.col[i].tex->vk; int ex, dec; VkFormat f = vt->fmt; if ((R.col[i].view.set & 0x84) == 0x84 && R.col[i].view.format) { VkFormat f2 = tex_vkfmt(R.col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; } k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples; }
    if (R.depth.tex && R.depth.tex->vk) { VTex *vt = R.depth.tex->vk; k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; }
}
#endif
/* ClearDepthStencil with a stencil mask that is not all bits: Vulkan's clears have no mask, so one triangle over the
 * rectangle replaces the masked bits (no colour, no depth). The pass is active. */
static void stencil_clear_masked(uint32_t mask, uint32_t value, int sx, int sy, int sw, int sh) {
    static VStage *vs;
    PKey k;
    if (!vs) {
        vs = calloc(1, sizeof *vs);
        vs->id = ++g_vstage_ids; vs->ok = 1; vs->stage = -1;
        vs->mod = module_from_glsl(0, "#version 460\nvoid main() { gl_Position = vec4(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0); }\n", "internal stencil clear", &g_spv_unused, NULL);
        if (!vs->mod) vkd_fail("the internal stencil-clear vertex shader does not compile");
        vs->next = g_vstage[0]; g_vstage[0] = vs;
    }
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.sample_mask = 0xffffffffu; k.topo_class = 2; k.poly = 2; k.logic = 3;
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, NULL);
    VkViewport vp = {0, 0, (float)R.pass_w, (float)R.pass_h, 0, 1};
    VkRect2D sc = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
    float zero4[4] = {0, 0, 0, 0};
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    if (D.has_eds3) {                              /* session 4: what the key held (no colour written, no blending, fill, every sample) is dynamic state now */
        VkBool32 en[8] = {0};
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8] = {0};
        VkSampleMask all = 0xffffffffu;
        uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
        for (int i = 0; i < 8; i++) { eq[i].srcColorBlendFactor = eq[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; eq[i].dstColorBlendFactor = eq[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; eq[i].colorBlendOp = eq[i].alphaBlendOp = VK_BLEND_OP_ADD; }
        vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en); vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq); vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
        vkCmdSetPolygonModeEXT(R.cmd, VK_POLYGON_MODE_FILL); vkCmdSetAlphaToCoverageEnableEXT(R.cmd, VK_FALSE); vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(k.samples ? k.samples : 1), &all);
        if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, VK_FALSE);
        R.dyn3_dirty = 1;
    }
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc); vkCmdSetLineWidth(R.cmd, 1.0f); vkCmdSetDepthBias(R.cmd, 0, 0, 0); vkCmdSetBlendConstants(R.cmd, zero4);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, mask); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, value);
    vkCmdSetCullMode(R.cmd, VK_CULL_MODE_NONE); vkCmdSetFrontFace(R.cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE); vkCmdSetPrimitiveTopology(R.cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE); vkCmdSetDepthCompareOp(R.cmd, VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(R.cmd, VK_TRUE);
    vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBiasEnable(R.cmd, VK_FALSE); vkCmdSetPrimitiveRestartEnable(R.cmd, VK_FALSE); vkCmdSetRasterizerDiscardEnable(R.cmd, VK_FALSE);
    if (D.has_vids) vkCmdSetVertexInputEXT(R.cmd, 0, NULL, 0, NULL);      /* else the key of this pipeline holds the empty layout */
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    R.pipe_bound = NULL; R.dyn_dirty = 1;
}

/* ---------------------------------------------------------------- copies */
/* CopyBufferToTexture and WriteTexels: texels from memory into a region of one level. The image is not synchronised
 * with the texture's own memory first (the OpenGL path does not either). */
static void write_region(Tex *t, const View *vp, const int *r, const void *data, int64_t row_stride, int64_t image_stride) {
    View v;
    memset(&v, 0, sizeof v);
    if (vp) v = *vp;
    VTex *vt = vt_get2(t, 0);
    if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->decode || vt->expand) { nvn_vk_missing("CopyBufferToTexture / WriteTexels into a depth, ASTC-decoded or 3-component texture"); return; }
    if (vt->sub) { static int nl; if (nl++ < 8) printf("nvn_vk_fmt: format fix: the game writes texels into a texture of a replaced format (NVN 0x%x): they are not converted\n", (unsigned)t->b.format); }
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0; else if (t->b.target != 2) layer += r[2];
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    uint64_t off = ring_raw(data, size);
    pass_end(); cmd_begin();
    if (vt->rs) { rs_upload(vt, level, layer, off, (uint32_t)rowlen, (uint32_t)rows, r[0], r[1], r[3], r[4]); t->gpu_written = 1; t->uploaded = 1; return; }      /* track O */
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off; c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    t->gpu_written = 1; t->uploaded = 1;
}
static void copy_buf_tex(const Cmd *m) {
    Tex *t = P(m->a[1]);
    View v;
    int r[6];
    if (!t || t->dead || !m->a[0]) return;
    memcpy(&v, BLOB(m), sizeof v); memcpy(r, BLOB(m) + 24, 24);
    write_region(t, &v, r, P(m->a[0]), R.row_stride, R.image_stride);
}
static void vk_write_texels(void *arg) {
    WriteJob *j = arg;
    if (V.alive && j->tex && !j->tex->dead) write_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->data, j->row_stride, 0);
}
/* Session 5. CopyTextureToBuffer, ReadTexels, the movie maker's present: a region of one level to host memory, as the
 * OpenGL path's glGetTextureSubImage (tight rows, or the given row / image strides). Synchronous: the command buffer is
 * submitted and waited for (these calls are rare: album, replays, screenshots). Returns the bytes written, 0 when it stopped. */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride) {
    VTex *vt = vt_get(t);
    if (vt->decode || vt->expand || vt->samples > 1) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of an ASTC-decoded, 3-component or multisample texture"); return 0; }
    if (vt->sub && !g_diag_busy) { static int nl; if (nl++ < 8) printf("nvn_vk_fmt: format fix: the game reads back a texture of a replaced format (NVN 0x%x): it gets the replaced format's bytes\n", (unsigned)t->b.format); }
    if (vt->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of a combined depth-stencil texture"); return 0; }
    int level = vp && (vp->set & 0x81) == 0x81 ? vp->lvl0 : 0, layer = vp && (vp->set & 0x82) == 0x82 ? vp->lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    if (!size) return 0;
    VMem mem;
    VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem);
    VkBufferImageCopy c;
    pass_end(); cmd_begin();
    VkImageLayout was = vt->layout;
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = vt->aspect; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)(layer + r[2]); c.imageSubresource.layerCount = vt->is3d ? 1 : (uint32_t)depth;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    if (vt->rs) {                                  /* track O: the game reads ITS rectangle: the image's part of it is blitted up to a temporary image of that size */
        int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
        VkImageBlit b;
        if (r[3] <= 0 || r[4] <= 0 || !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            nvn_vk_missing("resolution scale: ReadTexels / CopyTextureToBuffer of a scaled texture whose format cannot be blitted");
            if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
            vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
            return 0;
        }
        VTex *tmp = rs_temp(vt, r[3], r[4]);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = vt->aspect; b.srcSubresource.mipLevel = (uint32_t)level; b.srcSubresource.baseArrayLayer = (uint32_t)(layer + r[2]); b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = vt->aspect; b.dstSubresource.layerCount = 1;
        b.srcOffsets[0].x = rs_edge(r[0], vt->sx, lw); b.srcOffsets[0].y = rs_edge(r[1], vt->sy, lh); b.srcOffsets[1].x = rs_edge(r[0] + r[3], vt->sx, lw); b.srcOffsets[1].y = rs_edge(r[1] + r[4], vt->sy, lh); b.srcOffsets[1].z = 1;
        if (b.srcOffsets[1].x <= b.srcOffsets[0].x) b.srcOffsets[1].x = b.srcOffsets[0].x + 1; if (b.srcOffsets[1].y <= b.srcOffsets[0].y) b.srcOffsets[1].y = b.srcOffsets[0].y + 1;
        if (b.srcOffsets[1].x > lw) { b.srcOffsets[0].x = lw - 1; b.srcOffsets[1].x = lw; } if (b.srcOffsets[1].y > lh) { b.srcOffsets[0].y = lh - 1; b.srcOffsets[1].y = lh; }
        b.dstOffsets[1].x = r[3]; b.dstOffsets[1].y = r[4]; b.dstOffsets[1].z = 1;
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageSubresource.mipLevel = 0; c.imageSubresource.baseArrayLayer = 0; c.imageSubresource.layerCount = 1; c.imageOffset.x = c.imageOffset.y = c.imageOffset.z = 0;
        vkCmdCopyImageToBuffer(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        g_rs_down++;
    } else
    vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
    cmd_wait_all();
    memcpy(out, mem.map, (size_t)size);
    vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
    R.readbacks++;
    return (size_t)size;
}
/* SMO port: nvnTextureInvalidateTexels: what the GPU rendered or copied into the texture becomes readable in its pool
 * memory, in the texture's own layout (the OpenGL path's job_read_texels with to_pool does the same): each layer of the
 * level (the view's first level, else 0) is read back as packed rows and written into the pool memory with
 * tex_swizzle_level (a linear texture: row by row at its stride); the texture's hash is then taken again (no upload).
 * In SMO agl's SMAA lookup textures (agl::TextureTileConverter) call it. Not for a texture the GPU never wrote (its
 * memory is already right), one kept decoded on the CPU side, or a multisample one (said in the log). */
static void vk_invalidate_texels(ReadJob *j) {
    Tex *t = j->tex;
    static int nlog;
    if (!t->mem || !t->gpu_written || t->b.packaged) return;
    const TexFormat *tf = tex_format(t->b.format);
    if ((tf && tf->sw_decode) || t->b.samples > 1) {
        if (nlog++ < 5) printf("nvn_vk: InvalidateTexels on a texture of format 0x%x%s: its pool memory is not rewritten\n", t->b.format, t->b.samples > 1 ? " (multisample)" : " kept decoded");
        return;
    }
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0;
    TexLayout L;
    tex_layout(&L, t->b.target, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.levels, t->b.flags & ~0x50, 0,
               t->b.has_layout && t->b.layout[0] <= 4 ? t->b.layout[0] : -1);
    if (level >= L.levels) return;
    int is3d = t->b.target == 2, layers = is3d ? 1 : L.layers;
    uint8_t *lin = malloc(L.lvl_linear_size[level] ? L.lvl_linear_size[level] : 1);
    if (!lin) return;
    cb_sync();
    for (int layer = 0; layer < layers; layer++) {
        int r[6] = {0, 0, layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1};
        View v;
        memset(&v, 0, sizeof v);
        v.set = 0x81; v.lvl0 = level; v.nlvl = 1;
        if (!read_region(t, &v, r, lin, 0, 0)) break;
        if (t->b.flags & 0x10) {                                       /* linear: rows at the texture's stride */
            size_t row = L.lvl_linear_size[level] / (size_t)(L.lvl_h[level] > 0 ? L.lvl_h[level] : 1);
            int64_t stride = t->b.stride ? t->b.stride : (int64_t)row;
            if (t->mem + stride * L.lvl_h[level] > t->limit) break;
            for (int y = 0; y < L.lvl_h[level]; y++) memcpy(t->mem + stride * y, lin + row * (size_t)y, row);
        } else {
            uint8_t *dst = (uint8_t *)(uintptr_t)tex_level_ptr(&L, t->mem, level, layer);
            if (t->limit && dst + L.lvl_size[level] > t->limit) break;
            tex_swizzle_level(&L, level, lin, dst);
        }
    }
    free(lin);
    nvn_front_tex_rehash(t);
}
static void vk_read_texels(void *arg) {
    ReadJob *j = arg;
    if (!V.alive || !j->tex || j->tex->dead) return;
    if (j->to_pool) { vk_invalidate_texels(j); return; }
    cb_sync();
    read_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->out, j->row_stride, j->image_stride);
}
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);      /* nnalbum.c, as nvn_gl.c job_present_movie */
static void vk_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!V.alive || !t || t->dead || t->b.width <= 0 || t->b.height <= 0) return;
    VTex *vt = vt_get(t);
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, r[6] = {0, 0, 0, w, h, 1}, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("nvnQueuePresentTexture to the movie maker's window with a window texture that is not 8-bit RGBA"); return; }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    if (read_region(t, NULL, r, px, 0, 0)) {
        if (bgr) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
        album_movie_push_frame(px, w, h, w * 4, 0);
    }
    free(px);
}
static int64_t g_blit_same;                         /* SMO: copies within one texture (blit_tex) */
static void blit_tex(Tex *s, const View *sv, const int *sr, Tex *d, const View *dv, const int *dr, int linear) {
    VTex *vs = vt_get(s), *vd = vt_get(d);
    VkImageBlit b;
    d->gpu_written = 1;
    /* SMO: a copy within one texture (another level or layer of it, e.g. a mip chain made level by level) goes through the
     * GENERAL layout, with a barrier so that it reads what the copy before it wrote; the same level and layer is refused */
    int same = vs == vd;
    VkImageLayout sl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pass_end(); cmd_begin();
    if (same) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        vt_layout(vs, VK_IMAGE_LAYOUT_GENERAL);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        g_blit_same++;
    } else { vt_layout(vs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); }
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = vs->aspect; b.srcSubresource.mipLevel = sv && (sv->set & 0x81) == 0x81 ? (uint32_t)sv->lvl0 : 0; b.srcSubresource.baseArrayLayer = sv && (sv->set & 0x82) == 0x82 && !vs->is3d ? (uint32_t)sv->lay0 : 0; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = vd->aspect; b.dstSubresource.mipLevel = dv && (dv->set & 0x81) == 0x81 ? (uint32_t)dv->lvl0 : 0; b.dstSubresource.baseArrayLayer = dv && (dv->set & 0x82) == 0x82 && !vd->is3d ? (uint32_t)dv->lay0 : 0; b.dstSubresource.layerCount = 1;
    b.srcOffsets[0].x = sr[0]; b.srcOffsets[0].y = sr[1]; b.srcOffsets[1].x = sr[0] + sr[3]; b.srcOffsets[1].y = sr[1] + sr[4]; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = dr[0]; b.dstOffsets[0].y = dr[1]; b.dstOffsets[1].x = dr[0] + dr[3]; b.dstOffsets[1].y = dr[1] + dr[4]; b.dstOffsets[1].z = 1;
    /* SMO port: a 3D side copies the z range of its region (z, depth), starting at the view's first layer (a 3D texture's
     * view layers are its slices). Before, every 3D copy took z 0..1: SMO's 8x8x8 colour-grading LUT got one slice and
     * every 3D frame came out blue (boot86, nvn_vk_lut). Vulkan filters a 3D blit in all three axes, as the scaled copy needs. */
    if (vs->is3d) { int z0 = (sv && (sv->set & 0x82) == 0x82 ? sv->lay0 : 0) + sr[2]; b.srcOffsets[0].z = z0; b.srcOffsets[1].z = z0 + (sr[5] > 0 ? sr[5] : 1); }
    if (vd->is3d) { int z0 = (dv && (dv->set & 0x82) == 0x82 ? dv->lay0 : 0) + dr[2]; b.dstOffsets[0].z = z0; b.dstOffsets[1].z = z0 + (dr[5] > 0 ? dr[5] : 1); }
    if (vs->is3d != vd->is3d && (b.srcOffsets[1].z - b.srcOffsets[0].z != 1 || b.dstOffsets[1].z - b.dstOffsets[0].z != 1)) { nvn_vk_missing("CopyTextureToTexture of several slices between a 3D and a 2D texture"); return; }
    if (same && b.srcSubresource.mipLevel == b.dstSubresource.mipLevel && (vs->is3d || b.srcSubresource.baseArrayLayer == b.dstSubresource.baseArrayLayer)) { nvn_vk_missing("CopyTextureToTexture within one level and layer of a texture"); return; }
    int rs_sw = sr[3], rs_sh = sr[4], rs_dw = dr[3], rs_dh = dr[4];
    if (vs->rs || vd->rs) {                        /* track O: the game's rectangles on the scaled images */
        if (vs->rs) {
            int lw = lvl_(vs->w, (int)b.srcSubresource.mipLevel), lh = lvl_(vs->h, (int)b.srcSubresource.mipLevel);
            b.srcOffsets[0].x = rs_edge(sr[0], vs->sx, lw); b.srcOffsets[0].y = rs_edge(sr[1], vs->sy, lh); b.srcOffsets[1].x = rs_edge(sr[0] + sr[3], vs->sx, lw); b.srcOffsets[1].y = rs_edge(sr[1] + sr[4], vs->sy, lh);
            rs_sw = b.srcOffsets[1].x - b.srcOffsets[0].x; rs_sh = b.srcOffsets[1].y - b.srcOffsets[0].y;
        }
        if (vd->rs) {
            int lw = lvl_(vd->w, (int)b.dstSubresource.mipLevel), lh = lvl_(vd->h, (int)b.dstSubresource.mipLevel);
            b.dstOffsets[0].x = rs_edge(dr[0], vd->sx, lw); b.dstOffsets[0].y = rs_edge(dr[1], vd->sy, lh); b.dstOffsets[1].x = rs_edge(dr[0] + dr[3], vd->sx, lw); b.dstOffsets[1].y = rs_edge(dr[1] + dr[4], vd->sy, lh);
            rs_dw = b.dstOffsets[1].x - b.dstOffsets[0].x; rs_dh = b.dstOffsets[1].y - b.dstOffsets[0].y;
        }
        if (!rs_sw || !rs_sh || !rs_dw || !rs_dh) return;
        g_rs_blits++;
    }
    if (vs->aspect != vd->aspect) { nvn_vk_missing("CopyTextureToTexture between a colour and a depth format"); return; }
    if (vs->samples > 1 && vd->samples == 1 && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT && rs_sw == rs_dw && rs_sh == rs_dh) {
        VkImageResolve rv;
        memset(&rv, 0, sizeof rv);
        rv.srcSubresource = b.srcSubresource; rv.dstSubresource = b.dstSubresource; rv.srcOffset = b.srcOffsets[0]; rv.dstOffset = b.dstOffsets[0];
        rv.extent.width = (uint32_t)rs_sw; rv.extent.height = (uint32_t)rs_sh; rv.extent.depth = 1;
        vkCmdResolveImage(R.cmd, vs->img, sl, vd->img, dl, 1, &rv);
        return;
    }
    if (vs->samples > 1 || vd->samples > 1) { nvn_vk_missing("a scaled or depth copy of a multisample texture"); return; }
    /* glBlitFramebuffer with FRAMEBUFFER_SRGB off copies the stored values; vkCmdBlitImage converts between an sRGB and a
     * linear format. Same class on both sides = the same values. */
    vkCmdBlitImage(R.cmd, vs->img, sl, vd->img, dl, 1, &b, linear && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---------------------------------------------------------------- replay */
static void replay(uint64_t handle, int depth);
/* NVN_DRAW_PRIMITIVE_QUADS (7), which Vulkan has not (met in a Sand Kingdom room, SandWorldKillerExStage): each quad
 * v0 v1 v2 v3 is drawn as the triangles v0 v1 v2 and v0 v2 v3, the quad's own winding, from an index list made here in
 * the stream ring: for arrays the vertex numbers from 0 (the draw's first vertex is its vertex offset), for elements the
 * game's indices (type t: 0 8-bit, 1 16-bit, 2 32-bit) rearranged. Vertices after the last whole quad are left out, as
 * GL leaves them. Render thread only (the list is a static buffer); call before draw_prepare, like buf_ref: it may wrap
 * the ring. Returns the ring offset of the 32-bit list, its length in *n. */
static int64_t g_quad_draws;
static uint64_t quad_indices(uint64_t src, unsigned t, uint32_t count, uint32_t *n) {
    static uint32_t *buf, cap;
    uint32_t nq = count / 4;
    *n = nq * 6;
    if (*n > cap) { free(buf); cap = *n + 1024; buf = malloc((size_t)cap * 4); if (!buf) vkd_fail("out of memory for a quad index list of %u indices", *n); }
    for (uint32_t q = 0; q < nq; q++) {
        uint32_t v[4], *o = buf + 6 * q;
        for (uint32_t k = 0; k < 4; k++) {
            uint64_t i = (uint64_t)q * 4 + k;
            v[k] = !src ? (uint32_t)i : t == 0 ? rd8(src + i) : t == 1 ? rd16(src + 2 * i) : rd32(src + 4 * i);
        }
        o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = v[0]; o[4] = v[2]; o[5] = v[3];
    }
    if (g_quad_draws++ == 0)
        printf("nvn_vk: first quad draw (NVN_DRAW_PRIMITIVE_QUADS, %s, %u vertices): drawn as %u triangles, programs vertex #%d fragment #%d\n", src ? "elements" : "arrays",
               count, nq * 2, R.prog[0] ? R.prog[0]->serial : -1, R.prog[1] ? R.prog[1]->serial : -1);
    return ring_raw(buf, (uint64_t)*n * 4);
}
/* 0.1.6: what a captured frame's draws really do, for gaps in models. Pipeline statistics of each draw (nvn_vk_tri, printed when the
 * round ends): triangles the GPU was given, went to clipping and came out of it, fragments shaded; where the driver has them. On a
 * screen with few draws also checksums of the data it reads (nvn_vk_hash): the index list and each vertex stream, and a code hash
 * of each stage that is the same on every GPU, so one log can be held against another's. */
static void gw_after_draw(void);                   /* 0.1.7: the glow tracer, after diag_target */
#define DQ_MAX 300
static VkQueryPool g_dq_pool;
static int g_dq_on = -1, g_dq_n, g_dq_open, g_dq_hashes, g_dq_rounds;
static struct { int vs, fs; uint32_t count, inst; } g_dq[DQ_MAX];
static uint64_t stage_code_hash(const Prog *p, int s) { return p && p->st[s].code ? fnv(0xcbf29ce484222325ULL, p->st[s].code, (size_t)p->st[s].code_len) : 0; }
static void dq_begin(uint32_t count, uint32_t inst, uint64_t indices, unsigned t, int32_t base, uint32_t first) {
    if (g_diag_mode != 2 || !R.in_pass) return;
    int vs = R.prog[0] ? R.prog[0]->serial : -1, fs = R.prog[1] ? R.prog[1]->serial : -1;
    if (g_diag_low && g_dq_hashes < 1500) {
        char line[400];
        int64_t mv = indices ? (int64_t)index_max(indices, (int)count, t) + base : (int64_t)first + (int64_t)count - 1;
        uint32_t mask = 0;
        int lp = 0;
        g_dq_hashes++;
        for (int i = 0; i < R.nattr && i < 16; i++) if (R.vattr[i] & 0xff) { int st = (int)((R.vattr[i] >> 8) & 31); if (st < 16) mask |= 1u << st; }
        for (int st = 0; st < 16 && lp < 300; st++) {
            if (!(mask >> st & 1) || !R.vbuf[st].addr) continue;
            uint64_t bytes = R.vstream[st].divisor ? R.vbuf[st].size : (uint64_t)R.vstream[st].stride * (uint64_t)(mv > 0 ? mv + 1 : 1);
            if (bytes > R.vbuf[st].size) bytes = R.vbuf[st].size;
            lp += snprintf(line + lp, sizeof line - lp, " s%d stride %u bytes %llu %016llx", st, R.vstream[st].stride, (unsigned long long)bytes, (unsigned long long)fnv(0xcbf29ce484222325ULL, P(R.vbuf[st].addr), (size_t)bytes));
        }
        line[lp] = 0;
        printf("nvn_vk_hash: vs #%d %016llx fs #%d %016llx spv %016llx %016llx count %u inst %u index type %u list %016llx highest %lld:%s\n", vs, (unsigned long long)stage_code_hash(R.prog[0], 0), fs, (unsigned long long)stage_code_hash(R.prog[1], 1),
               R.prog[0] && R.prog[0]->st[0].vk ? (unsigned long long)((VStage *)R.prog[0]->st[0].vk)->spv : 0ull, R.prog[1] && R.prog[1]->st[1].vk ? (unsigned long long)((VStage *)R.prog[1]->st[1].vk)->spv : 0ull,
               count, inst, t, (unsigned long long)(indices ? fnv(0xcbf29ce484222325ULL, P(indices), (size_t)count << t) : 0), (long long)mv, line);
    }
    if (g_dq_on < 0) {
        g_dq_on = D.f2.features.pipelineStatisticsQuery && D.f12.hostQueryReset && !gs_dbg_mode();
        printf("nvn_vk_tri: pipeline statistics %s\n", g_dq_on ? "available" : "not available on this GPU");
        if (g_dq_on) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = DQ_MAX;
            qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
            VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_dq_pool));
            vkResetQueryPool(V.device, g_dq_pool, 0, DQ_MAX);
        }
    }
    if (!g_dq_on || g_dq_n >= DQ_MAX || g_dq_rounds >= 8) return;      /* a line per draw: the log stays a size people can send */
    g_dq[g_dq_n].vs = vs; g_dq[g_dq_n].fs = fs; g_dq[g_dq_n].count = count; g_dq[g_dq_n].inst = inst;
    vkCmdBeginQuery(R.cmd, g_dq_pool, (uint32_t)g_dq_n, 0);
    g_dq_open = 1;
}
static void dq_end(void) { if (g_dq_open) { vkCmdEndQuery(R.cmd, g_dq_pool, (uint32_t)g_dq_n++); g_dq_open = 0; } }
static void dq_report(void) {                      /* diag_present, every pass of the frame ended */
    if (!g_dq_n) return;
    static uint64_t res[DQ_MAX][4];
    pass_end(); cmd_begin(); cmd_wait_all();
    VKC(vkGetQueryPoolResults(V.device, g_dq_pool, 0, (uint32_t)g_dq_n, sizeof res, res, sizeof res[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    vkResetQueryPool(V.device, g_dq_pool, 0, (uint32_t)g_dq_n);
    for (int i = 0; i < g_dq_n; i++)
        printf("nvn_vk_tri: vs #%d fs #%d count %u inst %u: triangles in %llu, to clipping %llu, out of clipping %llu, fragments %llu\n", g_dq[i].vs, g_dq[i].fs, g_dq[i].count, g_dq[i].inst,
               (unsigned long long)res[i][0], (unsigned long long)res[i][1], (unsigned long long)res[i][2], (unsigned long long)res[i][3]);
    g_dq_n = 0; g_dq_rounds++;
}
/* The two kinds of draw (nvnCommandBufferDrawArrays* / DrawElements*, and each draw of an indirect one). */
static int64_t g_indirect_draws, g_indirect_disp;
static void vk_draw_quads(uint32_t first, uint32_t count, unsigned type, uint64_t indices, int32_t base_vertex, uint32_t base_inst, uint32_t inst) {
    unsigned t = type < 3 ? type : 1;
    uint32_t n;
    if (count < 4) { R.frame_skipped++; nvn_front_count(1, 1); return; }
    if (indices && g_cb_live && cb_overlap(indices, (uint64_t)count << t)) cb_sync();      /* the indices are read from guest memory: compute results there first */
    uint64_t off = quad_indices(indices, t, count, &n);
    vtx_switches();
    if (indices && g_wlog_n && wlog_overlap(0, indices, indices + ((uint64_t)count << t))) GEN_BUMP(GEN_INDEXW);      /* index_max's cache knows generations only */
    if (indices) { int64_t mv = (int64_t)index_max(indices, (int)count, t) + base_vertex; vtx_need(mv > 0 ? (uint64_t)mv : 0, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 1); }
    else vtx_need((uint64_t)first + count - 1, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 0);
    { int64_t t0 = tk(); int ok = draw_prepare(4 /* NVN_DRAW_PRIMITIVE_TRIANGLES */); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); return; } }
    vkCmdBindIndexBuffer(R.cmd, R.ring, off, VK_INDEX_TYPE_UINT32);
    if (!eq_begin(n)) occ_draw();
    dq_begin(n, inst, indices, t, base_vertex, first);
    vkCmdDrawIndexed(R.cmd, n, inst, 0, indices ? base_vertex : (int32_t)first, base_inst);
    dq_end();
    eq_end();
    gw_after_draw();
    memset(g_vneed, 0, sizeof g_vneed);
    R.frame_draws++; nvn_front_count(0, 1);
}
static void vk_draw_arrays(unsigned prim, uint32_t first, uint32_t count, uint32_t base_inst, uint32_t inst) {
    if (prim == 7) { vk_draw_quads(first, count, 0, 0, 0, base_inst, inst); return; }
    int gsr = R.prog[2] && g_gs_draws % 20 == 0 && g_gs_draws <= 400 && (gs_dbg_mode() == 1 || gs_dbg_mode() == 3);      /* SMO_VK_GS_DEBUG: gs_dbg_read */
    if (gsr) gs_dbg_read(0);
    vtx_switches();
    if (count) vtx_need((uint64_t)first + count - 1, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 0); else memset(g_vneed, 0, sizeof g_vneed);
    { int64_t t0 = tk(); int ok = draw_prepare(prim); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); return; } }
    if (!eq_begin(count)) occ_draw();
    dq_begin(count, inst, 0, 0, 0, first);
    if (R.prog[2]) gsq_begin();
    vkCmdDraw(R.cmd, count, inst, first, base_inst);
    gsq_end();
    dq_end();
    eq_end();
    gw_after_draw();
    if (gsr) gs_dbg_read(1);
    memset(g_vneed, 0, sizeof g_vneed);
    R.frame_draws++; nvn_front_count(0, 1);
}
static void vk_draw_elements(unsigned prim, unsigned type, int count, uint64_t indices, int32_t base_vertex, uint32_t base_inst, uint32_t inst) {
    unsigned t = type < 3 ? type : 1;
    if (count <= 0 || !indices) { R.frame_skipped++; nvn_front_count(1, 1); return; }
    if (prim == 7) { vk_draw_quads(0, (uint32_t)count, type, indices, base_vertex, base_inst, inst); return; }
    if (t == 0 && !D.has_iu8) { nvn_vk_missing("8-bit indices on a GPU without VK_EXT_index_type_uint8"); return; }
    BufRef ib;
    if (g_cb_live && cb_overlap(indices, (uint64_t)count << t)) cb_sync();      /* index_max reads the indices from guest memory: compute results there first */
    buf_ref(indices, (uint64_t)count << t, (uint64_t)1 << t, &ib);    /* before the draw is prepared: a ring wrap submits */
    vtx_switches();
    if (g_wlog_n && wlog_overlap(0, indices, indices + ((uint64_t)count << t))) GEN_BUMP(GEN_INDEXW);      /* index_max's cache knows generations only */
    {   int64_t mv = (int64_t)index_max(indices, count, t) + base_vertex;
        vtx_need(mv > 0 ? (uint64_t)mv : 0, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 1); }
    { int64_t t0 = tk(); int ok = draw_prepare(prim); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); return; } }
    vkCmdBindIndexBuffer(R.cmd, ib.buf, ib.off, t == 0 ? VK_INDEX_TYPE_UINT8_EXT : t == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (!eq_begin((uint32_t)count)) occ_draw();
    dq_begin((uint32_t)count, inst, indices, t, base_vertex, 0);
    vkCmdDrawIndexed(R.cmd, (uint32_t)count, inst, 0, base_vertex, base_inst);
    dq_end();
    eq_end();
    gw_after_draw();
    memset(g_vneed, 0, sizeof g_vneed);
    R.frame_draws++; nvn_front_count(0, 1);
}
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    switch (m->op) {
    case OP_NOP: case OP_DEBUG: case OP_CONSDILATE: case OP_SUBPIXEL: break;
    case OP_DISCARD:                               /* the game no longer needs an attachment's contents (a[0]: colour index, ~0 depth-stencil); counted with SMO_VK_PASSTIME */
        if (g_pt_on) { if (R.in_pass && g_pt_n < PT_MAX) g_pt[g_pt_n].disc |= a[0] == ~0ull ? 0x100u : 1u << (a[0] & 7); else g_pt_dout++; }
        break;
    case OP_TEXPOOL: R.tpool = P(a[0]); break;
    case OP_SMPPOOL: R.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); R.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? R.ubo[stage] : R.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            unsigned u = (unsigned)(first + i);
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            Tex *t;
            View v;
            SamplerParams sp;
            memcpy(&h, blob + i * 8, 8);
            R.nvn_tex[stage][u] = h;
            if (nvn_front_handle(R.tpool, R.spool, h, &t, &v, &sp) & 1) {      /* what TXQ reports, and whether the shader must use an integer sampler (resolve_handle's rules) */
                int glevels = nvn_front_tex_levels(t), l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : glevels - l0, is3d = t->b.target == 2;
                int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : nvn_front_tex_layers(t), nf = (v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
                dims[0] = (uint32_t)lvl_(t->b.width, l0); dims[1] = (uint32_t)lvl_(t->b.height, l0);
                dims[2] = is3d ? (uint32_t)lvl_(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
                dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
                if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) isint = k == 3 || k == 8 || k == 11 ? 2 : 1;
                if (nf == 0x3E) isint = 1;
            }
            if (memcmp(&R.c2[stage][48 + 2 * u], dims, 16)) { memcpy(&R.c2[stage][48 + 2 * u], dims, 16); g_c2r[stage].valid = 0; }
            R.tex_int[stage] = (R.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            R.tex_sint[stage] = (R.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        break;
    }
    case OP_VBUF: for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&R.vbuf[a[0] + i], blob + i * 16, 16); break;
    case OP_VATTR: R.nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(R.vattr, blob, (size_t)R.nattr * 4); break;
    case OP_VSTREAM: for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&R.vstream[i], blob + i * 8, 8); break;
    case OP_BLEND: { const uint8_t *b = (const uint8_t *)&a[0]; if (b[7]) nvn_vk_missing("an advanced blend equation (nvnBlendStateSetAdvancedMode)"); memcpy(R.blend[b[0] & 7], b + 1, 6); R.dyn3_dirty = 1; break; }
    case OP_COLOR: { const uint8_t *b = (const uint8_t *)&a[0]; R.blend_on = b[0]; R.logic = b[1]; R.alpha_func = b[2]; R.dyn3_dirty = 1; if (b[1] != 3 && !D.f2.features.logicOp) nvn_vk_missing("a logic op on a GPU without logicOp"); break; }
    case OP_CHANMASK: R.chanmask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_MULTISAMPLE: { const uint8_t *b = (const uint8_t *)&a[0]; memcpy(R.ms, b, 24); R.dyn3_dirty = 1; if (b[3] || b[5]) nvn_vk_missing("a multisample state with coverage-to-colour, coverage modulation, sample locations or raster samples"); break; }
    case OP_POLYGON: memcpy(R.poly, &a[0], 4); R.dyn_dirty = 1; R.dyn3_dirty = 1; break;
    case OP_DEPTHSTENCIL: R.ds = a[0]; R.dyn_dirty = 1; break;
    case OP_VIEWPORTS: if (a[0] == 0 && a[1] > 0) { memcpy(R.viewport, blob, 16); R.have_viewport = 1; R.dyn_dirty = 1; } break;
    case OP_SCISSORS: if (a[0] == 0 && a[1] > 0) { memcpy(R.scissor, blob, 16); if (R.scissor[2] < 0) R.scissor[2] = 0; if (R.scissor[3] < 0) R.scissor[3] = 0; R.have_scissor = 1; R.dyn_dirty = 1; } break;
    case OP_DEPTHRANGES: if (a[0] == 0 && a[1] > 0) { memcpy(R.drange, blob, 8); R.dyn_dirty = 1; } break;
    case OP_TARGETS: {
        const TargetRef *col = (const TargetRef *)blob, *dep = col + 8;
        int n = (int)a[0] < 8 ? (int)a[0] : 8;
        {   /* Android port: the same targets again (the game sets them before each group of draws: 61 of the 73 passes a frame
             * of the opening cutscene ended here, measured on a Galaxy S24 Ultra) keep the pass; a pass costs the CPU in the
             * driver and, on a tiled mobile GPU, a load and a store of every attachment */
            TargetRef nc[8], nd;
            int nn = 0;
            memset(nc, 0, sizeof nc); memset(&nd, 0, sizeof nd);
            for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { nc[i] = col[i]; nn = i + 1; }
            if (dep->tex && !dep->tex->dead) nd = *dep;
            if (R.in_pass && nn == R.ncol && !memcmp(nc, R.col, sizeof nc) && !memcmp(&nd, &R.depth, sizeof nd)) { g_targets_kept++; break; }
        }
        pass_end();
        submit_chunk();                            /* Android port: the GPU starts on what is recorded so far */
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) ((VTex *)R.col[i].tex->vk)->feedback = 0;
        if (R.depth.tex && R.depth.tex->vk) ((VTex *)R.depth.tex->vk)->feedback = 0;
        memset(R.col, 0, sizeof R.col); memset(&R.depth, 0, sizeof R.depth);
        R.ncol = 0; R.have_targets = 0;
        for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { R.col[i] = col[i]; R.ncol = i + 1; R.have_targets = 1; }
        if (dep->tex && !dep->tex->dead) { R.depth = *dep; R.have_targets = 1; }
        R.dyn_dirty = 1;                           /* default viewport / scissor follow the target size */
        break;
    }
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? R.sref : m->op == OP_STENCIL_VMASK ? R.svmask : R.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        R.dyn_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: memcpy(R.blendc, blob, 16); R.dyn_dirty = 1; break;
    case OP_ALPHAREF: memcpy(&R.alpha_ref, &a[0], 4); break;
    case OP_POINTSIZE: memcpy(&R.point_size, &a[0], 4); break;
    case OP_LINEWIDTH: memcpy(&R.line_width, &a[0], 4); R.dyn_dirty = 1; break;
    case OP_POLYOFFSET: memcpy(R.polyoff, &a[0], 12); R.dyn_dirty = 1; break;
    case OP_SAMPLEMASK: R.sample_mask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_RASTDISCARD: R.rast_discard = a[0] & 1; R.dyn_dirty = 1; break;
    case OP_DEPTHCLAMP: R.depth_clamp = a[0] & 1; R.dyn3_dirty = 1; if ((a[0] & 1) && !D.f2.features.depthClamp) nvn_vk_missing("depth clamp on a GPU without depthClamp"); break;
    case OP_PRIMRESTART: R.restart = a[0] & 1; break;
    case OP_PATCHSIZE: break;                      /* only read by tessellation, which stops at the draw */
    case OP_IMAGES: { int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2]; for (int i = 0; i < count && first + i < 8; i++) memcpy(&R.nvn_img[stage][first + i], blob + i * 8, 8); break; }
    case OP_FENCE: if (a[0]) { SyncObj *o = P(a[0]); if (!o->dead) o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); } break;
    case OP_WAITSYNC: break;                       /* one queue: what was submitted before has run before */
    /* SMO port: reports, as nvn_gl.c's replay writes them (NVNcounterData {counter, timestamp in ns}), after the work
     * recorded before them has run (cb_sync). Counter type 0 carries only the timestamp; types measured by the GPU
     * (samples passed, primitives, shader invocations: Vulkan queries) are not implemented yet and stop with the type. */
    case OP_REPORT_VALUE: if (a[1]) report_write(a[1], (uint32_t)a[0], 0); break;
    /* NVN counter types: 0 TIMESTAMP (bottom of pipe: after earlier work), 16 TIMESTAMP_TOP (top of pipe: when reached,
     * no wait); 1..15 are measured by the GPU (samples passed ... ZCULL stats), as nvn_gl.c's counter_target lists */
    case OP_COUNTER_RESET: if (a[0] == 1) { occ_reset(); break; } if (a[0] != 0 && a[0] != 16) { static char msg[80]; snprintf(msg, sizeof msg, "nvnCommandBufferResetCounter of counter type %u (a GPU query)", (unsigned)a[0]); nvn_vk_missing(msg); } break;
    case OP_COUNTER_REPORT:
        if (a[0] == 1) { occ_report(a[1]); break; }
        if (a[0] >= 2 && a[0] <= 15) {             /* SMO: a pipeline statistics counter (vertices, invocations, primitives, zcull; profiling): reported as 0 with the time */
            static uint32_t said;
            if (!(said >> a[0] & 1)) { said |= 1u << a[0]; printf("nvn_vk: nvnCommandBufferReportCounter of counter type %u (pipeline statistics): reported as 0 (not counted)\n", (unsigned)a[0]); }
            if (a[1]) { uint64_t d[2] = {0, counter_ticks(vk_now_ns())}; memcpy(P(a[1]), d, 16); host_wrote(a[1], 16, GEN_REPORT); }
            break;
        }
        if (a[0] != 0 && a[0] != 16) { static char msg[80]; snprintf(msg, sizeof msg, "nvnCommandBufferReportCounter of counter type %u (a GPU query)", (unsigned)a[0]); nvn_vk_missing(msg); break; }
        if (a[1]) { if (a[0] == 0) { report_write(a[1], 0, 1); cq_note(a[1]); } else rq_write(a[1], 0, 1); }      /* 16: top of pipe, when reached: no wait */
        break;
    case OP_CONSRASTER: if (a[0] & 1) nvn_vk_missing("conservative rasterisation (nvnCommandBufferSetConservativeRasterEnable)"); break;
    case OP_DEPTHBOUNDS: if (a[0] & 1) nvn_vk_missing("the depth bounds test (nvnCommandBufferSetDepthBounds)"); break;
    case OP_RENDER_ENABLE:                         /* a1 = 0: a0 is the switch; otherwise mode + 1 (0 render if equal, 1 if not) and a2 the address of
                                                    * two counter reports (16 bytes each). SMO port: compared on the CPU as nvn_gl.c does, once the
                                                    * reports queued for those addresses are in guest memory (waited for here when they are not) */
        if (!a[1]) R.render_off = !(a[0] & 1);
        else {
            int need = 0;
            for (int i = 0; i < g_occ_qn; i++) { const OccEntry *e = &g_occ_q[(g_occ_qh + i) % OCC_Q]; if (e->kind == 0 && e->dst < a[2] + 32 && e->dst + 16 > a[2]) need = i + 1; }
            g_occ_cond++;
            /* Android port, CONDITIONAL RENDERS ONE FRAME LATE: SMO compares occlusion counts it reported earlier in the same
             * frame, and waiting for them stopped the render thread until the GPU had caught up, about 5 times a frame:
             * 7 ms a frame in Cap Kingdom on a Galaxy S24 Ultra (SMO_VK_WAITWHO), with the GPU idle meanwhile. Now, when the
             * reports are not in yet, the counts in guest memory are compared: those of the frame before, as long as neither
             * of the two reports has been written in this frame; when only one has, the last decision for this address is
             * used (render, if there is none). SMO_VK_COND_WAIT=1: wait for them, as before. */
            static int cwait = -1;
            if (cwait < 0) {
                const char *e = getenv("SMO_VK_COND_WAIT");
#ifdef __ANDROID__
                cwait = e ? atoi(e) != 0 : 0;
#else
                cwait = e ? atoi(e) != 0 : 1;
#endif
            }
            if (need && !cwait) {
                occ_resolve(0);                    /* what has finished meanwhile */
                int pend0 = occ_queued_at(a[2]), pend1 = occ_queued_at(a[2] + 16), k = cond_slot(a[2], 1);
                if (pend0 != pend1) {              /* one count of this frame, one of the frame before: not comparable */
                    R.render_off = k >= 0 && g_cond[k].valid && g_cond[k].mode == (int)a[1] ? g_cond[k].off : 0;
                    if (k >= 0) { g_cond[k].pending = 1; g_cond[k].mode = (int)a[1]; }
                    g_occ_cond_kept++;
                } else {                           /* both of this frame (finished meanwhile) or both of the frame before */
                    if (cb_overlap(a[2], 32)) cb_sync();
                    int eq = rd64(a[2]) == rd64(a[2] + 16); R.render_off = a[1] == 1 ? !eq : eq;
                    if (pend0) g_occ_cond_late++;
                    if (k >= 0) { g_cond[k].mode = (int)a[1]; cond_decide(k); }
                }
                g_fr_cond++; g_fr_condoff += R.render_off; g_fr_condhash = (g_fr_condhash ^ (a[2] * 2 + (uint64_t)R.render_off)) * 1099511628211ull;
                break;
            }
            if (need) {
                static int logon = -1, nlog;
                if (logon < 0) logon = getenv("SMO_VK_OCC_LOG") && atoi(getenv("SMO_VK_OCC_LOG"));
                if (logon && nlog++ < 200) {
                    const OccEntry *e = &g_occ_q[(g_occ_qh + need - 1) % OCC_Q];
                    printf("nvn_vk_occ: frame %lld: conditional render on %llx waits for a report queued in frame %lld (entry %d of %d queued, segments %llu..%llu, %s)\n",
                           nvn_front_frame_next(), (unsigned long long)a[2], (long long)e->frame, need, g_occ_qn, (unsigned long long)e->lo, (unsigned long long)e->hi,
                           e->serial > R.serial ? "in the command buffer being recorded" : "submitted");
                }
                g_occ_cond_waits++;
                {   /* Android port, measurement (SMO_VK_CONDSHAPE=1): segments between the two reports, entries of other kinds between them */
                    static int on = -1; static int64_t hn[6], inpass, others, a_gone, n, a_first; static long long lastf = -1;
                    if (on < 0) on = getenv("SMO_VK_CONDSHAPE") && atoi(getenv("SMO_VK_CONDSHAPE"));
                    if (on) {
                        int ia = -1, ib = -1;
                        for (int i = 0; i < g_occ_qn; i++) { const OccEntry *e = &g_occ_q[(g_occ_qh + i) % OCC_Q]; if (e->kind == 0 && e->dst == a[2]) ia = i; if (e->kind == 0 && e->dst == a[2] + 16) ib = i; }
                        uint64_t seg = 0; int oth = 0;
                        for (int i = ia + 1; i <= ib; i++) { const OccEntry *e = &g_occ_q[(g_occ_qh + i) % OCC_Q]; seg += e->hi - e->lo; if (e->kind != 0) oth++; }
                        hn[seg == 0 ? 0 : seg == 1 ? 1 : seg == 2 ? 2 : seg <= 4 ? 3 : seg <= 8 ? 4 : 5]++;
                        inpass += R.in_pass; others += oth > 0; a_gone += ia < 0; a_first += ia >= 0 && ib >= 0 && ia < ib; n++;
                        long long f = nvn_front_frame_next();
                        if (f % 300 == 0 && f != lastf && n) {
                            lastf = f;
                            printf("nvn_vk_condshape: %lld waits: segments between the reports 0:%lld 1:%lld 2:%lld 3-4:%lld 5-8:%lld 9+:%lld; inside a pass %lld; other entries between %lld; first report already written %lld; both queued in order %lld\n",
                                   (long long)n, (long long)hn[0], (long long)hn[1], (long long)hn[2], (long long)hn[3], (long long)hn[4], (long long)hn[5], (long long)inpass, (long long)others, (long long)a_gone, (long long)a_first);
                            memset(hn, 0, sizeof hn); inpass = others = a_gone = n = a_first = 0;
                        }
                    }
                }
                occ_resolve(need);
                occ_publish(UINT64_MAX);           /* compared now: written now */
            }
            cb_sync(); int eq = a[2] && rd64(a[2]) == rd64(a[2] + 16); R.render_off = a[1] == 1 ? !eq : eq;
            g_fr_cond++; g_fr_condoff += R.render_off; g_fr_condhash = (g_fr_condhash ^ (a[2] * 2 + (uint64_t)R.render_off)) * 1099511628211ull;
        }
        break;
    case OP_ROWSTRIDE: R.row_stride = (int64_t)a[0]; break;
    case OP_IMAGESTRIDE: R.image_stride = (int64_t)a[0]; break;
    case OP_BARRIER:                               /* NVNbarrierBits: 2 / 4 order fragments, 8 indirect data, 0x10 texture memory changed */
        if (a[0] & 0x10) nvn_front_barrier_texmem();
        if (a[0] & 6) tex_barrier();
        break;
    case OP_DISPATCH: { int64_t t0 = tk(); compute_dispatch((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); g_tm[2] += tk() - t0; break; }
    case OP_COPY_BUF_BUF: cb_sync(); if (a[0] && a[1]) { memmove(P(a[1]), P(a[0]), (size_t)a[2]); host_wrote(a[1], a[2], GEN_BUF); } break;      /* guest memory is read / written: what compute wrote must be there (as OpenGL's compute_flush) */
    case OP_UPDATE_UBO:                            /* only compute results in the range written must be in guest memory first (it used to be all of them: a wait for the GPU at the first update after a dispatch) */
        if (g_cb_pending || (g_cb_live && cb_overlap(a[0] + a[2], a[3]))) cb_sync();
        if (a[0]) { memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); host_wrote(a[0] + a[2], a[3], GEN_UBO); }
        break;
    case OP_CLEAR_BUF: cb_sync(); if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); host_wrote(a[0], a[1], GEN_BUF); } break;
    case OP_CLEARCOLOR: {
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2] & 15;
        if (!R.have_targets || R.render_off || (int)i >= R.ncol || !R.col[i].tex) break;
        if (!R.in_pass && loadclear_on() && !R.col[i].tex->dead && (mask & 15) == 15) {      /* a whole clear before the pass: a load-clear */
            Tex *t = R.col[i].tex;
            int lv = target_level(&R.col[i]), gw = lvl_(t->b.width > 0 ? t->b.width : 1, lv), gh = lvl_(t->b.height > 0 ? t->b.height : 1, lv);
            if (!R.have_scissor || (R.scissor[0] <= 0 && R.scissor[1] <= 0 && R.scissor[0] + R.scissor[2] >= gw && R.scissor[1] + R.scissor[3] >= gh)) {
                memcpy(&g_lc_col[i], blob, 16); g_lc_mask |= 1u << i;
                R.frame_clears++; nvn_front_count(2, 1);
                break;
            }
        }
        if (!pass_begin()) break;
        if (mask != 15) {                          /* a partial channel mask only matters for channels the format has */
            Fmt f; int comps = 4, nf = R.col[i].tex->b.format;
            if (nf >= 1 && nf <= 0x30) comps = (nf - 1) / 12 + 1;
            if (!nvn_front_fmt(nf, &f) || (mask & ((1u << comps) - 1)) != (1u << comps) - 1) { if (mask) nvn_vk_missing("ClearColor with a partial channel mask"); break; }
        }
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = i; memcpy(&ca.clearValue.color, blob, 16);
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_CLEARDS: {
        if (!R.have_targets || R.render_off || !R.depth.tex || !((a[0] & 1) || (uint32_t)a[2])) break;
        if (!R.in_pass && loadclear_on() && !R.depth.tex->dead && (!(uint32_t)a[2] || ((uint32_t)a[2] & 0xff) == 0xff)) {
            Tex *t = R.depth.tex;
            int lv = target_level(&R.depth), gw = lvl_(t->b.width > 0 ? t->b.width : 1, lv), gh = lvl_(t->b.height > 0 ? t->b.height : 1, lv);
            if (!R.have_scissor || (R.scissor[0] <= 0 && R.scissor[1] <= 0 && R.scissor[0] + R.scissor[2] >= gw && R.scissor[1] + R.scissor[3] >= gh)) {
                if (a[0] & 1) { memcpy(&g_lc_ds.depth, &a[3], 4); g_lc_mask |= 0x100u; }
                if ((uint32_t)a[2]) { g_lc_ds.stencil = (uint32_t)a[1]; g_lc_mask |= 0x200u; }
                VTex *dv = vt_get(t);                  /* a format without stencil: its stencil "clear" is nothing to wait for */
                if (dv && !(dv->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (g_lc_mask & 0x100u)) g_lc_mask |= 0x200u;
                if (dv && !(dv->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (g_lc_mask & 0x200u)) g_lc_mask |= 0x100u;
                if ((g_lc_mask & 0x300u) == 0x300u || !dv) { R.frame_clears++; nvn_front_count(2, 1); break; }
                g_lc_mask &= ~0x300u;                  /* depth without the stencil of a depth-stencil format: recorded as before */
            }
        }
        if (!pass_begin()) break;
        VTex *vt = R.depth.tex->vk;
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0 || !vt) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        {   static int want = -2; want = passshot_frame() ? 1 : -1;
            if (want >= 0) { float dv; memcpy(&dv, &a[3], 4); printf("nvn_vk_ds: clear depth %d (value %g) stencil mask %x value %u on depth tex %p fmt 0x%x %dx%d\n", (int)(a[0] & 1), dv, (unsigned)a[2], (unsigned)a[1], (void *)R.depth.tex, R.depth.tex->b.format, R.depth.tex->b.width, R.depth.tex->b.height); } }
        ca.aspectMask = ((a[0] & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((uint32_t)a[2] ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if ((ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && ((uint32_t)a[2] & 0xff) != 0xff) {
            stencil_clear_masked((uint32_t)a[2] & 0xff, (uint32_t)a[1], sx, sy, sw, sh);
            ca.aspectMask &= ~(VkFlags)VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        ca.aspectMask &= vt->aspect;
        if (!ca.aspectMask) break;
        memcpy(&ca.clearValue.depthStencil.depth, &a[3], 4); ca.clearValue.depthStencil.stencil = (uint32_t)a[1];
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_DRAW_ARRAYS: vk_draw_arrays((unsigned)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]); break;
    case OP_DRAW_ELEMENTS: vk_draw_elements((unsigned)a[0], (unsigned)a[1], (int)a[2], a[3], (int32_t)a[4], (uint32_t)a[5], (uint32_t)a[6]); break;
    case OP_DRAW_INDIRECT: {                       /* SMO: a0 mode, a1 index type, a2 index buffer, a3 indirect data, a4 draws (or the maximum), a5 stride, a6 bit 0 elements, bit 1 the count read from memory (blob: its address) */
        /* as the OpenGL path: the parameters are read from guest memory (a dispatch that wrote them is copied back first) and each
         * becomes an ordinary draw; the draw path needs the counts on this side anyway (the vertices a draw reads, vtx_need) */
        unsigned n = (unsigned)a[4], stride = (unsigned)a[5];
        int elements = (int)(a[6] & 1);
        if (!stride) stride = elements ? 20 : 16;
        if (a[6] & 2) { uint64_t param; memcpy(&param, blob, 8); if (param && g_cb_live && cb_overlap(param, 4)) cb_sync(); uint32_t cnt = 0; if (param) memcpy(&cnt, P(param), 4); if (cnt < n) n = cnt; }
        if (!a[3] || !n) break;
        if (g_cb_live && cb_overlap(a[3], (uint64_t)n * stride)) cb_sync();
        for (unsigned k = 0; k < n; k++) {
            uint32_t d[5];                         /* arrays: count, instances, first, base instance; elements: count, instances, first index, base vertex, base instance */
            unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
            memcpy(d, P(a[3] + (uint64_t)k * stride), elements ? 20 : 16);
            if (!d[0] || !d[1]) continue;
            if (elements) vk_draw_elements((unsigned)a[0], t, (int)d[0], a[2] ? a[2] + ((uint64_t)d[2] << t) : 0, (int32_t)d[3], d[4], d[1]);
            else vk_draw_arrays((unsigned)a[0], d[2], d[0], d[3], d[1]);
            g_indirect_draws++;
        }
        break;
    }
    case OP_DISPATCH_INDIRECT: {                   /* SMO: a0 the address of three group counts (read as the OpenGL path does) */
        uint32_t g[3] = {0, 0, 0};
        if (a[0]) { if (g_cb_live && cb_overlap(a[0], 12)) cb_sync(); memcpy(g, P(a[0]), 12); }
        int64_t t0 = tk(); compute_dispatch(g[0], g[1], g[2]); g_tm[2] += tk() - t0; g_indirect_disp++;
        break;
    }
    case OP_COPY_BUF_TEX: cb_sync(); copy_buf_tex(m); break;
    case OP_COPY_TEX_BUF: {                        /* a0 texture, a3 buffer address; blob: view, region (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        if (!t || t->dead || !a[3]) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        cb_sync();
        read_region(t, &v, r, P(a[3]), R.row_stride, R.image_stride);
        GEN_BUMP(GEN_TEXBUF);                      /* guest memory changed: ranges are streamed again */
        break;
    }
    case OP_COPY_TEX_TEX: {
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(sr, blob + 24, 24); memcpy(&dv, blob + 48, sizeof dv); memcpy(dr, blob + 72, 24);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1));
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_CLEARTEX: {                            /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        Fmt f;
        if (!t || t->dead) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        int whole = a[6] || (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] >= lvl_(t->b.width, level) && r[4] >= lvl_(t->b.height, level));
        pass_end();
        VTex *vt = vt_get(t);
        int comps = t->b.format >= 1 && t->b.format <= 0x30 ? (t->b.format - 1) / 12 + 1 : 4;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !nvn_front_fmt(t->b.format, &f) || f.kind == 4 || vt->decode || vt->expand) { nvn_vk_missing("nvnCommandBufferClearTexture of a depth, compressed or 3-component texture"); break; }
        if (!whole || ((unsigned)a[4] & ((1u << comps) - 1)) != (1u << comps) - 1) { nvn_vk_missing("nvnCommandBufferClearTexture of a part of a level or with a partial channel mask"); break; }
        cmd_begin();
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, (uint32_t)(vt->is3d ? 0 : layer), 1};
        VkClearColorValue cv;
        memcpy(&cv, blob + 48, 16);
        vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
        t->gpu_written = 1; t->uploaded = 1;
        break;
    }
    case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); } break;
    default: { static char msg[OP_COUNT][64]; snprintf(msg[m->op % OP_COUNT], 64, "nvnCommandBuffer%s", nvn_front_op_name(m->op)); nvn_vk_missing(msg[m->op % OP_COUNT]); break; }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) { static int n; const char *nvn_front_list_why(uint64_t h); void nvn_front_list_stale(void); nvn_front_list_stale(); if (n++ < 10) printf("nvn_vk: command handle %llx is unknown or expired: %s\n", (unsigned long long)handle, nvn_front_list_why(handle)); return;}
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- the RBackend members */
static void prewarm_wait(void);
/* Android port, THE LOOK-AHEAD ON A THREAD OF ITS OWN: reading a batch for the pipelines it will need (scan_list) was 10%
 * of the render thread on a Galaxy S24 Ultra, done before the replay. It runs beside the replay now (SMO_VK_SCAN_THREAD,
 * default 1 on Android): it starts from the state at the batch's start (scan_begin, on the render thread), reads only the
 * batch's lists (alive until the batch's replay has finished: the render thread waits for the scan before it goes on)
 * and the stage and pipeline tables (under g_sh_lock, as the workers do). It stays ahead of the replay, which is about
 * ten times slower per draw. */
static plat_event *g_scan_go, *g_scan_done;
static struct { int count; const uint64_t *handles; Scan s; } g_scan_job;
static uint32_t scan_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_event_wait(g_scan_go, PLAT_INFINITE);
        int64_t ts = tk();
        for (int i = 0; i < g_scan_job.count; i++) scan_list(g_scan_job.handles[i], 0, &g_scan_job.s);
        g_tm[9] += tk() - ts;
        plat_event_set(g_scan_done);
    }
    return 0;
}
static int scan_thread_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SMO_VK_SCAN_THREAD");
#ifdef __ANDROID__
        on = e ? atoi(e) != 0 : 1;
#else
        on = e ? atoi(e) != 0 : 0;
#endif
        if (on) {
            g_scan_go = plat_event_create(0, 0); g_scan_done = plat_event_create(0, 0);
            if (!g_scan_go || !g_scan_done) on = 0;
            else plat_thread_detach(plat_thread_start32(scan_thread_main, NULL, 0, "vk_scan"));
        }
        printf("nvn_vk: look-ahead of a batch: %s\n", on ? "on a thread of its own, beside the replay (SMO_VK_SCAN_THREAD=0: before it)" : "before the replay, on the render thread");
    }
    return on;
}
static void vk_submit(void *arg) {
    SubmitJob *j = arg;
    if (!V.alive) return;                          /* after the shutdown at exit: the device is gone */
    prewarm_wait();
    int64_t t0 = tk();
    {   /* Android port, SMO_VK_CB_LAZY=1: the last batch's compute results land when the GPU has them (cb_poll, no wait), as on
         * the console, where the GPU runs behind the CPU; a read of those addresses by a draw, a copy or an index scan still
         * waits for them (cb_overlap). Measured on a Galaxy S24 Ultra in the opening cutscene: the wait here drained the GPU,
         * ~50 ms every 4 frames. */
        static int lazy = -1;
        if (lazy < 0) { const char *e = getenv("SMO_VK_CB_LAZY"); lazy = e ? atoi(e) != 0 : rq_on(); }      /* with deferred reports the wait would only move here */
        if (lazy) cb_poll(); else cb_sync();       /* what the last batch's dispatches wrote is in guest memory before this batch reads it */
    }
    if (g_occ_qn) occ_resolve(0);                  /* occlusion counts whose submissions have finished */
    int64_t t1 = tk();
    nvn_front_batch_begin();
    GEN_BUMP(GEN_BATCH);                           /* guest memory may have changed since the last batch */
    g_batch_no++;
    int scan_async = 0;
    if (g_scan_on && scan_thread_on() && j->count) {
        scan_begin(&g_scan_job.s); g_scan_job.count = j->count; g_scan_job.handles = j->handles;
        plat_event_set(g_scan_go); scan_async = 1;
    } else
    if (g_scan_on) { int64_t ts = tk(); Scan s; scan_begin(&s); for (int i = 0; i < j->count; i++) scan_list(j->handles[i], 0, &s); g_tm[9] += tk() - ts; }      /* session 4: what this batch will need goes to the workers before it is replayed */
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    if (scan_async) plat_event_wait(g_scan_done, PLAT_INFINITE);      /* the lists stay alive until the scan has read them */
    cb_batch_end();
    occ_fold();
    g_tm[7] += t1 - t0; g_tm[3] += tk() - t1;
    nvn_front_count(3, 1);
}
static void vk_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); vk_submit(j); }
static void vk_idle(void) { if (V.alive) { cb_poll(); if (g_occ_qn) occ_resolve(0); nvn_front_take_dead(vk_drop); } }
static void vk_housekeeping(void *arg) { (void)arg; if (V.alive) nvn_front_take_dead(vk_drop); }
static void vk_flush(void *arg) { (void)arg; if (V.alive) cmd_submit(NULL, NULL); }
static void vk_finish(void *arg) { (void)arg; if (V.alive) { cmd_wait_all(); if (g_occ_qn) occ_resolve(0); } }
static void vk_fence(void *arg) { SyncJob *j = arg; if (j->o->dead || !V.alive) return; j->o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); }
static void vk_sync_wait_(SyncJob *j) {
    uint64_t want = (uint64_t)(uintptr_t)j->o->s;
    if (!want || !V.alive) { j->result = 3; return; }       /* never fenced */
    want--;
    if (want <= R.done) { j->result = 0; return; }
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending || s->serial != want) continue;
        VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, j->timeout);
        if (r == VK_SUCCESS) { if (s->serial > R.done) R.done = s->serial; cq_resolve(s); j->result = 1; cb_poll(); } else j->result = r == VK_TIMEOUT ? 2 : 3;
        return;
    }
    j->result = 0;                                  /* its command buffer was reused: finished long ago */
}
static void vk_sync_wait(void *arg) { vk_sync_wait_(arg); if (V.alive) cq_poll(); if (V.alive && g_occ_qn) occ_resolve(0); }      /* occlusion counts the fence covers are in guest memory when the game goes on */
static void vk_server_wait(void *arg) { SyncJob *j = arg; j->timeout = ~0ull; if (j->o->s) vk_sync_wait(j); j->result = j->o->s != NULL; }
static void vk_sync_drop(void *arg) { SyncObj *o = arg; o->s = NULL; }
static void vk_reset_status(void *arg) { *(int *)arg = 0; }       /* NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR: a lost device ends the process with its own message */
static void vk_no_grid(void *arg) { nvn_vk_missing("nvnMultisampleStateGetSampleLocationsGrid"); ((int *)arg)[0] = ((int *)arg)[1] = 1; }

/* The window texture as a 24-bit BMP, the file save_bmp of the OpenGL path writes (row 0 of the texture is the top row). */
static void save_bmp_vk(const char *path, VTex *vt) {
    VMem mem;
    int w = vt->w, h = vt->h, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (vt->rs && getenv("SMO_VK_SHOT_NATIVE") && atoi(getenv("SMO_VK_SHOT_NATIVE"))) {      /* track O: the scaled image itself, texel for texel (the default below is the game's size, filtered up as the window shows it) */
        VMem m2;
        uint64_t size = (uint64_t)w * h * 4;
        VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m2);
        VkBufferImageCopy c;
        if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2); return; }
        pass_end(); cmd_begin();
        VkImageLayout was = vt->layout;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
        cmd_wait_all();
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t head[54] = {'B', 'M'};
            uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, fsize = 54 + row * (uint32_t)h, v;
            uint8_t *rowb = calloc(1, row);
            memcpy(head + 2, &fsize, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
            fwrite(head, 1, 54, f);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *s = (const uint8_t *)m2.map + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
                fwrite(rowb, 1, row, f);
            }
            free(rowb); fclose(f);
            printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
        }
        vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2);
        return;
    }
    if (vt->rs) { w = vt->t->b.width; h = vt->t->b.height; }      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk: screenshot: the window texture's format %d is not an 8-bit RGBA one; not saved\n", (int)vt->fmt); return; }
    /* session 5: through read_region, the path of ReadTexels / CopyTextureToBuffer (so every screenshot exercises it) */
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    (void)mem;
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
    uint8_t head[54] = {'B', 'M'}, *rowb;
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    FILE *f = fopen(path, "wb");
    if (f) {
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        fwrite(head, 1, 54, f);
        rowb = calloc(1, row);
        for (int y = h - 1; y >= 0; y--) {
            const uint8_t *s = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
            fwrite(rowb, 1, row, f);
        }
        free(rowb); fclose(f);
        printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);      /* the front end's line, word for word: tests/ref/port_capture.py and the shot tools look for it */
    }
    free(px);
}
/* SMO port, debug aid SMO_VK_PASSSHOT=<frame> (or a range, passshot_frame): at the end of every render pass of that frame,
 * colour target 0 is read back (read_region, the ReadTexels path) and saved as pass_[<frame>_]<n>_<format>.bmp in SMO_SHOT_DIR, with one line of figures
 * (pixels not black, mean of each channel). RGB10A2 (0x3d) is decoded to 8 bits per channel; 8-bit RGBA as is. */
/* SMO 0.1.3: what the render targets hold on the tester's GPU. A colour problem seen on one GPU (Mali: glow, haze, blown-out
 * patches) cannot be reproduced anywhere else, so the build measures it there. Every SMO_VK_DIAG frames (default on ARM
 * GPUs; graphics.ini diagnostics = off stops it; a number over 1 is the distance in frames) one round is taken while
 * the game is drawing a real scene:
 *   E, one frame left alone: every colour target it rendered to is read at the END of the frame;
 *   P, two frames later: every colour target is read after EACH pass, and the frame's draws are logged (passshot_frame).
 * Reading after a pass flushes and waits for the GPU, so if P looks right where E looks wrong, the fault is in the order
 * of the GPU's work (a missing barrier, tile memory) rather than in a shader. For each target one nvn_vk_diag line:
 * per-channel min/max/mean of the finite values, how many texels hold NaN or infinity, and how many are over 1 and over
 * 16, plus a small picture in <files>/diag (the launcher's log export zips them; the last 6 rounds are kept). In the
 * pictures float values are shown as v/(1+v), NaN magenta, infinity cyan: one such texel marks its whole cell. */
#define DIAG_LIST 160
#define DIAG_KEEP 6
static int g_diag_on = -1, g_diag_gap = 1800, g_diag_wait, g_diag_stable, g_diag_round, g_diag_pass, g_diag_n;
static int64_t g_diag_last = -1;
static TargetRef g_diag_list[DIAG_LIST];
static char g_diag_dir[256];
static float g_diag_h[65536], g_diag_uf[2][2048];
static int g_diag_tabs;
static void diag_tables(void) {
    if (g_diag_tabs) return;
    g_diag_tabs = 1;
    for (int i = 0; i < 65536; i++) {
        int e = (i >> 10) & 31, m = i & 1023;
        float f = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf(1.0f + (float)m / 1024.0f, e - 15);
        g_diag_h[i] = i & 0x8000 ? -f : f;
    }
    for (int b = 0; b < 2; b++) {                  /* [0] 11 bits (6 mantissa), [1] 10 bits (5 mantissa) */
        int mb = b ? 5 : 6, n = 1 << (mb + 5);
        for (int i = 0; i < n; i++) {
            int e = i >> mb, m = i & ((1 << mb) - 1);
            g_diag_uf[b][i] = e == 31 ? (m ? NAN : INFINITY) : e == 0 ? ldexpf((float)m, -14 - mb) : ldexpf(1.0f + (float)m / (float)(1 << mb), e - 15);
        }
    }
}
/* the texel as floats; returns the channels (0: format not known here), *kind 0 unorm 1 snorm 2 float */
static int diag_texel(VkFormat f, const uint8_t *p, float *o, int *kind) {
    const uint16_t *h = (const uint16_t *)p; const float *q = (const float *)p; const int8_t *s8 = (const int8_t *)p; const int16_t *s16 = (const int16_t *)p;
    uint32_t u;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: *kind = 0; for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; return 4;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: *kind = 0; o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; return 4;
    case VK_FORMAT_R8_UNORM: *kind = 0; o[0] = p[0] / 255.0f; return 1;
    case VK_FORMAT_R8G8_UNORM: *kind = 0; o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; return 2;
    case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8B8A8_SNORM: {
        int n = f == VK_FORMAT_R8_SNORM ? 1 : f == VK_FORMAT_R8G8_SNORM ? 2 : 4;
        *kind = 1; for (int i = 0; i < n; i++) { float v = s8[i] / 127.0f; o[i] = v < -1 ? -1 : v; } return n; }
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM: {
        int n = f == VK_FORMAT_R16_UNORM ? 1 : f == VK_FORMAT_R16G16_UNORM ? 2 : 4;
        *kind = 0; for (int i = 0; i < n; i++) o[i] = h[i] / 65535.0f; return n; }
    case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16B16A16_SNORM: {
        int n = f == VK_FORMAT_R16_SNORM ? 1 : f == VK_FORMAT_R16G16_SNORM ? 2 : 4;
        *kind = 1; for (int i = 0; i < n; i++) { float v = s16[i] / 32767.0f; o[i] = v < -1 ? -1 : v; } return n; }
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: {
        int n = f == VK_FORMAT_R16_SFLOAT ? 1 : f == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
        *kind = 2; for (int i = 0; i < n; i++) o[i] = g_diag_h[h[i]]; return n; }
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: {
        int n = f == VK_FORMAT_R32_SFLOAT ? 1 : f == VK_FORMAT_R32G32_SFLOAT ? 2 : 4;
        *kind = 2; for (int i = 0; i < n; i++) o[i] = q[i]; return n; }
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        memcpy(&u, p, 4); *kind = 2; o[0] = g_diag_uf[0][u & 2047]; o[1] = g_diag_uf[0][(u >> 11) & 2047]; o[2] = g_diag_uf[1][u >> 22]; return 3;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        memcpy(&u, p, 4); *kind = 0; o[0] = (u & 1023) / 1023.0f; o[1] = ((u >> 10) & 1023) / 1023.0f; o[2] = ((u >> 20) & 1023) / 1023.0f; o[3] = (u >> 30) / 3.0f; return 4;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
        *kind = 0; o[0] = (h[0] >> 11) / 31.0f; o[1] = ((h[0] >> 5) & 63) / 63.0f; o[2] = (h[0] & 31) / 31.0f; return 3;
    default: return 0;
    }
}
static uint8_t diag_byte(float v, int kind) {
    float x = kind == 2 ? (v <= 0 ? 0 : v / (1.0f + v)) : kind == 1 ? v * 0.5f + 0.5f : v;
    return (uint8_t)(x <= 0 ? 0 : x >= 1 ? 255 : x * 255.0f + 0.5f);
}
static void diag_target(char tag, int idx, int ci, const TargetRef *tr) {
    long long fr = nvn_front_frame_next();
    Tex *t = tr->tex;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    if (!vt) return;
    int level = target_level(tr), ex, dec;
    VkFormat f = vt->fmt;
    if ((tr->view.set & 0x84) == 0x84 && tr->view.format && tr->view.format != t->b.format) { VkFormat f2 = tex_vkfmt(tr->view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; }
    if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->decode || vt->expand || vt->samples > 1 || vt->is3d ||
        (vt->rs && !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))) {
        printf("nvn_vk_diag: %c f%lld #%d col%d nvn 0x%x vk %d: not read (samples %d, scaled %d, decoded %d)\n", tag, fr, idx, ci, (unsigned)t->b.format, (int)f, vt->samples, vt->rs, vt->decode);
        return;
    }
    int w = lvl_(vt->rs ? t->b.width : vt->w, level), h = lvl_(vt->rs ? t->b.height : vt->h, level), r[6] = {0, 0, 0, w, h, 1};
    size_t count = (size_t)w * h;
    int maxw = tag == 'E' ? 400 : 160, W = w > maxw ? maxw : w, H = (int)((int64_t)h * W / w); if (H < 1) H = 1;
    uint8_t *px = malloc(count * vt->bpb), *cell = calloc((size_t)W * H, 4);      /* cell: R G B state (0 empty, 1 colour, 2 infinity, 3 NaN) */
    if (px && cell && read_region(t, &tr->view, r, px, 0, 0)) {
        float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY}, o[4] = {0, 0, 0, 0};
        double sum[4] = {0, 0, 0, 0}; long long nfin[4] = {0, 0, 0, 0}, nnan = 0, ninf = 0, over1 = 0, over16 = 0;
        int kind = 0, nch = 0;
        diag_tables();
        const uint8_t *s = px;
        for (int y = 0; y < h; y++) {
            int cy = (int)((int64_t)y * H / h);
            for (int x = 0; x < w; x++, s += vt->bpb) {
                nch = diag_texel(f, s, o, &kind);
                if (!nch) { y = h; break; }
                int isnan_ = 0, isinf_ = 0, big1 = 0, big16 = 0;
                for (int c = 0; c < nch; c++) {
                    float v = o[c];
                    if (v != v) isnan_ = 1;
                    else if (v > 1e30f || v < -1e30f) isinf_ = 1;
                    else { if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; sum[c] += v; nfin[c]++; if (c < 3) { if (v > 1.001f) big1 = 1; if (v > 16.0f) big16 = 1; } }
                }
                nnan += isnan_; ninf += isinf_; over1 += big1; over16 += big16;
                uint8_t *q = cell + ((size_t)cy * W + (int)((int64_t)x * W / w)) * 4;
                if (isnan_) { q[0] = 255; q[1] = 0; q[2] = 255; q[3] = 3; }
                else if (isinf_) { if (q[3] < 2) { q[0] = 0; q[1] = 255; q[2] = 255; q[3] = 2; } }
                else if (!q[3]) { q[0] = diag_byte(o[0], kind); q[1] = nch > 1 ? diag_byte(o[1], kind) : q[0]; q[2] = nch > 2 ? diag_byte(o[2], kind) : nch > 1 ? 0 : q[0]; q[3] = 1; }
            }
        }
        if (!nch) printf("nvn_vk_diag: %c f%lld #%d col%d %dx%d nvn 0x%x vk %d: format not decoded\n", tag, fr, idx, ci, w, h, (unsigned)t->b.format, (int)f);
        else {
            char line[320], name[64], path[340]; int lp = 0;
            for (int c = 0; c < nch; c++) lp += snprintf(line + lp, sizeof line - lp, " %c %.4g..%.4g m%.4g", "RGBA"[c], nfin[c] ? mn[c] : 0.0f, nfin[c] ? mx[c] : 0.0f, nfin[c] ? sum[c] / (double)nfin[c] : 0.0);
            snprintf(name, sizeof name, "r%03d_%c_%03d_%d_%02x.bmp", g_diag_round, tag, idx, ci, (unsigned)t->b.format);
            snprintf(path, sizeof path, "%s/%s", g_diag_dir, name);
            printf("nvn_vk_diag: %c f%lld #%d col%d %dx%d nvn 0x%x vk %d lvl %d feedback %d:%s nan %lld inf %lld over1 %.2f%% over16 %.2f%% -> %s\n", tag, fr, idx, ci, w, h, (unsigned)t->b.format, (int)f, level, vt->feedback, line,
                   nnan, ninf, 100.0 * (double)over1 / (double)count, 100.0 * (double)over16 / (double)count, name);
            uint32_t row = ((uint32_t)W * 3 + 3) & ~3u, size = 54 + row * (uint32_t)H, v;
            uint8_t head[54] = {'B', 'M'}, *rowb = calloc(1, row);
            memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &W, 4); memcpy(head + 22, &H, 4); head[26] = 1; head[28] = 24;
            FILE *fp = fopen(path, "wb");
            if (fp && rowb) {
                fwrite(head, 1, 54, fp);
                for (int y = H - 1; y >= 0; y--) {
                    const uint8_t *c = cell + (size_t)y * W * 4;
                    for (int x = 0; x < W; x++, c += 4) { rowb[x * 3] = c[2]; rowb[x * 3 + 1] = c[1]; rowb[x * 3 + 2] = c[0]; }
                    fwrite(rowb, 1, row, fp);
                }
            }
            if (fp) fclose(fp);
            free(rowb);
        }
    }
    free(px); free(cell);
}
/* 0.1.6: the depth target after a pass, the part the capture never read: are the holes in a model already in its depth-only pass?
 * The image as it is (a scaled target at its own size), its depth only. The picture is stretched between the lowest and highest
 * value that is not exactly 0 or 1: low is bright, 0 and 1 black. 640 wide on a screen with few draws, 160 otherwise. */
static void diag_depth(int idx, const TargetRef *tr) {
    long long fr = nvn_front_frame_next();
    Tex *t = tr->tex;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    if (!vt) return;
    int d32 = vt->fmt == VK_FORMAT_D32_SFLOAT || vt->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT, d24 = vt->fmt == VK_FORMAT_X8_D24_UNORM_PACK32 || vt->fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    if ((!d32 && !d24) || vt->samples > 1 || vt->is3d) { printf("nvn_vk_diag: Z f%lld #%d nvn 0x%x vk %d: not read (samples %d)\n", fr, idx, (unsigned)t->b.format, (int)vt->fmt, vt->samples); return; }
    int level = target_level(tr), layer = (tr->view.set & 0x82) == 0x82 ? tr->view.lay0 : 0, w = lvl_(vt->w, level), h = lvl_(vt->h, level);
    size_t count = (size_t)w * h;
    int maxw = g_diag_low ? 640 : 160, W = w > maxw ? maxw : w, H = (int)((int64_t)h * W / w);
    if (H < 1) H = 1;
    float *px = malloc(count * 4), mn = INFINITY, mx = -INFINITY;
    if (!px) return;
    VMem mem;
    VkBuffer buf = vbuf_new((uint64_t)count * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    pass_end(); cmd_begin();
    VkImageLayout was = vt->layout;
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
    cmd_wait_all();
    memcpy(px, mem.map, count * 4);
    vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
    long long n0 = 0, n1 = 0, nnan = 0;
    for (size_t i = 0; i < count; i++) {
        float v;
        if (d24) { uint32_t u; memcpy(&u, &px[i], 4); v = (float)(u & 0xffffffu) / 16777215.0f; px[i] = v; }
        v = px[i];
        if (v != v) nnan++;
        else if (v == 0.0f) n0++;
        else if (v == 1.0f) n1++;
        else { if (v < mn) mn = v; if (v > mx) mx = v; }
    }
    static Tex *last_t; static float last_mn, last_mx; static long long last_n1;      /* nothing new to see: no picture (a title round had 5 of the same) */
    int same = last_t == t && last_mn == mn && last_mx == mx && last_n1 == n1;
    last_t = t; last_mn = mn; last_mx = mx; last_n1 = n1;
    if (same || mx < mn) {
        printf("nvn_vk_diag: Z f%lld #%d %dx%d nvn 0x%x lvl %d: %s, exactly 0 %.2f%%, exactly 1 %.2f%%, nan %lld\n", fr, idx, w, h, (unsigned)t->b.format, level, same ? "same as the pass before" : "all one value", 100.0 * (double)n0 / (double)count, 100.0 * (double)n1 / (double)count, nnan);
        free(px);
        return;
    }
    char name[64], path[340];
    snprintf(name, sizeof name, "r%03d_Z_%03d_%02x.bmp", g_diag_round, idx, (unsigned)t->b.format);
    snprintf(path, sizeof path, "%s/%s", g_diag_dir, name);
    printf("nvn_vk_diag: Z f%lld #%d %dx%d nvn 0x%x lvl %d: %.7g..%.7g, exactly 0 %.2f%%, exactly 1 %.2f%%, nan %lld -> %s\n", fr, idx, w, h, (unsigned)t->b.format, level,
           mx >= mn ? mn : 0.0f, mx >= mn ? mx : 0.0f, 100.0 * (double)n0 / (double)count, 100.0 * (double)n1 / (double)count, nnan, name);
    uint32_t row = ((uint32_t)W * 3 + 3) & ~3u, size = 54 + row * (uint32_t)H, v;
    uint8_t head[54] = {'B', 'M'}, *rowb = calloc(1, row);
    memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &W, 4); memcpy(head + 22, &H, 4); head[26] = 1; head[28] = 24;
    FILE *fp = fopen(path, "wb");
    if (fp && rowb) {
        fwrite(head, 1, 54, fp);
        for (int y = H - 1; y >= 0; y--) {
            for (int x = 0; x < W; x++) {
                float d = px[(size_t)((int64_t)y * h / H) * w + (size_t)((int64_t)x * w / W)];
                uint8_t g = d == 0.0f || d == 1.0f || d != d || mx <= mn ? 0 : (uint8_t)(40.0f + 215.0f * (mx - d) / (mx - mn));
                rowb[x * 3] = rowb[x * 3 + 1] = rowb[x * 3 + 2] = g;
            }
            fwrite(rowb, 1, row, fp);
        }
    }
    if (fp) fclose(fp);
    free(px); free(rowb);
}
/* 0.1.7, which draw starts a glow (idea: Power Moon Recomp's ODYSSEY_WATCH_MAX). One frame after every second capture round: each HDR target
 * (R11G11B10F, 64 wide or more) is read back after each draw into it, and the draw is named when the target's brightest value doubles or
 * its mean grows by a quarter. Stops itself after GW_MS, so a slow GPU keeps a hitch of about that size. SMO_VK_WATCH=0 off. */
#define GW_MS 2500
#define GW_LINES 250
static int g_gw_reads, g_gw_lines, g_gw_n, g_gw_stop;
static int64_t g_gw_t0;
static struct { Tex *t; int level; float mx, mean; } g_gw[32];
static void gw_begin(void) { g_gw_reads = g_gw_lines = g_gw_n = g_gw_stop = 0; g_gw_t0 = (int64_t)vk_now_ns(); diag_tables(); printf("nvn_vk_watch: frame traced\n"); }
static void gw_after_draw(void) {
    if (g_diag_mode != 3 || g_gw_stop || g_diag_busy || !R.in_pass) return;
    static uint32_t *px; static size_t cap;
    for (int i = 0; i < R.ncol; i++) {
        const TargetRef *tr = &R.col[i];
        Tex *t = tr->tex;
        VTex *vt = t && !t->dead ? t->vk : NULL;
        if (!vt || vt->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || vt->samples > 1 || vt->is3d) continue;
        int level = target_level(tr), w = lvl_(vt->rs ? t->b.width : vt->w, level), h = lvl_(vt->rs ? t->b.height : vt->h, level), r[6] = {0, 0, 0, w, h, 1}, k = 0;
        if (w < 64 || (vt->rs && !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))) continue;
        size_t count = (size_t)w * h;
        if (count > cap) { free(px); px = malloc(count * 4); cap = px ? count : 0; if (!px) { g_gw_stop = 1; return; } }
        g_diag_busy = 1;                           /* the pass ends here, and goes on at the next draw with its targets loaded */
        size_t got = read_region(t, &tr->view, r, px, 0, 0);
        g_diag_busy = 0;
        if (!got) continue;
        g_gw_reads++;
        float mx = 0; double sum = 0; size_t at = 0;
        for (size_t n = 0; n < count; n++) {
            uint32_t u = px[n];
            float c0 = g_diag_uf[0][u & 2047], c1 = g_diag_uf[0][(u >> 11) & 2047], c2 = g_diag_uf[1][u >> 22];
            float m = c0 > c1 ? c0 : c1; if (c2 > m) m = c2;
            if (m != m) m = 1e30f;                 /* NaN: named at once */
            if (m > mx) { mx = m; at = n; }
            sum += (double)c0 + c1 + c2;
        }
        float mean = (float)(sum / (3.0 * (double)count));
        for (; k < g_gw_n; k++) if (g_gw[k].t == t && g_gw[k].level == level) break;
        if (k == g_gw_n) { if (g_gw_n == 32) continue; g_gw_n++; g_gw[k].t = t; g_gw[k].level = level; g_gw[k].mx = 0; g_gw[k].mean = 0; }
        if (((mx > 2.0f * g_gw[k].mx && mx > 8.0f) || (mean > 1.25f * g_gw[k].mean && mean > 0.05f)) && g_gw_lines < GW_LINES) {
            g_gw_lines++;
            printf("nvn_vk_watch: vs #%d %016llx fs #%d %016llx target %dx%d lvl %d: max %.4g (was %.4g) at %d,%d, mean %.4g (was %.4g)\n", R.prog[0] ? R.prog[0]->serial : -1, (unsigned long long)stage_code_hash(R.prog[0], 0),
                   R.prog[1] ? R.prog[1]->serial : -1, (unsigned long long)stage_code_hash(R.prog[1], 1), w, h, level, mx, g_gw[k].mx, (int)(at % (size_t)w), (int)(at / (size_t)w), mean, g_gw[k].mean);
        }
        g_gw[k].mx = mx; g_gw[k].mean = mean;
        if (((int64_t)vk_now_ns() - g_gw_t0) / 1000000 > GW_MS) { g_gw_stop = 1; printf("nvn_vk_watch: stopped after %d GW_MS (%d reads): the rest of this frame is not traced\n", GW_MS, g_gw_reads); }
        return;                                    /* the pass has ended: the other targets of this draw wait for the next */
    }
}
static void diag_round_begin(int64_t frame) {       /* a new round number, after the highest in the folder; the older ones go */
    int top = 0; const char *name; int isdir; int64_t size;
    plat_mkdirs(g_diag_dir);
    plat_dir *d = plat_dir_open(g_diag_dir);
    if (d) { while (plat_dir_next(d, &name, &isdir, &size)) if (name[0] == 'r' && atoi(name + 1) > top) top = atoi(name + 1); plat_dir_close(d); }
    g_diag_round = top + 1;
    d = plat_dir_open(g_diag_dir);
    if (d) {
        while (plat_dir_next(d, &name, &isdir, &size)) if (name[0] == 'r' && atoi(name + 1) > 0 && atoi(name + 1) <= g_diag_round - DIAG_KEEP) { char p[340]; snprintf(p, sizeof p, "%s/%s", g_diag_dir, name); plat_remove(p); }
        plat_dir_close(d);
    }
    printf("nvn_vk_diag: round %d begins after frame %lld (%lld draws in the last frame)\n", g_diag_round, (long long)frame, (long long)R.frame_draws);
    g_diag_n = 0; g_diag_pass = 0;
}
static void diag_pass_end(void) {                  /* pass_shot, at the end of each render pass of a captured frame */
    if (g_diag_busy) return;
    if (g_diag_mode == 1) {
        for (int i = 0; i < R.ncol; i++) {
            if (!R.col[i].tex) continue;
            int k = 0;
            for (; k < g_diag_n; k++) if (g_diag_list[k].tex == R.col[i].tex && target_level(&g_diag_list[k]) == target_level(&R.col[i])) break;
            if (k == g_diag_n && g_diag_n < DIAG_LIST) g_diag_list[g_diag_n++] = R.col[i];
        }
    } else if (g_diag_mode == 2 && g_diag_pass < 400) {
        g_diag_busy = 1;
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex) diag_target('P', g_diag_pass, i, &R.col[i]);
        if (R.depth.tex) diag_depth(g_diag_pass, &R.depth);
        g_diag_pass++;
        g_diag_busy = 0;
    }
}
static void diag_present(int64_t frame) {          /* vk_present2, with every pass of the frame ended */
    if (g_diag_on < 0) {
        const char *e = getenv("SMO_VK_DIAG"), *sd = getenv("SMO_SHOT_DIR");
        g_diag_on = e ? atoi(e) != 0 : (V.props.vendorID == 0x13b5 && !gfx_options()->diag_off);
        if (e && atoi(e) > 1) g_diag_gap = atoi(e);
        snprintf(g_diag_dir, sizeof g_diag_dir, "%s", sd ? sd : "../diag");
        printf("nvn_vk_diag: capture %s (every %d frames)\n", g_diag_on ? "on" : "off", g_diag_gap);
    }
    if (!g_diag_on) return;
    if (g_diag_mode == 1) {
        g_diag_busy = 1;
        for (int k = 0; k < g_diag_n; k++) diag_target('E', k, 0, &g_diag_list[k]);
        g_diag_busy = 0;
        g_diag_mode = 0; g_diag_wait = 2;
        return;
    }
    if (g_diag_mode == 3) { printf("nvn_vk_watch: %d reads, %d lines, %.0f ms\n", g_gw_reads, g_gw_lines, (double)((int64_t)vk_now_ns() - g_gw_t0) / 1e6); g_diag_mode = 0; return; }
    if (g_diag_mode == 2) {
        static int trace_on = -1, rounds;
        dq_report(); printf("nvn_vk_diag: round %d done, %d passes in the frame\n", g_diag_round, g_diag_pass); g_diag_mode = 0; g_diag_last = frame; g_diag_stable = 0;
        if (trace_on < 0) { const char *e = getenv("SMO_VK_WATCH"); trace_on = !e || atoi(e) != 0; }
        if (trace_on && !g_diag_low && rounds++ % 2 == 0 && rounds <= 24) { gw_begin(); g_diag_mode = 3; }
        return;
    }
    if (g_diag_wait > 0) { if (--g_diag_wait == 0) g_diag_mode = 2; return; }
    g_diag_stable = R.frame_draws >= 150 ? g_diag_stable + 1 : 0;
    if (g_diag_stable >= 240 && (g_diag_last < 0 || frame - g_diag_last >= g_diag_gap)) { g_diag_low = 0; diag_round_begin(frame); g_diag_mode = 1; }
    else {                                         /* 0.1.5: a few rounds of a screen with fewer draws too, the title screen above all, which the rule above never caught */
        static int low_stable, low_rounds;
        low_stable = R.frame_draws >= 20 && R.frame_draws < 150 ? low_stable + 1 : 0;
        if (low_stable >= 240 && low_rounds < 3 && (g_diag_last < 0 || frame - g_diag_last >= 900)) { low_rounds++; low_stable = 0; g_diag_low = 1; diag_round_begin(frame); g_diag_mode = 1; }
    }
}
static void pass_shot(void) {
    static int want = -2, n, busy;
    if (g_diag_mode) diag_pass_end();
    want = passshot_env() ? 1 : -1;
    static int logonly = -1;                       /* SMO_VK_PASSSHOT_LOGONLY=1: the draws are logged, no pictures saved */
    if (logonly < 0) logonly = getenv("SMO_VK_PASSSHOT_LOGONLY") && atoi(getenv("SMO_VK_PASSSHOT_LOGONLY"));
    if (want < 0 || logonly || busy || !R.ncol || !R.col[0].tex || !R.col[0].tex->vk) return;
    static long long last_frame = -1;
    char pre[24] = "";                             /* "<frame>_" before the pass number when a range is captured */
    if (nvn_front_frame_next() != last_frame) { last_frame = nvn_front_frame_next(); n = 0; }
    if (passshot_range()) snprintf(pre, sizeof pre, "%lld_", last_frame);
    Tex *t = R.col[0].tex;
    VTex *vt = t->vk;
    int half4 = t->b.format == 0x29 && vt->bpb == 8;      /* SMO: RGBA16F, shown clamped to 0..1 (alpha too) */
    int half1 = t->b.format == 0x5 && vt->bpb == 2;       /* SMO: R16F (linear depth, shadow maps): raw dump + min/max/mean */
    if (half1) {
        int w1 = vt->w, h1 = vt->h, r1[6] = {0, 0, 0, w1, h1, 1};
        uint16_t *hp = malloc((size_t)w1 * h1 * 2);
        if (hp && read_region(t, NULL, r1, hp, 0, 0)) {
            double mn = 1e30, mx = -1e30, sm = 0;
            for (size_t k = 0; k < (size_t)w1 * h1; k++) { uint16_t hv = hp[k]; int ex = (hv >> 10) & 31, man = hv & 1023; float f = ex == 0 ? ldexpf((float)man, -24) : ex == 31 ? 65504.0f : ldexpf(1.0f + man / 1024.0f, ex - 15); if (hv & 0x8000) f = -f; if (f < mn) mn = f; if (f > mx) mx = f; sm += f; }
            printf("nvn_vk_pass: pass %d: %dx%d R16F min %g max %g mean %g\n", n, w1, h1, mn, mx, sm / ((double)w1 * h1));
            if (getenv("SMO_VK_PASSSHOT_RAW")) { char rp[320]; const char *dd = getenv("SMO_SHOT_DIR"); snprintf(rp, sizeof rp, "%s%spass_%s%03d_05_%dx%d.half", dd ? dd : "", dd ? "/" : "", pre, n, w1, h1); FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(hp, 2, (size_t)w1 * h1, fr); fclose(fr); } }
        }
        free(hp); n++; return;
    }
    if ((vt->bpb != 4 && !half4) || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk_pass: pass %d: colour format 0x%x not captured\n", n++, (unsigned)t->b.format); return; }
    busy = 1;
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    int got = 0;
    if (px && half4) {
        uint16_t *hp = malloc((size_t)w * h * 8);
        if (hp && read_region(t, NULL, rr, hp, 0, 0)) {
            got = 1;
            for (size_t k = 0; k < (size_t)w * h * 4; k++) {
                uint16_t hv = hp[k]; int ex = (hv >> 10) & 31, man = hv & 1023;
                float f = ex == 0 ? ldexpf((float)man, -24) : ex == 31 ? 65504.0f : ldexpf(1.0f + man / 1024.0f, ex - 15);
                if (hv & 0x8000) f = -f;
                px[k] = (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255.0f + 0.5f);
            }
            if (getenv("SMO_VK_PASSSHOT_RAW")) { char rp[320]; const char *dd = getenv("SMO_SHOT_DIR"); snprintf(rp, sizeof rp, "%s%spass_%s%03d_29_%dx%d.half", dd ? dd : "", dd ? "/" : "", pre, n, w, h); FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(hp, 8, (size_t)w * h, fr); fclose(fr); } }
        }
        free(hp);
    } else if (px) got = read_region(t, NULL, rr, px, 0, 0) != 0;
    if (got) {
        int ten = t->b.format == 0x3d || t->b.format == 0x3e;
        long long lit = 0, alpha_hist[4] = {0, 0, 0, 0}; double sum[3] = {0, 0, 0};
        for (size_t k = 0; k < (size_t)w * h; k++) { uint32_t q; memcpy(&q, px + k * 4, 4); alpha_hist[ten ? q >> 30 : (q >> 24) >> 6]++; }
        printf("nvn_vk_pass: pass %d: alpha (top two bits) histogram 0:%lld 1:%lld 2:%lld 3:%lld\n", n, alpha_hist[0], alpha_hist[1], alpha_hist[2], alpha_hist[3]);
        uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
        char path[300];
        const char *dir = getenv("SMO_SHOT_DIR");
        snprintf(path, sizeof path, "%s%spass_%s%03d_%02x.bmp", dir ? dir : "", dir ? "/" : "", pre, n, (unsigned)t->b.format);
        FILE *f = fopen(path, "wb");
        uint8_t head[54] = {'B', 'M'}, *rowb = calloc(1, row);
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        if (f) fwrite(head, 1, 54, f);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                const uint8_t *s = px + ((size_t)y * w + x) * 4;
                int r, g, b;
                if (ten) { uint32_t q; memcpy(&q, s, 4); r = (int)((q & 1023) >> 2); g = (int)(((q >> 10) & 1023) >> 2); b = (int)(((q >> 20) & 1023) >> 2); }
                else { r = s[0]; g = s[1]; b = s[2]; }
                rowb[x * 3] = (uint8_t)b; rowb[x * 3 + 1] = (uint8_t)g; rowb[x * 3 + 2] = (uint8_t)r;
                sum[0] += r; sum[1] += g; sum[2] += b; lit += (r | g | b) > 8;
            }
            if (f) fwrite(rowb, 1, row, f);
        }
        if (f) fclose(f);
        free(rowb);
        if (getenv("SMO_VK_PASSSHOT_RAW")) {         /* SMO debug aid: the texels as read (4 bytes each, rows top first), for alpha and exact values */
            char rp[320]; snprintf(rp, sizeof rp, "%s%spass_%s%03d_%02x_%dx%d.raw", dir ? dir : "", dir ? "/" : "", pre, n, (unsigned)t->b.format, w, h);
            FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(px, 4, (size_t)w * h, fr); fclose(fr); }
        }
        printf("nvn_vk_pass: pass %d: %dx%d format 0x%x: %.1f%% not black, mean %.1f %.1f %.1f -> %s\n", n, w, h, (unsigned)t->b.format,
               100.0 * (double)lit / ((double)w * h), sum[0] / ((double)w * h), sum[1] / ((double)w * h), sum[2] / ((double)w * h), path);
    }
    free(px);
    n++;
    busy = 0;
}
static VkSemaphore g_sem[8][2];
static int64_t g_sub_kept;                         /* Android port: SUBOPTIMAL presents after which the swapchain was kept (nothing had changed) */
static VkSemaphore g_sem_img[16];                  /* session 11: the "rendering finished" semaphore, one per swapchain image */
static int g_isem;
/* Session 11, the PROGRESS BAR: our own overlay, drawn over the swapchain image after the blit of the game's picture (never
 * into the game's textures, so pictures saved by SMO_SHOT* do not have it). A bar at the bottom of the window:
 *   - while the loading barrier holds a loading screen: the same picture is presented again about ten times a second with
 *     the bar growing (jobs done of jobs queued when the hold began), so the screen is not a still picture;
 *   - while the container store is read from data.arc (first start) and while the preparation runs: a thinner bar on every
 *     presented frame, NOT near a fight.
 * SMO_VK_PROGRESS=0 turns it off. The image: 8x8, two layers (bar colour, track colour), cleared once; drawn by two blits. */
static struct { VkImage img; VMem mem; int made, on; float frac; int thick; void *bar_arg; } g_ovl = {.on = -1};
/* SMO port: the amiibo menu's picture (vk_menu): an image of its own, blitted over the whole swapchain image */
static struct { VkImage img; VMem mem; VkBuffer buf; VMem bmem; VkFormat fmt; int w, h, on; } g_menu;
/* Android port: a rectangle of the window as the app sees it -> the swapchain image, turned V.rot quarter turns clockwise
 * (the present pass turns the picture; the overlays drawn by blits follow it) */
static void ovl_rot_rect(VkImageBlit *b) {
    int LW = (int)(V.rot & 1 ? V.extent.height : V.extent.width), LH = (int)(V.rot & 1 ? V.extent.width : V.extent.height);
    int x0 = b->dstOffsets[0].x, y0 = b->dstOffsets[0].y, x1 = b->dstOffsets[1].x, y1 = b->dstOffsets[1].y;
    if (V.rot == 1) { b->dstOffsets[0].x = LH - y1; b->dstOffsets[1].x = LH - y0; b->dstOffsets[0].y = x0; b->dstOffsets[1].y = x1; }
    else if (V.rot == 2) { b->dstOffsets[0].x = LW - x1; b->dstOffsets[1].x = LW - x0; b->dstOffsets[0].y = LH - y1; b->dstOffsets[1].y = LH - y0; }
    else if (V.rot == 3) { b->dstOffsets[0].x = y0; b->dstOffsets[1].x = y1; b->dstOffsets[0].y = LW - x1; b->dstOffsets[1].y = LW - x0; }
}
static void swapchain_views(int make) {
#ifdef __ANDROID__
    for (uint32_t i = 0; i < 16; i++) {
        if (V.views[i]) { vkDestroyImageView(V.device, V.views[i], NULL); V.views[i] = VK_NULL_HANDLE; }
        if (!make || i >= V.image_count) continue;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = V.images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = V.format.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(V.device, &vi, NULL, &V.views[i]) != VK_SUCCESS) V.views[i] = VK_NULL_HANDLE;
    }
#else
    (void)make;
#endif
}
#ifdef __ANDROID__
/* Android port: the PRESENT PASS. The game's window texture is drawn into the swapchain image by one full-screen
 * triangle: turned by V.rot (the surface's transform, so the compositor need not turn it: that cost ~100 ms a frame on
 * the emulator), in its own shape inside the window (bars cleared black), linear filtering. One pipeline per swapchain
 * format; one descriptor set per frame from a ring of 16 (8 command buffers are in flight at most). Returns 0 when it
 * cannot draw (no view of the swapchain image): the caller blits as before. The swapchain image is TRANSFER_DST before
 * and after (the blitted overlays follow). */
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet sets[16]; VkSampler smp; VkShaderModule vs, fs, ovs, ofs;
                struct { VkFormat fmt; VkPipeline p, po; } pipe[4]; int npipe, next, failed;      /* po: the touch controls' */
                VkBuffer ubo; VMem ubo_mem;                /* the overlay: 16 slices of OV_BYTES, one per descriptor set */
                int fps; int64_t fps_t0, fps_n; } g_pp;
/* the overlay's uniform block (std140): shapes (x, y in the window 0..1, r in heights, kind 1 disc / 2 ring), colours,
 * info (count, fps, aspect of the window, opacity) */
#define OV_SHAPES 24
#define OV_BYTES 2048
typedef struct { float sh[OV_SHAPES][4], col[OV_SHAPES][4], info[4], info2[4], glyph[OV_SHAPES], gs[43][4]; int32_t gr[9][4]; } OvBlock;      /* info2.x: sharpening 0..1; glyph: 0 a plain disc, else the icon (the shader's glyph()) */
/* the letters' strokes (x0 y0 x1 y1, in a button of radius 1, y down) and each letter's first stroke and count (A B X Y
 * house L R Z ~), read by the overlay shader from its uniform block: as constant arrays indexed by a variable the
 * Adreno 750 (system driver) made them again for every pixel, 20 ms of GPU time a frame for the touch controls and the
 * GPU clock held at 680..834 MHz instead of 231 (2026-11-03, heat) */
static const float k_ov_gs[43][4] = {
    {-0.32, 0.42, 0.0, -0.42}, {0.0, -0.42, 0.32, 0.42}, {-0.19, 0.1, 0.19, 0.1},      /* A */
    {-0.24, -0.42, -0.24, 0.42}, {-0.24, -0.42, 0.1, -0.42}, {0.1, -0.42, 0.24, -0.3}, {0.24, -0.3, 0.24, -0.12}, {0.24, -0.12, 0.1, 0.0}, {-0.24, 0.0, 0.12, 0.0}, {0.12, 0.0, 0.28, 0.13}, {0.28, 0.13, 0.28, 0.29}, {0.28, 0.29, 0.12, 0.42}, {0.12, 0.42, -0.24, 0.42},      /* B */
    {-0.3, -0.42, 0.3, 0.42}, {0.3, -0.42, -0.3, 0.42},      /* X */
    {-0.3, -0.42, 0.0, 0.0}, {0.3, -0.42, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.42},      /* Y */
    {-0.48, 0.02, 0.0, -0.44}, {0.0, -0.44, 0.48, 0.02}, {-0.3, -0.14, -0.3, 0.4}, {0.3, -0.14, 0.3, 0.4}, {-0.3, 0.4, 0.3, 0.4},      /* a house */
    {-0.2, -0.42, -0.2, 0.42}, {-0.2, 0.42, 0.26, 0.42},      /* L */
    {-0.24, -0.42, -0.24, 0.42}, {-0.24, -0.42, 0.1, -0.42}, {0.1, -0.42, 0.26, -0.28}, {0.26, -0.28, 0.26, -0.12}, {0.26, -0.12, 0.1, 0.0}, {0.1, 0.0, -0.24, 0.0}, {0.02, 0.0, 0.28, 0.42},      /* R */
    {-0.24, -0.42, 0.24, -0.42}, {0.24, -0.42, -0.24, 0.42}, {-0.24, 0.42, 0.26, 0.42},      /* Z */
    {-0.46, 0.0, -0.345, -0.092}, {-0.345, -0.092, -0.23, -0.13}, {-0.23, -0.13, -0.115, -0.092}, {-0.115, -0.092, 0.0, 0.0}, {0.0, 0.0, 0.115, 0.092}, {0.115, 0.092, 0.23, 0.13}, {0.23, 0.13, 0.345, 0.092}, {0.345, 0.092, 0.46, 0.0},      /* ~ */
};
static const int32_t k_ov_gr[9][4] = {{0, 3}, {3, 10}, {13, 2}, {15, 3}, {18, 5}, {23, 2}, {25, 7}, {32, 3}, {35, 8}};
/* the present pass's shaders, in parts: the picture (one triangle over the window) and the touch controls (one square
 * per button, drawn over it: only the pixels under a button work out its shape). The 20 ms a frame the touch controls
 * cost on an Adreno 750 was not the split's to fix (the time stayed the same): it was the letters' tables (k_ov_gs). */
#define PP_HEAD \
            "#version 460\n" \
            "layout(push_constant) uniform PC { vec4 rect; vec4 crop; vec2 ext; int rot; } pc;\n"   /* rect: the picture in the window (0..1), crop: its part of the texture (uv) */ \
            "layout(set = 0, binding = 0) uniform sampler2D src;\n" \
            "layout(std140, set = 0, binding = 1) uniform OV { vec4 sh[24]; vec4 col[24]; vec4 info; vec4 info2; vec4 glyph[6]; vec4 gs[43]; ivec4 gr[9]; } ov;\n"   /* the overlay: touch controls, fps */ \
            "layout(location = 0) out vec4 o;\n"
#define PP_SHAPES \
            "float sg(vec2 p, vec4 s) { vec2 pa = p - s.xy, ba = s.zw - s.xy; return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0)); }\n" \
            "float letter(int k, vec2 p, vec2 o, float sc) {\n" \
            "  ivec2 r = ov.gr[k].xy; vec2 q = (p - o) / sc; float d = 1e9;\n" \
            "  for (int j = 0; j < 10; j++) { if (j >= r.y) break; d = min(d, sg(q, ov.gs[r.x + j])); }\n" \
            "  return d * sc;\n" \
            "}\n" \
            "float rbox(vec2 p, vec2 b, vec4 r) {\n"   /* a rounded box, corner radii top left, top right, bottom left, bottom right */ \
            "  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.z) : (p.y < 0.0 ? r.y : r.w);\n" \
            "  vec2 q = abs(p) - b + rr; return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;\n" \
            "}\n" \
            "float tri(vec2 p, vec2 p0, vec2 p1, vec2 p2) {\n"   /* signed distance to a triangle (after Inigo Quilez) */ \
            "  vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2, v0 = p - p0, v1 = p - p1, v2 = p - p2;\n" \
            "  vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0), q1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0), q2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);\n" \
            "  float sn = sign(e0.x * e2.y - e0.y * e2.x);\n" \
            "  vec2 d = min(min(vec2(dot(q0, q0), sn * (v0.x * e0.y - v0.y * e0.x)), vec2(dot(q1, q1), sn * (v1.x * e1.y - v1.y * e1.x))), vec2(dot(q2, q2), sn * (v2.x * e2.y - v2.y * e2.x)));\n" \
            "  return -sqrt(d.x) * sign(d.y);\n" \
            "}\n" \
            "float body(int g, vec2 p) {\n"   /* the button's outline, a signed distance: 6 7 the L R tabs, 8 9 ZL ZR, 10 11 + -, else a circle */ \
            "  if (g == 6 || g == 7) return rbox(p, vec2(0.9, 0.62), vec4(0.22));\n" \
            "  if (g == 8) return rbox(p, vec2(0.9, 0.62), vec4(0.55, 0.18, 0.18, 0.18));\n" \
            "  if (g == 9) return rbox(p, vec2(0.9, 0.62), vec4(0.18, 0.55, 0.18, 0.18));\n" \
            "  if (g == 10) return min(rbox(p, vec2(0.82, 0.27), vec4(0.06)), rbox(p, vec2(0.27, 0.82), vec4(0.06)));\n" \
            "  if (g == 11) return rbox(p, vec2(0.82, 0.25), vec4(0.06));\n" \
            "  return length(p) - 0.86;\n" \
            "}\n" \
            "float glyph(int g, vec2 p) {\n"   /* the distance to its icon: 1-4 A B X Y, 5 a house, 6-9 L R ZL ZR, 12-15 arrows up right down left, 16 R3, 17 ~, 18 19 the sticks' knobs */ \
            "  if (g <= 7) return letter(g - 1, p, vec2(0.0), 1.0);\n" \
            "  if (g <= 9) return min(letter(7, p, vec2(-0.27, 0.0), 0.8), letter(g - 3, p, vec2(0.29, 0.0), 0.8));\n" \
            "  if (g <= 11) return 1e9;\n" \
            "  if (g <= 15) { vec2 q = p; for (int k = 0; k < 3; k++) if (k < g - 12) q = vec2(q.y, -q.x); return tri(q, vec2(0.0, -0.42), vec2(0.4, 0.28), vec2(-0.4, 0.28)) + 0.075; }\n" \
            "  if (g == 16) return min(min(abs(length(p - vec2(0.0, 0.18)) - 0.44), letter(6, p, vec2(0.0, 0.18), 0.55)), tri(p, vec2(0.0, -0.33), vec2(0.2, -0.58), vec2(-0.2, -0.58)) + 0.075);\n" \
            "  if (g == 17) return letter(8, p, vec2(0.0), 1.0);\n" \
            "  return min(abs(length(p) - 0.56), letter(g == 18 ? 5 : 6, p, vec2(0.0), 0.62));\n" \
            "}\n"
static VkPipeline pp_pipeline(VkFormat fmt, VkPipeline *po) {
    for (int i = 0; i < g_pp.npipe; i++) if (g_pp.pipe[i].fmt == fmt) { *po = g_pp.pipe[i].po; return g_pp.pipe[i].p; }
    if (g_pp.npipe >= 4) return VK_NULL_HANDLE;
    if (!g_pp.layout) {
        uint64_t h;
        VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2; li.pBindings = b;
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &g_pp.dsl));
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &g_pp.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &g_pp.layout));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 16; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &g_pp.pool));
        VkDescriptorSetLayout l16[16];
        for (int i = 0; i < 16; i++) l16[i] = g_pp.dsl;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_pp.pool; ai.descriptorSetCount = 16; ai.pSetLayouts = l16;
        VKC(vkAllocateDescriptorSets(V.device, &ai, g_pp.sets));
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(V.device, &si, NULL, &g_pp.smp));
        g_pp.ubo = vbuf_new((uint64_t)OV_BYTES * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_pp.ubo_mem);
        g_pp.vs = module_from_glsl(0, "#version 460\nvoid main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n",
                                   "present pass vertex", &h, NULL);
        g_pp.fs = module_from_glsl(1,
            PP_HEAD
            "bool seg7(int d, vec2 p) {\n"                                                      /* p: 0..1 in the digit's box */
            "  int m = int(((d < 4 ? 0x4F5B063Fu : d < 8 ? 0x077D6D66u : 0x6F7Fu) >> uint(8 * (d % 4))) & 0x7Fu); float t = 0.16;\n"   /* the segments of 0..9, packed: no array indexed by a variable (k_ov_gs) */
            "  bool h = p.x > t && p.x < 1.0 - t, up = p.y > t && p.y < 0.5, lo = p.y > 0.5 && p.y < 1.0 - t;\n"
            "  return ((m & 1) != 0 && p.y < t && h) || ((m & 2) != 0 && p.x > 1.0 - t && up) || ((m & 4) != 0 && p.x > 1.0 - t && lo) ||\n"
            "         ((m & 8) != 0 && p.y > 1.0 - t && h) || ((m & 16) != 0 && p.x < t && lo) || ((m & 32) != 0 && p.x < t && up) ||\n"
            "         ((m & 64) != 0 && abs(p.y - 0.5) < t * 0.5 && h);\n"
            "}\n"
            "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
            "vec3 fxaa(vec2 uv) {\n"                                                       /* anti_aliasing = fxaa: blend along an edge found from the luma of the neighbours */
            "  vec2 px = 1.0 / vec2(textureSize(src, 0)); vec3 m = texture(src, uv).rgb;\n"
            "  float lm = luma(m), ln = luma(texture(src, uv + vec2(0.0, -px.y)).rgb), ls = luma(texture(src, uv + vec2(0.0, px.y)).rgb);\n"
            "  float lw = luma(texture(src, uv + vec2(-px.x, 0.0)).rgb), le = luma(texture(src, uv + vec2(px.x, 0.0)).rgb);\n"
            "  float lo = min(lm, min(min(ln, ls), min(lw, le))), hi = max(lm, max(max(ln, ls), max(lw, le)));\n"
            "  if (hi - lo < max(0.0312, hi * 0.125)) return m;\n"
            "  float a = luma(texture(src, uv - px).rgb), b = luma(texture(src, uv + vec2(px.x, -px.y)).rgb);\n"
            "  float d = luma(texture(src, uv + vec2(-px.x, px.y)).rgb), e = luma(texture(src, uv + px).rgb);\n"
            "  vec2 dir = vec2((d + e) - (a + b), (a + d) - (b + e));\n"
            "  float k = 1.0 / (min(abs(dir.x), abs(dir.y)) + max((a + b + d + e) * 0.03125, 0.0078125));\n"
            "  dir = clamp(dir * k, -8.0, 8.0) * px;\n"
            "  vec3 p = 0.5 * (texture(src, uv - dir * (1.0 / 6.0)).rgb + texture(src, uv + dir * (1.0 / 6.0)).rgb);\n"
            "  vec3 q = p * 0.5 + 0.25 * (texture(src, uv - dir * 0.5).rgb + texture(src, uv + dir * 0.5).rgb);\n"
            "  float lq = luma(q);\n"
            "  return (lq < lo || lq > hi) ? p : q;\n"
            "}\n"
            "vec3 ssaa(vec2 uv, vec2 dx, vec2 dy, float r) {\n"                            /* the picture larger than its place: the mean of an n x n grid over the pixel's footprint (dx, dy: one screen pixel in uv) */
            "  int n = int(clamp(ceil(r), 2.0, 4.0)); vec3 s = vec3(0.0);\n"
            "  for (int j = 0; j < 4; j++) { if (j >= n) break;\n"
            "    for (int i = 0; i < 4; i++) { if (i >= n) break;\n"
            "      vec2 o = (vec2(float(i), float(j)) + 0.5) / float(n) - 0.5;\n"
            "      s += texture(src, uv + dx * o.x + dy * o.y).rgb; } }\n"
            "  return s / float(n * n);\n"
            "}\n"
            "vec3 sharpen(vec3 c, vec2 uv, vec2 dx, vec2 dy, float k) {\n"                 /* contrast-adaptive: the four neighbours one screen pixel away; less where the local contrast is already high */
            "  vec3 a = texture(src, uv - dy).rgb, b = texture(src, uv - dx).rgb, d = texture(src, uv + dx).rgb, e = texture(src, uv + dy).rgb;\n"
            "  vec3 mn = min(c, min(min(a, b), min(d, e))), mx = max(c, max(max(a, b), max(d, e)));\n"
            "  vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));\n"   /* room left before clipping, relative to the brightest */
            "  vec3 w = -amp * mix(0.125, 0.2, k);\n"
            "  return clamp((c + w * (a + b + d + e)) / (1.0 + 4.0 * w), 0.0, 1.0);\n"
            "}\n"
            "vec3 fpsc(vec3 c, vec2 l) {\n"                                                      /* l: the pixel in the window, 0..1 */
            "  float asp = ov.info.z;\n"
            "  int fps = int(ov.info.y);\n"                                                       /* the fps counter, top left */
            "  if (fps >= 0) {\n"
            "    vec2 q = vec2(l.x * asp, l.y) - vec2(0.02, 0.02); float dw = 0.028, dh = 0.05, gap = 0.012;\n"
            "    if (q.x >= -0.006 && q.y >= -0.006 && q.x < 3.0 * (dw + gap) && q.y < dh + 0.006) {\n"
            "      c = mix(c, vec3(0.0), 0.55);\n"
            "      int k = int(q.x / (dw + gap)); vec2 p = vec2((q.x - float(k) * (dw + gap)) / dw, q.y / dh);\n"
            "      int dig = k == 0 ? fps / 100 : k == 1 ? (fps / 10) % 10 : fps % 10;\n"
            "      bool show = k == 2 || (k == 1 && fps >= 10) || (k == 0 && fps >= 100);\n"
            "      if (show && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0 && seg7(dig, p)) c = fps >= 55 ? vec3(0.3, 1.0, 0.3) : fps >= 30 ? vec3(1.0, 0.9, 0.2) : vec3(1.0, 0.3, 0.3);\n"
            "    }\n"
            "  }\n"
            "  return c;\n"
            "}\n"
            "void main() {\n"
            "  vec2 b = gl_FragCoord.xy / pc.ext;\n"                                             /* the swapchain image, 0..1 */
            "  vec2 l = pc.rot == 1 ? vec2(b.y, 1.0 - b.x) : pc.rot == 2 ? vec2(1.0 - b.x, 1.0 - b.y) : pc.rot == 3 ? vec2(1.0 - b.y, b.x) : b;\n"   /* the window as the app sees it */
            "  vec2 q = (l - pc.rect.xy) / pc.rect.zw;\n"
            "  vec2 uv = pc.crop.xy + q * pc.crop.zw, dx = dFdx(uv), dy = dFdy(uv), ts = vec2(textureSize(src, 0));\n"   /* outside any branch: derivatives */
            "  float r = max(length(dx * ts), length(dy * ts));\n"                          /* texels of the picture per screen pixel */
            "  vec3 c = (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) ? vec3(0.0) : (r > 1.05 ? ssaa(uv, dx, dy, r) : ov.info.x >= 100.0 ? fxaa(uv) : texture(src, uv).rgb);\n"
            "  if (ov.info2.x > 0.0 && q.x >= 0.0 && q.y >= 0.0 && q.x <= 1.0 && q.y <= 1.0) c = sharpen(c, uv, dx, dy, ov.info2.x);\n"
            "  o = vec4(fpsc(c, l), 1.0);\n"
            "}\n", "present pass fragment", &h, NULL);
        g_pp.ovs = module_from_glsl(0,                 /* a square around button gl_VertexIndex / 6, in the window as the app sees it, turned into the swapchain image */
            "#version 460\n"
            "layout(push_constant) uniform PC { vec4 rect; vec4 crop; vec2 ext; int rot; } pc;\n"
            "layout(std140, set = 0, binding = 1) uniform OV { vec4 sh[24]; vec4 col[24]; vec4 info; vec4 info2; vec4 glyph[6]; vec4 gs[43]; ivec4 gr[9]; } ov;\n"
            "layout(location = 0) flat out int si;\n"
            "void main() {\n"
            "  int i = gl_VertexIndex / 6, k = gl_VertexIndex % 6; vec4 s = ov.sh[i];\n"
            "  vec2 c = vec2(k == 1 || k == 2 || k == 4 ? 1.0 : -1.0, k == 2 || k == 4 || k == 5 ? 1.0 : -1.0);\n"
            "  vec2 l = s.xy + c * vec2(1.2 * s.z / ov.info.z, 1.2 * s.z);\n"      /* the fragment shader draws within 1.15 radii */
            "  vec2 b = pc.rot == 1 ? vec2(1.0 - l.y, l.x) : pc.rot == 2 ? vec2(1.0 - l.x, 1.0 - l.y) : pc.rot == 3 ? vec2(l.y, 1.0 - l.x) : l;\n"   /* the inverse of the picture's turn */
            "  gl_Position = vec4(b * 2.0 - 1.0, 0.0, 1.0); si = i;\n"
            "}\n", "present pass touch controls vertex", &h, NULL);
        g_pp.ofs = module_from_glsl(1,                 /* one button, premultiplied: blended over the picture as overlay() mixed it in (the layers in order) */
            PP_HEAD
            "layout(location = 0) flat in int si;\n"
            PP_SHAPES
            "void lay(inout vec4 r, vec3 c, float a) { a = clamp(a, 0.0, 1.0); r = vec4(r.rgb * (1.0 - a) + c * a, r.a * (1.0 - a) + a); }\n"
            "void main() {\n"
            "  vec2 b = gl_FragCoord.xy / pc.ext;\n"
            "  vec2 l = pc.rot == 1 ? vec2(b.y, 1.0 - b.x) : pc.rot == 2 ? vec2(1.0 - b.x, 1.0 - b.y) : pc.rot == 3 ? vec2(1.0 - b.y, b.x) : b;\n"
            "  float asp = ov.info.z, op = ov.info.w; int i = si;\n"
            "  vec4 s = ov.sh[i]; vec2 p = vec2((l.x - s.x) * asp, l.y - s.y) / s.z; float d = length(p);\n"
            "  if (d > 1.15) discard;\n"
            "  int g = int(ov.glyph[i / 4][i % 4]); vec4 r = vec4(0.0);\n"
            "  if (s.w > 1.5 || g == 0) {\n"   /* rings (the stick bases, the on/off ring) and plain discs */
            "    if (d > 1.0) discard;\n"
            "    lay(r, ov.col[i].rgb, s.w > 1.5 ? (d > 0.88 ? op : 0.0) : (d > 0.85 ? min(1.0, op * 1.6) : op * ov.col[i].a));\n"
            "  } else {\n"
            "    float px = 1.0 / (s.z * (pc.rot % 2 == 1 ? pc.ext.x : pc.ext.y)), db = body(g, p), io = min(1.0, op * 2.0);\n"   /* an icon button: a dark fill, a white outline and icon (dark on white while pressed); px: one pixel in its units */
            "    vec3 ink = ov.col[i].a > 1.2 ? vec3(0.1) : vec3(1.0);\n"
            "    lay(r, ov.col[i].rgb, clamp(0.5 - db / px, 0.0, 1.0) * op * ov.col[i].a);\n"
            "    lay(r, ink, clamp((0.075 - abs(db)) / px + 0.5, 0.0, 1.0) * io);\n"
            "    lay(r, ink, clamp((0.075 - glyph(g, p)) / px + 0.5, 0.0, 1.0) * io);\n"
            "  }\n"
            "  o = r;\n"
            "}\n", "present pass touch controls fragment", &h, NULL);
        if (!g_pp.vs || !g_pp.fs || !g_pp.ovs || !g_pp.ofs) { printf("nvn_vk: the present pass's shaders were not made: the picture is blitted (not turned)\n"); g_pp.failed = 1; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_pp.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_pp.fs; st[1].pName = "main";
    VkPipelineShaderStageCreateInfo sto[2];
    memcpy(sto, st, sizeof st);
    sto[0].module = g_pp.ovs; sto[1].module = g_pp.ofs;
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkPipelineColorBlendAttachmentState bo = ba;
    bo.blendEnable = VK_TRUE; bo.colorBlendOp = bo.alphaBlendOp = VK_BLEND_OP_ADD;
    bo.srcColorBlendFactor = bo.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; bo.dstColorBlendFactor = bo.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo cbo = cb;
    cbo.pAttachments = &bo;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = g_pp.layout;
    VkGraphicsPipelineCreateInfo go = gi;
    go.pStages = sto; go.pColorBlendState = &cbo;
    VkPipeline p = VK_NULL_HANDLE;
    *po = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &p) != VK_SUCCESS || vkCreateGraphicsPipelines(V.device, NULL, 1, &go, NULL, po) != VK_SUCCESS) { printf("nvn_vk: the present pass's pipeline was not made: the picture is blitted (not turned)\n"); g_pp.failed = 1; return VK_NULL_HANDLE; }
    g_pp.pipe[g_pp.npipe].fmt = fmt; g_pp.pipe[g_pp.npipe].p = p; g_pp.pipe[g_pp.npipe].po = *po; g_pp.npipe++;
    return p;
}
static int present_pass(uint32_t index, VTex *vt, const int *s4) {
    if (!g_pp.failed && V.image_count && !V.views[0]) swapchain_views(1);      /* the first present on this swapchain (the device's functions are loaded by now) */
    if (g_pp.failed || index >= 16 || !V.views[index] || !vt || vt->samples != 1 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->is3d) return 0;
    VkPipeline po, p = pp_pipeline(V.format.format, &po);
    if (!p) return 0;
    int nshape = 0;
    EView e;
    eview_of(vt->t, vt, NULL, &e);
    e.target = 1; e.lvl0 = 0; e.nlvl = 1; e.lay0 = 0; e.nlay = 1;
    VkImageView src = vt_view(vt, &e, DIM_2D, 0);
    if (!src) return 0;
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    int slot = g_pp.next++ & 15;
    VkDescriptorSet set = g_pp.sets[slot];
    {   /* the overlay: the touch controls (nnpad_android.c) and the fps (presents per second, SMO_FPS=0 hides it) */
        OvBlock ob;
        TouchShape ts[OV_SHAPES];
        float op = 0.45f;
        memset(&ob, 0, sizeof ob);
        int n = nshape = android_touch_shapes(ts, OV_SHAPES, &op);
        for (int i = 0; i < n; i++) {
            const char *lb = ts[i].label;
            static const char *k_glyph[] = {"", "A", "B", "X", "Y", "EX", "L", "R", "ZL", "ZR", "+", "-", "^", ">", "v", "<", "R3", "~", "LS", "RS"};
            int gl = 0;
            for (int k = 1; k < (int)(sizeof k_glyph / sizeof k_glyph[0]); k++) if (!strcmp(lb, k_glyph[k])) gl = k;
            float r = 0.75f, g = 0.75f, b = 0.75f;
            if (gl) r = g = b = ts[i].pressed ? 1.0f : 0.07f;      /* icon buttons (the shader's glyph()): a dark fill, white while pressed */
            else if (!strcmp(lb, "T")) { r = g = b = ts[i].pressed ? 0.9f : 0.45f; ts[i].pressed = 0; }      /* the touch controls' on/off ring: bright on, faint off */
            else if (ts[i].pressed) r = g = b = 1.0f;
            ob.sh[i][0] = ts[i].x; ob.sh[i][1] = ts[i].y; ob.sh[i][2] = ts[i].r; ob.sh[i][3] = (float)ts[i].round;
            ob.glyph[i] = (float)gl;
            ob.col[i][0] = r; ob.col[i][1] = g; ob.col[i][2] = b; ob.col[i][3] = ts[i].pressed ? 1.6f : 1.0f;
        }
        int64_t now = tk();
        plat_li fq; ((fq).QuadPart = plat_qpf());
        if (!g_pp.fps_t0) g_pp.fps_t0 = now;
        g_pp.fps_n++;
        if (now - g_pp.fps_t0 >= fq.QuadPart / 2) { g_pp.fps = (int)((double)g_pp.fps_n * (double)fq.QuadPart / (double)(now - g_pp.fps_t0) + 0.5); g_pp.fps_n = 0; g_pp.fps_t0 = now; }
        static int show_fps = -1;
        if (show_fps < 0) show_fps = getenv("SMO_FPS") ? atoi(getenv("SMO_FPS")) != 0 : gfx_options()->fps_counter;      /* graphics.ini fps_counter (the launcher) */
        int BWo = (int)V.extent.width, BHo = (int)V.extent.height;
        ob.info2[0] = gfx_options()->sharpen;
        ob.info[0] = (float)n + (gfx_options()->fxaa ? 100.0f : 0.0f); ob.info[1] = show_fps ? (float)(g_pp.fps > 999 ? 999 : g_pp.fps) : -1.0f;
        ob.info[2] = (float)(V.rot & 1 ? BHo : BWo) / (float)(V.rot & 1 ? BWo : BHo); ob.info[3] = op;
        memcpy(ob.gs, k_ov_gs, sizeof ob.gs); memcpy(ob.gr, k_ov_gr, sizeof ob.gr);
        memcpy((uint8_t *)g_pp.ubo_mem.map + (size_t)slot * OV_BYTES, &ob, sizeof ob);
    }
    VkDescriptorImageInfo ii = {g_pp.smp, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo ubi = {g_pp.ubo, (VkDeviceSize)slot * OV_BYTES, sizeof(OvBlock)};
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof w);
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
    w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &ubi;
    vkUpdateDescriptorSets(V.device, 2, w, 0, NULL);
    /* the picture's place in the window as the app sees it, keeping its shape */
    int BW = (int)V.extent.width, BH = (int)V.extent.height, LW = V.rot & 1 ? BH : BW, LH = V.rot & 1 ? BW : BH, pw = LW, ph = LH;
    if (s4[2] > 0 && s4[3] > 0) { if ((int64_t)LW * s4[3] > (int64_t)LH * s4[2]) pw = (int)((int64_t)LH * s4[2] / s4[3]); else ph = (int)((int64_t)LW * s4[3] / s4[2]); }
    struct { float rect[4], crop[4], ext[2]; int32_t rot, pad; } pc;
    pc.rect[0] = (float)((LW - pw) / 2) / (float)LW; pc.rect[1] = (float)((LH - ph) / 2) / (float)LH; pc.rect[2] = (float)pw / (float)LW; pc.rect[3] = (float)ph / (float)LH;
    pc.crop[0] = (float)s4[0] / (float)vt->w; pc.crop[1] = (float)s4[1] / (float)vt->h; pc.crop[2] = (float)s4[2] / (float)vt->w; pc.crop[3] = (float)s4[3] / (float)vt->h;
    pc.ext[0] = (float)BW; pc.ext[1] = (float)BH; pc.rot = V.rot; pc.pad = 0;
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = V.views[index]; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = V.extent; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    if (g_pt_on && g_pt_n < PT_MAX) {              /* SMO_VK_PASSTIME: this pass too (format -1: the window's) */
        memset(&g_pt[g_pt_n], 0, sizeof g_pt[0]);
        g_pt[g_pt_n].w = BW; g_pt[g_pt_n].h = BH; g_pt[g_pt_n].ncol = 1; g_pt[g_pt_n].cfmt = -1; g_pt[g_pt_n].d0 = g_pt[g_pt_n].d1 = R.frame_draws;
        pt_mark(0);
    }
    vkCmdBeginRendering(R.cmd, &ri);
    VkViewport vpt = {0, 0, (float)BW, (float)BH, 0, 1};
    VkRect2D sc = {{0, 0}, V.extent};
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdSetViewport(R.cmd, 0, 1, &vpt); vkCmdSetScissor(R.cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pp.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(R.cmd, g_pp.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    if (nshape > 0) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, po); vkCmdDraw(R.cmd, 6u * (uint32_t)nshape, 1, 0, 0); }      /* the touch controls, one square each */
    vkCmdEndRendering(R.cmd);
    if (g_pt_on && g_pt_n < PT_MAX && g_pt[g_pt_n].cfmt == -1) pt_mark(1);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    R.pipe_bound = NULL; R.dyn_dirty = 1;          /* our own pipeline and dynamic state: the game's draws set theirs again */
    android_touch_aspect((float)LW / (float)(LH > 0 ? LH : 1));
    return 1;
}
#endif
static void overlay_draw(uint32_t index) {
    if (g_menu.on && g_menu.img) {
        VkImageBlit b;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1].x = g_menu.w; b.srcOffsets[1].y = g_menu.h; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)V.extent.width; b.dstOffsets[1].y = (int32_t)V.extent.height; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, g_menu.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        return;
    }
    if (g_ovl.on < 0) g_ovl.on = !(getenv("SMO_VK_PROGRESS") && !atoi(getenv("SMO_VK_PROGRESS")));
    if (!g_ovl.on || g_ovl.thick <= 0) return;
    if (g_ovl.thick < 8) g_ovl_frames++;
    VkImageSubresourceRange sr2 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!g_ovl.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        VkClearColorValue c0 = {{0.92f, 0.92f, 0.92f, 1.0f}}, c1 = {{0.06f, 0.06f, 0.06f, 1.0f}};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent.width = 8; ci.extent.height = 8; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 2;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_ovl.img));
        vkGetImageMemoryRequirements(V.device, g_ovl.img, &rq);
        g_ovl.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_ovl.img, g_ovl.mem.mem, g_ovl.mem.off));
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &sr2);
        sr2.baseArrayLayer = 1;
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c1, 1, &sr2);
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        g_ovl.made = 1;
    }
    int W = (int)(V.rot & 1 ? V.extent.height : V.extent.width), H = (int)(V.rot & 1 ? V.extent.width : V.extent.height), th = g_ovl.thick * H / 720, x0 = W / 10, x1 = W - W / 10, y1 = H - H / 40, y0;
    float f = g_ovl.frac < 0 ? 0 : g_ovl.frac > 1 ? 1 : g_ovl.frac;
    if (th < 2) th = 2;
    y0 = y1 - th;
    if (x1 - x0 < 8 || y0 < 0) return;
    for (int pass = 0; pass < 2; pass++) {          /* the track, then the bar over it */
        VkImageBlit b;
        int xe = pass ? x0 + (int)((float)(x1 - x0) * f) : x1;
        if (xe <= x0) continue;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.srcSubresource.baseArrayLayer = pass ? 0 : 1;
        b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = 8; b.srcOffsets[1].y = 8; b.srcOffsets[1].z = 1;
        b.dstOffsets[0].x = pass ? x0 : x0 - 2; b.dstOffsets[0].y = pass ? y0 : y0 - 2; b.dstOffsets[1].x = pass ? xe : x1 + 2; b.dstOffsets[1].y = pass ? y1 : y1 + 2; b.dstOffsets[1].z = 1;
        ovl_rot_rect(&b);
        vkCmdBlitImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
}
/* SMO port: the console's Capture button (F12 in the game window; a pad's share / capture button: display.c and
 * nnpad_sdl.c call smo_capture_request). The frame being presented is read back like a SMO_SHOT picture, made 8-bit
 * opaque RGBA, and handed to nnalbum.c, which turns it as the game asked (nn::oe::SetScreenShotImageOrientation: snapshot
 * mode with the camera turned) and saves it in the album folder beside the game's own screenshots. */
static volatile int32_t g_capture_req;
void smo_capture_request(void) { plat_atomic_xchg(&g_capture_req, 1); }
int album_save_capture(const uint8_t *rgba, int w, int h, char *out_path, size_t out_cap);       /* nnalbum.c */
static void capture_vk(VTex *vt) {
    int w = vt->rs ? (int)vt->t->b.width : vt->w, h = vt->rs ? (int)vt->t->b.height : vt->h;
    int ten = vt->t->b.format == 0x3d || vt->t->b.format == 0x3e;          /* RGB10A2 window texture */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("capture: the window texture's format %d is not a 32-bit colour one; no picture\n", (int)vt->fmt); return; }
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); printf("capture: the frame could not be read back; no picture\n"); return; }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *s = px + i * 4;
        if (ten) { uint32_t q; memcpy(&q, s, 4); s[0] = (uint8_t)((q & 1023) >> 2); s[1] = (uint8_t)(((q >> 10) & 1023) >> 2); s[2] = (uint8_t)(((q >> 20) & 1023) >> 2); }
        s[3] = 255;
    }
    album_save_capture(px, w, h, NULL, 0);
    free(px);
}
static void vk_present2(void *arg, int progress);
/* nvn_vk_m4.c m4_barrier, render thread, g_sh_lock NOT held: the held loading screen once more, with the bar at `frac` */
static void present_progress(float frac) { if (!g_ovl.bar_arg) return; g_ovl.frac = frac; g_ovl.thick = 8; vk_present2(g_ovl.bar_arg, 1); g_ovl.thick = 0; }
/* SMO port: the amiibo menu (smo_amiibo.c), render thread. READ: the frame the PresentJob presented (its window texture),
 * read back like a Capture-button picture. SHOW: the menu's picture, copied when it changed into an image of the swapchain's
 * colour class (a blit between them keeps the stored values, as the frame's own blit does), shown over the whole window by
 * a present of the held frame (vk_present2 progress = 1, as the loading-screen bar; overlay_draw blits the picture). END: the
 * image and its upload buffer freed. */
static void vk_menu(void *arg) {
    UiMenuJob *m = arg;
    PresentJob *j = m->present;
    m->ok = 0;
    if (!V.alive) return;
    if (m->op == UI_MENU_READ) {
        Tex *t = j && j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
        VTex *vt = t && !t->dead ? t->vk : NULL;
        if (!vt || vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
        int w = vt->rs ? (int)vt->t->b.width : vt->w, h = vt->rs ? (int)vt->t->b.height : vt->h, ten = vt->t->b.format == 0x3d || vt->t->b.format == 0x3e;
        int rr[6] = {0, 0, 0, w, h, 1};
        uint8_t *px = malloc((size_t)w * h * 4);
        if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t *s = px + i * 4;
            if (ten) { uint32_t q; memcpy(&q, s, 4); s[0] = (uint8_t)((q & 1023) >> 2); s[1] = (uint8_t)(((q >> 10) & 1023) >> 2); s[2] = (uint8_t)(((q >> 20) & 1023) >> 2); }
            s[3] = 255;
        }
        m->out = px; m->out_w = w; m->out_h = h; m->ok = 1;
        return;
    }
    VkFormat fmt = V.format.format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (g_menu.img && (m->op == UI_MENU_END || g_menu.w != m->w || g_menu.h != m->h || g_menu.fmt != fmt)) {
        cmd_wait_all();
        vkDestroyImage(V.device, g_menu.img, NULL); vmem_free(g_menu.mem);
        vkDestroyBuffer(V.device, g_menu.buf, NULL); vmem_free(g_menu.bmem);
        g_menu.img = NULL;
    }
    if (m->op == UI_MENU_END) { m->ok = 1; return; }
    if (!j || !m->rgba || m->w <= 0 || m->h <= 0) return;
    int fresh = 0;
    if (!g_menu.img) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent.width = (uint32_t)m->w; ci.extent.height = (uint32_t)m->h; ci.extent.depth = 1;
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_menu.img));
        vkGetImageMemoryRequirements(V.device, g_menu.img, &rq);
        g_menu.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_menu.img, g_menu.mem.mem, g_menu.mem.off));
        g_menu.buf = vbuf_new((uint64_t)m->w * m->h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_menu.bmem);
        g_menu.w = m->w; g_menu.h = m->h; g_menu.fmt = fmt;
        fresh = 1;
    }
    if (m->changed || fresh) {                          /* the previous present's copy was waited for by its own cmd_wait_all */
        VkBufferImageCopy c;
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
        c.imageExtent.width = (uint32_t)m->w; c.imageExtent.height = (uint32_t)m->h; c.imageExtent.depth = 1;
        memcpy(g_menu.bmem.map, m->rgba, (size_t)m->w * m->h * 4);
        pass_end();
        cmd_begin();
        image_barrier(g_menu.img, VK_IMAGE_ASPECT_COLOR_BIT, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(R.cmd, g_menu.buf, g_menu.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        image_barrier(g_menu.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    g_menu.on = 1;
    vk_present2(j, 1);
    g_menu.on = 0;
    m->ok = 1;
}
/* Process memory every 300 frames (tools/dev/memmap.py names the allocations): working set (pages in memory; the guest
 * heap's pages count once per address they are mapped at), its private part, private bytes (commit of private memory)
 * and the commit of pagefile-backed sections (the guest memory heap), next to the Vulkan memory this file allocated. */
typedef struct { uint32_t cb, faults; size_t peak_ws, ws, qpp, qp, qpnp, qnp, pagefile, peak_pagefile, priv, priv_ws, shared_commit; } MemCounters;
static void mem_report(int64_t frame) {
    MemCounters m;
    memset(&m, 0, sizeof m); m.cb = sizeof m;
#ifdef _WIN32
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m)) { m.cb = (uint32_t)offsetof(MemCounters, priv_ws); K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, m.cb); }
#else
    plat_process_memory(&m.ws, &m.priv, &m.peak_ws);      /* resident, private (anonymous), peak resident */
    m.priv_ws = m.priv;
#endif
    int64_t host = 0;
    for (int b = 0; b < g_nblk; b++) if (g_blk[b] && g_blk[b]->map) host += (int64_t)g_blk[b]->size;
    plat_li fq; ((fq).QuadPart = plat_qpf());
    printf("nvn_vk_mem: frame %lld: working set %llu MB (private %llu MB, peak %llu MB), private bytes %llu MB, section commit %llu MB | Vulkan memory %lld MB (host-visible %lld MB) | shader modules made %lld, released %lld (%.0f ms in vkDestroyShaderModule), made again %lld\n",
           (long long)frame, (unsigned long long)(m.ws >> 20), (unsigned long long)(m.priv_ws >> 20), (unsigned long long)(m.peak_ws >> 20), (unsigned long long)(m.priv >> 20),
           (unsigned long long)(m.shared_commit >> 20), (long long)(g_mem_total >> 20), (long long)(host >> 20), (long long)R.modules, (long long)g_mod_released,
           (double)g_mod_destroy_tk * 1000.0 / (double)fq.QuadPart, (long long)g_mod_again);
    fflush(stdout);
}
static void vk_present(void *arg) { vk_present2(arg, 0); }
/* Android port: frames that reached the window (GameActivity's loading screen goes once the game shows its own) */
static volatile int64_t g_win_presents;
long long nvn_vk_window_presents(void) { return g_win_presents; }
static void vk_present2(void *arg, int progress) {
    PresentJob *j = arg;
    static int shot = -2, every, shot_from, shot_to;
    if (!V.alive) return;
    int64_t t_present = tk();
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    int64_t frame = nvn_front_frame_next();
    {   /* session 12, TEST ONLY: SMO_VK_WD_TEST=<frame>[,<seconds>] holds the render thread at that frame (default 12 s), to see the watchdog's report */
        static int wt = -2, ws = 12;
        if (wt == -2) { const char *e = getenv("SMO_VK_WD_TEST"); wt = e ? atoi(e) : -1; if (e && strchr(e, ',')) ws = atoi(strchr(e, ',') + 1); }
        if (wt > 0 && frame == wt && !progress) { printf("nvn_vk_watchdog: TEST: the render thread is held %d s at frame %lld (SMO_VK_WD_TEST)\n", ws, (long long)frame); fflush(stdout); wt = -1; plat_sleep_ms((uint32_t)ws * 1000); }
    }
    if (progress) goto swap;
    if (j->win) for (int i = 0; i < j->win->count && i < 8; i++) wintex_note(j->win->tex[i]);
    g_ovl.bar_arg = arg;
    g_wd_where = 1;
    m4_barrier(frame, R.frame_draws);
    g_wd_where = 0;
    g_ovl.bar_arg = NULL;
    /* the thin bar of a normal frame: the store being read, or the preparation running, and no fight near */
    g_ovl.thick = 0;
    if ((g_prep_state == 5 || g_prep_state == 1) && !fight_recent()) {
        g_ovl.thick = 4;
        g_ovl.frac = g_prep_state == 5 ? (float)g_store_pct / 100.0f : g_prep_nfiles ? (float)g_pq_done / (float)(g_pq_queued > 0 ? g_pq_queued + (int64_t)(g_prep_nfiles - g_prep_read) * 90 : 1) : 0;      /* about 90 variations a file */
    }
    g_render_tid = plat_tid();
    if (g_skip_fr_n) { g_skip_frames++; printf("nvn_vk_skipf: frame %lld (%s): %lld draws left out (shader_wait = skip)\n", (long long)frame, g_in_fight ? "fight" : "no fight", (long long)g_skip_fr_n); g_skip_fr_n = 0; }
    if (shot == -2) { shot = getenv("SMO_SHOT") ? atoi(getenv("SMO_SHOT")) : -1; every = getenv("SMO_SHOT_EVERY") ? atoi(getenv("SMO_SHOT_EVERY")) : 0; shot_from = getenv("SMO_SHOT_FROM") ? atoi(getenv("SMO_SHOT_FROM")) : 0; shot_to = getenv("SMO_SHOT_TO") ? atoi(getenv("SMO_SHOT_TO")) : 0x7fffffff; }
    pass_end();
    diag_present(frame);
    { int step = nvn_front_take_step_shot(); if (step && vt) { char path[300]; const char *dir = getenv("SMO_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp_vk(path, vt); } }
    if (vt && (frame == shot || (every > 0 && frame >= shot_from && frame <= shot_to && frame % every == 0))) {
        char path[300];
        const char *dir = getenv("SMO_SHOT_DIR");
        snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
        save_bmp_vk(path, vt);
    }
    {   static char at[256] = {1};                    /* SMO_CAPTURE_AT=<frame>[,<frame>...]: the Capture button pressed at those frames (tests) */
        if (at[0] == 1) { const char *e = getenv("SMO_CAPTURE_AT"); snprintf(at, sizeof at, ",%s,", e ? e : ""); }
        char key[32];
        snprintf(key, sizeof key, ",%lld,", (long long)frame);
        if (at[1] != ',' && strstr(at, key)) smo_capture_request();
    }
    {   /* SMO_CAPTURE_GS=<n>: the Capture button pressed at n frames with a draw with a geometry stage (the lightning), at least
         * 60 frames apart (tests) */
        static int left = -1;
        static int64_t seen, last = -1000;
        if (left < 0) left = getenv("SMO_CAPTURE_GS") ? atoi(getenv("SMO_CAPTURE_GS")) : 0;
        if (left > 0 && g_gs_draws != seen && frame - last >= 60) { left--; last = frame; smo_capture_request(); }
        seen = g_gs_draws;
    }
    gsq_poll();
    cq_poll();
    eq_poll();
    if (vt && g_capture_req && plat_atomic_xchg(&g_capture_req, 0)) capture_vk(vt);       /* the Capture button */
    {   /* session 9, SMO_SHOT_BATTLE=<n>: one picture at the n-th battle frame (dispatches and more than 120 draws), i.e. at the
         * same moment of the fight whatever the loading took: battle_<n>.bmp in SMO_SHOT_DIR */
        static int bshot = -2;
        if (bshot == -2) bshot = getenv("SMO_SHOT_BATTLE") ? atoi(getenv("SMO_SHOT_BATTLE")) : -1;
        if (vt && bshot > 0 && g_battle_frames == bshot) {
            char path[300];
            const char *dir = getenv("SMO_SHOT_DIR");
            snprintf(path, sizeof path, "%s%sbattle_%d.bmp", dir ? dir : "", dir ? "/" : "", bshot);
            save_bmp_vk(path, vt);
            printf("nvn_vk: picture of battle frame %d saved at frame %lld: %s\n", bshot, (long long)frame, path);
            bshot = -1;
        }
    }
swap: ;
    pass_end();
    /* an extra present of a held loading screen: everything submitted so far is waited for first, so that the semaphore pair
     * taken below has no pending operation (validation run s11v2: VUID-vkAcquireNextImageKHR-semaphore-01779, three times,
     * all at such presents: with one submission a present the pair came round before its command buffer had been waited for) */
    if (progress) cmd_wait_all();
    /* The swapchain has the window texture's colour class: a blit between them then keeps the stored values (the OpenGL
     * path blits with FRAMEBUFFER_SRGB off). */
#ifdef __ANDROID__
    /* Android port: Android windows are RGBA (BGRA is often not offered, and on the emulator it came out red/blue swapped) */
    VkFormat want = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
#else
    VkFormat want = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
#endif
    if (want != V.format.format) { cmd_wait_all(); swapchain_destroy(); V.format.format = want; printf("nvn_vk: swapchain format %d (the window texture's colour class)\n", (int)want); }
    uint32_t index = 0;
    VkResult r = VK_SUCCESS;
    /* one pair of semaphores per present, 8 in turn: a semaphore handed to vkAcquireNextImageKHR must not still be waited
     * for by an earlier submission (validation layer: VUID-vkAcquireNextImageKHR-semaphore-01779) */
    g_isem = (g_isem + 1) % 8;
    if (!g_sem[g_isem][0]) { VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][0])); VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][1])); }
    VkSemaphore acq = g_sem[g_isem][0], rend = g_sem[g_isem][1];
    if (V.swapchain) {      /* the window changed size (F11, display.c): a driver need not say so with OUT_OF_DATE / SUBOPTIMAL */
        int ww = 0, wh = 0;
        uint32_t lw = V.rot & 1 ? V.extent.height : V.extent.width, lh = V.rot & 1 ? V.extent.width : V.extent.height;      /* Android port: a pre-rotated swapchain has the window's size turned (it was made again EVERY frame) */
        if (plat_window_size(V.hwnd, &ww, &wh) && ww > 0 && wh > 0 && ((uint32_t)ww != lw || (uint32_t)wh != lh)) {
            cmd_wait_all(); swapchain_destroy(); plat_atomic_inc(&g_vk_recreated);
        }
    }
    int ok = V.swapchain || swapchain_create();
    plat_atomic_inc(&g_wd_try);
    if (ok) {
        g_wd_where = 2;
        r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { static int nl; if (nl++ < 10) printf("nvn_vk: acquire answered OUT_OF_DATE at frame %lld: the swapchain is made again (%lld times so far)\n", (long long)frame, (long long)g_vk_recreated + 1);
                                             cmd_wait_all(); swapchain_destroy(); plat_atomic_inc(&g_vk_recreated); ok = swapchain_create(); if (ok) r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index); }
        if (ok && r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_ok("vkAcquireNextImageKHR", r); ok = 0; }
    }
    if (!ok) { plat_atomic_inc(&g_vk_skipped); cmd_submit(NULL, NULL); }
    else {
        int suboptimal = r == VK_SUBOPTIMAL_KHR;
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {   /* session 11: the semaphore a present waits for is one PER SWAPCHAIN IMAGE (validation run s11v3, twice in 9,783
             * frames: VUID-vkQueueSubmit-pSignalSemaphores-00067, "may still be in use by the swapchain", when the driver
             * hands out the same image many times in a row and the ring of 8 comes round); the acquire semaphore stays in the ring */
            if (index < 16) {
                if (!g_sem_img[index]) { VkSemaphoreCreateInfo smi2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi2, NULL, &g_sem_img[index])); }
                rend = g_sem_img[index];
            }
        }
        cmd_begin();
        if (vt) vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd_begin();
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
#ifdef __ANDROID__
        int s4p[4] = {0, 0, vt ? vt->w : 0, vt ? vt->h : 0};
        if (vt && j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4p, j->win->crop, sizeof s4p);
        if (vt && vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {      /* track O: the crop is in the game's pixels */
            int x1 = rs_edge(s4p[0] + s4p[2], vt->sx, vt->w), y1 = rs_edge(s4p[1] + s4p[3], vt->sy, vt->h);
            s4p[0] = rs_edge(s4p[0], vt->sx, vt->w); s4p[1] = rs_edge(s4p[1], vt->sy, vt->h); s4p[2] = x1 - s4p[0]; s4p[3] = y1 - s4p[1];
        }
        if (present_pass(index, vt, s4p)) { }       /* drawn, turned and in its shape (the swapchain image is TRANSFER_DST again) */
        else
#endif
        if (vt && vt->samples == 1) {
            VkImageBlit b;
            int s4[4] = {0, 0, vt->w, vt->h};
            if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4, j->win->crop, sizeof s4);
            if (vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {      /* track O: the crop is in the game's pixels */
                int x1 = rs_edge(s4[0] + s4[2], vt->sx, vt->w), y1 = rs_edge(s4[1] + s4[3], vt->sy, vt->h);
                s4[0] = rs_edge(s4[0], vt->sx, vt->w); s4[1] = rs_edge(s4[1], vt->sy, vt->h); s4[2] = x1 - s4[0]; s4[3] = y1 - s4[1];
            }
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)V.extent.width; b.dstOffsets[1].y = (int32_t)V.extent.height; b.dstOffsets[1].z = 1;
#ifdef __ANDROID__
            {   /* Android port: the picture keeps its shape (16:9) in a window of another one (a 20:9 phone): bars at the sides
                 * or above and below, cleared black; the touch layer is told where the picture is */
                int W = (int)V.extent.width, H = (int)V.extent.height, w = W, h = H;
                if (s4[2] > 0 && s4[3] > 0) { if ((int64_t)W * s4[3] > (int64_t)H * s4[2]) w = (int)((int64_t)H * s4[2] / s4[3]); else h = (int)((int64_t)W * s4[3] / s4[2]); }
                if (w != W || h != H) {
                    VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f;
                    vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
                    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
                    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                    b.dstOffsets[0].x = (W - w) / 2; b.dstOffsets[0].y = (H - h) / 2; b.dstOffsets[1].x = b.dstOffsets[0].x + w; b.dstOffsets[1].y = b.dstOffsets[0].y + h;
                }
                android_touch_aspect((float)W / (float)(H > 0 ? H : 1));
            }
#endif
            vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        } else { VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f; vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
        overlay_draw(index);
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        int pt_read = g_pt_on && frame == g_pt_frame;
        if (pt_read) g_pt_on = 0;
        cmd_submit(acq, rend);
        if (pt_read) {
            uint64_t *ts = calloc((size_t)g_pt_n * 2, 8);
            vkDeviceWaitIdle(V.device);
            if (ts && g_pt_n && vkGetQueryPoolResults(V.device, g_pt_pool, 0, (uint32_t)g_pt_n * 2, (size_t)g_pt_n * 16, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                double per = (double)V.props.limits.timestampPeriod / 1e6, pass_ms = 0, gap_ms = 0;
                int order[PT_MAX], gord[PT_MAX];
                double dur[PT_MAX], gap[PT_MAX];
                for (int i = 0; i < g_pt_n; i++) {
                    dur[i] = ts[i * 2 + 1] > ts[i * 2] ? (double)(ts[i * 2 + 1] - ts[i * 2]) * per : 0;
                    gap[i] = i && ts[i * 2] > ts[i * 2 - 1] ? (double)(ts[i * 2] - ts[i * 2 - 1]) * per : 0;
                    pass_ms += dur[i]; gap_ms += gap[i]; order[i] = gord[i] = i;
                }
                for (int a = 0; a < g_pt_n; a++) for (int b = a + 1; b < g_pt_n; b++) {
                    if (dur[order[b]] > dur[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }
                    if (gap[gord[b]] > gap[gord[a]]) { int t = gord[a]; gord[a] = gord[b]; gord[b] = t; }
                }
                printf("nvn_vk_passtime: frame %lld: %d passes, %.2f ms in passes, %.2f ms between them, first to last %.2f ms\n", (long long)frame, g_pt_n, pass_ms, gap_ms,
                       (double)(ts[g_pt_n * 2 - 1] - ts[0]) * per);
                for (int k = 0; k < g_pt_n && k < 15; k++) { int i = order[k]; printf("nvn_vk_passtime:   pass %3d: %6.2f ms  %4dx%-4d %d colour (format 0x%x) depth 0x%x, %lld draws\n", i, dur[i], g_pt[i].w, g_pt[i].h, g_pt[i].ncol, g_pt[i].cfmt, g_pt[i].dfmt, (long long)(g_pt[i].d1 - g_pt[i].d0)); }
                for (int k = 0; k < g_pt_n && k < 15; k++) { int i = gord[k]; if (gap[i] > 0.05) printf("nvn_vk_passtime:   gap before pass %3d: %6.2f ms\n", i, gap[i]); }
                printf("nvn_vk_passtime: every pass in order (load / clear / discarded in the pass: bits 0..7 colour, 0x100 depth); %d discards outside a pass\n", g_pt_dout);
                for (int i = 0; i < g_pt_n; i++) printf("nvn_vk_passtime:   #%3d %6.3f ms gap %6.3f  %4dx%-4d %d col 0x%x depth 0x%x  load %03x clear %03x disc %03x  %lld draws\n", i, dur[i], gap[i], g_pt[i].w, g_pt[i].h, g_pt[i].ncol, g_pt[i].cfmt, g_pt[i].dfmt,
                                                  g_pt[i].load, g_pt[i].clear, g_pt[i].disc, (long long)(g_pt[i].d1 - g_pt[i].d0));
            }
            free(ts);
        }
        VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rend; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
        g_wd_where = 4;
        r = vkQueuePresentKHR(V.queue, &pr);
        if (r >= 0) plat_atomic_inc(&g_win_presents);
#ifdef __ANDROID__
        /* Android port: Android answers SUBOPTIMAL for more than a changed window (measured on a Galaxy S24 Ultra: the
         * swapchain was made again over and over, 14% of the render thread in a cutscene). The swapchain is made again
         * only when the surface's transform or size really changed (Google's advice for pre-rotating apps). */
        if (r != VK_ERROR_OUT_OF_DATE_KHR && (r == VK_SUBOPTIMAL_KHR || suboptimal)) {
            VkSurfaceCapabilitiesKHR caps;
            int want_rot = V.rot, same = 0;
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.gpu, V.surface, &caps) == VK_SUCCESS) {
                VkSurfaceTransformFlagsKHR tr = caps.currentTransform;
                int rot = getenv("SMO_VK_ROTATE_COMPOSITOR") ? 0 : tr == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : tr == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2 : tr == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3 : 0;
                int w = 0, h = 0; plat_window_size(V.hwnd, &w, &h);
                uint32_t ew = (uint32_t)(rot & 1 ? h : w), eh = (uint32_t)(rot & 1 ? w : h);
                same = rot == want_rot && ew == V.extent.width && eh == V.extent.height;
                static int nl;
                if (nl++ < 10) printf("nvn_vk: present answered %s (transform 0x%x, window %dx%d): %s\n", r == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "a suboptimal acquire", (unsigned)tr, w, h,
                                      same ? "nothing changed, the swapchain is kept" : "made again");
            }
            if (same) { r = VK_SUCCESS; suboptimal = 0; g_sub_kept++; }
        }
#endif
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) { cmd_wait_all(); swapchain_destroy(); plat_atomic_inc(&g_vk_recreated); }
        else vk_ok("vkQueuePresentKHR", r);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { int64_t nowt = tk(); if (g_gap_prev && nowt - g_gap_prev > g_gap_max) g_gap_max = nowt - g_gap_prev; g_gap_prev = nowt; g_wd_beat = nowt; }      /* session 12: the watchdog's heartbeat = a frame reached the window */
        if (!progress) plat_atomic_inc(&g_vk_frames);
#ifdef __ANDROID__
        if (!progress) { void android_perf_frame(void); android_perf_frame(); }      /* android_perf.c: performance hints */
#endif
        if (g_pt_frame == -2) g_pt_frame = getenv("SMO_VK_PASSTIME") ? atoi(getenv("SMO_VK_PASSTIME")) : -1;
        if (!progress && g_pt_frame > 0 && frame + 1 == g_pt_frame && D.f12.hostQueryReset) {
            if (!g_pt_pool) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = PT_MAX * 2; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_pt_pool)); }
            vkResetQueryPool(V.device, g_pt_pool, 0, PT_MAX * 2);
            g_pt_n = 0; g_pt_dout = 0; g_pt_on = 1;
        }
    }
    g_wd_where = 0;
    if (progress) { g_ovl_presents++; return; }
    occ_frame_end();
    if (g_occ_qn) { int64_t w0 = g_occ_waits; occ_resolve(g_occ_present ? g_occ_qn : 0); g_occ_present_waits += g_occ_waits > w0; }      /* the frame's occlusion counts (read by the game in the next frame) */
    g_tm[4] += tk() - t_present;
    nvn_front_frame_end(R.frame_draws, R.frame_skipped, R.frame_clears);
    { static int64_t disp_prev; g_in_fight = R.dispatches != disp_prev; disp_prev = R.dispatches; if (g_in_fight) { g_fight_last = frame; if (g_prep_state == 1) g_prep_fight_frames++; } }      /* a frame with compute dispatches (skinning): fighters are on screen */
    {   /* session 9: per-frame record for the "nvn_vk_gpu_fight:" line */
        static int64_t t_prev;
        int64_t t_now = tk();
        int battle = g_in_fight && R.frame_draws > 120;      /* the fight draws 126..450 a frame (entrance over 205), loading screens 38..45 */
        if (battle) g_battle_frames++;
        {   /* how long the way to the fight took (menus are scripted by polls, so two runs differ by their loading) */
            static int64_t t_first, t_load; static int said;
            plat_li fq; ((fq).QuadPart = plat_qpf());
            if (!t_first) t_first = t_now;
            if (!battle && R.frame_draws <= 60 && frame > 600) { if (!t_load) t_load = t_now; } else if (!battle && R.frame_draws > 60) t_load = 0;      /* t_load: the start of the run of loading-screen frames before the battle */
            if (battle && !said) { said = 1; printf("nvn_vk: first battle frame is frame %lld, %.1f s after the first present; the loading screen before it lasted %.1f s\n", (long long)frame, (double)(t_now - t_first) / (double)fq.QuadPart, t_load ? (double)(t_now - t_load) / (double)fq.QuadPart : 0.0); }
        }
        if (frame >= 0 && frame < FRSTAT_N) {
            plat_li fq; ((fq).QuadPart = plat_qpf());
            g_fr_fight[frame] = (uint8_t)battle;
            g_fr_wall[frame] = t_prev ? (float)((double)(t_now - t_prev) * 1000.0 / (double)fq.QuadPart) : 0.0f;
        }
        t_prev = t_now;
        g_cur_frame = frame + 1;
    }
    {   /* session 8: what this frame paid for textures made or read at their use (hold log), then the textures made ahead */
        plat_li fq; ((fq).QuadPart = plat_qpf());
        if (holdlog_on() && g_up_fr_tk * 1000 > 3 * fq.QuadPart) { static int nl; if (nl++ < 300) printf("nvn_vk_upl: frame %lld (%s): %d textures made or read at their use, %.1f MB, %.1f ms\n", (long long)frame, g_in_fight ? "fight" : "no fight", g_up_fr_n, (double)g_up_fr_bytes / 1048576.0, (double)g_up_fr_tk * 1000.0 / (double)fq.QuadPart); }
        g_up_fr_tk = g_up_fr_bytes = 0; g_up_fr_n = 0;
        tex_ahead(frame);
    }
    tex_reclaim(frame);
    if (frame % 300 == 0) {                      /* where the render thread's time went, per frame, in the last 300 frames */
        static int64_t last[12], last_sets, last_flush, last_reused, last_disp;
        plat_li fq; ((fq).QuadPart = plat_qpf());
        double k = 1000.0 / (double)fq.QuadPart / 300.0;
        if (g_ts_on > 0) { static double ts_last; static int64_t n_last; printf("nvn_vk_gpu: frames %lld..%lld: GPU time %.2f ms per frame (first to last command of %lld command buffers finished in the window)\n", (long long)frame - 300, (long long)frame, (g_ts_ns - ts_last) / 1e6 / 300.0, (long long)(g_ts_n - n_last)); ts_last = g_ts_ns; n_last = g_ts_n; }
        printf("nvn_vk_perf: frames %lld..%lld ms/frame: batch replay %.2f (of it: dispatch %.2f, draw preparation %.2f, pipelines %.2f, shader modules %.2f), wait for the GPU %.2f, copy-back %.2f (waiting at batch start %.2f), present %.2f | per frame: %.1f dispatches, %.0f descriptor sets allocated, %.0f reused, %.1f submissions | pipelines not made ahead so far: %lld | holds so far: %lld, %.1f ms, longest %.1f ms\n",
               (long long)frame - 300, (long long)frame, (double)(g_tm[3] - last[3]) * k, (double)(g_tm[2] - last[2]) * k, (double)(g_tm[8] - last[8]) * k, (double)(g_tm[5] - last[5]) * k, (double)(g_tm[6] - last[6]) * k,
               (double)(g_tm[0] - last[0]) * k, (double)(g_tm[1] - last[1]) * k, (double)(g_tm[7] - last[7]) * k, (double)(g_tm[4] - last[4]) * k, (double)(R.dispatches - last_disp) / 300.0, (double)(R.sets - last_sets) / 300.0, (double)(R.sets_reused - last_reused) / 300.0, (double)(R.flushes - last_flush) / 300.0, (long long)g_pipe_miss,
               (long long)(g_hold_n[0] + g_hold_n[1] + g_hold_n[2] + g_hold_n[3]), (double)(g_hold_tk[0] + g_hold_tk[1] + g_hold_tk[2] + g_hold_tk[3]) * k * 300.0, (double)g_hold_max * k * 300.0);
        if (g_gs_draws) printf("nvn_vk_gs: frames %lld..%lld: draws with a geometry stage so far %lld, their pipelines %lld\n", (long long)frame - 300, (long long)frame, (long long)g_gs_draws, (long long)g_gs_pipes);
        if (g_ww_on > 0) ww_print();
        if (g_rq_queued) { printf("nvn_vk_reports: frames %lld..%lld: %lld report values waited in the queue for compute results (at most %lld at once)\n", (long long)frame - 300, (long long)frame, (long long)g_rq_queued, (long long)g_rq_max); g_rq_queued = g_rq_max = 0; }
        if (g_pe_on > 0) {
            printf("nvn_vk_passes: %.1f render passes per frame (%.1f settings of the same targets kept the pass); ended by:\n", (double)g_pe_total / 300.0, (double)g_targets_kept / 300.0);
            g_targets_kept = 0;
            for (int i = 0; i < 24 && g_pe[i].ra; i++) {
                if (g_pe[i].n) printf("nvn_vk_passes:   libsmo.so+0x%llx: %.1f per frame\n", (unsigned long long)((uintptr_t)g_pe[i].ra - plat_module_base((void *)ww_print)), (double)g_pe[i].n / 300.0);
                g_pe[i].n = 0;
            }
            g_pe_total = 0;
        }
        {   static int64_t lg[GEN_N], lw[GEN_N], lcalls, lhits, lagain, lagain_b, lbytes;
            double g[GEN_N], w[GEN_N];
            for (int i = 0; i < GEN_N; i++) { g[i] = (double)(g_gen_why[i] - lg[i]) / 300.0; w[i] = (double)(g_wlog_why[i] - lw[i]) / 300.0; }
            printf("nvn_vk_ring: frames %lld..%lld per frame: streamed %.2f MB, ring_put %.0f calls (%.1f%% cached), copied again within a batch %.0f ranges %.2f MB | new generations: batch %.1f, wrap %.2f, texture to buffer %.2f, write log full %.2f, index range written %.2f | host writes logged: copy-back %.1f, occlusion %.1f, UpdateUniformBuffer %.1f, report %.1f, buffer copy/clear %.1f\n",
                   (long long)frame - 300, (long long)frame, (double)(g_ring_bytes - lbytes) / 1048576.0 / 300.0, (double)(g_rput_calls - lcalls) / 300.0,
                   g_rput_calls > lcalls ? 100.0 * (double)(g_rput_hits - lhits) / (double)(g_rput_calls - lcalls) : 0.0, (double)(g_rput_again - lagain) / 300.0, (double)(g_rput_again_bytes - lagain_b) / 1048576.0 / 300.0,
                   g[GEN_BATCH], g[GEN_WRAP], g[GEN_TEXBUF], g[GEN_LOGFULL], g[GEN_INDEXW], w[GEN_CB], w[GEN_OCC], w[GEN_UBO], w[GEN_REPORT], w[GEN_BUF]);
            memcpy(lg, g_gen_why, sizeof lg); memcpy(lw, g_wlog_why, sizeof lw); lcalls = g_rput_calls; lhits = g_rput_hits; lagain = g_rput_again; lagain_b = g_rput_again_bytes; lbytes = g_ring_bytes;
        }
        if (frame % 3000 == 0) m4_report();
        mem_report(frame);
        memcpy(last, g_tm, sizeof last); last_sets = R.sets; last_flush = R.flushes; last_reused = R.sets_reused; last_disp = R.dispatches;
    }
    if (frame % 600 == 0)
        printf("nvn_vk: totals at frame %lld: %lld images made (%lld live, %lld views), %lld uploads (%lld MB), %lld shader modules, %lld pipelines, %lld descriptor sets, %lld command buffers, %lld ring wraps, %lld feedback passes, %lld MB device memory; images of textures nothing names freed %lld (%lld MB), waiting %d, made again %lld (first used after %lld)\n",
               (long long)frame, (long long)R.images, (long long)R.images_live, (long long)R.views, (long long)R.uploads, (long long)(R.upload_bytes >> 20), (long long)R.modules, (long long)R.pipelines, (long long)R.sets,
               (long long)R.flushes, (long long)R.ring_wraps, (long long)R.feedback, (long long)(g_mem_total >> 20), (long long)g_rc_freed, (long long)g_rc_freed_mb, g_rc_n, (long long)g_rc_again, (long long)g_rc_late);
    if (frame % 600 == 0 && R.dispatches) {
        plat_li fq; ((fq).QuadPart = plat_qpf());
        printf("nvn_vk: compute at frame %lld: %lld dispatches, %lld KB copied back to guest memory, draws using a vertex buffer written by compute in the same batch: %lld, copy-backs forced inside a batch: %lld, waits for the GPU before a copy-back: %lld (%.1f ms in all), %lld buffers made; words the guest wrote into a range pending copy-back: %lld (also written by compute: %lld)\n",
               (long long)frame, (long long)R.dispatches, (long long)(R.cb_bytes >> 10), (long long)R.cb_draws, (long long)R.cb_syncs, (long long)R.cb_waits, (double)R.cb_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)R.cb_made,
               (long long)R.cb_guest_words, (long long)R.cb_conflict_words);
    }
    if (frame % 600 == 0) { printf("nvn_vk: images made per NVN texture format (format:count):"); for (int i = 0; i < 256; i++) if (g_fmt_hist[i]) printf(" 0x%x:%u", i, g_fmt_hist[i]); printf("\n"); }
    {   static long long lo = -2, hi;
        if (lo == -2) { const char *e = getenv("SMO_VK_FRAMELOG"); lo = e ? atoll(e) : -1; hi = e && strchr(e, '-') ? atoll(strchr(e, '-') + 1) : lo; }
        if (lo >= 0 && frame >= lo && frame <= hi)
            printf("nvn_vk_frame: %lld draws %lld skipped %lld (render off %lld) conditional %lld (off %lld) passes %lld texture barriers %lld vertex over-reads %lld ring wraps %lld streamed %lld KB condhash %016llx\n", (long long)frame, (long long)R.frame_draws,
                   (long long)R.frame_skipped, (long long)g_fr_offskip, (long long)g_fr_cond, (long long)g_fr_condoff, (long long)g_fr_passes, (long long)g_fr_texbar, (long long)g_fr_vtxover, (long long)g_fr_wraps, (long long)(g_ring_bytes - g_fr_ring0) >> 10, (unsigned long long)g_fr_condhash);
        g_fr_cond = g_fr_condoff = g_fr_offskip = g_fr_passes = g_fr_texbar = g_fr_wraps = g_fr_vtxover = 0; g_fr_ring0 = g_ring_bytes; g_fr_condhash = 1469598103934665603ull;
    }
    R.frame_draws = R.frame_skipped = R.frame_clears = 0;
}
const RBackend rb_vk = {"vk", vk_submit, vk_submit_async, vk_present, vk_present_movie, vk_fence, vk_sync_wait, vk_server_wait, vk_sync_drop, vk_flush, vk_finish, vk_write_texels, vk_read_texels, vk_housekeeping,
                        vk_reset_status, vk_no_grid, vk_idle, vk_menu};

/* ---------------------------------------------------------------- start and end */
static VTex *dummy_image(VkImageType type, uint32_t layers, VkFlags flags) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    VkClearColorValue z;
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vt->magic = 0x56544558; vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = vt->h = vt->d = 1; vt->levels = 1; vt->layers = (int)layers; vt->samples = 1;
    ci.imageType = type; ci.format = vt->fmt; ci.extent.width = ci.extent.height = ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.flags = flags;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&z, 0, sizeof z);
    vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    return vt;
}
static VkImageView dummy_view(VTex *vt, VkImageViewType type, uint32_t layers) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VView *n = calloc(1, sizeof *n);
    ci.image = vt->img; ci.viewType = type; ci.format = vt->fmt; ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.levelCount = 1; ci.subresourceRange.layerCount = layers;
    VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
    n->vtype = 100 + (int)type; n->next = vt->views; vt->views = n;
    return n->view;
}
/* ---------------------------------------------------------------- M4: pipelines made before they are needed (design section 5, step 1)
 * At start the worker threads read ..\cache\vk\pipes.bin and make every pipeline listed there (stages from their records and
 * SPIR-V files, pipelines through the driver's pipeline cache kept in ..\cache\vk\pipelines_<gpu>.bin). The render thread
 * waits for them before it replays its first batch, so the tables are never shared with a running worker. */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's start-up workers) */
static HANDLE g_pw_thread[16];
static int g_pw_nthread, g_pw_n, g_pw_waited;
static PipeRec *g_pw_rec;
static volatile int32_t g_pw_next;
static int64_t g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
static VStage *pw_stage(int s, uint64_t h) {      /* g_sh_lock held */
    for (VStage *v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s) return v;
    VStage *v = vstage_from_rec(s, h);
    if (v) plat_atomic_inc(&g_pw_stages);
    return v;
}
static uint32_t  pw_main(void *arg) {
    (void)arg;
    for (;;) {
        int32_t i = plat_atomic_inc(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        plat_mutex_lock(&g_sh_lock);
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;
        plat_mutex_unlock(&g_sh_lock);
        if (!vs || (r->fs_h && !fs)) { plat_atomic_inc(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made at the draw as on a first run */
        if (r->compute) { compute_pipe_make(vs); plat_atomic_inc(&g_pw_pipes); continue; }
        double ms;
        r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
        Pipe *p = pipe_make(&r->k, vs, fs, &ms);
        uint64_t h = fnv(0xcbf29ce484222325ULL, &r->k, sizeof r->k);
        plat_mutex_lock(&g_sh_lock);
        p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
        plat_mutex_unlock(&g_sh_lock);
        plat_atomic_inc(&g_pw_pipes);
    }
    plat_atomic_xchg(&g_pw_t1, tk());
    return 0;
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    plat_mutex_init(&g_sh_lock);
    {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "../cache/vk/pipelines_%04x_%04x_%08x.bin", (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        printf("nvn_vk: pipeline cache %s: %llu KB read\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10));
        free(data);
    }
    if (getenv("SMO_VK_PREWARM") && !atoi(getenv("SMO_VK_PREWARM"))) { printf("nvn_vk: SMO_VK_PREWARM=0: no pipeline is made ahead\n"); return; }
    FILE *fp = fopen("../cache/vk/pipes.bin", "rb");
    if (!fp) { printf("nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC, every pipeline is made at the draw that first needs it\n"); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_pw_rec = malloc((size_t)n + 1);
    g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
    fclose(fp);
    int kept = 0;
    qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
    for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
    g_pw_n = kept;
    int si_n = plat_cpu_count();
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SMO_VK_WORKERS") && atoi(getenv("SMO_VK_WORKERS")) > 0 && atoi(getenv("SMO_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SMO_VK_WORKERS"));
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = plat_thread_start32(pw_main, NULL, 0, "pw_main");
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin: %d worker threads make them now\n", g_pw_n, g_pw_nthread);
}
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    if (!g_pw_nthread) return;
    plat_li fq; ((fq).QuadPart = plat_qpf());
    int64_t t0 = tk();
    plat_threads_wait_all(g_pw_thread, g_pw_nthread, PLAT_INFINITE);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(g_pw_rec); g_pw_rec = NULL;
}
static void pcache_save(void) {
    size_t n = 0;
    if (!g_pcache || vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (data && vkGetPipelineCacheData(V.device, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[176];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
        FILE *fp = fopen(tmp, "wb");
        if (fp) { fwrite(data, 1, n, fp); fclose(fp); plat_rename(tmp, g_pcache_path); printf("nvn_vk: pipeline cache saved: %llu KB\n", (unsigned long long)(n >> 10)); }
    }
    free(data);
}
#endif
#include "nvn_vk_wd.c"                             /* session 12: the freeze watchdog (wd_start / wd_stop) */
static void vkd_init(void) {
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has no " #n);
    VKD(X)
#undef X
    vkCmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(V.device, "vkCmdSetVertexInputEXT");
    if (D.has_push) { vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.device, "vkCmdPushDescriptorSetKHR"); if (!vkCmdPushDescriptorSetKHR) D.has_push = 0; }
    if (D.has_eds3) {
#define X3(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has VK_EXT_extended_dynamic_state3 but no " #n);
        X3(vkCmdSetColorBlendEnableEXT) X3(vkCmdSetColorBlendEquationEXT) X3(vkCmdSetColorWriteMaskEXT) X3(vkCmdSetPolygonModeEXT) X3(vkCmdSetAlphaToCoverageEnableEXT) X3(vkCmdSetSampleMaskEXT)
        if (D.f2.features.depthClamp) { X3(vkCmdSetDepthClampEnableEXT) }
#undef X3
    }
    if (!D.has_push && !(getenv("SMO_VK_DYN0") && !atoi(getenv("SMO_VK_DYN0")))) D.dyn0 = (int)(V.props.limits.maxDescriptorSetStorageBuffersDynamic < 64 ? V.props.limits.maxDescriptorSetStorageBuffersDynamic : 64);
    if (D.dyn0) printf("nvn_vk: descriptors: buffers (set 0) one set per distinct buffer list per command buffer, up to %d of them at dynamic offsets (the device allows %u), textures (set 1) one set per distinct binding list per command buffer\n", D.dyn0, V.props.limits.maxDescriptorSetStorageBuffersDynamic);
    else printf("nvn_vk: descriptors: buffers (set 0) %s, textures (set 1) one set per distinct binding list per command buffer\n", D.has_push ? "pushed (VK_KHR_push_descriptor)" : "one allocated set per draw");
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &D.mp);
    for (int i = 0; i < NSLOT; i++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pi.queueFamilyIndex = V.family;     /* one pool per slot, reset whole (cmd_begin) */
        VKC(vkCreateCommandPool(V.device, &pi, NULL, &R.slot[i].cpool));
        ai.commandPool = R.slot[i].cpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(V.device, &ai, &R.slot[i].cmd));
        VKC(vkCreateFence(V.device, &fi, NULL, &R.slot[i].fence));
        {   static int evp = -1;
            if (evp < 0) { const char *e = getenv("SMO_VK_EVENT_POLL");
#ifdef __ANDROID__
                evp = e ? atoi(e) != 0 : 1;
#else
                evp = e ? atoi(e) != 0 : 0;
#endif
                printf("nvn_vk: finished submissions are seen %s\n", evp ? "from an event each command buffer sets at its end (SMO_VK_EVENT_POLL=0: fence status)" : "from fence status"); }
            VkEventCreateInfo ei = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            if (evp) VKC(vkCreateEvent(V.device, &ei, NULL, &R.slot[i].ev));
        }
    }
    R.ring_size = 256ull << 20;
    R.ring = vbuf_new(R.ring_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &R.ring_mem);
    R.zero = vbuf_new(65536 + 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &R.zero_mem);
    memset(R.zero_mem.map, 0, 65536 + 64);
    { float f1 = 1.0f; uint32_t i1 = 1; memcpy((uint8_t *)R.zero_mem.map + 65536 + 12, &f1, 4); memcpy((uint8_t *)R.zero_mem.map + 65536 + 28, &i1, 4); }
    R.gen = 1; R.point_size = 1.0f; R.line_width = 1.0f; R.drange[1] = 1.0f; R.sample_mask = 0xffffffffu; R.chanmask = 0xffffffffu; R.logic = 3; R.poly[2] = 2; R.poly[1] = 1;
    R.svmask[0] = R.svmask[1] = R.swmask[0] = R.swmask[1] = 0xff; R.ds = 0x10 * 2;
    for (int i = 0; i < 8; i++) { R.blend[i][0] = 2; R.blend[i][1] = 1; R.blend[i][2] = 2; R.blend[i][3] = 1; R.blend[i][4] = 1; R.blend[i][5] = 1; }
    cmd_begin();
    g_dummy.d2 = dummy_image(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT); g_dummy.d1 = dummy_image(VK_IMAGE_TYPE_1D, 1, 0); g_dummy.d3 = dummy_image(VK_IMAGE_TYPE_3D, 1, 0);
    g_dummy.v[DIM_1D] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D, 1); g_dummy.v[DIM_1DA] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
    g_dummy.v[DIM_2D] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D, 1); g_dummy.v[DIM_2DA] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
    g_dummy.v[DIM_CUBE] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE, 6); g_dummy.v[DIM_CUBEA] = D.f2.features.imageCubeArray ? dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6) : g_dummy.v[DIM_CUBE];
    g_dummy.v[DIM_3D] = dummy_view(g_dummy.d3, VK_IMAGE_VIEW_TYPE_3D, 1);
    {   /* a texture bound without a sampler: the texture's own OpenGL parameters (nearest, nearest mip, repeat) */
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.maxLod = 1000.0f; ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VKC(vkCreateSampler(V.device, &ci, NULL, &g_smp_default));
    }
    cmd_wait_all();
    plat_mkdirs("../cache/vk");
    prewarm_start();
    wd_start();
    printf("nvn_vk: drawing part ready: %d command buffers in flight, stream ring %llu MB, %u memory types; debug checks %s\n", NSLOT, (unsigned long long)(R.ring_size >> 20), (unsigned)D.mp.memoryTypeCount, D.debug ? "on" : "off (SMO_VK_DEBUG=1)");
    if (RS_ON) printf("nvn_vk_scale: internal resolution scale %.3f (%s): screen-class targets (SMO: 1600x900, 800x450, ...; 1280x720 handheld) are made %.2f times their size, e.g. %d x %d instead of 1600 x 900; fragment stages are another translation\n",
                      (double)rs_scale(), getenv("SMO_VK_SCALE") ? "SMO_VK_SCALE" : "graphics.ini", (double)rs_scale(), (int)(1600.0f * rs_scale() + 0.5f), (int)(900.0f * rs_scale() + 0.5f));
    fflush(stdout);
}
static int frstat_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* Session 9: battle frames only (dispatches and more than 120 draws: the fight and the result screen's fighters); the first 30 are left out (their buffers were recorded
 * while the stage was still being set up). GPU ms = sum of the spans of the frame's command buffers (SMO_VK_GPUTIME=1). */
static void frstat_report(void) {
    static float g[FRSTAT_N], w[FRSTAT_N];
    int n = 0, seen = 0, slow = 0;
    double wsum = 0, gsum = 0;
    for (int f = 0; f < FRSTAT_N; f++) {
        if (!g_fr_fight[f] || seen++ < 30) continue;
        g[n] = g_fr_gpu[f]; w[n] = g_fr_wall[f]; wsum += w[n]; gsum += g[n]; if (w[n] > 33.4f) slow++; n++;
    }
    if (n < 100) return;
    qsort(g, (size_t)n, sizeof g[0], frstat_cmp); qsort(w, (size_t)n, sizeof w[0], frstat_cmp);
    printf("nvn_vk_gpu_fight: %d battle frames: GPU ms per frame median %.2f, p99 %.2f, max %.2f, mean %.2f%s | time between presents: median %.2f ms, p99 %.2f, max %.2f, %d frames over 33.4 ms | %.1f frames per second over the battle\n",
           n, (double)g[n / 2], (double)g[n * 99 / 100], (double)g[n - 1], gsum / n, g_ts_on > 0 ? "" : " (SMO_VK_GPUTIME not set: no GPU times)",
           (double)w[n / 2], (double)w[n * 99 / 100], (double)w[n - 1], slow, wsum > 0 ? 1000.0 * n / wsum : 0.0);
}
static void vkd_shutdown(void) {
    wd_stop();
    if (!R.ring) return;
    {   plat_li fq; ((fq).QuadPart = plat_qpf());
        double k = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_tex: textures made ahead of their first use: %lld (%.0f MB, %.0f ms in all, at most %.1f ms after one present); made or read at their use: %lld times, %.0f ms (of them in fight frames: %lld, %.0f ms)\n",
               (long long)g_ta_n, (double)g_ta_bytes / 1048576.0, (double)g_ta_tk * k, (double)g_ta_max_tk * k, (long long)g_late_n, (double)g_late_tk * k, (long long)g_late_fight_n, (double)g_late_fight_tk * k);
    }
    frstat_report();
    if (RS_ON && rs_ui_native()) printf("nvn_vk_scale: SMO_VK_UI_NATIVE=1: the window's textures are kept at the game's size; passes with such a colour target and a SCALED depth target (drawn without depth): %lld\n", (long long)g_rs_mixed);
    if (RS_ON) printf("nvn_vk_scale: resolution scale %.3f: %lld images made scaled, %lld fragment stages translated in this run for it (%lld of them read the pixel position, %lld fetch texels by integer position), %lld uploads into and %lld read-backs from a scaled image, %lld copies / blits touching one\n",
                      (double)rs_scale(), (long long)g_rs_images, (long long)g_rs_stages, (long long)g_rs_fcs, (long long)g_rs_tf, (long long)g_rs_up, (long long)g_rs_down, (long long)g_rs_blits);
    m4_stop();
    pcache_save();
    if (g_pcache) { vkDestroyPipelineCache(V.device, g_pcache, NULL); g_pcache = NULL; }
    for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]);
    R.done = ~0ull; garbage_collect();
    g_keep_n = 0; g_keep_bytes = 0;                /* the kept images are in the list below */
    while (g_vtex_all) { VTex *vt = g_vtex_all; if (vt->t) vt->t->vk = NULL; vt_destroy(vt); }
    for (int i = 0; i < 1024; i++) for (Pipe *p = g_pipes[i], *n; p; p = n) { n = p->next; vkDestroyPipeline(V.device, p->p, NULL); if (p->p_fast) vkDestroyPipeline(V.device, p->p_fast, NULL); if (!p->libbed) { vkDestroyPipelineLayout(V.device, p->layout, NULL); if (!(p->dl0_shared & 1)) vkDestroyDescriptorSetLayout(V.device, p->dl[0], NULL); if (!(p->dl0_shared & 2)) vkDestroyDescriptorSetLayout(V.device, p->dl[1], NULL); } free(p); }
    gpl_destroy();
    for (int i = 0; i < 8; i++) for (int k = 0; k < 2; k++) if (g_sem[i][k]) { vkDestroySemaphore(V.device, g_sem[i][k], NULL); g_sem[i][k] = NULL; }
    for (int i = 0; i < 16; i++) if (g_sem_img[i]) { vkDestroySemaphore(V.device, g_sem_img[i], NULL); g_sem_img[i] = NULL; }
    if (g_ovl.made) { vkDestroyImage(V.device, g_ovl.img, NULL); g_ovl.made = 0; }      /* the progress bar's image; its memory goes with the blocks */
    for (int i = 0; i < g_cb_used; i++) if (g_cb[i].buf) { vkDestroyBuffer(V.device, g_cb[i].buf, NULL); g_cb[i].buf = NULL; g_cb[i].state = 0; }
    g_cb_live = g_cb_pending = g_ncdisp = 0;
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i], *n; v; v = n) {
        n = v->next;
        if (v->cp) { vkDestroyPipeline(V.device, v->cp, NULL); vkDestroyPipelineLayout(V.device, v->cl, NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[0], NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[1], NULL); }
        if (v->mod) vkDestroyShaderModule(V.device, v->mod, NULL);
        free(v);
    }
    for (int i = 0; i < g_nsmp; i++) vkDestroySampler(V.device, g_smp[i].s, NULL);
    vkDestroySampler(V.device, g_smp_default, NULL);
    if (g_ts_pool) { vkDestroyQueryPool(V.device, g_ts_pool, NULL); g_ts_pool = NULL; g_ts_on = 0; }
    if (g_occ_pool) { vkDestroyQueryPool(V.device, g_occ_pool, NULL); g_occ_pool = NULL; g_occ_qn = 0; }
    for (int i = 0; i < NSLOT; i++) { for (int k = 0; k < R.slot[i].npool; k++) vkDestroyDescriptorPool(V.device, R.slot[i].pool[k], NULL); vkDestroyFence(V.device, R.slot[i].fence, NULL); if (R.slot[i].cpool) vkDestroyCommandPool(V.device, R.slot[i].cpool, NULL); }
    vkDestroyBuffer(V.device, R.ring, NULL); vkDestroyBuffer(V.device, R.zero, NULL);
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) { vkFreeMemory(V.device, g_blk[b]->mem, NULL); free(g_blk[b]); g_blk[b] = NULL; }
    R.ring = NULL;
    printf("nvn_vk: drawing part shut down: %lld images, %lld pipelines, %lld shader modules, %d samplers destroyed; %lld bytes of block space lost to fragmentation records\n", (long long)R.images, (long long)R.pipelines, (long long)R.modules, g_nsmp, (long long)g_mem_leak);
    printf("nvn_vk: draws reading vertices past the range the game bound: %lld%s, %lld left as bound (over 16 MB more); index ranges scanned for their largest index: %lld\n",
           (long long)g_vtx_over, g_vtxgrow > 0 ? " (copied as far as they read)" : g_vtxgrow == 0 ? " (SMO_VK_VTXGROW=0: left as bound)" : "", (long long)g_vtx_capped, (long long)g_imax_scans);
    printf("nvn_vk: texture barriers (nvnCommandBufferBarrier, order fragments): %lld, %lld of them inside a pass that samples its own target (the pass ended there), %lld inside other passes (nothing to do)\n", (long long)g_texbar, (long long)g_texbar_split, (long long)g_texbar_skip);
    printf("nvn_vk: stream ring: %lld wraps, %lld waits for a submission still reading the part about to be written%s\n", (long long)R.ring_wraps, (long long)g_ring_waits, g_ring_drain > 0 ? " (SMO_VK_RING_DRAIN=1: every wrap waited for the GPU)" : "");
    printf("nvn_vk: images: %lld textures made with storage use (NVN_TEXTURE_FLAGS_IMAGE), %lld storage views, %lld dispatches that wrote images; draws with a geometry stage: %lld (%lld pipelines); indirect: %lld draws, %lld dispatches\n", (long long)g_storage_tex, (long long)g_img_views, (long long)g_img_disp, (long long)g_gs_draws, (long long)g_gs_pipes, (long long)g_indirect_draws, (long long)g_indirect_disp);
    printf("nvn_vk: textures nothing names (agl gave up their pool slots; SMO_VK_TEXFREE=%d frames unused): %lld images freed (%lld MB), %d waiting at the end, %lld used again after their image went (made again), %lld first used after\n",
           texfree_frames(), (long long)g_rc_freed, (long long)g_rc_freed_mb, g_rc_n, (long long)g_rc_again, (long long)g_rc_late);
    if (g_occ_reports || g_occ_folds) {
        plat_li fq; ((fq).QuadPart = plat_qpf());
        printf("nvn_vk: occlusion counts: %lld reports (%lld written after their frame was presented), %lld compared by a conditional render (%lld of them waited for); "
               "%lld waits for the GPU, %.0f ms in all (%lld at a present%s); %lld long counts folded%s\n", (long long)g_occ_reports, (long long)g_occ_late, (long long)g_occ_cond,
               (long long)g_occ_cond_waits, (long long)g_occ_waits, (double)g_occ_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)g_occ_present_waits,
               g_occ_present ? "" : ", off: SMO_VK_OCC_PRESENT=0", (long long)g_occ_folds, g_occ_sync ? "; SMO_VK_OCC_SYNC=1: each report waited for" : "");
        printf("nvn_vk: conditional renders not waited for: %lld on the counts of the frame before, %lld on the last decision for the address\n", (long long)g_occ_cond_late, (long long)g_occ_cond_kept);
    }
}
