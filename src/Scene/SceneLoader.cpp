module wescene.scene_loader;
import wescene.pkg_fs;
import wescene.script;
import owe.user_property;
import rstd.log;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::OsStr;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::sync::Arc;

namespace owe
{
Json MakeUserPropertyDescriptor(Json value) {
    if (value.get("value"_str).is_some()) return value;
    auto object = rstd::json::Map::make();
    object.insert("value"_Str, rstd::move(value));
    return Json::Object(rstd::move(object));
}

Json InitialUserProperty(Json value) {
    if (value.is_string()) {
        auto raw = *value.as_str();
        return MakeUserPropertyWirePatch(raw);
    }
    return MakeUserPropertyDescriptor(rstd::move(value));
}

void MergeProjectUserProperties(ref<Path> project_dir, rstd::json::Map& out) {
    auto project_path = PathBuf::from(project_dir).join("project.json"_str);
    auto source       = rstd::fs::read_to_string(project_path);
    if (source.is_err()) return;
    auto parsed = ParseJson(source->as_str(), { .allow_comments = true });
    if (parsed.is_err()) {
        rstd::log::warn(
            "Can't parse {}: {}", project_path.as_path().to_string_lossy(), parsed.unwrap_err());
        return;
    }
    auto root    = parsed.unwrap();
    auto general = root.get("general"_str);
    if (general.is_none()) return;
    auto properties = (*general)->get("properties"_str);
    if (properties.is_none()) return;
    auto object = (*properties)->as_object();
    if (object.is_none()) return;

    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  raw_key           = entry_key->as_str();
        const auto& value             = *entry_value;
        auto        key               = CanonicalSceneUserPropertyKey(raw_key);
        auto        current           = out.get(key);
        auto        descriptor = current.is_some() ? MergeUserPropertyDescriptor(value, **current)
                                                   : MakeUserPropertyDescriptor(value.clone());
        out.insert(::alloc::string::String::make(key), rstd::move(descriptor));
    });
}

rstd::json::Map NormalizeSceneUserProperties(const rstd::json::Map& input) {
    auto out = rstd::json::Map::make();
    input.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = entry_key->as_str();
        const auto& value             = *entry_value;
        auto        canonical         = CanonicalSceneUserPropertyKey(key);
        if (key == canonical || out.get(canonical).is_none()) {
            out.insert(::alloc::string::String::make(canonical),
                       InitialUserProperty(value.clone()));
        }
    });
    return out;
}

