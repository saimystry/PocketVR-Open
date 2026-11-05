// SPDX-License-Identifier: Apache-2.0
#include "identity_store.hpp"
#include <sodium.h>
#include <fstream>
#include <algorithm>

namespace pocketvr::win {
namespace {
wire::Bytes read(const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 65536)
        throw std::runtime_error("Identity store file size is invalid");
    std::ifstream file(path, std::ios::binary);
    wire::Bytes bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Identity store read failed");
    return bytes;
}
struct Wipe {
    wire::Bytes &bytes;
    ~Wipe() {
        sodium_memzero(bytes.data(), bytes.size());
    }
};
struct KeyWipe {
    Key &key;
    ~KeyWipe() {
        sodium_memzero(key.data(), key.size());
    }
};
} // namespace
std::string key_hex(const Key &key) {
    std::array<char, 65> text{};
    sodium_bin2hex(text.data(), text.size(), key.data(), key.size());
    return text.data();
}
Key parse_key(const std::string &text) {
    Key key{};
    size_t length{};
    if (text.size() != 64 ||
        sodium_hex2bin(key.data(), key.size(), text.data(), text.size(), nullptr, &length,
                       nullptr) ||
        length != key.size())
        throw std::invalid_argument("Invalid device identity");
    return key;
}
Handle IdentityStore::lock() const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        Handle handle(CreateFileW((directory_ / "paired.lock").c_str(),
                                  GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (handle)
            return handle;
        if (GetLastError() != ERROR_SHARING_VIOLATION)
            throw std::runtime_error("Identity store lock failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Identity store is busy");
}
IdentityStore::IdentityStore(std::filesystem::path directory)
    : directory_(directory.empty() ? appdata() : std::move(directory)) {
    std::filesystem::create_directories(directory_);
    const auto guard = lock();
    const auto seed_path = directory_ / "identity.dpapi";
    if (std::filesystem::exists(seed_path)) {
        auto bytes = unprotect(read(seed_path));
        Wipe wipe{bytes};
        if (bytes.size() != 32)
            throw std::runtime_error("Identity seed size is invalid");
        Key seed{};
        std::copy(bytes.begin(), bytes.end(), seed.begin());
        KeyWipe seed_wipe{seed};
        identity_ = std::make_unique<Identity>(seed);
    } else {
        if (std::filesystem::exists(directory_ / "paired.json"))
            throw std::runtime_error("Identity is missing; restore it or explicitly reset pairing");
        identity_ = std::make_unique<Identity>();
        auto seed = identity_->seed();
        KeyWipe seed_wipe{seed};
        const auto sealed = protect(seed);
        atomic_write(seed_path, sealed);
    }
    (void)load(); // Fail closed on corrupt paired-device data, rather than silently replacing it.
}
nlohmann::json IdentityStore::load() const {
    const auto file = directory_ / "paired.json";
    if (!std::filesystem::exists(file))
        return nlohmann::json::array();
    const auto outer = nlohmann::json::parse(read(file));
    if (!outer.is_object() || outer.size() != 2 || outer.at("version") != 1)
        throw std::runtime_error("Unknown paired-device file format");
    const auto text = outer.at("sealed").get<std::string>();
    if (text.empty() || text.size() > 65536)
        throw std::runtime_error("Paired-device data size");
    wire::Bytes sealed(text.size());
    size_t size{};
    if (sodium_base642bin(sealed.data(), sealed.size(), text.data(), text.size(), nullptr, &size,
                          nullptr, sodium_base64_VARIANT_ORIGINAL))
        throw std::runtime_error("Paired-device encoding is invalid");
    sealed.resize(size);
    auto bytes = unprotect(sealed);
    Wipe wipe{bytes};
    auto peers = nlohmann::json::parse(bytes);
    if (!peers.is_array() || peers.size() > 16)
        throw std::runtime_error("Paired-device list is invalid");
    std::vector<Key> seen;
    for (const auto &peer : peers) {
        if (!peer.is_object() || peer.size() != 2 ||
            peer.at("name").get<std::string>().size() > 128)
            throw std::runtime_error("Paired-device record is invalid");
        const auto key = parse_key(peer.at("identity").get<std::string>());
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            throw std::runtime_error("Paired-device identity is duplicated");
        seen.push_back(key);
    }
    return peers;
}
void IdentityStore::save(const nlohmann::json &data) const {
    const auto text = data.dump();
    auto sealed = protect(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    std::string base64(sodium_base64_ENCODED_LEN(sealed.size(), sodium_base64_VARIANT_ORIGINAL),
                       '\0');
    sodium_bin2base64(base64.data(), base64.size(), sealed.data(), sealed.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    base64.resize(base64.size() - 1);
    const auto outer = nlohmann::json{{"version", 1}, {"sealed", base64}}.dump(2);
    atomic_write(directory_ / "paired.json",
                 std::span(reinterpret_cast<const uint8_t *>(outer.data()), outer.size()));
}
nlohmann::json IdentityStore::peers() const {
    const auto guard = lock();
    return load();
}
bool IdentityStore::known(const Key &key) const {
    const auto values = peers();
    return std::any_of(values.begin(), values.end(),
                       [&](const auto &peer) { return parse_key(peer.at("identity")) == key; });
}
void IdentityStore::trust(const Key &key, const std::string &name) {
    if (name.empty() || name.size() > 128)
        throw std::invalid_argument("Device name must be 1-128 bytes");
    const auto guard = lock();
    auto values = load();
    for (auto &peer : values)
        if (parse_key(peer.at("identity")) == key) {
            peer["name"] = name;
            save(values);
            return;
        }
    if (values.size() >= 16)
        throw std::runtime_error("Forget an old paired device before adding another");
    values.push_back({{"identity", key_hex(key)}, {"name", name}});
    save(values);
}
void IdentityStore::forget(const Key &key) {
    const auto guard = lock();
    auto values = load();
    values.erase(
        std::remove_if(values.begin(), values.end(),
                       [&](const auto &peer) { return parse_key(peer.at("identity")) == key; }),
        values.end());
    save(values);
}
} // namespace pocketvr::win
