module;
#include <rstd/macro.hpp>

module wescene.pkg.parse;
import wescene.pkg.spec_names;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import wescene.scene;
import wescene.pkg_asset_version;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

Puppet::PlayMode ToPlayMode(ref<str> m) {
    if (m == "loop"_str || m.is_empty()) return Puppet::PlayMode::Loop;
    if (m == "mirror"_str) return Puppet::PlayMode::Mirror;
    if (m == "single"_str) return Puppet::PlayMode::Single;

    rstd_error("unknown puppet animation play mode \"{}\"", m);
    rstd_assert(m == "loop"_str);
    return Puppet::PlayMode::Loop;
}

String ReadOwnedString(fs::BinaryReader& reader) {
    auto value = reader.ReadStr();
    return String::make(rstd::cppstd::as_str(value).unwrap());
}

template<typename T>
void ResetDefault(Vec<T>& values, usize length) {
    values.clear();
    values.reserve(length);
    for (usize i {}; i < length; ++i) values.emplace_back();
}

// Vertex layout bits (mirror imhex/mdl.hexpat MdlFlagBit).
constexpr uint32_t MDL_FLAG_NORMAL      = 0x00000002;
constexpr uint32_t MDL_FLAG_TANGENT     = 0x00000004;
constexpr uint32_t MDL_FLAG_UV          = 0x00000008;
constexpr uint32_t MDL_FLAG_UV2         = 0x00000020;
constexpr uint32_t MDL_FLAG_EXTRA4      = 0x00010000;
constexpr uint32_t MDL_FLAG_SKIN_BLEND  = 0x00800000;
constexpr uint32_t MDL_FLAG_SKIN_WEIGHT = 0x01000000;

constexpr uint32_t singile_indices_u16          = 2 * 3;
constexpr uint32_t singile_indices_u32          = 4 * 3;
constexpr uint32_t singile_bone_frame           = 4 * 9;
constexpr uint32_t mdls_offset_trans_entry_size = (3 + 16) * 4;

// Compute per-vertex byte stride from a layout flag bitset. Position is
// always emitted (12 bytes), other attributes are gated by their bits.
// UV2 implies a regular UV slot in addition to the UV2 slot.
uint32_t compute_vertex_stride(uint32_t flag) {
    uint32_t s = 12;
    if (flag & MDL_FLAG_NORMAL) s += 12;
    if (flag & MDL_FLAG_TANGENT) s += 16;
    if (flag & MDL_FLAG_EXTRA4) s += 4;
    if (flag & MDL_FLAG_SKIN_BLEND) s += 16;
    if (flag & MDL_FLAG_SKIN_WEIGHT) s += 16;
    if (flag & (MDL_FLAG_UV | MDL_FLAG_UV2)) s += 8;
    if (flag & MDL_FLAG_UV2) s += 8;
    return s;
}

// Peek the next 4 bytes; restore cursor before returning. Used to detect
// optional MDLS/MDAT/MDLA/MDMP/MDLE block headers without consuming them.
bool peek_block_magic(fs::BinaryReader& f, std::string_view expect4) {
    if (expect4.size() != 4) return false;
    auto save = f.Tell();
    if (save + 4 > f.Size()) return false;
    char buf[4] = { 0 };
    f.Read(buf, 4);
    bool ok = (std::memcmp(buf, expect4.data(), 4) == 0);
    f.SeekSet(save);
    return ok;
}

bool peek_uint8_at(fs::BinaryReader& f, rstd::ptrdiff_t off, uint8_t& out) {
    if (off < 0 || off + 1 > f.Size()) return false;
    auto save = f.Tell();
    f.SeekSet(off);
    out = f.ReadUint8();
    f.SeekSet(save);
    return true;
}

bool peek_uint32_at(fs::BinaryReader& f, rstd::ptrdiff_t off, uint32_t& out) {
    if (off < 0 || off + 4 > f.Size()) return false;
    auto save = f.Tell();
    f.SeekSet(off);
    out = f.ReadUint32();
    f.SeekSet(save);
    return true;
}

bool is_anim_trans_main_size(uint32_t byte_size, int32_t length) {
    if (length < 0 || byte_size == 0 || byte_size % 4 != 0) return false;
    auto samples = static_cast<uint64_t>(length) + 1;
    return byte_size == samples * singile_bone_frame || byte_size == samples * 4;
}

bool next_is_anim_trans_main(fs::BinaryReader& f, int32_t length) {
    uint32_t byte_size = 0;
    return peek_uint32_at(f, f.Tell(), byte_size) && is_anim_trans_main_size(byte_size, length);
}

bool next_after_zero_is_anim_trans_main(fs::BinaryReader& f, int32_t length) {
    auto     off  = f.Tell();
    uint32_t zero = 0;
    if (! peek_uint32_at(f, off, zero) || zero != 0) return false;
    uint32_t byte_size = 0;
    return peek_uint32_at(f, off + 4, byte_size) && is_anim_trans_main_size(byte_size, length);
}

bool next_is_anim_bone_curves(fs::BinaryReader& f) {
    auto    off        = f.Tell();
    uint8_t has_curves = 0;
    if (! peek_uint8_at(f, off, has_curves)) return false;
    if (! has_curves) return true;

    uint32_t zero_a = 0;
    if (! peek_uint32_at(f, off + 1, zero_a) || zero_a != 0) return false;
    uint32_t byte_size = 0;
    return peek_uint32_at(f, off + 5, byte_size) && byte_size % 4 == 0;
}

bool next_is_anim_record_padding(fs::BinaryReader& f, uint32_t end_offset) {
    auto off = f.Tell();
    if (end_offset == 0 || off + 12 > static_cast<rstd::ptrdiff_t>(end_offset)) return false;
    uint32_t zero = 0;
    if (! peek_uint32_at(f, off, zero) || zero != 0) return false;
    uint32_t next_id = 0;
    if (! peek_uint32_at(f, off + 4, next_id) || next_id == 0 || next_id > 100000) {
        return false;
    }
    uint32_t next_unk_after_id = 0;
    return peek_uint32_at(f, off + 8, next_unk_after_id) && next_unk_after_id == 0;
}

rstd::ptrdiff_t mdls_v2_indexed_trailer_start(uint32_t end_offset, uint16_t bones_num) {
    auto trailer_size = 1ull + static_cast<uint64_t>(bones_num) * mdls_offset_trans_entry_size +
                        1ull + static_cast<uint64_t>(bones_num) * 4ull;
    if (end_offset < trailer_size) return -1;
    return static_cast<rstd::ptrdiff_t>(end_offset - trailer_size);
}

