module;

#include <rstd/macro.hpp>
#include <lz4.h>

module wescene.pkg.parse;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import wescene.utils;
import wescene.scene;
import wescene.pkg_asset_version;
import wescene.stb_image;
import wescene.json;

using rstd::mem::memcmp;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CString;
using rstd::os::unix::ffi::OsStrExt;
using rstd::sync::Arc;

enum class TexFlagEnum : rstd::uint32_t
{
    // true for no bilinear
    noInterpolation = 0,
    // true for no repeat
    clampUVs = 1,
    sprite   = 2,

    compo1 = 20,
    compo2 = 21,
    compo3 = 22,
    compo4 = 23
};
using TexFlags = BitFlags<TexFlagEnum>;

namespace
{
class DecodedImageLease {
public:
    explicit DecodedImageLease(Arc<Mutex<ImageDecodeStatistics>> statistics)
        : m_statistics(rstd::move(statistics)) {
        auto stats = m_statistics->lock().unwrap_unchecked();
        ++stats->current_count;
        stats->peak_count = rstd::cmp::max(stats->peak_count, stats->current_count);
    }
    ~DecodedImageLease() {
        auto stats = m_statistics->lock().unwrap_unchecked();
        --stats->current_count;
    }
    void Acquire(u64 bytes) {
        auto stats = m_statistics->lock().unwrap_unchecked();
        stats->current_bytes += bytes;
        stats->total_bytes += bytes;
        stats->peak_bytes = rstd::cmp::max(stats->peak_bytes, stats->current_bytes);
    }
    void Release(u64 bytes) {
        auto stats = m_statistics->lock().unwrap_unchecked();
        stats->current_bytes -= bytes;
    }

private:
    Arc<Mutex<ImageDecodeStatistics>> m_statistics;
};

template<typename Release>
auto OwnDecodedPixels(rstd::uint8_t* pixels, u64 bytes, Release release,
                      Option<Arc<DecodedImageLease>>&          lease,
                      const Arc<Mutex<ImageDecodeStatistics>>& statistics) -> ImageDataPtr {
    if (! lease) lease = Some(Arc<DecodedImageLease>::make(statistics.clone()));
    auto owner = ImageDataPtr(
        pixels,
        [hold = lease->clone(), bytes, release = rstd::move(release)](rstd::uint8_t* data) mutable {
            release(data);
            hold->Release(bytes);
        });
    (*lease)->Acquire(bytes);
    return owner;
}

auto CloneImageResult(const Result<Arc<Image>, ImageParseError>& result)
    -> Result<Arc<Image>, ImageParseError> {
    if (result.is_ok()) return Ok(result->clone());
    const auto& error = result.as_ref().unwrap_err_unchecked();
    return Err(ImageParseError { .kind = error.kind, .message = error.message.clone() });
}

// Magic-bytes sniffer used as a fallback when the .tex header's
// `image_type` slot says UNKNOWN but the body is actually an embedded
// image container. Some PKGV0022+ assets ship this way (the texture's
// declared image_type is -1 even though the LZ4-decompressed payload
// is a self-contained PNG/JPEG). Without this fallback the body bytes
// are memcpy'd into a "raw RGBA8" slot, which decodes to garbage and
// the wallpaper renders as a flat clear-color screen.
ImageType DetectEmbeddedImageType(const unsigned char* data, rstd::size_t size) {
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", rstd::usize(8)) == 0) return ImageType::PNG;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ImageType::JPEG;
    if (size >= 6 && (memcmp(data, "GIF87a", rstd::usize(6)) == 0 ||
                      memcmp(data, "GIF89a", rstd::usize(6)) == 0))
        return ImageType::GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return ImageType::BMP;
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2a && data[3] == 0x00) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2a)))
        return ImageType::TIFF;
    // ISO BMFF / MP4 / MOV / 3GP — "....ftyp...." at offset 4. WE's scene
    // wallpapers can inline an H.264/AAC mp4 here; the renderer side
    // hands the bytes to wavsen::video::VideoDecoder.
    if (size >= 12 && memcmp(data + 4, "ftyp", rstd::usize(4)) == 0) return ImageType::VIDEO;
    // Matroska / WebM EBML header.
    if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3)
        return ImageType::VIDEO;
    return ImageType::UNKNOWN;
}

TextureFormat ToTexFormate(int type) {
    /*
        type
        RGBA8888 = 0,
        DXT5 = 4,
        DXT3 = 6,
        DXT1 = 7,
        RG88 = 8,
        R8 = 9,
    */
    switch (type) {
    case 0: return TextureFormat::RGBA8;
    case 4: return TextureFormat::BC3;
    case 6: return TextureFormat::BC2;
    case 7: return TextureFormat::BC1;
    case 8: return TextureFormat::RG8;
    case 9: return TextureFormat::R8;
    default:
        rstd_error("ERROR::ToTexFormate Unkown image type: {}", type);
        return TextureFormat::RGBA8;
    }
}
struct TexBodyHeader {
    TexFormatVersion version;
    rstd::uint32_t   condition_count {};
    Vec<u32>         selected_patches;
    bool             unsupported_conditions {};
};

