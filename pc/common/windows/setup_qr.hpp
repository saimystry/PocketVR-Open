// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/transport.hpp>
#include <vector>
namespace pocketvr::win {
struct SetupQr {
    std::string text;
    int size{};
    std::vector<bool> dark;
};
SetupQr connection_qr(const Endpoint &endpoint);
}
