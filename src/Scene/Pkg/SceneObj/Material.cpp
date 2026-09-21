module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd.log;
import rstd;
import wescene.json;

using namespace owe::wpscene;
using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::iter::from_slice;
using namespace rstd::literals;

namespace
{

void LoadUserShaderValues(const owe::Json& json, HashMap<String, String>& out) {
    auto values = json.get("usershadervalues"_str);
    if (values.is_none()) return;
    auto object = (*values)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto text                     = entry_value->as_str();
        if (text.is_some()) (void)out.insert(entry_key->clone(), rstd::into(*text));
    });
}

void MergeTextures(slice<String> src, Vec<String>& dst) {
    if (src.len() > dst.len()) dst.resize(src.len(), String {});
    for (auto [index, texture] : from_slice(src).enumerate()) {
        if (! texture->is_empty()) dst[index] = texture->clone();
    }
}

void MergeUserTextures(const rstd::json::Array& src, rstd::json::Array& dst) {
    while (src.len() > dst.len()) dst.push(owe::Json::Null());
    for (rstd::usize i = rstd::usize(); i < src.len(); ++i) {
        if (! src[i].is_null()) dst[i] = src[i].clone();
    }
}

void LoadConstantShaderValue(ref<str> name, const owe::Json& json,
                             HashMap<String, Vec<float>>& constant_values,
                             FieldBindings&               bindings) {
    Vec<float> value;
    owe::GetJsonValue(json, value);
    (void)constant_values.insert(rstd::into(name), rstd::move(value));
    if (! json.is_object()) return;

    (void)AbsorbFieldBinding(name, json, bindings);
}

} // namespace

auto owe::wpscene::Material::clone() const -> Material {
    Material clone;
    clone.blending                      = blending.clone();
    clone.cullmode                      = cullmode.clone();
    clone.shader                        = shader.clone();
    clone.alphawriting                  = alphawriting.clone();
    clone.depthtest                     = depthtest.clone();
    clone.depthwrite                    = depthwrite.clone();
    clone.textures                      = textures.clone();
    clone.combos                        = combos.clone();
    clone.constantshadervalues          = constantshadervalues.clone();
    clone.user_shader_values            = user_shader_values.clone();
    clone.use_puppet                    = use_puppet;
    clone.constantshadervalues_bindings = constantshadervalues_bindings.clone();
    MergeUserTextures(usertextures, clone.usertextures);
    return clone;
}

bool MaterialPassBindItem::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name"_str, name);
    owe::GetJsonValue(json, "index"_str, index);
    return true;
}

void MaterialPass::Update(const MaterialPass& p) {
    MergeTextures(p.textures.as_slice(), textures);
    for (auto [key, value] : p.constantshadervalues.iter()) {
        (void)constantshadervalues.insert(key->clone(), value->clone());
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (auto [key, value] : p.user_shader_values.iter()) {
        (void)user_shader_values.insert(key->clone(), value->clone());
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (auto [key, value] : p.combos.iter()) {
        (void)combos.insert(key->clone(), *value);
    }
}

void Material::MergePass(const MaterialPass& p) {
    MergeBindingOverrides(p.textures.as_slice(), p.usertextures, p.combos);
    for (auto [key, value] : p.constantshadervalues.iter()) {
        (void)constantshadervalues.insert(key->clone(), value->clone());
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (auto [key, value] : p.user_shader_values.iter()) {
        (void)user_shader_values.insert(key->clone(), value->clone());
    }
}

void Material::MergeBindingOverrides(slice<String> textures, const rstd::json::Array& usertextures,
                                     const HashMap<String, i32>& combos) {
    MergeTextures(textures, this->textures);
    MergeUserTextures(usertextures, this->usertextures);
    for (auto [key, value] : combos.iter()) {
        (void)this->combos.insert(key->clone(), *value);
    }
}

bool MaterialPass::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "id"_str, id, false);
    if (auto values = json.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                String tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push(rstd::move(tex));
            }
        }
    }
    if (auto values = json.get("usertextures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = json.get("constantshadervalues"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(entry_key->as_str(),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_bindings);
            });
    }
    LoadUserShaderValues(json, user_shader_values);
    if (auto values = json.get("combos"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                i32 value { 0 };
                owe::GetJsonValue(*entry_value, value);
                (void)combos.insert(entry_key->clone(), value);
            });
    }
    owe::GetJsonValue(json, "target"_str, target, false);
    if (auto values = json.get("bind"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jB : **array) {
                MaterialPassBindItem bindItem;
                bindItem.FromJson(jB);
                bind.push(rstd::move(bindItem));
            }
        }
    }
    return true;
}

bool Material::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool Material::FromJson(const owe::Json& json, SceneVersion /*v*/) {
    auto passes = json.get("passes"_str);
    if (passes.is_none()) {
        rstd_error("material no data");
        return false;
    }
    auto pass_array = (*passes)->as_array();
    if (pass_array.is_none() || (*pass_array)->is_empty()) {
        rstd_error("material no data");
        return false;
    }
    const auto& jContent = (**pass_array)[rstd::usize()];
    if (jContent.get("shader"_str).is_none()) {
        rstd_error("material no shader");
        return false;
    }
    owe::GetJsonValue(jContent, "blending"_str, blending);
    owe::GetJsonValue(jContent, "cullmode"_str, cullmode);
    owe::GetJsonValue(jContent, "alphawriting"_str, alphawriting, false);
    owe::GetJsonValue(jContent, "depthtest"_str, depthtest);
    owe::GetJsonValue(jContent, "depthwrite"_str, depthwrite);
    owe::GetJsonValue(jContent, "shader"_str, shader);
    if (auto values = jContent.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                String tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push(rstd::move(tex));
            }
        }
    }
    if (auto values = jContent.get("usertextures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& jU : **array) usertextures.push(jU.clone());
    }
    if (auto values = jContent.get("constantshadervalues"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                LoadConstantShaderValue(entry_key->as_str(),
                                        *entry_value,
                                        constantshadervalues,
                                        constantshadervalues_bindings);
            });
    }
    LoadUserShaderValues(jContent, user_shader_values);
    if (auto values = jContent.get("combos"_str); values.is_some()) {
        auto object = (*values)->as_object();
        if (object.is_some())
            (*object)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                i32 value { 0 };
                owe::GetJsonValue(*entry_value, value);
                (void)combos.insert(entry_key->clone(), value);
            });
    }
    return true;
}
