export module wescene.pkg.scene_obj:scene_document;
import rstd;
import rstd.cppstd;
import wescene.fs;
import wescene.json;
import :field_binding;
import :visibility_binding;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe

{

namespace wpscene
{

// parallaxDepth rules (resolved in UniformSceneState::ComputeParallaxOffset):
// - omitted: stored as kDefaultParallaxDepth; orthographic scenes use kImplicitOrthographicParallaxDepth
// - explicit value: used as authored depth (including {0,0} = frozen layer)
// - perspective scenes without any authored depth: layer parallax stays disabled
inline constexpr std::array<float, 2> kDefaultParallaxDepth { 0.0f, 0.0f };
inline constexpr std::array<float, 2> kImplicitOrthographicParallaxDepth { 1.0f, 1.0f };

struct ParallaxDepthBinding {
    std::array<float, 2> depth { kDefaultParallaxDepth };
    bool                 authored { false };
};

inline bool JsonHasParallaxDepth(const owe::Json& json) {
    auto member = json.get("parallaxDepth"_str);
    return member.is_some() && ! (*member)->is_null();
}

inline void ReadParallaxDepth(const owe::Json& json, ParallaxDepthBinding& binding) {
    binding.authored = JsonHasParallaxDepth(json);
    binding.depth    = kDefaultParallaxDepth;
    if (binding.authored) (void)owe::GetJsonValue(json, "parallaxDepth", binding.depth, false);
}

inline bool IsZeroParallaxDepth(const std::array<float, 2>& depth) {
    return depth[0] * depth[0] + depth[1] * depth[1] <= 1e-12f;
}

inline bool IsZeroParallaxDepth(const rstd::array<float, 2>& depth) {
    return depth[usize()] * depth[usize()] + depth[usize(1)] * depth[usize(1)] <= 1e-12f;
}

// pkg container version (the "PKGV00xx" stamp at the head of scene.pkg).
// Spans 1..23 in the live corpus. Scene JSON fields are read according to
// the earliest container version observed to carry them.
using SceneVersion = std::uint16_t;

// scene.json self-reported revision (top-level "version" int). Independent
// of SceneVersion: a single PKGV0023 pkg can contain scene.json with
// version 0/1/3/4/5. Captured for diagnostics, not used for dispatch.
using SceneJsonVersion = std::uint16_t;

constexpr SceneVersion     kSceneVersionUnknown     = 0;
constexpr SceneJsonVersion kSceneJsonVersionDefault = 0;

// Parse "PKGV0023" → 23. Returns kSceneVersionUnknown on any other shape.
SceneVersion ParsePkgVersionStamp(std::string_view stamp);

// Read top-level "version" number_unsigned; returns kSceneJsonVersionDefault
// when absent or wrong type.
SceneJsonVersion DetectSceneJsonVersion(const owe::Json& root);

class Orthogonalprojection {
public:
    bool FromJson(const owe::Json&);
    i32  width;
    i32  height;
    bool auto_ { false };
};

class SceneCamera {
public:
    bool                     FromJson(const owe::Json&);
    std::array<float, 3>     center { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     eye { 0.0f, 0.0f, 1.0f };
    std::array<float, 3>     up { 0.0f, 1.0f, 0.0f };
    std::vector<std::string> paths;
};

// PKGV0021+ — global maximum-light counts the runtime should be sized for
// (per WE editor configuration). All entries default to 0 if absent.
class SceneLightConfig {
public:
    bool FromJson(const owe::Json&);
    u32  directional { 0 };
    u32  directionalshadow { 0 };
    u32  point { 0 };
    u32  pointshadow { 0 };
    u32  spot { 0 };
    u32  spotshadow { 0 };
};

class SceneGeneral {
public:
    bool FromJson(const owe::Json&);               // legacy
    bool FromJson(const owe::Json&, SceneVersion); // canonical

    // ---- baseline (PKGV0001+) ------------------------------------------
    std::array<float, 3> clearcolor { 0.0f, 0.0f, 0.0f };
    bool                 clearenabled { true };
    bool                 camerafade { false };
    bool                 camerapreview { false };
    bool                 cameraparallax { false };
    float                cameraparallaxamount { 0.5f };
    float                cameraparallaxdelay { 0.1f };
    float                cameraparallaxmouseinfluence { 0.0f };
    bool                 isOrtho { false };
    Orthogonalprojection orthogonalprojection { i32(1920), i32(1080) };
    float                zoom { 1.0f };
    float                fov { 50.0f };
    float                nearz { 0.01f };
    float                farz { 10000.0f };
    std::array<float, 3> ambientcolor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightcolor { 0.3f, 0.3f, 0.3f };

    // bloom / camerashake scalars exist since PKGV0001 but were never
    // unpacked into the struct before the version-aware split.
    bool                                         bloom { false };
    float                                        bloomstrength { 0.0f };
    float                                        bloomthreshold { 0.0f };
    bool                                         camerashake { false };
    float                                        camerashakeamplitude { 0.0f };
    float                                        camerashakespeed { 0.0f };
    float                                        camerashakeroughness { 0.0f };
    FieldBindings                                field_bindings;
    std::unordered_map<std::string, std::string> user_bindings;

    // ---- PKGV0010+ ------------------------------------------------------
    bool  hdr { false };
    bool  norecompile { false };
    float bloomhdrfeather { 0.0f };
    u32   bloomhdriterations { 0 };
    float bloomhdrscatter { 0.0f };
    float bloomhdrstrength { 0.0f };
    float bloomhdrthreshold { 0.0f };

    // ---- PKGV0020+ ------------------------------------------------------
    std::array<float, 3> bloomtint { 1.0f, 1.0f, 1.0f };

    // ---- PKGV0021+ ------------------------------------------------------
    float                perspectiveoverridefov { 0.0f };
    bool                 windenabled { false };
    std::array<float, 3> winddirection { 0.0f, 0.0f, 1.0f };
    float                windstrength { 0.0f };
    std::array<float, 3> gravitydirection { 0.0f, -1.0f, 0.0f };
    float                gravitystrength { 0.0f };

    // ---- PKGV0022+ ------------------------------------------------------
    bool                 transparentsorting { false };
    bool                 fogdistance { false };
    float                fogdistancestart { 0.0f };
    float                fogdistanceend { 0.0f };
    std::array<float, 3> fogdistancecolor { 1.0f, 1.0f, 1.0f };
    float                fogdistancestartdensity { 0.0f };
    float                fogdistanceenddensity { 0.0f };
    bool                 fogheight { false };
    float                fogheightstart { 0.0f };
    float                fogheightend { 0.0f };
    std::array<float, 3> fogheightcolor { 1.0f, 1.0f, 1.0f };
    float                fogheightstartdensity { 0.0f };
    float                fogheightenddensity { 0.0f };

    // PKGV0021+ — global per-kind maximum light counts.
    SceneLightConfig lightconfig;
};

class SceneMetadata {
public:
    bool                       FromJson(const owe::Json&); // legacy: defaults to unknown version
    bool                       FromJson(const owe::Json&, SceneVersion); // canonical entry
    SceneVersion               pkg_version { kSceneVersionUnknown };
    SceneJsonVersion           scene_json_version { kSceneJsonVersionDefault };
    SceneCamera                camera;
    SceneGeneral               general;
    Option<std::array<u32, 2>> canvas_extent;
};

enum class SceneObjectKind
{
    Unknown,
    Container,
    Image,
    Shape,
    Particle,
    Sound,
    Light,
    Text,
    Model,
    Camera,
};

class SceneObjectMetadata {
public:
    SceneObjectKind              kind { SceneObjectKind::Unknown };
    std::size_t                  raw_index { 0 };
    i32                          id { 0 };
    bool                         has_id { false };
    std::string                  name;
    bool                         visible { true };
    VisibleUserBinding           visible_user;
    u32                          parent { 0 };
    bool                         solid { false };
    Option<std::array<float, 2>> size;
};

struct SceneObjectRecord {
    SceneObjectMetadata metadata;
    owe::Json           authored;
};

class SceneDocument {
public:
    SceneMetadata          metadata;
    Vec<SceneObjectRecord> objects;
    bool                   objects_are_array { true };
};

Option<SceneDocument>  ParseSceneDocumentValue(owe::Json, SceneVersion);
Vec<SceneObjectRecord> ParseSceneObjectRecords(const owe::Json&, bool& objects_are_array);
Option<SceneDocument>  ParseSceneDocumentJson(std::string_view, SceneVersion);
Option<SceneDocument>  LoadSceneDocumentFromVfs(fs::VFS&, std::string_view, SceneVersion);
Option<SceneDocument>  LoadSceneDocumentFromPkg(std::string_view);
Option<SceneDocument>  LoadSceneDocumentFromSource(std::string_view);

} // namespace wpscene
} // namespace owe
