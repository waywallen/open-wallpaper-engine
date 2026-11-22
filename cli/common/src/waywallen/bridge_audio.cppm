export module owe.cli.waywallen.bridge_audio;

import owe.audio_response;
import rstd;
import owe.cli.waywallen.bridge;

export namespace ww_wescene
{

bool DecodeAudioWindow(const ww_bridge_control_t& control, owe::audio::PcmWindow& output,
                       bool& ended) {
    ww_bridge_audio_window_t input {};
    if (ww_bridge_audio_window_from_control(&control, &input) != 0) return false;
    ended = (input.flags & WW_AUDIO_END_OF_STREAM) != 0;
    if (ended) return true;
    output.generation       = input.generation;
    output.sequence         = input.sequence;
    output.captured_at_ns   = input.captured_at_ns;
    output.end_sample_frame = input.end_sample_frame;
    output.sample_rate_hz   = input.sample_rate_hz;
    output.channels         = input.channels;
    output.frames           = input.frames;
    for (rstd::size_t index = 0; index < owe::audio::kSampleCount; ++index) {
        output.samples[rstd::usize(index)] = input.samples[index];
    }
    return true;
}

} // namespace ww_wescene
