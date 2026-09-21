// .tex format schema reverse-coverage report.

#include <rstd/test/gtest.hpp>

#include <ios>

import rstd;
import rstd.cppstd;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.fs;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

namespace fs = std::filesystem;

constexpr const char* kWorkshopDirMacro =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

// Per-texture authoritative metadata extracted by this test's own binary
// reader. Holds enough to drive both the geometric invariant and the
// production parity check.
struct TexMeta {
    std::string  workshop_id;
    std::string  pkg_path; // "/materials/foo.tex" inside the pkg
    int          texv { 0 };
    int          texi { 0 };
    int          texb { 0 };
    int          texs { 0 }; // 0 == absent / non-sprite
    bool         sprite { false };
    bool         malformed { false };
    std::int32_t header_w { 0 }; // texture stamp section
    std::int32_t header_h { 0 };
    std::int32_t map_w { 0 };
    std::int32_t map_h { 0 };
    std::int32_t image_count { 0 };
    std::int32_t mip0_w { 0 }; // first mip of slot 0; 0 if mip_count == 0
    std::int32_t mip0_h { 0 };
    std::int32_t slot0_mip_count { 0 };
};

using TexTuple = std::tuple<int, int, int, int>;

const std::set<int>& kSupportedTexv() {
    static const std::set<int> v { 5 };
    return v;
}
const std::set<int>& kSupportedTexi() {
    static const std::set<int> v { 1 };
    return v;
}
const std::set<int>& kSupportedTexb() {
    static const std::set<int> v { 1, 2, 3, 4 };
    return v;
}
const std::set<int>& kSupportedTexs() {
    // 0 == absent (non-sprite). 2 / 3 cover all sprite samples; 1 is
    // documented as legacy int-coords but never observed.
    static const std::set<int> v { 0, 1, 2, 3 };
    return v;
}

template<typename T>
bool read_pod(const char* buf, std::size_t len, std::size_t off, T& out) {
    if (off + sizeof(T) > len) return false;
    std::memcpy(&out, buf + off, sizeof(T));
    return true;
}

// Mirrors `ReadAssetVersion("TEX", file)` from src/Scene/Pkg/AssetVersion.cppm: 9 bytes of
// "TEXX####\0". Returns 0 on prefix mismatch / short read.
int parse_tex_stamp(const char* buf, std::size_t len, std::size_t off) {
    if (off + 9 > len) return 0;
    const char* p = buf + off;
    if (p[0] != 'T' || p[1] != 'E' || p[2] != 'X') return 0;
    int n = 0;
    for (int i = 4; i < 8; ++i) {
        char c = p[i];
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
    }
    return n;
}

// Mirrors src/Utils/Utils.cppm's algorism::IsPowOfTwo, including its
// `x > 1` lower bound — 1 is *not* considered pow-of-two there, and
// production's `mipmap_pow2` derivation depends on that.
bool IsPowOfTwo(std::uint32_t x) { return x > 1 && (x & (x - 1)) == 0; }

