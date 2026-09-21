module;

module wescene.pkg.scene_obj;
import rstd;

using namespace rstd::prelude;
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
    owe::GetJsonValue(json, "enabled"_str, out.enabled, false);
    owe::GetJsonValue(json, "x"_str, out.x, false);
    owe::GetJsonValue(json, "y"_str, out.y, false);
    owe::GetJsonValue(json, "magic"_str, out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const owe::Json& json, AnimKeyframe& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame"_str, out.frame, false);
    owe::GetJsonValue(json, "value"_str, out.value, false);
    owe::GetJsonValue(json, "step"_str, out.step, false);
    owe::GetJsonValue(json, "lockangle"_str, out.lockangle, false);
    owe::GetJsonValue(json, "locklength"_str, out.locklength, false);
    if (auto front = json.get("front"_str); front.is_some())
        ParseAnimKeyframeTangent(**front, out.front);
    if (auto back = json.get("back"_str); back.is_some())
        ParseAnimKeyframeTangent(**back, out.back);
    return true;
}

bool ParseAnimAxis(const owe::Json& json, Vec<AnimKeyframe>& out) {
    if (! json.is_array()) return false;
    auto array = json.as_array();
    out.reserve((*array)->len());
    for (const auto& jK : **array) {
        AnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push(rstd::move(k));
    }
    return true;
}

bool ParseAnimEvent(const owe::Json& json, AnimEvent& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame"_str, out.frame, false);
    owe::GetJsonValue(json, "name"_str, out.name, false);
    return ! out.name.is_empty();
}

bool ParseAnimOptions(const owe::Json& json, AnimOptions& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "fps"_str, out.fps, false);
    owe::GetJsonValue(json, "length"_str, out.length, false);
    owe::GetJsonValue(json, "mode"_str, out.mode, false);
    owe::GetJsonValue(json, "name"_str, out.name, false);
    owe::GetJsonValue(json, "startpaused"_str, out.startpaused, false);
    owe::GetJsonValue(json, "wraploop"_str, out.wraploop, false);
    if (auto value = json.get("smoothing"_str); value.is_some()) out.smoothing = (*value)->clone();
    if (auto value = json.get("parent"_str); value.is_some()) {
        String key;
        if (owe::GetJsonValue(**value, "key"_str, key, false) && ! key.is_empty())
            out.parent = Some(rstd::move(key));
    }
    if (auto value = json.get("children"_str); value.is_some()) {
        if (auto array = (*value)->as_array(); array.is_some()) {
            for (const auto& entry : **array) {
                String key;
                if (owe::GetJsonValue(entry, "key"_str, key, false) && ! key.is_empty())
                    out.children.push(rstd::move(key));
            }
        }
    }
    if (auto value = json.get("events"_str); value.is_some()) {
        if (auto array = (*value)->as_array(); array.is_some()) {
            for (const auto& entry : **array) {
                AnimEvent event;
                if (ParseAnimEvent(entry, event)) out.events.push(rstd::move(event));
            }
        }
    }
    return true;
}

auto AnimOptions::clone() const -> AnimOptions {
    AnimOptions result;
    result.fps         = fps;
    result.length      = length;
    result.mode        = mode.clone();
    result.name        = name.clone();
    result.startpaused = startpaused;
    result.wraploop    = wraploop;
    result.smoothing   = smoothing.clone();
    result.parent      = parent.is_some() ? Some(parent->clone()) : None();
    for (const auto& child : children) result.children.push(child.clone());
    result.events = events.clone();
    return result;
}

bool ParseAnimCurve(const owe::Json& json, AnimCurve& out) {
    if (! json.is_object()) return false;
    if (auto value = json.get("c0"_str); value.is_some()) ParseAnimAxis(**value, out.c0);
    if (auto value = json.get("c1"_str); value.is_some()) ParseAnimAxis(**value, out.c1);
    if (auto value = json.get("c2"_str); value.is_some()) ParseAnimAxis(**value, out.c2);
    if (auto value = json.get("options"_str); value.is_some())
        ParseAnimOptions(**value, out.options);
    owe::GetJsonValue(json, "relative"_str, out.relative, false);
    return true;
}

auto AnimCurve::clone() const -> AnimCurve {
    AnimCurve result;
    result.c0       = c0.clone();
    result.c1       = c1.clone();
    result.c2       = c2.clone();
    result.options  = options.clone();
    result.relative = relative;
    return result;
}

auto ScriptBinding::clone() const -> ScriptBinding {
    return ScriptBinding {
        .source        = source.clone(),
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
    return entries.iter().find([field](ref<FieldBindingSpec> binding) {
        return binding->field == field;
    });
}

auto FieldBindings::GetMut(ref<str> field) noexcept -> Option<mut_ref<FieldBindingSpec>> {
    return entries.iter_mut().find([field](mut_ref<FieldBindingSpec> binding) {
        return binding->field == field;
    });
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

usize AbsorbFieldBinding(ref<str> field, const owe::Json& field_value, FieldBindings& out) {
    if (! field_value.is_object()) return usize();
    usize count {};
    if (auto animation = field_value.get("animation"_str); animation.is_some()) {
        AnimCurve curve;
        if (ParseAnimCurve(**animation, curve)) {
            out.Ensure(field)->animation = Some(rstd::move(curve));
            ++count;
        }
    }
    if (auto properties = field_value.get("scriptproperties"_str); properties.is_some()) {
        out.Ensure(field)->script_properties = Some((*properties)->clone());
        ++count;
    }
    if (auto user = field_value.get("user"_str); user.is_some()) {
        auto key = (*user)->as_str();
        if (key.is_some()) {
            out.Ensure(field)->user = Some(String::make(*key));
            ++count;
        }
    }
    auto script = field_value.get("script"_str);
    if (script.is_some() && (*script)->is_string()) {
        ScriptBinding binding;
        binding.source = rstd::into(*(*script)->as_str());
        if (auto value = field_value.get("value"_str); value.is_some())
            binding.initial_value = (*value)->clone();
        out.Ensure(field)->script = Some(rstd::move(binding));
        ++count;
    }
    return count;
}

usize AbsorbAllFieldBindings(const owe::Json& obj_json, FieldBindings& out) {
    if (! obj_json.is_object()) return usize();
    usize n {};
    auto  object = obj_json.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  field             = entry_key->as_str();
        const auto& field_value       = *entry_value;
        n += AbsorbFieldBinding(field, field_value, out);
    });
    return n;
}

} // namespace owe::wpscene
