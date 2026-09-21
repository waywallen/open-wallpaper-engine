module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd.log;
import wescene.json;
import wescene.pkg_fs;

using namespace rstd::prelude;
using namespace owe::wpscene;
using namespace rstd::literals;

namespace owe::wpscene
{

SceneVersion ParsePkgVersionStamp(ref<str> stamp) {
    auto digits = stamp->strip_prefix("PKGV"_str);
    if (digits.is_none() || (*digits)->is_empty() || (*digits)->starts_with("+"_str))
        return kSceneVersionUnknown;
    auto parsed = rstd::from_str<rstd::u16>(*digits);
    return parsed.is_ok() ? parsed->to_primitive() : kSceneVersionUnknown;
}

SceneJsonVersion DetectSceneJsonVersion(const owe::Json& root) {
    auto version = root.get("version"_str);
    if (version.is_none()) return kSceneJsonVersionDefault;
    auto value = (*version)->as_u64();
    if (value.is_some() && value->to_primitive() <= rstd::u16::MAX.to_primitive())
        return static_cast<SceneJsonVersion>(value->to_primitive());
    return kSceneJsonVersionDefault;
}

} // namespace owe::wpscene

bool Orthogonalprojection::FromJson(const owe::Json& json) {
    if (json.is_null()) return false;
    if (json.get("auto"_str).is_some()) {
        owe::GetJsonValue(json, "auto"_str, auto_);
    } else {
        owe::GetJsonValue(json, "width"_str, width);
        owe::GetJsonValue(json, "height"_str, height);
    }
    return true;
}

bool SceneCamera::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "center"_str, center);
    owe::GetJsonValue(json, "eye"_str, eye);
    owe::GetJsonValue(json, "up"_str, up);
    if (auto raw_paths = json.get("paths"_str); raw_paths.is_some()) {
        auto array = (*raw_paths)->as_array();
        if (array.is_none()) return true;
        for (const auto& path : **array) {
            auto value = path.as_str();
            if (value.is_some()) paths.push(rstd::into(*value));
        }
    }
    return true;
}

bool SceneLightConfig::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "directional"_str, directional, false);
    owe::GetJsonValue(json, "directionalshadow"_str, directionalshadow, false);
    owe::GetJsonValue(json, "point"_str, point, false);
    owe::GetJsonValue(json, "pointshadow"_str, pointshadow, false);
    owe::GetJsonValue(json, "spot"_str, spot, false);
    owe::GetJsonValue(json, "spotshadow"_str, spotshadow, false);
    return true;
}

namespace
{

// A single SceneVersion is the sole gate for "should we attempt to read
// fields introduced in PKGVxxxx". An unknown version (loose dir mount,
// dump.cpp legacy entry) falls through every gate so behaviour matches
// the pre-refactor "try everything" path.
constexpr bool wants(SceneVersion v, SceneVersion gate) {
    return v == kSceneVersionUnknown || v >= gate;
}

void capture_user_bindings(SceneGeneral& g, const owe::Json& json) {
    auto object = json.as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto& field             = *entry_value;
        if (! field.is_object()) return;
        auto user = field.get("user"_str);
        if (user.is_none() || ! (*user)->is_string()) return;
        (void)g.user_bindings.insert(entry_key->clone(), rstd::into(*(*user)->as_str()));
    });
}

