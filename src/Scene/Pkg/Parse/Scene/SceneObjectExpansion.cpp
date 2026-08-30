module;

#include <rstd/enum.hpp>

module wescene.pkg.parse;
import :scene_context;
import wescene.spec_names;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::mtp::any;
using rstd::mtp::same;
using namespace owe;

namespace
{

auto UserPropertyValue(Option<ref<rstd::json::Map>> user_properties, std::string_view key)
    -> Option<ref<Json>> {
    if (key.empty()) return None();
    if (user_properties.is_none()) return None();
    auto value = (*user_properties)->get(rstd::cppstd::as_str(key).unwrap());
    if (value.is_none()) return None();
    const auto& payload = SceneUserPropertyPayload(**value);
    return Some(ref<Json>::from_raw_parts(rstd::addressof(payload)));
}

SceneUserVisibilityBinding
ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding& binding) {
    return SceneUserVisibilityBinding {
        .key           = String::make(rstd::cppstd::as_str(binding.name).unwrap()),
        .condition     = binding.condition.clone(),
        .has_condition = binding.has_condition,
    };
}

void CollectLinkedSourceId(ref<str> value, HashSet<i32>& out, Option<i32> effect_owner = None()) {
    if (auto id = ParseImageLayerCompositeId(value);
        id.is_some() && (effect_owner.is_none() || rstd::as_cast<i32>(*id) != *effect_owner))
        out.insert(rstd::as_cast<i32>(*id));
    if (IsSpecLinkTex(value)) out.insert(rstd::as_cast<i32>(ParseLinkTex(value)));
}

void CollectLinkedSourceIdsFromValue(const Json& value, HashSet<i32>& out,
                                     Option<i32> effect_owner = None()) {
    if (value.is_string()) {
        auto text = *value.as_str();
        CollectLinkedSourceId(text, out, effect_owner);
        return;
    }
    if (value.is_array()) {
        auto values = value.as_array();
        for (const auto& element : **values)
            CollectLinkedSourceIdsFromValue(element, out, effect_owner);
        return;
    }
    auto object = value.as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [_, entry_value] = entry;
        CollectLinkedSourceIdsFromValue(*entry_value, out, effect_owner);
    });
}

void CollectLinkedSourceId(std::string_view value, HashSet<i32>& out,
                           Option<i32> effect_owner = None()) {
    CollectLinkedSourceId(rstd::cppstd::as_str(value).unwrap(), out, effect_owner);
}

void CollectLinkedSourceIds(const wpscene::Material& material, HashSet<i32>& out,
                            Option<i32> effect_owner = None()) {
    for (const auto& texture : material.textures) CollectLinkedSourceId(texture, out, effect_owner);
    for (const auto& binding : material.usertextures)
        CollectLinkedSourceIdsFromValue(binding, out, effect_owner);
}

void CollectLinkedSourceIds(const wpscene::MaterialPass& pass, HashSet<i32>& out,
                            Option<i32> effect_owner = None()) {
    for (const auto& texture : pass.textures) CollectLinkedSourceId(texture, out, effect_owner);
    for (const auto& binding : pass.usertextures)
        CollectLinkedSourceIdsFromValue(binding, out, effect_owner);
    for (const auto& bind : pass.bind) CollectLinkedSourceId(bind.name, out, effect_owner);
}

void CollectLinkedSourceIds(const wpscene::ImageEffect& effect, HashSet<i32>& out,
                            Option<i32> effect_owner = None()) {
    for (const auto& material : effect.materials)
        CollectLinkedSourceIds(material, out, effect_owner);
    for (const auto& pass : effect.passes) CollectLinkedSourceIds(pass, out, effect_owner);
    for (const auto& command : effect.commands) {
        CollectLinkedSourceId(command.target, out, effect_owner);
        CollectLinkedSourceId(command.source, out, effect_owner);
    }
    for (const auto& fbo : effect.fbos) CollectLinkedSourceId(fbo.name, out, effect_owner);
}

void CollectLinkedSourceIds(const wpscene::Particle& particle, HashSet<i32>& out) {
    CollectLinkedSourceIds(particle.material, out);
    for (const auto& child : particle.children) CollectLinkedSourceIds(child.obj, out);
}

template<typename Dependencies>
void CollectExternalDependencies(const Dependencies& dependencies, i32 owner, HashSet<i32>& out) {
    for (auto id : dependencies) {
        if (id != owner) out.insert(i32(id));
    }
}

