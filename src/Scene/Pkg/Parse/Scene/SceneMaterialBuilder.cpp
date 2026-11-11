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
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{
BlendMode ParseBlendMode(ref<str> str) {
    BlendMode bm;
    if (str == "translucent"_str) {
        bm = BlendMode::Translucent;
    } else if (str == "additive"_str) {
        bm = BlendMode::Additive;
    } else if (str == "alphatocoverage"_str) {
        bm = BlendMode::AlphaToCoverage;
    } else if (str == "normal"_str) {
        bm = BlendMode::Normal;
    } else if (str == "disabled"_str) {
        bm = BlendMode::Disable;
    } else {
        bm = BlendMode::Normal;
        rstd_error("unknown blending: {}", str);
    }
    return bm;
}

Option<BlendMode> ApplyLayerColorBlend(wpscene::Material& material, i32 color_blend_mode) {
    if (color_blend_mode == i32()) return None<BlendMode>();

    if (color_blend_mode == i32(31)) {
        (void)material.combos.remove(WE_CB_BLENDMODE);
        material.blending = "additive"_Str;
        return Some(BlendMode::Additive);
    }
    (void)material.combos.insert(rstd::into(WE_CB_BLENDMODE), color_blend_mode);
    return None<BlendMode>();
}

ShaderValueMap NeutralColorUniforms(ShaderValueMap values) {
    (void)values.insert(rstd::into(G_COLOR4),
                        ShaderValue(array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f }));
    (void)values.insert(rstd::into(G_COLOR), ShaderValue(array<float, 3> { 1.0f, 1.0f, 1.0f }));
    (void)values.insert(rstd::into(G_ALPHA), ShaderValue(1.0f));
    (void)values.insert(rstd::into(G_USERALPHA), ShaderValue(1.0f));
    (void)values.insert(rstd::into(G_BRIGHTNESS), ShaderValue(1.0f));
    return values;
}

bool ParseEnabled(ref<str> str) { return str == "enabled"_str; }

Option<bool> ParseAlphaWrite(ref<str> str) {
    if (str == "enabled"_str) return Some(true);
    if (str == "disabled"_str) return Some(false);
    if (str == "default"_str || str.is_empty()) return None<bool>();
    rstd_error("unknown alphawriting: {}", str);
    return None<bool>();
}

CullMode ParseCullMode(ref<str> str) {
    if (str == "back"_str || str == "normal"_str) return CullMode::Back;
    if (str == "front"_str) return CullMode::Front;
    if (str == "nocull"_str || str == "none"_str || str.is_empty()) return CullMode::None;
    rstd_error("unknown cullmode: {}", str);
    return CullMode::None;
}

void ParseSpecTexName(String& name, const wpscene::Material& wpmat, const ShaderInfo& sinfo,
                      Scene& scene) {
    auto text = name.as_str();
    if (IsSpecTex(text)) {
        if (text == WE_FULL_FRAME_BUFFER) {
            name = rstd::into(SpecTex_Default);
            if (wpmat.shader == "genericimage2"_str && ! sinfo.combos.contains_key(WE_CB_BLENDMODE))
                name.clear();
            /*
            if(wpmat.shader == "genericparticle") {
                name = "_rt_ParticleRefract";
            }
            */
        } else if (auto wpid = ParseImageLayerCompositeId(text)) {
            rstd_info("link tex \"{}\"", name);
            name = GenLinkTex(rstd::as_cast<isize>(*wpid));
        } else if (text.starts_with(WE_MIP_MAPPED_FRAME_BUFFER)) {
        } else if (text.starts_with(WE_SHADOW_ATLAS_PREFIX)) {
            if (scene.RenderTarget(WE_SHADOW_ATLAS_PREFIX).is_none()) name.clear();
        } else if (text.starts_with(OWE_BLOOM_MIP_PREFIX)) {
        } else if (text.starts_with(WE_REFLECTION_PREFIX)) {
            name = rstd::into(WE_REFLECTION_PREFIX);
            scene.EnablePlanarReflection();
        } else if (text.starts_with(OWE_EFFECT_PPONG_PREFIX)) {
        } else if (text.starts_with(WE_HALF_COMPO_BUFFER_PREFIX)) {
        } else if (text.starts_with(WE_QUARTER_COMPO_BUFFER_PREFIX)) {
        } else if (text.starts_with(WE_FULL_COMPO_BUFFER_PREFIX)) {
        } else if (text.starts_with(WE_EIGHT_COMPO_BUFFER_PREFIX)) {
        } else if (text.starts_with(WE_VOLUMETRICS_PREFIX) ||
                   text.starts_with(WE_QUARTER_FORCE_RG_PREFIX) ||
                   text.starts_with(WE_BLOOM_PREFIX) ||
                   text.starts_with(WE_QUARTER_FRAME_BUFFER_PREFIX) ||
                   text.starts_with(WE_EIGHTH_FRAME_BUFFER_PREFIX)) {
            name.clear();
        } else if (scene.RenderTarget(name.as_str()).is_some()) {
            // an effect-local fbo registered with a non-conventional name
            // (e.g. WE DOF's `_rt__coc_<addr>`) — already a valid RT.
        } else {
            rstd_warn("ignoring unsupported special tex \"{}\"", name);
            name.clear();
        }
    }
}

SceneShaderTextureCompileInfo ToSceneShaderTextureCompileInfo(const ShaderTexInfo& info) {
    return SceneShaderTextureCompileInfo {
        .enabled    = info.enabled,
        .components = info.composEnabled,
    };
}

bool IsLegacyAtmosphereMaterial(const wpscene::Material& material) {
    return material.shader == "workshop/2839476907/effects/atmosphere"_str;
}

void ApplyLegacyAtmosphereLightCombo(const wpscene::Material& material, ShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    if (! info.combos.contains_key("LIGHT_INDEX"_str) ||
        material.combos.contains_key("LIGHT_INDEX"_str))
        return;
    if (! material.combos.contains_key("LIGHT1"_str)) return;

    (void)info.combos.insert("LIGHT_INDEX"_Str, "4"_Str);
}

