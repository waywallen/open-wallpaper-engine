#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import owe.audio_response;
import owe.scene_audio_response;
import rstd;
import rstd.argparse;
import rstd.cppstd;
import rstd.json;
import wavsen.audio;
import wescene.cli;

namespace
{

using namespace owe;
using namespace rstd::argparse;
using namespace rstd::literals;
using namespace rstd::prelude;

struct DumpArgs {
    std::string input;
    std::string output;
    std::string scene_log;
    std::string stage;
    std::string web_capture;
    std::string web_source;
    double      web_start_ms;
    double      web_silence_threshold;
    u32         fps;
    u32         capture_frames;
    u32         tail_ms;
    u32         offset_min_ms;
    u32         offset_max_ms;
};

struct CaptureFrame {
    u64                  end_sample_frame;
    audio::ResponseFrame response;
};

struct RenderFrame {
    rstd::uint64_t       sequence;
    double               runtime;
    double               frametime;
    u64                  end_sample_frame;
    audio::ResponseFrame response;
    scene_audio::Buffers scene_buffers;
};

struct SceneSide {
    rstd::uint64_t                           sequence;
    double                                   runtime;
    double                                   frametime;
    rstd::array<float, audio::kResponseBins> values;
};

struct Alignment {
    u32    playback_start_ms;
    double energy_cosine;
};

template<typename T>
const T& Value(const Matches& matches, const ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

std::string ToStdString(const String& value) { return rstd::cppstd::to_string(value.as_str()); }

auto ParseArgs(int argc, char** argv) -> Result<DumpArgs, cli::ParseExit> {
    auto command   = Command::make("owe-scene-audio-response-dump"_str);
    auto input     = command.add_arg(Arg<String>::value("input"_str, string_parser())
                                         .value_name("AUDIO"_str)
                                         .help("audio file supported by FFmpeg"_str)
                                         .required());
    auto output    = command.add_arg(Arg<String>::value("output"_str, string_parser())
                                         .short_name(u8('o'))
                                         .long_name("output"_str)
                                         .value_name("FILE"_str)
                                         .help("output file, or - for stdout"_str)
                                         .default_value("-"_str));
    auto scene_log = command.add_arg(Arg<String>::value("scene-log"_str, string_parser())
                                         .long_name("scene-log"_str)
                                         .value_name("FILE"_str)
                                         .help("official Scene log used as the output timeline"_str)
                                         .default_value(""_str));
    auto stage     = command.add_arg(Arg<String>::value("stage"_str, string_parser())
                                         .long_name("stage"_str)
                                         .value_name("shared|scene"_str)
                                         .help("response stage written to the output"_str)
                                         .default_value("scene"_str));
    auto web_capture = command.add_arg(Arg<String>::value("web-capture"_str, string_parser())
                                           .long_name("web-capture"_str)
                                           .value_name("JSONL"_str)
                                           .help("official Web response used as shared input"_str)
                                           .default_value(""_str));
    auto web_source  = command.add_arg(Arg<String>::value("web-source"_str, string_parser())
                                           .long_name("web-source"_str)
                                           .value_name("NAME"_str)
                                           .help("audio_source selected from the Web capture"_str)
                                           .default_value("external-inverse-probe"_str));
    auto web_start_ms =
        command.add_arg(Arg<f64>::value("web-start-ms"_str, from_str_parser<f64>())
                            .long_name("web-start-ms"_str)
                            .value_name("MS"_str)
                            .help("playback start on the Web capture t_ms clock"_str)
                            .default_value("0"_str));
    auto web_silence_threshold =
        command.add_arg(Arg<f64>::value("web-silence-threshold"_str, from_str_parser<f64>())
                            .long_name("web-silence-threshold"_str)
                            .value_name("VALUE"_str)
                            .help("Web system-noise floor treated as an empty response"_str)
                            .default_value("0.00001"_str));
    auto fps = command.add_arg(Arg<u32>::value("fps"_str, from_str_parser<u32>())
                                   .long_name("fps"_str)
                                   .help("fallback Scene render frames per second"_str)
                                   .default_value("30"_str));
    auto capture_frames =
        command.add_arg(Arg<u32>::value("capture-frames"_str, from_str_parser<u32>())
                            .long_name("capture-frames"_str)
                            .help("audio sample frames between response snapshots"_str)
                            .default_value("1584"_str));
    auto tail_ms = command.add_arg(Arg<u32>::value("tail-ms"_str, from_str_parser<u32>())
                                       .long_name("tail-ms"_str)
                                       .help("zero-input duration after the source ends"_str)
                                       .default_value("500"_str));
    auto offset_min_ms =
        command.add_arg(Arg<u32>::value("offset-min-ms"_str, from_str_parser<u32>())
                            .long_name("offset-min-ms"_str)
                            .help("minimum playback start searched in the Scene log"_str)
                            .default_value("0"_str));
    auto offset_max_ms =
        command.add_arg(Arg<u32>::value("offset-max-ms"_str, from_str_parser<u32>())
                            .long_name("offset-max-ms"_str)
                            .help("maximum playback start searched in the Scene log"_str)
                            .default_value("5000"_str));

    auto parsed = cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) return Err(parsed.unwrap_err());
    auto matches = rstd::move(parsed).unwrap();
    return Ok(DumpArgs {
        .input                 = ToStdString(Value(matches, input)),
        .output                = ToStdString(Value(matches, output)),
        .scene_log             = ToStdString(Value(matches, scene_log)),
        .stage                 = ToStdString(Value(matches, stage)),
        .web_capture           = ToStdString(Value(matches, web_capture)),
        .web_source            = ToStdString(Value(matches, web_source)),
        .web_start_ms          = Value(matches, web_start_ms).to_primitive(),
        .web_silence_threshold = Value(matches, web_silence_threshold).to_primitive(),
        .fps                   = Value(matches, fps),
        .capture_frames        = Value(matches, capture_frames),
        .tail_ms               = Value(matches, tail_ms),
        .offset_min_ms         = Value(matches, offset_min_ms),
        .offset_max_ms         = Value(matches, offset_max_ms),
    });
}

auto Decode(const std::string& input) -> Option<Vec<float>> {
    auto path   = rstd::path::PathBuf::from(rstd::cppstd::as_str(input).unwrap());
    auto source = wavsen::audio::open_file(path.as_path());
    if (source.is_err()) {
        std::fprintf(stderr, "cannot open audio input: %s\n", input.c_str());
        return None();
    }

    wavsen::audio::StreamDecoder decoder;
    if (! decoder.open(
            rstd::move(source).unwrap(),
            wavsen::audio::DeviceDesc { .channels = u32(2), .sample_rate = u32(48000) })) {
        std::fprintf(stderr, "cannot decode audio input: %s\n", input.c_str());
        return None();
    }

    constexpr rstd::size_t               kChunkFrames = 8192;
    rstd::array<float, kChunkFrames * 2> chunk {};
    auto                                 pcm = Vec<float>::make();
    while (true) {
        const auto produced = decoder.next_pcm(chunk.data(), u32(kChunkFrames));
        if (produced == u64()) break;
        const auto sample_count = produced.to_primitive() * 2;
        pcm.reserve(pcm.len() + usize(sample_count));
        for (rstd::uint64_t index = 0; index < sample_count; ++index) {
            pcm.push(float(chunk[usize(index)]));
        }
    }
    return Some(rstd::move(pcm));
}

bool NextToken(std::string_view& source, std::string_view& token) {
    while (! source.empty() &&
           (source.front() == ' ' || source.front() == '\t' || source.front() == ',')) {
        source.remove_prefix(1);
    }
    if (source.empty()) return false;
    const auto end = source.find_first_of(" \t,\r");
    if (end == std::string_view::npos) {
        token  = source;
        source = {};
    } else {
        token = source.substr(0, end);
        source.remove_prefix(end);
    }
    return ! token.empty();
}

bool ParseUnsigned(std::string_view& source, rstd::uint64_t& value) {
    std::string_view token;
    if (! NextToken(source, token)) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == token.data() + token.size();
}

bool ParseDouble(std::string_view& source, double& value) {
    std::string_view token;
    if (! NextToken(source, token)) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == token.data() + token.size() &&
           std::isfinite(value);
}

bool ParseSceneSide(std::string_view line, std::string_view marker, SceneSide& output) {
    const auto marker_offset = line.find(marker);
    if (marker_offset == std::string_view::npos) return false;
    line.remove_prefix(marker_offset + marker.size());
    if (! ParseUnsigned(line, output.sequence) || ! ParseDouble(line, output.runtime) ||
        ! ParseDouble(line, output.frametime)) {
        return false;
    }
    for (rstd::size_t index = 0; index < audio::kResponseBins; ++index) {
        double value = 0.0;
        if (! ParseDouble(line, value)) return false;
        output.values[usize(index)] = static_cast<float>(value);
    }
    return true;
}

auto LoadSceneLog(const std::string& input) -> Option<Vec<RenderFrame>> {
    auto path = rstd::path::PathBuf::from(rstd::cppstd::as_str(input).unwrap());
    auto read = rstd::fs::read(path.as_path());
    if (read.is_err()) {
        std::fprintf(stderr, "cannot read Scene log: %s\n", input.c_str());
        return None();
    }
    auto bytes = rstd::move(read).unwrap_unchecked();
    auto text =
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.len().to_primitive());
    auto         source = std::string_view(text);
    auto         frames = Vec<RenderFrame>::make();
    RenderFrame  pending {};
    bool         has_left    = false;
    rstd::size_t line_number = 0;
    while (! source.empty()) {
        const auto end  = source.find('\n');
        const auto line = source.substr(0, end);
        source = end == std::string_view::npos ? std::string_view() : source.substr(end + 1);
        ++line_number;
        if (line.find("WE_SCENE_AUDIO64_BEGIN") != std::string_view::npos) {
            frames   = Vec<RenderFrame>::make();
            has_left = false;
            continue;
        }

        SceneSide side {};
        if (line.find("WE_SCENE_AUDIO64_LEFT") != std::string_view::npos) {
            if (! ParseSceneSide(line, "WE_SCENE_AUDIO64_LEFT", side)) {
                std::fprintf(stderr, "invalid Scene LEFT row at line %zu\n", line_number);
                return None();
            }
            pending = {
                .sequence         = side.sequence,
                .runtime          = side.runtime,
                .frametime        = side.frametime,
                .end_sample_frame = u64(),
                .response         = {},
            };
            pending.response.left = side.values;
            has_left              = true;
            continue;
        }
        if (line.find("WE_SCENE_AUDIO64_RIGHT") == std::string_view::npos) continue;
        if (! ParseSceneSide(line, "WE_SCENE_AUDIO64_RIGHT", side) || ! has_left ||
            side.sequence != pending.sequence) {
            std::fprintf(stderr, "invalid Scene RIGHT row at line %zu\n", line_number);
            return None();
        }
        pending.response.right = side.values;
        frames.push(rstd::move(pending));
        has_left = false;
    }
    if (frames.is_empty()) {
        std::fprintf(stderr, "Scene log contains no complete audio frames: %s\n", input.c_str());
        return None();
    }
    return Some(rstd::move(frames));
}

