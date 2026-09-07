#include "doctest.h"
#include "../jelly.h"
#include "../voice_input.h"
#include <limits>

TEST_CASE("jelly: option voice commands are mode-aware") {
    VoiceCommandContext c; c.mode=MODE_OPTIONS;
    CHECK(parse_voice_command("jelly pieces",c)==VoiceCommand::ToggleJelly);
    CHECK(parse_voice_command("toggle jelly",c)==VoiceCommand::ToggleJelly);
    c.mode=MODE_PLAYING;
    CHECK(parse_voice_command("jelly pieces",c)==VoiceCommand::None);
}

TEST_CASE("jelly: resting shape stays unchanged") {
    JellyMotion j;
    CHECK_FALSE(j.step(.016f));
    auto p=jelly_position(j,.2f,.3f,.8f);
    CHECK(p[0]==doctest::Approx(.2f)); CHECK(p[1]==doctest::Approx(.3f));
    CHECK(p[2]==doctest::Approx(.8f));
}
TEST_CASE("jelly: a held stretch rebounds and settles") {
    JellyMotion j; j.pull(.8f,.3f,1.f);
    for(int i=0;i<120;++i) j.step(1.f/120);
    CHECK(j.offset[2]>.15f);
    CHECK(j.volume_ratio()==doctest::Approx(1.f).epsilon(.035f));
    CHECK(j.minimum_jacobian()>.17f);
    auto foot=jelly_position(j,0,0,-1);
    CHECK(foot[0]==0); CHECK(foot[2]==-1);
    j.release();
    for(int i=0;i<1200;++i) j.step(1.f/120);
    CHECK_FALSE(j.held); CHECK_FALSE(j.step(.016f));
}
TEST_CASE("jelly: extreme pointer jumps and frame stalls remain bounded") {
    JellyMotion j; j.pull(1e30f,-1e30f,-1e30f); j.impulse(1e30f,0,-1e30f);
    for(int i=0;i<12;++i) {
        j.step(20.f);
        for(float x:j.offset) { CHECK(std::isfinite(x)); CHECK(std::abs(x)<3.f); }
        CHECK(j.minimum_jacobian()>.17f);
    }
    j.pull(std::numeric_limits<float>::quiet_NaN(),0,0);
    CHECK(std::isfinite(j.target[0]));
}
TEST_CASE("jelly: collision impulses cause wobble without a grab") {
    JellyMotion j; j.impulse(2,0,-3); CHECK(j.step(.016f));
    CHECK_FALSE(j.held); CHECK(j.offset[2]<0);
}

TEST_CASE("jelly: settling is independent of render frame rate") {
    JellyMotion fast, slow;
    fast.impulse(2,1,3); slow=fast;
    for(int i=0;i<120;++i) fast.step(1.f/120);
    for(int i=0;i<30;++i) slow.step(1.f/30);
    for(int i=0;i<3;++i) CHECK(fast.offset[i]==doctest::Approx(slow.offset[i]).epsilon(.001));
    for(int i=0;i<1200;++i)slow.step(1.f/120);
    CHECK_FALSE(slow.step(.016f));
}

TEST_CASE("jelly: tetrahedral rest volume and barycentric embedding") {
    const auto& c=jelly::cage(0);
    CHECK(c.tets.size()==2592);
    CHECK(c.volume==doctest::Approx(2.88f).epsilon(.001));
    for(jelly::V p: {jelly::V{.17f,-.23f,.39f},jelly::V{.6f,.6f,1.f},jelly::V{0,0,-1.f}}) {
        auto b=c.bind(p);jelly::V q{};float sum=0;
        for(int k=0;k<4;++k){q+=c.rest[b.ids[k]]*b.w[k];sum+=b.w[k];CHECK(b.w[k]>=0);}
        CHECK(sum==doctest::Approx(1));CHECK(jelly::length(q-p)<.00001f);
    }
}
TEST_CASE("jelly: material point grabs deform locally and volume is conserved") {
    JellyMotion j;j.begin_grab(.4f,0,.5f);j.pull(.6f,.15f,.2f);
    for(int i=0;i<120;++i)j.step(1.f/120);
    CHECK(j.x.size()==jelly::NN);
    CHECK(j.position({.4f,0,.5f}).x>.5f);
    CHECK(j.minimum_jacobian()>.17f);
    CHECK(j.volume_ratio()==doctest::Approx(1).epsilon(.035));
    CHECK(jelly::length(j.position({0,0,-1})-jelly::V{0,0,-1})<.00001f);
    JellyMotion copy=j;copy.release();copy.step(.05f);
    CHECK(j.held);CHECK_FALSE(copy.held);CHECK_FALSE(j.x.empty());
}
