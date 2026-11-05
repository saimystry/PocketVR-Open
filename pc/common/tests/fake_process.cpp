// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
int wmain(int argc, wchar_t **argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--sleep") { Sleep(10000); return 0; }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--flood") { std::cout << std::string(100000, 'x') << std::flush; return 0; }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--fail") { std::cerr << "synthetic adb error" << std::flush; return 7; }
    auto values = nlohmann::json::array();
    for (int i = 1; i < argc; ++i) {
        const auto length = static_cast<int>(wcslen(argv[i]));
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, argv[i], length, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<size_t>(bytes), '\0');
        if (bytes) WideCharToMultiByte(CP_UTF8, 0, argv[i], length, value.data(), bytes, nullptr, nullptr);
        values.push_back(std::move(value));
    }
    std::cout << values.dump() << std::flush;
    return 0;
}
