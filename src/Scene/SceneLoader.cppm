export module wescene.scene_loader;
export import wescene.pkg.parse;
export import wescene.pkg.scene_obj;
export import wescene.scene_user_property;
export import wescene.fs;
export import wescene.json;
import wavsen.audio;
import wescene.scene;
import wescene.load_bench;
import rstd;
using namespace rstd::prelude;

export namespace owe
{
struct SceneLoadRequest {
    rstd::path::PathBuf                             source_pkg_path, assets_dir, cache_dir;
    String                                          scene_id;
    Option<rstd::sync::Arc<wpscene::SceneDocument>> document;
    rstd::json::Map                                 user_properties;
    rstd::json::Map                                 general_settings;
    SceneParseCapabilities                          capabilities;
    SceneLoadBenchRecorderView                      load_bench;
    Option<ImageReadyCallback>                      image_ready;
    Option<SceneSoundUnmountCallback>               sound_unmount;
    Option<SceneSoundMountCallback>                 sound_mount;
    TextureMemoryOptions                            texture_memory;
};
enum class SceneLoadStage
{
    Input,
    Assets,
    Source,
    Document,
    Parse
};
struct SceneLoadError {
    SceneLoadStage              stage;
    String                      message;
    Option<SceneParseErrorKind> parse_kind;
};
struct LoadedScene {
    ParsedScene               parsed;
    String                    id;
    rstd::json::Map           user_properties;
    SceneUserPropertyMutation initial_mutation;
};
Json            MakeUserPropertyDescriptor(Json);
rstd::json::Map NormalizeSceneUserProperties(const rstd::json::Map&);
// Sound is borrowed by scene content and must outlive the returned scene.
auto LoadScene(SceneLoadRequest, mut_ref<wavsen::audio::SoundManager>)
    -> Result<LoadedScene, SceneLoadError>;
} // namespace owe
