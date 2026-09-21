module weweb;

import rstd;
import rstd.cppstd;

import :cef;
import :cef_internal;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::to_string;
using rstd::fmt::Arguments;
using rstd::fmt::Formatter;

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

void InjectUserProperties(CefRefPtr<CefBrowser> browser, const owe::Json& props) {
    if (! browser) return;
    auto frame = browser->GetMainFrame();
    if (! frame) return;
    frame->ExecuteJavaScript(to_string(BuildPropertyListenerSnippet(props).as_str()),
                             "weweb://internal/inject_user_properties.js",
                             0);
}

} // namespace weweb
