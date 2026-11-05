// SPDX-License-Identifier: Apache-2.0
#include "usb.hpp"
#include "windows.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <future>

using namespace pocketvr;
using namespace pocketvr::win;
namespace {
struct MockAdb {
    ReverseMappings rules{{"tcp:8000", "localabstract:other-app"}};
    std::string state{"device"};
    unsigned creates{}, removes{}, lists{};
    bool fail_second{}, conflict_after_preflight{}, fail_remove{};
    std::stop_source *cancel_after_create{};
    ProcessResult run(const std::vector<std::wstring> &args, std::stop_token stop,
                       std::chrono::milliseconds) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        if (args == std::vector<std::wstring>{L"devices", L"-l"})
            return {0, "List of devices attached\nTEST_PHONE " + state + " model:Pixel_4a transport_id:1\n"};
        if (args.size() < 4 || args[0] != L"-s" || args[1] != L"TEST_PHONE" || args[2] != L"reverse")
            throw std::runtime_error("Unexpected command");
        const auto narrow = [](const std::wstring &s) {
            std::string result;
            for (const auto c : s) result.push_back(static_cast<char>(c));
            return result;
        };
        if (args[3] == L"--list") {
            if (++lists == 2 && conflict_after_preflight) rules["tcp:9944"] = "tcp:9999";
            std::string output;
            for (const auto &[from, to] : rules) output += "UsbFfs " + from + " " + to + "\n";
            return {0, output};
        }
        if (args[3] == L"--no-rebind" && args.size() == 6) {
            const auto from = narrow(args[4]), to = narrow(args[5]);
            if ((fail_second && from == "tcp:9944") || rules.contains(from))
                return {1, "cannot bind requested port"};
            rules[from] = to; ++creates;
            if (cancel_after_create) cancel_after_create->request_stop();
            return {0, ""};
        }
        if (args[3] == L"--remove" && args.size() == 5) {
            if (fail_remove && args[4] == L"tcp:9944") return {1, "remove failed"};
            rules.erase(narrow(args[4])); ++removes; return {0, ""};
        }
        throw std::runtime_error("Unexpected reverse mutation");
    }
    UsbSetup client() { return UsbSetup([this](const auto &a, auto s, auto t) { return run(a, s, t); }); }
};
TEST(UsbSetup, ParsesAuthorizationOfflineWirelessAndMultipleDeviceStates) {
    const auto phones = parse_adb_devices("* daemon started successfully\r\nList of devices attached\r\n"
        "PHONE_A device product:sunfish model:Pixel_4a transport_id:1\nPHONE_B unauthorized\n"
        "PHONE_C offline\nemulator-5554 device model:emulator\n192.168.0.2:5555 device\n"
        "adb-phone._adb-tls-connect._tcp device\nPHONE_D no permissions\n");
    ASSERT_EQ(phones.size(), 7);
    EXPECT_TRUE(phones[0].authorized()); EXPECT_EQ(phones[0].model, "Pixel_4a");
    EXPECT_FALSE(phones[1].authorized()); EXPECT_FALSE(phones[2].authorized());
    EXPECT_FALSE(phones[3].cable_candidate()); EXPECT_FALSE(phones[4].cable_candidate());
    EXPECT_FALSE(phones[5].cable_candidate()); EXPECT_EQ(phones[6].state, "no");
    EXPECT_THROW(parse_adb_devices("A device\nA device\n"), std::invalid_argument);
    EXPECT_THROW(parse_adb_devices("--bad device\n"), std::invalid_argument);
    EXPECT_THROW(parse_adb_reverse("UsbFfs tcp:1 tcp:2\nUsbFfs tcp:1 tcp:2\n"), std::invalid_argument);
    EXPECT_THROW(parse_adb_reverse("unexpected malformed\n"), std::invalid_argument);
}
TEST(UsbSetup, SetupAndRemovalAreIdempotentAndPreserveUnrelatedRules) {
    MockAdb adb; auto client = adb.client();
    EXPECT_NE(client.configure("TEST_PHONE", 9943, 9944, false).find("ready"), std::string::npos);
    EXPECT_EQ(adb.creates, 2);
    client.configure("TEST_PHONE", 9943, 9944, false); EXPECT_EQ(adb.creates, 2);
    EXPECT_EQ(adb.rules.at("tcp:8000"), "localabstract:other-app");
    client.configure("TEST_PHONE", 9943, 9944, true); EXPECT_EQ(adb.removes, 2);
    client.configure("TEST_PHONE", 9943, 9944, true); EXPECT_EQ(adb.removes, 2);
    EXPECT_EQ(adb.rules.size(), 1);
}
TEST(UsbSetup, ConflictsAndUnauthorizedDevicesCannotChangeEitherPort) {
    MockAdb adb; auto client = adb.client();
    adb.rules["tcp:9944"] = "tcp:9000";
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false), std::runtime_error);
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, true), std::runtime_error);
    EXPECT_EQ(adb.creates + adb.removes, 0);
    EXPECT_EQ(adb.rules.at("tcp:9944"), "tcp:9000");
    adb.rules.erase("tcp:9944"); adb.state = "unauthorized";
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false), std::runtime_error);
    EXPECT_THROW(client.configure("other", 9943, 9944, false), std::runtime_error);
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9943, false), std::invalid_argument);
    EXPECT_THROW(client.mappings("-bad argument"), std::invalid_argument);
    EXPECT_EQ(adb.creates + adb.removes, 0);
}
TEST(UsbReconnect, RestoresLostRulesWithoutRebindingExistingOrUnrelatedPorts) {
    MockAdb adb; auto client = adb.client(); UsbReconnect reconnect;
    EXPECT_EQ(reconnect.tick(client, 9943, 9944), "USB cable mappings ready");
    EXPECT_EQ(adb.creates, 2);
    reconnect.tick(client, 9943, 9944);
    EXPECT_EQ(adb.creates, 2);
    adb.rules.erase("tcp:9943"); adb.rules.erase("tcp:9944");
    EXPECT_EQ(reconnect.tick(client, 9943, 9944), "USB cable mappings ready");
    EXPECT_EQ(adb.creates, 4);
    EXPECT_EQ(adb.removes, 0);
    EXPECT_EQ(adb.rules.at("tcp:8000"), "localabstract:other-app");
}
TEST(UsbReconnect, DoesNotChooseAmbiguousUnauthorizedOrReplacementPhones) {
    MockAdb adb; auto client = adb.client(); UsbReconnect reconnect;
    adb.state = "unauthorized";
    EXPECT_NE(reconnect.tick(client, 9943, 9944).find("Waiting"), std::string::npos);
    adb.state = "device\nOTHER device";
    EXPECT_NE(reconnect.tick(client, 9943, 9944).find("Multiple"), std::string::npos);
    EXPECT_EQ(adb.creates, 0);
    adb.state = "device";
    EXPECT_EQ(reconnect.tick(client, 9943, 9944), "USB cable mappings ready");
    adb.state = "offline\nOTHER device";
    EXPECT_NE(reconnect.tick(client, 9943, 9944).find("Waiting"), std::string::npos);
    EXPECT_EQ(adb.creates, 2); // Never follows a different phone after unplug.
    adb.state = "device";
    adb.rules["tcp:9944"] = "tcp:9999";
    EXPECT_THROW(reconnect.tick(client, 9943, 9944), std::runtime_error);
    EXPECT_EQ(adb.rules.at("tcp:9944"), "tcp:9999");
    EXPECT_EQ(adb.removes, 0);
}
TEST(UsbSetup, FailedSecondRuleRollsBackOnlyNewConfirmedRules) {
    MockAdb adb; adb.fail_second = true; auto client = adb.client();
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false), std::runtime_error);
    EXPECT_EQ(adb.creates, 1); EXPECT_EQ(adb.removes, 1); EXPECT_EQ(adb.rules.size(), 1);
    adb.rules["tcp:9943"] = "tcp:9943";
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false), std::runtime_error);
    EXPECT_EQ(adb.removes, 1); EXPECT_TRUE(adb.rules.contains("tcp:9943"));
}
TEST(UsbSetup, CancellationRollsBackWithFreshTokenAndConcurrentChangesArePreserved) {
    MockAdb adb; auto client = adb.client(); std::stop_source stop;
    adb.cancel_after_create = &stop;
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false, stop.get_token()), std::runtime_error);
    EXPECT_EQ(adb.rules.size(), 1);
    adb.cancel_after_create = nullptr; adb.conflict_after_preflight = true; adb.lists = 0;
    EXPECT_THROW(client.configure("TEST_PHONE", 9943, 9944, false), std::runtime_error);
    EXPECT_FALSE(adb.rules.contains("tcp:9943")); EXPECT_EQ(adb.rules.at("tcp:9944"), "tcp:9999");
}
TEST(UsbSetup, PartialRemovalIsReportedWithoutRecreatingOrTouchingOtherRules) {
    MockAdb adb; adb.rules["tcp:9943"] = "tcp:9943"; adb.rules["tcp:9944"] = "tcp:9944";
    adb.fail_remove = true; auto client = adb.client();
    try { client.configure("TEST_PHONE", 9943, 9944, true); FAIL() << "Expected partial removal error"; }
    catch (const std::runtime_error &e) { EXPECT_NE(std::string(e.what()).find("partial"), std::string::npos); }
    EXPECT_FALSE(adb.rules.contains("tcp:9943")); EXPECT_TRUE(adb.rules.contains("tcp:9944"));
    EXPECT_EQ(adb.rules.at("tcp:8000"), "localabstract:other-app"); EXPECT_EQ(adb.creates, 0);
}
TEST(WindowsProcess, ArgumentsRoundtripWithoutShellAndExitErrorsRemainVisible) {
    const std::filesystem::path exe = POCKETVR_TEST_PROCESS_EXE;
    const std::vector<std::wstring> args{L"", L"space here", L"quote\"inside", L"C:\\folder with spaces\\",
        L"slashes\\\\\"quote", L"& echo $(not-a-command) ; |", L"\u03c0"};
    const auto result = run_process(exe, args);
    ASSERT_EQ(result.exit_code, 0);
    const auto parsed = nlohmann::json::parse(result.output);
    ASSERT_EQ(parsed.size(), args.size());
    EXPECT_EQ(parsed[0], ""); EXPECT_EQ(parsed[1], "space here");
    EXPECT_EQ(parsed[2], "quote\"inside"); EXPECT_EQ(parsed[3], "C:\\folder with spaces\\");
    EXPECT_EQ(parsed[4], "slashes\\\\\"quote"); EXPECT_EQ(parsed[5], "& echo $(not-a-command) ; |");
    EXPECT_EQ(parsed[6], "\xcf\x80");
    const auto failed = run_process(exe, std::vector<std::wstring>{L"--fail"});
    EXPECT_EQ(failed.exit_code, 7); EXPECT_NE(failed.output.find("synthetic adb error"), std::string::npos);
}
TEST(WindowsProcess, TimeoutCancellationAndOutputOverflowTerminateOnlyOwnedCommand) {
    const std::filesystem::path exe = POCKETVR_TEST_PROCESS_EXE;
    EXPECT_THROW(run_process(exe, std::vector<std::wstring>{L"--sleep"}, {}, std::chrono::milliseconds(100)), std::runtime_error);
    EXPECT_THROW(run_process(exe, std::vector<std::wstring>{L"--flood"}, {}, std::chrono::seconds(10), 1024), std::runtime_error);
    std::stop_source source;
    auto child = std::async(std::launch::async, [&] {
        EXPECT_THROW(run_process(exe, std::vector<std::wstring>{L"--sleep"}, source.get_token()), std::runtime_error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); source.request_stop();
    EXPECT_EQ(child.wait_for(std::chrono::seconds(3)), std::future_status::ready); child.get();
    EXPECT_THROW(quote_argument(std::wstring_view(L"a\0b", 3)), std::invalid_argument);
}
TEST(UsbSetup, ExplicitAdbPathRequiresAnAbsoluteExistingAdbExecutable) {
    const auto directory = std::filesystem::temp_directory_path() / ("PocketVR usb path " + std::to_string(monotonic_ns()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path / "adb.exe"); std::filesystem::remove(path); } } cleanup{directory};
    const auto file = directory / "adb.exe";
    std::filesystem::copy_file(POCKETVR_TEST_PROCESS_EXE, file);
    EXPECT_EQ(find_adb(file), file);
    EXPECT_EQ(nlohmann::json::parse(run_process(file, std::vector<std::wstring>{L"path with spaces"}).output)[0], "path with spaces");
    EXPECT_THROW(find_adb(directory / "missing.exe"), std::runtime_error);
    EXPECT_THROW(find_adb("adb.exe"), std::runtime_error);
}
TEST(UsbSetup, DiscoversAdbFromSdkEnvironmentWithoutSearchingWorkingDirectory) {
    const auto root = std::filesystem::temp_directory_path() / ("PocketVR sdk test " + std::to_string(monotonic_ns()));
    const auto tools = root / "platform-tools";
    std::filesystem::create_directories(tools);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove(root / "platform-tools" / "adb.exe"); std::filesystem::remove(root / "platform-tools"); std::filesystem::remove(root); }
    } cleanup{root};
    { std::ofstream out(tools / "adb.exe"); out << "synthetic path fixture"; }
    struct Environment {
        std::wstring name, saved;
        bool present{};
        Environment(const wchar_t *key, const std::wstring &value) : name(key) {
            const auto size = GetEnvironmentVariableW(key, nullptr, 0);
            if (size) {
                saved.resize(size); const auto n = GetEnvironmentVariableW(key, saved.data(), size);
                saved.resize(n); present = true;
            }
            SetEnvironmentVariableW(key, value.c_str());
        }
        ~Environment() { SetEnvironmentVariableW(name.c_str(), present ? saved.c_str() : nullptr); }
    } sdk(L"ANDROID_SDK_ROOT", root.native()), home(L"ANDROID_HOME", root.native());
    EXPECT_EQ(find_adb(), tools / "adb.exe");
}
}