auto LoadWebCapture(const DumpArgs& args) -> Option<Vec<CaptureFrame>> {
    auto path = rstd::path::PathBuf::from(rstd::cppstd::as_str(args.web_capture).unwrap());
    auto read = rstd::fs::read(path.as_path());
    if (read.is_err()) {
        std::fprintf(stderr, "cannot read Web capture: %s\n", args.web_capture.c_str());
        return None();
    }
    auto bytes = rstd::move(read).unwrap_unchecked();
    auto text =
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.len().to_primitive());
    auto         source = std::string_view(text);
    auto         frames = Vec<CaptureFrame>::make();
    rstd::size_t line_number {};
    while (! source.empty()) {
        const auto end  = source.find('\n');
        const auto line = source.substr(0, end);
        source = end == std::string_view::npos ? std::string_view() : source.substr(end + 1);
        ++line_number;
        if (line.empty()) continue;

        auto parsed = rstd::json::from_str(rstd::cppstd::as_str(line).unwrap());
        if (parsed.is_err()) {
            std::fprintf(stderr, "invalid Web JSON row at line %zu\n", line_number);
            return None();
        }
        auto row         = rstd::move(parsed).unwrap_unchecked();
        auto type_member = row.get("type"_str);
        if (type_member.is_none()) continue;
        auto type = (**type_member).as_str();
        if (type.is_none() || rstd::cppstd::as_string_view(*type) != "frame") continue;

        auto source_member = row.get("audio_source"_str);
        if (source_member.is_none()) continue;
        auto audio_source = (**source_member).as_str();
        if (audio_source.is_none() ||
            rstd::cppstd::as_string_view(*audio_source) != args.web_source) {
            continue;
        }

        auto sequence_member = row.get("seq"_str);
        auto time_member     = row.get("t_ms"_str);
        auto values_member   = row.get("values"_str);
        if (sequence_member.is_none() || time_member.is_none() || values_member.is_none()) {
            std::fprintf(stderr, "incomplete Web frame at line %zu\n", line_number);
            return None();
        }
        auto sequence = (**sequence_member).as_u64();
        auto time     = (**time_member).as_f64();
        auto values   = (**values_member).as_array();
        if (sequence.is_none() || time.is_none() || values.is_none() ||
            (**values).len() != usize(audio::kResponseBins * 2)) {
            std::fprintf(stderr, "invalid Web frame at line %zu\n", line_number);
            return None();
        }

        const double audio_ms = time->to_primitive() - args.web_start_ms;
        if (! std::isfinite(audio_ms) || audio_ms < 0.0) continue;
        const auto end_sample_frame = static_cast<rstd::uint64_t>(std::llround(audio_ms * 48.0));
        if (! frames.is_empty() &&
            end_sample_frame < frames.last().unwrap()->end_sample_frame.to_primitive()) {
            std::fprintf(stderr, "Web capture time moved backwards at line %zu\n", line_number);
            return None();
        }

        audio::ResponseFrame response {};
        response.generation       = 1;
        response.sequence         = sequence->to_primitive();
        response.end_sample_frame = end_sample_frame;
        response.captured_at_ns   = end_sample_frame * 1000000000ULL / audio::kSampleRate;
        double maximum            = 0.0;
        for (rstd::size_t index = 0; index < audio::kResponseBins * 2; ++index) {
            auto value = (**values)[usize(index)].as_f64();
            if (value.is_none() || ! std::isfinite(value->to_primitive())) {
                std::fprintf(stderr, "invalid Web response value at line %zu\n", line_number);
                return None();
            }
            const float output = static_cast<float>(value->to_primitive());
            maximum            = std::max(maximum, std::abs(value->to_primitive()));
            if (index < audio::kResponseBins) {
                response.left[usize(index)] = output;
            } else {
                response.right[usize(index - audio::kResponseBins)] = output;
            }
        }
        if (maximum <= args.web_silence_threshold) response.left = response.right = {};
        frames.push(CaptureFrame {
            .end_sample_frame = u64(end_sample_frame),
            .response         = rstd::move(response),
        });
    }
    if (frames.is_empty()) {
        std::fprintf(
            stderr, "Web capture contains no matching frames: %s\n", args.web_capture.c_str());
        return None();
    }
    std::fprintf(stderr,
                 "Web shared-input replay\n  source=%s frames=%zu start_ms=%.9g "
                 "silence_threshold=%.9g\n",
                 args.web_source.c_str(),
                 frames.len().to_primitive(),
                 args.web_start_ms,
                 args.web_silence_threshold);
    return Some(rstd::move(frames));
}

