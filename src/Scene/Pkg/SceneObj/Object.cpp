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
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "scale", scale, false);
    owe::GetJsonValue(json, "angles", angles, false);
    ReadParallaxDepth(json, parallax);
    ReadVisibleProperty(json, visible, visible_user);
    owe::GetJsonValue(json, "solid", solid, false);
    owe::GetJsonValue(json, "disablepropagation", disable_propagation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}

namespace
{

template<typename T, typename Make>
void DecodeObject(Vec<SceneObject>& objects, const SceneObjectRecord& record, owe::fs::VFS& vfs,
                  SceneVersion version, Make make) {
    T value;
    if (! value.FromJson(record.authored, vfs, version)) {
        rstd_error("parse scene object failed, name: {}", record.metadata.name);
        return;
    }
    objects.push(make(rstd::move(value)));
}

} // namespace

namespace owe::wpscene
{

Vec<SceneObject> DecodeSceneObjects(ref<SceneDocument> document, mut_ref<fs::VFS> vfs) {
    Vec<SceneObject> objects;
    objects.reserve(document->objects.len());
    for (const auto& record : document->objects) {
        switch (record.metadata.kind) {
        case SceneObjectKind::Container: {
            if (! record.metadata.has_id) break;
            ContainerObject value;
            if (value.FromJson(record.authored))
                objects.push(SceneObject::Container(rstd::move(value)));
            break;
        }
        case SceneObjectKind::Image:
            DecodeObject<ImageObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](ImageObject value) {
                    return SceneObject::Image(rstd::move(value));
                });
            break;
        case SceneObjectKind::Shape:
            DecodeObject<ShapeObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](ShapeObject value) {
                    return SceneObject::Shape(rstd::move(value));
                });
            break;
        case SceneObjectKind::Particle:
            DecodeObject<ParticleObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](ParticleObject value) {
                    return SceneObject::Particle(rstd::move(value));
                });
            break;
        case SceneObjectKind::Sound:
            DecodeObject<SoundObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](SoundObject value) {
                    return SceneObject::Sound(rstd::move(value));
                });
            break;
        case SceneObjectKind::Light:
            DecodeObject<LightObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](LightObject value) {
                    return SceneObject::Light(rstd::move(value));
                });
            break;
        case SceneObjectKind::Text:
            DecodeObject<TextObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](TextObject value) {
                    return SceneObject::Text(rstd::move(value));
                });
            break;
        case SceneObjectKind::Model:
            DecodeObject<ModelObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](ModelObject value) {
                    return SceneObject::Model(rstd::move(value));
                });
            break;
        case SceneObjectKind::Camera:
            DecodeObject<CameraObject>(
                objects, record, *vfs, document->metadata.pkg_version, [](CameraObject value) {
                    return SceneObject::Camera(rstd::move(value));
                });
            break;
        case SceneObjectKind::Unknown: break;
        }
    }
    return objects;
}

bool SceneObjectParallaxAuthored(const SceneObject& object) {
    if (object.is_Container()) return object.as_Container().value.parallax.authored;
    if (object.is_Image()) return object.as_Image().value.parallax.authored;
    if (object.is_Shape()) return object.as_Shape().value.parallax.authored;
    if (object.is_Particle()) return object.as_Particle().value.parallax.authored;
    if (object.is_Sound()) return false;
    if (object.is_Light()) return object.as_Light().value.parallax.authored;
    if (object.is_Text()) return object.as_Text().value.parallax.authored;
    if (object.is_Model()) return object.as_Model().value.parallax.authored;
    if (object.is_Camera()) return object.as_Camera().value.parallax.authored;
    return false;
}

} // namespace owe::wpscene
