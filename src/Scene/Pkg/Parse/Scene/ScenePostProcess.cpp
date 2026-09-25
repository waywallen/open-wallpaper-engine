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

using namespace rstd::prelude;
using namespace rstd::literals;
using owe::wpscene::MaterialPassBindItem;
using rstd::sync::Arc;

namespace owe
{

void BuildBloomPostProcess(SceneParseContext& context, fs::VFS& vfs,
                           const wpscene::SceneGeneral& g) {
    auto& scene = *context.scene;

    auto declare_rt = [&](ref<str> name, float inv_scale) {
        SceneRenderTarget rt {};
        rt.width       = i32(2);
        rt.height      = i32(2);
        rt.allowReuse  = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = inv_scale;
        scene.RegisterRenderTarget(rstd::into(name), rstd::move(rt));
    };
    declare_rt("_rt_bloom_mip1"_str, 0.25f);
    declare_rt("_rt_bloom_mip2"_str, 0.125f);
    declare_rt("_rt_bloom_mip3"_str, 0.125f);
    declare_rt("_rt_bloom_combine"_str, 1.0f);

    EffectRenderTargets render_targets;
    auto                default_target = String::make(SpecTex_Default);
    (void)render_targets.insert("previous"_Str, default_target.clone());
    (void)render_targets.insert("_rt_default"_Str, rstd::move(default_target));
    for (auto name : array<ref<str>, 4> { "_rt_bloom_mip1"_str,
                                          "_rt_bloom_mip2"_str,
                                          "_rt_bloom_mip3"_str,
                                          "_rt_bloom_combine"_str }) {
        (void)render_targets.insert(String::make(name), String::make(name));
    }

    auto pp  = Box<ScenePostProcess>::make();
    pp->name = "__bloom"_Str;

    auto add_pass = [&](ref<str> mat_relpath,
                        slice<MaterialPassBindItem>
                            binds,
                        ref<str>
                                                                          output_rt,
                        Option<Box<dyn<FnMut<void(wpscene::Material&)>>>> mutate = {}) -> bool {
        auto material_path = rstd::format("/assets/{}", mat_relpath);
        auto loaded        = ReadJsonFile(vfs, fs::Path(material_path.as_str()));
        if (loaded.is_err()) {
            rstd_error("bloom: parse material json failed {}", mat_relpath);
            return false;
        }
        auto              material_json = rstd::move(loaded).unwrap_unchecked();
        wpscene::Material wpmat;
        if (! wpmat.FromJson(material_json)) {
            rstd_error("bloom: Material::FromJson failed: {}", mat_relpath);
            return false;
        }
        ApplyTextureBinds(wpmat, binds, render_targets);
        if (mutate.is_some()) (**mutate)(wpmat);

        ShaderInfo wpShaderInfo;
        wpShaderInfo.baseConstSvs = context.global_base_uniforms.clone();

        auto                   pp_node = Arc<SceneNode>::make();
        SceneMaterial          material;
        UniformNodeConfigDraft svData;
        auto                   material_result = BuildMaterial(vfs,
                                                               *context.shader_cache,
                                                               context.shader_environment,
                                                               wpmat,
                                                               scene,
                                                               rstd::move(wpShaderInfo));
        if (material_result.is_err()) {
            rstd_error("bloom: BuildMaterial failed: {}", mat_relpath);
            return false;
        }
        auto material_build = rstd::move(material_result).unwrap_unchecked();
        material            = rstd::move(material_build.material);
        wpShaderInfo        = rstd::move(material_build.shader_info);
        LoadConstvalue(context, material, wpmat, wpShaderInfo);

        auto pp_mesh = Arc<SceneMesh>::make();
        pp_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
        pp_mesh->AddMaterial(rstd::move(material));
        RegisterMaterialBindings(
            scene, pp_mesh->MaterialSlots().first_mut().unwrap().get_mut(), wpmat, wpShaderInfo);
        pp_node->AddMesh(pp_mesh.clone());

        // Camera name drives CustomShaderPass color-write mask: empty or
        // "global" cameras strip the A bit (intent: swapchain ignores A
        // for direct local display). But waywallen DMA-BUF forwarding
        // negotiates COLOR_ALPHA_PREMUL; if A=0 reaches the consumer with
        // non-zero RGB, KWin reads it as premultiplied-transparent and
        // composites additively against the desktop -> washed-out tint.
        // Anchor to the existing "effect" cam (2x2 ortho, identity for
        // our NDC fullscreen quads) so A=1.0 from the shader survives.
        pp_node->SetCamera("effect"_str);
        SetUniformConfig(context, pp_node, rstd::move(svData));

        pp->steps.push(ScenePostProcessStep::Pass(ScenePostProcessPass {
            .node   = rstd::move(pp_node),
            .output = rstd::into(output_rt),
        }));
        return true;
    };

    // Scene HDR is authoring metadata, not an output capability. Wallpaper Engine uses
    // this LDR chain for SDR output even when the scene has HDR authoring enabled.
    if (! add_pass("materials/util/downsample_quarter_bloom.json"_str,
                   array<MaterialPassBindItem, 1> { MaterialPassBindItem { "previous"_Str, i32() } }
                       .as_slice(),
                   "_rt_bloom_mip1"_str,
                   Some(Box<dyn<FnMut<void(wpscene::Material&)>>>::make([&](wpscene::Material& m) {
                       (void)m.constantshadervalues.insert(
                           "bloomstrength"_Str,
                           Vec<float>::from(array<float, 1> { g.bloomstrength }.as_slice()));
                       (void)m.constantshadervalues.insert(
                           "bloomthreshold"_Str,
                           Vec<float>::from(array<float, 1> { g.bloomthreshold }.as_slice()));
                       (void)m.constantshadervalues.insert("bloomtint"_Str,
                                                           Vec<float>::from(array<float, 3> {
                                                               g.bloomtint[usize(0)],
                                                               g.bloomtint[usize(1)],
                                                               g.bloomtint[usize(2)],
                                                           }
                                                                                .as_slice()));
                   }))))
        return;

    if (! add_pass(
            "materials/util/downsample_eighth_blur_v.json"_str,
            array<MaterialPassBindItem, 1> { MaterialPassBindItem { "_rt_bloom_mip1"_Str, i32() } }
                .as_slice(),
            "_rt_bloom_mip2"_str))
        return;

    if (! add_pass(
            "materials/util/blur_h_bloom.json"_str,
            array<MaterialPassBindItem, 1> { MaterialPassBindItem { "_rt_bloom_mip2"_Str, i32() } }
                .as_slice(),
            "_rt_bloom_mip3"_str))
        return;

    if (! add_pass(
            "materials/util/combine_ldr.json"_str,
            array<MaterialPassBindItem, 2> { MaterialPassBindItem { "previous"_Str, i32() },
                                             MaterialPassBindItem { "_rt_bloom_mip3"_Str, i32(1) } }
                .as_slice(),
            "_rt_bloom_combine"_str))
        return;

    pp->steps.push(ScenePostProcessStep::Copy(ScenePostProcessCopy {
        .src = "_rt_bloom_combine"_Str,
        .dst = rstd::into(SpecTex_Default),
    }));

    (void)scene.RegisterPostProcess(rstd::move(pp));
}

} // namespace owe
