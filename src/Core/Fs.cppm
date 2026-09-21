module;

#include <rstd/macro.hpp>

export module wescene.fs;
export import wescene.io;
export import wescene.vfs;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::fs
{

using BinaryReader = owe::io::BinaryReader;
using BinaryWriter = owe::io::BinaryWriter;

inline auto ResolveAssetPath(ref<str> path) -> rstd::io::Result<rstd::path::PathBuf> {
    return resolve_beneath(Path("/assets"_str), Path(path));
}

inline auto OpenBinary(VFS& vfs, Path path) -> rstd::io::Result<BinaryReader> {
    auto range = rstd_try(vfs.open_read(path));
    return Ok(BinaryReader(rstd::move(range)));
}

inline auto OpenBinaryWriter(VFS& vfs, Path path, WriteOptions options)
    -> rstd::io::Result<BinaryWriter> {
    auto handle = rstd_try(vfs.open_write(path, options));
    return Ok(BinaryWriter(rstd::move(handle)));
}

inline auto OpenPhysicalBinary(Path path) -> rstd::io::Result<BinaryReader> {
    auto opened   = rstd_try(rstd::fs::File::open(path));
    auto metadata = rstd_try(opened.metadata());
    auto source   = rstd::io::SharedReadAt::make(rstd::move(opened));
    auto range    = rstd_try(rstd::io::ReadRange::make(rstd::move(source), u64(), metadata.len()));
    return Ok(BinaryReader(rstd::move(range)));
}

inline auto ReadFileBytes(VFS& vfs, Path path) -> rstd::io::Result<Vec<u8>> {
    auto reader = rstd_try(OpenBinary(vfs, path));
    return reader.read_all_bytes();
}

inline auto ReadFileContent(VFS& vfs, Path path) -> rstd::io::Result<String> {
    auto reader = rstd_try(OpenBinary(vfs, path));
    return reader.read_all_string();
}

} // namespace owe::fs
