module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd;
import rstd.log;
import wescene.json;

using namespace rstd::prelude;
using namespace owe::wpscene;
using namespace rstd::literals;

namespace
{

auto LoadJsonFile(owe::fs::VFS& vfs, ref<str> path) -> Option<owe::Json> {
    auto parsed = owe::ReadJsonFile(vfs, owe::fs::Path(path));
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return None();
    }
    return Some(rstd::move(parsed).unwrap_unchecked());
}

constexpr ref<str>     kFoliageSwayEffect = "effects/foliagesway/effect.json"_str;
constexpr SceneVersion kNormalizedFoliageSwayStrengthVersion = 9;

auto LoadImageAssetJson(ImageObject& object, owe::fs::VFS& vfs, SceneVersion version,
                        bool explicit_no_copy_background) -> Option<owe::Json> {
    auto json = LoadJsonFile(vfs, rstd::format("/assets/{}", object.image).as_str());
    if (json.is_none()) return None();

    owe::GetJsonValue(*json, "fullscreen"_str, object.fullscreen, false);
    owe::GetJsonValue(*json, "passthrough"_str, object.config.passthrough, false);
    owe::GetJsonValue(*json, "nopadding"_str, object.nopadding, false);
    owe::GetJsonValue(*json, "solidlayer"_str, object.solid_layer, false);
    owe::GetJsonValue(*json, "puppet"_str, object.puppet, false);

    if (! owe::GetJsonValue(*json, "material"_str, object.material_path, false)) {
        rstd_info("image object no material");
        return None();
    }
    auto material_json =
        LoadJsonFile(vfs, rstd::format("/assets/{}", object.material_path).as_str());
    if (material_json.is_none()) return None();
    object.material.FromJson(*material_json, version);
    if (object.composite_layer && explicit_no_copy_background)
        (void)object.material.combos.insert("CLEARALPHA"_Str, i32(1));
    return json;
}

void ScaleAnimCurve(AnimCurve& curve, float scale) {
    auto scale_axis = [scale](Vec<AnimKeyframe>& keys) {
        for (auto& key : keys) {
            key.value *= scale;
            key.front.y *= scale;
            key.back.y *= scale;
        }
    };
    scale_axis(curve.c0);
    scale_axis(curve.c1);
    scale_axis(curve.c2);
}

void NormalizeLegacyFoliageSwayStrength(MaterialPass& pass) {
    constexpr float scale = 0.01f;
    auto            value = pass.constantshadervalues.get_mut("strength"_str);
    if (value.is_some()) {
        for (float& component : **value) component *= scale;
    }
    auto binding = pass.constantshadervalues_bindings.GetMut("strength"_str);
    if (binding.is_some() && (**binding).animation.is_some())
        ScaleAnimCurve(*(**binding).animation, scale);
}

} // namespace

float owe::wpscene::NormalizeLayerAlpha(float alpha) {
    // Older WE scene JSON stores layer alpha as 0..100 percent.
    if (alpha > 1.0f) alpha /= 100.0f;
    return rstd::cmp::min(1.0f, rstd::cmp::max(0.0f, alpha));
}

bool EffectCommand::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "command"_str, command);
    owe::GetJsonValue(json, "target"_str, target);
    owe::GetJsonValue(json, "source"_str, source);
    return true;
}

bool ObjectInstance::FromJson(const owe::Json& json) {
    present = true;
    owe::GetJsonValue(json, "id"_str, id, false);
    if (auto values = json.get("combos"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                i32 value { 0 };
                if (owe::GetJsonValue(*entry_value, value) &&
                    ! combos.contains_key(entry_key->as_str()))
                    (void)combos.insert(entry_key->clone(), value);
            });
    }
    if (auto values = json.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) {
                String texture;
                auto   string = value.as_str();
                if (string.is_some()) texture = rstd::into(*string);
                textures.push(rstd::move(texture));
            }
        }
    }
    if (auto values = json.get("usertextures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& value : **array) usertextures.push(value.clone());
        }
    }
    return true;
}

void ObjectInstance::ApplyTo(Material& material) const {
    material.MergeBindingOverrides(textures.as_slice(), usertextures, combos);
}

bool EffectFbo::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name"_str, name);
    owe::GetJsonValue(json, "format"_str, format);
    owe::GetJsonValue(json, "scale"_str, scale);
    owe::GetJsonValue(json, "fit"_str, fit, false);
    owe::GetJsonValue(json, "unique"_str, unique, false);
    if (scale == u32()) {
        rstd_error("fbo scale can't be 0");
        scale = u32(1);
    }
    return true;
}

