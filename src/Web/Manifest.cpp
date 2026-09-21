module weweb;

import rstd;

import :manifest;

using namespace rstd::literals;
using namespace rstd::prelude;
using rstd::io::eprintln;
using rstd::path::Path;
using rstd::path::PathBuf;

namespace weweb
{

namespace
{

String LowerAscii(ref<str> text) {
    String result;
    for (auto codepoint : text.chars()) {
        auto c = static_cast<char32_t>(codepoint.to_primitive());
        result.push(c >= U'A' && c <= U'Z' ? c + (U'a' - U'A') : c);
    }
    return result;
}

} // namespace

rstd::Option<WebManifest> LoadWebManifest(ref<Path> workshop_dir) {
    auto pj_path         = PathBuf::from(workshop_dir).join("project.json"_str);
    auto diagnostic_path = pj_path.as_path().as_os_str().display();
    auto source          = rstd::fs::read_to_string(pj_path.as_path());
    if (source.is_err()) {
        eprintln("weweb: cannot read {}", diagnostic_path);
        return rstd::None();
    }

    // Invalid project.json is input data; keep parse failure on the
    // diagnostic return path.
    auto parsed = owe::ParseJson(source.unwrap().as_str(), { .allow_comments = true });
    if (parsed.is_err()) {
        auto error = parsed.unwrap_err();
        eprintln("weweb: invalid JSON in {} at line {} column {}",
                 diagnostic_path,
                 error.line(),
                 error.column());
        return rstd::None();
    }
    auto root = parsed.unwrap();

    auto type = root.get("type"_str);
    if (type.is_none() || (*type)->as_str().is_none()) {
        eprintln("weweb: {} is missing a string \"type\" field", diagnostic_path);
        return rstd::None();
    }
    // WE corpus has both "web" and "Web" for the type field; fold case.
    auto normalized_type = LowerAscii(*(*type)->as_str());
    if (normalized_type.as_str() != "web"_str) {
        eprintln("weweb: {} has type=\"{}\", expected \"web\"", diagnostic_path, normalized_type);
        return rstd::None();
    }

    WebManifest m;
    m.entry_html = "index.html"_Str;
    if (auto file = root.get("file"_str); file.is_some()) {
        auto string = (*file)->as_str();
        if (string.is_some()) m.entry_html = String::make(*string);
    }
    m.title = "Wallpaper"_Str;
    if (auto title = root.get("title"_str); title.is_some()) {
        auto string = (*title)->as_str();
        if (string.is_some()) m.title = String::make(*string);
    }

    if (auto preview = root.get("preview"_str); preview.is_some()) {
        auto string = (*preview)->as_str();
        if (string.is_some()) m.preview = rstd::Some(String::make(*string));
    }

    if (auto general = root.get("general"_str); general.is_some())
        if (auto properties = (*general)->get("properties"_str); properties.is_some())
            m.user_props = (*properties)->clone();

    return rstd::Some(rstd::move(m));
}

} // namespace weweb
