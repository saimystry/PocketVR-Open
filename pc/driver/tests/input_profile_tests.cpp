// SPDX-License-Identifier: Apache-2.0
// Validates the SteamVR input profile and binding files that ship in
// pc/driver/resources. SteamVR silently ignores a malformed or misspelled
// binding, so a typo in these files would leave a controller that appears to
// work but never delivers a button press. These tests fail loudly instead.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

// The OpenVR input components pc/driver/input.cpp creates for the virtual
// controller. A profile capability that is not backed by one of these can
// never carry a value, so the binding would look configured but do nothing.
constexpr std::array driver_components{
    "/input/trigger/click", "/input/trigger/value",
    "/input/grip/click", "/input/application_menu/click", "/input/system/click",
    "/input/trackpad/click", "/input/trackpad/touch",
    "/input/trackpad/x", "/input/trackpad/y",
    "/output/haptic"};

json load(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.good()) << "missing resource file: " << path.string();
    json document{};
    EXPECT_NO_THROW(stream >> document) << "invalid JSON: " << path.string();
    return document;
}

std::vector<char> read_bytes(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>());
}

// Resolves a "{pocketvr}/..." resource token against the driver resource root.
fs::path resolve(const fs::path &root, const std::string &reference) {
    constexpr std::string_view token = "{pocketvr}/";
    if (!reference.starts_with(token))
        return {};
    return root / fs::path(reference.substr(token.size()));
}

// The OpenVR input components a declared profile source needs for the
// capabilities it advertises.
std::vector<std::string> required_components(const std::string &source, const json &spec) {
    std::vector<std::string> result;
    const auto type = spec.at("type").get<std::string>();
    if (type == "pose") return result; // Poses come from the tracked device, not an input component.
    if (type == "vibration") {
        result.push_back(source);
        return result;
    }
    if (spec.value("click", false)) result.push_back(source + "/click");
    if (spec.value("touch", false)) result.push_back(source + "/touch");
    if (spec.value("value", false)) result.push_back(source + "/value");
    if (type == "trackpad") {
        result.push_back(source + "/x");
        result.push_back(source + "/y");
    }
    return result;
}

class InputProfileTest : public testing::Test {
  protected:
    fs::path root = POCKETVR_TEST_RESOURCE_ROOT;
    fs::path input_dir = root / "input";
    json profile = load(input_dir / "pocketvr_controller_profile.json");
    json sources = profile.at("input_source");

    // Every source path used by a binding, paired with the action set it belongs to.
    std::vector<std::pair<std::string, json>> binding_sources(const json &binding) const {
        std::vector<std::pair<std::string, json>> result;
        for (const auto &[action, set] : binding.at("bindings").items()) {
            if (!set.contains("sources")) continue;
            for (const auto &source : set.at("sources"))
                result.emplace_back(action, source);
        }
        return result;
    }

    // Checks one binding document against the profile that owns it.
    void expect_binding_is_consistent(const json &binding, const std::string &label) const {
        ASSERT_TRUE(binding.contains("controller_type")) << label;
        EXPECT_EQ(binding.at("controller_type").get<std::string>(),
                  profile.at("controller_type").get<std::string>()) << label;
        for (const auto &[action, source] : binding_sources(binding)) {
            const auto path = source.at("path").get<std::string>();            ASSERT_TRUE(path.starts_with("/user/hand/")) << label << " " << path;
            const auto hand = path.substr(11, path.find('/', 11) - 11);
            ASSERT_TRUE(hand == "left" || hand == "right") << label << " " << path;
            const auto source_path = path.substr(path.find('/', 11));
            ASSERT_TRUE(sources.contains(source_path))
                << label << " uses " << source_path << " which the profile does not declare";
            const auto &spec = sources.at(source_path);
            const auto type = spec.at("type").get<std::string>();
            const auto mode = source.at("mode").get<std::string>();
            if (mode == "trackpad")
                EXPECT_EQ(type, "trackpad") << label << " " << action << " " << path;
            if (mode == "trigger")
                EXPECT_EQ(type, "trigger") << label << " " << action << " " << path;
            if (mode == "button")
                EXPECT_TRUE(type == "button" || type == "trigger")
                    << label << " " << action << " " << path;
            for (const auto &[input, mapping] : source.at("inputs").items()) {
                EXPECT_TRUE(mapping.contains("output")) << label << " " << path << " " << input;
                if (input == "click")
                    EXPECT_TRUE(spec.value("click", false)) << label << " " << path;
                if (input == "pull") {
                    EXPECT_TRUE(spec.value("value", false)) << label << " " << path;
                    EXPECT_EQ(type, "trigger") << label << " " << path;
                }
                if (input == "position")
                    EXPECT_TRUE(type == "trackpad" || type == "joystick")
                        << label << " " << path;
                if (input == "touch") {
                    // SteamVR synthesizes analog trigger touch from the trigger
                    // value, exactly as the shipped Vive profile does, so a
                    // trigger source needs no separate touch capability.
                    const bool analog_trigger = type == "trigger" && spec.value("value", false);
                    EXPECT_TRUE(spec.value("touch", false) || analog_trigger)
                        << label << " " << path;
                }
                if (input == "double" || input == "held" || input == "long")
                    EXPECT_EQ(mode, "button") << label << " " << path;
            }
        }
        // Pose and haptic paths must resolve to a declared profile source too.
        for (const auto &item : binding.at("bindings").items()) {
            const auto &set = item.value();
            for (const char *section : {"poses", "haptics"}) {
                if (!set.contains(section)) continue;
                for (const auto &entry : set.at(section)) {
                    const auto path = entry.at("path").get<std::string>();
                    ASSERT_TRUE(path.starts_with("/user/hand/")) << label << " " << path;
                    const auto source_path = path.substr(path.find('/', 11));
                    EXPECT_TRUE(sources.contains(source_path))
                        << label << " uses " << source_path << " which the profile does not declare";
                }
            }
            // A chord names the same source paths in a different shape.
            if (!set.contains("chords")) continue;
            for (const auto &chord : set.at("chords")) {
                ASSERT_TRUE(chord.contains("output")) << label;
                for (const auto &input : chord.at("inputs")) {
                    ASSERT_TRUE(input.is_array() && input.size() == 2) << label;
                    const auto path = input.at(0).get<std::string>();
                    ASSERT_TRUE(path.starts_with("/user/hand/")) << label << " " << path;
                    const auto source_path = path.substr(path.find('/', 11));
                    EXPECT_TRUE(sources.contains(source_path))
                        << label << " chord uses " << source_path
                        << " which the profile does not declare";
                }
            }
        }
    }
};