auto InvalidTextureData(ref<str> name, ref<str> section) -> ImageParseError {
    return { .kind    = ImageParseErrorKind::InvalidData,
             .message = rstd::format("texture {} has invalid {}", name, section) };
}

using TextureBytes = Vec<u8, rstd::alloc::SharedAllocator>;

auto ImageAllocationError(ref<str> name) -> ImageParseError {
    return { .kind    = ImageParseErrorKind::OutOfMemory,
             .message = rstd::format("texture {} allocation failed", name) };
}

auto DecoderError(ref<str> name, stb::Error error) -> ImageParseError {
    if (error == stb::Error::OutOfMemory) return ImageAllocationError(name);
    return { .kind    = ImageParseErrorKind::DecodeFailed,
             .message = rstd::format("decode image {} failed", name) };
}

auto ReadTextureBytes(fs::BinaryReader& reader, ref<str> name, const TextureMemoryOptions& memory)
    -> Result<TextureBytes, ImageParseError> {
    auto input = reader.read_all_bytes_in(memory.bytes.clone());
    if (input.is_err()) {
        if (input.as_ref().unwrap_err().kind() ==
            rstd::io::ErrorKind { rstd::io::ErrorKind::OutOfMemory })
            return Err(ImageAllocationError(name));
        return Err(InvalidTextureData(name, "mip payload"_str));
    }
    return Ok(rstd::move(input).unwrap());
}

auto ReadMipBytes(rstd::io::ReadRange source, bool compressed, int decoded_size, ref<str> name,
                  const TextureMemoryOptions& memory) -> Result<TextureBytes, ImageParseError> {
    fs::BinaryReader reader(rstd::move(source));
    auto             input = rstd_try(ReadTextureBytes(reader, name, memory));
    if (! compressed) return Ok(rstd::move(input));
    if (decoded_size < 0 || input.len() > usize(i32::MAX.to_primitive()))
        return Err(InvalidTextureData(name, "LZ4 size"_str));
    auto output = TextureBytes::new_in(memory.bytes.clone());
    if (output.try_reserve_exact(usize(decoded_size)).is_err())
        return Err(ImageAllocationError(name));
    auto size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(input.data()),
        reinterpret_cast<char*>(output.spare_capacity_mut().as_uninit().as_raw_ptr()),
        static_cast<int>(input.len().to_primitive()),
        decoded_size);
    if (size != decoded_size) return Err(InvalidTextureData(name, "LZ4 payload"_str));
    output.set_len_unchecked(usize(size));
    return Ok(rstd::move(output));
}

constexpr stb::Callbacks ImageCallbacks {
    [](void* user, char* data, int size) -> int {
        return static_cast<int>(static_cast<fs::BinaryReader*>(user)->Read(data, size));
    },
    [](void* user, int count) {
        (void)static_cast<fs::BinaryReader*>(user)->SeekCur(count);
    },
    [](void* user) -> int {
        return static_cast<fs::BinaryReader*>(user)->remaining() == u64();
    },
};

auto DescribeMip(rstd::io::ReadRange source, bool compressed, int decoded_size, ImageType declared,
                 int width, int height, ref<str> name, const TextureMemoryOptions& memory)
    -> Result<ImageType, ImageParseError> {
    fs::BinaryReader reader(source.clone());
    auto             stored = TextureBytes::new_in(memory.bytes.clone());
    auto             prefix = TextureBytes::new_in(memory.bytes.clone());
    stb::Decoder     decoder(memory.bytes.clone());
    if (compressed) {
        stored = rstd_try(ReadTextureBytes(reader, name, memory));
        if (stored.len() > usize(i32::MAX.to_primitive()) || decoded_size < 0)
            return Err(InvalidTextureData(name, "LZ4 size"_str));
    }
    int limit = compressed ? rstd::min(decoded_size, 128) : 16;
    for (;;) {
        if (prefix.try_reserve_exact(usize(limit) - prefix.len()).is_err())
            return Err(ImageAllocationError(name));
        prefix.resize(usize(limit), u8());
        int size;
        if (compressed) {
            size = LZ4_decompress_safe_partial(reinterpret_cast<const char*>(stored.data()),
                                               reinterpret_cast<char*>(prefix.data()),
                                               static_cast<int>(stored.len().to_primitive()),
                                               limit,
                                               limit);
            if (size < 0) return Err(InvalidTextureData(name, "LZ4 header"_str));
        } else {
            size = static_cast<int>(reader.Read(prefix.data(), limit));
            reader.Rewind();
        }
        auto type = declared == ImageType::UNKNOWN
                        ? DetectEmbeddedImageType(
                              reinterpret_cast<const unsigned char*>(prefix.data()), size)
                        : declared;
        if (type == ImageType::UNKNOWN) return Ok(type);
        if (type == ImageType::VIDEO) {
            if (compressed) return Err(InvalidTextureData(name, "compressed video"_str));
            return Ok(type);
        }
        auto metadata =
            compressed
                ? decoder.InfoMemory(reinterpret_cast<const unsigned char*>(prefix.data()), size)
                : decoder.InfoCallbacks(ImageCallbacks, &reader);
        if (metadata.is_err() && metadata.as_ref().unwrap_err() == stb::Error::OutOfMemory)
            return Err(ImageAllocationError(name));
        if (metadata.is_ok()) {
            auto w = metadata->width, h = metadata->height;
            if (w != width || h != height || i64(w) * i64(h) > i64(i32::MAX.to_primitive() / 4))
                return Err(InvalidTextureData(name, "container extent"_str));
            return Ok(type);
        }
        if (! compressed || limit == decoded_size || size < limit)
            return Err(InvalidTextureData(name, "container header"_str));
        limit = limit > decoded_size / 2 ? decoded_size : limit * 2;
    }
}

