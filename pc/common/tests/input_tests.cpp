// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/input.hpp>
#include <gtest/gtest.h>
#include <limits>
using namespace pocketvr;
TEST(PhoneControls, WireRoundtripRejectsVersionReservedLengthsAndNonfiniteAxes) {
    RawInput input; input.active=input.gamepad=true; input.sequence=UINT32_MAX; input.timestamp_ns=1234;
    input.buttons=0xffff; input.axes={-1,1,0,.5f,0,1};
    std::array<uint8_t,input_packet_size> bytes{};
    ASSERT_TRUE(encode_input(input,bytes));
    const auto copy=decode_input(bytes); ASSERT_TRUE(copy);
    EXPECT_EQ(copy->buttons,input.buttons); EXPECT_EQ(copy->axes,input.axes); EXPECT_EQ(copy->sequence,input.sequence);
    for (size_t i=0;i<bytes.size();++i) EXPECT_FALSE(decode_input(std::span(bytes).first(i)));
    auto bad=bytes; bad[0]=2; EXPECT_FALSE(decode_input(bad));
    bad=bytes; bad[1]=4; EXPECT_FALSE(decode_input(bad));
    bad=bytes; bad[2]=1; EXPECT_FALSE(decode_input(bad));
    input.axes[4]=-1; EXPECT_FALSE(encode_input(input,bytes));
    input.axes[4]=0; input.axes[0]=std::numeric_limits<float>::quiet_NaN(); EXPECT_FALSE(encode_input(input,bytes));
    input.axes[0]=0; input.buttons=1U<<16; EXPECT_FALSE(encode_input(input,bytes));
}
TEST(PhoneControls, PresetsCustomBindingsDeadzoneAndInactiveAlwaysRelease) {
    RawInput raw; raw.active=raw.gamepad=true; raw.axes={.1f,-.1f,1,-1,.01f,1};
    auto mapped=map_input(raw,input_preset("xbox"));
    EXPECT_EQ(mapped.hands[0].x,0); EXPECT_EQ(mapped.hands[0].trigger,0);
    EXPECT_EQ(mapped.hands[1].x,1); EXPECT_EQ(mapped.hands[1].y,-1); EXPECT_EQ(mapped.hands[1].trigger,1);
    raw.buttons=(1U<<4)|(1U<<5)|(1U<<15);
    mapped=map_input(raw,input_preset("playstation"));
    EXPECT_TRUE(mapped.hands[0].grip); EXPECT_TRUE(mapped.hands[1].grip); EXPECT_TRUE(mapped.recenter);
    InputMapping custom{}; custom[0]=InputAction::right_menu; raw.buttons=1;
    EXPECT_TRUE(map_input(raw,custom).hands[1].menu);
    raw.gamepad=false; raw.buttons=1U<<13;
    EXPECT_EQ(map_input(raw,input_preset("one_button_cardboard")).hands[1].trigger,1);
    raw.active=false; mapped=map_input(raw,input_preset("touchscreen"));
    EXPECT_FALSE(mapped.active); EXPECT_EQ(mapped.hands[1].trigger,0);
    EXPECT_THROW(input_action("execute"),std::invalid_argument);
    EXPECT_THROW(input_preset("unknown"),std::invalid_argument);
}
TEST(PhoneControls, HapticsRejectMalformedAndBoundDurationAndAmplitude) {
    auto pulse=bounded_haptic(1,2.f,.5f,150.f); ASSERT_TRUE(pulse);
    EXPECT_EQ(pulse->duration_ms,500);
    std::array<uint8_t,haptic_packet_size> bytes{};
    ASSERT_TRUE(encode_haptic(*pulse,bytes));
    const auto decoded=decode_haptic(bytes); ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->hand,1); EXPECT_EQ(decoded->amplitude,.5f);
    EXPECT_FALSE(decode_haptic(std::span(bytes).first(bytes.size()-1)));
    bytes[0]=2; EXPECT_FALSE(decode_haptic(bytes));
    EXPECT_FALSE(bounded_haptic(2,1,1,100));
    EXPECT_FALSE(bounded_haptic(0,1,1.1f,100));
    EXPECT_FALSE(bounded_haptic(0,1,1,std::numeric_limits<float>::quiet_NaN()));
    EXPECT_TRUE(bounded_haptic(0,0,0,0));
}
