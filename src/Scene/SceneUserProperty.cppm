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

class SceneUserPropertyState {
public:
    void Set(ref<str> key, const Json& property);
    auto Values() const -> ref<rstd::json::Map>;

private:
    rstd::json::Map m_values;
};

auto EnsureSceneUserPropertyState(Scene&) -> SceneUserPropertyState&;

ref<str>                CanonicalSceneUserPropertyKey(ref<str> key);
Option<array<float, 3>> ResolveSceneUserPropertyColor(const Json&);
Option<float>           ResolveSceneUserPropertyScalar(const Json&);

class SceneUserPropertyApplier {
public:
    static SceneUserPropertyMutation Apply(Scene&, ref<str> key, const Json&);
    static SceneUserPropertyMutation ApplyAll(Scene&, const rstd::json::Map&);
    static void ApplyImageBinding(Scene&, const SceneNode&, ref<str> key, const Json&);
    static Vec<SceneMaterialId> ApplyTexture(Scene&, ref<str> key, const Json&);
};

Vec<SceneUserPropertyDiagnostic> CollectSceneUserPropertyDiagnostics(const Scene&, ref<str> key);

} // namespace owe
