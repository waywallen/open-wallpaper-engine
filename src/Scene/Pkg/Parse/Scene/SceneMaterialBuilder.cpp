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
BlendMode ParseBlendMode(std::string_view str) {
    BlendMode bm;
    if (str == "translucent") {
        bm = BlendMode::Translucent;
    } else if (str == "additive") {
        bm = BlendMode::Additive;
    } else if (str == "alphatocoverage") {
        bm = BlendMode::AlphaToCoverage;
    } else if (str == "normal") {
        bm = BlendMode::Normal;
    } else if (str == "disabled") {
        bm = BlendMode::Disable;
    } else {
        bm = BlendMode::Normal;
        rstd_error("unknown blending: {}", str);
    }
    return bm;
}

Option<BlendMode> ApplyImageColorBlend(wpscene::Material&          material,
                                       const wpscene::ImageObject& image) {
    if (image.colorBlendMode == i32()) return None<BlendMode>();

    if (image.colorBlendMode == i32(31)) {
        material.combos.erase(rstd::cppstd::to_string(WE_CB_BLENDMODE));
        material.blending = "additive";
        return Some(BlendMode::Additive);
    }
    material.combos[rstd::cppstd::to_string(WE_CB_BLENDMODE)] = image.colorBlendMode;
    return None<BlendMode>();
}

ShaderValueMap NeutralColorUniforms(ShaderValueMap values) {
    values[rstd::cppstd::to_string(G_COLOR4)]     = std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
    values[rstd::cppstd::to_string(G_COLOR)]      = std::array<float, 3> { 1.0f, 1.0f, 1.0f };
    values[rstd::cppstd::to_string(G_ALPHA)]      = 1.0f;
    values[rstd::cppstd::to_string(G_USERALPHA)]  = 1.0f;
    values[rstd::cppstd::to_string(G_BRIGHTNESS)] = 1.0f;
    return values;
}

i32 CountVisibleImageEffects(std::span<const wpscene::ImageEffect> effects) {
    i32 count {};
    for (const auto& effect : effects) {
        if (effect.visible || ! effect.visible_user.empty()) count += i32(1);
    }
    return count;
}

bool ParseEnabled(std::string_view str) { return str == "enabled"; }

CullMode ParseCullMode(std::string_view str) {
    if (str == "back" || str == "normal") return CullMode::Back;
    if (str == "front") return CullMode::Front;
    if (str == "nocull" || str == "none" || str.empty()) return CullMode::None;
    rstd_error("unknown cullmode: {}", str);
    return CullMode::None;
}