bool is_mdls_v2_indexed_trailer(fs::BinaryReader& f, rstd::ptrdiff_t start, uint32_t end_offset,
                                uint16_t bones_num) {
    if (start < 0 || start >= static_cast<rstd::ptrdiff_t>(end_offset)) return false;
    uint8_t has_offset_trans = 0;
    if (! peek_uint8_at(f, start, has_offset_trans) || has_offset_trans != 1) return false;

    auto has_index_off = start + 1 +
                         static_cast<rstd::ptrdiff_t>(bones_num) *
                             static_cast<rstd::ptrdiff_t>(mdls_offset_trans_entry_size);
    if (has_index_off >= static_cast<rstd::ptrdiff_t>(end_offset)) return false;
    uint8_t has_index = 0;
    return peek_uint8_at(f, has_index_off, has_index) && has_index == 1;
}

void ParseMasks(fs::BinaryReader& f, Mdl::Mesh& mesh);

bool UsesUint32Indices(const MdlHeader& header, uint32_t vertex_num) {
    return header.mdlv >= 23 && vertex_num > std::numeric_limits<uint16_t>::max();
}

// hexpat Mesh<MdlV, TopFlag, SinglePuppet, SkinCount>:
//   CStr mat_json[SkinCount] + u32 flag_a + (if flag_a==2: u32) + (if MdlV>=17: aabb)
//   + (if MdlV>14: u32 mesh_flag) + u32 vertex_size + Vertex[]
//   + u32 indices_size + Triangle[] + (if MdlV>=21: Parts) + (if MdlV>21: Masks)
bool ParseMesh(fs::BinaryReader& f, const MdlHeader& header, Mdl::Mesh& mesh,
               std::string_view path) {
    ResetDefault(mesh.mat_json_files, usize(header.skin_count));
    for (auto& material : mesh.mat_json_files) material = ReadOwnedString(f);
    mesh.flag_a = f.ReadUint32();
    if (mesh.flag_a == 2) {
        mesh.has_flag_a2_one = (f.ReadUint32() == 1);
    }

    if (header.mdlv >= 17) {
        for (auto& v : mesh.aabb_min) v = f.ReadFloat();
        for (auto& v : mesh.aabb_max) v = f.ReadFloat();
        mesh.has_aabb = true;
    }

    uint32_t mesh_flag = (header.mdlv > 14) ? f.ReadUint32() : header.mdl_flag;
    mesh.flag          = mesh_flag;

    uint32_t vertex_size = f.ReadUint32();
    uint32_t stride      = compute_vertex_stride(mesh_flag);
    if (stride == 0 || vertex_size % stride != 0) {
        rstd_error("unsupport mdl vertex size {} (flag=0x{:X} stride={}) in {}",
                   vertex_size,
                   mesh_flag,
                   stride,
                   std::string(path));
        return false;
    }

    uint32_t vertex_num = vertex_size / stride;
    ResetDefault(mesh.positions, usize(vertex_num));
    if (mesh_flag & MDL_FLAG_NORMAL) ResetDefault(mesh.normals, usize(vertex_num));
    if (mesh_flag & MDL_FLAG_TANGENT) ResetDefault(mesh.tangents, usize(vertex_num));
    if (mesh_flag & MDL_FLAG_EXTRA4) ResetDefault(mesh.extra4, usize(vertex_num));
    if (mesh_flag & MDL_FLAG_SKIN_BLEND) ResetDefault(mesh.blend_indices, usize(vertex_num));
    if (mesh_flag & MDL_FLAG_SKIN_WEIGHT) ResetDefault(mesh.blend_weights, usize(vertex_num));
    if (mesh_flag & (MDL_FLAG_UV | MDL_FLAG_UV2)) {
        ResetDefault(mesh.texcoords, usize(vertex_num));
    }
    if (mesh_flag & MDL_FLAG_UV2) ResetDefault(mesh.texcoord2, usize(vertex_num));

    for (uint32_t i = 0; i < vertex_num; ++i) {
        const usize index(i);
        for (auto& v : mesh.positions[index]) v = f.ReadFloat();
        if (mesh_flag & MDL_FLAG_NORMAL) {
            for (auto& v : mesh.normals[index]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_TANGENT) {
            for (auto& v : mesh.tangents[index]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_EXTRA4) {
            for (auto& v : mesh.extra4[index]) v = f.ReadUint8();
        }
        if (mesh_flag & MDL_FLAG_SKIN_BLEND) {
            for (auto& v : mesh.blend_indices[index]) v = f.ReadUint32();
        }
        if (mesh_flag & MDL_FLAG_SKIN_WEIGHT) {
            for (auto& v : mesh.blend_weights[index]) v = f.ReadFloat();
        }
        if (mesh_flag & (MDL_FLAG_UV | MDL_FLAG_UV2)) {
            for (auto& v : mesh.texcoords[index]) v = f.ReadFloat();
        }
        if (mesh_flag & MDL_FLAG_UV2) {
            for (auto& v : mesh.texcoord2[index]) v = f.ReadFloat();
        }
    }

    uint32_t       indices_size    = f.ReadUint32();
    const bool     use_u32_indices = UsesUint32Indices(header, vertex_num);
    const uint32_t index_stride    = use_u32_indices ? singile_indices_u32 : singile_indices_u16;
    if (indices_size % index_stride != 0) {
        rstd_error("unsupport mdl indices size {} (stride={}) in {}",
                   indices_size,
                   index_stride,
                   std::string(path));
        return false;
    }
    uint32_t indices_num = indices_size / index_stride;
    ResetDefault(mesh.indices, usize(indices_num));
    for (auto& id : mesh.indices) {
        for (auto& v : id) v = use_u32_indices ? f.ReadUint32() : f.ReadUint16();
    }

    // V21+ Parts sub-block (hexpat Parts<MdlV>): optional uv2 region followed
    // by an optional part draw-range list.
    if (header.mdlv >= 21) {
        uint8_t unk_a = f.ReadUint8();
        if (unk_a == 1) {
            uint8_t unk_b = f.ReadUint8();
            if (unk_b) {
                uint16_t unk_c = f.ReadUint16();
                if (unk_c != 0) {
                    rstd_info("mdlv{} parts unk_c expected 0, got {}", header.mdlv, unk_c);
                }
                (void)f.ReadUint8(); // vert_section_marker
                uint32_t payload_size = f.ReadUint32();
                if (payload_size != 12u * vertex_num) {
                    rstd_error("mdlv{} extras payload size {} != 12*{}",
                               header.mdlv,
                               payload_size,
                               vertex_num);
                    return false;
                }
                ResetDefault(mesh.part_uv2, usize(vertex_num));
                ResetDefault(mesh.part_uv2_pad, usize(vertex_num));
                for (uint32_t i = 0; i < vertex_num; ++i) {
                    const usize index(i);
                    mesh.part_uv2[index][usize(0)] = f.ReadFloat();
                    mesh.part_uv2[index][usize(1)] = f.ReadFloat();
                    mesh.part_uv2_pad[index]       = f.ReadUint32();
                }
            }
        } else if (unk_a != 0) {
            rstd_error("mdlv{} parts unhandled unk_a={}", header.mdlv, unk_a);
            return false;
        }
        uint8_t has_parts = f.ReadUint8();
        if (has_parts) {
            uint32_t parts_bytes = f.ReadUint32();
            if (parts_bytes % 16 != 0) {
                rstd_error("mdlv{} parts byte count {} not %% 16", header.mdlv, parts_bytes);
                return false;
            }
            uint32_t parts_num = parts_bytes / 16;
            ResetDefault(mesh.parts, usize(parts_num));
            for (auto& part : mesh.parts) {
                part.id = f.ReadUint32();
                (void)f.ReadUint32(); // reserved 0
                part.start = f.ReadUint32();
                part.size  = f.ReadUint32();
            }
        }
        if (header.mdlv > 21) {
            ParseMasks(f, mesh);
        }
    }
    return true;
}

bool ParseIkConfig(fs::BinaryReader& f, Puppet::IkConfig& ik) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) ik.chain_a_target(r, c) = f.ReadFloat();
    ik.ik_version          = f.ReadUint8();
    ik.ik_header[usize(0)] = f.ReadUint32();
    ik.ik_header[usize(1)] = f.ReadUint32();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) ik.chain_b_target(r, c) = f.ReadFloat();
    for (auto& b : ik.ik_flags) b = f.ReadUint8();
    for (auto& v : ik.pole_targets) {
        for (int k = 0; k < 3; ++k) v[k] = f.ReadFloat();
    }
    uint16_t rest_count = f.ReadUint16();
    ResetDefault(ik.rest_rotations, usize(rest_count));
    for (auto& br : ik.rest_rotations) {
        br.bone_id = f.ReadUint32();
        for (auto& v : br.dir) v = f.ReadFloat();
    }
    auto read_chain_bone_dir = [&](Puppet::ChainBoneDir& d) {
        d.chain_id = f.ReadUint16();
        d.bone_id  = f.ReadUint32();
        for (auto& v : d.dir) v = f.ReadFloat();
    };
    auto read_bone_dir = [&](Puppet::BoneDir& d) {
        d.bone_id = f.ReadUint32();
        for (auto& v : d.dir) v = f.ReadFloat();
    };
    ResetDefault(ik.ik_targets, usize(6));
    read_chain_bone_dir(ik.ik_targets[usize(0)]);
    (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[usize(1)]);
    for (int i = 0; i < 4; ++i) (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[usize(2)]);
    for (int i = 0; i < 3; ++i) (void)f.ReadUint16();
    read_chain_bone_dir(ik.ik_targets[usize(3)]);
    auto& root = ik.ik_target_root.insert(Puppet::BoneDir {});
    read_bone_dir(root);
    read_chain_bone_dir(ik.ik_targets[usize(4)]);
    read_chain_bone_dir(ik.ik_targets[usize(5)]);
    for (int i = 0; i < 3; ++i) (void)f.ReadUint16();
    ik.ik_constraint.cnt   = f.ReadUint16();
    ik.ik_constraint.id    = f.ReadUint32();
    ik.ik_constraint.child = f.ReadUint32();
    ik.ik_constraint.val   = f.ReadUint32();
    for (auto& lst : ik.ik_bone_lists) {
        uint16_t cnt = f.ReadUint16();
        ResetDefault(lst, usize(cnt));
        for (auto& v : lst) v = f.ReadUint32();
    }
    ik.ik_chain_count            = f.ReadUint32();
    ik.ik_chain_length[usize(0)] = f.ReadFloat();
    ik.ik_chain_length[usize(1)] = f.ReadFloat();
    uint16_t chain_bones_cnt     = f.ReadUint16();
    ResetDefault(ik.ik_chain_bones, usize(chain_bones_cnt));
    for (auto& v : ik.ik_chain_bones) v = f.ReadUint32();
    return true;
}

