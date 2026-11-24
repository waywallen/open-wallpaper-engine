module;

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

export module wescene.scene_spatial_audio;
import rstd;
import rstd.dlopn;
import rstd.log;

using namespace rstd::prelude;

namespace owe::openal
{

#if __is_target_os(macos)
constexpr const char* kSoname = "libopenal.1.dylib";
#else
constexpr const char* kSoname = "libopenal.so.1";
#endif

struct Api {
    decltype(&alcCloseDevice)         close_device {};
    decltype(&alcCreateContext)       create_context {};
    decltype(&alcDestroyContext)      destroy_context {};
    decltype(&alcGetError)            device_error {};
    decltype(&alcIsExtensionPresent)  device_has_extension {};
    decltype(&alIsExtensionPresent)   context_has_extension {};
    decltype(&alDistanceModel)        distance_model {};
    decltype(&alGetError)             context_error {};
    decltype(&alGenSources)           gen_sources {};
    decltype(&alGenBuffers)           gen_buffers {};
    decltype(&alGetSourcei)           get_source_i {};
    decltype(&alSourcei)              source_i {};
    decltype(&alSourcef)              source_f {};
    decltype(&alSource3f)             source_3f {};
    decltype(&alSourceQueueBuffers)   queue_buffers {};
    decltype(&alSourceUnqueueBuffers) unqueue_buffers {};
    decltype(&alBufferData)           buffer_data {};
    decltype(&alSourcePlay)           play {};
    decltype(&alSourceStop)           stop {};
    LPALCLOOPBACKOPENDEVICESOFT       open_loopback {};
    LPALCRENDERSAMPLESSOFT            render {};
    PFNALCSETTHREADCONTEXTPROC        set_thread_context {};
    PFNALCGETTHREADCONTEXTPROC        get_thread_context {};
};

template<typename T>
bool Resolve(const rstd::dlopn::Library& library, const char* name, T& function) {
    auto symbol = library.symbol<T>(rstd::ffi::CStr::from_ptr(name));
    if (symbol.is_err()) {
        rstd::log::debug("OpenAL symbol {} unavailable: {}", name, symbol.unwrap_err().message());
        return false;
    }
    function = symbol.unwrap();
    return function != nullptr;
}

struct Runtime {
    rstd::dlopn::Library library;
    Api                  api;
};

auto Load() -> Option<Runtime> {
    auto library = rstd::dlopn::Library::open(rstd::ffi::CStr::from_ptr(kSoname));
    if (library.is_err()) {
        rstd::log::debug("OpenAL unavailable: {}", library.unwrap_err().message());
        return None();
    }
    auto                         runtime = Runtime { library.unwrap(), {} };
    auto&                        api     = runtime.api;
    decltype(&alcGetProcAddress) get_proc {};
    if (! Resolve(runtime.library, "alcGetProcAddress", get_proc) ||
        ! Resolve(runtime.library, "alcCloseDevice", api.close_device) ||
        ! Resolve(runtime.library, "alcCreateContext", api.create_context) ||
        ! Resolve(runtime.library, "alcDestroyContext", api.destroy_context) ||
        ! Resolve(runtime.library, "alcGetError", api.device_error) ||
        ! Resolve(runtime.library, "alcIsExtensionPresent", api.device_has_extension) ||
        ! Resolve(runtime.library, "alIsExtensionPresent", api.context_has_extension) ||
        ! Resolve(runtime.library, "alDistanceModel", api.distance_model) ||
        ! Resolve(runtime.library, "alGetError", api.context_error) ||
        ! Resolve(runtime.library, "alGenSources", api.gen_sources) ||
        ! Resolve(runtime.library, "alGenBuffers", api.gen_buffers) ||
        ! Resolve(runtime.library, "alGetSourcei", api.get_source_i) ||
        ! Resolve(runtime.library, "alSourcei", api.source_i) ||
        ! Resolve(runtime.library, "alSourcef", api.source_f) ||
        ! Resolve(runtime.library, "alSource3f", api.source_3f) ||
        ! Resolve(runtime.library, "alSourceQueueBuffers", api.queue_buffers) ||
        ! Resolve(runtime.library, "alSourceUnqueueBuffers", api.unqueue_buffers) ||
        ! Resolve(runtime.library, "alBufferData", api.buffer_data) ||
        ! Resolve(runtime.library, "alSourcePlay", api.play) ||
        ! Resolve(runtime.library, "alSourceStop", api.stop)) {
        return None();
    }
    api.open_loopback = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
        get_proc(nullptr, "alcLoopbackOpenDeviceSOFT"));
    api.render =
        reinterpret_cast<LPALCRENDERSAMPLESSOFT>(get_proc(nullptr, "alcRenderSamplesSOFT"));
    api.set_thread_context =
        reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(get_proc(nullptr, "alcSetThreadContext"));
    api.get_thread_context =
        reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(get_proc(nullptr, "alcGetThreadContext"));
    if (! api.open_loopback || ! api.render || ! api.set_thread_context || ! api.get_thread_context)
        return None();
    return Some(rstd::move(runtime));
}

