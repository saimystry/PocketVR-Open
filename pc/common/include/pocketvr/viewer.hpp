// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/math.hpp>
#include <cstdint>

namespace pocketvr {
struct ViewerGeometry {
    std::array<Projection, 2> screen{}, eyes{};
    uint32_t id{};
    double ipd_m{0.0639};
};
// Extra rendered pixels for late rotation; the physical screen/lens aperture
// remains unchanged. Both the PC projection and phone sampler use this extent.
inline Projection guarded_projection(const Projection &visible) {
    if (!visible.valid())
        throw std::invalid_argument("Invalid visible projection");
    const auto expand = [](double extent) {
        return std::min(std::nextafter(20.0, 0.0),
                        std::tan(std::min(std::atan(20.0), std::atan(extent) + 8.0 * pi / 180.0)));
    };
    return {expand(visible.left), expand(visible.right), expand(visible.up), expand(visible.down)};
}
// Landscape screen, centered vertically. Physical lens centers determine the
// off-axis projection; user IPD belongs to the eye-to-head transform instead.
inline ViewerGeometry calibrated_viewer(double width, double height, double spacing,
                                        double distance, Lens lens, double fov_scale,
                                        double vertical_offset = 0,
                                        Projection limits_deg = {89, 89, 89, 89}) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(spacing) ||
        !std::isfinite(distance) || !std::isfinite(fov_scale) || width <= spacing || height <= 0 ||
        !std::isfinite(vertical_offset) || std::abs(vertical_offset) >= height / 2 ||
        !(std::isfinite(limits_deg.left) && std::isfinite(limits_deg.right) &&
          std::isfinite(limits_deg.up) && std::isfinite(limits_deg.down) &&
          limits_deg.left >= 1 && limits_deg.left <= 89 && limits_deg.right >= 1 && limits_deg.right <= 89 &&
          limits_deg.up >= 1 && limits_deg.up <= 89 && limits_deg.down >= 1 && limits_deg.down <= 89) || spacing <= 0 || distance <= 0 || fov_scale < 0.7 || fov_scale > 1.3 ||
        !std::isfinite(lens.k1) || !std::isfinite(lens.k2) || lens.k1 < 0 || lens.k2 < 0)
        throw std::invalid_argument("Invalid viewer geometry; lens spacing must fit the screen");
    const auto outer = (width - spacing) / (2 * distance);
    const auto inner = spacing / (2 * distance);
    const auto top = (height / 2 - vertical_offset) / distance;
    const auto bottom = (height / 2 + vertical_offset) / distance;
    ViewerGeometry result;
    result.screen = {Projection{outer, inner, top, bottom},
                     Projection{inner, outer, top, bottom}};
    const auto extent = [&](double value, double limit) {
        const auto angle = std::min(std::atan(lens.distort(value)), limit * pi / 180) * fov_scale;
        if (!std::isfinite(angle) || angle >= std::atan(20.0))
            throw std::invalid_argument(
                "Viewer field of view is too wide; check measurements and distortion");
        return std::tan(angle);
    };
    for (size_t eye = 0; eye < 2; ++eye) {
        const auto &p = result.screen[eye];
        result.eyes[eye] = {extent(p.left, eye == 0 ? limits_deg.left : limits_deg.right),
                            extent(p.right, eye == 0 ? limits_deg.right : limits_deg.left),
                            extent(p.up, limits_deg.up), extent(p.down, limits_deg.down)};
        if (!p.valid() || !result.eyes[eye].valid())
            throw std::invalid_argument("Viewer field of view is outside supported limits");
    }
    return result;
}
} // namespace pocketvr
