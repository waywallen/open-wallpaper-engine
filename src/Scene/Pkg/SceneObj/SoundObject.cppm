export module wescene.pkg.scene_obj:sound_object;
import wavsen.audio;
import wescene.fs;

import wescene.json;
export import :field_binding;
import :visibility_binding;
import :scene_document;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe

{

namespace wpscene
{

struct SoundObject {
    i32             id { 0 };
    String          playbackmode { "loop"_Str };
    array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    float           maxtime { 10.0f };
    float           mintime { 0.0f };
    float           volume { 1.0f };
    bool            visible { true };
    String          name;
    Vec<String>     sound;

    // Common cross-kind metadata.
    bool          locktransforms { false };
    bool          muteineditor { false };
    bool          nointerpolation { false };
    u32           parent { 0 };
    Vec<i32>      dependencies;
    owe::Json     instance;
    FieldBindings field_bindings;

    // Sound-kind specifics.
    bool   startsilent { false };    // PKGV0002+
    bool   blockalign { false };     // PKGV0018+
    bool   spatialization { false }; // PKGV0023+
    String queuemode;                // PKGV0020+

    VisibleUserBinding visible_user;
    String             visible_user_key;
    String             volume_user_key;

    bool FromJson(const owe::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }

    bool FromJson(const owe::Json& json, fs::VFS&, SceneVersion /*v*/) {
        owe::GetJsonValue(json, "volume"_str, volume);
        if (auto volume_json = json.get("volume"_str);
            volume_json.is_some() && (*volume_json)->is_object()) {
            if (auto user = (*volume_json)->get("user"_str); user.is_some()) {
                auto string = (*user)->as_str();
                if (string.is_some()) volume_user_key = rstd::into(*string);
            }
        }
        owe::GetJsonValue(json, "playbackmode"_str, playbackmode);
        owe::GetJsonValue(json, "origin"_str, origin, false);
        owe::GetJsonValue(json, "angles"_str, angles, false);
        owe::GetJsonValue(json, "scale"_str, scale, false);
        owe::GetJsonValue(json, "mintime"_str, mintime, false);
        owe::GetJsonValue(json, "maxtime"_str, maxtime, false);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name.clone();
        owe::GetJsonValue(json, "name"_str, name, false);
        owe::GetJsonValue(json, "id"_str, id, false);
        owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
        owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
        owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
        owe::GetJsonValue(json, "parent"_str, parent, false);
        owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
        if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();

        owe::GetJsonValue(json, "startsilent"_str, startsilent, false);
        owe::GetJsonValue(json, "blockalign"_str, blockalign, false);
        owe::GetJsonValue(json, "spatialization"_str, spatialization, false);
        owe::GetJsonValue(json, "queuemode"_str, queuemode, false);

        auto sound_json = json.get("sound"_str);
        if (sound_json.is_none()) return false;
        auto sound_array = (*sound_json)->as_array();
        if (sound_array.is_none()) return false;
        for (const auto& el : **sound_array) {
            String name;
            owe::GetJsonValue(el, name);
            if (! name.is_empty()) sound.push(rstd::move(name));
        }
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};
} // namespace wpscene
} // namespace owe
