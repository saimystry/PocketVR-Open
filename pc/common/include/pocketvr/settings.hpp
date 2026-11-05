// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <pocketvr/viewer.hpp>
#include <string>

namespace pocketvr {
class Settings {
  public:
    Settings();
    explicit Settings(nlohmann::json values);
    const nlohmann::json &values() const {
        return values_;
    }
    static const nlohmann::json &schema();
    static bool runtime_supported(const std::string &key);
    void set(const std::string &key, const nlohmann::json &value);
    void set_viewer(const nlohmann::json &values);
    nlohmann::json viewer_values() const;
    ViewerGeometry viewer_geometry() const;
    static void validate(const nlohmann::json &values);
    template <class T> T get(const std::string &key) const {
        return values_.at(pointer(key)).get<T>();
    }

  private:
    nlohmann::json values_;
    static nlohmann::json::json_pointer pointer(const std::string &key);
};
} // namespace pocketvr
