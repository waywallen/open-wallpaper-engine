#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import wescene.fs;
import wescene.json;
import wescene.pkg_fs;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

auto FsError(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

struct MemorySource {
    std::string data;

    auto read_at(mut_ref<u8[]> buffer, u64 offset) const -> rstd::io::Result<usize> {
        auto position = rstd::try_from<usize>(offset);
        if (position.is_err()) return rstd::Ok(usize());
        auto position_value = rstd::move(position).unwrap_unchecked();
        auto data_len       = usize(data.size());
        if (position_value >= data_len) return rstd::Ok(usize());
        auto count = rstd::min(buffer.len(), data_len - position_value);
        rstd::mem::memcpy(buffer.as_raw_ptr(), data.data() + position_value.to_primitive(), count);
        return rstd::Ok(count);
    }
};

} // namespace

template<>
struct rstd::Impl<rstd::io::ReadAt, MemorySource> : rstd::ImplBase<MemorySource> {
    auto read_at(mut_ref<u8[]> buffer, u64 offset) const -> rstd::io::Result<usize> {
        return this->self().read_at(buffer, offset);
    }
};

namespace
{

class TestMount {
public:
    explicit TestMount(std::unordered_map<std::string, std::string> files,
                       std::string                                  invalid_path = {})
        : m_files(std::move(files)), m_invalid_path(std::move(invalid_path)) {}

    auto open_read(owe::fs::Path path) const -> rstd::io::Result<owe::fs::ReadRange> {
        auto key = rstd::cppstd::to_string(path.as_os_str().to_str().unwrap());
        if (key == m_invalid_path) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        auto file = m_files.find(key);
        if (file == m_files.end()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
        }
        auto source = rstd::io::SharedReadAt::make(MemorySource { file->second });
        return owe::fs::ReadRange::make(std::move(source), u64(), u64(file->second.size()));
    }

    auto open_write(owe::fs::Path path, owe::fs::WriteOptions) const
        -> rstd::io::Result<owe::fs::WriteSeekHandle> {
        auto key = rstd::cppstd::to_string(path.as_os_str().to_str().unwrap());
        if (m_files.contains(key)) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
        }
        return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    }

    auto metadata(owe::fs::Path path) const -> rstd::io::Result<owe::fs::FileMetadata> {
        auto key  = rstd::cppstd::to_string(path.as_os_str().to_str().unwrap());
        auto file = m_files.find(key);
        if (file == m_files.end()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
        }
        return rstd::Ok(owe::fs::FileMetadata {
            .len          = u64(file->second.size()),
            .is_file      = true,
            .is_directory = false,
            .readonly     = true,
        });
    }

private:
    std::unordered_map<std::string, std::string> m_files;
    std::string                                  m_invalid_path;
};

auto MakeMount(std::unordered_map<std::string, std::string> files, std::string invalid_path = {})
    -> owe::fs::MountHandle;

auto ReadText(owe::fs::ReadRange range) -> String {
    owe::fs::BinaryReader reader(std::move(range));
    return reader.ReadAllStr();
}

void WriteI32(std::ofstream& output, std::int32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteSized(std::ofstream& output, std::string_view value) {
    WriteI32(output, static_cast<std::int32_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void WritePkg(const std::filesystem::path& path, std::int32_t body_length,
              std::string_view entry_path = "Materials/Foo.bin") {
    std::ofstream output(path, std::ios::binary);
    WriteSized(output, "PKGV0001");
    WriteI32(output, 1);
    WriteSized(output, entry_path);
    WriteI32(output, 0);
    WriteI32(output, body_length);
    output.write("hello", 5);
}

class TempDirectory {
public:
    TempDirectory() {
        auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("owe-vfs-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~TempDirectory() { std::filesystem::remove_all(path); }

    std::filesystem::path path;
};

} // namespace

template<>
struct rstd::Impl<owe::fs::MountFs, TestMount> : rstd::ImplBase<TestMount> {
    auto open_read(owe::fs::Path path) const -> rstd::io::Result<owe::fs::ReadRange> {
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

auto MakeMount(std::unordered_map<std::string, std::string> files, std::string invalid_path)
    -> owe::fs::MountHandle {
    return owe::fs::MountHandle::make(TestMount(std::move(files), std::move(invalid_path)));
}

TEST(Vfs, OverlayAndUnmountKeepOpenedRangeAlive) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, MakeMount({ { "shared", "lower" } })).is_ok());
    auto upper = vfs.mount("/assets"_str, MakeMount({ { "shared", "upper" } }));
    ASSERT_TRUE(upper.is_ok());

    auto opened = vfs.open_read("/assets/shared"_str);
    ASSERT_TRUE(opened.is_ok());
    auto retained = std::move(opened).unwrap_unchecked();

    EXPECT_TRUE(vfs.unmount(*upper));
    auto visible = vfs.open_read("/assets/shared"_str);
    ASSERT_TRUE(visible.is_ok());
    EXPECT_EQ(ReadText(std::move(visible).unwrap_unchecked()), "lower"_str);
    EXPECT_EQ(ReadText(std::move(retained)), "upper"_str);
}

TEST(Vfs, BackendErrorsAreNotOverlayMisses) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, MakeMount({ { "broken", "lower" } })).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, MakeMount({}, "broken")).is_ok());

    auto opened = vfs.open_read("/assets/broken"_str);
    ASSERT_TRUE(opened.is_err());
    EXPECT_EQ(std::move(opened).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidData);
}

