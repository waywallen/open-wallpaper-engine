export module viewer.audio;

export import wavsen.audio;
import owe.audio_response;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Condvar;
using rstd::sync::Mutex;
using rstd::thread::JoinHandle;
using rstd::time::Duration;
using rstd::time::Instant;

export namespace viewer
{

inline owe::audio::PcmWindow ConvertAudioWindow(const wavsen::audio::AudioPcmWindow& source) {
    owe::audio::PcmWindow destination {};
    destination.generation       = source.generation;
    destination.sequence         = source.sequence;
    destination.captured_at_ns   = source.captured_at_ns;
    destination.end_sample_frame = source.end_sample_frame;
    destination.sample_rate_hz   = source.sample_rate_hz;
    destination.channels         = source.channels;
    destination.frames           = source.frames;
    for (rstd::size_t index = 0; index < owe::audio::kSampleCount; ++index) {
        destination.samples[rstd::usize(index)] = source.samples[rstd::usize(index)].to_primitive();
    }
    return destination;
}

// CoreAudio initialization can block on system permission; keep it off the Cocoa thread.
class AudioCaptureWorker {
public:
    AudioCaptureWorker()
        : m_thread(rstd::thread::spawn([this] {
                       run();
                   }).unwrap()) {}
    ~AudioCaptureWorker() {
        {
            auto state     = m_state.lock().unwrap();
            state->stop    = true;
            state->enabled = false;
        }
        m_condition.notify_one();
        rstd::move(m_thread).join().unwrap();
    }

    void set_enabled(bool enabled) {
        {
            auto state = m_state.lock().unwrap();
            if (state->enabled == enabled) return;
            state->enabled = enabled;
        }
        m_condition.notify_one();
    }

    bool snapshot(wavsen::audio::AudioPcmWindow& out) {
        auto state = m_state.lock().unwrap();
        if (! state->latest || (state->latest->generation == state->last_generation &&
                                state->latest->sequence == state->last_sequence))
            return false;
        out                    = *state->latest;
        state->last_generation = state->latest->generation;
        state->last_sequence   = state->latest->sequence;
        return true;
    }

private:
    struct State {
        bool                                  stop {};
        bool                                  enabled {};
        Option<wavsen::audio::AudioPcmWindow> latest;
        rstd::uint64_t                        last_generation {};
        rstd::uint64_t                        last_sequence {};
    };

    void run() {
        wavsen::audio::AudioCapture capture;
        bool                        capture_ready = false;
        auto                        retry_at      = Instant::now();
        for (;;) {
            {
                auto state = m_state.lock().unwrap();
                m_condition.wait_while(state, [](const State& value) {
                    return ! value.stop && ! value.enabled;
                });
                if (state->stop) break;
            }
            if (! capture_ready) {
                {
                    auto state = m_state.lock().unwrap();
                    auto now   = Instant::now();
                    auto delay = retry_at > now ? retry_at - now : Duration {};
                    m_condition.wait_timeout_while(state, delay, [](const State& value) {
                        return ! value.stop && value.enabled;
                    });
                    if (state->stop) break;
                    if (! state->enabled) continue;
                }
                if (! capture.init()) {
                    retry_at = Instant::now() + Duration::from_secs(u64(1));
                    continue;
                }
                capture_ready = true;
            }
            wavsen::audio::AudioPcmWindow window {};
            if (capture.snapshot(window)) {
                auto state = m_state.lock().unwrap();
                if (! state->stop && state->enabled) state->latest = Some(window);
            }
            {
                auto state = m_state.lock().unwrap();
                if (m_condition
                        .wait_timeout_while(state,
                                            Duration::from_millis(u64(5)),
                                            [](const State& value) {
                                                return ! value.stop && value.enabled;
                                            })
                        .timed_out())
                    continue;
                if (state->stop) break;
            }
            capture.uninit();
            capture_ready                   = false;
            retry_at                        = Instant::now();
            m_state.lock().unwrap()->latest = None();
        }
        if (capture_ready) capture.uninit();
    }

    Mutex<State>     m_state;
    Condvar          m_condition;
    JoinHandle<void> m_thread;
};

} // namespace viewer
