// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>

#include "common/config.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Vulkan {

using Shader::LogicalStage;
using Shader::Stage;
using Shader::VsOutput;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformTexelBuffer, 128},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageTexelBuffer, 128},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

void GatherVertexOutputs(Shader::VertexRuntimeInfo& info,
                         const AmdGpu::Liverpool::VsOutputControl& ctl) {
    const auto add_output = [&](VsOutput x, VsOutput y, VsOutput z, VsOutput w) {
        if (x != VsOutput::None || y != VsOutput::None || z != VsOutput::None ||
            w != VsOutput::None) {
            info.outputs[info.num_outputs++] = Shader::VsOutputMap{x, y, z, w};
        }
    };
    // VS_OUT_MISC_VEC
    add_output(ctl.use_vtx_point_size ? VsOutput::PointSprite : VsOutput::None,
               ctl.use_vtx_edge_flag
                   ? VsOutput::EdgeFlag
                   : (ctl.use_vtx_gs_cut_flag ? VsOutput::GsCutFlag : VsOutput::None),
               ctl.use_vtx_kill_flag
                   ? VsOutput::KillFlag
                   : (ctl.use_vtx_render_target_idx ? VsOutput::GsMrtIndex : VsOutput::None),
               ctl.use_vtx_viewport_idx ? VsOutput::GsVpIndex : VsOutput::None);
    // VS_OUT_CCDIST0
    add_output(ctl.IsClipDistEnabled(0)
                   ? VsOutput::ClipDist0
                   : (ctl.IsCullDistEnabled(0) ? VsOutput::CullDist0 : VsOutput::None),
               ctl.IsClipDistEnabled(1)
                   ? VsOutput::ClipDist1
                   : (ctl.IsCullDistEnabled(1) ? VsOutput::CullDist1 : VsOutput::None),
               ctl.IsClipDistEnabled(2)
                   ? VsOutput::ClipDist2
                   : (ctl.IsCullDistEnabled(2) ? VsOutput::CullDist2 : VsOutput::None),
               ctl.IsClipDistEnabled(3)
                   ? VsOutput::ClipDist3
                   : (ctl.IsCullDistEnabled(3) ? VsOutput::CullDist3 : VsOutput::None));
    // VS_OUT_CCDIST1
    add_output(ctl.IsClipDistEnabled(4)
                   ? VsOutput::ClipDist4
                   : (ctl.IsCullDistEnabled(4) ? VsOutput::CullDist4 : VsOutput::None),
               ctl.IsClipDistEnabled(5)
                   ? VsOutput::ClipDist5
                   : (ctl.IsCullDistEnabled(5) ? VsOutput::CullDist5 : VsOutput::None),
               ctl.IsClipDistEnabled(6)
                   ? VsOutput::ClipDist6
                   : (ctl.IsCullDistEnabled(6) ? VsOutput::CullDist6 : VsOutput::None),
               ctl.IsClipDistEnabled(7)
                   ? VsOutput::ClipDist7
                   : (ctl.IsCullDistEnabled(7) ? VsOutput::CullDist7 : VsOutput::None));
}