TEST(BinaryReader, CompletesReadsAcrossBufferedBoundary) {
    std::string bytes(8195, '\0');
    bytes[8191] = '\x78';
    bytes[8192] = '\x56';
    bytes[8193] = '\x34';
    bytes[8194] = '\x12';

    auto source = rstd::io::SharedReadAt::make(MemorySource { std::move(bytes) });
    auto range  = rstd::io::ReadRange::make(std::move(source), u64(), u64(8195));
    ASSERT_TRUE(range.is_ok());
    owe::fs::BinaryReader reader(std::move(range).unwrap_unchecked());

    std::string prefix(8191, '\0');
    EXPECT_EQ(reader.Read(prefix.data(), prefix.size()), prefix.size());
    EXPECT_EQ(reader.ReadUint32(), 0x12345678u);
}

TEST(Vfs, PathsUseComponentBoundariesAndRejectTraversal) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/asset"_str, MakeMount({ { "file", "value" } })).is_ok());

    EXPECT_TRUE(vfs.open_read("/assets/file"_str).is_err());
    auto invalid = vfs.open_read("/asset/../file"_str);
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(std::move(invalid).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidInput);
    EXPECT_TRUE(vfs.mount("asset"_str, MakeMount({})).is_err());
}

TEST(Vfs, WriteRoutingPreservesReadonlyOverlay) {
    TempDirectory temp;
    {
        std::ofstream file(temp.path / "locked");
        file << "physical";
    }

    auto physical =
        owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(temp.path.string()).unwrap()));
    ASSERT_TRUE(physical.is_ok());

    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(physical).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, MakeMount({ { "locked", "readonly" } })).is_ok());

    auto locked = vfs.open_write("/assets/locked"_str, owe::fs::WriteOptions { .truncate = true });
    ASSERT_TRUE(locked.is_err());
    EXPECT_EQ(std::move(locked).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::ReadOnlyFilesystem);

    auto created = vfs.open_write("/assets/new"_str,
                                  owe::fs::WriteOptions { .create = true, .truncate = true });
    ASSERT_TRUE(created.is_ok());
    {
        owe::fs::BinaryWriter writer(std::move(created).unwrap_unchecked());
        EXPECT_EQ(writer.Write("new", 3), 3u);
    }

    std::ifstream file(temp.path / "new");
    std::string   content;
    file >> content;
    EXPECT_EQ(content, "new");
}

TEST(PkgFs, ReusesHeaderAndRejectsInvalidEntryRanges) {
    TempDirectory temp;
    auto          valid_path = temp.path / "valid.pkg";
    WritePkg(valid_path, 5);

    auto pkg =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(valid_path.string()).unwrap()));
    ASSERT_TRUE(pkg.is_ok());
    auto stamp = pkg->pkg_version_stamp();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(stamp.data()), stamp.len().to_primitive()),
              "PKGV0001");

    auto source = pkg->open_read(owe::fs::Path("/materials/foo.BIN"_str));
    ASSERT_TRUE(source.is_ok());
    EXPECT_EQ(ReadText(std::move(source).unwrap_unchecked()), "hello"_str);

    auto invalid_path = temp.path / "invalid.pkg";
    WritePkg(invalid_path, 100);
    auto invalid =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(invalid_path.string()).unwrap()));
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(std::move(invalid).unwrap_err_unchecked().kind().code,
              rstd::io::error::ErrorKind::InvalidData);
}

