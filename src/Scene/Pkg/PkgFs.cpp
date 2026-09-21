module;

#include <rstd/macro.hpp>

module wescene.pkg_fs;
import wescene.core;
import rstd;
import rstd.log;

import wescene.fs;

using rstd::collections::HashMap;
using namespace owe;
using namespace owe::fs;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

auto FsError(rstd::io::error::ErrorKind::Entity kind) -> rstd::io::error::Error {
    return rstd::io::error::Error::from_kind(rstd::io::error::ErrorKind { kind });
}

Option<String> ReadSizedString(BinaryReader& file, usize max_len) {
    auto signed_len = file.ReadInt32();
    if (signed_len < 0) return None();

    auto len = usize(static_cast<rstd::size_t>(signed_len));
    if (len > max_len) return None();
    auto bytes = Vec<u8>::with_capacity(len);
    bytes.resize(len, u8());
    if (file.read_exact(bytes.data(), len).is_err()) return None();
    auto result = String::from_utf8(rstd::move(bytes));
    if (result.is_err()) return None();
    return Some(rstd::move(result).unwrap_unchecked());
}

bool IsPkgVersionStamp(ref<str> stamp) {
    return stamp->len() > usize(4) && stamp->starts_with("PKGV"_str);
}

auto LookupKey(Path path) -> rstd::io::Result<String> {
    auto normalized = rstd_try(resolve_beneath("/"_str, path));
    auto output     = "/"_Str;
    auto components = normalized.as_path().components();
    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if (! component->is_normal()) continue;
        auto value = component->as_os_str().to_str();
        if (value.is_none()) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidFilename));
        }
        if (output.len() > usize(1)) output.push_ascii(u8('/'));
        output.push_str(*value);
    }
    output->make_ascii_lowercase();
    return rstd::Ok(rstd::move(output));
}

} // namespace

auto WPPkgFs::open(Path pkg_path) -> rstd::io::Result<PkgMount> {
    auto file = rstd::fs::File::open(pkg_path);
    if (file.is_err()) return rstd::Err(rstd::move(file).unwrap_err_unchecked());
    auto opened   = rstd::move(file).unwrap_unchecked();
    auto metadata = opened.metadata();
    if (metadata.is_err()) return rstd::Err(rstd::move(metadata).unwrap_err_unchecked());
    auto source = rstd::io::SharedReadAt::make(rstd::move(opened));
    auto range =
        ReadRange::make(rstd::move(source), u64(), rstd::move(metadata).unwrap_unchecked().len());
    if (range.is_err()) return rstd::Err(rstd::move(range).unwrap_err_unchecked());
    auto pkg_source = rstd::move(range).unwrap_unchecked();
    auto pkg        = BinaryReader(pkg_source.clone());

    auto version = ReadSizedString(pkg, usize(64));
    if (! version || ! IsPkgVersionStamp(version->as_str())) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
    }
    rstd_info("pkg version: {}", *version);

    struct PendingFile {
        String path;
        u64    offset;
        u64    length;
    };
    Vec<PendingFile> files;

    auto entry_count = pkg.ReadInt32();
    if (entry_count < 0) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
    }
    files.reserve(usize(entry_count));
    for (rstd::int32_t i = 0; i < entry_count; ++i) {
        auto path = ReadSizedString(pkg, usize(4096));
        if (! path) return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        auto key = LookupKey(Path(path->as_str()));
        if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());

        auto offset = pkg.ReadInt32();
        auto length = pkg.ReadInt32();
        if (offset < 0 || length < 0) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        files.push(PendingFile { .path   = rstd::move(key).unwrap_unchecked(),
                                 .offset = u64(static_cast<rstd::uint64_t>(offset)),
                                 .length = u64(static_cast<rstd::uint64_t>(length)) });
    }

    auto header_size = pkg.position();
    auto entries     = HashMap<String, PkgFile>::with_capacity(files.len());
    for (auto& file : files) {
        if (file.offset > u64::MAX - header_size) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        auto absolute = header_size + file.offset;
        if (absolute > pkg_source.len() || file.length > pkg_source.len() - absolute) {
            return rstd::Err(FsError(rstd::io::error::ErrorKind::InvalidData));
        }
        entries.insert(rstd::move(file.path),
                       PkgFile { .offset = absolute, .length = file.length });
    }

    auto version_string = rstd::move(*version);
    auto mount_version  = version_string.clone();
    auto mount          = MountHandle::make(
        WPPkgFs(rstd::move(pkg_source), rstd::move(version_string), rstd::move(entries)));
    return rstd::Ok(PkgMount(rstd::move(mount), rstd::move(mount_version)));
}

auto WPPkgFs::open_read(Path path) const -> rstd::io::Result<ReadRange> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    auto file = m_files.get(*key);
    if (file.is_none()) return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    return m_source.subrange((*file)->offset, (*file)->length);
}

auto WPPkgFs::open_write(Path path, WriteOptions) const -> rstd::io::Result<WriteSeekHandle> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    if (! m_files.contains_key(*key)) {
        return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    }
    return rstd::Err(FsError(rstd::io::error::ErrorKind::ReadOnlyFilesystem));
}

auto WPPkgFs::metadata(Path path) const -> rstd::io::Result<FileMetadata> {
    auto key = LookupKey(path);
    if (key.is_err()) return rstd::Err(rstd::move(key).unwrap_err_unchecked());
    auto file = m_files.get(*key);
    if (file.is_none()) return rstd::Err(FsError(rstd::io::error::ErrorKind::NotFound));
    return rstd::Ok(FileMetadata {
        .len = (*file)->length, .is_file = true, .is_directory = false, .readonly = true });
}