HashSet<i32> CollectLinkedSourceIds(slice<SceneObjectVar> objects) {
    HashSet<i32> out;
    for (usize index {}; index < objects.len(); ++index) {
        const auto& object = objects[index];
        RSTD_MATCH(object) {
            RSTD_CASE(Container, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
            }
            RSTD_CASE(Image, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIds(value.material, out);
                for (const auto& effect : value.effects)
                    CollectLinkedSourceIds(effect, out, Some(i32(value.id)));
                for (const auto& texture : value.instance.textures)
                    CollectLinkedSourceId(texture, out);
                for (const auto& binding : value.instance.usertextures)
                    CollectLinkedSourceIdsFromValue(binding, out);
            }
            RSTD_CASE(Shape, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                for (const auto& effect : value.effects)
                    CollectLinkedSourceIds(effect, out, Some(i32(value.id)));
            }
            RSTD_CASE(Particle, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIds(value.particleObj, out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
                CollectLinkedSourceIdsFromValue(value.particlesrc, out);
            }
            RSTD_CASE(Sound, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
            }
            RSTD_CASE(Light, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
            }
            RSTD_CASE(Text, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
                for (const auto& effect : value.effects)
                    CollectLinkedSourceIds(effect, out, Some(i32(value.id)));
            }
            RSTD_CASE(Model, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
            }
            RSTD_CASE(Camera, value) {
                CollectExternalDependencies(value.dependencies, i32(value.id), out);
                CollectLinkedSourceIdsFromValue(value.instance, out);
            }
        }
    }
    return out;
}

bool ResolveVisibleUserBinding(bool& visible, const wpscene::VisibleUserBinding& binding,
                               Option<ref<rstd::json::Map>> user_properties) {
    if (binding.empty()) return false;
    auto value = UserPropertyValue(user_properties, binding.name);
    if (value.is_some()) {
        if (auto resolved =
                ResolveSceneUserVisibilityBinding(ToSceneUserVisibilityBinding(binding), **value))
            visible = *resolved;
    }
    return true;
}

struct ObjectVisibilityInfo {
    u32  parent {};
    bool visible { true };
    bool user_bound { false };
};

auto BuildObjectVisibilityInfo(ref<wpscene::SceneDocument>  document,
                               Option<ref<rstd::json::Map>> user_properties)
    -> HashMap<i32, ObjectVisibilityInfo> {
    HashMap<i32, ObjectVisibilityInfo> result;
    for (const auto& record : document->objects) {
        const auto& metadata = record.metadata;
        if (metadata.kind == wpscene::SceneObjectKind::Unknown || ! metadata.has_id) continue;
        ObjectVisibilityInfo info {
            .parent     = metadata.parent,
            .visible    = metadata.visible,
            .user_bound = ! metadata.visible_user.empty(),
        };
        ResolveVisibleUserBinding(info.visible, metadata.visible_user, user_properties);
        (void)result.insert(metadata.id, rstd::move(info));
    }
    return result;
}

bool HasHiddenUserAncestor(u32 id, const HashMap<i32, ObjectVisibilityInfo>& objects) {
    HashSet<u32> seen;
    auto         object = objects.get(rstd::as_cast<i32>(id));
    if (object.is_none()) return false;
    auto parent = (**object).parent;
    while (parent != u32() && seen.insert(parent)) {
        auto parent_object = objects.get(rstd::as_cast<i32>(parent));
        if (parent_object.is_none()) return false;
        if ((**parent_object).user_bound && ! (**parent_object).visible) return true;
        parent = (**parent_object).parent;
    }
    return false;
}

HashSet<i32> CollectHiddenLinkedSourceIds(ref<wpscene::SceneDocument>  document,
                                          const HashSet<i32>&          linked_source_ids,
                                          Option<ref<rstd::json::Map>> user_properties) {
    HashSet<i32> result;
    auto         visibility = BuildObjectVisibilityInfo(document, user_properties);
    linked_source_ids.iter().for_each([&](ref<i32> linked_id) {
        auto info = visibility.get(*linked_id);
        if (info.is_none()) return;
        if (! (**info).visible || HasHiddenUserAncestor(rstd::as_cast<u32>(*linked_id), visibility))
            result.insert(*linked_id);
    });
    return result;
}

template<typename T>
bool PrepareSceneObject(T& object, Option<ref<rstd::json::Map>> user_properties,
                        ref<HashSet<i32>> linked_source_ids, bool force_invisible) {
    ResolveVisibleUserBinding(object.visible, object.visible_user, user_properties);
    if constexpr (any<T, wpscene::ImageObject, wpscene::ShapeObject>) {
        for (auto& effect : object.effects)
            ResolveVisibleUserBinding(effect.visible, effect.visible_user, user_properties);
    }
    if (force_invisible) object.visible = false;
    const bool linked         = ! object.visible && linked_source_ids->contains(object.id);
    const bool user_bound     = ! object.visible && ! object.visible_user.empty();
    const bool visible_script = ! object.visible && object.field_bindings.HasScript("visible"_str);
    constexpr bool keep_text  = same<T, wpscene::TextObject>;
    if constexpr (! same<T, wpscene::ImageObject>) {
        constexpr bool keep_user_visibility = ! same<T, wpscene::SoundObject>;
        if (! object.visible && ! linked && ! keep_text &&
            ! (keep_user_visibility && (user_bound || visible_script)))
            return false;
        if (linked) object.visible = true;
    }
    return true;
}

Vec<SceneObjectVar> FilterSceneObjects(Vec<SceneObjectVar>          decoded,
                                       ref<wpscene::SceneDocument>  document,
                                       Option<ref<rstd::json::Map>> user_properties,
                                       ref<HashSet<i32>>            linked_source_ids) {
    Vec<SceneObjectVar> result;
    result.reserve(decoded.len());
    auto visibility = BuildObjectVisibilityInfo(document, user_properties);
    for (auto& object : decoded) {
        auto force_invisible = [&](i32 id) {
            return HasHiddenUserAncestor(rstd::as_cast<u32>(id), visibility);
        };
        bool keep = object.visit_mut([&]<auto Tag>(rstd::choice_tag<Tag>, auto& payload) {
            auto& value = payload.value;
            if constexpr (Tag == SceneObjectVar::Tag::Container) {
                ResolveVisibleUserBinding(value.visible, value.visible_user, user_properties);
                if (force_invisible(value.id)) value.visible = false;
                return true;
            } else {
                return PrepareSceneObject(
                    value, user_properties, linked_source_ids, force_invisible(value.id));
            }
        });
        if (keep) result.push(rstd::move(object));
    }
    return result;
}

} // namespace

