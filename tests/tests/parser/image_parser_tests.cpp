#include <rstd/test/gtest.hpp>

#include <filesystem>
#include <fstream>
#include <lz4.h>

import rstd;
import vrento;
import rstd.cppstd;
import wescene.fs;
import wescene.json;
import wescene.pkg.parse;
import wescene.pkg_asset_version;
import wescene.scene;
import wescene.types;
import wescene.stb_image;
import wescene.texture_memory;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

TEST(BitFlags, PreservesWidthAndHighToLowText) {
    enum class Flag : rstd::uint8_t
    {
        First = 0,
        Last  = 7
    };
    owe::BitFlags<Flag> flags(0x181);
    EXPECT_EQ(flags.size(), 8u);
    EXPECT_EQ(flags.count(), 2u);
    EXPECT_EQ(flags.to_string(), "10000001"_str);
    EXPECT_TRUE(flags[Flag::First]);
    EXPECT_TRUE(flags[Flag::Last]);
    EXPECT_FALSE(flags.all());
    flags.reset(Flag::Last).set(Flag::First, false);
    EXPECT_TRUE(flags.none());
    EXPECT_FALSE(flags.any());
    flags = owe::BitFlags<Flag>(0x1ff);
    EXPECT_TRUE(flags.all());
    EXPECT_EQ(flags.count(), 8u);
    flags.reset();
    EXPECT_EQ(flags.to_string(), "00000000"_str);
}

TEST(BitFlags, SupportsSignedEnumsAndFull64BitWidth) {
    enum class Signed : rstd::int32_t
    {
        Last = 31
    };
    owe::BitFlags<Signed> signed_flags;
    signed_flags.set(Signed::Last);
    EXPECT_TRUE(signed_flags[31]);
    EXPECT_EQ(signed_flags.count(), 1u);
    EXPECT_EQ(signed_flags.to_string(), "10000000000000000000000000000000"_str);
    enum class Wide : rstd::uint64_t
    {
        First = 0,
        Last  = 63
    };
    owe::BitFlags<Wide> wide(u64::MAX.to_primitive());
    EXPECT_TRUE(wide.all());
    EXPECT_EQ(wide.count(), 64u);
    wide.reset().set(Wide::Last).set(Wide::First);
    EXPECT_TRUE(wide[63]);
    EXPECT_TRUE(wide[0]);
    EXPECT_EQ(wide.count(), 2u);
    EXPECT_EQ(wide.to_string().len(), usize(64));
}

TEST(ImageHeader, CloneOwnsMetadataIndependently) {
    owe::ImageHeader source;
    source.width     = 37;
    source.mapHeight = 19;
    source.isSprite  = true;
    source.extraHeader.insert("texv"_Str, owe::ImageExtra { .val = 3 });
    auto copy = source.clone();
    copy.extraHeader.insert("texv"_Str, owe::ImageExtra { .val = 4 });
    copy.extraHeader.insert("compo1"_Str, owe::ImageExtra { .val = 1 });
    EXPECT_EQ(source.extraHeader.get("texv"_str).unwrap()->val, 3);
    EXPECT_FALSE(source.extraHeader.contains_key("compo1"_str));
    EXPECT_EQ(copy.width, 37);
    EXPECT_EQ(copy.mapHeight, 19);
    EXPECT_TRUE(copy.isSprite);
    EXPECT_EQ(owe::ToString(owe::ImageType::PNG), "PNG"_str);
    EXPECT_EQ(owe::ToString(owe::TextureFormat::BC3), "BC3"_str);
    EXPECT_TRUE(owe::ToString(static_cast<owe::ImageType>(999)).is_empty());
    EXPECT_TRUE(owe::ToString(static_cast<owe::TextureFormat>(999)).is_empty());
}

TEST(TextureMemory, VirtualMemoryReleasesOnAnotherThread) {
    auto tracker =
        rstd::alloc::SharedTrackingAllocator<rstd::alloc::VirtualMemoryAllocator>::make();
    auto observer = tracker.downgrade();
    auto bytes    = Vec<u8, rstd::alloc::SharedAllocator>::new_in(
        rstd::alloc::SharedAllocator::make(tracker.clone()));
    ASSERT_TRUE(bytes.try_reserve_exact(usize(12345)).is_ok());
    EXPECT_GT(tracker.upstream_statistics().mapped_bytes, usize());
    auto worker = rstd::thread::spawn([bytes = rstd::move(bytes)] {
    });
    ASSERT_TRUE(worker.is_ok());
    EXPECT_TRUE(rstd::move(worker).unwrap().join().is_ok());
    EXPECT_EQ(tracker.snapshot().live_bytes, u64());
    EXPECT_EQ(tracker.upstream_statistics().mapped_bytes, usize());
    EXPECT_FALSE(observer.expired());
}

TEST(TextureMemory, AllocationKeepsBackendAliveAfterRootDies) {
    auto tracker =
        Some(rstd::alloc::SharedTrackingAllocator<rstd::alloc::VirtualMemoryAllocator>::make());
    auto observer = tracker->downgrade();
    auto bytes    = Vec<u8, rstd::alloc::SharedAllocator>::new_in(
        rstd::alloc::SharedAllocator::make(tracker->clone()));
    ASSERT_TRUE(bytes.try_reserve_exact(usize(12345)).is_ok());
    tracker = None();
    EXPECT_FALSE(observer.expired());
    auto worker = rstd::thread::spawn([bytes = rstd::move(bytes)] {
                  }).unwrap();
    EXPECT_TRUE(rstd::move(worker).join().is_ok());
    EXPECT_TRUE(observer.expired());
}

namespace
{

class TrackingImageParser {
public:
    auto Retention(ref<str>) const -> owe::ImageRetention { return owe::ImageRetention::Source; }
    auto Describe(ref<str>) const -> Result<owe::ImageDescription, owe::ImageParseError> {
        return Ok(owe::ImageDescription {});
    }
    auto Parse(ref<str> name) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        auto current  = m_active.fetch_add(1) + 1;
        auto observed = m_peak.load();
        while (current > observed && ! m_peak.compare_exchange_weak(observed, current)) {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto image          = Arc<owe::Image>::make();
        image->content->key = rstd::into(name);
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
    auto Retention(ref<str>) const -> owe::ImageRetention { return owe::ImageRetention::Source; }
    auto Describe(ref<str>) const -> Result<owe::ImageDescription, owe::ImageParseError> {
        return Ok(owe::ImageDescription {});
    }
    auto Parse(ref<str> name) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        if (rstd::cppstd::as_string_view(name) == "bad") {
            return Err(owe::ImageParseError {
                .kind    = owe::ImageParseErrorKind::DecodeFailed,
                .message = String::make("bad image"_str),
            });
        }
        auto image          = Arc<owe::Image>::make();
        image->content->key = rstd::into(name);
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
        auto physical =
            owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(root.string()).unwrap()));
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