void ApplySceneFogCombos(const SceneShaderEnvironment& environment, ShaderInfo& info) {
    auto fog = info.combos.get("FOG"_str);
    if (fog.is_none() || (**fog).as_str() == "0"_str) return;

    if (environment.fog_distance) (void)info.combos.insert("FOG_DIST"_Str, "1"_Str);
    if (environment.fog_height) (void)info.combos.insert("FOG_HEIGHT"_Str, "1"_Str);
    if (environment.fog_distance || environment.fog_height)
        (void)info.combos.insert("FOG_COMPUTED"_Str, "1"_Str);
}

void ApplyLegacyAtmosphereUniformAliases(const wpscene::Material& material, ShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    (void)info.baseConstSvs.insert(rstd::into(G_VIEWFORWARD),
                                   ShaderValue(array<float, 3> { 0.0f, 0.0f, 1.0f }));

    auto prefer_legacy = [&](ref<str> legacy, ref<str> current) {
        if (! material.constantshadervalues.contains_key(legacy)) return;
        auto value = info.alias.remove(current);
        if (value.is_none()) return;
        (void)info.alias.insert(rstd::into(legacy), rstd::move(value).unwrap());
    };

    prefer_legacy("Planet position"_str, "Position"_str);
    prefer_legacy("Planet radius"_str, "Planet size"_str);
    prefer_legacy("Atmosphere radius"_str, "Atmosphere size"_str);
    prefer_legacy("Thickness"_str, "Density falloff"_str);
    prefer_legacy("Color"_str, "Light color"_str);
    prefer_legacy("Intensity"_str, "Brightness"_str);
}

void ReplaceAllInPlace(String& body, ref<str> needle, ref<str> repl) {
    usize pos {};
    while (pos <= body.len()) {
        auto found = body.as_str().get(pos, body.len()).unwrap().find(needle);
        if (found.is_none()) break;
        pos += *found;
        body.replace_range(pos, pos + needle.len(), repl);
        pos += repl.len();
    }
}

void ApplyLegacyAtmosphereShaderCompat(const wpscene::Material& material, Vec<ShaderUnit>& units) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    for (auto& unit : units) {
        if (unit.stage != ShaderType::FRAGMENT) continue;
        auto& source = unit.src;
        ReplaceAllInPlace(source,
                          "float pointDensity, opticalDepth;"_str,
                          "float pointDensity = 0.0, opticalDepth = 0.0;"_str);
        ReplaceAllInPlace(source,
                          "float localDensity, cameraOpticalDepth, sunRayLength, "
                          "sunOpticalDepth, lightInstensity = 1.0;"_str,
                          "float localDensity = 0.0, cameraOpticalDepth = 0.0, "
                          "sunRayLength = 0.0, sunOpticalDepth = 0.0, lightInstensity = 1.0;"_str);
    }
}

bool IsLegacyAtmosphereShadowValue(const wpscene::Material& material, ref<str> name) {
    if (! IsLegacyAtmosphereMaterial(material)) return false;

    static constexpr array<ref<str>, 7> shadow_values = {
        "Position"_str,    "Planet size"_str, "Atmosphere size"_str, "Density falloff"_str,
        "Light color"_str, "Brightness"_str,  "Radius"_str,
    };

    for (auto shadow_value : shadow_values) {
        if (name == shadow_value) return true;
    }
    return false;
}

SceneShaderVariantDesc MakeSceneShaderVariantDesc(
    ref<str> scene_id, const wpscene::Material& material, const ShaderInfo& info,
    const Combos& input_combos, slice<ShaderUnit> units, slice<String> source_keys,
    slice<String> stage_sources, slice<ShaderTexInfo> texinfos, bool geometry_shader_enabled) {
    SceneShaderVariantDesc desc;
    desc.scene_id                = rstd::into(scene_id);
    desc.shader_name             = rstd::into(material.shader.as_str());
    desc.input_combos            = input_combos.clone();
    desc.resolved_combos         = info.combos.clone();
    desc.uniform_aliases         = info.alias.clone();
    desc.default_textures        = info.defTexs.clone();
    desc.geometry_shader_enabled = geometry_shader_enabled;

    desc.texture_infos.reserve(texinfos.len());
    for (const auto& texinfo : texinfos) {
        desc.texture_infos.push(ToSceneShaderTextureCompileInfo(texinfo));
    }

    desc.stages.reserve(units.len());
    for (usize i {}; i < units.len(); ++i) {
        desc.stages.push(SceneShaderVariantStage {
            .stage      = units[i].stage,
            .source_key = i < source_keys.len() ? source_keys[i].clone() : String(),
            .source     = i < stage_sources.len() ? stage_sources[i].clone() : units[i].src.clone(),
        });
    }
    return desc;
}

