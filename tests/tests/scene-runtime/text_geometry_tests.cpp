#include <rstd/test/gtest.hpp>

#include <array>
#include <memory>
#include <vector>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.text;
import wescene.types;

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
