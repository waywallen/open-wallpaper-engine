module;

export module wescene.types;
import wescene.core;
import rstd;
import rstd.cppstd;
export import vrento.texture_types;
export import vrento.vertex_types;
export import vrento.shader_types;
export import vrento.graphics_types;
export import vrento.video_playback;
export import vrento.image;

export namespace owe
{

// ---------- enums + TextureSample (was Type.hpp) ---------------------------

enum class ImageType
{
    UNKNOWN = -1,
    BMP     = 0,
    ICO     = 1,
    JPEG    = 2,
    JNG     = 3,
    KOALA   = 4,
    LBM     = 5,
    MNG     = 6,
    PBM     = 7,
    PBMRAW  = 8,
    PCD     = 9,
    PCX     = 10,
    PGM     = 11,
    PGMRAW  = 12,
    PNG     = 13,
    PPM     = 14,
    PPMRAW  = 15,
    RAS     = 16,
    TARGA   = 17,
    TIFF    = 18,
    WBMP    = 19,
    PSD     = 20,
    CUT     = 21,
    XBM     = 22,
    XPM     = 23,
    DDS     = 24,
    GIF     = 25,
    HDR     = 26,
    FAXG3   = 27,
    SGI     = 28,
    EXR     = 29,
    J2K     = 30,
    JP2     = 31,
    PFM     = 32,
    PICT    = 33,
    RAW     = 34,
    // Wallpaper Engine "scene format" wallpapers may inline an MP4/WebM
    // container as a .tex body. We extend ImageType past FreeImage's
    // range (which stops at RAW=34) so the value can flow through the
    // existing ImageHeader::type slot without colliding.
    VIDEO = 100,
};
std::string ToString(const ImageType&);

using vrento::TextureFormat;
std::string ToString(const TextureFormat&);

using vrento::BlendMode;

using vrento::CullMode;

using vrento::ShaderType;

using vrento::ShaderScalarKind;

using vrento::ShaderMatrixMajor;

using vrento::ShaderMatrixConvention;

using vrento::ShaderMatrixAbi;

enum class TextureType
{
    IMG_2D,
};

using vrento::MeshPrimitive;

using vrento::FillMode;

using vrento::TextureWrap;

using vrento::TextureFilter;

using vrento::CompareOp;

using vrento::TextureBorderColor;

using vrento::TextureSample;

using vrento::VideoPlaybackSnapshot;

class VideoPlaybackState {
public:
    VideoPlaybackState()                                             = default;
    VideoPlaybackState(const VideoPlaybackState&)                    = delete;
    VideoPlaybackState(VideoPlaybackState&&)                         = delete;
    auto operator=(const VideoPlaybackState&) -> VideoPlaybackState& = delete;
    auto operator=(VideoPlaybackState&&) -> VideoPlaybackState&      = delete;

    void Play() { m_playing.store(true, rstd::sync::atomic::Ordering::Release); }
    void Pause() { m_playing.store(false, rstd::sync::atomic::Ordering::Release); }
    void Stop() {
        Pause();
        Seek(rstd::f64());
    }
    void Seek(rstd::f64 seconds) {
        if (! seconds.is_finite() || seconds < rstd::f64()) seconds = rstd::f64();
        m_current_time.store(seconds, rstd::sync::atomic::Ordering::Release);
        m_seek_seconds.store(seconds, rstd::sync::atomic::Ordering::Release);
        m_seek_sequence.fetch_add(rstd::u64(1), rstd::sync::atomic::Ordering::AcqRel);
    }
    void SetRate(rstd::f64 rate) {
        if (! rate.is_finite() || rate <= rstd::f64()) return;
        m_rate.store(rate, rstd::sync::atomic::Ordering::Release);
    }