    void WritePayload(owe::ImageType type, bool compressed, slice<u8> payload, int width = 1,
                      int height = 1, unsigned format = 4) {
        Vec<u8> stored;
        if (compressed) {
            stored.resize(usize(LZ4_compressBound(int(payload.len().to_primitive()))), u8());
            auto size = LZ4_compress_default(reinterpret_cast<const char*>(payload.as_raw_ptr()),
                                             reinterpret_cast<char*>(stored.data()),
                                             int(payload.len().to_primitive()),
                                             int(stored.len().to_primitive()));
            ASSERT_GT(size, 0);
            stored.truncate(usize(size));
        } else {
            stored = Vec<u8>::from(payload);
        }
        std::ofstream output(root / "materials" / "payload.tex", std::ios::binary);
        output.write("TEXV0005", 9);
        output.write("TEXI0001", 9);
        for (auto value : array<unsigned, 7> { format,
                                               0u,
                                               unsigned(width),
                                               unsigned(height),
                                               unsigned(width),
                                               unsigned(height),
                                               0u })
            WriteU32(output, value);
        output.write("TEXB0003", 9);
        WriteU32(output, 1);
        WriteU32(output, unsigned(type));
        WriteU32(output, 1);
        WriteU32(output, width);
        WriteU32(output, height);
        WriteU32(output, compressed ? 1 : 0);
        WriteU32(output, payload.len().to_primitive());
        WriteU32(output, stored.len().to_primitive());
        output.write(reinterpret_cast<const char*>(stored.data()), stored.len().to_primitive());
    }
};

constexpr unsigned char RedPng[] {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x08,
    0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99,
    0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

TEST(TextureMemory, DecoderPixelsOutliveDecoderAndKeepTheirAllocator) {
    auto memory = owe::TextureMemorySession::VirtualMemory();
    auto pixels = [&] {
        owe::stb::Decoder decoder(memory.options().bytes);
        return decoder.LoadMemory(RedPng, sizeof(RedPng)).unwrap();
    }();
    EXPECT_EQ(pixels.info.width, 1);
    EXPECT_EQ(pixels.data()[0], 255);
    EXPECT_GT(memory.statistics().logical.live_bytes, u64());
    auto released = rstd::thread::spawn([pixels = rstd::move(pixels)]() mutable {
                        pixels.reset();
                    }).unwrap();
    EXPECT_TRUE(rstd::move(released).join().is_ok());
    EXPECT_EQ(memory.statistics().logical.live_bytes, u64());
    EXPECT_EQ(memory.statistics().virtual_memory->mapped_bytes, usize());
}

TEST(TextureMemory, ConcurrentDecodersDoNotShareAllocatorDomains) {
    auto a     = owe::TextureMemorySession::System();
    auto b     = owe::TextureMemorySession::VirtualMemory();
    auto start = [](owe::TextureMemoryOptions options) {
        return rstd::thread::spawn([options = rstd::move(options)] {
                   owe::stb::Decoder decoder(options.bytes.clone());
                   return decoder.LoadMemory(RedPng, sizeof(RedPng)).unwrap();
               })
            .unwrap();
    };
    auto first  = start(a.options());
    auto second = start(b.options());
    auto p      = rstd::move(first).join().unwrap();
    auto q      = rstd::move(second).join().unwrap();
    EXPECT_GT(a.statistics().logical.live_bytes, u64());
    EXPECT_GT(b.statistics().logical.live_bytes, u64());
    p.reset();
    EXPECT_EQ(a.statistics().logical.live_bytes, u64());
    EXPECT_GT(b.statistics().logical.live_bytes, u64());
    q.reset();
    EXPECT_EQ(b.statistics().virtual_memory->mapped_bytes, usize());
}

TEST(TextureMemory, DecoderAllocationFailuresReleaseAllTemporaryStorage) {
    for (auto limit :
         array<usize, 5> { usize(), usize(64), usize(512), usize(4096), usize(16384) }) {
        auto budget  = rstd::alloc::BudgetAllocator<>::try_make(limit).unwrap();
        auto tracker = rstd::alloc::SharedTrackingAllocator<rstd::alloc::BudgetAllocator<>>::make(
            rstd::move(budget));
        owe::stb::Decoder decoder(rstd::alloc::SharedAllocator::make(tracker.clone()));
        auto              result = decoder.LoadMemory(RedPng, sizeof(RedPng));
        if (result.is_ok())
            result->reset();
        else
            EXPECT_EQ(result.unwrap_err(), owe::stb::Error::OutOfMemory);
        EXPECT_EQ(tracker.snapshot().live_bytes, u64());
    }
}

TEST(TextureMemory, EmptyDecoderInputDoesNotAllocate) {
    auto              memory = owe::TextureMemorySession::VirtualMemory();
    owe::stb::Decoder decoder(memory.options().bytes);
    EXPECT_EQ(decoder.LoadMemory(nullptr, 0).unwrap_err(), owe::stb::Error::InvalidData);
    EXPECT_EQ(decoder.InfoMemory(nullptr, 1).unwrap_err(), owe::stb::Error::InvalidData);
    EXPECT_EQ(decoder.InfoMemory(RedPng, -1).unwrap_err(), owe::stb::Error::InvalidData);
    EXPECT_EQ(memory.statistics().logical.allocations, u64());
}

TEST(TextureMemory, DecoderFileFormatsPreservePixelsAndReleaseTheirMappings) {
    auto              memory = owe::TextureMemorySession::VirtualMemory();
    owe::stb::Decoder decoder(memory.options().bytes);
    for (auto file : array<ref<str>, 5> {
             "red.jpg"_str, "red.bmp"_str, "red.tga"_str, "red.gif"_str, "red16.png"_str }) {
        auto name = rstd::format("{}/image-memory/{}", WAYWALLEN_FIXTURE_DIR, file);
        auto path = rstd::ffi::CString::make(rstd::move(name).into_bytes()).unwrap();
        auto info = decoder.InfoFile(path.as_ref().as_ptr()).unwrap();
        EXPECT_EQ(info.width, 16);
        EXPECT_EQ(info.height, 16);
        auto pixels = decoder.LoadFile(path.as_ref().as_ptr()).unwrap();
        EXPECT_GT(pixels.data()[0], 240);
        EXPECT_LT(pixels.data()[1], 8);
        EXPECT_LT(pixels.data()[2], 8);
        EXPECT_EQ(pixels.data()[3], 255);
        pixels.reset();
        EXPECT_EQ(memory.statistics().virtual_memory->mapped_bytes, usize());
    }
}

struct TextureReadGate {
    struct Fields {
        bool  armed {}, entered {}, released {}, fail {};
        usize reads {}, fail_on_read {};
    };
    Vec<u8>                   bytes;
    rstd::sync::Mutex<Fields> fields { Fields {} };
    rstd::sync::Condvar       changed;

    explicit TextureReadGate(Vec<u8> data): bytes(rstd::move(data)) {}

    void Arm(usize fail_on_read = usize()) {
        auto state = fields.lock().unwrap();
        *state     = Fields { .armed = true, .fail_on_read = fail_on_read };
    }
    bool WaitUntilEntered() {
        auto state = fields.lock().unwrap();
        return ! changed
                     .wait_timeout_while(state,
                                         rstd::time::Duration::from_secs(u64(3)),
                                         [](const Fields& value) {
                                             return ! value.entered;
                                         })
                     .timed_out();
    }
    void Release(bool fail = false) {
        auto state      = fields.lock().unwrap();
        state->fail     = fail;
        state->released = true;
        changed.notify_all();
    }
};

struct ControlledTextureSource {
    Arc<TextureReadGate> gate;

    auto read_at(mut_ref<u8[]> buffer, u64 offset) const -> rstd::io::Result<usize> {
        {
            auto state = gate->fields.lock().unwrap();
            if (state->armed) {
                ++state->reads;
                state->entered = true;
                gate->changed.notify_all();
                gate->changed.wait_while(state, [](const auto& value) {
                    return ! value.released;
                });
                if (state->fail || state->reads == state->fail_on_read)
                    return Err(rstd::io::error::Error::from_kind(
                        rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::Other }));
            }
        }
        if (offset >= u64(gate->bytes.len().to_primitive())) return Ok(usize());
        auto position = usize(offset.to_primitive());
        auto count    = rstd::cmp::min(buffer.len(), gate->bytes.len() - position);
        rstd::mem::memcpy(buffer.as_raw_ptr(), gate->bytes.data() + position.to_primitive(), count);
        return Ok(count);
    }
};

