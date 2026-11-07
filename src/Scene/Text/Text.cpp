module;

#include <rstd/macro.hpp>

module wescene.text;
import eigen;
import wescene.pkg.spec_names;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import wescene.scene;
import wescene.shader_compile;

using rstd::mem::memcpy;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::ffi::CString;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::sync::Arc;

namespace owe::text
{

struct AtlasPage::Impl {
    rstd::uint32_t          extent, x {}, y {}, row {};
    String                  url;
    Arc<Vec<rstd::uint8_t>> pixels { Arc<Vec<rstd::uint8_t>>::make() };
    Vec<AtlasDirtyRect>     dirty;
    rstd::uint64_t          serial {}, acknowledged {};
    void                    MarkDirty(AtlasDirtyRect rect) {
        dirty.push(rstd::move(rect));
        ++serial;
    }
    Impl(rstd::uint32_t size, String name, bool white): extent(size), url(rstd::move(name)) {
        if (size < 8 || size > 8192) return;
        pixels->resize(usize(size) * usize(size), 0);
        if (! white) return;
        x = 4;
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) (*pixels)[usize(y * size + x)] = 255;
        row = 4;
        MarkDirty(AtlasDirtyRect { 0, 0, 4, 4 });
    }
};
AtlasPage::AtlasPage(rstd::uint32_t extent, String url, bool white)
    : m_impl(Box<Impl>::make(extent, rstd::move(url), white)) {}
AtlasPage::~AtlasPage() = default;
auto AtlasPage::Extent() const -> rstd::uint32_t { return m_impl->extent; }
auto AtlasPage::AtlasUrl() const -> ref<str> { return m_impl->url.as_str(); }
auto AtlasPage::AtlasPixels() const -> slice<rstd::uint8_t> { return m_impl->pixels->as_slice(); }
auto AtlasPage::DirtyRects() const -> slice<AtlasDirtyRect> { return m_impl->dirty.as_slice(); }
auto AtlasPage::SnapshotDirtyUpdates() const -> AtlasUpdateBatch {
    AtlasUpdateBatch batch { .serial = m_impl->serial };
    auto             dirty = DirtyRects();
    if (dirty.is_empty()) return batch;

    auto first = dirty.first().unwrap();
    auto min_x = first->x, min_y = first->y;
    auto max_x = first->x + first->w, max_y = first->y + first->h;
    for (auto& rect : dirty) {
        min_x = rstd::cmp::min(min_x, rect.x);
        min_y = rstd::cmp::min(min_y, rect.y);
        max_x = rstd::cmp::max(max_x, rect.x + rect.w);
        max_y = rstd::cmp::max(max_y, rect.y + rect.h);
    }

    AtlasUpdateRect update { .rect = { min_x, min_y, max_x - min_x, max_y - min_y } };
    Vec<u8>         bytes;
    bytes.reserve(usize(update.rect.w) * usize(update.rect.h));
    for (rstd::uint32_t y = 0; y < update.rect.h; ++y) {
        const auto offset = usize(min_y + y) * usize(m_impl->extent) + usize(min_x);
        bytes.extend_from_slice(slice<u8>::from_raw_parts(
            reinterpret_cast<const byte*>(m_impl->pixels->as_slice().as_raw_ptr() +
                                          offset.to_primitive()),
            usize(update.rect.w)));
    }
    update.bytes = rstd::move(bytes);
    batch.rects.push(rstd::move(update));
    return batch;
}
void AtlasPage::AcknowledgeUpdates(rstd::uint64_t serial) {
    if (serial <= m_impl->acknowledged || serial > m_impl->serial) return;
    // Dirty rectangles are the unacknowledged suffix of consecutive write serials.
    auto entry_serial = m_impl->acknowledged;
    m_impl->dirty.retain([&](const AtlasDirtyRect&) {
        return ++entry_serial > serial;
    });
    m_impl->acknowledged = serial;
}
void AtlasPage::ClearDirtyRects() { AcknowledgeUpdates(m_impl->serial); }
auto AtlasPage::RetainAtlasPixels() const -> owe::ImageDataPtr {
    struct Owner {
        Arc<Vec<rstd::uint8_t>> pixels;
        void                    operator()(rstd::uint8_t*) const noexcept {}
    };
    return ImageDataPtr(m_impl->pixels->data(), Owner { m_impl->pixels.clone() });
}
struct TextResources::Impl {
    vectext::SystemTextService  service;
    AtlasOptions                options;
    Vec<Arc<AtlasPage>>         pages;
    HashMap<String, AtlasGlyph> glyphs;
    rstd::uint64_t              id { vectext::new_identity() }, rasters {}, layouts {};
    void                        Initialize() {
        if (options.extent >= 8 && options.extent <= 8192 && options.max_pages &&
            options.max_pages <= 64 &&
            rstd::uint64_t(options.extent) * options.extent * options.max_pages <= 268435456)
            pages.push(
                Arc<AtlasPage>::make(options.extent, rstd::format("text_atlas_{}_0", id), true));
    }
    explicit Impl(AtlasOptions options): options(options) { Initialize(); }
    Impl(vectext::FontCollection collection, AtlasOptions options)
        : service(rstd::move(collection)), options(options) {
        Initialize();
    }
};
TextResources::TextResources(AtlasOptions options): m_impl(Box<Impl>::make(options)) {}
TextResources::TextResources(vectext::FontCollection collection, AtlasOptions options)
    : m_impl(Box<Impl>::make(rstd::move(collection), options)) {}
