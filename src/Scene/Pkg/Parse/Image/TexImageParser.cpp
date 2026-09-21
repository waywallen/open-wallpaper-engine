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
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.pkg_asset_version;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

enum class TexFlagEnum : uint32_t
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
    char* dst       = new char[static_cast<std::size_t>(decompressed_size)];
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
ImageType DetectEmbeddedImageType(const unsigned char* data, std::size_t size) {
    if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return ImageType::PNG;
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ImageType::JPEG;
    if (size >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0))
        return ImageType::GIF;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return ImageType::BMP;
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2a && data[3] == 0x00) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2a)))
        return ImageType::TIFF;
    // ISO BMFF / MP4 / MOV / 3GP — "....ftyp...." at offset 4. WE's scene
    // wallpapers can inline an H.264/AAC mp4 here; the renderer side
    // hands the bytes to wavsen::video::VideoDecoder.
    if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) return ImageType::VIDEO;
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
    uint32_t         condition_count {};
};

auto InvalidTextureData(ref<str> name, ref<str> section) -> ImageParseError {
    return { .kind    = ImageParseErrorKind::InvalidData,
             .message = rstd::format("texture {} has invalid {}", name, section) };
}

bool SkipTextureBytes(fs::BinaryReader& file, uint32_t size) {
    return u64(size) <= file.remaining() && file.SeekCur(static_cast<std::ptrdiff_t>(size));
}

auto SkipConditionalPatches(fs::BinaryReader& file, const TexBodyHeader& body, ref<str> name)
    -> Result<empty, ImageParseError> {
    if (body.condition_count == 0) return Ok(empty {});
    if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "patch groups"_str));
    const auto groups = file.ReadUint32();
    if (u64(groups) > file.remaining() / u64(4))
        return Err(InvalidTextureData(name, "patch group count"_str));
    for (uint32_t group = 0; group < groups; ++group) {
        if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "patch count"_str));
        const auto patches = file.ReadUint32();
        if (u64(patches) > file.remaining() / u64(32))
            return Err(InvalidTextureData(name, "patch count"_str));
        for (uint32_t patch = 0; patch < patches; ++patch) {
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
    v.texv                         = ReadTexVersion(file);
    v.texi                         = ReadTexVersion(file);
    header.extraHeader["texv"].val = v.texv;
    header.extraHeader["texi"].val = v.texi;

    header.format = ToTexFormate(file.ReadInt32());
    TexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[TexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[TexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[TexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        header.extraHeader["compo1"].val = flags[TexFlagEnum::compo1];
        header.extraHeader["compo2"].val = flags[TexFlagEnum::compo2];
        header.extraHeader["compo3"].val = flags[TexFlagEnum::compo3];
        header.extraHeader["compo4"].val = flags[TexFlagEnum::compo4];
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

    v.texb                         = ReadTexVersion(file);
    header.extraHeader["texb"].val = v.texb;

    header.count = file.ReadInt32();

    if (v.body_has_image_type()) header.type = static_cast<ImageType>(file.ReadInt32());
    if (v.body_has_conditions()) {
        if (file.remaining() < u64(4)) return Err(InvalidTextureData(name, "condition count"_str));
        body.condition_count = file.ReadUint32();
        if (u64(body.condition_count) > file.remaining() / u64(13))
            return Err(InvalidTextureData(name, "condition count"_str));
        for (uint32_t index = 0; index < body.condition_count; ++index) {
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

void SetHeaderPow2(ImageHeader& header, std::int32_t mip_0_w, std::int32_t mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo(u32(static_cast<std::uint32_t>(mip_0_w))) ||
                           algorism::IsPowOfTwo(u32(static_cast<std::uint32_t>(mip_0_h)));
    header.mipmap_larger = mip_0_w * mip_0_h > header.mapWidth * header.mapHeight;
}

Option<uint8_t> HexValue(char c) {
    if (c >= '0' && c <= '9') return Some(static_cast<uint8_t>(c - '0'));
    if (c >= 'a' && c <= 'f') return Some(static_cast<uint8_t>(c - 'a' + 10));
    if (c >= 'A' && c <= 'F') return Some(static_cast<uint8_t>(c - 'A' + 10));
    return None();
}

Option<std::string> PercentDecode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] != '%') {
            out.push_back(raw[i++]);
            continue;
        }
        if (i + 2 >= raw.size()) return None();
        auto hi = HexValue(raw[i + 1]);
        auto lo = HexValue(raw[i + 2]);
        if (hi.is_none() || lo.is_none()) return None();
        out.push_back(static_cast<char>((*hi << 4) | *lo));
        i += 3;
    }
    return Some(rstd::move(out));
}

Option<std::string> ResolveExternalImagePath(std::string_view name) {
    std::string path;
    if (name.starts_with("file://localhost/")) {
        path = "/" + std::string(name.substr(std::string_view("file://localhost/").size()));
    } else if (name.starts_with("file:///")) {
        path = std::string(name.substr(std::string_view("file://").size()));
    } else if (! name.empty() && name[0] == '/') {
        path = std::string(name);
    } else {
        return None();
    }
    auto            decoded = PercentDecode(path).unwrap_or(path);
    std::error_code ec;
    if (! std::filesystem::is_regular_file(decoded, ec)) return None();
    return Some(rstd::move(decoded));
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

auto ParseExternalImage(std::string_view key, const std::string& path)
    -> Result<Arc<Image>, ImageParseError> {
    int   width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (! pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::DecodeFailed,
            .message = rstd::format("decode external image {} failed", key),
        });
    }

    auto img_ptr          = Arc<Image>::make();
    img_ptr->content->key = std::string(key);
    img_ptr->header       = MakeExternalImageHeader(width, height);
    img_ptr->content->slots.resize(1);
    auto& slot  = img_ptr->content->slots[0];
    slot.width  = width;
    slot.height = height;
    slot.mipmaps.resize(1);
    auto& mipmap  = slot.mipmaps[0];
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size   = isize(static_cast<std::ptrdiff_t>(width * height * 4));
    mipmap.data   = ImageDataPtr(reinterpret_cast<uint8_t*>(pixels), [](uint8_t* data) {
        stbi_image_free(data);
    });
    img_ptr->FinalizeContent();
    return Ok(rstd::move(img_ptr));
}

} // namespace

