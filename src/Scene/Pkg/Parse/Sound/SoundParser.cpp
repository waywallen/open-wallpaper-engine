module wescene.pkg.parse;
import wescene.scene_spatial_audio;
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

bool SceneSoundMountState::IsCanceled() {
    auto state = m_state.lock().unwrap_unchecked();
    return state->canceled;
}

bool SceneSoundMountState::Attach(wavsen::audio::SoundStreamId id) {
    auto state = m_state.lock().unwrap_unchecked();
    if (state->canceled || state->id.is_some()) return false;
    state->id = Some(id);
    return true;
}

auto SceneSoundMountState::Cancel() -> Option<wavsen::audio::SoundStreamId> {
    auto state      = m_state.lock().unwrap_unchecked();
    state->canceled = true;
    return state->id.take();
}

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

class SoundLease final {
public:
    SoundLease(Arc<SceneSoundMountState> mount, Option<SceneSoundUnmountCallback> unmount)
        : m_mount(rstd::move(mount)), m_unmount(rstd::move(unmount)) {}
    ~SoundLease() {
        auto id = m_mount->Cancel();
        if (id.is_some() && m_unmount.is_some()) (**m_unmount)(*id);
    }

private:
    Arc<SceneSoundMountState>         m_mount;
    Option<SceneSoundUnmountCallback> m_unmount;
};

class SoundControl final {
public:
    SoundControl(Arc<SoundState> state, Option<Arc<SoundLease>> lease)
        : m_state(rstd::move(state)), m_lease(rstd::move(lease)) {}

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
    Arc<SoundState>         m_state;
    Option<Arc<SoundLease>> m_lease;
};

} // namespace

class SoundStream : public wavsen::audio::SoundStream {
public:
    struct Config {
        f32          maxtime { 10.0f };
        f32          mintime {};
        f32          volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
        float        reference_distance { 1.0f };
        float        rolloff { 1.0f };
    };
    SoundStream(Vec<String> paths, Arc<fs::VFS> vfs, Config c, Arc<SoundState> state,
                Option<Arc<SceneAudioAverage>>  audio_average,
                Option<Arc<SceneSoundPosition>> position)
        : vfs(rstd::move(vfs)),
          m_config(c),
          m_state(rstd::move(state)),
          m_soundPaths(rstd::move(paths)),
          m_audio_average(rstd::move(audio_average)),
          m_position(rstd::move(position)) {};
    virtual ~SoundStream() = default;

