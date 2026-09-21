module;

#include <rstd/macro.hpp>
#include <spirv_reflect.h>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>

#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#    include <glslang/SPIRV/GlslangToSpv.h>
#else
#    include <SPIRV/GlslangToSpv.h>
#endif

module wescene.shader_compile;
import wescene.core;
import wescene.types;
import wescene.utils;
import rstd;
import rstd.log;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeMap;
using rstd::cppstd::as_str;
using rstd::ffi::CStr;
using rstd::ffi::CString;
using rstd::path::PathBuf;
using rstd::sync::atomic::Atomic;
using namespace owe;
using namespace owe::vulkan;

namespace owe::vulkan
{
struct GlslangBackend {};
} // namespace owe::vulkan

namespace rstd
{
template<>
struct Impl<vrento::vulkan::ShaderBackend, owe::vulkan::GlslangBackend>
    : ImplBase<owe::vulkan::GlslangBackend> {
    bool Preprocess(ref<str> source, ShaderType stage, SourceLang lang, String& output) const {
        return owe::vulkan::Preprocess(source, stage, lang, output);
    }
    bool CompileAndLinkShaderUnits(slice<ShaderCompUnit> units, const ShaderCompOpt& options,
                                   Vec<Uni_ShaderSpv>& output) const {
        return owe::vulkan::CompileAndLinkShaderUnits(units, options, output);
    }
    bool GenReflect(slice<ShaderCode> codes, Vec<Uni_ShaderSpv>& stages,
                    ShaderReflected& output) const {
        return owe::vulkan::GenReflect(codes, stages, output);
    }
};
} // namespace rstd

auto owe::vulkan::MakeShaderBackend() -> Box<dyn<ShaderBackend>> {
    return Box<dyn<ShaderBackend>>::make(GlslangBackend {});
}

namespace
{

struct GlslangProcessRuntime {
    GlslangProcessRuntime(): active(glslang::InitializeProcess()) { rstd_assert(active); }
    GlslangProcessRuntime(const GlslangProcessRuntime&) = delete;
    GlslangProcessRuntime(GlslangProcessRuntime&& other) noexcept: active(other.active) {
        other.active = false;
    }
    ~GlslangProcessRuntime() {
        if (active) glslang::FinalizeProcess();
    }