bool ImageEffect::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ImageEffect::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    String filePath;
    owe::GetJsonValue(json, "file"_str, filePath);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name.clone();
    AbsorbAllFieldBindings(json, field_bindings);
    owe::GetJsonValue(json, "username"_str, username, false);
    owe::GetJsonValue(json, "id"_str, id, false);
    auto jEffect = LoadJsonFile(vfs, rstd::format("/assets/{}", filePath).as_str());
    if (! jEffect) return false;
    if (! FromFileJson(*jEffect, vfs)) return false;
    String instance_name;
    owe::GetJsonValue(json, "name"_str, instance_name, false);
    // SceneScript addresses effects by their authored instance names.
    if (! instance_name.is_empty()) name = rstd::move(instance_name);

    if (auto injected_passes = json.get("passes"_str); injected_passes.is_some()) {
        auto array = (*injected_passes)->as_array();
        if (array.is_none()) return true;
        if ((*array)->len() > passes.len()) {
            rstd_error("passes is not injective");
            return false;
        }
        usize i {};
        for (const auto& jP : **array) {
            MaterialPass pass;
            pass.FromJson(jP);
            if (filePath.as_str() == kFoliageSwayEffect && v != kSceneVersionUnknown &&
                v < kNormalizedFoliageSwayStrengthVersion)
                NormalizeLegacyFoliageSwayStrength(pass);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool ImageEffect::FromFileJson(const owe::Json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "version"_str, version, false);
    owe::GetJsonValue(json, "name"_str, name);
    if (auto values = json.get("fbos"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jF : **array) {
                EffectFbo fbo;
                fbo.FromJson(jF);
                fbos.push(rstd::move(fbo));
            }
        }
    }
    if (auto effect_passes = json.get("passes"_str); effect_passes.is_some()) {
        auto array = (*effect_passes)->as_array();
        if (array.is_none()) {
            rstd_error("passes in effect file is not an array");
            return false;
        }
        bool compose { false };
        for (const auto& jP : **array) {
            if (jP.get("material"_str).is_none()) {
                if (jP.get("command"_str).is_some()) {
                    EffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = rstd::as_cast<i32>(passes.len());
                    commands.push(rstd::move(cmd));
                    continue;
                }
                rstd_error("no material in effect pass");
                return false;
            }
            String matPath;
            owe::GetJsonValue(jP, "material"_str, matPath);
            auto jMat = LoadJsonFile(vfs, rstd::format("/assets/{}", matPath).as_str());
            if (! jMat) return false;
            Material material;
            material.FromJson(*jMat);
            materials.push(rstd::move(material));
            MaterialPass pass;
            pass.FromJson(jP);
            passes.push(rstd::move(pass));
            if (jP.get("compose"_str).is_some()) owe::GetJsonValue(jP, "compose"_str, compose);
        }
        if (compose) {
            if (passes.len() != usize(2)) {
                rstd_error("effect compose option error");
                return false;
            }
            EffectFbo fbo;
            {
                fbo.name  = "_rt_FullCompoBuffer1"_Str;
                fbo.scale = u32(1);
            }
            fbos.push(rstd::move(fbo));
            passes.first_mut().unwrap()->bind.push({ "previous"_Str, i32() });
            passes.first_mut().unwrap()->target = "_rt_FullCompoBuffer1"_Str;
            passes[usize(1)].bind.push({ "_rt_FullCompoBuffer1"_Str, i32() });
        }
    } else {
        rstd_error("no passes in effect file");
        return false;
    }
    return true;
}

namespace
{

void ReadImageEffects(const owe::Json& json, owe::fs::VFS& vfs, SceneVersion version,
                      Vec<ImageEffect>& effects) {
    auto values = json.get("effects"_str);
    if (values.is_none()) return;
    auto array = (*values)->as_array();
    if (array.is_none()) return;

    for (const auto& value : **array) {
        ImageEffect effect;
        if (effect.FromJson(value, vfs, version)) effects.push(rstd::move(effect));
    }
}

} // namespace

bool ImageObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

Option<ImageAssetInfo> owe::wpscene::LoadImageAssetInfo(fs::VFS& vfs, ref<str> image) {
    auto j_image = LoadJsonFile(vfs, rstd::format("/assets/{}", image).as_str());
    if (j_image.is_none()) return None();

    ImageAssetInfo info;
    owe::GetJsonValue(*j_image, "solidlayer"_str, info.solid_layer, false);
    i32 w {}, h {};
    if (j_image->get("width"_str).is_some() && j_image->get("height"_str).is_some()) {
        owe::GetJsonValue(*j_image, "width"_str, w, false);
        owe::GetJsonValue(*j_image, "height"_str, h, false);
        if (w > i32() && h > i32()) {
            info.size = Some(array<float, 2> { static_cast<float>(w.to_primitive()),
                                               static_cast<float>(h.to_primitive()) });
            return Some(rstd::move(info));
        }
    }

    String mat_path;
    if (! owe::GetJsonValue(*j_image, "material"_str, mat_path, false))
        return Some(rstd::move(info));
    auto j_mat = LoadJsonFile(vfs, rstd::format("/assets/{}", mat_path).as_str());
    if (j_mat.is_none()) return Some(rstd::move(info));
    Material mat;
    if (mat.FromJson(*j_mat) && ! mat.textures.is_empty())
        info.first_texture = mat.textures[usize()].clone();
    return Some(rstd::move(info));
}

