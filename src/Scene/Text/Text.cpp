module;

#include <rstd/macro.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>
module wescene.text;
import eigen;
import wescene.pkg.spec_names;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.scene;
import wescene.shader_compile;

using rstd::mem::memcpy;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::ffi::CString;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::sync::Arc;

namespace owe::text
{

namespace
{

constexpr rstd::uint32_t kMinAtlasDim { 1024 };

// 4×4 white cell at (0,0) so a single-channel atlas can also serve solid-fill
// quads (e.g. opaquebackground rectangle) and as a tofu fallback when the
// atlas overflows.
constexpr rstd::uint32_t kWhiteCellSize { 4 };

rstd::uint32_t AtlasDimForPixelSize(rstd::uint32_t pixel_size) {
    if (pixel_size > 512) return 4096;
    if (pixel_size > 256) return 2048;
    return kMinAtlasDim;
}

class FtLibrary {
public:
    static FtLibrary& Get() {
        static FtLibrary inst;
        return inst;
    }
    FT_Library handle() const noexcept { return m_lib; }

private:
    FtLibrary() {
        if (FT_Init_FreeType(&m_lib) != 0) {
            rstd_error("FT_Init_FreeType failed");
            m_lib = nullptr;
        }
    }
    ~FtLibrary() {
        if (m_lib != nullptr) FT_Done_FreeType(m_lib);
    }
    FtLibrary(const FtLibrary&)            = delete;
    FtLibrary& operator=(const FtLibrary&) = delete;

    FT_Library m_lib { nullptr };
};

bool IsFontExt(ref<Path> path) {
    auto ext = path.extension();
    if (ext.is_none()) return false;
    auto text = ext->to_str();
    if (text.is_none()) return false;
    String lower = into(*text);
    lower.as_mut_str().make_ascii_lowercase();
    return lower == "ttf"_str || lower == "otf"_str || lower == "ttc"_str;
}

Option<Arc<Vec<u8>>> ReadAll(ref<Path> path) {
    auto bytes = rstd::fs::read(path);
    if (bytes.is_err() || bytes->is_empty()) return None();
    return Some(Arc<Vec<u8>>::make(rstd::move(bytes).unwrap_unchecked()));
}

PathBuf ResolveFontconfigCodepoint(rstd::uint32_t codepoint) {
    if (! FcInit()) return {};

    FcPattern* pat = FcPatternCreate();
    if (pat == nullptr) return {};
    FcCharSet* charset = FcCharSetCreate();
    if (charset == nullptr) {
        FcPatternDestroy(pat);
        return {};
    }
    FcCharSetAddChar(charset, static_cast<FcChar32>(codepoint));
    FcPatternAddCharSet(pat, FC_CHARSET, charset);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   res   = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    FcCharSetDestroy(charset);
    FcPatternDestroy(pat);

    if (match == nullptr || res != FcResultMatch) {
        if (match != nullptr) FcPatternDestroy(match);
        return {};
    }

    FcChar8* file = nullptr;
    PathBuf  out;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
        out = PathBuf::from(as_str(reinterpret_cast<const char*>(file)).unwrap());
    }
    FcPatternDestroy(match);
    return out;
}

} // namespace

// -- FontFace -------------------------------------------------------------

struct FontFace::Impl {
    struct FallbackFace {
        Option<Arc<Vec<u8>>> blob;
        FT_Face              face { nullptr };

        ~FallbackFace() {
            if (face != nullptr) FT_Done_Face(face);
        }
    };

    Option<Arc<Vec<u8>>> blob;
    FT_Face              face { nullptr };
    rstd::uint32_t       pixel_size { 0 };

    rstd::uint32_t          atlas_w { kMinAtlasDim };
    rstd::uint32_t          atlas_h { kMinAtlasDim };
    Arc<Vec<rstd::uint8_t>> atlas { Arc<Vec<rstd::uint8_t>>::make() };

    // Shelf packer state: pen advances along the current shelf, falls to a
    // new shelf when the next glyph won't fit horizontally.
    rstd::uint32_t pen_x { 0 };
    rstd::uint32_t pen_y { 0 };
    rstd::uint32_t shelf_h { 0 };

    HashMap<u32, Box<GlyphInfo>>       glyphs;
    HashMap<String, Box<FallbackFace>> fallback_faces;
    HashMap<u32, FT_Face>              fallback_by_codepoint;
    HashSet<u32>                       fallback_misses;

    // Pixel-coord rects pushed by Populate() — drained once per frame by
    // the renderer to vkCmdCopyBufferToImage just the changed regions.
    Vec<AtlasDirtyRect> dirty_rects;

    // Set by FontCache::GetFace; consumed by the renderer's per-frame
    // atlas-commit hook to look the face's VkImage up by URL.
    String atlas_url;

    ~Impl() {
        if (face != nullptr) FT_Done_Face(face);
    }

    Impl() {
        atlas->resize(usize(atlas_w) * usize(atlas_h), rstd::uint8_t(0));
        SeedWhiteCell();
    }

    void ResetAtlas(rstd::uint32_t dim) {
        atlas_w = dim;
        atlas_h = dim;
        atlas   = Arc<Vec<rstd::uint8_t>>::make();
        atlas->resize(usize(atlas_w) * usize(atlas_h), rstd::uint8_t(0));
        dirty_rects.clear();
        glyphs.clear();
        pen_x   = 0;
        pen_y   = 0;
        shelf_h = 0;
        SeedWhiteCell();
    }

    void SeedWhiteCell() {
        for (rstd::uint32_t y = 0; y < kWhiteCellSize; ++y) {
            for (rstd::uint32_t x = 0; x < kWhiteCellSize; ++x) {
                (*atlas)[usize(y * atlas_w + x)] = 0xFF;
            }
        }
        pen_x   = kWhiteCellSize + 1;
        pen_y   = 0;
        shelf_h = kWhiteCellSize;
        dirty_rects.push({ 0, 0, kWhiteCellSize, kWhiteCellSize });
    }

    bool ReserveSlot(rstd::uint32_t w, rstd::uint32_t h, rstd::uint32_t& out_x,
                     rstd::uint32_t& out_y) {
        if (pen_x + w > atlas_w) {
            pen_y += shelf_h + 1;
            pen_x   = 0;
            shelf_h = 0;
        }
        if (pen_y + h > atlas_h) return false;
        out_x = pen_x;
        out_y = pen_y;
        pen_x += w + 1;
        if (h > shelf_h) shelf_h = h;
        return true;
    }

