module;

#include <rstd/macro.hpp>
module wescene.scene;
import eigen;
import rstd;
import rstd.log;
import wescene.utils;

using rstd::f64;

using namespace owe;
using namespace Eigen;

namespace
{
Matrix4d NodeCameraFrame(SceneNode& node) {
    node.UpdateTrans();

    Matrix4d frame = node.ModelTrans();
    if (! frame.allFinite()) return Matrix4d::Identity();

    constexpr double kAxisEps = 1e-10;
    Vector3d         x        = frame.block<3, 1>(0, 0);
    Vector3d         y        = frame.block<3, 1>(0, 1);
    Vector3d         z        = frame.block<3, 1>(0, 2);

    if (! z.allFinite() || z.squaredNorm() <= kAxisEps) {
        z = x.cross(y);
        if (! z.allFinite() || z.squaredNorm() <= kAxisEps) z = Vector3d::UnitZ();
        frame.block<3, 1>(0, 2) = z.normalized();
    }

    if (! frame.allFinite() || f64(frame.determinant()).abs().to_primitive() <= kAxisEps)
        return Matrix4d::Identity();
    return frame;
}
} // namespace

Vector3d SceneCamera::GetPosition(SceneRenderViewKind view) const {
    Vector3d position = Vector3d::Zero();
    if (m_lookat) {
        position = m_eye;
    } else if (m_node) {
        position = Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
    }
    if (view == SceneRenderViewKind::Reflection) position.y() = -position.y();
    return position;
}

Vector3d SceneCamera::GetDirection() const {
    if (m_lookat) return (m_center - m_eye).normalized();
    if (m_node) {
        return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
    }
    return -Vector3d::UnitZ();
}

auto SceneCamera::Transforms() const -> SceneCameraTransforms {
    if (m_lookat) return SceneCameraTransforms { .eye = m_eye, .center = m_center, .up = m_up };
    if (m_node) {
        const Matrix4d frame = NodeCameraFrame(*m_node);
        const Vector3d eye   = frame.block<3, 1>(0, 3);
        return SceneCameraTransforms {
            .eye    = eye,
            .center = eye - frame.block<3, 1>(0, 2),
            .up     = frame.block<3, 1>(0, 1),
        };
    }
    return {};
}

bool SceneCamera::SetTransforms(const SceneCameraTransforms& transforms) {
    const Vector3d direction = transforms.center - transforms.eye;
    if (! transforms.eye.allFinite() || ! transforms.center.allFinite() ||
        ! transforms.up.allFinite() || direction.squaredNorm() <= 1e-20 ||
        transforms.up.squaredNorm() <= 1e-20 ||
        direction.cross(transforms.up).squaredNorm() <= 1e-20)
        return false;
    SetLookAt(transforms.eye, transforms.center, transforms.up);
    Update();
    return true;
}

auto SceneCamera::AuthoredTransforms() const -> SceneCameraTransforms {
    auto transforms = Transforms();
    if (m_perspective || ! m_node) return transforms;

    const Matrix4d frame_inverse = NodeCameraFrame(*m_node).inverse();
    transforms.eye               = (frame_inverse * transforms.eye.homogeneous()).head<3>();
    transforms.center            = (frame_inverse * transforms.center.homogeneous()).head<3>();
    transforms.up                = frame_inverse.block<3, 3>(0, 0) * transforms.up;
    return transforms;
}

bool SceneCamera::SetAuthoredTransforms(const SceneCameraTransforms& transforms) {
    if (m_perspective || ! m_node) return SetTransforms(transforms);

    const Matrix4d frame = NodeCameraFrame(*m_node);
    return SetTransforms(SceneCameraTransforms {
        .eye    = (frame * transforms.eye.homogeneous()).head<3>(),
        .center = (frame * transforms.center.homogeneous()).head<3>(),
        .up     = frame.block<3, 3>(0, 0) * transforms.up,
    });
}

Matrix4d SceneCamera::GetViewMatrix() {
    CalculateViewProjectionMatrix();
    return m_camera.View();
}

Matrix4d SceneCamera::GetViewProjectionMatrix(SceneRenderViewKind view) {
    if (view == SceneRenderViewKind::Reflection) return CalculateReflectionViewProjectionMatrix();
    CalculateViewProjectionMatrix();
    return m_camera.ViewProjection();
}

auto SceneCamera::CameraSnapshot(SceneRenderViewKind view) -> vrento::CameraSnapshot<Matrix4d> {
    if (view == SceneRenderViewKind::Reflection) {
        CalculateReflectionViewProjectionMatrix();
        return m_reflection_camera.Snapshot();
    }
    CalculateViewProjectionMatrix();
    return m_camera.Snapshot();
}

Matrix4d SceneCamera::CalculateReflectionViewProjectionMatrix() {
    Vector3d eye    = Vector3d::Zero();
    Vector3d center = -Vector3d::UnitZ();
    Vector3d up     = Vector3d::UnitY();
    if (m_lookat) {
        eye    = m_eye;
        center = m_center;
        up     = m_up;
    } else if (m_node) {
        const Matrix4d frame = NodeCameraFrame(*m_node);
        eye                  = frame.block<3, 1>(0, 3);
        center               = eye - frame.block<3, 1>(0, 2);
        up                   = frame.block<3, 1>(0, 1);
    }
    eye.y()    = -eye.y();
    center.y() = -center.y();
    // WE preserves camera-up so the reflection texture remains screen-upright.

    const Matrix4d view = LookAt(eye, center, up);
    m_reflection_camera.Set(view, ProjectionMatrix());
    return m_reflection_camera.ViewProjection();
}

Matrix4d SceneCamera::ProjectionMatrix() const {
    if (m_perspective) {
        return Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip);
    }
    return Ortho(
        -m_width / 2.0, m_width / 2.0, -m_height / 2.0, m_height / 2.0, m_nearClip, m_farClip);
}

void SceneCamera::CalculateViewProjectionMatrix() {
    Matrix4d view;
    if (m_lookat) {
        view = LookAt(m_eye, m_center, m_up);
    } else if (m_node) {
        // view = inv(node.ModelTrans()) so the layer-local frame maps to
        // view origin regardless of where the node sits in the world (parent
        // chain + local translate / scale / rotate). With LookAt-only the
        // node's local scale would leak into clip space and a 9× scaled
        // layer would only see 1/9 of its quad inside the ortho viewport.
        view = NodeCameraFrame(*m_node).inverse();
    } else
        view = Matrix4d::Identity();
    m_camera.Set(view, ProjectionMatrix());
}

void SceneCamera::Update() { CalculateViewProjectionMatrix(); }

void SceneCamera::AttatchNode(SceneNode* node) {
    if (! node) {
        rstd_error("Attach a null node to camera");
        return;
    }
    m_node   = node;
    m_lookat = false; // node-based view takes over from any explicit LookAt
}