auto GetApi() -> const Api* {
    static constinit auto runtime = rstd::sync::OnceLock<Option<Runtime>>::make();
    const auto            loaded  = runtime.get_or_init(Load);
    return loaded->is_some() ? &(**loaded).api : nullptr;
}

} // namespace owe::openal

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
        m_api = openal::GetApi();
        if (! m_api) return false;
        m_device = m_api->open_loopback(nullptr);
        if (! m_device) return false;
        m_rate                = static_cast<ALCint>(rate.to_primitive());
        ALCint attributes[11] = { ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT,
                                  ALC_FLOAT_SOFT,           ALC_FREQUENCY,   m_rate };
        int    next           = 6;
        if (m_api->device_has_extension(m_device, "ALC_SOFT_HRTF")) {
            attributes[next++] = ALC_HRTF_SOFT;
            attributes[next++] = ALC_FALSE;
        }
        if (m_api->device_has_extension(m_device, "ALC_SOFT_output_limiter")) {
            attributes[next++] = ALC_OUTPUT_LIMITER_SOFT;
            attributes[next++] = ALC_FALSE;
        }
        m_context = m_api->create_context(m_device, attributes);
        if (! m_context) return false;

        CurrentContext context(*this);
        if (! context || ! m_api->context_has_extension("AL_EXT_FLOAT32")) return false;
        m_api->distance_model(AL_INVERSE_DISTANCE_CLAMPED);
        m_api->gen_sources(1, &m_source);
        m_api->gen_buffers(2, m_buffers);
        m_api->source_i(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
        m_api->source_f(m_source, AL_REFERENCE_DISTANCE, reference_distance);
        m_api->source_f(m_source, AL_ROLLOFF_FACTOR, rolloff);
        return m_api->context_error() == AL_NO_ERROR;
    }

    bool Render(mut_ref<float[]> mono, const array<float, 3>& position, float* stereo) {
        CurrentContext context(*this);
        if (! context) return false;
        // Leave one sample queued so the source stays playing across pulls.
        // Discard that sample from the next render, not from the decoded PCM.
        const auto frames = mono.len() - usize(1);
        mono[frames]      = mono[frames - usize(1)];
        ALint processed {};
        m_api->get_source_i(m_source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0) {
            ALuint old {};
            m_api->unqueue_buffers(m_source, 1, &old);
        }
        const ALuint buffer = m_buffers[m_next_buffer];
        m_api->buffer_data(buffer,
                           AL_FORMAT_MONO_FLOAT32,
                           mono.as_raw_ptr(),
                           static_cast<ALsizei>(mono.len().to_primitive() * sizeof(float)),
                           m_rate);
        m_api->queue_buffers(m_source, 1, &buffer);
        m_api->source_3f(
            m_source, AL_POSITION, position[usize()], position[usize(1)], position[usize(2)]);
        if (! m_started) m_api->play(m_source);
        if (m_api->context_error() != AL_NO_ERROR) return false;
        if (m_started) {
            const auto count = mono.len() * usize(2);
            if (m_stereo.len() < count) m_stereo.resize(count, 0.0f);
            m_api->render(
                m_device, m_stereo.data(), static_cast<ALCsizei>(mono.len().to_primitive()));
            rstd::mem::memcpy(stereo, m_stereo.data() + 2, frames * usize(2 * sizeof(float)));
        } else {
            m_api->render(m_device, stereo, static_cast<ALCsizei>(frames.to_primitive()));
        }
        m_started = true;
        m_next_buffer ^= 1;
        return m_api->device_error(m_device) == ALC_NO_ERROR;
    }

    void Stop() {
        if (! m_started) return;
        CurrentContext context(*this);
        if (context) {
            m_api->stop(m_source);
            m_api->source_i(m_source, AL_BUFFER, 0);
            // Consume the stopped voice's de-click tail before starting another clip.
            float discarded[2];
            m_api->render(m_device, discarded, 1);
        }
        m_started     = false;
        m_next_buffer = 0;
    }

    void Reset() {
        if (m_context) m_api->destroy_context(m_context);
        if (m_device) m_api->close_device(m_device);
        m_context     = nullptr;
        m_device      = nullptr;
        m_started     = false;
        m_next_buffer = 0;
    }

private:
    class CurrentContext {
    public:
        explicit CurrentContext(const SpatialSoundMixer& owner): m_api(owner.m_api) {
            if (! owner.m_context) return;
            m_previous = m_api->get_thread_context();
            m_active   = m_api->set_thread_context(owner.m_context) != ALC_FALSE;
        }
        ~CurrentContext() {
            if (m_active) m_api->set_thread_context(m_previous);
        }
        explicit operator bool() const { return m_active; }

    private:
        const openal::Api* m_api;
        ALCcontext*        m_previous {};
        bool               m_active {};
    };

    ALCdevice*         m_device {};
    ALCcontext*        m_context {};
    const openal::Api* m_api {};
    ALuint             m_source {};
    ALuint             m_buffers[2] {};
    ALsizei            m_rate {};
    Vec<float>         m_stereo;
    bool               m_started {};
    int                m_next_buffer {};
};

} // namespace owe