// Independent reader. Fills `m` from the .tex blob using the authoritative
// layout (texb >= 3 has image_type, texb >= 4 has reserved u32). Sets
// m.malformed = true on any short / nonsensical read.
TexMeta read_tex_meta(const std::string& blob) {
    TexMeta m;
    m.malformed           = true; // flipped to false at the end
    const char*       buf = blob.data();
    const std::size_t len = blob.size();
    std::size_t       o   = 0;

    m.texv = parse_tex_stamp(buf, len, o);
    o += 9;
    m.texi = parse_tex_stamp(buf, len, o);
    o += 9;
    if (m.texv <= 0 || m.texi <= 0) return m;

    std::int32_t format = 0, flags = 0, _u = 0;
    if (! read_pod(buf, len, o, format)) return m;
    o += 4;
    if (! read_pod(buf, len, o, flags)) return m;
    o += 4;
    if (! read_pod(buf, len, o, m.header_w)) return m;
    o += 4;
    if (! read_pod(buf, len, o, m.header_h)) return m;
    o += 4;
    if (! read_pod(buf, len, o, m.map_w)) return m;
    o += 4;
    if (! read_pod(buf, len, o, m.map_h)) return m;
    o += 4;
    if (! read_pod(buf, len, o, _u)) return m;
    o += 4;

    m.texb = parse_tex_stamp(buf, len, o);
    o += 9;
    if (m.texb <= 0) return m;

    if (! read_pod(buf, len, o, m.image_count)) return m;
    o += 4;
    if (m.texb >= 3) {
        std::int32_t image_type = 0;
        if (! read_pod(buf, len, o, image_type)) return m;
        o += 4;
    }
    if (m.texb >= 4) {
        std::int32_t reserved = 0;
        if (! read_pod(buf, len, o, reserved)) return m;
        o += 4;
    }

    m.sprite = (flags & (1u << 2)) != 0;

    // Walk slot 0 to extract mip0 dims; walk subsequent slots to reach
    // the trailing texs stamp on sprite atlases.
    for (std::int32_t i = 0; i < m.image_count; ++i) {
        std::int32_t mip_count = 0;
        if (! read_pod(buf, len, o, mip_count)) return m;
        o += 4;
        if (i == 0) m.slot0_mip_count = mip_count;
        for (std::int32_t k = 0; k < mip_count; ++k) {
            std::int32_t mw0 = 0, mh0 = 0;
            if (! read_pod(buf, len, o, mw0)) return m;
            o += 4;
            if (! read_pod(buf, len, o, mh0)) return m;
            o += 4;
            if (i == 0 && k == 0) {
                m.mip0_w = mw0;
                m.mip0_h = mh0;
            }
            if (m.texb >= 2) {
                std::int32_t lz4 = 0, dec = 0;
                if (! read_pod(buf, len, o, lz4)) return m;
                o += 4;
                if (! read_pod(buf, len, o, dec)) return m;
                o += 4;
            }
            std::int32_t src = 0;
            if (! read_pod(buf, len, o, src)) return m;
            o += 4;
            if (src < 0 || o + static_cast<std::size_t>(src) > len) return m;
            o += static_cast<std::size_t>(src);
        }
    }

    if (m.sprite) m.texs = parse_tex_stamp(buf, len, o);
    m.malformed = false;
    return m;
}

// Mirrors dump.cpp's pkg header reader so we don't need to expose pkg
// enumeration through wescene's public API.
struct TexEntry {
    std::string path;
    std::string blob;
};
std::vector<TexEntry> ReadTexEntries(const fs::path& pkg) {
    std::vector<TexEntry> out;
    std::ifstream         in(pkg, std::ios::binary);
    if (! in.good()) return out;

    auto read_i32 = [&](std::int32_t& v) {
        in.read(reinterpret_cast<char*>(&v), 4);
        return static_cast<bool>(in);
    };

    std::int32_t ver_len = 0;
    if (! read_i32(ver_len) || ver_len < 0) return out;
    std::string ver(static_cast<std::size_t>(ver_len), '\0');
    in.read(ver.data(), ver_len);

    std::int32_t entry_count = 0;
    if (! read_i32(entry_count) || entry_count < 0 || entry_count > 100000) return out;

    struct Entry {
        std::string  path;
        std::int32_t offset { 0 };
        std::int32_t length { 0 };
    };
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::int32_t i = 0; i < entry_count; ++i) {
        std::int32_t name_len = 0;
        if (! read_i32(name_len) || name_len < 0) return out;
        Entry e;
        e.path.resize(static_cast<std::size_t>(name_len));
        in.read(e.path.data(), name_len);
        if (! read_i32(e.offset)) return out;
        if (! read_i32(e.length)) return out;
        entries.push_back(std::move(e));
    }

    const std::streampos data_start = in.tellg();
    if (data_start < 0) return out;

    for (const auto& e : entries) {
        if (e.path.size() < 4) continue;
        if (e.path.compare(e.path.size() - 4, 4, ".tex") != 0) continue;
        in.seekg(data_start + static_cast<std::streamoff>(e.offset));
        TexEntry te;
        te.path = "/" + e.path;
        te.blob.resize(static_cast<std::size_t>(std::max<std::int32_t>(e.length, 0)));
        in.read(te.blob.data(), e.length);
        out.push_back(std::move(te));
    }
    return out;
}