TEST(PkgFs, PreservesUtf8WhileFoldingAsciiPathCase) {
    TempDirectory temp;
    auto          path = temp.path / "utf8.pkg";
    WritePkg(path, 5, "Materials/École/贴图.BIN");

    auto pkg = owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(path.string()).unwrap()));
    ASSERT_TRUE(pkg.is_ok());
    auto source = pkg->open_read(owe::fs::Path("/materials/École/贴图.bin"_str));
    ASSERT_TRUE(source.is_ok());
    EXPECT_EQ(ReadText(std::move(source).unwrap_unchecked()), "hello"_str);
}

TEST(PkgFs, ResolvesAuthoredParentPathsInsideAssetRoot) {
    TempDirectory temp;
    auto          path = temp.path / "parent.pkg";
    WritePkg(path, 5, "../海景画/particles/snow.json");

    auto pkg = owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(path.string()).unwrap()));
    ASSERT_TRUE(pkg.is_ok());

    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg->mount_handle()).is_ok());
    auto asset = owe::fs::ResolveAssetPath("../海景画/particles/snow.json"_str);
    ASSERT_TRUE(asset.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(asset->as_path().as_os_str().to_str().unwrap()),
              "/assets/海景画/particles/snow.json");

    auto source = vfs.open_read(asset->as_path());
    ASSERT_TRUE(source.is_ok());
    EXPECT_EQ(ReadText(std::move(source).unwrap_unchecked()), "hello"_str);
}

} // namespace

TEST(BinaryReader, NativeTextOwnsUtf8AndEmbeddedNul) {
    auto source = rstd::io::SharedReadAt::make(MemorySource { std::string("a\0\xc3\xa9", 4) });
    auto range  = owe::fs::ReadRange::make(rstd::move(source), u64(), u64(4)).unwrap();
    owe::fs::BinaryReader reader(rstd::move(range));
    auto                  text = reader.read_all_string();
    ASSERT_TRUE(text.is_ok());
    EXPECT_EQ(text->as_str(), "a\0\xc3\xa9"_str);
    EXPECT_EQ(reader.position(), u64(4));
    EXPECT_EQ(reader.remaining(), u64());
    auto empty = reader.read_all_string();
    ASSERT_TRUE(empty.is_ok());
    EXPECT_TRUE(empty->is_empty());
}

TEST(BinaryReader, NativeBytesPreserveInvalidUtf8AndTextRejectsIt) {
    auto source = rstd::io::SharedReadAt::make(MemorySource { std::string("\xff\0z", 3) });
    auto range  = owe::fs::ReadRange::make(rstd::move(source), u64(), u64(3)).unwrap();
    owe::fs::BinaryReader reader(rstd::move(range));
    auto                  bytes = reader.read_all_bytes();
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(bytes->as_slice(), "\xff\0z"_bytes);
    ASSERT_TRUE(reader.Rewind());
    auto text = reader.read_all_string();
    ASSERT_TRUE(text.is_err());
    EXPECT_EQ(text.unwrap_err().kind().code, rstd::io::error::ErrorKind::InvalidData);
    EXPECT_EQ(reader.position(), u64(3));
    ASSERT_TRUE(reader.Rewind());
    EXPECT_TRUE(reader.ReadAllStr().is_empty());
}

TEST(BinaryReader, NativeReadsReportShortSourceAndTrackConsumedBytes) {
    auto source = rstd::io::SharedReadAt::make(MemorySource { "abc" });
    auto range  = owe::fs::ReadRange::make(rstd::move(source), u64(), u64(5)).unwrap();
    owe::fs::BinaryReader reader(rstd::move(range));
    auto                  bytes = reader.read_all_bytes();
    ASSERT_TRUE(bytes.is_err());
    EXPECT_EQ(bytes.unwrap_err().kind().code, rstd::io::error::ErrorKind::UnexpectedEof);
    EXPECT_EQ(reader.position(), u64(3));
    ASSERT_TRUE(reader.Rewind());
    auto text = reader.read_all_string();
    ASSERT_TRUE(text.is_err());
    EXPECT_EQ(text.unwrap_err().kind().code, rstd::io::error::ErrorKind::UnexpectedEof);
}

