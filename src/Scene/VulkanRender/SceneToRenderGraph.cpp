module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import eigen;
import wescene.vulkan;
import wescene.scene;

import wescene.rgraph;

using namespace owe;
using namespace rstd::literals;
using namespace rstd::prelude;
using rstd::collections::BTreeSet;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;

namespace
{
auto StdString(const String& value) -> std::string {
    return rstd::cppstd::to_string(value.as_str());
}

auto CloneTextureDesc(const rg::TextureDesc& desc) -> rg::TextureDesc {
    return rg::TextureDesc {
        .name = desc.name.clone(),
        .key  = desc.key.clone(),
        .kind = desc.kind,
        .request =
            desc.request.is_some() ? Some(desc.request->clone()) : None<resource::TextureRequest>(),
        .allocation_family = desc.allocation_family.is_some()
                                 ? Some(desc.allocation_family->clone())
                                 : None<String>(),
    };
}
} // namespace

namespace owe::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TextureNodeRef in,
            TextureNodeRef out) {
    builder.read(in);
    builder.write(out);

    auto in_state  = builder.textureState(in);
    auto out_state = builder.textureState(out);
    rstd_assert(in_state.is_some() && out_state.is_some());
    if (in_state.is_none() || out_state.is_none()) return;
    desc.src     = StdString(in_state->desc.key);
    desc.dst     = StdString(out_state->desc.key);
    desc.src_use = Some(in_state->use);
    desc.dst_use = Some(out_state->use);
}
} // namespace owe::rg

struct ExtraInfo;

static rg::TextureDesc MakeTextureDescBase(std::string_view key) {
    auto name = as_str(key).unwrap();
    return rg::TextureDesc {
        .name = String::make(name),
        .key  = String::make(name),
        .kind = IsSpecTex(name) ? rg::TextureKind::Temp : rg::TextureKind::Imported,
    };
}

struct ExtraInfo {
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    Set<std::string>           depth_initialized_outputs {};
    HashSet<String>            transient_texture_families;
    Option<rg::TextureNodeRef> mip_framebuffer_history;
    const RenderSceneSnapshot* render_scene { nullptr };
};

static Option<vulkan::TextureRequest> BuildGraphTextureRequest(ExtraInfo&       extra,
                                                               std::string_view key) {
    if (key.empty()) return None();
    auto name = as_str(key).unwrap();
    if (! IsSpecTex(name)) {
        Option<RenderTextureDescId> texture;
        if (extra.render_scene != nullptr) texture = extra.render_scene->textureDescId(name);
        return Some(vulkan::MakeImportedTextureRequest(key, texture));
    }

    if (extra.render_scene != nullptr) {
        if (auto desc_id = extra.render_scene->renderTargetDescId(name)) {
            if (auto* desc = extra.render_scene->renderTargetDesc(*desc_id)) {
                return Some(vulkan::MakeRenderTargetTextureRequest(key, desc->desc));
            }
        }
    }

    if (extra.scene != nullptr) {
        auto target = extra.scene->RenderTarget(name);
        if (target.is_some()) {
            return Some(vulkan::MakeRenderTargetTextureRequest(key, **target));
        }
    }

    return None();
}

static rg::TextureDesc MakeTextureDesc(ExtraInfo& extra, std::string_view key) {
    auto desc    = MakeTextureDescBase(key);
    desc.request = BuildGraphTextureRequest(extra, key);
    auto name    = as_str(key).unwrap();
    if (extra.transient_texture_families.contains(name)) {
        desc.allocation_family = Some(String::make(name));
    }
    return desc;
}

static std::string_view ResolveEffectTarget(const SceneNodeLayer&    layer,
                                            const SceneEffectTarget& target) {
    if (target.kind == SceneEffectTargetKind::Named && ! target.key.empty()) return target.key;
    return layer.CompositeTarget();
}

static bool LoadsPreviousAttachment(BlendMode mode) {
    return mode == BlendMode::Translucent || mode == BlendMode::Additive ||
           mode == BlendMode::AlphaToCoverage;
}

