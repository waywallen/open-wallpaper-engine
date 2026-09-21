module;

module owe.user_property;

import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace owe
{

namespace
{

Json MakeDescriptor(Json value) {
    auto object = rstd::json::Map::make();
    object.insert("value"_Str, rstd::move(value));
    return Json::Object(rstd::move(object));
}

String DescriptorType(const Json& descriptor) {
    auto type = descriptor.get("type"_str);
    if (type.is_none()) return String::make();
    auto string = (**type).as_str();
    return string.is_some() ? String::make(*string) : String::make();
}

Json ParseWireValue(const Json& schema, const Json& value) {
    if (! value.is_string()) return value.clone();
    const auto type = DescriptorType(schema);
    if (type.is_empty() || type == "textinput"_str) return value.clone();

    auto parsed = rstd::json::from_str(*value.as_str(), { .allow_comments = true });
    return parsed.is_ok() ? parsed.unwrap() : value.clone();
}

} // namespace

Json MakeUserPropertyWirePatch(ref<str> value) {
    return MakeDescriptor(rstd::into<Json>(String::make(value)));
}

Json MergeUserPropertyDescriptor(const Json& schema, const Json& patch) {
    const Json* value = &patch;
    if (auto member = patch.get("value"_str); member.is_some()) value = &**member;
    const bool typed_patch = patch.get("type"_str).is_some();

    Json descriptor   = schema.is_object() ? schema.clone() : MakeDescriptor(value->clone());
    auto object       = descriptor.as_object_mut();
    Json merged_value = typed_patch ? value->clone() : ParseWireValue(schema, *value);
    if (object.is_none()) return MakeDescriptor(rstd::move(merged_value));
    (*object)->insert("value"_Str, rstd::move(merged_value));
    return descriptor;
}

} // namespace owe
