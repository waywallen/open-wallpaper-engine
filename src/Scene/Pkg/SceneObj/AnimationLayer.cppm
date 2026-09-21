module;

export module wescene.pkg.scene_obj:animation_layer;
import rstd;
import wescene.json;
export import wescene.pkg.puppet;
export import :field_binding;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::wpscene
{

struct PuppetAnimationLayer {
    PuppetLayer::AnimationLayer playback;
    FieldBindings               field_bindings;
};

inline void ReadPuppetAnimationLayers(const owe::Json& json, Vec<PuppetAnimationLayer>& out) {
    auto layers = json.get("animationlayers"_str);
    if (layers.is_none()) return;
    auto array = (*layers)->as_array();
    if (array.is_none()) return;
    for (const auto& jLayer : **array) {
        PuppetAnimationLayer entry;
        auto&                layer = entry.playback;
        AbsorbAllFieldBindings(jLayer, entry.field_bindings);
        owe::GetJsonValue(jLayer, "animation"_str, layer.id);
        owe::GetJsonValue(jLayer, "blend"_str, layer.blend);
        owe::GetJsonValue(jLayer, "rate"_str, layer.rate);
        owe::GetJsonValue(jLayer, "visible"_str, layer.visible, false);
        owe::GetJsonValue(jLayer, "id"_str, layer.layer_id, false);
        owe::GetJsonValue(jLayer, "name"_str, layer.name, false);
        owe::GetJsonValue(jLayer, "additive"_str, layer.additive, false);
        owe::GetJsonValue(jLayer, "blendin"_str, layer.blendin, false);
        owe::GetJsonValue(jLayer, "blendout"_str, layer.blendout, false);
        owe::GetJsonValue(jLayer, "blendtime"_str, layer.blendtime, false);
        out.push(rstd::move(entry));
    }
}

} // namespace owe::wpscene