void parse_baseline(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "ambientcolor"_str, g.ambientcolor);
    owe::GetJsonValue(json, "skylightcolor"_str, g.skylightcolor);
    owe::GetJsonValue(json, "clearcolor"_str, g.clearcolor);
    owe::GetJsonValue(json, "clearenabled"_str, g.clearenabled, false);
    owe::GetJsonValue(json, "camerafade"_str, g.camerafade, false);
    owe::GetJsonValue(json, "camerapreview"_str, g.camerapreview, false);
    owe::GetJsonValue(json, "cameraparallax"_str, g.cameraparallax);
    owe::GetJsonValue(json, "cameraparallaxamount"_str, g.cameraparallaxamount);
    owe::GetJsonValue(json, "cameraparallaxdelay"_str, g.cameraparallaxdelay);
    owe::GetJsonValue(json, "cameraparallaxmouseinfluence"_str, g.cameraparallaxmouseinfluence);
    owe::GetJsonValue(json, "zoom"_str, g.zoom, false);
    owe::GetJsonValue(json, "fov"_str, g.fov, false);
    owe::GetJsonValue(json, "nearz"_str, g.nearz, false);
    owe::GetJsonValue(json, "farz"_str, g.farz, false);
    owe::GetJsonValue(json, "bloom"_str, g.bloom, false);
    owe::GetJsonValue(json, "bloomstrength"_str, g.bloomstrength, false);
    owe::GetJsonValue(json, "bloomthreshold"_str, g.bloomthreshold, false);
    owe::GetJsonValue(json, "camerashake"_str, g.camerashake, false);
    owe::GetJsonValue(json, "camerashakeamplitude"_str, g.camerashakeamplitude, false);
    owe::GetJsonValue(json, "camerashakespeed"_str, g.camerashakespeed, false);
    owe::GetJsonValue(json, "camerashakeroughness"_str, g.camerashakeroughness, false);
    g.isOrtho = false;
    if (auto ortho = json.get("orthogonalprojection"_str); ortho.is_some()) {
        if ((*ortho)->is_null())
            g.isOrtho = false;
        else {
            g.isOrtho = true;
            g.orthogonalprojection.FromJson(**ortho);
        }
    }
}

void parse_v10_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "hdr"_str, g.hdr, false);
    owe::GetJsonValue(json, "norecompile"_str, g.norecompile, false);
    owe::GetJsonValue(json, "bloomhdrfeather"_str, g.bloomhdrfeather, false);
    owe::GetJsonValue(json, "bloomhdriterations"_str, g.bloomhdriterations, false);
    owe::GetJsonValue(json, "bloomhdrscatter"_str, g.bloomhdrscatter, false);
    owe::GetJsonValue(json, "bloomhdrstrength"_str, g.bloomhdrstrength, false);
    owe::GetJsonValue(json, "bloomhdrthreshold"_str, g.bloomhdrthreshold, false);
}

void parse_v20_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "bloomtint"_str, g.bloomtint, false);
}

void parse_v21_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "transparentsorting"_str, g.transparentsorting, false);
    owe::GetJsonValue(json, "perspectiveoverridefov"_str, g.perspectiveoverridefov, false);
    owe::GetJsonValue(json, "windenabled"_str, g.windenabled, false);
    owe::GetJsonValue(json, "winddirection"_str, g.winddirection, false);
    owe::GetJsonValue(json, "windstrength"_str, g.windstrength, false);
    owe::GetJsonValue(json, "gravitydirection"_str, g.gravitydirection, false);
    owe::GetJsonValue(json, "gravitystrength"_str, g.gravitystrength, false);
}

void parse_v22_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "fogdistance"_str, g.fogdistance, false);
    owe::GetJsonValue(json, "fogdistancestart"_str, g.fogdistancestart, false);
    owe::GetJsonValue(json, "fogdistanceend"_str, g.fogdistanceend, false);
    owe::GetJsonValue(json, "fogdistancecolor"_str, g.fogdistancecolor, false);
    owe::GetJsonValue(json, "fogdistancestartdensity"_str, g.fogdistancestartdensity, false);
    owe::GetJsonValue(json, "fogdistanceenddensity"_str, g.fogdistanceenddensity, false);
    owe::GetJsonValue(json, "fogheight"_str, g.fogheight, false);
    owe::GetJsonValue(json, "fogheightstart"_str, g.fogheightstart, false);
    owe::GetJsonValue(json, "fogheightend"_str, g.fogheightend, false);
    owe::GetJsonValue(json, "fogheightcolor"_str, g.fogheightcolor, false);
    owe::GetJsonValue(json, "fogheightstartdensity"_str, g.fogheightstartdensity, false);
    owe::GetJsonValue(json, "fogheightenddensity"_str, g.fogheightenddensity, false);
}

