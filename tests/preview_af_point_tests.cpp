// AF frame of the development preview (#426). Synthetic bytes and a fake body only: no camera, no device.
#include "a0/phase0/experimental/preview_af_point.hpp"
#include <iostream>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
int checks=0,failures=0;
void check(bool value,const char* what){++checks;if(!value){++failures;std::cout<<"FAILED "<<what<<"\n";}}
#define CHECK(x) check((x),#x)
void PutWord(std::vector<unsigned char>& data,size_t offset,int value){
    data[offset]=static_cast<unsigned char>((value>>8)&0xff);data[offset+1]=static_cast<unsigned char>(value&0xff);
}
// A frame with the 384-byte prefix and a six byte JPEG-shaped body, and a header that fits a 640x426 picture.
std::vector<unsigned char> Payload(){
    std::vector<unsigned char> data(390,0);
    data[384]=0xff;data[385]=0xd8;data[388]=0xff;data[389]=0xd9;
    PutWord(data,8,640);PutWord(data,10,426);
    PutWord(data,12,1280);PutWord(data,14,852);
    PutWord(data,24,160);PutWord(data,26,120);
    PutWord(data,28,500);PutWord(data,30,300);
    data[48]=2;data[37]=0;
    return data;
}
void HeaderWords(){
    auto data=Payload();
    const auto words=D810PreviewAfWords(data);
    CHECK(words.has_value());
    CHECK(words->live_width==640 && words->live_height==426);
    CHECK(words->image_width==1280 && words->image_height==852);
    CHECK(words->frame_width==160 && words->frame_height==120);
    CHECK(words->center_x==500 && words->center_y==300);
    CHECK(words->focus_mark==2 && words->FocusReported());
    // Big-endian, not little-endian: a swapped word reads as another number.
    data[12]=0x00;data[13]=0x05;CHECK(D810PreviewAfWords(data)->image_width==5);
    data[12]=0x05;data[13]=0x00;CHECK(D810PreviewAfWords(data)->image_width==1280);
    // Signed: 0xFFFF is -1, 0x8000 is -32768, 0x7FFF is 32767.
    PutWord(data,28,0xffff);CHECK(D810PreviewAfWords(data)->center_x==-1);
    PutWord(data,28,0x8000);CHECK(D810PreviewAfWords(data)->center_x==-32768);
    PutWord(data,28,0x7fff);CHECK(D810PreviewAfWords(data)->center_x==32767);
    // The mark byte: the public driver reads "!= 1" as focused. Reported only, never an optical decision.
    data=Payload();data[48]=1;CHECK(!D810PreviewAfWords(data)->FocusReported());
    data[48]=0;CHECK(D810PreviewAfWords(data)->FocusReported());
    data[37]=2;CHECK(D810PreviewAfWords(data)->orientation_byte==2);
    // Without the D810 envelope there are no words at all.
    data=Payload();data[384]=0;CHECK(!D810PreviewAfWords(data));
    data=Payload();CHECK(!D810PreviewAfWords(std::span<const unsigned char>(data.data(),100)));
    CHECK(!D810PreviewAfWords(std::span<const unsigned char>{}));
}
void Validation(){
    const auto reject=[](std::vector<unsigned char> data,unsigned jw=640,unsigned jh=426){
        D810AfReject reason{};const auto frame=D810PreviewAfFrame(data,jw,jh,&reason);
        return std::pair<bool,D810AfReject>{frame.has_value(),reason};
    };
    auto good=reject(Payload());CHECK(good.first && good.second==D810AfReject::None);
    // Image size: zero, negative and 16 bit overflow are unknown.
    for(int bad:{0,-1,-32768}){
        auto data=Payload();PutWord(data,12,bad);CHECK(reject(data).second==D810AfReject::ImageSize);
        data=Payload();PutWord(data,14,bad);CHECK(reject(data).second==D810AfReject::ImageSize);
    }
    CHECK(reject(Payload(),0,426).second==D810AfReject::ImageSize);
    CHECK(reject(Payload(),640,0).second==D810AfReject::ImageSize);
    // The ratio of the header's image size to the JPEG's: 2 percent is the limit.
    {
        auto data=Payload();PutWord(data,12,1280);PutWord(data,14,852);
        CHECK(reject(data,640,426).first);CHECK(reject(data,639,426).first);CHECK(reject(data,640,430).first);
        CHECK(reject(data,640,426*2).second==D810AfReject::AspectMismatch);
        CHECK(reject(data,426,640).second==D810AfReject::AspectMismatch);
        CHECK(reject(data,640,480).second==D810AfReject::AspectMismatch); // 4:3 against 3:2
        // Just inside / just outside 2 percent of 1280/852.
        PutWord(data,12,1000);PutWord(data,14,1000);
        CHECK(reject(data,1020,1000).first);CHECK(reject(data,1021,1000).second==D810AfReject::AspectMismatch);
        CHECK(reject(data,985,1000).first);CHECK(reject(data,975,1000).second==D810AfReject::AspectMismatch);
    }
    // Frame size: zero, larger than the image.
    for(int bad:{0,-5,1281}){
        auto data=Payload();PutWord(data,24,bad);CHECK(reject(data).second==D810AfReject::FrameSize);
    }
    for(int bad:{0,-5,853}){
        auto data=Payload();PutWord(data,26,bad);CHECK(reject(data).second==D810AfReject::FrameSize);
    }
    {auto data=Payload();PutWord(data,24,1280);PutWord(data,26,852);CHECK(reject(data).first);}
    // Centre: inside [0, size).
    for(int bad:{-1,1280,32767}){auto data=Payload();PutWord(data,28,bad);CHECK(reject(data).second==D810AfReject::Center);}
    for(int bad:{-1,852,32767}){auto data=Payload();PutWord(data,30,bad);CHECK(reject(data).second==D810AfReject::Center);}
    {auto data=Payload();PutWord(data,28,0);PutWord(data,30,0);CHECK(reject(data).first);}
    {auto data=Payload();PutWord(data,28,1279);PutWord(data,30,851);CHECK(reject(data).first);}
    // The live size words are not geometry: an odd value there does not make the frame unknown.
    {auto data=Payload();PutWord(data,8,0);PutWord(data,10,-3);CHECK(reject(data).first);}
    // No envelope: unknown.
    {auto data=Payload();data[385]=0;D810AfReject reason{};CHECK(!D810PreviewAfFrame(data,640,426,&reason) && reason!=D810AfReject::None);}
    // All-zero header, as a body that does not fill the words would send.
    {auto data=Payload();for(size_t i=0;i<384;++i)data[i]=0;CHECK(!reject(data).first);}
}
void Display(){
    D810AfHeader h;h.image_width=1280;h.image_height=852;h.frame_width=160;h.frame_height=120;h.center_x=640;h.center_y=426;
    const PreviewAfRect picture{100,200,640,426};
    const auto rect=PreviewAfFrameOnDisplay(h,picture);
    CHECK(rect && rect->left==100+280 && rect->top==200+183 && rect->width==80 && rect->height==60);
    // The whole-picture frame maps to the whole picture, in any picture size.
    h.frame_width=1280;h.frame_height=852;
    const auto whole=PreviewAfFrameOnDisplay(h,picture);
    CHECK(whole && whole->left==100 && whole->top==200 && whole->width==640 && whole->height==426);
    CHECK(!PreviewAfFrameOnDisplay(h,PreviewAfRect{0,0,0,10}));
    CHECK(!PreviewAfFrameOnDisplay(h,PreviewAfRect{0,0,10,0}));
    D810AfHeader empty;CHECK(!PreviewAfFrameOnDisplay(empty,picture));
}
void LogThrottle(){
    using Clock=std::chrono::steady_clock;
    const auto t0=Clock::time_point{}+std::chrono::seconds(100);
    PreviewAfLogThrottle throttle;
    auto a=D810PreviewAfWords(Payload());auto b=a;b->center_x+=10;
    CHECK(throttle.ShouldLog(a,D810AfReject::None,t0));                                 // the first frame
    CHECK(!throttle.ShouldLog(a,D810AfReject::None,t0+std::chrono::milliseconds(20)));   // unchanged
    CHECK(!throttle.ShouldLog(b,D810AfReject::None,t0+std::chrono::milliseconds(500)));  // changed, but within a second
    CHECK(throttle.ShouldLog(b,D810AfReject::None,t0+std::chrono::milliseconds(1100)));  // changed, a second later
    CHECK(!throttle.ShouldLog(b,D810AfReject::None,t0+std::chrono::seconds(4)));         // unchanged: heartbeat not due
    CHECK(throttle.ShouldLog(b,D810AfReject::None,t0+std::chrono::milliseconds(6200)));  // heartbeat after five seconds
    // A frame that becomes unusable is a change too.
    CHECK(throttle.ShouldLog(b,D810AfReject::Center,t0+std::chrono::seconds(8)));
    // A thousand frames at 50 per second produce a handful of lines, not a thousand.
    PreviewAfLogThrottle busy;unsigned lines{};auto words=a;
    for(int i=0;i<1000;++i){words->center_x=500+(i%7);if(busy.ShouldLog(words,D810AfReject::None,t0+std::chrono::milliseconds(20*i)))++lines;}
    CHECK(lines<=21);
    const auto text=DescribeD810AfWords(a,D810AfReject::None);
    CHECK(text.find("usable=1")!=std::string::npos && text.find("commands_sent=0")!=std::string::npos);
    CHECK(DescribeD810AfWords({},D810AfReject::ImageSize)=="af_header_words=unavailable");
}
}
int main(){
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    HeaderWords();Validation();Display();LogThrottle();
    CoUninitialize();
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
