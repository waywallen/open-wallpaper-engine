module;

export module wescene.pkg.puppet;
import eigen;
import wescene.core;
import wescene.scene;
import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe

{

class PuppetLayer;

class Puppet {
public:
    enum class PlayMode
    {
        Loop,
        Mirror,
        Single
    };
    static constexpr rstd::uint32_t NO_PARENT = 0xFFFFFFFFu;

    // Bind hierarchy and animation hierarchy are tracked independently.
    //
    // - bind_parent drives prepared(): world_bind = parent.world_bind * local_bind.
    // - anim_parent drives genFrame(): the per-frame pose is composed along the
    //   anim chain, so children inherit their parent's animated motion.
    //
    // MDLV<21 stores the same parent for both. MDLV21 stores a flat bind
    // silhouette (bones are already world-anchored) but keeps the file's parent
    // chain for animation, so eye/lash bones still inherit their parent's blink.
    struct Bone {
        String name;
        // hexpat MDLS Bone.sim_type: 0=static, 1=physics target, 3=IK chain.
        rstd::int32_t           sim_type { 0 };
        rstd::int32_t           draw_order { 0 };
        Eigen::Affine3f         local_bind { Eigen::Affine3f::Identity() };
        Option<Eigen::Affine3f> animation_reference;
        rstd::uint32_t          bind_parent { NO_PARENT };
        rstd::uint32_t          anim_parent { NO_PARENT };
        // Original on-file parent index. MDLV21 flattens bind_parent for
        // skinning while keeping anim_parent on this chain.
        rstd::uint32_t file_parent { NO_PARENT };

        // Per-bone WE bone_simulation JSON (spring/damping/gravity for
        // hair/cloth). Captured raw; evaluation hook is TBD.
        String simulation_json;

        // prepared
        Eigen::Affine3f world_bind { Eigen::Affine3f::Identity() };
        Eigen::Affine3f inv_bind { Eigen::Affine3f::Identity() };

        // Mean position of vertices weighted to this bone, expressed as an
        // offset from local_bind.translation(). Used to bake frame.scale into
        // a pure-translation g_Bones[i] (matches WE DXBC convention) — the
        // sprite anchored to this bone is approximated as a rigid body
        // centered at this offset, so scale compresses the sprite toward
        // bone.t without producing LBS triangle stretching.
        Eigen::Vector3f vertex_centroid_offset { 0.0f, 0.0f, 0.0f };

        // Per-bone "offset transform" from the MDLS section's `has_offset_trans`
        // block (3-float pos + 4x4 mat). The 3-float pos is the bone's true
        // skinning pivot in puppet-local coords — WE scales around this point,
        // not the computed vertex centroid. Captured even if mdls < 3 so the
        // existing parser path stays valid.
        Eigen::Vector3f file_skin_pivot { 0.0f, 0.0f, 0.0f };
        Eigen::Matrix4f file_skin_mat { Eigen::Matrix4f::Identity() };
        bool            has_file_skin_pivot { false };

        // Per-bone 4x4 from the MDLE section. Format is known; semantics
        // unconfirmed — linear() bit-matches local_bind.linear() but
        // translation() doesn't match world_bind / inv_bind / file_skin_pivot
        // / (local + centroid). Captured raw; no consumer yet.
        Eigen::Affine3f file_world_bind { Eigen::Affine3f::Identity() };
        bool            has_file_world_bind { false };

        bool noBindParent() const { return bind_parent == NO_PARENT; }
        bool noAnimParent() const { return anim_parent == NO_PARENT; }
    };

    // Named locator/anchor parsed from the MDAT section between MDLS and MDLA.
    // 64-byte payload is a column-major 4x4 affine transform in the anchored
    // bone's local space; `unk` is the bone index this attachment is wired to.
    // Consumed by ImageObject `attachment = "<name>"` to position child
    // images at named bone offsets (e.g. bangs under a head bone).
    struct Attachment {
        rstd::uint16_t  bone_index { 0 }; // hexpat MDAT Attachment.unk
        String          name;
        Eigen::Affine3f local_xform { Eigen::Affine3f::Identity() };

        Eigen::Affine3f bind_xform { Eigen::Affine3f::Identity() };
    };
    struct BoneFrame {
        Eigen::Vector3f position;
        Eigen::Vector3f angle;
        Eigen::Vector3f scale;

        // prepared
        Eigen::Quaterniond quaternion;
    };
    // One animated channel. Today only BoneTrack exists; the parser leaves
    // a vec slot per bone (dense, in bone order). MorphTrack / SlotTrack
    // are deliberately not declared yet — drop them in as sibling vectors
    // on Animation when V22+ shows up, not as variants over this one.
    struct BoneTrack {
        rstd::uint32_t bone_index { 0 };
        rstd::int32_t  unk { 0 }; // hexpat BoneTrack.unk
        Vec<BoneFrame> frames;

        bool HasTransformSamples() const { return (unk & 1) == 0 && ! frames.is_empty(); }
    };

    // MDLA >= 3 animation payload. Scalar main/tail tracks contain the texture-channel
    // blend curves consumed by puppettexturechannels; alternate wider tracks and the
    // optional prefix remain preserved until their semantics are identified.
    struct AnimTrans {
        Vec<float>      extra_track;
        Vec<float>      main_track;
        Vec<Vec<float>> tail_tracks;
    };

    // Per-bone, per-frame curve (anim.length + 1 samples). Reused by both the
    // mdla>=3 blend_curves block and mdla==6 scalar_curves
    // (draw order values).
    struct BoneFrameCurve {
        Vec<float> values;
    };

    struct AnimV4Curve {
        rstd::uint16_t id;
        Vec<float>     values;
    };

    // mdla>=4 timed morph event. Curves correspond to the MDMP entries at the same time.
    struct AnimV4Event {
        float            time;
        rstd::uint16_t   flags;
        Vec<AnimV4Curve> curves;
    };

    struct AnimSourceClip {
        rstd::uint16_t source_animation_index { 0 };
        rstd::uint32_t start_frame { 0 };
        rstd::uint32_t end_frame { 0 };
        rstd::uint32_t frame_offset { 0 };
        rstd::int32_t  motion_root_bone { -1 };
    };

    // Trailing event list. `event_json` is the WE editor's keyframe payload.
    struct AnimEvent {
        rstd::uint32_t time_value;
        String         event_json;
    };

    struct Animation {
        rstd::int32_t  id;
        rstd::uint32_t unk_after_id { 0 };
        rstd::uint32_t flags { 0 };
        double         fps;
        rstd::int32_t  length;
        PlayMode       mode;
        String         name;

        Vec<BoneTrack> bone_tracks;

        // mdla>=3 trans block (presence gated by trans_flag).
        Option<AnimTrans> trans;
        // MDLA >=3 per-bone scalar data, not TRS weights; retained for interpretation.
        Vec<BoneFrameCurve> blend_curves;
        // mdla>=4 timed events.
        Vec<AnimV4Event> v4_events;
        // mdla>=5 anim AABB.
        array<float, 3> aabb_min {};
        array<float, 3> aabb_max {};
        bool            has_aabb { false };
        // MDLA v6 per-bone draw order curves, sampled independently of TRS.
        Vec<BoneFrameCurve> scalar_curves;
        // Source animation and frame range for editor-created clips.
        Option<AnimSourceClip> source_clip;
        // Trailing event list (all mdla versions).
        Vec<AnimEvent> events;

        // prepared
        double max_time;
        double frame_time;
        struct InterpolationInfo {
            usize  frame_a;
            usize  frame_b;
            double t;
        };
        InterpolationInfo getInterpolationInfo(double* cur_time) const;
    };

    // MDLS v3 IK chain configuration. Schema derived from a single corpus
    // sample (hexpat MDLSBlock extras_flag==2 path); fields kept raw.
    struct BoneDir {
        rstd::uint32_t  bone_id;
        array<float, 3> dir;
    };
    struct ChainBoneDir {
        rstd::uint16_t  chain_id;
        rstd::uint32_t  bone_id;
        array<float, 3> dir;
    };
    struct BoneCond {
        rstd::uint16_t cnt;
        rstd::uint32_t id;
        rstd::uint32_t child;
        rstd::uint32_t val;
    };
    struct IkConfig {
        Eigen::Matrix4f               chain_a_target { Eigen::Matrix4f::Identity() };
        uint8_t                       ik_version { 0 };
        array<rstd::uint32_t, 2>      ik_header {};
        Eigen::Matrix4f               chain_b_target { Eigen::Matrix4f::Identity() };
        array<uint8_t, 7>             ik_flags {};
        array<Eigen::Vector3f, 6>     pole_targets {};
        Vec<BoneDir>                  rest_rotations;
        Vec<ChainBoneDir>             ik_targets;
        Option<BoneDir>               ik_target_root;
        BoneCond                      ik_constraint {};
        array<Vec<rstd::uint32_t>, 2> ik_bone_lists;
        rstd::uint32_t                ik_chain_count { 0 };
        array<float, 2>               ik_chain_length {};
        Vec<rstd::uint32_t>           ik_chain_bones;
    };

public:
    Vec<Bone>        bones;
    Vec<Animation>   anims;
    Vec<Attachment>  attachments;
    Option<IkConfig> ik_config;

    // MDLV21 puppets store bones as world-anchored (each `local_bind.t` is
    // a puppet-local position, not parent-relative) and sprite vertices live
    // at `bind.t + vertex_centroid_offset`. Skinning needs to flatten the
    // parent chain and pivot scale/rotation around the centroid; MDLV22+
    // uses standard chain LBS. Set at parse time from `header.mdlv`.
    bool world_anchored_bones { false };
    bool additive_uses_first_frame { false };

    void                    prepared();
    Option<usize>           attachmentIndex(ref<str> name) const noexcept;
    Option<Eigen::Affine3f> attachmentBindTransform(usize index) const noexcept;
};

class PuppetLayer {
public:
    explicit PuppetLayer(Arc<Puppet>);
    ~PuppetLayer();

    struct AnimationLayer {
        rstd::int32_t id { 0 }; // animation file id (PKGV0001+)
        double        rate { 1.0f };
        double        blend { 1.0f };
        bool          visible { true };
        double        cur_time { 0.0f };

        // Schema-only absorption (renderer reads only id/rate/blend/visible).
        rstd::int32_t layer_id { 0 };     // animationlayers[].id (was unread)
        String        name;               // animationlayers[].name (was unread)
        bool          additive { false }; // PKGV0019+; blend operator
        bool          blendin { false };  // PKGV0021+
        bool          blendout { false }; // PKGV0021+
        double        blendtime { 0.0 };  // PKGV0021+

        auto Clone() const -> AnimationLayer;
    };

    void prepared(slice<AnimationLayer>);

    // Borrowed until this instance is evaluated, prepared, or destroyed again.
    slice<Eigen::Affine3f>  genFrame(double time) noexcept;
    rstd::uint32_t          boneIndex(ref<str> name) const noexcept;
    Option<Eigen::Affine3f> boneTransform(rstd::uint32_t index, double time) noexcept;
    Option<Eigen::Affine3f> attachmentTransform(usize index, double time) noexcept;
    auto AnimationPlaybacks() const noexcept -> slice<Arc<SceneAnimationPlayback>>;
    auto AnimationPlayback(i32 layer_id) const -> Option<Arc<SceneAnimationPlayback>>;
    void SetAnimationVisible(i32 layer_id, bool visible);
    auto TextureChannelBlendMap(double time) noexcept -> slice<float>;

    struct PartOrder {
        u32 bone;
        i32 offset;
    };
    auto DrawOrder(slice<PartOrder>) const -> Vec<usize>;
    bool HasDrawOrderAnimation() const;

    void updateInterpolation(double time) noexcept;

private:
    struct Layer {
        AnimationLayer                       anim_layer;
        const Puppet::Animation*             anim { nullptr };
        Option<Arc<SceneAnimationPlayback>>  playback;
        bool                                 legacy_absolute_base { false };
        Puppet::Animation::InterpolationInfo interp_info {};

        bool IsAdditiveTransform() const { return anim_layer.additive && ! legacy_absolute_base; }

        operator bool() const noexcept { return anim != nullptr; };
    };

    Vec<Layer>                       m_layers;
    Vec<Arc<SceneAnimationPlayback>> m_playbacks;
    Vec<float>                       m_texture_channel_blend_map;
    Vec<Eigen::Affine3f>             m_final_affines;
    Arc<Puppet>                      m_puppet;
};

} // namespace owe