static void FillCopyTextureRequests(ExtraInfo& extra, vulkan::CopyPass::Desc& desc) {
    desc.src_request = BuildGraphTextureRequest(extra, desc.src);
    desc.dst_request = BuildGraphTextureRequest(extra, desc.dst);
}

static void AddCopyPass(ExtraInfo& extra, rg::TextureDesc in, rg::TextureDesc out) {
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy"_str,
        rg::PassNode::Type::Copy,
        [in = std::move(in), out = std::move(out), &extra](rg::RenderGraphBuilder& builder,
                                                           vulkan::CopyPass::Desc& desc) {
            auto in_node  = builder.createTexture(in);
            auto out_node = builder.createTexture(out, true);
            rg::doCopy(builder, desc, in_node, out_node);
            FillCopyTextureRequests(extra, desc);
        });
}

static rg::TextureNodeRef AddCopyPass(ExtraInfo& extra, rg::TextureNodeRef in,
                                      Option<rg::TextureDesc> out_desc = None()) {
    rg::TextureNodeRef copy {};
    extra.rgraph->addPass<vulkan::CopyPass>(
        "copy"_str,
        rg::PassNode::Type::Copy,
        [&copy, in, out_desc = std::move(out_desc), &extra](rg::RenderGraphBuilder& builder,
                                                            vulkan::CopyPass::Desc& pdesc) {
            auto state = builder.textureState(in);
            rstd_assert(state.is_some());
            if (state.is_none()) return;
            auto desc =
                out_desc.is_some() ? CloneTextureDesc(*out_desc) : CloneTextureDesc(state->desc);
            if (out_desc.is_none()) {
                auto suffix = rstd::format("_{}_copy", state->version);
                desc.key.push_str(suffix.as_str());
                desc.name.push_str(suffix.as_str());
            }
            copy = builder.createTexture(desc, true);
            rg::doCopy(builder, pdesc, in, copy);
            FillCopyTextureRequests(extra, pdesc);
            pdesc.dst_matches_src = out_desc.is_none();
            if (pdesc.dst_matches_src && pdesc.src_request.is_some()) {
                pdesc.dst_request       = Some(pdesc.src_request->clone());
                pdesc.dst_request->name = desc.key.clone();
            }
        });
    return copy;
}

static rg::TextureNodeRef AddMipFramebufferHistory(ExtraInfo&              extra,
                                                   rg::RenderGraphBuilder& builder) {
    if (extra.mip_framebuffer_history.is_some()) {
        return *extra.mip_framebuffer_history;
    }

    auto history_desc = MakeTextureDesc(extra, as_string_view(WE_MIP_MAPPED_FRAME_BUFFER));
    history_desc.kind = rg::TextureKind::Temp;
    auto history      = builder.createTexture(history_desc);
    builder.markVirtualWrite(history);
    extra.mip_framebuffer_history = Some<rg::TextureNodeRef>(history);
    return history;
}

static void StoreMipFramebufferHistory(ExtraInfo& extra) {
    if (extra.mip_framebuffer_history.is_none()) return;

    auto history_desc = MakeTextureDesc(extra, as_string_view(WE_MIP_MAPPED_FRAME_BUFFER));
    history_desc.kind = rg::TextureKind::Temp;
    AddCopyPass(
        extra, MakeTextureDesc(extra, as_string_view(SpecTex_Default)), rstd::move(history_desc));
}

