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
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

void BuildBloomPostProcess(SceneParseContext& context, fs::VFS& vfs,
                           const wpscene::SceneGeneral& g) {
    auto& scene = *context.scene;

    auto declare_rt = [&](std::string name, float inv_scale) {
        SceneRenderTarget rt {};
        rt.width       = i32(2);
        rt.height      = i32(2);
        rt.allowReuse  = true;
        rt.bind.enable = true;
        rt.bind.screen = true;
        rt.bind.scale  = inv_scale;
        scene.RegisterRenderTarget(String::make(as_str(name).unwrap()), rstd::move(rt));
    };
    declare_rt("_rt_bloom_mip1", g.hdr ? 0.5f : 0.25f);
    declare_rt("_rt_bloom_mip2", 0.25f);
    declare_rt("_rt_bloom_combine", 1.0f);

    EffectRenderTargets render_targets;
    auto                default_target = String::make(SpecTex_Default);
    (void)render_targets.insert(String::make("previous"_str), default_target.clone());
    (void)render_targets.insert(String::make("_rt_default"_str), rstd::move(default_target));
    for (auto name : array<ref<str>, 3> {
             "_rt_bloom_mip1"_str, "_rt_bloom_mip2"_str, "_rt_bloom_combine"_str }) {
        (void)render_targets.insert(String::make(name), String::make(name));
    }

    auto pp  = Box<ScenePostProcess>::make();
    pp->name = "__bloom";

    auto add_pass = [&](std::string_view mat_relpath,
                        std::vector<wpscene::MaterialPassBindItem>
                                                                binds,
                        std::string                             output_rt,
                        std::function<void(wpscene::Material&)> mutate         = nullptr,
                        std::function<void(ShaderInfo&)>        configure_info = nullptr) -> bool {
        std::string material_path { "/assets/" };
        material_path.append(mat_relpath);
        auto loaded = ReadJsonFile(vfs, material_path);
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
        ApplyTextureBinds(wpmat, std::span(binds), render_targets);
        if (mutate) mutate(wpmat);

        ShaderInfo wpShaderInfo;
        wpShaderInfo.baseConstSvs = context.global_base_uniforms;
        if (configure_info) configure_info(wpShaderInfo);

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

        auto pp_mesh = std::make_shared<SceneMesh>();
        pp_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
        pp_mesh->AddMaterial(std::move(material));
        RegisterMaterialBindings(scene, pp_mesh->MaterialSlots().front(), wpmat, wpShaderInfo);
        pp_node->AddMesh(pp_mesh);

        // Camera name drives CustomShaderPass color-write mask: empty or
        // "global" cameras strip the A bit (intent: swapchain ignores A
        // for direct local display). But waywallen DMA-BUF forwarding
        // negotiates COLOR_ALPHA_PREMUL; if A=0 reaches the consumer with
        // non-zero RGB, KWin reads it as premultiplied-transparent and
        // composites additively against the desktop -> washed-out tint.
        // Anchor to the existing "effect" cam (2x2 ortho, identity for
        // our NDC fullscreen quads) so A=1.0 from the shader survives.
        pp_node->SetCamera("effect");
        SetUniformConfig(context, pp_node, rstd::move(svData));

        pp->steps.push(ScenePostProcessStep::Pass(ScenePostProcessPass {
            .node   = rstd::move(pp_node),
            .output = std::move(output_rt),
        }));
        return true;
    };

    if (g.hdr) {
        auto hdr_offsets = [](float source_scale) {
            float x = 1.0f / (1920.0f * source_scale);
            float y = 1.0f / (1080.0f * source_scale);
            return std::array { x, y, -x, -y };
        };
        auto set_render_var = [](ShaderInfo& info, std::array<float, 4> value) {
            info.baseConstSvs[rstd::cppstd::to_string(G_RENDERVAR0)] = value;
        };
        float threshold = g.bloomhdrthreshold;
        float knee      = threshold * g.bloomhdrfeather;
        float scatter   = g.bloomhdrscatter > 0.0f ? g.bloomhdrscatter : 1.0f;

        if (! add_pass(
                "materials/util/hdr_downsample_bloom.json",
                { { "previous", i32() } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["bloomstrength"] = { g.bloomhdrstrength };
                    m.constantshadervalues["blend"]         = {
                        threshold,
                        threshold - knee,
                        2.0f * knee,
                        knee > 0.0f ? 0.25f / knee : 0.0f,
                    };
                    m.constantshadervalues["bloomtint"] = {
                        g.bloomtint[0],
                        g.bloomtint[1],
                        g.bloomtint[2],
                    };
                },
                [&](ShaderInfo& info) {
                    set_render_var(info, hdr_offsets(1.0f));
                }))
            return;

        if (! add_pass("materials/util/hdr_downsample.json",
                       { { "_rt_bloom_mip1", i32() } },
                       "_rt_bloom_mip2",
                       nullptr,
                       [&](ShaderInfo& info) {
                           set_render_var(info, hdr_offsets(0.5f));
                       }))
            return;

        if (! add_pass(
                "materials/util/hdr_upsample.json",
                { { "_rt_bloom_mip2", i32() } },
                "_rt_bloom_mip1",
                [&](wpscene::Material& m) {
                    m.constantshadervalues["scatter"] = { scatter };
                },
                [&](ShaderInfo& info) {
                    set_render_var(info, hdr_offsets(0.25f));
                }))
            return;

        if (! add_pass("materials/util/combine_hdr_upsample_linear.json",
                       { { "previous", i32() }, { "_rt_bloom_mip1", i32(1) } },
                       "_rt_bloom_combine",
                       nullptr,
                       [&](ShaderInfo& info) {
                           set_render_var(info, { 1.0f, 0.0f, 0.0f, 0.0f });
                       }))
            return;
    } else {
        if (! add_pass("materials/util/downsample_quarter_bloom.json",
                       { { "previous", i32() } },
                       "_rt_bloom_mip1",
                       [&](wpscene::Material& m) {
                           m.constantshadervalues["bloomstrength"]  = { g.bloomstrength };
                           m.constantshadervalues["bloomthreshold"] = { g.bloomthreshold };
                           m.constantshadervalues["bloomtint"]      = {
                               g.bloomtint[0],
                               g.bloomtint[1],
                               g.bloomtint[2],
                           };
                       }))
            return;

        if (! add_pass("materials/util/downsample_eighth_blur_v.json",
                       { { "_rt_bloom_mip1", i32() } },
                       "_rt_bloom_mip2"))
            return;

        if (! add_pass("materials/util/blur_h_bloom.json",
                       { { "_rt_bloom_mip2", i32() } },
                       "_rt_bloom_mip1"))
            return;

        if (! add_pass("materials/util/combine_ldr.json",
                       { { "previous", i32() }, { "_rt_bloom_mip1", i32(1) } },
                       "_rt_bloom_combine"))
            return;
    }

    pp->steps.push(ScenePostProcessStep::Copy(ScenePostProcessCopy {
        .src = "_rt_bloom_combine",
        .dst = rstd::cppstd::to_string(SpecTex_Default),
    }));

    (void)scene.RegisterPostProcess(rstd::move(pp));
}

} // namespace owe