bool ParseMDLS(fs::BinaryReader& f, Mdl& mdl, std::string_view path) {
    mdl.mdls = ReadMdlVersion(f);

    uint32_t end_offset = f.ReadUint32();

    uint16_t bones_num = f.ReadUint16();
    f.ReadUint16(); // zero pad

    mdl.puppet  = Some(Arc<Puppet>::make());
    auto& bones = (*mdl.puppet)->bones;

    ResetDefault(bones, usize(bones_num));
    for (unsigned i = 0; i < bones_num; ++i) {
        auto& bone    = bones[usize(i)];
        bone.name     = ReadOwnedString(f);
        bone.sim_type = f.ReadInt32();

        uint32_t file_parent = f.ReadUint32();
        if (file_parent >= i && file_parent != Puppet::NO_PARENT) {
            rstd_info("mdl bone[{}] forward parent {} in {}; treating as root",
                      i,
                      file_parent,
                      std::string(path));
            file_parent = Puppet::NO_PARENT;
        }
        bone.bind_parent = file_parent;
        bone.anim_parent = file_parent;
        bone.file_parent = file_parent;

        uint32_t size = f.ReadUint32();
        if (size != 64) {
            rstd_error("mdl unsupport bones size: {}", size);
            return false;
        }
        for (auto row : bone.local_bind.matrix().colwise()) {
            for (auto& x : row) x = f.ReadFloat();
        }
        bone.simulation_json = ReadOwnedString(f);
    }

    if (mdl.mdls > 1) {
        uint16_t extras_flag = f.ReadUint16();

        if (mdl.mdls == 2) {
            uint8_t has_world_binds = f.ReadUint8();
            if (has_world_binds) {
                // Per-bone world-bind mat4 inline (mdls v2 only).
                for (unsigned i = 0; i < bones_num; ++i)
                    for (unsigned j = 0; j < 16; ++j) f.ReadFloat();
            }
            uint8_t pad[8];
            f.Read(pad, sizeof(pad));
            if (extras_flag == 5) {
                auto trailer_start = mdls_v2_indexed_trailer_start(end_offset, bones_num);
                if (trailer_start >= f.Tell() &&
                    is_mdls_v2_indexed_trailer(f, trailer_start, end_offset, bones_num)) {
                    f.SeekSet(trailer_start);
                } else {
                    rstd_info("MDLSv2 extras_flag 5 did not match indexed trailer in {}",
                              std::string(path));
                }
            } else if (extras_flag != 0) {
                rstd_info("MDLSv2 unexpected extras_flag {}", extras_flag);
            }
        } else {
            uint8_t zero_b = f.ReadUint8();
            if (zero_b != 0) {
                rstd_info("MDLSv{} zero_b expected 0, got {}", mdl.mdls, zero_b);
            }
            uint32_t pair0 = f.ReadUint32();
            uint32_t pair1 = f.ReadUint32();
            (void)pair0;
            (void)pair1;

            // extras_flag==2 means an IK config block follows. The hexpat
            // schema is verified against only one corpus sample and breaks
            // for other puppets (3669680904's rw_puppet reads 992K of garbage
            // before tripping a downstream bone curve assert). The trailer
            // (has_offset_trans / has_index / has_depth) sits past the IK
            // block but isn't consumed render-side, so when IK is present we
            // skip the whole MDLS body and let the end_offset rescue at the
            // bottom of this function position the cursor for MDAT/MDLA.
            if (extras_flag == 2) {
                f.SeekSet(end_offset);
            } else if (extras_flag != 0) {
                rstd_info("MDLSv{} unexpected extras_flag {}", mdl.mdls, extras_flag);
            }
        }

        // Parse the per-bone metadata trailer only when no IK block forced
        // the cursor to end_offset above.
        if (static_cast<uint32_t>(f.Tell()) < end_offset) {
            uint8_t has_offset_trans = f.ReadUint8();
            if (has_offset_trans) {
                for (unsigned i = 0; i < bones_num; ++i) {
                    auto& b               = (*mdl.puppet)->bones[usize(i)];
                    b.has_file_skin_pivot = true;
                    b.file_skin_pivot.x() = f.ReadFloat();
                    b.file_skin_pivot.y() = f.ReadFloat();
                    b.file_skin_pivot.z() = f.ReadFloat();
                    for (auto col : b.file_skin_mat.colwise()) {
                        for (auto& v : col) v = f.ReadFloat();
                    }
                }
            }

            uint8_t has_index = f.ReadUint8();
            if (has_index) {
                for (unsigned i = 0; i < bones_num; ++i) f.ReadUint32();
            }

            if (mdl.mdls >= 3) {
                uint8_t has_depth = f.ReadUint8();
                if (has_depth) {
                    for (unsigned i = 0; i < bones_num; ++i) (void)f.ReadUint32();
                }
            }
        }
    }

    // Honour the block's declared end so partial IK / unknown trailer can't
    // poison subsequent MDxx scans.
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDLS body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()),
                  end_offset,
                  std::string(path));
        f.SeekSet(end_offset);
    }
    return true;
}

