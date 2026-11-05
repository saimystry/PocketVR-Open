// SPDX-License-Identifier: Apache-2.0
#include "setup_qr.hpp"
#include "qrcodegen.hpp"
namespace pocketvr::win {
SetupQr connection_qr(const Endpoint &endpoint) {
    const auto a = endpoint.address;
    const bool lan = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8;
    if (!lan || !endpoint.port)
        throw std::invalid_argument("Choose a private LAN interface; use USB setup for adb");
    SetupQr result;
    result.text = "pocketvr://connect?v=1&ip=" + endpoint.ip() + "&port=" + std::to_string(endpoint.port);
    const auto code = qrcodegen::QrCode::encodeText(result.text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    result.size = code.getSize();
    result.dark.reserve(static_cast<size_t>(result.size) * result.size);
    for (int y = 0; y < result.size; ++y)
        for (int x = 0; x < result.size; ++x)
            result.dark.push_back(code.getModule(x, y));
    return result;
}
}
