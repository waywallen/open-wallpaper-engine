// SPDX-License-Identifier: MIT
//
// Wire protocol for the waywallen IPC fabric. This header is hand-mirrored
// against waywallen/src/ipc/proto.rs — every field here must match the Rust
// side exactly. Keep the schema small.
//
// Iteration 0: definitions + serialization only. Wiring to SceneWallpaper
// happens in iteration 2 under open-wallpaper-engine/host/main.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <nlohmann/json.hpp>

namespace waywallen::ipc {

constexpr uint32_t kProtocolVersion = 1;

// ---------------------------------------------------------------------------
// Daemon → renderer-host  (control plane)
// ---------------------------------------------------------------------------

struct HelloCtrl {
    std::string client;
    uint32_t    version { kProtocolVersion };
};

struct LoadScene {
    std::string pkg;
    std::string assets;
    uint32_t    fps    { 30 };
    uint32_t    width  { 1280 };
    uint32_t    height { 720 };
};

struct Play {};
struct Pause {};

struct Mouse {
    double x { 0.0 };
    double y { 0.0 };
};

struct SetFps {
    uint32_t fps { 30 };
};

struct Shutdown {};

using ControlMsg =
    std::variant<HelloCtrl, LoadScene, Play, Pause, Mouse, SetFps, Shutdown>;

// ---------------------------------------------------------------------------
// Renderer-host → daemon  (events)
// ---------------------------------------------------------------------------

struct Ready {};

// Sent once after the first successful render. Carries the full set of
// swapchain image FDs as ancillary SCM_RIGHTS data on the same sendmsg.
struct BindBuffers {
    uint32_t              count { 0 };       // number of attached FDs
    uint32_t              fourcc { 0 };      // DRM fourcc
    uint32_t              width { 0 };
    uint32_t              height { 0 };
    uint32_t              stride { 0 };      // plane 0
    uint64_t              modifier { 0 };    // DRM format modifier
    uint64_t              plane_offset { 0 };
    std::vector<uint64_t> sizes;             // per-buffer size
};

// Zero FDs on the hot path. image_index points into the BindBuffers array.
struct FrameReady {
    uint32_t image_index { 0 };
    uint64_t seq { 0 };
    uint64_t ts_ns { 0 };
};

struct ErrorEv {
    std::string msg;
};

using EventMsg = std::variant<Ready, BindBuffers, FrameReady, ErrorEv>;

// ---------------------------------------------------------------------------
// Viewer client ↔ daemon  (external viewer protocol)
// ---------------------------------------------------------------------------

struct HelloViewer {
    std::string client;
    uint32_t    version { kProtocolVersion };
};

struct Subscribe {
    std::string renderer_id;
};

struct Unsubscribe {};

using ViewerMsg = std::variant<HelloViewer, Subscribe, Unsubscribe>;

// ---------------------------------------------------------------------------
// JSON serialization
//
// Matches serde's `#[serde(tag = "type")]` layout: each message is a JSON
// object with a `"type"` field naming the variant, plus the variant's
// fields at the same level.
// ---------------------------------------------------------------------------

inline nlohmann::json to_json(const ControlMsg& m) {
    using nlohmann::json;
    return std::visit(
        [](auto const& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, HelloCtrl>) {
                return json { { "type", "Hello" },
                              { "client", v.client },
                              { "version", v.version } };
            } else if constexpr (std::is_same_v<T, LoadScene>) {
                return json { { "type", "LoadScene" },
                              { "pkg", v.pkg },
                              { "assets", v.assets },
                              { "fps", v.fps },
                              { "width", v.width },
                              { "height", v.height } };
            } else if constexpr (std::is_same_v<T, Play>) {
                return json { { "type", "Play" } };
            } else if constexpr (std::is_same_v<T, Pause>) {
                return json { { "type", "Pause" } };
            } else if constexpr (std::is_same_v<T, Mouse>) {
                return json { { "type", "Mouse" }, { "x", v.x }, { "y", v.y } };
            } else if constexpr (std::is_same_v<T, SetFps>) {
                return json { { "type", "SetFps" }, { "fps", v.fps } };
            } else if constexpr (std::is_same_v<T, Shutdown>) {
                return json { { "type", "Shutdown" } };
            }
        },
        m);
}