    FT_Face ResolveFallbackFace(rstd::uint32_t codepoint) {
        if (auto it = fallback_by_codepoint.get(u32(codepoint)); it.is_some()) {
            return **it;
        }
        if (fallback_misses.contains(u32(codepoint))) return nullptr;

        auto path = ResolveFontconfigCodepoint(codepoint);
        if (path.is_empty()) {
            (void)fallback_misses.insert(u32(codepoint));
            return nullptr;
        }

        String key = path.as_path().to_string_lossy();
        auto   it  = fallback_faces.get(key.as_str());
        if (it.is_none()) {
            auto bytes = ReadAll(path.as_path());
            if (! bytes) {
                (void)fallback_misses.insert(u32(codepoint));
                return nullptr;
            }

            auto       fallback = Box<FallbackFace>::make();
            FT_Library lib      = FtLibrary::Get().handle();
            if (lib == nullptr ||
                FT_New_Memory_Face(lib,
                                   reinterpret_cast<const FT_Byte*>(
                                       rstd::as_bytes((*bytes)->as_slice()).as_raw_ptr()),
                                   static_cast<FT_Long>((*bytes)->len().to_primitive()),
                                   0,
                                   &fallback->face) != 0 ||
                FT_Set_Pixel_Sizes(fallback->face, 0, pixel_size) != 0) {
                (void)fallback_misses.insert(u32(codepoint));
                return nullptr;
            }
            fallback->blob = rstd::move(bytes);
            (void)fallback_faces.insert(key.clone(), rstd::move(fallback));
            it = fallback_faces.get(key.as_str());
        }

        FT_Face fallback_face = (**it)->face;
        if (fallback_face == nullptr || FT_Get_Char_Index(fallback_face, codepoint) == 0) {
            (void)fallback_misses.insert(u32(codepoint));
            return nullptr;
        }
        (void)fallback_by_codepoint.insert(u32(codepoint), fallback_face);
        return fallback_face;
    }

    void Blit(rstd::uint32_t x, rstd::uint32_t y, rstd::uint32_t w, rstd::uint32_t h,
              const rstd::uint8_t* src, rstd::uint32_t pitch) {
        for (rstd::uint32_t row = 0; row < h; ++row) {
            memcpy(&(*atlas)[usize((y + row) * atlas_w + x)], src + row * pitch, rstd::usize(w));
        }
    }
};

FontFace::FontFace(): m_impl(Box<Impl>::make()) {}
FontFace::~FontFace()                              = default;
FontFace::FontFace(FontFace&&) noexcept            = default;
FontFace& FontFace::operator=(FontFace&&) noexcept = default;

FontMetrics FontFace::Metrics() const {
    FontMetrics m {};
    if (m_impl->face != nullptr && m_impl->face->size != nullptr) {
        const auto& sm = m_impl->face->size->metrics;
        m.ascender     = static_cast<float>(sm.ascender) / 64.0f;
        m.descender    = static_cast<float>(sm.descender) / 64.0f;
        m.line_height  = static_cast<float>(sm.height) / 64.0f;
        m.pixel_size   = m_impl->pixel_size;
    }
    m.atlas_w = m_impl->atlas_w;
    m.atlas_h = m_impl->atlas_h;
    return m;
}

slice<rstd::uint8_t> FontFace::AtlasPixels() const { return m_impl->atlas->as_slice(); }

auto FontFace::RetainAtlasPixels() const -> owe::ImageDataPtr {
    struct Owner {
        Arc<Vec<rstd::uint8_t>> pixels;
        explicit Owner(Arc<Vec<rstd::uint8_t>> value): pixels(rstd::move(value)) {}
        void operator()(rstd::uint8_t*) const noexcept {}
    };
    return owe::ImageDataPtr(m_impl->atlas->data(), Owner(m_impl->atlas.clone()));
}

slice<AtlasDirtyRect> FontFace::DirtyRects() const noexcept {
    return m_impl->dirty_rects.as_slice();
}
void     FontFace::ClearDirtyRects() noexcept { m_impl->dirty_rects.clear(); }
ref<str> FontFace::AtlasUrl() const noexcept { return m_impl->atlas_url.as_str(); }

const GlyphInfo* FontFace::Lookup(rstd::uint32_t codepoint) const noexcept {
    auto& impl = *m_impl;
    if (auto it = impl.glyphs.get(u32(codepoint)); it.is_some()) {
        return (**it).as_ptr().as_raw_ptr();
    }
    return nullptr;
}

