module;

#include <rstd/macro.hpp>
#include <lz4.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

module wescene.pkg.parse;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import wescene.utils;
import wescene.scene;
import wescene.pkg_asset_version;

using rstd::mem::memcmp;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CString;
using rstd::os::unix::ffi::OsStrExt;
using rstd::ptr_::copy_nonoverlapping;
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
char* Lz4Decompress(const char* src, int size, int decompressed_size) {
    char* dst       = new char[static_cast<rstd::size_t>(decompressed_size)];
    int   load_size = LZ4_decompress_safe(src, dst, size, decompressed_size);
    if (load_size < decompressed_size) {
        rstd_error("lz4 decompress failed");
        delete[] dst;
        return nullptr;
    }
    return dst;
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
};

auto InvalidTextureData(ref<str> name, ref<str> section) -> ImageParseError {
    return { .kind    = ImageParseErrorKind::InvalidData,
             .message = rstd::format("texture {} has invalid {}", name, section) };
}

bool SkipTextureBytes(fs::BinaryReader& file, rstd::uint32_t size) {
    return u64(size) <= file.remaining() && file.SeekCur(static_cast<rstd::ptrdiff_t>(size));
}

auto SkipConditionalPatches(fs::BinaryReader& file, const TexBodyHeader& body, ref<str> name)
    -> Result<empty, ImageParseError> {
    if (body.condition_count == 0) return Ok(empty {});
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
            // Conditional overrides follow each base mip, including when not applied.
            if (file.remaining() < u64(32)) return Err(InvalidTextureData(name, "patch"_str));
            file.ReadUint32(); // unknown
            file.ReadUint32(); // condition id
            file.ReadUint32(); // x
            file.ReadUint32(); // y
            file.ReadUint32(); // width
            file.ReadUint32(); // height
            file.ReadInt32();  // image type
            if (! SkipTextureBytes(file, file.ReadUint32()))
                return Err(InvalidTextureData(name, "patch payload"_str));
        }
    }
    return Ok(empty {});
}

auto LoadHeader(fs::BinaryReader& file, ImageHeader& header, ref<str> name)
    -> Result<TexBodyHeader, ImageParseError> {
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
            if (! SkipTextureBytes(file, 12))
                return Err(InvalidTextureData(name, "condition record"_str));
            char value;
            do {
                if (file.Read(&value, 1) != 1)
                    return Err(InvalidTextureData(name, "condition string"_str));
            } while (value != '\0');
        }
    }

    if (v.texv != 5 || v.texi != 1 || v.texb < 1 || v.texb > 4) {
        rstd_error(
            "TexImageParser: unsupported version texv={} texi={} texb={}", v.texv, v.texi, v.texb);
    }
    body.version = v;
    return Ok(body);
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

