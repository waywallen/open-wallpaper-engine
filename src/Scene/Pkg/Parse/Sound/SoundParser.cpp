module wescene.pkg.parse;
import wescene.core;
import wescene.scene;
import rstd.log;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace owe;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

enum class PlaybackMode
{
    Random,
    Loop,
    Single
};

static PlaybackMode ToPlaybackMode(ref<str> s) {
    if (s == "loop"_str)
        return PlaybackMode::Loop;
    else if (s == "random"_str)
        return PlaybackMode::Random;
    else if (s == "single"_str)
        return PlaybackMode::Single;
    return PlaybackMode::Loop;
};

namespace
{

struct SoundState {
    Atomic<bool> playing { false };
    Atomic<f32>  volume { f32(1.0f) };
    Atomic<u32>  play_seq {};
    Atomic<u32>  stop_seq {};
};

class SoundControl final {
public:
    explicit SoundControl(Arc<SoundState> state): m_state(rstd::move(state)) {}

    void Play() {
        m_state->playing.store(true, Ordering::Release);
        m_state->play_seq.fetch_add(u32(1), Ordering::AcqRel);
    }
    void Stop() {
        m_state->playing.store(false, Ordering::Release);
        m_state->stop_seq.fetch_add(u32(1), Ordering::AcqRel);
    }
    void Pause() { m_state->playing.store(false, Ordering::Release); }
    bool IsPlaying() const { return m_state->playing.load(Ordering::Acquire); }
    void SetVolume(float volume) {
        m_state->volume.store(f32(volume).clamp(f32(), f32(1.0f)), Ordering::Release);
    }

private:
    Arc<SoundState> m_state;
};

} // namespace

class SoundStream : public wavsen::audio::SoundStream {
public:
    struct Config {
        f32          maxtime { 10.0f };
        f32          mintime {};
        f32          volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    SoundStream(Vec<String> paths, fs::VFS& vfs, Config c, Arc<SoundState> state,
                Option<Arc<SceneAudioAverage>> audio_average)
        : vfs(vfs),
          m_config(c),
          m_state(rstd::move(state)),
          m_soundPaths(rstd::move(paths)),
          m_audio_average(rstd::move(audio_average)) {};
    virtual ~SoundStream() = default;

    u64 next_pcm(void* pData, u32 frameCount) override {
        SyncControl();
        if (m_dead) return u64();
        if (! m_state->playing.load(Ordering::Acquire)) return u64();

        if (! m_curActive) {
            Switch();
        }

        u64 frameReads = m_curActive ? (*m_curActive)->stream().next_pcm(pData, frameCount) : u64();
        if (frameReads == u64() && ! m_dead) {
            m_curActive = None();
            if (m_config.mode == PlaybackMode::Single) {
                m_state->playing.store(false, Ordering::Release);
                return u64();
            }
            Switch();
            frameReads = m_curActive ? (*m_curActive)->stream().next_pcm(pData, frameCount) : u64();
        }
        UpdateAudioAverage(pData, frameReads);
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num =
                usize(frameReads.to_primitive()) * usize(m_desc.channels.to_primitive());
            const auto volume = m_state->volume.load(Ordering::Acquire).to_primitive();
            for (usize i {}; i < num; ++i, ++pData_float) {
                (*pData_float) *= volume;
            }
        }
        return frameReads;
    };
    void pass_desc(const Desc& d) override { m_desc = d; }