static void AddMaterialTextureReads(SceneMaterial& material, std::string_view pass_output,
                                    ExtraInfo& extra, rg::RenderGraphBuilder& builder,
                                    vulkan::CustomShaderPass::Desc& pdesc,
                                    bool                            reuses_previous_output) {
    auto snapshots = HashMap<String, rg::TextureNodeRef>::make();
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        rstd_assert(index < material.texture_sources.len().to_primitive());
        if (index >= material.texture_sources.len().to_primitive()) {
            pdesc.texture_bindings.emplace_back();
            continue;
        }
        const auto&                source = material.texture_sources[usize(index)];
        Option<rg::TextureNodeRef> input;
        std::string                binding_key;
        if (source.kind == SceneMaterialTextureSourceKind::Empty) {
            pdesc.texture_bindings.emplace_back();
            continue;
        }
        if (source.kind == SceneMaterialTextureSourceKind::UnsupportedSpecial) {
            rstd_error("material '{}' references unsupported scene texture '{}'",
                       material.name,
                       source.key);
            pdesc.texture_bindings.emplace_back();
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
                pdesc.texture_bindings.emplace_back();
                continue;
            }
            binding_key = StdString(link->render_target_key);
            input       = Some(builder.createTexture(MakeTextureDesc(extra, binding_key)));
            builder.markVirtualWrite(*input);
        } else {
            binding_key = StdString(source.key);
            auto name   = as_str(binding_key).unwrap();
            if (source.kind == SceneMaterialTextureSourceKind::MipMappedFramebuffer) {
                input = Some(AddMipFramebufferHistory(extra, builder));
            } else {
                input = Some(builder.createTexture(MakeTextureDesc(extra, binding_key)));
            }
            if (IsSpecTex(name) && ! name.starts_with(WE_MIP_MAPPED_FRAME_BUFFER)) {
                builder.markVirtualWrite(*input);
            }
        }

        if (binding_key == pass_output && reuses_previous_output) {
            auto key      = as_str(binding_key).unwrap();
            auto snapshot = snapshots.get(key);
            if (snapshot.is_some()) {
                input = Some<rg::TextureNodeRef>(rg::TextureNodeRef {
                    .handle = (**snapshot).handle,
                });
            } else {
                builder.markSelfWrite(*input);
                input = Some(AddCopyPass(extra, *input));
                (void)snapshots.insert(String::make(key), *input);
            }
        }
        builder.read(*input);
        auto sampled_state = builder.textureState(*input);
        rstd_assert(sampled_state.is_some());
        if (sampled_state.is_none()) {
            pdesc.texture_bindings.emplace_back();
            continue;
        }
        auto sampled_key = StdString(sampled_state->desc.key);
        pdesc.texture_bindings.emplace_back(vulkan::TextureBindingRequest {
            .name    = String::make(as_str(sampled_key).unwrap()),
            .use     = Some(sampled_state->use),
            .request = BuildGraphTextureRequest(extra, sampled_key),
        });
    }
}

static SceneNodeLayer* ToGraphPass(SceneNode* node, std::string_view output, ExtraInfo& extra,
                                   bool                defer_effect = false,
                                   SceneRenderViewKind render_view  = SceneRenderViewKind::Primary);

static void LoadGraphEffects(SceneNodeLayer* effs, ExtraInfo& extra) {
    for (auto* eff : effs->ResolvedEffects()) {
        if (eff == nullptr) continue;
        auto cmdItor = eff->commands.begin();
        auto cmdEnd  = eff->commands.end();
        int  nodePos = 0;
        for (auto& n : eff->nodes) {
            if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos.to_primitive()) {
                auto source = ResolveEffectTarget(*effs, cmdItor->src);
                auto target = ResolveEffectTarget(*effs, cmdItor->dst);
                AddCopyPass(extra, MakeTextureDesc(extra, source), MakeTextureDesc(extra, target));
                cmdItor++;
            }
            auto target = effs->ResolvedTarget(n);
            ToGraphPass(n.sceneNode.as_ptr(), ResolveEffectTarget(*effs, target), extra);
            nodePos++;
        }
    }
}