auto BuildMaterial(fs::VFS& vfs, ShaderCache& shader_cache,
                   const SceneShaderEnvironment& environment, const wpscene::Material& wpmat,
                   Scene& scene, ShaderInfo shader_info, GeometryStageRequirement geometry_stage,
                   SceneLoadBenchRecorderView bench) -> Result<MaterialBuild, MaterialBuildError> {
    auto          build_span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_build);
    MaterialBuild build { .shader_info = rstd::move(shader_info) };
    auto&         material        = build.material;
    auto&         shader_info_ref = build.shader_info;
    auto          blend_mode      = ParseBlendMode(wpmat.blending.as_str());

    SceneMaterialCustomShader materialShader;

    materialShader.shader     = Some(Arc<SceneShader>::make());
    auto& shader              = *materialShader.shader;
    shader->name              = rstd::into(wpmat.shader.as_str());
    shader->matrix_convention = ShaderMatrixConvention::RowVector;
    shader->matrix_abi        = ShaderMatrixAbi::Hlsl;
    auto shaderPath           = rstd::format("/assets/shaders/{}", wpmat.shader);

    auto            sources_span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_sources);
    Vec<ShaderUnit> sd_units;
    Vec<String>     sd_source_keys;
    Vec<String>     sd_original_sources;
    auto            append_shader_unit = [&](ShaderType stage, String source_key, String source) {
        sd_source_keys.push(rstd::move(source_key));
        sd_original_sources.push(source.clone());
        sd_units.push({
            .stage           = stage,
            .src             = rstd::move(source),
            .preprocess_info = {},
        });
    };
    auto add_shader_unit = [&](ShaderType stage, String source_key) {
        auto   loaded = fs::ReadFileContent(vfs, fs::Path(source_key.as_str()));
        String source;
        if (loaded.is_ok()) {
            source = rstd::move(loaded).unwrap_unchecked();
        } else {
            rstd_error("Can't read shader source {}", source_key);
        }
        append_shader_unit(stage, rstd::move(source_key), rstd::move(source));
    };
    add_shader_unit(ShaderType::VERTEX, rstd::format("{}.vert", shaderPath));
    if (geometry_stage == GeometryStageRequirement::Required ||
        geometry_stage == GeometryStageRequirement::IfAvailable) {
        auto geom_path = rstd::format("{}.geom", shaderPath);
        auto loaded    = fs::ReadFileContent(vfs, fs::Path(geom_path.as_str()));
        if (loaded.is_err()) {
            auto error = rstd::move(loaded).unwrap_err_unchecked();
            if (geometry_stage == GeometryStageRequirement::Required ||
                error.kind().code != rstd::io::error::ErrorKind::NotFound) {
                rstd_error("Can't read geometry shader source {}: {}", geom_path, error);
                return Err(MaterialBuildError {
                    .message = "failed to read geometry shader source"_Str,
                });
            }
        } else if (auto source = rstd::move(loaded).unwrap_unchecked(); ! source.is_empty()) {
            append_shader_unit(ShaderType::GEOMETRY, rstd::move(geom_path), rstd::move(source));
            build.has_geometry_stage = true;
        } else if (geometry_stage == GeometryStageRequirement::Required) {
            rstd_error("required geometry shader source is empty: {}", geom_path);
            return Err(MaterialBuildError {
                .message = "required geometry shader source is empty"_Str,
            });
        }
    }
    const bool has_geometry_stage = build.has_geometry_stage;
    if (geometry_stage != GeometryStageRequirement::None)
        (void)shader_info_ref.combos.insert(rstd::into(WE_CB_GS_ENABLED),
                                            has_geometry_stage ? "1"_Str : "0"_Str);
    add_shader_unit(ShaderType::FRAGMENT, rstd::format("{}.frag", shaderPath));
    (void)sources_span.finish();

    auto texture_span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_texture_metadata);
    Vec<ShaderTexInfo>           texinfos;
    HashMap<String, ImageHeader> tex_headers;
    for (const auto& el : wpmat.textures) {
        if (el.is_empty()) {
            texinfos.push({ false });
        } else if (! IsSpecTex(el.as_str())) {
            auto parsed_header = scene.ParseImageHeader(el.as_str());
            auto texh = parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                              : ImageHeader {};
            if (! texh.extraHeader.contains_key("compo1"_str)) {
                texinfos.push({ false });
                (void)tex_headers.insert(String::make(el.as_str()), rstd::move(texh));
                continue;
            }
            texinfos.push({ true,
                            {
                                (bool)texh.extraHeader.get("compo1"_str).unwrap()->val,
                                (bool)texh.extraHeader.get("compo2"_str).unwrap()->val,
                                (bool)texh.extraHeader.get("compo3"_str).unwrap()->val,
                                (bool)texh.extraHeader.get("compo4"_str).unwrap()->val,
                            } });
            (void)tex_headers.insert(String::make(el.as_str()), rstd::move(texh));
        } else
            texinfos.push({ true });
    }

    (void)texture_span.finish();
    {
        auto span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_preprocess);
        for (auto& unit : sd_units) {
            unit.src = ShaderParser::PreShaderSrc(
                vfs, unit.src, &shader_info_ref, texinfos.as_slice(), &shader_cache, bench);
        }
    }
    auto combos_span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_combos);
    ApplyLegacyAtmosphereUniformAliases(wpmat, shader_info_ref);
    ApplyLegacyAtmosphereShaderCompat(wpmat, sd_units);

    for (auto [key, value] : wpmat.combos.iter()) {
        (void)shader_info_ref.combos.insert(key->clone(), rstd::format("{}", *value));
    }
    if (geometry_stage != GeometryStageRequirement::None)
        (void)shader_info_ref.combos.insert(rstd::into(WE_CB_GS_ENABLED),
                                            has_geometry_stage ? "1"_Str : "0"_Str);
    if (blend_mode == BlendMode::AlphaToCoverage) {
        (void)shader_info_ref.combos.insert("ALPHATOCOVERAGE"_Str, "1"_Str);
    }
    ApplySceneFogCombos(environment, shader_info_ref);
    ApplyLegacyAtmosphereLightCombo(wpmat, shader_info_ref);
    (void)combos_span.finish();

    texture_span  = SceneLoadSpan(bench, &SceneLoadProbeIds::material_texture_metadata);
    auto textures = wpmat.textures.clone();
    if (! shader_info_ref.defTexs.is_empty()) {
        for (const auto& t : shader_info_ref.defTexs) {
            const auto index = rstd::as_cast<usize>(t.slot);
            if (textures.len() > index) {
                if (! textures[index].is_empty()) continue;
            } else {
                textures.resize(index + usize(1), String {});
            }
            textures[index] = t.texture.clone();
        }
    }

    for (usize i {}; i < textures.len(); ++i) {
        auto name = textures[i].clone();
        ParseSpecTexName(name, wpmat, shader_info_ref, scene);
        material.textures.push(rstd::into(name.as_str()));
        material.texture_metadata.push(SceneMaterialTextureMetadata {});
        material.defines.push(rstd::format("g_Texture{}", i));
        if (name.is_empty()) {
            continue;
        }

        array<i32, 4> resolution {};
        auto          texture_name = name.as_str();
        if (IsSpecTex(texture_name)) {
            auto target = scene.RenderTarget(name.as_str());
            if (! IsSpecLinkTex(texture_name) && target.is_none()) {
                rstd_error("{} not found in render targes", name);
            } else if (target.is_some()) {
                const auto& rt = **target;
                resolution     = { i32(rt.width), i32(rt.height), i32(rt.width), i32(rt.height) };
            }
        } else {
            auto texh = [&] {
                if (auto header = tex_headers.get(name.as_str()); header.is_some())
                    return (**header).clone();
                auto parsed_header = scene.ParseImageHeader(name.as_str());
                return parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                             : ImageHeader {};
            }();
            if (i == usize()) {
                if (texh.format == TextureFormat::R8)
                    (void)shader_info_ref.combos.insert("TEX0FORMAT"_Str, "FORMAT_R8"_Str);
                else if (texh.format == TextureFormat::RG8)
                    (void)shader_info_ref.combos.insert("TEX0FORMAT"_Str, "FORMAT_RG88"_Str);
            }
            if (texh.mipmap_larger) {
                resolution = {
                    i32(texh.width), i32(texh.height), i32(texh.mapWidth), i32(texh.mapHeight)
                };
            } else {
                resolution = {
                    i32(texh.mapWidth), i32(texh.mapHeight), i32(texh.mapWidth), i32(texh.mapHeight)
                };
            }
            material.texture_metadata.last_mut().unwrap().get_mut() = SceneMaterialTextureMetadata {
                .has_extent    = true,
                .source_extent = { rstd::as_cast<float>(resolution[usize(0)]),
                                   rstd::as_cast<float>(resolution[usize(1)]) },
                .sample_extent = { rstd::as_cast<float>(resolution[usize(2)]),
                                   rstd::as_cast<float>(resolution[usize(3)]) },
            };

            auto scene_texture = scene.Texture(name.as_str());
            if (scene_texture.is_none()) {
                SceneTexture stex;
                stex.sample  = texh.sample;
                stex.url     = rstd::into(name.as_str());
                stex.isVideo = texh.type == ImageType::VIDEO;
                if (texh.isSprite) {
                    stex.isSprite   = texh.isSprite;
                    stex.spriteAnim = texh.spriteAnim.clone();
                }
                scene.RegisterTexture(String::make(name.as_str()), rstd::move(stex));
                scene_texture = scene.Texture(name.as_str());
            }
            if (scene_texture.is_some() && (**scene_texture).isSprite) {
                material.hasSprite = true;
                const auto& f1     = texh.spriteAnim.GetCurFrame();
                if (wpmat.shader == "genericparticle"_str ||
                    wpmat.shader == "genericropeparticle"_str) {
                    (void)shader_info_ref.combos.insert(rstd::into(WE_CB_SPRITESHEET), "1"_Str);
                    (void)shader_info_ref.combos.insert(rstd::into(WE_CB_THICK_FORMAT), "1"_Str);
                    if (algorism::IsPowOfTwo(rstd::as_cast<u32>(texh.width)) &&
                        algorism::IsPowOfTwo(rstd::as_cast<u32>(texh.height))) {
                        (void)shader_info_ref.combos.insert(rstd::into(WE_CB_SPRITESHEETBLENDNPOT),
                                                            "1"_Str);
                        resolution[usize(2)] = resolution[usize(0)] -
                                               resolution[usize(0)] % rstd::as_cast<i32>(f1.width);
                        resolution[usize(3)] = resolution[usize(1)] -
                                               resolution[usize(1)] % rstd::as_cast<i32>(f1.height);
                        material.texture_metadata.last_mut().unwrap().get_mut().sample_extent = {
                            rstd::as_cast<float>(resolution[usize(2)]),
                            rstd::as_cast<float>(resolution[usize(3)])
                        };
                    }
                    (void)materialShader.constValues.insert(
                        rstd::into(G_RENDERVAR1),
                        ShaderValue(array<float, 4> {
                            f1.xAxis[usize()],
                            f1.yAxis[usize(1)],
                            static_cast<float>(texh.spriteAnim.numFrames().to_primitive()),
                            f1.rate }));
                }
            }
        }
        if (! resolution.is_empty()) {
            const auto gResolution = WE_GLTEX_RESOLUTION_NAMES[usize(i)];

            (void)materialShader.constValues.insert(
                rstd::into(gResolution), ShaderValue(resolution.clone().map([](i32 value) {
                    return rstd::as_cast<float>(value);
                })));
        }
    }
    if (shader_info_ref.combos.contains_key(WE_CB_LIGHTING)) {
        if (environment.directional_shadow &&
            (**shader_info_ref.combos.get(WE_CB_LIGHTING)).as_str() != "0"_str) {
            (void)shader_info_ref.combos.insert("LIGHTS_SHADOW_MAPPING"_Str, "1"_Str);
            (void)shader_info_ref.combos.insert("LIGHTS_SHADOW_MAPPING_QUALITY"_Str, "2"_Str);
        }
    }

    (void)texture_span.finish();
    combos_span            = SceneLoadSpan(bench, &SceneLoadProbeIds::material_combos);
    auto input_combos      = shader_info_ref.combos.clone();
    shader_info_ref.combos = ShaderParser::ResolveShaderCombos(shader_info_ref, input_combos);
    (void)combos_span.finish();
    auto variant_span          = SceneLoadSpan(bench, &SceneLoadProbeIds::material_variant);
    auto scene_id              = scene.SceneId();
    auto variant_desc          = MakeSceneShaderVariantDesc(scene_id,
                                                            wpmat,
                                                            shader_info_ref,
                                                            input_combos,
                                                            sd_units.as_slice(),
                                                            sd_source_keys.as_slice(),
                                                            sd_original_sources.as_slice(),
                                                            texinfos.as_slice(),
                                                            has_geometry_stage);
    variant_desc.texture_slots = material.textures.clone();
    (void)variant_span.finish();

    auto compile_span = SceneLoadSpan(bench, &SceneLoadProbeIds::material_compile);
    auto compiled     = ShaderParser::CompileToSpv(scene_id,
                                                   sd_units.as_mut_slice().as_mut_ref(),
                                                   &shader_info_ref,
                                                   texinfos.as_slice(),
                                                   &shader_cache,
                                                   bench);
    if (compiled.is_err()) {
        return Err(MaterialBuildError {
            .message = rstd::move(compiled).unwrap_err_unchecked().message,
        });
    }
    auto artifact      = rstd::move(compiled).unwrap_unchecked();
    shader->codes      = artifact->Codes();
    shader->reflection = Some(artifact->ReflectionOwner());
    (void)compile_span.finish();
    shader->default_uniforms      = SceneShaderDefaults::from(shader_info_ref.svs.clone());
    variant_desc.default_uniforms = shader->default_uniforms.clone();
    ShaderParser::ApplyCompiledShaderInterface(variant_desc, *artifact, bench);
    shader->sampler_bindings = variant_desc.sampler_bindings.clone();
    shader->uniform_blocks   = variant_desc.uniform_blocks.clone();
    shader->descriptor_sets  = variant_desc.descriptor_sets.clone();

    auto pipeline        = material.Pipeline();
    pipeline.blend_mode  = blend_mode;
    pipeline.alpha_write = ParseAlphaWrite(wpmat.alphawriting.as_str());
    pipeline.depth_test  = ParseEnabled(wpmat.depthtest.as_str());
    // WE disables depth writes for blended materials, including fully transparent texels.
    pipeline.depth_write = ParseEnabled(wpmat.depthwrite.as_str()) &&
                           blend_mode != BlendMode::Translucent &&
                           blend_mode != BlendMode::Additive;
    pipeline.cull_mode   = ParseCullMode(wpmat.cullmode.as_str());
    material.SetPipeline(rstd::move(pipeline));

    // FS is always the last unit (VS may be followed by optional GS, then FS).
    const auto& fs_active = sd_units.last().unwrap()->preprocess_info.active_tex_slots;
    for (usize i {}; i < material.textures.len(); ++i) {
        if (! fs_active.contains(rstd::as_cast<u32>(i))) material.textures[i].clear();
    }

    for (const auto& [key, value] : shader_info_ref.baseConstSvs.iter()) {
        (void)materialShader.constValues.insert(key->clone(), value->clone());
    }
    // Register bindings only after AddMaterial places the material in its
    // stable mesh-owned allocation. Registering the stack-local pointer here
    // would leave a dangling binding after the move.
    for (const auto& var : shader_info_ref.scalar_uniforms) {
        if (! var.is_user || var.material.is_empty()) continue;
        shader_info_ref.user_var_staging.push(UserVarRecord {
            .material      = var.material.clone(),
            .name          = var.name.clone(),
            .default_value = var.default_value.clone(),
        });
        if (auto value = shader->default_uniforms.get(var.name.as_str()); value.is_some()) {
            (void)materialShader.constValues.insert(var.name.clone(), **value);
        }
    }

    material.customShader         = rstd::move(materialShader);
    material.customShader.variant = Some(SceneShaderVariant::from(rstd::move(variant_desc)));
    material.name                 = rstd::into(wpmat.shader.as_str());

    return Ok(rstd::move(build));
}

