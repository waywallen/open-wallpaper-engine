#include <rstd/test/gtest.hpp>

import eigen;
import rstd.cppstd;
import wescene.fs;
import wescene.pkg_fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.spec_names;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

std::uint32_t MaxMeshIndex(const owe::Mdl::Mesh& mesh) {
    std::uint32_t max_index = 0;
    for (const auto& tri : mesh.indices) {
        for (std::uint32_t idx : tri) max_index = std::max(max_index, idx);
    }
    return max_index;
}

std::uint32_t CountUvSeamTriangles(const owe::Mdl::Mesh& mesh) {
    std::uint32_t seam_triangles = 0;
    for (const auto& tri : mesh.indices) {
        float min_u = std::numeric_limits<float>::max();
        float max_u = std::numeric_limits<float>::lowest();
        for (std::uint32_t idx : tri) {
            min_u = std::min(min_u, mesh.texcoords[usize(idx)][usize(0)]);
            max_u = std::max(max_u, mesh.texcoords[usize(idx)][usize(0)]);
        }
        if (max_u - min_u > 0.5f) ++seam_triangles;
    }
    return seam_triangles;
}

} // namespace

static auto MakeReplacementPuppet() -> Arc<owe::Puppet> {
    auto  puppet                      = Arc<owe::Puppet>::make();
    auto& bone                        = puppet->bones.emplace_back();
    bone.local_bind.translation().x() = 100.0f;
    auto& reference             = bone.animation_reference.insert(Eigen::Affine3f::Identity());
    reference.translation().x() = 4.0f;
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            track.frames.push(owe::Puppet::BoneFrame {
                .position =
                    Eigen::Vector3f(static_cast<float>(id * 20 - 10 + frame * 10), 0.0f, 0.0f),
                .angle = Eigen::Vector3f::Zero(),
                .scale = Eigen::Vector3f::Ones(),
            });
        }
    }
    puppet->prepared();
    return puppet;
}

TEST(Puppet, VisibilityChangesKeepTheSamePlayback) {
    auto                             puppet = MakeReplacementPuppet();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 1, .visible = false, .layer_id = 42 };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    auto playback = layer.AnimationPlayback(i32(42));
    ASSERT_TRUE(playback.is_some());
    EXPECT_TRUE(layer.AnimationPlayback(i32(99)).is_none());
    (*playback)->SetFrame(0.5f);
    EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), 4.0f);
    layer.SetAnimationVisible(i32(42), true);
    EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), 15.0f);
    layer.SetAnimationVisible(i32(42), false);
    EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), 4.0f);
    EXPECT_EQ((*layer.AnimationPlayback(i32(42))).as_ptr(), (*playback).as_ptr());
    EXPECT_FLOAT_EQ((*playback)->Sample().current, 0.5f);
}

TEST(Puppet, ReplacementLayersBlendSequentiallyFromReference) {
    auto puppet = MakeReplacementPuppet();
    for (bool reverse : { false, true }) {
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored[] = {
            { .id = reverse ? 2 : 1, .blend = reverse ? 0.25 : 0.5 },
            { .id = reverse ? 1 : 2, .blend = reverse ? 0.5 : 0.25 },
        };
        layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
        for (const auto& playback : layer.AnimationPlaybacks()) {
            playback->SetFrame(i32(1));
            playback->Pause();
        }
        EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), reverse ? 16.5f : 19.0f);
        EXPECT_FLOAT_EQ(layer.boneTransform(1, 100.0)->translation().x(), reverse ? 16.5f : 19.0f);
    }
}

TEST(Puppet, FirstLayerWeightAndVisibilityDoNotSelectAnAbsoluteAnchor) {
    auto puppet = MakeReplacementPuppet();
    for (bool additive : { false, true }) {
        for (bool visible : { false, true }) {
            for (double weight : { 0.0, 0.5, 1.0 }) {
                owe::PuppetLayer                 layer(puppet.clone());
                owe::PuppetLayer::AnimationLayer authored {
                    .id       = 1,
                    .blend    = weight,
                    .visible  = visible,
                    .additive = additive,
                };
                layer.prepared(
                    slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
                EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(),
                                visible ? 4.0f + 6.0f * static_cast<float>(weight) : 4.0f);
            }
        }
    }
}

TEST(Puppet, TransformFlagsPreserveMaskedTracksAndAcceptZeroPoses) {
    for (int flag : { 0, 1, 2, 3 }) {
        auto  puppet = MakeReplacementPuppet();
        auto& track  = puppet->anims[usize()].bone_tracks[usize()];
        track.unk    = flag;
        for (auto& frame : track.frames) frame.position.setZero();
        puppet->prepared();
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored { .id = 1 };
        layer.prepared(
            slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
        EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), (flag & 1) ? 4.0f : 0.0f);
    }
    auto puppet = MakeReplacementPuppet();
    puppet->anims[usize()].bone_tracks[usize()].frames.clear();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 1 };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), 4.0f);
}

TEST(Puppet, MixedLayersAndMaskedReplacementPreserveCurrentPose) {
    for (bool reverse : { false, true }) {
        auto                             puppet = MakeReplacementPuppet();
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored[] = {
            { .id = reverse ? 2 : 1, .blend = reverse ? 0.5 : 1.0, .additive = reverse },
            { .id = reverse ? 1 : 2, .blend = reverse ? 1.0 : 0.5, .additive = ! reverse },
        };
        layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
        EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), reverse ? 10.0f : 23.0f);
    }
    auto puppet                                      = MakeReplacementPuppet();
    puppet->anims[usize(1)].bone_tracks[usize()].unk = 1;
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = { { .id = 1 }, { .id = 2 } };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    EXPECT_FLOAT_EQ(layer.boneTransform(1, 0.0)->translation().x(), 10.0f);
}

