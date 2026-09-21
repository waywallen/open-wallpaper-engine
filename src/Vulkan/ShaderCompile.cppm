export module wescene.shader_compile;
export import vrento.shader_compile;
import wescene.types;
import rstd;

using namespace rstd::prelude;

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
bool GenReflect(slice<ShaderCode>, Vec<Uni_ShaderSpv>&, ShaderReflected&);
bool CompileAndLinkShaderUnits(slice<ShaderCompUnit>, const ShaderCompOpt&, Vec<Uni_ShaderSpv>&);
bool Preprocess(ref<str>, ShaderType, SourceLang, String&);
rstd::boxed::Box<rstd::dyn<ShaderBackend>> MakeShaderBackend();
} // namespace owe::vulkan
