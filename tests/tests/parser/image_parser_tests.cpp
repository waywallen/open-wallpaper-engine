#include <rstd/test/gtest.hpp>

#include <filesystem>
#include <fstream>

import rstd;
import rstd.cppstd;
import wescene.fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

class TrackingImageParser {
public:
    auto Parse(ref<str> name) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        auto current  = m_active.fetch_add(1) + 1;
        auto observed = m_peak.load();
        while (current > observed && ! m_peak.compare_exchange_weak(observed, current)) {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto image          = Arc<owe::Image>::make();
        image->content->key = rstd::cppstd::to_string(name);
        m_active.fetch_sub(1);
        return Ok(rstd::move(image));
    }

    auto ParseHeader(ref<str>) const -> Result<owe::ImageHeader, owe::ImageParseError> {
        return Ok(owe::ImageHeader {});
    }

    auto ParseMany(slice<String> names) const
        -> Vec<Result<Arc<owe::Image>, owe::ImageParseError>> {
        auto parser = dyn<owe::IImageParser>::from_ref(*this);
        return owe::ParseImages(parser.as_ref(), names);
    }

    int peak() const { return m_peak.load(); }

private:
    mutable std::atomic<int> m_active { 0 };
    mutable std::atomic<int> m_peak { 0 };
};

class MixedImageParser {
public:
    auto Parse(ref<str> name) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        if (rstd::cppstd::as_string_view(name) == "bad") {
            return Err(owe::ImageParseError {
                .kind    = owe::ImageParseErrorKind::DecodeFailed,
                .message = String::make("bad image"_str),
            });
        }
        auto image          = Arc<owe::Image>::make();
        image->content->key = rstd::cppstd::to_string(name);
        return Ok(rstd::move(image));
    }

    auto ParseHeader(ref<str>) const -> Result<owe::ImageHeader, owe::ImageParseError> {
        return Ok(owe::ImageHeader {});
    }

    auto ParseMany(slice<String> names) const
        -> Vec<Result<Arc<owe::Image>, owe::ImageParseError>> {
        auto parser = dyn<owe::IImageParser>::from_ref(*this);
        return owe::ParseImages(parser.as_ref(), names);
    }
};

void WriteU32(std::ofstream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

class ConditionalTextureTest {
protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("owe-conditional-texture-" + std::to_string(rstd::process::id().to_primitive()));
    owe::fs::VFS vfs;

    void SetUp() noexcept {
        std::filesystem::create_directories(root / "materials");
        auto physical = owe::fs::make_physical_fs(owe::fs::ToPath(root.string()));
        ASSERT_TRUE(physical.is_ok());
        ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(physical).unwrap_unchecked()).is_ok());
    }
    void TearDown() noexcept { std::filesystem::remove_all(root); }

    void WriteTexture(bool sprite, std::uint32_t conditions) {
        std::ofstream output(root / "materials" / "conditional.tex", std::ios::binary);
        output.write("TEXV0005", 9);
        output.write("TEXI0001", 9);
        for (auto value : { 0u, sprite ? 4u : 0u, 2u, 2u, 2u, 2u, 0u }) WriteU32(output, value);
        output.write("TEXB0004", 9);
        WriteU32(output, 1);
        WriteU32(output, 0xffffffffu);
        WriteU32(output, conditions);
        for (std::uint32_t index = 0; index < conditions; ++index) {
            for (auto value : { index + 1, 1u, 0u }) WriteU32(output, value);
            const char json[] = R"({"condition":"newproperty"})";
            output.write(json, sizeof(json));
        }
        WriteU32(output, 2);
        for (auto extent : { 2u, 1u }) {
            for (auto value : { extent, extent, 0u, 0u, extent * extent * 4 })
                WriteU32(output, value);
            for (std::uint32_t pixel = 0; pixel < extent * extent; ++pixel)
                WriteU32(output, 0xff332211u);
            if (conditions == 0) continue;
            WriteU32(output, 2);
            WriteU32(output, 0);
            WriteU32(output, conditions);
            for (std::uint32_t index = 0; index < conditions; ++index) {
                for (auto value : { 0u, index + 1, 0u, 0u, 1u, 1u, 4u, 4u, 0xffffffffu })
                    WriteU32(output, value);
            }
        }
        if (! sprite) return;
        output.write("TEXS0003", 9);
        for (auto value : { 1u, 2u, 2u, 0u }) WriteU32(output, value);
        const float frame[] { 0.1f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f };
        output.write(reinterpret_cast<const char*>(frame), sizeof(frame));
    }
};

