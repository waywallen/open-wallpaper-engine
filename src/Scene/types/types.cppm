module;

export module wescene.types;
import wescene.core;
import rstd;
export import vrento.texture_types;
export import vrento.vertex_types;
export import vrento.shader_types;
export import vrento.graphics_types;
export import vrento.video_playback;
export import vrento.image;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

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
ref<str> ToString(const ImageType&);

using vrento::TextureFormat;
ref<str> ToString(const TextureFormat&);

using vrento::BlendMode;

using vrento::CullMode;

using vrento::ShaderCode;
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

    void Play() { m_playing.store(true, Ordering::Release); }
    void Pause() { m_playing.store(false, Ordering::Release); }
    void Stop() {
        Pause();
        Seek(f64());
    }
    void Seek(f64 seconds) {
        if (! seconds.is_finite() || seconds < f64()) seconds = f64();
        m_current_time.store(seconds, Ordering::Release);
        m_seek_seconds.store(seconds, Ordering::Release);
        m_seek_sequence.fetch_add(u64(1), Ordering::AcqRel);
    }
    void SetRate(f64 rate) {
        if (! rate.is_finite() || rate <= f64()) return;
        m_rate.store(rate, Ordering::Release);
    }

    auto Snapshot() const -> VideoPlaybackSnapshot {
        return VideoPlaybackSnapshot {
            .playing       = m_playing.load(Ordering::Acquire),
            .rate          = m_rate.load(Ordering::Acquire),
            .seek_sequence = m_seek_sequence.load(Ordering::Acquire),
            .seek_seconds  = m_seek_seconds.load(Ordering::Acquire),
        };
    }

    void PublishTime(f64 current, Option<f64> duration) {
        m_current_time.store(current, Ordering::Release);
        m_duration.store(duration.unwrap_or(f64(-1.0)), Ordering::Release);
    }
    auto CurrentTime() const -> f64 { return m_current_time.load(Ordering::Acquire); }
    auto Duration() const -> Option<f64> {
        auto value = m_duration.load(Ordering::Acquire);
        return value >= f64() ? Some(value) : None<f64>();
    }

private:
    Atomic<bool> m_playing { true };
    Atomic<f64>  m_rate { f64(1.0) };
    Atomic<u64>  m_seek_sequence {};
    Atomic<f64>  m_seek_seconds {};
    Atomic<f64>  m_current_time {};
    Atomic<f64>  m_duration { f64(-1.0) };
};

struct SharedVideoPlayback {
    Arc<VideoPlaybackState> state;
};

using vrento::VertexType;

// ---------- BitFlags<EnumT> (was in Utils.cppm) ---------------------------

template<typename EnumT>
class BitFlags {
    static_assert(rstd::mtp::is_enum<EnumT>, "BitFlags requires an enum");
    static constexpr rstd::size_t bit_count = sizeof(rstd::mtp::underlying<EnumT>) * 8;
    static_assert(bit_count <= 64, "BitFlags supports enums up to 64 bits");
    static constexpr u64 mask = u64::MAX >> u64(64 - bit_count);

public:
    constexpr BitFlags() noexcept = default;
    constexpr BitFlags(rstd::uint64_t value) noexcept: bits_(u64(value) & mask) {}

    BitFlags& set(EnumT value, bool enabled = true) noexcept {
        auto bit = bit_mask(static_cast<rstd::uint64_t>(value));
        bits_    = enabled ? bits_ | bit : bits_ & ~bit;
        return *this;
    }
    BitFlags& reset(EnumT value) noexcept { return set(value, false); }
    BitFlags& reset() noexcept {
        bits_ = u64();
        return *this;
    }
    bool                   all() const noexcept { return bits_ == mask; }
    bool                   any() const noexcept { return bits_ != u64(); }
    bool                   none() const noexcept { return bits_ == u64(); }
    constexpr rstd::size_t size() const noexcept { return bit_count; }
    rstd::size_t           count() const noexcept { return bits_.count_ones().to_primitive(); }
    constexpr bool         operator[](EnumT value) const {
        return (*this)[static_cast<rstd::uint64_t>(value)];
    }
    constexpr bool operator[](rstd::uint64_t index) const {
        return (bits_ & bit_mask(index)) != u64();
    }
    auto to_string() const -> String {
        String text;
        text.reserve(usize(bit_count));
        for (rstd::size_t index = bit_count; index > 0; --index)
            text.push_str((*this)[index - 1] ? "1"_str : "0"_str);
        return text;
    }

private:
    static constexpr u64 bit_mask(rstd::uint64_t index) {
        if (index >= bit_count) rstd::panic { "BitFlags index out of range" };
        return u64(1) << u64(index);
    }
    u64 bits_ {};
};