TEST(Puppet, PartialReplacementInterpolatesRotationAndScaleFromReference) {
    auto  puppet    = MakeReplacementPuppet();
    auto& reference = *puppet->bones[usize()].animation_reference;
    reference.rotate(Eigen::AngleAxisf(0.4f, Eigen::Vector3f::UnitZ()));
    reference.scale(Eigen::Vector3f(2.0f, 3.0f, 1.0f));
    auto& track                                  = puppet->anims[usize()].bone_tracks[usize()];
    track.frames.first_mut().unwrap()->angle.x() = -1.6f;
    track.frames[usize(1)].angle.x()             = 1.2f;
    for (auto& frame : track.frames) frame.scale = Eigen::Vector3f(4.0f, 1.0f, 2.0f);
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 1, .blend = 0.25 };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    Vec<owe::SceneAnimationEvent> events;
    auto                          playback = layer.AnimationPlaybacks()[usize()].clone();
    playback->Advance(0.0, events);
    playback->Advance(0.25, events);
    playback->Pause();
    Eigen::Quaterniond sample;
    sample.coeffs() = (0.75 * track.frames.first().unwrap()->quaternion.coeffs() +
                       0.25 * track.frames[usize(1)].quaternion.coeffs())
                          .normalized();
    const Eigen::Quaterniond reference_rotation(Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitZ()));
    Eigen::Quaterniond       expected_rotation;
    expected_rotation.coeffs() =
        (0.75 * reference_rotation.coeffs() + 0.25 * sample.coeffs()).normalized();
    Eigen::Affine3f expected = Eigen::Affine3f::Identity();
    expected.translate(Eigen::Vector3f(6.125f, 0.0f, 0.0f));
    expected.rotate(expected_rotation.cast<float>());
    expected.scale(Eigen::Vector3f(2.5f, 2.5f, 1.25f));
    EXPECT_TRUE(layer.boneTransform(1, 0.0)->matrix().isApprox(expected.matrix(), 0.0001f));
}

TEST(Puppet, ArcOwnedLayerExposesBorrowedTransforms) {
    auto              puppet = Arc<owe::Puppet>::make();
    owe::Puppet::Bone bone;
    bone.name = String::make("root"_str);
    puppet->bones.push(rstd::move(bone));
    puppet->prepared();

    owe::PuppetLayer layer(puppet.clone());
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer> {});

    EXPECT_EQ(layer.boneIndex("root"_str), 1u);
    EXPECT_EQ(layer.boneIndex("missing"_str), 0u);
    EXPECT_TRUE(layer.boneTransform(0u, 0.0).is_none());
    auto transform = layer.boneTransform(1u, 0.0);
    ASSERT_TRUE(transform.is_some());
    EXPECT_TRUE(transform->matrix().isApprox(Eigen::Matrix4f::Identity()));
}

TEST(Puppet, SamplesTextureChannelBlendMapFromAnimationPlayback) {
    auto  puppet     = Arc<owe::Puppet>::make();
    auto& animation  = puppet->anims.emplace_back();
    animation.id     = 781;
    animation.name   = String::make("Arona Drool"_str);
    animation.mode   = owe::Puppet::PlayMode::Single;
    animation.fps    = 1.0f;
    animation.length = 2;
    auto& channels   = animation.trans.insert(owe::Puppet::AnimTrans {});
    channels.main_track.push(0.0f);
    channels.main_track.push(1.0f);
    channels.main_track.push(0.0f);
    auto& second = channels.tail_tracks.emplace_back();
    second.push(1.0f);
    second.push(0.5f);
    second.push(0.0f);
    puppet->prepared();

    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored {
        .id      = 781,
        .visible = true,
        .name    = String::make("Arona Drool"_str),
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    ASSERT_EQ(layer.AnimationPlaybacks().len(), usize(1));
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    layer.AnimationPlaybacks()[usize()]->Pause();

    const auto blend_map = layer.TextureChannelBlendMap(0.0);
    ASSERT_EQ(blend_map.len(), usize(4));
    EXPECT_FLOAT_EQ(blend_map[usize()], 1.0f);
    EXPECT_FLOAT_EQ(blend_map[usize(1)], 0.5f);
    EXPECT_FLOAT_EQ(blend_map[usize(2)], 0.0f);
    EXPECT_FLOAT_EQ(blend_map[usize(3)], 0.0f);
}

TEST(Puppet, SharedModelKeepsInstanceTransformsIndependent) {
    auto puppet = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back();
    auto& child                    = puppet->bones.emplace_back();
    child.bind_parent              = 0;
    child.anim_parent              = 0;
    child.file_parent              = 0;
    child.local_bind.translation() = Eigen::Vector3f(0.0f, 5.0f, 0.0f);
    auto& animation                = puppet->anims.emplace_back();
    animation.id                   = 1;
    animation.fps                  = 1.0;
    animation.length               = 1;
    animation.mode                 = owe::Puppet::PlayMode::Single;
    auto& track                    = animation.bone_tracks.emplace_back();
    for (float x : { 10.0f, 20.0f }) {
        track.frames.push(owe::Puppet::BoneFrame {
            .position = Eigen::Vector3f(x, 0.0f, 0.0f),
            .angle    = Eigen::Vector3f::Zero(),
            .scale    = Eigen::Vector3f::Ones(),
        });
    }
    puppet->prepared();
    owe::PuppetLayer                 first(puppet.clone());
    owe::PuppetLayer                 second(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 1 };
    auto layers = slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1));
    first.prepared(layers);
    second.prepared(layers);
    first.AnimationPlaybacks()[usize()]->Pause();
    second.AnimationPlaybacks()[usize()]->Pause();
    second.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    const auto first_frame = first.genFrame(0.0);
    ASSERT_EQ(first_frame.len(), usize(2));
    EXPECT_FLOAT_EQ(first_frame[usize()].translation().x(), 10.0f);
    EXPECT_FLOAT_EQ(first_frame[usize(1)].translation().x(), 10.0f);
    const auto second_frame = second.genFrame(0.0);
    EXPECT_FLOAT_EQ(second_frame[usize()].translation().x(), 20.0f);
    EXPECT_FLOAT_EQ(second_frame[usize(1)].translation().x(), 20.0f);
    EXPECT_FLOAT_EQ(first_frame[usize()].translation().x(), 10.0f);
    EXPECT_FLOAT_EQ(first_frame[usize(1)].translation().x(), 10.0f);

    first.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    EXPECT_FLOAT_EQ(first.boneTransform(2, 0.0)->translation().y(), 5.0f);
    first.AnimationPlaybacks()[usize()]->SetFrame(i32(0));
    EXPECT_FLOAT_EQ(first.genFrame(0.0)[usize(1)].translation().x(), 10.0f);
    EXPECT_FLOAT_EQ(second_frame[usize(1)].translation().x(), 20.0f);
    first.prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    EXPECT_TRUE(first.genFrame(0.0)[usize(1)].matrix().isApprox(Eigen::Matrix4f::Identity()));
    EXPECT_FLOAT_EQ(second_frame[usize(1)].translation().x(), 20.0f);
}