    auto Snapshot() const -> VideoPlaybackSnapshot {
        return VideoPlaybackSnapshot {
            .playing       = m_playing.load(rstd::sync::atomic::Ordering::Acquire),
            .rate          = m_rate.load(rstd::sync::atomic::Ordering::Acquire),
            .seek_sequence = m_seek_sequence.load(rstd::sync::atomic::Ordering::Acquire),
            .seek_seconds  = m_seek_seconds.load(rstd::sync::atomic::Ordering::Acquire),
        };
    }

    void PublishTime(rstd::f64 current, rstd::Option<rstd::f64> duration) {
        m_current_time.store(current, rstd::sync::atomic::Ordering::Release);
        m_duration.store(duration.unwrap_or(rstd::f64(-1.0)),
                         rstd::sync::atomic::Ordering::Release);
    }
    auto CurrentTime() const -> rstd::f64 {
        return m_current_time.load(rstd::sync::atomic::Ordering::Acquire);
    }
    auto Duration() const -> rstd::Option<rstd::f64> {
        auto value = m_duration.load(rstd::sync::atomic::Ordering::Acquire);
        return value >= rstd::f64() ? rstd::Some(value) : rstd::None<rstd::f64>();
    }

private:
    rstd::sync::atomic::Atomic<bool>      m_playing { true };
    rstd::sync::atomic::Atomic<rstd::f64> m_rate { rstd::f64(1.0) };
    rstd::sync::atomic::Atomic<rstd::u64> m_seek_sequence {};
    rstd::sync::atomic::Atomic<rstd::f64> m_seek_seconds {};
    rstd::sync::atomic::Atomic<rstd::f64> m_current_time {};
    rstd::sync::atomic::Atomic<rstd::f64> m_duration { rstd::f64(-1.0) };
};

struct SharedVideoPlayback {
    rstd::sync::Arc<VideoPlaybackState> state;
};

using vrento::VertexType;

// ---------- BitFlags<EnumT> (was in Utils.cppm) ---------------------------

template<typename EnumT>
class BitFlags {
    static_assert(std::is_enum_v<EnumT>, "Flags can only be specialized for enum types");

    using UnderlyingT = typename std::make_unsigned_t<typename std::underlying_type_t<EnumT>>;

public:
    constexpr BitFlags() noexcept: bits_(0u) {}
    constexpr BitFlags(UnderlyingT val) noexcept: bits_(val) {}

    BitFlags& set(EnumT e, bool value = true) noexcept {
        bits_.set(underlying(e), value);
        return *this;
    }
    BitFlags& reset(EnumT e) noexcept {
        set(e, false);
        return *this;
    }
    BitFlags& reset() noexcept {
        bits_.reset();
        return *this;
    }
    [[nodiscard]] bool                  all() const noexcept { return bits_.all(); }
    [[nodiscard]] bool                  any() const noexcept { return bits_.any(); }
    [[nodiscard]] bool                  none() const noexcept { return bits_.none(); }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return bits_.size(); }
    [[nodiscard]] std::size_t           count() const noexcept { return bits_.count(); }
    constexpr bool                      operator[](EnumT e) const { return bits_[underlying(e)]; }
    constexpr bool                      operator[](UnderlyingT t) const { return bits_[t]; }
    auto                                to_string() const { return bits_.to_string(); }

private:
    static constexpr UnderlyingT         underlying(EnumT e) { return static_cast<UnderlyingT>(e); }
    std::bitset<sizeof(UnderlyingT) * 8> bits_;
};

// ---------- SpriteAnimation (was SpriteAnimation.hpp) ---------------------

struct SpriteFrame {
    std::int32_t imageId { 0 };
    float        frametime { 0 };
    float        x { 0 };
    float        y { 0 };
    float        width { 1 };
    float        height { 1 };
    float        rate { 1 }; // real h / w

    rstd::array<float, 2> xAxis { 1.0f, 0.0f };
    rstd::array<float, 2> yAxis { 0.0f, 1.0f };
};

