// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/fec.hpp>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace pocketvr {
namespace {
struct Galois {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    // product[a][b] == a*b, so the per-byte loops are one lookup with no branches.
    std::array<std::array<uint8_t, 256>, 256> product{};
    Galois() {
        unsigned x = 1;
        for (unsigned i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 256)
                x ^= 0x11d;
        }
        for (unsigned i = 255; i < 512; ++i)
            exp[i] = exp[i - 255];
        for (unsigned a = 1; a < 256; ++a)
            for (unsigned b = 1; b < 256; ++b)
                product[a][b] = exp[static_cast<unsigned>(log[a]) + log[b]];
    }
    uint8_t mul(uint8_t a, uint8_t b) const {
        return a && b ? exp[static_cast<unsigned>(log[a]) + log[b]] : 0;
    }
    uint8_t inverse(uint8_t a) const {
        if (!a)
            throw std::invalid_argument("Zero GF inverse");
        return exp[255 - log[a]];
    }
};
const Galois gf;
using Matrix = std::array<uint8_t, 32 * 32>;
void add_scaled(std::span<uint8_t> out, uint8_t scale, std::span<const uint8_t> in) {
    if (!scale)
        return;
    const auto &row = gf.product[scale];
    for (size_t b = 0; b < out.size(); ++b)
        out[b] ^= row[in[b]];
}
bool invert(Matrix a, Matrix &out, size_t n) {
    out.fill(0);
    for (size_t i = 0; i < n; ++i)
        out[i * 32 + i] = 1;
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        while (pivot < n && !a[pivot * 32 + col])
            ++pivot;
        if (pivot == n)
            return false;
        if (pivot != col)
            for (size_t j = 0; j < n; ++j) {
                std::swap(a[col * 32 + j], a[pivot * 32 + j]);
                std::swap(out[col * 32 + j], out[pivot * 32 + j]);
            }
        const auto scale = gf.inverse(a[col * 32 + col]);
        for (size_t j = 0; j < n; ++j) {
            a[col * 32 + j] = gf.mul(a[col * 32 + j], scale);
            out[col * 32 + j] = gf.mul(out[col * 32 + j], scale);
        }
        for (size_t row = 0; row < n; ++row)
            if (row != col) {
                const auto f = a[row * 32 + col];
                for (size_t j = 0; j < n; ++j) {
                    a[row * 32 + j] ^= gf.mul(f, a[col * 32 + j]);
                    out[row * 32 + j] ^= gf.mul(f, out[col * 32 + j]);
                }
            }
    }
    return true;
}
} // namespace
Fec::Fec(uint8_t k, uint8_t m, FecKind kind) : k_(k), m_(m) {
    if (k == 0 || k > 32 || m > 8 || (kind == FecKind::xor_parity && m != 1))
        throw std::invalid_argument("FEC dimensions");
    for (size_t i = 0; i < k; ++i)
        matrix_[i * 32 + i] = 1;
    if (kind == FecKind::xor_parity) {
        for (size_t j = 0; j < k; ++j)
            matrix_[static_cast<size_t>(k) * 32 + j] = 1;
        return;
    }
    Matrix base{}, inv{};
    for (size_t i = 0; i < k; ++i) {
        uint8_t power = 1;
        for (size_t j = 0; j < k; ++j) {
            base[i * 32 + j] = power;
            power = gf.mul(power, static_cast<uint8_t>(i + 1));
        }
    }
    if (!invert(base, inv, k))
        throw std::runtime_error("FEC generator singular");
    for (size_t i = k; i < static_cast<size_t>(k + m); ++i)
        for (size_t j = 0; j < k; ++j) {
            uint8_t power = 1, sum = 0;
            for (size_t c = 0; c < k; ++c) {
                sum ^= gf.mul(power, inv[c * 32 + j]);
                power = gf.mul(power, static_cast<uint8_t>(i + 1));
            }
            matrix_[i * 32 + j] = sum;
        }
}
bool Fec::valid(std::span<std::span<uint8_t>> shards) const {
    if (shards.size() != static_cast<size_t>(k_ + m_) || shards.empty() || shards[0].empty() ||
        shards[0].size() > 2048)
        return false;
    return std::all_of(shards.begin(), shards.end(),
                       [&](auto s) { return s.size() == shards[0].size(); });
}
bool Fec::encode(std::span<std::span<uint8_t>> shards) const {
    if (!valid(shards))
        return false;
    for (size_t i = k_; i < shards.size(); ++i) {
        std::fill(shards[i].begin(), shards[i].end(), 0);
        for (size_t j = 0; j < k_; ++j)
            add_scaled(shards[i], matrix_[i * 32 + j], shards[j]);
    }
    return true;
}
bool Fec::recover(std::span<std::span<uint8_t>> shards, std::span<const uint8_t> present) const {
    if (!valid(shards) || present.size() != shards.size())
        return false;
    std::array<size_t, 32> chosen{};
    size_t count = 0;
    for (size_t i = 0; i < shards.size() && count < k_; ++i)
        if (present[i])
            chosen[count++] = i;
    if (count < k_)
        return false;
    Matrix selected{}, inverse{};
    for (size_t i = 0; i < k_; ++i)
        for (size_t j = 0; j < k_; ++j)
            selected[i * 32 + j] = matrix_[chosen[i] * 32 + j];
    if (!invert(selected, inverse, k_))
        return false;
    // Chosen shards are immutable during recovery: missing destinations are never chosen.
    for (size_t i = 0; i < k_; ++i)
        if (!present[i]) {
            std::fill(shards[i].begin(), shards[i].end(), 0);
            for (size_t j = 0; j < k_; ++j)
                add_scaled(shards[i], inverse[i * 32 + j], shards[chosen[j]]);
        }
    return encode(shards);
}
const Fec &shared_fec(uint8_t k, uint8_t m, FecKind kind) {
    if (k == 0 || k > 32 || m > 8)
        throw std::invalid_argument("FEC dimensions");
    static thread_local std::array<std::unique_ptr<Fec>, 33 * 9 * 2> coders;
    auto &coder = coders[(static_cast<size_t>(k) * 9 + m) * 2 + (kind == FecKind::reed_solomon ? 1 : 0)];
    if (!coder)
        coder = std::make_unique<Fec>(k, m, kind);
    return *coder;
}
} // namespace pocketvr