TEST_F(InputProfileTest, ProfileDeclaresTheDriverIdentitySteamVrNeeds) {
    ASSERT_TRUE(profile.is_object());
    EXPECT_EQ(profile.at("jsonid").get<std::string>(), "input_profile");
    EXPECT_EQ(profile.at("controller_type").get<std::string>(), "vive_controller");
    EXPECT_EQ(profile.at("device_class").get<std::string>(), "TrackedDeviceClass_Controller");
    EXPECT_EQ(profile.at("resource_root").get<std::string>(), "pocketvr");
    EXPECT_EQ(profile.at("driver_name").get<std::string>(), "pocketvr");
    // The driver advertises this profile path to SteamVR.
    EXPECT_EQ(profile.at("legacy_binding").get<std::string>(),
              "{pocketvr}/input/pocketvr_legacy_bindings_vive_controller.json");
}

TEST_F(InputProfileTest, EveryBindingUiImageExists) {
    for (const char *side : {"input_bindingui_left", "input_bindingui_right"}) {
        ASSERT_TRUE(profile.contains(side)) << side;
        const auto image = profile.at(side).at("image").get<std::string>();
        const auto path = resolve(root, image);
        ASSERT_FALSE(path.empty()) << side << " does not use a {pocketvr}/ resource token";
        EXPECT_TRUE(fs::exists(path)) << side << " references a missing image: " << path.string();
    }
}

TEST_F(InputProfileTest, DeclaredCapabilitiesAreBackedByRealDriverComponents) {
    std::set<std::string> covered;
    for (const auto &[source, spec] : sources.items()) {
        for (const auto &component : required_components(source, spec)) {
            covered.insert(component);
            EXPECT_NE(std::find(driver_components.begin(), driver_components.end(), component),
                      driver_components.end())
                << "profile advertises " << component << " that the driver never creates";
        }
    }
    for (const auto &component : driver_components)
        EXPECT_NE(covered.find(component), covered.end())
            << "driver creates " << component << " that the profile never declares";
}

TEST_F(InputProfileTest, LegacyBindingIsReferencedResolvesAndCoversTheLegacyActionSet) {
    const auto legacy = resolve(root, profile.at("legacy_binding").get<std::string>());
    ASSERT_FALSE(legacy.empty());
    ASSERT_TRUE(fs::exists(legacy)) << "legacy binding is missing: " << legacy.string();
    const auto binding = load(legacy);
    ASSERT_TRUE(binding.contains("bindings"));
    ASSERT_TRUE(binding.at("bindings").contains("/actions/legacy"));
    expect_binding_is_consistent(binding, "legacy");

    std::set<std::string> outputs;
    for (const auto &source : binding.at("bindings").at("/actions/legacy").at("sources"))
        for (const auto &[input, mapping] : source.at("inputs").items())
            outputs.insert(mapping.at("output").get<std::string>());
    for (const auto &entry : binding.at("bindings").at("/actions/legacy").at("poses"))
        outputs.insert(entry.at("output").get<std::string>());
    for (const auto &entry : binding.at("bindings").at("/actions/legacy").at("haptics"))
        outputs.insert(entry.at("output").get<std::string>());

    // The legacy contract every OpenVR application reads by name.
    for (const std::string hand : {"left", "right"}) {
        for (const std::string suffix : {"system_press", "grip_press", "applicationmenu_press",
                                            "axis0_press", "axis0_value", "axis0_touch",
                                            "axis1_press", "axis1_value", "axis1_touch"}) {
            const auto output = "/actions/legacy/in/" + hand + "_" + suffix;
            EXPECT_NE(outputs.find(output), outputs.end()) << "legacy binding is missing " << output;
        }
        const auto haptic = "/actions/legacy/out/" + hand + "_haptic";
        EXPECT_NE(outputs.find(haptic), outputs.end()) << "legacy binding is missing " << haptic;
    }
    EXPECT_NE(outputs.find("/actions/legacy/in/Left_Pose"), outputs.end());
    EXPECT_NE(outputs.find("/actions/legacy/in/Right_Pose"), outputs.end());
}