void ParseMDAT(fs::BinaryReader& f, Mdl& mdl) {
    uint32_t end_offset      = f.ReadUint32();
    uint32_t num_attachments = f.ReadUint16();
    auto&    attachments     = (*mdl.puppet)->attachments;
    ResetDefault(attachments, usize(num_attachments));
    for (auto& att : attachments) {
        att.bone_index = f.ReadUint16();
        att.name       = ReadOwnedString(f);
        // 64-byte payload = column-major 4x4 affine in the anchored bone's
        // local space (linear 3x3 in cols 0-2, translation in col 3).
        att.local_xform = Eigen::Affine3f::Identity();
        for (auto col : att.local_xform.matrix().colwise()) {
            for (auto& v : col) v = f.ReadFloat();
        }
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        f.SeekSet(end_offset);
    }
}

// hexpat AnimBoneCurves: u8 has_curves; if(has_curves) BoneFrameCurve[bone_count].
// Each BoneFrameCurve = u32 zero + u32 byte_size + float[byte_size/4].
bool ParseAnimBoneCurves(fs::BinaryReader& f, Vec<Puppet::BoneFrameCurve>& out,
                         uint32_t bone_count) {
    uint8_t has_curves = f.ReadUint8();
    if (! has_curves) return true;
    ResetDefault(out, usize(bone_count));
    for (auto& curve : out) {
        uint32_t zero_a = f.ReadUint32();
        if (zero_a != 0) {
            rstd_info("BoneFrameCurve zero_a expected 0, got {}", zero_a);
        }
        uint32_t byte_size = f.ReadUint32();
        if (byte_size % 4 != 0) {
            rstd_error("BoneFrameCurve byte_size {} not %% 4", byte_size);
            return false;
        }
        ResetDefault(curve.values, usize(byte_size / 4));
        for (auto& v : curve.values) v = f.ReadFloat();
    }
    return true;
}

bool ParseAnimTransMainTrack(fs::BinaryReader& f, Vec<float>& out, int32_t length,
                             std::string_view path) {
    uint32_t byte_size = f.ReadUint32();
    if (! is_anim_trans_main_size(byte_size, length)) {
        rstd_error("AnimTransMain byte_size {} does not match animation length {} in {}",
                   byte_size,
                   length,
                   std::string(path));
        return false;
    }
    ResetDefault(out, usize(byte_size / 4));
    for (auto& v : out) v = f.ReadFloat();
    return true;
}