TEST_F(ConditionalTextureTest, BaseMipmapsRemainAlignedWithMultipleConditions) {
    for (auto conditions : { 0u, 1u, 2u }) {
        WriteTexture(false, conditions);
        owe::TexImageParser parser(&vfs);
        auto                parsed = parser.Parse("conditional"_str);
        ASSERT_TRUE(parsed.is_ok());
        auto image = rstd::move(parsed).unwrap_unchecked();
        ASSERT_EQ(image->content->slots.size(), 1u);
        ASSERT_EQ(image->content->slots[0].mipmaps.size(), 2u);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& mip = image->content->slots[0].mipmaps[index];
            EXPECT_EQ(mip.width, index == 0 ? 2 : 1);
            EXPECT_EQ(mip.height, mip.width);
            EXPECT_EQ(mip.size, isize(mip.width * mip.height * 4));
            ASSERT_NE(mip.data, nullptr);
            EXPECT_EQ(mip.data.get()[0], 0x11);
        }
        auto header = parser.ParseHeader("conditional"_str);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->width, image->header.width);
        EXPECT_EQ(header->mipmap_larger, image->header.mipmap_larger);
    }
}

TEST_F(ConditionalTextureTest, SpriteHeaderSkipsConditionalPatches) {
    WriteTexture(true, 1);
    owe::TexImageParser parser(&vfs);
    auto                header = parser.ParseHeader("conditional"_str);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->spriteAnim.numFrames(), usize(1));
    EXPECT_EQ(header->extraHeader.at("texs").val, 3);
    EXPECT_TRUE(parser.Parse("conditional"_str).is_ok());
}