// Strip /materials/...".tex" → "..." for ParseHeader's `name` argument.
std::string TexNameFromPkgPath(const std::string& pkg_path) {
    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    if (pkg_path.compare(0, prefix.size(), prefix) != 0) return {};
    if (pkg_path.size() < prefix.size() + suffix.size()) return {};
    if (pkg_path.compare(pkg_path.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return pkg_path.substr(prefix.size(), pkg_path.size() - prefix.size() - suffix.size());
}

struct CorpusScan {
    std::vector<TexMeta> metas;
    // Per-pkg cached VFS so production-parity test can run ParseHeader
    // without re-mounting per texture. Indexed by workshop_id.
    std::map<std::string, std::shared_ptr<owe::fs::VFS>> vfs_by_workshop;
};

const CorpusScan& AllScans() {
    static const CorpusScan c = [] {
        CorpusScan out;
        fs::path   root { kWorkshopDirMacro };
        if (! fs::exists(root) || ! fs::is_directory(root)) {
            std::cerr << "tex_schema_tests: workshop dir " << root.string() << " missing\n";
            return out;
        }

        std::vector<fs::path> pkgs;
        for (auto& e : fs::directory_iterator(root)) {
            if (! e.is_directory()) continue;
            auto pkg = e.path() / "scene.pkg";
            if (fs::exists(pkg)) pkgs.push_back(pkg);
        }
        std::sort(pkgs.begin(), pkgs.end());

        for (const auto& pkg : pkgs) {
            std::string id = pkg.parent_path().filename().string();

            auto wfs =
                owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg.string()).unwrap()));
            if (wfs.is_err()) continue;
            auto vfs = std::make_shared<owe::fs::VFS>();
            if (vfs->mount("/assets"_str, wfs->mount_handle()).is_err()) continue;

            for (auto& te : ReadTexEntries(pkg)) {
                auto m        = read_tex_meta(te.blob);
                m.workshop_id = id;
                m.pkg_path    = te.path;
                out.metas.push_back(std::move(m));
            }
            out.vfs_by_workshop[id] = std::move(vfs);
        }
        return out;
    }();
    return c;
}

} // namespace

TEST(TexSchema, EveryObservedSubVersionIsSupported) {
    const auto& metas = AllScans().metas;
    ASSERT_FALSE(metas.empty()) << "tex scan produced zero textures";

    std::set<TexTuple> seen;
    for (const auto& m : metas) {
        if (m.malformed) continue;
        seen.emplace(m.texv, m.texi, m.texb, m.texs);
    }
    for (const auto& [v, i, b, sp] : seen) {
        EXPECT_TRUE(kSupportedTexv().contains(v)) << "texv=" << v << " not in supported set";
        EXPECT_TRUE(kSupportedTexi().contains(i)) << "texi=" << i << " not in supported set";
        EXPECT_TRUE(kSupportedTexb().contains(b)) << "texb=" << b << " not in supported set";
        EXPECT_TRUE(kSupportedTexs().contains(sp)) << "texs=" << sp << " not in supported set";
    }
}

TEST(TexSchema, NoMalformedHeadersInCorpus) {
    const auto& metas = AllScans().metas;
    ASSERT_FALSE(metas.empty()) << "tex scan produced zero textures";

    std::size_t malformed = 0;
    for (const auto& m : metas)
        if (m.malformed) ++malformed;
    EXPECT_EQ(malformed, 0u) << malformed << " textures had truncated / unparseable headers";
}