TextResources::~TextResources() = default;
auto TextResources::RegisterFont(slice<u8> bytes)
    -> Result<vectext::FontSource, vectext::TextError> {
    auto collection = m_impl->service.collection();
    if (collection.is_err()) return Err(collection.unwrap_err());
    return collection->register_font(slice<unsigned char>::from_raw_parts(
        reinterpret_cast<const unsigned char*>(bytes.as_raw_ptr()), bytes.len()));
}
auto TextResources::Layout(vectext::TextRequest request)
    -> Result<vectext::TextLayout, vectext::TextError> {
    auto result = m_impl->service.layout(request);
    if (result.is_ok()) ++m_impl->layouts;
    return result;
}
auto TextResources::Pages() const -> slice<Arc<AtlasPage>> { return m_impl->pages.as_slice(); }
auto TextResources::RasterCount() const -> rstd::uint64_t { return m_impl->rasters; }
auto TextResources::LayoutCount() const -> rstd::uint64_t { return m_impl->layouts; }
auto TextResources::Rasterize(const vectext::FontInstance& font, vectext::Index glyph, double scale)
    -> Result<AtlasGlyph, vectext::TextError> {
    auto& im = *m_impl;
    if (! vectext::finite(scale) || scale <= 0 || scale > 64)
        return Err(vectext::TextError::InvalidValue);
    auto bits = rstd::mem::transmute<rstd::uint64_t>(scale);
    auto key  = rstd::format("{}:{}:{}", font.id(), glyph, bits);
    if (auto cached = im.glyphs.get(key.as_str()); cached.is_some())
        return Ok(AtlasGlyph(**cached));
    auto raster = font.rasterize(glyph, scale);
    if (raster.is_err()) return Err(raster.unwrap_err());
    auto bitmap = rstd::move(raster).unwrap();
    auto extent = im.options.extent;
    if (extent < 8 || extent > 8192 || ! im.options.max_pages || im.options.max_pages > 64 ||
        rstd::uint64_t(extent) * extent * im.options.max_pages > 268435456 ||
        bitmap.width > extent - 2 || bitmap.height > extent - 2)
        return Err(vectext::TextError::Limit);
    AtlasGlyph entry {
        .width = bitmap.width, .height = bitmap.height, .origin = bitmap.origin, .scale = scale
    };
    if (bitmap.width && bitmap.height) {
        usize page {};
        for (;;) {
            if (page == im.pages.len()) {
                if (page == usize(im.options.max_pages)) return Err(vectext::TextError::Limit);
                im.pages.push(
                    Arc<AtlasPage>::make(extent, rstd::format("text_atlas_{}_{}", im.id, page)));
            }
            auto& p = *im.pages[page]->m_impl;
            auto  x = p.x, y = p.y, row = p.row;
            if (x + bitmap.width + 2 > extent) {
                x = 0;
                y += row;
                row = 0;
            }
            if (y + bitmap.height + 2 > extent) {
                ++page;
                continue;
            }
            entry.page = static_cast<rstd::uint32_t>(page.to_primitive());
            entry.x    = x + 1;
            entry.y    = y + 1;
            for (unsigned yy = 0; yy < bitmap.height; ++yy)
                for (unsigned xx = 0; xx < bitmap.width; ++xx)
                    (*p.pixels)[usize((entry.y + yy) * extent + entry.x + xx)] =
                        bitmap.coverage[usize(yy * bitmap.width + xx)];
            p.x   = x + bitmap.width + 2;
            p.y   = y;
            p.row = rstd::cmp::max(row, bitmap.height + 2);
            p.MarkDirty(AtlasDirtyRect { entry.x, entry.y, entry.width, entry.height });
            break;
        }
    }
    ++im.rasters;
    (void)im.glyphs.insert(rstd::move(key), AtlasGlyph(entry));
    return Ok(entry);
}
auto EnsureSceneTextResources(Scene& scene) -> Arc<TextResources> {
    auto existing = scene.ExtensionMut<Arc<TextResources>>();
    if (existing.is_none()) {
        scene.InstallExtension(Box<Arc<TextResources>>::make(Arc<TextResources>::make()));
        existing = scene.ExtensionMut<Arc<TextResources>>();
    }
    return (**existing).clone();
}
auto SceneTextResources(Scene& scene) -> TextResources* {
    auto existing = scene.ExtensionMut<Arc<TextResources>>();
    return existing.is_some() ? (**existing).as_ptr().as_raw_ptr() : nullptr;
}
auto TextResources::RegisterPages(Scene& scene) -> bool {
    for (auto& page : m_impl->pages) {
        auto url = page->AtlasUrl();
        if (scene.Texture(url).is_some()) continue;
        auto image = BuildAtlasImage(*page, url);
        if (image.is_none()) return false;
        SceneTexture texture;
        texture.url    = String::make(url);
        texture.sample = (*image)->header.sample;
        scene.RegisterTexture(String::make(url), rstd::move(texture));
        scene.RegisterRuntimeImage(String::make(url), rstd::move(image).unwrap());
    }
    return true;
}