Option<CString> ResolveExternalImagePath(ref<str> name) {
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
    auto metadata = rstd::fs::metadata(fs::Path(OsStrExt::from_bytes(bytes.as_slice())));
    if (metadata.is_err() || ! metadata->is_file()) return None();
    return Some(CString::make(rstd::move(bytes)).unwrap());
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

auto ParseExternalImage(ref<str> key, const CString& path) -> Result<Arc<Image>, ImageParseError> {
    int   width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(path.as_ref().as_ptr(), &width, &height, &channels, 4);
    if (! pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
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
    mipmap.data   = ImageDataPtr(reinterpret_cast<rstd::uint8_t*>(pixels), [](rstd::uint8_t* data) {
        stbi_image_free(data);
    });
    img_ptr->FinalizeContent();
    return Ok(rstd::move(img_ptr));
}

} // namespace

auto TexImageParser::Parse(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
    if (auto path = ResolveExternalImagePath(name)) {
        return ParseExternalImage(name, *path);
    }

    auto  prepared   = rstd_try(PrepareHeader(name));
    auto  img_ptr    = Arc<Image>::make();
    auto& img        = *img_ptr;
    img.content->key = rstd::into(name);
    img.header       = prepared->header.clone();
    auto tex_source  = prepared->source.clone();
    auto file        = fs::BinaryReader(tex_source.clone());
    if (! file.SeekSet(prepared->body_offset))
        return Err(InvalidTextureData(name, "body offset"_str));
    TexBodyHeader body { .version         = prepared->version,
                         .condition_count = prepared->condition_count };
    auto          ver = body.version;
    if (body.condition_count > 0)
        rstd_warn("texture {}: conditional patches are not applied; using base mipmaps", name);

    // image
    rstd::int32_t _image_count = img.header.count;
    if (_image_count < 0) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::InvalidData,
            .message = rstd::format("texture {} has a negative image count", name),
        });
    }
    rstd::size_t image_count = static_cast<rstd::size_t>(_image_count);

    img.content->slots.reserve(usize(image_count));
    for (rstd::size_t i_image = 0; i_image < image_count; i_image++) {
        img.content->slots.push(vrento::Image::Slot {});
        auto& img_slot = img.content->slots[usize(i_image)];
        auto& mipmaps  = img_slot.mipmaps;

        rstd::size_t mipmap_count =
            static_cast<rstd::size_t>(rstd::cmp::max<rstd::int32_t>(0, file.ReadInt32()));
        mipmaps.reserve(usize(mipmap_count));
        // load image
        for (rstd::size_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            mipmaps.push(ImageData {});
            auto& mipmap  = mipmaps[usize(i_mipmap)];
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool          LZ4_compressed    = false;
            rstd::int32_t decompressed_size = 0;
            // check compress
            if (ver.body_has_lz4_prelude()) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            rstd::int32_t src_size = file.ReadInt32();
            if (src_size <= 0 || mipmap.width <= 0 || mipmap.height <= 0 || decompressed_size < 0) {
                return Err(ImageParseError {
                    .kind    = ImageParseErrorKind::InvalidData,
                    .message = rstd::format("texture {} has an invalid mipmap", name),
                });
            }

            // Peek the first 16 bytes of the body so we can route MP4 /
            // WebM containers into the video-tex path without ever
            // pulling the (possibly hundreds of MiB) payload into RAM.
            // The range reader is seekable, so sniff without loading the
            // complete video payload.
            if (ver.body_has_image_type() &&
                (img.header.type == ImageType::UNKNOWN || img.header.type == ImageType::VIDEO) &&
                ! LZ4_compressed && src_size >= 16) {
                rstd::ptrdiff_t body_off = file.Tell();
                unsigned char   sniff[16] {};
                file.Read(sniff, sizeof(sniff));
                ImageType maybe_video = img.header.type == ImageType::VIDEO
                                            ? ImageType::VIDEO
                                            : DetectEmbeddedImageType(sniff, sizeof(sniff));
                if (maybe_video == ImageType::VIDEO) {
                    img.header.type   = ImageType::VIDEO;
                    img.header.format = TextureFormat::RGBA8;
                    auto video_source =
                        tex_source.subrange(u64(static_cast<rstd::uint64_t>(body_off)),
                                            u64(static_cast<rstd::uint64_t>(src_size)));
                    if (video_source.is_err()) {
                        return Err(ImageParseError {
                            .kind    = ImageParseErrorKind::InvalidData,
                            .message = rstd::format("texture {} has an invalid video range", name),
                        });
                    }
                    mipmap.video_source = rstd::Some(rstd::move(video_source).unwrap_unchecked());
                    mipmap.size         = isize();
                    file.SeekSet(body_off + src_size);
                    rstd_try(SkipConditionalPatches(file, body, name));
                    continue;
                }
                file.SeekSet(body_off);
            }

            char* result;
            result = new char[static_cast<rstd::size_t>(src_size)];
            file.Read(result, static_cast<rstd::size_t>(src_size));

            // is LZ4 compress
            if (LZ4_compressed) {
                char* decompressed_char = Lz4Decompress(result, src_size, decompressed_size);
                src_size                = decompressed_size;
                if (decompressed_char != nullptr) {
                    delete[] result;
                    result = decompressed_char;
                } else {
                    rstd_error("lz4 decompress failed");
                    delete[] result;
                    return Err(ImageParseError {
                        .kind    = ImageParseErrorKind::DecodeFailed,
                        .message = rstd::format("decompress texture {} failed", name),
                    });
                }
            }
            // is image container — declared image_type takes precedence; if
            // it's UNKNOWN, sniff the magic bytes so PKGV0022+ assets that
            // ship containerised PNG/JPEG with image_type=-1 still decode.
            ImageType embedded = img.header.type;
            if (ver.body_has_image_type() && embedded == ImageType::UNKNOWN) {
                embedded = DetectEmbeddedImageType(reinterpret_cast<const unsigned char*>(result),
                                                   static_cast<rstd::size_t>(src_size));
            }
            if (ver.body_has_image_type() && embedded != ImageType::UNKNOWN) {
                rstd::int32_t w, h, n;
                auto*         data =
                    stbi_load_from_memory((const unsigned char*)result, src_size, &w, &h, &n, 4);
                if (data == nullptr) {
                    rstd_error("stbi failed to decode embedded image (type={})", (int)embedded);
                    delete[] result;
                    return Err(ImageParseError {
                        .kind    = ImageParseErrorKind::DecodeFailed,
                        .message = rstd::format("decode embedded image {} failed", name),
                    });
                }
                img.header.type   = embedded;
                img.header.format = TextureFormat::RGBA8;
                mipmap.data =
                    ImageDataPtr(reinterpret_cast<rstd::uint8_t*>(data), [](rstd::uint8_t* data) {
                        stbi_image_free(reinterpret_cast<unsigned char*>(data));
                    });
                src_size = w * h * 4;
            } else {
                mipmap.data = ImageDataPtr(new rstd::uint8_t[static_cast<rstd::size_t>(src_size)],
                                           [](rstd::uint8_t* data) {
                                               delete[] data;
                                           });
                copy_nonoverlapping(ptr<rstd::uint8_t>::from_raw_parts(
                                        reinterpret_cast<const rstd::uint8_t*>(result)),
                                    mut_ptr<rstd::uint8_t>::from_raw_parts(mipmap.data.get()),
                                    usize(static_cast<rstd::size_t>(src_size)));
            }
            mipmap.size = isize(static_cast<rstd::ptrdiff_t>(src_size * sizeof(rstd::uint8_t)));
            delete[] result;
            rstd_try(SkipConditionalPatches(file, body, name));
        }
    }
    img_ptr->FinalizeContent();
    return Ok(rstd::move(img_ptr));
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
    ImageHeader header;
    if (auto path = ResolveExternalImagePath(name)) {
        int width = 0, height = 0, channels = 0;
        if (stbi_info(path->as_ref().as_ptr(), &width, &height, &channels) && width > 0 &&
            height > 0)
            return Ok(MakeExternalImageHeader(width, height));
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::DecodeFailed,
            .message = rstd::format("read external image header {} failed", name),
        });
    }
    // WE "_alias_*" textures are runtime aliases the engine resolves
    // internally (light cookies, etc.). We don't model that, so just
    // return an empty header without spamming a vfs miss.
    if (name.contains("_alias_"_str)) return Ok(rstd::move(header));
    auto prepared = rstd_try(PrepareHeader(name));
    return Ok(prepared->header.clone());
}