    bool active { false };
};

void EnsureGlslangProcess() {
    static const rstd::sync::OnceLock<GlslangProcessRuntime> runtime;
    (void)runtime.get_or_init([] {
        return GlslangProcessRuntime {};
    });
}

ShaderScalarKind ReflectedScalarKind(const SpvReflectBlockVariable& variable) {
    if (variable.type_description == nullptr) return ShaderScalarKind::Unknown;
    const auto flags = variable.type_description->type_flags;
    if ((flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) return ShaderScalarKind::Float;
    if ((flags & SPV_REFLECT_TYPE_FLAG_BOOL) != 0) return ShaderScalarKind::Boolean;
    if ((flags & SPV_REFLECT_TYPE_FLAG_INT) == 0) return ShaderScalarKind::Unknown;
    return variable.numeric.scalar.signedness != 0 ? ShaderScalarKind::SignedInteger
                                                   : ShaderScalarKind::UnsignedInteger;
}

ShaderMatrixMajor ReflectedMatrixMajor(const SpvReflectBlockVariable& variable) {
    if ((variable.decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0)
        return ShaderMatrixMajor::Row;
    if ((variable.decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0)
        return ShaderMatrixMajor::Column;
    return ShaderMatrixMajor::None;
}

unsigned ReflectedMatrixStride(const SpvReflectBlockVariable& variable, ShaderMatrixMajor major) {
    if (variable.numeric.matrix.stride != 0) return variable.numeric.matrix.stride;
    const auto major_count = major == ShaderMatrixMajor::Row ? variable.numeric.matrix.row_count
                                                             : variable.numeric.matrix.column_count;
    if (major_count == 0 || variable.array.stride == 0 ||
        variable.array.stride % major_count != 0) {
        return 0;
    }
    return variable.array.stride / major_count;
}

usize ReflectedArrayCount(const SpvReflectArrayTraits& array) {
    usize count { 1 };
    for (rstd::uint32_t index = 0; index < array.dims_count; ++index) {
        count *= usize(array.dims[index]);
    }
    return count;
}

auto ReflectBlockedUniform(const SpvReflectBlockVariable& variable, int block_index)
    -> ShaderReflected::BlockedUniform {
    ShaderReflected::BlockedUniform uniform {};
    uniform.block_index  = block_index;
    uniform.size         = usize(variable.size);
    uniform.offset       = variable.offset;
    uniform.num          = ReflectedArrayCount(variable.array);
    uniform.scalar_kind  = ReflectedScalarKind(variable);
    uniform.scalar_width = variable.numeric.scalar.width;
    uniform.vector_components =
        variable.numeric.vector.component_count == 0 ? 1 : variable.numeric.vector.component_count;
    uniform.matrix_rows    = variable.numeric.matrix.row_count;
    uniform.matrix_columns = variable.numeric.matrix.column_count;
    uniform.matrix_major   = ReflectedMatrixMajor(variable);
    uniform.matrix_stride  = ReflectedMatrixStride(variable, uniform.matrix_major);
    uniform.array_stride   = variable.array.stride;
    uniform.array_dimensions.reserve(usize(variable.array.dims_count));
    for (rstd::uint32_t dimension = 0; dimension < variable.array.dims_count; ++dimension) {
        uniform.array_dimensions.push(u32(variable.array.dims[dimension]));
    }
    return uniform;
}

bool SameBlockedUniformLayout(const ShaderReflected::BlockedUniform& lhs,
                              const ShaderReflected::BlockedUniform& rhs) {
    return lhs.offset == rhs.offset && lhs.size == rhs.size && lhs.num == rhs.num &&
           lhs.scalar_kind == rhs.scalar_kind && lhs.scalar_width == rhs.scalar_width &&
           lhs.vector_components == rhs.vector_components && lhs.matrix_rows == rhs.matrix_rows &&
           lhs.matrix_columns == rhs.matrix_columns && lhs.matrix_stride == rhs.matrix_stride &&
           lhs.matrix_major == rhs.matrix_major && lhs.array_stride == rhs.array_stride &&
           lhs.array_dimensions == rhs.array_dimensions;
}

bool SameUniformBlockLayout(const ShaderReflected::Block&  reflected,
                            const SpvReflectBlockVariable& block) {
    if (reflected.size != block.size || reflected.member_map.len() != usize(block.member_count))
        return false;
    for (rstd::uint32_t index = 0; index < block.member_count; ++index) {
        const auto& variable = block.members[index];
        auto        name     = as_str(variable.name);
        if (name.is_err()) return false;
        auto existing = reflected.member_map.get(name.unwrap());
        if (existing.is_none() ||
            ! SameBlockedUniformLayout(**existing,
                                       ReflectBlockedUniform(variable, reflected.index))) {
            return false;
        }
    }
    return true;
}

// Spill a payload to /tmp/<sha1> for post-mortem inspection. Returns the
// written path so callers can mention it in the error message.
PathBuf logToTmpfileWithSha1(ref<str> source) {
    auto digest   = utils::genSha1(slice<rstd::byte>::from_raw_parts(source.data(), source.len()));
    auto path     = rstd::env::temp_dir().join(digest.as_str());
    auto contents = String::make(source);
    contents.push_str("\n"_str);
    (void)rstd::fs::write(path.as_path(), contents.as_str().as_bytes());
    return path;
}

inline VkShaderStageFlagBits ToVkType(owe::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
    }
    rstd_assert(false);
    return VK_SHADER_STAGE_VERTEX_BIT;
}

inline VkFormat ToVkType(SpvReflectFormat type) { return static_cast<VkFormat>(type); }

inline VkShaderStageFlagBits ToVkType(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_GEOMETRY_BIT;
    default: rstd_assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline owe::ShaderType FromSpvStage(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return ShaderType::VERTEX;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return ShaderType::FRAGMENT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return ShaderType::GEOMETRY;
    default: rstd_assert(false); return ShaderType::VERTEX;
    }
}

template<typename T, typename FUNC>
bool EnumAllRef(Vec<T>& vec, FUNC&& func) {
    unsigned count { 0 };
    auto     result = func(&count, nullptr);
    rstd_assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(usize(count), T {});
    result = func(&count, vec.as_mut_ptr().as_raw_ptr());
    rstd_assert(result == SPV_REFLECT_RESULT_SUCCESS);
    return result == SPV_REFLECT_RESULT_SUCCESS;
}

inline EShLanguage ToEShLanguage(owe::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX: return EShLangVertex;
    case ShaderType::FRAGMENT: return EShLangFragment;
    case ShaderType::GEOMETRY: return EShLangGeometry;
    }
    rstd_assert(false);
    return EShLangVertex;
}

inline glslang::EShTargetClientVersion ToClientVersion(VulkanTarget t) {
    switch (t) {
    case VulkanTarget::Vulkan_1_0: return glslang::EShTargetVulkan_1_0;
    case VulkanTarget::Vulkan_1_1: return glslang::EShTargetVulkan_1_1;
    case VulkanTarget::Vulkan_1_2: return glslang::EShTargetVulkan_1_2;
    case VulkanTarget::Vulkan_1_3: return glslang::EShTargetVulkan_1_3;
    }
    return glslang::EShTargetVulkan_1_1;
}

inline glslang::EShTargetLanguageVersion ToSpvVersion(VulkanTarget t) {
    // Pair Vulkan target with the matching SPIR-V version. See
    // https://github.com/KhronosGroup/glslang/blob/main/StandAlone/StandAlone.cpp
    switch (t) {
    case VulkanTarget::Vulkan_1_0: return glslang::EShTargetSpv_1_0;
    case VulkanTarget::Vulkan_1_1: return glslang::EShTargetSpv_1_3;
    case VulkanTarget::Vulkan_1_2: return glslang::EShTargetSpv_1_5;
    case VulkanTarget::Vulkan_1_3: return glslang::EShTargetSpv_1_6;
    }
    return glslang::EShTargetSpv_1_3;
}

inline const char* DefaultEntryName(SourceLang lang, owe::ShaderType s) {
    if (lang == SourceLang::Glsl) return "main";
    switch (s) {
    case ShaderType::VERTEX: return "main_vs";
    case ShaderType::FRAGMENT: return "main_ps";
    case ShaderType::GEOMETRY: return "main_gs";
    }
    return "main";
}

} // namespace

bool owe::vulkan::GenReflect(slice<ShaderCode> codes, Vec<Uni_ShaderSpv>& spvs,
                             ShaderReflected& ref) {
    spvs.clear();
    BTreeMap<String, usize> uniform_block_indices;
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code.len().to_primitive() * sizeof(rstd::uint32_t),
                                          code.data(),
                                          SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            auto spv   = Box<ShaderSpv>::make();
            spv->stage = ::FromSpvStage(spv_ref.GetShaderStage());
            spv->spirv = code.clone();
            if (const char* ep = spv_ref.GetEntryPointName(); ep && ep[0] != '\0') {
                spv->entry_point = rstd::into(as_str(ep).unwrap());
            }
            spvs.push(rstd::move(spv));
        }
        Vec<SpvReflectInterfaceVariable*> inputs;
        Vec<SpvReflectDescriptorBinding*> bindings;

