// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openvr.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace pocketvr::dashboard {
inline constexpr int width = 1280, height = 900;
struct InputAction {
    enum Kind { move, button, wheel, focus, quit } kind;
    float x{}, y{};
    int index{};
    bool down{};
};
class Pointer {
    std::array<bool, 3> held_{};
  public:
    using Sink = std::function<void(InputAction)>;
    void reset(const Sink &sink) {
        for (int i = 0; i < 3; ++i) {
            if (held_[static_cast<size_t>(i)]) sink({InputAction::button, 0, 0, i, false});
        }
        held_.fill(false);
        sink({InputAction::move, -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
        sink({InputAction::focus, 0, 0, 0, false});
    }
    void event(const vr::VREvent_t &event, const Sink &sink) {
        const auto type = event.eventType;
        if (type == vr::VREvent_OverlayHidden || type == vr::VREvent_FocusLeave) { reset(sink); return; }
        if (type == vr::VREvent_OverlayShown || type == vr::VREvent_FocusEnter) {
            sink({InputAction::focus, 0, 0, 0, true}); return;
        }
        if (type == vr::VREvent_Quit) { reset(sink); sink({InputAction::quit}); return; }
        if (type == vr::VREvent_MouseMove || type == vr::VREvent_MouseButtonDown || type == vr::VREvent_MouseButtonUp) {
            const auto &m = event.data.mouse;
            const bool valid = std::isfinite(m.x) && std::isfinite(m.y) && m.x >= 0 && m.x < width && m.y >= 0 && m.y < height;
            if (type == vr::VREvent_MouseButtonDown && !valid) return;
            if (valid) sink({InputAction::move, m.x, static_cast<float>(height) - m.y});
            else sink({InputAction::move, -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
            if (type == vr::VREvent_MouseMove) return;
            const int index = m.button == vr::VRMouseButton_Left ? 0 : m.button == vr::VRMouseButton_Right ? 1 : m.button == vr::VRMouseButton_Middle ? 2 : -1;
            if (index < 0) return;
            const bool down = type == vr::VREvent_MouseButtonDown;
            held_[static_cast<size_t>(index)] = down;
            sink({InputAction::button, 0, 0, index, down});
        } else if (type == vr::VREvent_ScrollDiscrete) {
            const auto &s = event.data.scroll;
            if (std::isfinite(s.xdelta) && std::isfinite(s.ydelta))
                sink({InputAction::wheel, std::clamp(s.xdelta, -10.f, 10.f), std::clamp(s.ydelta, -10.f, 10.f)});
        }
    }
};
} // namespace pocketvr::dashboard