void parse_lightconfig(SceneGeneral& g, const owe::Json& json) {
    if (auto lightconfig = json.get("lightconfig"_str);
        lightconfig.is_some() && (*lightconfig)->is_object()) {
        g.lightconfig.FromJson(**lightconfig);
    }
}

SceneObjectKind object_kind(const owe::Json& obj) {
    if (! obj.is_object()) return SceneObjectKind::Unknown;
    if (auto value = obj.get("image"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Image;
    if (auto value = obj.get("shape"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Shape;
    if (auto value = obj.get("particle"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Particle;
    if (auto value = obj.get("sound"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Sound;
    if (auto value = obj.get("light"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Light;
    if (auto value = obj.get("text"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Text;
    if (auto value = obj.get("model"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Model;
    if (auto value = obj.get("camera"_str); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Camera;
    return SceneObjectKind::Container;
}

SceneObjectMetadata parse_object_metadata(const owe::Json& obj, rstd::size_t raw_index) {
    SceneObjectMetadata metadata;
    metadata.raw_index = raw_index;
    metadata.kind      = object_kind(obj);
    if (! obj.is_object()) return metadata;

    metadata.has_id = owe::GetJsonValue(obj, "id"_str, metadata.id, false);
    owe::GetJsonValue(obj, "name"_str, metadata.name, false);
    ReadVisibleProperty(obj, metadata.visible, metadata.visible_user);
    owe::GetJsonValue(obj, "parent"_str, metadata.parent, false);
    owe::GetJsonValue(obj, "solid"_str, metadata.solid, false);

    array<float, 2> size {};
    if (owe::GetJsonValue(obj, "size"_str, size, false) && size[usize(0)] > 0.0f &&
        size[usize(1)] > 0.0f) {
        metadata.size = Some(size);
    }
    return metadata;
}

Vec<SceneObjectRecord> parse_object_records(const owe::Json& root, bool& objects_are_array) {
    Vec<SceneObjectRecord> objects;
    auto                   raw_objects = root.get("objects"_str);
    if (raw_objects.is_none()) return objects;

    auto array = (*raw_objects)->as_array();
    if (array.is_none()) {
        objects_are_array = false;
        return objects;
    }
    const auto count = (*array)->len().to_primitive();
    objects.reserve(rstd::usize(count));
    for (rstd::size_t i = 0; i < count; ++i) {
        const auto& authored = (**array)[rstd::usize(i)];
        objects.push(SceneObjectRecord {
            .metadata = parse_object_metadata(authored, i),
            .authored = authored.clone(),
        });
    }
    return objects;
}

Option<array<u32, 2>> image_extent(const SceneObjectMetadata& obj) {
    if (obj.kind != SceneObjectKind::Image || obj.size.is_none()) return None();
    return Some(array<u32, 2> {
        rstd::as_cast<u32>((*obj.size)[usize(0)]),
        rstd::as_cast<u32>((*obj.size)[usize(1)]),
    });
}

Option<array<u32, 2>> largest_image_extent(const Vec<SceneObjectRecord>& objects) {
    Option<array<u32, 2>> best;
    uint64_t              best_area = 0;
    for (const auto& record : objects) {
        auto extent = image_extent(record.metadata);
        if (extent.is_none()) continue;
        const uint64_t area = static_cast<uint64_t>((*extent)[usize(0)].to_primitive()) *
                              static_cast<uint64_t>((*extent)[usize(1)].to_primitive());
        if (area > best_area) {
            best      = Some(*extent);
            best_area = area;
        }
    }
    return best;
}

Option<array<u32, 2>> scene_canvas_extent(const SceneMetadata&          metadata,
                                          const Vec<SceneObjectRecord>& objects) {
    const auto& general = metadata.general;
    if (! general.isOrtho) return None();

    const auto& ortho = general.orthogonalprojection;
    if (! ortho.auto_) {
        if (ortho.width <= i32() || ortho.height <= i32()) return None();
        return Some(
            array<u32, 2> { rstd::as_cast<u32>(ortho.width), rstd::as_cast<u32>(ortho.height) });
    }
    return largest_image_extent(objects);
}

} // namespace

bool SceneGeneral::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneGeneral::FromJson(const owe::Json& json, SceneVersion v) {
    parse_baseline(*this, json);
    if (wants(v, 10)) parse_v10_plus(*this, json);
    if (wants(v, 20)) parse_v20_plus(*this, json);
    if (wants(v, 21)) parse_v21_plus(*this, json);
    if (wants(v, 22)) parse_v22_plus(*this, json);
    if (wants(v, 21)) parse_lightconfig(*this, json);
    AbsorbAllFieldBindings(json, field_bindings);
    capture_user_bindings(*this, json);
    return true;
}

bool SceneMetadata::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneMetadata::FromJson(const owe::Json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if (auto camera_json = json.get("camera"_str); camera_json.is_some()) {
        // camera schema is identical across PKGV0001..PKGV0023; no version gate needed.
        camera.FromJson(**camera_json);
    } else {
        rstd_error("scene no camera");
        return false;
    }
    if (auto general_json = json.get("general"_str); general_json.is_some()) {
        general.FromJson(**general_json, v);
    } else {
        rstd_error("scene no genera data");
        return false;
    }
    return true;
}

namespace owe::wpscene
{

Vec<SceneObjectRecord> ParseSceneObjectRecords(const owe::Json& root, bool& objects_are_array) {
    return parse_object_records(root, objects_are_array);
}

Option<SceneDocument> ParseSceneDocumentJson(ref<str> buf, SceneVersion pkg_version) {
    auto parsed = rstd::json::from_str(buf);
    if (parsed.is_err()) {
        rstd_error("Can't parse scene json: {}", parsed.unwrap_err());
        return None();
    }
    return ParseSceneDocumentValue(parsed.unwrap(), pkg_version);
}

Option<SceneDocument> ParseSceneDocumentValue(owe::Json root, SceneVersion pkg_version) {
    SceneDocument doc;
    if (! doc.metadata.FromJson(root, pkg_version)) return None();
    doc.objects                = ParseSceneObjectRecords(root, doc.objects_are_array);
    doc.metadata.canvas_extent = scene_canvas_extent(doc.metadata, doc.objects);
    return Some(rstd::move(doc));
}

Option<SceneDocument> LoadSceneDocumentFromVfs(fs::VFS& vfs, fs::Path scene_path,
                                               SceneVersion pkg_version) {
    auto parsed = owe::ReadJsonFile(vfs, scene_path);
    if (parsed.is_err()) {
        if (parsed.unwrap_err().kind == owe::JsonFileErrorKind::Parse)
            rstd_error("Can't parse scene json: {}", parsed.unwrap_err());
        return None();
    }
    return ParseSceneDocumentValue(rstd::move(parsed).unwrap_unchecked(), pkg_version);
}

Option<SceneDocument> LoadSceneDocumentFromPkg(fs::Path pkg_path) {
    auto pkg = fs::WPPkgFs::open(pkg_path);
    if (pkg.is_err()) return None();

    auto scene_source = pkg->open_read("/scene.json"_str);
    if (scene_source.is_err()) return None();
    auto scene_file = fs::BinaryReader(rstd::move(scene_source).unwrap_unchecked());

    auto       stamp       = pkg->pkg_version_stamp();
    const auto pkg_version = ParsePkgVersionStamp(stamp);
    return ParseSceneDocumentJson(scene_file.ReadAllStr().as_str(), pkg_version);
}

Option<SceneDocument> LoadSceneDocumentFromSource(fs::Path source_path) {
    auto extension = source_path.extension();
    if (extension.is_none()) return None();
    if (extension->eq_ignore_ascii_case(fs::Path("pkg"_str).as_os_str()))
        return LoadSceneDocumentFromPkg(source_path);
    if (! extension->eq_ignore_ascii_case(fs::Path("json"_str).as_os_str())) return None();

    auto scene_file = fs::OpenPhysicalBinary(source_path);
    if (scene_file.is_err()) return None();
    return ParseSceneDocumentJson(scene_file->ReadAllStr().as_str(), kSceneVersionUnknown);
}

} // namespace owe::wpscene
