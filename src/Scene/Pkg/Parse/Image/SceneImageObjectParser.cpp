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
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
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
    if (! std::isfinite(value) || value < 1.0f) return i32(1);
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
    return { obj.size[0], obj.size[1] };
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
    const bool alpha_can_change   = ! wpimgobj.alpha_user_key.empty() ||
                                    wpimgobj.field_bindings.HasAnimation("alpha"_str) ||
                                    wpimgobj.field_bindings.HasScript("alpha"_str);
    const auto geometry_size      = wpimgobj.size;
    const auto effect_target_size = ImageEffectTargetSize(context, wpimgobj);

    Option<Box<Mdl>>      puppet;
    bool                  has_bones = false;
    bool                  has_mesh  = false;
    const Mdl::Mesh*      primary_puppet_mesh { nullptr };
    Vec<const Mdl::Mesh*> supplemental_puppet_meshes;
    if (! wpimgobj.puppet.empty()) {
        auto parsed_puppet = Box<Mdl>::make();
        if (! MdlParser::Parse(
                rstd::cppstd::as_str(wpimgobj.puppet).unwrap(), vfs, *parsed_puppet)) {
            rstd_error("parse puppet failed: {}", wpimgobj.puppet);
        } else {
            has_bones =
                parsed_puppet->puppet.is_some() && ! (*parsed_puppet->puppet)->bones.is_empty();
            if (! wpimgobj.material_path.empty()) {
                auto primary_index = MdlParser::FindMeshByMaterial(
                    *parsed_puppet, rstd::cppstd::as_str(wpimgobj.material_path).unwrap());
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

    const bool has_author_effect = CountVisibleImageEffects(wpimgobj.effects) > i32();
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
        auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
        if (! json) {
            return;
        }
        colorMat.FromJson(*json);
        colorMat.combos[rstd::cppstd::to_string(WE_CB_BONECOUNT)] = i32(1);
        color_blend_attachment_override = ApplyImageColorBlend(colorMat, wpimgobj);
        colorEffect.materials.push_back(std::move(colorMat));
        wpimgobj.effects.push_back(std::move(colorEffect));
    }
    const bool is_linked_source = context.linked_source_ids.contains(wpimgobj.id);
    if (! has_author_effect && wpimgobj.composite_layer && ! is_linked_source) {
        AppendLayerCompositePassthroughEffect(vfs, wpimgobj);
    }

    bool hasEffect = CountVisibleImageEffects(wpimgobj.effects) > i32() || is_linked_source;

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
                                                    wpimgobj.name);
    const Vector3f alignment_offset =
        wpimgobj.fullscreen ? Vector3f::Zero()
                            : AlignmentOffset(rstd::cppstd::as_str(wpimgobj.alignment).unwrap(),
                                              { geometry_size[0], geometry_size[1] });
    const bool solid_composite_context = HasSolidCompositeContext(context, wpimgobj);
    spImgNode->SetSize({ geometry_size[0], geometry_size[1] });
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
    Vec<std::shared_ptr<SceneMaterial>> image_property_materials;
    auto track_image_property_material = [&](std::shared_ptr<SceneMaterial> mat) {
        if ((wpimgobj.color_user_key.empty() && wpimgobj.alpha_user_key.empty()) || ! mat) return;
        image_property_materials.emplace_back(std::move(mat));
    };
    Option<Arc<PuppetLayer>> image_puppet_layer;
    if (puppet.is_some() && has_bones) {
        image_puppet_layer =
            Some(MakePuppetLayer((*(*puppet)->puppet).clone(), wpimgobj.puppet_layers));
        RegisterPuppetLayer(context, spImgNode.as_ptr(), (*image_puppet_layer).clone());
    }

    // Puppet clipping masks: register the half-res shared RT here; per-mask
    // submeshes (pre-pass + clipped main) are emitted below after the base
    // material/mesh are built. Main material stays unmodified — only the
    // clipped-main submesh gets a CLIPPINGTARGET combo + g_Texture8 binding.
    constexpr std::string_view PUPPET_MASK_RT   = "_rt_puppet_mask";
    bool                       puppet_has_masks = false;
    if (primary_puppet_mesh != nullptr) puppet_has_masks = ! primary_puppet_mesh->masks.is_empty();
    if (puppet_has_masks && has_bones &&
        context.scene->RenderTarget(as_str(PUPPET_MASK_RT).unwrap()).is_none()) {
        SceneRenderTarget rt {};
        rt.width       = i32(2);
        rt.height      = i32(2);
        rt.allowReuse  = true;
        rt.force_clear = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = 0.5f;
        context.scene->RegisterRenderTarget(String::make(as_str(PUPPET_MASK_RT).unwrap()),
                                            rstd::move(rt));
    }

    SceneMaterial          material;
    UniformNodeConfigDraft svData;

    ShaderValueMap    baseConstSvs = context.global_base_uniforms;
    ShaderInfo        shaderInfo;
    wpscene::Material image_wpmat = wpimgobj.material.clone();
    image_wpmat.combos[rstd::cppstd::to_string(WE_CB_SCENE_ORTHO)] =
        i32(wpimgobj.perspective ? 0 : 1);
    image_wpmat.combos[rstd::cppstd::to_string(OWE_CB_IMAGE_LAYER)] = i32(1);
    wpscene::Material image_user_texture_fallback                   = image_wpmat.clone();
    if (color_blend_uses_layer_material && ! hasEffect) ApplyImageColorBlend(image_wpmat, wpimgobj);
    ApplyUserTextureBindings(context, image_wpmat);
    {
        svData.SetParallaxContract(wpimgobj.parallax, wpimgobj.id, ! wpimgobj.disablepropagation);
        if (! hasEffect && puppet.is_some() && has_bones) {
            MdlParser::AddPuppetShaderInfo(shaderInfo, **puppet);
        }

        baseConstSvs[rstd::cppstd::to_string(G_COLOR4)] = std::array<float, 4> {
            wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2], wpimgobj.alpha
        };
        baseConstSvs[rstd::cppstd::to_string(G_COLOR)] =
            std::array<float, 3> { wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2] };
        baseConstSvs[rstd::cppstd::to_string(G_ALPHA)]      = wpimgobj.alpha;
        baseConstSvs[rstd::cppstd::to_string(G_USERALPHA)]  = wpimgobj.alpha;
        baseConstSvs[rstd::cppstd::to_string(G_BRIGHTNESS)] = wpimgobj.brightness;

        shaderInfo.baseConstSvs = baseConstSvs;

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
    if (! material.textures.empty()) {
        auto texture =
            context.scene->Texture(rstd::cppstd::as_str(material.textures.front()).unwrap());
        point_source = texture.is_some() && (**texture).sample.magFilter == TextureFilter::NEAREST;
    }

    // mesh
    SceneMesh             effct_final_mesh {};
    auto                  spMesh                  = std::make_shared<SceneMesh>();
    auto&                 mesh                    = *spMesh;
    const array<float, 2> mapRate                 = Texture0UvScale(material, wpimgobj.nopadding);
    const Vector3f        source_alignment_offset = hasEffect ? Vector3f::Zero() : alignment_offset;
    auto                  add_puppet_mask_submeshes = [&](SceneMesh& target, u32 first_mask_slot) {
        if (! puppet_has_masks || primary_puppet_mesh == nullptr) return;
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : (*puppet)->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
                if (pmesh.positions.is_empty()) continue;
                if (smi >= target.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len().to_primitive());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count(static_cast<uint32_t>(i.to_primitive())) != 0)
                        continue;
                    kept.push_back({ u32(p.start), u32(p.size) });
                }
                target.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        u32 slot = first_mask_slot;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                target.Submeshes().emplace_back();
                auto& pre_sm = target.Submeshes().back();
                MdlParser::GenMaskSubmeshFromMdl(pre_sm,
                                                 pmesh,
                                                 mb.part_ids_b.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                pre_sm.material_slot   = slot++;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                target.Submeshes().emplace_back();
                auto& clip_sm = target.Submeshes().back();
                MdlParser::GenMaskSubmeshFromMdl(clip_sm,
                                                 pmesh,
                                                 mb.part_ids_a.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                clip_sm.material_slot = slot++;
            }
        }
    };

    if (puppet.is_some()) {
        if (hasEffect) {
            effct_final_mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            GenCardMesh(
                mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
            if (primary_puppet_mesh != nullptr) {
                effct_final_mesh.Submeshes().emplace_back();
                MdlParser::GenMeshFromMdl(
                    effct_final_mesh.Submeshes().back(), *primary_puppet_mesh, { 1.0f, 1.0f });
            }
            if (has_bones) add_puppet_mask_submeshes(effct_final_mesh, u32(1));

            if (has_bones) {
                wpscene::ImageEffect puppet_effect;
                wpscene::Material    puppet_mat = image_wpmat.clone();
                puppet_mat.textures[0]          = "";
                MdlParser::AddPuppetMatInfo(puppet_mat, **puppet);
                if (color_blend_uses_layer_material)
                    color_blend_attachment_override = ApplyImageColorBlend(puppet_mat, wpimgobj);
                puppet_effect.materials.push_back(std::move(puppet_mat));
                wpimgobj.effects.push_back(std::move(puppet_effect));
            }
        } else {
            mesh.SetGeometryTransform(
                Affine3d(Translation3d(alignment_offset.cast<double>())).matrix());
            if (primary_puppet_mesh != nullptr) {
                mesh.Submeshes().emplace_back();
                MdlParser::GenMeshFromMdl(mesh.Submeshes().back(),
                                          *primary_puppet_mesh,
                                          { mapRate[usize()], mapRate[usize(1)] });
            }
        }
    }
    if (puppet.is_none()) {
        GenCardMesh(mesh, { geometry_size[0], geometry_size[1] }, mapRate, source_alignment_offset);
        if (parse_geometry.final_mesh.is_some()) {
            effct_final_mesh.ChangeMeshDataFrom(**parse_geometry.final_mesh);
        } else {
            GenCardMesh(effct_final_mesh,
                        { geometry_size[0], geometry_size[1] },
                        { 1.0f, 1.0f },
                        alignment_offset);
        }
    }
    // The final pass owns the authored blend mode.
    auto finalMaterialState = material;
    if (color_blend_attachment_override.is_some())
        finalMaterialState.blenmode = *color_blend_attachment_override;
    SceneNodeLayer* image_effect_layer { nullptr };
    if (! material.textures.empty()) {
        auto control =
            context.scene->VideoControl(rstd::cppstd::as_str(material.textures.front()).unwrap());
        if (control.is_some()) spImgNode->SetVideoControl(rstd::move(*control));
    }
    mesh.AddMaterial(std::move(material));
    track_image_property_material(mesh.MaterialSlots().back());
    RegisterMaterialBindings(
        *context.scene,
        mesh.MaterialSlots().front(),
        image_wpmat,
        shaderInfo,
        Some(ref<wpscene::Material>::from_raw_parts(&image_user_texture_fallback)));
    WireMaterialShaderValueScripts(
        context, spImgNode, mesh.MaterialSlots().back(), image_wpmat, shaderInfo);

    for (const auto* supplemental_mesh : supplemental_puppet_meshes) {
        if (supplemental_mesh->mat_json_files.is_empty()) continue;
        const auto& material_ref       = supplemental_mesh->mat_json_files[usize()];
        auto        supplemental_wpmat = MdlParser::ParseMaterial(material_ref, vfs);
        if (supplemental_wpmat.is_none()) continue;

        supplemental_wpmat->combos[rstd::cppstd::to_string(WE_CB_SCENE_ORTHO)] =
            i32(wpimgobj.perspective ? 0 : 1);
        supplemental_wpmat->combos[rstd::cppstd::to_string(OWE_CB_IMAGE_LAYER)] = i32(1);

        auto supplemental_user_texture_fallback = supplemental_wpmat->clone();
        ApplyUserTextureBindings(context, *supplemental_wpmat);

        SceneMaterial supplemental_material;
        ShaderInfo    supplemental_shader_info;
        supplemental_shader_info.baseConstSvs = baseConstSvs;
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
        const auto supplemental_slot     = rstd::as_cast<u32>(usize(mesh.MaterialSlots().size()));
        mesh.AddMaterial(std::move(supplemental_material));
        track_image_property_material(mesh.MaterialSlots().back());
        RegisterMaterialBindings(
            *context.scene,
            mesh.MaterialSlots().back(),
            *supplemental_wpmat,
            supplemental_shader_info,
            Some(ref<wpscene::Material>::from_raw_parts(&supplemental_user_texture_fallback)));
        WireMaterialShaderValueScripts(context,
                                       spImgNode,
                                       mesh.MaterialSlots().back(),
                                       *supplemental_wpmat,
                                       supplemental_shader_info);

        mesh.Submeshes().emplace_back();
        auto& supplemental_submesh = mesh.Submeshes().back();
        MdlParser::GenMeshFromMdl(
            supplemental_submesh,
            *supplemental_mesh,
            { supplemental_uv_scale[usize()], supplemental_uv_scale[usize(1)] });
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
        std::set<uint32_t> clipped_indices;
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                for (auto idx : mb.part_ids_a) clipped_indices.insert(idx);
            }
        }
        // Rebuild main submeshes' draw_ranges: drop any part whose position
        // index is in `part_ids_a` of any mask block.
        if (! clipped_indices.empty()) {
            size_t smi = 0;
            for (const auto& pmesh : (*puppet)->meshes) {
                if (&pmesh != primary_puppet_mesh) continue;
                if (pmesh.positions.is_empty()) continue;
                if (smi >= mesh.Submeshes().size()) break;
                std::vector<SceneMesh::DrawRange> kept;
                kept.reserve(pmesh.parts.len().to_primitive());
                for (usize i {}; i < pmesh.parts.len(); ++i) {
                    const auto& p = pmesh.parts[i];
                    if (p.size == 0) continue;
                    if (clipped_indices.count(static_cast<uint32_t>(i.to_primitive())) != 0)
                        continue;
                    kept.push_back({ u32(p.start), u32(p.size) });
                }
                mesh.Submeshes()[smi].draw_ranges = std::move(kept);
                ++smi;
            }
        }

        const std::string albedo_tex =
            image_wpmat.textures.empty() ? std::string {} : image_wpmat.textures[0];
        for (const auto& pmesh : (*puppet)->meshes) {
            if (&pmesh != primary_puppet_mesh) continue;
            for (const auto& mb : pmesh.masks) {
                // (1) mask pre-pass submesh
                wpscene::Material mask_wpmat;
                mask_wpmat.shader     = "clippingmaskimage4";
                mask_wpmat.blending   = "translucent";
                mask_wpmat.depthtest  = "disabled";
                mask_wpmat.depthwrite = "disabled";
                mask_wpmat.cullmode   = "nocull";
                mask_wpmat.textures.resize(2);
                mask_wpmat.textures[0] = albedo_tex;
                mask_wpmat.textures[1] = rstd::cppstd::to_string(mb.mat_json.as_str());
                MdlParser::AddPuppetMatInfo(mask_wpmat, **puppet);

                SceneMaterial mask_scene_mat;
                ShaderInfo    mask_shaderInfo;
                mask_shaderInfo.baseConstSvs = baseConstSvs;
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
                auto mask_build     = rstd::move(mask_result).unwrap_unchecked();
                mask_scene_mat      = rstd::move(mask_build.material);
                mask_shaderInfo     = rstd::move(mask_build.shader_info);
                const auto pre_slot = rstd::as_cast<u32>(usize(mesh.MaterialSlots().size()));
                mesh.AddMaterial(std::move(mask_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back());
                mesh.Submeshes().emplace_back();
                auto& pre_sm = mesh.Submeshes().back();
                MdlParser::GenMaskSubmeshFromMdl(pre_sm,
                                                 pmesh,
                                                 mb.part_ids_b.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                pre_sm.material_slot   = pre_slot;
                pre_sm.output_override = std::string(PUPPET_MASK_RT);

                // (2) clipped-main submesh: main material + CLIPPINGTARGET
                wpscene::Material clip_wpmat        = image_wpmat.clone();
                clip_wpmat.combos["CLIPPINGTARGET"] = i32(1);
                clip_wpmat.combos["CLIPPINGUVS"]    = i32(1);
                if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                MdlParser::AddPuppetMatInfo(clip_wpmat, **puppet);

                SceneMaterial clip_scene_mat;
                ShaderInfo    clip_shaderInfo;
                clip_shaderInfo.baseConstSvs = baseConstSvs;
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
                const auto clip_slot = rstd::as_cast<u32>(usize(mesh.MaterialSlots().size()));
                mesh.AddMaterial(std::move(clip_scene_mat));
                track_image_property_material(mesh.MaterialSlots().back());
                mesh.Submeshes().emplace_back();
                auto& clip_sm = mesh.Submeshes().back();
                MdlParser::GenMaskSubmeshFromMdl(clip_sm,
                                                 pmesh,
                                                 mb.part_ids_a.as_slice(),
                                                 { mapRate[usize()], mapRate[usize(1)] });
                clip_sm.material_slot = clip_slot;
            }
        }
    }

    spImgNode->AddMesh(spMesh);

    SetUniformConfig(context, spImgNode, rstd::move(svData));
    if (hasEffect) {
        auto&       scene    = *context.scene;
        std::string nodeAddr = rstd::cppstd::to_string(
            scene.NodeResourceKey(image_node_id, "layer_camera"_str).as_str());
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
            scene.RegisterLinkedCamera(String::make("global"_str),
                                       String::make(rstd::cppstd::as_str(nodeAddr).unwrap()));
        } else {
            // Attach the per-layer effect camera to spImgNode itself so the
            // camera follows the layer through any parent-container world
            // translation. Otherwise the layer's quad ends up off-center in
            // the ping-pong RT whenever the layer is nested under a non-zero
            // container.
            layer_camera->AttatchNode(spImgNode.as_ptr());
        }
        scene.RegisterCamera(String::make(rstd::cppstd::as_str(nodeAddr).unwrap()),
                             layer_camera.clone());
        if (wpimgobj.composite_layer) {
            const std::string group_camera       = nodeAddr + "_group";
            auto              group_camera_owner = Arc<SceneCamera>::make(
                SceneCamera::MakeOrthographic(rstd::as_cast<double>(effect_extent[usize()]),
                                              rstd::as_cast<double>(effect_extent[usize(1)]),
                                              -1.0,
                                              1.0));
            group_camera_owner->AttatchNode(spImgNode.as_ptr());
            scene.RegisterCamera(String::make(rstd::cppstd::as_str(group_camera).unwrap()),
                                 rstd::move(group_camera_owner));
            scene.RegisterRenderGroup(WallpaperLayerId { .value = wpimgobj.id },
                                      String::make(rstd::cppstd::as_str(group_camera).unwrap()));
        }
        spImgNode->SetCamera(nodeAddr);
        const std::string effect_composite = rstd::cppstd::to_string(
            scene.NodeResourceKey(image_node_id, "layer_composite"_str).as_str());
        // set image effect
        auto imgEffectLayer =
            std::make_shared<SceneNodeLayer>(spImgNode.as_ptr(),
                                             rstd::as_cast<float>(effect_extent[usize()]),
                                             rstd::as_cast<float>(effect_extent[usize(1)]),
                                             effect_composite);
        image_effect_layer = imgEffectLayer.get();
        {
            imgEffectLayer->SetRequiresSourceDraw(parse_geometry.requires_source_draw);
            imgEffectLayer->SetIntermediateSourceBlend(BlendMode::Normal);
            imgEffectLayer->SetFullscreen(wpimgobj.fullscreen);
            imgEffectLayer->SetFinalMaterialState(finalMaterialState);
            imgEffectLayer->SetSkipWhenNoRuntimeEffect(wpimgobj.fullscreen || isPassthrough);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            spImgNode->AttachLayer(imgEffectLayer);
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
            if (wpimgobj.fullscreen) {
                target.bind = { .enable = true, .screen = true };
            }
            // Point-art images (noInterpolation) must stay point-sampled through
            // the whole effect chain.
            if (point_source) {
                auto& s     = target.sample;
                s.magFilter = s.minFilter = TextureFilter::NEAREST;
            }
            scene.RegisterRenderTarget(String::make(as_str(effect_composite).unwrap()),
                                       rstd::move(target));
        }

        bool       last_effect_can_composite_final { false };
        const bool allow_transparent_previous_final = ! solid_composite_context;
        const bool passthrough_can_composite_final =
            isPassthrough || ! parse_geometry.requires_source_draw;
        for (const auto& wpeffobj : wpimgobj.effects) {
            if (! wpeffobj.visible && ! wpeffobj.visible_can_change()) {
                continue;
            }
            std::shared_ptr<SceneImageEffect> imgEffect = std::make_shared<SceneImageEffect>();
            imgEffect->name                             = wpeffobj.name;
            imgEffect->runtime_visible                  = wpeffobj.visible;
            const auto effect_id = scene.RegisterEffect(image_node_id, *imgEffectLayer, imgEffect);
            if (! wpeffobj.visible_user.empty()) {
                imgEffect->visible_user_binding =
                    ToSceneUserVisibilityBinding(wpeffobj.visible_user);
            }
            WireImageEffectVisibilityScript(context, spImgNode.as_ptr(), wpeffobj, effect_id);

            const std::string inRT { effect_composite };

            EffectRenderTargets render_targets;
            {
                (void)render_targets.insert(String::make("previous"_str),
                                            String::make(as_str(inRT).unwrap()));
                for (std::size_t i = 0; i < wpeffobj.fbos.size(); i++) {
                    const auto& wpfbo = wpeffobj.fbos.at(i);
                    // Some effects (e.g. WE DOF) use fbo names without the
                    // `_rt_` prefix (`_coc`, `_downscaled1`, ...). Force the
                    // prefix so IsSpecTex / render-target lookups treat them
                    // as render targets instead of disk textures.
                    std::string rtname = rstd::cppstd::to_string(
                        scene.EffectResourceKey(effect_id, as_str(wpfbo.name).unwrap()).as_str());
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
                        scene.RegisterRenderTarget(String::make(as_str(rtname).unwrap()),
                                                   rstd::move(target));
                    } else {
                        auto fbo_size = [&]() -> array<i32, 2> {
                            if (wpfbo.fit > u32()) {
                                const float max_size = std::max(effect_target_size[usize()],
                                                                effect_target_size[usize(1)]);
                                if (max_size > 0.0f) {
                                    const float fit_scale =
                                        static_cast<float>(wpfbo.fit.to_primitive()) / max_size;
                                    const auto fit_extent = NonZeroRenderTargetExtent(
                                        std::round(effect_target_size[usize()] * fit_scale),
                                        std::round(effect_target_size[usize(1)] * fit_scale));
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
                            String::make(as_str(rtname).unwrap()),
                            SceneRenderTarget { .width      = fbo_size[usize()],
                                                .height     = fbo_size[usize(1)],
                                                .allowReuse = ! wpfbo.unique });
                    }
                    (void)render_targets.insert(String::make(as_str(wpfbo.name).unwrap()),
                                                String::make(as_str(rtname).unwrap()));
                }
            }
            // load! effect commands
            {
                for (const auto& el : wpeffobj.commands) {
                    if (el.command != "copy") {
                        rstd_error("Unknown effect command: {}", el.command);
                        continue;
                    }
                    auto target = render_targets.get(as_str(el.target).unwrap());
                    auto source = render_targets.get(as_str(el.source).unwrap());
                    if (target.is_none() || source.is_none()) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", el.target, el.source);
                        continue;
                    }
                    auto command_target = el.target == "previous"
                                              ? SceneEffectTarget::LayerNext()
                                              : SceneEffectTarget::Named(
                                                    rstd::cppstd::to_string((**target).as_str()));
                    auto command_source = el.source == "previous"
                                              ? SceneEffectTarget::LayerPrevious()
                                              : SceneEffectTarget::Named(
                                                    rstd::cppstd::to_string((**source).as_str()));
                    imgEffect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                    .dst      = std::move(command_target),
                                                    .src      = std::move(command_source),
                                                    .afterpos = el.afterpos });
                }
            }

            bool eff_mat_ok { true };

            for (std::size_t i_mat = 0; i_mat < wpeffobj.materials.size(); i_mat++) {
                wpscene::Material         wpmat = wpeffobj.materials.at(i_mat).clone();
                SceneEffectTarget         matOutRT { SceneEffectTarget::LayerNext() };
                Option<wpscene::Material> user_texture_fallback;
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    ApplyTextureBinds(wpmat, std::span(wppass.bind), render_targets);
                    user_texture_fallback = Some(wpmat.clone());
                    ApplyUserTextureBindings(context, wpmat);
                    if (! wppass.target.empty()) {
                        auto target = render_targets.get(as_str(wppass.target).unwrap());
                        if (target.is_none()) {
                            rstd_error("fbo {} not found", wppass.target);
                        } else {
                            matOutRT = SceneEffectTarget::Named(
                                rstd::cppstd::to_string((**target).as_str()));
                        }
                    }
                }
                // A layer's own effect referencing its composite
                // (`_rt_imageLayerComposite_<self>[_a|_b]`) wants this layer's
                // running chain result.
                for (auto& t : wpmat.textures) {
                    auto composite_id = ParseImageLayerCompositeId(as_str(t).unwrap());
                    if (composite_id.is_some() &&
                        *composite_id == rstd::as_cast<u32>(wpimgobj.id)) {
                        t = effect_composite;
                    }
                }
                if (wpmat.textures.size() == 0) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) {
                    wpmat.textures[0] = inRT;
                }
                auto spEffNode = Arc<SceneNode>::make();
                scene.RegisterNode(*spEffNode);
                ShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ETVP)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs[rstd::cppstd::to_string(G_ETVPI)] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
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
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.effect_projection_node = Some(spImgNode.clone());
                    svData.effect_projection_size = { rstd::as_cast<float>(effect_extent[usize()]),
                                                      rstd::as_cast<float>(
                                                          effect_extent[usize(1)]) };
                    if (puppet.is_some() && wpmat.use_puppet) {
                        auto effect_puppet_layer =
                            MakePuppetLayer((*(*puppet)->puppet).clone(), wpimgobj.puppet_layers);
                        RegisterPuppetLayer(
                            context, spEffNode.as_ptr(), rstd::move(effect_puppet_layer));
                    }
                }
                spMesh->AddMaterial(std::move(material));
                track_image_property_material(spMesh->MaterialSlots().back());
                Option<ref<wpscene::Material>> binding_fallback;
                if (user_texture_fallback.is_some()) {
                    binding_fallback = Some(ref<wpscene::Material>::from_raw_parts(
                        rstd::addressof(*user_texture_fallback)));
                }
                RegisterMaterialBindings(*context.scene,
                                         spMesh->MaterialSlots().front(),
                                         wpmat,
                                         wpEffShaderInfo,
                                         binding_fallback);
                RegisterLayerPreviousBindings(*context.scene,
                                              *spMesh->Material(),
                                              wpmat,
                                              image_node_id,
                                              as_str(effect_composite).unwrap());
                WireMaterialShaderValueScripts(
                    context, spImgNode, spMesh->MaterialSlots().front(), wpmat, wpEffShaderInfo);
                auto add_puppet_mask_materials = [&]() -> bool {
                    if (! (puppet.is_some() && wpmat.use_puppet && puppet_has_masks)) return true;
                    const std::string source_tex =
                        wpmat.textures.empty() ? std::string {} : wpmat.textures[0];
                    for (const auto& pmesh : (*puppet)->meshes) {
                        if (&pmesh != primary_puppet_mesh) continue;
                        for (const auto& mb : pmesh.masks) {
                            wpscene::Material mask_wpmat;
                            mask_wpmat.shader     = "clippingmaskimage4";
                            mask_wpmat.blending   = "translucent";
                            mask_wpmat.depthtest  = "disabled";
                            mask_wpmat.depthwrite = "disabled";
                            mask_wpmat.cullmode   = "nocull";
                            mask_wpmat.textures.resize(2);
                            mask_wpmat.textures[0] = source_tex;
                            mask_wpmat.textures[1] = rstd::cppstd::to_string(mb.mat_json.as_str());
                            MdlParser::AddPuppetMatInfo(mask_wpmat, **puppet);

                            SceneMaterial mask_material;
                            ShaderInfo    mask_shaderInfo;
                            mask_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
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
                                                          as_str(effect_composite).unwrap());
                            spMesh->AddMaterial(std::move(mask_material));
                            track_image_property_material(spMesh->MaterialSlots().back());

                            wpscene::Material clip_wpmat        = wpmat.clone();
                            clip_wpmat.combos["CLIPPINGTARGET"] = i32(1);
                            clip_wpmat.combos["CLIPPINGUVS"]    = i32(1);
                            if (clip_wpmat.textures.size() < 9) clip_wpmat.textures.resize(9);
                            clip_wpmat.textures[8] = std::string(PUPPET_MASK_RT);
                            MdlParser::AddPuppetMatInfo(clip_wpmat, **puppet);

                            SceneMaterial clip_material;
                            ShaderInfo    clip_shaderInfo;
                            clip_shaderInfo.baseConstSvs = wpEffShaderInfo.baseConstSvs;
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
                                                          as_str(effect_composite).unwrap());
                            spMesh->AddMaterial(std::move(clip_material));
                            track_image_property_material(spMesh->MaterialSlots().back());
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
                        mat->name, wpEffShaderInfo, allow_transparent_previous_final);
                }
                spEffNode->AddMesh(spMesh);

                SetUniformConfig(context, spEffNode, rstd::move(svData));
                imgEffect->nodes.push_back(SceneImageEffectNode {
                    .output                   = std::move(matOutRT),
                    .sceneNode                = spEffNode.clone(),
                    .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                    .final_quad_shader_values = std::move(final_quad_shader_values),
                });
            }

            if (eff_mat_ok)
                imgEffectLayer->AddEffect(imgEffect);
            else {
                rstd_error("effect \'{}\' failed to load", wpeffobj.name);
            }
        }

        auto make_internal_passthrough =
            [&](std::string_view input, SceneEffectTarget output, ref<str> name) {
                std::shared_ptr<SceneImageEffect> result;
                wpscene::Material                 passthrough_mat;
                auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
                if (! json || ! passthrough_mat.FromJson(*json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                    return result;
                }
                if (passthrough_mat.textures.empty())
                    passthrough_mat.textures.push_back(std::string(input));
                else
                    passthrough_mat.textures[0] = std::string(input);

                auto       node = Arc<SceneNode>::make();
                ShaderInfo shader_info;
                shader_info.baseConstSvs = NeutralColorUniforms(baseConstSvs);
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
                auto mesh = std::make_shared<SceneMesh>();
                mesh->AddMaterial(std::move(material));
                RegisterMaterialBindings(
                    *context.scene, mesh->MaterialSlots().front(), passthrough_mat, shader_info);
                RegisterLayerPreviousBindings(*context.scene,
                                              *mesh->Material(),
                                              passthrough_mat,
                                              image_node_id,
                                              as_str(effect_composite).unwrap());
                node->AddMesh(std::move(mesh));
                scene.RegisterNode(*node);
                SetUniformConfig(context, node, rstd::move(uniform_config));

                result       = std::make_shared<SceneImageEffect>();
                result->name = rstd::cppstd::to_string(name);
                result->nodes.push_back(SceneImageEffectNode {
                    .output    = std::move(output),
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
                                                     SceneEffectTarget::Named(link_output),
                                                     "linked_layer_publish"_str);
            auto visible = make_internal_passthrough(
                link_output,
                SceneEffectTarget::Named(rstd::cppstd::to_string(SpecTex_Default)),
                "linked_layer_visible_resolve"_str);
            if (publish && visible) {
                imgEffectLayer->SetPublishedEffect(rstd::move(publish));
                imgEffectLayer->SetVisibleResolveEffect(rstd::move(visible));
                (void)scene.SetNodeVisible(*spImgNode, wpimgobj.visible);
            }
        }

        if (! is_linked_source && ! wpimgobj.fullscreen && ! wpimgobj.copybackground &&
            ! passthrough_can_composite_final && ! last_effect_can_composite_final) {
            wpscene::Material passthrough_mat;
            auto json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
            if (! json) {
                rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
            } else {
                if (! passthrough_mat.FromJson(*json)) {
                    rstd_error("parse effectpassthrough.json failed for '{}'", wpimgobj.name);
                } else {
                    if (passthrough_mat.textures.empty())
                        passthrough_mat.textures.push_back(effect_composite);
                    else
                        passthrough_mat.textures[0] = effect_composite;

                    auto finalEffect = std::make_shared<SceneImageEffect>();
                    auto spFinalNode = Arc<SceneNode>::make();

                    ShaderInfo wpFinalShaderInfo;
                    wpFinalShaderInfo.baseConstSvs = NeutralColorUniforms(baseConstSvs);
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
                        auto spFinalMesh = std::make_shared<SceneMesh>();
                        spFinalMesh->AddMaterial(std::move(finalMaterial));
                        RegisterMaterialBindings(*context.scene,
                                                 spFinalMesh->MaterialSlots().front(),
                                                 passthrough_mat,
                                                 wpFinalShaderInfo);
                        RegisterLayerPreviousBindings(*context.scene,
                                                      *spFinalMesh->Material(),
                                                      passthrough_mat,
                                                      image_node_id,
                                                      as_str(effect_composite).unwrap());
                        spFinalNode->AddMesh(spFinalMesh);
                        SetUniformConfig(context, spFinalNode, rstd::move(finalSvData));
                        finalEffect->nodes.push_back(SceneImageEffectNode {
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
    RegisterImageAlignmentBinding(
        context,
        spImgNode.as_ptr(),
        rstd::cppstd::as_str(wpimgobj.alignment).unwrap(),
        SceneParseContext::ImageAlignmentSetter::make(
            [image_effect_layer, alignment_base_transform, alignment_offset, geometry_size](
                SceneNode* node, ref<str> alignment) {
                const Vector3f delta =
                    AlignmentOffset(alignment, { geometry_size[0], geometry_size[1] }) -
                    alignment_offset;
                Matrix4d transform = alignment_base_transform *
                                     Affine3d(Translation3d(delta.cast<double>())).matrix();
                if (image_effect_layer)
                    image_effect_layer->FinalMesh().SetGeometryTransform(rstd::move(transform));
                else if (node)
                    node->SetGeometryTransform(rstd::move(transform));
            }));

    AssignNodeFieldAnimations(context, *spImgNode.as_ptr(), wpimgobj.field_bindings);
    WireFieldScripts(context, spImgNode, wpimgobj.field_bindings);
    if (! wpimgobj.color_user_key.empty()) {
        context.scene->RegisterImageColorUserBinding(
            String::make(as_str(wpimgobj.color_user_key).unwrap()),
            spImgNode,
            image_property_materials.as_slice());
    }
    if (! wpimgobj.alpha_user_key.empty()) {
        context.scene->RegisterImageAlphaUserBinding(
            String::make(as_str(wpimgobj.alpha_user_key).unwrap()),
            spImgNode,
            image_property_materials.as_slice());
    }
    RegisterNodeRef(context,
                    wpimgobj.id,
                    SceneParseContext::NodeRef {
                        wpimgobj.parent,
                        Some(spImgNode.clone()),
                        (puppet.is_some() && (*puppet)->puppet.is_some())
                            ? Some((*(*puppet)->puppet).clone())
                            : None(),
                        String::make(rstd::cppstd::as_str(wpimgobj.attachment).unwrap()),
                        image_puppet_layer.is_some() ? Some((*image_puppet_layer).clone()) : None(),
                    });
}

void ParseShapeObj(SceneParseContext& context, wpscene::ShapeObject& shape_obj) {
    PrepareAnimationBindings(context, shape_obj);
    if (shape_obj.shape != "quad") {
        rstd_error("unsupported shape '{}' for '{}'", shape_obj.shape, shape_obj.name);
        return;
    }

    const wpscene::ImageEffect* first_effect { nullptr };
    const wpscene::ImageEffect* last_effect { nullptr };
    for (const auto& effect : shape_obj.effects) {
        if (! effect.visible && ! effect.visible_can_change()) continue;
        if (first_effect == nullptr) first_effect = &effect;
        last_effect = &effect;
    }
    if (first_effect == nullptr || first_effect->materials.empty() ||
        first_effect->passes.empty() || last_effect == nullptr || last_effect->materials.empty() ||
        last_effect->passes.empty()) {
        rstd_error("shape '{}' has no renderable effect", shape_obj.name);
        return;
    }
    auto direct_draw_material = first_effect->materials.front().clone();
    direct_draw_material.MergePass(first_effect->passes.front());
    auto direct_draw = direct_draw_material.combos.find("DIRECTDRAW");
    if (direct_draw == direct_draw_material.combos.end() || direct_draw->second == i32()) {
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
    image.material = last_effect->materials.back().clone();
    image.material.MergePass(last_effect->passes.back());
    image.material.blending  = "additive";
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
