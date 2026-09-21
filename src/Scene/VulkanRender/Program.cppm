module;
#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

export module wescene.vulkan_render:program;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.load_bench;
import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;
import wescene.spec_names;
import wescene.rgraph;
import :vulkan_pass;
import :resource;
import :pipeline_layout;
import :pass_common;
import :pre_pass;
import :fin_pass;
import :shader_reflection_cache;
import :uniform_buffer;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;

export namespace owe::vulkan
{
class DeclaredShaderArtifactProvider {
public:
    explicit DeclaredShaderArtifactProvider(const ResourceDeclarationContext& declarations)
        : m_declarations(rstd::ref<ResourceDeclarationContext>::from_raw_parts(
              rstd::addressof(declarations))) {}

    auto LoadShader(const resource::ShaderRequest& request)
        -> rstd::Result<resource::ShaderArtifact, resource::ResourceError> {
        auto artifact = m_declarations->ShaderArtifact(request);
        if (artifact.is_none()) {
            return rstd::Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("shader artifact {} unavailable", request.name),
            });
        }
        return rstd::Ok((**artifact).clone());
    }

private:
    rstd::ref<ResourceDeclarationContext> m_declarations;
};

inline bool SameProgramRenderItemId(owe::RenderItemId lhs, owe::RenderItemId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

struct PreparedPassDiagnostic {
    bool                                      frame_pass { false };
    Option<rg::NodeHandle>                    graph_node;
    std::string                               pass_name;
    Option<rg::PassNode::Type>                pass_type;
    Option<RenderItemId>                      render_item;
    PassInvalidationFlags                     invalidation_flags { PassInvalidationNone };
    Option<PipelineCacheKey>                  pipeline_cache_key;
    bool                                      pipeline_cache_hit { false };
    u64                                       pipeline_cache_observed_count { 0 };
    Option<RenderPassCacheKey>                render_pass_cache_key;
    bool                                      render_pass_cache_hit { false };
    u64                                       render_pass_cache_observed_count { 0 };
    Option<FramebufferCacheKey>               framebuffer_cache_key;
    bool                                      framebuffer_cache_hit { false };
    u64                                       framebuffer_cache_observed_count { 0 };
    std::vector<std::string>                  release_textures;
    std::vector<PassTextureRequestDiagnostic> texture_requests;
    bool                                      prepared { false };
};

enum class RenderProgramPrepareStatus
{
    BatchReady,
    Complete,
    Failed,
};

struct UploadCommitResult {
    bool success { false };
    u64  signal_value {};
};

struct RenderProgram {
    enum class PreparedPassKind
    {
        Graph,
        Frame,
    };

    struct ProgramPassHandle {
        PreparedPassKind kind { PreparedPassKind::Graph };
        rg::PassHandle   graph;
        rstd::usize      frame_index { 0 };
    };

    struct PreparedPassRecord {
        PreparedPassKind                kind { PreparedPassKind::Graph };
        Option<owe::rg::NodeHandle>     graph_node;
        String                          pass_name;
        Option<owe::rg::PassNode::Type> pass_type;
        ProgramPassHandle               pass;
        rstd::vec::Vec<String>          release_textures;
        PassResourceUses                resources;
        PassInvalidationFlags           invalidation_flags { PassInvalidationNone };

        bool invalidated() const { return invalidation_flags != PassInvalidationNone; }

        void invalidate(PassInvalidationFlags flags) { invalidation_flags |= flags; }

        void invalidate(PassInvalidation invalidation) {
            invalidate(ToPassInvalidationFlags(invalidation));
        }

        void invalidateResources() { invalidate(PassInvalidation::Resources); }

        void invalidatePipeline() { invalidate(PassInvalidation::Pipeline); }

        void invalidateFramebuffer() { invalidate(PassInvalidation::Framebuffer); }

        void invalidateAll() { invalidate(PassInvalidationAll); }

        void clearInvalidation() { invalidation_flags = PassInvalidationNone; }

        bool needsPrepare(const VulkanPass& resolved) const {
            return ! resolved.prepared() || invalidated();
        }

        void resetPrepared(VulkanPass& resolved, const Device& device) {
            if (resolved.prepared()) {
                resolved.destory(device);
            }
            resolved.resetPrepared();
        }

        void prepareIfNeeded(VulkanPass& resolved, owe::Scene& scene, const Device& device,
                             PassPrepareContext& context) {
            if (invalidated() && resolved.prepared()) {
                resetPrepared(resolved, device);
            }
            if (! resolved.prepared()) {
                resolved.prepare(scene, device, context);
            }
            if (resolved.prepared()) clearInvalidation();
        }
    };

    struct RenderPassScope {
        rstd::Option<rstd::usize>   single;
        rstd::vec::Vec<rstd::usize> scoped_passes;
    };

    rstd::vec::Vec<PreparedPassRecord>                      pass_records;
    rstd::vec::Vec<RenderPassScope>                         scopes;
    owe::resource::ResourcePlan                             resource_plan;
    rstd::usize                                             graph_texture_count { 0 };
    rstd::Option<rstd::mut_ref<owe::rg::RenderGraph>>       graph;
    rstd::vec::Vec<rstd::mut_ref<VulkanPass>>               frame_passes;
    rstd::Option<rstd::mut_ref<PrePass>>                    frame_prepass;
    rstd::Option<rstd::mut_ref<FinPass>>                    frame_finpass;
    rstd::vec::Vec<Box<dyn<UniformBufferUpdate>>>           uniform_update_owners;
    rstd::vec::Vec<ref<dyn<UniformBufferUpdate>>>           uniform_updates;
    resource_registry::DescriptorBindingRecordState         descriptor_record_state;
    PipelineLayoutAssignments                               pipeline_layout_assignments;
    Option<resource::DescriptorBindingHandle>               global_descriptor_binding;
    rstd::Option<resource_registry::ResourcePrepareSession> resource_prepare_session;
    bool                                                    loaded { false };

    void clear() {
        uniform_updates.clear();
        uniform_update_owners.clear();
        resource_prepare_session = rstd::None();
        pipeline_layout_assignments.entries.clear();
        global_descriptor_binding = None();
        scopes.clear();
        pass_records.clear();
        resource_plan       = {};
        graph_texture_count = rstd::usize();
        graph               = rstd::None();
        frame_passes.clear();
        frame_prepass = rstd::None();
        frame_finpass = rstd::None();
        loaded        = false;
    }

    auto resolve(const PreparedPassRecord& record) -> rstd::Option<VulkanPass&> {
        if (record.pass.kind == PreparedPassKind::Frame) {
            if (record.pass.frame_index >= frame_passes.len()) return rstd::None();
            return rstd::Some<VulkanPass&>(*frame_passes[record.pass.frame_index]);
        }
        if (graph.is_none()) return rstd::None();
        auto resolved = (*graph)->getPass(record.pass.graph);
        if (resolved.is_none()) return rstd::None();
        return rstd::Some<VulkanPass&>(static_cast<VulkanPass&>(*resolved));
    }

    auto resolve(const PreparedPassRecord& record) const -> rstd::Option<const VulkanPass&> {
        if (record.pass.kind == PreparedPassKind::Frame) {
            if (record.pass.frame_index >= frame_passes.len()) return rstd::None();
            return rstd::Some<const VulkanPass&>(*frame_passes[record.pass.frame_index]);
        }
        if (graph.is_none()) return rstd::None();
        auto resolved =
            static_cast<const owe::rg::RenderGraph&>(**graph).getPass(record.pass.graph);
        if (resolved.is_none()) return rstd::None();
        return rstd::Some<const VulkanPass&>(static_cast<const VulkanPass&>(*resolved));
    }

    bool buildFromGraph(owe::rg::RenderGraph& graph) {
        auto ordered = graph.topologicalOrder();
        if (ordered.is_err()) {
            clear();
            return false;
        }
        auto nodes             = rstd::move(ordered).unwrap_unchecked();
        auto node_release_texs = graph.getLastReadTextures(nodes.as_slice());
        auto plan              = graph.resourcePlan();

        clear();
        this->graph =
            rstd::Some(rstd::mut_ref<owe::rg::RenderGraph>::from_raw_parts(rstd::addressof(graph)));
        resource_plan       = rstd::move(plan);
        graph_texture_count = resource_plan.textures.len();
        pass_records =
            rstd::vec::Vec<PreparedPassRecord>::with_capacity(nodes.len() + rstd::usize(2));

        for (rstd::usize i {}; i < nodes.len(); ++i) {
            auto id    = nodes[i];
            auto state = graph.passState(id);
            rstd_assert(state.is_some());
            if (state.is_none()) continue;
            rstd_assert(graph.getPass(state->pass).is_some());

            PreparedPassRecord record {
                .kind       = PreparedPassKind::Graph,
                .graph_node = Some(id),
                .pass_name  = state->name.clone(),
                .pass_type  = Some(state->type),
                .pass =
                    ProgramPassHandle {
                        .kind  = PreparedPassKind::Graph,
                        .graph = state->pass,
                    },
            };
            for (const auto& tex : node_release_texs[i]) {
                record.release_textures.push(tex.desc.key.clone());
            }
            pass_records.push(rstd::move(record));
        }
        return true;
    }

    void injectFramePasses(PrePass& prepass, FinPass& finpass) {
        prepass.resetResourceUses();
        finpass.resetResourceUses();
        frame_prepass =
            rstd::Some(rstd::mut_ref<PrePass>::from_raw_parts(rstd::addressof(prepass)));
        frame_finpass =
            rstd::Some(rstd::mut_ref<FinPass>::from_raw_parts(rstd::addressof(finpass)));
        frame_passes.clear();
        frame_passes.push(rstd::mut_ref<VulkanPass>::from_raw_parts(rstd::addressof(prepass)));
        frame_passes.push(rstd::mut_ref<VulkanPass>::from_raw_parts(rstd::addressof(finpass)));
        auto combined =
            rstd::vec::Vec<PreparedPassRecord>::with_capacity(pass_records.len() + rstd::usize(2));
        combined.push(PreparedPassRecord {
            .kind      = PreparedPassKind::Frame,
            .pass_name = String::make("frame/pre"_str),
            .pass =
                ProgramPassHandle {
                    .kind        = PreparedPassKind::Frame,
                    .frame_index = rstd::usize(),
                },
        });
        for (auto& record : pass_records) combined.push(rstd::move(record));
        combined.push(PreparedPassRecord {
            .kind      = PreparedPassKind::Frame,
            .pass_name = String::make("frame/fin"_str),
            .pass =
                ProgramPassHandle {
                    .kind        = PreparedPassKind::Frame,
                    .frame_index = rstd::usize(1),
                },
        });
        pass_records = rstd::move(combined);
    }

    std::vector<PreparedPassDiagnostic> diagnostics() const {
        std::vector<PreparedPassDiagnostic> out;
        out.reserve(pass_records.len().to_primitive());
        for (const auto& record : pass_records) {
            auto pass        = resolve(record);
            auto render_item = Option<RenderItemId> {};
            if (pass.is_some()) {
                auto id = pass->renderItemId();
                if (id.is_some()) render_item = Some<RenderItemId>(*id);
            }
            auto release_textures = std::vector<std::string> {};
            release_textures.reserve(record.release_textures.len().to_primitive());
            for (const auto& texture : record.release_textures) {
                release_textures.push_back(rstd::cppstd::to_string(texture.as_str()));
            }
            out.push_back(PreparedPassDiagnostic {
                .frame_pass         = record.kind == PreparedPassKind::Frame,
                .graph_node         = record.graph_node,
                .pass_name          = rstd::cppstd::to_string(record.pass_name.as_str()),
                .pass_type          = record.pass_type,
                .render_item        = render_item,
                .invalidation_flags = record.invalidation_flags,
                .pipeline_cache_key = pass ? pass->pipelineCacheKey() : None<PipelineCacheKey>(),
                .pipeline_cache_hit = pass && pass->pipelineCacheHit(),
                .pipeline_cache_observed_count = pass ? pass->pipelineCacheObservedCount() : u64(),
                .render_pass_cache_key =
                    pass ? pass->renderPassCacheKey() : None<RenderPassCacheKey>(),
                .render_pass_cache_hit = pass && pass->renderPassCacheHit(),
                .render_pass_cache_observed_count =
                    pass ? pass->renderPassCacheObservedCount() : u64(),
                .framebuffer_cache_key =
                    pass ? pass->framebufferCacheKey() : None<FramebufferCacheKey>(),
                .framebuffer_cache_hit = pass && pass->framebufferCacheHit(),
                .framebuffer_cache_observed_count =
                    pass ? pass->framebufferCacheObservedCount() : u64(),
                .release_textures = rstd::move(release_textures),
                .texture_requests = pass ? pass->textureRequestDiagnostics()
                                         : std::vector<PassTextureRequestDiagnostic> {},
                .prepared         = pass && pass->prepared(),
            });
        }
        return out;
    }

    void invalidatePass(ProgramPassHandle pass, PassInvalidationFlags flags) {
        if (flags == PassInvalidationNone) return;
        for (auto& record : pass_records) {
            auto same =
                record.pass.kind == pass.kind &&
                (pass.kind == PreparedPassKind::Frame ? record.pass.frame_index == pass.frame_index
                                                      : record.pass.graph == pass.graph);
            if (! same) continue;
            record.invalidate(flags);
            loaded = false;
            return;
        }
    }

    void invalidateRenderItems(slice<owe::RenderItemId> render_items, PassInvalidationFlags flags) {
        if (flags == PassInvalidationNone || render_items.is_empty()) return;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto pass_render_item = pass->renderItemId();
            if (pass_render_item.is_none()) continue;
            bool matched = false;
            for (usize index {}; index < render_items.len(); ++index) {
                if (! SameProgramRenderItemId(*pass_render_item, render_items[index])) continue;
                matched = true;
                break;
            }
            if (! matched) continue;
            record.invalidate(flags);
            loaded = false;
        }
    }

    bool refreshMaterialTextureBindings(const owe::RenderSceneSnapshot& render_scene,
                                        slice<owe::RenderItemId>        render_items) {
        if (render_items.is_empty()) return false;

        bool requires_graph_rebuild = false;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto pass_render_item = pass->renderItemId();
            if (pass_render_item.is_none()) continue;
            bool matched = false;
            for (usize index {}; index < render_items.len(); ++index) {
                if (! SameProgramRenderItemId(*pass_render_item, render_items[index])) continue;
                matched = true;
                break;
            }
            if (! matched) continue;

            auto refresh = pass->refreshMaterialTextureBindings(render_scene);
            if (refresh.requires_graph_rebuild) {
                requires_graph_rebuild = true;
            }
            if (refresh.invalidation_flags == PassInvalidationNone) continue;
            record.invalidate(refresh.invalidation_flags);
            loaded = false;
        }
        return requires_graph_rebuild;
    }

    void finalizeRenderTargetSizes(owe::Scene& scene, VkExtent2D extent,
                                   VkExtent2D            max_framebuffer_extent,
                                   VkSampleCountFlagBits msaa_samples) {
        auto names = scene.RenderTargetNames();
        for (usize index {}; index < names.len(); ++index) {
            auto target = scene.RenderTargetMut(names[index].as_str());
            if (target.is_none()) continue;
            auto& rt = **target;
            if (rt.bind.enable && rt.bind.screen) {
                rt.width = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>(extent.width));
                rt.height =
                    rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>(extent.height));
            }
        }
        for (usize index {}; index < names.len(); ++index) {
            auto target = scene.RenderTargetMut(names[index].as_str());
            if (target.is_none()) continue;
            auto& rt = **target;
            if (rt.bind.screen || ! rt.bind.enable) continue;
            auto bind_rt = scene.RenderTarget(as_str(rt.bind.name).unwrap());
            if (rt.bind.name.empty() || bind_rt.is_none()) {
                rstd_error("unknonw render target bind: {}", rt.bind.name);
                continue;
            }
            rt.width = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>((**bind_rt).width));
            rt.height =
                rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>((**bind_rt).height));
        }
        for (usize index {}; index < names.len(); ++index) {
            auto target = scene.RenderTargetMut(names[index].as_str());
            if (target.is_none()) continue;
            auto& rt = **target;
            if (! names[index].is_empty() && (rt.width <= i32() || rt.height <= i32())) {
                rstd_error("wrong size for render target: {}", names[index].as_str());
            }

            const auto physical_width =
                std::clamp(std::max(rt.width, i32(1)),
                           i32(1),
                           rstd::as_cast<i32>(max_framebuffer_extent.width));
            const auto physical_height =
                std::clamp(std::max(rt.height, i32(1)),
                           i32(1),
                           rstd::as_cast<i32>(max_framebuffer_extent.height));
            const bool physical_size_changed =
                rt.physical_width != physical_width || rt.physical_height != physical_height;
            rt.physical_width  = physical_width;
            rt.physical_height = physical_height;
            if (physical_size_changed &&
                (rt.physical_width != rt.width || rt.physical_height != rt.height)) {
                rstd_warn("clamp render target {} from {}x{} to {}x{}",
                          names[index].as_str(),
                          rt.width,
                          rt.height,
                          rt.physical_width,
                          rt.physical_height);
            }

            if (rt.has_mipmap) {
                rt.mipmap_level =
                    std::max(3u,
                             static_cast<unsigned>(std::floor(std::log2(rstd::as_cast<double>(
                                 std::min(rt.physical_width, rt.physical_height)))))) -
                    2u;
            }
        }
        if (msaa_samples != VK_SAMPLE_COUNT_1_BIT) {
            auto target = scene.RenderTargetMut(owe::SpecTex_Default);
            if (target.is_some()) {
                (**target).sample_count = static_cast<unsigned>(msaa_samples);
            }
        }
    }

    void finalizeFramePassRequests(owe::Scene& scene) {
        if (frame_prepass.is_none() || frame_finpass.is_none()) return;

        auto& prepass        = **frame_prepass;
        auto& finpass        = **frame_finpass;
        auto  prepass_handle = ProgramPassHandle {
            .kind        = PreparedPassKind::Frame,
            .frame_index = rstd::usize(),
        };
        auto finpass_handle = ProgramPassHandle {
            .kind        = PreparedPassKind::Frame,
            .frame_index = rstd::usize(1),
        };

        const auto key    = rstd::cppstd::to_string(owe::SpecTex_Default);
        auto       target = scene.RenderTarget(as_str(key).unwrap());
        if (target.is_none()) {
            if (prepass.setResultRequest(rstd::None())) {
                invalidatePass(prepass_handle,
                               ToPassInvalidationFlags(PassInvalidation::Resources) |
                                   ToPassInvalidationFlags(PassInvalidation::Framebuffer));
            }
            if (finpass.setResultRequest(rstd::None())) {
                invalidatePass(finpass_handle,
                               ToPassInvalidationFlags(PassInvalidation::Resources));
            }
            return;
        }

        const auto&                  rt = **target;
        rstd::Option<TextureRequest> msaa_request;
        auto                         samples = TextureSampleCount(rt.sample_count);
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            auto twin_name = MsaaTwinName(key, samples);
            msaa_request   = rstd::Some(MakeMsaaTextureRequest(twin_name, rt, samples));
        }

        if (prepass.setResultRequest(rstd::Some(MakeRenderTargetNoMipTextureRequest(key, rt)),
                                     std::move(msaa_request))) {
            invalidatePass(prepass_handle,
                           ToPassInvalidationFlags(PassInvalidation::Resources) |
                               ToPassInvalidationFlags(PassInvalidation::Framebuffer));
        }
        if (finpass.setResultRequest(rstd::Some(MakeRenderTargetTextureRequest(key, rt)))) {
            invalidatePass(finpass_handle, ToPassInvalidationFlags(PassInvalidation::Resources));
        }
    }

    void destroyPasses(const Device& device) {
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass) record.resetPrepared(*pass, device);
        }
    }

    void finalizeResourceRequests(owe::Scene& scene) {
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto flags = pass->finalizeResourceRequests(scene);
            if (flags != PassInvalidationNone) {
                record.invalidate(flags);
                loaded = false;
            }
            for (const auto& diagnostic : pass->textureRequestDiagnostics()) {
                if (diagnostic.request.is_none()) continue;
                if (diagnostic.use.is_some() && resource_plan.UpdateTextureRequest(
                                                    *diagnostic.use, diagnostic.request->clone())) {
                    continue;
                }
                auto request_name = rstd::cppstd::as_string_view(diagnostic.request->name.as_str());
                for (auto& entry : resource_plan.textures) {
                    if (entry.request.kind != diagnostic.request->kind ||
                        rstd::cppstd::as_string_view(entry.request.name.as_str()) != request_name) {
                        continue;
                    }
                    entry.request = diagnostic.request->clone();
                }
            }
        }
    }

    auto beginPrepare(owe::Scene& scene, const Device& device, RenderingResources& rr,
                      const owe::RenderSceneSnapshot& render_scene,
                      resource::ResourcePlanSections  sections = resource::ResourcePlanAll,
                      SceneLoadBenchRecorderView load_bench    = {}) -> RenderProgramPrepareStatus {
        auto prepare_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_resources_prepare);
        loaded            = false;
        resource_prepare_session = rstd::None();
        if (rr.shader_reflection_cache.is_none()) {
            rstd_error("shader artifact compiler unavailable");
            return RenderProgramPrepareStatus::Failed;
        }
        ResourceDeclarationContext declarations(resource_plan, **rr.shader_reflection_cache);
        const bool                 prepare_buffers =
            resource::ResourcePlanIncludes(sections, resource::ResourcePlanBuffers);
        const bool prepare_shaders =
            resource::ResourcePlanIncludes(sections, resource::ResourcePlanShaders);
        if (prepare_buffers != prepare_shaders) {
            rstd_error("render resource declaration requires buffers and shaders together");
            return RenderProgramPrepareStatus::Failed;
        }
        if (prepare_buffers) {
            resource_plan.textures.truncate(graph_texture_count);
            resource_plan.buffers.clear();
            resource_plan.shaders.clear();
            for (auto& record : pass_records) {
                auto pass = resolve(record);
                if (pass) {
                    pass->declareResources(declarations);
                    if (pass->prepared() && pass->resourceUses() != record.resources) {
                        record.invalidateAll();
                    }
                }
            }
        }

        SnapshotImportedTextureProvider imported_textures(
            render_scene, ref<Scene>::from_raw_parts(rstd::addressof(scene)));
        SnapshotTexturePrepareObserver texture_observer(load_bench);
        auto                           content =
            rstd::dyn<owe::resource::TextureContentProvider>::from_ref(imported_textures);
        auto observer =
            rstd::dyn<owe::resource::TexturePrepareObserver>::from_ref(texture_observer);
        auto buffer_content =
            rstd::dyn<owe::resource::BufferContentProvider>::from_ref(declarations);
        DeclaredShaderArtifactProvider declared_shaders(declarations);
        auto                           shader_artifacts =
            rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(declared_shaders);
        auto started =
            rr.resources.BeginPreparePlan(resource_plan,
                                          owe::resource_registry::ResourceContentProviders {
                                              .texture = rstd::Some(content),
                                              .buffer  = rstd::Some(buffer_content.as_mut_ref()),
                                              .shader  = rstd::Some(shader_artifacts.as_mut_ref()),
                                          },
                                          sections,
                                          Some(observer.as_mut_ref()));
        if (started.is_err()) {
            auto error = rstd::move(started).unwrap_err_unchecked();
            rstd_error("prepare resource plan failed: {}", error.message);
            return RenderProgramPrepareStatus::Failed;
        }
        resource_prepare_session.insert(rstd::move(started).unwrap_unchecked());
        return continuePrepare(scene, device, rr, load_bench);
    }

    auto continuePrepare(owe::Scene& scene, const Device& device, RenderingResources& rr,
                         SceneLoadBenchRecorderView load_bench = {}) -> RenderProgramPrepareStatus {
        auto prepare_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_resources_prepare);
        if (resource_prepare_session.is_none()) return RenderProgramPrepareStatus::Failed;
        SnapshotTexturePrepareObserver texture_observer(load_bench);
        auto                           observer =
            rstd::dyn<owe::resource::TexturePrepareObserver>::from_ref(texture_observer);
        auto progress = rr.resources.ContinuePreparePlan(*resource_prepare_session,
                                                         Some(observer.as_mut_ref()));
        if (progress.is_err()) {
            auto error = rstd::move(progress).unwrap_err_unchecked();
            rstd_error("prepare resource plan failed: {}", error.message);
            resource_prepare_session = rstd::None();
            return RenderProgramPrepareStatus::Failed;
        }
        if (progress.unwrap_unchecked() == resource_registry::ResourcePrepareProgress::BatchReady) {
            return RenderProgramPrepareStatus::BatchReady;
        }
        resource_prepare_session = rstd::None();
        return finishPrepare(scene, device, rr);
    }

    auto finishPrepare(owe::Scene& scene, const Device& device, RenderingResources& rr)
        -> RenderProgramPrepareStatus {
        auto state_preparer = rstd::dyn<owe::resource_registry::TextureStatePreparer>::from_ref(
            rr.resources.States());
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            if (! pass->prepareResourceStates(state_preparer.as_mut_ref())) {
                rstd_error("prepare resource states failed for {}", record.pass_name);
                return RenderProgramPrepareStatus::Failed;
            }
        }
        Vec<PipelineLayoutRequirement> layout_requirements;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto                  uses = pass->resourceUses();
            PreparedPassResources resources(rr.resources.Prepared(), uses);
            auto                  requirement = pass->pipelineLayoutRequirement(resources);
            if (requirement.is_err()) {
                auto error = rstd::move(requirement).unwrap_err_unchecked();
                rstd_error("collect pipeline layout failed for {}: {}",
                           record.pass_name,
                           error.message.as_str());
                return RenderProgramPrepareStatus::Failed;
            }
            auto value = rstd::move(requirement).unwrap_unchecked();
            if (value.is_some()) layout_requirements.push(rstd::move(*value));
        }
        auto planned = PlanPipelineLayouts(layout_requirements.as_slice(),
                                           device.capabilities().push_descriptor,
                                           u32(device.capabilities().max_push_descriptors),
                                           u32(device.limits().maxPushConstantsSize),
                                           rstd::addressof(device.limits()));
        if (planned.is_err()) {
            auto error = rstd::move(planned).unwrap_err_unchecked();
            rstd_error("plan pipeline layouts failed: {}", error.message.as_str());
            return RenderProgramPrepareStatus::Failed;
        }
        auto layout_plan = rstd::move(planned).unwrap_unchecked();
        for (const auto& conflict : layout_plan.conflicts) {
            rstd_debug("pipeline {} did not join layout family {}: {}",
                       conflict.pipeline.index,
                       conflict.family,
                       conflict.message.as_str());
        }
        PipelineLayoutAssignments next_layout_assignments;
        for (auto& family : layout_plan.families) {
            auto layout = rr.resources.PreparePipelineLayout(device, family.request);
            if (layout.is_err()) {
                auto error = rstd::move(layout).unwrap_err_unchecked();
                rstd_error("prepare pipeline layout failed: {}", error.message.as_str());
                return RenderProgramPrepareStatus::Failed;
            }
            auto handle = rstd::move(layout).unwrap_unchecked();
            for (const auto& pipeline : family.pipelines) {
                next_layout_assignments.entries.push(PipelineLayoutAssignment {
                    .pipeline = pipeline,
                    .layout   = handle,
                });
            }
        }
        bool layout_assignment_changed = false;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            bool pass_changed = false;
            for (const auto& pipeline : pass->resourceUses().pipelines) {
                auto previous = pipeline_layout_assignments.Resolve(pipeline);
                auto next     = next_layout_assignments.Resolve(pipeline);
                if (previous.is_some() == next.is_some() &&
                    (previous.is_none() || *previous == *next)) {
                    continue;
                }
                pass_changed = true;
                break;
            }
            if (! pass_changed) continue;
            record.invalidatePipeline();
            layout_assignment_changed = true;
        }
        if (layout_assignment_changed) loaded = false;
        pipeline_layout_assignments = rstd::move(next_layout_assignments);
        auto graphics =
            rstd::dyn<owe::resource_registry::GraphicsResourcePreparer>::from_ref(rr.resources);

        auto global_uses = Vec<GlobalDescriptorBufferUse>::make();
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            auto                  uses = pass->resourceUses();
            PreparedPassResources resources(rr.resources.Prepared(), uses);
            auto                  collected = pass->globalDescriptorBufferUses(resources);
            if (collected.is_err()) {
                auto error = rstd::move(collected).unwrap_err_unchecked();
                rstd_error("collect global descriptor buffers failed for {}: {}",
                           record.pass_name,
                           error.message.as_str());
                return RenderProgramPrepareStatus::Failed;
            }
            for (const auto& use : collected.unwrap_unchecked()) {
                bool known = false;
                for (const auto& existing : global_uses) {
                    if (existing.binding != use.binding) continue;
                    if (existing.identity != use.identity || existing.buffer != use.buffer) {
                        rstd_error("global descriptor binding {} has incompatible resources",
                                   use.binding);
                        return RenderProgramPrepareStatus::Failed;
                    }
                    known = true;
                    break;
                }
                if (! known) {
                    global_uses.push(GlobalDescriptorBufferUse {
                        .binding  = use.binding,
                        .identity = use.identity,
                        .buffer   = use.buffer,
                    });
                }
            }
        }
        rstd::slice_::sort_unstable_by(global_uses.as_mut_slice().as_mut_ref(),
                                       [](const auto& lhs, const auto& rhs) {
                                           return lhs.binding < rhs.binding;
                                       });
        if (global_uses.len() != layout_plan.global_bindings.len()) {
            rstd_error("global descriptor writes do not cover the planned layout");
            return RenderProgramPrepareStatus::Failed;
        }
        for (const auto& binding : layout_plan.global_bindings) {
            bool matched = false;
            for (const auto& use : global_uses) {
                if (use.binding == binding.binding && use.identity == binding.identity) {
                    matched = true;
                    break;
                }
            }
            if (! matched) {
                rstd_error("global descriptor binding {} does not match the planned layout",
                           binding.binding);
                return RenderProgramPrepareStatus::Failed;
            }
        }
        Option<resource::DescriptorBindingHandle> next_global_descriptor_binding;
        if (global_descriptor_binding.is_some())
            rr.resources.RemovePreparedDescriptor(*global_descriptor_binding);
        if (! global_uses.is_empty()) {
            if (pipeline_layout_assignments.entries.is_empty()) {
                rstd_error("global descriptor buffers have no pipeline layout");
                return RenderProgramPrepareStatus::Failed;
            }
            auto buffers =
                Vec<resource_registry::DescriptorBufferBinding>::with_capacity(global_uses.len());
            for (const auto& use : global_uses) {
                auto prepared = rr.resources.Prepared().Resolve(use.buffer);
                if (prepared.is_none()) {
                    rstd_error("global descriptor buffer {} is unavailable", use.binding);
                    return RenderProgramPrepareStatus::Failed;
                }
                auto& allocation = (**prepared).buffer.physical->buffer;
                buffers.push(resource_registry::DescriptorBufferBinding {
                    .binding = use.binding.to_primitive(),
                    .buffer  = allocation.buffer(),
                    .offset  = allocation.offset(),
                    .size    = allocation.size(),
                });
            }
            auto images = Vec<resource_registry::DescriptorImageBinding>::make();
            auto prepared =
                graphics->PrepareDescriptor(device,
                                            pipeline_layout_assignments.entries[usize()].layout,
                                            u32(),
                                            images.as_slice(),
                                            buffers.as_slice(),
                                            resource_registry::DescriptorBindingReuse::Shared);
            if (prepared.is_err()) {
                auto error = rstd::move(prepared).unwrap_err_unchecked();
                rstd_error("prepare global descriptor set failed: {}", error.message.as_str());
                return RenderProgramPrepareStatus::Failed;
            }
            next_global_descriptor_binding = Some(rstd::move(prepared).unwrap_unchecked());
        }
        PassPrepareContext prepare_context {
            .shader_backend = (*rr.shader_reflection_cache)->Backend(),
            .resources = rstd::ref<owe::resource_registry::PreparedResourceTable>::from_raw_parts(
                rstd::addressof(rr.resources.Prepared())),
            .graphics         = graphics.as_mut_ref(),
            .pipeline_layouts = rstd::ref<PipelineLayoutAssignments>::from_raw_parts(
                rstd::addressof(pipeline_layout_assignments)),
        };
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass) {
                if (record.invalidated()) {
                    rr.resources.RemovePreparedGraphics(record.resources.pipelines.as_slice(),
                                                        record.resources.render_passes.as_slice(),
                                                        record.resources.framebuffers.as_slice(),
                                                        record.resources.descriptors.as_slice(),
                                                        record.resources.externals.as_slice());
                }
                record.prepareIfNeeded(*pass, scene, device, prepare_context);
                if (! pass->prepared()) {
                    rstd_error("prepare pass failed for {}", record.pass_name);
                    return RenderProgramPrepareStatus::Failed;
                }
                record.resources = pass->resourceUses();
            }
        }
        uniform_updates.clear();
        uniform_update_owners.clear();
        SceneUniformBindingPrepareContext uniform_prepare_impl(scene);
        auto uniform_prepare = dyn<UniformBindingPrepareContext>::from_ref(uniform_prepare_impl);
        Vec<resource::BufferUseHandle> prepared_uniform_buffers;
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (pass.is_none()) continue;
            PreparedPassResources resources(rr.resources.Prepared(), record.resources);
            auto created = pass->createUniformBufferUpdates(uniform_prepare.as_ref(), resources);
            if (created.is_err()) {
                auto error = rstd::move(created).unwrap_err_unchecked();
                rstd_error("prepare uniform binding failed for {}: {}",
                           record.pass_name,
                           error.message.as_str());
                return RenderProgramPrepareStatus::Failed;
            }
            auto bindings = rstd::move(created).unwrap_unchecked();
            for (auto& binding : bindings) {
                bool exists = false;
                for (const auto& prepared : prepared_uniform_buffers) {
                    if (prepared == binding->Buffer()) {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;
                prepared_uniform_buffers.push(binding->Buffer());
                uniform_update_owners.push(rstd::move(binding));
            }
        }
        uniform_updates.reserve(uniform_update_owners.len());
        for (const auto& binding : uniform_update_owners) {
            uniform_updates.push(binding.as_ref());
        }
        global_descriptor_binding = next_global_descriptor_binding;
        return RenderProgramPrepareStatus::Complete;
    }

    void abortPrepare(RenderingResources& rr) {
        rr.resources.AbortPreparePlan();
        resource_prepare_session = rstd::None();
        loaded                   = false;
    }

    void invalidateAllPreparedPasses() {
        for (auto& record : pass_records) record.invalidateAll();
        loaded = false;
    }

    auto commitUploads(const Device& device, RenderingResources& rr, vvk::CommandBuffer& upload_cmd)
        -> UploadCommitResult {
        if (! rr.resources.HasPendingUploads()) {
            return UploadCommitResult { .success = true };
        }
        VVK_CHECK_ACT(return UploadCommitResult {},
                             upload_cmd.Begin(VkCommandBufferBeginInfo {
                                 .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .pNext = nullptr,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                             }));
        RecordedBufferUploads recorded;
        RecordedImageUploads  recorded_images;
        if (! rr.resources.RecordPendingUploads(upload_cmd, recorded, recorded_images)) {
            return UploadCommitResult {};
        }
        VVK_CHECK_ACT(return UploadCommitResult {}, upload_cmd.End());
        {
            auto                          ready        = rr.resources.ReserveUpload();
            const uint64_t                signal_value = ready.value.to_primitive();
            VkTimelineSemaphoreSubmitInfo timeline_info {
                .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .pNext                     = nullptr,
                .waitSemaphoreValueCount   = 0,
                .pWaitSemaphoreValues      = nullptr,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues    = &signal_value,
            };
            VkSubmitInfo sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = &timeline_info,
                .commandBufferCount   = 1,
                .pCommandBuffers      = upload_cmd.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_upload.address(),
            };
            VVK_CHECK_ACT(return UploadCommitResult {},
                                 device.graphics_queue().handle.Submit(sub_info, {}));
            if (! rr.resources.MarkUploadSubmitted(
                    ready, rstd::move(recorded), rstd::move(recorded_images))) {
                return UploadCommitResult {};
            }
            return UploadCommitResult { .success = true, .signal_value = u64(signal_value) };
        }
        return UploadCommitResult {};
    }

    void rebuildScopes() {
        scopes.clear();
        auto pending_scope_passes = rstd::vec::Vec<rstd::usize>::make();

        auto flushScopePasses = [&]() {
            if (pending_scope_passes.is_empty()) return;
            RenderPassScope scope;
            scope.scoped_passes = rstd::move(pending_scope_passes);
            scopes.push(rstd::move(scope));
            pending_scope_passes = rstd::vec::Vec<rstd::usize>::make();
        };

        for (rstd::usize index {}; index < pass_records.len(); ++index) {
            auto& record = pass_records[index];
            auto  pass   = resolve(record);
            if (pass.is_none()) continue;
            if (pass->supportsRenderScope()) {
                bool can_join = false;
                if (! pending_scope_passes.is_empty()) {
                    auto previous =
                        resolve(pass_records[pending_scope_passes[pending_scope_passes.len() -
                                                                  rstd::usize(1)]]);
                    can_join = previous && pass->canJoinRenderScopeAfter(*previous);
                }
                if (can_join) {
                    pending_scope_passes.push(rstd::usize(index));
                } else {
                    flushScopePasses();
                    pending_scope_passes.push(rstd::usize(index));
                }
                continue;
            }

            flushScopePasses();
            scopes.push(RenderPassScope { .single = rstd::Some(index) });
        }

        flushScopePasses();
    }

    template<typename Callback>
    void withRecordContext(rstd::usize index, RenderingResources& rr, Callback&& callback) {
        if (index >= pass_records.len()) return;
        auto pass = resolve(pass_records[index]);
        if (pass.is_none()) return;

        PreparedPassResources resources(rr.resources.Prepared(), pass_records[index].resources);
        auto global_descriptor = global_descriptor_binding.is_some()
                                     ? rr.resources.Prepared().Resolve(*global_descriptor_binding)
                                     : None<ref<resource_registry::PreparedDescriptorBinding>>();
        PassRecordContext context {
            .command =
                rstd::mut_ref<vvk::CommandBuffer>::from_raw_parts(rstd::addressof(rr.command)),
            .resources =
                rstd::ref<PreparedPassResources>::from_raw_parts(rstd::addressof(resources)),
            .descriptor_state =
                rstd::mut_ref<resource_registry::DescriptorBindingRecordState>::from_raw_parts(
                    rstd::addressof(descriptor_record_state)),
            .global_descriptor = global_descriptor,
        };
        callback(*pass, context);
    }

    bool update(const SceneFrame& frame, VkExtent2D extent,
                ref<dyn<SceneTextureAnimationView>> textures, RenderingResources& rr) {
        auto buffer_writer = rstd::dyn<resource::BufferContentWriter>::from_ref(rr.resources);
        ProgramUniformFrameContext frame_context(
            frame,
            { static_cast<float>(extent.width), static_cast<float>(extent.height) },
            textures);
        auto context = dyn<UniformBufferFrameContext>::from_ref(frame_context);
        for (auto& binding : uniform_updates) {
            auto result = binding->Update(context.as_ref(), buffer_writer.as_mut_ref());
            if (result.is_err()) {
                auto error = rstd::move(result).unwrap_err_unchecked();
                rstd_error("update uniform buffer failed: {}", error.message.as_str());
                return false;
            }
        }
        auto graphics = dyn<resource_registry::GraphicsResourcePreparer>::from_ref(rr.resources);
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (! pass || ! pass->prepared()) continue;
            PreparedPassResources resources(rr.resources.Prepared(), record.resources);
            PassUpdateContext     update_context {
                .buffers   = buffer_writer.as_mut_ref(),
                .resources = ref<PreparedPassResources>::from_raw_parts(rstd::addressof(resources)),
                .graphics  = graphics.as_mut_ref(),
                .textures  = textures,
            };
            if (! pass->update(update_context)) {
                rstd_error("update pass resources failed: {}", record.pass_name);
                return false;
            }
        }
        for (auto& record : pass_records) {
            auto pass = resolve(record);
            if (! pass || ! pass->prepared()) continue;
            pass->completeUpdate();
        }
        return true;
    }

    bool record(RenderingResources& rr, RecordedBufferUploads& recorded_uploads) {
        descriptor_record_state.Reset();
        if (! rr.resources.RecordPendingUploads(rr.command, recorded_uploads)) {
            rstd_error("record dynamic buffer uploads failed");
            return false;
        }

        for (auto& scope : scopes) {
            if (scope.single) {
                auto index = *scope.single;
                auto pass  = resolve(pass_records[index]);
                if (pass && pass->prepared()) {
                    withRecordContext(
                        index, rr, [](VulkanPass& target, PassRecordContext& context) {
                            target.record(context);
                        });
                }
                continue;
            }

            auto& scoped_passes = scope.scoped_passes;
            if (scoped_passes.is_empty()) continue;
            if (scoped_passes.len() == rstd::usize(1)) {
                auto pass = resolve(pass_records[scoped_passes[rstd::usize()]]);
                if (pass && pass->prepared()) {
                    withRecordContext(scoped_passes[rstd::usize()],
                                      rr,
                                      [](VulkanPass& target, PassRecordContext& context) {
                                          target.record(context);
                                      });
                }
                continue;
            }

            if (! std::all_of(scoped_passes.begin(), scoped_passes.end(), [&](auto index) {
                    auto pass = resolve(pass_records[index]);
                    return pass && pass->prepared();
                })) {
                continue;
            }

            for (auto index : scoped_passes) {
                withRecordContext(index, rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.prepareRenderScopeDraw(context);
                });
            }
            withRecordContext(scoped_passes[rstd::usize()],
                              rr,
                              [](VulkanPass& target, PassRecordContext& context) {
                                  target.beginRenderScope(context);
                              });
            for (auto index : scoped_passes) {
                withRecordContext(index, rr, [](VulkanPass& target, PassRecordContext& context) {
                    target.recordRenderScopeDraw(context);
                });
            }
            withRecordContext(scoped_passes[rstd::usize()],
                              rr,
                              [](VulkanPass& target, PassRecordContext& context) {
                                  target.endRenderScope(context);
                              });
        }
        return true;
    }
};

} // namespace owe::vulkan