struct ControlledTextureMount {
    rstd::io::ReadRange source;

    auto open_read(owe::fs::Path) const -> rstd::io::Result<rstd::io::ReadRange> {
        return Ok(source.clone());
    }
    auto open_write(owe::fs::Path, owe::fs::WriteOptions) const
        -> rstd::io::Result<owe::fs::WriteSeekHandle> {
        return Err(rstd::io::error::Error::from_kind(
            rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::Unsupported }));
    }
    auto metadata(owe::fs::Path) const -> rstd::io::Result<owe::fs::FileMetadata> {
        return Ok(owe::fs::FileMetadata { .len = source.len(), .is_file = true, .readonly = true });
    }
};

} // namespace

template<>
struct rstd::Impl<rstd::io::ReadAt, ControlledTextureSource>
    : rstd::ImplBase<ControlledTextureSource> {
    auto read_buf_at(rstd::io::BorrowedCursor buffer, u64 offset) const -> rstd::io::Result<empty> {
        auto count = this->self().read_at(buffer.ensure_init(), offset);
        if (count.is_err()) return Err(count.unwrap_err());
        (void)buffer.advance(*count);
        return Ok(empty {});
    }
    auto read_at(mut_ref<u8[]> buffer, u64 offset) const -> rstd::io::Result<usize> {
        return this->self().read_at(buffer, offset);
    }
};

template<>
struct rstd::Impl<owe::fs::MountFs, ControlledTextureMount>
    : rstd::ImplBase<ControlledTextureMount> {
    auto open_read(owe::fs::Path path) const -> rstd::io::Result<rstd::io::ReadRange> {
        return this->self().open_read(path);
    }
    auto open_write(owe::fs::Path path, owe::fs::WriteOptions options) const
        -> rstd::io::Result<owe::fs::WriteSeekHandle> {
        return this->self().open_write(path, options);
    }
    auto metadata(owe::fs::Path path) const -> rstd::io::Result<owe::fs::FileMetadata> {
        return this->self().metadata(path);
    }
};