bool SkipTextureBytes(fs::BinaryReader& file, rstd::uint32_t size) {
    return u64(size) <= file.remaining() && file.SeekCur(static_cast<rstd::ptrdiff_t>(size));
}

auto SelectConditionalVideo(fs::BinaryReader& file, const rstd::io::ReadRange& input,
                            const TexBodyHeader& body, ref<str> name, bool& unsupported)
    -> Result<Option<rstd::io::ReadRange>, ImageParseError> {
    Option<rstd::io::ReadRange> selected;
    if (body.condition_count == 0) return Ok(rstd::move(selected));
    if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "patch groups"_str));
    const auto groups = file.ReadUint32();
    if (u64(groups) > file.remaining() / u64(4))
        return Err(InvalidTextureData(name, "patch group count"_str));
    for (rstd::uint32_t group = 0; group < groups; ++group) {
        if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "patch count"_str));
        const auto patches = file.ReadUint32();
        if (u64(patches) > file.remaining() / u64(32))
            return Err(InvalidTextureData(name, "patch count"_str));
        for (rstd::uint32_t patch = 0; patch < patches; ++patch) {
            if (file.remaining() < u64(32)) return Err(InvalidTextureData(name, "patch"_str));
            file.ReadUint32(); // unknown
            const auto patch_id = u32(file.ReadUint32());
            const auto x        = file.ReadUint32();
            const auto y        = file.ReadUint32();
            const auto width    = file.ReadUint32();
            const auto height   = file.ReadUint32();
            const auto type     = static_cast<ImageType>(file.ReadInt32());
            const auto size     = file.ReadUint32();
            const auto pos      = file.position();
            if (! SkipTextureBytes(file, size))
                return Err(InvalidTextureData(name, "patch payload"_str));
            if (! body.selected_patches.iter().any([&](auto id) {
                    return *id == patch_id;
                }))
                continue;
            if (x || y || width || height ||
                (type != ImageType::VIDEO && type != ImageType::UNKNOWN)) {
                unsupported = true;
                continue;
            }
            auto range = input.subrange(pos, u64(size));
            if (range.is_err()) return Err(InvalidTextureData(name, "patch range"_str));
            auto             source = range.unwrap();
            fs::BinaryReader reader(source.clone());
            unsigned char    prefix[16] {};
            const auto       n = reader.Read(prefix, sizeof(prefix));
            if (DetectEmbeddedImageType(prefix, static_cast<int>(n)) == ImageType::VIDEO)
                selected = Some(rstd::move(source));
            else
                unsupported = true;
        }
    }
    return Ok(rstd::move(selected));
}

