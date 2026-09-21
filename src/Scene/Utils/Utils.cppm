module;

#include <rstd/macro.hpp>

export module wescene.utils;
import wescene.core;
import eigen;
import rstd;
import rstd.log;
export import wescene.types;

using rstd::time::Instant;

using namespace rstd::prelude;

// ---------- Module-purview entities ----------------------------------------

export namespace owe
{

// FpsCounter — was Utils/FpsCounter.h. Implementation in Utils/FpsCounter.cpp.
class FpsCounter {
public:
    FpsCounter();
    u32  Fps() const { return m_fps; }
    u32  FrameCount() const { return m_frameCount; }
    void RegisterFrame();

private:
    u32     m_fps;
    u32     m_frameCount;
    Instant m_startTime;
};

namespace algorism
{
// Was Utils/Algorism.h. Out-of-line in Utils/Algorism.cpp module impl unit.
double CalculatePersperctiveDistance(double fov, double height) noexcept;
double CalculatePersperctiveFov(double distence, double height) noexcept;
double PerlinNoise(double x, double y, double z) noexcept;

constexpr u32 PowOfTwo(u32 x) {
    u32 pow2 { 8 };
    while (pow2 < x) pow2 *= u32(2);
    return pow2;
}
constexpr bool IsPowOfTwo(u32 x) { return (x > u32(1)) && ((x & (x - u32(1))) == u32()); }

inline Eigen::Vector3d sph2cart(const Eigen::Vector3d& sph) noexcept {
    double azimuth   = sph.x();
    double elevation = sph.y();
    double radius    = sph.z();
    return radius * Eigen::Vector3d {
        f64(azimuth).cos().to_primitive() * f64(elevation).cos().to_primitive(),
        f64(azimuth).sin().to_primitive() * f64(elevation).cos().to_primitive(),
        f64(elevation).sin().to_primitive(),
    };
}

template<typename TFUNC>
Eigen::Vector3d GenSphereSurface(TFUNC&& random) noexcept {
    double azimuth   = f64::consts::TAU.to_primitive() * random();
    double elevation = f64(2.0 * random() - 1.0).asin().to_primitive();
    return sph2cart({ azimuth, elevation, 1.0 });
}

template<typename TFUNC>
Eigen::Vector3d GenSphereSurfaceNormal(TFUNC&&                normal_random,
                                       const Eigen::Vector3d& direct) noexcept {
    double u    = direct.x() > 0.0 ? normal_random(0.0, direct.x()) : 0.0;
    double v    = direct.y() > 0.0 ? normal_random(0.0, direct.y()) : 0.0;
    double w    = direct.z() > 0.0 ? normal_random(0.0, direct.z()) : 0.0;
    double norm = f64((u * u + v * v + w * w)).sqrt().to_primitive();
    return Eigen::Vector3d(u, v, w) / norm;
}

template<typename TFUNC>
Eigen::Vector3d GenSphereIn(TFUNC&& random) noexcept {
    return f64(random()).powf(f64(1.0 / 3.0)).to_primitive() * GenSphereSurface(random);
}

constexpr double DragForce(double speed, double strength, double density) {
    return -speed * strength * density;
}
inline Eigen::Vector3d DragForce(Eigen::Vector3d v, double strength,
                                 double density = 1.0) noexcept {
    return v.normalized() * DragForce(v.norm(), strength, density);
}

constexpr double lerp(double t, double a, double b) noexcept { return a + t * (b - a); }

constexpr double PerlinEase(double t) noexcept { return t * t * t * (t * (t * 6 - 15) + 10); }

inline Eigen::Vector3d PerlinNoiseVec3(Eigen::Vector3d p) noexcept {
    return Eigen::Vector3d { PerlinNoise(p[0], p[1], p[2]),
                             PerlinNoise(p[0] + 89.2, p[1] + 33.1, p[2] + 57.3),
                             PerlinNoise(p[0] + 100.3, p[1] + 120.1, p[2] + 142.2) };
}

inline Eigen::Vector3d CurlNoise(Eigen::Vector3d p) noexcept {
    using namespace Eigen;
    constexpr double e = 1e-5;
    Vector3d         dx(e, 0, 0), dy(0, e, 0), dz(0, 0, e);
    Vector3d         x0 = PerlinNoiseVec3(p - dx), x1 = PerlinNoiseVec3(p + dx),
                     y0 = PerlinNoiseVec3(p - dy), y1 = PerlinNoiseVec3(p + dy),
                     z0 = PerlinNoiseVec3(p - dz), z1 = PerlinNoiseVec3(p + dz);
    double           x = y1.z() - y0.z() - z1.y() + z0.y();
    double           y = z1.x() - z0.x() - x1.z() + x0.z();
    double           z = x1.y() - x0.y() - y1.x() + y0.x();
    return Vector3d(x, y, z) / (2.0 * e);
}
} // namespace algorism

} // namespace owe

