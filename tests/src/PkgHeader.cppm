// Shared scene.pkg header re-parser used by the test/CLI tools.
//
// We deliberately re-parse scene.pkg's header here instead of reaching into
// WPPkgFs internals: the production class throws away the version string
// after logging it and exposes neither file enumeration nor the version.

module;

export module wescene.testing.pkg_header;

import rstd.cppstd;
import wescene.fs;
using namespace rstd::literals;

export namespace owe::testing
{

struct PkgEntry {
    std::string  path;
    std::int32_t offset { 0 };
    std::int32_t length { 0 };
};

// Reads the header of a scene.pkg-format file. `version` is filled with the
// stamp (e.g. "PKGV0001") and `entries` lists every file with its absolute
// in-pkg path (leading slash) and (offset, length) into the body.
// Returns false on stream open / shape errors.
bool ReadPkgHeader(const std::string& pkg_path, std::string& version,
                   std::vector<PkgEntry>& entries);

} // namespace owe::testing

namespace owe::testing
{

namespace
{

std::string ReadSizedString(owe::fs::BinaryReader& f) {
    std::int32_t len = f.ReadInt32();
    if (len < 0) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(len));
    f.Read(out.data(), static_cast<std::size_t>(len));
    return out;
}

} // namespace

bool ReadPkgHeader(const std::string& pkg_path, std::string& version,
                   std::vector<PkgEntry>& entries) {
    auto stream =
        owe::fs::OpenPhysicalBinary(owe::fs::Path(rstd::cppstd::as_str(pkg_path).unwrap()));
    if (stream.is_err()) return false;
    version            = ReadSizedString(*stream);
    std::int32_t count = stream->ReadInt32();
    if (count < 0) return false;
    entries.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        PkgEntry e;
        e.path   = "/" + ReadSizedString(*stream);
        e.offset = stream->ReadInt32();
        e.length = stream->ReadInt32();
        entries.push_back(std::move(e));
    }
    return true;
}

} // namespace owe::testing