TEST(Puppet, AdditiveUsesSeparateAnimationReference) {
    auto  puppet                  = Arc<owe::Puppet>::make();
    auto& bone                    = puppet->bones.emplace_back();
    bone.local_bind.translation() = Eigen::Vector3f(100.0f, 200.0f, 0.0f);
    Eigen::Affine3f reference     = Eigen::Affine3f::Identity();
    reference.translate(Eigen::Vector3f(10.0f, 20.0f, 0.0f));
    reference.rotate(Eigen::AngleAxisf(0.25f, Eigen::Vector3f::UnitZ()));
    reference.scale(Eigen::Vector3f(2.0f, 3.0f, 1.0f));
    bone.animation_reference.insert(Eigen::Affine3f(reference));
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            track.frames.push(owe::Puppet::BoneFrame {
                .position = Eigen::Vector3f(10.0f + (id == 2 ? 4.0f * frame : 0.0f), 20.0f, 0.0f),
                .angle    = Eigen::Vector3f(0.0f, 0.0f, 0.25f),
                .scale    = Eigen::Vector3f(2.0f, 3.0f, 1.0f),
            });
        }
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true },
        { .id = 2, .blend = 0.5, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    for (const auto& playback : layer.AnimationPlaybacks()) playback->Pause();
    auto expected = reference * bone.inv_bind;
    EXPECT_TRUE(layer.genFrame(0.0)[usize()].matrix().isApprox(expected.matrix(), 0.0001f));
    layer.AnimationPlaybacks()[usize(1)]->SetFrame(i32(1));
    expected.pretranslate(Eigen::Vector3f(2.0f, 0.0f, 0.0f));
    EXPECT_TRUE(layer.genFrame(0.0)[usize()].matrix().isApprox(expected.matrix(), 0.0001f));
}

TEST(Puppet, LegacyAdditiveOnlyStackUsesEachClipsFirstFrame) {
    auto puppet                                           = Arc<owe::Puppet>::make();
    puppet->additive_uses_first_frame                     = true;
    puppet->bones.emplace_back().local_bind.translation() = Eigen::Vector3f(100.0f, 0.0f, 0.0f);
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            track.frames.push(owe::Puppet::BoneFrame {
                .position = Eigen::Vector3f(10.0f * id + 4.0f * frame, 0.0f, 0.0f),
                .angle    = Eigen::Vector3f::Zero(),
                .scale    = Eigen::Vector3f::Ones(),
            });
        }
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true, .additive = true },
        { .id = 2, .blend = 0.5, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    for (const auto& playback : layer.AnimationPlaybacks()) playback->Pause();
    EXPECT_FLOAT_EQ(layer.genFrame(0.0)[usize()].translation().x(), -90.0f);
    layer.AnimationPlaybacks()[usize(1)]->SetFrame(i32(1));
    EXPECT_FLOAT_EQ(layer.genFrame(0.0)[usize()].translation().x(), -88.0f);
}

TEST(Puppet, ReferenceAdditiveUsesLocalRotationDeltaAndNormalizedLinearBlend) {
    using Eigen::AngleAxisd;
    using Eigen::Quaterniond;
    using Eigen::Vector3d;
    const Quaterniond reference(AngleAxisd(0.7, Vector3d::UnitX()));
    const Quaterniond base(AngleAxisd(0.6, Vector3d::UnitZ()));
    const Quaterniond sample_a =
        AngleAxisd(1.8, Vector3d::UnitY()) * AngleAxisd(-0.4, Vector3d::UnitX());
    const Quaterniond sample_b =
        AngleAxisd(4.4, Vector3d::UnitZ()) * AngleAxisd(1.5, Vector3d::UnitY());
    ASSERT_LT(sample_a.dot(sample_b), 0.0);

    auto            puppet = Arc<owe::Puppet>::make();
    auto&           bone   = puppet->bones.emplace_back();
    Eigen::Affine3f rest   = Eigen::Affine3f::Identity();
    rest.rotate(reference.cast<float>());
    rest.scale(Eigen::Vector3f(2.0f, 3.0f, 1.0f));
    bone.animation_reference.insert(Eigen::Affine3f(rest));
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0;
        animation.length = 1;
        animation.mode   = owe::Puppet::PlayMode::Single;
        auto& track      = animation.bone_tracks.emplace_back();
        for (int frame : { 0, 1 }) {
            const Eigen::Vector3f angle = id == 1      ? Eigen::Vector3f(0.0f, 0.0f, 0.6f)
                                          : frame == 0 ? Eigen::Vector3f(-0.4f, 1.8f, 0.0f)
                                                       : Eigen::Vector3f(0.0f, 1.5f, 4.4f);
            track.frames.push(owe::Puppet::BoneFrame {
                .position = Eigen::Vector3f(10.0f, 0.0f, 0.0f),
                .angle    = angle,
                .scale    = Eigen::Vector3f(2.0f, 3.0f, 1.0f),
            });
        }
    }
    puppet->prepared();
    for (double weight : { 0.0, 0.25, 0.5, 1.0 }) {
        for (double t : { 0.0, 0.25, 1.0 }) {
            owe::PuppetLayer                 layer(puppet.clone());
            owe::PuppetLayer::AnimationLayer authored[] = {
                { .id = 1 },
                { .id = 2, .blend = weight, .additive = true },
            };
            layer.prepared(
                slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
            Vec<owe::SceneAnimationEvent> events;
            for (const auto& playback : layer.AnimationPlaybacks()) {
                playback->Advance(0.0, events);
                playback->Advance(t, events);
                playback->Pause();
            }
            Quaterniond sample;
            sample.coeffs() = ((1.0 - t) * sample_a.coeffs() - t * sample_b.coeffs()).normalized();
            Quaterniond delta = reference.conjugate() * sample;
            if (delta.w() < 0.0) delta.coeffs() *= -1.0;
            Quaterniond weighted;
            weighted.coeffs() = weight * delta.coeffs();
            weighted.w() += 1.0 - weight;
            weighted.normalize();
            Eigen::Affine3f expected = Eigen::Affine3f::Identity();
            expected.translate(
                Eigen::Vector3f(10.0f + 10.0f * static_cast<float>(weight), 0.0f, 0.0f));
            expected.rotate((base * weighted).cast<float>());
            expected.scale(Eigen::Vector3f(2.0f, 3.0f, 1.0f));
            EXPECT_TRUE(
                layer.genFrame(0.0)[usize()].matrix().isApprox(expected.matrix(), 0.00001f));
            EXPECT_TRUE(
                layer.genFrame(100.0)[usize()].matrix().isApprox(expected.matrix(), 0.00001f));
        }
    }
}

