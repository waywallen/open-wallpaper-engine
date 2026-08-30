// scene.json schema reverse-coverage report.
//
// Two-direction check against the live corpus (sourced from ScanSceneKeys),
// applied independently per top-level scope:
//
//   * "general.<X>" — fields directly under the scene's `general` object.
//     Source of truth: src/Scene/Pkg/SceneObj/SceneDocument.cpp parse_*/capture_* helpers.
//   * "objects[].<X>" — fields directly on each scene object (image/light/
//     particle/sound). Source of truth: the union of fields read across
//     ImageObject / LightObject / ParticleObject / SoundObject
//     FromJson implementations.
//
// For each scope we run:
//
//   1. ASSERT — every key the parser declares it reads must appear in at
//      least one observed scene. Catches typos and dead declarations.
//
//   2. REPORT (stderr only, no assertion) — for each PKGV version, the top
//      N keys that *do* appear in scenes but are NOT in the parsed set.
//      Drives the priority list for absorbing more keys into the structs.
//
// `general` additionally asserts (3) that each declared min_v gate is not
// later than the corpus's actual earliest observation of that key (would
// otherwise silently miss data on older scenes).
//
// kParsedGeneralKeys / kParsedObjectKeys must be kept in sync with
// src/Scene/Pkg/SceneObj/SceneDocument.cpp and src/Scene/Pkg/SceneObj/*Object.cpp
// respectively; when you add a new owe::GetJsonValue(json, "key", ...) call for a
// top-level field, list it here too.

#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import wescene.json;
import wescene.pkg.scene_obj;
import wescene.testing.scene_keys;

using namespace rstd::literals;

TEST(CameraPathDocument, ParsesClipCurvesAndInheritedOptions) {
    auto json = owe::ParseJson(R"({
        "paths": [{
            "id": 7,
            "name": "Orbit",
            "visible": true,
            "options": {"fps": 24.0, "length": 120, "mode": "single"},
            "eye": {
                "c0": [{"frame": 0, "value": 1.0}],
                "c1": [{"frame": 0, "value": 2.0}],
                "c2": [{"frame": 0, "value": 3.0}]
            },
            "fov": [{"frame": 0, "value": 45.0}],
            "zoom": null
        }]
    })")
                    .unwrap();

    owe::wpscene::CameraPathDocument document;
    ASSERT_TRUE(document.FromJson(json));
    ASSERT_EQ(document.paths.len(), rstd::usize(1));
    const auto& clip = document.paths[rstd::usize()];
    EXPECT_EQ(clip.id, rstd::i32(7));
    EXPECT_FLOAT_EQ(clip.options.fps, 24.0f);
    EXPECT_EQ(clip.options.length, rstd::i32(120));
    ASSERT_TRUE(clip.eye.is_some());
    EXPECT_EQ(clip.eye->c0.size(), 1u);
    EXPECT_EQ(clip.eye->c1.size(), 1u);
    EXPECT_EQ(clip.eye->c2.size(), 1u);
    EXPECT_EQ(clip.eye->options.length, rstd::i32(120));
    ASSERT_TRUE(clip.fov.is_some());
    EXPECT_EQ(clip.fov->c0.size(), 1u);
    EXPECT_TRUE(clip.zoom.is_none());
}

