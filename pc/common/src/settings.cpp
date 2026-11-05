// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/settings.hpp>
#include <pocketvr/generated_settings.hpp>
#include <cmath>

namespace pocketvr {
bool Settings::runtime_supported(const std::string &key) {
    return key != "connection.mode" &&
           key != "input.recenter_hotkey";
}
const nlohmann::json &Settings::schema() {
    static const auto catalog = nlohmann::json::parse(generated::settings_schema);
    return catalog;
}
static bool matches(const nlohmann::json &value, const nlohmann::json &spec) {
    if (spec.contains("anyOf")) {
        for (const auto &choice : spec.at("anyOf"))
            if (matches(value, choice))
                return true;
        return false;
    }
    const auto type = spec.at("type").get<std::string>();
    if (type == "object") {
        if (!value.is_object())
            return false;
        const auto &properties = spec.at("properties");
        for (const auto &key : spec.at("required"))
            if (!value.contains(key.get<std::string>()))
                return false;
        for (const auto &[key, item] : value.items())
            if (!properties.contains(key) || !matches(item, properties.at(key)))
                return false;
        return true;
    }
    if (type == "boolean" && !value.is_boolean())
        return false;
    if (type == "string" && !value.is_string())
        return false;
    if (type == "integer" && !value.is_number_integer())
        return false;
    if (type == "number" && !value.is_number())
        return false;
    if (value.is_number()) {
        const double v = value.get<double>();
        if (!std::isfinite(v) ||
            (spec.contains("minimum") && v < spec.at("minimum").get<double>()) ||
            (spec.contains("maximum") && v > spec.at("maximum").get<double>()))
            return false;
    }
    if (spec.contains("enum") &&
        std::find(spec.at("enum").begin(), spec.at("enum").end(), value) == spec.at("enum").end())
        return false;
    if (value.is_string() && spec.contains("maxLength") &&
        value.get_ref<const std::string &>().size() > spec.at("maxLength").get<size_t>())
        return false;
    return type == "boolean" || type == "string" || type == "integer" || type == "number";
}
void Settings::validate(const nlohmann::json &values) {
    if (!matches(values, schema()))
        throw std::invalid_argument("Settings violate the PocketVR v1 schema");
    const auto &video = values.at("video");
    if (video.at("min_bitrate_mbps").get<double>() > video.at("max_bitrate_mbps").get<double>())
        throw std::invalid_argument("Minimum bitrate exceeds maximum bitrate");
    const auto &lens = values.at("lens");
    if (lens.at("profile") == "manual")
        (void)calibrated_viewer(lens.at("screen_width_m"), lens.at("screen_height_m"),
                                lens.at("inter_lens_distance_m"), lens.at("screen_to_lens_m"),
                                {lens.at("k1"), lens.at("k2")},
                                values.at("display").at("fov_scale"), lens.at("vertical_offset_m"),
                                {lens.at("fov_left_deg"), lens.at("fov_right_deg"),
                                 lens.at("fov_top_deg"), lens.at("fov_bottom_deg")});
}
Settings::Settings() : values_(nlohmann::json::parse(generated::defaults)) {
    validate(values_);
}
Settings::Settings(nlohmann::json values) : values_(std::move(values)) {
    if (values_.contains("input")&&values_["input"].is_object())
        for (const auto &[key,spec]:schema().at("properties").at("input").at("properties").items())
            if (key.starts_with("bind_")&&!values_["input"].contains(key)) values_["input"][key]=spec.at("default");
    // Migrate newly introduced geometry/eye-height fields. Missing legacy
    // sections/keys and unknown fields still fail schema validation.
    if (values_.contains("lens") && values_["lens"].is_object()) {
        for (const auto *key : {"inter_lens_distance_m", "screen_width_m", "screen_height_m",
                                "vertical_offset_m", "fov_left_deg", "fov_right_deg",
                                "fov_bottom_deg", "fov_top_deg"})
            if (!values_["lens"].contains(key))
                values_["lens"][key] =
                    schema().at("properties").at("lens").at("properties").at(key).at("default");
    }
    if (values_.contains("tracking") && values_["tracking"].is_object() &&
        !values_["tracking"].contains("eye_height_m"))
        values_["tracking"]["eye_height_m"] =
            schema().at("properties").at("tracking").at("properties").at("eye_height_m").at("default");
    validate(values_);
}
ViewerGeometry Settings::viewer_geometry() const {
    ViewerGeometry geometry;
    geometry.ipd_m = get<double>("display.ipd_mm") / 1000;
    if (get<std::string>("lens.profile") == "manual") {
        geometry = calibrated_viewer(
            get<double>("lens.screen_width_m"), get<double>("lens.screen_height_m"),
            get<double>("lens.inter_lens_distance_m"), get<double>("lens.screen_to_lens_m"),
            {get<double>("lens.k1"), get<double>("lens.k2")}, get<double>("display.fov_scale"),
            get<double>("lens.vertical_offset_m"),
            {get<double>("lens.fov_left_deg"), get<double>("lens.fov_right_deg"),
             get<double>("lens.fov_top_deg"), get<double>("lens.fov_bottom_deg")});
        geometry.ipd_m = get<double>("display.ipd_mm") / 1000;
        return geometry;
    }
    const auto extent = std::tan(pi / 4 * get<double>("display.fov_scale"));
    geometry.screen.fill({extent, extent, extent, extent});
    geometry.eyes = geometry.screen;
    return geometry;
}
nlohmann::json Settings::viewer_values() const {
    auto result = values_.at("lens");
    result.erase("profile");
    result.erase("distortion_tweak");
    result["ipd_mm"] = get<double>("display.ipd_mm");
    result["fov_scale"] = get<double>("display.fov_scale");
    return result;
}
void Settings::set_viewer(const nlohmann::json &values) {
    const auto expected = viewer_values();
    auto complete = values;
    // Old saved calibrations remain valid; only new optical fields may be absent.
    for (const auto *key : {"vertical_offset_m", "fov_left_deg", "fov_right_deg",
                            "fov_bottom_deg", "fov_top_deg"})
        if (complete.is_object() && !complete.contains(key))
            complete[key] = schema().at("properties").at("lens").at("properties").at(key).at("default");
    if (!complete.is_object() || complete.size() != expected.size())
        throw std::invalid_argument("Expected a complete manual viewer calibration");
    auto next = values_;
    for (const auto &[key, unused] : expected.items()) {
        (void)unused;
        if (!complete.contains(key))
            throw std::invalid_argument("Missing viewer field: " + key);
        next[key == "ipd_mm" || key == "fov_scale" ? "display" : "lens"][key] = complete.at(key);
    }
    next["lens"]["profile"] = "manual";
    next["lens"]["distortion_tweak"] = 0;
    validate(next);
    values_ = std::move(next);
}
nlohmann::json::json_pointer Settings::pointer(const std::string &key) {
    const auto dot = key.find('.');
    if (dot == std::string::npos || key.find('.', dot + 1) != std::string::npos ||
        key.find_first_of("/~") != std::string::npos)
        throw std::invalid_argument("Expected section.key setting");
    return nlohmann::json::json_pointer("/" + key.substr(0, dot) + "/" + key.substr(dot + 1));
}
void Settings::set(const std::string &key, const nlohmann::json &value) {
    auto copy = values_;
    const auto path = pointer(key);
    if (!copy.contains(path))
        throw std::invalid_argument("Unknown setting");
    copy[path] = value;
    validate(copy);
    values_ = std::move(copy);
}
} // namespace pocketvr