void FontFace::Populate(slice<rstd::uint32_t> codepoints) {
    auto& impl = *m_impl;
    if (impl.face == nullptr) return;
    for (rstd::uint32_t codepoint : codepoints) {
        if (impl.glyphs.contains_key(u32(codepoint))) continue;

        if (codepoint == '\t') {
            (void)impl.glyphs.insert(u32(codepoint), Box<GlyphInfo>::make());
            continue;
        }

        FT_Face render_face = impl.face;
        FT_UInt glyph_index = FT_Get_Char_Index(render_face, codepoint);
        if (glyph_index == 0) {
            render_face = impl.ResolveFallbackFace(codepoint);
            glyph_index = render_face != nullptr ? FT_Get_Char_Index(render_face, codepoint) : 0;
        }
        if (glyph_index == 0) {
            (void)impl.glyphs.insert(u32(codepoint), Box<GlyphInfo>::make());
            continue;
        }

        if (FT_Load_Glyph(render_face, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            continue;
        }
        FT_GlyphSlot g = render_face->glyph;
        GlyphInfo    gi {};
        gi.pixel_w   = g->bitmap.width;
        gi.pixel_h   = g->bitmap.rows;
        gi.bearing_x = static_cast<float>(g->bitmap_left);
        gi.bearing_y = static_cast<float>(g->bitmap_top);
        gi.advance_x = static_cast<float>(g->advance.x) / 64.0f;

        if (gi.pixel_w == 0 || gi.pixel_h == 0) {
            gi.atlas_x = 0;
            gi.atlas_y = 0;
            (void)impl.glyphs.insert(u32(codepoint), Box<GlyphInfo>::make(gi));
            continue;
        }

        if (! impl.ReserveSlot(gi.pixel_w, gi.pixel_h, gi.atlas_x, gi.atlas_y)) {
            // Atlas full → tofu mapped to the white cell. Cache the fallback so
            // we don't FT_Load the same codepoint every frame.
            gi.atlas_x = 0;
            gi.atlas_y = 0;
            gi.pixel_w = kWhiteCellSize;
            gi.pixel_h = kWhiteCellSize;
            (void)impl.glyphs.insert(u32(codepoint), Box<GlyphInfo>::make(gi));
            continue;
        }

        impl.Blit(gi.atlas_x,
                  gi.atlas_y,
                  gi.pixel_w,
                  gi.pixel_h,
                  g->bitmap.buffer,
                  static_cast<rstd::uint32_t>(g->bitmap.pitch));
        impl.dirty_rects.push({ gi.atlas_x, gi.atlas_y, gi.pixel_w, gi.pixel_h });
        (void)impl.glyphs.insert(u32(codepoint), Box<GlyphInfo>::make(gi));
    }
}

// -- FontCache ------------------------------------------------------------

struct FontCache::Impl {
    HashMap<String, HashMap<u32, Box<FontFace>>> faces;
    u64                                          next_atlas_id { 0 };
};

FontCache::FontCache(): m_impl(Box<Impl>::make()) {}
FontCache::~FontCache() = default;

FontFace* FontCache::GetFace(const ResolvedBlob& font, rstd::uint32_t pixel_size) {
    if (! font.bytes || (*font.bytes)->is_empty() || font.source.is_empty() || pixel_size == 0)
        return nullptr;

    auto source = font.source.as_str();
    if (auto source_faces = m_impl->faces.get_mut(source); source_faces.is_some()) {
        if (auto face = (*source_faces)->get_mut(u32(pixel_size)); face.is_some()) {
            return (*face)->as_mut_ptr().as_raw_ptr();
        }
    }

    auto blob_span = rstd::as_bytes((*font.bytes)->as_slice());

    FT_Library lib = FtLibrary::Get().handle();
    if (lib == nullptr) return nullptr;

    auto face = Box<FontFace>::make();
    if (FT_New_Memory_Face(lib,
                           reinterpret_cast<const FT_Byte*>(blob_span.as_raw_ptr()),
                           static_cast<FT_Long>(blob_span.len().to_primitive()),
                           0,
                           &face->m_impl->face) != 0) {
        rstd_error("FT_New_Memory_Face failed");
        return nullptr;
    }
    if (FT_Set_Pixel_Sizes(face->m_impl->face, 0, pixel_size) != 0) {
        rstd_error("FT_Set_Pixel_Sizes failed (px={})", pixel_size);
        return nullptr;
    }
    // Keep the bytes alive for the face's lifetime: FT_Face holds raw
    // pointers into this buffer and dereferences them on every glyph load.
    face->m_impl->blob       = font.bytes.clone();
    face->m_impl->pixel_size = pixel_size;
    face->m_impl->ResetAtlas(AtlasDimForPixelSize(pixel_size));
    face->m_impl->atlas_url = rstd::format("_text_atlas_{}", m_impl->next_atlas_id++);

    FontFace* raw          = face.as_mut_ptr().as_raw_ptr();
    auto      source_faces = m_impl->faces.get_mut(source);
    if (source_faces.is_none()) {
        (void)m_impl->faces.insert(String::make(source), HashMap<u32, Box<FontFace>>::make());
        source_faces = m_impl->faces.get_mut(source);
    }
    (void)(*source_faces)->insert(u32(pixel_size), rstd::move(face));
    return raw;
}

Vec<FontFace*> FontCache::Faces() const {
    Vec<FontFace*> out;
    for (auto source_faces : m_impl->faces.values()) {
        for (auto face : source_faces->values()) out.push(face->as_mut_ptr().as_raw_ptr());
    }
    return out;
}

FontCache& EnsureSceneFontCache(owe::Scene& scene) {
    auto cache = scene.ExtensionMut<FontCache>();
    if (cache.is_some()) return **cache;
    scene.InstallExtension(Box<FontCache>::make());
    return **scene.ExtensionMut<FontCache>();
}
FontCache* SceneFontCache(owe::Scene& scene) noexcept {
    auto cache = scene.ExtensionMut<FontCache>();
    return cache.is_some() ? (*cache).as_raw_ptr() : nullptr;
}

// WE references system fonts as `systemfont_<lowercased-windows-name>`,
// e.g. `systemfont_arial`. On Linux those exact files don't exist; fontconfig
// has an alias table that maps Windows family names (Arial, Courier New, …)
// to whatever the user actually has installed. Strip the prefix and ask fc.
static PathBuf ResolveViaFontconfig(ref<str> name) {
    auto path = PathBuf::from(name);
    auto base = path.as_path().file_name();
    if (base.is_none()) return {};
    auto text = base->to_str();
    if (text.is_none() || ! text->starts_with("systemfont_"_str)) return {};
    auto family_bytes = Vec<u8>::from(text->get(usize(11), text->len()).unwrap().as_bytes());
    if (family_bytes.is_empty()) return {};
    const u8 first = family_bytes.first().unwrap().get();
    family_bytes.first_mut().unwrap().get_mut() =
        u8(static_cast<unsigned char>(std::toupper(first.to_primitive())));
    auto family = CString::make(rstd::move(family_bytes));
    if (family.is_err() || ! FcInit()) return {};
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family->as_ptr()));
    if (pat == nullptr) return {};
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult   res   = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (match == nullptr || res != FcResultMatch) {
        if (match != nullptr) FcPatternDestroy(match);
        return {};
    }
    FcChar8* file = nullptr;
    PathBuf  out;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
        out = PathBuf::from(as_str(reinterpret_cast<const char*>(file)).unwrap());
    }
    FcPatternDestroy(match);
    return out;
}