TEST_F(InputProfileTest, DefaultBindingsResolveAndAreConsistentWithTheProfile) {
    ASSERT_TRUE(profile.contains("default_bindings"));
    const auto defaults = profile.at("default_bindings");
    ASSERT_TRUE(defaults.is_array());
    ASSERT_FALSE(defaults.empty()) << "the driver ships no default binding";
    for (const auto &entry : defaults) {
        ASSERT_TRUE(entry.contains("app_key")) << entry.dump();
        ASSERT_TRUE(entry.contains("binding_url")) << entry.dump();
        const auto path = input_dir / entry.at("binding_url").get<std::string>();
        ASSERT_TRUE(fs::exists(path)) << "default binding is missing: " << path.string();
        const auto binding = load(path);
        ASSERT_TRUE(binding.contains("app_key")) << path.string();
        EXPECT_EQ(binding.at("app_key").get<std::string>(), entry.at("app_key").get<std::string>());
        expect_binding_is_consistent(binding, path.filename().string());
    }
    // The SteamVR dashboard is the application that must work out of the box.
    const auto compositor = std::find_if(defaults.begin(), defaults.end(), [](const json &entry) {
        return entry.at("app_key").get<std::string>() == "openvr.component.vrcompositor";
    });
    ASSERT_NE(compositor, defaults.end()) << "no default binding for the SteamVR dashboard";
}

TEST_F(InputProfileTest, CompositorBindingDrivesTheDashboardSystemActions) {
    const auto path = input_dir / "pocketvr_vrcompositor_bindings_vive_controller.json";
    ASSERT_TRUE(fs::exists(path));
    const auto binding = load(path);
    expect_binding_is_consistent(binding, "vrcompositor");
    ASSERT_TRUE(binding.at("bindings").contains("/actions/system"));
    const auto &system = binding.at("bindings").at("/actions/system");
    std::set<std::string> outputs;
    for (const auto &source : system.at("sources"))
        for (const auto &[input, mapping] : source.at("inputs").items())
            outputs.insert(mapping.at("output").get<std::string>());
    EXPECT_NE(outputs.find("/actions/system/in/ToggleDashboard"), outputs.end());
    EXPECT_NE(outputs.find("/actions/system/in/ToggleRoomView"), outputs.end());
    EXPECT_NE(outputs.find("/actions/system/in/SystemButtonChord"), outputs.end());
    // Both hands must be able to toggle the dashboard.
    std::set<std::string> hands;
    for (const auto &source : system.at("sources")) {
        const auto source_path = source.at("path").get<std::string>();
        hands.insert(source_path.substr(11, source_path.find('/', 11) - 11));
    }
    EXPECT_EQ(hands.size(), 2U) << "only one hand can toggle the dashboard";
}

TEST_F(InputProfileTest, PackagedResourcesMatchTheSourceTree) {
    // The driver copies its resources next to the DLL. That copy must never
    // lag behind the source, or the shipped driver would run a different input
    // profile than the one that was reviewed.
    const fs::path package_root = POCKETVR_TEST_PACKAGE_ROOT;
    const fs::path packaged = package_root / "resources";
    ASSERT_TRUE(fs::exists(packaged)) << "the package contains no driver resources";
    std::vector<std::string> source_files;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        source_files.push_back(fs::relative(entry.path(), root).generic_string());
    }
    std::vector<std::string> packaged_files;
    for (const auto &entry : fs::recursive_directory_iterator(packaged)) {
        if (!entry.is_regular_file()) continue;
        packaged_files.push_back(fs::relative(entry.path(), packaged).generic_string());
    }
    std::sort(source_files.begin(), source_files.end());
    std::sort(packaged_files.begin(), packaged_files.end());
    EXPECT_EQ(packaged_files, source_files)
        << "the packaged driver resources do not match pc/driver/resources";
    for (const auto &relative : source_files) {
        const auto source_bytes = read_bytes(root / relative);
        const auto packaged_bytes = read_bytes(packaged / relative);
        EXPECT_EQ(packaged_bytes, source_bytes) << "packaged resource differs: " << relative;
    }
    // The driver manifest travels with the same package.
    const fs::path manifest = package_root / "driver.vrdrivermanifest";
    ASSERT_TRUE(fs::exists(manifest));
    EXPECT_EQ(read_bytes(manifest), read_bytes(fs::path(POCKETVR_TEST_RESOURCE_ROOT).parent_path() /
                                                "driver.vrdrivermanifest"));
}
} // namespace
