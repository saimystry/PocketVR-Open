// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "process.hpp"
#include <functional>
#include <map>
#include <vector>

namespace pocketvr::win {
struct AdbDevice {
    std::string serial, state, model;
    bool cable_candidate() const;
    bool authorized() const { return state == "device" && cable_candidate(); }
};
using ReverseMappings = std::map<std::string, std::string>;
std::vector<AdbDevice> parse_adb_devices(std::string_view text);
ReverseMappings parse_adb_reverse(std::string_view text);
std::filesystem::path find_adb(const std::filesystem::path &configured = {});
using AdbRunner = std::function<ProcessResult(const std::vector<std::wstring> &,
    std::stop_token, std::chrono::milliseconds)>;
class UsbSetup {
  public:
    explicit UsbSetup(AdbRunner runner) : runner_(std::move(runner)) {}
    explicit UsbSetup(const std::filesystem::path &adb);
    std::vector<AdbDevice> devices(std::stop_token stop = {}) const;
    ReverseMappings mappings(const std::string &serial, std::stop_token stop = {}) const;
    // Mutates only the two matching mappings, with preflight, no-rebind and
    // rollback of confirmed creations. Neither operation affects pairing.
    std::string configure(const std::string &serial, uint16_t control, uint16_t media,
                          bool remove, std::stop_token stop = {}) const;
  private:
    AdbRunner runner_;
    ProcessResult call(const std::vector<std::wstring> &args, std::stop_token stop,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;
};
// Select one authorized cable device, then keep that identity until this helper
// exits. Never switch to another phone after an unplug, or replace other rules.
class UsbReconnect {
  public:
    std::string tick(const UsbSetup &client, uint16_t control, uint16_t media,
                     std::stop_token stop = {});
  private:
    std::string selected_;
};
} // namespace pocketvr::win