auto AnalyzeCaptures(const Vec<float>& pcm, u64 input_frames, u64 total_frames, u32 capture_frames)
    -> Vec<CaptureFrame> {
    auto                  captures = Vec<CaptureFrame>::make();
    audio::ResponseEngine engine;
    const u64             interval(capture_frames.to_primitive());
    u64 end_frame = ((u64(audio::kWindowFrames) + interval - u64(1)) / interval) * interval;
    rstd::uint64_t sequence = 0;
    while (end_frame <= total_frames) {
        audio::PcmWindow window {};
        window.generation = 1;
        window.sequence   = ++sequence;
        window.captured_at_ns =
            (end_frame * u64(1000000000) / u64(audio::kSampleRate)).to_primitive();
        window.end_sample_frame = end_frame.to_primitive();
        window.sample_rate_hz   = audio::kSampleRate;
        window.channels         = audio::kChannels;
        window.frames           = static_cast<rstd::uint32_t>(audio::kWindowFrames);
        const u64 first_frame   = end_frame - u64(audio::kWindowFrames);
        for (rstd::size_t frame = 0; frame < audio::kWindowFrames; ++frame) {
            const u64 source_frame = first_frame + u64(frame);
            for (rstd::size_t channel = 0; channel < audio::kChannels; ++channel) {
                if (source_frame >= input_frames) continue;
                const auto source      = source_frame * u64(audio::kChannels) + u64(channel);
                const auto destination = frame * audio::kChannels + channel;
                window.samples[usize(destination)] = pcm[usize(source.to_primitive())];
            }
        }
        audio::ResponseFrame response {};
        if (engine.analyze(window, response)) {
            captures.push(CaptureFrame {
                .end_sample_frame = end_frame,
                .response         = rstd::move(response),
            });
        }
        end_frame += interval;
    }
    return captures;
}

