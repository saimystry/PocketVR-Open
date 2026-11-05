// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pocketvr {
// Starting points, not measured results. Each maps full setting keys to values.
inline const nlohmann::json &built_in_profiles() {
    static const nlohmann::json profiles{
        {"War Thunder",
         {{"video.codec", "hevc"}, {"video.preset", "quality"}, {"video.bitrate_mbps", 80},
          {"video.min_bitrate_mbps", 30}, {"video.max_bitrate_mbps", 120}, {"video.adaptive", true},
          {"reprojection.enabled", true}}},
        {"Lowest delay",
         {{"video.codec", "h264"}, {"video.preset", "fast"}, {"video.bitrate_mbps", 40},
          {"video.render_scale", 1}, {"video.send_spread_pct", 0}}},
        {"Best picture",
         {{"video.codec", "hevc"}, {"video.preset", "quality"}, {"video.bitrate_mbps", 100},
          {"video.max_bitrate_mbps", 150}, {"video.render_scale", 1.2}}}};
    return profiles;
}
// What a saved profile captures: the picture and lenses, not ports or logging.
inline bool profile_section(std::string_view section) {
    return section == "video" || section == "fec" || section == "display" || section == "lens" ||
           section == "reprojection";
}
inline bool valid_profile_name(const std::string &name) {
    return !name.empty() && name.size() <= 40 && !built_in_profiles().contains(name);
}
// A picture for judging the encoder: a slow sky gradient shows banding and blocks, and
// fine detail shows sharpness. Each eye gets the same image. BGRA, top row first.
inline std::vector<uint8_t> test_pattern_pixels(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const auto eye_width = width / 2;
    const auto sky_end = height * 3 / 5;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto eye_x = x % eye_width;
            uint8_t red, green, blue;
            if (y < sky_end) {
                const auto t = static_cast<double>(y) / sky_end;
                red = static_cast<uint8_t>(40 + 150 * t);
                green = static_cast<uint8_t>(90 + 125 * t);
                blue = static_cast<uint8_t>(160 + 80 * t);
            } else {
                // Lines get finer from left to right: 8, 4, 2 and 1 pixels wide.
                const auto band = eye_x * 4 / eye_width;
                const auto line = 8U >> band;
                const bool lit = (((y - sky_end) / line) + (eye_x / line)) % 2 == 0;
                red = green = blue = lit ? 235 : 20;
            }
            auto *pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 255;
        }
    return pixels;
}
enum class MenuInput { up, down, left, right };
struct MenuChange {
    std::string key; // Empty for an action.
    double value{};
    bool recenter{}, close{};
};
// The small in-headset menu. It only decides what changes; the driver applies it.
class VrMenu final {
    struct Item {
        const char *label, *section, *name, *unit;
        double step, scale;
    };
    static constexpr std::array<Item, 7> items_{{
        {"EYE SIZE", "display", "eye_size", "%", 0.05, 100},
        {"FIELD OF VIEW", "display", "fov_scale", "%", 0.02, 100},
        {"BITRATE", "video", "bitrate_mbps", " MBPS", 5, 1},
        {"ANTIALIASING", "video", "render_scale", "%", 0.05, 100},
        {"EDGE DARKENING", "display", "vignette_strength", "%", 0.05, 100},
        {"RECENTER VIEW", nullptr, nullptr, "", 0, 0},
        {"CLOSE MENU", nullptr, nullptr, "", 0, 0},
    }};
    size_t selected_{};
    bool open_{};

  public:
    bool open() const { return open_; }
    void toggle() {
        open_ = !open_;
        selected_ = 0;
    }
    std::optional<MenuChange> input(MenuInput key, const nlohmann::json &values, const nlohmann::json &schema) {
        if (!open_)
            return {};
        if (key == MenuInput::up || key == MenuInput::down) {
            selected_ = (selected_ + (key == MenuInput::down ? 1 : items_.size() - 1)) % items_.size();
            return {};
        }
        const auto &item = items_[selected_];
        if (!item.section) {
            if (key != MenuInput::right)
                return {};
            MenuChange change;
            change.recenter = selected_ == 5;
            change.close = selected_ == 6;
            if (change.close)
                open_ = false;
            return change;
        }
        const auto &rule = schema.at("properties").at(item.section).at("properties").at(item.name);
        const auto now = values.at(item.section).at(item.name).get<double>();
        const auto next = now + (key == MenuInput::right ? item.step : -item.step);
        const auto low = rule.at("minimum").get<double>(), high = rule.at("maximum").get<double>();
        return MenuChange{std::string(item.section) + "." + item.name, next < low ? low : next > high ? high : next};
    }
    // A title and the three items around the selection, in the headset font.
    std::vector<std::string> lines(const nlohmann::json &values) const {
        if (!open_)
            return {};
        std::vector<std::string> result{"POCKETVR MENU"};
        const size_t first = selected_ == 0 ? 0 : selected_ + 1 >= items_.size() ? items_.size() - 3 : selected_ - 1;
        for (size_t i = first; i < first + 3; ++i) {
            const auto &item = items_[i];
            auto text = std::string(i == selected_ ? "> " : "  ") + item.label;
            if (item.section)
                text += " " + std::to_string(static_cast<int>(
                                  values.at(item.section).at(item.name).get<double>() * item.scale + 0.5)) + item.unit;
            result.push_back(std::move(text));
        }
        return result;
    }
};
} // namespace pocketvr