auto LoadScene(SceneLoadRequest request, mut_ref<wavsen::audio::SoundManager> sound)
    -> Result<LoadedScene, SceneLoadError> {
    if (request.assets_dir.is_empty() || (request.source_pkg_path.is_empty() && ! request.document))
        return Err(
            SceneLoadError { SceneLoadStage::Input, "assets and a scene source are required"_Str });
    auto properties = NormalizeSceneUserProperties(request.user_properties);
    auto vfs        = Box<fs::VFS>::make();
    {
        auto span   = SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_vfs_assets);
        auto assets = fs::make_physical_fs(request.assets_dir.as_path());
        if (assets.is_err() || vfs->mount("/assets"_str, assets.unwrap(), "assets"_str).is_err())
            return Err(
                SceneLoadError { SceneLoadStage::Assets, "cannot mount built-in assets"_Str });
    }
    wpscene::SceneVersion version = wpscene::kSceneVersionUnknown;
    PathBuf               entry;
    if (! request.source_pkg_path.is_empty()) {
        auto package = rstd::move(request.source_pkg_path);
        (void)package.set_extension("pkg"_str);
        entry =
            PathBuf::from(package.as_path().file_name().unwrap_or(ref<OsStr> {}).to_os_string());
        (void)entry.set_extension("json"_str);
        auto directory = PathBuf::from(package.as_path().parent().unwrap_or(ref<Path> {}));
        if (request.scene_id.is_empty())
            request.scene_id = String::make(directory.as_path()
                                                .file_name()
                                                .unwrap_or(ref<OsStr> {})
                                                .to_str()
                                                .unwrap_or("scene"_str));
        {
            auto span =
                SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_project_properties);
            MergeProjectUserProperties(directory.as_path(), properties);
        }
        auto span = SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_package);
        auto pkg  = fs::WPPkgFs::open(package.as_path());
        bool mounted {};
        if (pkg.is_ok()) {
            version = wpscene::ParsePkgVersionStamp(pkg->pkg_version_stamp());
            mounted = vfs->mount("/assets"_str, pkg->mount_handle()).is_ok();
        }
        if (! mounted) {
            version    = wpscene::kSceneVersionUnknown;
            auto loose = fs::make_physical_fs(directory.as_path());
            if (loose.is_err() || vfs->mount("/assets"_str, loose.unwrap()).is_err())
                return Err(SceneLoadError { SceneLoadStage::Source,
                                            "cannot mount scene package or directory"_Str });
        }
    }
    if (request.scene_id.is_empty()) request.scene_id = "scene"_Str;
    if (request.scene_id.as_str().contains("/"_str) ||
        request.scene_id.as_str().contains("\\"_str) || request.scene_id == "."_str ||
        request.scene_id == ".."_str)
        return Err(SceneLoadError { SceneLoadStage::Input,
                                    "scene id must be a single path component"_Str });
    auto document = rstd::move(request.document);
    if (! document) {
        auto span   = SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_scene_document);
        auto path   = PathBuf::from("/assets"_str).join(entry.as_path());
        auto loaded = wpscene::LoadSceneDocumentFromVfs(*vfs, path.as_path(), version);
        if (loaded) document = Some(Arc<wpscene::SceneDocument>::make(rstd::move(*loaded)));
    }
    if (! document)
        return Err(SceneLoadError { SceneLoadStage::Document,
                                    "unsupported or missing scene document"_Str });
    Option<PathBuf> shader_cache;
    if (! request.cache_dir.is_empty()) shader_cache = Some(request.cache_dir.clone());
    SceneParser parser;
    auto        parsed = parser.Parse(
        request.scene_id.as_str(),
        ref<wpscene::SceneDocument>::from_raw_parts(document->as_ptr()),
        mut_ref<fs::VFS>::from_raw_parts(&*vfs),
        sound,
        { .load_bench       = request.load_bench,
          .user_properties  = Some(ref<rstd::json::Map>::from_raw_parts(&properties)),
          .general_settings = Some(ref<rstd::json::Map>::from_raw_parts(&request.general_settings)),
          .shader_cache_dir = rstd::move(shader_cache),
          .capabilities     = request.capabilities,
          .image_ready      = rstd::move(request.image_ready),
          .sound_unmount    = rstd::move(request.sound_unmount),
          .sound_mount      = rstd::move(request.sound_mount),
          .texture_memory   = rstd::move(request.texture_memory) });
    if (parsed.is_err()) {
        auto error = parsed.unwrap_err();
        return Err(
            SceneLoadError { SceneLoadStage::Parse, rstd::move(error.message), Some(error.kind) });
    }
    auto loaded = parsed.unwrap();
    auto span   = SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_runtime_setup);
    loaded.scene->InstallExtension(rstd::move(vfs));
    auto mutation = [&] {
        auto property_span =
            SceneLoadSpan(request.load_bench, &SceneLoadProbeIds::load_initial_properties);
        return SceneUserPropertyApplier::ApplyAll(*loaded.scene, properties);
    }();
    if (! request.cache_dir.is_empty()) {
        auto directory = request.cache_dir.join("script_localstorage"_str);
        (void)rstd::fs::create_dir_all(directory);
        auto file = directory.join(rstd::format("{}.json", request.scene_id).as_str());
        script::SetScenePersistence(*loaded.scene, rstd::move(file));
    }
    return Ok(LoadedScene { rstd::move(loaded),
                            rstd::move(request.scene_id),
                            rstd::move(properties),
                            rstd::move(mutation) });
}
} // namespace owe
