module;
#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

export module wescene.vulkan_render:scene_render_state;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.load_bench;
import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;
import wescene.spec_names;
import :scene_render_state_input;
import :scene_render_resources;
import :resource;
import :pipeline_layout;
import :shader_reflection_cache;
import :uniform_buffer;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;

export namespace owe::vulkan
{
inline bool SameSceneRenderItemId(owe::RenderItemId lhs, owe::RenderItemId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

struct PreparedPassDiagnostic {
    Option<fg::PassId>                pass_id;
    fg::Serial                        scope {}, revision {};
    String                            pass_name;
    Option<fg::Serial>                pass_kind;
    Option<RenderItemId>              render_item;
    PassInvalidationFlags             invalidation_flags { PassInvalidationNone };
    Option<PipelineCacheKey>          pipeline_cache_key;
    bool                              pipeline_cache_hit { false };
    u64                               pipeline_cache_observed_count { 0 };
    Option<RenderPassCacheKey>        render_pass_cache_key;
    bool                              render_pass_cache_hit { false };
    u64                               render_pass_cache_observed_count { 0 };
    Option<FramebufferCacheKey>       framebuffer_cache_key;
    bool                              framebuffer_cache_hit { false };
    u64                               framebuffer_cache_observed_count { 0 };
    Vec<String>                       release_textures;
    Vec<PassTextureRequestDiagnostic> texture_requests;
    bool                              prepared { false };
};

inline void FinalizeRenderTargetSizes(owe::Scene& scene, VkExtent2D extent,
                                      VkExtent2D            max_framebuffer_extent,
                                      VkSampleCountFlagBits msaa_samples) {
    auto names = scene.RenderTargetNames();
    for (usize index {}; index < names.len(); ++index) {
        auto target = scene.RenderTargetMut(names[index].as_str());
        if (target.is_none()) continue;
        auto& rt = **target;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>(extent.width));
            rt.height = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>(extent.height));
        }
    }
    for (usize index {}; index < names.len(); ++index) {
        auto target = scene.RenderTargetMut(names[index].as_str());
        if (target.is_none()) continue;
        auto& rt = **target;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.RenderTarget(rt.bind.name.as_str());
        if (rt.bind.name.is_empty() || bind_rt.is_none()) {
            rstd_error("unknonw render target bind: {}", rt.bind.name);
            continue;
        }
        rt.width  = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>((**bind_rt).width));
        rt.height = rstd::as_cast<i32>(rt.bind.scale * rstd::as_cast<double>((**bind_rt).height));
    }
    for (usize index {}; index < names.len(); ++index) {
        auto target = scene.RenderTargetMut(names[index].as_str());
        if (target.is_none()) continue;
        auto& rt = **target;
        if (! names[index].is_empty() && (rt.width <= i32() || rt.height <= i32())) {
            rstd_error("wrong size for render target: {}", names[index].as_str());
        }

        const auto physical_width = rstd::cmp::min(
            rstd::cmp::max(rt.width, i32(1)), rstd::as_cast<i32>(max_framebuffer_extent.width));
        const auto physical_height = rstd::cmp::min(
            rstd::cmp::max(rt.height, i32(1)), rstd::as_cast<i32>(max_framebuffer_extent.height));
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
            auto min_extent = rstd::cmp::min(rt.physical_width, rt.physical_height);
            auto level      = rstd::as_cast<u32>(rstd::as_cast<f64>(min_extent).log2().floor());
            rt.mipmap_level = (rstd::cmp::max(u32(3), level) - u32(2)).to_primitive();
        }
    }
    if (msaa_samples != VK_SAMPLE_COUNT_1_BIT) {
        auto target = scene.RenderTargetMut(owe::SpecTex_Default);
        if (target.is_some()) {
            (**target).sample_count = static_cast<unsigned>(msaa_samples);
        }
    }
}

struct SceneRenderUpdateProgress {
    usize uniforms {}, draws {};
};

