// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pocketvr {
enum class FecKind { xor_parity, reed_solomon };
class Fec {
  public:
    Fec(uint8_t k, uint8_t m, FecKind kind = FecKind::reed_solomon);
    bool encode(std::span<std::span<uint8_t>> shards) const;
    bool recover(std::span<std::span<uint8_t>> shards, std::span<const uint8_t> present) const;
    uint8_t data_count() const {
        return k_;
    }
    uint8_t parity_count() const {
        return m_;
    }

  private:
    uint8_t k_, m_;
    std::array<uint8_t, 40 * 32> matrix_{};
    bool valid(std::span<std::span<uint8_t>> shards) const;
};
// Building a coder inverts a matrix, so streaming code reuses one per shape. Each
// thread keeps its own, and the reference stays valid for the life of that thread.
const Fec &shared_fec(uint8_t k, uint8_t m, FecKind kind = FecKind::reed_solomon);
} // namespace pocketvr