class SpriteAnimation {
public:
    const auto& GetAnimateFrame(double newtime) {
        if ((m_remainTime -= newtime) < 0.0f) {
            SwitchToNext();
            const auto& frame = m_frames.at(m_curFrame);
            m_remainTime      = frame.frametime;
        }
        const auto& frame = m_frames.at(m_curFrame);
        return frame;
    }
    const auto& GetCurFrame() const { return m_frames.at(m_curFrame); }
    void        AppendFrame(const SpriteFrame& frame) { m_frames.push_back(frame); }
    // Read a specific frame without advancing the internal cursor. Used by
    // the script-driven setFrame() override path.
    const SpriteFrame& GetFrame(usize i) const { return m_frames.at(i.to_primitive()); }

    usize numFrames() const { return usize(m_frames.size()); }
    usize CurrentFrameIndex() const { return usize(m_curFrame); }

private:
    void SwitchToNext() {
        if (m_curFrame + 1 >= m_frames.size())
            m_curFrame = 0;
        else
            m_curFrame++;
    }
    std::size_t m_curFrame { 0 };
    double      m_remainTime { 0 };

    std::vector<SpriteFrame> m_frames;
};

// ---------- Image (was Image.hpp) -----------------------------------------

union ImageExtra {
    int32_t val { 0 };
    char    str[125];
};

using vrento::ImageData;
using vrento::ImageDataPtr;

struct ImageHeader {
    std::int32_t width { 0 };
    std::int32_t height { 0 };
    std::int32_t mapWidth { 0 };
    std::int32_t mapHeight { 0 };

    bool mipmap_larger { false };
    bool mipmap_pow2 { false };

    ImageType     type { ImageType::UNKNOWN };
    TextureFormat format { TextureFormat::RGBA8 };
    std::int32_t  count { 0 };

    bool          isSprite { false };
    TextureSample sample;

    SpriteAnimation                             spriteAnim;
    std::unordered_map<std::string, ImageExtra> extraHeader;
};

struct Image : NoCopy, NoMove {
    ImageHeader                    header;
    rstd::sync::Arc<vrento::Image> content { rstd::sync::Arc<vrento::Image>::make() };

    void FinalizeContent() {
        content->header = vrento::ImageHeader {
            .kind   = header.type == ImageType::VIDEO ? vrento::ImageKind::Video
                                                      : vrento::ImageKind::Pixels,
            .format = header.format,
            .sample = header.sample,
        };
    }
};

} // namespace owe

export namespace rstd
{
template<>
struct Impl<vrento::VideoPlayback, owe::SharedVideoPlayback> : ImplBase<owe::SharedVideoPlayback> {
    auto Snapshot() const -> vrento::VideoPlaybackSnapshot {
        return this->self().state->Snapshot();
    }
    void PublishTime(f64 current, Option<f64> duration) const {
        this->self().state->PublishTime(current, duration);
    }
};
} // namespace rstd

// Small OS utility — dlopen/dlsym wrapper. Lives here so wescene-vulkan-runtime
// can reach it without dragging wescene-base in. hash_combine is co-located
// for the same reason (TextureCache key hashing).
export namespace utils
{

template<typename T>
inline void hash_combine(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
template<typename T>
inline void hash_combine_fast(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>()(val) << 1u;
}

class DynamicLibrary : NoCopy {
public:
    DynamicLibrary();
    ~DynamicLibrary();

    DynamicLibrary(const char* filename);

    DynamicLibrary(DynamicLibrary&& o) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& o) noexcept;

    bool IsOpen() const;
    bool Open(const char* filename);
    void Close();

    void* GetSymbolAddr(const char* name) const;

    template<typename T>
    bool GetSymbol(const char* name, T& pfunc) const {
        pfunc = reinterpret_cast<T>(GetSymbolAddr(name));
        return pfunc != nullptr;
    }

private:
    void* handle { nullptr };
};

} // namespace utils