auto BuildAtlasImage(const AtlasPage& face, ref<str> key) -> Option<Arc<owe::Image>> {
    auto dim = face.Extent();
    auto pix = face.AtlasPixels();
    if (dim == 0 || dim == 0 || pix.is_empty()) return None();

    auto img          = Arc<owe::Image>::make();
    img->content->key = rstd::into(key);

    img->header.width         = static_cast<rstd::int32_t>(dim);
    img->header.height        = static_cast<rstd::int32_t>(dim);
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
    float g_TextOpacity;
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
    return float4(i.v_col.rgb, i.v_col.a * a * g_TextOpacity);
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

auto TextOpacityUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    auto result = sink->Bind(
        UniformOutputId { .value = u32(static_cast<rstd::uint32_t>(TextOpacityOutput::Opacity)) },
        "g_TextOpacity"_str,
        UniformValueShape::Float(u32(1)));
    if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
    return Ok(empty {});
}

auto TextOpacityUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto TextOpacityUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                        mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    const auto id =
        UniformOutputId { .value = u32(static_cast<rstd::uint32_t>(TextOpacityOutput::Opacity)) };
    if (! sink->Wants(id)) return Ok(empty {});
    const float opacity = m_node.is_some() ? (**m_node).EffectiveAlpha() : 1.0f;
    const auto  value   = UniformValue(ShaderValue(opacity));
    return sink->Write(id, value.View());
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
    const Eigen::Matrix4d model  = (**m_state->camera).GetViewProjectionMatrix() *
                                   m_state->node->ModelTrans() * m_state->node->GeometryTransform();
    auto                  result = write(TextUniformOutput::ModelViewProjection, model);
    if (result.is_err()) return result;

    if (! m_state->effect_projection) return rstd::Ok(rstd::empty {});
    auto& projection_node = *(*m_state->effect_projection)->node;
    projection_node.UpdateTrans();
    Eigen::Matrix4d effect_model =
        projection_node.ModelTrans() * projection_node.GeometryTransform();
    const auto& size = (*m_state->effect_projection)->size;
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

