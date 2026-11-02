// Special K-style SDR->HDR grade mod.
//
// When the engine runs in an HDR output mode, the composed frame (after HUD) is resolved, run
// through a Special K-style grade in res/hdr.wgsl (EOTF, perceptual/color boost, saturation,
// gamut expansion) and written back gamma-encoded relative to paper white, so the engine's
// normal HDR decode restores the graded values and SDR copies of the frame (menu backdrops)
// stay valid. The mod passes its paper white/peak to the engine via set_hdr_frame.
// With HDR output off the mod idles.

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/log.h"
#include "mods/svc/log.hpp"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"
#include "settings.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <webgpu/webgpu.h>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);

namespace {

ResourceBuffer g_shaderSource = RESOURCE_BUFFER_INIT;
GfxDeviceInfo g_deviceInfo = GFX_DEVICE_INFO_INIT;
WGPUSampler g_sampler = nullptr;
WGPURenderPipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_bindLayout = nullptr;
uint64_t g_layoutKey = 0;
GfxDrawTypeHandle g_drawType = 0;
GfxStageHookHandle g_frameHook = 0;
bool g_warnedResolve = false;
std::atomic<bool> g_warnedPipeline{false};

// Must match struct Params in res/hdr.wgsl.
struct alignas(16) GradeParams {
    float eotf = 2.2f;
    uint32_t pb_enable = 0;
    uint32_t pad0[2] = {}; // WGSL vec4<f32> is 16-byte aligned
    float pb[4] = {1000.f, 100.f, 1273.f, 500.f};
    float color_boost = 0.333f;
    float saturation = 1.f;
    float gamut = 0.01f;
    float luma_scale = 2.5f;
    uint32_t debug_stage = 0;
};
static_assert(offsetof(GradeParams, pb) == 16 && offsetof(GradeParams, luma_scale) == 44 &&
              offsetof(GradeParams, debug_stage) == 48 && sizeof(GradeParams) == 64,
    "GradeParams must match the WGSL Params layout");

struct DrawPayload {
    WGPUTextureView color;
    uint32_t uniform_offset;
    uint32_t uniform_size;
};

WGPUShaderModule create_shader_module(const char* label, const ResourceBuffer& source) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};
    WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain = &wgsl.chain;
    moduleDesc.label = {label, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_deviceInfo.device, &moduleDesc);
}

void release_pipeline() {
    if (g_pipeline != nullptr) {
        wgpuRenderPipelineRelease(g_pipeline);
        g_pipeline = nullptr;
    }
    if (g_bindLayout != nullptr) {
        wgpuBindGroupLayoutRelease(g_bindLayout);
        g_bindLayout = nullptr;
    }
    g_layoutKey = 0;
}

// Rebuild when the frame pass layout changes (resolution, format, MSAA, sample count).
bool ensure_pipeline(const GfxRenderTargetLayout& layout) {
    if (g_pipeline != nullptr && g_layoutKey == layout.key) {
        return true;
    }
    release_pipeline();

    WGPUShaderModule module = create_shader_module("HDR grade", g_shaderSource);
    if (module == nullptr) {
        return false;
    }

    WGPUColorTargetState colorTargets[GFX_MAX_COLOR_ATTACHMENTS];
    const uint32_t colorTargetCount =
        gfx_init_color_target_states(&layout, colorTargets, nullptr, WGPUColorWriteMask_All);
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = colorTargetCount;
    fragment.targets = colorTargets;
    // Depth state must match the frame pass when it has one; the composed frame target
    // usually has no depth attachment at all.
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    const bool hasDepth = layout.depth_stencil_format != WGPUTextureFormat_Undefined;
    depthStencil.format = layout.depth_stencil_format;
    depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
    depthStencil.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDesc.label = {"HDR grade", WGPU_STRLEN};
    pipelineDesc.vertex.module = module;
    pipelineDesc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.depthStencil = hasDepth ? &depthStencil : nullptr;
    pipelineDesc.multisample.count = layout.sample_count;
    pipelineDesc.fragment = &fragment;
    g_pipeline = wgpuDeviceCreateRenderPipeline(g_deviceInfo.device, &pipelineDesc);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr) {
        g_warnedPipeline.store(true, std::memory_order_relaxed);
        release_pipeline();
        return false;
    }
    g_bindLayout = wgpuRenderPipelineGetBindGroupLayout(g_pipeline, 0);
    if (g_bindLayout == nullptr) {
        release_pipeline();
        return false;
    }
    g_layoutKey = layout.key;
    return true;
}

// Render worker: fullscreen grade of the resolved frame, no blend.
void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payloadSize, void*) {
    if (payloadSize != sizeof(DrawPayload) || !ensure_pipeline(ctx->layout)) {
        return;
    }
    DrawPayload data;
    std::memcpy(&data, payload, sizeof(data));
    if (data.color == nullptr) {
        return;
    }

    const WGPUBindGroupEntry entries[3] = {
        {.binding = 0,
            .buffer = ctx->uniform_buffer,
            .offset = data.uniform_offset,
            .size = data.uniform_size},
        {.binding = 1, .textureView = data.color},
        {.binding = 2, .sampler = g_sampler},
    };
    WGPUBindGroupDescriptor bindGroupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDesc.layout = g_bindLayout;
    bindGroupDesc.entryCount = 3;
    bindGroupDesc.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(ctx->device, &bindGroupDesc);
    if (bindGroup == nullptr) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, g_pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(bindGroup);
}