bool ImageObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    owe::GetJsonValue(json, "image"_str, image);
    composite_layer = image == "models/util/composelayer.json"_str;
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name.clone();
    owe::GetJsonValue(json, "alignment"_str, alignment, false);
    bool copy_background_value { true };
    bool explicit_no_copy_background =
        owe::GetJsonValue(json, "copybackground"_str, copy_background_value, false) &&
        ! copy_background_value;
    auto jImage = LoadImageAssetJson(*this, vfs, v, explicit_no_copy_background);
    if (! jImage) return false;
    owe::GetJsonValue(json, "name"_str, name, false);
    owe::GetJsonValue(json, "id"_str, id, false);
    owe::GetJsonValue(json, "colorBlendMode"_str, colorBlendMode, false);
    ReadParallaxDepth(json, parallax);
    if (! fullscreen) {
        owe::GetJsonValue(json, "origin"_str, origin);
        owe::GetJsonValue(json, "angles"_str, angles);
        owe::GetJsonValue(json, "scale"_str, scale);
        if (jImage->get("width"_str).is_some()) {
            i32 w {}, h {};
            owe::GetJsonValue(*jImage, "width"_str, w);
            owe::GetJsonValue(*jImage, "height"_str, h);
            size = { static_cast<float>(w.to_primitive()), static_cast<float>(h.to_primitive()) };
        } else if (json.get("size"_str).is_some()) {
            owe::GetJsonValue(json, "size"_str, size);
        } else {
            size = { origin[usize(0)] * 2, origin[usize(1)] * 2 };
        }
    }
    owe::GetJsonValue(json, "color"_str, color, false);
    ReadUserValueBinding(json, "color"_str, color_user);
    color_user_key = color_user.name.clone();
    owe::GetJsonValue(json, "alpha"_str, alpha, false);
    alpha = NormalizeLayerAlpha(alpha);
    ReadUserValueBinding(json, "alpha"_str, alpha_user);
    alpha_user_key = alpha_user.name.clone();
    owe::GetJsonValue(json, "brightness"_str, brightness, false);

    ReadImageEffects(json, vfs, v, effects);
    ReadPuppetAnimationLayers(json, puppet_layers);
    if (auto config_json = json.get("config"_str); config_json.is_some()) {
        owe::GetJsonValue(**config_json, "passthrough"_str, config.passthrough, false);
    }

    owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
    owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
    owe::GetJsonValue(json, "parent"_str, parent, false);
    owe::GetJsonValue(json, "attachment"_str, attachment, false);
    owe::GetJsonValue(json, "perspective"_str, perspective, false);
    owe::GetJsonValue(json, "reflected"_str, reflected, false);
    owe::GetJsonValue(json, "copybackground"_str, copybackground, false);
    owe::GetJsonValue(json, "solid"_str, solid, false);
    owe::GetJsonValue(json, "opaquebackground"_str, opaquebackground, false);
    owe::GetJsonValue(json, "clampuvs"_str, clampuvs, false);
    owe::GetJsonValue(json, "castshadow"_str, castshadow, false);
    owe::GetJsonValue(json, "disablepropagation"_str, disablepropagation, false);
    owe::GetJsonValue(json, "depthtest"_str, depthtest, false);
    owe::GetJsonValue(json, "backgroundcolor"_str, backgroundcolor, false);
    owe::GetJsonValue(json, "backgroundbrightness"_str, backgroundbrightness, false);
    owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
    if (auto instance_json = json.get("instance"_str);
        instance_json.is_some() && (*instance_json)->is_object()) {
        instance.FromJson(**instance_json);
        instance.ApplyTo(material);
    }
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}

bool ImageObject::FromAsset(rstd::ref<rstd::str> asset, array<float, 2> asset_size, fs::VFS& vfs,
                            SceneVersion version) {
    image           = rstd::into(asset);
    name            = rstd::format("__createLayer:{}", asset);
    size            = { asset_size[rstd::usize()], asset_size[rstd::usize(1)] };
    composite_layer = image == "models/util/composelayer.json"_str;
    return LoadImageAssetJson(*this, vfs, version, false).is_some();
}

bool ShapeObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion v) {
    owe::GetJsonValue(json, "shape"_str, shape);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name.clone();
    owe::GetJsonValue(json, "name"_str, name, false);
    owe::GetJsonValue(json, "id"_str, id, false);
    owe::GetJsonValue(json, "origin"_str, origin);
    owe::GetJsonValue(json, "angles"_str, angles);
    owe::GetJsonValue(json, "scale"_str, scale);
    ReadParallaxDepth(json, parallax);

    ReadImageEffects(json, vfs, v, effects);

    owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
    owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
    owe::GetJsonValue(json, "reflected"_str, reflected, false);
    owe::GetJsonValue(json, "castshadow"_str, castshadow, false);
    owe::GetJsonValue(json, "disablepropagation"_str, disablepropagation, false);
    owe::GetJsonValue(json, "parent"_str, parent, false);
    owe::GetJsonValue(json, "attachment"_str, attachment, false);
    owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
