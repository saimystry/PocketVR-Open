// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/video.hpp>
#include <algorithm>
#include <functional>
#include <limits>

namespace pocketvr {
static uint8_t limited(int value, int low, int high) {
    return static_cast<uint8_t>(std::clamp(value, low, high));
}
bool bgra_to_nv12(std::span<const uint8_t> source, size_t stride, uint32_t width, uint32_t height,
                  std::span<uint8_t> output) {
    if (!width || !height || (width & 1) || (height & 1) || width > 8192 || height > 8192 ||
        stride < static_cast<size_t>(width) * 4 || stride > 64 * 1024 ||
        source.size() < stride * height ||
        output.size() < static_cast<size_t>(width) * height * 3 / 2)
        return false;
    for (size_t y = 0; y < height; ++y)
        for (size_t x = 0; x < width; ++x) {
            const auto *p = source.data() + y * stride + x * 4;
            output[y * width + x] =
                limited(16 + ((11966 * p[2] + 40254 * p[1] + 4064 * p[0] + 32768) >> 16), 16, 235);
        }
    const auto offset = static_cast<size_t>(width) * height;
    for (size_t y = 0; y < height; y += 2)
        for (size_t x = 0; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (size_t dy = 0; dy < 2; ++dy)
                for (size_t dx = 0; dx < 2; ++dx) {
                    const auto *p = source.data() + (y + dy) * stride + (x + dx) * 4;
                    r += p[2];
                    g += p[1];
                    b += p[0];
                }
            r = (r + 2) / 4;
            g = (g + 2) / 4;
            b = (b + 2) / 4;
            const auto at = offset + (y / 2) * width + x;
            output[at] =
                limited(128 + ((-6596 * r - 22189 * g + 28784 * b + 32768) >> 16), 16, 240);
            output[at + 1] =
                limited(128 + ((28784 * r - 26145 * g - 2639 * b + 32768) >> 16), 16, 240);
        }
    return true;
}
static size_t start_code(std::span<const uint8_t> input, size_t at) {
    if (at + 3 <= input.size() && input[at] == 0 && input[at + 1] == 0) {
        if (input[at + 2] == 1)
            return 3;
        if (at + 4 <= input.size() && input[at + 2] == 0 && input[at + 3] == 1)
            return 4;
    }
    return 0;
}
static bool nals(std::span<const uint8_t> input,
                 const std::function<void(std::span<const uint8_t>)> &visitor) {
    size_t at = 0;
    bool found = false;
    while (at < input.size()) {
        const auto code = start_code(input, at);
        if (!code) {
            if (!found && input[at] != 0)
                return false;
            ++at;
            continue;
        }
        const auto begin = at + code;
        if (begin >= input.size())
            return false;
        size_t end = begin;
        while (end < input.size() && !start_code(input, end))
            ++end;
        if (end == begin)
            return false;
        visitor(input.subspan(begin, end - begin));
        found = true;
        at = end;
    }
    return found;
}
std::optional<wire::Bytes> to_annex_b(std::span<const uint8_t> input, unsigned length_bytes) {
    if (input.empty() || input.size() > 8 * 1024 * 1024 || length_bytes < 1 || length_bytes > 4)
        return {};
    wire::Bytes out;
    out.reserve(input.size() + 64);
    size_t at = 0;
    bool prefixed = true;
    while (at < input.size()) {
        if (input.size() - at < length_bytes) {
            prefixed = false;
            break;
        }
        uint32_t length = 0;
        for (unsigned i = 0; i < length_bytes; ++i)
            length = (length << 8) | input[at++];
        if (!length || length > input.size() - at || out.size() + length + 4 > 8 * 1024 * 1024) {
            prefixed = false;
            break;
        }
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), input.begin() + static_cast<ptrdiff_t>(at),
                   input.begin() + static_cast<ptrdiff_t>(at + length));
        at += length;
    }
    if (prefixed)
        return out;
    if (nals(input, [](auto) {}))
        return wire::Bytes(input.begin(), input.end());
    return {};
}
std::optional<wire::Bytes> codec_headers(std::span<const uint8_t> input, Codec codec) {
    if (input.empty() || input.size() > 65536)
        return {};
    if (nals(input, [](auto) {}))
        return wire::Bytes(input.begin(), input.end());
    if (input[0] != 1)
        return {};
    wire::Bytes out;
    size_t at = 0;
    auto array = [&](size_t count) -> bool {
        for (size_t i = 0; i < count; ++i) {
            if (input.size() - at < 2)
                return false;
            const auto length =
                static_cast<size_t>((static_cast<unsigned>(input[at]) << 8) | input[at + 1]);
            at += 2;
            if (!length || length > input.size() - at)
                return false;
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), input.begin() + static_cast<ptrdiff_t>(at),
                       input.begin() + static_cast<ptrdiff_t>(at + length));
            at += length;
        }
        return true;
    };
    if (codec == Codec::h264) {
        if (input.size() < 7)
            return {};
        at = 6;
        if (!array(input[5] & 31) || at >= input.size())
            return {};
        const auto count = input[at++];
        if (!array(count))
            return {};
    } else {
        if (input.size() < 23)
            return {};
        at = 23;
        for (unsigned i = 0; i < input[22]; ++i) {
            if (input.size() - at < 3)
                return {};
            ++at;
            const auto count = (static_cast<unsigned>(input[at]) << 8) | input[at + 1];
            at += 2;
            if (!array(count))
                return {};
        }
    }
    if (out.empty())
        return {};
    return out;
}
bool contains_idr(std::span<const uint8_t> input, Codec codec) {
    bool result = false;
    nals(input, [&](auto nal) {
        const auto type = codec == Codec::h264 ? nal[0] & 31 : (nal[0] >> 1) & 63;
        result |= codec == Codec::h264 ? type == 5 : (type >= 16 && type <= 23);
    });
    return result;
}
bool contains_parameter_sets(std::span<const uint8_t> input, Codec codec) {
    unsigned bits = 0;
    nals(input, [&](auto nal) {
        const auto type = codec == Codec::h264 ? nal[0] & 31 : (nal[0] >> 1) & 63;
        if (codec == Codec::h264) {
            if (type == 7)
                bits |= 1;
            if (type == 8)
                bits |= 2;
        } else if (type >= 32 && type <= 34)
            bits |= 1U << (type - 32);
    });
    return bits == (codec == Codec::h264 ? 3U : 7U);
}
double psnr(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    if (a.empty() || a.size() != b.size())
        throw std::invalid_argument("PSNR sizes");
    double squared = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double difference = static_cast<double>(a[i]) - b[i];
        squared += difference * difference;
    }
    return squared == 0 ? std::numeric_limits<double>::infinity()
                        : 10 * std::log10(255.0 * 255 * a.size() / squared);
}
} // namespace pocketvr
