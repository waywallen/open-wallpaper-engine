module;

#include <rstd/macro.hpp>

export module wescene.spec_names;
import rstd;
import rstd.log;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe
{

inline constexpr ref<str> WE_SPEC_PREFIX                  = "_rt_"_str;
inline constexpr ref<str> WE_IMAGE_LAYER_COMPOSITE_PREFIX = "_rt_imageLayerComposite_"_str;
inline constexpr ref<str> WE_MIP_MAPPED_FRAME_BUFFER      = "_rt_MipMappedFrameBuffer"_str;
inline constexpr ref<str> WE_REFLECTION_PREFIX            = "_rt_Reflection"_str;
inline constexpr ref<str> SpecTex_Default                 = "_rt_default"_str;
inline constexpr ref<str> SpecTex_Link                    = "_rt_link_"_str;

inline constexpr ref<str> WE_IN_POSITION     = "a_Position"_str;
inline constexpr ref<str> WE_IN_NORMAL       = "a_Normal"_str;
inline constexpr ref<str> WE_IN_TEXCOORD     = "a_TexCoord"_str;
inline constexpr ref<str> WE_IN_TANGENT4     = "a_Tangent4"_str;
inline constexpr ref<str> WE_IN_BLENDINDICES = "a_BlendIndices"_str;
inline constexpr ref<str> WE_IN_BLENDWEIGHTS = "a_BlendWeights"_str;
inline constexpr ref<str> WE_IN_CENTER       = "a_Center"_str;
inline constexpr ref<str> WE_IN_COLOR4U      = "a_Color4u"_str;
inline constexpr ref<str> WE_IN_POSITIONC1   = "a_PositionC1"_str;

// particle

inline constexpr ref<str> WE_IN_POSITIONVEC4   = "a_PositionVec4"_str;
inline constexpr ref<str> WE_IN_COLOR          = "a_Color"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4   = "a_TexCoordVec4"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C1 = "a_TexCoordVec4C1"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C2 = "a_TexCoordVec4C2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC4C3 = "a_TexCoordVec4C3"_str;
inline constexpr ref<str> WE_IN_TEXCOORDVEC3C2 = "a_TexCoordVec3C2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC2     = "a_TexCoordC2"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC3     = "a_TexCoordC3"_str;
inline constexpr ref<str> WE_IN_TEXCOORDC4     = "a_TexCoordC4"_str;

// Compile-time (name, type) pair for declarative attribute layouts.
struct VertexAttrSpec {
    ref<str>   name;
    VertexType type;
    bool       padding { true };
};

namespace VAttr
{
inline constexpr VertexAttrSpec Position { WE_IN_POSITION, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Normal { WE_IN_NORMAL, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec PositionVec4 { WE_IN_POSITIONVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoord { WE_IN_TEXCOORD, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec Tangent4 { WE_IN_TANGENT4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4 { WE_IN_TEXCOORDVEC4, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C1 { WE_IN_TEXCOORDVEC4C1, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C2 { WE_IN_TEXCOORDVEC4C2, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec4C3 { WE_IN_TEXCOORDVEC4C3, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec TexCoordVec3C2 { WE_IN_TEXCOORDVEC3C2, VertexType::FLOAT3, false };
inline constexpr VertexAttrSpec TexCoordC2 { WE_IN_TEXCOORDC2, VertexType::FLOAT2 };
inline constexpr VertexAttrSpec TexCoordC3 { WE_IN_TEXCOORDC3, VertexType::FLOAT2, false };
inline constexpr VertexAttrSpec TexCoordC4 { WE_IN_TEXCOORDC4, VertexType::FLOAT2, false };
inline constexpr VertexAttrSpec Color { WE_IN_COLOR, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec BlendIndices { WE_IN_BLENDINDICES, VertexType::UINT4 };
inline constexpr VertexAttrSpec BlendWeights { WE_IN_BLENDWEIGHTS, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec Center { WE_IN_CENTER, VertexType::FLOAT3 };
inline constexpr VertexAttrSpec Color4u { WE_IN_COLOR4U, VertexType::FLOAT4 };
inline constexpr VertexAttrSpec PositionC1 { WE_IN_POSITIONC1, VertexType::FLOAT3 };
} // namespace VAttr

inline bool IsSpecTex(ref<str> name) { return name.starts_with(WE_SPEC_PREFIX); }
inline bool IsSpecLinkTex(ref<str> name) { return name.starts_with(SpecTex_Link); }
inline u32  ParseLinkTex(ref<str> name) {
    auto suffix = name.strip_prefix(SpecTex_Link);
    if (suffix.is_none()) {
        rstd_error("invalid linked texture id: {}", name);
        return u32();
    }
    auto result = rstd::from_str<u32>(*suffix);
    if (result.is_err()) {
        rstd_error("invalid linked texture id: {}", name);
        return u32();
    }
    return rstd::move(result).unwrap();
}
inline String GenLinkTex(isize id) { return rstd::format("{}{}", SpecTex_Link, id); }

inline bool IsImageLayerComposite(ref<str> name) {
    return name.starts_with(WE_IMAGE_LAYER_COMPOSITE_PREFIX);
}
// Parse <id> from `_rt_imageLayerComposite_<id>[_a|_b]`; None when it isn't a
// composite ref or no id digits follow the prefix.
inline Option<u32> ParseImageLayerCompositeId(ref<str> name) {
    auto rest = name.strip_prefix(WE_IMAGE_LAYER_COMPOSITE_PREFIX);
    if (rest.is_none()) return None();
    usize i {};
    u32   id {};
    for (; i < rest->size(); ++i) {
        auto value = (*rest)[i].to_primitive();
        if (value < '0' || value > '9') break;
        id = id * u32(10) + u32(value - '0');
    }
    if (i == usize()) return None();
    return Some(id);
}

} // namespace owe