namespace
{

auto MountControlledTexture(owe::fs::VFS& vfs, const std::filesystem::path& path)
    -> Arc<TextureReadGate> {
    auto filepath = rstd::path::PathBuf::from(rstd::cppstd::as_str(path.string()).unwrap());
    auto bytes    = rstd::fs::read(filepath.as_path()).unwrap();
    auto gate     = Arc<TextureReadGate>::make(rstd::move(bytes));
    auto source   = rstd::io::ReadRange::make(
                        rstd::io::SharedReadAt::make(ControlledTextureSource { gate.clone() }),
                        u64(),
                        u64(gate->bytes.len().to_primitive()))
                        .unwrap();
    vfs.mount("/assets"_str,
              owe::fs::MountHandle::make(ControlledTextureMount { rstd::move(source) }))
        .unwrap();
    return gate;
}

bool WaitForRegisteredReads(const owe::TexImageParser& parser, u64 count) {
    auto start = rstd::time::Instant::now();
    while (parser.Statistics().in_flight_reuses < count) {
        if (start.elapsed() >= rstd::time::Duration::from_secs(u64(3))) return false;
        rstd::thread::yield_now();
    }
    return true;
}

TEST_F(ConditionalTextureTest, PixelStatisticsFollowStorageNotImageWrappersOrArcClones) {
    WriteTexture(false, 0);
    auto [tx, rx] = rstd::sync::mpmc::channel<rstd::sync::Weak<vrento::Image>>();
    owe::TexImageParser parser(&vfs,
                               Some(owe::ImageReadyCallback::make([tx](Arc<vrento::Image> image) {
                                   tx.send(image.downgrade()).unwrap();
                               })));
    ASSERT_TRUE(parser.Describe("conditional"_str).is_ok());
    EXPECT_EQ(parser.Statistics().decode_attempts, u64());
    auto image   = parser.Parse("conditional"_str).unwrap();
    auto old     = image.downgrade();
    auto content = image->content.clone();
    EXPECT_TRUE(rx.try_recv().is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(image, parser.Parse("conditional"_str).unwrap()));
    auto stats = parser.Statistics();
    EXPECT_EQ(stats.current_count, u64(1));
    EXPECT_EQ(stats.current_bytes, u64(20));
    EXPECT_EQ(stats.total_bytes, u64(20));
    image.reset();
    EXPECT_TRUE(old.expired());
    auto reused = parser.Parse("conditional"_str).unwrap();
    EXPECT_TRUE(Arc<vrento::Image>::ptr_eq(content, reused->content));
    reused.reset();
    EXPECT_EQ(parser.Statistics().current_count, u64(1));
    EXPECT_TRUE(rx.try_recv().is_err());
    content.reset();
    stats = parser.Statistics();
    EXPECT_EQ(stats.current_count, u64());
    EXPECT_EQ(stats.current_bytes, u64());
    EXPECT_EQ(stats.peak_count, u64(1));
    EXPECT_EQ(stats.peak_bytes, u64(20));
    EXPECT_EQ(stats.decode_total, u64(1));
    EXPECT_EQ(stats.reuse_total, u64(2));
    EXPECT_TRUE(parser.Describe("conditional"_str).is_ok());
    EXPECT_TRUE(parser.Parse("conditional"_str).is_ok());
    EXPECT_EQ(parser.Statistics().decode_total, u64(2));
    EXPECT_EQ(parser.Statistics().total_bytes, u64(40));
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
    EXPECT_TRUE(rx.try_recv().is_ok());
}

TEST_F(ConditionalTextureTest, RegisteredReadsShareResultWhenConsumersImmediatelyReleaseIt) {
    WritePayload(owe::ImageType::UNKNOWN, false, "0123456789abcdef"_str.as_bytes(), 4, 4);
    auto                gate = MountControlledTexture(vfs, root / "materials" / "payload.tex");
    owe::TexImageParser parser(&vfs);
    ASSERT_TRUE(parser.Describe("payload"_str).is_ok());
    gate->Arm();
    auto read = [&] {
        auto image = parser.Parse("payload"_str).unwrap();
        return image->content.downgrade();
    };
    auto producer = rstd::thread::spawn(read).unwrap();
    auto entered  = gate->WaitUntilEntered();
    Vec<rstd::thread::JoinHandle<rstd::sync::Weak<vrento::Image>>> consumers;
    for (usize index {}; index < usize(8); ++index)
        consumers.push(rstd::thread::spawn(read).unwrap());
    auto registered = WaitForRegisteredReads(parser, u64(8));
    gate->Release();
    auto                                 first = rstd::move(producer).join().unwrap();
    Vec<rstd::sync::Weak<vrento::Image>> results;
    for (auto& consumer : consumers) {
        auto result = rstd::move(consumer).join().unwrap();
        EXPECT_TRUE(result.as_ptr() == first.as_ptr());
        results.push(rstd::move(result));
    }
    for (const auto& result : results) EXPECT_TRUE(result.expired());
    EXPECT_TRUE(entered);
    EXPECT_TRUE(registered);
    auto stats = parser.Statistics();
    EXPECT_EQ(stats.decode_attempts, u64(1));
    EXPECT_EQ(stats.decode_total, u64(1));
    EXPECT_EQ(stats.in_flight_reuses, u64(8));
    EXPECT_EQ(stats.current_count, u64());
    EXPECT_EQ(stats.current_bytes, u64());
    EXPECT_EQ(gate->fields.lock().unwrap()->reads, usize(1));
}

TEST_F(ConditionalTextureTest, RawPixelsMoveIntoUploadPayloadWithoutAnotherAllocation) {
    WritePayload(owe::ImageType::UNKNOWN, false, "0123456789abcdef"_str.as_bytes(), 4, 4);
    auto memory = owe::TextureMemorySession::VirtualMemory();
    Option<vrento::texture::TextureUploadPayload> payload;
    {
        owe::TexImageParser parser(&vfs, None(), memory.options());
        ASSERT_TRUE(parser.Describe("payload"_str).is_ok());
        auto before = memory.statistics().logical.allocations;
        auto image  = parser.Parse("payload"_str).unwrap();
        EXPECT_EQ(memory.statistics().logical.allocations - before, u64(1));
        EXPECT_EQ(memory.statistics().logical.live_bytes, u64(16));
        payload = Some(vrento::texture::TextureUploadPayload::FromImage(
                           image->content.clone(), usize(), usize())
                           .unwrap());
    }
    EXPECT_EQ(memory.statistics().logical.live_bytes, u64(16));
    EXPECT_EQ((*payload)[usize()], u8('0'));
    payload = None();
    EXPECT_EQ(memory.statistics().logical.live_bytes, u64());
    EXPECT_EQ(memory.statistics().virtual_memory->mapped_bytes, usize());
}

TEST_F(ConditionalTextureTest, CompressedRawPixelsUseTheSameConfiguredAllocator) {
    WritePayload(owe::ImageType::UNKNOWN, true, "0123456789abcdef"_str.as_bytes(), 4, 4);
    auto                memory = owe::TextureMemorySession::VirtualMemory();
    owe::TexImageParser parser(&vfs, None(), memory.options());
    {
        auto image = parser.Parse("payload"_str).unwrap();
        EXPECT_EQ(memory.statistics().logical.live_bytes, u64(16));
        EXPECT_EQ(image->content->slots[usize()].mipmaps[usize()].data.get()[0], '0');
    }
    EXPECT_EQ(memory.statistics().virtual_memory->mapped_bytes, usize());
}

TEST_F(ConditionalTextureTest, EmbeddedPngTracksScratchAndReleasesPixels) {
    WritePayload(
        owe::ImageType::PNG,
        true,
        slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(RedPng), usize(sizeof(RedPng))),
        1,
        1,
        0);
    auto                memory = owe::TextureMemorySession::VirtualMemory();
    owe::TexImageParser parser(&vfs, None(), memory.options());
    {
        auto image = parser.Parse("payload"_str).unwrap();
        EXPECT_EQ(image->content->slots[usize()].mipmaps[usize()].data.get()[0], 255);
        EXPECT_GT(memory.statistics().logical.peak_live_bytes,
                  memory.statistics().logical.live_bytes);
    }
    EXPECT_EQ(memory.statistics().logical.live_bytes, u64());
    EXPECT_EQ(memory.statistics().virtual_memory->mapped_bytes, usize());
}

TEST_F(ConditionalTextureTest, FailedFlightDeliversSameErrorAndNextReadRetries) {
    WritePayload(owe::ImageType::UNKNOWN, false, "0123456789abcdef"_str.as_bytes(), 4, 4);
    auto                gate = MountControlledTexture(vfs, root / "materials" / "payload.tex");
    owe::TexImageParser parser(&vfs);
    ASSERT_TRUE(parser.Describe("payload"_str).is_ok());
    gate->Arm();
    auto read = [&] {
        return parser.Parse("payload"_str);
    };
    auto producer   = rstd::thread::spawn(read).unwrap();
    auto entered    = gate->WaitUntilEntered();
    auto consumer   = rstd::thread::spawn(read).unwrap();
    auto registered = WaitForRegisteredReads(parser, u64(1));
    gate->Release(true);
    auto first  = rstd::move(producer).join().unwrap();
    auto second = rstd::move(consumer).join().unwrap();
    EXPECT_TRUE(entered);
    EXPECT_TRUE(registered);
    ASSERT_TRUE(first.is_err());
    ASSERT_TRUE(second.is_err());
    const auto& error = first.as_ref().unwrap_err();
    EXPECT_EQ(error.kind, owe::ImageParseErrorKind::InvalidData);
    EXPECT_EQ(second.as_ref().unwrap_err().message, error.message);
    EXPECT_EQ(parser.Statistics().decode_attempts, u64(1));
    EXPECT_EQ(parser.Statistics().decode_total, u64());
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
    gate->Release();
    EXPECT_TRUE(parser.Parse("payload"_str).is_ok());
    EXPECT_EQ(parser.Statistics().decode_attempts, u64(2));
    EXPECT_EQ(parser.Statistics().decode_total, u64(1));
    EXPECT_EQ(parser.Statistics().current_count, u64());
}

TEST_F(ConditionalTextureTest, PartialDecodeFailureReleasesStorageAndReusesPreparedSourceOnRetry) {
    WriteTexture(false, 0);
    auto                gate = MountControlledTexture(vfs, root / "materials" / "conditional.tex");
    owe::TexImageParser parser(&vfs);
    ASSERT_TRUE(parser.Describe("conditional"_str).is_ok());
    gate->Arm(usize(2));
    gate->Release();
    auto failed = parser.Parse("conditional"_str);
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(failed.unwrap_err().kind, owe::ImageParseErrorKind::InvalidData);
    auto stats = parser.Statistics();
    EXPECT_EQ(stats.current_count, u64());
    EXPECT_EQ(stats.current_bytes, u64());
    EXPECT_EQ(stats.peak_count, u64(1));
    EXPECT_EQ(stats.peak_bytes, u64(16));
    EXPECT_EQ(stats.decode_total, u64());
    EXPECT_TRUE(parser.Parse("conditional"_str).is_ok());
    stats = parser.Statistics();
    EXPECT_EQ(stats.decode_attempts, u64(2));
    EXPECT_EQ(stats.decode_total, u64(1));
    EXPECT_EQ(stats.total_bytes, u64(36));
    EXPECT_EQ(stats.current_bytes, u64());
}

