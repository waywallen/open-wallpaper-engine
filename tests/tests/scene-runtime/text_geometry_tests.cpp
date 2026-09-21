#include <rstd/test/gtest.hpp>

#include <array>
#include <memory>
#include <vector>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.text;
import wescene.types;

using namespace rstd::literals;
using namespace rstd::prelude;
using rstd::sync::Arc;

TEST(FontFace, ImageRetainsLivePixelsAfterCacheDestruction) {
    rstd::Option<rstd::sync::Arc<owe::Image>> image;
    Vec<std::uint8_t>                         expected;
    {
        auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace"_str);
        ASSERT_TRUE(font.bytes.is_some());
        owe::text::FontCache cache;
        auto*                face = cache.GetFace(font, 64);
        ASSERT_NE(face, nullptr);
        face->Populate(owe::text::DecodeUtf8("A"_str.as_bytes()).as_slice());
        image = owe::text::BuildAtlasImage(*face, "atlas"_str);
        ASSERT_TRUE(image.is_some());
        face->Populate(owe::text::DecodeUtf8("B"_str.as_bytes()).as_slice());
        auto pixels = face->AtlasPixels();
        expected    = Vec<std::uint8_t>::from(pixels);
        EXPECT_EQ((*image)->content->slots[usize(0)].mipmaps[usize(0)].data.get(),
                  pixels.as_raw_ptr());
    }
    const auto& mip = (*image)->content->slots[usize(0)].mipmaps[usize(0)];
    EXPECT_EQ(Vec<std::uint8_t>::from(
                  slice<std::uint8_t>::from_raw_parts(mip.data.get(), expected.len())),
              expected);
}

TEST(VideoPlayback, AdapterSharesAndRetainsControlState) {
    using namespace rstd;
    auto                                  state = sync::Arc<owe::VideoPlaybackState>::make();
    sync::Arc<dyn<vrento::VideoPlayback>> playback =
        sync::Arc<dyn<vrento::VideoPlayback>>::make(owe::SharedVideoPlayback { state.clone() });
    state->Pause();
    state->Seek(f64(3));
    state->SetRate(f64(2));
    EXPECT_FALSE(playback->Snapshot().playing);
    EXPECT_EQ(playback->Snapshot().seek_seconds, f64(3));
    EXPECT_EQ(playback->Snapshot().rate, f64(2));
    playback->PublishTime(f64(4), Some(f64(20)));
    EXPECT_EQ(state->CurrentTime(), f64(4));
    EXPECT_EQ(state->Duration().unwrap(), f64(20));
    state = sync::Arc<owe::VideoPlaybackState>::make();
    playback->PublishTime(f64(5), None<f64>());
    EXPECT_FALSE(playback->Snapshot().playing);
    EXPECT_EQ(playback->Snapshot().seek_seconds, f64(3));
}

TEST(FontFace, TabHasNoLayoutOrRasterizedGlyph) {
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace"_str);
    ASSERT_TRUE(font.bytes.is_some());

    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);

    const rstd::array<std::uint32_t, 1> codepoints { std::uint32_t('\t') };
    face->Populate(codepoints.as_slice());

    const auto* tab = face->Lookup('\t');
    ASSERT_NE(tab, nullptr);
    EXPECT_EQ(tab->pixel_w, 0u);
    EXPECT_EQ(tab->pixel_h, 0u);
    EXPECT_EQ(tab->advance_x, 0.0f);
}

TEST(TextLayouter, TabIsIgnoredWithoutAControlQuad) {
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace"_str);
    ASSERT_TRUE(font.bytes.is_some());

    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);
    const auto tab_text   = owe::text::DecodeUtf8("hour:\n\t\t\t\tminute:"_str.as_bytes());
    const auto plain_text = owe::text::DecodeUtf8("hour:\nminute:"_str.as_bytes());
    face->Populate(tab_text.as_slice());
    face->Populate(plain_text.as_slice());

    constexpr std::size_t peak_quads = 16;
    auto                  mesh       = Arc<owe::SceneMesh>::make();
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attributes {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord"_Str, .type = owe::VertexType::FLOAT2 },
        { .name = "a_Color"_Str, .type = owe::VertexType::FLOAT4 },
    };
    mesh->AddVertexArray(owe::SceneVertexArray(attributes, rstd::usize(peak_quads * 4)));
    mesh->AddIndexArray(owe::SceneIndexArray(rstd::usize(peak_quads * 6)));

    owe::text::TextLayouter layouter(face, mesh.clone(), {}, peak_quads);
    layouter.SetText("hour:\n\t\t\t\tminute:"_str);
    const float tab_width = layouter.TextWidth();
    EXPECT_EQ(mesh->GetIndexArray(rstd::usize()).RenderDataCount(), rstd::usize(72));

    layouter.SetText("hour:\nminute:"_str);
    EXPECT_FLOAT_EQ(layouter.TextWidth(), tab_width);
    EXPECT_EQ(mesh->GetIndexArray(rstd::usize()).RenderDataCount(), rstd::usize(72));
}

