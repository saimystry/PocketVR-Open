// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <span>

namespace pocketvr {
constexpr double pi = 3.14159265358979323846;
struct Vec3 {
    double x{}, y{}, z{};
    Vec3 operator+(Vec3 b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    Vec3 operator-(Vec3 b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    Vec3 operator*(double s) const {
        return {x * s, y * s, z * s};
    }
    double dot(Vec3 b) const {
        return x * b.x + y * b.y + z * b.z;
    }
    Vec3 cross(Vec3 b) const {
        return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x};
    }
    double norm() const {
        return std::sqrt(dot(*this));
    }
    bool finite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    Vec3 unit() const {
        const auto n = norm();
        if (n < 1e-12 || !std::isfinite(n))
            throw std::invalid_argument("Invalid vector");
        return *this * (1 / n);
    }
};
struct Quat {
    double x{}, y{}, z{}, w{1};
    bool finite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
    }
    double norm2() const {
        return x * x + y * y + z * z + w * w;
    }
    Quat unit() const {
        const auto n = std::sqrt(norm2());
        if (n < 1e-12 || !std::isfinite(n))
            throw std::invalid_argument("Invalid quaternion");
        return {x / n, y / n, z / n, w / n};
    }
    Quat inverse() const {
        const auto n = norm2();
        if (n < 1e-24 || !std::isfinite(n))
            throw std::invalid_argument("Invalid quaternion");
        return {-x / n, -y / n, -z / n, w / n};
    }
    Quat operator*(Quat b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Vec3 rotate(Vec3 v) const {
        const auto q = unit();
        const Vec3 u{q.x, q.y, q.z};
        return v + u.cross(v) * (2 * q.w) + u.cross(u.cross(v)) * 2;
    }
    static Quat exponential(Vec3 rotation) {
        const auto a = rotation.norm();
        if (!std::isfinite(a))
            throw std::invalid_argument("Non-finite rotation");
        const auto s = a < 1e-8 ? 0.5 - a * a / 48 : std::sin(a / 2) / a;
        return Quat{rotation.x * s, rotation.y * s, rotation.z * s, std::cos(a / 2)}.unit();
    }
};
inline Quat predict(Quat q, Vec3 body_omega, double seconds, double horizon = 0.08) {
    if (!std::isfinite(seconds) || !std::isfinite(horizon) || horizon < 0)
        throw std::invalid_argument("Invalid prediction interval");
    return (q.unit() * Quat::exponential(body_omega * std::clamp(seconds, 0.0, horizon))).unit();
}
inline Quat sampling_rotation(Quat rendered, Quat current) {
    return (rendered.inverse() * current).unit();
}
inline Quat geometry_rotation(Quat rendered, Quat current) {
    return (current.inverse() * rendered).unit();
}
struct Pose {
    Vec3 position{};
    Quat orientation{};
    Vec3 angular_velocity{};
};
struct Projection {
    double left{1}, right{1}, up{1}, down{1}; // Positive tangent extents.
    bool valid() const {
        return std::isfinite(left) && std::isfinite(right) && std::isfinite(up) &&
               std::isfinite(down) && left > 0 && right > 0 && up > 0 && down > 0 && left < 20 &&
               right < 20 && up < 20 && down < 20;
    }
    std::array<double, 2> uv(Vec3 ray) const {
        if (!valid() || !ray.finite() || ray.z >= -1e-8)
            return {-1, -1};
        return {(ray.x / -ray.z + left) / (left + right), (up - ray.y / -ray.z) / (up + down)};
    }
    Vec3 ray(double u, double v) const {
        return {(left + right) * u - left, up - (up + down) * v, -1};
    }
};
struct Lens {
    double k1{0.34}, k2{0.55};
    double distort(double r) const {
        const double r2 = r * r;
        return r * (1 + k1 * r2 + k2 * r2 * r2);
    }
    double inverse(double target) const {
        if (!std::isfinite(target) || target < 0 || !std::isfinite(k1) || !std::isfinite(k2) ||
            k1 < 0 || k2 < 0)
            throw std::invalid_argument("Invalid monotonic lens");
        double lo = 0, hi = std::max(1.0, target);
        for (int i = 0; i < 48; ++i) {
            const auto mid = (lo + hi) / 2;
            if (distort(mid) < target)
                lo = mid;
            else
                hi = mid;
        }
        return (lo + hi) / 2;
    }
};
struct MeshVertex {
    float x, y, ray_x, ray_y;
};
inline void lens_mesh_into(const Lens &lens, const Projection &p, std::span<MeshVertex> vertices,
                           unsigned columns = 40, unsigned rows = 40) {
    if (!p.valid() || !std::isfinite(lens.k1) || !std::isfinite(lens.k2) || lens.k1 < 0 ||
        lens.k2 < 0 || columns < 2 || rows < 2 || columns > 256 || rows > 256)
        throw std::invalid_argument("Invalid lens mesh");
    if (vertices.size() != static_cast<size_t>(columns + 1) * (rows + 1))
        throw std::invalid_argument("Invalid lens mesh storage size");
    size_t index{};
    for (unsigned y = 0; y <= rows; ++y)
        for (unsigned x = 0; x <= columns; ++x) {
            const double u = static_cast<double>(x) / columns, v = static_cast<double>(y) / rows;
            const auto ray = p.ray(u, v);
            const auto radius = std::hypot(ray.x, ray.y);
            // Fixed screen-grid vertices map to texture rays using forward
            // distortion. Cardboard's texture-grid construction instead moves
            // screen vertices with the inverse of this same mapping.
            const auto scale = radius < 1e-12 ? 1 : lens.distort(radius) / radius;
            vertices[index++] = {static_cast<float>(2 * u - 1), static_cast<float>(1 - 2 * v),
                                 static_cast<float>(ray.x * scale),
                                 static_cast<float>(ray.y * scale)};
        }
}
// Eye view size. Scales the picture about the lens axis so each eye stays centered
// on its lens; the rays are untouched, so lens correction is unchanged.
inline void scale_mesh_about_lens(std::span<MeshVertex> vertices, const Projection &p, double scale) {
    if (!p.valid() || !std::isfinite(scale) || scale < 0.7 || scale > 1.6)
        throw std::invalid_argument("Invalid eye view size");
    const auto axis_x = 2 * p.left / (p.left + p.right) - 1;
    const auto axis_y = 1 - 2 * p.up / (p.up + p.down);
    for (auto &vertex : vertices) {
        vertex.x = static_cast<float>(axis_x + (vertex.x - axis_x) * scale);
        vertex.y = static_cast<float>(axis_y + (vertex.y - axis_y) * scale);
    }
}
inline std::vector<MeshVertex> lens_mesh(const Lens &lens, const Projection &p,
                                         unsigned columns = 40, unsigned rows = 40) {
    if (columns < 2 || rows < 2 || columns > 256 || rows > 256)
        throw std::invalid_argument("Invalid lens mesh dimensions");
    std::vector<MeshVertex> vertices(static_cast<size_t>(columns + 1) * (rows + 1));
    lens_mesh_into(lens, p, vertices, columns, rows);
    return vertices;
}
} // namespace pocketvr
