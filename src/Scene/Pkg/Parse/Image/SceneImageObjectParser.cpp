module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.pkg.spec_names;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using rstd::collections::BTreeSet;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

struct ImageParseGeometry {
    bool                   requires_source_draw { true };
    Option<ref<SceneMesh>> final_mesh { None() };
};

// TODO: Confirm WE's exact semantics for zero-height audio-buffer layers.
i32 NonZeroRenderTargetDimension(float value) {
    if (! f32(value).is_finite() || value < 1.0f) return i32(1);
    return rstd::as_cast<i32>(value);
}

array<i32, 2> NonZeroRenderTargetExtent(float width, float height) {
    return { NonZeroRenderTargetDimension(width), NonZeroRenderTargetDimension(height) };
}

array<float, 2> ImageEffectTargetSize(const SceneParseContext&    context,
                                      const wpscene::ImageObject& obj) {
    auto camera = context.scene->ActiveCamera();
    if (obj.fullscreen && camera.is_some()) {
        return { static_cast<float>((**camera).Width()), static_cast<float>((**camera).Height()) };
    }
    return { obj.size[usize(0)], obj.size[usize(1)] };
}

void ParseImageObjImpl(SceneParseContext& context, wpscene::ImageObject& img_obj,
                       ImageParseGeometry parse_geometry = {}) {
    auto& wpimgobj = img_obj;
    // Invisible image layers are kept in the scene tree because their composite
    // may be sampled by other layers via `_rt_imageLayerComposite_<id>`. The
    // render-graph builder decides whether to actually emit passes for them.
    if (! wpimgobj.visible) {
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = wpimgobj.id });
    }

    auto& vfs = *context.vfs;

    bool       isPassthrough      = wpimgobj.config.passthrough;
    const bool alpha_can_change   = ! wpimgobj.alpha_user_key.is_empty() ||
                                    wpimgobj.field_bindings.HasAnimation("alpha"_str) ||
                                    wpimgobj.field_bindings.HasScript("alpha"_str);
    const auto geometry_size      = wpimgobj.size;
    const auto effect_target_size = ImageEffectTargetSize(context, wpimgobj);

    Option<Box<Mdl>>      puppet;
    bool                  has_bones = false;
    bool                  has_mesh  = false;
    const Mdl::Mesh*      primary_puppet_mesh { nullptr };
    Vec<const Mdl::Mesh*> supplemental_puppet_meshes;
    if (! wpimgobj.puppet.is_empty()) {
        auto parsed_puppet = Box<Mdl>::make();
        if (! MdlParser::Parse(wpimgobj.puppet.as_str(), vfs, *parsed_puppet)) {
            rstd_error("parse puppet failed: {}", wpimgobj.puppet);
        } else {
            has_bones =
                parsed_puppet->puppet.is_some() && ! (*parsed_puppet->puppet)->bones.is_empty();
            if (! wpimgobj.material_path.is_empty()) {
                auto primary_index =
                    MdlParser::FindMeshByMaterial(*parsed_puppet, wpimgobj.material_path.as_str());
                if (primary_index.is_some() &&
                    ! parsed_puppet->meshes[*primary_index].positions.is_empty())
                    primary_puppet_mesh = &parsed_puppet->meshes[*primary_index];
            }
            if (primary_puppet_mesh == nullptr) {
                for (const auto& candidate : parsed_puppet->meshes) {
                    if (candidate.positions.is_empty()) continue;
                    primary_puppet_mesh = &candidate;
                    break;
                }
            }
            for (const auto& candidate : parsed_puppet->meshes) {
                if (candidate.positions.is_empty() || &candidate == primary_puppet_mesh) continue;
                supplemental_puppet_meshes.push(&candidate);
            }
            has_mesh = primary_puppet_mesh != nullptr;
            if (! has_bones && ! has_mesh) {
                rstd_error("puppet has no mesh data: {}", wpimgobj.puppet);
            } else {
                puppet = Some(rstd::move(parsed_puppet));
            }
        }
    }

    const bool has_author_effect = ! wpimgobj.effects.is_empty();
    // A solid layer's flat material only produces its source color; a final compositor owns
    // BLENDMODE and the previous-framebuffer input.
    const bool layer_material_is_final =
        (! has_author_effect || has_bones) && ! wpimgobj.solid_layer;
    const bool color_blend_uses_layer_material =
        wpimgobj.colorBlendMode != i32() && layer_material_is_final;
    const bool append_color_blend_final_effect =
        wpimgobj.colorBlendMode != i32() && ! color_blend_uses_layer_material;
    Option<BlendMode> color_blend_attachment_override;
    if (append_color_blend_final_effect) {
        wpscene::ImageEffect colorEffect;
        wpscene::Material    colorMat;
        auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json"_str);
        if (! json) {
            return;
        }
        colorMat.FromJson(*json);
        (void)colorMat.combos.insert(rstd::into(WE_CB_BONECOUNT), i32(1));
        color_blend_attachment_override = ApplyLayerColorBlend(colorMat, wpimgobj.colorBlendMode);
        colorEffect.materials.push(rstd::move(colorMat));
        wpimgobj.effects.push(rstd::move(colorEffect));
    }
    const bool is_linked_source = context.linked_source_ids.contains(wpimgobj.id);
    if (! has_author_effect && wpimgobj.composite_layer && ! is_linked_source) {
        AppendLayerCompositePassthroughEffect(vfs, wpimgobj);
    }

    bool hasEffect = ! wpimgobj.effects.is_empty() || is_linked_source;

    // No-effect fullscreen / compose layers contribute nothing on their own
    // (they just sample `_rt_default` and write it back). Mark as elidable
    // so the render-graph builder drops them when unreferenced, or routes
    // them to `_rt_link_<id>` when another layer reads their composite.
    if (! hasEffect && wpimgobj.visible && (wpimgobj.fullscreen || isPassthrough)) {
        context.scene->MarkLayerStaticElidable(WallpaperLayerId { .value = wpimgobj.id });
    }
    if (! hasEffect && wpimgobj.visible && wpimgobj.alpha <= 0.0f && ! alpha_can_change) {
        context.scene->MarkLayerStaticElidable(WallpaperLayerId { .value = wpimgobj.id });
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto           spImgNode = Arc<SceneNode>::make(Vector3f(wpimgobj.origin.data()),
                                                    Vector3f(wpimgobj.scale.data()),
                                                    Vector3f(wpimgobj.angles.data()),
                                                    wpimgobj.name.as_str());
    const Vector3f alignment_offset =
        wpimgobj.fullscreen ? Vector3f::Zero()
                            : AlignmentOffset(wpimgobj.alignment.as_str(),
                                              { geometry_size[usize(0)], geometry_size[usize(1)] });
    const bool solid_composite_context = HasSolidCompositeContext(context, wpimgobj);
    spImgNode->SetSize({ geometry_size[usize(0)], geometry_size[usize(1)] });
    spImgNode->SetPerspective(wpimgobj.perspective);
    spImgNode->SetReflected(wpimgobj.reflected);
    spImgNode->SetBaseColor(Vector3f(wpimgobj.color.data()), wpimgobj.alpha);
    spImgNode->ID()          = i32(wpimgobj.id);
    const auto image_node_id = context.scene->RegisterNode(
        *spImgNode,
        wpimgobj.id >= i32() ? Some(WallpaperLayerId { .value = wpimgobj.id })
                             : None<WallpaperLayerId>());
    if (! wpimgobj.visible_user.empty())
        spImgNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wpimgobj.visible_user));
    Vec<Arc<SceneMaterial>> image_property_materials;
    auto                    track_image_property_material = [&](const Arc<SceneMaterial>& mat) {
        if ((wpimgobj.color_user_key.is_empty() && wpimgobj.alpha_user_key.is_empty()) || ! mat)
            return;
        image_property_materials.push(mat.clone());
    };
    Option<Arc<PuppetLayer>> image_puppet_layer;
    if (puppet.is_some() && has_bones) {
        image_puppet_layer =
            Some(MakePuppetLayer((*(*puppet)->puppet).clone(), wpimgobj.puppet_layers.as_slice()));
        RegisterPuppetLayer(context, spImgNode.as_ptr(), (*image_puppet_layer).clone());
    }

    // Puppet clipping masks: register the half-res shared RT here; per-mask
    // submeshes (pre-pass + clipped main) are emitted below after the base
    // material/mesh are built. Main material stays unmodified — only the
    // clipped-main submesh gets a CLIPPINGTARGET combo + g_Texture8 binding.
    constexpr ref<str> PUPPET_MASK_RT   = "_rt_puppet_mask"_str;
    bool               puppet_has_masks = false;
    if (primary_puppet_mesh != nullptr) puppet_has_masks = ! primary_puppet_mesh->masks.is_empty();
    if (puppet_has_masks && has_bones && context.scene->RenderTarget(PUPPET_MASK_RT).is_none()) {
        SceneRenderTarget rt {};
        rt.width       = i32(2);
        rt.height      = i32(2);
        rt.allowReuse  = true;
        rt.force_clear = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = 0.5f;
        context.scene->RegisterRenderTarget(String::make(PUPPET_MASK_RT), rstd::move(rt));
    }

    SceneMaterial          material;
    UniformNodeConfigDraft svData;

    ShaderValueMap    baseConstSvs = context.global_base_uniforms.clone();
    ShaderInfo        shaderInfo;
    wpscene::Material image_wpmat = wpimgobj.material.clone();
    (void)image_wpmat.combos.insert(rstd::into(WE_CB_SCENE_ORTHO),
                                    i32(wpimgobj.perspective ? 0 : 1));
    (void)image_wpmat.combos.insert(rstd::into(OWE_CB_IMAGE_LAYER), i32(1));
    wpscene::Material image_user_texture_fallback = image_wpmat.clone();
    if (color_blend_uses_layer_material && ! hasEffect)
        ApplyLayerColorBlend(image_wpmat, wpimgobj.colorBlendMode);
    ApplyUserTextureBindings(context, image_wpmat);
    {
        svData.SetParallaxContract(wpimgobj.parallax, wpimgobj.id, ! wpimgobj.disablepropagation);
        if (! hasEffect && puppet.is_some() && has_bones) {
            MdlParser::AddPuppetShaderInfo(shaderInfo, **puppet);
        }

        (void)baseConstSvs.insert(rstd::into(G_COLOR4),
                                  ShaderValue(array<float, 4> { wpimgobj.color[usize(0)],
                                                                wpimgobj.color[usize(1)],
                                                                wpimgobj.color[usize(2)],
                                                                wpimgobj.alpha }));
        (void)baseConstSvs.insert(rstd::into(G_COLOR),
                                  ShaderValue(array<float, 3> { wpimgobj.color[usize(0)],
                                                                wpimgobj.color[usize(1)],
                                                                wpimgobj.color[usize(2)] }));
        (void)baseConstSvs.insert(rstd::into(G_ALPHA), ShaderValue(wpimgobj.alpha));
        (void)baseConstSvs.insert(rstd::into(G_USERALPHA), ShaderValue(wpimgobj.alpha));
        (void)baseConstSvs.insert(rstd::into(G_BRIGHTNESS), ShaderValue(wpimgobj.brightness));

        shaderInfo.baseConstSvs = baseConstSvs.clone();

        auto material_result = BuildMaterial(vfs,
                                             *context.shader_cache,
                                             context.shader_environment,
                                             image_wpmat,
                                             *context.scene,
                                             rstd::move(shaderInfo));
        if (material_result.is_err()) {
            rstd_error("load imageobj '{}' material faild", wpimgobj.name);
            return;
        }
        auto material_build = rstd::move(material_result).unwrap_unchecked();
        material            = rstd::move(material_build.material);
        shaderInfo          = rstd::move(material_build.shader_info);
        LoadConstvalue(context, material, image_wpmat, shaderInfo);
    }

    // Whether the layer's base texture is point-sampled (noInterpolation).
    // Captured here because `material` is moved into the mesh below, well
    // before the effect ping-pong RTs are created.
    bool point_source = false;
    if (! material.textures.is_empty()) {
        auto texture = context.scene->Texture(material.textures[usize()].as_str());
        point_source = texture.is_some() && (**texture).sample.magFilter == TextureFilter::NEAREST;
    }

    // mesh
    SceneMesh             effct_final_mesh {};
    auto                  spMesh  = Arc<SceneMesh>::make();
    auto&                 mesh    = *spMesh;
    const array<float, 2> mapRate = Texture0UvScale(material, wpimgobj.nopadding);
    const bool            source_uses_framebuffer_space = hasEffect && wpimgobj.composite_layer;
    const Vector3f        source_alignment_offset = hasEffect ? Vector3f::Zero() : alignment_offset;
    auto                  add_puppet_mask_submeshes = [&](SceneMesh& target, u32 first_mask_slot) {
        if (! puppet_has_masks || primary_puppet_mesh == nullptr) return;
        BTreeSet<u32> clipped_indices;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(u32(idx));
            }
        }
        if (! clipped_indices.is_empty()) {
            size_t smi = 0;
            for (const auto& pmesh : (*puppet)->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
                if (pmesh.positions.is_empty()) continue;
                if (smi >= target.Submeshes().len().to_primitive()) break;
                Vec<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.contains(rstd::as_cast<u32>(i))) continue;
                    kept.push({ u32(p.start), u32(p.size) });
                }
                target.Submeshes()[usize(smi)].draw_ranges = rstd::move(kept);
                ++smi;
            }
        }

        u32 slot = first_mask_slot;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                target.Submeshes().emplace_back();
                auto& pre_sm = target.Submeshes().last_mut().unwrap().get_mut();
                MdlParser::GenMaskSubmeshFromMdl(
                    pre_sm, pmesh, mb.part_ids_b.as_slice(), { 1.0f, 1.0f });
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = rstd::into(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().last_mut().unwrap().get_mut();
                MdlParser::GenMaskSubmeshFromMdl(
                    clip_sm, pmesh, mb.part_ids_a.as_slice(), { 1.0f, 1.0f });
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet.is_some()) {
        if (hasEffect) {
            effct_final_mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            GenCardMesh(mesh,
                        { geometry_size[usize(0)], geometry_size[usize(1)] },
                        mapRate,
                        source_alignment_offset);
            if (primary_puppet_mesh != nullptr) {
                effct_final_mesh.Submeshes().emplace_back();
                MdlParser::GenMeshFromMdl(
                    effct_final_mesh.Submeshes().last_mut().unwrap().get_mut(),
                    *primary_puppet_mesh,
                    { 1.0f, 1.0f });
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, u32(1));

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat = image_wpmat.clone();
                puppet_mat.textures[usize()].clear();
                MdlParser::AddPuppetMatInfo(puppet_mat, **puppet);
                if (color_blend_uses_layer_material)
                    color_blend_attachment_override =
                        ApplyLayerColorBlend(puppet_mat, wpimgobj.colorBlendMode);
                puppet_effect.materials.push(rstd::move(puppet_mat));
                wpimgobj.effects.push(rstd::move(puppet_effect));
            }
        } else {
            mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            if (primary_puppet_mesh != nullptr) {
                mesh.Submeshes().emplace_back();
                MdlParser::GenMeshFromMdl(mesh.Submeshes().last_mut().unwrap().get_mut(),
                                          *primary_puppet_mesh,
                                          { mapRate[usize()], mapRate[usize(1)] });
            }
        }
    }
    if (puppet.is_none()) {
        if (source_uses_framebuffer_space) {
            mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
        }
        GenCardMesh(mesh,
                    { geometry_size[usize(0)], geometry_size[usize(1)] },
                    mapRate,
                    source_alignment_offset);
        if (parse_geometry.final_mesh.is_some()) {
            effct_final_mesh.ChangeMeshDataFrom(**parse_geometry.final_mesh);
        } else {
            GenCardMesh(effct_final_mesh,
                        { geometry_size[usize(0)], geometry_size[usize(1)] },
                        { 1.0f, 1.0f },
                        alignment_offset);
        }
    }
    // The final pass owns the authored blend mode.
    auto finalMaterialState = material;
    if (color_blend_attachment_override.is_some())
        finalMaterialState.SetBlendMode(*color_blend_attachment_override);
    SceneNodeLayer* image_effect_layer { nullptr };
    if (! material.textures.is_empty()) {
        auto control = context.scene->VideoControl(material.textures[usize()].as_str());
        if (control.is_some()) spImgNode->SetVideoControl(rstd::move(*control));
    }
    mesh.AddMaterial(rstd::move(material));
    track_image_property_material(mesh.MaterialSlots().last_mut().unwrap().get_mut());
    RegisterMaterialBindings(
        *context.scene,
        mesh.MaterialSlots().first_mut().unwrap().get_mut(),
        image_wpmat,
        shaderInfo,
        Some(ref<wpscene::Material>::from_raw_parts(&image_user_texture_fallback)));
    WireMaterialShaderValueScripts(context,
                                   spImgNode,
                                   mesh.MaterialSlots().last_mut().unwrap().get_mut(),
                                   image_wpmat,
                                   shaderInfo);

    for (const auto* supplemental_mesh : supplemental_puppet_meshes) {
        if (supplemental_mesh->mat_json_files.is_empty()) continue;
        const auto& material_ref       = supplemental_mesh->mat_json_files.first().unwrap().get();
        auto        supplemental_wpmat = MdlParser::ParseMaterial(material_ref, vfs);
        if (supplemental_wpmat.is_none()) continue;

        (void)supplemental_wpmat->combos.insert(rstd::into(WE_CB_SCENE_ORTHO),
                                                i32(wpimgobj.perspective ? 0 : 1));
        (void)supplemental_wpmat->combos.insert(rstd::into(OWE_CB_IMAGE_LAYER), i32(1));

        auto supplemental_user_texture_fallback = supplemental_wpmat->clone();
        ApplyUserTextureBindings(context, *supplemental_wpmat);

        SceneMaterial supplemental_material;
        ShaderInfo    supplemental_shader_info;
        supplemental_shader_info.baseConstSvs = baseConstSvs.clone();
        auto supplemental_result              = BuildMaterial(vfs,
                                                              *context.shader_cache,
                                                              context.shader_environment,
                                                              *supplemental_wpmat,
                                                              *context.scene,
                                                              rstd::move(supplemental_shader_info));
        if (supplemental_result.is_err()) {
            rstd_warn("load puppet material '{}' failed for '{}'", material_ref, wpimgobj.name);
            continue;
        }
        auto supplemental_build  = rstd::move(supplemental_result).unwrap_unchecked();
        supplemental_material    = rstd::move(supplemental_build.material);
        supplemental_shader_info = rstd::move(supplemental_build.shader_info);
        LoadConstvalue(
            context, supplemental_material, *supplemental_wpmat, supplemental_shader_info);

        const auto supplemental_uv_scale = Texture0UvScale(supplemental_material);
        const auto supplemental_slot =
            rstd::as_cast<u32>(usize(mesh.MaterialSlots().len().to_primitive()));
        mesh.AddMaterial(rstd::move(supplemental_material));
        track_image_property_material(mesh.MaterialSlots().last_mut().unwrap().get_mut());
        RegisterMaterialBindings(
            *context.scene,
            mesh.MaterialSlots().last_mut().unwrap().get_mut(),
            *supplemental_wpmat,
            supplemental_shader_info,
            Some(ref<wpscene::Material>::from_raw_parts(&supplemental_user_texture_fallback)));
        WireMaterialShaderValueScripts(context,
                                       spImgNode,
                                       mesh.MaterialSlots().last_mut().unwrap().get_mut(),
                                       *supplemental_wpmat,
                                       supplemental_shader_info);

        mesh.Submeshes().emplace_back();
        auto& supplemental_submesh = mesh.Submeshes().last_mut().unwrap().get_mut();
        MdlParser::GenMeshFromMdl(
            supplemental_submesh,
            *supplemental_mesh,
            { supplemental_uv_scale[usize()], supplemental_uv_scale[usize(1)] },
            hasEffect ? array<float, 3> { effect_target_size[usize()] * -0.5f,
                                          effect_target_size[usize(1)] * -0.5f,
                                          0.0f }
                      : array<float, 3> {});
        supplemental_submesh.material_slot   = supplemental_slot;
        supplemental_submesh.preserve_output = true;
    }

    // Puppet clipping masks: each MaskBlock becomes a pair of submeshes.
    // 1) Pre-pass: clippingmaskimage4 over `part_ids_b` (mask shape mesh)
    //    writes the mask RT.
    // 2) Clipped main: a clone of the main material with CLIPPINGTARGET combo
    //    + g_Texture8 = mask RT, draw range = `part_ids_a` (the clipped parts).
    // The original main submesh has all `part_ids_a` parts removed so the
    // clipped region is only drawn through the masked variant.
    if (puppet.is_some() && ! hasEffect && has_bones && puppet_has_masks) {
        // `part_ids_a` indexes into pmesh.parts[] (position), not part.id.
        BTreeSet<u32> clipped_indices;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(u32(idx));
            }
        }
        // Rebuild main submeshes' draw_ranges: drop any part whose position
        // index is in `part_ids_a` of any mask block.
        if (! clipped_indices.is_empty()) {
            size_t smi = 0;
            for (const auto& pmesh : (*puppet)->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
                if (pmesh.positions.is_empty()) continue;
                if (smi >= mesh.Submeshes().len().to_primitive()) break;
                Vec<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.contains(rstd::as_cast<u32>(i))) continue;
                    kept.push({ u32(p.start), u32(p.size) });
                }
                mesh.Submeshes()[usize(smi)].draw_ranges = rstd::move(kept);
                ++smi;
            }
        }

        const auto albedo_tex =
            image_wpmat.textures.is_empty() ? String {} : image_wpmat.textures[usize(0)].clone();
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                // (1) mask pre-pass submesh
                wpscene::Material mask_wpmat;
                mask_wpmat.shader     = "clippingmaskimage4"_Str;
                mask_wpmat.blending   = "translucent"_Str;
                mask_wpmat.depthtest  = "disabled"_Str;
                mask_wpmat.depthwrite = "disabled"_Str;
                mask_wpmat.cullmode   = "nocull"_Str;
                mask_wpmat.textures.resize(usize(2), String {});
                mask_wpmat.textures[usize(0)] = albedo_tex.clone();
                mask_wpmat.textures[usize(1)] = mb.mat_json.clone();
                MdlParser::AddPuppetMatInfo(mask_wpmat, **puppet);

                SceneMaterial mask_scene_mat;
                ShaderInfo    mask_shaderInfo;
                mask_shaderInfo.baseConstSvs = baseConstSvs.clone();
                auto mask_result             = BuildMaterial(vfs,
                                                             *context.shader_cache,
                                                             context.shader_environment,
                                                             mask_wpmat,
                                                             *context.scene,
                                                             rstd::move(mask_shaderInfo));
                if (mask_result.is_err()) {
                    rstd_warn("load mask pre-pass material failed for '{}'", wpimgobj.name);
                    continue;
                }
                auto mask_build = rstd::move(mask_result).unwrap_unchecked();
                mask_scene_mat  = rstd::move(mask_build.material);
                mask_shaderInfo = rstd::move(mask_build.shader_info);
                const auto pre_slot =
                    rstd::as_cast<u32>(usize(mesh.MaterialSlots().len().to_primitive()));
                mesh.AddMaterial(rstd::move(mask_scene_mat));
                track_image_property_material(mesh.MaterialSlots().last_mut().unwrap().get_mut());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().last_mut().unwrap().get_mut();
                MdlParser::GenMaskSubmeshFromMdl(pre_sm,
                                                 pmesh,
                                                 mb.part_ids_b.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                pre_sm.material_slot   = pre_slot;
                pre_sm.output_override = rstd::into(PUPPET_MASK_RT);

                // (2) clipped-main submesh: main material + CLIPPINGTARGET
                wpscene::Material clip_wpmat = image_wpmat.clone();
                (void)clip_wpmat.combos.insert("CLIPPINGTARGET"_Str, i32(1));
                (void)clip_wpmat.combos.insert("CLIPPINGUVS"_Str, i32(1));
                if (clip_wpmat.textures.len() < usize(9))
                    clip_wpmat.textures.resize(usize(9), String {});
                clip_wpmat.textures[usize(8)] = rstd::into(PUPPET_MASK_RT);
                MdlParser::AddPuppetMatInfo(clip_wpmat, **puppet);

                SceneMaterial clip_scene_mat;
                ShaderInfo    clip_shaderInfo;
                clip_shaderInfo.baseConstSvs = baseConstSvs.clone();
                auto clip_result             = BuildMaterial(vfs,
                                                             *context.shader_cache,
                                                             context.shader_environment,
                                                             clip_wpmat,
                                                             *context.scene,
                                                             rstd::move(clip_shaderInfo));
                if (clip_result.is_err()) {
                    rstd_warn("load clipped main material failed for '{}'", wpimgobj.name);
                    continue;
                }
                auto clip_build = rstd::move(clip_result).unwrap_unchecked();
                clip_scene_mat  = rstd::move(clip_build.material);
                clip_shaderInfo = rstd::move(clip_build.shader_info);
                LoadConstvalue(context, clip_scene_mat, clip_wpmat, clip_shaderInfo);
                const auto clip_slot =
                    rstd::as_cast<u32>(usize(mesh.MaterialSlots().len().to_primitive()));
                mesh.AddMaterial(rstd::move(clip_scene_mat));
                track_image_property_material(mesh.MaterialSlots().last_mut().unwrap().get_mut());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().last_mut().unwrap().get_mut();
                MdlParser::GenMaskSubmeshFromMdl(clip_sm,
                                                 pmesh,
                                                 mb.part_ids_a.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                clip_sm.material_slot = clip_slot;
            }
        }
    }

    if (image_puppet_layer.is_some() && primary_puppet_mesh != nullptr) {
        auto& ordered_mesh = hasEffect ? effct_final_mesh : mesh;
        for (auto& submesh : ordered_mesh.Submeshes())
            MdlParser::BindDrawOrder(submesh, *primary_puppet_mesh, (*image_puppet_layer).clone());
    }
    spImgNode->AddMesh(spMesh.clone());

    SetUniformConfig(context, spImgNode, rstd::move(svData));
    if (hasEffect) {
        auto&      scene    = *context.scene;
        auto       nodeAddr = scene.NodeResourceKey(image_node_id, "layer_camera"_str);
        const auto effect_extent =
            NonZeroRenderTargetExtent(effect_target_size[usize()], effect_target_size[usize(1)]);
        auto active = scene.ActiveCamera();
        if (active.is_none()) return;
        // set camera to attatch effect
        Arc<SceneCamera> layer_camera =
            isPassthrough ? Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(
                                (**active).Width(), (**active).Height(), -1.0, 1.0))
                          : Arc<SceneCamera>::make(SceneCamera::MakeOrthographic(
                                rstd::as_cast<double>(effect_extent[usize()]),
                                rstd::as_cast<double>(effect_extent[usize(1)]),
                                -1.0,
                                1.0));
        if (isPassthrough) {
            auto attached = (**active).GetAttachedNode();
            if (attached.is_some()) layer_camera->AttatchNode(attached.unwrap());
            scene.RegisterLinkedCamera("global"_Str, String::make(nodeAddr));
        } else {
            // Attach the per-layer effect camera to spImgNode itself so the
            // camera follows the layer through any parent-container world
            // translation. Otherwise the layer's quad ends up off-center in
            // the ping-pong RT whenever the layer is nested under a non-zero
            // container.
            layer_camera->AttatchNode(spImgNode.as_ptr());
        }
        scene.RegisterCamera(String::make(nodeAddr), layer_camera.clone());
        if (wpimgobj.composite_layer) {
            const auto group_camera       = rstd::format("{}_group", nodeAddr);
            auto       group_camera_owner = Arc<SceneCamera>::make(
                SceneCamera::MakeOrthographic(rstd::as_cast<double>(effect_extent[usize()]),
                                              rstd::as_cast<double>(effect_extent[usize(1)]),
                                              -1.0,
                                              1.0));
            group_camera_owner->AttatchNode(spImgNode.as_ptr());
            scene.RegisterCamera(String::make(group_camera), rstd::move(group_camera_owner));
            scene.RegisterRenderGroup(WallpaperLayerId { .value = wpimgobj.id },
                                      String::make(group_camera));
        }
        spImgNode->SetCamera(nodeAddr);
        const auto effect_composite = scene.NodeResourceKey(image_node_id, "layer_composite"_str);
        // set image effect
        auto imgEffectLayer =
            Arc<SceneNodeLayer>::make(spImgNode.as_ptr(),
                                      rstd::as_cast<float>(effect_extent[usize()]),
                                      rstd::as_cast<float>(effect_extent[usize(1)]),
                                      effect_composite);
        image_effect_layer = imgEffectLayer.as_ptr().as_raw_ptr();
        {
            imgEffectLayer->SetRequiresSourceDraw(parse_geometry.requires_source_draw);
            imgEffectLayer->SetIntermediateSourceBlend(BlendMode::Normal);
            imgEffectLayer->SetFullscreen(wpimgobj.fullscreen);
            imgEffectLayer->SetFinalMaterialState(finalMaterialState);
            imgEffectLayer->SetSkipWhenNoRuntimeEffect(wpimgobj.fullscreen || isPassthrough);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            spImgNode->AttachLayer(imgEffectLayer.clone());
        }
        // set renderTarget for ping-pong operate
        {
            SceneRenderTarget target {
                .width                = effect_extent[usize()],
                .height               = effect_extent[usize(1)],
                .allowReuse           = true,
                .force_clear          = ! wpimgobj.fullscreen && ! wpimgobj.composite_layer,
                .clear_on_first_write = true,
                .preserve_on_write    = wpimgobj.composite_layer,
            };
            if (wpimgobj.composite_layer) {
                const auto clear_alpha = image_wpmat.combos.get("CLEARALPHA"_str);
                if (clear_alpha.is_none() || **clear_alpha != i32(1))
                    target.blend_alpha_write = Some(false);
            }
            if (wpimgobj.fullscreen) {
                target.bind = { .enable = true, .screen = true };
            }
            // Point-art images (noInterpolation) must stay point-sampled through
            // the whole effect chain.
            if (point_source) {
                auto& s     = target.sample;
                s.magFilter = s.minFilter = TextureFilter::NEAREST;
            }
            scene.RegisterRenderTarget(String::make(effect_composite), rstd::move(target));
        }

        bool       last_effect_can_composite_final { false };
        const bool allow_transparent_previous_final = ! solid_composite_context;
        const bool passthrough_can_composite_final =
            isPassthrough || ! parse_geometry.requires_source_draw;
        for (const auto& wpeffobj : wpimgobj.effects) {
            Arc<SceneImageEffect> imgEffect = Arc<SceneImageEffect>::make();
            imgEffect->name                 = rstd::into(wpeffobj.name.as_str());
            imgEffect->runtime_visible      = wpeffobj.visible;
            const auto effect_id =
                scene.RegisterEffect(image_node_id, *imgEffectLayer, imgEffect.clone());
            if (! wpeffobj.visible_user.empty()) {
                imgEffect->visible_user_binding =
                    ToSceneUserVisibilityBinding(wpeffobj.visible_user);
            }
            WireImageEffectVisibilityScript(context, spImgNode.as_ptr(), wpeffobj, effect_id);

            const auto inRT = effect_composite.as_str();

            EffectRenderTargets render_targets;
            {
                (void)render_targets.insert("previous"_Str, String::make(inRT));
                for (usize i {}; i < wpeffobj.fbos.len(); i++) {
                    const auto& wpfbo = wpeffobj.fbos.at(i);
                    // Some effects (e.g. WE DOF) use fbo names without the
                    // `_rt_` prefix (`_coc`, `_downscaled1`, ...). Force the
                    // prefix so IsSpecTex / render-target lookups treat them
                    // as render targets instead of disk textures.
                    auto rtname = scene.EffectResourceKey(effect_id, wpfbo.name.as_str());
                    if (wpimgobj.fullscreen) {
                        SceneRenderTarget target {
                            .width      = i32(2),
                            .height     = i32(2),
                            .allowReuse = ! wpfbo.unique,
                        };
                        target.bind = {
                            .enable = true,
                            .screen = true,
                            .scale  = 1.0 / static_cast<double>(wpfbo.scale.to_primitive()),
                        };
                        scene.RegisterRenderTarget(String::make(rtname), rstd::move(target));
                    } else {
                        auto fbo_size = [&]() -> array<i32, 2> {
                            if (wpfbo.fit > u32()) {
                                const float max_size = rstd::cmp::max(effect_target_size[usize(1)],
                                                                      effect_target_size[usize()]);
                                if (max_size > 0.0f) {
                                    const float fit_scale =
                                        static_cast<float>(wpfbo.fit.to_primitive()) / max_size;
                                    const auto fit_extent = NonZeroRenderTargetExtent(
                                        f32(effect_target_size[usize()] * fit_scale)
                                            .round()
                                            .to_primitive(),
                                        f32(effect_target_size[usize(1)] * fit_scale)
                                            .round()
                                            .to_primitive());
                                    return fit_extent;
                                }
                            }
                            const auto scaled_extent = NonZeroRenderTargetExtent(
                                effect_target_size[usize()] /
                                    static_cast<float>(wpfbo.scale.to_primitive()),
                                effect_target_size[usize(1)] /
                                    static_cast<float>(wpfbo.scale.to_primitive()));
                            return scaled_extent;
                        }();
                        scene.RegisterRenderTarget(
                            String::make(rtname),
                            SceneRenderTarget { .width      = fbo_size[usize()],
                                                .height     = fbo_size[usize(1)],
                                                .allowReuse = ! wpfbo.unique });
                    }
                    (void)render_targets.insert(String::make(wpfbo.name.as_str()),
                                                String::make(rtname));
                }
            }
            // load! effect commands
            {
                for (const auto& el : wpeffobj.commands) {
                    if (el.command != "copy"_str) {
                        rstd_error("Unknown effect command: {}", el.command);
                        continue;
                    }
                    auto target = render_targets.get(el.target.as_str());
                    auto source = render_targets.get(el.source.as_str());
                    if (target.is_none() || source.is_none()) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", el.target, el.source);
                        continue;
                    }
                    auto command_target = el.target == "previous"_str
                                              ? SceneEffectTarget::LayerNext()
                                              : SceneEffectTarget::Named((**target).as_str());
                    auto command_source = el.source == "previous"_str
                                              ? SceneEffectTarget::LayerPrevious()
                                              : SceneEffectTarget::Named((**source).as_str());
                    imgEffect->commands.push({ .cmd      = SceneImageEffect::CmdType::Copy,
                                               .dst      = rstd::move(command_target),
                                               .src      = rstd::move(command_source),
                                               .afterpos = el.afterpos });
                }
            }

            bool eff_mat_ok { true };

            for (usize i_mat {}; i_mat < wpeffobj.materials.len(); i_mat++) {
                wpscene::Material         wpmat = wpeffobj.materials.at(i_mat).clone();
                SceneEffectTarget         matOutRT { SceneEffectTarget::LayerNext() };
                Option<wpscene::Material> user_texture_fallback;
                if (wpeffobj.passes.len() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    ApplyTextureBinds(wpmat, wppass.bind.as_slice(), render_targets);
                    user_texture_fallback = Some(wpmat.clone());
                    ApplyUserTextureBindings(context, wpmat);
                    if (! wppass.target.is_empty()) {
                        auto target = render_targets.get(wppass.target.as_str());
                        if (target.is_none()) {
                            rstd_error("fbo {} not found", wppass.target);
                        } else {
                            matOutRT = SceneEffectTarget::Named((**target).as_str());
                        }
                    }
                }
                // A layer's own effect referencing its composite
                // (`_rt_imageLayerComposite_<self>[_a|_b]`) wants this layer's
                // running chain result.
                for (auto& t : wpmat.textures) {
                    auto composite_id = ParseImageLayerCompositeId(t.as_str());
                    if (composite_id.is_some() &&
                        *composite_id == rstd::as_cast<u32>(wpimgobj.id)) {
                        t = effect_composite.clone();
                    }
                }
                if (wpmat.textures.is_empty()) wpmat.textures.resize(usize(1), String {});
                if (wpmat.textures[usize()].is_empty()) {
                    wpmat.textures[usize(0)] = rstd::into(inRT);
                }
                auto spEffNode = Arc<SceneNode>::make();
                scene.RegisterNode(*spEffNode);
                ShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs.clone();
                (void)wpEffShaderInfo.baseConstSvs.insert(
                    rstd::into(G_ETVP),
                    ShaderValue(ShaderValue::fromMatrix(Eigen::Matrix4f::Identity())));
                (void)wpEffShaderInfo.baseConstSvs.insert(
                    rstd::into(G_ETVPI),
                    ShaderValue(ShaderValue::fromMatrix(Eigen::Matrix4f::Identity())));
                SceneMaterial          material;
                UniformNodeConfigDraft svData;
                svData.SetParallaxContract(
                    wpimgobj.parallax, wpimgobj.id, ! wpimgobj.disablepropagation);
                SceneShaderValueAnimationMap final_quad_shader_values;
                auto effect_result = BuildMaterial(vfs,
                                                   *context.shader_cache,
                                                   context.shader_environment,
                                                   wpmat,
                                                   *context.scene,
                                                   rstd::move(wpEffShaderInfo));
                if (effect_result.is_err()) {
                    eff_mat_ok = false;
                    break;
                }
                auto effect_build = rstd::move(effect_result).unwrap_unchecked();
                material          = rstd::move(effect_build.material);
                wpEffShaderInfo   = rstd::move(effect_build.shader_info);

                // load glname from alias and load to constvalue
                LoadConstvalue(
                    context, material, wpmat, wpEffShaderInfo, &final_quad_shader_values);
                auto spMesh = Arc<SceneMesh>::make();
                {
                    svData.effect_projection_node = Some(spImgNode.clone());
                    svData.effect_projection_size = { rstd::as_cast<float>(effect_extent[usize()]),
                                                      rstd::as_cast<float>(
                                                          effect_extent[usize(1)]) };
                    if (puppet.is_some() && wpmat.use_puppet) {
                        auto effect_puppet_layer =
                            image_puppet_layer.is_some()
                                ? (*image_puppet_layer).clone()
                                : MakePuppetLayer((*(*puppet)->puppet).clone(),
                                                  wpimgobj.puppet_layers.as_slice());
                        RegisterPuppetLayer(
                            context, spEffNode.as_ptr(), rstd::move(effect_puppet_layer));
                    }
                }
                spMesh->AddMaterial(rstd::move(material));
                track_image_property_material(
                    spMesh->MaterialSlots().last_mut().unwrap().get_mut());
                Option<ref<wpscene::Material>> binding_fallback;
                if (user_texture_fallback.is_some()) {
                    binding_fallback = Some(ref<wpscene::Material>::from_raw_parts(
                        rstd::addressof(*user_texture_fallback)));
                }
                RegisterMaterialBindings(*context.scene,
                                         spMesh->MaterialSlots().first_mut().unwrap().get_mut(),
                                         wpmat,
                                         wpEffShaderInfo,
                                         binding_fallback);
                RegisterLayerPreviousBindings(
                    *context.scene, *spMesh->Material(), wpmat, image_node_id, effect_composite);
                WireMaterialShaderValueScripts(
                    context,
                    spImgNode,
                    spMesh->MaterialSlots().first_mut().unwrap().get_mut(),
                    wpmat,
                    wpEffShaderInfo);
                auto add_puppet_mask_materials = [&]() -> bool {
                    if (! (puppet.is_some() && wpmat.use_puppet && puppet_has_masks)) return true;
                    const auto source_tex =
                        wpmat.textures.is_empty() ? String {} : wpmat.textures[usize(0)].clone();
                    for (const auto& pmesh : (*puppet)->meshes) {
                        if (&pmesh != primary_puppet_mesh) continue;
                        for (const auto& mb : pmesh.masks) {
                            wpscene::Material mask_wpmat;
                            mask_wpmat.shader     = "clippingmaskimage4"_Str;
                            mask_wpmat.blending   = "translucent"_Str;
                            mask_wpmat.depthtest  = "disabled"_Str;
                            mask_wpmat.depthwrite = "disabled"_Str;
                            mask_wpmat.cullmode   = "nocull"_Str;
                            mask_wpmat.textures.resize(usize(2), String {});
                            mask_wpmat.textures[usize(0)] = source_tex.clone();
                            mask_wpmat.textures[usize(1)] = mb.mat_json.clone();
                            MdlParser::AddPuppetMatInfo(mask_wpmat, **puppet);

                            SceneMaterial mask_material;
                            ShaderInfo    mask_shaderInfo;
                            mask_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs.clone();
                            auto mask_result = BuildMaterial(vfs,
                                                             *context.shader_cache,
                                                             context.shader_environment,
                                                             mask_wpmat,
                                                             *context.scene,
                                                             rstd::move(mask_shaderInfo));
                            if (mask_result.is_err()) {
                                return false;
                            }
                            auto mask_build = rstd::move(mask_result).unwrap_unchecked();
                            mask_material   = rstd::move(mask_build.material);
                            mask_shaderInfo = rstd::move(mask_build.shader_info);
                            LoadConstvalue(context, mask_material, mask_wpmat, mask_shaderInfo);
                            RegisterLayerPreviousBindings(*context.scene,
                                                          mask_material,
                                                          mask_wpmat,
                                                          image_node_id,
                                                          effect_composite);
                            spMesh->AddMaterial(rstd::move(mask_material));
                            track_image_property_material(
                                spMesh->MaterialSlots().last_mut().unwrap().get_mut());

                            wpscene::Material clip_wpmat = wpmat.clone();
                            (void)clip_wpmat.combos.insert("CLIPPINGTARGET"_Str, i32(1));
                            (void)clip_wpmat.combos.insert("CLIPPINGUVS"_Str, i32(1));
                            if (clip_wpmat.textures.len() < usize(9))
                                clip_wpmat.textures.resize(usize(9), String {});
                            clip_wpmat.textures[usize(8)] = rstd::into(PUPPET_MASK_RT);
                            MdlParser::AddPuppetMatInfo(clip_wpmat, **puppet);

                            SceneMaterial clip_material;
                            ShaderInfo    clip_shaderInfo;
                            clip_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs.clone();
                            auto clip_result = BuildMaterial(vfs,
                                                             *context.shader_cache,
                                                             context.shader_environment,
                                                             clip_wpmat,
                                                             *context.scene,
                                                             rstd::move(clip_shaderInfo));
                            if (clip_result.is_err()) {
                                return false;
                            }
                            auto clip_build = rstd::move(clip_result).unwrap_unchecked();
                            clip_material   = rstd::move(clip_build.material);
                            clip_shaderInfo = rstd::move(clip_build.shader_info);
                            LoadConstvalue(context, clip_material, clip_wpmat, clip_shaderInfo);
                            RegisterLayerPreviousBindings(*context.scene,
                                                          clip_material,
                                                          clip_wpmat,
                                                          image_node_id,
                                                          effect_composite);
                            spMesh->AddMaterial(rstd::move(clip_material));
                            track_image_property_material(
                                spMesh->MaterialSlots().last_mut().unwrap().get_mut());
                        }
                    }
                    return true;
                };
                if (! add_puppet_mask_materials()) {
                    eff_mat_ok = false;
                    break;
                }
                if (auto* mat = spMesh->Material(); mat != nullptr) {
                    last_effect_can_composite_final = CanCompositeFinalEffectMaterial(
                        mat->name.as_str(), wpEffShaderInfo, allow_transparent_previous_final);
                }
                spEffNode->AddMesh(spMesh.clone());

                SetUniformConfig(context, spEffNode, rstd::move(svData));
                imgEffect->AddNode(SceneImageEffectNode {
                    .output                   = rstd::move(matOutRT),
                    .sceneNode                = spEffNode.clone(),
                    .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                    .final_quad_shader_values = rstd::move(final_quad_shader_values),
                });
            }

            if (eff_mat_ok)
                imgEffectLayer->AddEffect(imgEffect);
            else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        auto make_internal_passthrough =
            [&](ref<str> input, SceneEffectTarget output, ref<str> name) {
                Option<Arc<SceneImageEffect>> result;
                wpscene::Material             passthrough_mat;
                auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json"_str);
                if (! json || ! passthrough_mat.FromJson(*json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                    return result;
                }
                if (passthrough_mat.textures.is_empty())
                    passthrough_mat.textures.push(rstd::into(input));
                else
                    passthrough_mat.textures[usize()] = rstd::into(input);

                auto       node = Arc<SceneNode>::make();
                ShaderInfo shader_info;
                shader_info.baseConstSvs = NeutralColorUniforms(baseConstSvs.clone());
                SceneMaterial          material;
                UniformNodeConfigDraft uniform_config;
                uniform_config.SetParallaxContract(
                    wpimgobj.parallax, wpimgobj.id, ! wpimgobj.disablepropagation);
                uniform_config.effect_projection_node = Some(spImgNode.clone());
                uniform_config.effect_projection_size = {
                    rstd::as_cast<float>(effect_extent[usize()]),
                    rstd::as_cast<float>(effect_extent[usize(1)])
                };
                auto material_result = BuildMaterial(vfs,
                                                     *context.shader_cache,
                                                     context.shader_environment,
                                                     passthrough_mat,
                                                     *context.scene,
                                                     rstd::move(shader_info));
                if (material_result.is_err()) {
                    rstd_error("effect passthrough failed to load for '{}'", wpimgobj.name);
                    return result;
                }
                auto material_build = rstd::move(material_result).unwrap_unchecked();
                material            = rstd::move(material_build.material);
                shader_info         = rstd::move(material_build.shader_info);
                LoadConstvalue(context, material, passthrough_mat, shader_info);
                auto mesh = Arc<SceneMesh>::make();
                mesh->AddMaterial(rstd::move(material));
                RegisterMaterialBindings(*context.scene,
                                         mesh->MaterialSlots().first_mut().unwrap().get_mut(),
                                         passthrough_mat,
                                         shader_info);
                RegisterLayerPreviousBindings(*context.scene,
                                              *mesh->Material(),
                                              passthrough_mat,
                                              image_node_id,
                                              effect_composite);
                node->AddMesh(rstd::move(mesh));
                scene.RegisterNode(*node);
                SetUniformConfig(context, node, rstd::move(uniform_config));

                result          = Some(Arc<SceneImageEffect>::make());
                (*result)->name = rstd::into(name);
                (*result)->AddNode(SceneImageEffectNode {
                    .output    = rstd::move(output),
                    .sceneNode = node.clone(),
                });
                return result;
            };

        if (is_linked_source) {
            const auto link_output = scene.EnsureLinkRenderTarget(
                WallpaperLayerId { .value = i32(wpimgobj.id) }, *spImgNode);
            scene.RegisterLayerLinkSource(WallpaperLayerId { .value = i32(wpimgobj.id) },
                                          *spImgNode);
            auto publish = make_internal_passthrough(effect_composite,
                                                     SceneEffectTarget::Named(link_output.as_str()),
                                                     "linked_layer_publish"_str);
            auto visible = make_internal_passthrough(link_output.as_str(),
                                                     SceneEffectTarget::Named(SpecTex_Default),
                                                     "linked_layer_visible_resolve"_str);
            if (publish && visible) {
                imgEffectLayer->SetPublishedEffect(rstd::move(*publish));
                imgEffectLayer->SetVisibleResolveEffect(rstd::move(*visible));
                (void)scene.SetNodeVisible(*spImgNode, wpimgobj.visible);
            }
        }

        if (! is_linked_source && ! wpimgobj.fullscreen && ! wpimgobj.copybackground &&
            ! passthrough_can_composite_final && ! last_effect_can_composite_final) {
            wpscene::Material passthrough_mat;
            auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json"_str);
            if (! json) {
                rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
            } else {
                if (! passthrough_mat.FromJson(*json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                } else {
                    if (passthrough_mat.textures.is_empty())
                        passthrough_mat.textures.push(effect_composite.clone());
                    else
                        passthrough_mat.textures[usize()] = effect_composite.clone();

                    auto finalEffect = Arc<SceneImageEffect>::make();
                    auto spFinalNode = Arc<SceneNode>::make();

                    ShaderInfo wpFinalShaderInfo;
                    wpFinalShaderInfo.baseConstSvs = NeutralColorUniforms(baseConstSvs.clone());
                    SceneMaterial          finalMaterial;
                    UniformNodeConfigDraft finalSvData;
                    finalSvData.SetParallaxContract(
                        wpimgobj.parallax, wpimgobj.id, ! wpimgobj.disablepropagation);
                    finalSvData.effect_projection_node = Some(spImgNode.clone());
                    finalSvData.effect_projection_size = {
                        rstd::as_cast<float>(effect_extent[usize()]),
                        rstd::as_cast<float>(effect_extent[usize(1)])
                    };
                    auto final_result = BuildMaterial(vfs,
                                                      *context.shader_cache,
                                                      context.shader_environment,
                                                      passthrough_mat,
                                                      *context.scene,
                                                      rstd::move(wpFinalShaderInfo));
                    if (final_result.is_ok()) {
                        auto final_build  = rstd::move(final_result).unwrap_unchecked();
                        finalMaterial     = rstd::move(final_build.material);
                        wpFinalShaderInfo = rstd::move(final_build.shader_info);
                        LoadConstvalue(context, finalMaterial, passthrough_mat, wpFinalShaderInfo);
                        auto spFinalMesh = Arc<SceneMesh>::make();
                        spFinalMesh->AddMaterial(rstd::move(finalMaterial));
                        RegisterMaterialBindings(
                            *context.scene,
                            spFinalMesh->MaterialSlots().first_mut().unwrap().get_mut(),
                            passthrough_mat,
                            wpFinalShaderInfo);
                        RegisterLayerPreviousBindings(*context.scene,
                                                      *spFinalMesh->Material(),
                                                      passthrough_mat,
                                                      image_node_id,
                                                      effect_composite);
                        spFinalNode->AddMesh(spFinalMesh.clone());
                        SetUniformConfig(context, spFinalNode, rstd::move(finalSvData));
                        finalEffect->AddNode(SceneImageEffectNode {
                            .output    = SceneEffectTarget::LayerNext(),
                            .sceneNode = spFinalNode.clone(),
                        });
                        imgEffectLayer->AddEffect(finalEffect);
                    } else {
                        rstd_error("effect passthrough failed to load for '{}'", wpimgobj.name);
                    }
                }
            }
        }
    }
    const Matrix4d alignment_base_transform =
        image_effect_layer ? image_effect_layer->FinalMesh().GeometryTransform()
                           : spImgNode->GeometryTransform();
    const Matrix4d source_alignment_base_transform = spMesh->GeometryTransform();
    const Matrix4d direct_alignment_base_transform =
        source_uses_framebuffer_space
            ? source_alignment_base_transform
            : (Affine3d(Translation3d(alignment_offset.cast<double>())).matrix() *
               source_alignment_base_transform)
                  .eval();
    if (image_effect_layer) {
        image_effect_layer->SetSourceGeometryTransforms(source_alignment_base_transform,
                                                        direct_alignment_base_transform);
    }
    RegisterImageAlignmentBinding(
        context,
        spImgNode.as_ptr(),
        wpimgobj.alignment.as_str(),
        SceneParseContext::ImageAlignmentSetter::make([image_effect_layer,
                                                       source_uses_framebuffer_space,
                                                       alignment_base_transform,
                                                       source_alignment_base_transform,
                                                       direct_alignment_base_transform,
                                                       alignment_offset,
                                                       geometry_size](
                                                          SceneNode* node, ref<str> alignment) {
            const Vector3f delta =
                AlignmentOffset(alignment, { geometry_size[usize(0)], geometry_size[usize(1)] }) -
                alignment_offset;
            const Matrix4d alignment_delta = Affine3d(Translation3d(delta.cast<double>())).matrix();
            Matrix4d       transform       = alignment_base_transform * alignment_delta;
            if (image_effect_layer) {
                image_effect_layer->FinalMesh().SetGeometryTransform(rstd::move(transform));
                const Matrix4d source_transform =
                    source_uses_framebuffer_space
                        ? (source_alignment_base_transform * alignment_delta).eval()
                        : source_alignment_base_transform;
                image_effect_layer->SetSourceGeometryTransforms(
                    source_transform, direct_alignment_base_transform * alignment_delta);
            } else if (node)
                node->SetGeometryTransform(rstd::move(transform));
        }));

    AssignNodeFieldAnimations(context, *spImgNode.as_ptr(), wpimgobj.field_bindings);
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (image_puppet_layer.is_some())
        WirePuppetAnimationScripts(context,
                                   spImgNode.as_ptr(),
                                   (*image_puppet_layer).clone(),
                                   wpimgobj.puppet_layers.as_slice());
    if (! wpimgobj.color_user_key.is_empty()) {
        context.scene->RegisterImageColorUserBinding(
            wpimgobj.color_user_key.clone(), spImgNode, image_property_materials.as_slice());
    }
    if (! wpimgobj.alpha_user_key.is_empty()) {
        context.scene->RegisterImageAlphaUserBinding(
            wpimgobj.alpha_user_key.clone(), spImgNode, image_property_materials.as_slice());
    }
    RegisterNodeRef(context,
                    wpimgobj.id,
                    SceneParseContext::NodeRef {
                        wpimgobj.parent,
                        Some(spImgNode.clone()),
                        (puppet.is_some() && (*puppet)->puppet.is_some())
                            ? Some((*(*puppet)->puppet).clone())
                            : None(),
                        wpimgobj.attachment.clone(),
                        image_puppet_layer.is_some() ? Some((*image_puppet_layer).clone()) : None(),
                    });
}