FontCache::ResolvedBlob FontCache::ResolveSystemFont(ref<str> name, bool fallback_to_any) {
    auto try_load = [](ref<Path> path) -> ResolvedBlob {
        auto metadata = rstd::fs::metadata(path);
        if (metadata.is_err() || ! metadata->is_file()) return {};
        auto bytes = ReadAll(path);
        if (bytes.is_none()) return {};
        return { rstd::move(bytes), path.to_string_lossy() };
    };
    auto search = [&](slice<PathBuf> roots, usize cap, auto matches) -> ResolvedBlob {
        usize scanned {};
        for (const auto& root : roots) {
            auto entries = rstd::fs::read_dir(root.as_path());
            if (entries.is_err()) continue;
            Vec<rstd::fs::ReadDir> stack;
            stack.push(rstd::move(entries).unwrap_unchecked());
            while (! stack.is_empty()) {
                auto entry = stack.last_mut().unwrap()->next();
                if (entry.is_none()) {
                    (void)stack.pop();
                    continue;
                }
                if (entry->is_err()) continue;
                if (++scanned > cap) return {};
                auto path = (*entry)->path();
                auto type = (*entry)->file_type();
                if (type.is_err()) continue;
                if (type->is_dir()) {
                    auto children = rstd::fs::read_dir(path.as_path());
                    if (children.is_ok()) stack.push(rstd::move(children).unwrap_unchecked());
                    continue;
                }
                if (matches(path.as_path())) {
                    if (auto blob = try_load(path.as_path()); blob.bytes.is_some()) return blob;
                }
            }
        }
        return {};
    };
    Vec<PathBuf> roots;
    roots.push(PathBuf::from("/usr/share/fonts"_str));
    roots.push(PathBuf::from("/usr/local/share/fonts"_str));
    if (! name.is_empty()) {
        auto path = PathBuf::from(name);
        if (auto blob = try_load(path.as_path()); blob.bytes.is_some()) return blob;
        auto fc_path = ResolveViaFontconfig(name);
        if (! fc_path.is_empty()) {
            if (auto blob = try_load(fc_path.as_path()); blob.bytes.is_some()) return blob;
        }
        if (auto xdg = rstd::env::var_os("XDG_DATA_HOME"_str)) {
            roots.push(PathBuf::from(rstd::move(*xdg)).join("fonts"_str));
        }
        if (auto home = rstd::env::var_os("HOME"_str)) {
            auto home_path = PathBuf::from(rstd::move(*home));
            roots.push(home_path.join(PathBuf::from(".local/share/fonts"_str).as_path()));
            roots.push(home_path.join(PathBuf::from(".fonts"_str).as_path()));
        }
        auto base = path.as_path().file_name();
        auto blob = search(roots.as_slice(), usize(8192), [&](ref<Path> candidate) {
            auto filename = candidate.file_name();
            return base.is_some() && filename.is_some() &&
                   base->as_encoded_bytes() == filename->as_encoded_bytes();
        });
        if (blob.bytes.is_some()) return blob;
    }
    if (! fallback_to_any) return {};
    roots.truncate(usize(2));
    return search(roots.as_slice(), usize(4096), IsFontExt);
}

// -- Atlas snapshot -------------------------------------------------------

auto BuildAtlasImage(const FontFace& face, ref<str> key) -> Option<Arc<owe::Image>> {
    auto fm  = face.Metrics();
    auto pix = face.AtlasPixels();
    if (fm.atlas_w == 0 || fm.atlas_h == 0 || pix.is_empty()) return None();

    auto img          = Arc<owe::Image>::make();
    img->content->key = rstd::into(key);

    img->header.width         = static_cast<rstd::int32_t>(fm.atlas_w);
    img->header.height        = static_cast<rstd::int32_t>(fm.atlas_h);
    img->header.mapWidth      = img->header.width;
    img->header.mapHeight     = img->header.height;
    img->header.mipmap_larger = false;
    img->header.mipmap_pow2   = false;
    img->header.type          = owe::ImageType::UNKNOWN;
    img->header.format        = owe::TextureFormat::R8;
    img->header.count         = 1;
    img->header.isSprite      = false;
    img->header.sample        = { owe::TextureWrap::CLAMP_TO_EDGE,
                                  owe::TextureWrap::CLAMP_TO_EDGE,
                                  owe::TextureFilter::LINEAR,
                                  owe::TextureFilter::LINEAR };

    img->content->slots.push(vrento::Image::Slot {});
    auto& slot  = img->content->slots[usize(0)];
    slot.width  = img->header.width;
    slot.height = img->header.height;
    slot.mipmaps.push(ImageData {});
    auto& mip  = slot.mipmaps[usize(0)];
    mip.width  = img->header.width;
    mip.height = img->header.height;
    mip.size   = rstd::as_cast<isize>(pix.len());

    // Retain the live atlas storage even when the face is destroyed before upload.
    mip.data = face.RetainAtlasPixels();

    img->FinalizeContent();
    return Some(rstd::move(img));
}

// -- Text shader ----------------------------------------------------------

namespace
{

constexpr auto kTextShaderHlsl = R"hlsl(
[[vk::binding(0, 1)]] cbuffer ww_Uniforms {
    column_major float4x4 g_ModelViewProjectionMatrix;
};

struct VSInput {
    float3 a_Position : a_Position;
    float2 a_TexCoord : a_TexCoord;
    float4 a_Color    : a_Color;
};
struct PSInput {
    float4 sv_pos : SV_Position;
    float2 v_uv   : TEXCOORD0;
    float4 v_col  : COLOR0;
};

PSInput main_vs(VSInput i) {
    PSInput o;
    o.sv_pos = mul(g_ModelViewProjectionMatrix, float4(i.a_Position, 1.0));
    o.v_uv   = i.a_TexCoord;
    o.v_col  = i.a_Color;
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(1, 1)]]
Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 1)]]
SamplerState g_Texture0_sampler;

float4 main_ps(PSInput i) : SV_Target {
    float a = g_Texture0.Sample(g_Texture0_sampler, i.v_uv).r;
    return float4(i.v_col.rgb, i.v_col.a * a);
}
)hlsl"_str;

constexpr auto kTextCopyBackgroundShaderHlsl = R"hlsl(
[[vk::binding(0, 1)]] cbuffer ww_Uniforms {
    column_major float4x4 g_ModelViewProjectionMatrix;
    column_major float4x4 g_EffectModelViewProjectionMatrix;
};

struct VSInput {
    float3 a_Position : a_Position;
    float2 a_TexCoord : a_TexCoord;
};
struct PSInput {
    float4 sv_pos : SV_Position;
    float4 v_proj : TEXCOORD0;
};

PSInput main_vs(VSInput i) {
    float4 pos = float4(i.a_Position, 1.0);
    PSInput o;
    o.sv_pos = mul(g_ModelViewProjectionMatrix, pos);
    o.v_proj = mul(g_EffectModelViewProjectionMatrix, pos);
    return o;
}

[[vk::combinedImageSampler]][[vk::binding(1, 1)]]
Texture2D<float4> g_Texture0;
[[vk::combinedImageSampler]][[vk::binding(1, 1)]]
SamplerState g_Texture0_sampler;

float4 main_ps(PSInput i) : SV_Target {
    float2 uv = (i.v_proj.xy / i.v_proj.w) * 0.5 + 0.5;
    return float4(g_Texture0.Sample(g_Texture0_sampler, uv).rgb, 0.0);
}
)hlsl"_str;

