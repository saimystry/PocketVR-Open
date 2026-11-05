// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "model.hpp"
#include "setup_qr.hpp"
#include <map>

namespace pocketvr::dashboard {
struct UiActions {
    std::function<bool(Json)> command;
    std::function<bool()> logs, bundle;
    std::function<void(bool)> open_folder; // false logs, true diagnostics
};
class Ui {
  public:
    int page{};
    bool fixture{};
    void draw(const Snapshot &snapshot, const UiActions &actions);
  private:
    struct Draft { Json value; bool dirty{}; };
    std::map<std::string, Draft> drafts_;
    History history_;
    std::string feedback_, digits_, peer_, qr_key_;
    std::optional<win::SetupQr> qr_;
    int endpoint_{};
    void section(const char *section, const Json &values, const UiActions &actions);
    void status(const Snapshot &snapshot, const UiActions &actions);
    void pairing(const Snapshot &snapshot, const UiActions &actions);
    void send(const UiActions &actions, Json command);
};
Snapshot fixture_snapshot();
} // namespace pocketvr::dashboard
