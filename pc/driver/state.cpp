// SPDX-License-Identifier: Apache-2.0
#include "state.hpp"
#include <pocketvr/profiles.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace pocketvr::driver {
namespace {
nlohmann::json projection_description(const ViewerGeometry &geometry) {
    const auto extents = [](const Projection &p) {
        return nlohmann::json::array({p.left, p.right, p.up, p.down});
    };
    return {{"id", geometry.id},
            {"left", extents(geometry.eyes[0])},
            {"right", extents(geometry.eyes[1])},
            {"render_left", extents(guarded_projection(geometry.eyes[0]))},
            {"render_right", extents(guarded_projection(geometry.eyes[1]))},
            {"screen_left", extents(geometry.screen[0])},
            {"screen_right", extents(geometry.screen[1])}};
}
} // namespace
State::State(std::filesystem::path config)
    : encoded_dump(config.empty() ? win::appdata(true) / "captures" : config.parent_path() / "captures"),
      config_(config.empty() ? win::appdata() / "pocketvr.json" : std::move(config)) {
    if (std::filesystem::exists(config_)) {
        try {
            std::ifstream input(config_);
            // A file from an older version lacks newer keys; keep what it does hold.
            auto merged = settings_.values();
            merged.merge_patch(nlohmann::json::parse(input));
            settings_ = Settings(merged);
        } catch (const std::exception &e) {
            spdlog::warn("Invalid settings; using defaults without replacing original: {}",
                         e.what());
        }
    } else {
        const auto text = settings_.values().dump(2);
        win::atomic_write(config_,
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    }
    apply_settings();
}
void State::menu_text(std::vector<std::string> lines) {
    std::lock_guard lock(menu_mutex_);
    menu_lines_ = std::move(lines);
    ++menu_revision_;
}
std::pair<uint64_t, std::vector<std::string>> State::menu_snapshot() const {
    std::lock_guard lock(menu_mutex_);
    return {menu_revision_, menu_lines_};
}
nlohmann::json State::load_profiles() const {
    try {
        std::ifstream input(config_.parent_path() / "profiles.json");
        if (input) {
            auto saved = nlohmann::json::parse(input);
            if (saved.is_object())
                return saved;
        }
    } catch (const std::exception &e) {
        spdlog::warn("Ignoring unreadable saved profiles: {}", e.what());
    }
    return nlohmann::json::object();
}
void State::apply_settings() {
    encoded_dump.set_enabled(settings_.get<bool>("debug.dump_frames"));
    const auto preset=settings_.get<std::string>("input.mapping");
    auto mapping=input_preset(preset);
    if (preset=="custom") for (size_t i=0;i<mapping.size();++i)
        mapping[i]=input_action(settings_.get<std::string>("input.bind_"+std::string(input_buttons[i])));
    const auto old_mapping=input_mapping_.load();
    if (!old_mapping||*old_mapping!=mapping) {
        input_mapping_.store(std::make_shared<const InputMapping>(mapping));
        phone_input.release(); phone_recenter_held_.store(false);
    }
    auto viewer = settings_.viewer_geometry();
    const auto previous = viewer_.load();
    viewer.id = previous && previous->id != UINT32_MAX ? previous->id + 1 : 1;
    viewer_.store(std::make_shared<const ViewerGeometry>(viewer));
    ipd_m.store(settings_.get<double>("display.ipd_mm") / 1000);
    render_scale.store(settings_.get<double>("video.render_scale"));
    test_pattern.store(settings_.get<bool>("debug.test_pattern"));
    const auto &value = settings_.values().at("tracking").at("prediction_ms");
    prediction_ms.store(value.is_number() ? value.get<double>() : 40.0);
    target_lead_ms.store(settings_.get<double>("pacing.target_lead_ms"));
    eye_height_m.store(settings_.get<double>("tracking.eye_height_m"));
    spdlog::set_level(spdlog::level::from_str(settings_.get<std::string>("debug.log_level")));
    settings_changed.store(true);
}
Settings State::settings_copy() const {
    std::lock_guard lock(settings_mutex_);
    return settings_;
}
void State::phone_controls(const RawInput &input,uint64_t now) {
    std::unique_lock settings_guard(settings_mutex_,std::try_to_lock);
    if (!settings_guard.owns_lock()) { phone_input.release(); return; }
    const auto mapped=map_input(input,*input_mapping_.load());
    DesktopInput next;
    next.hands=mapped.hands; next.active=mapped.active; next.gamepad=mapped.gamepad; next.recenter=mapped.recenter;
    const bool armed=phone_input.submit(next,now);
    const bool held=phone_recenter_held_.exchange(mapped.recenter&&armed);
    if (armed&&mapped.recenter&&!held) {
        const auto pose=head.read();
        if (pose.timestamp_ns&&now>=pose.timestamp_ns&&now-pose.timestamp_ns<250'000'000) {
            const auto forward=pose.pose.orientation.rotate({0,0,-1});
            recenter_yaw.store(-std::atan2(-forward.x,-forward.z));
        }
    }
}
void State::record_frame(const wire::VideoHeader &frame) {
    std::lock_guard lock(history_mutex_);
    history_.put({frame.frame_id, frame.render_ns, frame.render_pose});
}
void State::route_haptic(TimedHaptic event) {
    if (haptic_owner.load() == 1) (void)desktop_haptics.push(event);
    else if (haptic_owner.load() == 2) (void)haptics.push(event);
}
nlohmann::json State::request(const nlohmann::json &input) {
    const auto method = input.at("method").get<std::string>();
    if (method == "debug.dump.status") return encoded_dump.status();
    if (method == "input.desktop") {
        const auto now = monotonic_ns();
        const auto armed = desktop_input.submit(input, now);
        const auto current = desktop_input.read(now);
        const bool allowed = armed && current.active && current.gamepad && session_connected.load() &&
            !gpu_lost.load() && !standby.load() && haptic_owner.load() == 1 && input.value("haptics", false);
        auto pulses = nlohmann::json::array();
        for (const auto &event : desktop_haptics.take(now, allowed))
            pulses.push_back({{"hand", event.pulse.hand}, {"duration_ms", event.pulse.duration_ms},
                {"amplitude", event.pulse.amplitude}, {"frequency", event.pulse.frequency},
                {"timestamp_pc_ns", event.timestamp_ns}});
        return {{"ok", true}, {"armed", armed}, {"rumble", std::move(pulses)}};
    }
    if (method == "status") {
        const auto p = head.read();
        const auto now = monotonic_ns();
        const bool active =
            p.timestamp_ns && now >= p.timestamp_ns && now - p.timestamp_ns < 250'000'000;
        const auto last_stream = last_stream_ns.load();
        return {{"ok", true},
                {"connected", active},
                {"state", gpu_lost.load() ? "GPU device lost; restart SteamVR" : active ? "tracking" : "waiting for phone"},
                {"gpu_lost", gpu_lost.load()},
                {"frames", frames.load()},
                {"display_hz", 60},
                {"session_connected", session_connected.load()},
                {"video_streaming", session_connected.load() && last_stream && now >= last_stream &&
                                        now - last_stream < 1'000'000'000}};
    }
    if (method == "settings.get") {
        std::lock_guard lock(settings_mutex_);
        return {{"ok", true},
                {"values", settings_.values()},
                {"schema", Settings::schema()},
                {"projection", projection_description(*viewer_.load())}};
    }
    if (method == "viewer.set") {
        std::lock_guard lock(settings_mutex_);
        auto next = settings_;
        next.set_viewer(input.at("values"));
        const auto text = next.values().dump(2);
        win::atomic_write(config_,
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        settings_ = std::move(next);
        apply_settings();
        return {{"ok", true},
                {"values", settings_.values()},
                {"viewer", settings_.viewer_values()},
                {"projection", projection_description(*viewer_.load())}};
    }
    if (method == "frame.pose") {
        std::optional<PoseRecord> record;
        {
            std::lock_guard lock(history_mutex_);
            record = history_.frame(input.at("frame_id").get<uint64_t>());
        }
        if (!record)
            throw std::invalid_argument("Frame pose is no longer in history");
        const auto &q = record->pose.orientation;
        return {{"ok", true},
                {"frame_id", record->frame_id},
                {"render_ns", record->timestamp_ns},
                {"orientation", {q.x, q.y, q.z, q.w}},
                {"position",
                 {record->pose.position.x, record->pose.position.y, record->pose.position.z}}};
    }
    if (method == "input.mapping.set") {
        std::lock_guard lock(settings_mutex_);
        const auto &values=input.at("values");
        if (!values.is_object()||values.size()!=17) throw std::invalid_argument("Mapping requires a preset and 16 bindings");
        auto next=settings_;
        next.set("input.mapping",values.at("mapping"));
        for (const auto name:input_buttons) next.set("input.bind_"+std::string(name),values.at("bind_"+std::string(name)));
        const auto text=next.values().dump(2);
        win::atomic_write(config_,std::span(reinterpret_cast<const uint8_t*>(text.data()),text.size()));
        settings_=std::move(next); apply_settings();
        return {{"ok",true},{"values",settings_.values()}};
    }
    if (method == "settings.set") {
        std::lock_guard lock(settings_mutex_);
        if (!Settings::runtime_supported(input.at("key").get<std::string>()))
            throw std::invalid_argument("This setting is reserved for an unfinished feature and cannot be changed at runtime");
        auto next = settings_;
        const auto key = input.at("key").get<std::string>();
        next.set(key, input.at("value"));
        // Lens measurements are only used by the manual profile.
        if (key.starts_with("lens.") && key != "lens.profile")
            next.set("lens.profile", std::string("manual"));
        const auto text = next.values().dump(2);
        win::atomic_write(config_,
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        settings_ = std::move(next);
        apply_settings();
        return {{"ok", true},
                {"values", settings_.values()},
                {"projection", projection_description(*viewer_.load())}};
    }
    if (method == "profiles.list") {
        std::lock_guard lock(settings_mutex_);
        auto built_in = nlohmann::json::array(), saved = nlohmann::json::array();
        for (const auto &[name, unused] : built_in_profiles().items()) {
            (void)unused;
            built_in.push_back(name);
        }
        const auto stored = load_profiles();
        for (const auto &[name, unused] : stored.items()) {
            (void)unused;
            saved.push_back(name);
        }
        return {{"ok", true}, {"built_in", built_in}, {"saved", saved}};
    }
    if (method == "profiles.save") {
        std::lock_guard lock(settings_mutex_);
        const auto name = input.at("name").get<std::string>();
        if (!valid_profile_name(name))
            throw std::invalid_argument("Choose a different profile name");
        auto saved = load_profiles();
        if (saved.size() >= 32 && !saved.contains(name))
            throw std::invalid_argument("Delete a profile before saving another");
        nlohmann::json values = nlohmann::json::object();
        for (const auto &[section, group] : settings_.values().items())
            if (profile_section(section))
                for (const auto &[field, value] : group.items())
                    values[section + "." + field] = value;
        saved[name] = std::move(values);
        const auto text = saved.dump(2);
        win::atomic_write(config_.parent_path() / "profiles.json",
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return {{"ok", true}};
    }
    if (method == "profiles.delete") {
        std::lock_guard lock(settings_mutex_);
        auto saved = load_profiles();
        saved.erase(input.at("name").get<std::string>());
        const auto text = saved.dump(2);
        win::atomic_write(config_.parent_path() / "profiles.json",
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return {{"ok", true}};
    }
    if (method == "profiles.apply") {
        std::lock_guard lock(settings_mutex_);
        const auto name = input.at("name").get<std::string>();
        const auto saved = load_profiles();
        const auto &values = built_in_profiles().contains(name) ? built_in_profiles().at(name) : saved.at(name);
        auto next = settings_;
        for (const auto &[key, value] : values.items())
            if (Settings::runtime_supported(key))
                next.set(key, value);
        const auto text = next.values().dump(2);
        win::atomic_write(config_,
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        settings_ = std::move(next);
        apply_settings();
        return {{"ok", true},
                {"values", settings_.values()},
                {"projection", projection_description(*viewer_.load())}};
    }
    if (method == "settings.reset") {
        // Everything returns to its default except what describes this setup:
        // the network ports and the lenses measured for the viewer.
        std::lock_guard lock(settings_mutex_);
        auto next = settings_;
        const Settings defaults;
        for (const auto &[section, spec] : Settings::schema().at("properties").items()) {
            if (section == "connection" || section == "lens")
                continue;
            for (const auto &[field, unused] : spec.at("properties").items()) {
                (void)unused;
                const auto key = section + "." + field;
                if (Settings::runtime_supported(key) && key != "display.ipd_mm")
                    next.set(key, defaults.values().at(section).at(field));
            }
        }
        const auto text = next.values().dump(2);
        win::atomic_write(config_,
                          std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        settings_ = std::move(next);
        apply_settings();
        return {{"ok", true}, {"values", settings_.values()}};
    }
    if (method == "steamvr.restart") {
        restart_direct.store(input.value("direct", false));
        restart_requested.store(true);
        return {{"ok", true}};
    }
    if (method == "bridge.status") {
        auto controllers = nlohmann::json::array();
        const auto now = monotonic_ns();
        for (size_t i = 0; i < 2; ++i) {
            const auto pose = controller_tracking[i].read();
            controllers.push_back({{"device", i + 1}, {"owned", controller_override[i].load()},
                {"fresh", pose.timestamp_ns && (pose.timestamp_ns > now || now - pose.timestamp_ns <= 250'000'000)},
                {"timestamp_pc_ns", pose.timestamp_ns}, {"space", pose.space ? "standing" : "tracking"},
                {"position", {pose.pose.position.x, pose.pose.position.y, pose.pose.position.z}},
                {"linear_velocity", {pose.linear_velocity.x, pose.linear_velocity.y, pose.linear_velocity.z}},
                {"angular_velocity", {pose.pose.angular_velocity.x, pose.pose.angular_velocity.y, pose.pose.angular_velocity.z}},
                {"orientation", {pose.pose.orientation.x, pose.pose.orientation.y, pose.pose.orientation.z, pose.pose.orientation.w}}});
        }
        return {{"ok", true}, {"controllers", controllers}, {"recenter_yaw_rad", recenter_yaw.load()},
            {"eye_height_m", eye_height_m.load()}};
    }
    if (method == "bridge.release") {
        const auto device = input.at("device").get<int>();
        if (device < 0 || device > 2) throw std::invalid_argument("Bridge device must be 0, 1 or 2");
        std::lock_guard lock(bridge_mutex_);
        if (!device) head.publish({});
        else {
            const auto index = static_cast<size_t>(device - 1);
            controller_override[index].store(false); controller_tracking[index].publish({});
            desktop_input.release(); phone_input.release();
        }
        return {{"ok", true}};
    }
    if (method == "bridge.pose") {
        const auto device = input.value("device", 0);
        if (device < 0 || device > 2) throw std::invalid_argument("Bridge device must be 0, 1 or 2");
        const auto q = input.at("orientation").get<std::array<double, 4>>();
        const auto w = input.value("angular_velocity", std::array<double, 3>{});
        const auto position = device ? input.at("position").get<std::array<double, 3>>() : input.value("position", std::array<double, 3>{});
        const auto velocity = input.value("linear_velocity", std::array<double, 3>{});
        const auto space = input.value("space", std::string("tracking"));
        if (space != "tracking" && space != "standing") throw std::invalid_argument("Unknown bridge coordinate space");
        if (!device && space != "tracking") throw std::invalid_argument("Standing space is supported for controllers only");
        if (device && !input.value("positioned", true)) throw std::invalid_argument("Controller bridge needs measured position");
        const Vec3 p{position[0], position[1], position[2]};
        if (!p.finite() || p.norm() > 1000) throw std::invalid_argument("Bridge position outside supported range");
        const auto now = monotonic_ns();
        const auto timestamp = input.value("timestamp_pc_ns", now);
        if ((timestamp > now && timestamp - now > 100'000'000) ||
            (timestamp <= now && now - timestamp > 1'000'000'000))
            throw std::invalid_argument("Tracker timestamp outside allowed interval");
        const PoseSnapshot next{{{position[0], position[1], position[2]},
                       {q[0], q[1], q[2], q[3]},
                       {w[0], w[1], w[2]}},
                      timestamp,
                      static_cast<uint8_t>(device || input.value("positioned", false) ? 2 : 1),
                      {velocity[0], velocity[1], velocity[2]}, static_cast<uint8_t>(space == "standing")};
        std::lock_guard lock(bridge_mutex_);
        if (!device) head.publish(next);
        else {
            const auto index = static_cast<size_t>(device - 1);
            const auto previous = controller_tracking[index].read();
            if (previous.timestamp_ns && timestamp <= previous.timestamp_ns) throw std::invalid_argument("Controller bridge timestamp did not advance");
            if (controller_override[index].load() && previous.space != next.space)
                throw std::invalid_argument("Release controller bridge before changing coordinate space");
            controller_tracking[index].publish(next);
            if (!controller_override[index].exchange(true)) { desktop_input.release(); phone_input.release(); }
        }
        return {{"ok", true}};
    }
    if (method == "recenter") {
        const auto s = head.read();
        if (!s.timestamp_ns)
            throw std::invalid_argument("No tracking pose to recenter");
        const auto forward = s.pose.orientation.rotate({0, 0, -1});
        recenter_yaw.store(-std::atan2(-forward.x, -forward.z));
        return {{"ok", true}};
    }
    throw std::invalid_argument("Unknown RPC method");
}
} // namespace pocketvr::driver
