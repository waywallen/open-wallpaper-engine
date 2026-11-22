module;
#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import rstd;
import rstd.log;
import wescene.core;
import wescene.pkg.spec_names;
import wescene.scene;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace owe
{
namespace
{
void MakeLightSphere(SceneMesh& mesh) {
    constexpr unsigned  segments = 24;
    Vec<float>          positions;
    Vec<rstd::uint32_t> indices;
    for (unsigned latitude = 0; latitude <= segments; ++latitude) {
        const auto theta = f32::consts::PI * f32(static_cast<float>(latitude) / segments);
        for (unsigned longitude = 0; longitude <= segments; ++longitude) {
            const auto phi = f32::consts::TAU * f32(static_cast<float>(longitude) / segments);
            positions.push((theta.sin() * phi.cos()).to_primitive());
            positions.push(theta.cos().to_primitive());
            positions.push((theta.sin() * phi.sin()).to_primitive());
        }
    }
    for (unsigned latitude = 0; latitude < segments; ++latitude) {
        for (unsigned longitude = 0; longitude < segments; ++longitude) {
            const auto a = latitude * (segments + 1) + longitude;
            const auto b = a + 1;
            const auto c = a + segments + 1;
            const auto d = c + 1;
            if (latitude != 0)
                indices.extend_from_slice(array<rstd::uint32_t, 3> { a, b, c }.as_slice());
            if (latitude + 1 != segments)
                indices.extend_from_slice(array<rstd::uint32_t, 3> { b, d, c }.as_slice());
        }
    }
    SceneVertexArray vertices(MakeAttrSet({ VAttr::Position }),
                              usize((segments + 1) * (segments + 1)));
    vertices.SetVertex(WE_IN_POSITION, positions.as_slice());
    mesh.SetPrimitive(MeshPrimitive::TRIANGLE);
    mesh.AddVertexArray(rstd::move(vertices));
    mesh.AddIndexArray(SceneIndexArray(indices.as_slice()));
}

void MakeLightFrustum(SceneMesh& mesh) {
    constexpr usize segments { 32 };
    Vec<float>      positions;
    positions.extend_from_slice(array<float, 6> { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }.as_slice());
    for (usize index {}; index < segments; ++index) {
        const auto angle = f32::consts::TAU * f32(static_cast<float>(index.to_primitive()) / 32.0f);
        positions.extend_from_slice(array<float, 6> { angle.cos().to_primitive(),
                                                      angle.sin().to_primitive(),
                                                      0.0f,
                                                      angle.cos().to_primitive(),
                                                      angle.sin().to_primitive(),
                                                      1.0f }
                                        .as_slice());
    }
    Vec<rstd::uint32_t> indices;
    for (usize index {}; index < segments; ++index) {
        const auto a = static_cast<rstd::uint32_t>((index * usize(2) + usize(2)).to_primitive());
        const auto b = static_cast<rstd::uint32_t>(
            (((index + usize(1)) % segments) * usize(2) + usize(2)).to_primitive());
        indices.extend_from_slice(array<rstd::uint32_t, 12> {
            0u, b, a, 1u, a + 1u, b + 1u, a, b, a + 1u, b, b + 1u, a + 1u }
                                      .as_slice());
    }
    SceneVertexArray vertices(MakeAttrSet({ VAttr::Position }), segments * usize(2) + usize(2));
    vertices.SetVertex(WE_IN_POSITION, positions.as_slice());
    mesh.SetPrimitive(MeshPrimitive::TRIANGLE);
    mesh.AddVertexArray(rstd::move(vertices));
    mesh.AddIndexArray(SceneIndexArray(indices.as_slice()));
}
} // namespace

void BuildVolumetricPostProcess(SceneParseContext& context, fs::VFS& vfs) {
    auto&               scene = *context.scene;
    Vec<Arc<SceneNode>> light_nodes;
    for (const auto& light : scene.Lights()) {
        const auto& desc = light->desc();
        if (! desc.cast_volumetrics) continue;
        if ((desc.type != SceneLightType::Point && desc.type != SceneLightType::Spot) ||
            (desc.cast_shadow &&
             (desc.type != SceneLightType::Spot || ! light->node() ||
              ! context.layer_resources->shadow_layout->Find(light->node()->ID())))) {
            rstd_warn("volumetrics: light '{}' has no supported shadow projection",
                      light->node() ? light->node()->Name() : ""_str);
            continue;
        }
        if (! f32(desc.radius).is_finite() || desc.radius <= 0.0f ||
            ! f32(desc.volumetric_density).is_finite() || desc.volumetric_density <= 0.0f)
            continue;
        for (const auto& [id, draft] : context.layer_build.node_id_map.iter()) {
            if (draft->node && draft->node->as_ptr().as_raw_ptr() == light->node()) {
                light_nodes.push((*draft->node).clone());
                break;
            }
        }
    }
    if (light_nodes.is_empty()) return;
    auto target = scene.RenderTarget(SpecTex_Default);
    if (! target || ! (**target).withDepth) return;
    const auto camera  = scene.ActiveCameraHandle();
    const bool reverse = camera && (*camera)->NearClip() > (*camera)->FarClip();

    auto declare_rt = [&](ref<str> name, bool depth, bool clear) {
        SceneRenderTarget rt;
        rt.width       = i32(2);
        rt.height      = i32(2);
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = 0.125f;
        rt.allowReuse  = true;
        rt.kind        = depth ? SceneRenderTargetKind::DepthSampled : SceneRenderTargetKind::Color;
        rt.force_clear = depth;
        rt.depth_clear_value    = reverse ? 0.0f : 1.0f;
        rt.clear_on_first_write = clear;
        if (depth)
            rt.sample = { TextureWrap::CLAMP_TO_EDGE,
                          TextureWrap::CLAMP_TO_EDGE,
                          TextureFilter::NEAREST,
                          TextureFilter::NEAREST };
        scene.RegisterRenderTarget(String::make(name), rstd::move(rt));
    };
    declare_rt("_rt_volumetricsSingle"_str, true, false);
    declare_rt("_rt_volumetricsLightBuffer"_str, false, true);
    declare_rt("_rt_volumetricsLightBufferB"_str, false, false);

    auto pp                        = Box<ScenePostProcess>::make();
    pp->name                       = "__volumetrics"_Str;
    pp->single_sample_depth_target = Some(String::make(SpecTex_Default));
    auto sphere                    = Arc<SceneMesh>::make();
    MakeLightSphere(*sphere);
    auto frustum = Arc<SceneMesh>::make();
    MakeLightFrustum(*frustum);

    auto add_pass = [&](ref<str> material_file,
                        ref<str>
                            output,
                        Option<VolumetricDrawKind>
                                                      kind,
                        const Option<Arc<SceneNode>>& light_node) {
        auto loaded = ReadJsonFile(
            vfs, fs::Path(rstd::format("/assets/materials/util/{}", material_file).as_str()));
        wpscene::Material wpmat;
        if (! loaded || ! wpmat.FromJson(*loaded)) return false;
        const SceneLight* light = nullptr;
        if (kind)
            for (const auto& candidate : scene.Lights())
                if (candidate->node() == (*light_node).as_ptr().as_raw_ptr())
                    light = candidate.as_ptr().as_raw_ptr();
        const bool spot   = light && light->type() == SceneLightType::Spot;
        const bool shadow = light && light->desc().cast_shadow;
        if (kind) {
            (void)wpmat.combos.insert("POINTLIGHT"_Str, i32(spot ? 0 : 1));
            (void)wpmat.combos.insert("SHADOW"_Str, i32(shadow ? 1 : 0));
            (void)wpmat.combos.insert("REVERSEDEPTH"_Str, i32(reverse ? 1 : 0));
            (void)wpmat.combos.insert("QUALITY"_Str, i32(2));
            if (*kind == VolumetricDrawKind::Back) {
                wpmat.depthtest  = "enabled"_Str;
                wpmat.depthwrite = "enabled"_Str;
                wpmat.cullmode   = "front"_Str;
            } else {
                wpmat.textures.resize(usize(4), String {});
                if (shadow) wpmat.textures[usize()] = String::make(WE_SHADOW_ATLAS_PREFIX);
                wpmat.textures[usize(1)] = String::make(SpecTex_Default);
                wpmat.textures[usize(3)] = "_rt_volumetricsSingle"_Str;
            }
        }
        if (output == SpecTex_Default) wpmat.alphawriting = "disabled"_Str;
        ShaderInfo info;
        info.baseConstSvs = context.layer_resources->global_base_uniforms.clone();
        auto built        = BuildMaterial(vfs,
                                          *context.layer_resources->shader_cache,
                                          context.layer_resources->shader_environment,
                                          wpmat,
                                          scene,
                                          rstd::move(info),
                                          GeometryStageRequirement::None,
                                          context.load_bench);
        if (! built) {
            rstd_error("volumetrics: {}: {}", material_file, built.unwrap_err().message);
            return false;
        }
        auto result = rstd::move(built).unwrap();
        if (kind && *kind == VolumetricDrawKind::Back && reverse) {
            auto pipeline          = result.material.Pipeline();
            pipeline.depth_compare = CompareOp::Greater;
            result.material.SetPipeline(rstd::move(pipeline));
        }
        LoadConstvalue(context, result.material, wpmat, result.shader_info);
        if (kind && *kind != VolumetricDrawKind::Back &&
            ! scene.SetMaterialDepthAttachmentSource(result.material, u32(1), SpecTex_Default))
            return false;
        auto mesh = Arc<SceneMesh>::make();
        mesh->ChangeMeshDataFrom(kind && *kind != VolumetricDrawKind::Fullscreen
                                     ? (spot ? *frustum : *sphere)
                                     : *scene.DefaultEffectMesh());
        mesh->AddMaterial(rstd::move(result.material));
        RegisterMaterialBindings(
            scene, mesh->MaterialSlots().first_mut().unwrap().get_mut(), wpmat, result.shader_info);
        auto node = Arc<SceneNode>::make();
        node->SetCamera("effect"_str);
        node->AddMesh(mesh.clone());
        if (kind)
            context.layer_build.volumetric_uniform_configs.push(
                { node.clone(), (*light_node).clone(), *kind });
        else
            SetUniformConfig(context, node, {});
        pp->steps.push(ScenePostProcessStep::Pass(
            ScenePostProcessPass { node.clone(), String::make(output) }));
        return true;
    };
    for (const auto& node : light_nodes) {
        const auto light_node = Some(node.clone());
        if (! add_pass("volumetrics_back.json"_str,
                       "_rt_volumetricsSingle"_str,
                       Some(VolumetricDrawKind::Back),
                       light_node) ||
            ! add_pass("volumetrics_front.json"_str,
                       "_rt_volumetricsLightBuffer"_str,
                       Some(VolumetricDrawKind::Front),
                       light_node) ||
            ! add_pass("volumetrics_fullscreen.json"_str,
                       "_rt_volumetricsLightBuffer"_str,
                       Some(VolumetricDrawKind::Fullscreen),
                       light_node))
            return;
    }
    if (! add_pass(
            "volumetrics_blur_h.json"_str, "_rt_volumetricsLightBufferB"_str, None(), None()) ||
        ! add_pass(
            "volumetrics_blur_v.json"_str, "_rt_volumetricsLightBuffer"_str, None(), None()) ||
        ! add_pass("volumetrics_combine.json"_str, SpecTex_Default, None(), None()))
        return;
    (void)scene.RegisterPostProcess(rstd::move(pp));
}
} // namespace owe