TEST(Puppet, BoneScalarCurvesDoNotMaskTransformSamples) {
    for (bool additive : { false, true }) {
        for (double weight : { 0.0, 0.5, 1.0 }) {
            auto  puppet    = MakeReplacementPuppet();
            auto& animation = puppet->anims[usize()];
            auto& curve     = animation.blend_curves.emplace_back();
            curve.values.push(0.0f);
            curve.values.push(1.0f);
            for (auto& frame : animation.bone_tracks[usize()].frames) frame.scale.y() = 0.0f;
            puppet->prepared();
            owe::PuppetLayer                 layer(puppet.clone());
            owe::PuppetLayer::AnimationLayer authored { .id       = 1,
                                                        .blend    = weight,
                                                        .additive = additive };
            layer.prepared(
                slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
            auto playback = layer.AnimationPlaybacks()[usize()].clone();
            playback->Pause();
            for (int frame : { 0, 1 }) {
                playback->SetFrame(i32(frame));
                const auto pose = layer.boneTransform(1, 0.0);
                ASSERT_TRUE(pose.is_some());
                EXPECT_NEAR(pose->linear()(1, 1), 1.0 - weight, 0.00001);
                EXPECT_NEAR(pose->translation().x(), 4.0 + weight * (6.0 + 10.0 * frame), 0.00001);
            }
        }
    }
}

TEST(MdlMesh, LegacyMissingReferenceSelectsFirstFrameDeltas) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "2907385672" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 2907385672 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/hu tao bos 3_puppet.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    ASSERT_EQ(mdl.mdls, 2);
    EXPECT_TRUE((*mdl.puppet)->additive_uses_first_frame);
    EXPECT_TRUE((*mdl.puppet)->bones[usize(3)].animation_reference.is_none());
}

TEST(MdlMesh, ReadsLegacyAnimationReferenceWithoutReplacingMeshBind) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3148125112" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 3148125112 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/人物2_puppet.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    ASSERT_EQ(mdl.mdls, 2);
    EXPECT_FALSE((*mdl.puppet)->additive_uses_first_frame);
    const auto& bone = (*mdl.puppet)->bones[usize(3)];
    ASSERT_TRUE(bone.animation_reference.is_some());
    EXPECT_FLOAT_EQ(bone.local_bind.translation().x(), 1627.5517578125f);
    EXPECT_FLOAT_EQ(bone.animation_reference->translation().x(), 318.3017578125f);
}

TEST(Puppet, FirstFrameRotationDeltasUseReferenceLocalSpace) {
    auto puppet = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back();
    puppet->additive_uses_first_frame = true;
    auto& animation                   = puppet->anims.emplace_back();
    animation.id                      = 1;
    animation.fps                     = 1.0;
    animation.length                  = 1;
    animation.mode                    = owe::Puppet::PlayMode::Single;
    auto& track                       = animation.bone_tracks.emplace_back();
    track.frames.push(owe::Puppet::BoneFrame {
        .position = Eigen::Vector3f::Zero(),
        .angle    = Eigen::Vector3f(0.6f, -0.4f, 0.2f),
        .scale    = Eigen::Vector3f::Ones(),
    });
    track.frames.push(owe::Puppet::BoneFrame {
        .position = Eigen::Vector3f::Zero(),
        .angle    = Eigen::Vector3f(-0.3f, 0.7f, -0.5f),
        .scale    = Eigen::Vector3f::Ones(),
    });
    puppet->prepared();
    const auto& base = track.frames.first().unwrap()->quaternion;
    const auto& end  = track.frames[usize(1)].quaternion;
    for (bool additive : { false, true }) {
        for (double weight : { 0.25, 0.5, 1.0 }) {
            owe::PuppetLayer::AnimationLayer authored {
                .id       = 1,
                .blend    = weight,
                .additive = additive,
            };
            for (float time : { 0.0f, 0.25f, 1.0f }) {
                owe::PuppetLayer layer(puppet.clone());
                layer.prepared(
                    slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
                Vec<owe::SceneAnimationEvent> events;
                const auto&                   playback = layer.AnimationPlaybacks()[usize()];
                playback->Advance(0.0, events);
                playback->Advance(time, events);
                playback->Pause();
                const auto expected = base.slerp(weight, base.slerp(time, end));
                EXPECT_TRUE(layer.genFrame(0.0)[usize()].linear().isApprox(
                    expected.toRotationMatrix().cast<float>(), 0.00001f));
            }
        }
    }
}

TEST(MdlMesh, StaggeredBodyAnimationsReproduceCapturedPose) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3462491575" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 3462491575 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/身体_puppet.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    auto puppet = mdl.puppet->clone();
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = { { .id = 729 },
                                                    { .id = 3835, .additive = true },
                                                    { .id = 4400, .additive = true } };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(3)));
    auto playbacks = layer.AnimationPlaybacks();
    playbacks[usize(0)]->SetFrame(i32(120));
    playbacks[usize(1)]->SetFrame(i32(96));
    playbacks[usize(2)]->SetFrame(43.2f);
    Vec<owe::SceneAnimationEvent> events;
    for (const auto& playback : playbacks) {
        playback->Advance(0.0, events);
        playback->Advance(1.3582, events);
    }
    const auto pose = layer.genFrame(1.3582);
    // Official frame 92, draw 450. Runtime fitted to the base animation's translation.
    const float translations[][2] = { { 0, 0 },
                                      { 5.5143356f, 14.705261f },
                                      { 8.888123f, 20.167690f },
                                      { 12.198914f, 16.576313f },
                                      { 17.055420f, 13.853511f },
                                      { 17.055412f, 13.853513f },
                                      { 8.888115f, 20.167698f },
                                      { 1.031670f, 10.194763f },
                                      { 1.263428f, 18.676138f },
                                      { 1.263428f, 18.676159f } };
    ASSERT_EQ(pose.len(), usize(10));
    for (usize i {}; i < pose.len(); ++i) {
        EXPECT_NEAR(pose[i].translation().x(), translations[i.to_primitive()][0], 0.02f);
        EXPECT_NEAR(pose[i].translation().y(), translations[i.to_primitive()][1], 0.02f);
    }
    EXPECT_NEAR(pose[usize(7)].matrix()(0, 0), 0.92220455f, 0.0001f);
}

