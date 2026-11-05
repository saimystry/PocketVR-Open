// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"
#include <array>
#include <string_view>

namespace pocketvr {
// Stable physical button order, independent of Android/Windows key codes.
inline constexpr std::array<std::string_view,16> input_buttons{
    "a","b","x","y","left_bumper","right_bumper","left_stick","right_stick",
    "start","select","volume_up","volume_down","touch_left","touch_right","touch_menu","recenter"};
enum class InputAction : uint8_t { none, left_trigger, right_trigger, left_grip, right_grip,
    left_menu, right_menu, system, left_pad, right_pad, recenter };
inline constexpr std::array<std::string_view,11> input_actions{
    "none","left_trigger","right_trigger","left_grip","right_grip","left_menu","right_menu",
    "system","left_pad","right_pad","recenter"};
struct RawInput {
    uint32_t sequence{}, buttons{};
    uint64_t timestamp_ns{};
    std::array<float,6> axes{}; // LX, LY, RX, RY, LT, RT; Y up, triggers 0..1.
    bool active{}, gamepad{};
};
struct HandState { float trigger{}, x{}, y{}; bool grip{}, menu{}, system{}, pad{}; };
struct MappedInput { std::array<HandState,2> hands{}; bool active{}, gamepad{}, recenter{}; };
using InputMapping = std::array<InputAction,16>;
constexpr size_t input_packet_size = 44;
bool valid_input(const RawInput &input);
bool encode_input(const RawInput &input, std::span<uint8_t> output);
std::optional<RawInput> decode_input(std::span<const uint8_t> input);
InputMapping input_preset(std::string_view name);
InputAction input_action(std::string_view name);
MappedInput map_input(const RawInput &input, const InputMapping &mapping);
struct HapticPulse { uint16_t duration_ms{}; uint8_t hand{}; float amplitude{},frequency{}; };
struct TimedHaptic { HapticPulse pulse; uint64_t timestamp_ns{}; };
constexpr size_t haptic_packet_size=12;
bool encode_haptic(const HapticPulse &pulse,std::span<uint8_t> output);
std::optional<HapticPulse> decode_haptic(std::span<const uint8_t> input);
std::optional<HapticPulse> bounded_haptic(uint8_t hand,float duration_seconds,float amplitude,float frequency);
} // namespace pocketvr