    u64 next_pcm(void* pData, u32 frameCount) override {
        SyncControl();
        if (m_dead) return u64();
        if (! m_state->playing.load(Ordering::Acquire)) return u64();

        const auto frameReads = m_config.mode == PlaybackMode::Random && m_config.maxtime > f32()
                                    ? ReadRandom(static_cast<float*>(pData), frameCount)
                                    : ReadContinuous(pData, frameCount);
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
    void pass_desc(const Desc& d) override {
        if (m_desc.channels == d.channels && m_desc.sample_rate == d.sample_rate) return;
        m_desc = d;
        m_spatial.Reset();
        m_spatial_attempted = false;
        m_spatial_ready     = false;
        if (m_curActive && ! RetargetClip(*m_curActive)) m_curActive = None();
    }

private:
    bool RetargetClip(wavsen::audio::StreamDecoder& clip) {
        if (m_position && m_desc.channels == u32(2) && clip.channels() == u32(1) &&
            ! m_spatial_attempted) {
            m_spatial_attempted = true;
            m_spatial_ready =
                m_spatial.Init(m_desc.sample_rate, m_config.reference_distance, m_config.rolloff);
            if (! m_spatial_ready) {
                m_spatial.Reset();
                rstd::log::warn(
                    "SoundStream: OpenAL loopback unavailable; using ordinary playback");
            }
        }
        m_spatial_clip = m_spatial_ready && clip.channels() == u32(1);
        if (m_spatial_clip)
            clip.retarget({ u32(1), m_desc.sample_rate });
        else
            clip.retarget({ m_desc.channels, m_desc.sample_rate });
        return clip.error().kind == wavsen::audio::MediaErrorKind::None;
    }

    auto ReadClip(void* output, u32 frames) -> u64 {
        if (! m_spatial_clip) return m_curActive->next_pcm(output, frames);
        const auto count = rstd::as_cast<usize>(frames);
        if (m_mono.len() <= count) m_mono.resize(count + usize(1), 0.0f);
        const auto produced = m_curActive->next_pcm(m_mono.data(), frames);
        if (produced == u64()) return produced;
        auto* stereo = static_cast<float*>(output);
        auto  mono   = slice<float>::from_raw_parts(m_mono.data(), rstd::as_cast<usize>(produced));
        const auto position = [&] {
            auto locked = (*m_position)->lock().unwrap_unchecked();
            return array<float, 3>(*locked);
        }();
        if (m_spatial_ready) {
            auto queued = mut_ref<float[]>::from_raw_parts(m_mono.data(), mono.len() + usize(1));
            if (m_spatial.Render(queued, position, stereo)) return produced;
            m_spatial_ready = false;
            m_spatial.Reset();
            rstd::log::warn("SoundStream: OpenAL loopback render failed; using ordinary playback");
        }
        for (const float sample : mono) {
            const float value = sample * 0.70710678f;
            *stereo++         = value;
            *stereo++         = value;
        }
        return produced;
    }

    auto ReadContinuous(void* output, u32 frames) -> u64 {
        if (! m_curActive) Switch();
        if (! m_curActive) return u64();
        const auto produced = ReadClip(output, frames);
        if (produced != u64()) return produced;

        m_curActive = None();
        if (m_config.mode == PlaybackMode::Single) {
            m_state->playing.store(false, Ordering::Release);
            return u64();
        }
        Switch();
        return m_curActive ? ReadClip(output, frames) : u64();
    }

    auto NextRandomGap() const -> u64 {
        const auto lo      = double(m_config.mintime.to_primitive());
        const auto hi      = double(m_config.maxtime.to_primitive());
        const auto seconds = lo == hi ? lo : Random::get<double>(lo, hi);
        const auto frames  = f64(seconds * m_desc.sample_rate.to_primitive()).ceil().to_primitive();
        if (frames >= double(u64::MAX.to_primitive())) return u64::MAX;
        return u64(static_cast<rstd::uint64_t>(frames));
    }

    auto ReadRandom(float* output, u32 frames) -> u64 {
        const auto channels = rstd::as_cast<usize>(m_desc.channels);
        const auto capacity = rstd::as_cast<u64>(frames);
        u64        written {};
        u64        offset {};
        bool       retried_empty = false;
        while (offset < capacity) {
            const auto available = capacity - offset;
            const auto skipped   = m_random_gap.min(available);
            m_random_gap -= skipped;
            offset += skipped;
            if (skipped == available) break;

            if (! m_curActive) Switch();
            if (! m_curActive) break;

            const auto request  = available - skipped;
            auto*      next     = output + (rstd::as_cast<usize>(offset) * channels).to_primitive();
            const auto produced = ReadClip(next, rstd::as_cast<u32>(request));
            if (produced != u64()) {
                // The mixer supplies trailing silence, but any gap before new PCM is ours.
                if (written != offset) {
                    auto* start =
                        output + (rstd::as_cast<usize>(written) * channels).to_primitive();
                    auto samples = rstd::as_cast<usize>(offset - written) * channels;
                    rstd::mem::memset(start, u8(), samples * usize(sizeof(float)));
                }
                offset += produced;
                written       = offset;
                retried_empty = false;
            }
            if (produced == request) break;

            m_curActive  = None();
            m_random_gap = NextRandomGap();
            if (produced == u64() && m_random_gap == u64()) {
                if (retried_empty) break;
                retried_empty = true;
            }
        }
        return written;
    }

    // Walk paths until one opens. If all fail, disable the stream so the
    // audio callback stops re-trying every tick (which spammed FFmpeg's
    // demuxer-probe errors at audio-callback rate).
    void Switch() {
        m_spatial.Stop();
        m_curActive  = None();
        const auto n = rstd::as_cast<u32>(m_soundPaths.len());
        if (n == u32()) {
            m_dead = true;
            return;
        }
        const u32 base = SelectStartIndex(n);
        for (u32 tried {}; tried < n; ++tried) {
            const auto& path = m_soundPaths[rstd::as_cast<usize>((base + tried) % n)];
            auto source      = vfs->open_read(fs::Path(rstd::format("/assets/{}", path).as_str()));
            if (source.is_err()) continue;
            auto handle =
                rstd::io::ReadSeekHandle::make(rstd::move(source).unwrap_unchecked().into_reader());
            wavsen::audio::StreamDecoder stream;
            if (stream.open(rstd::move(handle), { m_desc.channels, m_desc.sample_rate })) {
                if (m_position && ! RetargetClip(stream)) continue;
                m_curActive = Some(rstd::move(stream));
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

    void SyncControl() {
        const u32 stop_seq = m_state->stop_seq.load(Ordering::Acquire);
        if (stop_seq != m_seenStopSeq) {
            m_spatial.Stop();
            m_seenStopSeq = stop_seq;
            m_curActive   = None();
            m_random_gap  = u64();
            m_dead        = false;
        }
        const u32 play_seq = m_state->play_seq.load(Ordering::Acquire);
        if (play_seq != m_seenPlaySeq) {
            m_spatial.Stop();
            m_seenPlaySeq = play_seq;
            m_curActive   = None();
            m_random_gap  = u64();
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

    Arc<fs::VFS>    vfs;
    Config          m_config;
    Desc            m_desc {};
    Arc<SoundState> m_state;
    u32             m_curIndex {};
    u32             m_seenPlaySeq {};
    u32             m_seenStopSeq {};
    u64             m_random_gap {};
    bool            m_dead { false };

    const Vec<String>                    m_soundPaths;
    Option<wavsen::audio::StreamDecoder> m_curActive;
    Option<Arc<SceneAudioAverage>>       m_audio_average;
    Option<Arc<SceneSoundPosition>>      m_position;
    SpatialSoundMixer                    m_spatial;
    Vec<float>                           m_mono;
    bool                                 m_spatial_attempted {};
    bool                                 m_spatial_ready {};
    bool                                 m_spatial_clip {};
};

auto SoundParser::Prepare(const wpscene::SoundObject& obj, fs::VFS& vfs, Scene* scene,
                          Option<SceneSoundUnmountCallback> unmount) -> ScenePreparedSound {
    const auto min_delay = f32(obj.mintime).is_finite() ? f32(obj.mintime).max(f32()) : f32();
    const auto max_delay = f32(obj.maxtime).is_finite() ? f32(obj.maxtime).max(f32()) : f32();
    SoundStream::Config config { .maxtime = min_delay.max(max_delay),
                                 .mintime = min_delay.min(max_delay),
                                 .volume  = f32(obj.volume).clamp(f32(), f32(1.0f)),
                                 .mode    = ToPlaybackMode(obj.playbackmode.as_str()),
                                 .reference_distance =
                                     f32(obj.mindistance).is_finite()
                                         ? f32(obj.mindistance).max(f32()).to_primitive()
                                         : 1.0f,
                                 .rolloff = f32(obj.attenuation).is_finite()
                                                ? f32(obj.attenuation).max(f32()).to_primitive()
                                                : 1.0f };

    Option<Arc<SceneAudioAverage>> audio_average = None();
    if (scene != nullptr) audio_average = Some(scene->AudioAverageHandle());
    auto state = Arc<SoundState>::make();
    // The node's sound control is attached after its visibility is applied, so
    // a layer that starts hidden has to be silenced here or it would play until
    // something toggles it.
    state->playing.store(obj.visible && ! obj.startsilent, Ordering::Release);
    state->volume.store(config.volume, Ordering::Release);
    Option<Arc<SceneSoundPosition>> position;
    if (obj.spatialization) position = Some(Arc<SceneSoundPosition>::make(array<float, 3> {}));
    auto ss    = Box<SoundStream>::make(obj.sound.clone(),
                                        vfs.Snapshot(),
                                        config,
                                        state.clone(),
                                        rstd::move(audio_average),
                                        position.clone());
    auto mount = Arc<SceneSoundMountState>::make();
    auto lease = Arc<SoundLease>::make(mount.clone(), rstd::move(unmount));
    return {
        .control = Arc<dyn<SceneSoundControl>>::make(
            SoundControl(rstd::move(state), Some(rstd::move(lease)))),
        .stream = Box<dyn<wavsen::audio::SoundStreamObject>>::from_raw(
            dyn<wavsen::audio::SoundStreamObject>::from_ptr(rstd::move(ss).into_raw())),
        .mount    = rstd::move(mount),
        .position = rstd::move(position),
    };
}

namespace
{
auto ListenerPosition(Scene& scene, SceneNode& node, bool orthographic) -> array<float, 3> {
    auto camera = scene.ActiveCamera();
    if (! camera) return {};

    node.UpdateTrans();
    const Eigen::Vector3d world    = node.ModelTrans().block<3, 1>(0, 3);
    const auto            listener = (*camera)->Transforms();
    const Eigen::Vector3d delta    = world - listener.eye;
    const Eigen::Vector3d forward  = (listener.center - listener.eye).normalized();
    const Eigen::Vector3d right    = forward.cross(listener.up).normalized();
    const Eigen::Vector3d up       = right.cross(forward);
    Eigen::Vector3d       position(right.dot(delta), up.dot(delta), -forward.dot(delta));
    if (orthographic) {
        auto         extent = scene.Ortho();
        const double size =
            (rstd::as_cast<double>(extent[usize()]) + rstd::as_cast<double>(extent[usize(1)])) *
            0.25;
        if (size <= 0.0) return {};
        position = position.normalized() * (rstd::cmp::max(0.0, size - world.z()) / size);
    }
    const Eigen::Vector3f value = position.cast<float>();
    if (! value.allFinite()) return {};
    return { value.x(), value.y(), value.z() };
}
} // namespace

void SoundParser::TrackPosition(Scene& scene, SceneNode& node, Arc<SceneSoundPosition> state,
                                bool orthographic) {
    auto publish = [&scene, &node, state = rstd::move(state), orthographic](f64) {
        const auto position = ListenerPosition(scene, node, orthographic);
        auto       locked   = state->lock().unwrap_unchecked();
        *locked             = position;
    };
    publish(f64());
    scene.RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>>::make(rstd::move(publish)),
                                   scene.RegisterNode(node));
}