bool IsLayerCompositeShader(ref<str> shader) {
    return shader == "genericimage"_str || shader == "genericimage2"_str ||
           shader == "genericimage3"_str || shader == "genericimage4"_str ||
           shader == "passthrough"_str;
}

String ResolveShaderMaterialKey(const ShaderInfo& info, const wpscene::Material& material,
                                ref<str> material_key) {
    if (auto value = info.alias.get(material_key); value.is_some()) return (**value).clone();

    Option<ref<str>> resolved;
    for (const auto& [_, value] : info.alias.iter()) {
        // Material keys are case-sensitive; obsolete Alpha must not replace alpha.
        if (value->len() <= usize(2) ||
            value->as_str().get(usize(2), value->len()).unwrap() != material_key)
            continue;
        if (resolved.is_some() && *resolved != value->as_str()) return {};
        resolved = Some(value->as_str());
    }
    if (resolved.is_none()) return {};
    // A legacy spelling must not overwrite an explicitly authored shader material key.
    for (const auto& [alias, uniform] : info.alias.iter()) {
        if (uniform->as_str() == *resolved &&
            material.constantshadervalues.contains_key(alias->as_str()))
            return {};
    }
    return rstd::into(*resolved);
}

bool IsShaderPositionUniform(const ShaderInfo& info, ref<str> glname) {
    for (const auto& var : info.scalar_uniforms) {
        if (var.name == glname) return var.position;
    }
    return false;
}