TEST_F(ConditionalTextureTest, CancelledQueuedReadDoesNotPoisonParserOrRetainCompletedPixels) {
    WritePayload(owe::ImageType::UNKNOWN, false, "0123456789abcdef"_str.as_bytes(), 4, 4);
    auto                gate = MountControlledTexture(vfs, root / "materials" / "payload.tex");
    owe::TexImageParser parser(&vfs);
    ASSERT_TRUE(parser.Describe("payload"_str).is_ok());
    gate->Arm();
    auto pool  = rstd::thread::ThreadPoolBuilder::make().worker_count(usize(1)).build().unwrap();
    auto tasks = rstd::thread::BlockingTaskSet<bool>::make(pool.handle(), usize(2)).unwrap();
    auto read  = [&] {
        return parser.Parse("payload"_str).is_ok();
    };
    ASSERT_TRUE(tasks.try_submit(read).is_ok());
    auto entered = gate->WaitUntilEntered();
    EXPECT_TRUE(tasks.try_submit(read).is_ok());
    tasks.cancel_pending();
    gate->Release();
    auto first  = tasks.recv().unwrap();
    auto second = tasks.recv().unwrap();
    rstd::move(pool).join();
    EXPECT_TRUE(entered);
    EXPECT_TRUE(first.is_completed());
    EXPECT_TRUE(second.is_cancelled());
    EXPECT_EQ(parser.Statistics().decode_total, u64(1));
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
    EXPECT_TRUE(parser.Parse("payload"_str).is_ok());
    EXPECT_EQ(parser.Statistics().decode_total, u64(2));
}

TEST_F(ConditionalTextureTest, MissingContentCanBeRetriedAfterItBecomesAvailable) {
    owe::TexImageParser parser(&vfs);
    auto                missing = parser.Parse("conditional"_str);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err().kind, owe::ImageParseErrorKind::MissingContent);
    WriteTexture(false, 0);
    EXPECT_TRUE(parser.Parse("conditional"_str).is_ok());
    EXPECT_EQ(parser.Statistics().decode_attempts, u64(2));
    EXPECT_EQ(parser.Statistics().decode_total, u64(1));
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
}

TEST_F(ConditionalTextureTest, EmbeddedDescriptionMatchesDecodeWithoutPublishingPixels) {
    for (auto compressed : array<bool, 2> { false, true }) {
        for (auto type :
             array<owe::ImageType, 2> { owe::ImageType::UNKNOWN, owe::ImageType::PNG }) {
            WritePayload(type,
                         compressed,
                         slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(RedPng),
                                                   usize(sizeof(RedPng))));
            auto [tx, rx] = rstd::sync::mpmc::channel<Arc<vrento::Image>>();
            owe::TexImageParser parser(
                &vfs, Some(owe::ImageReadyCallback::make([tx](Arc<vrento::Image> image) {
                    tx.send(rstd::move(image)).unwrap();
                })));
            auto description = parser.Describe("payload"_str);
            ASSERT_TRUE(description.is_ok());
            EXPECT_EQ(description->header.type, owe::ImageType::PNG);
            EXPECT_EQ(description->header.format, owe::TextureFormat::RGBA8);
            ASSERT_EQ(description->slots.len(), usize(1));
            EXPECT_EQ(description->slots[usize()].mip_count, usize(1));
            EXPECT_TRUE(parser.ParseHeader("payload"_str).is_ok());
            EXPECT_TRUE(rx.try_recv().is_err());
            auto image = parser.Parse("payload"_str);
            ASSERT_TRUE(image.is_ok());
            auto& mip = (*image)->content->slots[usize()].mipmaps[usize()];
            EXPECT_EQ(mip.width, description->slots[usize()].width);
            EXPECT_EQ(mip.height, description->slots[usize()].height);
            EXPECT_EQ(mip.size, isize(4));
            EXPECT_EQ(mip.data.get()[0], 255);
            EXPECT_EQ(mip.data.get()[1], 0);
            EXPECT_EQ(mip.data.get()[2], 0);
            EXPECT_EQ(mip.data.get()[3], 255);
            EXPECT_EQ(parser.Statistics().current_count, u64(1));
            EXPECT_EQ(parser.Statistics().current_bytes, u64(4));
            EXPECT_TRUE(rx.try_recv().is_ok());
        }
    }
}

TEST_F(ConditionalTextureTest, ContainerDescriptionDoesNotDecodeImageData) {
    auto bytes = Vec<u8>::from(
        slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(RedPng), usize(sizeof(RedPng))));
    for (usize i(41); i < usize(54); ++i) bytes[i] = u8();
    WritePayload(owe::ImageType::UNKNOWN, false, bytes.as_slice());
    owe::TexImageParser parser(&vfs);
    EXPECT_TRUE(parser.Describe("payload"_str).is_ok());
    auto decoded = parser.Parse("payload"_str);
    ASSERT_TRUE(decoded.is_err());
    EXPECT_EQ(decoded.unwrap_err().kind, owe::ImageParseErrorKind::DecodeFailed);
}

TEST_F(ConditionalTextureTest, RawBcDescriptionAndVideoRangeMatchContent) {
    for (auto compressed : array<bool, 2> { false, true }) {
        Vec<u8> bytes;
        bytes.resize(usize(16), u8(17));
        WritePayload(owe::ImageType::UNKNOWN, compressed, bytes.as_slice(), 4, 4);
        owe::TexImageParser parser(&vfs);
        auto                description = parser.Describe("payload"_str).unwrap();
        auto                image       = parser.Parse("payload"_str).unwrap();
        EXPECT_EQ(description.header.format, owe::TextureFormat::BC3);
        EXPECT_EQ(image->header.format, description.header.format);
        EXPECT_EQ(image->content->slots[usize()].mipmaps[usize()].size, isize(16));
        EXPECT_EQ(parser.Statistics().current_count, u64(1));
        EXPECT_EQ(parser.Statistics().current_bytes, u64(16));
    }
    auto bytes = Vec<u8>::from("\0\0\0\x10"
                               "ftypisom00000000"_str.as_bytes());
    WritePayload(owe::ImageType::UNKNOWN, false, bytes.as_slice(), 16, 8);
    owe::TexImageParser parser(&vfs);
    auto                description = parser.Describe("payload"_str).unwrap();
    EXPECT_EQ(description.header.type, owe::ImageType::VIDEO);
    EXPECT_EQ(description.header.format, owe::TextureFormat::RGBA8);
    auto  image = parser.Parse("payload"_str).unwrap();
    auto& mip   = image->content->slots[usize()].mipmaps[usize()];
    EXPECT_FALSE(bool(mip.data));
    ASSERT_TRUE(mip.video_source.is_some());
    EXPECT_EQ(mip.video_source->len(), rstd::as_cast<u64>(bytes.len()));
    owe::fs::BinaryReader reader(mip.video_source->clone());
    EXPECT_EQ(reader.read_all_bytes().unwrap().as_slice(), bytes.as_slice());
    auto retained = image.downgrade();
    image.reset();
    EXPECT_FALSE(retained.expired());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(retained.upgrade(), parser.Parse("payload"_str).unwrap()));
    EXPECT_EQ(parser.Statistics().decode_total, u64(1));
    EXPECT_EQ(parser.Statistics().current_count, u64());
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
}