TEST(MdlMesh, RayquazaReplacementPreservesSampledBoneChain) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3045001236" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 3045001236 is not available";
    owe::fs::VFS vfs;
    auto         pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/Rayquaza/Rayquaza.mdl"_str, vfs, mdl));
    ASSERT_TRUE(mdl.puppet.is_some());
    auto puppet = mdl.puppet->clone();
    ASSERT_TRUE(puppet->additive_uses_first_frame);
    puppet->prepared();
    int checked_animations = 0;
    for (const auto& animation : puppet->anims) {
        if (animation.id != 160 && animation.id != 161) continue;
        ++checked_animations;
        ASSERT_EQ(animation.bone_tracks.len(), puppet->bones.len());
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored { .id = animation.id };
        layer.prepared(
            slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
        for (int frame : { 0, 15, 30, 45 }) {
            layer.AnimationPlaybacks()[usize()]->SetFrame(i32(frame));
            layer.AnimationPlaybacks()[usize()]->Pause();
            const auto           actual = layer.genFrame(0.0);
            Vec<Eigen::Affine3f> expected;
            for (usize i {}; i < puppet->bones.len(); ++i) {
                const auto&     bone  = puppet->bones[i];
                const auto&     track = animation.bone_tracks[i];
                Eigen::Affine3f local = bone.local_bind;
                if (track.HasTransformSamples()) {
                    ASSERT_GT(track.frames.len(), usize(frame));
                    const auto& sample = track.frames[usize(frame)];
                    local              = Eigen::Affine3f::Identity();
                    local.translate(sample.position);
                    local.rotate(sample.quaternion.cast<float>());
                    local.scale(sample.scale);
                }
                if (! bone.noAnimParent()) local = expected[usize(bone.anim_parent)] * local;
                expected.push(rstd::move(local));
                const Eigen::Affine3f world = actual[i] * bone.world_bind;
                EXPECT_TRUE(world.matrix().isApprox(expected[i].matrix(), 0.0001f))
                    << "animation " << animation.id << " frame " << frame << " bone "
                    << i.to_primitive() << " " << rstd::cppstd::to_string(bone.name);
            }
        }
    }
    EXPECT_EQ(checked_animations, 2);
}

TEST(Puppet, SortCurvesDoNotScaleBoneTransforms) {
    for (float scalar : { 0.0f, -6100.0f, 9500.0f }) {
        auto puppet = Arc<owe::Puppet>::make();
        puppet->bones.emplace_back();
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = 688;
        animation.mode   = owe::Puppet::PlayMode::Single;
        animation.fps    = 1.0f;
        animation.length = 1;
        auto& track      = animation.bone_tracks.emplace_back();
        track.frames.push(owe::Puppet::BoneFrame {
            .position = Eigen::Vector3f::Zero(),
            .angle    = Eigen::Vector3f::Zero(),
            .scale    = Eigen::Vector3f::Ones(),
        });
        track.frames.push(owe::Puppet::BoneFrame {
            .position = Eigen::Vector3f(4.0f, 0.0f, 0.0f),
            .angle    = Eigen::Vector3f::Zero(),
            .scale    = Eigen::Vector3f(1.0f, 0.5f, 1.0f),
        });
        auto& curve = animation.scalar_curves.emplace_back();
        curve.values.emplace_back(scalar);
        curve.values.emplace_back(scalar);
        puppet->prepared();
        owe::PuppetLayer                 layer(puppet.clone());
        owe::PuppetLayer::AnimationLayer authored {
            .id       = 688,
            .blend    = 0.5,
            .visible  = true,
            .additive = true,
        };
        layer.prepared(
            slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
        layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
        layer.AnimationPlaybacks()[usize()]->Pause();
        auto matrices = layer.genFrame(0.0);
        ASSERT_EQ(matrices.len(), usize(1));
        EXPECT_FLOAT_EQ(matrices[usize()].translation().x(), 2.0f);
        EXPECT_FLOAT_EQ(matrices[usize()].linear()(1, 1), 0.75f);
    }
}

TEST(Puppet, DrawOrderUsesPlaybackAndStableIntegerKeys) {
    auto puppet = Arc<owe::Puppet>::make();
    for (int depth : { 100, 200, 300 }) puppet->bones.emplace_back().draw_order = depth;
    auto& animation  = puppet->anims.emplace_back();
    animation.id     = 42;
    animation.mode   = owe::Puppet::PlayMode::Single;
    animation.fps    = 1.0f;
    animation.length = 1;
    for (float target : { 500.0f, -200.0f, 100.0f }) {
        auto& curve = animation.scalar_curves.emplace_back();
        curve.values.push(0.0f);
        curve.values.emplace_back(target);
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored { .id = 42, .blend = 0.5, .visible = true };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(&authored, usize(1)));
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) },
                                                  { u32(1), i32(0) },
                                                  { u32(2), i32(0) } };
    const auto input = slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(3));
    layer.AnimationPlaybacks()[usize()]->Pause();
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(1));
    auto order = layer.DrawOrder(input);
    ASSERT_EQ(order.len(), usize(3));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(2));
    EXPECT_EQ(order[usize(2)], usize(0));
    layer.AnimationPlaybacks()[usize()]->SetFrame(i32(0));
    order = layer.DrawOrder(input);
    EXPECT_EQ(order[usize(0)], usize(0));
    EXPECT_EQ(order[usize(1)], usize(1));
    EXPECT_EQ(order[usize(2)], usize(2));
}