static SceneNodeLayer* ToGraphPass(SceneNode* node, std::string_view output, ExtraInfo& extra,
                                   bool defer_effect, SceneRenderViewKind render_view) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    if (node->Mesh() == nullptr) return nullptr;
    auto* mesh = node->Mesh();
    if (mesh->Submeshes().empty()) return nullptr;
    const auto& slots = mesh->MaterialSlots();

    SceneNodeLayer* imgeff = nullptr;
    if (node->HasLayer()) {
        auto*      effect       = node->Layer().get();
        const bool intermediate = effect->RequiresIntermediateTarget();
        effect->ConfigureSourceDraw(intermediate);
        if (intermediate) {
            imgeff = effect;
            imgeff->ResolveEffect(*scene.DefaultEffectMesh(), "effect");
            output = imgeff->CompositeTarget();
            (void)extra.transient_texture_families.insert(String::make(as_str(output).unwrap()));
        }
    }
    if (imgeff != nullptr) {
        for (auto& prefill : imgeff->PrefillNodes()) {
            auto prefill_output = ResolveEffectTarget(*imgeff, prefill.output);
            ToGraphPass(prefill.sceneNode.as_ptr(), prefill_output, extra);
        }
    }

    const bool draw_source = imgeff == nullptr || imgeff->RequiresSourceDraw();
    for (std::size_t smi = 0; draw_source && smi < mesh->Submeshes().size(); smi++) {
        const auto& submesh       = mesh->Submeshes()[smi];
        const auto  material_slot = submesh.material_slot.to_primitive();
        if (material_slot >= slots.size() || ! slots[material_slot]) continue;
        SceneMaterial* material = slots[material_slot].get();
        scene.ResolveMaterialTextureSources(*material);
        std::shared_ptr<SceneMaterial> material_override;
        if (imgeff != nullptr && submesh.output_override.empty() && ! submesh.preserve_output) {
            auto source_blend = imgeff->IntermediateSourceBlend();
            if (source_blend.is_some()) {
                material_override           = std::make_shared<SceneMaterial>(*material);
                material_override->blenmode = *source_blend;
            }
        }
        std::string passName = material->name;
        // Per-submesh output override (clipping-mask submeshes write into a
        // shared RT that the main puppet pass samples via g_Texture8).
        std::string_view pass_output =
            submesh.output_override.empty() ? output : std::string_view(submesh.output_override);

        rgraph.addPass<vulkan::CustomShaderPass>(
            rstd::cppstd::as_str(passName).unwrap(),
            rg::PassNode::Type::CustomShader,
            [material,
             node,
             smi,
             pass_output,
             material_override,
             preserve_output = submesh.preserve_output,
             render_view,
             &scene,
             &extra](rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
                const auto& pass        = builder.workPassNode();
                pdesc.node              = Some(rstd::mut_ref<SceneNode>::from_raw_parts(node));
                pdesc.submesh_index     = u32(static_cast<rstd::uint32_t>(smi));
                pdesc.graph_pass_index  = pass.pass.index;
                pdesc.render_view       = render_view;
                pdesc.material_override = material_override;
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
                std::string pass_output_s(pass_output);
                auto        output_rt = scene.RenderTarget(as_str(pass_output_s).unwrap());
                rstd_assert(output_rt.is_some());
                if (output_rt.is_none()) return;
                const auto& output_target = **output_rt;
                const auto* pass_material = material_override ? material_override.get() : material;
                const bool  reuses_previous_output =
                    ! (output_target.force_clear && ! preserve_output) &&
                    (output_target.preserve_on_write || preserve_output ||
                     LoadsPreviousAttachment(pass_material->blenmode));

                pdesc.output = pass_output_s;
                AddMaterialTextureReads(
                    *material, pass_output, extra, builder, pdesc, reuses_previous_output);

                auto output_node =
                    builder.createTexture(MakeTextureDesc(extra, pass_output_s), true);
                auto output_state = builder.textureState(output_node);
                rstd_assert(output_state.is_some());
                if (output_state.is_none()) return;
                const bool first_output_write = output_state->version == usize();
                if (! first_output_write && reuses_previous_output) {
                    builder.reusePreviousAllocation(output_node);
                }
                pdesc.output_use     = Some(output_state->use);
                pdesc.output_request = BuildGraphTextureRequest(extra, pass_output_s);
                pdesc.samples        = vulkan::TextureSampleCount(output_target.sample_count);
                if (pdesc.samples != VK_SAMPLE_COUNT_1_BIT) {
                    auto twin_name            = vulkan::MsaaTwinName(pass_output_s, pdesc.samples);
                    pdesc.output_msaa_request = Some(
                        vulkan::MakeMsaaTextureRequest(twin_name, output_target, pdesc.samples));
                    auto msaa_node = builder.createTexture(
                        rg::TextureDesc {
                            .name    = String::make(rstd::cppstd::as_str(twin_name).unwrap()),
                            .key     = String::make(rstd::cppstd::as_str(twin_name).unwrap()),
                            .kind    = rg::TextureKind::Temp,
                            .request = Some(pdesc.output_msaa_request->clone()),
                        },
                        true);
                    auto msaa_state = builder.textureState(msaa_node);
                    if (msaa_state) pdesc.output_msaa_use = Some(msaa_state->use);
                }
                pdesc.transparent_clear = first_output_write && output_target.clear_on_first_write;
                pdesc.clear_output =
                    ! preserve_output &&
                    ((first_output_write && output_target.bind.screen) || pdesc.transparent_clear);
                pdesc.preserve_output = output_state->version > usize() &&
                                        (output_target.preserve_on_write || preserve_output);
                const bool uses_depth =
                    output_target.withDepth && vulkan::UsesDepthAttachment(*pass_material);
                pdesc.has_depth_attachment = uses_depth;
                if (uses_depth) {
                    auto depth_name = pass_output_s + "::depth";
                    pdesc.depth_request =
                        Some(vulkan::MakeDepthTextureRequest(depth_name, output_target));
                    auto depth_node = builder.createTexture(
                        rg::TextureDesc {
                            .name    = String::make(rstd::cppstd::as_str(depth_name).unwrap()),
                            .key     = String::make(rstd::cppstd::as_str(depth_name).unwrap()),
                            .kind    = rg::TextureKind::Temp,
                            .request = Some(pdesc.depth_request->clone()),
                        },
                        true);
                    auto depth_state = builder.textureState(depth_node);
                    if (depth_state) pdesc.depth_use = Some(depth_state->use);
                }
                pdesc.clear_depth =
                    uses_depth && (pdesc.clear_output || output_target.force_clear ||
                                   extra.depth_initialized_outputs.count(pass_output_s) == 0);
                if (uses_depth) {
                    extra.depth_initialized_outputs.insert(pass_output_s);
                } else if (pdesc.clear_output || output_target.force_clear) {
                    extra.depth_initialized_outputs.erase(pass_output_s);
                }
                builder.write(output_node);
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
                                    Set<const SceneNode*>& out_skip,
                                    bool                   visibility_hidden_ancestor = false) {
    const auto wallpaper   = node->WallpaperIdentity();
    const auto link_source = scene.ResolveLayerLinkSource(*node);
    const i32 layer_id = link_source.is_some() ? link_source->value
                                               : (wallpaper.is_some() ? wallpaper->value : i32(-1));
    const bool linked  = link_source.is_some() && linked_ids.contains(link_source->value);
    const bool visibility_hidden_self =
        (! node->Visible() || (layer_id >= i32() && scene.IsLayerVisibilityElidable(
                                                        WallpaperLayerId { .value = layer_id }))) &&
        ! linked;
    const bool visibility_hidden = visibility_hidden_ancestor || visibility_hidden_self;

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
        out_skip.insert(node);
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

static void ConfigureNestedOutput(SceneNode* node, std::string_view output,
                                  std::string_view inherited_camera) {
    if (! inherited_camera.empty() && node->Camera().empty()) {
        node->SetCamera(std::string(inherited_camera));
    }
    if (! node->HasLayer()) return;
    auto& effect_layer = node->Layer();
    if (! effect_layer) return;
    auto output_text = as_str(output).unwrap();
    if (output_text != SpecTex_Default &&
        as_str(effect_layer->FinalTarget()).unwrap() == SpecTex_Default) {
        effect_layer->SetFinalTarget(std::string(output));
    }
    if (effect_layer->FinalTarget() == output) {
        effect_layer->SetFinalCamera(std::string(inherited_camera));
    }
}

static void EmitSceneNode(SceneNode* node, std::string_view inherited_output,
                          std::string_view inherited_camera, ExtraInfo& extra,
                          const Set<const SceneNode*>& emit_skip_subtrees,
                          const BTreeSet<i32>&         linked_ids) {
    if (node == nullptr || emit_skip_subtrees.count(node) != 0) return;

    auto&      scene       = *extra.scene;
    const auto wallpaper   = node->WallpaperIdentity();
    const auto link_source = scene.ResolveLayerLinkSource(*node);
    const i32 layer_id = link_source.is_some() ? link_source->value
                                               : (wallpaper.is_some() ? wallpaper->value : i32(-1));
    const bool       elidable = scene.IsLayerElidable(WallpaperLayerId { .value = layer_id });
    const bool       linked   = link_source.is_some() && linked_ids.contains(link_source->value);
    bool             emit     = true;
    std::string      link_output;
    std::string_view node_output = inherited_output;

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
                link_output = rstd::cppstd::to_string(source_record->render_target_key);
                node_output = link_output;
                if (node->HasLayer() && ! node->Layer()->PublishesOutput()) {
                    node->Layer()->SetFinalTarget(link_output);
                    node->Layer()->SetFinalLocal(true);
                }
            }
        }
    }

    auto group_camera =
        wallpaper.is_some() ? scene.RenderGroupCamera(*wallpaper) : None<ref<str>>();
    if (emit && group_camera) {
        ConfigureNestedOutput(node, node_output, inherited_camera);
        auto* effect_layer = ToGraphPass(node, node_output, extra, true);
        if (effect_layer == nullptr) {
            rstd_error("render group layer {} has no effect target", wallpaper->value);
        }
        const std::string_view child_output =
            effect_layer == nullptr ? node_output
                                    : std::string_view(effect_layer->CompositeTarget());
        for (auto& child : node->GetChildren()) {
            EmitSceneNode(child.as_ptr(),
                          child_output,
                          rstd::cppstd::as_string_view(*group_camera),
                          extra,
                          emit_skip_subtrees,
                          linked_ids);
        }
        if (effect_layer != nullptr && effect_layer->HasRenderEffects()) {
            LoadGraphEffects(effect_layer, extra);
        }
        return;
    }

    if (emit) {
        ConfigureNestedOutput(node, node_output, inherited_camera);
        ToGraphPass(node, node_output, extra);
    }
    for (auto& child : node->GetChildren()) {
        EmitSceneNode(child.as_ptr(),
                      inherited_output,
                      inherited_camera,
                      extra,
                      emit_skip_subtrees,
                      linked_ids);
    }
}