TEST_F(ConditionalTextureTest, DescriptionPreservesEverySlotAndMip) {
    {
        std::ofstream output(root / "materials" / "slots.tex", std::ios::binary);
        output.write("TEXV0005", 9);
        output.write("TEXI0001", 9);
        for (auto value : array<unsigned, 7> { 4u, 0u, 8u, 8u, 8u, 8u, 0u })
            WriteU32(output, value);
        output.write("TEXB0002", 9);
        WriteU32(output, 2);
        for (unsigned slot {}; slot < 2; ++slot) {
            WriteU32(output, 2);
            for (unsigned mip {}; mip < 2; ++mip) {
                auto extent = 8u >> (slot + mip);
                auto size   = ((extent + 3) / 4) * ((extent + 3) / 4) * 16;
                for (auto value : array<unsigned, 5> { extent, extent, 0u, 0u, size })
                    WriteU32(output, value);
                for (unsigned index {}; index < size / 4; ++index) WriteU32(output, index);
            }
        }
    }
    owe::TexImageParser parser(&vfs);
    auto                described = parser.Describe("slots"_str).unwrap();
    auto                image     = parser.Parse("slots"_str).unwrap();
    ASSERT_EQ(described.slots.len(), usize(2));
    for (usize index {}; index < described.slots.len(); ++index) {
        const auto& slot = image->content->slots[index];
        EXPECT_EQ(described.slots[index].width, slot.width);
        EXPECT_EQ(described.slots[index].height, slot.height);
        EXPECT_EQ(described.slots[index].mip_count, slot.mipmaps.len());
        EXPECT_EQ(slot.mipmaps.len(), usize(2));
    }
}

TEST_F(ConditionalTextureTest, BaseMipmapsRemainAlignedWithMultipleConditions) {
    for (auto conditions : { 0u, 1u, 2u }) {
        WriteTexture(false, conditions);
        owe::TexImageParser parser(&vfs);
        auto                parsed = parser.Parse("conditional"_str);
        ASSERT_TRUE(parsed.is_ok());
        auto image = rstd::move(parsed).unwrap_unchecked();
        ASSERT_EQ(image->content->slots.len(), usize(1));
        ASSERT_EQ(image->content->slots[usize(0)].mipmaps.len(), usize(2));
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& mip = image->content->slots[usize(0)].mipmaps[usize(index)];
            EXPECT_EQ(mip.width, index == 0 ? 2 : 1);
            EXPECT_EQ(mip.height, mip.width);
            EXPECT_EQ(mip.size, isize(mip.width * mip.height * 4));
            ASSERT_TRUE(static_cast<bool>(mip.data));
            EXPECT_EQ(mip.data.get()[0], 0x11);
        }
        auto header = parser.ParseHeader("conditional"_str);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->width, image->header.width);
        EXPECT_EQ(header->mipmap_larger, image->header.mipmap_larger);
    }
}

TEST_F(ConditionalTextureTest, SelectsFullVideoPayloadByComboProperty) {
    {
        std::ofstream output(root / "materials" / "alternate.tex", std::ios::binary);
        output.write("TEXV0005", 9);
        output.write("TEXI0001", 9);
        for (auto value : { 0u, 0u, 16u, 8u, 16u, 8u, 0u }) WriteU32(output, value);
        output.write("TEXB0004", 9);
        for (auto value : { 1u, 0xffffffffu, 2u }) WriteU32(output, value);
        for (auto id : { 7u, 9u }) {
            for (auto value : { 1u, id, 2u }) WriteU32(output, value);
            const char first[]  = R"({"condition":{"name":"mode","condition":"1"}})";
            const char second[] = R"({"condition":{"name":"mode","condition":"2"}})";
            output.write(id == 7 ? first : second, sizeof(first));
        }
        for (auto value : { 1u, 16u, 8u, 0u, 0u, 16u }) WriteU32(output, value);
        output.write("\0\0\0\x10"
                     "ftypisom0000",
                     16);
        for (auto value : { 1u, 2u }) WriteU32(output, value);
        for (auto id : { 7u, 9u }) {
            for (auto value : { 1u, id, 0u, 0u, 0u, 0u, 0xffffffffu, 16u }) WriteU32(output, value);
            output.write("\0\0\0\x10"
                         "ftypisom000",
                         15);
            output.put(id == 7 ? '1' : '2');
        }
    }

    for (auto mode : { "0"_str, "1"_str, "2"_str }) {
        auto property = rstd::json::Map::make();
        property.insert("value"_Str, owe::Json::String(String::make(mode)));
        auto properties = rstd::json::Map::make();
        properties.insert("mode"_Str, owe::Json::Object(rstd::move(property)));
        owe::TexImageParser parser(&vfs, None(), {}, rstd::move(properties));
        EXPECT_EQ(parser.Describe("alternate"_str).unwrap().header.type, owe::ImageType::VIDEO);
        auto  image = parser.Parse("alternate"_str).unwrap();
        auto& mip   = image->content->slots[usize()].mipmaps[usize()];
        ASSERT_TRUE(mip.video_source.is_some());
        owe::fs::BinaryReader reader(mip.video_source->clone());
        EXPECT_EQ(reader.read_all_bytes().unwrap()[usize(15)], u8(mode.as_bytes()[usize()]));
    }
}

TEST_F(ConditionalTextureTest, AssetVersionWriterPreservesFixedRecord) {
    struct Case {
        ref<str> prefix;
        int      version;
        ref<str> expected;
    };
    for (const auto& item : array<Case, 8> {
             Case { "SPVS"_str, 5, "SPVS0005\0"_str },
             Case { "SPVS-extra"_str, -1, "SPVS-000\0"_str },
             Case { "X"_str, -1, "X-0001\0\0\0"_str },
             Case { ""_str, 0, "0000\0\0\0\0\0"_str },
             Case { "SPVS"_str, 12345, "SPVS1234\0"_str },
             Case { "X\0YZ"_str, 7, "X0007\0\0\0\0"_str },
             Case { "SPVS"_str, i32::MIN.to_primitive(), "SPVS-214\0"_str },
             Case { "SPVS"_str, i32::MAX.to_primitive(), "SPVS2147\0"_str },
         }) {
        auto output = vfs.open_write("/assets/version"_str,
                                     owe::fs::WriteOptions { .create = true, .truncate = true });
        ASSERT_TRUE(output.is_ok());
        {
            owe::fs::BinaryWriter writer(rstd::move(output).unwrap());
            owe::WriteAssetVersion(item.prefix, writer, item.version);
        }
        auto path =
            rstd::path::PathBuf::from(rstd::cppstd::as_str((root / "version").string()).unwrap());
        auto bytes = rstd::fs::read(path.as_path());
        ASSERT_TRUE(bytes.is_ok());
        EXPECT_EQ(bytes->as_slice(), item.expected.as_bytes());
    }
}