Option<Arc<owe::SceneShader>> CompileInlineShader(ref<str> name, ref<str> source) {
    using namespace owe::vulkan;
    const auto                     src = source;
    rstd::array<ShaderCompUnit, 2> units {
        ShaderCompUnit {
            owe::ShaderType::VERTEX, rstd::into(src), "main_vs"_Str, SourceLang::Hlsl },
        ShaderCompUnit {
            owe::ShaderType::FRAGMENT, rstd::into(src), "main_ps"_Str, SourceLang::Hlsl },
    };
    ShaderCompOpt opt {};
    opt.target   = VulkanTarget::Vulkan_1_1;
    opt.optimize = false;

    Vec<Uni_ShaderSpv> spvs;
    if (! CompileAndLinkShaderUnits(units.as_slice(), opt, spvs)) {
        rstd_error("{} shader compile failed", name);
        return None();
    }

    auto shader        = Arc<owe::SceneShader>::make();
    shader->id         = owe::u32();
    shader->name       = rstd::into(name);
    shader->matrix_abi = owe::ShaderMatrixAbi::Hlsl;
    shader->codes.reserve(spvs.len());
    for (auto& spv : spvs) {
        shader->codes.push(rstd::move(spv->spirv));
    }
    shader->sampler_bindings.push(owe::SceneSamplerBinding {
        .texture_slot  = 0,
        .shader_member = "g_Texture0"_Str,
    });
    return Some(rstd::move(shader));
}

} // namespace

Option<Arc<owe::SceneShader>> GetTextSceneShader() {
    static const auto shader = CompileInlineShader("text"_str, kTextShaderHlsl);
    return shader.clone();
}

Option<Arc<owe::SceneShader>> GetTextCopyBackgroundSceneShader() {
    static const auto shader =
        CompileInlineShader("text_copybackground"_str, kTextCopyBackgroundShaderHlsl);
    return shader.clone();
}

auto TextUniformSource::Describe(rstd::mut_ref<rstd::dyn<UniformBindingSink>> sink) const
    -> rstd::Result<rstd::empty, UniformError> {
    auto bind = [&](TextUniformOutput output, ref<str> name) {
        return sink->Bind(
            UniformOutputId {
                .value = rstd::u32(static_cast<rstd::uint32_t>(output)),
            },
            name,
            UniformValueShape::Matrix(rstd::u32(4), rstd::u32(4)));
    };
    auto model = bind(TextUniformOutput::ModelViewProjection, G_MVP);
    if (model.is_err()) return rstd::Err(rstd::move(model).unwrap_err_unchecked());
    auto effect = bind(TextUniformOutput::EffectModelViewProjection, G_EMVP);
    if (effect.is_err()) return rstd::Err(rstd::move(effect).unwrap_err_unchecked());
    return rstd::Ok(rstd::empty {});
}

auto TextUniformSource::Version(rstd::ref<rstd::dyn<UniformUpdateContext>> context) const
    -> rstd::u64 {
    return context->Frame()->revision;
}

auto TextUniformSource::Evaluate(rstd::ref<rstd::dyn<UniformUpdateContext>>,
                                 rstd::mut_ref<rstd::dyn<UniformValueSink>> sink) const
    -> rstd::Result<rstd::empty, UniformError> {
    if (m_state->camera.is_none()) return rstd::Ok(rstd::empty {});

    auto write = [&](TextUniformOutput      output,
                     const Eigen::Matrix4d& matrix) -> rstd::Result<rstd::empty, UniformError> {
        const auto id = UniformOutputId {
            .value = rstd::u32(static_cast<rstd::uint32_t>(output)),
        };
        if (! sink->Wants(id)) return rstd::Ok(rstd::empty {});
        const auto value = UniformValue(ShaderValue::fromMatrix(matrix));
        return sink->Write(id, value.View());
    };

    m_state->node->UpdateTrans();
    const Eigen::Matrix4d model =
        (**m_state->camera).GetViewProjectionMatrix() * m_state->node->ModelTrans();
    auto result = write(TextUniformOutput::ModelViewProjection, model);
    if (result.is_err()) return result;

    if (! m_state->effect_projection) return rstd::Ok(rstd::empty {});
    auto& projection_node = *(*m_state->effect_projection)->node;
    projection_node.UpdateTrans();
    Eigen::Matrix4d effect_model = projection_node.ModelTrans();
    const auto&     size         = (*m_state->effect_projection)->size;
    if (size[rstd::usize(0)] > 0.0f && size[rstd::usize(1)] > 0.0f) {
        effect_model =
            effect_model *
            Eigen::Affine3d(Eigen::Scaling(static_cast<double>(size[rstd::usize(0)]) * 0.5,
                                           static_cast<double>(size[rstd::usize(1)]) * 0.5,
                                           1.0))
                .matrix();
    }
    const Eigen::Matrix4d effect_view = m_state->active_camera.is_some()
                                            ? (**m_state->active_camera).GetViewProjectionMatrix()
                                            : (**m_state->camera).GetViewProjectionMatrix();
    return write(TextUniformOutput::EffectModelViewProjection, effect_view * effect_model);
}

// -- TextLayouter ---------------------------------------------------------

namespace
{

struct TextLineRunGI {
    Vec<const GlyphInfo*> glyphs;
    float                 width { 0.0f };
    // Glyph index just past the last break opportunity on this line, i.e.
    // where the word currently being filled starts. 0 means the line holds
    // one unbroken word.
    rstd::size_t word_start { 0 };
};

bool IsBreakSpace(rstd::uint32_t cp) noexcept { return cp == ' ' || cp == '\t'; }

} // namespace

struct TextLayouter::Impl {
    FontFace*           face { nullptr };
    Arc<owe::SceneMesh> mesh;
    TextLayoutStyle     style;
    rstd::size_t        peak_quads { 0 };
    FontMetrics         metrics;

    float  last_text_w { 0.0f };
    float  last_text_h { 0.0f };
    float  last_source_w { 0.0f };
    float  last_source_h { 0.0f };
    float  last_source_center_x { 0.0f };
    float  last_source_center_y { 0.0f };
    String current_text;
    bool   missing_glyph_logged { false };
    bool   truncate_logged { false };

    // Scratch buffers reused across SetText calls — avoids reallocs for
    // every script tick. Sized at construction to peak capacity.
    Vec<float>          positions;
    Vec<float>          texcoords;
    Vec<float>          colors;
    Vec<rstd::uint32_t> indices;

    Impl(FontFace* f, Arc<owe::SceneMesh> m, TextLayoutStyle s, rstd::size_t pq)
        : face(f),
          mesh(rstd::move(m)),
          style(rstd::move(s)),
          peak_quads(pq),
          metrics(face->Metrics()) {
        positions.resize(usize(pq * 4 * 3), 0.0f);
        texcoords.resize(usize(pq * 4 * 2), 0.0f);
        colors.resize(usize(pq * 4 * 4), 0.0f);
        indices.resize(usize(pq * 6), 0u);
    }
};