Shader::RuntimeInfo PipelineCache::BuildRuntimeInfo(Stage stage, LogicalStage l_stage) {
    auto info = Shader::RuntimeInfo{stage};
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.num_user_data = program.settings.num_user_regs;
        info.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.num_allocated_vgprs = program.settings.num_vgprs * 4;
        info.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.fp_round_mode32 = program.settings.fp_round_mode32;
    };
    switch (stage) {
    case Stage::Local: {
        BuildCommon(regs.ls_program);
        if (regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Hull))) {
            info.ls_info.links_with_tcs = true;
            Shader::TessellationDataConstantBuffer tess_constants;
            const auto* pgm = regs.ProgramForStage(static_cast<u32>(Stage::Hull));
            const auto params = Liverpool::GetParams(*pgm);
            const auto& hull_info = program_cache.at(params.hash)->info;
            hull_info.ReadTessConstantBuffer(tess_constants);
            info.ls_info.ls_stride = tess_constants.m_lsStride;
        }
        break;
    }
    case Stage::Hull: {
        BuildCommon(regs.hs_program);
        // TODO: ls_hs_config.output_control_points seems to be == 1 when doing passthrough
        // instead of the real number which matches the input patch topology
        // info.hs_info.output_control_points = regs.ls_hs_config.hs_output_control_points.Value();

        // TODO dont rely on HullStateConstants
        info.hs_info.output_control_points = regs.hs_constants.num_output_cp;
        info.hs_info.tess_factor_stride = regs.hs_constants.tess_factor_stride;

        // We need to initialize most hs_info fields after finding the V# with tess constants
        break;
    }
    case Stage::Export: {
        BuildCommon(regs.es_program);
        info.es_info.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    }
    case Stage::Vertex: {
        BuildCommon(regs.vs_program);
        GatherVertexOutputs(info.vs_info, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == Liverpool::ClipSpace::MinusWToW;
        if (l_stage == LogicalStage::TessellationEval) {
            info.vs_info.tess_type = regs.tess_config.type;
            info.vs_info.tess_topology = regs.tess_config.topology;
            info.vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Geometry: {
        BuildCommon(regs.gs_program);
        auto& gs_info = info.gs_info;
        gs_info.output_vertices = regs.vgt_gs_max_vert_out;
        gs_info.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        gs_info.in_primitive = regs.primitive_type;
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            gs_info.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        gs_info.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        gs_info.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        const auto params_vc = Liverpool::GetParams(regs.vs_program);
        gs_info.vs_copy = params_vc.code;
        gs_info.vs_copy_hash = params_vc.hash;
        DumpShader(gs_info.vs_copy, gs_info.vs_copy_hash, Shader::Stage::Vertex, 0, "copy.bin");
        break;
    }
    case Stage::Fragment: {
        BuildCommon(regs.ps_program);
        info.fs_info.en_flags = regs.ps_input_ena;
        info.fs_info.addr_flags = regs.ps_input_addr;
        const auto& ps_inputs = regs.ps_inputs;
        info.fs_info.num_inputs = regs.num_interp;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset.Value()),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.fs_info.color_buffers[i] = {
                .num_format = graphics_key.color_num_formats[i],
                .mrt_swizzle = static_cast<Shader::MrtSwizzle>(graphics_key.mrt_swizzles[i]),
            };
        }
        break;
    }
    case Stage::Compute: {
        const auto& cs_pgm = regs.cs_program;
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = regs.cs_program.settings.num_vgprs * 4;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .supported_spirv = instance.ApiVersion() >= VK_API_VERSION_1_3 ? 0x00010600U : 0x00010500U,
        .subgroup_size = instance.SubgroupSize(),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_explicit_workgroup_layout = true,
        .support_legacy_vertex_attributes = instance_.IsLegacyVertexAttributesSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
    };
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
}

