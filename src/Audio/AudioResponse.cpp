module;

#include <cmath>

module owe.audio_response;

import owe.fft;
import rstd;

namespace owe::audio
{

using namespace rstd::prelude;

namespace
{

constexpr rstd::size_t kFftFrames      = 2089;
constexpr rstd::size_t kAnalysisFrames = 1392;
constexpr rstd::size_t kSpectrumBins   = 640;
constexpr float        kBandExponent   = 0.25f;
constexpr float        kWeightBase     = 0.5009999871253967f;
constexpr float        kInputScale     = 127.0f;
constexpr float        kOutputScale    = 0.001f * 640.0f / (2089.0f * 0.5f);

const fft::DftPlan32& Plan() {
    static const fft::DftPlan32 plan(kFftFrames);
    return plan;
}

void analyze_channel(const PcmWindow& window, rstd::size_t channel,
                     rstd::array<float, kResponseBins>& output, fft::DftWorkspace32& workspace) {
    rstd::array<fft::Complex32, kFftFrames> input {};
    rstd::array<fft::Complex32, kFftFrames> spectrum {};
    constexpr float                         baseline_reciprocal = 1.0f / kInputScale;
    for (auto& value : input) value = { .real = kInputScale, .imag = baseline_reciprocal };

    constexpr auto first_frame = kWindowFrames - kAnalysisFrames;
    bool           has_signal  = false;
    for (rstd::size_t frame = 0; frame < kAnalysisFrames; ++frame) {
        const auto  sample_frame  = first_frame + frame;
        const float sample        = window.samples[usize(sample_frame * kChannels + channel)];
        const float finite_sample = f32(sample).is_finite() ? sample : 0.0f;
        has_signal                = has_signal || finite_sample != 0.0f;
        const float value         = finite_sample * kInputScale + kInputScale;
        const float reciprocal    = value == 0.0f ? baseline_reciprocal : 1.0f / value;
        input[usize(frame)]       = { .real = value, .imag = reciprocal };
    }
    if (! has_signal) return;
    Plan().forward(input.as_slice(), spectrum.as_mut_slice(), workspace);

    rstd::size_t band = 0;
    for (rstd::size_t bin = 1; bin < kSpectrumBins; ++bin) {
        const float coordinate =
            static_cast<float>(bin - 1) / static_cast<float>(kSpectrumBins - 1);
        const auto candidate =
            static_cast<rstd::size_t>(f32(coordinate).powf(f32(kBandExponent)).to_primitive() *
                                      static_cast<float>(kResponseBins));
        band              = rstd::cmp::min(candidate, band + 1);
        const float angle = f32::consts::PI.to_primitive() * coordinate;
        const float weight =
            f32(kWeightBase - (1.0f - kWeightBase) * f32(angle).cos().to_primitive())
                .sqrt()
                .to_primitive();
        const auto  value = spectrum[usize(bin)];
        const float magnitude =
            f32(value.real * value.real + value.imag * value.imag).sqrt().to_primitive();
        output[usize(band)] =
            rstd::cmp::max(output[usize(band)], magnitude * weight * kOutputScale);
    }
}

} // namespace

bool ResponseEngine::analyze(const PcmWindow& window, ResponseFrame& output) {
    if (window.sample_rate_hz != kSampleRate || window.channels != kChannels ||
        window.frames != kWindowFrames || window.sequence == 0)
        return false;
    if (primed_ && window.generation == generation_ && window.sequence <= sequence_) return false;
    if (primed_ && window.generation < generation_) return false;

    ResponseFrame response {};
    response.generation       = window.generation;
    response.sequence         = window.sequence;
    response.captured_at_ns   = window.captured_at_ns;
    response.end_sample_frame = window.end_sample_frame;
    analyze_channel(window, 0, response.left, dft_workspace_);
    analyze_channel(window, 1, response.right, dft_workspace_);
    output      = response;
    generation_ = window.generation;
    sequence_   = window.sequence;
    primed_     = true;
    return true;
}

void ResponseEngine::end() {
    generation_ = 0;
    sequence_   = 0;
    primed_     = false;
}

} // namespace owe::audio
