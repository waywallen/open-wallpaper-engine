module;
#include <rstd/macro.hpp>
module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import eigen;
import wescene.vulkan;
import wescene.scene;
using namespace owe;
using namespace rstd::literals;
using namespace rstd::prelude;
using rstd::collections::BTreeSet;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;
namespace fg = vrento::fg;
using vrento::pass::ContentRef;
namespace
{
auto NextSceneRenderScope() -> rstd::uint64_t {
    static rstd::sync::atomic::Atomic<u64> next { u64(1) };
    return next.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed).to_primitive();
}
struct ExtraInfo {
    fg::FrameGraphBuilder              graph;
    vulkan::SceneRenderTextures        textures;
    Vec<vulkan::SceneRenderDrawSource> draws;
    Vec<vrento::vulkan::PassInput>     descriptions;

    HashMap<String, ContentRef> selected;
    Option<fg::Error>           error;
    fg::Serial                  next_pass { 1 };
    Arc<SceneImageSource>       image_source;
    Scene*                      scene;
    const RenderSceneSnapshot*  render_scene;
    HashSet<String>             depth_initialized_outputs;
    HashSet<String>             transient_texture_families;
    Option<ContentRef>          mip_framebuffer_history;
    struct DepthDraw {
        SceneNode* node;
        bool       writes_depth;
    };
    Vec<DepthDraw> pending_depth_draws;
    ExtraInfo(Scene& value, const RenderSceneSnapshot& snapshot)
        : graph(NextSceneRenderScope()),
          textures(u64(graph.data().scope)),
          image_source(value.CaptureImageSource()),
          scene(&value),
          render_scene(&snapshot) {}
};
template<typename T>
auto Remember(ExtraInfo& extra, Result<T, fg::Error> result) -> T {
    if (result.is_err()) {
        if (! extra.error) extra.error = Some(result.unwrap_err());
        return T {};
    }
    return rstd::move(result).unwrap();
}
auto SelectTexture(ExtraInfo& extra, const vulkan::SceneRenderTextureSpec& spec) -> ContentRef {
    if (auto selected = extra.selected.get(spec.key.as_str()); selected) return **selected;
    auto key      = spec.key.clone();
    auto declared = spec.clone();
    if (declared.imported && declared.definition.valid()) {
        vulkan::SnapshotImportedTextureProvider provider(*extra.render_scene,
                                                         extra.image_source.clone());
        auto metadata = provider.Declare(declared.source_request());
        if (metadata.is_err()) {
            rstd_error("declare texture failed: {}", metadata.unwrap_err().message);
            extra.error = Some(fg::Error { fg::ErrorKind::MissingResource });
            return {};
        }
        declared.metadata = Some(metadata.unwrap());
    }
    auto version = Remember(extra, extra.textures.initial(extra.graph, rstd::move(declared)));
    if (version.valid()) (void)extra.selected.insert(rstd::move(key), version.clone());
    return version;
}
void PublishTexture(ExtraInfo& extra, ContentRef version) {
    auto texture = extra.textures.find(version);
    if (texture) (void)extra.selected.insert(texture->spec.key.clone(), version.clone());
}
auto Use(ExtraInfo& extra, ContentRef version) -> resource::TextureUseHandle {
    return Remember(extra, extra.textures.handle(version));
}
void ImportHistory(ExtraInfo& extra, ContentRef version) {
    Remember(extra, extra.textures.import_history(extra.graph, version));
}
template<typename Callback>
void EmitDraw(ExtraInfo& extra, ref<str> name, Callback&& callback) {
    auto id = Remember(extra, extra.graph.add_pass({ extra.next_pass++ }, {}, String::make(name)));
    vulkan::SceneDraw::Input input {};
    callback(id, input);
    if (extra.error) return;
    auto draw = Box<vulkan::SceneDraw>::make(rstd::move(input));
    (void)draw->finalizeResourceRequests(*extra.scene);
    extra.draws.push({ id, rstd::move(draw) });
}
auto AddTextureCopy(ExtraInfo& extra, ContentRef source, ContentRef target) -> ContentRef {
    if (extra.error) return {};
    auto id     = Remember(extra, extra.graph.add_pass({ extra.next_pass++ }, { 1 }, "copy"_Str));
    auto output = Remember(extra, extra.textures.output(extra.graph, id, target));
    if (extra.error) return {};
    fg::ResourceRange range { .aspect = extra.textures.shape(extra.graph, source)->aspects };
    auto              declared =
        vrento::vulkan::declare_texture_copy_output(extra.graph, id, source, output, range, range);
    if (declared.is_err()) {
        extra.error = Some(declared.unwrap_err());
        return {};
    }
    (void)Use(extra, source);
    (void)Use(extra, output);
    extra.descriptions.push(declared.unwrap());
    auto shape = extra.textures.shape(extra.graph, output);
    if (shape->mip_levels > 1) {
        auto mip = vrento::vulkan::declare_texture_mips(
            extra.graph, { extra.next_pass++ }, output, shape->mip_levels, shape->layers);
        if (mip.is_err()) {
            extra.error = Some(mip.unwrap_err());
            return {};
        }
        output = mip->output;
        (void)Use(extra, output);
        extra.descriptions.push(rstd::move(mip->description));
    }
    PublishTexture(extra, output);
    return output;
}
} // namespace
namespace
{
static vulkan::SceneRenderTextureSpec MakeTextureDesc(ExtraInfo& extra, ref<str> key) {
    if (auto existing = extra.textures.find(key)) return existing->spec.clone();
    vulkan::SceneRenderTextureSpec desc { .key = String::make(key), .imported = ! IsSpecTex(key) };
    if (desc.imported) {
        if (auto id = extra.render_scene->textureDescId(key)) {
            if (auto record = extra.render_scene->textureDesc(*id)) {
                desc.definition = record->desc.resource.clone();
                desc.source     = Some(resource::TextureDefinitionId { id->index, id->generation });
            }
        }
    } else if (auto id = extra.render_scene->renderTargetDescId(key)) {
        if (auto record = extra.render_scene->renderTargetDesc(*id))
            desc.definition = record->desc.resource.clone();
    } else if (auto definition = extra.scene->TargetResource(key)) {
        desc.definition = definition.unwrap();
    }
    if (extra.transient_texture_families.contains(key))
        desc.allocation_family = Some(String::make(key));
    // Scene-owned color history starts transparent; ordinary undefined reads remain invalid.
    desc.initialize_history_transparent = ! desc.imported && desc.definition.valid();
    return desc;
}

static ref<str> ResolveEffectTarget(const SceneNodeLayer& layer, const SceneEffectTarget& target) {
    if (target.kind == SceneEffectTargetKind::Named && ! target.key.is_empty())
        return target.key.as_str();
    return layer.CompositeTarget();
}

static bool LoadsPreviousAttachment(BlendMode mode) {
    return mode == BlendMode::Translucent || mode == BlendMode::Additive ||
           mode == BlendMode::AlphaToCoverage;
}

static void AddTextureCopy(ExtraInfo& extra, vulkan::SceneRenderTextureSpec in,
                           vulkan::SceneRenderTextureSpec out) {
    auto source = SelectTexture(extra, in);
    auto target = SelectTexture(extra, out);
    (void)AddTextureCopy(extra, source, target);
}

static ContentRef AddTextureCopy(ExtraInfo& extra, ContentRef in,
                                 Option<vulkan::SceneRenderTextureSpec> out_desc = None()) {
    auto source = extra.textures.find(in);
    if (! source) {
        extra.error = Some(fg::Error { fg::ErrorKind::MissingResource, {} });
        return {};
    }
    auto target = out_desc ? out_desc->clone() : source->spec.clone();
    if (! out_desc)
        target.key.push_str(rstd::format("_{}_{}_copy",
                                         in.output() ? in.output()->producer.value : 0,
                                         in.output() ? in.output()->index : 0)
                                .as_str());
    if (! out_desc) {
        auto request      = target.request().clone();
        request.name      = target.key.clone();
        request.source    = None();
        target.definition = resource::TextureResourceRef(rstd::move(request));
        target.source     = None();
    }
    auto initial = SelectTexture(extra, target);
    return AddTextureCopy(extra, in, initial);
}

static ContentRef AddMipFramebufferHistory(ExtraInfo& extra) {
    if (extra.mip_framebuffer_history.is_some()) {
        return *extra.mip_framebuffer_history;
    }

    auto history_desc     = MakeTextureDesc(extra, WE_MIP_MAPPED_FRAME_BUFFER);
    history_desc.imported = false;
    auto history          = SelectTexture(extra, history_desc);
    ImportHistory(extra, history);
    extra.mip_framebuffer_history = Some(history.clone());
    return history;
}

static void StoreMipFramebufferHistory(ExtraInfo& extra) {
    if (extra.mip_framebuffer_history.is_none()) return;

    auto history_desc     = MakeTextureDesc(extra, WE_MIP_MAPPED_FRAME_BUFFER);
    history_desc.imported = false;
    AddTextureCopy(extra, MakeTextureDesc(extra, SpecTex_Default), rstd::move(history_desc));
}

static void AddMaterialTextureReads(SceneMaterial& material, ref<str> pass_output, ExtraInfo& extra,
                                    fg::PassId pass, vulkan::SceneDraw::Desc& pdesc,
                                    bool reuses_previous_output) {
    auto snapshots = HashMap<String, ContentRef>::make();
    for (rstd::size_t index = 0; index < material.textures.len().to_primitive(); ++index) {
        if (index >= material.texture_sources.len().to_primitive()) {
            extra.error = Some(fg::Error { fg::ErrorKind::MissingResource, pass });
            return;
        }
        const auto& source  = material.texture_sources[usize(index)];
        const bool  sampled = material.customShader.shader &&
                              ! (**material.customShader.shader).SamplerMember(index).is_empty();
        Option<ContentRef> input;
        ref<str>           binding_key = ""_str;
        if (source.kind == SceneMaterialTextureSourceKind::Empty) {
            pdesc.texture_bindings.push(vulkan::TextureBindingRequest {});
            continue;
        }
        if (source.kind == SceneMaterialTextureSourceKind::UnsupportedSpecial) {
            rstd_error("material '{}' references unsupported scene texture '{}'",
                       material.name,
                       source.key);
            pdesc.texture_bindings.push(vulkan::TextureBindingRequest {});
            continue;
        }
        if (source.kind == SceneMaterialTextureSourceKind::LayerOutput) {
            auto* link = extra.render_scene != nullptr && source.wallpaper_layer >= i32()
                             ? extra.render_scene->linkSource(WallpaperLayerId {
                                   .value = source.wallpaper_layer,
                               })
                             : nullptr;
            if (link == nullptr || source.layer.is_none() || link->scene_node != *source.layer ||
                extra.render_scene->renderTargetDesc(link->render_target) == nullptr) {
                rstd_error("material '{}' has unresolved linked layer {}",
                           material.name,
                           source.wallpaper_layer);
                pdesc.texture_bindings.push(vulkan::TextureBindingRequest {});
                continue;
            }
            binding_key = link->render_target_key.as_str();
        } else {
            binding_key = source.key.as_str();
        }
        if (! sampled) {
            auto metadata = MakeTextureDesc(extra, binding_key);
            pdesc.texture_bindings.push(vulkan::TextureBindingRequest {
                .name    = String::make(binding_key),
                .request = metadata.definition.valid() ? Some(metadata.request().clone())
                                                       : None<vulkan::TextureRequest>(),
            });
            continue;
        }
        if (source.kind == SceneMaterialTextureSourceKind::LayerOutput) {
            input = Some(SelectTexture(extra, MakeTextureDesc(extra, binding_key)));
            // Forward links sample the previous frame; rebinding to the final producer
            // would cycle when that producer also samples the consumer's framebuffer.
            ImportHistory(extra, *input);
        } else {
            auto name = binding_key;
            if (source.kind == SceneMaterialTextureSourceKind::MipMappedFramebuffer) {
                input = Some(AddMipFramebufferHistory(extra));
            } else {
                input = Some(SelectTexture(extra, MakeTextureDesc(extra, binding_key)));
            }
            if (IsSpecTex(name) && ! name.starts_with(WE_MIP_MAPPED_FRAME_BUFFER)) {
                ImportHistory(extra, *input);
            }
        }

        if (binding_key == pass_output && reuses_previous_output) {
            auto key      = binding_key;
            auto snapshot = snapshots.get(key);
            if (snapshot.is_some()) {
                input = Some<ContentRef>(ContentRef(**snapshot));
            } else {
                ImportHistory(extra, *input);
                input = Some(AddTextureCopy(extra, *input));
                (void)snapshots.insert(String::make(key), input->clone());
            }
        }

        auto sampled_state = extra.textures.find(*input);
        if (! sampled_state) {
            extra.error = Some(fg::Error { fg::ErrorKind::MissingResource, pass });
            return;
        }
        auto sampled_key = sampled_state->spec.key.as_str();
        pdesc.texture_bindings.push(vulkan::TextureBindingRequest {
            .name    = String::make(sampled_key),
            .use     = Some(Use(extra, *input)),
            .request = sampled_state->spec.definition.valid()
                           ? Some(sampled_state->spec.request().clone())
                           : None<vulkan::TextureRequest>(),
        });
    }
}

static SceneNodeLayer* ToGraphPass(SceneNode* node, ref<str> output, ExtraInfo& extra,
                                   bool                defer_effect = false,
                                   SceneRenderViewKind render_view  = SceneRenderViewKind::Primary);

static void LoadGraphEffects(SceneNodeLayer* effs, ExtraInfo& extra) {
    for (auto* eff : effs->ResolvedEffects()) {
        if (eff == nullptr) continue;
        usize command_index {};
        int   nodePos = 0;
        for (auto& n : eff->Nodes()) {
            if (command_index < eff->commands.len() &&
                nodePos == eff->commands[command_index].afterpos.to_primitive()) {
                const auto& command = eff->commands[command_index];
                auto        source  = ResolveEffectTarget(*effs, command.src);
                auto        target  = ResolveEffectTarget(*effs, command.dst);
                AddTextureCopy(
                    extra, MakeTextureDesc(extra, source), MakeTextureDesc(extra, target));
                ++command_index;
            }
            auto target = effs->ResolvedTarget(*n);
            ToGraphPass(n->sceneNode.as_ptr(), ResolveEffectTarget(*effs, target), extra);
            nodePos++;
        }
    }
}

static SceneNodeLayer* ToGraphPass(SceneNode* node, ref<str> output, ExtraInfo& extra,
                                   bool defer_effect, SceneRenderViewKind render_view) {
    auto& scene = *extra.scene;

    if (node->Mesh() == nullptr) return nullptr;
    auto* mesh = node->Mesh();
    if (mesh->Submeshes().is_empty()) return nullptr;
    const auto& slots = mesh->MaterialSlots();

    SceneNodeLayer* imgeff = nullptr;
    if (node->HasLayer()) {
        auto*      effect       = node->Layer().as_ptr().as_raw_ptr();
        const bool intermediate = effect->RequiresIntermediateTarget();
        effect->ConfigureSourceDraw(intermediate);
        if (intermediate) {
            imgeff = effect;
            imgeff->ResolveEffect(*scene.DefaultEffectMesh(), "effect"_str);
            output = imgeff->CompositeTarget();
            (void)extra.transient_texture_families.insert(String::make(output));
        }
    }
    if (imgeff != nullptr) {
        for (auto& prefill : imgeff->PrefillNodes()) {
            auto prefill_output = ResolveEffectTarget(*imgeff, prefill.output);
            ToGraphPass(prefill.sceneNode.as_ptr(), prefill_output, extra);
        }
    }

    const bool draw_source = imgeff == nullptr || imgeff->RequiresSourceDraw();
    for (rstd::size_t smi = 0; draw_source && smi < mesh->Submeshes().len().to_primitive(); smi++) {
        const auto& submesh       = mesh->Submeshes()[usize(smi)];
        const auto  material_slot = submesh.material_slot.to_primitive();
        if (material_slot >= slots.len().to_primitive() || ! slots[usize(material_slot)]) continue;
        SceneMaterial* material = slots[usize(material_slot)].as_ptr().as_raw_ptr();
        scene.ResolveMaterialTextureSources(*material);
        Option<Arc<SceneMaterial>> material_override;
        if (imgeff != nullptr && submesh.output_override.is_empty() && ! submesh.preserve_output) {
            auto source_blend = imgeff->IntermediateSourceBlend();
            if (source_blend.is_some()) {
                material_override = Some(Arc<SceneMaterial>::make(*material));
                (*material_override)->SetBlendMode(*source_blend);
            }
        }
        auto pass_name = material->name.as_str();
        // Per-submesh output override (clipping-mask submeshes write into a
        // shared RT that the main puppet pass samples via g_Texture8).
        ref<str> pass_output =
            submesh.output_override.is_empty() ? output : submesh.output_override.as_str();

        EmitDraw(
            extra,
            pass_name,
            [material,
             node,
             smi,
             pass_output,
             material_override = material_override.clone(),
             preserve_output   = submesh.preserve_output,
             render_view,
             &scene,
             &extra](fg::PassId pass, vulkan::SceneDraw::Input& input) {
                auto& pdesc                     = input.description;
                input.updates.node              = Some(mut_ref<SceneNode>::from_raw_parts(node));
                pdesc.submesh_index             = u32(static_cast<rstd::uint32_t>(smi));
                pdesc.graph_pass_index          = usize(pass.value);
                pdesc.render_view               = render_view;
                input.updates.material_override = material_override.clone();
                if (auto node_id = scene.ResourceIndex().nodeId(*node)) {
                    if (auto draw_item = scene.ResourceIndex().drawItemFor(
                            *node_id, u32(static_cast<rstd::uint32_t>(smi)))) {
                        pdesc.draw_item = *draw_item;
                        if (extra.render_scene != nullptr) {
                            if (auto render_item = extra.render_scene->renderItemFor(*draw_item)) {
                                pdesc.render_item = *render_item;
                            }
                        }
                    }
                }
                auto pass_output_s = pass_output;
                auto output_rt     = scene.RenderTarget(pass_output_s);
                if (! output_rt) {
                    extra.error = Some(fg::Error { fg::ErrorKind::MissingResource, pass });
                    return;
                }
                const auto& output_target = **output_rt;
                const auto* pass_material =
                    material_override ? material_override->as_ptr().as_raw_ptr() : material;
                const bool reuses_previous_output =
                    ! (output_target.force_clear && ! preserve_output) &&
                    (output_target.preserve_on_write || preserve_output ||
                     LoadsPreviousAttachment(pass_material->Pipeline().blend_mode));

                pdesc.output = rstd::into(pass_output_s);
                AddMaterialTextureReads(
                    *material, pass_output, extra, pass, pdesc, reuses_previous_output);

                auto output_node =
                    Remember(extra,
                             extra.textures.output(
                                 extra.graph,
                                 pass,
                                 SelectTexture(extra, MakeTextureDesc(extra, pass_output_s))));
                auto output_state = extra.textures.find(output_node);
                if (! output_state) {
                    extra.error = Some(fg::Error { fg::ErrorKind::MissingResource, pass });
                    return;
                }
                const bool first_output_write = output_node.first_output();
                if (! first_output_write && reuses_previous_output) {
                    Remember(extra, extra.textures.reuse_storage(extra.graph, output_node));
                }
                pdesc.output_use     = Some(Use(extra, output_node));
                pdesc.output_request = output_state->spec.definition.valid()
                                           ? Some(output_state->spec.request().clone())
                                           : None<vulkan::TextureRequest>();
                pdesc.samples        = vulkan::TextureSampleCount(output_target.sample_count);
                if (pdesc.samples != VK_SAMPLE_COUNT_1_BIT) {
                    auto twin_name = vulkan::MsaaTwinName(pdesc.output.as_str(), pdesc.samples);
                    pdesc.output_msaa_request = Some(vulkan::MakeMsaaTextureRequest(
                        twin_name.as_str(), output_target, pdesc.samples));
                    auto msaa_node            = Remember(
                        extra,
                        extra.textures.output(
                            extra.graph,
                            pass,
                            SelectTexture(
                                extra,
                                vulkan::SceneRenderTextureSpec {
                                    .key      = twin_name.clone(),
                                    .imported = false,
                                    .definition =
                                        extra.scene
                                            ->TargetResource(
                                                pdesc.output.as_str(),
                                                resource::TextureRequestKind::RenderTargetMsaa,
                                                u32(static_cast<unsigned>(pdesc.samples)))
                                            .unwrap(),
                                })));
                    auto msaa_state = extra.textures.find(msaa_node);
                    if (msaa_state) pdesc.output_msaa_use = Some(Use(extra, msaa_node));
                    PublishTexture(extra, msaa_node);
                }
                pdesc.transparent_clear = first_output_write && output_target.clear_on_first_write;
                pdesc.clear_output =
                    ! preserve_output &&
                    ((first_output_write && output_target.bind.screen) || pdesc.transparent_clear);
                pdesc.preserve_output      = ! output_node.first_output() &&
                                             (output_target.preserve_on_write || preserve_output);
                const bool uses_depth      = output_target.withDepth &&
                                             vulkan::UsesDepthAttachment(pass_material->Pipeline());
                pdesc.has_depth_attachment = uses_depth;
                if (uses_depth) {
                    auto depth_name = rstd::format("{}::depth", pass_output_s);
                    pdesc.depth_request =
                        Some(vulkan::MakeDepthTextureRequest(depth_name.as_str(), output_target));
                    auto depth_node = Remember(
                        extra,
                        extra.textures.output(
                            extra.graph,
                            pass,
                            SelectTexture(
                                extra,
                                vulkan::SceneRenderTextureSpec {
                                    .key      = depth_name.clone(),
                                    .imported = false,
                                    .definition =
                                        extra.scene
                                            ->TargetResource(
                                                pass_output_s,
                                                resource::TextureRequestKind::DepthAttachment,
                                                u32(output_target.sample_count))
                                            .unwrap(),
                                })));
                    auto depth_state = extra.textures.find(depth_node);
                    if (depth_state) pdesc.depth_use = Some(Use(extra, depth_node));
                    PublishTexture(extra, depth_node);
                }
                pdesc.clear_depth =
                    uses_depth && (pdesc.clear_output || output_target.force_clear ||
                                   ! extra.depth_initialized_outputs.contains(pass_output_s));
                if (uses_depth) {
                    (void)extra.depth_initialized_outputs.insert(String::make(pass_output_s));
                } else if (pdesc.clear_output || output_target.force_clear) {
                    (void)extra.depth_initialized_outputs.remove(pass_output_s);
                }
                PublishTexture(extra, output_node);
            });
    }

    if (! defer_effect && imgeff != nullptr && imgeff->HasRenderEffects())
        LoadGraphEffects(imgeff, extra);
    return imgeff;
}

// Bottom-up collect: identify SceneNode subtrees whose every node can be
// elided without losing a link source. Visibility-hidden ancestors also hide
// anonymous/generated descendants such as particle children, so the skip set
// is keyed by node pointer instead of WE layer id.
static bool CollectEmitSkipSubtrees(SceneNode* node, Scene& scene, const BTreeSet<i32>& linked_ids,
                                    HashSet<const SceneNode*>& out_skip,
                                    bool                       visibility_hidden_ancestor = false) {
    const auto wallpaper   = node->WallpaperIdentity();
    const auto link_source = scene.ResolveLayerLinkSource(*node);
    const i32 layer_id = link_source.is_some() ? link_source->value
                                               : (wallpaper.is_some() ? wallpaper->value : i32(-1));
    const bool linked  = link_source.is_some() && linked_ids.contains(link_source->value);
    const bool visibility_hidden_self =
        (! node->Visible() || (layer_id >= i32() && scene.IsLayerVisibilityElidable(
                                                        WallpaperLayerId { .value = layer_id }))) &&
        ! linked;
    const bool visibility_hidden =
        linked ? false : visibility_hidden_ancestor || visibility_hidden_self;

    bool all_children_skippable = true;
    for (auto& c : node->GetChildren()) {
        if (! CollectEmitSkipSubtrees(c.as_ptr(), scene, linked_ids, out_skip, visibility_hidden))
            all_children_skippable = false;
    }
    const bool self_skippable =
        ! linked &&
        (visibility_hidden ||
         (layer_id >= i32() && scene.IsLayerElidable(WallpaperLayerId { .value = layer_id })));
    if (self_skippable && all_children_skippable) {
        (void)out_skip.insert(node);
        return true;
    }
    return false;
}

static bool ShouldSkipNoRuntimeEffect(SceneNode* node, Scene& scene) {
    (void)scene;
    if (node == nullptr || ! node->HasLayer()) return false;
    const auto& effect_layer = node->Layer();
    return effect_layer && effect_layer->SkipWhenNoRuntimeEffect() &&
           ! effect_layer->PublishesOutput() && effect_layer->EffectCount() > usize() &&
           ! effect_layer->HasRuntimeVisibleEffect();
}

static bool HasExternalRenderTargetRead(const SceneMaterial& material, ref<str> local_target) {
    for (const auto& texture : material.textures) {
        auto name = texture.as_str();
        if (! IsLocalSceneMaterialTextureDependency(ClassifySceneMaterialTexture(name)) &&
            name != local_target)
            return true;
    }
    return false;
}

static Option<bool> DepthDrawKind(SceneNode& node, Scene& scene) {
    auto* mesh = node.Mesh();
    if (mesh == nullptr || mesh->Submeshes().is_empty()) return None();
    if (! node.HasLayer() && ! node.Camera().is_empty()) return None();
    if (node.Perspective()) {
        auto active = scene.ActiveCamera();
        auto global = scene.Camera("global_perspective"_str);
        if (active.is_none() || global.is_none() ||
            (*active).as_raw_ptr() != (*global).as_raw_ptr())
            return None();
    }
    Option<bool> writes_depth;
    for (const auto& submesh : mesh->Submeshes()) {
        if (! submesh.output_override.is_empty() || submesh.preserve_output) return None();
        auto slot = rstd::as_cast<usize>(submesh.material_slot);
        if (slot >= mesh->MaterialSlots().len() || ! mesh->MaterialSlots()[slot]) return None();
        const auto& material = *mesh->MaterialSlots()[slot];
        if (! material.Pipeline().depth_test || HasExternalRenderTargetRead(material, ""_str))
            return None();
        const bool writes = material.Pipeline().depth_write;
        const auto blend  = material.Pipeline().blend_mode;
        if ((writes && (blend == BlendMode::Translucent || blend == BlendMode::Additive)) ||
            (! writes && ! LoadsPreviousAttachment(blend)))
            return None();
        if (writes_depth.is_some() && *writes_depth != writes) return None();
        writes_depth = Some<bool>(writes);
    }
    if (node.HasLayer()) {
        auto& layer = *node.Layer();
        if (layer.PublishesOutput() || ! layer.PrefillNodes().is_empty() ||
            ! layer.FinalCamera().is_empty() || layer.FinalTarget() != SpecTex_Default ||
            ! layer.FinalDepthTest() || layer.FinalDepthWrite() != *writes_depth ||
            (layer.FinalDepthWrite() && (layer.FinalBlend() == BlendMode::Translucent ||
                                         layer.FinalBlend() == BlendMode::Additive)) ||
            (! layer.FinalDepthWrite() && ! LoadsPreviousAttachment(layer.FinalBlend())))
            return None();
        layer.ResolveEffect(*scene.DefaultEffectMesh(), "effect"_str);
        for (auto* effect : layer.ResolvedEffects()) {
            if (effect == nullptr || ! effect->commands.is_empty()) return None();
            for (const auto& entry : effect->Nodes()) {
                auto target = layer.ResolvedTarget(*entry);
                auto output = ResolveEffectTarget(layer, target);
                if (output != layer.CompositeTarget() && output != SpecTex_Default) return None();
                auto* effect_mesh = entry->sceneNode->Mesh();
                if (effect_mesh == nullptr) return None();
                for (const auto& material : effect_mesh->MaterialSlots()) {
                    if (material && HasExternalRenderTargetRead(*material, layer.CompositeTarget()))
                        return None();
                }
            }
        }
    }
    return writes_depth;
}

static void FlushDepthDraws(ExtraInfo& extra) {
    for (bool writes : { true, false }) {
        for (const auto& draw : extra.pending_depth_draws) {
            if (draw.writes_depth == writes) ToGraphPass(draw.node, SpecTex_Default, extra);
        }
    }
    extra.pending_depth_draws.clear();
}

static void ConfigureNestedOutput(SceneNode* node, ref<str> output, ref<str> inherited_camera) {
    if (! inherited_camera.is_empty() && node->Camera().is_empty()) {
        node->SetCamera(inherited_camera);
    }
    if (! node->HasLayer()) return;
    auto& effect_layer = node->Layer();
    if (! effect_layer) return;
    auto output_text = output;
    if (output_text != SpecTex_Default && effect_layer->FinalTarget() == SpecTex_Default) {
        effect_layer->SetFinalTarget(output);
    }
    if (effect_layer->FinalTarget() == output) {
        effect_layer->SetFinalCamera(inherited_camera);
    }
}

static void EmitSceneNode(SceneNode* node, ref<str> inherited_output, ref<str> inherited_camera,
                          ExtraInfo& extra, const HashSet<const SceneNode*>& emit_skip_subtrees,
                          const BTreeSet<i32>& linked_ids, bool hidden_ancestor = false) {
    if (node == nullptr || emit_skip_subtrees.contains(node)) return;

    auto&      scene       = *extra.scene;
    const auto wallpaper   = node->WallpaperIdentity();
    const auto link_source = scene.ResolveLayerLinkSource(*node);
    const i32 layer_id = link_source.is_some() ? link_source->value
                                               : (wallpaper.is_some() ? wallpaper->value : i32(-1));
    const bool elidable     = scene.IsLayerElidable(WallpaperLayerId { .value = layer_id });
    const bool linked       = link_source.is_some() && linked_ids.contains(link_source->value);
    const bool hidden       = hidden_ancestor || (! node->Visible() && ! linked);
    const bool child_hidden = linked ? false : hidden;
    bool       emit         = ! hidden || linked;

    ref<str> node_output = inherited_output;

    if (! linked && ShouldSkipNoRuntimeEffect(node, scene)) emit = false;
    if (elidable) {
        if (! linked) {
            emit = false;
        } else {
            auto* source_record = extra.render_scene->linkSource(*link_source);
            if (source_record == nullptr) {
                rstd_error("link render target for layer {} not found in snapshot", layer_id);
                emit = false;
            } else {
                node_output = source_record->render_target_key.as_str();
                if (node->HasLayer() && ! node->Layer()->PublishesOutput()) {
                    node->Layer()->SetFinalTarget(node_output);
                    node->Layer()->SetFinalLocal(true);
                }
            }
        }
    }

    auto group_camera =
        wallpaper.is_some() ? scene.RenderGroupCamera(*wallpaper) : None<ref<str>>();
    if (emit && group_camera) {
        FlushDepthDraws(extra);
        ConfigureNestedOutput(node, node_output, inherited_camera);
        auto* effect_layer = ToGraphPass(node, node_output, extra, true);
        if (effect_layer == nullptr) {
            rstd_error("render group layer {} has no effect target", wallpaper->value);
        }
        const ref<str> child_output =
            effect_layer == nullptr ? node_output : effect_layer->CompositeTarget();
        for (auto& child : node->GetChildren()) {
            EmitSceneNode(child.as_ptr(),
                          child_output,
                          *group_camera,
                          extra,
                          emit_skip_subtrees,
                          linked_ids,
                          child_hidden);
        }
        if (effect_layer != nullptr && effect_layer->HasRenderEffects()) {
            FlushDepthDraws(extra);
            LoadGraphEffects(effect_layer, extra);
        }
        return;
    }

    if (emit) {
        ConfigureNestedOutput(node, node_output, inherited_camera);
        if (node->Mesh() != nullptr) {
            auto camera = scene.ActiveCamera();
            auto kind = ! linked && node_output == SpecTex_Default && inherited_camera.is_empty() &&
                                camera.is_some() && (*camera)->IsPerspective()
                            ? DepthDrawKind(*node, scene)
                            : None<bool>();
            if (kind.is_some()) {
                extra.pending_depth_draws.push({ node, *kind });
            } else {
                FlushDepthDraws(extra);
                ToGraphPass(node, node_output, extra);
            }
        }
    }
    for (auto& child : node->GetChildren()) {
        EmitSceneNode(child.as_ptr(),
                      linked && elidable && emit ? node_output : inherited_output,
                      inherited_camera,
                      extra,
                      emit_skip_subtrees,
                      linked_ids,
                      child_hidden);
    }
}

static bool SamplesPlanarReflection(SceneNode& node) {
    auto* mesh = node.Mesh();
    if (mesh == nullptr) return false;
    for (const auto& material : mesh->MaterialSlots()) {
        if (! material) continue;
        for (const auto& texture : material->textures) {
            if (texture.as_str().starts_with(WE_REFLECTION_PREFIX)) return true;
        }
    }
    return false;
}

static void EmitPlanarReflectionNode(SceneNode* node, ExtraInfo& extra,
                                     const HashSet<const SceneNode*>& emit_skip_subtrees) {
    if (node == nullptr || emit_skip_subtrees.contains(node)) return;
    if (node->Reflected() && ! SamplesPlanarReflection(*node)) {
        ToGraphPass(node, WE_REFLECTION_PREFIX, extra, true, SceneRenderViewKind::Reflection);
    }
    for (auto& child : node->GetChildren()) {
        EmitPlanarReflectionNode(child.as_ptr(), extra, emit_skip_subtrees);
    }
}

static void EmitShadowPasses(ExtraInfo& extra) {
    if (extra.render_scene == nullptr) return;
    auto definitions = extra.render_scene->ShadowDefinitions();
    if (definitions.is_empty()) return;
    const auto& definition = definitions.first().unwrap().get();
    const auto  target     = definition.target.as_str();
    bool        first      = true;

    for (const auto& caster : extra.render_scene->ShadowCasters()) {
        const auto* item = extra.render_scene->renderItem(caster.render_item);
        if (item == nullptr || ! caster.material) continue;
        auto draw = extra.scene->ResourceIndex().resolve(item->scene_draw_item);
        if (draw.is_none() || draw->node == nullptr || draw->mesh == nullptr) continue;
        extra.scene->ResolveMaterialTextureSources(*caster.material);

        EmitDraw(
            extra,
            caster.material->name.is_empty() ? "shadow"_str : caster.material->name.as_str(),
            [&,
             item,
             draw,
             material       = caster.material.clone(),
             instance_count = caster.instance_count,
             clear_depth    = first,
             target](fg::PassId pass, vulkan::SceneDraw::Input& input) {
                auto& pdesc            = input.description;
                input.updates.node     = Some(mut_ref<SceneNode>::from_raw_parts(draw->node));
                pdesc.draw_item        = item->scene_draw_item;
                pdesc.render_item      = item->id;
                pdesc.submesh_index    = item->submesh_index;
                pdesc.graph_pass_index = usize(pass.value);
                input.updates.material_override = Some(material.clone());
                pdesc.depth_only                = true;
                pdesc.instance_count            = instance_count;
                pdesc.output                    = rstd::into(target);
                pdesc.clear_depth               = clear_depth;

                pdesc.viewports.reserve(definition.viewports.len());
                pdesc.scissors.reserve(definition.viewports.len());
                for (const auto& viewport : definition.viewports) {
                    pdesc.viewports.push(VkViewport {
                        .x        = viewport.x.to_primitive(),
                        .y        = viewport.y.to_primitive(),
                        .width    = viewport.width.to_primitive(),
                        .height   = viewport.height.to_primitive(),
                        .minDepth = 0.0f,
                        .maxDepth = 1.0f,
                    });
                    pdesc.scissors.push(VkRect2D {
                        .offset = { viewport.scissor_x.to_primitive(),
                                    viewport.scissor_y.to_primitive() },
                        .extent = { viewport.scissor_width.to_primitive(),
                                    viewport.scissor_height.to_primitive() },
                    });
                }

                AddMaterialTextureReads(*material, target, extra, pass, pdesc, true);
                auto atlas = Remember(
                    extra,
                    extra.textures.output(
                        extra.graph, pass, SelectTexture(extra, MakeTextureDesc(extra, target))));
                auto state = extra.textures.find(atlas);
                if (state == nullptr) return;
                pdesc.depth_use     = Some(Use(extra, atlas));
                pdesc.depth_request = state->spec.definition.valid()
                                          ? Some(state->spec.request().clone())
                                          : None<vulkan::TextureRequest>();
                PublishTexture(extra, atlas);
            });
        first = false;
    }
}

} // namespace

