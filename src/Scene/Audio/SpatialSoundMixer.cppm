module;

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

export module wescene.scene_spatial_audio;
import rstd;

using namespace rstd::prelude;

export namespace owe
{

class SpatialSoundMixer {
public:
    SpatialSoundMixer()                                    = default;
    SpatialSoundMixer(const SpatialSoundMixer&)            = delete;
    SpatialSoundMixer& operator=(const SpatialSoundMixer&) = delete;
    ~SpatialSoundMixer() { Reset(); }

    bool Init(u32 rate, float reference_distance, float rolloff) {
        Reset();
        auto open = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
            alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
        m_render = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
            alcGetProcAddress(nullptr, "alcRenderSamplesSOFT"));
        m_set_context = reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(
            alcGetProcAddress(nullptr, "alcSetThreadContext"));
        m_get_context = reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(
            alcGetProcAddress(nullptr, "alcGetThreadContext"));
        if (! open || ! m_render || ! m_set_context || ! m_get_context) return false;
        m_device = open(nullptr);
        if (! m_device) return false;
        m_rate                = static_cast<ALCint>(rate.to_primitive());
        ALCint attributes[11] = { ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT,
                                  ALC_FLOAT_SOFT,           ALC_FREQUENCY,   m_rate };
        int    next           = 6;
        if (alcIsExtensionPresent(m_device, "ALC_SOFT_HRTF")) {
            attributes[next++] = ALC_HRTF_SOFT;
            attributes[next++] = ALC_FALSE;
        }
        if (alcIsExtensionPresent(m_device, "ALC_SOFT_output_limiter")) {
            attributes[next++] = ALC_OUTPUT_LIMITER_SOFT;
            attributes[next++] = ALC_FALSE;
        }
        m_context = alcCreateContext(m_device, attributes);
        if (! m_context) return false;

        CurrentContext context(*this);
        if (! context || ! alIsExtensionPresent("AL_EXT_FLOAT32")) return false;
        alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
        alGenSources(1, &m_source);
        alGenBuffers(2, m_buffers);
        alSourcei(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(m_source, AL_REFERENCE_DISTANCE, reference_distance);
        alSourcef(m_source, AL_ROLLOFF_FACTOR, rolloff);
        return alGetError() == AL_NO_ERROR;
    }

    bool Render(mut_ref<float[]> mono, const array<float, 3>& position, float* stereo) {
        CurrentContext context(*this);
        if (! context) return false;
        // Leave one sample queued so the source stays playing across pulls.
        // Discard that sample from the next render, not from the decoded PCM.
        const auto frames = mono.len() - usize(1);
        mono[frames]      = mono[frames - usize(1)];
        ALint processed {};
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0) {
            ALuint old {};
            alSourceUnqueueBuffers(m_source, 1, &old);
        }
        const ALuint buffer = m_buffers[m_next_buffer];
        alBufferData(buffer,
                     AL_FORMAT_MONO_FLOAT32,
                     mono.as_raw_ptr(),
                     static_cast<ALsizei>(mono.len().to_primitive() * sizeof(float)),
                     m_rate);
        alSourceQueueBuffers(m_source, 1, &buffer);
        alSource3f(
            m_source, AL_POSITION, position[usize()], position[usize(1)], position[usize(2)]);
        if (! m_started) alSourcePlay(m_source);
        if (alGetError() != AL_NO_ERROR) return false;
        if (m_started) {
            const auto count = mono.len() * usize(2);
            if (m_stereo.len() < count) m_stereo.resize(count, 0.0f);
            m_render(m_device, m_stereo.data(), static_cast<ALCsizei>(mono.len().to_primitive()));
            rstd::mem::memcpy(stereo, m_stereo.data() + 2, frames * usize(2 * sizeof(float)));
        } else {
            m_render(m_device, stereo, static_cast<ALCsizei>(frames.to_primitive()));
        }
        m_started = true;
        m_next_buffer ^= 1;
        return alcGetError(m_device) == ALC_NO_ERROR;
    }

    void Stop() {
        if (! m_started) return;
        CurrentContext context(*this);
        if (context) {
            alSourceStop(m_source);
            alSourcei(m_source, AL_BUFFER, 0);
            // Consume the stopped voice's de-click tail before starting another clip.
            float discarded[2];
            m_render(m_device, discarded, 1);
        }
        m_started     = false;
        m_next_buffer = 0;
    }

    void Reset() {
        if (m_context) alcDestroyContext(m_context);
        if (m_device) alcCloseDevice(m_device);
        m_context     = nullptr;
        m_device      = nullptr;
        m_started     = false;
        m_next_buffer = 0;
    }

private:
    class CurrentContext {
    public:
        explicit CurrentContext(const SpatialSoundMixer& owner)
            : m_set(owner.m_set_context),
              m_previous(owner.m_get_context()),
              m_active(m_set(owner.m_context) != ALC_FALSE) {}
        ~CurrentContext() {
            if (m_active) m_set(m_previous);
        }
        explicit operator bool() const { return m_active; }

    private:
        PFNALCSETTHREADCONTEXTPROC m_set;
        ALCcontext*                m_previous;
        bool                       m_active;
    };

    ALCdevice*                 m_device {};
    ALCcontext*                m_context {};
    LPALCRENDERSAMPLESSOFT     m_render {};
    PFNALCSETTHREADCONTEXTPROC m_set_context {};
    PFNALCGETTHREADCONTEXTPROC m_get_context {};
    ALuint                     m_source {};
    ALuint                     m_buffers[2] {};
    ALsizei                    m_rate {};
    Vec<float>                 m_stereo;
    bool                       m_started {};
    int                        m_next_buffer {};
};

} // namespace owe