class SceneRenderer {
public:
    explicit SceneRenderer(const Vec<CaptureFrame>& captures): captures_(captures) {}

    auto render(rstd::uint64_t sequence, double runtime, double frametime, u64 available_end_frame)
        -> RenderFrame {
        const CaptureFrame* latest = nullptr;
        while (capture_index_ < captures_.len() &&
               captures_[capture_index_].end_sample_frame <= available_end_frame) {
            latest = &captures_[capture_index_];
            ++capture_index_;
        }
        if (latest != nullptr) {
            auto response = latest->response;
            processor_.submit(rstd::move(response));
            submitted_end_frame_ = latest->end_sample_frame;
        }

        scene_audio::Buffers buffers {};
        (void)processor_.advance(static_cast<float>(frametime), buffers);
        audio::ResponseFrame response {
            .generation       = buffers.generation,
            .sequence         = buffers.sequence,
            .captured_at_ns   = buffers.captured_at_ns,
            .end_sample_frame = buffers.end_sample_frame,
            .left             = buffers.bands64.left,
            .right            = buffers.bands64.right,
        };
        return RenderFrame {
            .sequence         = sequence,
            .runtime          = runtime,
            .frametime        = frametime,
            .end_sample_frame = submitted_end_frame_,
            .response         = rstd::move(response),
            .scene_buffers    = rstd::move(buffers),
        };
    }

private:
    const Vec<CaptureFrame>&       captures_;
    scene_audio::ResponseProcessor processor_;
    usize                          capture_index_ {};
    u64                            submitted_end_frame_ {};
};

