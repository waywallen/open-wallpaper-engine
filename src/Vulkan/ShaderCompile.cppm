export module wescene.shader_compile;
export import vrento.shader_compile;
import wescene.types;
import rstd;
import rstd.cppstd;

export namespace owe::vulkan
{
using vrento::vulkan::ShaderBackend;
using vrento::vulkan::ShaderCompOpt;
using vrento::vulkan::ShaderCompUnit;
using vrento::vulkan::ShaderReflected;
using vrento::vulkan::ShaderSpv;
using vrento::vulkan::SourceLang;
using vrento::vulkan::Uni_ShaderSpv;
using vrento::vulkan::VulkanTarget;
bool GenReflect(std::span<const std::vector<unsigned int>>, std::vector<Uni_ShaderSpv>&,
                ShaderReflected&);
bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit>, const ShaderCompOpt&,
                               std::vector<Uni_ShaderSpv>&);
bool Preprocess(std::string_view, ShaderType, SourceLang, std::string&);
rstd::boxed::Box<rstd::dyn<ShaderBackend>> MakeShaderBackend();
} // namespace owe::vulkan
