module;

module wescene.scene;
import eigen;
import rstd;

using namespace owe;
using namespace Eigen;

Matrix4d SceneNode::GetLocalTrans() const {
    Affine3d trans = Affine3d::Identity();
    trans.prescale(m_scale.cast<double>());

    // m_rotation is in radians. Static scene.json `angles` are already radians;
    // the JS scripting API uses degrees and converts at the boundary (Script.cpp
    // NodeSetAngles / the transform actuator), so everything stored here is rad.
    trans.prerotate(AngleAxis<double>(m_rotation.x(), Vector3d::UnitX())); // x
    trans.prerotate(AngleAxis<double>(m_rotation.y(), Vector3d::UnitY())); // y
    trans.prerotate(AngleAxis<double>(m_rotation.z(), Vector3d::UnitZ())); // z

    trans.pretranslate(m_translate.cast<double>());

    return m_local_frame * trans.matrix();
}

void SceneNode::SetLocalTrans(const Matrix4d& transform) {
    Matrix3d rotation;
    Matrix3d scaling;
    Affine3d(transform).computeRotationScaling(&rotation, &scaling);
    m_translate = transform.block<3, 1>(0, 3).cast<float>();
    m_scale     = scaling.diagonal().cast<float>();
    for (int axis = 0; axis < 3; ++axis)
        if (m_scale[axis] == 0.0f) m_scale[axis] = 1.0f;
    const Vector3d zyx = rotation.canonicalEulerAngles(2, 1, 0);
    m_rotation         = Vector3f(zyx.z(), zyx.y(), zyx.x());
    m_local_frame      = Matrix4d::Identity();
    // Retain shear introduced by non-uniformly scaled parents.
    m_local_frame = transform * GetLocalTrans().inverse();
    MarkTransDirty();
}

void SceneNode::RotateObjectSpace(const Vector3f& rotation) {
    const Quaternionf current = AngleAxisf(m_rotation.z(), Vector3f::UnitZ()) *
                                AngleAxisf(m_rotation.y(), Vector3f::UnitY()) *
                                AngleAxisf(m_rotation.x(), Vector3f::UnitX());
    const Quaternionf local   = AngleAxisf(rotation.z(), Vector3f::UnitZ()) *
                                AngleAxisf(rotation.y(), Vector3f::UnitY()) *
                                AngleAxisf(rotation.x(), Vector3f::UnitX());
    const Vector3f    zyx     = (current * local).toRotationMatrix().canonicalEulerAngles(2, 1, 0);
    SetRotation({ zyx.z(), zyx.y(), zyx.x() });
}

void SceneNode::UpdateTrans() { m_node.UpdateWorld(); }

void SceneNode::MarkTransDirty() { m_node.SetLocalMatrix(GetLocalTrans()); }

auto SceneNode::ChildIndex(const SceneNode& child) const -> Option<usize> {
    return m_node.ChildIndex(child);
}

bool SceneNode::MoveChild(SceneNode& child, usize index) { return m_node.MoveChild(child, index); }

SceneNode* SceneNode::FindByName(ref<str> name) {
    if (m_name.as_str() == name) return this;
    for (auto& child : GetChildren()) {
        if (auto* hit = child->FindByName(name)) return hit;
    }
    return nullptr;
}
