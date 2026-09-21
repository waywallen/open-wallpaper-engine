module wescene.pkg.parse;
import rstd;
import wescene.json;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;

namespace owe::wpscene
{

namespace
{

bool ReadString(const Json& json, ref<str> key, String& output) {
    auto value = json.get(key);
    if (value.is_none()) return false;
    auto text = (**value).as_str();
    if (text.is_none()) return false;
    output = String::make(*text);
    return true;
}

void ReadMap(const Json& json, ref<str> key, HashMap<String, i32>& output) {
    auto values = json.get(key);
    if (values.is_none()) return;
    auto object = (*values)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        int value {};
        owe::GetJsonValue(*entry_value, value);
        (void)output.insert(String::make(entry_key->as_str()), i32(value));
    });
}

} // namespace

bool UniformTex::FromJson(const Json& json) {
    (void)ReadString(json, "material"_str, material);
    (void)ReadString(json, "label"_str, label);
    (void)ReadString(json, "default"_str, default_);
    (void)ReadString(json, "mode"_str, mode);
    (void)ReadString(json, "combo"_str, combo);
    if (auto values = json.get("components"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& element : **array) {
                Component component;
                (void)ReadString(element, "label"_str, component.label);
                (void)ReadString(element, "combo"_str, component.combo);
                components.push(rstd::move(component));
            }
        }
    }
    owe::GetJsonValue(json, "requireany"_str, requireany, false);
    ReadMap(json, "require"_str, require);

    owe::GetJsonValue(json, "hidden"_str, hidden, false);
    owe::GetJsonValue(json, "nonremovable"_str, nonremovable, false);
    (void)ReadString(json, "group"_str, group);
    owe::GetJsonValue(json, "linked"_str, linked, false);
    (void)ReadString(json, "format"_str, format);
    owe::GetJsonValue(json, "formatcombo"_str, formatcombo, false);
    owe::GetJsonValue(json, "direction"_str, direction, false);
    (void)ReadString(json, "conversion"_str, conversion);
    int order_value {};
    owe::GetJsonValue(json, "order"_str, order_value, false);
    order = i32(order_value);
    return true;
}

bool UniformVar::FromJson(const Json& json, String uniform_name) {
    name    = rstd::move(uniform_name);
    is_user = name->starts_with("u_"_str);
    (void)ReadString(json, "material"_str, material);
    (void)ReadString(json, "label"_str, label);
    (void)ReadString(json, "group"_str, group);
    (void)ReadString(json, "type"_str, type);
    owe::GetJsonValue(json, "position"_str, position, false);
    owe::GetJsonValue(json, "linked"_str, linked, false);
    owe::GetJsonValue(json, "nobindings"_str, nobindings, false);
    if (auto values = json.get("range"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some() && (*array)->len() >= usize(2)) {
            has_range = owe::GetJsonValue((**array)[usize()], range[usize()]) &&
                        owe::GetJsonValue((**array)[usize(1)], range[usize(1)]);
        }
    }
    if (auto value = json.get("default"_str); value.is_some()) default_value = (*value)->clone();
    return true;
}

bool Combo::FromJson(const Json& json) {
    (void)ReadString(json, "material"_str, material);
    (void)ReadString(json, "combo"_str, combo);
    (void)ReadString(json, "type"_str, type);
    int default_value {};
    owe::GetJsonValue(json, "default"_str, default_value, false);
    default_ = i32(default_value);
    ReadMap(json, "options"_str, options);
    ReadMap(json, "require"_str, require);
    return true;
}

} // namespace owe::wpscene