        bool ok = EnumAllRef(bindings, [&](auto&&... args) {
            return spv_ref.EnumerateDescriptorBindings(args...);
        });
        if (! ok) return false;

        VkDescriptorSetLayoutBinding vkbinding {};
        vkbinding.stageFlags = stage;

        for (auto pb : bindings) {
            auto& b = *pb;
            if (! b.accessed && b.descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                continue;

            auto name = as_str(b.name[0] == '\0' && b.type_description->type_name != nullptr
                                   ? b.type_description->type_name
                                   : b.name);
            if (name.is_err()) return false;
            const auto bind_name = name.unwrap();

            if (auto existing = ref.binding_map.get_mut(bind_name); existing.is_some()) {
                auto&      bind = **existing;
                const auto descriptor_type =
                    b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                        ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                        : static_cast<VkDescriptorType>(b.descriptor_type);
                if (bind.set != b.set || bind.layout.binding != b.binding ||
                    bind.layout.descriptorType != descriptor_type) {
                    rstd_error("descriptor {} differs across shader stages", bind_name);
                    return false;
                }
                if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    auto block_index = uniform_block_indices.get(bind_name);
                    if (block_index.is_none() ||
                        ! SameUniformBlockLayout(ref.blocks[**block_index], b.block)) {
                        rstd_error("uniform block {} layout differs across shader stages",
                                   bind_name);
                        return false;
                    }
                }
                bind.layout.stageFlags |= stage;
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                auto& block      = b.block;
                auto  block_name = as_str(block.name);
                if (block_name.is_err()) return false;
                ref.blocks.push(ShaderReflected::Block {
                    .index      = static_cast<int>(ref.blocks.len().to_primitive()),
                    .size       = block.size,
                    .name       = rstd::into(block_name.unwrap()->is_empty() ? bind_name
                                                                             : block_name.unwrap()),
                    .set        = b.set,
                    .binding    = b.binding,
                    .member_map = {} });
                auto& ref_block = ref.blocks[ref.blocks.len() - usize(1)];
                (void)uniform_block_indices.insert(rstd::into(bind_name),
                                                   ref.blocks.len() - usize(1));

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (rstd::uint32_t i = 0; i < block.member_count; i++) {
                    auto& unif        = block.members[i];
                    auto  member_name = as_str(unif.name);
                    if (member_name.is_err()) return false;
                    (void)ref_block.member_map.insert(rstd::into(member_name.unwrap()),
                                                      ReflectBlockedUniform(unif, ref_block.index));
                }
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                // Our HLSL synth emits `Texture2D + SamplerState` at the same
                // binding with `[[vk::combinedImageSampler]]`. glslang doesn't
                // expand that attribute into an OpTypeSampledImage; SPIRV-Reflect
                // sees SAMPLED_IMAGE (the Texture2D half) accessed and the
                // SamplerState as unaccessed. Bind it as a VK combined image
                // sampler — per Vulkan 1.0 §14.5.2 a COMBINED_IMAGE_SAMPLER
                // descriptor is legal to access via either OpTypeSampledImage
                // or separate OpTypeImage/OpTypeSampler.
                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER) {
                // The paired SamplerState half of the combined image sampler
                // pair — already covered by the SAMPLED_IMAGE entry at the
                // same binding.
                continue;
            } else {
                rstd_error("unknown DescriptorBinding {}", (int)b.descriptor_type);
                return false;
            }

            (void)ref.binding_map.insert(rstd::into(bind_name),
                                         ShaderReflected::Binding {
                                             .set    = b.set,
                                             .layout = vkbinding,
                                         });
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if ((input.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) continue;

                if (input.location == u32::MAX.to_primitive()) {
                    rstd_error("shader input {} no location", input.name);
                    return false;
                }
                ShaderReflected::Input rinput;
                rinput.location = input.location;
                rinput.format   = ::ToVkType(input.format);

                // Strip HLSL struct-prefixed names. glslang HLSL emits VS
                // inputs as `<entry_param_name>.<field>` (e.g. our wrapper's
                // `_ww_in.a_Position`). DXC used `in.var.<SEMANTIC>`. C++
                // vertex-buffer layout matches by bare attribute name
                // (`a_Position`), so strip everything up to the final `.`.
                if (! input.name) return false;
                auto input_name = CStr::from_ptr(input.name).to_str();
                if (input_name.is_err()) return false;
                auto name = input_name.unwrap();
                if (auto split = name.rsplit_once("."_str)) name = split->template get<1>();
                (void)ref.input_location_map.insert(String::make(name), rstd::move(rinput));
            }
        }
    }
    return true;
}