namespace owe
{

auto ExpandSceneObjects(ref<wpscene::SceneDocument> document, mut_ref<fs::VFS> vfs,
                        Option<ref<rstd::json::Map>> user_properties) -> ExpandedSceneObjects {
    auto decoded = wpscene::DecodeSceneObjects(document, vfs);
    auto linked  = CollectLinkedSourceIds(decoded.as_slice());
    auto hidden  = CollectHiddenLinkedSourceIds(document, linked, user_properties);
    auto objects = FilterSceneObjects(rstd::move(decoded),
                                      document,
                                      user_properties,
                                      ref<HashSet<i32>>::from_raw_parts(rstd::addressof(linked)));
    return ExpandedSceneObjects {
        .objects                = rstd::move(objects),
        .linked_source_ids      = rstd::move(linked),
        .hidden_link_source_ids = rstd::move(hidden),
    };
}

Vec<SceneObjectVar> ExpandObjects(const Json& json, fs::VFS& vfs, wpscene::SceneVersion version,
                                  Option<ref<rstd::json::Map>> user_properties) {
    wpscene::SceneDocument document;
    document.metadata.pkg_version = version;
    document.objects = wpscene::ParseSceneObjectRecords(json, document.objects_are_array);
    if (! document.objects_are_array) return {};
    return ExpandObjects(ref<wpscene::SceneDocument>::from_raw_parts(rstd::addressof(document)),
                         mut_ref<fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
                         user_properties);
}

Vec<SceneObjectVar> ExpandObjects(ref<wpscene::SceneDocument> document, mut_ref<fs::VFS> vfs,
                                  Option<ref<rstd::json::Map>> user_properties) {
    return ExpandSceneObjects(document, vfs, user_properties).objects;
}

array<i32, 2> ResolveOrthoProjectionExtent(const wpscene::SceneMetadata& metadata,
                                           slice<SceneObjectVar>         objects) {
    auto width  = metadata.general.orthogonalprojection.width;
    auto height = metadata.general.orthogonalprojection.height;
    if (! metadata.general.orthogonalprojection.auto_) return { width, height };
    width  = i32();
    height = i32();
    for (usize index {}; index < objects.len(); ++index) {
        const auto& object = objects[index];
        if (! object.is_Image()) continue;
        const auto& image = object.as_Image().value;
        const auto  area  = rstd::as_cast<i32>(image.size[0] * image.size[1]);
        if (area > width * height) {
            width  = rstd::as_cast<i32>(image.size[0]);
            height = rstd::as_cast<i32>(image.size[1]);
        }
    }
    return { width, height };
}

} // namespace owe
