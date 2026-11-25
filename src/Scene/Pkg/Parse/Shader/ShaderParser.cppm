module;

export module wescene.pkg.parse:shader_parser;
import wescene.core;
import wescene.types;
import rstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.fs;
import wescene.load_bench;

export import :uniform;

using namespace rstd::prelude;
using rstd::sync::Arc;
using namespace rstd::literals;
using rstd::collections::BTreeMap;
using rstd::collections::BTreeSet;
using rstd::collections::HashMap;
using rstd::path::Path;
using rstd::path::PathBuf;

export namespace owe

{
using Combos = BTreeMap<String, String>;

// ui material name to gl uniform name
using AliasValueDict = BTreeMap<String, String>;

using DefaultTexs = Vec<SceneShaderDefaultTexture>;

// Staged direct-route u_* uniforms (shader annotation's `material` field
// equals the wallpaper-level project.json key). BuildMaterial fills this
// during compile; the caller registers it through Scene after AddMaterial
// establishes the material's stable owner.
struct UserVarRecord {
    String material;      // project.json key (== shader annotation's material)
    String name;          // GLSL identifier (e.g. "u_Brightness")
    Json   default_value; // raw default from annotation; may be null
};

struct ShaderInfo {
    Combos         combos;
    ShaderValueMap svs;
    ShaderValueMap baseConstSvs;
    AliasValueDict alias;
    DefaultTexs    defTexs;

    // Full annotation metadata. Renderer reads `combos / svs / defTexs /
    // alias` on the hot path; the editor / material UI and the user-property
    // bridge for `u_*` uniforms read the vectors below.
    Vec<wpscene::Combo>      combo_defs;
    Vec<wpscene::UniformTex> texture_uniforms;
    Vec<wpscene::UniformVar> scalar_uniforms;
    String                   shadow_pass;

    // Filled by BuildMaterial for the direct-binding u_* route. The
    // scene-instance-level user-binding route (effect-key → wallpaper-key)
    // is registered separately from material field bindings.
    Vec<UserVarRecord> user_var_staging;
};

struct PreprocessorInfo {
    BTreeMap<String, String> input; // name to line
    BTreeMap<String, String> output;

    // `uniform TYPE NAME;` declarations for non-sampler types. Captured
    // per-stage so Finalprocessor can build a cross-stage union and emit
    // a single shared cbuffer (matching what glslang's iomapper used to
    // produce). Without this, DXC's per-stage $Globals cbuffers desync
    // and FS-only uniforms read as zero.
    BTreeMap<String, String> uniforms; // name -> "TYPE"

    BTreeSet<u32> active_tex_slots;
};

struct ShaderTexInfo {
    bool           enabled { false };
    array<bool, 4> composEnabled { false, false, false, false };
};

struct ShaderUnit {
    ShaderType       stage;
    String           src;
    PreprocessorInfo preprocess_info;
};

struct CompiledShaderStage {
    ShaderType               stage;
    BTreeMap<String, String> uniforms;
    BTreeSet<u32>            active_tex_slots;
    usize                    code_hash {};
};

struct CompiledShaderInterface {
    Vec<SceneSamplerBinding>               sampler_bindings;
    Vec<SceneShaderUniformBlockInterface>  uniform_blocks;
    Vec<SceneShaderDescriptorSetInterface> descriptor_sets;
    usize                                  descriptor_layout_hash {};
};

class CompiledShaderArtifact {
public:
    CompiledShaderArtifact(Vec<CompiledShaderStage> stages, Arc<SceneShaderReflection> reflection,
                           CompiledShaderInterface interface)
        : m_stages(rstd::move(stages)),
          m_reflection(rstd::move(reflection)),
          m_interface(rstd::move(interface)) {}

    auto StageMetadata() const -> slice<CompiledShaderStage> { return m_stages.as_slice(); }
    auto ReflectionOwner() const -> Arc<SceneShaderReflection> { return m_reflection.clone(); }
    auto Interface() const -> const CompiledShaderInterface& { return m_interface; }
    auto Codes() const -> Vec<ShaderCode> {
        auto codes = Vec<ShaderCode>::with_capacity(m_stages.len());
        for (const auto& stage : m_reflection->Stages()) codes.push(stage.spirv.clone());
        return codes;
    }
    auto RetainedBytes() const -> usize;

private:
    const Vec<CompiledShaderStage>   m_stages;
    const Arc<SceneShaderReflection> m_reflection;
    const CompiledShaderInterface    m_interface;
};

struct ShaderCompileError {
    String message;
};

class ShaderCache {
    struct SourceEntry {
        String     source;
        ShaderInfo annotations;
        usize      bytes {};
    };

    struct CompileEntry {
        Arc<CompiledShaderArtifact> artifact;
        usize                       bytes {};
    };

    static constexpr usize kMaxSourceBytes { 4 * 1024 * 1024 };
    static constexpr usize kMaxCompileBytes { 8 * 1024 * 1024 };
    static constexpr usize kMaxSourceEntries { 64 };
    static constexpr usize kMaxCompileEntries { 32 };

public:
    explicit ShaderCache(Option<PathBuf> directory = None()): m_directory(rstd::move(directory)) {}

    auto directory() const noexcept -> Option<ref<Path>> {
        if (m_directory.is_none()) return None();
        return Some(m_directory->as_path());
    }