namespace
{

// Configure a TShader for our Vulkan target. The user shader has already
// gone through PreShaderHeader (combo `#define`s + GLSL prologue + the
// __SHADER_PLACEHOLD__ slot for synthesized layouts) so glslang sees
// fully resolved source.
void ConfigureShader(glslang::TShader& shader, SourceLang lang, VulkanTarget target,
                     const char* entry) {
    glslang::EShSource src_lang =
        (lang == SourceLang::Hlsl) ? glslang::EShSourceHlsl : glslang::EShSourceGlsl;
    shader.setEnvInput(src_lang, shader.getStage(), glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, ToClientVersion(target));
    shader.setEnvTarget(glslang::EShTargetSpv, ToSpvVersion(target));
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    // We emit explicit `layout(location=N)` and `layout(set=B, binding=K)`
    // ourselves from Finalprocessor, so glslang's auto-binding stays off.
    shader.setAutoMapLocations(false);
    shader.setAutoMapBindings(false);
    // Be lenient about combinations of #version / unset clip ranges in WE
    // shaders — they mostly look like GLSL 110 desktop with Vulkan semantics
    // grafted on top.
    shader.setEnvInputVulkanRulesRelaxed();
}

constexpr EShMessages kCompileMessages =
    static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors |
                             EShMsgSuppressWarnings | EShMsgKeepUncalled);

} // namespace

