// SPDX-License-Identifier: Apache-2.0
#include "encoded_dump.hpp"
#include <pocketvr/transport.hpp>
#include <gtest/gtest.h>
#include <fstream>

using namespace pocketvr;
using namespace pocketvr::driver;
namespace {
const wire::Bytes keyframe{0,0,0,1,0x67,0x11,0,0,0,1,0x68,0x22,0,0,0,1,0x65,0x33};
const wire::Bytes delta{0,0,0,1,0x41,0x44};
struct CaptureFixture : testing::Test {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("PocketVR-dump-" + std::to_string(monotonic_ns()));
    void TearDown() override { std::filesystem::remove_all(root); }
    static nlohmann::json wait(EncodedDump &dump, const std::function<bool(const nlohmann::json &)> &predicate) {
        const auto deadline = monotonic_ns() + 3'000'000'000;
        while (monotonic_ns() < deadline) {
            auto status = dump.status();
            if (predicate(status)) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return dump.status();
    }
};
TEST_F(CaptureFixture, WaitsForDecodableKeyframeAndKeepsWholeFramesWithinByteCap) {
    EncodedDump dump(root, {1'000'000'000, keyframe.size() + delta.size()});
    dump.set_enabled(true);
    dump.record(delta, false, Codec::h264);
    dump.record(delta, true, Codec::h264); // Flag alone cannot establish codec headers.
    EXPECT_EQ(dump.status().at("frames"), 0);
    EXPECT_TRUE(dump.waiting());
    dump.record(keyframe, true, Codec::h264);
    auto s = wait(dump, [](const auto &v) { return v.at("frames") == 1; });
    ASSERT_EQ(s.at("frames"), 1);
    dump.record(delta, false, Codec::h264);
    s = wait(dump, [](const auto &v) { return v.at("frames") == 2; });
    ASSERT_EQ(s.at("frames"), 2);
    dump.record(delta, false, Codec::h264); // Complete before crossing the cap.
    s = wait(dump, [](const auto &v) { return v.at("state") == "complete"; });
    EXPECT_EQ(s.at("state"), "complete");
    ASSERT_EQ(s.at("bytes"), keyframe.size() + delta.size());
    std::ifstream file(s.at("path").get<std::string>(), std::ios::binary);
    wire::Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    auto expected = keyframe; expected.insert(expected.end(), delta.begin(), delta.end());
    EXPECT_EQ(bytes, expected);
    dump.set_enabled(true); // Same setting does not create endless recordings.
    dump.record(keyframe, true, Codec::h264);
    EXPECT_EQ(dump.status().at("frames"), 2);
}
TEST_F(CaptureFixture, DurationStopsWhenNoMoreFramesArriveAndRearmUsesNewFile) {
    EncodedDump dump(root, {100'000'000, 1024});
    dump.set_enabled(true); dump.record(keyframe, true, Codec::h264);
    auto first = wait(dump, [](const auto &s) { return s.at("state") == "complete"; });
    ASSERT_EQ(first.at("frames"), 1);
    dump.set_enabled(false); dump.set_enabled(true);
    const wire::Bytes hevc{0,0,0,1,64,1,0,0,0,1,66,1,0,0,0,1,68,1,0,0,0,1,38,1};
    dump.record(hevc, true, Codec::hevc);
    auto second = wait(dump, [](const auto &s) { return s.at("state") == "complete"; });
    EXPECT_EQ(second.at("frames"), 1);
    EXPECT_NE(second.at("path"), first.at("path"));
    EXPECT_EQ(std::filesystem::path(second.at("path").get<std::string>()).extension(), ".hevc");
    EXPECT_TRUE(std::filesystem::exists(first.at("path").get<std::string>()));
}
TEST_F(CaptureFixture, FileFailureAndCodecChangeCannotEscapeOrOverwriteFiles) {
    std::filesystem::create_directories(root);
    const auto blocker = root / "not-a-directory";
    { std::ofstream file(blocker); file << "preserve me"; }
    EncodedDump broken(blocker);
    broken.set_enabled(true); broken.record(keyframe, true, Codec::h264);
    const auto failed = wait(broken, [](const auto &s) { return s.at("state") == "failed"; });
    EXPECT_EQ(failed.at("state"), "failed");
    EXPECT_FALSE(failed.at("error").get<std::string>().empty());
    EXPECT_EQ(std::filesystem::file_size(blocker), 11);
    EncodedDump dump(root / "valid");
    dump.set_enabled(true); dump.record(keyframe, true, Codec::h264);
    ASSERT_EQ(wait(dump, [](const auto &s) { return s.at("frames") == 1; }).at("frames"), 1);
    dump.record(delta, false, Codec::hevc);
    const auto changed = wait(dump, [](const auto &s) { return s.at("state") == "failed"; });
    EXPECT_EQ(changed.at("state"), "failed");
    EXPECT_EQ(changed.at("frames"), 1);
}
} // namespace
