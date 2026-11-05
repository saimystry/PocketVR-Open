// SPDX-License-Identifier: Apache-2.0
#include "windows.hpp"
#include "identity_store.hpp"
#include <gtest/gtest.h>
#include <pocketvr/transport.hpp>
#include <fstream>
#include <future>

using namespace pocketvr;
TEST(WindowsSecurity, DpapiRoundtripAndTamperDetection) {
    const std::vector<uint8_t> secret{1, 2, 3, 4, 5, 6};
    auto protected_bytes = win::protect(secret);
    EXPECT_NE(protected_bytes, secret);
    EXPECT_EQ(win::unprotect(protected_bytes), secret);
    protected_bytes.back() ^= 1;
    EXPECT_THROW(win::unprotect(protected_bytes), std::runtime_error);
}
TEST(WindowsRpc, OwnerRestrictedPipeEchoAndBoundedRequests) {
    const auto name =
        std::wstring(LR"(\\.\pipe\PocketVR.test.)") + std::to_wstring(GetCurrentProcessId());
    win::RpcServer server(
        [](const nlohmann::json &request) {
            return nlohmann::json{{"ok", true}, {"echo", request}};
        },
        name);
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        ready = WaitNamedPipeW(name.c_str(), 10) != FALSE;
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(ready);
    for (int i = 0; i < 10; ++i) {
        const auto response = win::rpc({{"method", "test"}, {"index", i}}, name);
        EXPECT_TRUE(response.at("ok").get<bool>());
        EXPECT_EQ(response.at("echo").at("index"), i);
    }
    EXPECT_THROW(win::rpc({{"oversized", std::string(70 * 1024, 'x')}}, name),
                 std::invalid_argument);
    nlohmann::json nested = 1;
    for (unsigned i = 0; i < 40; ++i) nested = {{"nested", std::move(nested)}};
    const auto rejected = win::rpc(nested, name);
    EXPECT_FALSE(rejected.at("ok").get<bool>());
    EXPECT_TRUE(win::rpc({{"method", "after-depth-rejection"}}, name).at("ok").get<bool>());
    std::atomic<unsigned> errors{};
    std::array<std::jthread, 4> clients;
    for (auto &client : clients)
        client = std::jthread([&] {
            for (int i = 0; i < 10; ++i) {
                try {
                    if (win::rpc({{"index", i}}, name).at("echo").at("index") != i)
                        ++errors;
                } catch (...) {
                    ++errors;
                }
            }
        });
    for (auto &client : clients)
        client.join();
    EXPECT_EQ(errors.load(), 0);
}
TEST(WindowsStorage, AtomicReplacePreservesExactBytes) {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("PocketVR-test-" + std::to_string(monotonic_ns()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const auto file = directory / "settings.json";
    const std::array<uint8_t, 3> first{1, 2, 3};
    const std::array<uint8_t, 2> second{4, 5};
    win::atomic_write(file, first);
    win::atomic_write(file, second);
    std::ifstream input(file, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    EXPECT_EQ(data, (std::vector<uint8_t>{4, 5}));
    input.close();
    EXPECT_TRUE(std::filesystem::remove(file));
    EXPECT_TRUE(std::filesystem::remove(directory));
}
TEST(WindowsRpc, CancellingAnInFlightCallDoesNotWaitForTheServerReply) {
    const auto name = std::wstring(LR"(\\.\pipe\PocketVR.cancel.)") + std::to_wstring(GetCurrentProcessId());
    std::promise<void> entered, release;
    auto arrived = entered.get_future();
    auto resume = release.get_future();
    win::RpcServer server([&](const nlohmann::json &) {
        entered.set_value();
        resume.wait();
        return nlohmann::json{{"ok", true}};
    }, name);
    std::promise<bool> completed;
    auto done = completed.get_future();
    std::jthread caller([&](std::stop_token stop) {
        bool cancelled = false;
        try { (void)win::rpc({{"method", "wait"}}, name, stop); }
        catch (const std::exception &) { cancelled = true; }
        completed.set_value(cancelled);
    });
    const bool reached = arrived.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    caller.request_stop();
    const auto status = done.wait_for(std::chrono::seconds(1));
    release.set_value();
    caller.join();
    EXPECT_TRUE(reached);
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(done.get());
}
TEST(WindowsSecurity, DeviceIdentityAndPairedRecordsPersistAndRejectTampering) {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("PocketVR-pairing-test-" + std::to_string(monotonic_ns()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    Identity peer;
    {
        win::IdentityStore first(directory);
        EXPECT_FALSE(first.known(peer.public_key()));
        first.trust(peer.public_key(), "Test phone");
        EXPECT_TRUE(first.known(peer.public_key()));
        win::IdentityStore second(directory);
        EXPECT_EQ(first.identity().public_key(), second.identity().public_key());
        EXPECT_TRUE(second.known(peer.public_key()));
        second.trust(peer.public_key(), "Renamed phone");
        EXPECT_EQ(first.peers().size(), 1);
        EXPECT_EQ(first.peers()[0]["name"], "Renamed phone");
        first.forget(peer.public_key());
        EXPECT_FALSE(second.known(peer.public_key()));
        first.trust(peer.public_key(), "Test phone");
        const std::string corrupted = R"({"version":1,"sealed":"AAAA"})";
        win::atomic_write(
            directory / "paired.json",
            std::span(reinterpret_cast<const uint8_t *>(corrupted.data()), corrupted.size()));
        EXPECT_THROW(second.peers(), std::runtime_error);
        EXPECT_THROW((void)win::IdentityStore{directory}, std::runtime_error);
    }
    // Only this test's three explicitly named files are removed.
    for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
        EXPECT_TRUE(std::filesystem::remove(directory / name));
    EXPECT_TRUE(std::filesystem::remove(directory));
}