bool owe::vulkan::Preprocess(ref<str> src, ShaderType stage, SourceLang lang, String& out) {
    EnsureGlslangProcess();
    glslang::TShader shader(ToEShLanguage(stage));
    const auto       source = src;
    if (source.len() > usize(i32::MAX.to_primitive())) return false;
    const char* data = reinterpret_cast<const char*>(source.data());
    const int   len  = static_cast<int>(source.len().to_primitive());
    const char* name = "ww";
    shader.setStringsWithLengthsAndNames(&data, &len, &name, 1);
    ConfigureShader(shader, lang, VulkanTarget::Vulkan_1_1, DefaultEntryName(lang, stage));

    const int                        default_version = 110;
    const EProfile                   profile         = ECoreProfile;
    const bool                       forward_compat  = false;
    glslang::TShader::ForbidIncluder includer;

    std::string preprocessed;
    bool        ok = shader.preprocess(GetDefaultResources(),
                                       default_version,
                                       profile,
                                       false,
                                       forward_compat,
                                       kCompileMessages,
                                       &preprocessed,
                                       includer);
    if (! ok) {
        auto tmp = logToTmpfileWithSha1(source);
        rstd_error("glslang(preprocess): {}", shader.getInfoLog());
        rstd_error("shader source is at {}", tmp.as_path().to_string_lossy());
        return false;
    }
    auto text = as_str(preprocessed);
    if (text.is_err()) return false;
    out = rstd::into(text.unwrap());
    return true;
}

