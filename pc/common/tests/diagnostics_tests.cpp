// SPDX-License-Identifier: Apache-2.0
#include "diagnostics.hpp"
#include "windows.hpp"
#include <gtest/gtest.h>
#include <fstream>

using namespace pocketvr;
using namespace pocketvr::win;
namespace {
using Json = nlohmann::json;
constexpr auto line = "2026-11-03T12:34:56.123 [info] [42] ";
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("PocketVR-diagnostics-test-" + std::to_string(monotonic_ns()));
    Directory() { std::filesystem::create_directories(path); }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) throw std::runtime_error("Cannot write diagnostic test fixture");
}
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
TEST(Diagnostics, SnapshotExcludesPairingAndNestedSecretsButKeepsTimingAndSettings) {
    const Json source{{"pairing", {{"approved_code", "123456"}}}, {"peers", {"secret peer"}},
        {"status", {{"connected", true}, {"private_key", "secret"}}},
        {"network", {{"session", {{"encoder_backend", "AMD AMF"}, {"decode_p95_ms", 14.2},
                                 {"peer_identity", "hidden"}, {"token", "hidden"}}}}},
        {"settings", {{"values", {{"video", {{"bitrate_mbps", 60}}}}}, {"schema", "not needed"}}}};
    const auto exported = diagnostic_snapshot(source);
    const auto text = exported.dump();
    EXPECT_EQ(text.find("123456"), std::string::npos);
    EXPECT_EQ(text.find("hidden"), std::string::npos);
    EXPECT_EQ(text.find("secret"), std::string::npos);
    EXPECT_FALSE(exported.contains("pairing"));
    EXPECT_EQ(exported["network"]["session"]["decode_p95_ms"], 14.2);
    EXPECT_EQ(exported["settings"]["video"]["bitrate_mbps"], 60);
    EXPECT_FALSE(exported["settings"].contains("schema"));
    const auto offline = diagnostic_snapshot({{"error", "Driver unavailable"}});
    EXPECT_EQ(offline["driver_available"], false);
    EXPECT_EQ(offline["driver_error"], "Driver unavailable");
    EXPECT_FALSE(diagnostic_snapshot(Json::object())["driver_available"].get<bool>());
}
TEST(Diagnostics, HardLinkedLogsAreNotCollected) {
    Directory directory;
    write(directory.path / "paired.json", std::string(line) + "Not a real log\n");
    std::filesystem::create_hard_link(directory.path / "paired.json", directory.path / "driver.log");
    Handle linked(CreateFileW((directory.path / "driver.log").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(linked);
    BY_HANDLE_FILE_INFORMATION info{};
    ASSERT_TRUE(GetFileInformationByHandle(linked.get(), &info));
    linked.reset();
    if (info.nNumberOfLinks < 2)
        GTEST_SKIP() << "Filesystem does not report hard-link counts; verify on local NTFS.";
    const auto files = collect_diagnostics({}, directory.path);
    EXPECT_EQ(files.size(), 3);
    const auto metadata = std::find_if(files.begin(), files.end(), [](const auto &f) { return f.name == "manifest.json"; });
    ASSERT_NE(metadata, files.end());
    EXPECT_EQ(Json::parse(metadata->contents)["logs"][0]["status"], "not a regular unlinked log");
}
TEST(Diagnostics, LogsRejectSecretsBinaryFragmentsAndUnstructuredContinuations) {
    const std::string good = std::string(line) + "Encoder ready: AMD AMF\n";
    const auto text = good + line + "pairing code 123456\n" + line + "key=abc\n" +
        line + "opaque abcdefghijklmnopqrstuvwxyz0123456789\n" +
        "unstructured continuation with a secret\n" +
        line + std::string("binary\0value\n", 13) + line + "unfinished";
    size_t filtered{};
    EXPECT_EQ(diagnostic_log(text, filtered), good);
    EXPECT_EQ(filtered, 6);
}
TEST(Diagnostics, CollectionOnlyReadsKnownLogTailsAndReportsMissingLogsOffline) {
    Directory directory;
    write(directory.path / "driver.log", std::string(diagnostic_log_limit + 50, 'x') +
        "\n" + line + "Final encoder sample\n");
    write(directory.path / "paired.json", "never collect identity stores");
    write(directory.path / "capture.h264", "never collect video");
    write(directory.path / "other.log", std::string(line) + "Not a PocketVR log\n");
    const auto files = collect_diagnostics({{"error", "Driver unavailable"}}, directory.path);
    EXPECT_EQ(files.size(), 4);
    const auto log = std::find_if(files.begin(), files.end(), [](const auto &f) { return f.name == "logs/driver.log"; });
    ASSERT_NE(log, files.end());
    EXPECT_EQ(log->contents, std::string(line) + "Final encoder sample\n");
    const auto metadata = std::find_if(files.begin(), files.end(), [](const auto &f) { return f.name == "manifest.json"; });
    ASSERT_NE(metadata, files.end());
    const auto manifest = Json::parse(metadata->contents);
    EXPECT_TRUE(manifest["logs"][0]["truncated"]);
    EXPECT_EQ(manifest["logs"][1]["status"], "missing");
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW(collect_diagnostics({}, directory.path, stop.get_token()), std::runtime_error);
}
TEST(Diagnostics, ZipRejectsTraversalAliasesAndSizeOverflow) {
    for (const auto *name : {"../escape", "/absolute", "logs/../secret", "C:/file", "logs\\file",
                              "logs//file", "./file", "folder/"}) {
        const std::vector<DiagnosticFile> files{{name, "x"}};
        EXPECT_THROW(diagnostic_zip(files), std::invalid_argument);
    }
    std::vector<DiagnosticFile> files{{"log.txt", "a"}, {"LOG.txt", "b"}};
    EXPECT_THROW(diagnostic_zip(files), std::invalid_argument);
    files = {{"big.txt", std::string(1024 * 1024 + 1, 'x')}};
    EXPECT_THROW(diagnostic_zip(files), std::invalid_argument);
    files.clear();
    for (unsigned i = 0; i < 9; ++i) files.push_back({std::to_string(i) + ".txt", std::string(1024 * 1024, 'x')});
    EXPECT_THROW(diagnostic_zip(files), std::invalid_argument);
    files.assign(33, {"x", ""});
    EXPECT_THROW(diagnostic_zip(files), std::invalid_argument);
}
TEST(Diagnostics, ExportCancellationAndReplaceFailurePreserveExistingDataAndCleanTemps) {
    Directory directory;
    const auto destination = directory.path / "report.zip";
    write(destination, "existing report");
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW(export_diagnostics(destination, {}, directory.path, stop.get_token()), std::runtime_error);
    EXPECT_EQ(read(destination), "existing report");
    const auto blocked = directory.path / "blocked.zip";
    std::filesystem::create_directory(blocked);
    EXPECT_THROW(export_diagnostics(blocked, {}, directory.path), std::runtime_error);
    size_t count{};
    for (const auto &entry : std::filesystem::directory_iterator(directory.path)) {
        EXPECT_NE(entry.path().extension(), ".tmp"); ++count;
    }
    EXPECT_EQ(count, 2);
}
TEST(Diagnostics, ZipInteroperabilityFixture) {
    Directory directory;
    write(directory.path / "driver.log", std::string(line) + "Encoder ready: AMD AMF\n" +
          line + "pairing code 123456\n");
    write(directory.path / "paired.json", "DO_NOT_EXPORT_IDENTITY");
    std::array<wchar_t, 32768> external{};
    const auto length = GetEnvironmentVariableW(L"POCKETVR_DIAGNOSTICS_TEST_EXPORT", external.data(),
                                                static_cast<DWORD>(external.size()));
    ASSERT_LT(length, external.size());
    const auto destination = length ? std::filesystem::path(external.data()) : directory.path / "report.zip";
    export_diagnostics(destination,
        {{"status", {{"state", "tracking"}}}, {"pairing", {{"approved_code", "123456"}}},
         {"settings", {{"values", {{"video", {{"bitrate_mbps", 60}}}}}}}}, directory.path);
    const auto bytes = read(destination);
    EXPECT_GT(bytes.size(), 100);
    EXPECT_EQ(bytes.substr(0, 4), "PK\003\004");
    EXPECT_EQ(bytes.find("DO_NOT_EXPORT_IDENTITY"), std::string::npos);
    EXPECT_EQ(bytes.find("123456"), std::string::npos);
}
}

TEST(Diagnostics, FullRotationsRemainWithinArchiveBudgetAndKeepCurrentOverlayLog) {
    Directory directory;
    std::string text;
    const auto record = std::string(line) + "ordinary rendering message\n";
    while (text.size() < diagnostic_log_limit + record.size()) text += record;
    for (unsigned rotation = 0; rotation < 4; ++rotation)
        for (const auto *component : {"driver", "control", "input", "overlay"}) {
            auto name = std::string(component);
            if (rotation) name += "." + std::to_string(rotation);
            write(directory.path / (name + ".log"), text);
        }
    const auto files = collect_diagnostics({{"error", "Offline fixture"}}, directory.path);
    const auto overlay = std::find_if(files.begin(), files.end(), [](const auto &file) { return file.name == "logs/overlay.log"; });
    ASSERT_NE(overlay, files.end()); EXPECT_FALSE(overlay->contents.empty());
    size_t payload{};
    for (const auto &file : files) if (file.name.starts_with("logs/")) payload += file.contents.size();
    EXPECT_LE(payload, 6 * 1024 * 1024);
    EXPECT_LE(diagnostic_zip(files).size(), diagnostic_zip_limit);
}
