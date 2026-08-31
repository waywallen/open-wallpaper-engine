module;

export module wescene.pkg.parse:mdl_parser;
import eigen;
import wescene.core;
import rstd;
import wescene.fs;
import wescene.scene;
import wescene.pkg.scene_obj;

export import wescene.pkg.puppet;
import :shader_parser; // ShaderInfo

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe

{

// File header preceding the per-mesh body. Per hexpat: 4-byte version tag +
// u32 vertex layout flag + u32 skin_count + u32 mesh_count.
struct MdlHeader {
    rstd::int32_t  mdlv { 13 };
    rstd::uint32_t mdl_flag { 0 }; // vertex layout bitmask; mdlv<=14 meshes inherit this
    rstd::uint32_t skin_count { 1 };
    rstd::uint32_t mesh_count { 1 };
};

struct Mdl {
    MdlHeader header;

    // One element per header.mesh_count. Image puppets may use later meshes
    // for material-specific overlays such as texture-channel animation.
    struct Mesh {
        Vec<String>     mat_json_files;
        rstd::uint32_t  flag_a { 0 }; // hexpat Mesh.flag_a (usually 0; 2 has trailing 1)
        bool            has_flag_a2_one { false };
        rstd::uint32_t  flag { 0 }; // per-mesh vertex layout flag (mdlv>14); 0 = inherit header
        array<float, 3> aabb_min {};
        array<float, 3> aabb_max {};
        bool            has_aabb { false }; // mdlv>=17

        // SoA attributes; empty means the bit was not set in `flag`.
        Vec<array<float, 3>>    positions;
        Vec<array<float, 3>>    normals;
        Vec<array<float, 4>>    tangents; // tangent[3] + tangent_sign
        Vec<array<uint8_t, 4>>  extra4;
        Vec<array<uint32_t, 4>> blend_indices;
        Vec<array<float, 4>>    blend_weights;
        Vec<array<float, 2>>    texcoords;
        Vec<array<float, 2>>    texcoord2;

        Vec<array<uint32_t, 3>> indices;

        // V21+ Parts sub-block — uv2 region per vertex + part draw ranges.
        struct Part {
            uint32_t id;
            uint32_t start;
            uint32_t size;
        };
        Vec<array<float, 2>> part_uv2;
        Vec<uint32_t>        part_uv2_pad;
        Vec<Part>            parts;

        // V23+ Mask blocks attached to a single-puppet mesh.
        struct MaskBlock {
            uint32_t      leading_a;
            String        mat_json;
            Vec<uint32_t> part_ids_a;
            Vec<uint32_t> part_ids_b;
        };
        Vec<MaskBlock> masks;
    };
    Vec<Mesh> meshes;

    rstd::int32_t mdls { 1 };
    rstd::int32_t mdla { 1 };
    rstd::int32_t mdle { 0 }; // 0 = section not present
    rstd::int32_t mdmp { 0 };

    // MDMP morph sections — present when an animation drives shape blends.
    // Each section keyed by event_time matching a v4 AnimV4Event.time.
    struct MorphSectionData {
        uint32_t                shape_id;
        String                  tag;
        uint32_t                hash;
        Vec<array<uint16_t, 3>> vertices;
        Vec<uint16_t>           vertex_trailers; // shape_id != 0
        Vec<uint8_t>            trailer;         // shape_id == 0
    };
    struct MorphSection {
        float                 event_time;
        uint16_t              event_id;
        Vec<MorphSectionData> sections;
    };
    Vec<MorphSection> morph_sections;

    Option<Arc<Puppet>> puppet;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class MdlParser {
public:
    // Reads only the bytes preceding mat_json_files. Cheap; safe to call
    // over the whole corpus even on mdls that would hang full Parse.
    static bool ParseHeader(ref<str> path, fs::VFS&, MdlHeader&);

    static bool                      Parse(ref<str> path, fs::VFS&, Mdl&);
    static Option<wpscene::Material> ParseMaterial(ref<str> material_ref, fs::VFS&);
    static Option<usize>             FindMeshByMaterial(const Mdl&, ref<str> material_ref);

    static void AddPuppetShaderInfo(ShaderInfo& info, const Mdl& mdl);
    static void AddPuppetMatInfo(wpscene::Material& mat, const Mdl& mdl);

    // Emit vertex/index arrays for any Mdl::Mesh, sending only the vertex
    // attribute streams whose SoA vectors are populated. Skinning combos must
    // be wired separately via AddPuppetShaderInfo / AddPuppetMatInfo when the
    // mesh has bone weights.
    static void GenMeshFromMdl(SceneMesh::Submesh& submesh, const Mdl::Mesh& src,
                               array<float, 2> texcoord_scale  = { 1.0f, 1.0f },
                               array<float, 3> position_offset = {});

    // Like GenMeshFromMdl, but the submesh draws only the parts whose `id` is
    // in `clip_part_ids` — used for clipping-mask submeshes that only cover the
    // affected (e.g. iris) parts. Material slot is the caller's responsibility.
    static void GenMaskSubmeshFromMdl(SceneMesh::Submesh& submesh, const Mdl::Mesh& src,
                                      slice<uint32_t> clip_part_ids,
                                      array<float, 2> texcoord_scale = { 1.0f, 1.0f });
};

} // namespace owe
