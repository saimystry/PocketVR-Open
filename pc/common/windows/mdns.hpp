// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "windows.hpp"
#include <pocketvr/crypto.hpp>
#include <memory>

namespace pocketvr::win {
class MdnsAdvertiser final {
  public:
    MdnsAdvertiser(PrivateInterface nic, uint16_t control_port, uint16_t media_port,
                   const Key &identity);
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser &) = delete;
    MdnsAdvertiser &operator=(const MdnsAdvertiser &) = delete;
    bool advertised() const noexcept;

  private:
    class State;
    std::shared_ptr<State> state_;
};
} // namespace pocketvr::win
