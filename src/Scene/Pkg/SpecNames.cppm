export module wescene.pkg.spec_names;
export import wescene.spec_names;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace rstd::literals;

#define BASE_GLTEX_NAMES(ext)                                                                      \
    "g_Texture0" #ext, "g_Texture1" #ext, "g_Texture2" #ext, "g_Texture3" #ext, "g_Texture4" #ext, \
        "g_Texture5" #ext, "g_Texture6" #ext, "g_Texture7" #ext, "g_Texture8" #ext,                \
        "g_Texture9" #ext, "g_Texture10" #ext, "g_Texture11" #ext, "g_Texture12" #ext

export namespace owe
{

inline constexpr array<std::string_view, 13> WE_GLTEX_NAMES { BASE_GLTEX_NAMES() };
inline constexpr array<std::string_view, 13> WE_GLTEX_RESOLUTION_NAMES { BASE_GLTEX_NAMES(
    Resolution) };
inline constexpr array<std::string_view, 13> WE_GLTEX_ROTATION_NAMES { BASE_GLTEX_NAMES(Rotation) };
inline constexpr array<std::string_view, 13> WE_GLTEX_TRANSLATION_NAMES { BASE_GLTEX_NAMES(
    Translation) };
inline constexpr array<std::string_view, 13> WE_GLTEX_MIPMAPINFO_NAMES { BASE_GLTEX_NAMES(
    MipMapInfo) };
inline constexpr array<std::string_view, 13> WE_GLTEX_TEXEL_NAMES { BASE_GLTEX_NAMES(Texel) };

inline constexpr ref<str> WE_FULL_COMPO_BUFFER_PREFIX    = "_rt_FullCompoBuffer"_str;
inline constexpr ref<str> WE_HALF_COMPO_BUFFER_PREFIX    = "_rt_HalfCompoBuffer"_str;
inline constexpr ref<str> WE_QUARTER_COMPO_BUFFER_PREFIX = "_rt_QuarterCompoBuffer"_str;
inline constexpr ref<str> WE_EIGHT_COMPO_BUFFER_PREFIX   = "_rt_EightBuffer"_str;
inline constexpr ref<str> WE_FULL_FRAME_BUFFER           = "_rt_FullFrameBuffer"_str;
inline constexpr ref<str> WE_SHADOW_ATLAS_PREFIX         = "_rt_shadowAtlas"_str;
inline constexpr ref<str> WE_VOLUMETRICS_PREFIX          = "_rt_volumetrics"_str;
inline constexpr ref<str> WE_QUARTER_FORCE_RG_PREFIX     = "_rt_QuarterForceRG"_str;
inline constexpr ref<str> WE_BLOOM_PREFIX                = "_rt_Bloom"_str;
inline constexpr ref<str> WE_QUARTER_FRAME_BUFFER_PREFIX = "_rt_QuarterFrameBuffer"_str;
inline constexpr ref<str> WE_EIGHTH_FRAME_BUFFER_PREFIX  = "_rt_EighthFrameBuffer"_str;
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX        = "_rt_effect_pingpong_"_str;
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX_A      = "_rt_effect_pingpong_a_"_str;
inline constexpr ref<str> OWE_EFFECT_PPONG_PREFIX_B      = "_rt_effect_pingpong_b_"_str;
inline constexpr ref<str> OWE_BLOOM_MIP_PREFIX           = "_rt_bloom_mip"_str;

inline constexpr ref<str> WE_CB_BLENDMODE            = "BLENDMODE"_str;
inline constexpr ref<str> WE_CB_BONECOUNT            = "BONECOUNT"_str;
inline constexpr ref<str> WE_CB_SPRITESHEET          = "SPRITESHEET"_str;
inline constexpr ref<str> WE_CB_SPRITESHEETBLENDNPOT = "SPRITESHEETBLENDNPOT"_str;
inline constexpr ref<str> WE_CB_THICK_FORMAT         = "THICKFORMAT"_str;
inline constexpr ref<str> WE_CB_TRAILRENDERER        = "TRAILRENDERER"_str;
inline constexpr ref<str> WE_CB_GS_ENABLED           = "GS_ENABLED"_str;
inline constexpr ref<str> WE_CB_LIGHTING             = "LIGHTING"_str;
inline constexpr ref<str> WE_CB_REFLECTION           = "REFLECTION"_str;
inline constexpr ref<str> WE_CB_NORMALMAP            = "NORMALMAP"_str;
inline constexpr ref<str> WE_CB_SCENE_ORTHO          = "SCENE_ORTHO"_str;
inline constexpr ref<str> WE_CB_MORPHING             = "MORPHING"_str;
inline constexpr ref<str> WE_CB_SKINNING             = "SKINNING"_str;
inline constexpr ref<str> WE_CB_POINTEMITTER         = "POINTEMITTER"_str;
inline constexpr ref<str> WE_CB_LINEEMITTER          = "LINEEMITTER"_str;
inline constexpr ref<str> WE_PRENDER_ROPE            = "PRENDER_ROPE"_str;
inline constexpr ref<str> WE_PRENDER_ROPE_TRAIL      = "PRENDER_ROPE_TRAIL"_str;
inline constexpr ref<str> OWE_CB_IMAGE_LAYER         = "OWE_IMAGE_LAYER"_str;

inline constexpr ref<str> G_M                       = "g_ModelMatrix"_str;
inline constexpr ref<str> G_VP                      = "g_ViewProjectionMatrix"_str;
inline constexpr ref<str> G_MVP                     = "g_ModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_AM                      = "g_AltModelMatrix"_str;
inline constexpr ref<str> G_ALTVIEWPROJECTIONMATRIX = "g_AltViewProjectionMatrix"_str;
inline constexpr ref<str> G_MI                      = "g_ModelMatrixInverse"_str;
inline constexpr ref<str> G_MVPI                    = "g_ModelViewProjectionMatrixInverse"_str;
inline constexpr ref<str> G_EYEPOSITION             = "g_EyePosition"_str;
inline constexpr ref<str> G_EFFECTMODELMATRIX       = "g_EffectModelMatrix"_str;
inline constexpr ref<str> G_EFFECTMODELVIEWPROJECTIONMATRIX =
    "g_EffectModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE =
    "g_EffectModelViewProjectionMatrixInverse"_str;
inline constexpr ref<str> G_EFFECTTEXTUREPROJECTIONMATRIX = "g_EffectTextureProjectionMatrix"_str;
inline constexpr ref<str> G_EFFECTTEXTUREPROJECTIONMATRIXINVERSE =
    "g_EffectTextureProjectionMatrixInverse"_str;
inline constexpr ref<str> G_LAYERMODELMATRIX   = "g_LayerModelMatrix"_str;
inline constexpr ref<str> G_EMVP               = "g_EffectModelViewProjectionMatrix"_str;
inline constexpr ref<str> G_ETVP               = "g_EffectTextureProjectionMatrix"_str;
inline constexpr ref<str> G_ETVPI              = "g_EffectTextureProjectionMatrixInverse"_str;
inline constexpr ref<str> G_LP                 = "g_LightsPosition"_str;
inline constexpr ref<str> G_LCP                = "g_LightsColorPremultiplied"_str;
inline constexpr ref<str> G_LCR                = "g_LightsColorRadius"_str;
inline constexpr ref<str> G_LIGHTDIRECTIONTYPE = "g_LightsDirectionType"_str;
inline constexpr ref<str> G_LIGHTCONEEXPONENT  = "g_LightsConeExponent"_str;
inline constexpr ref<str> G_LIGHTCASTSHADOW    = "g_LightsCastShadow"_str;
inline constexpr ref<str> G_LIGHTAMBIENTCOLOR  = "g_LightAmbientColor"_str;
inline constexpr ref<str> G_LIGHTSKYLIGHTCOLOR = "g_LightSkylightColor"_str;
inline constexpr ref<str> G_FOGDISTANCECOLOR   = "g_FogDistanceColor"_str;
inline constexpr ref<str> G_FOGDISTANCEPARAMS  = "g_FogDistanceParams"_str;
inline constexpr ref<str> G_FOGHEIGHTCOLOR     = "g_FogHeightColor"_str;
inline constexpr ref<str> G_FOGHEIGHTPARAMS    = "g_FogHeightParams"_str;

inline constexpr ref<str> G_TIME                           = "g_Time"_str;
inline constexpr ref<str> G_FRAMETIME                      = "g_Frametime"_str;
inline constexpr ref<str> G_DAYTIME                        = "g_Daytime"_str;
inline constexpr ref<str> G_DAYTIME_LEGACY                 = "g_DayTime"_str;
inline constexpr ref<str> G_POINTERPOSITION                = "g_PointerPosition"_str;
inline constexpr ref<str> G_POINTERPOSITIONLAST            = "g_PointerPositionLast"_str;
inline constexpr ref<str> G_TEXELSIZE                      = "g_TexelSize"_str;
inline constexpr ref<str> G_TEXELSIZEHALF                  = "g_TexelSizeHalf"_str;
inline constexpr ref<str> G_TEXTURE0SAMPLERSTATE           = "g_Texture0SamplerState"_str;
inline constexpr ref<str> G_BONES                          = "g_Bones"_str;
inline constexpr ref<str> G_BONESALPHA                     = "g_BonesAlpha"_str;
inline constexpr ref<str> G_BLENDMAP                       = "g_BlendMap"_str;
inline constexpr ref<str> G_SCREEN                         = "g_Screen"_str;
inline constexpr ref<str> G_PARALLAXPOSITION               = "g_ParallaxPosition"_str;
inline constexpr ref<str> G_MORPHWEIGHTS                   = "g_MorphWeights"_str;
inline constexpr ref<str> G_MORPHOFFSETS                   = "g_MorphOffsets"_str;
inline constexpr ref<str> G_VIEWPORTVIEWPROJECTIONMATRICES = "g_ViewportViewProjectionMatrices"_str;
inline constexpr ref<str> G_SHADOWATLASTRANSFORMS          = "g_ShadowAtlasTransforms"_str;
inline constexpr ref<str> G_VIEWUP                         = "g_ViewUp"_str;
inline constexpr ref<str> G_VIEWRIGHT                      = "g_ViewRight"_str;
inline constexpr ref<str> G_VIEWFORWARD                    = "g_ViewForward"_str;
inline constexpr ref<str> G_ORIENTATIONUP                  = "g_OrientationUp"_str;
inline constexpr ref<str> G_ORIENTATIONRIGHT               = "g_OrientationRight"_str;
inline constexpr ref<str> G_ORIENTATIONFORWARD             = "g_OrientationForward"_str;
inline constexpr ref<str> G_NORMALMODELMATRIX              = "g_NormalModelMatrix"_str;
inline constexpr ref<str> G_COLOR4                         = "g_Color4"_str;
inline constexpr ref<str> G_COLOR                          = "g_Color"_str;
inline constexpr ref<str> G_ALPHA                          = "g_Alpha"_str;
inline constexpr ref<str> G_USERALPHA                      = "g_UserAlpha"_str;
inline constexpr ref<str> G_BRIGHTNESS                     = "g_Brightness"_str;
inline constexpr ref<str> G_RENDERVAR0                     = "g_RenderVar0"_str;
inline constexpr ref<str> G_RENDERVAR1                     = "g_RenderVar1"_str;
inline constexpr ref<str> G_RENDERVAR2                     = "g_RenderVar2"_str;
inline constexpr ref<str> G_AUDIOFREQUENCYMIN              = "g_AudioFrequencyMin"_str;
inline constexpr ref<str> G_AUDIOFREQUENCYMAX              = "g_AudioFrequencyMax"_str;

inline constexpr ref<str> G_AUDIO_SPEC_16_L = "g_AudioSpectrum16Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_16_R = "g_AudioSpectrum16Right"_str;
inline constexpr ref<str> G_AUDIO_SPEC_32_L = "g_AudioSpectrum32Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_32_R = "g_AudioSpectrum32Right"_str;
inline constexpr ref<str> G_AUDIO_SPEC_64_L = "g_AudioSpectrum64Left"_str;
inline constexpr ref<str> G_AUDIO_SPEC_64_R = "g_AudioSpectrum64Right"_str;

} // namespace owe

#undef BASE_GLTEX_NAMES