TEST_F(ConditionalTextureTest, MetadataDoesNotDecodeAndParsePublishesOwnedContentOnce) {
    WriteTexture(false, 0);
    auto [tx, rx] = rstd::sync::mpmc::channel<Arc<vrento::Image>>();
    auto ready    = owe::ImageReadyCallback::make([tx](Arc<vrento::Image> image) {
        tx.send(rstd::move(image)).unwrap();
    });
    Option<Arc<vrento::Image>> retained;
    {
        owe::TexImageParser parser(&vfs, Some(rstd::move(ready)));
        ASSERT_TRUE(parser.ParseHeader("conditional"_str).is_ok());
        auto description = parser.Describe("conditional"_str);
        ASSERT_TRUE(description.is_ok());
        ASSERT_EQ(description->slots.len(), usize(1));
        EXPECT_EQ(description->slots[usize()].mip_count, usize(2));
        EXPECT_TRUE(rx.try_recv().is_err());
        ASSERT_TRUE(parser.Parse("conditional"_str).is_ok());
        auto queued = rx.try_recv();
        ASSERT_TRUE(queued.is_ok());
        retained = Some(queued.unwrap());
        Vec<String> names;
        names.push("conditional"_Str);
        names.push("conditional"_Str);
        auto images = parser.ParseMany(names.as_slice());
        for (const auto& image : images) {
            ASSERT_TRUE(image.is_ok());
            EXPECT_TRUE(Arc<vrento::Image>::ptr_eq((*image)->content, *retained));
        }
        EXPECT_TRUE(rx.try_recv().is_err());
    }
    EXPECT_EQ((*retained)->slots[usize()].mipmaps[usize()].data.get()[0], 0x11);
}

TEST_F(ConditionalTextureTest, SpriteHeaderSkipsConditionalPatches) {
    WriteTexture(true, 1);
    owe::TexImageParser parser(&vfs);
    auto                header = parser.ParseHeader("conditional"_str);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->spriteAnim.numFrames(), usize(1));
    EXPECT_EQ(header->extraHeader.get("texs"_str).unwrap()->val, 3);
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
    EXPECT_EQ(image->content->slots[usize(0)].mipmaps.len(), usize(2));
    image.reset();
    EXPECT_EQ(parser.Statistics().current_bytes, u64());
    EXPECT_TRUE(parser.Describe("conditional"_str).is_ok());
    auto reloaded = parser.Parse("conditional"_str).unwrap();
    EXPECT_EQ(reloaded->header.spriteAnim.numFrames(), usize(1));
    EXPECT_EQ(reloaded->content->slots[usize()].mipmaps.len(), usize(2));
    EXPECT_EQ(parser.Statistics().decode_total, u64(2));
}

TEST_F(ConditionalTextureTest, ParserRetainsMountedSourcesAfterVfsDestruction) {
    WriteTexture(false, 0);
    Option<owe::TexImageParser> parser;
    {
        owe::fs::VFS source;
        auto         mount =
            owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(root.string()).unwrap()));
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
        EXPECT_EQ(image->content->key, names[index].as_str());
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
        for (int i {}; i < 4; ++i) WriteU32(output, 1);
        WriteU32(output, 0);
        output.write("TEXB0001", 9);
        for (int i {}; i < 4; ++i) WriteU32(output, 1);
        WriteU32(output, 16);
        for (int i {}; i < 4; ++i) WriteU32(output, 0);
    }

    auto physical =
        owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(root.string()).unwrap()));
    ASSERT_TRUE(physical.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(physical).unwrap_unchecked()).is_ok());

    owe::TexImageParser parser(&vfs);
    auto                parsed = parser.ParseHeader("mask"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto header = rstd::move(parsed).unwrap_unchecked();
    EXPECT_EQ(header.extraHeader.get("compo1"_str).unwrap()->val, 0);
    EXPECT_EQ(header.extraHeader.get("compo2"_str).unwrap()->val, 0);
    EXPECT_EQ(header.extraHeader.get("compo3"_str).unwrap()->val, 0);
    EXPECT_EQ(header.extraHeader.get("compo4"_str).unwrap()->val, 1);

    std::filesystem::remove_all(root);
}

TEST(ImageParser, SceneBatchPreservesRuntimeParserAndErrorPositions) {
    owe::Scene scene;
    scene.SetImageParser(rstd::sync::Arc<dyn<owe::IImageParser>>::make(MixedImageParser {}));
    auto runtime          = Arc<owe::Image>::make();
    runtime->content->key = "runtime-value"_Str;
    scene.RegisterRuntimeImage(String::make("runtime"_str), runtime.clone());

    Vec<String> names;
    for (const char* name : { "first", "runtime", "bad", "last" }) {
        names.push(String::make(rstd::cppstd::as_str(name).unwrap()));
    }
    auto images = scene.ParseImages(names.as_slice());

    ASSERT_EQ(images.len(), names.len());
    ASSERT_TRUE(images[usize(0)].is_ok());
    EXPECT_EQ((*images[usize(0)])->content->key, "first"_str);
    ASSERT_TRUE(images[usize(1)].is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(*images[usize(1)], runtime));
    ASSERT_TRUE(images[usize(2)].is_err());
    EXPECT_EQ(images[usize(2)].unwrap_err_unchecked().kind, owe::ImageParseErrorKind::DecodeFailed);
    ASSERT_TRUE(images[usize(3)].is_ok());
    EXPECT_EQ((*images[usize(3)])->content->key, "last"_str);
}

TEST(ImageParser, CapturedSourceRetainsOldRuntimeImageAndParser) {
    Option<Arc<owe::SceneImageSource>> source;
    auto                               original = Arc<owe::Image>::make();
    original->content->key                      = "original"_Str;
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
    auto first = (*source)->Parse("first"_str);
    ASSERT_TRUE(first.is_ok());
    auto reused = (*source)->Parse("first"_str);
    ASSERT_TRUE(reused.is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(*first, *reused));
}

