// SPDX-License-Identifier: Apache-2.0
#include "setup_qr.hpp"
#include <pocketvr/settings.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>

TEST(SetupQr, LanOnlyAndNoTrustMaterial) {
    for (const auto *ip : {"10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.1.2"}) {
        const auto code = pocketvr::win::connection_qr(pocketvr::Endpoint::parse(ip, 9943));
        EXPECT_EQ(code.text, std::string("pocketvr://connect?v=1&ip=") + ip + "&port=9943");
        EXPECT_EQ(code.dark.size(), static_cast<size_t>(code.size * code.size));
        EXPECT_GE(code.size, 21);
    }
    for (const auto *ip : {"127.0.0.1", "0.0.0.0", "172.15.0.1", "172.32.0.1", "8.8.8.8", "169.254.1.2"})
        EXPECT_THROW(pocketvr::win::connection_qr(pocketvr::Endpoint::parse(ip, 9943)), std::invalid_argument);
    EXPECT_THROW(pocketvr::win::connection_qr(pocketvr::Endpoint::parse("192.168.1.2", 0)), std::invalid_argument);
}
TEST(SetupQr, DesktopMatrixForIndependentAndroidDecoder) {
    const auto code = pocketvr::win::connection_qr(pocketvr::Endpoint::parse("192.168.42.7", 12345));
    // Explicit developer fixture export, exercised through ZXing on Android.
    char *path{};
    size_t length{};
    if (_dupenv_s(&path, &length, "POCKETVR_QR_FIXTURE") == 0 && path) {
        std::ofstream output(path, std::ios::binary);
        free(path);
        ASSERT_TRUE(output.good());
        output << code.text << '\n' << code.size << '\n';
        for (int y = 0; y < code.size; ++y) {
            for (int x = 0; x < code.size; ++x)
                output << (code.dark[static_cast<size_t>(y) * code.size + x] ? '1' : '0');
            output << '\n';
        }
        ASSERT_TRUE(output.good());
    }
}
TEST(SetupQr, ViewerOffsetsLimitsAndOldProfileMigration) {
    using namespace pocketvr;
    Settings settings;
    auto viewer = settings.viewer_values();
    viewer["screen_width_m"] = 0.140; viewer["screen_height_m"] = 0.060;
    viewer["screen_to_lens_m"] = 0.040; viewer["inter_lens_distance_m"] = 0.064;
    viewer["k1"] = 0; viewer["k2"] = 0; viewer["vertical_offset_m"] = 0.005;
    viewer["fov_left_deg"] = 30; viewer["fov_right_deg"] = 35;
    settings.set_viewer(viewer);
    const auto geometry = settings.viewer_geometry();
    EXPECT_NEAR(geometry.screen[0].up, 0.625, 1e-12);
    EXPECT_NEAR(geometry.screen[0].down, 0.875, 1e-12);
    EXPECT_NEAR(geometry.eyes[0].left, std::tan(pi / 6), 1e-12);
    EXPECT_EQ(geometry.eyes[0].left, geometry.eyes[1].right);
    EXPECT_EQ(geometry.eyes[0].up, geometry.eyes[1].up);
    auto bad = viewer; bad["vertical_offset_m"] = 0.03;
    EXPECT_THROW(settings.set_viewer(bad), std::invalid_argument);
    EXPECT_EQ(settings.viewer_values(), viewer); // Failed imports leave current geometry intact.
    for (const auto *key : {"vertical_offset_m", "fov_left_deg", "fov_right_deg", "fov_bottom_deg", "fov_top_deg"})
        viewer.erase(key);
    settings.set_viewer(viewer);
    EXPECT_EQ(settings.viewer_geometry().screen[0].up, settings.viewer_geometry().screen[0].down);
}