bool ParseAnimation(fs::BinaryReader& f, Puppet::Animation& anim, int mdla_ver,
                    uint32_t mdla_end_offset, std::string_view path) {
    anim.id           = f.ReadInt32();
    anim.unk_after_id = f.ReadUint32();

    anim.name = ReadOwnedString(f);
    if (anim.name.is_empty()) anim.name = ReadOwnedString(f);

    auto play_mode = f.ReadStr();
    anim.mode      = ToPlayMode(rstd::cppstd::as_str(play_mode).unwrap());
    anim.fps       = f.ReadFloat();
    anim.length    = f.ReadInt32();
    f.ReadInt32(); // anim_zero

    uint32_t b_num = f.ReadUint32();
    ResetDefault(anim.bone_tracks, usize(b_num));
    for (uint32_t ti = 0; ti < b_num; ++ti) {
        auto& track        = anim.bone_tracks[usize(ti)];
        track.bone_index   = ti; // dense: slot i animates bone i
        track.unk          = f.ReadInt32();
        uint32_t byte_size = f.ReadUint32();
        if (byte_size % singile_bone_frame != 0) {
            rstd_error("wrong bone frame size {} in {}", byte_size, std::string(path));
            return false;
        }
        uint32_t num = byte_size / singile_bone_frame;
        ResetDefault(track.frames, usize(num));
        for (auto& frame : track.frames) {
            for (auto& v : frame.position) v = f.ReadFloat();
            for (auto& v : frame.angle) v = f.ReadFloat();
            for (auto& v : frame.scale) v = f.ReadFloat();
        }
    }

    if (mdla_ver >= 3) {
        uint32_t trans_flag = f.ReadUint32();
        if (trans_flag == 1) {
            auto&    tr         = anim.trans.insert(Puppet::AnimTrans {});
            uint32_t extra_size = f.ReadUint32();
            if (extra_size > 0) {
                if (extra_size % 4 != 0) {
                    rstd_error("UnkAnimTrans extra_size {} not %% 4", extra_size);
                    return false;
                }
                ResetDefault(tr.extra_track, usize(extra_size / 4));
                for (auto& v : tr.extra_track) v = f.ReadFloat();
                uint32_t extra_zero = f.ReadUint32();
                if (extra_zero != 0) {
                    rstd_info("UnkAnimTrans extra_zero expected 0, got {}", extra_zero);
                }
            }
            uint32_t main_size = f.ReadUint32();
            if (main_size % 4 != 0) {
                rstd_error("UnkAnimTrans main_size {} not %% 4", main_size);
                return false;
            }
            ResetDefault(tr.main_track, usize(main_size / 4));
            for (auto& v : tr.main_track) v = f.ReadFloat();
            if (extra_size > 0) {
                uint32_t trail_zero = f.ReadUint32();
                if (trail_zero != 0) {
                    rstd_info("UnkAnimTrans trail_zero expected 0, got {}", trail_zero);
                }
            }
        } else if (trans_flag == 0) {
            if (next_is_anim_trans_main(f, anim.length)) {
                auto& tr = anim.trans.insert(Puppet::AnimTrans {});
                if (! ParseAnimTransMainTrack(f, tr.main_track, anim.length, path)) return false;
                while (next_after_zero_is_anim_trans_main(f, anim.length)) {
                    uint32_t trail_zero = f.ReadUint32();
                    if (trail_zero != 0) {
                        rstd_info("AnimTransMain trail_zero expected 0, got {}", trail_zero);
                    }
                    auto& tail_track = tr.tail_tracks.emplace_back();
                    if (! ParseAnimTransMainTrack(f, tail_track, anim.length, path)) return false;
                }
            }
        } else {
            rstd_error("Animation {} trans_flag expected 0/1, got {} in {}",
                       anim.name,
                       trans_flag,
                       std::string(path));
            return false;
        }
        if (! ParseAnimBoneCurves(f, anim.blend_curves, b_num)) return false;
    }

    if (mdla_ver >= 4) {
        uint8_t has_v4_events = f.ReadUint8();
        if (has_v4_events == 1) {
            uint32_t v4_count = f.ReadUint32();
            ResetDefault(anim.v4_events, usize(v4_count));
            for (auto& ev : anim.v4_events) {
                ev.time              = f.ReadFloat();
                uint16_t curve_count = f.ReadUint16();
                ev.flags             = f.ReadUint16();
                if (curve_count == 0) {
                    rstd_error("AnimV4Event curve_count is zero in {}", std::string(path));
                    return false;
                }
                ResetDefault(ev.curves, usize(curve_count));
                for (uint16_t curve_index = 0; curve_index < curve_count; ++curve_index) {
                    auto& curve = ev.curves[usize(curve_index)];
                    curve.id    = curve_index == 0 ? 0 : f.ReadUint16();
                    uint32_t bs = f.ReadUint32();
                    if (bs % 4 != 0) {
                        rstd_error("AnimV4Curve byte_size {} not %% 4", bs);
                        return false;
                    }
                    ResetDefault(curve.values, usize(bs / 4));
                    for (auto& v : curve.values) v = f.ReadFloat();
                }
            }
        } else if (has_v4_events != 0) {
            rstd_info("Animation has_v4_events expected 0/1, got {}", has_v4_events);
        }
    }

    if (mdla_ver >= 5) {
        for (auto& v : anim.aabb_min) v = f.ReadFloat();
        for (auto& v : anim.aabb_max) v = f.ReadFloat();
        anim.has_aabb = true;
    }

    if (mdla_ver == 6) {
        if (next_is_anim_bone_curves(f)) {
            if (! ParseAnimBoneCurves(f, anim.scalar_curves, b_num)) return false;
        }
    }

    // Trailing event list — present on every animation regardless of mdla
    // version. Pre-mdla>=3 anims start here directly.
    uint32_t event_count = f.ReadUint32();
    ResetDefault(anim.events, usize(event_count));
    for (auto& ev : anim.events) {
        ev.time_value = f.ReadUint32();
        ev.event_json = ReadOwnedString(f);
    }
    if (next_is_anim_record_padding(f, mdla_end_offset)) {
        uint32_t record_padding_zero = f.ReadUint32();
        if (record_padding_zero != 0) {
            rstd_info("Animation {} record_padding_zero expected 0, got {}",
                      anim.name,
                      record_padding_zero);
        }
    }
    return true;
}

bool ParseMDLA(fs::BinaryReader& f, Mdl& mdl, std::string_view tag, std::string_view path) {
    mdl.mdla = std::stoi(std::string(tag.substr(4, 4)));
    if (mdl.mdla == 0) return true;

    uint32_t end_offset = f.ReadUint32();

    uint32_t anim_num = f.ReadUint32();
    auto&    anims    = (*mdl.puppet)->anims;
    ResetDefault(anims, usize(anim_num));
    bool ok = true;
    for (auto& anim : anims) {
        if (! ParseAnimation(f, anim, mdl.mdla, end_offset, path)) {
            ok = false;
            break;
        }
    }

    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) + 4 == end_offset) {
        uint32_t final_padding_zero = f.ReadUint32();
        if (final_padding_zero != 0) {
            rstd_info("MDLA final_padding_zero expected 0, got {} ({})",
                      final_padding_zero,
                      std::string(path));
        }
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDLA body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()),
                  end_offset,
                  std::string(path));
        f.SeekSet(end_offset);
    }
    return ok;
}

void ParseMasks(fs::BinaryReader& f, Mdl::Mesh& mesh) {
    uint32_t mask_count = f.ReadUint32();
    ResetDefault(mesh.masks, usize(mask_count));
    for (auto& m : mesh.masks) {
        m.leading_a     = f.ReadUint32();
        uint32_t zero_a = f.ReadUint32();
        if (zero_a != 0) rstd_info("MaskBlock zero_a expected 0, got {}", zero_a);
        m.mat_json        = ReadOwnedString(f);
        uint32_t zero_pad = f.ReadUint32();
        if (zero_pad != 0) rstd_info("MaskBlock zero_pad expected 0, got {}", zero_pad);
        uint32_t a_count = f.ReadUint32();
        ResetDefault(m.part_ids_a, usize(a_count));
        for (auto& v : m.part_ids_a) v = f.ReadUint32();
        uint32_t b_count = f.ReadUint32();
        ResetDefault(m.part_ids_b, usize(b_count));
        for (auto& v : m.part_ids_b) v = f.ReadUint32();
    }
}

