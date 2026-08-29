module;

export module wescene.pkg.parse:shader_parser;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.fs;

export import :uniform;

using namespace rstd::prelude;

export namespace owe

{
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using AliasValueDict = Map<std::string, std::string>;

using DefaultTexs = std::vector<std::pair<i32, std::string>>;

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
    // is registered separately from `Material::constantshadervalues_user`.
    Vec<UserVarRecord> user_var_staging;
};

struct PreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;

    // `uniform TYPE NAME;` declarations for non-sampler types. Captured
    // per-stage so Finalprocessor can build a cross-stage union and emit
    // a single shared cbuffer (matching what glslang's iomapper used to
    // produce). Without this, DXC's per-stage $Globals cbuffers desync
    // and FS-only uniforms read as zero.
    Map<std::string, std::string> uniforms; // name -> "TYPE"

    Set<unsigned> active_tex_slots;
};

struct ShaderTexInfo {
    bool           enabled { false };
    array<bool, 4> composEnabled { false, false, false, false };
};

struct ShaderUnit {
    ShaderType       stage;
    std::string      src;
    PreprocessorInfo preprocess_info;
};

class ShaderCache {
    struct SourceEntry {
        String     source;
        ShaderInfo annotations;
        usize      bytes {};
    };

    struct CompiledStage {
        ShaderType                                 stage;
        rstd::collections::HashMap<String, String> uniforms;
        Vec<u32>                                   active_tex_slots;
    };

    struct CompileEntry {
        Vec<CompiledStage> stages;
        Vec<Vec<u32>>      codes;
        usize              bytes {};
    };

    static constexpr usize kMaxSourceBytes { 4 * 1024 * 1024 };
    static constexpr usize kMaxCompileBytes { 8 * 1024 * 1024 };
    static constexpr usize kMaxSourceEntries { 64 };
    static constexpr usize kMaxCompileEntries { 32 };

public:
    explicit ShaderCache(Option<rstd::path::PathBuf> directory = None())
        : m_directory(rstd::move(directory)) {}

    auto directory() const noexcept -> Option<ref<rstd::path::Path>> {
        if (m_directory.is_none()) return None();
        return Some(m_directory->as_path());
    }

    void ReleaseTransientEntries() {
        m_source_entries  = rstd::collections::HashMap<String, SourceEntry>::make();
        m_compile_entries = rstd::collections::HashMap<String, CompileEntry>::make();
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

    Option<rstd::path::PathBuf>                      m_directory;
    rstd::collections::HashMap<String, SourceEntry>  m_source_entries;
    rstd::collections::HashMap<String, CompileEntry> m_compile_entries;
    Vec<String>                                      m_source_order;
    Vec<String>                                      m_compile_order;
    usize                                            m_source_bytes {};
    usize                                            m_compile_bytes {};

    friend class ShaderParser;
};

// Output of CompileMaterialShader. On ok=true, spvs holds one SPIR-V
// blob per stage (currently always vertex+fragment in that order).
// On ok=false, error carries a short diagnostic.
struct CompileMaterialShaderResult {
    bool                                           ok { false };
    std::vector<ShaderCode>                        spvs;
    ShaderInfo                                     info;
    std::vector<ShaderTexInfo>                     tex_info;
    std::vector<SceneShaderUniformBlockInterface>  uniform_blocks;
    std::vector<SceneShaderDescriptorSetInterface> descriptor_sets;
    std::string                                    error;
    std::string                                    shader_name;
};

struct CompileSceneShaderVariantResult {
    bool                         ok { false };
    std::shared_ptr<SceneShader> shader;
    SceneShaderVariantDesc       variant;
    ShaderInfo                   info;
    std::vector<ShaderTexInfo>   tex_info;
    std::string                  error;
};

// Per-stage shader-annotation parser. Implementation lives in
// ShaderParser_Pegtl.cpp; declaration here so the rest of the parse
// module sees it. Not exported — internal helper.
void ParseShader(const std::string& src, ShaderInfo* info,
                 const std::vector<ShaderTexInfo>& texinfos);

class ShaderParser {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, ShaderInfo* pShaderInfo,
                                    const std::vector<ShaderTexInfo>& texs,
                                    ShaderCache*                      cache = nullptr);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static Combos ResolveShaderCombos(const ShaderInfo&, const Combos& input_combos);

    static bool CompileToSpv(std::string_view         scene_id, std::span<ShaderUnit>,
                             std::vector<ShaderCode>& spvs, ShaderInfo*,
                             std::span<const ShaderTexInfo>, ShaderCache* cache = nullptr);

    static void UpdateSceneShaderVariantDescFromCompiledUnits(SceneShaderVariantDesc&,
                                                              std::span<const ShaderUnit>,
                                                              std::span<const ShaderCode>);

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
    static CompileMaterialShaderResult CompileMaterialShader(const Json&      material_json,
                                                             fs::VFS&         vfs,
                                                             std::string_view scene_id = "test",
                                                             const Combos&    combos_override = {},
                                                             ShaderCache*     cache = nullptr);

    static CompileSceneShaderVariantResult
    CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                              const Combos& combos_override = {}, ShaderCache* cache = nullptr);
};
} // namespace owe
