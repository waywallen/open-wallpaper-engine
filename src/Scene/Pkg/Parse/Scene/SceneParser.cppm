export module wescene.pkg.parse:scene_parser;
import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.load_bench;
import wescene.scene;
import wescene.pkg.scene_obj;
import :uniform_source;
import :sound_parser;
import wescene.texture_memory;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe
{

enum class SceneParseErrorKind
{
    Document,
    ObjectExpansion,
    Asset,
    Shader,
    Finalize,
};

struct SceneParseError {
    SceneParseErrorKind kind { SceneParseErrorKind::Document };
    String              message;
};

struct SceneParseCapabilities {
    bool directional_shadow { false };
    bool geometry_shader_supported { true };
    u32  max_geometry_output_vertices { 256 };
    u32  max_geometry_total_output_components { 1024 };
};

struct SceneParseOptions {
    SceneLoadBenchRecorderView        load_bench;
    Option<ref<rstd::json::Map>>      user_properties;
    Option<ref<rstd::json::Map>>      general_settings;
    Option<rstd::path::PathBuf>       shader_cache_dir;
    SceneParseCapabilities            capabilities;
    Option<ImageReadyCallback>        image_ready;
    Option<SceneSoundUnmountCallback> sound_unmount;
    Option<SceneSoundMountCallback>   sound_mount;
    TextureMemoryOptions              texture_memory;
};

struct ParsedScene {
    Box<Scene>               scene;
    Arc<UniformRuntimeInput> runtime_input;
};

class SceneParser {
public:
    auto Parse(ref<str> scene_id, ref<wpscene::SceneDocument> document, mut_ref<fs::VFS> vfs,
               mut_ref<wavsen::audio::SoundManager> sound, SceneParseOptions options = {})
        -> Result<ParsedScene, SceneParseError>;
};

} // namespace owe

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::SceneParseError> : ImplBase<owe::SceneParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("{}", this->self().message));
    }
};

template<>
struct Impl<fmt::Debug, owe::SceneParseError> : ImplBase<owe::SceneParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("SceneParseError(kind={}, message={})",
                                                        static_cast<int>(this->self().kind),
                                                        this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::SceneParseError>
    : DefaultInImpl<error::Error, owe::SceneParseError> {};

} // namespace rstd

static_assert(rstd::Impled<owe::SceneParseError, rstd::error::Error>);