    // Walk paths until one opens. If all fail, disable the stream so the
    // audio callback stops re-trying every tick (which spammed FFmpeg's
    // demuxer-probe errors at audio-callback rate).
    void Switch() {
        m_curActive  = None();
        const auto n = rstd::as_cast<u32>(m_soundPaths.len());
        if (n == u32()) {
            m_dead = true;
            return;
        }
        const u32 base = SelectStartIndex(n);
        for (u32 tried {}; tried < n; ++tried) {
            const auto& path   = m_soundPaths[rstd::as_cast<usize>((base + tried) % n)];
            auto        source = vfs.open_read(fs::Path(rstd::format("/assets/{}", path).as_str()));
            if (source.is_err()) continue;
            auto handle =
                rstd::io::ReadSeekHandle::make(rstd::move(source).unwrap_unchecked().into_reader());
            auto stream = wavsen::audio::make_stream(rstd::move(handle), m_desc);
            if (stream) {
                m_curActive = rstd::move(stream);
                return;
            }
        }
        m_dead = true;
        m_state->playing.store(false, Ordering::Release);
        rstd::log::warn("SoundStream: all {} sound path(s) failed to open; disabling stream", n);
    }
    u32 SelectStartIndex(u32 n) {
        if (n == u32()) return u32();
        if (m_config.mode == PlaybackMode::Random) {
            return u32(Random::get<uint32_t>(0, (n - u32(1)).to_primitive()));
        }
        if (m_config.mode == PlaybackMode::Single) {
            return u32(Random::get<uint32_t>(0, (n - u32(1)).to_primitive()));
        }
        u32 idx    = m_curIndex;
        m_curIndex = (m_curIndex + u32(1)) % n;
        return idx;
    }

private:
    void SyncControl() {
        const u32 stop_seq = m_state->stop_seq.load(Ordering::Acquire);
        if (stop_seq != m_seenStopSeq) {
            m_seenStopSeq = stop_seq;
            m_curActive   = None();
            m_dead        = false;
        }
        const u32 play_seq = m_state->play_seq.load(Ordering::Acquire);
        if (play_seq != m_seenPlaySeq) {
            m_seenPlaySeq = play_seq;
            m_curActive   = None();
            m_dead        = false;
        }
    }

    void UpdateAudioAverage(const void* pData, u64 frameReads) {
        if (m_audio_average.is_none() || frameReads == u64() || m_desc.channels == u32()) return;

        const float* samples = static_cast<const float*>(pData);
        const auto total = usize(frameReads.to_primitive()) * usize(m_desc.channels.to_primitive());
        if (total == usize()) return;

        const usize bin_count = (*m_audio_average)->Len();
        for (usize bin {}; bin < bin_count; ++bin) {
            const auto begin = bin * total / bin_count;
            const auto end   = (bin + usize(1)) * total / bin_count;
            if (end <= begin) continue;

            float sum = 0.0f;
            for (usize i = begin; i < end; ++i)
                sum += f32(samples[i.to_primitive()]).abs().to_primitive();
            float level = rstd::cmp::min(
                1.0f, rstd::cmp::max(0.0f, sum / static_cast<float>((end - begin).to_primitive())));

            const float old = (*m_audio_average)->Load(bin).to_primitive();
            (*m_audio_average)->Store(bin, f32(rstd::cmp::max(level, old * 0.75f)));
        }
    }

    fs::VFS&        vfs;
    Config          m_config;
    Desc            m_desc;
    Arc<SoundState> m_state;
    u32             m_curIndex {};
    u32             m_seenPlaySeq {};
    u32             m_seenStopSeq {};
    bool            m_dead { false };

    const Vec<String>                                  m_soundPaths;
    Option<Box<dyn<wavsen::audio::SoundStreamObject>>> m_curActive;
    Option<Arc<SceneAudioAverage>>                     m_audio_average;
};

Arc<dyn<SceneSoundControl>> SoundParser::Parse(const wpscene::SoundObject& obj, fs::VFS& vfs,
                                               wavsen::audio::SoundManager& sm, Scene* scene) {
    SoundStream::Config config { .maxtime = f32(obj.maxtime),
                                 .mintime = f32(obj.mintime),
                                 .volume  = f32(obj.volume).clamp(f32(), f32(1.0f)),
                                 .mode    = ToPlaybackMode(obj.playbackmode.as_str()) };

    Option<Arc<SceneAudioAverage>> audio_average = None();
    if (scene != nullptr) audio_average = Some(scene->AudioAverageHandle());
    auto state = Arc<SoundState>::make();
    // The node's sound control is attached after its visibility is applied, so
    // a layer that starts hidden has to be silenced here or it would play until
    // something toggles it.
    state->playing.store(obj.visible && ! obj.startsilent, Ordering::Release);
    state->volume.store(config.volume, Ordering::Release);
    auto control = Arc<dyn<SceneSoundControl>>::make(SoundControl(state.clone()));
    auto ss      = Box<SoundStream>::make(
        obj.sound.clone(), vfs, config, rstd::move(state), rstd::move(audio_average));
    sm.mount(Box<dyn<wavsen::audio::SoundStreamObject>>::from_raw(
        dyn<wavsen::audio::SoundStreamObject>::from_ptr(rstd::move(ss).into_raw())));
    return control;
}