namespace
{

constexpr std::string_view kGeneralPrefix = "general.";
constexpr std::string_view kObjectsPrefix = "objects[].";

// Mirrors src/Scene/Pkg/SceneObj/SceneDocument.cpp. Group keyed by the min PKGV version where
// the parser starts attempting the read. Updates here are docs; the
// assertion below treats the union as the parsed set.
const auto& kParsedGeneralKeys() {
    using set                              = std::set<std::string>;
    static const std::map<unsigned, set> m = {
        { 1u,
          set { "ambientcolor",
                "skylightcolor",
                "clearcolor",
                "clearenabled",
                "camerafade",
                "camerapreview",
                "cameraparallax",
                "cameraparallaxamount",
                "cameraparallaxdelay",
                "cameraparallaxmouseinfluence",
                "zoom",
                "fov",
                "nearz",
                "farz",
                "bloom",
                "bloomstrength",
                "bloomthreshold",
                "camerashake",
                "camerashakeamplitude",
                "camerashakespeed",
                "camerashakeroughness",
                "orthogonalprojection" } },
        { 10u,
          set { "hdr",
                "norecompile",
                "bloomhdrfeather",
                "bloomhdriterations",
                "bloomhdrscatter",
                "bloomhdrstrength",
                "bloomhdrthreshold" } },
        { 20u, set { "bloomtint" } },
        { 21u,
          set { "perspectiveoverridefov",
                "lightconfig",
                "windenabled",
                "winddirection",
                "windstrength",
                "gravitydirection",
                "gravitystrength" } },
        { 22u,
          set { "transparentsorting",
                "fogdistance",
                "fogdistancestart",
                "fogdistanceend",
                "fogdistancecolor",
                "fogdistancestartdensity",
                "fogdistanceenddensity",
                "fogheight",
                "fogheightstart",
                "fogheightend",
                "fogheightcolor",
                "fogheightstartdensity",
                "fogheightenddensity" } },
    };
    return m;
}

// Union of objects[].<X> fields read across the four object-kind parsers.
// No version gates: object schemas evolve much more slowly than general,
// and per-kind dispatch is the dominant axis. Keep this list as a flat
// set; the SceneSchema.EveryParsedObjectKeyIsObserved test verifies each
// entry actually shows up in the corpus.
const std::set<std::string>& kParsedObjectKeys() {
    static const std::set<std::string> s = {
        // shared by all kinds
        "id",
        "name",
        "visible",
        "origin",
        "angles",
        "scale",
        "parallaxDepth",
        "locktransforms",
        "muteineditor",
        "nointerpolation",
        "parent",
        "dependencies",
        "instance",
        // shared by drawable kinds
        "reflected",
        // image-only
        "image",
        "alignment",
        "colorBlendMode",
        "color",
        "alpha",
        "brightness",
        "size",
        "effects",
        "animationlayers",
        "config",
        "perspective",
        "copybackground",
        "solid",
        "opaquebackground",
        "clampuvs",
        "castshadow",
        "disablepropagation",
        "depthtest",
        "backgroundcolor",
        "backgroundbrightness",
        // light-only
        "light",
        "radius",
        "intensity",
        "shape",
        "ledsource",
        "castvolumetrics",
        "outercone",
        "innercone",
        "attenuation",
        "exponent",
        "density",
        "volumetricsexponent",
        "lightsourcesize",
        "mindistance",
        "cascadedistance0",
        "cascadedistance1",
        "cascadedistance2",
        // particle-only
        "particle",
        "instanceoverride",
        "particlesrc",
        "controlpoint",
        // sound-only
        "sound",
        "volume",
        "playbackmode",
        "mintime",
        "maxtime",
        "startsilent",
        "blockalign",
        "spatialization",
        "queuemode",
        // text-only (TextObject)
        "text",
        "font",
        "pointsize",
        "padding",
        "horizontalalign",
        "verticalalign",
        "anchor",
        "maxrows",
        "maxwidth",
        "limitrows",
        "limitwidth",
        "limituseellipsis",
        // model-only (ModelObject)
        "model",
        "attachment",
        "skin",
        // camera-only (CameraObject)
        "camera",
        "fov",
        "zoom",
        "path",
    };
    return s;
}

std::set<std::string> AllParsedGeneral() {
    std::set<std::string> out;
    for (const auto& [_, ks] : kParsedGeneralKeys()) out.insert(ks.begin(), ks.end());
    return out;
}

unsigned PkgIntFromStamp(std::string_view s) {
    if (s.size() < 5) return 0;
    return static_cast<unsigned>(std::stoi(std::string(s.substr(4))));
}

// Accepts only paths of shape "<prefix><X>" with no further '.' or '['
// — i.e. a direct child of the scope, not a nested sub-field.
bool IsDirectChildOf(std::string_view prefix, std::string_view path) {
    if (! path.starts_with(prefix)) return false;
    return path.find_first_of(".[", prefix.size()) == std::string_view::npos;
}

// Parsed direct children of selected nested parents. Mirrors the parser
// code paths (see SceneGeneral / ParticleObject / ImageObject /
// PuppetLayer parsing). Used by ReportTopUnparsedNestedKeys.
const std::map<std::string, std::set<std::string>>& kParsedNestedKeys() {
    using set                                 = std::set<std::string>;
    static const std::map<std::string, set> m = {
        { "general.orthogonalprojection.",
          // `auto` is read in Orthogonalprojection::FromJson when present,
          // but no scene in the live corpus exercises that branch — listing
          // it here would trip EveryParsedNestedKeyIsObservedSomewhere.
          set { "width", "height" } },
        { "general.lightconfig.",
          set {
              "directional", "directionalshadow", "point", "pointshadow", "spot", "spotshadow" } },
        { "objects[].config.", set { "passthrough" } },
        { "objects[].instance.", set { "id", "combos", "textures", "usertextures" } },
        { "objects[].instanceoverride.",
          set { "alpha",
                "size",
                "lifetime",
                "rate",
                "speed",
                "count",
                "brightness",
                "id",
                "color",
                "colorn",
                "controlpoint0",
                "controlpoint1",
                "controlpoint2",
                "controlpoint3",
                "controlpoint4",
                "controlpoint5",
                "controlpoint6",
                "controlpoint7",
                "controlpointangle0",
                "controlpointangle1",
                "controlpointangle2",
                "controlpointangle3",
                "controlpointangle4",
                "controlpointangle5",
                "controlpointangle6",
                "controlpointangle7" } },
        { "objects[].animationlayers[].",
          set { "animation",
                "blend",
                "rate",
                "visible",
                "id",
                "name",
                "additive",
                "blendin",
                "blendout",
                "blendtime" } },

        // Effects: scene-level entries on top of ImageEffect-loaded data.
        { "objects[].effects[].", set { "file", "id", "name", "passes", "username", "visible" } },
        { "objects[].effects[].passes[].",
          set { "combos", "constantshadervalues", "id", "textures", "usertextures" } },

        // Property-binding side channel — any animatable scalar field on
        // an object can carry an `.animation` curve subtree. The shape is
        // identical regardless of which field it hangs off, so a single
        // <field>.animation.* parent description applies to all fields
        // (origin, scale, alpha, color, angles, parallaxDepth, visible,
        // brightness, alignment, ...). Captured by AbsorbAllFieldBindings
        // into FieldBindings::animations.
        { "objects[].alpha.animation.", set { "c0", "options" } },
        { "objects[].alpha.animation.options.",
          set { "fps",
                "length",
                "mode",
                "name",
                "startpaused",
                "wraploop",
                "smoothing",
                "children",
                "events",
                "parent" } },
        // `step` is parsed for official evaluator compatibility but does not
        // occur in the live corpus; its parser behavior has a focused test.
        { "objects[].alpha.animation.c0[].",
          set { "frame", "value", "lockangle", "locklength", "front", "back" } },
        { "objects[].alpha.animation.c0[].front.", set { "enabled", "x", "y", "magic" } },
        { "objects[].alpha.animation.c0[].back.", set { "enabled", "x", "y", "magic" } },
        { "objects[].origin.animation.", set { "c0", "c1", "c2", "options", "relative" } },
        { "objects[].origin.animation.options.",
          set { "fps",
                "length",
                "mode",
                "name",
                "startpaused",
                "wraploop",
                "smoothing",
                "children",
                "events",
                "parent" } },
    };
    return m;
}

const owe::Json& Report() {
    static const owe::Json r = owe::testing::ScanSceneKeys(WAYWALLEN_WORKSHOP_DIR);
    return r;
}

template<typename F>
void ForEachVersion(F&& function) {
    auto object = Report().as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        function(rstd::cppstd::as_string_view(entry_key->as_str()), *entry_value);
    });
}