auto TexImageParser::PrepareHeader(ref<str> name) const
    -> Result<Arc<PreparedHeader>, ImageParseError> {
    auto cache = m_headers->lock().unwrap_unchecked();
    if (auto cached = cache->get(name); cached.is_some()) return Ok((*cached)->clone());
    ImageHeader header;
    auto        path   = rstd::format("/assets/materials/{}.tex", name);
    auto        source = m_vfs->open_read(fs::Path(path.as_str()));
    if (source.is_err()) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::MissingContent,
            .message = rstd::format("open texture header {} failed", name),
        });
    }
    auto input = rstd::move(source).unwrap_unchecked();
    auto file  = fs::BinaryReader(input.clone());

    auto       body        = rstd_try(LoadHeader(file, header, name));
    const auto body_offset = file.Tell();
    auto       ver         = body.version;
    if (header.count < 0) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::InvalidData,
            .message = rstd::format("texture {} has a negative image count", name),
        });
    }

    rstd::size_t image_count = static_cast<rstd::size_t>(header.count);

    // load sprite info
    if (header.isSprite) {
        // bypass image data, store width and height
        Vec<Option<array<float, 2>>> imageDatas;
        imageDatas.resize(usize(image_count), None());
        for (rstd::size_t i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            if (mipmap_count < 0) {
                return Err(ImageParseError {
                    .kind    = ImageParseErrorKind::InvalidData,
                    .message = rstd::format("texture {} has a negative sprite mip count", name),
                });
            }
            for (rstd::int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                rstd::int32_t width  = file.ReadInt32();
                rstd::int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas[usize(i_image)] =
                        Some(array<float, 2> { (float)width, (float)height });
                    header.mipmap_pow2 =
                        algorism::IsPowOfTwo(u32(static_cast<rstd::uint32_t>(width * height)));
                }
                if (ver.body_has_lz4_prelude()) {
                    rstd::int32_t LZ4_compressed    = file.ReadInt32();
                    rstd::int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                rstd::int32_t src_size = file.ReadInt32();
                if (src_size < 0 ||
                    ! SkipTextureBytes(file, static_cast<rstd::uint32_t>(src_size))) {
                    return Err(ImageParseError {
                        .kind    = ImageParseErrorKind::InvalidData,
                        .message = rstd::format("texture {} has an invalid sprite mip body", name),
                    });
                }
                rstd_try(SkipConditionalPatches(file, body, name));
            }
        }
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
    } else {
        rstd::int32_t mipmap_count = file.ReadInt32();
        (void)mipmap_count;
        rstd::int32_t width  = file.ReadInt32();
        rstd::int32_t height = file.ReadInt32();
        SetHeaderPow2(header, width, height);
        /* Sniff the body for a video container so the validator can
         * report "video tex" without needing a full Parse(). Mirrors
         * the peek in Parse() (line ~210). Cheap: 16 bytes + a
         * SeekSet. */
        if (ver.body_has_image_type() && header.type == ImageType::UNKNOWN) {
            bool          lz4               = false;
            rstd::int32_t decompressed_size = 0;
            if (ver.body_has_lz4_prelude()) {
                lz4               = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }
            (void)decompressed_size;
            rstd::int32_t src_size = file.ReadInt32();
            if (! lz4 && src_size >= 16) {
                rstd::ptrdiff_t body_off = file.Tell();
                unsigned char   sniff[16] {};
                file.Read(sniff, sizeof(sniff));
                if (DetectEmbeddedImageType(sniff, sizeof(sniff)) == ImageType::VIDEO) {
                    header.type   = ImageType::VIDEO;
                    header.format = TextureFormat::RGBA8;
                }
                file.SeekSet(body_off);
            }
        }
    }
    auto prepared = Arc<PreparedHeader>::make(PreparedHeader {
        .header          = rstd::move(header),
        .source          = rstd::move(input),
        .version         = body.version,
        .condition_count = body.condition_count,
        .body_offset     = body_offset,
    });
    (void)cache->insert(String::make(name), prepared.clone());
    return Ok(rstd::move(prepared));
}