void ParseSpecTexName(std::string& name, const wpscene::Material& wpmat, const ShaderInfo& sinfo,
                      Scene& scene) {
    auto text = as_str(name).unwrap();
    if (IsSpecTex(text)) {
        if (text == WE_FULL_FRAME_BUFFER) {
            name = rstd::cppstd::to_string(SpecTex_Default);
            if (wpmat.shader == "genericimage2" &&
                ! exists(sinfo.combos, as_string_view(WE_CB_BLENDMODE)))
                name = "";
            /*
            if(wpmat.shader == "genericparticle") {
                name = "_rt_ParticleRefract";
            }
            */
        } else if (auto wpid = ParseImageLayerCompositeId(text)) {
            rstd_info("link tex \"{}\"", name);
            name = GenLinkTex(wpid->to_primitive());
        } else if (text.starts_with(WE_MIP_MAPPED_FRAME_BUFFER)) {
        } else if (text.starts_with(WE_SHADOW_ATLAS_PREFIX)) {
            if (scene.RenderTarget(WE_SHADOW_ATLAS_PREFIX).is_none()) name.clear();
        } else if (text.starts_with(OWE_BLOOM_MIP_PREFIX)) {
        } else if (text.starts_with(WE_REFLECTION_PREFIX)) {
            name = rstd::cppstd::to_string(WE_REFLECTION_PREFIX);
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
        } else if (scene.RenderTarget(as_str(name).unwrap()).is_some()) {
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
    return material.shader == "workshop/2839476907/effects/atmosphere";
}

void ApplyLegacyAtmosphereLightCombo(const wpscene::Material& material, ShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    if (! info.combos.contains("LIGHT_INDEX") || material.combos.contains("LIGHT_INDEX")) return;
    if (! material.combos.contains("LIGHT1")) return;

    info.combos["LIGHT_INDEX"] = "4";
}

void ApplySceneFogCombos(const SceneShaderEnvironment& environment, ShaderInfo& info) {
    auto fog = info.combos.find("FOG");
    if (fog == info.combos.end() || fog->second == "0") return;

    if (environment.fog_distance) info.combos["FOG_DIST"] = "1";
    if (environment.fog_height) info.combos["FOG_HEIGHT"] = "1";
    if (environment.fog_distance || environment.fog_height) info.combos["FOG_COMPUTED"] = "1";
}

void ApplyLegacyAtmosphereUniformAliases(const wpscene::Material& material, ShaderInfo& info) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    info.baseConstSvs[rstd::cppstd::to_string(G_VIEWFORWARD)] = std::array { 0.0f, 0.0f, 1.0f };

    auto prefer_legacy = [&](std::string_view legacy, std::string_view current) {
        if (! material.constantshadervalues.contains(std::string(legacy))) return;
        auto current_it = info.alias.find(std::string(current));
        if (current_it == info.alias.end()) return;
        info.alias[std::string(legacy)] = current_it->second;
        info.alias.erase(current_it);
    };

    prefer_legacy("Planet position", "Position");
    prefer_legacy("Planet radius", "Planet size");
    prefer_legacy("Atmosphere radius", "Atmosphere size");
    prefer_legacy("Thickness", "Density falloff");
    prefer_legacy("Color", "Light color");
    prefer_legacy("Intensity", "Brightness");
}

void ReplaceAllInPlace(std::string& body, std::string_view needle, std::string_view repl) {
    for (std::size_t pos = 0; (pos = body.find(needle, pos)) != std::string::npos;
         pos += repl.size()) {
        body.replace(pos, needle.size(), repl);
    }
}

void ApplyLegacyAtmosphereShaderCompat(const wpscene::Material& material,
                                       std::vector<ShaderUnit>& units) {
    if (! IsLegacyAtmosphereMaterial(material)) return;
    for (auto& unit : units) {
        if (unit.stage != ShaderType::FRAGMENT) continue;
        ReplaceAllInPlace(unit.src,
                          "float pointDensity, opticalDepth;",
                          "float pointDensity = 0.0, opticalDepth = 0.0;");
        ReplaceAllInPlace(unit.src,
                          "float localDensity, cameraOpticalDepth, sunRayLength, "
                          "sunOpticalDepth, lightInstensity = 1.0;",
                          "float localDensity = 0.0, cameraOpticalDepth = 0.0, "
                          "sunRayLength = 0.0, sunOpticalDepth = 0.0, lightInstensity = 1.0;");
    }
}

bool IsLegacyAtmosphereShadowValue(const wpscene::Material& material, std::string_view name) {
    if (! IsLegacyAtmosphereMaterial(material)) return false;

    static constexpr std::string_view shadow_values[] = {
        "Position",    "Planet size", "Atmosphere size", "Density falloff",
        "Light color", "Brightness",  "Radius",
    };

    for (std::string_view shadow_value : shadow_values) {
        if (name == shadow_value) return true;
    }
    return false;
}

std::vector<SceneShaderDefaultTexture> ToSceneShaderDefaultTextures(const ShaderInfo& info) {
    std::vector<SceneShaderDefaultTexture> out;
    out.reserve(info.defTexs.size());
    for (const auto& [slot, texture] : info.defTexs) {
        out.push_back(SceneShaderDefaultTexture { .slot = slot, .texture = texture });
    }
    return out;
}

SceneShaderVariantDesc MakeSceneShaderVariantDesc(
    std::string_view scene_id, const wpscene::Material& material, const ShaderInfo& info,
    const Combos& input_combos, std::span<const ShaderUnit> units,
    std::span<const std::string> source_keys, std::span<const std::string> stage_sources,
    std::span<const ShaderTexInfo> texinfos, bool geometry_shader_enabled) {
    SceneShaderVariantDesc desc;
    desc.scene_id                = std::string(scene_id);
    desc.shader_name             = material.shader;
    desc.input_combos            = input_combos;
    desc.resolved_combos         = info.combos;
    desc.uniform_aliases         = info.alias;
    desc.default_uniforms        = info.svs;
    desc.default_textures        = ToSceneShaderDefaultTextures(info);
    desc.geometry_shader_enabled = geometry_shader_enabled;

    desc.texture_infos.reserve(texinfos.size());
    for (const auto& texinfo : texinfos) {
        desc.texture_infos.push_back(ToSceneShaderTextureCompileInfo(texinfo));
    }

    desc.stages.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        desc.stages.push_back(SceneShaderVariantStage {
            .stage      = units[i].stage,
            .source_key = i < source_keys.size() ? source_keys[i] : std::string {},
            .source     = i < stage_sources.size() ? stage_sources[i] : units[i].src,
        });
    }
    return desc;
}

