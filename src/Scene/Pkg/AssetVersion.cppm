module;

#include <rstd/macro.hpp>

export module wescene.pkg_asset_version;
import wescene.core;
import rstd;

import rstd.log;

import wescene.fs;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe
{

rstd::int32_t ReadAssetVersion(ref<str> prefix, fs::BinaryReader& file) {
    char str_v[10] {};
    file.Read(str_v, 9);
    if (prefix.len() > usize(9)) return 0;
    for (usize i {}; i < prefix.len(); ++i)
        if (static_cast<rstd::uint8_t>(str_v[i.to_primitive()]) !=
            prefix.as_bytes()[i].to_primitive())
            return 0;

    rstd::size_t end = str_v[4] == '-' ? 5 : 4;
    while (end < 9 && str_v[end] >= '0' && str_v[end] <= '9') ++end;
    auto digits =
        slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(str_v + 4), usize(end - 4));
    auto parsed = rstd::from_str<i32>(rstd::str_::from_utf8_unchecked(digits));
    if (parsed.is_err()) {
        str_v[8] = '\0';
        rstd_error("read version of '{}' failed", str_v);
        return 0;
    }
    return parsed.unwrap().to_primitive();
}

void WriteAssetVersion(ref<str> prefix, fs::BinaryWriter& file, int ver) {
    array<u8, 9> bytes {};
    usize        offset;
    for (auto byte : prefix.as_bytes()) {
        if (offset == usize(4) || byte == u8()) break;
        bytes[offset++] = byte;
    }
    auto number = i64(ver);
    if (number < i64()) bytes[offset++] = u8('-');
    auto digits = rstd::format("{}", number.abs());
    for (auto width = digits.len(); width < usize(4); ++width) bytes[offset++] = u8('0');
    for (auto byte : digits.as_str().as_bytes()) {
        if (offset == usize(8)) break;
        bytes[offset++] = byte;
    }
    file.Write(bytes.data(), bytes.len().to_primitive());
}

rstd::int32_t ReadTexVersion(fs::BinaryReader& file) { return ReadAssetVersion("TEX"_str, file); }
rstd::int32_t ReadMdlVersion(fs::BinaryReader& file) { return ReadAssetVersion("MDL"_str, file); }

// DIY
rstd::int32_t ReadShaderCacheVersion(fs::BinaryReader& file) {
    return ReadAssetVersion("SPV"_str, file);
}
void WriteShaderCacheVersion(fs::BinaryWriter& file, int ver) {
    WriteAssetVersion("SPVS"_str, file, ver);
}

} // namespace owe