bool ParseMDMP(fs::BinaryReader& f, Mdl& mdl, std::string_view tag, std::string_view path) {
    mdl.mdmp            = std::stoi(std::string(tag.substr(4, 4)));
    uint32_t end_offset = f.ReadUint32();
    while (f.Tell() < end_offset) {
        auto&    sec    = mdl.morph_sections.emplace_back();
        uint16_t count  = f.ReadUint16();
        sec.event_time  = f.ReadFloat();
        sec.event_id    = f.ReadUint16();
        uint16_t zero_a = f.ReadUint16();
        if (zero_a != 0) {
            rstd_info("MDMPSection zero_a expected 0, got {}", zero_a);
        }
        ResetDefault(sec.sections, usize(count));
        for (auto& sd : sec.sections) {
            sd.shape_id      = f.ReadUint32();
            uint32_t sd_zero = f.ReadUint32();
            if (sd_zero != 0) {
                rstd_info("MDMPSectionData zero_a expected 0, got {}", sd_zero);
            }
            sd.tag          = ReadOwnedString(f);
            uint32_t length = f.ReadUint32();
            sd.hash         = f.ReadUint32();
            if (length % 6 != 0) {
                rstd_error("MDMPSectionData length {} not %% 6", length);
                return false;
            }
            uint32_t vcount = length / 6;
            ResetDefault(sd.vertices, usize(vcount));
            for (auto& v : sd.vertices) {
                for (auto& x : v) x = f.ReadUint16();
            }
            if (sd.shape_id == 0) {
                ResetDefault(sd.trailer, usize(length));
                for (auto& b : sd.trailer) b = f.ReadUint8();
            } else {
                ResetDefault(sd.vertex_trailers, usize(vcount));
                for (auto& v : sd.vertex_trailers) v = f.ReadUint16();
            }
        }
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        rstd_info("MDMP body ended at 0x{:X} but end_offset=0x{:X} ({})",
                  static_cast<uint32_t>(f.Tell()),
                  end_offset,
                  std::string(path));
        f.SeekSet(end_offset);
    }
    return true;
}

bool ParseMDLE(fs::BinaryReader& f, Mdl& mdl, std::string_view tag) {
    mdl.mdle                   = std::stoi(std::string(tag.substr(4, 4)));
    uint32_t     end_offset    = f.ReadUint32();
    uint32_t     payload_bytes = f.ReadUint32();
    const size_t nbones        = (*mdl.puppet)->bones.len().to_primitive();
    const size_t expected      = nbones * 64;
    if (payload_bytes != expected) {
        rstd_error("MDLE payload_bytes {} != bones_num*64 {}", payload_bytes, expected);
        return false;
    }
    for (auto& bone : (*mdl.puppet)->bones) {
        bone.file_world_bind = Eigen::Affine3f::Identity();
        for (auto col : bone.file_world_bind.matrix().colwise()) {
            for (auto& v : col) v = f.ReadFloat();
        }
        bone.has_file_world_bind = true;
    }
    if (end_offset > 0 && static_cast<uint32_t>(f.Tell()) != end_offset) {
        f.SeekSet(end_offset);
    }
    return true;
}

// MDLS v3+ vertex-centroid offsets per bone. MDLV21 puppets need the bind
// chain flattened and the centroid_offset bracketed around scale/rotation
// in genFrame (bones are world-anchored, sprite lives at bind.t + vco), while
// animation still follows the file parent chain via parent skin deltas.
// MDLV22+ keeps file_parent intact for chain LBS; `vertex_centroid_offset`
// is still computed but not consumed by the chain path.
void ApplyMDLS3CentroidPivot(Mdl& mdl) {
    if (mdl.meshes.is_empty()) return;
    if ((*mdl.puppet)->world_anchored_bones) {
        for (auto& b : (*mdl.puppet)->bones) {
            b.bind_parent = Puppet::NO_PARENT;
            b.anim_parent = b.file_parent;
        }
    }
    const size_t                 nbones = (*mdl.puppet)->bones.len().to_primitive();
    std::vector<Eigen::Vector3d> sum_pos(nbones, Eigen::Vector3d::Zero());
    std::vector<double>          sum_w(nbones, 0.0);
    auto                         v_to_e = [](const array<float, 3>& p) {
        return Eigen::Vector3d { p[usize(0)], p[usize(1)], p[usize(2)] };
    };

    // Multi-mesh puppets (mesh_count > 1) may distribute skin data across
    // sub-meshes; accumulate centroid contributions from every mesh that has
    // bone indices. Meshes that only carry SKIN_BLEND (no SKIN_WEIGHT) follow
    // the WE 1-bone rigid convention: implicit weight 1.0 on slot 0.
    auto contribute = [&](const Mdl::Mesh& m) {
        if (m.blend_indices.is_empty()) return;
        const bool has_w  = ! m.blend_weights.is_empty();
        auto       weight = [&](size_t vi, int k) -> float {
            if (! has_w) return k == 0 ? 1.0f : 0.0f;
            return m.blend_weights[usize(vi)][usize(static_cast<size_t>(k))];
        };
        if (! m.indices.is_empty()) {
            for (const auto& tri : m.indices) {
                if (tri[usize(0)] >= m.positions.len().to_primitive() ||
                    tri[usize(1)] >= m.positions.len().to_primitive() ||
                    tri[usize(2)] >= m.positions.len().to_primitive())
                    continue;
                Eigen::Vector3d p0           = v_to_e(m.positions[usize(tri[usize(0)])]);
                Eigen::Vector3d p1           = v_to_e(m.positions[usize(tri[usize(1)])]);
                Eigen::Vector3d p2           = v_to_e(m.positions[usize(tri[usize(2)])]);
                Eigen::Vector3d centroid_tri = (p0 + p1 + p2) / 3.0;
                double          area         = 0.5 * (p1 - p0).cross(p2 - p0).norm();
                if (area <= 0.0) continue;
                const int slots = has_w ? 4 : 1;
                for (int corner = 0; corner < 3; ++corner) {
                    uint32_t vi = tri[usize(static_cast<size_t>(corner))];
                    for (int slot = 0; slot < slots; ++slot) {
                        float    w  = weight(vi, slot);
                        uint32_t bi = m.blend_indices[usize(vi)][usize(static_cast<size_t>(slot))];
                        if (w > 0.0f && bi < nbones) {
                            double tri_w = (area / 3.0) * static_cast<double>(w);
                            sum_pos[bi] += centroid_tri * tri_w;
                            sum_w[bi] += tri_w;
                        }
                    }
                }
            }
        } else {
            const int slots = has_w ? 4 : 1;
            for (size_t vi = 0; vi < m.positions.len().to_primitive(); ++vi) {
                Eigen::Vector3d p = v_to_e(m.positions[usize(vi)]);
                for (int k = 0; k < slots; ++k) {
                    float    w  = weight(vi, k);
                    uint32_t bi = m.blend_indices[usize(vi)][usize(static_cast<size_t>(k))];
                    if (w > 0.0f && bi < nbones) {
                        sum_pos[bi] += p * (double)w;
                        sum_w[bi] += (double)w;
                    }
                }
            }
        }
    };
    for (const auto& m : mdl.meshes) contribute(m);

    for (size_t i = 0; i < nbones; ++i) {
        if (sum_w[i] > 0.0) {
            Eigen::Vector3f centroid = (sum_pos[i] / sum_w[i]).cast<float>();
            (*mdl.puppet)->bones[usize(i)].vertex_centroid_offset =
                centroid - (*mdl.puppet)->bones[usize(i)].local_bind.translation();
        }
    }
}