auto BuildMaterial(fs::VFS& vfs, ShaderCache& shader_cache,
                   const SceneShaderEnvironment& environment, const wpscene::Material& wpmat,
                   Scene& scene, ShaderInfo shader_info, GeometryStageRequirement geometry_stage)
    -> Result<MaterialBuild, MaterialBuildError> {
    MaterialBuild build { .shader_info = rstd::move(shader_info) };
    auto&         material        = build.material;
    auto&         shader_info_ref = build.shader_info;
    auto          blend_mode      = ParseBlendMode(wpmat.blending);

    SceneMaterialCustomShader materialShader;

    auto& shader              = materialShader.shader;
    shader                    = std::make_shared<SceneShader>();
    shader->name              = wpmat.shader;
    shader->matrix_convention = ShaderMatrixConvention::RowVector;
    shader->matrix_abi        = ShaderMatrixAbi::Hlsl;
    std::string shaderPath("/assets/shaders/" + wpmat.shader);

    std::vector<ShaderUnit>  sd_units;
    std::vector<std::string> sd_source_keys;
    std::vector<std::string> sd_original_sources;
    auto                     add_shader_unit = [&](ShaderType stage, std::string source_key) {
        auto        loaded = fs::ReadFileContent(vfs, source_key);
        std::string source;
        if (loaded.is_ok()) {
            source = rstd::move(loaded).unwrap_unchecked();
        } else {
            rstd_error("Can't read shader source {}", source_key);
        }
        sd_source_keys.push_back(std::move(source_key));
        sd_original_sources.push_back(source);
        sd_units.push_back({
            .stage           = stage,
            .src             = std::move(source),
            .preprocess_info = {},
        });
    };
    add_shader_unit(ShaderType::VERTEX, shaderPath + ".vert");
    bool has_geometry_stage = geometry_stage == GeometryStageRequirement::Required;
    if (has_geometry_stage) {
        std::string geom_path = shaderPath + ".geom";
        if (vfs.metadata(fs::ToPath(geom_path)).is_err()) {
            rstd_error("required geometry shader source missing: {}", geom_path);
            return Err(MaterialBuildError {
                .message = String::make("required geometry shader source is missing"_str),
            });
        }
        add_shader_unit(ShaderType::GEOMETRY, std::move(geom_path));
        shader_info_ref.combos[rstd::cppstd::to_string(WE_CB_GS_ENABLED)] = "1";
    }
    add_shader_unit(ShaderType::FRAGMENT, shaderPath + ".frag");

    std::vector<ShaderTexInfo>   texinfos;
    HashMap<String, ImageHeader> tex_headers;
    for (const auto& el : wpmat.textures) {
        if (el.empty()) {
            texinfos.push_back({ false });
        } else if (! IsSpecTex(as_str(el).unwrap())) {
            auto parsed_header = scene.ParseImageHeader(rstd::cppstd::as_str(el).unwrap());
            auto texh = parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                              : ImageHeader {};
            if (texh.extraHeader.count("compo1") == 0) {
                texinfos.push_back({ false });
                (void)tex_headers.insert(String::make(as_str(el).unwrap()), rstd::move(texh));
                continue;
            }
            texinfos.push_back({ true,
                                 {
                                     (bool)texh.extraHeader.at("compo1").val,
                                     (bool)texh.extraHeader.at("compo2").val,
                                     (bool)texh.extraHeader.at("compo3").val,
                                     (bool)texh.extraHeader.at("compo4").val,
                                 } });
            (void)tex_headers.insert(String::make(as_str(el).unwrap()), rstd::move(texh));
        } else
            texinfos.push_back({ true });
    }

    for (auto& unit : sd_units) {
        unit.src =
            ShaderParser::PreShaderSrc(vfs, unit.src, &shader_info_ref, texinfos, &shader_cache);
    }
    ApplyLegacyAtmosphereUniformAliases(wpmat, shader_info_ref);
    ApplyLegacyAtmosphereShaderCompat(wpmat, sd_units);

    for (const auto& el : wpmat.combos) {
        shader_info_ref.combos[el.first] = std::to_string(el.second.to_primitive());
    }
    if (blend_mode == BlendMode::AlphaToCoverage) {
        shader_info_ref.combos["ALPHATOCOVERAGE"] = "1";
    }
    ApplySceneFogCombos(environment, shader_info_ref);
    ApplyLegacyAtmosphereLightCombo(wpmat, shader_info_ref);

    auto textures = wpmat.textures;
    if (shader_info_ref.defTexs.size() > 0) {
        for (auto& t : shader_info_ref.defTexs) {
            const auto index = rstd::as_cast<usize>(t.first).to_primitive();
            if (textures.size() > index) {
                if (! textures.at(index).empty()) continue;
            } else {
                textures.resize(index + 1);
            }
            textures[index] = t.second;
        }
    }

    for (std::size_t i = 0; i < textures.size(); i++) {
        std::string name = textures.at(i);
        ParseSpecTexName(name, wpmat, shader_info_ref, scene);
        material.textures.push_back(name);
        material.texture_metadata.emplace_back();
        material.defines.push_back("g_Texture" + std::to_string(i));
        if (name.empty()) {
            continue;
        }

        std::array<i32, 4> resolution {};
        auto               texture_name = as_str(name).unwrap();
        if (IsSpecTex(texture_name)) {
            auto target = scene.RenderTarget(as_str(name).unwrap());
            if (! IsSpecLinkTex(texture_name) && target.is_none()) {
                rstd_error("{} not found in render targes", name);
            } else if (target.is_some()) {
                const auto& rt = **target;
                resolution     = { i32(rt.width), i32(rt.height), i32(rt.width), i32(rt.height) };
            }
        } else {
            auto texh = [&] {
                if (auto header = tex_headers.get(as_str(name).unwrap()); header.is_some())
                    return **header;
                auto parsed_header = scene.ParseImageHeader(rstd::cppstd::as_str(name).unwrap());
                return parsed_header.is_ok() ? rstd::move(parsed_header).unwrap_unchecked()
                                             : ImageHeader {};
            }();
            if (i == 0) {
                if (texh.format == TextureFormat::R8)
                    shader_info_ref.combos["TEX0FORMAT"] = "FORMAT_R8";
                else if (texh.format == TextureFormat::RG8)
                    shader_info_ref.combos["TEX0FORMAT"] = "FORMAT_RG88";
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
            material.texture_metadata.back() = SceneMaterialTextureMetadata {
                .has_extent    = true,
                .source_extent = { rstd::as_cast<float>(resolution[0]),
                                   rstd::as_cast<float>(resolution[1]) },
                .sample_extent = { rstd::as_cast<float>(resolution[2]),
                                   rstd::as_cast<float>(resolution[3]) },
            };

            auto scene_texture = scene.Texture(rstd::cppstd::as_str(name).unwrap());
            if (scene_texture.is_none()) {
                SceneTexture stex;
                stex.sample  = texh.sample;
                stex.url     = name;
                stex.isVideo = texh.type == ImageType::VIDEO;
                if (texh.isSprite) {
                    stex.isSprite   = texh.isSprite;
                    stex.spriteAnim = texh.spriteAnim;
                }
                scene.RegisterTexture(String::make(rstd::cppstd::as_str(name).unwrap()),
                                      rstd::move(stex));
                scene_texture = scene.Texture(rstd::cppstd::as_str(name).unwrap());
            }
            if (scene_texture.is_some() && (**scene_texture).isSprite) {
                material.hasSprite = true;
                const auto& f1     = texh.spriteAnim.GetCurFrame();
                if (wpmat.shader == "genericparticle" || wpmat.shader == "genericropeparticle") {
                    shader_info_ref.combos[rstd::cppstd::to_string(WE_CB_SPRITESHEET)]  = "1";
                    shader_info_ref.combos[rstd::cppstd::to_string(WE_CB_THICK_FORMAT)] = "1";
                    if (algorism::IsPowOfTwo(rstd::as_cast<u32>(texh.width)) &&
                        algorism::IsPowOfTwo(rstd::as_cast<u32>(texh.height))) {
                        shader_info_ref
                            .combos[rstd::cppstd::to_string(WE_CB_SPRITESHEETBLENDNPOT)] = "1";
                        resolution[2] =
                            resolution[0] - resolution[0] % rstd::as_cast<i32>(f1.width);
                        resolution[3] =
                            resolution[1] - resolution[1] % rstd::as_cast<i32>(f1.height);
                        material.texture_metadata.back().sample_extent = {
                            rstd::as_cast<float>(resolution[2]), rstd::as_cast<float>(resolution[3])
                        };
                    }
                    materialShader.constValues[rstd::cppstd::to_string(G_RENDERVAR1)] =
                        std::array { f1.xAxis[0],
                                     f1.yAxis[1],
                                     static_cast<float>(texh.spriteAnim.numFrames().to_primitive()),
                                     f1.rate };
                }
            }
        }
        if (! resolution.empty()) {
            const std::string gResolution { WE_GLTEX_RESOLUTION_NAMES[usize(i)] };

            materialShader.constValues[gResolution] = array_cast<float>(resolution);
        }
    }
    if (exists(shader_info_ref.combos, rstd::cppstd::to_string(WE_CB_LIGHTING))) {
        if (environment.directional_shadow &&
            shader_info_ref.combos[rstd::cppstd::to_string(WE_CB_LIGHTING)] != "0") {
            shader_info_ref.combos["LIGHTS_SHADOW_MAPPING"]         = "1";
            shader_info_ref.combos["LIGHTS_SHADOW_MAPPING_QUALITY"] = "2";
        }
    }

    auto input_combos          = shader_info_ref.combos;
    shader_info_ref.combos     = ShaderParser::ResolveShaderCombos(shader_info_ref, input_combos);
    auto scene_id              = as_string_view(scene.SceneId());
    auto variant_desc          = MakeSceneShaderVariantDesc(scene_id,
                                                            wpmat,
                                                            shader_info_ref,
                                                            input_combos,
                                                            sd_units,
                                                            sd_source_keys,
                                                            sd_original_sources,
                                                            texinfos,
                                                            has_geometry_stage);
    variant_desc.texture_slots = material.textures;

    if (! ShaderParser::CompileToSpv(
            scene_id, sd_units, shader->codes, &shader_info_ref, texinfos, &shader_cache)) {
        return Err(MaterialBuildError {
            .message = String::make("shader compilation failed"_str),
        });
    }
    shader->default_uniforms      = shader_info_ref.svs;
    variant_desc.default_uniforms = shader_info_ref.svs;
    ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        variant_desc, sd_units, shader->codes);
    shader->sampler_bindings = variant_desc.sampler_bindings;
    shader->uniform_blocks   = variant_desc.uniform_blocks;
    shader->descriptor_sets  = variant_desc.descriptor_sets;

    material.blenmode    = blend_mode;
    material.depth_test  = ParseEnabled(wpmat.depthtest);
    material.depth_write = ParseEnabled(wpmat.depthwrite);
    material.cull_mode   = ParseCullMode(wpmat.cullmode);

    // FS is always the last unit (VS may be followed by optional GS, then FS).
    const auto& fs_active = sd_units.back().preprocess_info.active_tex_slots;
    for (unsigned i = 0; i < material.textures.size(); i++) {
        if (! exists(fs_active, i)) material.textures[i].clear();
    }

    for (const auto& el : shader_info_ref.baseConstSvs) {
        materialShader.constValues[el.first] = el.second;
    }
    // Register bindings only after AddMaterial places the material in its
    // stable mesh-owned allocation. Registering the stack-local pointer here
    // would leave a dangling binding after the move.
    for (const auto& var : shader_info_ref.scalar_uniforms) {
        if (! var.is_user || var.material.is_empty()) continue;
        auto uniform_name = rstd::cppstd::to_string(var.name.as_str());
        shader_info_ref.user_var_staging.push(UserVarRecord {
            .material      = var.material.clone(),
            .name          = var.name.clone(),
            .default_value = var.default_value.clone(),
        });
        if (auto value = shader->default_uniforms.find(uniform_name);
            value != shader->default_uniforms.end()) {
            materialShader.constValues[uniform_name] = value->second;
        }
    }

    material.customShader         = rstd::move(materialShader);
    material.customShader.variant = Some(rstd::move(variant_desc));
    material.name                 = wpmat.shader;

    return Ok(rstd::move(build));
}

