// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/stream_format.hpp>
#include <pocketvr/reassembler.hpp>
#include <gtest/gtest.h>
using namespace pocketvr;
namespace {
const std::vector<StreamFormat> ladder{{2160,1080,60},{1920,960,60},{1280,640,60},{1280,640,45},{1280,640,30}};
FormatFeedback sample(uint64_t seq,double encode=20) {
    FormatFeedback f;
    f.sequence=seq;f.decoded=f.accepted=seq*50;f.decode_p95_ms=4;f.encode_p95_ms=encode;
    f.fresh=f.clock_ready=f.encode_fresh=f.decode_fresh=true; f.thermal=0;return f;
}
TEST(StreamFormat, ExactCapabilitiesPreserveAspectCeilingAndCadencePriority) {
    auto sizes=nlohmann::json::array();
    for(const auto f:ladder) sizes.push_back(stream_format_json(f));
    sizes.push_back({{"width",1920},{"height",1080},{"fps",60}});
    sizes.push_back(stream_format_json(ladder[0]));
    const auto codecs=nlohmann::json::array({{{"mime","video/avc"},{"sizes",sizes}}});
    EXPECT_EQ(stream_format_ladder(codecs,"video/avc",ladder[0]),ladder);
    EXPECT_EQ(stream_format_ladder(codecs,"video/hevc",ladder[0]),std::vector<StreamFormat>{ladder[0]});
    auto bad=stream_format_json(ladder[0]);bad["width"]=4294969456ULL;
    EXPECT_THROW(parse_stream_format(bad),std::invalid_argument);
    bad["width"]=2160.0;EXPECT_THROW(parse_stream_format(bad),std::invalid_argument);
    bad["width"]=2162;EXPECT_THROW(parse_stream_format(bad),std::invalid_argument);
    bad["width"]=2160;bad["fps"]=59;EXPECT_THROW(parse_stream_format(bad),std::invalid_argument);
}
TEST(StreamFormat, SustainedPressureReducesResolutionAndSettlingPreventsOscillation) {
    StreamFormatController control(ladder);
    EXPECT_FALSE(control.update(1000,ladder[0],true,sample(1)));
    EXPECT_FALSE(control.update(2000,ladder[0],true,sample(2)));
    for(uint64_t i=3;i<7;++i) EXPECT_FALSE(control.update(i*1000,ladder[0],true,sample(i)));
    ASSERT_EQ(control.update(7000,ladder[0],true,sample(7)),ladder[1]);
    control.settled(7000);
    for(uint64_t i=8;i<17;++i) EXPECT_FALSE(control.update(i*1000,ladder[1],true,sample(i)));
    EXPECT_EQ(control.update(17000,ladder[1],true,sample(17)),ladder[2]);
    StreamFormatController decoder_pressure(ladder);
    for(uint64_t i=1;i<=4;++i) {
        auto f=sample(i,4);f.decoder_drops=i*3;
        const auto next=decoder_pressure.update(i*1000,ladder[0],true,f);
        if(i==4) EXPECT_EQ(next,ladder[1]); else EXPECT_FALSE(next);
    }
}
// Replays one simulated session at 1 Hz and counts format switches. Stage time
// follows the pixel count when scaled, which is what let the old controller
// bounce between two steps.
int switches(double decode,double encode,bool scaled,uint64_t late_per_second,uint64_t seconds) {
    StreamFormatController control(ladder);
    StreamFormat current=ladder[0];uint64_t late{},decoded{},accepted{};int count{};
    for(uint64_t i=1;i<=seconds;++i) {
        const double k=scaled?double(current.width)*current.height/(2160.0*1080):1;
        auto f=sample(i,encode*k);f.decode_p95_ms=decode*k;late+=late_per_second;f.late=late;
        // A decoder past its budget in these sessions cannot keep up with what arrives.
        accepted+=60;decoded+=f.decode_p95_ms>1000.0/current.fps?50:60;
        f.accepted=accepted;f.decoded=decoded;
        if(const auto next=control.update(i*1000,current,true,f)) {
            current=*next;++count;control.settled(i*1000);
        }
    }
    return count;
}
TEST(StreamFormat, MeasuredTimingsThatStillFitAFrameNeverBlankThePicture) {
    EXPECT_EQ(switches(14.36,13.31,false,0,600),0);
    EXPECT_EQ(switches(15.3,13.56,true,0,600),0);
    EXPECT_EQ(switches(12,12.4,false,1,600),0);
}
TEST(StreamFormat, HealthyStreamSlowerThanOneFrameNeverBlanksThePicture) {
    // Measured live on a Pixel 4a: decode p95 19-25 ms at a steady 59.8 fps, no drops.
    StreamFormatController control(ladder);
    for(uint64_t i=1;i<=600;++i) {
        auto f=sample(i,12);f.decoded=f.accepted=i*60;f.decode_p95_ms=25;
        EXPECT_FALSE(control.update(i*1000,ladder[0],true,f));
    }
    // A game running below 60 fps is not the decoder falling behind.
    StreamFormatController slow_game(ladder);
    for(uint64_t i=1;i<=600;++i) {
        auto f=sample(i,12);f.decoded=f.accepted=i*40;f.decode_p95_ms=25;
        EXPECT_FALSE(slow_game.update(i*1000,ladder[0],true,f));
    }
}
TEST(StreamFormat, OverloadedFormatIsNotRetriedInARepeatingCycle) {
    // The previous thresholds switched 21 times in five minutes on these values.
    EXPECT_EQ(switches(23.5,13.56,true,0,300),2);
    const auto ten_minutes=switches(23.5,13.56,true,0,600);
    EXPECT_EQ(switches(23.5,13.56,true,0,3600),ten_minutes);
}
TEST(StreamFormat, ThermalUsesAdvertisedRateAndRecoveryRequiresFreshSpareCapacity) {
    StreamFormatController control(ladder);
    auto f=sample(1,4);f.thermal=2;
    EXPECT_FALSE(control.update(1000,ladder[0],true,f));
    for(uint64_t i=2;i<=4;++i) {
        f=sample(i,4);f.thermal=2;
        const auto next=control.update(i*1000,ladder[0],true,f);
        if(i==4) EXPECT_EQ(next,ladder[3]); else EXPECT_FALSE(next);
    }
    control.settled(4000);
    std::optional<StreamFormat> recovered;
    for(uint64_t i=5;i<=24;++i) recovered=control.update(i*1000,ladder[3],true,sample(i,4));
    EXPECT_EQ(recovered,ladder[2]);
    StreamFormatController missing(ladder);
    for(uint64_t i=1;i<40;++i) {
        auto no_measurement=sample(i,4);no_measurement.decode_fresh=false;
        EXPECT_FALSE(missing.update(i*1000,ladder[3],true,no_measurement));
    }
    StreamFormatController stale_thermal(ladder);
    for(uint64_t i=1;i<40;++i) {
        auto cooling_unknown=sample(i,4);cooling_unknown.recovery_allowed=false;
        EXPECT_FALSE(stale_thermal.update(i*1000,ladder[3],true,cooling_unknown));
    }
}
TEST(StreamFormat, StaleIdleReplayedAndResetCountersCannotChangeFormat) {
    StreamFormatController control(ladder);
    EXPECT_FALSE(control.update(1000,ladder[0],true,sample(1)));
    for(uint64_t i=2;i<12;++i) EXPECT_FALSE(control.update(i*1000,ladder[0],true,sample(1)));
    auto f=sample(2);f.fresh=false;EXPECT_FALSE(control.update(12000,ladder[0],true,f));
    f=sample(3);f.decoded=0;EXPECT_FALSE(control.update(13000,ladder[0],true,f));
    f.sequence=4;EXPECT_FALSE(control.update(14000,ladder[0],true,f));
    EXPECT_EQ(control.update(15000,ladder[2],false,f),ladder[0]);
}
TEST(StreamFormat, NetworkDropsOnlyReduceFormatAfterBitrateFloorAndPersistence) {
    StreamFormatController control(ladder);
    for(uint64_t i=1;i<8;++i) {
        auto f=sample(i,4);f.drops=i*10;
        EXPECT_FALSE(control.update(i*1000,ladder[0],true,f));
    }
    for(uint64_t i=8;i<=12;++i) {
        auto f=sample(i,4);f.drops=i*10;f.bitrate_floor=true;
        const auto next=control.update(i*1000,ladder[0],true,f);
        if(i==12) EXPECT_EQ(next,ladder[1]); else EXPECT_FALSE(next);
    }
}
TEST(StreamFormat, ReassemblyRateChangeDiscardsPartialFramesAndPreservesCounters) {
    Reassembler assembler(4096,3);
    wire::Bytes bytes(2000,77); wire::VideoHeader header;header.frame_id=1;header.render_ns=1000;
    std::optional<FrameView> received;
    ASSERT_TRUE(packetize_frame(bytes,header,0,[&](auto h,auto payload) {
        received=assembler.push(h,payload,1000);return true;
    }));
    ASSERT_TRUE(received);ASSERT_EQ(assembler.stats().completed,1);
    header.frame_id=2;header.render_ns=2000;
    ASSERT_TRUE(packetize_frame(bytes,header,0,[&](auto h,auto payload) {
        if(!h.shard) (void)assembler.push(h,payload,2000);return true;
    }));
    assembler.format_rate(30);
    EXPECT_EQ(assembler.stats().completed,1);EXPECT_TRUE(assembler.stats().needs_idr);
    EXPECT_THROW(assembler.format_rate(0),std::invalid_argument);
}
}
