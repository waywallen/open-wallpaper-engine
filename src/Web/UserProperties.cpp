module weweb;

import rstd;
import rstd.cppstd;

import :cef;
import :cef_internal;
import :frame;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::to_string;
using rstd::fmt::Arguments;
using rstd::fmt::Formatter;

namespace
{

// The daemon resolves MPRIS artwork to an absolute local path. Pages load it
// through an <img src>/CSS URL, so hand them a percent-escaped file:// URI.
// Anything that already looks like a URI is passed through untouched.
String PageImageSrc(ref<str> path) {
    if (path.is_empty()) return String {};
    auto bytes = path.as_bytes();
    if (bytes[usize()].to_primitive() != '/') return String::make(path);

    static constexpr char kHex[] = "0123456789ABCDEF";
    String                result = "file://"_Str;
    for (auto byte : bytes) {
        auto       value = byte.to_primitive();
        const bool plain = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
                           value == '_' || value == '~' || value == '/';
        if (plain) {
            result.push_ascii(static_cast<char>(value));
        } else {
            result.push_ascii('%');
            result.push_ascii(kHex[(value >> 4) & 0xF]);
            result.push_ascii(kHex[value & 0xF]);
        }
    }
    return result;
}

} // namespace

namespace weweb
{

String BuildPropertyListenerSnippet(const owe::Json& props) {
    // The page side typically registers a listener like:
    //
    //   window.wallpaperPropertyListener = {
    //     applyUserProperties: function(props) { ... }
    //   };
    //
    // We hand it the project.json `general.properties` object verbatim;
    // each entry preserves its `type` and `value` fields, matching what
    // WE's own runtime delivers.
    String snippet =
        "(function(){"
        "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
        "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
        "  try {"
        "    window.wallpaperPropertyListener.applyUserProperties("_Str;
    auto json = props.is_object() ? owe::DumpString(props) : "{}"_Str;
    snippet.push_str(json.as_str());
    snippet.push_str("    );"
                     "  } catch (e) {"
                     "    console.error('weweb: applyUserProperties threw:', e);"
                     "  }"
                     "})();"_str);
    return snippet;
}

String BuildPropertyPatchSnippet(ref<str> key, const owe::Json& value) {
    auto object = rstd::json::Map::make();
    object.insert(String::make(key), value.clone());
    auto properties = owe::Json::Object(rstd::move(object));
    auto snippet =
        "(function(){"
        "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
        "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
        "  try {"
        "    window.wallpaperPropertyListener.applyUserProperties("_Str;
    snippet.push_str(owe::DumpString(properties).as_str());
    snippet.push_str(");"
                     "  } catch (e) {"
                     "    console.error('weweb: applyUserProperties patch threw:', e);"
                     "  }"
                     "})();"_str);
    return snippet;
}

String BuildAudioResponseSnippet(slice<float> data) {
    auto snippet = "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio(["_Str;
    Formatter formatter(snippet);
    bool      first = true;
    for (float value : data) {
        if (! first) snippet.push_ascii(',');
        first       = false;
        auto number = f32(value);
        if (number.is_nan()) {
            snippet.push_str("NaN"_str);
        } else if (! number.is_finite()) {
            snippet.push_str(number < f32() ? "-Infinity"_str : "Infinity"_str);
        } else {
            formatter.write_fmt(Arguments::make("{:.8e}", number));
        }
    }
    snippet.push_str("]);})();"_str);
    return snippet;
}

String BuildMediaSnippet(const MediaInfo& media) {
    // One MPRIS snapshot, serialized as the single object our injected media
    // API expects. The page side dedupes properties/playback/thumbnail and
    // only fires the listeners whose payload actually changed.
    auto object = rstd::json::Map::make();
    object.insert("title"_Str, owe::Json::String(media.title.clone()));
    object.insert("artist"_Str, owe::Json::String(media.artist.clone()));
    object.insert("albumTitle"_Str, owe::Json::String(media.album.clone()));
    object.insert("albumArtist"_Str, owe::Json::String(media.album_artist.clone()));
    object.insert("thumbnail"_Str, owe::Json::String(PageImageSrc(media.thumbnail.as_str())));
    object.insert("state"_Str, rstd::into<owe::Json>(i32(media.state)));

    auto snippet = "(function(){if(!window.__weweb_pushMedia)return;window.__weweb_pushMedia("_Str;
    snippet.push_str(owe::DumpString(owe::Json::Object(rstd::move(object))).as_str());
    snippet.push_str(");})();"_Str);
    return snippet;
}

void InjectUserProperties(CefRefPtr<CefBrowser> browser, const owe::Json& props) {
    if (! browser) return;
    auto frame = browser->GetMainFrame();
    if (! frame) return;
    frame->ExecuteJavaScript(to_string(BuildPropertyListenerSnippet(props).as_str()),
                             "weweb://internal/inject_user_properties.js",
                             0);
}

} // namespace weweb