inline std::optional<ControlMsg> control_from_json(const nlohmann::json& j) {
    if (!j.contains("type") || !j["type"].is_string()) return std::nullopt;
    const auto tag = j["type"].get<std::string>();
    if (tag == "Hello") {
        return HelloCtrl { j.value("client", std::string {}),
                           j.value("version", kProtocolVersion) };
    }
    if (tag == "LoadScene") {
        return LoadScene { j.value("pkg", std::string {}),
                           j.value("assets", std::string {}),
                           j.value("fps", 30u),
                           j.value("width", 1280u),
                           j.value("height", 720u) };
    }
    if (tag == "Play") return Play {};
    if (tag == "Pause") return Pause {};
    if (tag == "Mouse") {
        return Mouse { j.value("x", 0.0), j.value("y", 0.0) };
    }
    if (tag == "SetFps") return SetFps { j.value("fps", 30u) };
    if (tag == "Shutdown") return Shutdown {};
    return std::nullopt;
}

inline nlohmann::json to_json(const EventMsg& m) {
    using nlohmann::json;
    return std::visit(
        [](auto const& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Ready>) {
                return json { { "type", "Ready" } };
            } else if constexpr (std::is_same_v<T, BindBuffers>) {
                return json { { "type", "BindBuffers" },
                              { "count", v.count },
                              { "fourcc", v.fourcc },
                              { "width", v.width },
                              { "height", v.height },
                              { "stride", v.stride },
                              { "modifier", v.modifier },
                              { "plane_offset", v.plane_offset },
                              { "sizes", v.sizes } };
            } else if constexpr (std::is_same_v<T, FrameReady>) {
                return json { { "type", "FrameReady" },
                              { "image_index", v.image_index },
                              { "seq", v.seq },
                              { "ts_ns", v.ts_ns } };
            } else if constexpr (std::is_same_v<T, ErrorEv>) {
                return json { { "type", "Error" }, { "msg", v.msg } };
            }
        },
        m);
}

inline std::optional<EventMsg> event_from_json(const nlohmann::json& j) {
    if (!j.contains("type") || !j["type"].is_string()) return std::nullopt;
    const auto tag = j["type"].get<std::string>();
    if (tag == "Ready") return Ready {};
    if (tag == "BindBuffers") {
        BindBuffers b;
        b.count        = j.value("count", 0u);
        b.fourcc       = j.value("fourcc", 0u);
        b.width        = j.value("width", 0u);
        b.height       = j.value("height", 0u);
        b.stride       = j.value("stride", 0u);
        b.modifier     = j.value("modifier", uint64_t { 0 });
        b.plane_offset = j.value("plane_offset", uint64_t { 0 });
        b.sizes        = j.value("sizes", std::vector<uint64_t> {});
        return b;
    }
    if (tag == "FrameReady") {
        return FrameReady { j.value("image_index", 0u),
                            j.value("seq", uint64_t { 0 }),
                            j.value("ts_ns", uint64_t { 0 }) };
    }
    if (tag == "Error") return ErrorEv { j.value("msg", std::string {}) };
    return std::nullopt;
}

inline nlohmann::json to_json(const ViewerMsg& m) {
    using nlohmann::json;
    return std::visit(
        [](auto const& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, HelloViewer>) {
                return json { { "type", "Hello" },
                              { "client", v.client },
                              { "version", v.version } };
            } else if constexpr (std::is_same_v<T, Subscribe>) {
                return json { { "type", "Subscribe" },
                              { "renderer_id", v.renderer_id } };
            } else if constexpr (std::is_same_v<T, Unsubscribe>) {
                return json { { "type", "Unsubscribe" } };
            }
        },
        m);
}

inline std::optional<ViewerMsg> viewer_from_json(const nlohmann::json& j) {
    if (!j.contains("type") || !j["type"].is_string()) return std::nullopt;
    const auto tag = j["type"].get<std::string>();
    if (tag == "Hello") {
        return HelloViewer { j.value("client", std::string {}),
                             j.value("version", kProtocolVersion) };
    }
    if (tag == "Subscribe") {
        return Subscribe { j.value("renderer_id", std::string {}) };
    }
    if (tag == "Unsubscribe") return Unsubscribe {};
    return std::nullopt;
}

} // namespace waywallen::ipc