struct TextLayouter::Impl {
    Arc<TextResources>          resources;
    TextFont                    font;
    Arc<SceneMesh>              mesh;
    TextLayoutStyle             style;
    rstd::size_t                peak;
    Scene*                      scene;
    String                      text;
    Option<vectext::TextLayout> layout;
    TextLayoutMetrics           metrics;
    Vec<rstd::size_t>           capacities;
    Impl(Arc<TextResources> r, TextFont f, Arc<SceneMesh> m, TextLayoutStyle s, rstd::size_t p,
         Scene* sc)
        : resources(rstd::move(r)),
          font(rstd::move(f)),
          mesh(rstd::move(m)),
          style(rstd::move(s)),
          peak(p),
          scene(sc) {}
    auto Update(ref<str> next, double size, ref<str> alignment)
        -> Result<empty, vectext::TextError> {
        vectext::TextRequest request;
        request.bytes        = Some(next->as_bytes());
        request.invalid_text = vectext::InvalidText::Replace;
        request.family       = font.family.is_empty() ? "sans"_str : font.family.as_str();
        request.source       = font.source;
        request.size         = size;
        if (style.wrap_width > 0) request.width = Some(double(style.wrap_width));
        request.tabs       = vectext::TextTabs::Ignore;
        request.whitespace = vectext::TextWhitespace::TrimWrap;
        request.max_lines  = style.max_rows;
        request.overflow   = style.row_limit_ellipsis ? vectext::TextOverflow::ThreeDots
                                                      : vectext::TextOverflow::Clip;
        request.alignment  = alignment->contains("left"_str)    ? vectext::TextAlignment::Left
                             : alignment->contains("right"_str) ? vectext::TextAlignment::Right
                                                                : vectext::TextAlignment::Center;
        auto built         = resources->Layout(request);
        if (built.is_err()) return Err(built.unwrap_err());
        auto output = rstd::move(built).unwrap();
        struct Quad {
            AtlasGlyph      atlas;
            float           left, right, bottom, top;
            array<float, 4> color;
        };
        Vec<Quad>         quads;
        TextLayoutMetrics measured;
        const auto        bounds = output.typographic_bounds();
        measured.text_width      = float(bounds.width);
        measured.text_height     = float(bounds.height);
        measured.source_width    = measured.text_width;
        measured.source_height   = measured.text_height;
        measured.padding         = style.padding;
        auto pages               = resources->Pages();
        if (pages.is_empty()) return Err(vectext::TextError::Limit);
        if (style.opaquebackground) {
            auto pad = style.padding;
            quads.push(Quad { AtlasGlyph { .x = 1, .y = 1, .width = 2, .height = 2 },
                              -measured.text_width / 2 - pad,
                              measured.text_width / 2 + pad,
                              -measured.text_height / 2 - pad,
                              measured.text_height / 2 + pad,
                              { style.background_color[usize()] * style.background_brightness,
                                style.background_color[usize(1)] * style.background_brightness,
                                style.background_color[usize(2)] * style.background_brightness,
                                1.0f } });
        }
        bool  have_bounds {};
        float minx {}, maxx {}, miny {}, maxy {};
        for (auto& glyph : output.glyphs()) {
            if (! glyph.drawable) continue;
            auto raster = resources->Rasterize(output.fonts()[usize(glyph.font)], glyph.glyph);
            if (raster.is_err()) return Err(raster.unwrap_err());
            auto atlas = raster.unwrap();
            if (! atlas.width || ! atlas.height) continue;
            float left = float(glyph.position.x + atlas.origin.x) - measured.text_width / 2;
            float top =
                measured.text_height / 2 - float(glyph.position.y - bounds.y + atlas.origin.y);
            float right  = left + float(atlas.width / atlas.scale),
                  bottom = top - float(atlas.height / atlas.scale);
            if (! have_bounds) {
                minx        = left;
                maxx        = right;
                miny        = bottom;
                maxy        = top;
                have_bounds = true;
            } else {
                minx = rstd::cmp::min(minx, left);
                maxx = rstd::cmp::max(maxx, right);
                miny = rstd::cmp::min(miny, bottom);
                maxy = rstd::cmp::max(maxy, top);
            }
            quads.push(Quad { atlas,
                              left,
                              right,
                              bottom,
                              top,
                              { style.color[usize()],
                                style.color[usize(1)],
                                style.color[usize(2)],
                                style.alpha } });
            if (quads.len() > usize(peak)) return Err(vectext::TextError::Limit);
        }
        if (quads.len() > usize(peak)) return Err(vectext::TextError::Limit);
        if (have_bounds && style.wrap_width <= 0) {
            measured.source_width    = rstd::cmp::max(maxx - minx, 1.0f);
            measured.source_height   = rstd::cmp::max(maxy - miny, 1.0f);
            measured.source_center_x = (minx + maxx) / 2;
            measured.source_center_y = (miny + maxy) / 2;
            if (style.mesh_origin == TextMeshOrigin::InkBounds)
                for (auto& quad : quads) {
                    quad.left -= measured.source_center_x;
                    quad.right -= measured.source_center_x;
                    quad.bottom -= measured.source_center_y;
                    quad.top -= measured.source_center_y;
                }
        }
        Vec<SceneMesh::Submesh> parts;
        Vec<rstd::size_t>       next_capacities;
        bool                    topology {};
        usize                   begin {};
        do {
            auto page = begin < quads.len() ? quads[begin].atlas.page : 0;
            auto end  = begin;
            while (end < quads.len() && quads[end].atlas.page == page) ++end;
            auto count      = (end - begin).to_primitive();
            auto part_index = parts.len();
            auto capacity   = rstd::cmp::max<rstd::size_t>(count, 1);
            if (part_index < capacities.len() && part_index < mesh->Submeshes().len() &&
                mesh->Submeshes()[part_index].material_slot == u32(page))
                capacity = rstd::cmp::max(capacity, capacities[part_index]);
            if (part_index >= capacities.len() || capacity != capacities[part_index] ||
                part_index >= mesh->Submeshes().len() ||
                mesh->Submeshes()[part_index].material_slot != u32(page))
                topology = true;
            next_capacities.push(rstd::size_t(capacity));
            Vec<float>          positions, uv, colors;
            Vec<rstd::uint32_t> indices;
            positions.resize(usize(capacity * 12), 0);
            uv.resize(usize(capacity * 8), 0);
            colors.resize(usize(capacity * 16), 0);
            indices.resize(usize(capacity * 6), 0);
            auto extent = float(resources->Pages()[usize(page)]->Extent());
            for (auto i = begin; i < end; ++i) {
                auto& q = quads[i];
                auto  n = (i - begin).to_primitive();
                float pos[] { q.left,  q.top,    0, q.right, q.top,    0,
                              q.right, q.bottom, 0, q.left,  q.bottom, 0 };
                auto  l = q.atlas.x / extent, r = (q.atlas.x + q.atlas.width) / extent;
                auto  t = q.atlas.y / extent, b = (q.atlas.y + q.atlas.height) / extent;
                float tex[] { l, t, r, t, r, b, l, b };
                for (unsigned j = 0; j < 12; ++j) positions[usize(n * 12 + j)] = pos[j];
                for (unsigned j = 0; j < 8; ++j) uv[usize(n * 8 + j)] = tex[j];
                for (unsigned j = 0; j < 16; ++j) colors[usize(n * 16 + j)] = q.color[usize(j % 4)];
                unsigned order[] { 0, 1, 2, 0, 2, 3 };
                for (unsigned j = 0; j < 6; ++j)
                    indices[usize(n * 6 + j)] = static_cast<rstd::uint32_t>(n * 4 + order[j]);
            }
            SceneMesh::Submesh part;
            part.material_slot   = u32(page);
            part.preserve_output = ! parts.is_empty();
            SceneVertexArray vertex({ { .name = "a_Position"_Str, .type = VertexType::FLOAT3 },
                                      { .name = "a_TexCoord"_Str, .type = VertexType::FLOAT2 },
                                      { .name = "a_Color"_Str, .type = VertexType::FLOAT4 } },
                                    usize(capacity * 4));
            vertex.SetVertex(WE_IN_POSITION, positions.as_slice());
            vertex.SetVertex(WE_IN_TEXCOORD, uv.as_slice());
            vertex.SetVertex(WE_IN_COLOR, colors.as_slice());
            part.vertex_arrays.push(rstd::move(vertex));
            SceneIndexArray index(usize(capacity * 6));
            index.Assign(usize(), indices.as_slice());
            index.SetRenderDataCount(usize(count * 6));
            part.index_arrays.push(rstd::move(index));
            parts.push(rstd::move(part));
            begin = end;
        } while (begin < quads.len());
        if (scene && ! resources->RegisterPages(*scene)) return Err(vectext::TextError::Raster);
        auto& materials = mesh->MaterialSlots();
        if (materials.is_empty()) materials.push(Arc<SceneMaterial>::make());
        for (usize i = materials.len(); i < resources->Pages().len(); ++i) {
            auto material = Arc<SceneMaterial>::make(*materials[usize()]);
            material->texture_sources.clear();
            material->textures.clear();
            material->textures.push(String::make(resources->Pages()[i]->AtlasUrl()));
            materials.push(rstd::move(material));
            topology = true;
        }
        if (materials[usize()]->textures.is_empty())
            materials[usize()]->textures.push(
                String::make(resources->Pages()[usize()]->AtlasUrl()));
        topology = topology || parts.len() != mesh->Submeshes().len();
        if (topology)
            mesh->Submeshes() = rstd::move(parts);
        else
            for (usize i {}; i < parts.len(); ++i) {
                auto& target = mesh->Submeshes()[i];
                auto& vertex = parts[i].vertex_arrays[usize()];
                target.vertex_arrays[usize()].SetVertexs(
                    usize(), slice<float>::from_raw_parts(vertex.Data(), vertex.DataSize()));
                auto& index = parts[i].index_arrays[usize()];
                target.index_arrays[usize()].Assign(
                    usize(),
                    slice<rstd::uint32_t>::from_raw_parts(index.Data(), index.DataCount()));
                target.index_arrays[usize()].SetRenderDataCount(index.RenderDataCount());
            }
        capacities   = rstd::move(next_capacities);
        text         = String::make(next);
        font.size    = size;
        style.halign = String::make(alignment);
        layout       = Some(rstd::move(output));
        metrics      = measured;
        if (topology)
            mesh->SetLayoutDirty();
        else
            mesh->SetDirty();
        return Ok(empty {});
    }
};
TextLayouter::TextLayouter(Arc<TextResources> resources, TextFont font, Arc<SceneMesh> mesh,
                           TextLayoutStyle style, rstd::size_t peak, Scene* scene)
    : m_impl(Box<Impl>::make(rstd::move(resources), rstd::move(font), rstd::move(mesh),
                             rstd::move(style), peak, scene)) {}