template<typename F>
void ForEachKey(const owe::Json& version, F&& function) {
    auto keys = version.get("keys"_str);
    if (keys.is_none()) return;
    auto object = (*keys)->as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        function(rstd::cppstd::as_string_view(entry_key->as_str()), *entry_value);
    });
}

// Print top-N unparsed direct-child keys per pkg version for the given
// scope. Pure stderr signal — no assertion.
void PrintUnparsedReport(std::string_view prefix, std::string_view scope_label,
                         const std::set<std::string>& parsed, std::size_t top_n) {
    std::cerr << "\n=== unparsed top-level " << scope_label
              << "<X> keys per pkg version "
                 "(top "
              << top_n << " by present_in count) ===\n";

    std::vector<std::pair<unsigned, std::string>> stamps;
    ForEachVersion([&](std::string_view stamp, const owe::Json&) {
        stamps.emplace_back(PkgIntFromStamp(stamp), stamp);
    });
    std::sort(stamps.begin(), stamps.end());

    for (const auto& [v, stamp] : stamps) {
        auto ver_data = (Report()).get(rstd::cppstd::as_str(stamp).unwrap());
        if (ver_data.is_none()) continue;

        struct Entry {
            std::string   key;
            std::uint64_t present_in;
        };
        std::vector<Entry> miss;
        ForEachKey(**ver_data, [&](std::string_view path, const owe::Json& info) {
            if (! IsDirectChildOf(prefix, path)) return;
            const std::string k { path.substr(prefix.size()) };
            if (parsed.contains(k)) return;
            std::uint64_t present_in = 0;
            if (auto value = info.get("present_in"_str); value.is_some())
                present_in = (*value)->as_u64().unwrap_or(rstd::u64()).to_primitive();
            miss.push_back({ k, present_in });
        });
        std::sort(miss.begin(), miss.end(), [](auto& a, auto& b) {
            return a.present_in > b.present_in;
        });

        std::cerr << "  " << stamp << ": ";
        if (miss.empty()) {
            std::cerr << "(all top-level " << scope_label << "* keys are parsed)";
        } else {
            const std::size_t n = std::min(miss.size(), top_n);
            for (std::size_t i = 0; i < n; ++i) {
                if (i) std::cerr << ", ";
                std::cerr << miss[i].key << "(" << miss[i].present_in << ")";
            }
            if (miss.size() > n) std::cerr << ", … +" << (miss.size() - n) << " more";
        }
        std::cerr << "\n";
    }
}

} // namespace