auto RenderFixedTimeline(const Vec<CaptureFrame>& captures, u64 total_frames, u32 fps)
    -> Vec<RenderFrame> {
    auto           frames = Vec<RenderFrame>::make();
    SceneRenderer  renderer(captures);
    rstd::uint64_t frame_index = 0;
    while (true) {
        const u64 render_frame((frame_index * audio::kSampleRate + fps.to_primitive() / 2) /
                               fps.to_primitive());
        if (render_frame > total_frames) break;
        const double runtime   = static_cast<double>(frame_index) / fps.to_primitive();
        const double frametime = frame_index == 0 ? 0.0 : 1.0 / fps.to_primitive();
        frames.push(renderer.render(frame_index + 1, runtime, frametime, render_frame));
        ++frame_index;
    }
    return frames;
}

auto RenderSharedTimeline(const Vec<CaptureFrame>& captures, u32 capture_frames)
    -> Vec<RenderFrame> {
    auto         frames = Vec<RenderFrame>::with_capacity(captures.len());
    const double frametime =
        static_cast<double>(capture_frames.to_primitive()) / audio::kSampleRate;
    for (const auto& capture : captures) {
        frames.push(RenderFrame {
            .sequence = capture.response.sequence,
            .runtime =
                static_cast<double>(capture.end_sample_frame.to_primitive()) / audio::kSampleRate,
            .frametime        = frametime,
            .end_sample_frame = capture.end_sample_frame,
            .response         = capture.response,
            .scene_buffers    = {},
        });
    }
    return frames;
}

double ResponseEnergy(const audio::ResponseFrame& response) {
    double energy = 0.0;
    for (rstd::size_t index = 0; index < audio::kResponseBins; ++index) {
        const double left  = response.left[usize(index)];
        const double right = response.right[usize(index)];
        energy += left * left + right * right;
    }
    return std::sqrt(energy);
}

auto FindAlignment(const Vec<RenderFrame>& official, const Vec<RenderFrame>& current,
                   const DumpArgs& args) -> Alignment {
    auto official_energy = Vec<double>::with_capacity(official.len());
    for (const auto& frame : official) official_energy.push(ResponseEnergy(frame.response));
    auto current_energy = Vec<double>::with_capacity(current.len());
    for (const auto& frame : current) current_energy.push(ResponseEnergy(frame.response));

    Alignment best { .playback_start_ms = args.offset_min_ms, .energy_cosine = -1.0 };
    for (rstd::uint32_t offset = args.offset_min_ms.to_primitive();
         offset <= args.offset_max_ms.to_primitive();
         ++offset) {
        double dot              = 0.0;
        double official_squared = 0.0;
        double current_squared  = 0.0;
        for (usize index; index < official.len(); ++index) {
            const double current_ms = official[index].runtime * 1000.0 - offset;
            if (current_ms < 0.0) continue;
            const auto current_index = static_cast<rstd::uint64_t>(
                std::llround(current_ms * args.fps.to_primitive() / 1000.0));
            if (current_index >= current.len().to_primitive()) continue;
            const double x = current_energy[usize(current_index)];
            const double y = official_energy[index];
            dot += x * y;
            current_squared += x * x;
            official_squared += y * y;
        }
        if (current_squared == 0.0 || official_squared == 0.0) continue;
        const double cosine = dot / std::sqrt(current_squared * official_squared);
        if (cosine > best.energy_cosine) {
            best = Alignment { .playback_start_ms = u32(offset), .energy_cosine = cosine };
        }
        if (offset == std::numeric_limits<rstd::uint32_t>::max()) break;
    }
    return best;
}