auto owe::BuildSceneRenderState(Scene& scene, const RenderSceneSnapshot& render_scene,
                                fg::AnalysisStatistics* statistics)
    -> Result<vulkan::SceneRenderStateInput, fg::Error> {
    ExtraInfo extra(scene, render_scene);
    auto      clear_id = fg::PassId { extra.next_pass++ };
    auto      result   = SelectTexture(extra, MakeTextureDesc(extra, SpecTex_Default));
    if (extra.error) return Err(*extra.error);
    auto color   = scene.ClearColor();
    auto cleared = vrento::vulkan::declare_graphics_clear(
        extra.graph,
        clear_id,
        result,
        { color[usize()], color[usize(1)], color[usize(2)], 1.0f },
        1,
        "frame/clear"_Str);
    if (cleared.is_err()) return Err(cleared.unwrap_err());
    result = cleared->output.clone();
    PublishTexture(extra, result);
    (void)Use(extra, result);
    extra.descriptions.push(rstd::move(cleared->description));

    if (auto target = scene.RenderTarget(SpecTex_Default)) {
        auto samples = vulkan::TextureSampleCount((**target).sample_count);
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            auto name    = vulkan::MsaaTwinName(SpecTex_Default, samples);
            auto initial = SelectTexture(
                extra,
                { .key        = name.clone(),
                  .definition = scene
                                    .TargetResource(SpecTex_Default,
                                                    resource::TextureRequestKind::RenderTargetMsaa,
                                                    u32(static_cast<unsigned>(samples)))
                                    .unwrap() });
            auto clear = vrento::vulkan::declare_graphics_clear(
                extra.graph,
                fg::PassId { extra.next_pass++ },
                initial,
                { color[usize()], color[usize(1)], color[usize(2)], 1.0f },
                1,
                "frame/clear-msaa"_Str);
            if (clear.is_err()) return Err(clear.unwrap_err());
            initial = clear->output.clone();
            PublishTexture(extra, initial);
            (void)Use(extra, initial);
            extra.descriptions.push(rstd::move(clear->description));
        }
    }

    // The snapshot owns link-consumer discovery; graph build only consumes the
    // resulting source ids.
    const auto& linked_ids = render_scene.LinkedLayerIds();

    // Skip subtrees the parser tagged as elidable (user-hidden, or no-effect
    // identity passthrough layers) when nothing in the subtree links anything.
    // Most corpora have ~25x more elidable layers than link-referenced ones;
    // the skip set lets the emit walk short-circuit without mutating the tree.
    HashSet<const SceneNode*> emit_skip_subtrees;
    CollectEmitSkipSubtrees(scene.RootMut().as_raw_ptr(), scene, linked_ids, emit_skip_subtrees);

    EmitShadowPasses(extra);

    if (scene.PlanarReflectionEnabled()) {
        EmitPlanarReflectionNode(scene.RootMut().as_raw_ptr(), extra, emit_skip_subtrees);
    }

    EmitSceneNode(
        scene.RootMut().as_raw_ptr(), SpecTex_Default, {}, extra, emit_skip_subtrees, linked_ids);
    FlushDepthDraws(extra);

    // Emit global post-process draws and copies after the scene traversal.
    auto post_processes = scene.PostProcesses();
    for (usize index {}; index < post_processes.len(); ++index) {
        const auto& pp = post_processes[index];
        for (auto& step : pp->steps) {
            if (step.is_Pass()) {
                auto&    sp     = step.as_Pass().value;
                ref<str> target = sp.output.is_empty() ? SpecTex_Default : sp.output.as_str();
                ToGraphPass(sp.node.as_ptr(), target, extra);
            } else {
                auto& cp = step.as_Copy().value;
                AddTextureCopy(extra,
                               MakeTextureDesc(extra, cp.src.as_str()),
                               MakeTextureDesc(extra, cp.dst.as_str()));
            }
        }
    }

    StoreMipFramebufferHistory(extra);

    auto           final_result = SelectTexture(extra, MakeTextureDesc(extra, SpecTex_Default));
    auto           final_use    = Use(extra, final_result);
    fg::ResourceId surface_id { extra.textures.textures().len().to_primitive() + 1,
                                extra.graph.data().scope };
    auto           surface = Remember(extra,
                                      extra.graph.import_resource({ .id      = surface_id,
                                                                    .spec    = surface_id.value,
                                                                    .storage = surface_id.value,
                                                                    .name = "frame/surface"_Str }));
    auto           output_id  = fg::PassId { extra.next_pass++ },
                   release_id = fg::PassId { extra.next_pass++ };
    auto           output     = vrento::vulkan::declare_surface_output(
        extra.graph,
        output_id,
        release_id,
        final_result,
        Remember(extra, ContentRef::bind(extra.graph, surface)));
    if (output.is_err()) return Err(output.unwrap_err());
    extra.descriptions.push(rstd::move(output->transfer));
    extra.descriptions.push(rstd::move(output->release));
    scene.RebuildResourceIndex();
    if (extra.error) return Err(*extra.error);
    resource::ResourcePlan             resource_plan { .generation = extra.textures.generation() };
    auto                               shader_backend = vulkan::MakeShaderBackend();
    vulkan::ShaderReflectionCache      shader_cache(shader_backend.as_ref());
    vulkan::ResourceDeclarationContext declarations(resource_plan, shader_cache);
    struct DrawInput {
        fg::PassId                       id;
        vrento::vulkan::GraphicsDrawDesc description;
    };
    Vec<DrawInput> recipes;
    for (auto& draw : extra.draws) {
        auto recipe = draw.source->declareResources(declarations);
        if (! recipe || ! draw.source->parameters().shader_use ||
            ! draw.source->parameters().shader_use->Valid())
            return Err(fg::Error { fg::ErrorKind::Conflict, draw.id });
        recipes.push({ draw.id, rstd::move(*recipe) });
    }
    auto buffers = extra.textures.declare_buffers(extra.graph, resource_plan.buffers.as_slice());
    if (buffers.is_err()) return Err(buffers.unwrap_err());
    for (const auto& draw : extra.draws) {
        auto uses  = draw.source->resourceUses();
        auto reads = vrento::vulkan::graphics_buffer_reads(
            extra.graph, extra.textures, resource_plan, uses.buffers.as_slice());
        if (reads.is_err()) return Err(reads.unwrap_err());
        auto graphics = draw.source->describePass(extra.graph, extra.textures, draw.id);
        if (graphics.is_err()) return Err(graphics.unwrap_err());
        graphics->buffers = reads.unwrap();
        auto declared =
            vrento::vulkan::resolve_graphics_inputs(extra.graph, draw.id, graphics.unwrap());
        if (declared.is_err()) return Err(declared.unwrap_err());
        for (auto& recipe : recipes)
            if (recipe.id == draw.id) {
                recipe.description.resources = declared.unwrap();
                auto input                   = vrento::vulkan::declare_input(
                    extra.graph, draw.id, rstd::move(recipe.description));
                if (input.is_err()) return Err(input.unwrap_err());
                extra.descriptions.push(input.unwrap());
                break;
            }
    }
    auto graph = rstd::move(extra.graph).finish({}, statistics);
    if (graph.is_err()) return Err(graph.unwrap_err());
    auto plan = extra.textures.resourcePlan(*graph);
    if (plan.is_err()) return Err(plan.unwrap_err());
    resource_plan.textures = rstd::move(plan->textures);
    for (const auto& texture : extra.textures.textures()) {
        const auto& definition = texture.spec.definition;
        rstd_debug("scene texture: scope={} name={} logical={}:{} revision={}",
                   graph->scope(),
                   texture.spec.key,
                   definition.id().index,
                   definition.id().generation,
                   definition.revision());
    }
    vulkan::SceneRenderStateInput input { graph.unwrap(),
                                          rstd::move(extra.textures),
                                          rstd::move(extra.draws),
                                          rstd::move(extra.descriptions),
                                          output_id,
                                          release_id,
                                          output->output,
                                          Some(final_use) };
    input.image_source  = Some(extra.image_source.clone());
    input.resource_data = declarations.takeData();
    input.resource_plan = rstd::move(resource_plan);
    return Ok(rstd::move(input));
}

auto owe::BuildSceneRenderState(Scene& scene) -> Result<vulkan::SceneRenderStateInput, fg::Error> {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    return BuildSceneRenderState(scene, render_scene);
}