TextLayouter::~TextLayouter() = default;
auto TextLayouter::SetText(ref<str> text) -> Result<empty, vectext::TextError> {
    if (m_impl->layout.is_some() && text == m_impl->text.as_str()) return Ok(empty {});
    return m_impl->Update(text, m_impl->font.size, m_impl->style.halign.as_str());
}
auto TextLayouter::SetPixelSize(double size) -> Result<empty, vectext::TextError> {
    if (m_impl->layout.is_some() && size == m_impl->font.size) return Ok(empty {});
    return m_impl->Update(m_impl->text.as_str(), size, m_impl->style.halign.as_str());
}
auto TextLayouter::SetHorizontalAlign(ref<str> alignment) -> Result<empty, vectext::TextError> {
    if (m_impl->layout.is_some() && alignment == m_impl->style.halign.as_str()) return Ok(empty {});
    return m_impl->Update(m_impl->text.as_str(), m_impl->font.size, alignment);
}
auto  TextLayouter::Layout() const -> const Option<vectext::TextLayout>& { return m_impl->layout; }
float TextLayouter::TextWidth() const noexcept { return m_impl->metrics.text_width; }
float TextLayouter::TextHeight() const noexcept { return m_impl->metrics.text_height; }
float TextLayouter::SourceWidth() const noexcept { return m_impl->metrics.source_width; }
float TextLayouter::SourceHeight() const noexcept { return m_impl->metrics.source_height; }
auto  TextLayouter::Metrics() const noexcept -> TextLayoutMetrics { return m_impl->metrics; }
auto  TextLayouter::Mesh() const noexcept -> SceneMesh& { return *m_impl->mesh; }

} // namespace owe::text