TextLayouter::TextLayouter(FontFace* face, Arc<owe::SceneMesh> mesh, TextLayoutStyle style,
                           rstd::size_t peak_quads)
    : m_impl(Box<Impl>::make(face, rstd::move(mesh), rstd::move(style), peak_quads)) {}

TextLayouter::~TextLayouter() = default;

float             TextLayouter::TextWidth() const noexcept { return m_impl->last_text_w; }
float             TextLayouter::TextHeight() const noexcept { return m_impl->last_text_h; }
float             TextLayouter::SourceWidth() const noexcept { return m_impl->last_source_w; }
float             TextLayouter::SourceHeight() const noexcept { return m_impl->last_source_h; }
FontFace*         TextLayouter::Face() const noexcept { return m_impl->face; }
owe::SceneMesh&   TextLayouter::Mesh() const noexcept { return *m_impl->mesh; }
TextLayoutMetrics TextLayouter::Metrics() const noexcept {
    return {
        .text_width      = m_impl->last_text_w,
        .text_height     = m_impl->last_text_h,
        .source_width    = m_impl->last_source_w,
        .source_height   = m_impl->last_source_h,
        .source_center_x = m_impl->last_source_center_x,
        .source_center_y = m_impl->last_source_center_y,
        .padding         = m_impl->style.padding,
    };
}

void TextLayouter::SetFace(FontFace* face) {
    auto& im = *m_impl;
    if (face == nullptr || face == im.face) return;
    im.face                 = face;
    im.metrics              = face->Metrics();
    im.missing_glyph_logged = false;
    im.truncate_logged      = false;
    SetText(im.current_text.as_str());
}

TextGeometry ResolveTextGeometry(const TextGeometryPolicy& policy,
                                 const TextLayoutMetrics&  metrics) {
    auto positive = [](float value, float fallback) {
        return value > 0.0f ? value : fallback;
    };
    const float frame_w = positive(policy.frame_width, 1.0f);
    const float frame_h = positive(policy.frame_height, 1.0f);
    const float text_w  = positive(metrics.text_width, 1.0f);
    const float text_h  = positive(metrics.text_height, 1.0f);
    const float src_w   = positive(metrics.source_width, text_w);
    const float src_h   = positive(metrics.source_height, text_h);
    const float pad     = rstd::cmp::max(metrics.padding, 0.0f);
    const float src_cx  = f32(metrics.source_center_x).is_finite() ? metrics.source_center_x : 0.0f;
    const float src_cy  = f32(metrics.source_center_y).is_finite() ? metrics.source_center_y : 0.0f;

    const float text_bbox_w = text_w + 2.0f * pad;
    const float text_bbox_h = text_h + 2.0f * pad;
    const float src_bbox_w  = src_w + 2.0f * pad;
    const float src_bbox_h  = src_h + 2.0f * pad;

    const float dynamic_w = rstd::cmp::max(
        1024.0f, rstd::cmp::max(frame_w * 3.0f, rstd::cmp::max(text_bbox_w, src_bbox_w)));
    const float dynamic_h = rstd::cmp::max(
        256.0f, rstd::cmp::max(frame_h * 2.0f, rstd::cmp::max(text_bbox_h, src_bbox_h)));
    const bool dynamic_effect = policy.dynamic && policy.has_effect;

    const float dynamic_effect_w = rstd::cmp::max(text_bbox_w, frame_w);
    const float dynamic_effect_h = rstd::cmp::max(text_bbox_h, frame_h);
    const float rt_max_w = policy.dynamic ? (! policy.has_effect ? dynamic_w : dynamic_effect_w)
                                          : (policy.has_effect ? frame_w : src_bbox_w);
    const float rt_max_h = policy.dynamic ? (! policy.has_effect ? dynamic_h : dynamic_effect_h)
                                          : (policy.has_effect ? frame_h : src_bbox_h);

    TextGeometry out;
    out.rt_width            = policy.frame_bound ? frame_w : rstd::cmp::max(rt_max_w, src_bbox_w);
    out.rt_height           = policy.frame_bound ? frame_h : rstd::cmp::max(rt_max_h, src_bbox_h);
    out.effect_frame_width  = frame_w;
    out.effect_frame_height = frame_h;

    if (! policy.has_effect) {
        const bool tight_bbox = ! policy.preserve_text_bbox;
        out.draw_width        = tight_bbox ? src_w : text_bbox_w;
        out.draw_height       = tight_bbox ? src_h : text_bbox_h;
        out.draw_offset_x     = tight_bbox ? src_cx : 0.0f;
        out.draw_offset_y     = tight_bbox ? src_cy : 0.0f;
        out.uv_source_width   = tight_bbox ? src_w : src_bbox_w;
        out.uv_source_height  = tight_bbox ? src_h : src_bbox_h;
        return out;
    }

    if (dynamic_effect) {
        out.draw_width          = dynamic_effect_w;
        out.draw_height         = dynamic_effect_h;
        out.uv_source_width     = out.draw_width;
        out.uv_source_height    = out.draw_height;
        out.effect_frame_width  = out.uv_source_width;
        out.effect_frame_height = out.uv_source_height;
        return out;
    }

    out.draw_width       = frame_w;
    out.draw_height      = frame_h;
    out.uv_source_width  = frame_w;
    out.uv_source_height = frame_h;
    return out;
}

