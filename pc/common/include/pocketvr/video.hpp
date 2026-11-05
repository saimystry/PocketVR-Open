// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"
#include <optional>

namespace pocketvr {
enum class Codec { h264, hevc };
bool bgra_to_nv12(std::span<const uint8_t> bgra, size_t row_pitch, uint32_t width, uint32_t height,
                  std::span<uint8_t> nv12);
std::optional<wire::Bytes> to_annex_b(std::span<const uint8_t> input, unsigned length_bytes = 4);
std::optional<wire::Bytes> codec_headers(std::span<const uint8_t> configuration, Codec codec);
bool contains_idr(std::span<const uint8_t> annex_b, Codec codec);
bool contains_parameter_sets(std::span<const uint8_t> annex_b, Codec codec);
double psnr(std::span<const uint8_t> a, std::span<const uint8_t> b);
} // namespace pocketvr