auto LoadHeader(fs::BinaryReader& file, ImageHeader& header, ref<str> name,
                const rstd::json::Map& properties) -> Result<TexBodyHeader, ImageParseError> {
    TexBodyHeader    body;
    TexFormatVersion v;
    v.texv = ReadTexVersion(file);
    v.texi = ReadTexVersion(file);
    (void)header.extraHeader.insert("texv"_Str, ImageExtra { .val = v.texv });
    (void)header.extraHeader.insert("texi"_Str, ImageExtra { .val = v.texi });

    header.format = ToTexFormate(file.ReadInt32());
    TexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[TexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[TexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[TexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        (void)header.extraHeader.insert("compo1"_Str,
                                        ImageExtra { .val = flags[TexFlagEnum::compo1] });
        (void)header.extraHeader.insert("compo2"_Str,
                                        ImageExtra { .val = flags[TexFlagEnum::compo2] });
        (void)header.extraHeader.insert("compo3"_Str,
                                        ImageExtra { .val = flags[TexFlagEnum::compo3] });
        (void)header.extraHeader.insert("compo4"_Str,
                                        ImageExtra { .val = flags[TexFlagEnum::compo4] });
    }

    /*
        picture:
        width, height --> pow of 2 (tex size)
        mapw, maph    --> pic size
        mips
        mipw,miph     --> pow of 2

        sprites:
        width, height --> piece of sprite sheet
        mapw, maph    --> same
        1 mip
        mipw,mimp     --> tex size
    */

    header.width  = file.ReadInt32();
    header.height = file.ReadInt32();
    // in sprite this mean one pic
    header.mapWidth  = file.ReadInt32();
    header.mapHeight = file.ReadInt32();

    file.ReadInt32(); // unknown

    v.texb = ReadTexVersion(file);
    (void)header.extraHeader.insert("texb"_Str, ImageExtra { .val = v.texb });

    header.count = file.ReadInt32();

    if (v.body_has_image_type()) header.type = static_cast<ImageType>(file.ReadInt32());
    if (v.body_has_conditions()) {
        if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "condition count"_str));
        body.condition_count = file.ReadUint32();
        if (u64(body.condition_count) > file.remaining() / u64(13))
            return Err(InvalidTextureData(name, "condition count"_str));
        for (rstd::uint32_t index = 0; index < body.condition_count; ++index) {
            if (file.remaining() < u64(12))
                return Err(InvalidTextureData(name, "condition record"_str));
            file.ReadUint32(); // condition group
            const auto patch_id = u32(file.ReadUint32());
            const auto flags    = file.ReadUint32();
            const bool examine  = flags == 2 && ! properties.is_empty();
            body.unsupported_conditions |= flags != 2;
            Vec<u8> bytes;
            u8      value;
            do {
                if (file.Read(&value, 1) != 1)
                    return Err(InvalidTextureData(name, "condition string"_str));
                if (examine && value != u8()) bytes.push(u8(value));
            } while (value != u8());
            if (! examine) continue;
            auto text = String::from_utf8(rstd::move(bytes));
            if (! text) continue;
            auto json = ParseJson(text->as_str());
            if (! json) continue;
            const auto rule = json->get("condition"_str);
            if (! rule) continue;
            const auto key      = (*rule)->get("name"_str);
            const auto expected = (*rule)->get("condition"_str);
            if (! key || ! expected) continue;
            const auto key_name = (*key)->as_str();
            if (! key_name) continue;
            auto actual = properties.get(*key_name);
            if (actual && SceneJsonScalarEquals(SceneUserPropertyPayload(**actual), **expected))
                body.selected_patches.push(u32(patch_id));
        }
    }

    if (v.texv != 5 || v.texi != 1 || v.texb < 1 || v.texb > 4) {
        rstd_error(
            "TexImageParser: unsupported version texv={} texi={} texb={}", v.texv, v.texi, v.texb);
    }
    body.version = v;
    return Ok(rstd::move(body));
}

void SetHeaderPow2(ImageHeader& header, rstd::int32_t mip_0_w, rstd::int32_t mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo(u32(static_cast<rstd::uint32_t>(mip_0_w))) ||
                           algorism::IsPowOfTwo(u32(static_cast<rstd::uint32_t>(mip_0_h)));
    header.mipmap_larger = mip_0_w * mip_0_h > header.mapWidth * header.mapHeight;
}

Option<rstd::uint8_t> HexValue(char c) {
    if (c >= '0' && c <= '9') return Some(static_cast<rstd::uint8_t>(c - '0'));
    if (c >= 'a' && c <= 'f') return Some(static_cast<rstd::uint8_t>(c - 'a' + 10));
    if (c >= 'A' && c <= 'F') return Some(static_cast<rstd::uint8_t>(c - 'A' + 10));
    return None();
}

Option<Vec<u8>> PercentDecode(ref<str> raw) {
    auto       out   = Vec<u8>::with_capacity(raw.len());
    const auto bytes = raw.as_bytes();
    for (usize i {}; i < bytes.len();) {
        if (bytes[i] != u8('%')) {
            out.push(u8(bytes[i++]));
            continue;
        }
        if (i + usize(2) >= bytes.len()) return None();
        auto hi = HexValue(static_cast<char>(bytes[i + usize(1)].to_primitive()));
        auto lo = HexValue(static_cast<char>(bytes[i + usize(2)].to_primitive()));
        if (hi.is_none() || lo.is_none()) return None();
        out.push(u8(static_cast<rstd::uint8_t>((*hi << 4) | *lo)));
        i += usize(3);
    }
    return Some(rstd::move(out));
}

Option<CString> ExternalImagePath(ref<str> name) {
    ref<str> path;
    if (name.starts_with("file://localhost/"_str)) {
        path = *name.get(usize(16), name.len());
    } else if (name.starts_with("file:///"_str)) {
        path = *name.get(usize(7), name.len());
    } else if (name.starts_with("/"_str)) {
        path = name;
    } else {
        return None();
    }
    auto decoded = PercentDecode(path);
    auto bytes   = decoded.is_some() ? rstd::move(*decoded) : Vec<u8>::from(path.as_bytes());
    // Filesystem and stb previously consumed the path through a terminating NUL.
    for (usize i {}; i < bytes.len(); ++i) {
        if (bytes[i] == u8()) {
            bytes.truncate(i);
            break;
        }
    }
    return Some(CString::make(rstd::move(bytes)).unwrap());
}

Option<CString> ResolveExternalImagePath(ref<str> name) {
    auto path = ExternalImagePath(name);
    if (! path) return None();
    auto metadata = rstd::fs::metadata(fs::Path(OsStrExt::from_bytes(path->as_ref().to_bytes())));
    if (metadata.is_err() || ! metadata->is_file()) return None();
    return path;
}