PipelineCache::~PipelineCache() = default;

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline() {
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (is_new) {
        it.value() = graphics_pipeline_pool.Create(instance, scheduler, desc_heap, graphics_key,
                                                   *pipeline_cache, infos, fetch_shader, modules);
    }
    return it->second;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        it.value() = compute_pipeline_pool.Create(instance, scheduler, desc_heap, *pipeline_cache,
                                                  compute_key, *infos[0], modules[0]);
    }
    return it->second;
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));

    auto& regs = liverpool->regs;
    auto& key = graphics_key;

    key.depth_stencil = regs.depth_control;
    key.depth_stencil.depth_write_enable.Assign(regs.depth_control.depth_write_enable.Value() &&
                                                !regs.depth_render_control.depth_clear_enable);
    key.depth_bias_enable = regs.polygon_control.NeedsBias();

    const auto& db = regs.depth_buffer;
    const auto ds_format = instance.GetSupportedFormat(
        LiverpoolToVK::DepthFormat(db.z_info.format, db.stencil_info.format),
        vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    if (db.z_info.format != AmdGpu::Liverpool::DepthBuffer::ZFormat::Invalid) {
        key.depth_format = ds_format;
    } else {
        key.depth_format = vk::Format::eUndefined;
    }
    if (regs.depth_control.depth_enable) {
        key.depth_stencil.depth_enable.Assign(key.depth_format != vk::Format::eUndefined);
    }
    key.stencil = regs.stencil_control;

    if (db.stencil_info.format != AmdGpu::Liverpool::DepthBuffer::StencilFormat::Invalid) {
        key.stencil_format = key.depth_format;
    } else {
        key.stencil_format = vk::Format::eUndefined;
    }
    if (key.depth_stencil.stencil_enable) {
        key.depth_stencil.stencil_enable.Assign(key.stencil_format != vk::Format::eUndefined);
    }
    key.prim_type = regs.primitive_type;
    key.enable_primitive_restart = regs.enable_primitive_restart & 1;
    key.primitive_restart_index = regs.primitive_restart_index;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.cull_mode = regs.polygon_control.CullingMode();
    key.clip_space = regs.clipper_control.clip_space;
    key.front_face = regs.polygon_control.front_face;
    key.num_samples = regs.aa_config.NumSamples();

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::Liverpool::ColorControl::OperationMode::Disable;

    // `RenderingInfo` is assumed to be initialized with a contiguous array of valid color
    // attachments. This might be not a case as HW color buffers can be bound in an arbitrary
    // order. We need to do some arrays compaction at this stage
    key.color_formats.fill(vk::Format::eUndefined);
    key.color_num_formats.fill(AmdGpu::NumberFormat::Unorm);
    key.blend_controls.fill({});
    key.write_masks.fill({});
    key.mrt_swizzles.fill(Liverpool::ColorBuffer::SwapMode::Standard);
    key.vertex_buffer_formats.fill(vk::Format::eUndefined);

    key.patch_control_points = 0;
    if (regs.stage_enable.hs_en.Value() && !instance.IsPatchControlPointsDynamicState()) {
        key.patch_control_points = regs.ls_hs_config.hs_input_control_points.Value();
    }

    // First pass of bindings check to idenitfy formats and swizzles and pass them to rhe shader
    // recompiler.
    for (auto cb = 0u, remapped_cb = 0u; cb < Liverpool::NumColorBuffers; ++cb) {
        auto const& col_buf = regs.color_buffers[cb];
        if (skip_cb_binding || !col_buf) {
            continue;
        }
        const auto base_format =
            LiverpoolToVK::SurfaceFormat(col_buf.info.format, col_buf.NumFormat());
        key.color_formats[remapped_cb] =
            LiverpoolToVK::AdjustColorBufferFormat(base_format, col_buf.info.comp_swap.Value());
        key.color_num_formats[remapped_cb] = col_buf.NumFormat();
        if (base_format == key.color_formats[remapped_cb]) {
            key.mrt_swizzles[remapped_cb] = col_buf.info.comp_swap.Value();
        }

        ++remapped_cb;
    }

    fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto& TryBindStage = [&](Shader::Stage stage_in, Shader::LogicalStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto& bininfo = Liverpool::GetBinaryInfo(*pgm);
        if (!bininfo.Valid()) {
            LOG_WARNING(Render_Vulkan, "Invalid binary info structure!");
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        auto params = Liverpool::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(infos[stage_out_idx], modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            fetch_shader = fetch_shader_;
        }
        return true;
    };

    const auto& IsGsFeaturesSupported = [&]() -> bool {
        // These checks are temporary until all functionality is implemented.
        return !regs.vgt_gs_mode.onchip && !regs.vgt_strmout_config.raw;
    };

    infos.fill(nullptr);
    TryBindStage(Stage::Fragment, LogicalStage::Fragment);

    const auto* fs_info = infos[static_cast<u32>(LogicalStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;

    switch (regs.stage_enable.raw) {
    case Liverpool::ShaderStageEnable::VgtStages::EsGs: {
        if (!instance.IsGeometryStageSupported() || !IsGsFeaturesSupported()) {
            return false;
        }
        if (!TryBindStage(Stage::Export, LogicalStage::Vertex)) {
            return false;
        }
        if (!TryBindStage(Stage::Geometry, LogicalStage::Geometry)) {
            return false;
        }
        break;
    }
    case Liverpool::ShaderStageEnable::VgtStages::LsHs: {
        if (!instance.IsTessellationSupported()) {
            break;
        }
        if (!TryBindStage(Stage::Hull, LogicalStage::TessellationControl)) {
            return false;
        }
        if (!TryBindStage(Stage::Vertex, LogicalStage::TessellationEval)) {
            return false;
        }
        if (!TryBindStage(Stage::Local, LogicalStage::Vertex)) {
            return false;
        }
        break;
    }
    default: {
        TryBindStage(Stage::Vertex, LogicalStage::Vertex);
        break;
    }
    }

    const auto vs_info = infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            if (attrib.UsesStepRates()) {
                continue;
            }
            const auto& buffer = attrib.GetSharp(*vs_info);
            if (buffer.GetSize() == 0) {
                continue;
            }
            ASSERT(vertex_binding < MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    u32 num_samples = 1u;

    // Second pass to fill remain CB pipeline key data
    for (auto cb = 0u, remapped_cb = 0u; cb < Liverpool::NumColorBuffers; ++cb) {
        auto const& col_buf = regs.color_buffers[cb];
        if (skip_cb_binding || !col_buf || (key.mrt_mask & (1u << cb)) == 0) {
            key.color_formats[cb] = vk::Format::eUndefined;
            key.mrt_swizzles[cb] = Liverpool::ColorBuffer::SwapMode::Standard;
            continue;
        }

        key.blend_controls[remapped_cb] = regs.blend_control[cb];
        key.blend_controls[remapped_cb].enable.Assign(key.blend_controls[remapped_cb].enable &&
                                                      !col_buf.info.blend_bypass);
        key.write_masks[remapped_cb] = vk::ColorComponentFlags{regs.color_target_mask.GetMask(cb)};
        key.cb_shader_mask.SetMask(remapped_cb, regs.color_shader_mask.GetMask(cb));

        num_samples = std::max(num_samples, 1u << col_buf.attrib.num_samples_log2);

        ++remapped_cb;
    }

    // It seems that the number of samples > 1 set in the AA config doesn't mean we're always
    // rendering with MSAA, so we need to derive MS ratio from the CB settings.
    num_samples = std::max(num_samples, regs.depth_buffer.NumSamples());
    key.num_samples = num_samples;

    return true;
} // namespace Vulkan

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto* cs_pgm = &liverpool->regs.cs_program;
    const auto cs_params = Liverpool::GetParams(*cs_pgm);
    std::tie(infos[0], modules[0], fetch_shader, compute_key) =
        GetProgram(Stage::Compute, LogicalStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              std::span<const u32> code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");
    auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
    if (patch) {
        spv = *patch;
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage, info.pgm_hash);
    }

    const auto module = CompileSPV(spv, instance.GetDevice());
    const auto name = fmt::format("{}_{:#x}_{}", info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (Config::collectShadersForDebug()) {
        DebugState.CollectShader(name, spv, code);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(Stage stage, LogicalStage l_stage,
                                                Shader::ShaderParams params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(stage, l_stage);
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        Program* program = program_pool.Create(stage, l_stage, params);
        auto start = binding;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        const auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        program->AddPermut(module, std::move(spec));
        it_pgm.value() = program;
        return std::make_tuple(&program->info, module, spec.fetch_shader_data,
                               HashCombine(params.hash, 0));
    }

    Program* program = it_pgm->second;
    auto& info = program->info;
    info.RefreshFlatBuf();
    if (l_stage == LogicalStage::TessellationControl || l_stage == LogicalStage::TessellationEval) {
        Shader::TessellationDataConstantBuffer tess_constants;
        info.ReadTessConstantBuffer(tess_constants);
        if (l_stage == LogicalStage::TessellationControl) {
            runtime_info.hs_info.InitFromTessConstants(tess_constants);
        } else {
            runtime_info.vs_info.InitFromTessConstants(tess_constants);
        }
    }
    const auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
    size_t perm_idx = program->modules.size();
    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(stage, l_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
    }
    return std::make_tuple(&info, module, spec.fetch_shader_data,
                           HashCombine(params.hash, perm_idx));
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!Config::dumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}_{}.{}", stage, hash, perm_idx, ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Write};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::Stage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {
    if (!Config::patchShaders()) {
        return {};
    }

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}_{}.{}", stage, hash, perm_idx, ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}

} // namespace Vulkan