auto RenderOfficialTimeline(const Vec<CaptureFrame>& captures, const Vec<RenderFrame>& official,
                            const Alignment& alignment) -> Vec<RenderFrame> {
    auto          frames = Vec<RenderFrame>::make();
    SceneRenderer renderer(captures);
    const double  playback_start = alignment.playback_start_ms.to_primitive() / 1000.0;
    for (usize index; index < official.len(); ++index) {
        const auto&  reference  = official[index];
        const double audio_time = reference.runtime - playback_start;
        u64          available_end {};
        if (audio_time >= 0.0) {
            const auto audio_frame =
                static_cast<rstd::uint64_t>(std::floor(audio_time * audio::kSampleRate));
            available_end = u64(audio_frame);
        }
        frames.push(renderer.render(
            reference.sequence, reference.runtime, reference.frametime, available_end));
    }
    return frames;
}

void ReportComparison(const Vec<RenderFrame>& official, const Vec<RenderFrame>& current,
                      const Alignment& alignment, u64 total_frames) {
    double              dot              = 0.0;
    double              official_squared = 0.0;
    double              current_squared  = 0.0;
    double              error_squared    = 0.0;
    double              absolute_error   = 0.0;
    double              official_sum     = 0.0;
    rstd::uint64_t      samples          = 0;
    rstd::uint64_t      matched_frames   = 0;
    rstd::uint64_t      active_channels  = 0;
    rstd::uint64_t      peak_matches     = 0;
    double              peak_error_sum   = 0.0;
    std::vector<double> peak_errors;
    std::vector<double> amplitude_errors;
    std::vector<double> frame_cosines;
    const double        start = alignment.playback_start_ms.to_primitive() / 1000.0;
    const double duration = static_cast<double>(total_frames.to_primitive()) / audio::kSampleRate;
    for (usize frame; frame < official.len(); ++frame) {
        const double audio_time = official[frame].runtime - start;
        if (audio_time < 0.0 || audio_time > duration) continue;
        ++matched_frames;
        double frame_dot              = 0.0;
        double frame_official_squared = 0.0;
        double frame_current_squared  = 0.0;
        for (rstd::size_t index = 0; index < audio::kResponseBins; ++index) {
            for (const auto pair : {
                     std::pair { current[frame].response.left[usize(index)],
                                 official[frame].response.left[usize(index)] },
                     std::pair { current[frame].response.right[usize(index)],
                                 official[frame].response.right[usize(index)] },
                 }) {
                const double x = pair.first;
                const double y = pair.second;
                dot += x * y;
                current_squared += x * x;
                official_squared += y * y;
                error_squared += (x - y) * (x - y);
                absolute_error += std::abs(x - y);
                official_sum += std::abs(y);
                amplitude_errors.push_back(std::abs(x - y));
                frame_dot += x * y;
                frame_current_squared += x * x;
                frame_official_squared += y * y;
                ++samples;
            }
        }
        if (frame_current_squared > 0.0 && frame_official_squared > 0.0) {
            frame_cosines.push_back(frame_dot /
                                    std::sqrt(frame_current_squared * frame_official_squared));
        }
        for (const auto channels : {
                 std::pair { &current[frame].response.left, &official[frame].response.left },
                 std::pair { &current[frame].response.right, &official[frame].response.right },
             }) {
            rstd::size_t current_peak {};
            rstd::size_t official_peak {};
            for (rstd::size_t index = 1; index < audio::kResponseBins; ++index) {
                if ((*channels.first)[usize(index)] > (*channels.first)[usize(current_peak)]) {
                    current_peak = index;
                }
                if ((*channels.second)[usize(index)] > (*channels.second)[usize(official_peak)]) {
                    official_peak = index;
                }
            }
            if ((*channels.second)[usize(official_peak)] < 0.0001f) continue;
            const auto peak_error = current_peak > official_peak ? current_peak - official_peak
                                                                 : official_peak - current_peak;
            ++active_channels;
            if (peak_error == 0) ++peak_matches;
            peak_error_sum += static_cast<double>(peak_error);
            peak_errors.push_back(static_cast<double>(peak_error));
        }
    }
    const double gain   = current_squared == 0.0 ? 0.0 : dot / current_squared;
    const double cosine = current_squared == 0.0 || official_squared == 0.0
                              ? 0.0
                              : dot / std::sqrt(current_squared * official_squared);
    const double relative_rmse =
        official_squared == 0.0 ? 0.0 : std::sqrt(error_squared / official_squared);
    const double scaled_error_squared =
        official_squared + gain * gain * current_squared - 2.0 * gain * dot;
    const double scaled_relative_rmse =
        official_squared == 0.0 ? 0.0
                                : std::sqrt(std::max(0.0, scaled_error_squared) / official_squared);
    const double normalized_mae = official_sum == 0.0 ? 0.0 : absolute_error / official_sum;
    const auto   percentile     = [](std::vector<double>& values, double fraction) {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        const auto index = static_cast<std::size_t>(
            std::llround(fraction * static_cast<double>(values.size() - 1)));
        return values[index];
    };
    const double peak_match =
        active_channels == 0 ? 0.0 : static_cast<double>(peak_matches) / active_channels;
    const double peak_mean   = active_channels == 0 ? 0.0 : peak_error_sum / active_channels;
    const double peak_median = percentile(peak_errors, 0.5);
    const double amplitude_error_p50 = percentile(amplitude_errors, 0.5);
    const double amplitude_error_p90 = percentile(amplitude_errors, 0.9);
    const double amplitude_error_p95 = percentile(amplitude_errors, 0.95);
    const double frame_cosine_p10    = percentile(frame_cosines, 0.1);
    const double frame_cosine_p50    = percentile(frame_cosines, 0.5);
    std::fprintf(stderr,
                 "Scene log alignment\n"
                 "  playback_start_ms=%u energy_cosine=%.9g\n"
                 "  matched_frames=%llu samples=%llu response_cosine=%.9g\n"
                 "  relative_rmse=%.9g normalized_mae=%.9g\n"
                 "  peak_match=%.9g peak_band_error_mean=%.9g peak_band_error_median=%.9g\n"
                 "  amplitude_abs_error_p50=%.9g p90=%.9g p95=%.9g\n"
                 "  frame_cosine_p10=%.9g frame_cosine_p50=%.9g\n"
                 "  fitted_output_gain=%.9g gain_adjusted_relative_rmse=%.9g\n",
                 alignment.playback_start_ms.to_primitive(),
                 alignment.energy_cosine,
                 static_cast<unsigned long long>(matched_frames),
                 static_cast<unsigned long long>(samples),
                 cosine,
                 relative_rmse,
                 normalized_mae,
                 peak_match,
                 peak_mean,
                 peak_median,
                 amplitude_error_p50,
                 amplitude_error_p90,
                 amplitude_error_p95,
                 frame_cosine_p10,
                 frame_cosine_p50,
                 gain,
                 scaled_relative_rmse);
}