ImageHeader MakeExternalImageHeader(int width, int height) {
    ImageHeader header;
    header.width     = width;
    header.height    = height;
    header.mapWidth  = width;
    header.mapHeight = height;
    header.format    = TextureFormat::RGBA8;
    header.type      = ImageType::PNG;
    header.count     = 1;
    header.sample    = TextureSample { TextureWrap::CLAMP_TO_EDGE,
                                       TextureWrap::CLAMP_TO_EDGE,
                                       TextureFilter::LINEAR,
                                       TextureFilter::LINEAR };
    SetHeaderPow2(header, width, height);
    return header;
}

auto ParseExternalImage(ref<str> key, const CString& path,
                        const Arc<Mutex<ImageDecodeStatistics>>& statistics,
                        const TextureMemoryOptions& memory) -> Result<Arc<Image>, ImageParseError> {
    stb::Decoder decoder(memory.bytes.clone());
    auto         result = decoder.LoadFile(path.as_ref().as_ptr());
    if (result.is_err()) return Err(DecoderError(key, result.unwrap_err()));
    auto pixels = rstd::move(result).unwrap();
    auto width = pixels.info.width, height = pixels.info.height;
    if (width <= 0 || height <= 0 || i64(width) * i64(height) > i64(i32::MAX.to_primitive() / 4)) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::DecodeFailed,
            .message = rstd::format("decode external image {} failed", key),
        });
    }

    auto img_ptr          = Arc<Image>::make();
    img_ptr->content->key = String::make(key);
    img_ptr->header       = MakeExternalImageHeader(width, height);
    img_ptr->content->slots.push(vrento::Image::Slot {});
    auto& slot  = img_ptr->content->slots[usize(0)];
    slot.width  = width;
    slot.height = height;
    slot.mipmaps.push(ImageData {});
    auto& mipmap  = slot.mipmaps[usize(0)];
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size   = isize(static_cast<rstd::ptrdiff_t>(width * height * 4));
    Option<Arc<DecodedImageLease>> lease;
    auto*                          pixel_data = pixels.data();
    mipmap.data                               = OwnDecodedPixels(
        pixel_data,
        u64(rstd::uint64_t(width) * height * 4),
        [pixels = rstd::move(pixels)](rstd::uint8_t*) mutable {
            pixels.reset();
        },
        lease,
        statistics);
    img_ptr->FinalizeContent();
    return Ok(rstd::move(img_ptr));
}

} // namespace

auto TexImageParser::Retention(ref<str> name) const -> ImageRetention {
    return ExternalImagePath(name) ? ImageRetention::Source : ImageRetention::Parser;
}

auto TexImageParser::Parse(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
    auto decode = [&] {
        {
            auto stats = m_statistics->lock().unwrap_unchecked();
            ++stats->decode_attempts;
        }
        auto result = Decode(name);
        if (result.is_ok()) {
            auto stats = m_statistics->lock().unwrap_unchecked();
            ++stats->decode_total;
        }
        return result;
    };
    if (ResolveExternalImagePath(name)) return decode();
    // Package content is immutable for the lifetime of this parser snapshot.
    auto cell = [&] {
        auto cache = m_images->lock().unwrap_unchecked();
        if (auto image = cache->get(name)) return (*image)->clone();
        auto created = Arc<ImageCell>::make(CachedImage {});
        (void)cache->insert(String::make(name), created.clone());
        return created;
    }();
    Option<Arc<InFlightImage>> pending;
    bool                       producer {};
    {
        auto cached = cell->lock().unwrap_unchecked();
        auto image  = cached->image.upgrade();
        if (! image) {
            if (auto content = cached->content.upgrade()) {
                auto prepared  = rstd_try(Prepare(name));
                image          = Arc<Image>::make();
                image->header  = prepared->description.header.clone();
                image->content = rstd::move(content);
                cached->image  = image.downgrade();
            }
        }
        if (image) {
            auto stats = m_statistics->lock().unwrap_unchecked();
            ++stats->reuse_total;
            return Ok(rstd::move(image));
        }
        if (cached->in_flight) {
            pending    = Some(cached->in_flight->clone());
            auto stats = m_statistics->lock().unwrap_unchecked();
            ++stats->reuse_total;
            ++stats->in_flight_reuses;
        } else {
            pending           = Some(Arc<InFlightImage>::make());
            cached->in_flight = Some(pending->clone());
            producer          = true;
        }
    }
    auto flight = pending.take().unwrap_unchecked();
    if (! producer) return CloneImageResult(*flight->result.wait());

    struct Completion {
        const Arc<ImageCell>&     cell;
        const Arc<InFlightImage>& flight;
        bool                      finished {};
        ~Completion() {
            if (finished) return;
            auto cached = cell->lock().unwrap_unchecked();
            (void)flight->result.set(Err(ImageParseError {
                .kind    = ImageParseErrorKind::DecodeFailed,
                .message = "texture decode interrupted"_Str,
            }));
            cached->in_flight = None();
        }
    } completion { cell, flight };

    auto result = decode();
    {
        auto cached = cell->lock().unwrap_unchecked();
        if (result.is_ok()) {
            cached->image   = result->downgrade();
            cached->content = (*result)->content.downgrade();
            if ((*result)->header.type == ImageType::VIDEO) cached->video = Some(result->clone());
        }
        (void)flight->result.set(rstd::move(result));
        cached->in_flight   = None();
        completion.finished = true;
    }
    const auto& delivered = *flight->result.wait();
    if (delivered.is_ok() && m_ready) (*m_ready)->operator()((*delivered)->content.clone());
    return CloneImageResult(delivered);
}