void ParseShapeObj(SceneParseContext& context, wpscene::ShapeObject& shape_obj) {
    PrepareAnimationBindings(context, shape_obj);
    if (shape_obj.shape != "quad"_str) {
        rstd_error("unsupported shape '{}' for '{}'", shape_obj.shape, shape_obj.name);
        return;
    }

    const wpscene::ImageEffect* first_effect { nullptr };
    const wpscene::ImageEffect* last_effect { nullptr };
    for (const auto& effect : shape_obj.effects) {
        if (first_effect == nullptr) first_effect = &effect;
        last_effect = &effect;
    }
    if (first_effect == nullptr || first_effect->materials.is_empty() ||
        first_effect->passes.is_empty() || last_effect == nullptr ||
        last_effect->materials.is_empty() || last_effect->passes.is_empty()) {
        rstd_error("shape '{}' has no renderable effect", shape_obj.name);
        return;
    }
    auto direct_draw_material = first_effect->materials.first().unwrap()->clone();
    direct_draw_material.MergePass(first_effect->passes.first().unwrap().get());
    auto direct_draw = direct_draw_material.combos.get("DIRECTDRAW"_str);
    if (direct_draw.is_none() || **direct_draw == i32()) {
        rstd_error("shape '{}' first effect is not direct draw", shape_obj.name);
        return;
    }
    auto points = ReadDirectDrawQuad(direct_draw_material);
    if (points.is_none()) {
        rstd_error("shape '{}' has invalid direct draw points", shape_obj.name);
        return;
    }

    const auto edge = static_cast<float>(context.ortho_h.to_primitive());
    SceneMesh  direct_draw_mesh;
    GenDirectDrawQuadMesh(direct_draw_mesh, edge, *points);

    wpscene::ImageObject image;
    image.id       = shape_obj.id;
    image.name     = rstd::move(shape_obj.name);
    image.origin   = shape_obj.origin;
    image.scale    = shape_obj.scale;
    image.angles   = shape_obj.angles;
    image.size     = { edge, edge };
    image.visible  = shape_obj.visible;
    image.material = last_effect->materials.last().unwrap()->clone();
    image.material.MergePass(last_effect->passes.last().unwrap().get());
    image.material.blending  = "additive"_Str;
    image.effects            = rstd::move(shape_obj.effects);
    image.nopadding          = true;
    image.locktransforms     = shape_obj.locktransforms;
    image.muteineditor       = shape_obj.muteineditor;
    image.nointerpolation    = shape_obj.nointerpolation;
    image.reflected          = shape_obj.reflected;
    image.castshadow         = shape_obj.castshadow;
    image.disablepropagation = shape_obj.disablepropagation;
    image.parent             = shape_obj.parent;
    image.attachment         = rstd::move(shape_obj.attachment);
    image.dependencies       = rstd::move(shape_obj.dependencies);
    image.field_bindings     = rstd::move(shape_obj.field_bindings);
    image.visible_user       = rstd::move(shape_obj.visible_user);
    image.visible_user_key   = rstd::move(shape_obj.visible_user_key);
    image.parallax           = shape_obj.parallax;
    ParseImageObjImpl(context,
                      image,
                      ImageParseGeometry {
                          .requires_source_draw = false,
                          .final_mesh = Some(ref<SceneMesh>::from_raw_parts(&direct_draw_mesh)),
                      });
}

void ParseImageObj(SceneParseContext& context, wpscene::ImageObject& image) {
    PrepareAnimationBindings(context, image);
    ParseImageObjImpl(context, image);
}

} // namespace owe
