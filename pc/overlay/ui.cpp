// SPDX-License-Identifier: Apache-2.0
#include "ui.hpp"
#include "input.hpp"
#include <pocketvr/settings.hpp>
#include <imgui.h>

namespace pocketvr::dashboard {
namespace {
std::string label(const Json &value) { return value.is_string() ? value.get<std::string>() : value.dump(); }
void graph(const char *name, const std::vector<float> &values) {
    ImGui::PushID(name);
    if (values.empty()) { ImGui::TextUnformatted(name); ImGui::TextDisabled("Unavailable / waiting for samples"); ImGui::Dummy({0, 125}); }
    else {
        ImGui::Text("%s: %.2f", name, static_cast<double>(values.back()));
        ImGui::PlotLines("##plot", values.data(), static_cast<int>(values.size()), 0, nullptr,
                          0, FLT_MAX, {ImGui::GetContentRegionAvail().x - 10, 150});
    }
    ImGui::PopID();
}
}
void Ui::send(const UiActions &actions, Json command) {
    feedback_ = actions.command(std::move(command)) ? "Request queued; waiting for driver" : "Driver unavailable, stale or request queue full";
}
void Ui::section(const char *name, const Json &values, const UiActions &actions) {
    ImGui::SeparatorText(name);
    const auto &properties = Settings::schema().at("properties").at(name).at("properties");
    for (const auto &[field, spec] : properties.items()) {
        const auto key = std::string(name) + "." + field;
        auto [found, inserted] = drafts_.try_emplace(key, Draft{values.at(name).at(field)});
        auto &draft = found->second;
        if (!inserted && !draft.dirty) draft.value = values.at(name).at(field);
        const bool supported = Settings::runtime_supported(key);
        ImGui::PushID(key.c_str());
        ImGui::BeginDisabled(!supported);
        auto display_name = field;
        std::replace(display_name.begin(), display_name.end(), '_', ' ');
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(display_name.c_str());
        ImGui::SameLine(350); ImGui::SetNextItemWidth(570);
        bool changed = false;
        if (spec.contains("enum")) {
            if (ImGui::BeginCombo("##value", label(draft.value).c_str())) {
                for (const auto &choice : spec.at("enum")) {
                    if (key == "lens.profile" && choice == "qr") continue; // Phone QR import saves calibrated geometry.
                    if (ImGui::Selectable(label(choice).c_str(), choice == draft.value)) { draft.value = choice; changed = true; }
                }
                ImGui::EndCombo();
            }
        } else if (draft.value.is_boolean()) {
            auto value = draft.value.get<bool>();
            if (ImGui::Checkbox("##value", &value)) { draft.value = value; changed = true; }
        } else if (draft.value.is_number() || key == "tracking.prediction_ms") {
            if (key == "tracking.prediction_ms") {
                bool automatic = draft.value.is_string();
                if (ImGui::Checkbox("Automatic", &automatic)) { draft.value = automatic ? Json("auto") : Json(40.0); changed = true; }
                if (!automatic) { ImGui::SameLine(); ImGui::SetNextItemWidth(420); }
            }
            if (draft.value.is_number()) {
                double value = draft.value.get<double>();
                const double low = spec.value("minimum", 0.0), high = spec.value("maximum", 80.0);
                const bool integer = spec.value("type", std::string{}) == "integer";
                if (ImGui::SliderScalar("##value", ImGuiDataType_Double, &value, &low, &high, integer ? "%.0f" : "%.4g")) {
                    draft.value = integer ? Json(static_cast<int64_t>(std::round(value))) : Json(value);
                    changed = true;
                }
            }
        } else ImGui::TextDisabled("%s", label(draft.value).c_str());
        draft.dirty = draft.dirty || changed;
        ImGui::SameLine(960);
        ImGui::BeginDisabled(!draft.dirty);
        if (ImGui::Button("Apply", {110, 0})) {
            try {
                auto candidate = Settings(values);
                candidate.set(key, draft.value);
                send(actions, {{"method", "settings.set"}, {"key", key}, {"value", draft.value}});
                // Keep the draft until the driver echoes it; never claim optimistic success.
            } catch (const std::exception &error) { feedback_ = error.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", {100, 0})) { draft.value = values.at(name).at(field); draft.dirty = false; }
        ImGui::EndDisabled(); ImGui::EndDisabled();
        if (draft.dirty && draft.value == values.at(name).at(field)) draft.dirty = false;
        if (!supported) ImGui::TextDisabled("Runtime control for this reserved setting is not implemented.");
        ImGui::PopID();
    }
}
void Ui::status(const Snapshot &snapshot, const UiActions &actions) {
    if (!snapshot.online()) { ImGui::TextWrapped("Start SteamVR with the PocketVR driver registered. This panel retries automatically."); return; }
    ImGui::TextWrapped("%s", snapshot.data.at("status").value("state", std::string("Unknown")).c_str());
    const auto session = snapshot.data.at("network").value("session", Json{});
    if (session.is_object()) {
        ImGui::Text("Encoder: %s | Transport: %s", session.value("encoder_backend", std::string("Unavailable")).c_str(), session.value("transport", std::string("Unknown")).c_str());
        const auto quality = session.value("adaptation", Json::object());
        if (quality.contains("encoder_accepted_bitrate_bps"))
            ImGui::Text("Encoder bitrate setting: %.1f Mbps", quality.at("encoder_accepted_bitrate_bps").get<double>() / 1e6);
        else ImGui::TextDisabled("Encoder bitrate setting unavailable");
        ImGui::TextWrapped("%s", quality.value("reason", std::string{}).c_str());
        const auto phone = session.value("phone_stats", Json::object());
        if (phone.value("available", false)) {
            const auto &v = phone.at("values");
            ImGui::Text("Phone render CPU: %.2f ms | Display: %.1f FPS", v.value("render_cpu_ms", 0.0), v.value("display_fps", 0.0));
        } else ImGui::TextDisabled("Fresh phone diagnostics unavailable");
    } else ImGui::TextUnformatted("No authenticated phone session");
    ImGui::TextDisabled("Encode time, one-way latency and network loss are not reported by this endpoint.");
    if (ImGui::Button("Recenter", {180, 45})) send(actions, {{"method", "recenter"}});
    ImGui::SameLine(); if (ImGui::Button("Restart SteamVR", {180, 45})) send(actions, {{"method", "steamvr.restart"}});
    ImGui::SameLine(); if (ImGui::Button("Restore defaults", {180, 45})) send(actions, {{"method", "settings.reset"}});
    if (ImGui::BeginTable("history", 2)) {
        ImGui::TableNextColumn(); graph("Sent bytes rate (Mbps)", history_.bitrate);
        ImGui::TableNextColumn(); graph("Phone decode p95 (ms)", history_.decode);
        ImGui::TableNextColumn(); graph("Phone video FPS", history_.fps);
        ImGui::TableNextColumn(); graph("PC frame drop fraction (%)", history_.drops);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Up to 120 polled samples. PC frame drops are not network packet loss.");
}
void Ui::pairing(const Snapshot &snapshot, const UiActions &) {
    if (!snapshot.online()) return;
    ImGui::SeparatorText("LAN connection QR");
    std::vector<Endpoint> endpoints;
    for (const auto &e : snapshot.data.at("network").value("endpoints", Json::array())) {
        try {
            const auto p = Endpoint::parse(e.at("ip"), e.at("control_port"));
            const auto a = p.address;
            if (p.port && ((a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8)) endpoints.push_back(p);
        }
        catch (const std::exception &) {}
    }
    if (endpoints.empty()) { qr_.reset(); ImGui::TextDisabled("No private network found. Connect the phone by USB."); return; }
    endpoint_ = std::clamp(endpoint_, 0, static_cast<int>(endpoints.size()) - 1);
    ImGui::TextUnformatted("PC interface");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10);
    if (ImGui::BeginCombo("##PCinterface", endpoints[static_cast<size_t>(endpoint_)].ip().c_str())) {
        for (size_t i = 0; i < endpoints.size(); ++i)
            if (ImGui::Selectable(endpoints[i].ip().c_str(), endpoint_ == static_cast<int>(i))) endpoint_ = static_cast<int>(i);
        ImGui::EndCombo();
    }
    const auto &endpoint = endpoints[static_cast<size_t>(endpoint_)];
    const auto key = endpoint.ip() + ":" + std::to_string(endpoint.port);
    if (key != qr_key_ || !qr_) { qr_ = win::connection_qr(endpoint); qr_key_ = key; }
    const auto origin = ImGui::GetCursorScreenPos();
    const float scale = 7, side = (qr_->size + 8) * scale;
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + side, origin.y + side}, IM_COL32_WHITE);
    for (int y = 0; y < qr_->size; ++y) for (int x = 0; x < qr_->size; ++x)
        if (qr_->dark[static_cast<size_t>(y) * qr_->size + x])
            draw->AddRectFilled({origin.x + (x + 4) * scale, origin.y + (y + 4) * scale},
                {origin.x + (x + 5) * scale, origin.y + (y + 5) * scale}, IM_COL32_BLACK);
    ImGui::Dummy({side, side}); ImGui::TextWrapped("%s", qr_->text.c_str());
}
void Ui::draw(const Snapshot &snapshot, const UiActions &actions) {
    history_.update(snapshot);
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({static_cast<float>(width), static_cast<float>(height)});
    ImGui::Begin("PocketVR", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(fixture ? "PocketVR - OFFLINE TEST FIXTURE" : "PocketVR dashboard");
    ImGui::TextColored(snapshot.online() ? ImVec4{.3f, 1, .65f, 1} : ImVec4{1, .7f, .25f, 1}, "%s",
        snapshot.online() ? "Driver connected" : snapshot.error.empty() ? "Driver status stale; controls disabled" : snapshot.error.c_str());
    constexpr const char *pages[]{"Status", "Video", "Display / lens", "Tracking", "Input", "Connection", "About / logs"};
    for (int i = 0; i < 7; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Selectable(pages[i], page == i, 0, {i == 2 ? 195.f : i == 6 ? 185.f : 140.f, 40})) page = i;
    }
    ImGui::Separator();
    ImGui::BeginChild(("page" + std::to_string(page)).c_str(), {0, -82});
    const auto values = snapshot.online() ? snapshot.data.at("settings").at("values") : Settings().values();
    ImGui::BeginDisabled(!snapshot.online());
    switch (page) {
    case 0: status(snapshot, actions); break;
    case 1:
        ImGui::TextWrapped("Codec, preset, FPS, render scale and FEC changes require a new phone connection. Bitrate changes depend on encoder support.");
        section("video", values, actions); section("fec", values, actions); break;
    case 2:
        ImGui::TextWrapped("IPD is pupil spacing, separate from lens spacing. Select manual profile for lens measurements, or import viewer QR on the phone. A saved phone profile can override calibration when connecting.");
        section("display", values, actions); section("lens", values, actions); section("reprojection", values, actions); break;
    case 3:
        if (ImGui::Button("Recenter head tracking", {360, 45})) send(actions, {{"method", "recenter"}});
        section("tracking", values, actions); section("pacing", values, actions); section("power", values, actions); break;
    case 4:
        ImGui::TextWrapped("Desktop input takes priority while its helper is focused. Phone gamepad, touch zones and volume buttons use the selected preset below. Choose custom to edit bind_* actions; changes require releasing controls. Haptics and second-phone control are separate paths.");
        section("input", values, actions); section("ui", values, actions); break;
    case 5: pairing(snapshot, actions); break;
    case 6:
        ImGui::EndDisabled(); // Local diagnostics remain available without the driver.
        ImGui::TextWrapped("PocketVR - Apache-2.0. Local PC/phone VR streaming; audio stays on the PC. No ARCore or NVENC in v1. Notices are shipped with the package.");
        if (ImGui::Button("Refresh filtered log tails", {430, 45})) feedback_ = actions.logs() ? "Log request queued" : "Driver unavailable or queue full";
        ImGui::SameLine();
        if (ImGui::Button("Export diagnostics ZIP", {430, 45})) feedback_ = actions.bundle() ? "Export queued" : "Driver unavailable or queue full";
        if (ImGui::Button("Open logs folder on desktop", {430, 45})) {
            try { actions.open_folder(false); } catch (const std::exception &error) { feedback_ = error.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open diagnostics folder", {430, 45})) {
            try { actions.open_folder(true); } catch (const std::exception &error) { feedback_ = error.what(); }
        }
        ImGui::TextWrapped("Exports stay local. Review addresses, settings and filtered logs before sharing. Folder windows appear on the desktop.");
        ImGui::BeginChild("logs", {0, 340}, ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(snapshot.logs.empty() ? "Choose Refresh to load bounded, filtered tails of known PocketVR logs." : snapshot.logs.c_str());
        ImGui::EndChild();
        ImGui::BeginDisabled(!snapshot.online());
        section("debug", values, actions); break;
    }
    ImGui::EndDisabled(); ImGui::EndChild();
    ImGui::Separator(); ImGui::TextWrapped("%s", feedback_.c_str()); ImGui::TextWrapped("%s", snapshot.result.c_str());
    ImGui::End();
}
Snapshot fixture_snapshot() {
    Snapshot s;
    s.received = Clock::now(); s.revision = 1;
    s.data = {{"status", {{"state", "Synthetic preview - no live VR measurements"}}},
        {"settings", {{"values", Settings().values()}}},
        {"network", {{"endpoints", Json::array({{{"ip", "192.168.42.7"}, {"control_port", 12345}}})},
            {"session", {{"sent_bytes", 1000}, {"encoded_frames", 60}, {"dropped_frames", 1},
                {"encoder_backend", "Synthetic fixture"}, {"transport", "fixture"},
                {"adaptation", {{"encoder_accepted_bitrate_bps", 60e6}, {"reason", "Synthetic fixture; not a performance result"}}},
                {"phone_stats", {{"available", true}, {"values", {{"decode_samples", 12}, {"decode_p95_ms", 7.5}, {"render_cpu_ms", 1.2}, {"video_fps", 60.0}, {"display_fps", 60.0}}}}}}}}},
        {"peers", Json::array()}};
    return s;
}
} // namespace pocketvr::dashboard