auto TexImageParser::Parse(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
    const auto name_view = rstd::cppstd::as_string_view(name);
    if (auto path = ResolveExternalImagePath(name_view)) {
        return ParseExternalImage(name_view, *path);
    }

    auto  prepared   = rstd_try(PrepareHeader(name));
    auto  img_ptr    = Arc<Image>::make();
    auto& img        = *img_ptr;
    img.content->key = name_view;
    img.header       = prepared->header;
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
    std::int32_t _image_count = img.header.count;
    if (_image_count < 0) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::InvalidData,
            .message = rstd::format("texture {} has a negative image count", name),
        });
    }
    std::size_t image_count = static_cast<std::size_t>(_image_count);

    img.content->slots.resize(image_count);
    for (std::size_t i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.content->slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        std::size_t mipmap_count =
            static_cast<std::size_t>(std::max<std::int32_t>(file.ReadInt32(), 0));
        mipmaps.resize(mipmap_count);
        // load image
        for (std::size_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            auto& mipmap  = mipmaps.at(i_mipmap);
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            // check compress
            if (ver.body_has_lz4_prelude()) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            std::int32_t src_size = file.ReadInt32();
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
                std::ptrdiff_t body_off = file.Tell();
                unsigned char  sniff[16] {};
                file.Read(sniff, sizeof(sniff));
                ImageType maybe_video = img.header.type == ImageType::VIDEO
                                            ? ImageType::VIDEO
                                            : DetectEmbeddedImageType(sniff, sizeof(sniff));
                if (maybe_video == ImageType::VIDEO) {
                    img.header.type   = ImageType::VIDEO;
                    img.header.format = TextureFormat::RGBA8;
                    auto video_source =
                        tex_source.subrange(u64(static_cast<std::uint64_t>(body_off)),
                                            u64(static_cast<std::uint64_t>(src_size)));
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
            result = new char[static_cast<std::size_t>(src_size)];
            file.Read(result, static_cast<std::size_t>(src_size));

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
                                                   static_cast<std::size_t>(src_size));
            }
            if (ver.body_has_image_type() && embedded != ImageType::UNKNOWN) {
                int32_t w, h, n;
                auto*   data =
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
                mipmap.data = ImageDataPtr(reinterpret_cast<uint8_t*>(data), [](uint8_t* data) {
                    stbi_image_free(reinterpret_cast<unsigned char*>(data));
                });
                src_size    = w * h * 4;
            } else {
                mipmap.data = ImageDataPtr(new uint8_t[static_cast<std::size_t>(src_size)],
                                           [](uint8_t* data) {
                                               delete[] data;
                                           });
                std::copy(result, result + src_size, mipmap.data.get());
            }
            mipmap.size = isize(static_cast<std::ptrdiff_t>(src_size * sizeof(uint8_t)));
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
                .message = String::make("texture parse task failed"_str),
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
    const auto  name_view = rstd::cppstd::as_string_view(name);
    ImageHeader header;
    if (auto path = ResolveExternalImagePath(name_view)) {
        int width = 0, height = 0, channels = 0;
        if (stbi_info(path->c_str(), &width, &height, &channels) && width > 0 && height > 0)
            return Ok(MakeExternalImageHeader(width, height));
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::DecodeFailed,
            .message = rstd::format("read external image header {} failed", name),
        });
    }
    // WE "_alias_*" textures are runtime aliases the engine resolves
    // internally (light cookies, etc.). We don't model that, so just
    // return an empty header without spamming a vfs miss.
    if (name_view.find("_alias_") != std::string_view::npos) return Ok(rstd::move(header));
    auto prepared = rstd_try(PrepareHeader(name));
    return Ok(prepared->header);
}