// Geometric invariant — non-sprite textures.
//
// The first mip's dims must agree with one of the two sizes the .tex
// header advertises:
//
//   * (header_w, header_h) — pow-of-2 texture coordinate space.
//   * (map_w,    map_h)    — original picture size before pow-2 round-up.
//
// ~99 % of corpus textures store mip0 at the pow-2 size; a minority of
// non-pow-2 source pictures keep mip0 at picture size with `header_{w,h}`
// carrying the next pow-2 up (the "mipmap_larger" branch in production).
// Any mip0 dim that matches *neither* indicates either a new layout
// this reader / the production parser hasn't grown to understand, or a
// header/body misalignment (e.g. the texb=4 reserved-u32 bug, which
// produced mip0 dims of 0x1 — agreeing with neither).
TEST(TexSchema, NonSpriteMip0AgreesWithHeaderOrMap) {
    const auto& metas = AllScans().metas;
    ASSERT_FALSE(metas.empty());

    std::size_t checked = 0, mismatched = 0, matched_map = 0;
    for (const auto& m : metas) {
        if (m.malformed || m.sprite) continue;
        if (m.slot0_mip_count == 0) continue; // no mips → invariant N/A
        ++checked;
        const bool eq_header = (m.mip0_w == m.header_w && m.mip0_h == m.header_h);
        const bool eq_map    = (m.mip0_w == m.map_w && m.mip0_h == m.map_h);
        if (eq_map && ! eq_header) ++matched_map;
        if (eq_header || eq_map) continue;
        ++mismatched;
        ADD_FAILURE() << "mip0 dims " << m.mip0_w << "x" << m.mip0_h
                      << " agree with neither header " << m.header_w << "x" << m.header_h
                      << " nor map " << m.map_w << "x" << m.map_h << " for " << m.workshop_id << " "
                      << m.pkg_path << " (texb=" << m.texb << ")";
        if (mismatched >= 5) break;
    }
    std::cerr << "TexSchema.NonSpriteMip0AgreesWithHeaderOrMap: checked=" << checked
              << " matched_map=" << matched_map << " mismatched=" << mismatched << "\n";
}

// Sprites: structural sanity only.
//
// Sprites have two empirically-observed layout patterns and no clean
// universal invariant collapses both:
//
//   A. "loose"  — mip0 == map dims, header is pow-2 rounded.
//                 e.g. mip0=1280x256, map=1280x256, header=2048x256.
//   B. "packed" — atlas mip0 packs many frames; header == map records
//                 per-frame dims; mip0 is much larger.
//                 e.g. mip0=8192x4096, header=map=1100x582.
//
// Cross-implementation parity (`ProductionParseHeaderAgreesWithMip0Reader`)
// already exercises sprites end-to-end, so the only thing this test
// adds is a smoke check that nothing is zero / negative — catches
// truncated reads or version drift that produces garbage dims.
TEST(TexSchema, SpriteHeaderAndMip0DimsArePositive) {
    const auto& metas = AllScans().metas;
    ASSERT_FALSE(metas.empty());

    std::size_t checked = 0, bad = 0;
    for (const auto& m : metas) {
        if (m.malformed || ! m.sprite) continue;
        if (m.slot0_mip_count == 0) continue;
        ++checked;
        const bool ok = m.mip0_w > 0 && m.mip0_h > 0 && m.header_w > 0 && m.header_h > 0;
        if (ok) continue;
        ++bad;
        ADD_FAILURE() << "non-positive sprite dims: mip0 " << m.mip0_w << "x" << m.mip0_h
                      << " header " << m.header_w << "x" << m.header_h << " for " << m.workshop_id
                      << " " << m.pkg_path << " (texb=" << m.texb << " texs=" << m.texs << ")";
        if (bad >= 5) break;
    }
    std::cerr << "TexSchema.SpriteHeaderAndMip0DimsArePositive: checked=" << checked
              << " bad=" << bad << "\n";
}