bool IsLayerCompositeShader(std::string_view shader) {
    return shader == "genericimage" || shader == "genericimage2" || shader == "genericimage3" ||
           shader == "genericimage4" || shader == "passthrough";
}

std::string ResolveShaderMaterialKey(const ShaderInfo& info, const std::string& material_key) {
    if (auto it = info.alias.find(material_key); it != info.alias.end()) return it->second;

    for (const auto& el : info.alias) {
        if (el.second.size() > 2 && el.second.substr(2) == material_key) return el.second;
    }
    return {};
}

bool IsShaderPositionUniform(const ShaderInfo& info, const std::string& glname) {
    for (const auto& var : info.scalar_uniforms) {
        if (var.name == rstd::cppstd::as_str(glname).unwrap()) return var.position;
    }
    return false;
}

bool UsesEffectPositionSpace(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/spin" && wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == i32(1);
}

bool UsesUnitFinalQuad(const wpscene::Material& wpmat) {
    if (wpmat.shader != "effects/transform") return false;
    auto mode_it = wpmat.combos.find("MODE");
    return mode_it != wpmat.combos.end() && mode_it->second == i32(1);
}

bool CanCompositeFinalEffectShader(std::string_view shader) {
    return IsLayerCompositeShader(shader) || shader == "effects/transform" ||
           shader == "effects/scroll" || shader == "effects/spin" ||
           shader == "effects/perspective" || shader == "effects/foliagesway" ||
           shader == "effects/blend" || shader == "effects/tint";
}