TEST_F(ConditionalTextureTest, FullLoadReusesPreparedHeaderAndOpenSource) {
    WriteTexture(true, 1);
    owe::TexImageParser parser(&vfs);
    auto                header = parser.ParseHeader("conditional"_str);
    ASSERT_TRUE(header.is_ok());
    std::filesystem::rename(root / "materials" / "conditional.tex", root / "original.tex");
    {
        std::ofstream replacement(root / "materials" / "conditional.tex");
    }
    auto parsed = parser.Parse("conditional"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto image = rstd::move(parsed).unwrap_unchecked();
    EXPECT_EQ(image->header.spriteAnim.numFrames(), usize(1));
    EXPECT_EQ(image->content->slots[0].mipmaps.size(), 2u);
}

TEST_F(ConditionalTextureTest, ParserRetainsMountedSourcesAfterVfsDestruction) {
    WriteTexture(false, 0);
    Option<owe::TexImageParser> parser;
    {
        owe::fs::VFS source;
        auto         mount = owe::fs::make_physical_fs(owe::fs::ToPath(root.string()));
        ASSERT_TRUE(mount.is_ok());
        ASSERT_TRUE(source.mount("/assets"_str, rstd::move(mount).unwrap_unchecked()).is_ok());
        parser = Some(owe::TexImageParser(&source));
    }
    EXPECT_TRUE(parser->Parse("conditional"_str).is_ok());
}

TEST_F(ConditionalTextureTest, RejectsTruncatedConditionsAndPatches) {
    for (auto length : { 63u, 70u, 83u, 260u }) {
        WriteTexture(false, 1);
        std::filesystem::resize_file(root / "materials" / "conditional.tex", length);
        owe::TexImageParser parser(&vfs);
        auto                parsed = parser.Parse("conditional"_str);
        ASSERT_TRUE(parsed.is_err()) << length;
        EXPECT_EQ(parsed.unwrap_err_unchecked().kind, owe::ImageParseErrorKind::InvalidData);
        if (length < 100) EXPECT_TRUE(parser.ParseHeader("conditional"_str).is_err());
    }
}

TEST(ImageParser, BatchPreservesOrderAndBoundsConcurrency) {
    TrackingImageParser parser;
    Vec<String>         names;
    for (const char* name : { "0", "1", "2", "3", "4", "5", "6", "7" })
        names.push(String::make(rstd::cppstd::as_str(name).unwrap()));

    auto image_parser = dyn<owe::IImageParser>::from_ref(parser);
    auto images       = owe::ParseImages(image_parser.as_ref(), names.as_slice());

    ASSERT_EQ(images.len(), names.len());
    for (usize index {}; index < names.len(); ++index) {
        ASSERT_TRUE(images[index].is_ok());
        auto image = rstd::move(images[index]).unwrap_unchecked();
        EXPECT_EQ(image->content->key, rstd::cppstd::to_string(names[index].as_str()));
    }
    EXPECT_GT(parser.peak(), 1);
    EXPECT_LE(parser.peak(), 4);
}

TEST(ImageParser, TextureHeaderExposesFourthPackedComponent) {
    auto root = std::filesystem::temp_directory_path() /
                ("owe-image-parser-" + std::to_string(rstd::process::id().to_primitive()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "materials");
    {
        std::ofstream output(root / "materials" / "mask.tex", std::ios::binary);
        output.write("TEXV0005", 9);
        output.write("TEXI0001", 9);
        WriteU32(output, 4);
        WriteU32(output, 1u << 23);
        WriteU32(output, 1);
        WriteU32(output, 1);
        WriteU32(output, 1);
        WriteU32(output, 1);
        WriteU32(output, 0);
        output.write("TEXB0001", 9);
        WriteU32(output, 1);
        WriteU32(output, 1);
        WriteU32(output, 1);
        WriteU32(output, 1);
    }

    auto physical = owe::fs::make_physical_fs(owe::fs::ToPath(root.string()));
    ASSERT_TRUE(physical.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(physical).unwrap_unchecked()).is_ok());

    owe::TexImageParser parser(&vfs);
    auto                parsed = parser.ParseHeader("mask"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto header = rstd::move(parsed).unwrap_unchecked();
    EXPECT_EQ(header.extraHeader.at("compo1").val, 0);
    EXPECT_EQ(header.extraHeader.at("compo2").val, 0);
    EXPECT_EQ(header.extraHeader.at("compo3").val, 0);
    EXPECT_EQ(header.extraHeader.at("compo4").val, 1);

    std::filesystem::remove_all(root);
}

TEST(ImageParser, SceneBatchPreservesRuntimeParserAndErrorPositions) {
    owe::Scene scene;
    scene.SetImageParser(rstd::sync::Arc<dyn<owe::IImageParser>>::make(MixedImageParser {}));
    auto runtime          = Arc<owe::Image>::make();
    runtime->content->key = "runtime-value";
    scene.RegisterRuntimeImage(String::make("runtime"_str), runtime.clone());

    Vec<String> names;
    for (const char* name : { "first", "runtime", "bad", "last" }) {
        names.push(String::make(rstd::cppstd::as_str(name).unwrap()));
    }
    auto images = scene.ParseImages(names.as_slice());

    ASSERT_EQ(images.len(), names.len());
    ASSERT_TRUE(images[usize(0)].is_ok());
    EXPECT_EQ((*images[usize(0)])->content->key, "first");
    ASSERT_TRUE(images[usize(1)].is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(*images[usize(1)], runtime));
    ASSERT_TRUE(images[usize(2)].is_err());
    EXPECT_EQ(images[usize(2)].unwrap_err_unchecked().kind, owe::ImageParseErrorKind::DecodeFailed);
    ASSERT_TRUE(images[usize(3)].is_ok());
    EXPECT_EQ((*images[usize(3)])->content->key, "last");
}

TEST(ImageParser, CapturedSourceRetainsOldRuntimeImageAndParser) {
    Option<Arc<owe::SceneImageSource>> source;
    auto                               original = Arc<owe::Image>::make();
    original->content->key                      = "original";
    {
        owe::Scene scene;
        scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(MixedImageParser {}));
        scene.RegisterRuntimeImage(String::make("runtime"_str), original.clone());
        source = Some(scene.CaptureImageSource());
        scene.RegisterRuntimeImage(String::make("runtime"_str), Arc<owe::Image>::make());
    }
    auto retained = (*source)->Parse("runtime"_str);
    ASSERT_TRUE(retained.is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(retained.unwrap_unchecked(), original));
    EXPECT_TRUE((*source)->Parse("bad"_str).is_err());
    EXPECT_TRUE((*source)->Parse("first"_str).is_ok());
}

} // namespace