// Game thread, after HUD: grade the frame and hand the engine the matching paper white.
void on_frame_after_hud(ModContext*, const GfxStageContext*, void*) {
    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = false;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr)
    {
        if (!g_warnedResolve) {
            g_warnedResolve = true;
            svc_log->warn(mod_ctx, "color snapshot unavailable; grade idle");
        }
        return;
    }

    GradeParams params{};
    params.eotf = hdr::cvar_int(hdr::cvar_eotf, 0) == 1 ? -2.2f : 2.2f;
    params.pb_enable = hdr::cvar_bool(hdr::cvar_pb_enable, true) ? 1u : 0u;
    // Special K's raw PQBoost0..3 (v1 defaults 1.0, 0.1, 1.273, 0.5), stored x1000.
    params.pb[0] = hdr::cvar_int(hdr::cvar_pb0, 1000) / 1000.0f;
    params.pb[1] = hdr::cvar_int(hdr::cvar_pb1, 100) / 1000.0f;
    params.pb[2] = hdr::cvar_int(hdr::cvar_pb2, 1273) / 1000.0f;
    params.pb[3] = hdr::cvar_int(hdr::cvar_pb3, 500) / 1000.0f;
    params.color_boost = hdr::cvar_int(hdr::cvar_color_boost, 333) / 1000.0f;
    params.saturation = hdr::cvar_int(hdr::cvar_saturation, 1000) / 1000.0f;
    params.gamut = hdr::cvar_int(hdr::cvar_gamut, 10) / 1000.0f;
    params.luma_scale = hdr::cvar_int(hdr::cvar_paper_white, 200) / 80.0f;
    params.debug_stage = static_cast<uint32_t>(hdr::cvar_int(hdr::cvar_debug_view, 0));

    GfxRange uniformRange{0, 0};
    if (svc_gfx->push_uniform(mod_ctx, &params, sizeof(params), &uniformRange) != MOD_OK) {
        return;
    }
    const DrawPayload drawPayload{resolved.color, uniformRange.offset, uniformRange.size};
    if (svc_gfx->push_draw(mod_ctx, g_drawType, &drawPayload, sizeof(drawPayload)) != MOD_OK) {
        return;
    }

    GfxHdrFrameDesc frameDesc = GFX_HDR_FRAME_DESC_INIT;
    frameDesc.scene_linear = false; // encode_out in hdr.wgsl keeps the engine's gamma convention
    frameDesc.unclip_lighting = hdr::cvar_bool(hdr::cvar_unclip_lighting, false);
    frameDesc.paper_white_nits = static_cast<float>(hdr::cvar_int(hdr::cvar_paper_white, 200));
    frameDesc.peak_nits = static_cast<float>(hdr::cvar_int(hdr::cvar_peak, 1000));
    svc_gfx->set_hdr_frame(mod_ctx, &frameDesc);
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = hdr::settings_register(error);
    if (result != MOD_OK) {
        return result;
    }

    GfxHdrMode mode = GFX_HDR_OFF;
    if (svc_gfx->get_hdr_mode(mod_ctx, &mode) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query HDR mode");
    }
    if (mode == GFX_HDR_OFF) {
        svc_log->info(mod_ctx, "HDR output is off in Graphics settings; mod idle");
        return MOD_OK;
    }

    result = svc_resource->load(mod_ctx, "hdr.wgsl", &g_shaderSource);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load HDR grade shader");
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_deviceInfo) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query device info");
    }

    WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samplerDesc.label = {"HDR grade sampler", WGPU_STRLEN};
    samplerDesc.magFilter = WGPUFilterMode_Linear;
    samplerDesc.minFilter = WGPUFilterMode_Linear;
    samplerDesc.addressModeU = WGPUAddressMode_ClampToEdge;
    samplerDesc.addressModeV = WGPUAddressMode_ClampToEdge;
    g_sampler = wgpuDeviceCreateSampler(g_deviceInfo.device, &samplerDesc);
    if (g_sampler == nullptr) {
        return mods::set_error(error, MOD_ERROR, "failed to create sampler");
    }

    GfxDrawTypeDesc drawDesc = GFX_DRAW_TYPE_DESC_INIT;
    drawDesc.label = "HDR grade";
    drawDesc.draw = on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &drawDesc, &g_drawType) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register draw type");
    }
    GfxStageHookDesc stageDesc = GFX_STAGE_HOOK_DESC_INIT;
    stageDesc.callback = on_frame_after_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_AFTER_HUD, &stageDesc, &g_frameHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register stage hook");
    }

    mods::log::info("HDR grade active (output mode {})", mode == GFX_HDR_HDR10 ? 1 : 2);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (g_warnedPipeline.exchange(false, std::memory_order_relaxed)) {
        svc_log->warn(mod_ctx, "HDR grade pipeline creation failed; grade idle");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_resource->free(mod_ctx, &g_shaderSource);
    release_pipeline();
    if (g_sampler != nullptr) {
        wgpuSamplerRelease(g_sampler);
        g_sampler = nullptr;
    }
    return MOD_OK;
}
}