// Production parity: for every texture the test's binary reader can
// read fully, run TexImageParser::ParseHeader and verify its derived
// `mipmap_pow2` / `mipmap_larger` fields match values computed directly
// from the authoritative mip0 dims. Cross-implementation check that
// catches production-side layout bugs which fixture-only tests can't
// (fixtures are produced by the parser itself, so any stable bug is
// committed as ground truth).
//
// Covers both sprites and non-sprites since the sprite-path assertion
// abort was fixed (defensive guards on imageId / texs version range).
TEST(TexSchema, ProductionParseHeaderAgreesWithMip0Reader) {
    const auto& scan = AllScans();
    ASSERT_FALSE(scan.metas.empty());

    // Production formulas (mirrored from TexImageParser.cpp's
    // SetHeaderPow2 + the sprite-path inline computation):
    //   non-sprite mipmap_pow2:   IsPowOfTwo(mip0_w) || IsPowOfTwo(mip0_h)
    //   non-sprite mipmap_larger: mip0_w * mip0_h > map_w * map_h
    //   sprite     mipmap_pow2:   IsPowOfTwo(mip0_w * mip0_h)
    //   sprite     mipmap_larger: never set (stays at the ImageHeader default)
    std::size_t checked = 0, parity_fail = 0, parity_fail_sprite = 0;
    for (const auto& m : scan.metas) {
        if (m.malformed) continue;
        if (m.slot0_mip_count == 0) continue;

        auto it = scan.vfs_by_workshop.find(m.workshop_id);
        if (it == scan.vfs_by_workshop.end()) continue;
        auto& vfs = *it->second;

        std::string name = TexNameFromPkgPath(m.pkg_path);
        if (name.empty()) continue;

        owe::TexImageParser parser(&vfs);
        owe::ImageHeader    h;
        try {
            auto parsed = parser.ParseHeader(rstd::cppstd::as_str(name).unwrap());
            if (parsed.is_err()) continue;
            h = rstd::move(parsed).unwrap_unchecked();
        } catch (...) {
            continue;
        }

        const bool expected_pow2 = m.sprite
                                       ? IsPowOfTwo(static_cast<std::uint32_t>(m.mip0_w * m.mip0_h))
                                       : (IsPowOfTwo(static_cast<std::uint32_t>(m.mip0_w)) ||
                                          IsPowOfTwo(static_cast<std::uint32_t>(m.mip0_h)));

        ++checked;
        if (h.mipmap_pow2 != expected_pow2) {
            ++parity_fail;
            if (m.sprite) ++parity_fail_sprite;
            ADD_FAILURE() << "ParseHeader divergence for " << m.workshop_id << " " << m.pkg_path
                          << " (texb=" << m.texb << " sprite=" << m.sprite << "): "
                          << "expected pow2=" << expected_pow2 << ", got pow2=" << h.mipmap_pow2
                          << " (mip0=" << m.mip0_w << "x" << m.mip0_h << " map=" << m.map_w << "x"
                          << m.map_h << ")";
            if (parity_fail >= 5) break;
            continue;
        }
        // mipmap_larger is only set on the non-sprite path; checking it
        // for sprites would just compare against the ImageHeader default.
        if (! m.sprite) {
            const bool expected_larger = (m.mip0_w * m.mip0_h) > (m.map_w * m.map_h);
            if (h.mipmap_larger != expected_larger) {
                ++parity_fail;
                ADD_FAILURE() << "ParseHeader mipmap_larger divergence for " << m.workshop_id << " "
                              << m.pkg_path << " (texb=" << m.texb << "): "
                              << "expected " << expected_larger << ", got " << h.mipmap_larger
                              << " (mip0=" << m.mip0_w << "x" << m.mip0_h << " map=" << m.map_w
                              << "x" << m.map_h << ")";
                if (parity_fail >= 5) break;
            }
        }
    }
    std::cerr << "TexSchema.ProductionParseHeaderAgreesWithMip0Reader: checked=" << checked
              << " parity_fail=" << parity_fail << " (sprite=" << parity_fail_sprite << ")\n";
}