auto TexImageParser::Decode(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
    if (auto path = ResolveExternalImagePath(name))
        return ParseExternalImage(name, *path, m_statistics, m_memory);
    auto                           prepared = rstd_try(Prepare(name));
    auto                           image    = Arc<Image>::make();
    Option<Arc<DecodedImageLease>> lease;
    image->content->key = String::make(name);
    image->header       = prepared->description.header.clone();
    if (prepared->unsupported_conditions)
        rstd_warn("texture {}: unsupported conditional patches were skipped", name);
    image->content->slots.reserve(prepared->slots.len());
    for (const auto& source_slot : prepared->slots) {
        vrento::Image::Slot slot;
        slot.mipmaps.reserve(source_slot.len());
        for (const auto& source : source_slot) {
            ImageData mip { .width = source.width, .height = source.height };
            if (source.type == ImageType::VIDEO) {
                mip.video_source = Some(source.source.clone());
            } else {
                auto bytes = rstd_try(ReadMipBytes(
                    source.source.clone(), source.compressed, source.decoded_size, name, m_memory));
                if (source.type != ImageType::UNKNOWN) {
                    if (bytes.len() > usize(i32::MAX.to_primitive()))
                        return Err(InvalidTextureData(name, "container size"_str));
                    stb::Decoder decoder(m_memory.bytes.clone());
                    auto         result =
                        decoder.LoadMemory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                           static_cast<int>(bytes.len().to_primitive()));
                    if (result.is_err()) return Err(DecoderError(name, result.unwrap_err()));
                    auto  pixels = rstd::move(result).unwrap();
                    auto  width = pixels.info.width, height = pixels.info.height;
                    auto* pixel_data = pixels.data();
                    mip.data         = OwnDecodedPixels(
                        pixel_data,
                        u64(rstd::uint64_t(width) * height * 4),
                        [pixels = rstd::move(pixels)](rstd::uint8_t*) mutable {
                            pixels.reset();
                        },
                        lease,
                        m_statistics);
                    if (width != source.width || height != source.height)
                        return Err(InvalidTextureData(name, "decoded extent"_str));
                    mip.size = isize(rstd::ptrdiff_t(width) * height * 4);
                } else {
                    auto  size    = bytes.len().to_primitive();
                    auto* pointer = reinterpret_cast<rstd::uint8_t*>(bytes.data());
                    mip.data      = OwnDecodedPixels(
                        pointer,
                        u64(size),
                        [bytes = rstd::move(bytes)](rstd::uint8_t*) mutable {
                            bytes = TextureBytes::new_in(bytes.allocator().clone());
                        },
                        lease,
                        m_statistics);
                    mip.size = isize(size);
                }
            }
            slot.mipmaps.push(rstd::move(mip));
        }
        if (! source_slot.is_empty()) {
            slot.width  = source_slot[usize()].width;
            slot.height = source_slot[usize()].height;
        }
        image->content->slots.push(rstd::move(slot));
    }
    image->FinalizeContent();
    return Ok(rstd::move(image));
}

auto owe::ParseImages(ref<dyn<IImageParser>> parser, slice<String> names, usize max_workers)
    -> Vec<Result<Arc<Image>, ImageParseError>> {
    auto parse_sequential = [parser, names]() mutable {
        auto images = Vec<Result<Arc<Image>, ImageParseError>>::with_capacity(names.len());
        for (usize index {}; index < names.len(); ++index)
            images.push(parser->Parse(names[index].as_str()));
        return images;
    };

    if (names.len() < usize(2) || max_workers < usize(2)) return parse_sequential();

    const auto worker_count = rstd::min(names.len(), max_workers);
    auto       group = rstd::thread::BlockingTaskGroup<Result<Arc<Image>, ImageParseError>>::make(
        worker_count, worker_count);
    if (group.is_err()) return parse_sequential();

    usize submitted_count {};
    for (; submitted_count < names.len(); ++submitted_count) {
        auto name      = names[submitted_count].clone();
        auto submitted = group->submit([parser, name = rstd::move(name)]() mutable {
            return parser->Parse(name.as_str());
        });
        if (submitted.is_err()) break;
    }

    auto outcomes = rstd::move(*group).join();
    auto images   = Vec<Result<Arc<Image>, ImageParseError>>::with_capacity(names.len());
    for (auto& outcome : outcomes) {
        auto value = rstd::move(outcome).into_value();
        if (value.is_some()) {
            images.push(rstd::move(value).unwrap_unchecked());
        } else {
            images.push(Err(ImageParseError {
                .kind    = ImageParseErrorKind::DecodeFailed,
                .message = "texture parse task failed"_Str,
            }));
        }
    }
    for (usize index = submitted_count; index < names.len(); ++index) {
        images.push(parser->Parse(names[index].as_str()));
    }
    return images;
}

