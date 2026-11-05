// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace pocketvr::android {
// First-party 5x7 bitmap alphabet. The fixed-size HUD never allocates on GL's
// display callback and is uploaded only when the sampled counters change.
struct StatsBitmap {
    static constexpr int width = 256, height = 32;
    std::array<uint8_t, width * height> pixels{};
    static constexpr std::array<uint8_t, 7> glyph(char c) {
        switch (c) {
        case '>':
            return {16, 8, 4, 2, 4, 8, 16};
        case '%':
            return {25, 25, 2, 4, 8, 19, 19};
        case '0':
            return {14, 17, 19, 21, 25, 17, 14};
        case '1':
            return {4, 12, 4, 4, 4, 4, 14};
        case '2':
            return {14, 17, 1, 2, 4, 8, 31};
        case '3':
            return {30, 1, 1, 14, 1, 1, 30};
        case '4':
            return {2, 6, 10, 18, 31, 2, 2};
        case '5':
            return {31, 16, 16, 30, 1, 1, 30};
        case '6':
            return {14, 16, 16, 30, 17, 17, 14};
        case '7':
            return {31, 1, 2, 4, 8, 8, 8};
        case '8':
            return {14, 17, 17, 14, 17, 17, 14};
        case '9':
            return {14, 17, 17, 15, 1, 1, 14};
        case 'A':
            return {14, 17, 17, 31, 17, 17, 17};
        case 'B':
            return {30, 17, 17, 30, 17, 17, 30};
        case 'C':
            return {14, 17, 16, 16, 16, 17, 14};
        case 'D':
            return {30, 17, 17, 17, 17, 17, 30};
        case 'E':
            return {31, 16, 16, 30, 16, 16, 31};
        case 'F':
            return {31, 16, 16, 30, 16, 16, 16};
        case 'G':
            return {14, 17, 16, 23, 17, 17, 15};
        case 'H':
            return {17, 17, 17, 31, 17, 17, 17};
        case 'I':
            return {14, 4, 4, 4, 4, 4, 14};
        case 'J':
            return {7, 2, 2, 2, 18, 18, 12};
        case 'K':
            return {17, 18, 20, 24, 20, 18, 17};
        case 'L':
            return {16, 16, 16, 16, 16, 16, 31};
        case 'M':
            return {17, 27, 21, 21, 17, 17, 17};
        case 'N':
            return {17, 25, 25, 21, 19, 19, 17};
        case 'O':
            return {14, 17, 17, 17, 17, 17, 14};
        case 'P':
            return {30, 17, 17, 30, 16, 16, 16};
        case 'Q':
            return {14, 17, 17, 17, 21, 18, 13};
        case 'R':
            return {30, 17, 17, 30, 20, 18, 17};
        case 'S':
            return {15, 16, 16, 14, 1, 1, 30};
        case 'T':
            return {31, 4, 4, 4, 4, 4, 4};
        case 'U':
            return {17, 17, 17, 17, 17, 17, 14};
        case 'V':
            return {17, 17, 17, 17, 17, 10, 4};
        case 'W':
            return {17, 17, 17, 21, 21, 21, 10};
        case 'X':
            return {17, 17, 10, 4, 10, 17, 17};
        case 'Y':
            return {17, 17, 10, 4, 4, 4, 4};
        case 'Z':
            return {31, 1, 2, 4, 8, 16, 31};
        case '-':
            return {0, 0, 0, 31, 0, 0, 0};
        case '.':
            return {0, 0, 0, 0, 0, 12, 12};
        default:
            return {};
        }
    }
    void line(unsigned row, std::string_view text) {
        if (row >= 4)
            return;
        for (size_t column = 0; column < text.size() && column < 42; ++column) {
            const auto bits = glyph(text[column]);
            for (unsigned y = 0; y < 7; ++y)
                for (unsigned x = 0; x < 5; ++x)
                    pixels[(row * 8 + y) * width + column * 6 + x] =
                        bits[y] & (1U << (4 - x)) ? 255 : 0;
        }
    }
};
} // namespace pocketvr::android
