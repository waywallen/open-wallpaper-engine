module;
#include <rstd/enum.hpp>

export module wescene.pkg.scene_obj:object;
import rstd;
import wescene.fs;
import wescene.json;
import :scene_document;
import :image_object;
import :light_object;
import :misc_object;
import :particle_object;
import :sound_object;

using namespace rstd::prelude;

export namespace owe::wpscene
{

struct ContainerObject {
    bool FromJson(const owe::Json&);

    i32                  id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    ParallaxDepthBinding parallax;
    bool                 visible { true };
    bool                 solid { false };
    bool                 disable_propagation { false };
    u32                  parent { 0 };
    std::string          attachment;
    Vec<i32>             dependencies;
    owe::Json            instance;
    VisibleUserBinding   visible_user;
    FieldBindings        field_bindings;
};

class SceneObject {
    RSTD_ENUM(SceneObject, (Container, (ContainerObject value;)), (Image, (ImageObject value;)),
              (Shape, (ShapeObject value;)), (Particle, (ParticleObject value;)),
              (Sound, (SoundObject value;)), (Light, (LightObject value;)),
              (Text, (TextObject value;)), (Model, (ModelObject value;)),
              (Camera, (CameraObject value;)))
};

Vec<SceneObject> DecodeSceneObjects(ref<SceneDocument>, mut_ref<fs::VFS>);

} // namespace owe::wpscene
