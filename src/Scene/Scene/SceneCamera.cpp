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
using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{
Matrix4d SanitizeNodeCameraFrame(Matrix4d frame) {
    if (! frame.allFinite()) return Matrix4d::Identity();

    constexpr double kAxisEps = 1e-10;
    const Vector3d   x        = frame.block<3, 1>(0, 0).stableNormalized();
    const Vector3d   y        = frame.block<3, 1>(0, 1).stableNormalized();
    const Vector3d   normal   = x.cross(y);
    Vector3d         z        = frame.block<3, 1>(0, 2).stableNormalized();

    if (z.isZero(0.0)) {
        if (normal.squaredNorm() <= kAxisEps) return Matrix4d::Identity();
        z                       = normal.normalized();
        frame.block<3, 1>(0, 2) = z;
    }

    // Test the axes, not the world-space volume of a small but invertible frame.
    if (f64(normal.dot(z)).abs().to_primitive() <= kAxisEps) return Matrix4d::Identity();
    return frame;
}
} // namespace

Vector3d SceneCamera::GetPosition(SceneRenderViewKind view) const {
    return CurrentRenderTransforms(view).eye;
}

Vector3d SceneCamera::GetDirection() const {
    const auto transforms = Transforms();
    return (transforms.center - transforms.eye).normalized();
}

auto SceneCamera::Transforms() const -> SceneCameraTransforms {
    if (! m_lookat && m_node) RefreshAttachedFrame();
    return BaseTransforms();
}

auto SceneCamera::BaseTransforms() const -> SceneCameraTransforms {
    if (m_lookat) return SceneCameraTransforms { .eye = m_eye, .center = m_center, .up = m_up };
    if (m_node) {
        const Vector3d eye = m_attached_frame.block<3, 1>(0, 3);
        return SceneCameraTransforms {
            .eye    = eye,
            .center = eye - m_attached_frame.block<3, 1>(0, 2),
            .up     = m_attached_frame.block<3, 1>(0, 1),
        };
    }
    return {};
}

auto SceneCamera::RenderTransforms(SceneRenderViewKind view) const -> SceneCameraTransforms {
    return CurrentRenderTransforms(view);
}

auto SceneCamera::ViewCache(SceneRenderViewKind view) const -> CachedView& {
    return view == SceneRenderViewKind::Reflection ? m_reflection_view : m_primary_view;
}

auto SceneCamera::CurrentRenderTransforms(SceneRenderViewKind view) const
    -> const SceneCameraTransforms& {
    if (! m_lookat && m_node) RefreshAttachedFrame();
    return CachedTransforms(ViewCache(view), view);
}

auto SceneCamera::CachedTransforms(CachedView& cache, SceneRenderViewKind view) const
    -> const SceneCameraTransforms& {
    if (cache.transforms) return *cache.transforms;
    auto transforms = BaseTransforms();
    transforms.eye += m_view_offset;
    transforms.center += m_view_offset;
    if (view == SceneRenderViewKind::Reflection) {
        transforms.eye.y()    = -transforms.eye.y();
        transforms.center.y() = -transforms.center.y();
    }
    cache.transforms = Some(rstd::move(transforms));
    return *cache.transforms;
}

void SceneCamera::InvalidateMatrices() const {
    m_primary_view.matrices_dirty    = true;
    m_reflection_view.matrices_dirty = true;
}

void SceneCamera::InvalidateRenderTransforms() const {
    m_primary_view.transforms    = None();
    m_reflection_view.transforms = None();
    InvalidateMatrices();
}

void SceneCamera::InvalidateAttachment() {
    m_attached_revision = None();
    InvalidateRenderTransforms();
}

void SceneCamera::RefreshAttachedFrame() const {
    m_node->UpdateTrans();
    const auto revision = m_node->NodeState().WorldRevision();
    if (m_attached_revision && *m_attached_revision == revision) return;
    m_attached_frame    = SanitizeNodeCameraFrame(m_node->ModelTrans());
    m_attached_revision = Some(u64(revision));
    if (! m_lookat) InvalidateRenderTransforms();
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

    RefreshAttachedFrame();
    const Matrix4d frame_inverse = m_attached_frame.inverse();
    transforms.eye               = (frame_inverse * transforms.eye.homogeneous()).head<3>();
    transforms.center            = (frame_inverse * transforms.center.homogeneous()).head<3>();
    transforms.up                = frame_inverse.block<3, 3>(0, 0) * transforms.up;
    return transforms;
}

bool SceneCamera::SetAuthoredTransforms(const SceneCameraTransforms& transforms) {
    if (m_perspective || ! m_node) return SetTransforms(transforms);

    RefreshAttachedFrame();
    const auto& frame = m_attached_frame;
    return SetTransforms(SceneCameraTransforms {
        .eye    = (frame * transforms.eye.homogeneous()).head<3>(),
        .center = (frame * transforms.center.homogeneous()).head<3>(),
        .up     = frame.block<3, 3>(0, 0) * transforms.up,
    });
}

