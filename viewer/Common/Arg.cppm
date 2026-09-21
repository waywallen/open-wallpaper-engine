export module viewer.common:arg;

export import rstd;
import rstd.argparse;
import wescene.cli;

using namespace rstd::prelude;
using namespace rstd::argparse;
using namespace rstd::literals;
using rstd::path::PathBuf;

export namespace viewer
{

struct Resolution {
    unsigned w;
    unsigned h;
};

struct SceneViewerArgs {
    PathBuf     assets_dir;
    PathBuf     scene_path;
    PathBuf     cache_path;
    PathBuf     user_properties_path;
    String      mouse_position;
    String      load_bench_output;
    Option<u64> random_seed;
    Resolution  resolution;
    i32         fps;
    u32         msaa_samples;
    bool        enable_valid_layer;
    bool        graphviz;
    bool        stdin_json;
};

SceneViewerArgs ParseSceneViewerArgs(int argc, char** argv);
auto            ParseMousePosition(ref<str>) -> Option<array<double, 2>>;

} // namespace viewer

namespace viewer
{

template<typename T>
const T& Value(const Matches& matches, const ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

bool Flag(const Matches& matches, const ArgKey<bool>& key) {
    auto value = matches.get_one(key);
    if (value.is_err()) rstd::unreachable();
    return value->is_some() && ***value;
}

template<typename T>
auto OptionalValue(const Matches& matches, const ArgKey<T>& key) -> Option<T> {
    auto value = matches.get_one(key);
    if (value.is_err()) rstd::unreachable();
    if (value->is_none()) return None();
    return Some(T(***value));
}

auto ResolutionParser(ref<rstd::ffi::OsStr> raw) -> Result<Resolution, ValueError> {
    auto text = raw.to_str();
    if (text.is_none()) return Err(ValueError::InvalidUtf8());

    auto parts = text->split_once("x"_str);
    if (! parts) return Ok(Resolution { 1280, 720 });
    auto [width_text, height_text] = *parts;
    if (width_text.starts_with("+"_str) || height_text.starts_with("+"_str))
        return Ok(Resolution { 1280, 720 });
    auto width  = rstd::from_str<u32>(width_text);
    auto height = rstd::from_str<u32>(height_text);
    if (width.is_err() || height.is_err()) return Ok(Resolution { 1280, 720 });
    return Ok(Resolution { width->to_primitive(), height->to_primitive() });
}

auto ParseMousePosition(ref<str> input) -> Option<array<double, 2>> {
    auto parts = input.split_once(","_str);
    if (! parts) return None();
    auto [xs, ys] = *parts;
    auto x        = rstd::from_str<f64>(xs);
    auto y        = rstd::from_str<f64>(ys);
    if (x.is_err() || y.is_err() || ! x->is_finite() || ! y->is_finite()) return None();
    return Some(array<double, 2> { x->clamp(f64(0.0), f64(1.0)).to_primitive(),
                                   y->clamp(f64(0.0), f64(1.0)).to_primitive() });
}

SceneViewerArgs ParseSceneViewerArgs(int argc, char** argv) {
    auto command     = Command::make("scene-viewer"_str);
    auto assets      = command.add_arg(Arg<String>::value("assets"_str, string_parser())
                                           .value_name("ASSETS"_str)
                                           .help("assets folder"_str)
                                           .required());
    auto scene       = command.add_arg(Arg<String>::value("scene"_str, string_parser())
                                           .value_name("SCENE"_str)
                                           .help("scene file"_str)
                                           .required());
    auto fps         = command.add_arg(Arg<i32>::value("fps"_str, from_str_parser<i32>())
                                           .short_name(u8('f'))
                                           .long_name("fps"_str)
                                           .help("fps"_str)
                                           .default_value("15"_str));
    auto valid_layer = command.add_arg(Arg<bool>::flag("valid-layer"_str)
                                           .short_name(u8('V'))
                                           .long_name("valid-layer"_str)
                                           .help("enable vulkan valid layer"_str));
    auto graphviz =
        command.add_arg(Arg<bool>::flag("graphviz"_str)
                            .short_name(u8('G'))
                            .long_name("graphviz"_str)
                            .help("generate graphviz of render graph, output to 'graph.dot'"_str));
    auto stdin_json = command.add_arg(
        Arg<bool>::flag("stdin-json"_str)
            .long_name("stdin-json"_str)
            .help("read set_user_property and set_mpris JSONL commands from stdin"_str));
    auto cache_path = command.add_arg(Arg<String>::value("cache-path"_str, string_parser())
                                          .short_name(u8('C'))
                                          .long_name("cache-path"_str)
                                          .help("shader cache directory"_str)
                                          .default_value(""_str));
    auto msaa =
        command.add_arg(Arg<u32>::value("msaa"_str, from_str_parser<u32>())
                            .short_name(u8('M'))
                            .long_name("msaa"_str)
                            .help("MSAA samples for screen RT (1/2/4/8/16; default 1=off)"_str)
                            .default_value("1"_str));
    auto user_properties = command.add_arg(
        Arg<String>::value("user-properties"_str, string_parser())
            .short_name(u8('P'))
            .long_name("user-properties"_str)
            .help(
                "Path to a JSON file mapping project.json property keys to user-edited values"_str)
            .default_value(""_str));
    auto mouse_position =
        command.add_arg(Arg<String>::value("mouse-position"_str, string_parser())
                            .long_name("mouse-position"_str)
                            .help("Set initial normalized mouse position, e.g. 0,1"_str)
                            .default_value(""_str));
    auto random_seed = command.add_arg(Arg<u64>::value("random-seed"_str, from_str_parser<u64>())
                                           .long_name("random-seed"_str)
                                           .help("Set the scene random seed"_str));
    auto load_bench_output =
        command.add_arg(Arg<String>::value("load-bench-output"_str, string_parser())
                            .long_name("load-bench-output"_str)
                            .help("Write scene load probe report to FILE"_str)
                            .value_name("FILE"_str)
                            .default_value(""_str));
    auto resolution = command.add_arg(
        Arg<Resolution>::value("resolution"_str, parse_with<Resolution>(ResolutionParser))
            .short_name(u8('R'))
            .long_name("resolution"_str)
            .help("Set the resolution, eg. 1920x1080"_str)
            .default_value("1280x720"_str));

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) rstd::process::exit(i32(parsed.unwrap_err().code));
    auto matches = rstd::move(parsed).unwrap();

    return SceneViewerArgs {
        .assets_dir           = PathBuf::from(Value(matches, assets).as_str()),
        .scene_path           = PathBuf::from(Value(matches, scene).as_str()),
        .cache_path           = PathBuf::from(Value(matches, cache_path).as_str()),
        .user_properties_path = PathBuf::from(Value(matches, user_properties).as_str()),
        .mouse_position       = Value(matches, mouse_position).clone(),
        .load_bench_output    = Value(matches, load_bench_output).clone(),
        .random_seed          = OptionalValue(matches, random_seed),
        .resolution           = Value(matches, resolution),
        .fps                  = Value(matches, fps),
        .msaa_samples         = Value(matches, msaa),
        .enable_valid_layer   = Flag(matches, valid_layer),
        .graphviz             = Flag(matches, graphviz),
        .stdin_json           = Flag(matches, stdin_json),
    };
}

} // namespace viewer