TEST_F(ConditionalTextureTest, CapturedPackageSourceSharesSceneParserWithoutPinningPixels) {
    WriteTexture(false, 0);
    auto [tx, rx] = rstd::sync::mpmc::channel<rstd::sync::Weak<vrento::Image>>();
    owe::Scene scene;
    scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(owe::TexImageParser(
        &vfs, Some(owe::ImageReadyCallback::make([tx](Arc<vrento::Image> image) {
            tx.send(image.downgrade()).unwrap();
        })))));
    auto source  = scene.CaptureImageSource();
    auto image   = scene.ParseImage("conditional"_str).unwrap();
    auto content = image->content.downgrade();
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(image, source->Parse("conditional"_str).unwrap()));
    {
        Vec<String> names;
        names.push("conditional"_Str);
        names.push("conditional"_Str);
        auto batch = scene.ParseImages(names.as_slice());
        for (const auto& parsed : batch) {
            ASSERT_TRUE(parsed.is_ok());
            EXPECT_TRUE(Arc<owe::Image>::ptr_eq(image, *parsed));
        }
    }
    EXPECT_TRUE(rx.try_recv().is_ok());
    EXPECT_TRUE(rx.try_recv().is_err());
    image.reset();
    EXPECT_TRUE(content.expired());
    EXPECT_TRUE(source->Describe("conditional"_str).is_ok());
    EXPECT_TRUE(rx.try_recv().is_err());
    EXPECT_TRUE(source->Parse("conditional"_str).is_ok());
    EXPECT_TRUE(rx.try_recv().is_ok());
    EXPECT_TRUE(content.expired());
}

TEST_F(ConditionalTextureTest, CapturedPackageSourcePreservesParserSnapshotAfterReplacement) {
    WritePayload(owe::ImageType::UNKNOWN, false, "old0123456789abc"_str.as_bytes(), 4, 4);
    owe::Scene scene;
    scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(owe::TexImageParser(&vfs)));
    auto before = scene.CaptureImageSource();
    ASSERT_TRUE(before->Describe("payload"_str).is_ok());
    std::filesystem::rename(root / "materials" / "payload.tex", root / "original.tex");
    WritePayload(owe::ImageType::UNKNOWN, false, "new0123456789abc"_str.as_bytes(), 4, 4);
    scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(owe::TexImageParser(&vfs)));
    auto after       = scene.CaptureImageSource();
    auto original    = before->Parse("payload"_str).unwrap();
    auto replacement = after->Parse("payload"_str).unwrap();
    EXPECT_EQ(original->content->slots[usize()].mipmaps[usize()].data.get()[0], 'o');
    EXPECT_EQ(replacement->content->slots[usize()].mipmaps[usize()].data.get()[0], 'n');
    EXPECT_FALSE(Arc<vrento::Image>::ptr_eq(original->content, replacement->content));
}

TEST_F(ConditionalTextureTest, CapturedExternalSourcePinsItsRevisionUntilTextureReplacement) {
    auto write = [&](unsigned char value) {
        std::ofstream output(root / "external.ppm", std::ios::binary);
        output << "P6\n1 1\n255\n";
        const unsigned char pixel[] { value, 34, 51 };
        output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    };
    auto key = String::make(rstd::cppstd::as_str((root / "external.ppm").string()).unwrap());
    write(17);
    owe::Scene scene;
    scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(owe::TexImageParser(&vfs)));
    scene.RegisterTexture("external"_Str, { .url = key.clone() });
    auto before   = scene.CaptureImageSource();
    auto original = before->Parse(key.as_str()).unwrap();
    auto content  = original->content.downgrade();
    write(99);
    original.reset();
    EXPECT_FALSE(content.expired());
    auto retained = before->Parse(key.as_str()).unwrap();
    EXPECT_EQ(retained->content->slots[usize()].mipmaps[usize()].data.get()[0], 17);
    scene.RegisterTexture("external"_Str, { .url = key.clone() });
    auto after       = scene.CaptureImageSource();
    auto replacement = after->Parse(key.as_str()).unwrap();
    EXPECT_EQ(replacement->content->slots[usize()].mipmaps[usize()].data.get()[0], 99);
    EXPECT_FALSE(Arc<vrento::Image>::ptr_eq(retained->content, replacement->content));
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(retained, before->Parse(key.as_str()).unwrap()));
    std::filesystem::remove(root / "external.ppm");
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(retained, before->Parse(key.as_str()).unwrap()));
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(replacement, after->Parse(key.as_str()).unwrap()));
    EXPECT_EQ(before->Describe(key.as_str()).unwrap().header.width, 1);
    EXPECT_EQ(after->Describe(key.as_str()).unwrap().header.width, 1);
}

} // namespace

TEST_F(ConditionalTextureTest, ExternalPathsPreserveDecodedBytesAndMalformedEscapes) {
    struct Case {
        std::string filename;
        std::string encoded;
    };
    for (const auto& item : {
             Case { "space \xc3\xa9.ppm", "space%20%C3%A9.ppm" },
             Case { "raw-\xff.ppm", "raw-%FF.ppm" },
             Case { "literal%GG.ppm", "literal%GG.ppm" },
             Case { "nul.ppm", "nul.ppm%00ignored" },
         }) {
        {
            std::ofstream output(root / item.filename, std::ios::binary);
            output << "P6\n1 1\n255\n";
            const unsigned char pixel[] { 17, 34, 51 };
            output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        }
        for (const auto& prefix :
             { std::string {}, std::string("file://"), std::string("file://localhost") }) {
            const auto          key = prefix + root.string() + "/" + item.encoded;
            owe::TexImageParser parser(&vfs);
            auto                header = parser.ParseHeader(rstd::cppstd::as_str(key).unwrap());
            ASSERT_TRUE(header.is_ok()) << key;
            EXPECT_EQ(header->width, 1);
            EXPECT_EQ(header->height, 1);
            auto description = parser.Describe(rstd::cppstd::as_str(key).unwrap());
            ASSERT_TRUE(description.is_ok());
            ASSERT_EQ(description->slots.len(), usize(1));
            EXPECT_EQ(description->slots[usize()].width, 1);
            EXPECT_EQ(description->slots[usize()].height, 1);
            auto image = parser.Parse(rstd::cppstd::as_str(key).unwrap());
            ASSERT_TRUE(image.is_ok()) << key;
            auto& mip = (*image)->content->slots[usize()].mipmaps[usize()];
            EXPECT_EQ(mip.data.get()[0], 17);
            EXPECT_EQ(mip.data.get()[1], 34);
            EXPECT_EQ(mip.data.get()[2], 51);
            EXPECT_EQ(mip.data.get()[3], 255);
        }
    }
    owe::TexImageParser parser(&vfs);
    EXPECT_TRUE(parser.ParseHeader(rstd::cppstd::as_str(root.string()).unwrap()).is_err());
}

TEST(ImageParser, AssetVersionPreservesPrefixNumericAndCursorRules) {
    struct Case {
        ref<str> tag;
        int      expected;
    };
    for (const auto& item : array<Case, 7> {
             Case { "TEXV0005"_str, 5 },
             Case { "TEXZ12x9"_str, 12 },
             Case { "TEXV-123"_str, -123 },
             Case { "TEXV+123"_str, 0 },
             Case { "TEXV 123"_str, 0 },
             Case { "MDLV0005"_str, 0 },
             Case { "TEXV000"_str, 0 },
         }) {
        auto bytes = Vec<u8>::from(item.tag.as_bytes());
        bytes.push(u8());
        bytes.push(u8(127));
        owe::fs::BinaryReader reader(rstd::move(bytes));
        EXPECT_EQ(owe::ReadTexVersion(reader), item.expected);
        EXPECT_EQ(reader.Tell(), 9);
    }
}
