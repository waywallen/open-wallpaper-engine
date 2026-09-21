module;

export module wescene.vulkan_render:shader_reflection_cache;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;
import wescene.scene;

export namespace owe::vulkan
{

struct ShaderReflectionKey {
    rstd::u32   shader_id { 0 };
    rstd::usize code_hash { 0 };

    bool operator==(const ShaderReflectionKey&) const = default;
};

} // namespace owe::vulkan

export namespace rstd
{

template<>
struct Impl<hash::Hash, owe::vulkan::ShaderReflectionKey>
    : ImplBase<owe::vulkan::ShaderReflectionKey> {
    template<typename H>
        requires Impled<H, hash::Hasher>
    void hash(H& state) const noexcept {
        hash::hash_into(this->self().shader_id, state);
        hash::hash_into(this->self().code_hash, state);
    }
};

} // namespace rstd

export namespace owe::vulkan
{

struct CachedShaderStage {
    std::string               entry_point;
    ShaderType                stage;
    std::vector<unsigned int> spirv;
};

struct CachedShaderReflection {
    std::vector<CachedShaderStage> stages;
    ShaderReflected                reflected;
};

class ShaderReflectionCache : NoCopy, NoMove {
public:
    explicit ShaderReflectionCache(rstd::ref<rstd::dyn<ShaderBackend>> backend)
        : m_backend(backend) {}
    auto Backend() const -> rstd::ref<rstd::dyn<ShaderBackend>> { return m_backend; }
    auto Query(const SceneShader&) -> rstd::Option<rstd::ref<CachedShaderReflection>>;
    void Clear();

private:
    rstd::ref<rstd::dyn<ShaderBackend>>                                     m_backend;
    rstd::collections::HashMap<ShaderReflectionKey, CachedShaderReflection> m_entries;
};

auto MakeSceneShaderRequest(const SceneShader&) -> resource::ShaderRequest;

class SceneShaderArtifactProvider {
public:
    SceneShaderArtifactProvider(ShaderReflectionCache&, const SceneShader&);

    auto Request() const -> resource::ShaderRequest;
    auto LoadShader(const resource::ShaderRequest&)
        -> rstd::Result<resource::ShaderArtifact, resource::ResourceError>;

private:
    rstd::mut_ref<ShaderReflectionCache> m_cache;
    rstd::ref<SceneShader>               m_shader;
};

std::vector<Uni_ShaderSpv> ShaderSpvsFromArtifact(const resource::ShaderArtifact&);
auto ShaderReflectionFromArtifact(const resource::ShaderArtifact&) -> ShaderReflected;

} // namespace owe::vulkan