bool UsesEffectPositionSpace(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/spin"_str && wpmat.shader != "effects/transform"_str) return false;
    auto mode_it = wpmat.combos.get("MODE"_str);
    return mode_it.is_some() && **mode_it == i32(1);
}

bool UsesUnitFinalQuad(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/transform"_str) return false;
    auto mode_it = wpmat.combos.get("MODE"_str);
    return mode_it.is_some() && **mode_it == i32(1);
}

bool CanCompositeFinalEffectShader(ref<str> shader) {
    return IsLayerCompositeShader(shader) || shader == "effects/transform"_str ||
           shader == "effects/scroll"_str || shader == "effects/spin"_str ||
           shader == "effects/perspective"_str || shader == "effects/foliagesway"_str ||
           shader == "effects/blend"_str || shader == "effects/tint"_str;
}

bool HasShaderCombo(const ShaderInfo& info, ref<str> combo_name) {
    auto name = combo_name;
    for (const auto& combo : info.combo_defs)
        if (combo.combo == name) return true;
    return false;
}

bool HasShaderTextureMaterial(const ShaderInfo& info, ref<str> material_key) {
    auto key = material_key;
    for (const auto& texture : info.texture_uniforms)
        if (texture.material == key) return true;
    return false;
}

bool HasSolidCompositeContext(const SceneLayerBuildContext& context,
                              const wpscene::ImageObject&   obj) {
    if (obj.solid || context.layer_resources->solid_layer_ids.contains(obj.id)) return true;

    HashSet<i32> seen;
    u32          parent = obj.parent;
    while (parent != u32() && seen.insert(rstd::as_cast<i32>(parent))) {
        const auto parent_id = rstd::as_cast<i32>(parent);
        if (context.layer_resources->solid_layer_ids.contains(parent_id)) return true;

        auto found = context.layer_resources->object_parent_ids.get(parent_id);
        if (found.is_none()) break;
        parent = **found;
    }

    return false;
}

bool CanCompositeFinalEffectMaterial(ref<str> shader, const ShaderInfo& info,
                                     bool allow_transparent_previous) {
    if (CanCompositeFinalEffectShader(shader)) return true;
    if (! allow_transparent_previous) return false;

    // TODO: WE does not document this as the final-composite rule. This keeps
    // the historical shortcut only for non-solid layer contexts.
    return HasShaderCombo(info, "TRANSPARENCY"_str) &&
           HasShaderTextureMaterial(info, "previous"_str);
}

void NormalizeEffectPositionCurve(SceneAnimationCurve& curve) {
    auto normalize_axis = [&](Vec<SceneAnimationKey>& keys) {
        for (auto& key : keys) {
            key.value = curve.relative ? key.value * 2.0f : key.value * 2.0f - 1.0f;
        }
    };
    normalize_axis(curve.c0);
    normalize_axis(curve.c1);
}