bool HasShaderCombo(const ShaderInfo& info, std::string_view combo_name) {
    auto name = rstd::cppstd::as_str(combo_name).unwrap();
    for (const auto& combo : info.combo_defs)
        if (combo.combo == name) return true;
    return false;
}

bool HasShaderTextureMaterial(const ShaderInfo& info, std::string_view material_key) {
    auto key = rstd::cppstd::as_str(material_key).unwrap();
    for (const auto& texture : info.texture_uniforms)
        if (texture.material == key) return true;
    return false;
}

bool HasSolidCompositeContext(const SceneParseContext& context, const wpscene::ImageObject& obj) {
    if (obj.solid || context.solid_layer_ids.contains(obj.id)) return true;

    HashSet<i32> seen;
    u32          parent = obj.parent;
    while (parent != u32() && seen.insert(rstd::as_cast<i32>(parent))) {
        const auto parent_id = rstd::as_cast<i32>(parent);
        if (context.solid_layer_ids.contains(parent_id)) return true;

        auto found = context.object_parent_ids.get(parent_id);
        if (found.is_none()) break;
        parent = **found;
    }

    return false;
}

bool CanCompositeFinalEffectMaterial(std::string_view shader, const ShaderInfo& info,
                                     bool allow_transparent_previous) {
    if (CanCompositeFinalEffectShader(shader)) return true;
    if (! allow_transparent_previous) return false;

    // TODO: WE does not document this as the final-composite rule. This keeps
    // the historical shortcut only for non-solid layer contexts.
    return HasShaderCombo(info, "TRANSPARENCY") && HasShaderTextureMaterial(info, "previous");
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
//   (2) Instance-bound effect-internal keys from
//       `wpmat.constantshadervalues_user`, mapped through `info.alias` to
//       the GLSL uniform name.
//   (3) Legacy material `usershadervalues` bindings: project key to shader
//       material key.
void RegisterShaderUserVarIndexImpl(Scene* pScene, const std::shared_ptr<SceneMaterial>& stable_mat,
                                    const wpscene::Material& wpmat, const ShaderInfo& info) {
    if (! pScene || ! stable_mat) return;
    for (const auto& combo : info.combo_defs) {
        if (combo.material.is_empty() || combo.combo.is_empty()) continue;
        Scene::ShaderComboUserBinding binding {
            .material = stable_mat,
            .combo    = combo.combo.clone(),
            .fallback =
                String::make(as_str(std::to_string(combo.default_.to_primitive())).unwrap()),
        };
        combo.options.iter().for_each([&](auto entry) {
            auto [label, value] = entry;
            (void)binding.options.insert(
                label->clone(),
                String::make(as_str(std::to_string(value->to_primitive())).unwrap()));
        });
        pScene->RegisterShaderComboUserBinding(combo.material.clone(), rstd::move(binding));
    }
    for (const auto& rec : info.user_var_staging) {
        pScene->RegisterShaderUserBinding(rec.material.clone(), stable_mat, rec.name.clone());
    }
    for (const auto& [effect_key, wallpaper_key] : wpmat.constantshadervalues_user) {
        // Resolve effect-internal key → GLSL uniform name via alias.
        // LoadConstvalue's fallback search (alias entry whose value, after
        // dropping the leading "u_", matches the key) is honored here too.
        std::string glname = ResolveShaderMaterialKey(info, effect_key);
        if (glname.empty()) {
            rstd_warn("user binding '{}' → no shader uniform with material='{}'",
                      wallpaper_key,
                      effect_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(String::make(as_str(wallpaper_key).unwrap()),
                                          stable_mat,
                                          String::make(as_str(glname).unwrap()));
    }
    for (const auto& [wallpaper_key, material_key] : wpmat.user_shader_values) {
        std::string glname = ResolveShaderMaterialKey(info, material_key);
        if (glname.empty()) {
            rstd_warn("user shader value '{}' -> no shader uniform with material='{}'",
                      wallpaper_key,
                      material_key);
            continue;
        }
        pScene->RegisterShaderUserBinding(String::make(as_str(wallpaper_key).unwrap()),
                                          stable_mat,
                                          String::make(as_str(glname).unwrap()));
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
    if (type_string.is_none() || value_string.is_none() ||
        rstd::cppstd::as_string_view(*type_string) != "system")
        return None();
    auto name = rstd::cppstd::as_string_view(*value_string);
    if (name != "$mediaThumbnail" && name != "$mediaPreviousThumbnail") return None();
    return Some(String::make(*value_string));
}

bool IsSystemMediaTextureBinding(const Json& binding) {
    return UserTexturePropertyKey(binding).is_some() && binding.is_object();
}

std::string ResolveMaterialTextureFallback(Scene& scene, const wpscene::Material& fallback_material,
                                           const ShaderInfo& shader_info, usize slot) {
    std::string fallback;
    if (slot.to_primitive() < fallback_material.textures.size()) {
        fallback = fallback_material.textures[slot.to_primitive()];
    }
    if (fallback.empty()) {
        for (const auto& [index, texture] : shader_info.defTexs) {
            if (rstd::as_cast<usize>(index) == slot) {
                fallback = texture;
                break;
            }
        }
    }
    ParseSpecTexName(fallback, fallback_material, shader_info, scene);
    return fallback;
}

void RegisterMaterialUserTextureIndex(Scene*                                pScene,
                                      const std::shared_ptr<SceneMaterial>& stable_mat,
                                      const wpscene::Material&              fallback_material,
                                      const ShaderInfo&                     shader_info) {
    if (! pScene || ! stable_mat) return;
    for (usize i {}; i < fallback_material.usertextures.len(); ++i) {
        auto key = UserTexturePropertyKey(fallback_material.usertextures[i]);
        if (key.is_none()) continue;
        std::string fallback =
            ResolveMaterialTextureFallback(*pScene, fallback_material, shader_info, i);
        if (IsSystemMediaTextureBinding(fallback_material.usertextures[i]) &&
            i.to_primitive() < stable_mat->textures.size()) {
            fallback = stable_mat->textures[i.to_primitive()];
        }
        pScene->RegisterMaterialTextureUserBinding(
            rstd::move(*key),
            Scene::MaterialTextureUserBinding {
                .material = stable_mat,
                .slot     = rstd::as_cast<u32>(i),
                .fallback = String::make(as_str(fallback).unwrap()),
            });
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
void ApplyTextureBindsImpl(wpscene::Material&                             wpmat,
                           std::span<const wpscene::MaterialPassBindItem> binds,
                           const EffectRenderTargets&                     render_targets) {
    for (const auto& el : binds) {
        auto target = render_targets.get(as_str(el.name).unwrap());
        if (target.is_none()) {
            rstd_error("fbo {} not found", el.name);
            continue;
        }
        const auto index = rstd::as_cast<usize>(el.index).to_primitive();
        if (wpmat.textures.size() <= index) wpmat.textures.resize(index + 1);
        wpmat.textures[index] = rstd::cppstd::to_string((**target).as_str());
    }
}

std::string ResolveSceneTextureProperty(const SceneParseContext& context, std::string_view key) {
    if (context.user_properties.is_none()) return {};
    auto prop = (*context.user_properties)->get(rstd::cppstd::as_str(key).unwrap());
    if (prop.is_none()) return {};
    const auto& payload = **prop;
    if (payload.is_string()) {
        auto text = rstd::cppstd::to_string(*payload.as_str());
        return text.empty() ? std::string {} : text;
    }
    if (! payload.is_object()) return {};

    std::string type;
    if (auto value = payload.get("type"_str); value.is_some()) {
        auto string = (*value)->as_str();
        if (string.is_some()) type = rstd::cppstd::to_string(*string);
    }
    if (! type.empty() && type != "scenetexture" && type != "texture" && type != "replacetexture")
        return {};
    auto value = payload.get("value"_str);
    if (value.is_none()) return {};
    auto string = (*value)->as_str();
    return string.is_none() ? std::string {} : rstd::cppstd::to_string(*string);
}

std::string ResolveUserTextureProperty(const SceneParseContext& context, const Json& binding) {
    if (! binding.is_string()) return {};
    auto key = rstd::cppstd::to_string(*binding.as_str());
    return ResolveSceneTextureProperty(context, key);
}

std::string ResolveMaterialTextureSlot(const SceneParseContext& context,
                                       const wpscene::Material& material, usize slot) {
    std::string fallback;
    if (slot.to_primitive() < material.textures.size()) {
        fallback = material.textures[slot.to_primitive()];
    }
    if (slot >= material.usertextures.len()) return fallback;

    if (auto prop = ResolveUserTextureProperty(context, material.usertextures[slot]);
        ! prop.empty())
        return prop;
    return fallback;
}

bool CanUseImageAsSystemMediaFallback(const wpscene::ImageObject& image) {
    if (! image.puppet.empty()) return false;
    if (image.fullscreen || image.config.passthrough) return false;
    return CountVisibleImageEffects(image.effects) == i32();
}

std::string ResolveLinkedImageFallback(const SceneParseContext& context, std::string_view texture) {
    auto name      = as_str(texture).unwrap();
    auto linked_id = ParseImageLayerCompositeId(name);
    if (! linked_id && IsSpecLinkTex(name)) {
        linked_id = Some(ParseLinkTex(name));
    }
    if (! linked_id) return {};

    auto fallback = context.system_media_image_fallbacks.get(rstd::as_cast<i32>(*linked_id));
    return fallback.is_some() ? rstd::cppstd::to_string((**fallback).as_str()) : std::string {};
}

std::string ResolveSystemMediaFallback(const SceneParseContext& context,
                                       const wpscene::Material& material, usize slot) {
    if (slot.to_primitive() >= material.textures.size()) return {};
    return ResolveLinkedImageFallback(context, material.textures[slot.to_primitive()]);
}

void ApplyUserTextureBindings(SceneParseContext& context, wpscene::Material& material) {
    for (usize i {}; i < material.usertextures.len(); ++i) {
        const auto& binding = material.usertextures[i];
        if (binding.is_null()) continue;

        std::string resolved = ResolveUserTextureProperty(context, binding);
        if (resolved.empty() && IsSystemMediaTextureBinding(binding)) {
            resolved = ResolveSystemMediaFallback(context, material, i);
        }
        if (resolved.empty()) continue;

        if (material.textures.size() <= i.to_primitive()) {
            material.textures.resize(i.to_primitive() + 1);
        }
        material.textures[i.to_primitive()] = std::move(resolved);
    }
}

void IndexSystemMediaImageFallbacks(SceneParseContext& context, slice<SceneObjectVar> scene_objs) {
    context.system_media_image_fallbacks.clear();
    for (usize index {}; index < scene_objs.len(); ++index) {
        const auto& object = scene_objs[index];
        if (! object.is_Image()) continue;
        const auto& image = object.as_Image().value;
        if (! CanUseImageAsSystemMediaFallback(image)) continue;

        auto texture = ResolveMaterialTextureSlot(context, image.material, usize(0));
        if (texture.empty() || IsSpecTex(as_str(texture).unwrap())) continue;
        (void)context.system_media_image_fallbacks.insert(
            image.id, String::make(rstd::cppstd::as_str(texture).unwrap()));
    }
}

void LoadConstvalueImpl(SceneMaterial& material, const wpscene::Material& wpmat,
                        const ShaderInfo&             info,
                        SceneShaderValueAnimationMap* final_quad_shader_values) {
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name   = cs.first;
        const std::vector<float>& value  = cs.second;
        std::string               glname = ResolveShaderMaterialKey(info, name);
        if (glname.empty()) {
            if (IsLegacyAtmosphereShadowValue(wpmat, name)) continue;
            if (wpmat.constantshadervalues_animations.contains(name)) {
                rstd_warn("animated shader value '{}' has no uniform in '{}'", name, wpmat.shader);
            } else {
                rstd_debug(
                    "ignoring shader value '{}' without a uniform in '{}'", name, wpmat.shader);
            }
        } else {
            std::vector<float> const_value = value;
            bool               normalize_position =
                UsesEffectPositionSpace(wpmat) && IsShaderPositionUniform(info, glname);
            Option<SceneShaderValueAnimation> final_quad_value;
            if (normalize_position && const_value.size() >= 2) {
                final_quad_value       = Some(SceneShaderValueAnimation {});
                final_quad_value->base = ShaderValue(value);
                const_value[0]         = const_value[0] * 2.0f - 1.0f;
                const_value[1]         = const_value[1] * 2.0f - 1.0f;
            }
            material.SetShaderValue(
                glname,
                ShaderValue(std::span<const float>(const_value.data(), const_value.size())));
            if (auto it = wpmat.constantshadervalues_animations.find(name);
                it != wpmat.constantshadervalues_animations.end()) {
                auto curve = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(it->second));
                if (final_quad_value) final_quad_value->curve = Some(curve.clone());
                if (normalize_position) {
                    curve = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(it->second));
                    NormalizeEffectPositionCurve(*curve);
                }
                material.SetShaderValueAnimation(
                    String::make(rstd::cppstd::as_str(glname).unwrap()), rstd::move(curve));
            }
            if (final_quad_value && final_quad_shader_values) {
                (void)final_quad_shader_values->insert(
                    String::make(rstd::cppstd::as_str(glname).unwrap()),
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
    if (auto* scalar = std::get_if<script::ScalarValue>(&value))
        return Some(ShaderValue(static_cast<float>(scalar->v)));
    if (auto* boolean = std::get_if<script::BoolValue>(&value))
        return Some(ShaderValue(boolean->v ? 1.0f : 0.0f));
    if (auto* vector = std::get_if<script::Vec2Value>(&value))
        return Some(ShaderValue(rstd::array<float, 2> { static_cast<float>(vector->x),
                                                        static_cast<float>(vector->y) }));
    if (auto* vector = std::get_if<script::Vec3Value>(&value))
        return Some(ShaderValue(rstd::array<float, 3> { static_cast<float>(vector->x),
                                                        static_cast<float>(vector->y),
                                                        static_cast<float>(vector->z) }));
    if (auto* vector = std::get_if<script::Vec4Value>(&value))
        return Some(ShaderValue(rstd::array<float, 4> { static_cast<float>(vector->x),
                                                        static_cast<float>(vector->y),
                                                        static_cast<float>(vector->z),
                                                        static_cast<float>(vector->w) }));
    if (auto* color = std::get_if<script::ColorValue>(&value))
        return Some(ShaderValue(rstd::array<float, 3> { static_cast<float>(color->r),
                                                        static_cast<float>(color->g),
                                                        static_cast<float>(color->b) }));
    return None();
}

void WireMaterialShaderValueScripts(SceneParseContext& context, const Arc<SceneNode>& owner,
                                    const std::shared_ptr<SceneMaterial>& material,
                                    const wpscene::Material& wpmat, const ShaderInfo& info) {
    if (! material || wpmat.constantshadervalues_bindings.scripts.empty()) return;
    auto& scripts = EnsureScriptScene(context);
    for (const auto& [material_key, binding] : wpmat.constantshadervalues_bindings.scripts) {
        auto value = wpmat.constantshadervalues.find(material_key);
        if (value == wpmat.constantshadervalues.end()) continue;
        auto kind = ShaderValueScriptKind(usize(value->second.size()));
        if (kind == script::FieldKind::Unknown) continue;
        auto uniform_name = ResolveShaderMaterialKey(info, material_key);
        if (uniform_name.empty()) continue;

        auto  sha          = utils::genSha1(std::span<const char>(binding.source));
        auto* field_script = scripts.runtime().MakeFieldScript(
            binding.source, sha, kind, binding.properties, binding.initial_value, owner.as_ptr());
        if (! field_script) continue;
        SetScriptInitializationOrder(context, *field_script, owner.as_ptr());
        TrackRegisteredAssets(context, field_script);
        auto* scene = context.scene.get();
        scripts.AddActuator({
            field_script,
            [scene, material, uniform_name = rstd::move(uniform_name)](
                const script::ScriptValue& script_value) {
                auto value = ScriptValueAsShaderValue(script_value);
                if (value.is_none()) return;
                (void)scene->SetMaterialShaderValue(
                    *material, rstd::cppstd::as_str(uniform_name).unwrap(), *value);
            },
        });
    }
}

void RegisterMaterialBindings(Scene& scene, const std::shared_ptr<SceneMaterial>& material,
                              const wpscene::Material& authored, const ShaderInfo& shader_info,
                              Option<ref<wpscene::Material>> user_texture_fallback) {
    RegisterShaderUserVarIndexImpl(&scene, material, authored, shader_info);
    if (user_texture_fallback.is_some()) {
        RegisterMaterialUserTextureIndex(&scene, material, **user_texture_fallback, shader_info);
    }
}

void RegisterLayerPreviousBindings(Scene& scene, SceneMaterial& material,
                                   const wpscene::Material& authored, SceneNodeId layer,
                                   ref<str> composite_target) {
    for (usize index {}; index < usize(authored.textures.size()); ++index) {
        if (authored.textures[index.to_primitive()] !=
            rstd::cppstd::as_string_view(composite_target)) {
            continue;
        }
        (void)scene.SetMaterialLayerPreviousSource(
            material, rstd::as_cast<u32>(index), layer, composite_target);
    }
}

void ApplyTextureBinds(wpscene::Material&                             material,
                       std::span<const wpscene::MaterialPassBindItem> bindings,
                       const EffectRenderTargets&                     render_targets) {
    ApplyTextureBindsImpl(material, bindings, render_targets);
}

void LoadConstvalue(SceneMaterial& material, const wpscene::Material& authored,
                    const ShaderInfo&             shader_info,
                    SceneShaderValueAnimationMap* final_quad_shader_values) {
    LoadConstvalueImpl(material, authored, shader_info, final_quad_shader_values);
}

} // namespace owe