// hexpat Header: VersionTag mdlv + u32 mdl_flag + u32 skin_count + u32 mesh_count.
bool ReadHeaderFromStream(fs::BinaryReader& f, MdlHeader& h, std::string_view path_for_log) {
    h.mdlv       = ReadMdlVersion(f);
    h.mdl_flag   = f.ReadUint32();
    h.skin_count = f.ReadUint32();
    h.mesh_count = f.ReadUint32();
    if (h.skin_count == 0) {
        rstd_error("mdl '{}' header has no material skins", std::string(path_for_log));
        return false;
    }
    return true;
}

std::string ResolveMdlMaterialPath(std::string_view ref) {
    std::string path(ref);
    if (! path.ends_with(".json")) path += ".json";
    if (path.starts_with("materials/")) return "/assets/" + path;
    return "/assets/materials/" + path;
}

} // namespace

bool MdlParser::ParseHeader(ref<str> path, fs::VFS& vfs, MdlHeader& h) {
    auto path_view = rstd::cppstd::as_string_view(path);
    auto pfile     = fs::OpenBinary(vfs, "/assets/" + std::string(path_view));
    if (pfile.is_err()) return false;
    auto f = rstd::move(pfile).unwrap_unchecked();
    return ReadHeaderFromStream(f, h, path_view);
}

bool MdlParser::Parse(ref<str> path, fs::VFS& vfs, Mdl& mdl) {
    auto str_path = std::string(rstd::cppstd::as_string_view(path));
    auto pfile    = fs::OpenBinary(vfs, "/assets/" + str_path);
    if (pfile.is_err()) return false;
    auto f = rstd::move(pfile).unwrap_unchecked();

    if (! ReadHeaderFromStream(f, mdl.header, str_path)) return false;

    ResetDefault(mdl.meshes, usize(mdl.header.mesh_count));
    for (auto& m : mdl.meshes) {
        if (! ParseMesh(f, mdl.header, m, str_path)) return false;
    }

    // Consume the 9-byte VersionTag for blocks whose body parser expects to
    // start at `end_offset`. MDLS reads its tag internally via ReadMdlVersion.
    auto consume_tag = [&]() -> std::string {
        char buf[9] { 0 };
        f.Read(buf, 9);
        return std::string(buf, 8);
    };

    if (peek_block_magic(f, "MDLS")) {
        if (! ParseMDLS(f, mdl, str_path)) return false;
    }
    if (peek_block_magic(f, "MDAT")) {
        (void)consume_tag();
        ParseMDAT(f, mdl);
    }
    if (peek_block_magic(f, "MDLA")) {
        std::string tag = consume_tag();
        // MDLA body's verified schema doesn't cover every puppet (rw_puppet
        // in 3669680904 trips a garbage BoneFrameCurve byte_size). Treat a
        // failure as fatal-to-animation only: clear any partially populated
        // anims so the puppet stays at bind pose, then jump to MDLA end via
        // the rescue inside ParseMDLA. Bones + mesh are still usable.
        if (! ParseMDLA(f, mdl, tag, str_path)) {
            if (mdl.puppet.is_some()) (*mdl.puppet)->anims.clear();
            rstd_info("MDLA parse aborted for {}; puppet keeps bind pose only", str_path);
        }
    }
    if (peek_block_magic(f, "MDMP")) {
        std::string tag = consume_tag();
        if (! ParseMDMP(f, mdl, tag, str_path)) return false;
    }
    if (peek_block_magic(f, "MDLE")) {
        std::string tag = consume_tag();
        if (! ParseMDLE(f, mdl, tag)) return false;
    }

    // hexpat Body: u8 trailing_nul (mdlv>=14). mdlv==13 file end is padded
    // with zeros until EOF.
    if (mdl.header.mdlv >= 14 && f.Tell() < f.Size()) {
        uint8_t trailing_nul = f.ReadUint8();
        if (trailing_nul != 0) {
            rstd_info("mdlv{} trailing_nul expected 0, got {}", mdl.header.mdlv, trailing_nul);
        }
    } else if (mdl.header.mdlv == 13) {
        while (f.Tell() < f.Size()) {
            auto    save = f.Tell();
            uint8_t b    = f.ReadUint8();
            if (b != 0) {
                f.SeekSet(save);
                break;
            }
        }
    }

    if (mdl.puppet.is_some()) {
        (*mdl.puppet)->world_anchored_bones = (mdl.header.mdlv == 21);
    }

    if (mdl.mdls >= 3) ApplyMDLS3CentroidPivot(mdl);

    if (mdl.puppet.is_some()) (*mdl.puppet)->prepared();

    rstd_info("read puppet: mdlv: {}, nmdls: {}, mdla: {}, mdle: {}, bones: {}, anims: {}",
              mdl.header.mdlv,
              mdl.mdls,
              mdl.mdla,
              mdl.mdle,
              mdl.puppet.is_some() ? (*mdl.puppet)->bones.len() : usize(),
              mdl.puppet.is_some() ? (*mdl.puppet)->anims.len() : usize());
    return true;
}

Option<wpscene::Material> MdlParser::ParseMaterial(ref<str> material_ref, fs::VFS& vfs) {
    const auto path   = ResolveMdlMaterialPath(rstd::cppstd::as_string_view(material_ref));
    auto       parsed = owe::ReadJsonFile(vfs, path, { .allow_comments = true });
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("load mdl material '{}' failed: {}", path, error.message.as_str());
        return None();
    }
    auto json = rstd::move(parsed).unwrap_unchecked();

    wpscene::Material material;
    material.blending   = "disabled";
    material.depthtest  = "enabled";
    material.depthwrite = "enabled";
    material.cullmode   = "back";
    if (! material.FromJson(json)) {
        rstd_error("parse mdl material '{}' failed", path);
        return None();
    }
    return Some(rstd::move(material));
}

Option<usize> MdlParser::FindMeshByMaterial(const Mdl& mdl, ref<str> material_ref) {
    const auto wanted = ResolveMdlMaterialPath(rstd::cppstd::as_string_view(material_ref));
    for (usize mesh_index {}; mesh_index < mdl.meshes.len(); ++mesh_index) {
        for (const auto& candidate : mdl.meshes[mesh_index].mat_json_files) {
            if (ResolveMdlMaterialPath(rstd::cppstd::as_string_view(candidate.as_str())) == wanted)
                return Some(mesh_index);
        }
    }
    return None();
}