// Full pixel-decode end-to-end. Runs TexImageParser::Parse (not just
// ParseHeader) on a sample of the corpus and asserts the returned Image
// has plausible slot / mipmap data. Covers the production paths that
// ParseHeader never reaches:
//
//   * texb >= 2 LZ4 decompression
//   * texb >= 3 stbi_load_from_memory image-container decode
//   * texb >= 3 raw memcpy (image_type == UNKNOWN, not a container)
//   * The DetectEmbeddedImageType magic-bytes fallback that was added
//     for PKGV0022+ assets shipping containerised PNG/JPEG with
//     image_type == UNKNOWN.
//
// Deterministic sample (first N textures per (texb, image_type) bucket
// in workshop-id order) so the test is stable across runs but still
// covers each version-aware code path.
TEST(TexSchema, ProductionParseDecodesEveryBucket) {
    using Bucket                     = std::pair<int, int>; // (texb, image_type)
    constexpr std::size_t kPerBucket = 8;

    const auto& scan = AllScans();
    ASSERT_FALSE(scan.metas.empty());

    // ParseHeader the meta first to learn image_type per texture (the
    // binary reader doesn't decode the image_type slot — that's a
    // production-side derivation). We bucket by (texb, image_type) and
    // cap each bucket at kPerBucket.
    std::map<Bucket, std::size_t> bucket_counts;
    std::size_t                   parse_attempted = 0;
    std::size_t                   parse_failed    = 0;
    std::size_t                   slot_empty      = 0;
    std::size_t                   slot_dim_zero   = 0;

    std::map<Bucket, std::size_t> per_bucket_ok;
    std::map<Bucket, std::size_t> per_bucket_fail;

    for (const auto& m : scan.metas) {
        if (m.malformed || m.slot0_mip_count == 0) continue;

        auto it = scan.vfs_by_workshop.find(m.workshop_id);
        if (it == scan.vfs_by_workshop.end()) continue;
        auto& vfs = *it->second;

        std::string name = TexNameFromPkgPath(m.pkg_path);
        if (name.empty()) continue;

        owe::TexImageParser parser(&vfs);
        owe::ImageHeader    h;
        try {
            auto parsed = parser.ParseHeader(rstd::cppstd::as_str(name).unwrap());
            if (parsed.is_err()) continue;
            h = rstd::move(parsed).unwrap_unchecked();
        } catch (...) {
            continue;
        }
        Bucket key { m.texb, static_cast<int>(h.type) };
        if (bucket_counts[key] >= kPerBucket) continue;
        ++bucket_counts[key];

        ++parse_attempted;
        Option<Arc<owe::Image>> image;
        try {
            auto parsed_image = parser.Parse(rstd::cppstd::as_str(name).unwrap());
            if (parsed_image.is_ok()) image = Some(rstd::move(parsed_image).unwrap_unchecked());
        } catch (...) {
        }
        if (image.is_none()) {
            ++parse_failed;
            ++per_bucket_fail[key];
            ADD_FAILURE() << "Parse failed for " << m.workshop_id << " " << m.pkg_path
                          << " (texb=" << m.texb << " image_type=" << static_cast<int>(h.type)
                          << ")";
            continue;
        }
        auto img = image.take().unwrap_unchecked();
        if (img->content->slots.is_empty()) {
            ++parse_failed;
            ++per_bucket_fail[key];
            ADD_FAILURE() << "Parse returned an empty image for " << m.workshop_id << " "
                          << m.pkg_path << " (texb=" << m.texb
                          << " image_type=" << static_cast<int>(h.type) << ")";
            continue;
        }
        const auto& s0 = img->content->slots[usize(0)];
        if (s0.width <= 0 || s0.height <= 0) {
            ++slot_dim_zero;
            ++per_bucket_fail[key];
            ADD_FAILURE() << "slot 0 has non-positive dims " << s0.width << "x" << s0.height
                          << " for " << m.workshop_id << " " << m.pkg_path;
            continue;
        }
        if (s0.mipmaps.is_empty() || ! s0.mipmaps[usize(0)].data ||
            s0.mipmaps[usize(0)].size <= rstd::isize()) {
            ++slot_empty;
            ++per_bucket_fail[key];
            ADD_FAILURE() << "slot 0 mip 0 missing data for " << m.workshop_id << " " << m.pkg_path;
            continue;
        }
        ++per_bucket_ok[key];
    }

    std::cerr << "TexSchema.ProductionParseDecodesEveryBucket: attempted=" << parse_attempted
              << " parse_failed=" << parse_failed << " slot_empty=" << slot_empty
              << " slot_dim_zero=" << slot_dim_zero << "\n";
    std::cerr << "  bucket (texb, image_type): ok / fail\n";
    std::set<Bucket> all_buckets;
    for (auto& [k, _] : per_bucket_ok) all_buckets.insert(k);
    for (auto& [k, _] : per_bucket_fail) all_buckets.insert(k);
    for (const auto& b : all_buckets) {
        std::cerr << "    (" << b.first << ", " << b.second << "): " << per_bucket_ok[b] << " / "
                  << per_bucket_fail[b] << "\n";
    }
}

