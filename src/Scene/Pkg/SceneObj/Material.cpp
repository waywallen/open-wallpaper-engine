module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import wescene.json;

using namespace owe::wpscene;
using namespace rstd::literals;

namespace
{

void LoadUserShaderValues(const owe::Json&                              json,
                          std::unordered_map<std::string, std::string>& out) {
    auto values = json.get("usershadervalues"_str);
    if (values.is_none()) return;
    auto object = (*values)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto text                     = entry_value->as_str();
        if (text.is_some())
            out[rstd::cppstd::to_string(entry_key->as_str())] = rstd::cppstd::to_string(*text);
    });
}

void MergeUserTextures(const rstd::json::Array& src, rstd::json::Array& dst) {
    while (src.len() > dst.len()) dst.push(owe::Json::Null());
    for (rstd::usize i = rstd::usize(); i < src.len(); ++i) {
        if (! src[i].is_null()) dst[i] = src[i].clone();
    }
}

void LoadConstantShaderValue(std::string name, const owe::Json& json,
                             std::unordered_map<std::string, std::vector<float>>& constant_values,
                             FieldBindings&                                       bindings) {
    std::vector<float> value;
    owe::GetJsonValue(json, value);
    constant_values[name] = std::move(value);
    if (! json.is_object()) return;

    (void)AbsorbFieldBinding(name, json, bindings);
}

} // namespace

auto owe::wpscene::Material::clone() const -> Material {
    Material clone;
    clone.blending                      = blending;
    clone.cullmode                      = cullmode;
    clone.shader                        = shader;
    clone.alphawriting                  = alphawriting;
    clone.depthtest                     = depthtest;
    clone.depthwrite                    = depthwrite;
    clone.textures                      = textures;
    clone.combos                        = combos;
    clone.constantshadervalues          = constantshadervalues;
    clone.user_shader_values            = user_shader_values;
    clone.use_puppet                    = use_puppet;
    clone.constantshadervalues_bindings = constantshadervalues_bindings.clone();
    MergeUserTextures(usertextures, clone.usertextures);
    return clone;
}

bool MaterialPassBindItem::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "index", index);
    return true;
}

void MaterialPass::Update(const MaterialPass& p) {
    std::size_t i = 0;
    for (const auto& el : p.textures) {
        if (p.textures.size() > textures.size()) textures.resize(p.textures.size());
        if (! el.empty()) {
            textures[i] = el;
        }
        ++i;
    }
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
    MergeUserTextures(p.usertextures, usertextures);
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

void Material::MergePass(const MaterialPass& p) {
    MergeBindingOverrides(p.textures, p.usertextures, p.combos);
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    constantshadervalues_bindings.Update(p.constantshadervalues_bindings);
    for (const auto& el : p.user_shader_values) {
        user_shader_values[el.first] = el.second;
    }
}

void Material::MergeBindingOverrides(const std::vector<std::string>&             textures,
                                     const rstd::json::Array&                    usertextures,
                                     const std::unordered_map<std::string, i32>& combos) {
    if (textures.size() > this->textures.size()) this->textures.resize(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (! textures[i].empty()) this->textures[i] = textures[i];
    }
    MergeUserTextures(usertextures, this->usertextures);
    for (const auto& el : combos) {
        this->combos[el.first] = el.second;
    }
}

bool MaterialPass::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "id", id, false);
    if (auto values = json.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
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
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
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
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    owe::GetJsonValue(json, "target", target, false);
    if (auto values = json.get("bind"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jB : **array) {
                MaterialPassBindItem bindItem;
                bindItem.FromJson(jB);
                bind.push_back(bindItem);
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
    owe::GetJsonValue(jContent, "blending", blending);
    owe::GetJsonValue(jContent, "cullmode", cullmode);
    owe::GetJsonValue(jContent, "alphawriting", alphawriting, false);
    owe::GetJsonValue(jContent, "depthtest", depthtest);
    owe::GetJsonValue(jContent, "depthwrite", depthwrite);
    owe::GetJsonValue(jContent, "shader", shader);
    if (auto values = jContent.get("textures"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& jT : **array) {
                std::string tex;
                if (! jT.is_null()) owe::GetJsonValue(jT, tex);
                textures.push_back(std::move(tex));
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
                LoadConstantShaderValue(rstd::cppstd::to_string(entry_key->as_str()),
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
                combos[rstd::cppstd::to_string(entry_key->as_str())] = value;
            });
    }
    return true;
}