template<rstd::size_t N>
void WriteValues(FILE* output, const rstd::array<float, N>& values) {
    for (rstd::size_t index = 0; index < N; ++index) {
        if (index != 0) std::fputc(',', output);
        std::fprintf(output, "%.9g", static_cast<double>(values[usize(index)]));
    }
    std::fputc('\n', output);
}

template<rstd::size_t N>
void WriteSceneBuffers(FILE* output, const char* marker,
                       const scene_audio::ResolutionBuffers<N>& buffers, const RenderFrame& frame) {
    for (const auto channel : {
             std::pair { "LEFT", &buffers.left },
             std::pair { "RIGHT", &buffers.right },
             std::pair { "AVERAGE", &buffers.average },
         }) {
        std::fprintf(output,
                     "%s_%s %llu %.17g %.17g ",
                     marker,
                     channel.first,
                     static_cast<unsigned long long>(frame.sequence),
                     frame.runtime,
                     frame.frametime);
        WriteValues(output, *channel.second);
    }
}

void WriteFrame(FILE* output, const RenderFrame& frame, bool scene_stage) {
    std::fprintf(output,
                 "WE_SCENE_AUDIO64_WINDOW %llu %llu\n",
                 static_cast<unsigned long long>(frame.sequence),
                 static_cast<unsigned long long>(frame.end_sample_frame.to_primitive()));
    std::fprintf(output,
                 "WE_SCENE_AUDIO64_LEFT %llu %.17g %.17g ",
                 static_cast<unsigned long long>(frame.sequence),
                 frame.runtime,
                 frame.frametime);
    WriteValues(output, frame.response.left);
    std::fprintf(output,
                 "WE_SCENE_AUDIO64_RIGHT %llu %.17g %.17g ",
                 static_cast<unsigned long long>(frame.sequence),
                 frame.runtime,
                 frame.frametime);
    WriteValues(output, frame.response.right);
    if (! scene_stage) return;
    std::fprintf(output,
                 "WE_SCENE_AUDIO64_AVERAGE %llu %.17g %.17g ",
                 static_cast<unsigned long long>(frame.sequence),
                 frame.runtime,
                 frame.frametime);
    WriteValues(output, frame.scene_buffers.bands64.average);
    WriteSceneBuffers(output, "WE_SCENE_AUDIO32", frame.scene_buffers.bands32, frame);
    WriteSceneBuffers(output, "WE_SCENE_AUDIO16", frame.scene_buffers.bands16, frame);
}