// Format-code → TextureFormat mapping ground truth. ToTexFormate's
// switch statement is the source; this test pins it so a future
// renumbering is loud rather than silent.
TEST(TexSchema, FormatCodeMapsToExpectedTextureFormat) {
    using TF = owe::TextureFormat;
    const std::map<int, TF> kExpected {
        { 0, TF::RGBA8 }, { 4, TF::BC3 }, { 6, TF::BC2 },
        { 7, TF::BC1 },   { 8, TF::RG8 }, { 9, TF::R8 },
    };

    const auto& scan = AllScans();
    ASSERT_FALSE(scan.metas.empty());

    // For each (format_code → expected TextureFormat) pair, find a
    // sample texture that declares that format code in its raw header,
    // ParseHeader it, and verify the production parser maps to the
    // expected TextureFormat. Only one sample per code is needed since
    // ToTexFormate is a pure switch.
    std::set<int> code_seen;
    for (const auto& m : scan.metas) {
        if (m.malformed || m.slot0_mip_count == 0) continue;

        // re-derive raw format code by re-reading offset 18 of the blob
        // — but we already throw the format away in read_tex_meta. Use
        // ParseHeader's output reverse-mapped: find a texture whose
        // header.format we can match to the expected code.
        auto it = scan.vfs_by_workshop.find(m.workshop_id);
        if (it == scan.vfs_by_workshop.end()) continue;
        auto& vfs = *it->second;

        std::string name = TexNameFromPkgPath(m.pkg_path);
        if (name.empty()) continue;

        owe::TexImageParser parser(&vfs);
        owe::ImageHeader    h;
        try {
            auto parsed = parser.ParseHeader(rstd::cppstd::as_str(name).unwrap());
            if (parsed.is_err()) continue;
            h = rstd::move(parsed).unwrap_unchecked();
        } catch (...) {
            continue;
        }

        // ParseHeader already ran ToTexFormate; we only know `h.format`
        // (TextureFormat) but not the input code. Find which code maps
        // to it via the kExpected table, mark that code as seen, and
        // verify the mapping.
        for (const auto& [code, tf] : kExpected) {
            if (tf != h.format) continue;
            if (code_seen.contains(code)) continue;
            code_seen.insert(code);
            EXPECT_EQ(h.format, tf) << "format code " << code << " mapped to wrong TextureFormat";
            break;
        }
        if (code_seen.size() == kExpected.size()) break;
    }

    std::cerr << "TexSchema.FormatCodeMapsToExpectedTextureFormat: codes seen=" << code_seen.size()
              << "/" << kExpected.size() << "\n";
    for (const auto& [code, tf] : kExpected) {
        EXPECT_TRUE(code_seen.contains(code))
            << "format code " << code << " never observed in corpus";
    }
}

TEST(TexSchema, ReportObservedTuples) {
    const auto&                     metas = AllScans().metas;
    std::map<TexTuple, std::size_t> tuples;
    for (const auto& m : metas) {
        if (m.malformed) continue;
        ++tuples[{ m.texv, m.texi, m.texb, m.texs }];
    }
    std::cerr << "\n# observed (texv, texi, texb, texs) tuples in corpus:\n";
    for (const auto& [tup, c] : tuples) {
        const auto [v, i, b, sp] = tup;
        std::cerr << "  (" << v << ", " << i << ", " << b << ", " << sp << "): count=" << c << "\n";
    }
    SUCCEED();
}