TEST(SceneSchema, EveryParsedGeneralKeyIsObservedSomewhere) {
    std::set<std::string> observed;
    ForEachVersion([&](std::string_view, const owe::Json& ver_data) {
        ForEachKey(ver_data, [&](std::string_view path, const owe::Json&) {
            if (! IsDirectChildOf(kGeneralPrefix, path)) return;
            observed.insert(std::string(path.substr(kGeneralPrefix.size())));
        });
    });

    for (const auto& k : AllParsedGeneral()) {
        EXPECT_TRUE(observed.contains(k))
            << "general." << k
            << " is read by the parser but never appears in any scene "
               "across the corpus — typo or dead declaration?";
    }
}

TEST(SceneSchema, ParsedKeyDeclarationLowerBoundIsRespected) {
    // For each declared (min_v, key), assert the key actually appears in
    // some scene whose pkg version >= min_v. Catches off-by-one in the
    // version gating (e.g. listing a v21 field as v22).
    std::map<std::string, unsigned /*earliest_observed_pkg*/> earliest;
    ForEachVersion([&](std::string_view stamp, const owe::Json& ver_data) {
        const unsigned v = PkgIntFromStamp(stamp);
        ForEachKey(ver_data, [&](std::string_view path, const owe::Json&) {
            if (! IsDirectChildOf(kGeneralPrefix, path)) return;
            const std::string k { path.substr(kGeneralPrefix.size()) };
            auto              it = earliest.find(k);
            if (it == earliest.end() || v < it->second) earliest[k] = v;
        });
    });
    for (const auto& [min_v, keys] : kParsedGeneralKeys()) {
        for (const auto& k : keys) {
            auto it = earliest.find(k);
            if (it == earliest.end()) continue; // covered by previous test
            // Allow the declared min_v to be <= the earliest observed (i.e.
            // we may attempt to read earlier than necessary; that's fine).
            // But warn if we declare LATER than the field actually exists,
            // because then v < min_v scenes would silently miss it.
            EXPECT_LE(min_v, it->second)
                << "general." << k << " is declared as min_v=" << min_v << " but observed in PKGV"
                << it->second << " — gate is too late";
        }
    }
}

