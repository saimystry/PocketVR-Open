// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/input.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pocketvr {
bool valid_input(const RawInput &v) {
    if (v.buttons & 0xffff0000U) return false;
    for (size_t i=0;i<v.axes.size();++i)
        if (!std::isfinite(v.axes[i]) || v.axes[i] < (i<4 ? -1.f : 0.f) || v.axes[i]>1) return false;
    return true;
}
bool encode_input(const RawInput &v, std::span<uint8_t> output) {
    if (output.size()!=input_packet_size || !valid_input(v)) return false;
    wire::Writer w(output);
    if (!w.u8(1) || !w.u8(static_cast<uint8_t>((v.active?1:0)|(v.gamepad?2:0))) || !w.u16(0) ||
        !w.u32(v.sequence) || !w.u64(v.timestamp_ns) || !w.u32(v.buttons)) return false;
    for (const auto axis:v.axes) if (!w.f32(axis)) return false;
    return true;
}
std::optional<RawInput> decode_input(std::span<const uint8_t> data) {
    if (data.size()!=input_packet_size) return {};
    wire::Reader r(data); RawInput v; uint8_t version{},flags{}; uint16_t reserved{};
    if (!r.u8(version)||version!=1||!r.u8(flags)||flags>3||!r.u16(reserved)||reserved||
        !r.u32(v.sequence)||!r.u64(v.timestamp_ns)||!r.u32(v.buttons)) return {};
    v.active=(flags&1)!=0; v.gamepad=(flags&2)!=0;
    for (auto &axis:v.axes) if (!r.f32(axis)) return {};
    return valid_input(v)?std::optional(v):std::nullopt;
}
InputAction input_action(std::string_view name) {
    const auto found=std::find(input_actions.begin(),input_actions.end(),name);
    if (found==input_actions.end()) throw std::invalid_argument("Unknown input action");
    return static_cast<InputAction>(found-input_actions.begin());
}
InputMapping input_preset(std::string_view name) {
    InputMapping m{};
    if (name!="xbox" && name!="playstation" && name!="one_button_cardboard" && name!="touchscreen" && name!="custom")
        throw std::invalid_argument("Unknown input mapping preset");
    if (name=="xbox" || name=="playstation" || name=="custom") {
        m={InputAction::left_pad,InputAction::right_pad,InputAction::left_menu,InputAction::right_menu,
           InputAction::left_grip,InputAction::right_grip,InputAction::left_pad,InputAction::right_pad,
           InputAction::system,InputAction::recenter,InputAction::right_trigger,InputAction::right_grip,
           InputAction::left_trigger,InputAction::right_trigger,InputAction::system,InputAction::recenter};
    } else {
        m[0]=m[10]=m[13]=InputAction::right_trigger;
        m[11]=InputAction::right_grip; m[12]=InputAction::left_trigger;
        m[14]=InputAction::system; m[15]=InputAction::recenter;
    }
    return m;
}
MappedInput map_input(const RawInput &v,const InputMapping &mapping) {
    MappedInput result;
    if (!valid_input(v) || !v.active) return result;
    result.active=true; result.gamepad=v.gamepad;
    const auto axis=[](float x) { return std::abs(x)<=0.15f?0.f:std::copysign((std::abs(x)-0.15f)/0.85f,x); };
    if (v.gamepad) for (size_t i=0;i<2;++i) {
        result.hands[i].x=axis(v.axes[i*2]); result.hands[i].y=axis(v.axes[i*2+1]);
        result.hands[i].trigger=v.axes[i+4]<=0.05f?0.f:(v.axes[i+4]-0.05f)/0.95f;
    }
    for (size_t i=0;i<mapping.size();++i) if (v.buttons&(1U<<i)) {
        switch(mapping[i]) {
        case InputAction::none: break;
        case InputAction::left_trigger: result.hands[0].trigger=1; break;
        case InputAction::right_trigger: result.hands[1].trigger=1; break;
        case InputAction::left_grip: result.hands[0].grip=true; break;
        case InputAction::right_grip: result.hands[1].grip=true; break;
        case InputAction::left_menu: result.hands[0].menu=true; break;
        case InputAction::right_menu: result.hands[1].menu=true; break;
        case InputAction::system: result.hands[1].system=true; break;
        case InputAction::left_pad: result.hands[0].pad=true; break;
        case InputAction::right_pad: result.hands[1].pad=true; break;
        case InputAction::recenter: result.recenter=true; break;
        }
    }
    return result;
}
std::optional<HapticPulse> bounded_haptic(uint8_t hand,float duration,float amplitude,float frequency) {
    if (hand>1||!std::isfinite(duration)||!std::isfinite(amplitude)||!std::isfinite(frequency)||
        duration<0||duration>60||amplitude<0||amplitude>1||frequency<0||frequency>1000) return {};
    const auto ms=static_cast<uint16_t>(std::clamp(std::ceil(duration*1000.f),0.f,500.f));
    return HapticPulse{ms,hand,amplitude,frequency};
}
bool encode_haptic(const HapticPulse &p,std::span<uint8_t> output) {
    if (output.size()!=haptic_packet_size||p.hand>1||p.duration_ms>500||!std::isfinite(p.amplitude)||
        !std::isfinite(p.frequency)||p.amplitude<0||p.amplitude>1||p.frequency<0||p.frequency>1000) return false;
    wire::Writer w(output);
    return w.u8(1)&&w.u8(p.hand)&&w.u16(p.duration_ms)&&w.f32(p.amplitude)&&w.f32(p.frequency);
}
std::optional<HapticPulse> decode_haptic(std::span<const uint8_t> data) {
    if (data.size()!=haptic_packet_size) return {};
    wire::Reader r(data); uint8_t version{}; HapticPulse p;
    if (!r.u8(version)||version!=1||!r.u8(p.hand)||!r.u16(p.duration_ms)||!r.f32(p.amplitude)||!r.f32(p.frequency)) return {};
    std::array<uint8_t,haptic_packet_size> checked{};
    return encode_haptic(p,checked)?std::optional(p):std::nullopt;
}
} // namespace pocketvr