TEST(Vfs, NativeContentSeparatesBinaryTextAndJsonFailures) {
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str,
                          MakeMount({
                              { "binary", std::string("\xff\0z", 3) },
                              { "valid.json", "{\"value\":7}" },
                              { "broken.json", "{" },
                              { "empty", "" },
                          }))
                    .is_ok());
    auto bytes = owe::fs::ReadFileBytes(vfs, owe::fs::Path("/assets/binary"_str));
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(bytes->as_slice(), "\xff\0z"_bytes);
    auto text = owe::fs::ReadFileContent(vfs, owe::fs::Path("/assets/binary"_str));
    ASSERT_TRUE(text.is_err());
    EXPECT_EQ(text.unwrap_err().kind().code, rstd::io::error::ErrorKind::InvalidData);
    auto json = owe::ReadAssetJsonFile(vfs, "valid.json"_str);
    ASSERT_TRUE(json.is_ok());
    EXPECT_EQ(*(*json->get("value"_str))->as_u64(), u64(7));
    auto malformed = owe::ReadAssetJsonFile(vfs, "broken.json"_str);
    ASSERT_TRUE(malformed.is_err());
    EXPECT_EQ(malformed.unwrap_err().kind, owe::JsonFileErrorKind::Parse);
    auto binary = owe::ReadAssetJsonFile(vfs, "binary"_str);
    ASSERT_TRUE(binary.is_err());
    EXPECT_EQ(binary.unwrap_err().kind, owe::JsonFileErrorKind::Io);
    auto missing = owe::ReadAssetJsonFile(vfs, "missing"_str);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err().kind, owe::JsonFileErrorKind::Io);
    auto empty = owe::fs::ReadFileContent(vfs, owe::fs::Path("/assets/empty"_str));
    ASSERT_TRUE(empty.is_ok());
    EXPECT_TRUE(empty->is_empty());
}

TEST(BinaryReader, TerminatedStringsPreserveCursorAndEofPrefix) {
    auto source =
        rstd::io::SharedReadAt::make(MemorySource { std::string("\0ab\xc3\xa9\0tail", 10) });
    auto range = owe::fs::ReadRange::make(rstd::move(source), u64(), u64(10)).unwrap();
    owe::fs::BinaryReader reader(rstd::move(range));
    EXPECT_TRUE(reader.ReadStr().is_empty());
    EXPECT_EQ(reader.position(), u64(1));
    auto owned = reader.ReadStr();
    EXPECT_EQ(owned, "ab\xc3\xa9"_str);
    EXPECT_EQ(reader.position(), u64(6));
    EXPECT_EQ(reader.ReadStr(), "tail"_str);
    EXPECT_EQ(reader.position(), u64(10));
    EXPECT_TRUE(reader.ReadStr().is_empty());
    EXPECT_EQ(owned, "ab\xc3\xa9"_str);
}

TEST(BinaryReader, NativeBufferAndRemainingReaderOwnIndependentStorage) {
    Vec<u8> data;
    for (auto value : "abcdef"_bytes) data.push(rstd::move(value));
    owe::fs::BinaryReader source(rstd::move(data));
    ASSERT_TRUE(source.SeekSet(2));
    owe::fs::BinaryReader remaining(source);
    EXPECT_EQ(source.position(), u64(6));
    EXPECT_EQ(remaining.ReadAllStr(), "cdef"_str);
    ASSERT_TRUE(source.Rewind());
    EXPECT_EQ(source.ReadAllStr(), "abcdef"_str);
}

TEST(PkgFs, RejectsMalformedUtf8AndOversizedEntryNames) {
    TempDirectory temporary;
    auto          path = temporary.path / "invalid-name.pkg";
    WritePkg(path, 5, std::string("\xff", 1));
    auto invalid =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(path.string()).unwrap()));
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(invalid.unwrap_err().kind().code, rstd::io::error::ErrorKind::InvalidData);
    WritePkg(path, 5, std::string(4097, 'a'));
    auto oversized =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(path.string()).unwrap()));
    ASSERT_TRUE(oversized.is_err());
    EXPECT_EQ(oversized.unwrap_err().kind().code, rstd::io::error::ErrorKind::InvalidData);
}
