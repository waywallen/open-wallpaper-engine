module;
export module wescene.pkg.parse:tex_image_parser;
import rstd;
import wescene.types;
import wescene.scene;
import wescene.fs;

using rstd::collections::HashMap;
using rstd::sync::Mutex;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe

{

// Sub-version stamps embedded in a `.tex` file. All four are read from
// independent "TEXX0000" stamps interleaved with the header / sprite
// payload (see `LoadHeader` and the sprite branch of `ParseHeader`).
//
// Observed corpus distribution (732 pkgs, 15399 textures across PKGV0001..23):
//   texv: always 5
//   texi: always 1
//   texb: 1 (PKGV0001 only; 4)  |  2 (early; 170)  |  3 (historic; 10096)  |  4 (PKGV0022+; 5129)
//   texs: 0 (non-sprite)  |  2 (early sprites; 15)  |  3 (current sprites; 491, including 90 with
//   texb=4)
//          texs == 1 is documented in legacy code but never observed.
//
// Full binary layout, by version. All ints are little-endian int32
// unless noted; "TEXX####" stamps are 9-byte ASCII strings.
//
//   ┌── texv stamp ("TEXV0005")
//   ├── texi stamp ("TEXI0001")
//   ├── format       int32  (0=RGBA8, 4=BC3, 6=BC2, 7=BC1, 8=RG8, 9=R8)
//   ├── flags        uint32 (bit0=noInterpolation bit1=clampUVs bit2=sprite
//   │                        bit20..23=compo1..4)
//   ├── width/height int32 ×2  pow-2 texture coord size (or pic size when not pow-2)
//   ├── map_w/h      int32 ×2  original picture size (== width/height when pow-2)
//   ├── reserved_a   int32  unused
//   ├── texb stamp ("TEXB0001"..0004)
//   ├── count        int32  number of image slots
//   ├── image_type   int32  if texb >= 3   (-1=UNKNOWN, FreeImage enum otherwise)
//   ├── condition_count uint32 if texb >= 4
//   ├── conditions: { id, operation, flags } uint32 x3 + NUL-terminated JSON
//   │
//   ├── per slot (× count):
//   │   ├── mip_count int32
//   │   └── per mip:
//   │       ├── mip_w/mip_h int32 ×2
//   │       ├── lz4_compressed   int32  if texb >= 2
//   │       ├── decompressed_sz  int32  if texb >= 2
//   │       ├── src_size         int32
//   │       └── src_size bytes (LZ4 if compressed; image-container body when
//   │           texb>=3 + image_type valid; raw pixel data otherwise)
//   │       └── if condition_count > 0: patch groups (workspace imhex/tex.hexpat)
//   │
//   └── if flags.sprite:
//       ├── texs stamp ("TEXS0001"..0003)  ← only valid texs values
//       ├── frame_count int32
//       ├── atlas_w/h   int32 ×2  if texs >= 3
//       └── per frame (× frame_count):
//           ├── image_id int32
//           ├── frametime float32
//           └── (x, y, xAxis[0..1], yAxis[0..1])  6 ×
//               int32 if texs == 1, float32 otherwise
//
// The predicate methods below collapse texb / texs version drift into a
// single source of truth so the parser body and the sprite branch share
// the same dispatch rules.
struct TexFormatVersion {
    rstd::int32_t texv { 0 };
    rstd::int32_t texi { 0 };
    rstd::int32_t texb { 0 };
    rstd::int32_t texs { 0 };

    // texb >= 2 — body has per-mip { LZ4_compressed, decompressed_size } prelude.
    constexpr bool body_has_lz4_prelude() const noexcept { return texb >= 2; }
    // texb >= 3 — header carries an int32 image_type slot before the mip body
    // (UNKNOWN/-1 for raw pixel data, or a FreeImage-style enum for png/jpg
    // containers). Pre-fix this was gated on `texb == 3`, which silently
    // dropped the slot for texb=4 and misaligned the entire body parse on
    // PKGV0022+ assets.
    constexpr bool body_has_image_type() const noexcept { return texb >= 3; }
    constexpr bool body_has_conditions() const noexcept { return texb >= 4; }
    // texs == 1 — sprite frame coordinates are int pixels (legacy; never
    // observed in our corpus). Otherwise floats.
    constexpr bool sprite_frame_coords_int() const noexcept { return texs == 1; }
    // texs >= 3 — sprite section carries an extra trailing { width, height }
    // pair after framecount (atlas dimensions).
    constexpr bool sprite_has_atlas_size() const noexcept { return texs >= 3; }
    constexpr bool valid() const noexcept { return texv != 0 && texi != 0 && texb != 0; }
};

auto ParseImages(ref<dyn<IImageParser>> parser, slice<String> names, usize max_workers = usize(4))
    -> Vec<Result<Arc<Image>, ImageParseError>>;

class TexImageParser {
public:
    TexImageParser(fs::VFS* vfs): m_vfs(vfs->Snapshot()) {}

    auto Parse(ref<str> name) const -> Result<Arc<Image>, ImageParseError>;
    auto ParseMany(slice<String> names) const -> Vec<Result<Arc<Image>, ImageParseError>>;
    auto ParseHeader(ref<str> name) const -> Result<ImageHeader, ImageParseError>;

private:
    struct PreparedHeader {
        ImageHeader         header;
        rstd::io::ReadRange source;
        TexFormatVersion    version;
        rstd::uint32_t      condition_count {};
        rstd::ptrdiff_t     body_offset {};
    };
    using HeaderCache = HashMap<String, Arc<PreparedHeader>>;
    auto PrepareHeader(ref<str> name) const -> Result<Arc<PreparedHeader>, ImageParseError>;

    Arc<fs::VFS>            m_vfs;
    Arc<Mutex<HeaderCache>> m_headers { Arc<Mutex<HeaderCache>>::make(HeaderCache {}) };
};
} // namespace owe
