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

#include "Utils/Sha.hpp"
module wescene.shader_compile;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;

using namespace rstd::prelude;
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
    bool Preprocess(std::string_view source, ShaderType stage, SourceLang lang,
                    std::string& output) const {
        return owe::vulkan::Preprocess(source, stage, lang, output);
    }
    bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> units,
                                   const ShaderCompOpt&            options,
                                   std::vector<Uni_ShaderSpv>&     output) const {
        return owe::vulkan::CompileAndLinkShaderUnits(units, options, output);
    }
    bool GenReflect(std::span<const std::vector<unsigned int>> codes,
                    std::vector<Uni_ShaderSpv>& stages, ShaderReflected& output) const {
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
    for (std::uint32_t index = 0; index < array.dims_count; ++index) {
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
    uniform.array_dimensions.reserve(variable.array.dims_count);
    for (std::uint32_t dimension = 0; dimension < variable.array.dims_count; ++dimension) {
        uniform.array_dimensions.push_back(variable.array.dims[dimension]);
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
    if (reflected.size != block.size || reflected.member_map.size() != block.member_count)
        return false;
    for (std::uint32_t index = 0; index < block.member_count; ++index) {
        const auto& variable = block.members[index];
        auto        existing = reflected.member_map.find(variable.name);
        if (existing == reflected.member_map.end() ||
            ! SameBlockedUniformLayout(existing->second,
                                       ReflectBlockedUniform(variable, reflected.index))) {
            return false;
        }
    }
    return true;
}

// Spill a payload to /tmp/<sha1> for post-mortem inspection. Returns the
// written path so callers can mention it in the error message.
std::string logToTmpfileWithSha1(std::span<const char> in) {
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "wb");
    if (! file) return path;
    std::fwrite(in.data(), 1, in.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
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

template<typename VEC, typename FUNC>
bool EnumAllRef(VEC& vec, FUNC&& func) {
    unsigned count { 0 };
    auto     result = func(&count, nullptr);
    rstd_assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(count);
    result = func(&count, vec.data());
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

bool owe::vulkan::GenReflect(std::span<const std::vector<unsigned>> codes,
                             std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& ref) {
    spvs.clear();
    Map<std::string, usize> uniform_block_indices;
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code, SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            auto spv   = Box<ShaderSpv>::make();
            spv->stage = ::FromSpvStage(spv_ref.GetShaderStage());
            spv->spirv = code;
            if (const char* ep = spv_ref.GetEntryPointName(); ep && ep[0] != '\0') {
                spv->entry_point = ep;
            }
            spvs.emplace_back(std::move(spv));
        }
        std::vector<SpvReflectInterfaceVariable*> inputs;
        std::vector<SpvReflectDescriptorBinding*> bindings;

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

            auto bind_name = std::string(b.name).empty() && b.type_description->type_name != nullptr
                                 ? b.type_description->type_name
                                 : b.name;

            if (exists(ref.binding_map, bind_name)) {
                auto&      bind = ref.binding_map[bind_name];
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
                    auto block_index = uniform_block_indices.find(bind_name);
                    if (block_index == uniform_block_indices.end() ||
                        ! SameUniformBlockLayout(ref.blocks[block_index->second.to_primitive()],
                                                 b.block)) {
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
                auto  block_name = std::string(block.name).empty() ? bind_name : block.name;
                ref.blocks.push_back(
                    ShaderReflected::Block { .index      = static_cast<int>(ref.blocks.size()),
                                             .size       = block.size,
                                             .name       = std::move(block_name),
                                             .set        = b.set,
                                             .binding    = b.binding,
                                             .member_map = {} });
                auto& ref_block                  = ref.blocks.back();
                uniform_block_indices[bind_name] = usize(ref.blocks.size() - 1);

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (std::uint32_t i = 0; i < block.member_count; i++) {
                    auto& unif                      = block.members[i];
                    ref_block.member_map[unif.name] = ReflectBlockedUniform(unif, ref_block.index);
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

            ref.binding_map[bind_name] = ShaderReflected::Binding {
                .set    = b.set,
                .layout = vkbinding,
            };
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if ((input.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) continue;

                if (input.location == std::numeric_limits<decltype(input.location)>::max()) {
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
                std::string_view name = input.name;
                if (auto dot = name.rfind('.'); dot != std::string_view::npos) {
                    name.remove_prefix(dot + 1);
                }
                ref.input_location_map[std::string(name)] = rinput;
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

bool owe::vulkan::Preprocess(std::string_view src, ShaderType stage, SourceLang lang,
                             std::string& out) {
    EnsureGlslangProcess();
    glslang::TShader shader(ToEShLanguage(stage));
    std::string      src_copy(src);
    const char*      data = src_copy.c_str();
    const int        len  = (int)src_copy.size();
    const char*      name = "ww";
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
        std::string tmp = logToTmpfileWithSha1(src);
        rstd_error("glslang(preprocess): {}", shader.getInfoLog());
        rstd_error("shader source is at {}", tmp);
        return false;
    }
    out = std::move(preprocessed);
    return true;
}

bool owe::vulkan::CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits,
                                            const ShaderCompOpt&            opt,
                                            std::vector<Uni_ShaderSpv>&     spvs) {
    EnsureGlslangProcess();
    spvs.clear();
    spvs.reserve(compUnits.size());

    for (const auto& unit : compUnits) {
        const std::string entry_str =
            unit.entry_point.empty() ? DefaultEntryName(unit.lang, unit.stage) : unit.entry_point;
        const char* entry = entry_str.c_str();

        glslang::TShader shader(ToEShLanguage(unit.stage));
        const char*      data = unit.src.c_str();
        const int        len  = (int)unit.src.size();
        const char*      name = "ww";
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
            std::string tmp = logToTmpfileWithSha1(unit.src);
            // Strip WARNING lines; EShMsgSuppressWarnings doesn't actually
            // omit them from the info log on this glslang build.
            std::string log = shader.getInfoLog();
            std::string filtered;
            for (std::size_t i = 0, e = log.size(); i < e;) {
                std::size_t nl = log.find('\n', i);
                if (nl == std::string::npos) nl = e;
                std::string_view line(log.data() + i, nl - i);
                if (line.find("WARNING") == std::string_view::npos) {
                    filtered.append(line);
                    filtered.push_back('\n');
                }
                i = nl + 1;
            }
            rstd_error("glslang(parse): {}", filtered);
            if (const char* d = shader.getInfoDebugLog(); d && d[0])
                rstd_error("glslang(parse debug): {}", d);
            rstd_error("shader source is at {}", tmp);
            return false;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (! program.link(kCompileMessages)) {
            std::string tmp = logToTmpfileWithSha1(unit.src);
            rstd_error("glslang(link): {}", program.getInfoLog());
            rstd_error("shader source is at {}", tmp);
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
        spv->entry_point = entry_str;
        glslang::GlslangToSpv(*intermediate, spv->spirv, &logger, &spv_opts);

        if (auto msgs = logger.getAllMessages(); ! msgs.empty()) {
            rstd_warn("glslang(spv): {}", msgs);
        }
        if (spv->spirv.empty()) {
            rstd_error("glslang(spv): no SPIR-V output produced");
            return false;
        }

        if (std::getenv("WP_DUMP_SPIRV")) {
            static int  dump_idx = 0;
            std::string base     = "/tmp/ww_dump_" + std::to_string(dump_idx++) + "_" + entry_str;
            std::string spv_path = base + ".spv";
            std::string src_path = base + ".glsl";
            if (auto* f = std::fopen(spv_path.c_str(), "wb")) {
                std::fwrite(spv->spirv.data(), sizeof(u32), spv->spirv.size(), f);
                std::fclose(f);
            }
            if (auto* f = std::fopen(src_path.c_str(), "wb")) {
                std::fwrite(unit.src.data(), 1, unit.src.size(), f);
                std::fclose(f);
            }
            rstd_info("dumped SPIR-V + source: {}.{{spv,glsl}}", base);
        }

        spvs.emplace_back(std::move(spv));
    }

    return true;
}