class SceneRenderState {
    resource::ResourcePlan             resource_plan;
    Option<vrento::vulkan::GraphInput> input_;
    Option<SceneRenderTextures>        textures;
    Vec<SceneRenderDrawSource>         draws;
    fg::ResourceVersion                surface;
    fg::PassId                         output_pass, release_pass;
    Option<resource::TextureUseHandle> result_use;
    ResourceDeclarationData            resource_data;
    Option<Arc<SceneImageSource>>      image_source;
    Vec<Box<dyn<UniformBufferUpdate>>> uniform_update_owners;

public:
    void clear() {
        uniform_update_owners.clear();
        draws.clear();
        input_        = None();
        textures      = None();
        resource_plan = {};
        resource_data = {};
        image_source  = None();
        result_use    = None();
        surface       = {};
        output_pass = release_pass = {};
    }
    auto installed() const -> bool { return input_.is_some(); }
    auto valid() const -> bool {
        if (! input_) return false;
        for (const auto& entry : draws)
            if ((entry.invalidation & ~ToPassInvalidationFlags(PassInvalidation::Content)) !=
                PassInvalidationNone)
                return false;
        return true;
    }
    auto inputGraph() const -> const fg::FrameGraph& { return input_->graph(); }
    auto inputDescriptions() const -> slice<vrento::vulkan::PassInput> {
        return input_->descriptions();
    }
    auto renderInput() const -> const vrento::vulkan::GraphInput& { return *input_; }
    auto resourcePlan() const -> const resource::ResourcePlan& { return resource_plan; }
    auto textureResource(ref<str> name) const -> Option<resource::TextureResourceRef> {
        if (! textures) return None();
        for (const auto& texture : textures->textures())
            if (texture.spec.key == name) return Some(texture.spec.definition.clone());
        return None();
    }
    auto imageSource(Scene& scene) const -> Arc<SceneImageSource> {
        return image_source ? image_source->clone() : scene.CaptureImageSource();
    }
    auto declarations() -> ResourceDeclarationContext {
        return ResourceDeclarationContext(resource_plan, rstd::move(resource_data));
    }
    void retainDeclarations(ResourceDeclarationData data) { resource_data = rstd::move(data); }
    auto createOutput() const -> vrento::vulkan::TextureOutput {
        vrento::vulkan::TextureOutput output;
        for (const auto& description : inputDescriptions()) {
            if (description.execution().id != output_pass) continue;
            if (auto recipe = description.get<vrento::vulkan::GraphicsDrawDesc>()) {
                const auto& value = **recipe;
                output.configure(result_use, value.request.color_format);
                output.graphics_resources(value.pipeline, value.render_pass);
                return output;
            }
            break;
        }
        output.configure(result_use, VK_FORMAT_UNDEFINED);
        return output;
    }
    auto install(SceneRenderStateInput input) -> Result<empty, fg::Error> {
        if (input.resource_plan.generation != input.textures.generation()) {
            auto plan = input.textures.resourcePlan(input.graph);
            if (plan.is_err()) return Err(plan.unwrap_err());
            input.resource_plan = plan.unwrap();
        }
        auto mapping = input.textures.bindings(input.graph, input.resource_plan);
        if (mapping.is_err()) return Err(mapping.unwrap_err());
        auto published = vrento::vulkan::GraphInput::create(
            rstd::move(input.graph), rstd::move(input.descriptions), mapping.unwrap());
        if (published.is_err()) return Err(published.unwrap_err());
        clear();
        input_        = Some(published.unwrap());
        resource_plan = rstd::move(input.resource_plan);
        resource_data = rstd::move(input.resource_data);
        image_source  = rstd::move(input.image_source);
        textures      = Some(rstd::move(input.textures));
        draws         = rstd::move(input.draws);
        output_pass   = input.output_pass;
        release_pass  = input.release_pass;
        surface       = input.surface;
        result_use    = input.result_use;
        return Ok(empty {});
    }
    auto draw(fg::Serial scope, fg::PassId id) const -> Option<ref<SceneDraw>> {
        if (! input_ || input_->graph().scope() != scope) return None();
        for (const auto& entry : draws)
            if (entry.id == id) return Some(entry.source.as_ref());
        return None();
    }
    auto containsRenderItem(RenderItemId item) const -> bool {
        for (const auto& entry : draws)
            if (auto id = entry.source->renderItemId())
                if (SameSceneRenderItemId(item, *id)) return true;
        return false;
    }
    void invalidateRenderItems(slice<owe::RenderItemId> items, PassInvalidationFlags flags) {
        for (auto& entry : draws) {
            auto id = entry.source->renderItemId();
            if (! id) continue;
            for (auto item : items)
                if (SameSceneRenderItemId(item, *id)) entry.invalidation |= flags;
        }
    }
    auto sharedBuffers(const resource_registry::PreparedResourceTable& prepared) const
        -> Result<Vec<vrento::vulkan::SharedDescriptorBufferUse>, resource::ResourceError> {
        Vec<vrento::vulkan::SharedDescriptorBufferUse> result;
        for (const auto& entry : draws) {
            auto                  uses = entry.source->resourceUses();
            PreparedPassResources resources(prepared, uses);
            auto                  collected = entry.source->globalDescriptorBufferUses(resources);
            if (collected.is_err()) return Err(collected.unwrap_err());
            for (const auto& use : *collected)
                result.push({ use.binding, use.identity, use.buffer });
        }
        return Ok(rstd::move(result));
    }
    void clearBindings() {
        uniform_update_owners.clear();
        for (auto& entry : draws) entry.source->clearResources();
    }
    auto bindResources(Scene& scene, RenderingResources& rr,
                       const vrento::vulkan::PreparedGraphicsResources& prepared,
                       SceneUniformPreparationCache&                    uniform_cache)
        -> Result<empty, RenderPrepareError> {
        auto graphics = dyn<resource_registry::GraphicsResourcePreparer>::from_ref(rr.resources);
        DrawResourceContext context {
            .resources = ref<resource_registry::PreparedResourceTable>::from_raw_parts(
                &rr.resources.Prepared()),
            .graphics         = graphics.as_mut_ref(),
            .pipeline_layouts = ref<PipelineLayoutAssignments>::from_raw_parts(&prepared.layouts())
        };
        for (auto& entry : draws) {
            auto uses = entry.source->resourceUses();
            for (auto descriptor : uses.descriptors)
                rr.resources.RemovePreparedDescriptor(descriptor);
            entry.source->clearResources();
            auto resolved = entry.source->resolveResources(
                rr.resources.TextureContext().session().device(), context);
            if (resolved.is_err())
                return Err(RenderPrepareError::Draw(entry.id,
                                                    entry.source->parameters().render_item,
                                                    rstd::move(resolved).unwrap_err()));
        }
        uniform_update_owners.clear();
        SceneUniformBindingPrepareContext uniform_prepare_impl(scene);
        auto uniform_prepare = dyn<UniformBindingPrepareContext>::from_ref(uniform_prepare_impl);
        Vec<resource::BufferUseHandle> prepared_uniform_buffers;
        uniform_cache.begin(scene.ResourceGeneration());
        for (auto& entry : draws) {
            auto                  uses = entry.source->resourceUses();
            PreparedPassResources resources(rr.resources.Prepared(), uses);
            auto                  created = entry.source->createUniformBufferUpdates(
                uniform_prepare.as_ref(), resources, uniform_cache);
            if (created.is_err()) {
                uniform_cache.prune();
                auto error = rstd::move(created).unwrap_err_unchecked();
                return Err(RenderPrepareError::Draw(entry.id,
                                                    entry.source->parameters().render_item,
                                                    DrawPrepareError::Uniform(rstd::move(error))));
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
        uniform_cache.prune();
        for (auto& entry : draws) entry.invalidation = PassInvalidationNone;
        return Ok(empty {});
    }
    bool acceptsTextureContent(Scene& scene, const RenderSceneSnapshot& snapshot,
                               Arc<SceneImageSource> source) {
        if (! textures) return false;
        SnapshotImportedTextureProvider provider(snapshot, source.clone());
        for (const auto& texture : textures->textures()) {
            const auto& spec = texture.spec;
            if (! spec.definition.valid()) return false;
            if (spec.request().kind == TextureRequestKind::Imported) {
                auto request = spec.request().clone();
                auto id      = snapshot.textureDescId(spec.request().name.as_str());
                if (! id) return false;
                auto record = snapshot.textureDesc(*id);
                if (! record || record->desc.resource.id() != spec.definition.id() ||
                    record->desc.resource.revision() != spec.definition.revision())
                    return false;
                request.source = Some(resource::TextureDefinitionId { id->index, id->generation });
                auto shape     = provider.Declare(request);
                if (shape.is_err() || ! spec.metadata || *shape != *spec.metadata) return false;
            } else if (spec.request().kind == TextureRequestKind::RenderTarget) {
                if (auto target = scene.RenderTarget(spec.request().name.as_str()))
                    if (! SameTextureRequest(
                            spec.request(),
                            MakeRenderTargetTextureRequest(spec.request().name.as_str(), **target)))
                        return false;
            }
        }
        for (auto& draw : draws)
            if (draw.source->finalizeResourceRequests(scene) != PassInvalidationNone) return false;
        for (auto& entry : resource_plan.textures) {
            if (entry.request().kind != TextureRequestKind::Imported) continue;
            auto id = snapshot.textureDescId(entry.request().name.as_str());
            if (! id) return false;
            auto record = snapshot.textureDesc(*id);
            if (! record || record->desc.resource.id() != entry.definition.id() ||
                record->desc.resource.revision() != entry.definition.revision())
                return false;
            entry.source = Some(resource::TextureDefinitionId { id->index, id->generation });
        }
        image_source = Some(rstd::move(source));
        return true;
    }

    Vec<PreparedPassDiagnostic>
    diagnostics(bool prepared, const vrento::vulkan::PreparedGraphicsResources* graphics,
                slice<vrento::vulkan::PassInput> compiled = {}) const {
        Vec<PreparedPassDiagnostic> result;
        if (! input_) return result;
        if (! compiled.is_empty() && compiled[usize()].scope() != input_->graph().scope())
            compiled = {};
        if (compiled.is_empty()) compiled = input_->descriptions();
        for (auto index : input_->graph().inspect().order) {
            const auto& node = input_->graph().passes()[usize(index)];
            Vec<String> releases;
            for (const auto& life : input_->graph().inspect().lifetimes)
                if (life.first != fg::invalid_index &&
                    input_->graph()
                            .passes()[usize(input_->graph().inspect().order[usize(life.last)])]
                            .id == node.id)
                    if (auto texture = textures->find(life.version.resource))
                        releases.push(texture->spec.key.clone());
            PassInvalidationFlags invalidation = PassInvalidationNone;
            for (const auto& entry : draws)
                if (entry.id == node.id) invalidation = entry.invalidation;
            PreparedPassDiagnostic value { .pass_id            = Some(fg::PassId(node.id)),
                                           .pass_name          = node.name.clone(),
                                           .invalidation_flags = invalidation,
                                           .release_textures   = rstd::move(releases) };
            for (const auto& desc : compiled)
                if (desc.execution().id == node.id) {
                    value.scope     = desc.scope();
                    value.revision  = desc.revision();
                    value.pass_kind = Some(fg::Serial(desc.execution().kind));
                    break;
                }
            if (auto source = draw(input_->graph().scope(), node.id)) {
                value.render_item      = (*source)->renderItemId();
                value.texture_requests = (*source)->textureRequestDiagnostics();
                value.prepared         = prepared;
                if (graphics && (*source)->parameters().pipeline_use) {
                    auto prepared = graphics->pipeline(*(*source)->parameters().pipeline_use);
                    if (prepared) {
                        const auto& pipeline                = **prepared;
                        value.pipeline_cache_key            = Some(pipeline.cache_key.clone());
                        value.pipeline_cache_hit            = pipeline.cache_hit;
                        value.pipeline_cache_observed_count = pipeline.cache_observed_count;
                        value.render_pass_cache_key = Some(pipeline.render_pass_key.clone());
                        value.render_pass_cache_hit = pipeline.render_pass_cache_hit;
                        value.render_pass_cache_observed_count =
                            pipeline.render_pass_cache_observed_count;
                    }
                }
            } else {
                if (textures) value.texture_requests = textures->textureRequestDiagnostics(node);
                value.prepared = prepared;
            }
            result.push(rstd::move(value));
        }
        return result;
    }

    bool update(const SceneFrame& frame, VkExtent2D extent,
                ref<dyn<SceneTextureAnimationView>> textures, RenderingResources& rr,
                SceneRenderUpdateProgress& progress) {
        auto buffer_writer = dyn<resource::BufferContentWriter>::from_ref(rr.resources);
        SceneRenderUniformFrameContext frame_context(
            frame,
            { static_cast<float>(extent.width), static_cast<float>(extent.height) },
            textures);
        auto context = dyn<UniformBufferFrameContext>::from_ref(frame_context);
        for (; progress.uniforms < uniform_update_owners.len(); ++progress.uniforms) {
            auto& binding = uniform_update_owners[progress.uniforms];
            auto  result  = binding->Update(context.as_ref(), buffer_writer.as_mut_ref());
            if (result.is_err()) {
                auto error = rstd::move(result).unwrap_err_unchecked();
                rstd_error("update uniform buffer failed: {}", error.message.as_str());
                return false;
            }
        }
        auto graphics = dyn<resource_registry::GraphicsResourcePreparer>::from_ref(rr.resources);
        for (; progress.draws < draws.len(); ++progress.draws) {
            auto&                 entry = draws[progress.draws];
            auto                  uses  = entry.source->resourceUses();
            PreparedPassResources resources(rr.resources.Prepared(), uses);
            DrawUpdateContext     update_context {
                .buffers   = buffer_writer.as_mut_ref(),
                .resources = ref<PreparedPassResources>::from_raw_parts(&resources),
                .graphics  = graphics.as_mut_ref(),
                .textures  = textures
            };
            if (! entry.source->update(update_context)) return false;
        }
        for (auto& entry : draws) {
            entry.source->acknowledgeGeometry();
            entry.invalidation &= ~ToPassInvalidationFlags(PassInvalidation::Content);
        }
        return true;
    }

    auto captureFrame(RenderingResources&, const vrento::vulkan::CompiledGraph&,
                      const vrento::vulkan::TextureOutput&, vrento::vulkan::SurfaceTarget,
                      Option<resource::DescriptorBindingHandle>)
        -> Result<vrento::vulkan::GraphFrameRequest, vrento::vulkan::GraphExecutionError>;
};
} // namespace owe::vulkan
