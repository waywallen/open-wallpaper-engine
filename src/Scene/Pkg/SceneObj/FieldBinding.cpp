module;

module wescene.pkg.scene_obj;
import rstd;
import rstd.cppstd;

using namespace rstd::literals;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

namespace
{
Atomic<u64> next_field_binding_identity { u64(1) };
}

namespace owe::wpscene
{

bool ParseAnimKeyframeTangent(const owe::Json& json, AnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    out.enabled = true;
    owe::GetJsonValue(json, "enabled", out.enabled, false);
    owe::GetJsonValue(json, "x", out.x, false);
    owe::GetJsonValue(json, "y", out.y, false);
    owe::GetJsonValue(json, "magic", out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const owe::Json& json, AnimKeyframe& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame", out.frame, false);
    owe::GetJsonValue(json, "value", out.value, false);
    owe::GetJsonValue(json, "step", out.step, false);
    owe::GetJsonValue(json, "lockangle", out.lockangle, false);
    owe::GetJsonValue(json, "locklength", out.locklength, false);
    if (auto front = json.get("front"_str); front.is_some())
        ParseAnimKeyframeTangent(**front, out.front);
    if (auto back = json.get("back"_str); back.is_some())
        ParseAnimKeyframeTangent(**back, out.back);
    return true;
}

bool ParseAnimAxis(const owe::Json& json, std::vector<AnimKeyframe>& out) {
    if (! json.is_array()) return false;
    auto array = json.as_array();
    out.reserve((*array)->len().to_primitive());
    for (const auto& jK : **array) {
        AnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push_back(std::move(k));
    }
    return true;
}

bool ParseAnimEvent(const owe::Json& json, AnimEvent& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame", out.frame, false);
    owe::GetJsonValue(json, "name", out.name, false);
    return ! out.name.empty();
}

bool ParseAnimOptions(const owe::Json& json, AnimOptions& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "fps", out.fps, false);
    owe::GetJsonValue(json, "length", out.length, false);
    owe::GetJsonValue(json, "mode", out.mode, false);
    owe::GetJsonValue(json, "name", out.name, false);
    owe::GetJsonValue(json, "startpaused", out.startpaused, false);
    owe::GetJsonValue(json, "wraploop", out.wraploop, false);
    if (auto value = json.get("smoothing"_str); value.is_some()) out.smoothing = (*value)->clone();
    if (auto value = json.get("parent"_str); value.is_some()) {
        std::string key;
        if (owe::GetJsonValue(**value, "key", key, false) && ! key.empty())
            out.parent = Some(String::make(rstd::cppstd::as_str(key).unwrap()));
    }
    if (auto value = json.get("children"_str); value.is_some()) {
        if (auto array = (*value)->as_array(); array.is_some()) {
            for (const auto& entry : **array) {
                std::string key;
                if (owe::GetJsonValue(entry, "key", key, false) && ! key.empty())
                    out.children.push(String::make(rstd::cppstd::as_str(key).unwrap()));
            }
        }
    }
    if (auto value = json.get("events"_str); value.is_some()) {
        if (auto array = (*value)->as_array(); array.is_some()) {
            for (const auto& entry : **array) {
                AnimEvent event;
                if (ParseAnimEvent(entry, event)) out.events.push_back(std::move(event));
            }
        }
    }
    return true;
}

auto AnimOptions::clone() const -> AnimOptions {
    AnimOptions result;
    result.fps         = fps;
    result.length      = length;
    result.mode        = mode;
    result.name        = name;
    result.startpaused = startpaused;
    result.wraploop    = wraploop;
    result.smoothing   = smoothing.clone();
    result.parent      = parent.is_some() ? Some(parent->clone()) : None();
    for (const auto& child : children) result.children.push(child.clone());
    result.events = events;
    return result;
}

bool ParseAnimCurve(const owe::Json& json, AnimCurve& out) {
    if (! json.is_object()) return false;
    if (auto value = json.get("c0"_str); value.is_some()) ParseAnimAxis(**value, out.c0);
    if (auto value = json.get("c1"_str); value.is_some()) ParseAnimAxis(**value, out.c1);
    if (auto value = json.get("c2"_str); value.is_some()) ParseAnimAxis(**value, out.c2);
    if (auto value = json.get("options"_str); value.is_some())
        ParseAnimOptions(**value, out.options);
    owe::GetJsonValue(json, "relative", out.relative, false);
    return true;
}

auto AnimCurve::clone() const -> AnimCurve {
    AnimCurve result;
    result.c0       = c0;
    result.c1       = c1;
    result.c2       = c2;
    result.options  = options.clone();
    result.relative = relative;
    return result;
}

auto ScriptBinding::clone() const -> ScriptBinding {
    return ScriptBinding {
        .source        = source,
        .initial_value = initial_value.clone(),
    };
}

auto FieldBindingSpec::clone() const -> FieldBindingSpec {
    return FieldBindingSpec {
        .identity  = identity,
        .field     = field.clone(),
        .animation = animation.is_some() ? Some(animation->clone()) : None(),
        .script_properties =
            script_properties.is_some() ? Some(script_properties->clone()) : None(),
        .script = script.is_some() ? Some(script->clone()) : None(),
        .user   = user.is_some() ? Some(user->clone()) : None(),
    };
}

auto FieldBindingSpec::ScriptProperties() const noexcept -> const owe::Json& {
    static const auto empty = owe::Json::Null();
    return script_properties.is_some() ? *script_properties : empty;
}

auto FieldBindings::Get(ref<str> field) const noexcept -> Option<ref<FieldBindingSpec>> {
    for (const auto& binding : entries) {
        if (binding.field == field)
            return Some(ref<FieldBindingSpec>::from_raw_parts(rstd::addressof(binding)));
    }
    return None();
}

auto FieldBindings::GetMut(ref<str> field) noexcept -> Option<mut_ref<FieldBindingSpec>> {
    for (auto& binding : entries) {
        if (binding.field == field)
            return Some(mut_ref<FieldBindingSpec>::from_raw_parts(rstd::addressof(binding)));
    }
    return None();
}

auto FieldBindings::Ensure(ref<str> field) -> mut_ref<FieldBindingSpec> {
    auto binding = GetMut(field);
    if (binding.is_some()) return *binding;
    entries.push(FieldBindingSpec {
        .identity = next_field_binding_identity.fetch_add(u64(1), Ordering::Relaxed),
        .field    = String::make(field),
    });
    return mut_ref<FieldBindingSpec>::from_raw_parts(
        rstd::addressof(entries[entries.len() - usize(1)]));
}

bool FieldBindings::HasAnimation(ref<str> field) const noexcept {
    auto binding = Get(field);
    return binding.is_some() && (**binding).animation.is_some();
}

bool FieldBindings::HasScript(ref<str> field) const noexcept {
    auto binding = Get(field);
    return binding.is_some() && (**binding).script.is_some();
}

auto FieldBindings::clone() const -> FieldBindings {
    FieldBindings result;
    result.entries.reserve(entries.len());
    for (const auto& binding : entries) result.entries.push(binding.clone());
    return result;
}

void FieldBindings::Update(const FieldBindings& other) {
    for (const auto& binding : other.entries) *Ensure(binding.field.as_str()) = binding.clone();
}

std::size_t AbsorbFieldBinding(std::string_view field, const owe::Json& field_value,
                               FieldBindings& out) {
    if (! field_value.is_object()) return 0;
    std::size_t count = 0;
    if (auto animation = field_value.get("animation"_str); animation.is_some()) {
        AnimCurve curve;
        if (ParseAnimCurve(**animation, curve)) {
            out.Ensure(rstd::cppstd::as_str(field).unwrap())->animation = Some(rstd::move(curve));
            ++count;
        }
    }
    if (auto properties = field_value.get("scriptproperties"_str); properties.is_some()) {
        out.Ensure(rstd::cppstd::as_str(field).unwrap())->script_properties =
            Some((*properties)->clone());
        ++count;
    }
    if (auto user = field_value.get("user"_str); user.is_some()) {
        auto key = (*user)->as_str();
        if (key.is_some()) {
            out.Ensure(rstd::cppstd::as_str(field).unwrap())->user = Some(String::make(*key));
            ++count;
        }
    }
    auto script = field_value.get("script"_str);
    if (script.is_some() && (*script)->is_string()) {
        ScriptBinding binding;
        binding.source = rstd::cppstd::to_string(*(*script)->as_str());
        if (auto value = field_value.get("value"_str); value.is_some())
            binding.initial_value = (*value)->clone();
        out.Ensure(rstd::cppstd::as_str(field).unwrap())->script = Some(rstd::move(binding));
        ++count;
    }
    return count;
}

std::size_t AbsorbAllFieldBindings(const owe::Json& obj_json, FieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n      = 0;
    auto        object = obj_json.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  field             = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& field_value       = *entry_value;
        n += AbsorbFieldBinding(field, field_value, out);
    });
    return n;
}

} // namespace owe::wpscene
