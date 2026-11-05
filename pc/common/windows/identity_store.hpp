// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "windows.hpp"
#include <pocketvr/pairing.hpp>
#include <memory>

namespace pocketvr::win {
class IdentityStore final {
  public:
    explicit IdentityStore(std::filesystem::path directory = {});
    const Identity &identity() const noexcept {
        return *identity_;
    }
    bool known(const Key &key) const;
    void trust(const Key &key, const std::string &name);
    void forget(const Key &key);
    nlohmann::json peers() const;

  private:
    std::filesystem::path directory_;
    std::unique_ptr<Identity> identity_;
    Handle lock() const;
    nlohmann::json load() const;
    void save(const nlohmann::json &data) const;
};
std::string key_hex(const Key &key);
Key parse_key(const std::string &hex);
} // namespace pocketvr::win