    void ReleaseTransientEntries() {
        m_source_entries  = HashMap<String, SourceEntry>::make();
        m_compile_entries = HashMap<String, CompileEntry>::make();
        m_source_order    = Vec<String>::make();
        m_compile_order   = Vec<String>::make();
        m_source_bytes    = usize {};
        m_compile_bytes   = usize {};
    }

private:
    bool ReserveSource(usize bytes) {
        if (bytes > kMaxSourceBytes) return false;
        while (! m_source_order.is_empty() && (m_source_entries.len() >= kMaxSourceEntries ||
                                               m_source_bytes + bytes > kMaxSourceBytes)) {
            auto key     = m_source_order.remove(usize {});
            auto removed = m_source_entries.remove(key.as_str());
            if (removed.is_some()) {
                m_source_bytes -= removed->bytes;
            }
        }
        return true;
    }

    bool ReserveCompile(usize bytes) {
        if (bytes > kMaxCompileBytes) return false;
        while (! m_compile_order.is_empty() && (m_compile_entries.len() >= kMaxCompileEntries ||
                                                m_compile_bytes + bytes > kMaxCompileBytes)) {
            auto key     = m_compile_order.remove(usize {});
            auto removed = m_compile_entries.remove(key.as_str());
            if (removed.is_some()) {
                m_compile_bytes -= removed->bytes;
            }
        }
        return true;
    }

    Option<PathBuf>               m_directory;
    HashMap<String, SourceEntry>  m_source_entries;
    HashMap<String, CompileEntry> m_compile_entries;
    Vec<String>                   m_source_order;
    Vec<String>                   m_compile_order;
    usize                         m_source_bytes {};
    usize                         m_compile_bytes {};

    friend class ShaderParser;
};

// Output of CompileMaterialShader. On ok=true, spvs holds one SPIR-V
// blob per stage (currently always vertex+fragment in that order).
// On ok=false, error carries a short diagnostic.
struct CompileMaterialShaderResult {
    bool                                   ok { false };
    Vec<ShaderCode>                        spvs;
    Option<Arc<SceneShaderReflection>>     reflection;
    ShaderInfo                             info;
    Vec<ShaderTexInfo>                     tex_info;
    Vec<SceneShaderUniformBlockInterface>  uniform_blocks;
    Vec<SceneShaderDescriptorSetInterface> descriptor_sets;
    String                                 error;
    String                                 shader_name;
};

struct CompileSceneShaderVariantResult {
    bool                     ok { false };
    Option<Arc<SceneShader>> shader;
    SceneShaderVariantDesc   variant;
    ShaderInfo               info;
    Vec<ShaderTexInfo>       tex_info;
    String                   error;
};

// Per-stage shader-annotation parser. Implementation lives in
// ShaderParser_Pegtl.cpp; declaration here so the rest of the parse
// module sees it. Not exported — internal helper.
void ParseShader(ref<str> src, ShaderInfo* info, slice<ShaderTexInfo> texinfos);

class ShaderParser {
public:
    static String PreShaderSrc(fs::VFS&, ref<str> src, ShaderInfo* pShaderInfo,
                               slice<ShaderTexInfo> texs, ShaderCache* cache = nullptr,
                               SceneLoadBenchRecorderView = {});

    static String PreShaderHeader(ref<str> src, const Combos& combos, ShaderType);

    static Combos ResolveShaderCombos(const ShaderInfo&, const Combos& input_combos);
    static auto   FindCombo(const Combos&, ref<str> name) -> Option<ref<str>>;

    static auto CompileToSpv(ref<str> scene_id, mut_ref<ShaderUnit[]>, ShaderInfo*,
                             slice<ShaderTexInfo>, ShaderCache* cache = nullptr,
                             SceneLoadBenchRecorderView = {})
        -> Result<Arc<CompiledShaderArtifact>, ShaderCompileError>;

    static void ApplyCompiledShaderInterface(SceneShaderVariantDesc&, const CompiledShaderArtifact&,
                                             SceneLoadBenchRecorderView = {});

    // Lightweight entry point: compile the vert+frag shader pair for one
    // material directly, without instantiating a Scene or running the
    // full SceneParser pipeline.
    //
    // Inputs come from the material JSON (parsed via Material::FromJson)
    // plus the VFS that resolves /assets/shaders/<material.shader>.{vert,frag}
    // and #include directives. combos_override entries win over the
    // material's own combos. BLENDMODE=0 and BONECOUNT=1 are seeded if
    // absent.
    //
    // Caveat: combos that ParseImageObj derives from object-level state
    // (color-blend mode, sprite-sheet flags, puppet bone count beyond
    // default, etc.) are NOT injected. Materials that hard-require them
    // will fail compile here; supply the right values via combos_override.
    static CompileMaterialShaderResult
    CompileMaterialShader(const Json& material_json, fs::VFS& vfs, ref<str> scene_id = "test"_str,
                          const Combos& combos_override = {}, ShaderCache* cache = nullptr);

    static CompileSceneShaderVariantResult
    CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                              const Combos& combos_override = {}, ShaderCache* cache = nullptr);

private:
    static auto PrepareCompiledArtifact(slice<ShaderUnit>, slice<ShaderCode>,
                                        SceneLoadBenchRecorderView)
        -> Result<Arc<CompiledShaderArtifact>, ShaderCompileError>;
};
} // namespace owe