TEST(SceneSchema, EveryParsedObjectKeyIsObservedSomewhere) {
    std::set<std::string> observed;
    ForEachVersion([&](std::string_view, const owe::Json& ver_data) {
        ForEachKey(ver_data, [&](std::string_view path, const owe::Json&) {
            if (! IsDirectChildOf(kObjectsPrefix, path)) return;
            observed.insert(std::string(path.substr(kObjectsPrefix.size())));
        });
    });

    for (const auto& k : kParsedObjectKeys()) {
        EXPECT_TRUE(observed.contains(k))
            << "objects[]." << k
            << " is read by the parser but never appears in any scene "
               "across the corpus — typo or dead declaration?";
    }
}

TEST(SceneSchema, ReportTopUnparsedGeneralKeysPerVersion) {
    PrintUnparsedReport(kGeneralPrefix, "general.", AllParsedGeneral(), 15);
    SUCCEED();
}

TEST(SceneSchema, ReportTopUnparsedObjectKeysPerVersion) {
    PrintUnparsedReport(kObjectsPrefix, "objects[].", kParsedObjectKeys(), 15);
    SUCCEED();
}

TEST(SceneSchema, EveryParsedNestedKeyIsObservedSomewhere) {
    // For each nested-parent in kParsedNestedKeys, assert each declared
    // child appears under that prefix in the corpus. Catches typos in
    // sub-struct field names exactly the same way as the top-level test.
    for (const auto& [parent, parsed] : kParsedNestedKeys()) {
        std::set<std::string> observed;
        ForEachVersion([&](std::string_view, const owe::Json& ver_data) {
            ForEachKey(ver_data, [&](std::string_view path, const owe::Json&) {
                if (! IsDirectChildOf(parent, path)) return;
                observed.insert(std::string(path.substr(parent.size())));
            });
        });
        for (const auto& k : parsed) {
            EXPECT_TRUE(observed.contains(k))
                << parent << k
                << " is read by the parser but never appears in any scene "
                   "across the corpus — typo or dead declaration?";
        }
    }
}

TEST(SceneSchema, ReportTopUnparsedNestedKeys) {
    // Aggregated (cross-version) miss list per nested parent. Most of
    // these parents are sparsely populated, so per-version columns add
    // noise without insight. Print one row per parent.
    std::cerr << "\n=== unparsed direct-child keys per declared nested parent "
                 "(top 10 by aggregate present_in) ===\n";

    for (const auto& [parent, parsed] : kParsedNestedKeys()) {
        struct Entry {
            std::string   key;
            std::uint64_t present_in;
        };
        std::map<std::string, std::uint64_t> agg;
        ForEachVersion([&](std::string_view, const owe::Json& ver_data) {
            ForEachKey(ver_data, [&](std::string_view path, const owe::Json& info) {
                if (! IsDirectChildOf(parent, path)) return;
                const std::string k { path.substr(parent.size()) };
                if (parsed.contains(k)) return;
                if (auto value = info.get("present_in"_str); value.is_some())
                    agg[k] += (*value)->as_u64().unwrap_or(rstd::u64()).to_primitive();
            });
        });
        std::vector<Entry> miss;
        miss.reserve(agg.size());
        for (auto& kv : agg) miss.push_back({ kv.first, kv.second });
        std::sort(miss.begin(), miss.end(), [](auto& a, auto& b) {
            return a.present_in > b.present_in;
        });

        std::cerr << "  " << parent << "<X>: ";
        if (miss.empty()) {
            std::cerr << "(all observed keys parsed)";
        } else {
            const std::size_t n = std::min(miss.size(), std::size_t { 10 });
            for (std::size_t i = 0; i < n; ++i) {
                if (i) std::cerr << ", ";
                std::cerr << miss[i].key << "(" << miss[i].present_in << ")";
            }
            if (miss.size() > n) std::cerr << ", … +" << (miss.size() - n) << " more";
        }
        std::cerr << "\n";
    }
    SUCCEED();
}