auto TexImageParser::PrepareHeader(ref<str> name) const
    -> Result<Arc<PreparedHeader>, ImageParseError> {
    auto cache = m_headers->lock().unwrap_unchecked();
    if (auto cached = cache->get(name); cached.is_some()) return Ok((*cached)->clone());
    const auto  name_view = rstd::cppstd::as_string_view(name);
    ImageHeader header;
    std::string path   = "/assets/materials/" + std::string(name_view) + ".tex";
    auto        source = m_vfs->open_read(fs::ToPath(path));
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

    std::size_t image_count = static_cast<std::size_t>(header.count);

    // load sprite info
    if (header.isSprite) {
        // bypass image data, store width and height
        std::vector<std::vector<float>> imageDatas(image_count);
        for (std::size_t i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            if (mipmap_count < 0) {
                return Err(ImageParseError {
                    .kind    = ImageParseErrorKind::InvalidData,
                    .message = rstd::format("texture {} has a negative sprite mip count", name),
                });
            }
            for (int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                int32_t width  = file.ReadInt32();
                int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas.at(i_image) = { (float)width, (float)height };
                    header.mipmap_pow2 =
                        algorism::IsPowOfTwo(u32(static_cast<std::uint32_t>(width * height)));
                }
                if (ver.body_has_lz4_prelude()) {
                    int32_t LZ4_compressed    = file.ReadInt32();
                    int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                std::int32_t src_size = file.ReadInt32();
                if (src_size < 0 || ! SkipTextureBytes(file, static_cast<uint32_t>(src_size))) {
                    return Err(ImageParseError {
                        .kind    = ImageParseErrorKind::InvalidData,
                        .message = rstd::format("texture {} has an invalid sprite mip body", name),
                    });
                }
                rstd_try(SkipConditionalPatches(file, body, name));
            }
        }
        // sprite pos
        ver.texs                       = ReadTexVersion(file);
        header.extraHeader["texs"].val = ver.texs;
        if (ver.texs < 1 || ver.texs > 3) {
            return Err(ImageParseError {
                .kind = ImageParseErrorKind::InvalidData,
                .message =
                    rstd::format("texture {} has unsupported texs version {}", name, ver.texs),
            });
        }
        int32_t framecount = file.ReadInt32();
        if (framecount <= 0) {
            return Err(ImageParseError {
                .kind    = ImageParseErrorKind::InvalidData,
                .message = rstd::format("texture {} has no sprite frames", name),
            });
        }
        if (ver.sprite_has_atlas_size()) {
            std::int32_t width  = file.ReadInt32();
            std::int32_t height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            // Two ways an imageId can be poison: outright negative (old
            // sentinel) or pointing past image_count, or pointing to an
            // image whose mip section was empty. All three previously
            // tripped vector::operator[]'s assertion. Skip the frame's
            // remaining bytes so subsequent frames stay aligned.
            const auto bad_id = sf.imageId < 0 ||
                                static_cast<std::size_t>(sf.imageId) >= imageDatas.size() ||
                                imageDatas[static_cast<std::size_t>(sf.imageId)].size() < 2;
            if (bad_id) {
                rstd_error("TexImageParser: invalid sprite frame imageId={} (image_count={}) in {}",
                           sf.imageId,
                           imageDatas.size(),
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
            float spriteWidth  = imageDatas[static_cast<std::size_t>(sf.imageId)][0];
            float spriteHeight = imageDatas[static_cast<std::size_t>(sf.imageId)][1];

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
            sf.width =
                (float)std::sqrt(std::pow(sf.xAxis[usize()], 2) + std::pow(sf.xAxis[usize(1)], 2));
            sf.height =
                (float)std::sqrt(std::pow(sf.yAxis[usize()], 2) + std::pow(sf.yAxis[usize(1)], 2));
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
        std::int32_t mipmap_count = file.ReadInt32();
        (void)mipmap_count;
        std::int32_t width  = file.ReadInt32();
        std::int32_t height = file.ReadInt32();
        SetHeaderPow2(header, width, height);
        /* Sniff the body for a video container so the validator can
         * report "video tex" without needing a full Parse(). Mirrors
         * the peek in Parse() (line ~210). Cheap: 16 bytes + a
         * SeekSet. */
        if (ver.body_has_image_type() && header.type == ImageType::UNKNOWN) {
            bool    lz4               = false;
            int32_t decompressed_size = 0;
            if (ver.body_has_lz4_prelude()) {
                lz4               = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }
            (void)decompressed_size;
            std::int32_t src_size = file.ReadInt32();
            if (! lz4 && src_size >= 16) {
                std::ptrdiff_t body_off = file.Tell();
                unsigned char  sniff[16] {};
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
