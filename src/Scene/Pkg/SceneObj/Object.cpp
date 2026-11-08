module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd;
import rstd.log;
import wescene.core;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace owe::wpscene;

bool ContainerObject::FromJson(const owe::Json& json) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "id"_str, id, false);
    owe::GetJsonValue(json, "name"_str, name, false);
    owe::GetJsonValue(json, "origin"_str, origin, false);
    ReadLayerScale(json, scale, false);
    owe::GetJsonValue(json, "angles"_str, angles, false);
    ReadParallaxDepth(json, parallax);
    ReadVisibleProperty(json, visible, visible_user);
    owe::GetJsonValue(json, "solid"_str, solid, false);
    owe::GetJsonValue(json, "disablepropagation"_str, disable_propagation, false);
    owe::GetJsonValue(json, "parent"_str, parent, false);
    owe::GetJsonValue(json, "attachment"_str, attachment, false);
    owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}

namespace
{

template<typename T, typename Make>
Option<SceneObject> DecodeObject(const owe::Json& authored, owe::fs::VFS& vfs, SceneVersion version,
                                 Make make) {
    T value;
    if (! value.FromJson(authored, vfs, version)) return None();
    return Some(make(rstd::move(value)));
}

} // namespace

namespace owe::wpscene
{

Option<SceneObject> DecodeSceneObject(const owe::Json& authored, mut_ref<fs::VFS> vfs,
                                      SceneVersion version) {
    switch (ClassifySceneObject(authored)) {
    case SceneObjectKind::Container: {
        ContainerObject value;
        if (value.FromJson(authored)) return Some(SceneObject::Container(rstd::move(value)));
        return None();
    }
    case SceneObjectKind::Image:
        return DecodeObject<ImageObject>(authored, *vfs, version, [](ImageObject value) {
            return SceneObject::Image(rstd::move(value));
        });
    case SceneObjectKind::Shape:
        return DecodeObject<ShapeObject>(authored, *vfs, version, [](ShapeObject value) {
            return SceneObject::Shape(rstd::move(value));
        });
    case SceneObjectKind::Particle:
        return DecodeObject<ParticleObject>(authored, *vfs, version, [](ParticleObject value) {
            return SceneObject::Particle(rstd::move(value));
        });
    case SceneObjectKind::Sound:
        return DecodeObject<SoundObject>(authored, *vfs, version, [](SoundObject value) {
            return SceneObject::Sound(rstd::move(value));
        });
    case SceneObjectKind::Light:
        return DecodeObject<LightObject>(authored, *vfs, version, [](LightObject value) {
            return SceneObject::Light(rstd::move(value));
        });
    case SceneObjectKind::Text:
        return DecodeObject<TextObject>(authored, *vfs, version, [](TextObject value) {
            return SceneObject::Text(rstd::move(value));
        });
    case SceneObjectKind::Model:
        return DecodeObject<ModelObject>(authored, *vfs, version, [](ModelObject value) {
            return SceneObject::Model(rstd::move(value));
        });
    case SceneObjectKind::Camera:
        return DecodeObject<CameraObject>(authored, *vfs, version, [](CameraObject value) {
            return SceneObject::Camera(rstd::move(value));
        });
    case SceneObjectKind::Unknown: return None();
    }
    return None();
}

Vec<SceneObject> DecodeSceneObjects(ref<SceneDocument> document, mut_ref<fs::VFS> vfs) {
    Vec<SceneObject> objects;
    objects.reserve(document->objects.len());
    for (const auto& record : document->objects) {
        if (record.metadata.kind == SceneObjectKind::Unknown ||
            (record.metadata.kind == SceneObjectKind::Container && ! record.metadata.has_id))
            continue;
        auto object = DecodeSceneObject(record.authored, vfs, document->metadata.pkg_version);
        if (object.is_some())
            objects.push(rstd::move(*object));
        else
            rstd_error("parse scene object failed, name: {}", record.metadata.name);
    }
    return objects;
}

} // namespace owe::wpscene