void TextLayouter::SetText(ref<str> utf8) {
    auto& im = *m_impl;

    String next_text = rstd::into(utf8);
    im.current_text  = rstd::move(next_text);
    auto codepoints  = DecodeUtf8(im.current_text.as_str().as_bytes());

    // Split into lines and look up pre-rasterised glyph metrics. A layer with
    // `limitwidth` also wraps on word boundaries once a line would grow past
    // `maxwidth`; a word that does not fit on a line of its own is broken
    // where it runs out of room.
    const float        wrap = im.style.wrap_width;
    Vec<TextLineRunGI> lines;
    lines.emplace_back();
    rstd::size_t total_glyph_quads = 0;
    for (rstd::uint32_t cp : codepoints) {
        if (cp == '\n') {
            lines.emplace_back();
            continue;
        }
        const auto* gi = im.face->Lookup(cp);
        if (gi == nullptr) {
            // Actuator is expected to Populate() before SetText, so this only
            // fires for codepoints that genuinely failed to rasterise (e.g.
            // missing in the font). Log-once keeps log noise bounded.
            if (! im.missing_glyph_logged) {
                rstd_info("text: codepoint U+{:04X} not rasterised, skipping", cp);
                im.missing_glyph_logged = true;
            }
            continue;
        }
        if (wrap > 0.0f && ! lines.last().unwrap()->glyphs.is_empty() &&
            lines.last().unwrap()->width + gi->advance_x > wrap) {
            if (IsBreakSpace(cp)) {
                // The break falls on the space itself; drop it rather than
                // carry it to the head of the next line.
                lines.emplace_back();
                continue;
            }
            auto&              line  = lines.last_mut().unwrap().get_mut();
            const rstd::size_t start = line.word_start;
            if (start > 0 && start < line.glyphs.len().to_primitive()) {
                TextLineRunGI next;
                for (usize i(start); i < line.glyphs.len(); ++i)
                    next.glyphs.emplace_back(line.glyphs[i]);
                for (const auto* moved : next.glyphs) next.width += moved->advance_x;
                line.glyphs.truncate(usize(start));
                line.width -= next.width;
                // The space that ended the word stays behind; it is invisible
                // but would still count towards the finished line's width.
                while (! line.glyphs.is_empty() &&
                       line.glyphs.last().unwrap().get()->pixel_w == 0 &&
                       line.glyphs.last().unwrap().get()->pixel_h == 0) {
                    line.width -= line.glyphs.last().unwrap().get()->advance_x;
                    (void)line.glyphs.pop();
                }
                line.word_start = 0;
                lines.push(rstd::move(next));
            } else {
                lines.emplace_back();
            }
        }
        auto& line = lines.last_mut().unwrap().get_mut();
        line.glyphs.emplace_back(gi);
        line.width += gi->advance_x;
        if (IsBreakSpace(cp)) line.word_start = line.glyphs.len().to_primitive();
        if (gi->pixel_w != 0 && gi->pixel_h != 0) ++total_glyph_quads;
    }

    // `maxrows` drops whatever did not fit; `limituseellipsis` marks the cut
    // by replacing the tail of the last kept row with an ellipsis.
    if (im.style.max_rows > 0 && lines.len().to_primitive() > im.style.max_rows) {
        if (im.style.row_limit_ellipsis) {
            const auto* dot = im.face->Lookup('.');
            if (dot != nullptr) {
                auto& last = lines[usize(im.style.max_rows - 1)];
                while (! last.glyphs.is_empty() &&
                       (wrap > 0.0f && last.width + 3.0f * dot->advance_x > wrap)) {
                    last.width -= last.glyphs.last().unwrap().get()->advance_x;
                    (void)last.glyphs.pop();
                }
                for (int i = 0; i < 3; ++i) {
                    last.glyphs.emplace_back(dot);
                    last.width += dot->advance_x;
                }
            }
        }
        lines.truncate(usize(im.style.max_rows));
        total_glyph_quads = 0;
        for (const auto& line : lines)
            for (const auto* gi : line.glyphs)
                if (gi->pixel_w != 0 && gi->pixel_h != 0) ++total_glyph_quads;
    }

    bool         has_bg      = im.style.opaquebackground;
    rstd::size_t total_quads = total_glyph_quads + (has_bg ? 1u : 0u);
    if (total_quads > im.peak_quads) {
        // Off-RT overflow only — the layouter emits top-to-bottom, so the
        // dropped tail quads are below the layer RT's visible window. Log-once
        // keeps a runaway terminal/log script from spamming every frame.
        if (! im.truncate_logged) {
            rstd_info("text: {} quads exceed peak capacity {}, truncating tail",
                      total_quads,
                      im.peak_quads);
            im.truncate_logged = true;
        }
        total_quads = im.peak_quads;
        if (has_bg && total_glyph_quads + 1 > im.peak_quads)
            total_glyph_quads = im.peak_quads - 1;
        else if (! has_bg)
            total_glyph_quads = total_quads;
    }

    auto& fm     = im.metrics;
    float text_w = 0.0f;
    for (auto& l : lines)
        if (l.width > text_w) text_w = l.width;
    // A wrapping layer aligns its lines inside the maxwidth box rather than
    // against the longest line, so a message shorter than the box still
    // starts at the left edge of a left-aligned layer.
    if (wrap > 0.0f) text_w = rstd::cmp::max(wrap, text_w);
    float text_h            = fm.ascender - fm.descender +
                              static_cast<float>(lines.len().to_primitive() - 1) * fm.line_height;
    im.last_text_w          = text_w;
    im.last_text_h          = text_h;
    im.last_source_w        = text_w;
    im.last_source_h        = text_h;
    im.last_source_center_x = 0.0f;
    im.last_source_center_y = 0.0f;

    // Zero the unused tail so stale data from the previous (longer) text
    // doesn't show up. Cheaper than tracking exact quad count downstream.
    for (auto& value : im.positions) value = 0.0f;
    for (auto& value : im.texcoords) value = 0.0f;
    for (auto& value : im.colors) value = 0.0f;
    for (auto& value : im.indices) value = 0u;

    auto write_quad = [&](rstd::size_t           q_idx,
                          float                  left,
                          float                  right,
                          float                  bottom,
                          float                  top,
                          float                  u_l,
                          float                  u_r,
                          float                  v_t,
                          float                  v_b,
                          const array<float, 4>& rgba) {
        rstd::size_t v_off     = q_idx * 4;
        const float  pos[4][3] = {
            { left, top, 0.0f },
            { right, top, 0.0f },
            { right, bottom, 0.0f },
            { left, bottom, 0.0f },
        };
        const float uv[4][2] = {
            { u_l, v_t },
            { u_r, v_t },
            { u_r, v_b },
            { u_l, v_b },
        };
        for (rstd::size_t k = 0; k < 4; ++k) {
            memcpy(&im.positions[usize((v_off + k) * 3)], pos[k], rstd::usize(sizeof(pos[k])));
            memcpy(&im.texcoords[usize((v_off + k) * 2)], uv[k], rstd::usize(sizeof(uv[k])));
            memcpy(&im.colors[usize((v_off + k) * 4)], rgba.data(), rstd::usize(sizeof(float) * 4));
        }
        rstd::size_t         i_off   = q_idx * 6;
        const rstd::uint32_t base    = static_cast<rstd::uint32_t>(v_off);
        im.indices[usize(i_off + 0)] = base + 0;
        im.indices[usize(i_off + 1)] = base + 1;
        im.indices[usize(i_off + 2)] = base + 2;
        im.indices[usize(i_off + 3)] = base + 0;
        im.indices[usize(i_off + 4)] = base + 2;
        im.indices[usize(i_off + 5)] = base + 3;
    };

    float text_top    = +text_h * 0.5f;
    float text_bottom = -text_h * 0.5f;
    float text_left   = -text_w * 0.5f;
    float text_right  = +text_w * 0.5f;
    (void)text_left;
    (void)text_right;
    (void)text_bottom;
    float pad = im.style.padding;

    rstd::size_t q = 0;

    if (has_bg) {
        float           u_l = 1.0f / static_cast<float>(fm.atlas_w);
        float           u_r = 3.0f / static_cast<float>(fm.atlas_w);
        float           v_t = 1.0f / static_cast<float>(fm.atlas_h);
        float           v_b = 3.0f / static_cast<float>(fm.atlas_h);
        array<float, 4> rgba {
            im.style.background_color[usize(0)] * im.style.background_brightness,
            im.style.background_color[usize(1)] * im.style.background_brightness,
            im.style.background_color[usize(2)] * im.style.background_brightness,
            1.0f,
        };
        write_quad(q++,
                   -text_w * 0.5f - pad,
                   +text_w * 0.5f + pad,
                   -text_h * 0.5f - pad,
                   +text_h * 0.5f + pad,
                   u_l,
                   u_r,
                   v_t,
                   v_b,
                   rgba);
    }

    array<float, 4> text_rgba {
        im.style.color[usize(0)],
        im.style.color[usize(1)],
        im.style.color[usize(2)],
        im.style.alpha,
    };

    rstd::size_t emitted_glyphs       = 0;
    bool         have_glyph_bounds    = false;
    float        glyph_min_x          = 0.0f;
    float        glyph_max_x          = 0.0f;
    float        glyph_min_y          = 0.0f;
    float        glyph_max_y          = 0.0f;
    auto         include_glyph_bounds = [&](float left, float right, float bottom, float top) {
        if (! have_glyph_bounds) {
            glyph_min_x       = left;
            glyph_max_x       = right;
            glyph_min_y       = bottom;
            glyph_max_y       = top;
            have_glyph_bounds = true;
            return;
        }
        glyph_min_x = rstd::cmp::min(left, glyph_min_x);
        glyph_max_x = rstd::cmp::max(right, glyph_max_x);
        glyph_min_y = rstd::cmp::min(bottom, glyph_min_y);
        glyph_max_y = rstd::cmp::max(top, glyph_max_y);
    };
    for (rstd::size_t li = 0; li < lines.len().to_primitive(); ++li) {
        const auto& line = lines[usize(li)];
        float       line_origin_x;
        if (im.style.halign.as_str().contains("left"_str)) {
            line_origin_x = -text_w * 0.5f;
        } else if (im.style.halign.as_str().contains("right"_str)) {
            line_origin_x = +text_w * 0.5f - line.width;
        } else {
            line_origin_x = -line.width * 0.5f;
        }
        float baseline_y = text_top - fm.ascender - static_cast<float>(li) * fm.line_height;

        float pen_x = line_origin_x;
        for (const auto* gi : line.glyphs) {
            if (q >= im.peak_quads) break;
            if (gi->pixel_w == 0 || gi->pixel_h == 0) {
                pen_x += gi->advance_x;
                continue;
            }
            float left   = pen_x + gi->bearing_x;
            float right  = left + static_cast<float>(gi->pixel_w);
            float top    = baseline_y + gi->bearing_y;
            float bottom = top - static_cast<float>(gi->pixel_h);
            float u_l    = static_cast<float>(gi->atlas_x) / static_cast<float>(fm.atlas_w);
            float u_r =
                static_cast<float>(gi->atlas_x + gi->pixel_w) / static_cast<float>(fm.atlas_w);
            float v_t = static_cast<float>(gi->atlas_y) / static_cast<float>(fm.atlas_h);
            float v_b =
                static_cast<float>(gi->atlas_y + gi->pixel_h) / static_cast<float>(fm.atlas_h);
            write_quad(q++, left, right, bottom, top, u_l, u_r, v_t, v_b, text_rgba);
            include_glyph_bounds(left, right, bottom, top);
            pen_x += gi->advance_x;
            ++emitted_glyphs;
            if (emitted_glyphs >= total_glyph_quads) break;
        }
        if (emitted_glyphs >= total_glyph_quads) break;
    }

    // A wrapping layer keeps its maxwidth box as the source rectangle: the
    // lines are already placed inside that box, and shrinking the source to
    // the ink would re-centre them and lose the alignment again.
    if (have_glyph_bounds && wrap <= 0.0f) {
        im.last_source_w        = rstd::cmp::max(glyph_max_x - glyph_min_x, 1.0f);
        im.last_source_h        = rstd::cmp::max(glyph_max_y - glyph_min_y, 1.0f);
        im.last_source_center_x = 0.5f * (glyph_min_x + glyph_max_x);
        im.last_source_center_y = 0.5f * (glyph_min_y + glyph_max_y);
        // Texture composition restores this offset; direct draws retain layout coordinates.
        if (im.style.mesh_origin == TextMeshOrigin::InkBounds) {
            const float        shift_x      = -im.last_source_center_x;
            const float        shift_y      = -im.last_source_center_y;
            const rstd::size_t vertex_count = q * 4;
            for (rstd::size_t i = 0; i < vertex_count; ++i) {
                im.positions[usize(i * 3 + 0)] += shift_x;
                im.positions[usize(i * 3 + 1)] += shift_y;
            }
        }
    }

    // Push into the mesh. Vertex array's stride is interleaved with padding
    // already laid out by SceneVertexArray; SetVertex scatters by name.
    auto& v = im.mesh->GetVertexArray(rstd::usize());
    v.SetVertex(WE_IN_POSITION, im.positions.as_slice());
    v.SetVertex(WE_IN_TEXCOORD, im.texcoords.as_slice());
    v.SetVertex(WE_IN_COLOR, im.colors.as_slice());

    auto& idx = im.mesh->GetIndexArray(rstd::usize());
    idx.Assign(rstd::usize(), im.indices.as_slice());
    // Render only the indices we actually populated (rest are zeroed out
    // and reference vertex 0, which is harmless but wastes draw calls).
    idx.SetRenderDataCount(rstd::usize(q * 6));

    im.mesh->SetDirty();
}

void TextLayouter::SetHorizontalAlign(ref<str> align) {
    auto& im        = *m_impl;
    im.style.halign = rstd::into(align);
    SetText(im.current_text.as_str());
}

} // namespace owe::text
