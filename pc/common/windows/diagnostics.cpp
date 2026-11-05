// SPDX-License-Identifier: Apache-2.0
#include "diagnostics.hpp"
#include "windows.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace pocketvr::win {
namespace {
using Json = nlohmann::json;
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Diagnostics export cancelled");
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (char &c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
bool sensitive_key(std::string_view name) {
    const auto key = lower(name);
    constexpr std::array exact{"code", "approved_code", "pairing", "peers", "paired", "pin",
                               "nonce", "signature", "key", "keys"};
    if (std::find(exact.begin(), exact.end(), key) != exact.end()) return true;
    for (const auto *word : {"identity", "secret", "password", "credential", "token", "seed"})
        if (key.find(word) != std::string::npos) return true;
    return key.ends_with("_key") || key.ends_with("_keys");
}
bool sensitive_text(std::string_view text) {
    const auto value = lower(text);
    for (const auto *word : {"pair", "identity", "secret", "password", "credential", "token",
                             "seed", "private key", "public key", "key=", "key:", "code=", "code:",
                             "nonce", "signature", "github_pat_", "ghp_", "bearer "})
        if (value.find(word) != std::string::npos) return true;
    size_t opaque{}, digits{};
    for (const unsigned char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        opaque = digit || alpha || c == '+' || c == '/' || c == '_' || c == '-' || c == '='
                     ? opaque + 1 : 0;
        digits = digit ? digits + 1 : 0;
        if (opaque >= 32 || digits >= 6) return true;
    }
    return false;
}
Json clean(const Json &value, unsigned depth = 0) {
    if (depth > 32) throw std::invalid_argument("Diagnostics snapshot too deeply nested");
    if (value.is_object()) {
        auto result = Json::object();
        for (const auto &[key, child] : value.items())
            if (!sensitive_key(key)) result[key] = clean(child, depth + 1);
        return result;
    }
    if (value.is_array()) {
        auto result = Json::array();
        for (const auto &child : value) result.push_back(clean(child, depth + 1));
        return result;
    }
    if (value.is_string() && sensitive_text(value.get_ref<const std::string &>()))
        return "[filtered]";
    return value;
}
std::string json_text(const Json &j) {
    auto text = j.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
    if (text.size() > 1024 * 1024) throw std::invalid_argument("Diagnostics JSON exceeds 1 MiB");
    return text;
}
bool stamped(std::string_view line) {
    // Only complete lines in PocketVR's structured rotating-log format qualify.
    if (line.size() < 29 || line[4] != '-' || line[7] != '-' || line[10] != 'T' ||
        line[13] != ':' || line[16] != ':' || line[19] != '.' || line[23] != ' ' || line[24] != '[')
        return false;
    for (size_t i = 0; i < 23; ++i)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 &&
            (line[i] < '0' || line[i] > '9')) return false;
    return line.find("] [", 25) != std::string_view::npos;
}
uint32_t crc32(std::string_view text) {
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < values.size(); ++i) {
            auto c = i;
            for (unsigned bit = 0; bit < 8; ++bit) c = c & 1 ? (c >> 1) ^ 0xedb88320U : c >> 1;
            values[i] = c;
        }
        return values;
    }();
    uint32_t crc = 0xffffffffU;
    for (const unsigned char c : text) crc = (crc >> 8) ^ table[(crc ^ c) & 255];
    return crc ^ 0xffffffffU;
}
void word(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value)); out.push_back(static_cast<uint8_t>(value >> 8));
}
void dword(std::vector<uint8_t> &out, uint32_t value) {
    word(out, static_cast<uint16_t>(value)); word(out, static_cast<uint16_t>(value >> 16));
}
bool safe_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '/' || name.back() == '/') return false;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            const auto part = name.substr(start, i - start);
            if (part.empty() || part == "." || part == "..") return false;
            start = i + 1;
        } else {
            const auto c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
    }
    return true;
}
}
Json diagnostic_snapshot(const Json &snapshot) {
    auto result = Json{{"format_version", 1}, {"driver_available",
        snapshot.contains("status") && snapshot.at("status").is_object() && !snapshot.contains("error")}};
    for (const auto *section : {"status", "network"})
        if (snapshot.contains(section)) result[section] = clean(snapshot.at(section));
    if (snapshot.contains("settings") && snapshot.at("settings").is_object() &&
        snapshot.at("settings").contains("values"))
        result["settings"] = clean(snapshot.at("settings").at("values"));
    if (snapshot.contains("error")) result["driver_error"] = clean(snapshot.at("error"));
    return result;
}
std::string diagnostic_log(std::string_view text, size_t &redacted_lines) {
    std::string result;
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string_view::npos) { ++redacted_lines; break; }
        auto line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        const bool controls = std::any_of(line.begin(), line.end(), [](unsigned char c) {
            return c < 32 && c != '\t';
        });
        if (line.size() > 4096 || controls || !stamped(line) || sensitive_text(line))
            ++redacted_lines;
        else { result.append(line); result += '\n'; }
        start = end + 1;
    }
    return result;
}
std::vector<DiagnosticFile> collect_diagnostics(const Json &snapshot,
    const std::filesystem::path &logs, std::stop_token stop) {
    cancelled(stop);
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    size_t log_budget = 6 * 1024 * 1024;
    Json manifest{{"format_version", 1}, {"product_version", "0.1.0"},
        {"utc", {utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond}},
        {"log_tail_limit_bytes", diagnostic_log_limit}, {"aggregate_log_budget_bytes", log_budget}, {"logs", Json::array()},
        {"privacy", "Only status/settings/network and filtered known text logs; review before sharing."}};
    std::vector<DiagnosticFile> files{{"diagnostics.json", json_text(diagnostic_snapshot(snapshot))}};
    const auto attributes = GetFileAttributesW(logs.c_str());
    const bool root_safe = attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    // Current logs precede older rotations when the aggregate budget is exhausted.
    for (unsigned rotation = 0; rotation < 4; ++rotation)
        for (const auto *component : {"driver", "control", "input", "overlay"}) {
            cancelled(stop);
            auto name = std::string(component);
            if (rotation) name += "." + std::to_string(rotation);
            name += ".log";
            // All eligible names are generated ASCII, never taken from directory entries.
            const auto &narrow = name;
            Json item{{"name", narrow}, {"status", "missing"}};
            if (!root_safe) { item["status"] = "log directory unavailable or linked"; }
            else if (!log_budget) { item["status"] = "aggregate log budget exhausted"; }
            else {
                Handle file(CreateFileW((logs / name).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (file) {
                    BY_HANDLE_FILE_INFORMATION info{};
                    LARGE_INTEGER size{};
                    if (!GetFileInformationByHandle(file.get(), &info) ||
                        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                        info.nNumberOfLinks != 1 || GetFileType(file.get()) != FILE_TYPE_DISK ||
                        !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
                        item["status"] = "not a regular unlinked log";
                    } else {
                        const auto count = static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, static_cast<LONGLONG>(std::min(diagnostic_log_limit, log_budget))));
                        const auto offset = size.QuadPart - static_cast<LONGLONG>(count);
                        LARGE_INTEGER start{}; start.QuadPart = offset;
                        std::string tail(count, '\0'); DWORD read{};
                        if (!SetFilePointerEx(file.get(), start, nullptr, FILE_BEGIN) ||
                            !ReadFile(file.get(), tail.data(), static_cast<DWORD>(count), &read, nullptr)) {
                            item["status"] = "read unavailable";
                        } else {
                            tail.resize(read);
                            // Discard an initial partial record after seeking into a large log.
                            if (offset) {
                                const auto newline = tail.find('\n');
                                tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
                            }
                            size_t filtered{};
                            auto text = diagnostic_log(tail, filtered);
                            log_budget -= text.size();
                            item["status"] = "included";
                            item["source_bytes"] = size.QuadPart;
                            item["truncated"] = offset != 0;
                            item["filtered_lines"] = filtered;
                            item["exported_bytes"] = text.size();
                            files.push_back({"logs/" + narrow, std::move(text)});
                        }
                    }
                } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
                    item["status"] = "read unavailable";
            }
            manifest["logs"].push_back(std::move(item));
        }
    files.push_back({"manifest.json", json_text(manifest)});
    files.push_back({"README.txt", "PocketVR diagnostics\r\n"
        "diagnostics.json is a frozen PC status/network/settings snapshot.\r\n"
        "manifest.json lists omitted, filtered, unreadable and truncated logs.\r\n"
        "Only recent tails of driver/control/input/overlay rotating text logs are eligible.\r\n"
        "Pairing/identity stores, captures and arbitrary files are never collected.\r\n"
        "Filtering is conservative but cannot recognize every possible secret.\r\n"
        "Review network addresses, settings and log contents before sharing.\r\n"
        "This bundle is not an end-to-end or scan-out latency measurement.\r\n"});
    cancelled(stop);
    return files;
}
std::vector<uint8_t> diagnostic_zip(std::span<const DiagnosticFile> files) {
    if (files.size() > 32) throw std::invalid_argument("Too many diagnostic ZIP entries");
    size_t bytes = 22;
    std::set<std::string> names;
    for (const auto &file : files) {
        if (!safe_name(file.name) || !names.insert(lower(file.name)).second)
            throw std::invalid_argument("Unsafe or duplicate diagnostic ZIP entry");
        if (file.contents.size() > 1024 * 1024) throw std::invalid_argument("Diagnostic entry too large");
        bytes += 76 + 2 * file.name.size() + file.contents.size();
        if (bytes > diagnostic_zip_limit) throw std::invalid_argument("Diagnostic ZIP exceeds size bound");
    }
    std::vector<uint8_t> out; out.reserve(bytes);
    std::array<uint32_t, 32> offsets{}, checksums{};
    size_t index = 0;
    // Classic ZIP, method STORE, DOS date 1980-01-01. Capture UTC is in manifest.
    for (const auto &file : files) {
        offsets[index] = static_cast<uint32_t>(out.size());
        checksums[index] = crc32(file.contents);
        dword(out, 0x04034b50); word(out, 20); word(out, 0x0800); word(out, 0);
        word(out, 0); word(out, 33); dword(out, checksums[index++]);
        dword(out, static_cast<uint32_t>(file.contents.size()));
        dword(out, static_cast<uint32_t>(file.contents.size()));
        word(out, static_cast<uint16_t>(file.name.size())); word(out, 0);
        out.insert(out.end(), file.name.begin(), file.name.end());
        out.insert(out.end(), file.contents.begin(), file.contents.end());
    }
    const auto directory = static_cast<uint32_t>(out.size()); index = 0;
    for (const auto &file : files) {
        dword(out, 0x02014b50); word(out, 20); word(out, 20); word(out, 0x0800); word(out, 0);
        word(out, 0); word(out, 33); dword(out, checksums[index]);
        dword(out, static_cast<uint32_t>(file.contents.size()));
        dword(out, static_cast<uint32_t>(file.contents.size()));
        word(out, static_cast<uint16_t>(file.name.size()));
        word(out, 0); word(out, 0); word(out, 0); word(out, 0); dword(out, 0);
        dword(out, offsets[index++]); out.insert(out.end(), file.name.begin(), file.name.end());
    }
    const auto directory_size = static_cast<uint32_t>(out.size()) - directory;
    dword(out, 0x06054b50); word(out, 0); word(out, 0);
    word(out, static_cast<uint16_t>(files.size())); word(out, static_cast<uint16_t>(files.size()));
    dword(out, directory_size); dword(out, directory); word(out, 0);
    return out;
}
void export_diagnostics(const std::filesystem::path &destination, const Json &snapshot,
    const std::filesystem::path &logs, std::stop_token stop) {
    const auto files = collect_diagnostics(snapshot, logs, stop);
    const auto bytes = diagnostic_zip(files);
    cancelled(stop);
    struct Temporary {
        std::filesystem::path path;
        Handle file;
        ~Temporary() { file.reset(); if (!path.empty()) DeleteFileW(path.c_str()); }
    } pending;
    pending.path = destination;
    pending.path += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(monotonic_ns()) + L".tmp";
    pending.file = Handle(CreateFileW(pending.path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!pending.file) { pending.path.clear(); throw std::runtime_error("Cannot create diagnostics ZIP"); }
    DWORD written{};
    if (!WriteFile(pending.file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size() || !FlushFileBuffers(pending.file.get()))
        throw std::runtime_error("Cannot write diagnostics ZIP");
    pending.file.reset();
    cancelled(stop);
    if (!MoveFileExW(pending.path.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace diagnostics ZIP destination");
    pending.path.clear();
}
} // namespace pocketvr::win