static bool SamplesPlanarReflection(SceneNode& node) {
    auto* mesh = node.Mesh();
    if (mesh == nullptr) return false;
    for (const auto& material : mesh->MaterialSlots()) {
        if (! material) continue;
        for (const auto& texture : material->textures) {
            if (as_str(texture).unwrap().starts_with(WE_REFLECTION_PREFIX)) return true;
        }
    }
    return false;
}

static void EmitPlanarReflectionNode(SceneNode* node, ExtraInfo& extra,
                                     const Set<const SceneNode*>& emit_skip_subtrees) {
    if (node == nullptr || emit_skip_subtrees.count(node) != 0) return;
    if (node->Reflected() && ! SamplesPlanarReflection(*node)) {
        ToGraphPass(node,
                    as_string_view(WE_REFLECTION_PREFIX),
                    extra,
                    true,
                    SceneRenderViewKind::Reflection);
    }
    for (auto& child : node->GetChildren()) {
        EmitPlanarReflectionNode(child.as_ptr(), extra, emit_skip_subtrees);
    }
}

static void EmitShadowPasses(ExtraInfo& extra) {
    if (extra.render_scene == nullptr) return;
    auto definitions = extra.render_scene->ShadowDefinitions();
    if (definitions.is_empty()) return;
    const auto& definition = definitions[usize()];
    const auto  target     = StdString(definition.target);
    bool        first      = true;

    for (const auto& caster : extra.render_scene->ShadowCasters()) {
        const auto* item = extra.render_scene->renderItem(caster.render_item);
        if (item == nullptr || ! caster.material) continue;
        auto draw = extra.scene->ResourceIndex().resolve(item->scene_draw_item);
        if (draw.is_none() || draw->node == nullptr || draw->mesh == nullptr) continue;
        extra.scene->ResolveMaterialTextureSources(*caster.material);

        extra.rgraph->addPass<vulkan::CustomShaderPass>(
            caster.material->name.empty() ? "shadow"_str : as_str(caster.material->name).unwrap(),
            rg::PassNode::Type::CustomShader,
            [&,
             item,
             draw,
             material       = caster.material,
             instance_count = caster.instance_count,
             clear_depth    = first,
             target](rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
                const auto& pass        = builder.workPassNode();
                pdesc.node              = Some(mut_ref<SceneNode>::from_raw_parts(draw->node));
                pdesc.draw_item         = item->scene_draw_item;
                pdesc.render_item       = item->id;
                pdesc.submesh_index     = item->submesh_index;
                pdesc.graph_pass_index  = pass.pass.index;
                pdesc.material_override = material;
                pdesc.depth_only        = true;
                pdesc.instance_count    = instance_count;
                pdesc.output            = target;
                pdesc.clear_depth       = clear_depth;

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

                AddMaterialTextureReads(*material, target, extra, builder, pdesc, true);
                auto atlas = builder.createTexture(MakeTextureDesc(extra, target), true);
                auto state = builder.textureState(atlas);
                if (state.is_none()) return;
                pdesc.depth_use     = Some(state->use);
                pdesc.depth_request = BuildGraphTextureRequest(extra, target);
                builder.write(atlas);
            });
        first = false;
    }
}