bool owe::vulkan::CompileAndLinkShaderUnits(slice<ShaderCompUnit> compUnits,
                                            const ShaderCompOpt& opt, Vec<Uni_ShaderSpv>& spvs) {
    EnsureGlslangProcess();
    spvs.clear();
    spvs.reserve(compUnits.len());

    for (const auto& unit : compUnits) {
        auto entry_name =
            unit.entry_point.is_empty()
                ? CStr::from_ptr(DefaultEntryName(unit.lang, unit.stage)).to_str().unwrap()
                : unit.entry_point.as_str();
        auto encoded_entry = CString::make(String::make(entry_name));
        if (encoded_entry.is_err()) return false;
        auto        entry_c = rstd::move(encoded_entry).unwrap();
        const char* entry   = entry_c.as_ptr();

        glslang::TShader shader(ToEShLanguage(unit.stage));
        const auto       source = unit.src.as_str();
        if (source.len() > usize(i32::MAX.to_primitive())) return false;
        const char* data = reinterpret_cast<const char*>(source.data());
        const int   len  = static_cast<int>(source.len().to_primitive());
        const char* name = "ww";
        shader.setStringsWithLengthsAndNames(&data, &len, &name, 1);
        ConfigureShader(shader, unit.lang, opt.target, entry);

        const int                        default_version = 110;
        const EProfile                   profile         = ECoreProfile;
        const bool                       forward_compat  = false;
        glslang::TShader::ForbidIncluder includer;

        if (! shader.parse(GetDefaultResources(),
                           default_version,
                           profile,
                           false,
                           forward_compat,
                           kCompileMessages,
                           includer)) {
            auto tmp = logToTmpfileWithSha1(source);
            // Strip WARNING lines; EShMsgSuppressWarnings doesn't actually
            // omit them from the info log on this glslang build.
            auto   log       = CStr::from_ptr(shader.getInfoLog()).to_str();
            auto   remaining = log.unwrap_or(""_str);
            String filtered;
            while (! remaining.is_empty()) {
                auto split = remaining.split_once("\n"_str);
                auto line  = split ? split->template get<0>() : remaining;
                if (! line.contains("WARNING"_str)) {
                    filtered.push_str(line);
                    filtered.push_str("\n"_str);
                }
                remaining = split ? split->template get<1>() : ""_str;
            }
            if (log.is_ok())
                rstd_error("glslang(parse): {}", filtered);
            else
                rstd_error("glslang(parse): {}", shader.getInfoLog());
            if (const char* d = shader.getInfoDebugLog(); d && d[0])
                rstd_error("glslang(parse debug): {}", d);
            rstd_error("shader source is at {}", tmp.as_path().to_string_lossy());
            return false;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (! program.link(kCompileMessages)) {
            auto tmp = logToTmpfileWithSha1(source);
            rstd_error("glslang(link): {}", program.getInfoLog());
            rstd_error("shader source is at {}", tmp.as_path().to_string_lossy());
            return false;
        }

        glslang::TIntermediate* intermediate = program.getIntermediate(ToEShLanguage(unit.stage));
        if (! intermediate) {
            rstd_error("glslang(intermediate): no intermediate for stage");
            return false;
        }

        glslang::SpvOptions spv_opts;
        spv_opts.validate          = true;
        spv_opts.generateDebugInfo = false;
        spv_opts.disableOptimizer  = ! opt.optimize;
        spv::SpvBuildLogger logger;

        auto spv         = Box<ShaderSpv>::make();
        spv->stage       = unit.stage;
        spv->entry_point = String::make(entry_name);
        std::vector<unsigned int> words;
        glslang::GlslangToSpv(*intermediate, words, &logger, &spv_opts);
        spv->spirv = ShaderCode::from(
            slice<rstd::uint32_t>::from_raw_parts(words.data(), usize(words.size())));

        if (auto msgs = logger.getAllMessages(); ! msgs.empty()) {
            rstd_warn("glslang(spv): {}", msgs);
        }
        if (spv->spirv.is_empty()) {
            rstd_error("glslang(spv): no SPIR-V output produced");
            return false;
        }

        if (rstd::env::var_os("WP_DUMP_SPIRV"_str).is_some()) {
            static Atomic<u64> dump_idx {};
            auto base = rstd::format("/tmp/ww_dump_{}_{}", dump_idx.fetch_add(u64(1)), entry_name);
            auto spv_path = PathBuf::from(rstd::format("{}.spv", base).as_str());
            auto src_path = PathBuf::from(rstd::format("{}.glsl", base).as_str());
            (void)rstd::fs::write(
                spv_path.as_path(),
                slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(spv->spirv.data()),
                                          spv->spirv.len() * usize(sizeof(rstd::uint32_t))));
            (void)rstd::fs::write(src_path.as_path(), source.as_bytes());
            rstd_info("dumped SPIR-V + source: {}.{{spv,glsl}}", base);
        }

        spvs.push(rstd::move(spv));
    }

    return true;
}