int Run(const DumpArgs& args) {
    if (args.fps == u32() || args.capture_frames == u32() ||
        args.offset_min_ms > args.offset_max_ms || ! std::isfinite(args.web_start_ms) ||
        ! std::isfinite(args.web_silence_threshold) || args.web_silence_threshold < 0.0) {
        std::fputs("invalid fps, capture interval, or offset search range\n", stderr);
        return 2;
    }
    if (args.stage != "shared" && args.stage != "scene") {
        std::fputs("--stage must be shared or scene\n", stderr);
        return 2;
    }
    if (args.stage == "shared" && ! args.scene_log.empty()) {
        std::fputs("--scene-log can only be used with --stage scene\n", stderr);
        return 2;
    }

    auto decoded = Decode(args.input);
    if (decoded.is_none()) return 1;
    auto      pcm = rstd::move(decoded).unwrap_unchecked();
    const u64 input_frames(pcm.len().to_primitive() / 2);
    const u64 tail_frames(static_cast<rstd::uint64_t>(args.tail_ms.to_primitive()) * 48);
    const u64 total_frames = input_frames + tail_frames;
    auto      captures     = Vec<CaptureFrame>::make();
    if (args.web_capture.empty()) {
        captures = AnalyzeCaptures(pcm, input_frames, total_frames, args.capture_frames);
    } else {
        auto loaded = LoadWebCapture(args);
        if (loaded.is_none()) return 1;
        captures = rstd::move(loaded).unwrap_unchecked();
    }
    auto frames = args.stage == "shared" ? RenderSharedTimeline(captures, args.capture_frames)
                                         : RenderFixedTimeline(captures, total_frames, args.fps);
    Alignment alignment {};
    bool      aligned = false;
    if (! args.scene_log.empty()) {
        auto loaded = LoadSceneLog(args.scene_log);
        if (loaded.is_none()) return 1;
        auto       official = rstd::move(loaded).unwrap_unchecked();
        const auto fitted   = FindAlignment(official, frames, args);
        if (fitted.energy_cosine < 0.0) {
            std::fputs("cannot align the audio with the Scene log\n", stderr);
            return 1;
        }
        frames = RenderOfficialTimeline(captures, official, fitted);
        ReportComparison(official, frames, fitted, total_frames);
        alignment = fitted;
        aligned   = true;
    }

    FILE* output = stdout;
    if (args.output != "-") {
        output = std::fopen(args.output.c_str(), "wb");
        if (output == nullptr) {
            std::fprintf(stderr, "cannot open output: %s\n", args.output.c_str());
            return 1;
        }
    }

    std::fprintf(output,
                 "WE_SCENE_AUDIO64_BEGIN owe-current-v1 %u\n",
                 aligned ? alignment.playback_start_ms.to_primitive() : 0);
    std::fprintf(output,
                 "WE_SCENE_AUDIO64_CONFIG sample_rate=48000 channels=2 input_frames=%llu "
                 "capture_frames=%u fps=%u tail_ms=%u shared_input=%s stage=%s timeline=%s\n",
                 static_cast<unsigned long long>(input_frames.to_primitive()),
                 args.capture_frames.to_primitive(),
                 args.fps.to_primitive(),
                 args.tail_ms.to_primitive(),
                 args.web_capture.empty() ? "pcm" : "official-web",
                 args.stage.c_str(),
                 aligned                  ? "official-scene"
                 : args.stage == "shared" ? "capture"
                                          : "fixed");
    for (const auto& frame : frames) WriteFrame(output, frame, args.stage == "scene");

    if (output != stdout) std::fclose(output);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    auto args = ParseArgs(argc, argv);
    if (args.is_err()) return args.unwrap_err().code;
    return Run(args.unwrap());
}
