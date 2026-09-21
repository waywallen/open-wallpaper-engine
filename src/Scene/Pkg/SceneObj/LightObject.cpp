module;

module wescene.pkg.scene_obj;

using namespace owe::wpscene;
using namespace rstd::literals;

bool LightObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool LightObject::FromJson(const owe::Json& json, fs::VFS&, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "origin"_str, origin);
    owe::GetJsonValue(json, "angles"_str, angles);
    owe::GetJsonValue(json, "scale"_str, scale);
    owe::GetJsonValue(json, "color"_str, color);
    owe::GetJsonValue(json, "light"_str, light);
    owe::GetJsonValue(json, "radius"_str, radius);
    owe::GetJsonValue(json, "intensity"_str, intensity);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name.clone();
    owe::GetJsonValue(json, "name"_str, name, false);
    owe::GetJsonValue(json, "id"_str, id, false);
    ReadParallaxDepth(json, parallax);
    owe::GetJsonValue(json, "shape"_str, shape, false);
    owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
    owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
    owe::GetJsonValue(json, "parent"_str, parent, false);
    owe::GetJsonValue(json, "ledsource"_str, ledsource, false);
    owe::GetJsonValue(json, "castshadow"_str, castshadow, false);
    owe::GetJsonValue(json, "castvolumetrics"_str, castvolumetrics, false);
    owe::GetJsonValue(json, "outercone"_str, outercone, false);
    owe::GetJsonValue(json, "innercone"_str, innercone, false);
    owe::GetJsonValue(json, "attenuation"_str, attenuation, false);
    owe::GetJsonValue(json, "exponent"_str, exponent, false);
    owe::GetJsonValue(json, "density"_str, density, false);
    owe::GetJsonValue(json, "volumetricsexponent"_str, volumetricsexponent, false);
    owe::GetJsonValue(json, "lightsourcesize"_str, lightsourcesize, false);
    owe::GetJsonValue(json, "mindistance"_str, mindistance, false);
    owe::GetJsonValue(json, "cascadedistance0"_str, cascadedistance0, false);
    owe::GetJsonValue(json, "cascadedistance1"_str, cascadedistance1, false);
    owe::GetJsonValue(json, "cascadedistance2"_str, cascadedistance2, false);
    owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