// ---------- SpriteAnimation (was SpriteAnimation.hpp) ---------------------

struct SpriteFrame {
    rstd::int32_t imageId { 0 };
    float         frametime { 0 };
    float         x { 0 };
    float         y { 0 };
    float         width { 1 };
    float         height { 1 };
    float         rate { 1 }; // real h / w

    array<float, 2> xAxis { 1.0f, 0.0f };
    array<float, 2> yAxis { 0.0f, 1.0f };
};

class SpriteAnimation {
public:
    SpriteAnimation() = default;
    SpriteAnimation(const SpriteAnimation& other)
        : m_curFrame(other.m_curFrame),
          m_remainTime(other.m_remainTime),
          m_frames(other.m_frames.clone()) {}
    SpriteAnimation(SpriteAnimation&&) noexcept            = default;
    SpriteAnimation& operator=(SpriteAnimation&&) noexcept = default;
    SpriteAnimation& operator=(const SpriteAnimation& other) {
        if (this != &other) *this = other.clone();
        return *this;
    }
    auto        clone() const -> SpriteAnimation { return SpriteAnimation(*this); }
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
    void        AppendFrame(const SpriteFrame& frame) { m_frames.emplace_back(frame); }
    // Read a specific frame without advancing the internal cursor. Used by
    // the script-driven setFrame() override path.
    const SpriteFrame& GetFrame(usize i) const { return m_frames.at(i); }

    usize numFrames() const { return m_frames.len(); }
    usize CurrentFrameIndex() const { return m_curFrame; }

private:
    void SwitchToNext() {
        if (m_curFrame + usize(1) >= m_frames.len())
            m_curFrame = usize();
        else
            m_curFrame++;
    }
    usize  m_curFrame;
    double m_remainTime { 0 };

    Vec<SpriteFrame> m_frames;
};

// ---------- Image (was Image.hpp) -----------------------------------------

union ImageExtra {
    rstd::int32_t val { 0 };
    char          str[125];
};

} // namespace owe

export namespace rstd
{
template<>
struct Impl<Copy, owe::ImageExtra> {};
} // namespace rstd

export namespace owe
{
using vrento::ImageData;
using vrento::ImageDataPtr;

struct ImageHeader {
    rstd::int32_t width { 0 };
    rstd::int32_t height { 0 };
    rstd::int32_t mapWidth { 0 };
    rstd::int32_t mapHeight { 0 };

    bool mipmap_larger { false };
    bool mipmap_pow2 { false };

    ImageType     type { ImageType::UNKNOWN };
    TextureFormat format { TextureFormat::RGBA8 };
    rstd::int32_t count { 0 };

    bool          isSprite { false };
    TextureSample sample;

    SpriteAnimation             spriteAnim;
    HashMap<String, ImageExtra> extraHeader;

    auto clone() const -> ImageHeader {
        return { .width         = width,
                 .height        = height,
                 .mapWidth      = mapWidth,
                 .mapHeight     = mapHeight,
                 .mipmap_larger = mipmap_larger,
                 .mipmap_pow2   = mipmap_pow2,
                 .type          = type,
                 .format        = format,
                 .count         = count,
                 .isSprite      = isSprite,
                 .sample        = sample,
                 .spriteAnim    = spriteAnim.clone(),
                 .extraHeader   = extraHeader.clone() };
    }
};

struct Image : NoCopy, NoMove {
    ImageHeader        header;
    Arc<vrento::Image> content { Arc<vrento::Image>::make() };

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

// Shared dlopen/dlsym wrapper.
export namespace utils
{

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
