// SPDX-License-Identifier: Apache-2.0
#include "windows.hpp"
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char **argv) {
    bool watch{};
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "pocketvr_diagnostics [--watch]\n"
                     "Read local driver/phone diagnostics as JSON; --watch samples once a second.\n"
                     "Start SteamVR with PocketVR registered first. Ctrl+C stops watching.\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--watch")
        watch = true;
    else if (argc != 1) {
        std::cerr << "Usage: pocketvr_diagnostics [--watch]\n";
        return 2;
    }
    do {
        try {
            const auto response = pocketvr::win::rpc({{"method", "network.status"}});
            if (!response.value("ok", false))
                throw std::runtime_error(response.value("error", "Driver rejected diagnostics"));
            std::cout << response.dump() << '\n' << std::flush;
        } catch (const std::exception &error) {
            std::cout << nlohmann::json({{"ok", false}, {"error", error.what()}}).dump() << '\n'
                      << std::flush;
            if (!watch)
                return 1;
        }
        if (watch)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (watch);
    return 0;
}