TEST(Puppet, DrawOrderKeepsTiesAndAppliesPartOffsets) {
    auto puppet                             = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back().draw_order = 100;
    puppet->prepared();
    owe::PuppetLayer layer(puppet.clone());
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) },
                                                  { u32(0), i32(-200) },
                                                  { u32(0), i32(0) } };
    const auto                        order =
        layer.DrawOrder(slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(3)));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(0));
    EXPECT_EQ(order[usize(2)], usize(2));
}

TEST(Puppet, DrawOrderBoundsAdditiveAgainstCurrentAndSample) {
    auto puppet                             = Arc<owe::Puppet>::make();
    puppet->bones.emplace_back().draw_order = 100;
    puppet->bones.emplace_back().draw_order = 100;
    for (int id : { 1, 2 }) {
        auto& animation  = puppet->anims.emplace_back();
        animation.id     = id;
        animation.fps    = 1.0f;
        animation.length = 1;
        animation.scalar_curves.emplace_back().values.push(id == 1 ? 300.0f : 150.0f);
        animation.scalar_curves.emplace_back().values.push(id == 1 ? 200.0f : 325.0f);
    }
    puppet->prepared();
    owe::PuppetLayer                 layer(puppet.clone());
    owe::PuppetLayer::AnimationLayer authored[] = {
        { .id = 1, .blend = 1.0, .visible = true },
        { .id = 2, .blend = 1.0, .visible = true, .additive = true },
    };
    layer.prepared(slice<owe::PuppetLayer::AnimationLayer>::from_raw_parts(authored, usize(2)));
    const owe::PuppetLayer::PartOrder parts[] = { { u32(0), i32(0) }, { u32(1), i32(0) } };
    const auto                        order =
        layer.DrawOrder(slice<owe::PuppetLayer::PartOrder>::from_raw_parts(parts, usize(2)));
    EXPECT_EQ(order[usize(0)], usize(0));
    EXPECT_EQ(order[usize(1)], usize(1));
}

TEST(MdlMesh, DrawOrderWithoutAnimationPreservesFileOrder) {
    auto puppet = Arc<owe::Puppet>::make();
    puppet->prepared();
    auto layer = Arc<owe::PuppetLayer>::make(puppet.clone());
    layer->prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    owe::Mdl::Mesh source;
    source.parts.push(owe::Mdl::Mesh::Part { 2, 0, 3 });
    owe::SceneMesh::Submesh submesh;
    submesh.draw_ranges.push({ u32(0), u32(3) });
    owe::MdlParser::BindDrawOrder(submesh, source, layer.clone());
    EXPECT_TRUE(submesh.draw_range_order.is_none());
}

TEST(MdlMesh, DrawOrderOwnsFilteredPartMapping) {
    auto puppet = Arc<owe::Puppet>::make();
    for (int depth : { 100, 200, 300 }) puppet->bones.emplace_back().draw_order = depth;
    puppet->anims.emplace_back().scalar_curves.emplace_back();
    puppet->prepared();
    auto layer = Arc<owe::PuppetLayer>::make(puppet.clone());
    layer->prepared(slice<owe::PuppetLayer::AnimationLayer> {});
    owe::SceneMesh::Submesh submesh;
    {
        owe::Mdl::Mesh source;
        source.parts.push(owe::Mdl::Mesh::Part { 2, 0, 3 });
        source.parts.push(owe::Mdl::Mesh::Part { 1, 3, 3 });
        source.parts.push(owe::Mdl::Mesh::Part { 0, 6, 3 });
        submesh.draw_ranges.push({ u32(0), u32(3) });
        submesh.draw_ranges.push({ u32(6), u32(3) });
        owe::MdlParser::BindDrawOrder(submesh, source, layer.clone());
    }
    ASSERT_TRUE(submesh.draw_range_order.is_some());
    const auto order = (*submesh.draw_range_order)->operator()();
    ASSERT_EQ(order.len(), usize(2));
    EXPECT_EQ(order[usize(0)], usize(1));
    EXPECT_EQ(order[usize(1)], usize(0));
}

TEST(MdlMesh, KeepsPuppetPositionsInMdlLocalSpace) {
    owe::Mdl::Mesh source;
    source.positions.push(array<float, 3> { 244.0f, 349.5f, 0.0f });
    source.texcoords.push(array<float, 2> { 0.25f, 0.75f });
    source.indices.push(array<std::uint32_t, 3> { 0u, 0u, 0u });

    owe::SceneMesh::Submesh submesh;
    owe::MdlParser::GenMeshFromMdl(submesh, source);

    ASSERT_EQ(submesh.vertex_arrays.len().to_primitive(), 1u);
    const auto& vertices = submesh.vertex_arrays[usize(usize())];
    ASSERT_NE(vertices.Data(), nullptr);
    EXPECT_FLOAT_EQ(vertices.Data()[0], 244.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[1], 349.5f);
    EXPECT_FLOAT_EQ(vertices.Data()[2], 0.0f);
}