Box<rg::RenderGraph> owe::sceneToRenderGraph(Scene&                     scene,
                                             const RenderSceneSnapshot& render_scene) {
    auto      rgraph = Box<rg::RenderGraph>::make();
    ExtraInfo extra { .rgraph = rgraph.get(), .scene = &scene, .render_scene = &render_scene };

    // The snapshot owns link-consumer discovery; graph build only consumes the
    // resulting source ids.
    const auto& linked_ids = render_scene.LinkedLayerIds();

    // Skip subtrees the parser tagged as elidable (user-hidden, or no-effect
    // identity passthrough layers) when nothing in the subtree links anything.
    // Most corpora have ~25x more elidable layers than link-referenced ones;
    // the skip set lets the emit walk short-circuit without mutating the tree.
    Set<const SceneNode*> emit_skip_subtrees;
    CollectEmitSkipSubtrees(scene.RootMut().as_raw_ptr(), scene, linked_ids, emit_skip_subtrees);

    EmitShadowPasses(extra);

    if (scene.PlanarReflectionEnabled()) {
        EmitPlanarReflectionNode(scene.RootMut().as_raw_ptr(), extra, emit_skip_subtrees);
    }

    EmitSceneNode(scene.RootMut().as_raw_ptr(),
                  as_string_view(SpecTex_Default),
                  {},
                  extra,
                  emit_skip_subtrees,
                  linked_ids);

    // Emit global post-process passes after the main scene-graph traversal.
    // Each step is either a CustomShaderPass (built on the synthetic node's
    // mesh+material) or a CopyPass (RT-to-RT blit).
    auto post_processes = scene.PostProcesses();
    for (usize index {}; index < post_processes.len(); ++index) {
        const auto& pp = post_processes[index];
        for (auto& step : pp->steps) {
            if (step.is_Pass()) {
                auto&            sp     = step.as_Pass().value;
                std::string_view target = sp.output.empty() ? as_string_view(SpecTex_Default)
                                                            : std::string_view(sp.output);
                ToGraphPass(sp.node.as_ptr(), target, extra);
            } else {
                auto& cp = step.as_Copy().value;
                AddCopyPass(extra, MakeTextureDesc(extra, cp.src), MakeTextureDesc(extra, cp.dst));
            }
        }
    }

    StoreMipFramebufferHistory(extra);

    scene.RebuildResourceIndex();
    return rgraph;
}

Box<rg::RenderGraph> owe::sceneToRenderGraph(Scene& scene) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    return sceneToRenderGraph(scene, render_scene);
}