void MdlParser::GenMeshFromMdl(SceneMesh::Submesh& submesh, const Mdl::Mesh& src,
                               array<float, 2> texcoord_scale, array<float, 3> position_offset) {
    const size_t vert_num = src.positions.len().to_primitive();
    if (vert_num == 0) return;
    if (! src.part_uv2.is_empty() || ! src.parts.is_empty()) {
        // V21+ part meshes already store primary UVs in backing-texture space.
        texcoord_scale = { 1.0f, 1.0f };
    }

    // Build the attribute list in a stable order. Skinning attrs come early so
    // a puppet vertex layout matches what WE shaders historically expect.
    std::vector<VertexAttrSpec>                      specs;
    std::vector<std::function<void(size_t, float*)>> packers;

    // Position is always present (the parser would have failed otherwise).
    specs.push_back(VAttr::Position);
    packers.push_back([&src, position_offset](size_t i, float* dst) {
        dst[0] = src.positions[usize(i)][usize(0)] + position_offset[usize(0)];
        dst[1] = src.positions[usize(i)][usize(1)] + position_offset[usize(1)];
        dst[2] = src.positions[usize(i)][usize(2)] + position_offset[usize(2)];
    });
    if (! src.normals.is_empty()) {
        specs.push_back(VAttr::Normal);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(dst, src.normals[usize(i)].data(), sizeof(src.normals[usize(i)]));
        });
    }
    if (! src.tangents.is_empty()) {
        specs.push_back(VAttr::Tangent4);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(dst, src.tangents[usize(i)].data(), sizeof(src.tangents[usize(i)]));
        });
    }
    if (! src.blend_indices.is_empty()) {
        specs.push_back(VAttr::BlendIndices);
        packers.push_back([&src](size_t i, float* dst) {
            std::memcpy(
                dst, src.blend_indices[usize(i)].data(), sizeof(src.blend_indices[usize(i)]));
        });
        // SKIN_BLEND without SKIN_WEIGHT is the WE 1-bone rigid convention;
        // emit synthetic [1,0,0,0] so the SKINNING shader path always has
        // valid weights to read.
        specs.push_back(VAttr::BlendWeights);
        const bool has_w = ! src.blend_weights.is_empty();
        packers.push_back([&src, has_w](size_t i, float* dst) {
            if (has_w) {
                std::memcpy(
                    dst, src.blend_weights[usize(i)].data(), sizeof(src.blend_weights[usize(i)]));
            } else {
                dst[0] = 1.0f;
                dst[1] = 0.0f;
                dst[2] = 0.0f;
                dst[3] = 0.0f;
            }
        });
    }
    if (! src.texcoords.is_empty()) {
        specs.push_back(VAttr::TexCoord);
        packers.push_back([&src, texcoord_scale](size_t i, float* dst) {
            dst[0] = src.texcoords[usize(i)][usize(0)] * texcoord_scale[usize(0)];
            dst[1] = src.texcoords[usize(i)][usize(1)] * texcoord_scale[usize(1)];
        });
    }
    const auto* uv2 = ! src.part_uv2.is_empty()    ? &src.part_uv2
                      : ! src.texcoord2.is_empty() ? &src.texcoord2
                                                   : nullptr;
    if (! src.texcoords.is_empty() && uv2 != nullptr && uv2->len() == usize(vert_num)) {
        specs.push_back(VAttr::TexCoordVec4);
        packers.push_back([&src, uv2, texcoord_scale](size_t i, float* dst) {
            dst[0] = src.texcoords[usize(i)][usize(0)] * texcoord_scale[usize(0)];
            dst[1] = src.texcoords[usize(i)][usize(1)] * texcoord_scale[usize(1)];
            dst[2] = (*uv2)[usize(i)][usize(0)];
            dst[3] = (*uv2)[usize(i)][usize(1)];
        });
    }

    auto             attrs = MakeAttrSet(specs);
    SceneVertexArray vertex(attrs, usize(vert_num));

    size_t stride_floats = 0;
    for (auto& a : attrs) stride_floats += SceneVertexArray::RealAttributeSize(a);
    std::vector<float> one_vert(stride_floats);

    for (size_t i = 0; i < vert_num; ++i) {
        size_t offset = 0;
        for (size_t k = 0; k < packers.size(); ++k) {
            packers[k](i, one_vert.data() + offset);
            offset += SceneVertexArray::RealAttributeSize(attrs[k]);
        }
        vertex.SetVertexs(
            usize(i), rstd::slice<float>::from_raw_parts(one_vert.data(), usize(one_vert.size())));
    }

    std::vector<uint32_t> indices;
    indices.reserve(src.indices.len().to_primitive() * 3);
    for (const auto& tri : src.indices) {
        for (uint32_t v : tri) indices.push_back(v);
    }

    submesh.vertex_arrays.emplace_back(std::move(vertex));
    submesh.index_arrays.emplace_back(
        SceneIndexArray(slice<uint32_t>::from_raw_parts(indices.data(), usize(indices.size()))));

    // V21 parts[] enumerates index sub-ranges in artist-chosen z-order. We
    // issue one DrawIndexed per range so each "part" is drawn as a separate
    // primitive batch, which lets later parts overdraw earlier ones (eyelid
    // covering pupil at peak blink) and leaves headroom for per-part state.
    if (! src.parts.is_empty()) {
        submesh.draw_ranges.reserve(src.parts.len().to_primitive());
        for (const auto& p : src.parts) {
            if (p.size == 0) continue;
            submesh.draw_ranges.push_back({ u32(p.start), u32(p.size) });
        }
    }
}

void MdlParser::GenMaskSubmeshFromMdl(SceneMesh::Submesh& submesh, const Mdl::Mesh& src,
                                      slice<uint32_t> clip_part_indices,
                                      array<float, 2> texcoord_scale) {
    GenMeshFromMdl(submesh, src, texcoord_scale);
    // `clip_part_indices` are positions in src.parts[] (0-based), not `part.id`.
    std::vector<SceneMesh::DrawRange> ranges;
    for (usize i {}; i < clip_part_indices.len(); ++i) {
        const uint32_t idx = clip_part_indices[i];
        if (idx >= src.parts.len().to_primitive()) continue;
        const auto& p = src.parts[usize(idx)];
        if (p.size == 0) continue;
        ranges.push_back({ u32(p.start), u32(p.size) });
    }
    submesh.draw_ranges = std::move(ranges);
}

void MdlParser::AddPuppetShaderInfo(ShaderInfo& info, const Mdl& mdl) {
    info.combos[rstd::cppstd::to_string(WE_CB_SKINNING)] = "1";
    info.combos[rstd::cppstd::to_string(WE_CB_BONECOUNT)] =
        std::to_string((*mdl.puppet)->bones.len().to_primitive());
}

void MdlParser::AddPuppetMatInfo(wpscene::Material& mat, const Mdl& mdl) {
    mat.combos[rstd::cppstd::to_string(WE_CB_SKINNING)] = i32(1);
    mat.combos[rstd::cppstd::to_string(WE_CB_BONECOUNT)] =
        rstd::as_cast<i32>((*mdl.puppet)->bones.len());
    mat.use_puppet = true;
}