TEST(MdlMesh, PackedAttributesPreserveIntegerBitsAndPadding) {
    owe::Mdl::Mesh source;
    source.positions.push(array<float, 3> { 1.0f, 2.0f, 3.0f });
    source.normals.push(array<float, 3> { 0.0f, 1.0f, 0.0f });
    source.tangents.push(array<float, 4> { 1.0f, 0.0f, 0.0f, -1.0f });
    const array<uint32_t, 4> bones { 1u, 17u, 255u, 4096u };
    source.blend_indices.push(array<uint32_t, 4>(bones));
    source.texcoords.push(array<float, 2> { 0.25f, 0.75f });
    source.indices.push(array<uint32_t, 3> { 0u, 0u, 0u });

    owe::SceneMesh::Submesh submesh;
    owe::MdlParser::GenMeshFromMdl(submesh, source);
    const auto& vertices         = submesh.vertex_arrays[usize(usize())];
    auto        component_offset = [&](ref<str> name) {
        return (vertices.AttributeOffset(name).unwrap() / usize(sizeof(float))).to_primitive();
    };
    const auto blend   = component_offset(owe::VAttr::BlendIndices.name);
    const auto weights = component_offset(owe::VAttr::BlendWeights.name);
    for (usize component {}; component < usize(4); ++component) {
        EXPECT_EQ(rstd::bit_cast<uint32_t>(vertices.Data()[blend + component.to_primitive()]),
                  bones[component]);
        EXPECT_FLOAT_EQ(vertices.Data()[weights + component.to_primitive()],
                        component == usize() ? 1.0f : 0.0f);
    }
    const auto normal = component_offset(owe::VAttr::Normal.name);
    EXPECT_FLOAT_EQ(vertices.Data()[normal + 1], 1.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[normal + 3], 0.0f);
    const auto tangent = component_offset(owe::VAttr::Tangent4.name);
    EXPECT_FLOAT_EQ(vertices.Data()[tangent + 3], -1.0f);
    const auto uv = component_offset(owe::VAttr::TexCoord.name);
    EXPECT_FLOAT_EQ(vertices.Data()[uv], 0.25f);
    EXPECT_FLOAT_EQ(vertices.Data()[uv + 1], 0.75f);
}

TEST(MdlMesh, FindsMeshByNormalizedMaterialReference) {
    owe::Mdl mdl;
    mdl.meshes.push(owe::Mdl::Mesh {});
    mdl.meshes.push(owe::Mdl::Mesh {});
    mdl.meshes[usize()].mat_json_files.push(String::make("materials/main.json"_str));
    mdl.meshes[usize(1)].mat_json_files.push(String::make("overlay"_str));

    auto main = owe::MdlParser::FindMeshByMaterial(mdl, "materials/main"_str);
    ASSERT_TRUE(main.is_some());
    EXPECT_EQ(*main, usize());

    auto overlay = owe::MdlParser::FindMeshByMaterial(mdl, "materials/overlay.json"_str);
    ASSERT_TRUE(overlay.is_some());
    EXPECT_EQ(*overlay, usize(1));
    EXPECT_TRUE(owe::MdlParser::FindMeshByMaterial(mdl, "missing"_str).is_none());
}

TEST(MdlMesh, Mdlv23LargeStaticMeshUsesUint32GlobalIndices) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3557068717" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 3557068717 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/球体01/球体01.mdl"_str, vfs, mdl));
    ASSERT_FALSE(mdl.meshes.is_empty());

    const auto& mesh = mdl.meshes[usize()];
    ASSERT_EQ(mdl.header.mdlv, 23);
    ASSERT_EQ(mesh.positions.len(), usize(520192));
    ASSERT_EQ(mesh.texcoords.len(), mesh.positions.len());
    ASSERT_EQ(mesh.indices.len(), usize(260096));
    ASSERT_LT(MaxMeshIndex(mesh), mesh.positions.len().to_primitive());
    EXPECT_EQ(mesh.indices.first().unwrap().get(), (array<std::uint32_t, 3> { 0u, 1u, 2u }));
    EXPECT_EQ(mesh.indices[usize(1)], (array<std::uint32_t, 3> { 0u, 2u, 3u }));
    EXPECT_EQ(mesh.indices.last().unwrap().get(),
              (array<std::uint32_t, 3> { 520188u, 520190u, 520191u }));
    EXPECT_EQ(CountUvSeamTriangles(mesh), 0u);

    owe::SceneMesh::Submesh submesh;
    owe::MdlParser::GenMeshFromMdl(submesh, mesh);
    ASSERT_EQ(submesh.vertex_arrays.len().to_primitive(), 1u);
    ASSERT_EQ(submesh.index_arrays.len().to_primitive(), 1u);
    EXPECT_TRUE(submesh.draw_ranges.is_empty());

    const auto& index_array = submesh.index_arrays[usize(usize())];
    ASSERT_EQ(index_array.DataCount(), rstd::usize(780288));
    EXPECT_EQ(index_array.Data()[0], 0u);
    EXPECT_EQ(index_array.Data()[1], 1u);
    EXPECT_EQ(index_array.Data()[2], 2u);
    EXPECT_EQ(index_array.Data()[780285], 520188u);
    EXPECT_EQ(index_array.Data()[780286], 520190u);
    EXPECT_EQ(index_array.Data()[780287], 520191u);
}

TEST(MdlMesh, Mdlv23ReadsPerMeshMaterialSkins) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "1979606285" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 1979606285 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/prism/prism.mdl"_str, vfs, mdl));
    ASSERT_EQ(mdl.header.mdlv, 23);
    ASSERT_EQ(mdl.header.skin_count, 2u);
    ASSERT_EQ(mdl.header.mesh_count, 1u);
    ASSERT_EQ(mdl.meshes.len(), usize(1));

    const auto& mesh = mdl.meshes[usize()];
    ASSERT_EQ(mesh.mat_json_files.len(), usize(2));
    EXPECT_EQ(mesh.mat_json_files.first().unwrap()->as_str(), "materials/prism/prism.json"_str);
    EXPECT_EQ(mesh.mat_json_files[usize(1)].as_str(), "materials/prism/prism_main.json"_str);
    EXPECT_EQ(mesh.positions.len(), usize(60));
    EXPECT_EQ(mesh.indices.len(), usize(32));
}