TEST(TextLayouter, LayoutOriginPreservesFontBaselineAcrossTextChanges) {
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace"_str);
    ASSERT_TRUE(font.bytes.is_some());
    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);
    face->Populate(owe::text::DecodeUtf8("0g"_str.as_bytes()).as_slice());

    const rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attributes {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord"_Str, .type = owe::VertexType::FLOAT2 },
        { .name = "a_Color"_Str, .type = owe::VertexType::FLOAT4 },
    };
    for (auto origin :
         { owe::text::TextMeshOrigin::Layout, owe::text::TextMeshOrigin::InkBounds }) {
        auto mesh = Arc<owe::SceneMesh>::make();
        mesh->AddVertexArray(owe::SceneVertexArray(attributes, rstd::usize(4)));
        mesh->AddIndexArray(owe::SceneIndexArray(rstd::usize(6)));
        owe::text::TextLayoutStyle style;
        style.mesh_origin = origin;
        owe::text::TextLayouter layouter(face, mesh.clone(), rstd::move(style), 1);
        for (const auto* text : { "0", "g" }) {
            layouter.SetText(rstd::cppstd::as_str(text).unwrap());
            const auto* glyph = face->Lookup(static_cast<std::uint32_t>(text[0]));
            ASSERT_NE(glyph, nullptr);
            const auto  metrics  = layouter.Metrics();
            const float baseline = metrics.text_height * 0.5f - face->Metrics().ascender;
            const float top      = baseline + glyph->bearing_y;
            const float center   = top - static_cast<float>(glyph->pixel_h) * 0.5f;
            EXPECT_FLOAT_EQ(metrics.source_center_y, center);
            const auto& vertices = mesh->GetVertexArray(rstd::usize());
            const auto  stride   = vertices.OneSize().to_primitive();
            const float offset   = origin == owe::text::TextMeshOrigin::InkBounds ? center : 0.0f;
            EXPECT_FLOAT_EQ(vertices.Data()[1], top - offset);
            EXPECT_FLOAT_EQ(vertices.Data()[2 * stride + 1],
                            top - static_cast<float>(glyph->pixel_h) - offset);
        }
    }
}

TEST(TextGeometry, DynamicEffectFollowsCurrentTextBounds) {
    const owe::text::TextGeometryPolicy policy {
        .frame_width  = 419.0f,
        .frame_height = 221.0f,
        .dynamic      = true,
        .has_effect   = true,
    };
    const owe::text::TextLayoutMetrics metrics {
        .text_width    = 607.0f,
        .text_height   = 157.0f,
        .source_width  = 563.0f,
        .source_height = 143.0f,
        .padding       = 32.0f,
    };

    const auto geometry = owe::text::ResolveTextGeometry(policy, metrics);

    EXPECT_FLOAT_EQ(geometry.rt_width, 671.0f);
    EXPECT_FLOAT_EQ(geometry.draw_width, 671.0f);
    EXPECT_FLOAT_EQ(geometry.uv_source_width, 671.0f);
    EXPECT_FLOAT_EQ(geometry.effect_frame_width, 671.0f);
    EXPECT_FLOAT_EQ(geometry.draw_height, 221.0f);
    EXPECT_FLOAT_EQ(geometry.uv_source_height, 221.0f);
    EXPECT_FLOAT_EQ(geometry.effect_frame_height, 221.0f);
}

TEST(FontFace, RetainsFontBlobAndGlyphAddressesAcrossGrowth) {
    owe::text::FontCache cache;
    owe::text::FontFace* face = nullptr;
    {
        auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace"_str);
        ASSERT_TRUE(font.bytes.is_some());
        face = cache.GetFace(font, 32);
        ASSERT_NE(face, nullptr);
    }
    face->Populate(owe::text::DecodeUtf8("A"_str.as_bytes()).as_slice());
    const auto* first = face->Lookup('A');
    ASSERT_NE(first, nullptr);
    const auto         width = first->pixel_w;
    Vec<std::uint32_t> codepoints;
    for (std::uint32_t cp = 32; cp < 512; ++cp) codepoints.emplace_back(cp);
    face->Populate(codepoints.as_slice());
    EXPECT_EQ(first, face->Lookup('A'));
    EXPECT_EQ(first->pixel_w, width);
}

TEST(TextDecode, PreservesMalformedByteReplacement) {
    array<u8, 5> bytes { u8('A'), u8(0xff), u8(0xc2), u8('B'), u8(0xe2) };
    auto         decoded = owe::text::DecodeUtf8(bytes.as_slice());
    ASSERT_EQ(decoded.len(), usize(5));
    EXPECT_EQ(decoded[usize()], uint32_t('A'));
    EXPECT_EQ(decoded[usize(1)], uint32_t(0xfffd));
    EXPECT_EQ(decoded[usize(2)], uint32_t(0xfffd));
    EXPECT_EQ(decoded[usize(3)], uint32_t('B'));
    EXPECT_EQ(decoded[usize(4)], uint32_t(0xfffd));
}
