module;

module wescene.pkg.scene_obj;

using namespace owe::wpscene;
using namespace rstd::literals;

bool LightObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool LightObject::FromJson(const owe::Json& json, fs::VFS&, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "color", color);
    owe::GetJsonValue(json, "light", light);
    owe::GetJsonValue(json, "radius", radius);
    owe::GetJsonValue(json, "intensity", intensity);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    ReadParallaxDepth(json, parallax);
    owe::GetJsonValue(json, "shape", shape, false);
    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "ledsource", ledsource, false);
    owe::GetJsonValue(json, "castshadow", castshadow, false);
    owe::GetJsonValue(json, "castvolumetrics", castvolumetrics, false);
    owe::GetJsonValue(json, "outercone", outercone, false);
    owe::GetJsonValue(json, "innercone", innercone, false);
    owe::GetJsonValue(json, "attenuation", attenuation, false);
    owe::GetJsonValue(json, "exponent", exponent, false);
    owe::GetJsonValue(json, "density", density, false);
    owe::GetJsonValue(json, "volumetricsexponent", volumetricsexponent, false);
    owe::GetJsonValue(json, "lightsourcesize", lightsourcesize, false);
    owe::GetJsonValue(json, "mindistance", mindistance, false);
    owe::GetJsonValue(json, "cascadedistance0", cascadedistance0, false);
    owe::GetJsonValue(json, "cascadedistance1", cascadedistance1, false);
    owe::GetJsonValue(json, "cascadedistance2", cascadedistance2, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