// Register a (material, shader-info, wpmat) triple into the scene-wide user
// variable index. Must be called after SceneMaterial has entered its stable
// mesh-owned allocation. Wires up:
//   (1) Direct-route u_* whose shader annotation's `material` field is the
//       wallpaper-level project.json key (the legacy convention).
//   (2) Instance-bound effect-internal keys mapped through `info.alias` to
//       the GLSL uniform name.
//   (3) Legacy material `usershadervalues` bindings: project key to shader
//       material key.
void RegisterShaderUserVarIndexImpl(Scene* pScene, const Arc<SceneMaterial>& stable_mat,
                                    const wpscene::Material& wpmat, const ShaderInfo& info,
                                    SceneNodeId owner) {
    if (! pScene || ! stable_mat) return;
    for (const auto& combo : info.combo_defs) {
        if (combo.material.is_empty() || combo.combo.is_empty()) continue;
        Scene::ShaderComboUserBinding binding {
            .material = stable_mat.clone(),
            .combo    = combo.combo.clone(),
            .fallback = rstd::format("{}", combo.default_),
        };
        combo.options.iter().for_each([&](auto entry) {
            auto [label, value] = entry;
            (void)binding.options.insert(label->clone(), rstd::format("{}", *value));
        });
        pScene->RegisterShaderComboUserBinding(combo.material.clone(), rstd::move(binding), owner);
    }
    for (const auto& rec : info.user_var_staging) {
        pScene->RegisterShaderUserBinding(
            rec.material.clone(), stable_mat.clone(), rec.name.clone(), owner);
    }
    for (const auto& field_binding : wpmat.constantshadervalues_bindings.Entries()) {
        if (field_binding.user.is_none()) continue;
        const auto effect_key    = field_binding.field.as_str();
        const auto wallpaper_key = field_binding.user->as_str();
        // Resolve effect-internal key → GLSL uniform name via alias.
        // LoadConstvalue's fallback search (alias entry whose value, after
        // dropping the leading "u_", matches the key) is honored here too.
        auto glname = ResolveShaderMaterialKey(info, wpmat, effect_key);
        if (glname.is_empty()) {
            rstd_warn("user binding '{}' → no shader uniform with material='{}'",
                      wallpaper_key,
                      effect_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(
            String::make(wallpaper_key), stable_mat.clone(), String::make(glname.as_str()), owner);
    }
    for (auto [wallpaper_key, material_key] : wpmat.user_shader_values.iter()) {
        auto glname = ResolveShaderMaterialKey(info, wpmat, material_key->as_str());
        if (glname.is_empty()) {
            rstd_warn("user shader value '{}' -> no shader uniform with material='{}'",
                      *wallpaper_key,
                      *material_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(
            wallpaper_key->clone(), stable_mat.clone(), rstd::move(glname), owner);
    }
}

Option<String> UserTexturePropertyKey(const Json& binding) {
    if (binding.is_string()) {
        auto key = *binding.as_str();
        if (key.is_empty()) return None();
        return Some(String::make(key));
    }
    if (! binding.is_object()) return None();
    auto type  = binding.get("type"_str);
    auto value = binding.get("name"_str);
    if (type.is_none() || value.is_none()) return None();
    auto type_string  = (*type)->as_str();
    auto value_string = (*value)->as_str();
    if (type_string.is_none() || value_string.is_none() || *type_string != "system"_str)
        return None();
    auto name = *value_string;
    if (name != "$mediaThumbnail"_str && name != "$mediaPreviousThumbnail"_str) return None();
    return Some(String::make(*value_string));
}

bool IsSystemMediaTextureBinding(const Json& binding) {
    return UserTexturePropertyKey(binding).is_some() && binding.is_object();
}

String ResolveMaterialTextureFallback(Scene& scene, const wpscene::Material& fallback_material,
                                      const ShaderInfo& shader_info, usize slot) {
    String fallback;
    if (slot < fallback_material.textures.len()) {
        fallback = fallback_material.textures[slot].clone();
    }
    if (fallback.is_empty()) {
        for (const auto& [index, texture] : shader_info.defTexs) {
            if (rstd::as_cast<usize>(index) == slot) {
                fallback = texture.clone();
                break;
            }
        }
    }
    ParseSpecTexName(fallback, fallback_material, shader_info, scene);
    return fallback;
}

void RegisterMaterialUserTextureIndex(Scene* pScene, const Arc<SceneMaterial>& stable_mat,
                                      const wpscene::Material& fallback_material,
                                      const ShaderInfo& shader_info, SceneNodeId owner) {
    if (! pScene || ! stable_mat) return;
    for (usize i {}; i < fallback_material.usertextures.len(); ++i) {
        auto key = UserTexturePropertyKey(fallback_material.usertextures[i]);
        if (key.is_none()) continue;
        String fallback =
            ResolveMaterialTextureFallback(*pScene, fallback_material, shader_info, i);
        if (IsSystemMediaTextureBinding(fallback_material.usertextures[i]) &&
            i < stable_mat->textures.len()) {
            fallback = stable_mat->textures[i].clone();
        }
        pScene->RegisterMaterialTextureUserBinding(rstd::move(*key),
                                                   Scene::MaterialTextureUserBinding {
                                                       .material = stable_mat.clone(),
                                                       .slot     = rstd::as_cast<u32>(i),
                                                       .fallback = String::make(fallback.as_str()),
                                                   },
                                                   owner);
    }
}

Vector3f AlignmentOffset(ref<str> align, Vector2f size) {
    Vector3f offset = Vector3f::Zero();
    size *= 0.5f;
    size.y() *= 1.0f;

    // topleft top center ...
    if (align.contains("top"_str)) offset.y() -= size.y();
    if (align.contains("left"_str)) offset.x() += size.x();
    if (align.contains("right"_str)) offset.x() -= size.x();
    if (align.contains("bottom"_str)) offset.y() += size.y();

    return offset;
}

// Apply effect-pass `bind` overrides onto wpmat.textures by index, using
// The render-target map resolves effect-local FBO names to actual scene RT keys.
void ApplyTextureBindsImpl(wpscene::Material& wpmat, slice<wpscene::MaterialPassBindItem> binds,
                           const EffectRenderTargets& render_targets) {
    for (const auto& el : binds) {
        auto target = render_targets.get(el.name.as_str());
        if (target.is_none()) {
            rstd_error("fbo {} not found", el.name);
            continue;
        }
        const auto index = rstd::as_cast<usize>(el.index);
        if (wpmat.textures.len() <= index) wpmat.textures.resize(index + usize(1), String {});
        wpmat.textures[index] = (**target).clone();
    }
}

String ResolveSceneTextureProperty(const SceneLayerBuildContext& context, ref<str> key) {
    if (context.user_properties.is_none()) return {};
    auto prop = (*context.user_properties)->get(key);
    if (prop.is_none()) return {};
    const auto& payload = **prop;
    if (payload.is_string()) {
        return rstd::into(*payload.as_str());
    }
    if (! payload.is_object()) return {};

    String type;
    if (auto value = payload.get("type"_str); value.is_some()) {
        auto string = (*value)->as_str();
        if (string.is_some()) type = rstd::into(*string);
    }
    if (! type.is_empty() && type != "scenetexture"_str && type != "texture"_str &&
        type != "replacetexture"_str)
        return {};
    auto value = payload.get("value"_str);
    if (value.is_none()) return {};
    auto string = (*value)->as_str();
    return string.is_none() ? String {} : rstd::into<String>(*string);
}

String ResolveUserTextureProperty(const SceneLayerBuildContext& context, const Json& binding) {
    if (! binding.is_string()) return {};
    auto key = *binding.as_str();
    return ResolveSceneTextureProperty(context, key);
}

String ResolveMaterialTextureSlot(const SceneLayerBuildContext& context,
                                  const wpscene::Material& material, usize slot) {
    String fallback;
    if (slot < material.textures.len()) {
        fallback = material.textures[slot].clone();
    }
    if (slot >= material.usertextures.len()) return fallback;

    if (auto prop = ResolveUserTextureProperty(context, material.usertextures[slot]);
        ! prop.is_empty())
        return prop;
    return fallback;
}

bool CanUseImageAsSystemMediaFallback(const wpscene::ImageObject& image) {
    if (! image.puppet.is_empty()) return false;
    if (image.fullscreen || image.config.passthrough) return false;
    return image.effects.is_empty();
}

String ResolveLinkedImageFallback(const SceneLayerBuildContext& context, ref<str> texture) {
    auto name      = texture;
    auto linked_id = ParseImageLayerCompositeId(name);
    if (! linked_id && IsSpecLinkTex(name)) {
        linked_id = Some(ParseLinkTex(name));
    }
    if (! linked_id) return {};

    auto fallback =
        context.layer_resources->system_media_image_fallbacks.get(rstd::as_cast<i32>(*linked_id));
    return fallback.is_some() ? (**fallback).clone() : String {};
}

String ResolveSystemMediaFallback(const SceneLayerBuildContext& context,
                                  const wpscene::Material& material, usize slot) {
    if (slot >= material.textures.len()) return {};
    return ResolveLinkedImageFallback(context, material.textures[slot].as_str());
}

void ApplyUserTextureBindings(SceneLayerBuildContext& context, wpscene::Material& material) {
    for (usize i {}; i < material.usertextures.len(); ++i) {
        const auto& binding = material.usertextures[i];
        if (binding.is_null()) continue;

        auto resolved = ResolveUserTextureProperty(context, binding);
        if (resolved.is_empty() && IsSystemMediaTextureBinding(binding)) {
            resolved = ResolveSystemMediaFallback(context, material, i);
        }
        if (resolved.is_empty()) continue;

        if (material.textures.len() <= i) {
            material.textures.resize(i + usize(1), String {});
        }
        material.textures[i] = rstd::move(resolved);
    }
}

void IndexSystemMediaImageFallbacks(SceneLayerBuildContext& context,
                                    slice<SceneObjectVar>   scene_objs) {
    context.layer_resources->system_media_image_fallbacks.clear();
    for (usize index {}; index < scene_objs.len(); ++index) {
        const auto& object = scene_objs[index];
        if (! object.is_Image()) continue;
        const auto& image = object.as_Image().value;
        if (! CanUseImageAsSystemMediaFallback(image)) continue;

        auto texture = ResolveMaterialTextureSlot(context, image.material, usize(0));
        if (texture.is_empty() || IsSpecTex(texture.as_str())) continue;
        (void)context.layer_resources->system_media_image_fallbacks.insert(
            image.id, String::make(texture.as_str()));
    }
}

void LoadConstvalueImpl(SceneLayerBuildContext& context, SceneMaterial& material,
                        const wpscene::Material& wpmat, const ShaderInfo& info,
                        SceneShaderValueAnimationMap* final_quad_shader_values) {
    // load glname from alias and load to constvalue
    for (auto [key, values] : wpmat.constantshadervalues.iter()) {
        const auto& name   = *key;
        const auto& value  = *values;
        auto        glname = ResolveShaderMaterialKey(info, wpmat, name.as_str());
        if (glname.is_empty()) {
            if (IsLegacyAtmosphereShadowValue(wpmat, name.as_str())) continue;
            if (wpmat.constantshadervalues_bindings.HasAnimation(name.as_str())) {
                rstd_warn("animated shader value '{}' has no uniform in '{}'", name, wpmat.shader);
            } else {
                rstd_debug(
                    "ignoring shader value '{}' without a uniform in '{}'", name, wpmat.shader);
            }
        } else {
            auto const_value = value.clone();
            bool normalize_position =
                UsesEffectPositionSpace(wpmat) && IsShaderPositionUniform(info, glname.as_str());
            Option<SceneShaderValueAnimation> final_quad_value;
            if (normalize_position && const_value.len() >= usize(2)) {
                final_quad_value       = Some(SceneShaderValueAnimation {});
                final_quad_value->base = ShaderValue(value.as_slice());
                const_value[usize()]   = const_value[usize()] * 2.0f - 1.0f;
                const_value[usize(1)]  = const_value[usize(1)] * 2.0f - 1.0f;
            }
            material.SetShaderValue(glname.as_str(), ShaderValue(const_value.as_slice()));
            auto binding = wpmat.constantshadervalues_bindings.Get(name.as_str());
            if (binding.is_some() && (**binding).animation.is_some()) {
                const auto& authored = *(**binding).animation;
                auto        track    = ResolveAnimationTrack(context, **binding);
                if (final_quad_value)
                    final_quad_value->track = Some(Arc<SceneAnimationTrack>::make(track.Share()));
                if (normalize_position) {
                    auto curve = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(authored));
                    NormalizeEffectPositionCurve(*curve);
                    track.curve = rstd::move(curve);
                }
                material.SetShaderValueAnimation(String::make(glname.as_str()),
                                                 Arc<SceneAnimationTrack>::make(rstd::move(track)));
            }
            if (final_quad_value && final_quad_shader_values) {
                (void)final_quad_shader_values->insert(String::make(glname.as_str()),
                                                       rstd::move(*final_quad_value));
            }
        }
    }
}

script::FieldKind ShaderValueScriptKind(usize component_count) {
    switch (component_count.to_primitive()) {
    case 1: return script::FieldKind::Scalar;
    case 2: return script::FieldKind::Vec2;
    case 3: return script::FieldKind::Vec3;
    case 4: return script::FieldKind::Vec4;
    default: return script::FieldKind::Unknown;
    }
}

auto ScriptValueAsShaderValue(const script::ScriptValue& value) -> Option<ShaderValue> {
    if (auto* scalar = (value.is_Scalar() ? &value.as_Scalar().value : nullptr))
        return Some(ShaderValue(static_cast<float>(scalar->v)));
    if (auto* boolean = (value.is_Bool() ? &value.as_Bool().value : nullptr))
        return Some(ShaderValue(boolean->v ? 1.0f : 0.0f));
    if (auto* vector = (value.is_Vec2() ? &value.as_Vec2().value : nullptr))
        return Some(ShaderValue(
            array<float, 2> { static_cast<float>(vector->x), static_cast<float>(vector->y) }));
    if (auto* vector = (value.is_Vec3() ? &value.as_Vec3().value : nullptr))
        return Some(ShaderValue(array<float, 3> { static_cast<float>(vector->x),
                                                  static_cast<float>(vector->y),
                                                  static_cast<float>(vector->z) }));
    if (auto* vector = (value.is_Vec4() ? &value.as_Vec4().value : nullptr))
        return Some(ShaderValue(array<float, 4> { static_cast<float>(vector->x),
                                                  static_cast<float>(vector->y),
                                                  static_cast<float>(vector->z),
                                                  static_cast<float>(vector->w) }));
    if (auto* color = (value.is_Color() ? &value.as_Color().value : nullptr))
        return Some(ShaderValue(array<float, 3> { static_cast<float>(color->r),
                                                  static_cast<float>(color->g),
                                                  static_cast<float>(color->b) }));
    return None();
}

void WireMaterialShaderValueScripts(SceneLayerBuildContext& context, const Arc<SceneNode>& owner,
                                    const Arc<SceneMaterial>& material,
                                    const wpscene::Material& wpmat, const ShaderInfo& info) {
    if (! material) return;
    material->RegisterAnimations(*owner);
    for (const auto& field_binding : wpmat.constantshadervalues_bindings.Entries()) {
        auto material_key = field_binding.field.as_str();
        auto uniform_name = ResolveShaderMaterialKey(info, wpmat, material_key);
        if (uniform_name.is_empty()) continue;
        auto animation = material->ShaderValueAnimation(uniform_name.as_str());
        if (field_binding.script.is_none()) continue;

        const auto& binding = *field_binding.script;
        auto        value   = wpmat.constantshadervalues.get(material_key);
        if (value.is_none()) continue;
        auto kind = ShaderValueScriptKind((*value)->len());
        if (kind == script::FieldKind::Unknown) continue;

        auto& scripts      = EnsureScriptScene(context);
        auto  sha          = utils::genSha1(rstd::as_bytes(binding.source.as_str().as_bytes()));
        auto* field_script = scripts.runtime().MakeFieldScript(
            binding.source.as_str(),
            sha.as_str(),
            kind,
            field_binding.ScriptProperties(),
            binding.initial_value,
            script::ScriptBindingContext::ForMaterial(owner.as_ptr(),
                                                      material.as_ptr().as_raw_ptr(),
                                                      field_binding.field.as_str(),
                                                      rstd::move(animation)),
            context.script_initialization);
        if (! field_script) continue;
        SetScriptInitializationOrder(context, *field_script, owner.as_ptr());
        TrackFieldScript(context, field_script);
        auto* scene = context.scene;
        scripts.AddActuator({
            field_script,
            [scene, material = material.clone(), uniform_name = rstd::move(uniform_name)](
                const script::ScriptValue& script_value) {
                auto value = ScriptValueAsShaderValue(script_value);
                if (value.is_none()) return;
                (void)scene->SetMaterialShaderValue(*material, uniform_name.as_str(), *value);
            },
        });
    }
}

void RegisterMaterialBindings(Scene& scene, const Arc<SceneMaterial>& material,
                              const wpscene::Material& authored, const ShaderInfo& shader_info,
                              Option<ref<wpscene::Material>> user_texture_fallback,
                              SceneNodeId                    owner) {
    RegisterShaderUserVarIndexImpl(&scene, material, authored, shader_info, owner);
    if (user_texture_fallback.is_some()) {
        RegisterMaterialUserTextureIndex(
            &scene, material, **user_texture_fallback, shader_info, owner);
    }
}

void RegisterLayerPreviousBindings(Scene& scene, SceneMaterial& material,
                                   const wpscene::Material& authored, SceneNodeId layer,
                                   ref<str> composite_target) {
    for (usize index {}; index < authored.textures.len(); ++index) {
        if (authored.textures[index] != composite_target) {
            continue;
        }
        (void)scene.SetMaterialLayerPreviousSource(
            material, rstd::as_cast<u32>(index), layer, composite_target);
    }
}

void ApplyTextureBinds(wpscene::Material& material, slice<wpscene::MaterialPassBindItem> bindings,
                       const EffectRenderTargets& render_targets) {
    ApplyTextureBindsImpl(material, bindings, render_targets);
}

void LoadConstvalue(SceneLayerBuildContext& context, SceneMaterial& material,
                    const wpscene::Material& authored, const ShaderInfo& shader_info,
                    SceneShaderValueAnimationMap* final_quad_shader_values) {
    LoadConstvalueImpl(context, material, authored, shader_info, final_quad_shader_values);
}

} // namespace owe
