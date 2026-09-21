#include <rstd/test/gtest.hpp>

#include <array>
#include <memory>
#include <vector>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.text;
import wescene.types;

TEST(FontFace, ImageRetainsLivePixelsAfterCacheDestruction) {
    using namespace rstd::literals;
    rstd::Option<rstd::sync::Arc<owe::Image>> image;
    std::vector<std::uint8_t>                 expected;
    {
        auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace");
        ASSERT_NE(font.bytes, nullptr);
        owe::text::FontCache cache;
        auto*                face = cache.GetFace(font, 64);
        ASSERT_NE(face, nullptr);
        face->Populate(owe::text::DecodeUtf8("A"));
        image = owe::text::BuildAtlasImage(*face, "atlas"_str);
        ASSERT_TRUE(image.is_some());
        face->Populate(owe::text::DecodeUtf8("B"));
        auto pixels = face->AtlasPixels();
        expected.assign(pixels.begin(), pixels.end());
        EXPECT_EQ((*image)->content->slots[0].mipmaps[0].data.get(), pixels.data());
    }
    const auto& mip = (*image)->content->slots[0].mipmaps[0];
    EXPECT_EQ(std::vector<std::uint8_t>(mip.data.get(), mip.data.get() + expected.size()),
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
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace");
    ASSERT_NE(font.bytes, nullptr);

    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);

    const std::array<std::uint32_t, 1> codepoints { '\t' };
    face->Populate(codepoints);

    const auto* tab = face->Lookup('\t');
    ASSERT_NE(tab, nullptr);
    EXPECT_EQ(tab->pixel_w, 0u);
    EXPECT_EQ(tab->pixel_h, 0u);
    EXPECT_EQ(tab->advance_x, 0.0f);
}

TEST(TextLayouter, TabIsIgnoredWithoutAControlQuad) {
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace");
    ASSERT_NE(font.bytes, nullptr);

    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);
    const auto tab_text   = owe::text::DecodeUtf8("hour:\n\t\t\t\tminute:");
    const auto plain_text = owe::text::DecodeUtf8("hour:\nminute:");
    face->Populate(tab_text);
    face->Populate(plain_text);

    constexpr std::size_t peak_quads = 16;
    auto                  mesh       = std::make_shared<owe::SceneMesh>();
    std::vector<owe::SceneVertexArray::SceneVertexAttribute> attributes {
        { .name = "a_Position", .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord", .type = owe::VertexType::FLOAT2 },
        { .name = "a_Color", .type = owe::VertexType::FLOAT4 },
    };
    mesh->AddVertexArray(owe::SceneVertexArray(attributes, rstd::usize(peak_quads * 4)));
    mesh->AddIndexArray(owe::SceneIndexArray(rstd::usize(peak_quads * 6)));

    owe::text::TextLayouter layouter(face, mesh, {}, peak_quads);
    layouter.SetText("hour:\n\t\t\t\tminute:");
    const float tab_width = layouter.TextWidth();
    EXPECT_EQ(mesh->GetIndexArray(rstd::usize()).RenderDataCount(), rstd::usize(72));

    layouter.SetText("hour:\nminute:");
    EXPECT_FLOAT_EQ(layouter.TextWidth(), tab_width);
    EXPECT_EQ(mesh->GetIndexArray(rstd::usize()).RenderDataCount(), rstd::usize(72));
}

TEST(TextLayouter, LayoutOriginPreservesFontBaselineAcrossTextChanges) {
    auto font = owe::text::FontCache::ResolveSystemFont("systemfont_monospace");
    ASSERT_NE(font.bytes, nullptr);
    owe::text::FontCache cache;
    auto*                face = cache.GetFace(font, 64);
    ASSERT_NE(face, nullptr);
    face->Populate(owe::text::DecodeUtf8("0g"));

    const std::vector<owe::SceneVertexArray::SceneVertexAttribute> attributes {
        { .name = "a_Position", .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord", .type = owe::VertexType::FLOAT2 },
        { .name = "a_Color", .type = owe::VertexType::FLOAT4 },
    };
    for (auto origin :
         { owe::text::TextMeshOrigin::Layout, owe::text::TextMeshOrigin::InkBounds }) {
        auto mesh = std::make_shared<owe::SceneMesh>();
        mesh->AddVertexArray(owe::SceneVertexArray(attributes, rstd::usize(4)));
        mesh->AddIndexArray(owe::SceneIndexArray(rstd::usize(6)));
        owe::text::TextLayoutStyle style;
        style.mesh_origin = origin;
        owe::text::TextLayouter layouter(face, mesh, style, 1);
        for (const auto* text : { "0", "g" }) {
            layouter.SetText(text);
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
