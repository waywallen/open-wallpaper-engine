module;

export module wescene.scene_user_property;

import rstd;
import wescene.json;
import wescene.scene;

using namespace rstd::prelude;

export namespace owe
{

struct SceneUserPropertyMutation {
    bool                    graph_changed {};
    bool                    diagnostics_changed {};
    Option<array<float, 3>> clear_color;
    Vec<SceneMaterialId>    texture_materials;
};

ref<str>                CanonicalSceneUserPropertyKey(ref<str> key);
Option<array<float, 3>> ResolveSceneUserPropertyColor(const Json&);

class SceneUserPropertyApplier {
public:
    static SceneUserPropertyMutation Apply(Scene&, ref<str> key, const Json&);
    static SceneUserPropertyMutation ApplyAll(Scene&, const rstd::json::Map&);
    static Vec<SceneMaterialId>      ApplyTexture(Scene&, ref<str> key, const Json&);
};

Vec<SceneUserPropertyDiagnostic> CollectSceneUserPropertyDiagnostics(const Scene&, ref<str> key);

} // namespace owe