auto TexImageParser::ParseMany(slice<String> names) const
    -> Vec<Result<Arc<Image>, ImageParseError>> {
    auto parser = dyn<IImageParser>::from_ref(*this);
    return ParseImages(parser, names);
}

auto TexImageParser::ParseHeader(ref<str> name) const -> Result<ImageHeader, ImageParseError> {
    if (auto path = ResolveExternalImagePath(name)) {
        stb::Decoder decoder(m_memory.bytes.clone());
        auto         metadata = decoder.InfoFile(path->as_ref().as_ptr());
        if (metadata.is_err()) return Err(DecoderError(name, metadata.unwrap_err()));
        if (metadata->width > 0 && metadata->height > 0)
            return Ok(MakeExternalImageHeader(metadata->width, metadata->height));
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::DecodeFailed,
            .message = rstd::format("read external image header {} failed", name),
        });
    }
    if (name.contains("_alias_"_str)) return Ok(ImageHeader {});
    auto prepared = rstd_try(Prepare(name));
    return Ok(prepared->description.header.clone());
}

auto TexImageParser::Describe(ref<str> name) const -> Result<ImageDescription, ImageParseError> {
    if (ResolveExternalImagePath(name)) {
        auto             header = rstd_try(ParseHeader(name));
        ImageDescription result { .header = rstd::move(header) };
        result.slots.push({ result.header.width, result.header.height, usize(1) });
        return Ok(rstd::move(result));
    }
    auto prepared = rstd_try(Prepare(name));
    return Ok(prepared->description.clone());
}