Matrix4d SceneCamera::GetViewMatrix(SceneRenderViewKind view) {
    return RenderMatrices(view).View();
}

Matrix4d SceneCamera::GetViewProjectionMatrix(SceneRenderViewKind view) {
    return RenderMatrices(view).ViewProjection();
}

auto SceneCamera::CameraSnapshot(SceneRenderViewKind view) -> vrento::CameraSnapshot<Matrix4d> {
    return RenderMatrices(view).Snapshot();
}

Matrix4d SceneCamera::ProjectionMatrix() const {
    if (m_perspective) {
        return Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip);
    }
    return Ortho(
        -m_width / 2.0, m_width / 2.0, -m_height / 2.0, m_height / 2.0, m_nearClip, m_farClip);
}

auto SceneCamera::RenderMatrices(SceneRenderViewKind kind) const
    -> const vrento::CameraState<Matrix4d>& {
    if (! m_lookat && m_node) RefreshAttachedFrame();
    auto& cache = ViewCache(kind);
    if (! cache.matrices_dirty) return cache.matrices;

    Matrix4d view;
    if (kind == SceneRenderViewKind::Reflection) {
        // WE preserves camera-up so the reflection stays screen-upright.
        const auto& transforms = CachedTransforms(cache, kind);
        view                   = LookAt(transforms.eye, transforms.center, transforms.up);
    } else {
        if (m_lookat) view = LookAt(m_eye, m_center, m_up);
        // Invert the full frame so layer-local coordinates retain their scale.
        else if (m_node)
            view = m_attached_frame.inverse();
        else
            view = Matrix4d::Identity();
        view = view * Affine3d(Translation3d(-m_view_offset)).matrix();
    }
    cache.matrices.Set(view, ProjectionMatrix());
    cache.matrices_dirty = false;
    return cache.matrices;
}

void SceneCamera::Update() { (void)RenderMatrices(SceneRenderViewKind::Primary); }

void Scene::UpdateCameraFillMode(u32 width, u32 height, FillMode fillmode) {
    if (width == u32() || height == u32()) return;
    auto         extent = OrthographicProjectionExtent();
    const double sw = extent[usize()], sh = extent[usize(1)];
    if (sw <= 0.0 || sh <= 0.0) return;
    const double output_aspect = rstd::as_cast<double>(width) / rstd::as_cast<double>(height);
    const double scene_aspect  = sw / sh;
    auto         global        = CameraMut("global"_str);
    auto         perspective   = CameraMut("global_perspective"_str);
    if (global.is_none() || perspective.is_none()) return;
    auto& gcam = **global;
    auto& pcam = **perspective;

    switch (fillmode) {
    case FillMode::STRETCH:
        gcam.SetWidth(sw);
        gcam.SetHeight(sh);
        pcam.SetAspect(scene_aspect);
        break;
    case FillMode::ASPECTFIT:
        if (output_aspect < scene_aspect) {
            gcam.SetWidth(sw);
            gcam.SetHeight(sw / output_aspect);
        } else {
            gcam.SetWidth(sh * output_aspect);
            gcam.SetHeight(sh);
        }
        pcam.SetAspect(output_aspect);
        break;
    case FillMode::ASPECTCROP:
    default:
        if (output_aspect > scene_aspect) {
            gcam.SetWidth(sw);
            gcam.SetHeight(sw / output_aspect);
        } else {
            gcam.SetWidth(sh * output_aspect);
            gcam.SetHeight(sh);
        }
        pcam.SetAspect(output_aspect);
        break;
    }
    if (pcam.FovPolicy() == SceneCameraFovPolicy::OrthographicProjection)
        pcam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gcam.Height()));
    gcam.Update();
    pcam.Update();
    UpdateLinkedCamera("global"_str);
    UpdateLinkedCamera("global_perspective"_str);
    CaptureCameraPathViewports();
    // Reapply the current camera path sample to the updated viewport.
    TickCameraPaths();
}

void SceneCamera::AttatchNode(SceneNode* node) {
    if (! node) {
        rstd_error("Attach a null node to camera");
        return;
    }
    if (m_owned_node.is_some() && (*m_owned_node).as_ptr().as_raw_ptr() != node)
        m_owned_node = None();
    m_node   = node;
    m_lookat = false; // node-based view takes over from any explicit LookAt
    InvalidateAttachment();
}

void SceneCamera::AttachOwnedNode(Arc<SceneNode> node) {
    m_node       = node.as_ptr().as_raw_ptr();
    m_owned_node = Some(rstd::move(node));
    m_lookat     = false;
    InvalidateAttachment();
}