// ---------- Migrated from former Utils/{Hash,DynamicLibrary} ----------------

export namespace utils
{

String genSha1(slice<rstd::byte> input);

// DynamicLibrary lives in wescene.types now (re-exported above).

} // namespace utils

// Eigen helpers — were Utils/Eigen.h. Module-attached additions to the Eigen
// namespace; only visible to TUs that `import wescene.utils;`.
export namespace Eigen
{
constexpr double Radians(double a) noexcept {
    return (a / 180.0f) * f64::consts::PI.to_primitive();
}

inline Matrix4d LookAt(Vector3d eye, Vector3d center, Vector3d up) noexcept {
    Vector3d camDir = center - eye;
    Vector3d zAxis  = -camDir.normalized();
    Vector3d xAxis  = up.cross(zAxis).normalized();
    Vector3d yAxis  = zAxis.cross(xAxis).normalized();

    Matrix4d view          = Matrix4d::Identity();
    view.block<1, 3>(0, 0) = xAxis.transpose();
    view.block<1, 3>(1, 0) = yAxis.transpose();
    view.block<1, 3>(2, 0) = zAxis.transpose();
    view(0, 3)             = -xAxis.dot(eye);
    view(1, 3)             = -yAxis.dot(eye);
    view(2, 3)             = -zAxis.dot(eye);
    return view;
}

inline Matrix4d Ortho(double left, double right, double bottom, double top, double nearz,
                      double farz) noexcept {
    Affine3d trans = Affine3d::Identity();
    trans.pretranslate(
        Vector3d(-(left + right) / 2.0f, -(top + bottom) / 2.0f, -(nearz + farz) / 2.0f));
    trans.prescale(Vector3d(2.0f / (right - left), 2.0f / (top - bottom), 2.0f / (farz - nearz)));
    trans.scale(Vector3d(1.0f, 1.0f, -1.0f));
    trans.prescale(Vector3d(1.0f, 1.0f, 0.5f));
    trans.pretranslate(Vector3d(0.0f, 0.0f, 0.5f));
    return trans.matrix();
}

inline Matrix4d Perspective(double fov, double aspect, double nearz, double farz) noexcept {
    Projective3d trans = Projective3d::Identity();
    trans.prescale(Vector3d(nearz, nearz, (nearz + farz)));
    trans(3, 2)  = 1.0f;
    trans(3, 3)  = 0.0f;
    trans(2, 3)  = -nearz * farz;
    double top   = f64(fov / 2.0f).tan().to_primitive() * f64(nearz).abs().to_primitive();
    double right = top * aspect;
    trans.scale(Vector3d(1.0f, 1.0f, -1.0f));
    trans.prescale(Vector3d(1.0f, 1.0f, -1.0f));
    return Ortho(-right, right, -top, top, nearz, farz) * trans.matrix();
}
} // namespace Eigen