auto TexImageParser::Prepare(ref<str> name) const -> Result<Arc<PreparedImage>, ImageParseError> {
    auto cache = m_headers->lock().unwrap_unchecked();
    if (auto cached = cache->get(name)) return Ok((*cached)->clone());
    auto path   = rstd::format("/assets/materials/{}.tex", name);
    auto source = m_vfs->open_read(fs::Path(path.as_str()));
    if (source.is_err())
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::MissingContent,
            .message = rstd::format("open texture {} failed", name),
        });
    auto             input = rstd::move(source).unwrap();
    fs::BinaryReader file(input.clone());
    auto             prepared        = Arc<PreparedImage>::make();
    auto&            header          = prepared->description.header;
    auto             body            = rstd_try(LoadHeader(file, header, name, m_properties));
    prepared->unsupported_conditions = body.unsupported_conditions;
    auto ver                         = body.version;
    if (header.count < 0 || u64(header.count) > file.remaining() / u64(4))
        return Err(InvalidTextureData(name, "image count"_str));
    auto                         declared = header.type;
    Vec<Option<array<float, 2>>> imageDatas;
    imageDatas.resize(usize(header.count), None());
    prepared->slots.reserve(usize(header.count));
    for (int index {}; index < header.count; ++index) {
        if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "mip count"_str));
        auto count = file.ReadInt32();
        if (count < 0 || count > 32) return Err(InvalidTextureData(name, "mip count"_str));
        Vec<PreparedMip> mips;
        mips.reserve(usize(count));
        for (int mip_index {}; mip_index < count; ++mip_index) {
            auto fields = ver.body_has_lz4_prelude() ? u64(20) : u64(12);
            if (file.remaining() < fields) return Err(InvalidTextureData(name, "mip header"_str));
            auto width  = file.ReadInt32();
            auto height = file.ReadInt32();
            bool compressed {};
            int  decoded_size {};
            if (ver.body_has_lz4_prelude()) {
                compressed   = file.ReadInt32() == 1;
                decoded_size = file.ReadInt32();
            }
            auto stored_size = file.ReadInt32();
            if (width <= 0 || height <= 0 || stored_size <= 0 || decoded_size < 0 ||
                (compressed && decoded_size == 0) || u64(stored_size) > file.remaining())
                return Err(InvalidTextureData(name, "mip payload"_str));
            auto range = input.subrange(file.position(), u64(stored_size));
            if (range.is_err()) return Err(InvalidTextureData(name, "mip range"_str));
            PreparedMip mip { .width        = width,
                              .height       = height,
                              .source       = rstd::move(range).unwrap(),
                              .compressed   = compressed,
                              .decoded_size = decoded_size };
            if (! SkipTextureBytes(file, stored_size))
                return Err(InvalidTextureData(name, "mip payload"_str));
            auto conditional = rstd_try(
                SelectConditionalVideo(file, input, body, name, prepared->unsupported_conditions));
            if (conditional) {
                mip.source       = conditional.take().unwrap_unchecked();
                mip.compressed   = false;
                mip.decoded_size = 0;
                mip.type         = ImageType::VIDEO;
            } else if (ver.body_has_image_type()) {
                mip.type = rstd_try(DescribeMip(mip.source.clone(),
                                                mip.compressed,
                                                mip.decoded_size,
                                                declared,
                                                mip.width,
                                                mip.height,
                                                name,
                                                m_memory));
            }
            if (mip.type != ImageType::UNKNOWN) {
                header.type   = mip.type;
                header.format = TextureFormat::RGBA8;
            }
            if (mip_index == 0) {
                imageDatas[usize(index)] =
                    Some(array<float, 2> { float(mip.width), float(mip.height) });
                SetHeaderPow2(header, mip.width, mip.height);
            }
            mips.push(rstd::move(mip));
        }
        ImageSlotDescription slot { .mip_count = mips.len() };
        if (! mips.is_empty()) {
            slot.width  = mips[usize()].width;
            slot.height = mips[usize()].height;
        }
        prepared->description.slots.push(rstd::move(slot));
        prepared->slots.push(rstd::move(mips));
    }
    if (header.isSprite) {
        // sprite pos
        ver.texs = ReadTexVersion(file);
        (void)header.extraHeader.insert("texs"_Str, ImageExtra { .val = ver.texs });
        if (ver.texs < 1 || ver.texs > 3) {
            return Err(ImageParseError {
                .kind = ImageParseErrorKind::InvalidData,
                .message =
                    rstd::format("texture {} has unsupported texs version {}", name, ver.texs),
            });
        }
        rstd::int32_t framecount = file.ReadInt32();
        if (framecount <= 0) {
            return Err(ImageParseError {
                .kind    = ImageParseErrorKind::InvalidData,
                .message = rstd::format("texture {} has no sprite frames", name),
            });
        }
        if (ver.sprite_has_atlas_size()) {
            rstd::int32_t width  = file.ReadInt32();
            rstd::int32_t height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (rstd::int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            // Two ways an imageId can be poison: outright negative (old
            // sentinel) or pointing past image_count, or pointing to an
            // image whose mip section was empty. All three previously
            // tripped vector::operator[]'s assertion. Skip the frame's
            // remaining bytes so subsequent frames stay aligned.
            const auto bad_id =
                sf.imageId < 0 ||
                static_cast<rstd::size_t>(sf.imageId) >= imageDatas.len().to_primitive() ||
                imageDatas[usize(static_cast<rstd::size_t>(sf.imageId))].is_none();
            if (bad_id) {
                rstd_error("TexImageParser: invalid sprite frame imageId={} (image_count={}) in {}",
                           sf.imageId,
                           imageDatas.len().to_primitive(),
                           name);
                file.ReadFloat();             // frametime
                for (int j = 0; j < 6; ++j) { // x, y, xAxis[0..1], yAxis[0..1]
                    if (ver.sprite_frame_coords_int())
                        file.ReadInt32();
                    else
                        file.ReadFloat();
                }
                continue;
            }
            float spriteWidth =
                (*imageDatas[usize(static_cast<rstd::size_t>(sf.imageId))])[usize()];
            float spriteHeight =
                (*imageDatas[usize(static_cast<rstd::size_t>(sf.imageId))])[usize(1)];

            sf.frametime = file.ReadFloat();
            if (ver.sprite_frame_coords_int()) {
                sf.x               = (float)file.ReadInt32() / spriteWidth;
                sf.y               = (float)file.ReadInt32() / spriteHeight;
                sf.xAxis[usize()]  = (float)file.ReadInt32();
                sf.xAxis[usize(1)] = (float)file.ReadInt32();
                sf.yAxis[usize()]  = (float)file.ReadInt32();
                sf.yAxis[usize(1)] = (float)file.ReadInt32();
            } else {
                sf.x               = file.ReadFloat() / spriteWidth;
                sf.y               = file.ReadFloat() / spriteHeight;
                sf.xAxis[usize()]  = file.ReadFloat();
                sf.xAxis[usize(1)] = file.ReadFloat();
                sf.yAxis[usize()]  = file.ReadFloat();
                sf.yAxis[usize(1)] = file.ReadFloat();
            }
            sf.width  = (float)f64(f64(sf.xAxis[usize()]).powf(f64(2)).to_primitive() +
                                   f64(sf.xAxis[usize(1)]).powf(f64(2)).to_primitive())
                            .sqrt()
                            .to_primitive();
            sf.height = (float)f64(f64(sf.yAxis[usize()]).powf(f64(2)).to_primitive() +
                                   f64(sf.yAxis[usize(1)]).powf(f64(2)).to_primitive())
                            .sqrt()
                            .to_primitive();
            sf.xAxis[usize()] /= spriteWidth;
            sf.xAxis[usize(1)] /= spriteWidth;
            sf.yAxis[usize()] /= spriteHeight;
            sf.yAxis[usize(1)] /= spriteHeight;
            sf.rate = sf.height / sf.width;
            header.spriteAnim.AppendFrame(sf);
        }
        if (header.spriteAnim.numFrames() == usize()) {
            return Err(ImageParseError {
                .kind    = ImageParseErrorKind::InvalidData,
                .message = rstd::format("texture {} has no valid sprite frames", name),
            });
        }
    }
    (void)cache->insert(String::make(name), prepared.clone());
    return Ok(rstd::move(prepared));
}