TEST(MdlPuppet, Mdlv23ReadsMultiCurveMorphEvents) {
    const std::filesystem::path pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3686252018" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) {
        GTEST_SKIP() << "workshop 3686252018 is not available";
    }

    owe::fs::VFS vfs;
    auto         assets_fs = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    if (assets_fs.is_ok()) {
        ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets_fs).unwrap_unchecked()).is_ok());
    }
    auto pkg_fs =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg_fs.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg_fs->mount_handle()).is_ok());

    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/sheet_puppet.mdl"_str, vfs, mdl));
    ASSERT_EQ(mdl.mdla, 6);
    ASSERT_TRUE(mdl.puppet.is_some());

    const auto& anims = (*mdl.puppet)->anims;
    ASSERT_EQ(anims.len(), usize(17));
    const auto& left_eye = anims[usize()];
    ASSERT_EQ(left_eye.name.as_str(), "Left eye"_str);
    ASSERT_EQ(left_eye.v4_events.len(), usize(1));

    const auto& event = left_eye.v4_events.first().unwrap().get();
    EXPECT_EQ(event.flags, 0);
    ASSERT_EQ(event.curves.len(), usize(6));
    for (usize i {}; i < event.curves.len(); ++i) {
        const auto& curve = event.curves[i];
        EXPECT_EQ(curve.id, i.to_primitive());
        ASSERT_EQ(curve.values.len(), usize(211));
        EXPECT_FLOAT_EQ(curve.values.first().unwrap().get(), 1.0f);
    }

    ASSERT_EQ(mdl.morph_sections.len(), usize(1));
    EXPECT_FLOAT_EQ(mdl.morph_sections[usize()].event_time, event.time);
    EXPECT_EQ(mdl.morph_sections[usize()].sections.len(), event.curves.len());
}

TEST(MdlMesh, BlockVersionsUseBoundedDecimalPrefixes) {
    struct Case {
        ref<str>    text;
        Option<int> version;
    };
    const Case cases[] = {
        { "0000"_str, Some(0) },
        { "0001"_str, Some(1) },
        { "9999"_str, Some(9999) },
        { " +2x"_str, Some(2) },
        { "-1xx"_str, Some(-1) },
        { "\v3\0x"_str, Some(3) },
        { "xxxx"_str, None() },
        { " +xx"_str, None() },
        { "\0"
          "123"_str,
          None() },
        { "    "_str, None() },
    };
    auto         temporary = rstd::fs::TempDir::make("owe-mdl-version"_str).unwrap();
    auto         root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    owe::fs::VFS vfs;
    auto         mount = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(mount.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(mount).unwrap()).is_ok());
    for (auto block : { "MDLA"_str, "MDMP"_str, "MDLE"_str }) {
        for (const auto& test : cases) {
            Vec<u8> bytes;
            auto    append = [&](ref<str> text) {
                for (auto byte : text.as_bytes()) bytes.push(rstd::move(byte));
            };
            auto append_u32 = [&](unsigned value) {
                for (unsigned shift = 0; shift < 32; shift += 8)
                    bytes.push(u8((value >> shift) & 255));
            };
            append("MDLV0014\0"_str);
            append_u32(0);
            append_u32(1);
            append_u32(0);
            append("MDLS0001\0"_str);
            append_u32(38);
            append_u32(0);
            append(block);
            append(test.text);
            bytes.push(u8());
            if (block != "MDLA"_str || test.version != Some(0)) {
                append_u32(static_cast<unsigned>(bytes.len().to_primitive()) +
                           (block == "MDMP"_str ? 4u : 8u));
                if (block != "MDMP"_str) append_u32(0);
            }
            bytes.push(u8());
            ASSERT_TRUE(
                rstd::fs::write(root.join("test.mdl"_str).as_path(), bytes.as_slice()).is_ok());
            owe::Mdl   mdl;
            const bool parsed = owe::MdlParser::Parse("test.mdl"_str, vfs, mdl);
            EXPECT_EQ(parsed, test.version.is_some());
            if (test.version.is_some()) {
                EXPECT_EQ(block == "MDLA"_str   ? mdl.mdla
                          : block == "MDMP"_str ? mdl.mdmp
                                                : mdl.mdle,
                          *test.version);
            }
        }
    }
}

TEST(MdlMesh, NativeMaterialPathsShareResolutionAndPreserveDefaults) {
    auto temporary = rstd::fs::TempDir::make("owe-mdl-material"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    ASSERT_TRUE(
        rstd::fs::write(
            root.join("main.json"_str).as_path(),
            R"({/* authored material */"passes":[{"shader":"genericimage","textures":["diffuse"]}]})"_bytes)
            .is_ok());
    owe::fs::VFS vfs;
    auto         mount = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(mount.is_ok());
    ASSERT_TRUE(vfs.mount("/assets/materials"_str, rstd::move(mount).unwrap()).is_ok());
    auto short_ref = owe::MdlParser::ParseMaterial("main"_str, vfs);
    auto full_ref  = owe::MdlParser::ParseMaterial("materials/main.json"_str, vfs);
    ASSERT_TRUE(short_ref.is_some());
    ASSERT_TRUE(full_ref.is_some());
    EXPECT_EQ(short_ref->shader, full_ref->shader);
    EXPECT_EQ(short_ref->shader, "genericimage"_str);
    EXPECT_EQ(short_ref->depthtest, "enabled"_str);
    EXPECT_EQ(short_ref->depthwrite, "enabled"_str);
    EXPECT_EQ(short_ref->cullmode, "back"_str);
    ASSERT_EQ(short_ref->textures.len(), usize(1));
    EXPECT_EQ(short_ref->textures[usize()], "diffuse"_str);
    EXPECT_TRUE(owe::MdlParser::ParseMaterial("missing"_str, vfs).is_none());
}
