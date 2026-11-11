// AF frame of the development preview (#426). Synthetic bytes and a fake body only: no camera, no device.
#include "a0/phase0/experimental/preview_af_point.hpp"
#include <iostream>
#include <vector>
using namespace a0::phase0::experimental;
namespace {
int checks=0,failures=0;
void check(bool value,const char* what){++checks;if(!value){++failures;std::cout<<"FAILED "<<what<<"\n";}}
#define CHECK(...) check((__VA_ARGS__),#__VA_ARGS__)
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
// ---- stage 2 ------------------------------------------------------------------------------------
using Clock=PreviewAfClock;
const Clock::time_point kT0=Clock::time_point{}+std::chrono::seconds(1000);
WpdValues Reply(bool final_reply=false,DWORD mtp=0x2001){
    WpdValues result;
    CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&result));
    result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT,S_OK);
    if(final_reply)result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE,mtp);
    return result;
}
WpdLiveViewExchange FinalReply(DWORD mtp=0x2001){return {S_OK,Reply(true,mtp)};}
WpdLiveViewExchange CommandFailedReply(){auto result=Reply();result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT,E_FAIL);return {S_OK,result};}
// A request of the without-data-phase form with an operation and typed 32 bit arguments.
WpdValues Request(DWORD opcode,std::initializer_list<std::pair<VARTYPE,DWORD>> arguments){
    auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start);
    request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,opcode);
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
    request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
    args->Clear();
    for(const auto& [type,number]:arguments){PROPVARIANT value{};value.vt=type;value.ulVal=number;args->Add(&value);}
    return request;
}
WpdValues WithCommand(WpdValues request,const PROPERTYKEY& command){
    request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,command.fmtid);
    request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,command.pid);
    return request;
}
void Gate(){
    const PreviewAfBounds bounds{1280,852,kT0};
    const auto now=kT0+std::chrono::milliseconds(100);
    const auto allowed=[&](const WpdValues& request){return AllowedPreviewAfPointRequest(request.Get(),bounds,now);};
    // What is admitted: 0x9205, two UINT32 values, inside the bounds.
    CHECK(allowed(BuildPreviewAfPointRequest(0,0)));
    CHECK(allowed(BuildPreviewAfPointRequest(640,426)));
    CHECK(allowed(BuildPreviewAfPointRequest(1279,851)));
    CHECK(allowed(BuildPreviewAfPointRequest(1279,0)));
    CHECK(allowed(BuildPreviewAfPointRequest(0,851)));
    // The range is exclusive at the size, on each axis, and 32 bit values do not wrap.
    CHECK(!allowed(BuildPreviewAfPointRequest(1280,0)));
    CHECK(!allowed(BuildPreviewAfPointRequest(0,852)));
    CHECK(!allowed(BuildPreviewAfPointRequest(1280,852)));
    CHECK(!allowed(BuildPreviewAfPointRequest(0xffffffffu,0)));
    CHECK(!allowed(BuildPreviewAfPointRequest(0,0xffffffffu)));
    CHECK(!allowed(BuildPreviewAfPointRequest(0x10000,0)));
    // Type and count: exactly two UINT32.
    CHECK(allowed(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}})));
    CHECK(!allowed(Request(0x9205,{{VT_I4,5},{VT_UI4,6}})));
    // (The WPD collection keeps one type for all its items, so the type of the first value is the type of both.)
    CHECK(!allowed(Request(0x9205,{{VT_I4,5},{VT_I4,6}})));
    CHECK(!allowed(Request(0x9205,{{VT_UI2,5},{VT_UI2,6}})));
    CHECK(!allowed(Request(0x9205,{{VT_UI1,5},{VT_UI1,6}})));
    CHECK(!allowed(Request(0x9205,{})));
    CHECK(!allowed(Request(0x9205,{{VT_UI4,5}})));
    CHECK(!allowed(Request(0x9205,{{VT_UI4,5},{VT_UI4,6},{VT_UI4,0}})));
    // Every other operation is refused with otherwise acceptable arguments: the neighbours of the
    // AF point operation, the Live View and focus operations, capture and property writes.
    for(DWORD opcode:{0x9206u,0x9207u,0x90c2u,0x90c0u,0x90c1u,0x90c7u,0x90c8u,0x9204u,0x9201u,0x9202u,0x9203u,0x1015u,0x1016u,0x1014u,0x100eu,0x0000u,0x9a05u,0x92050u}){
        CHECK(!allowed(Request(opcode,{{VT_UI4,5},{VT_UI4,6}})));
        CHECK(!allowed(Request(opcode,{})));
    }
    // Other command forms are refused even with 0x9205 and good arguments.
    CHECK(!allowed(WithCommand(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}}),WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ)));
    CHECK(!allowed(WithCommand(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}}),WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE)));
    CHECK(!allowed(WithCommand(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}}),WPD_COMMAND_MTP_EXT_READ_DATA)));
    CHECK(!allowed(WithCommand(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}}),WPD_COMMAND_MTP_EXT_WRITE_DATA)));
    CHECK(!allowed(WithCommand(Request(0x9205,{{VT_UI4,5},{VT_UI4,6}}),WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER)));
    CHECK(!allowed(BuildWpdLiveViewRead(L"test",16)));
    CHECK(!allowed(BuildWpdLiveViewEndTransfer(L"test")));
    CHECK(!AllowedPreviewAfPointRequest(nullptr,bounds,now));
    // Nothing the Live View allowlist admits passes here.
    for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far})CHECK(!allowed(BuildPreviewFocusRequest(action)));
    CHECK(!allowed(BuildPreviewDeviceReadyRequest()));
    CHECK(!allowed(D810PreviewStatusRequest(0xd061)));
    for(auto op:{WpdLiveViewOperation::Start,WpdLiveViewOperation::Frame,WpdLiveViewOperation::Stop})CHECK(!allowed(BuildWpdLiveViewOperation(op)));
    // The bounds: missing, zero, over 16 bit, stale, from the future.
    const auto request=BuildPreviewAfPointRequest(10,10);
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),std::nullopt,now));
    CHECK(AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,852,kT0},kT0));
    CHECK(AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,852,kT0},kT0+std::chrono::milliseconds(1999)));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,852,kT0},kT0+std::chrono::seconds(2)));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,852,kT0},kT0-std::chrono::milliseconds(1)));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{0,852,kT0},kT0));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,0,kT0},kT0));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{0x8000,852,kT0},kT0));
    CHECK(!AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{1280,0x8000,kT0},kT0));
    CHECK(AllowedPreviewAfPointRequest(request.Get(),PreviewAfBounds{0x7fff,0x7fff,kT0},kT0));
    CHECK(!AllowedPreviewAfPointRequest(BuildPreviewAfPointRequest(11,10).Get(),PreviewAfBounds{11,852,kT0},kT0));
    // Session ownership, a stop request and the deadline.
    CHECK(AdmitPreviewAfPointDispatch(request.Get(),bounds,now,true,false,false));
    CHECK(!AdmitPreviewAfPointDispatch(request.Get(),bounds,now,false,false,false));
    CHECK(!AdmitPreviewAfPointDispatch(request.Get(),bounds,now,true,true,false));
    CHECK(!AdmitPreviewAfPointDispatch(request.Get(),bounds,now,true,false,true));
    CHECK(!AdmitPreviewAfPointDispatch(request.Get(),bounds,kT0+std::chrono::seconds(3),true,false,false));
    // The Live View allowlist still refuses 0x9205, with good arguments, and so does its dispatch admission.
    CHECK(!AllowedD810PreviewRequest(request.Get()));
    CHECK(!AdmitD810PreviewDispatch(request.Get(),true,false,false));
    // Bounds from a header.
    D810AfHeader h;h.image_width=1280;h.image_height=852;
    const auto made=PreviewAfBoundsFrom(h,kT0);
    CHECK(made && made->width==1280 && made->height==852 && made->at==kT0);
    CHECK(!PreviewAfBoundsFrom(std::nullopt,kT0));
    CHECK(PreviewAfBoundsUsable(made,kT0) && !PreviewAfBoundsUsable(made,kT0+std::chrono::seconds(2)));
}
void Conversion(){
    D810AfHeader h;h.image_width=1280;h.image_height=852;h.frame_width=160;h.frame_height=120;
    const PreviewAfRect picture{100,200,640,426};
    const auto at=[&](int x,int y){return PreviewAfPointFromClick(picture,x,y,h);};
    auto centre=at(420,413);CHECK(centre && centre->x==641 && centre->y==427 && centre->width==1280 && centre->height==852);
    // The corners are moved inside by half the frame, so the frame lies wholly in the image.
    auto top_left=at(100,200);CHECK(top_left && top_left->x==80 && top_left->y==60);
    auto bottom_right=at(739,625);CHECK(bottom_right && bottom_right->x==1200 && bottom_right->y==792);
    auto top_right=at(739,200);CHECK(top_right && top_right->x==1200 && top_right->y==60);
    auto bottom_left=at(100,625);CHECK(bottom_left && bottom_left->x==80 && bottom_left->y==792);
    // The margin and the outside are not points.
    CHECK(!at(99,300) && !at(740,300) && !at(300,199) && !at(300,626) && !at(-5,-5) && !at(5000,5000));
    // A frame as large as the image leaves exactly one place; an odd size rounds the same way on each side.
    h.frame_width=1280;h.frame_height=852;auto whole=at(150,250);CHECK(whole && whole->x==640 && whole->y==426);
    h.frame_width=161;h.frame_height=121;auto odd=at(100,200);CHECK(odd && odd->x==80 && odd->y==60);
    auto odd_far=at(739,625);CHECK(odd_far && odd_far->x==1280-81 && odd_far->y==852-61);
    h.frame_width=160;h.frame_height=120;
    // A frame larger than the image, or a header without a space, gives nothing.
    {auto bad=h;bad.frame_width=1281;CHECK(!PreviewAfPointFromClick(picture,300,300,bad));}
    {auto bad=h;bad.image_width=0;CHECK(!PreviewAfPointFromClick(picture,300,300,bad));}
    {auto bad=h;bad.image_height=0x8000;CHECK(!PreviewAfPointFromClick(picture,300,300,bad));}
    CHECK(!PreviewAfPointFromClick(PreviewAfRect{100,200,0,426},300,300,h));
    // Every pixel of several picture sizes: the result is valid, the frame stays inside, unclamped points come
    // back to within a display pixel, and the order of the clicked pixels is kept.
    for(const PreviewAfRect& p:{PreviewAfRect{100,200,640,426},PreviewAfRect{12,174,447,298},PreviewAfRect{0,0,1280,852},PreviewAfRect{7,9,321,213}}){
        int previous_x=-1;bool monotonic=true,inside=true,valid=true,back=true;
        for(int dx=0;dx<p.width;++dx){
            const auto r=PreviewAfPointFromClick(p,p.left+dx,p.top+p.height/2,h);
            if(!r || !r->Plausible()){valid=false;continue;}
            if(r->x<previous_x)monotonic=false;previous_x=r->x;
            if(r->x-h.frame_width/2<0 || r->x-h.frame_width/2+h.frame_width>h.image_width)inside=false;
            const int shown=p.left+static_cast<int>(std::lround(static_cast<double>(r->x)*p.width/h.image_width));
            if(r->x>h.frame_width/2 && r->x<h.image_width-(h.frame_width-h.frame_width/2) && std::abs(shown-(p.left+dx))>1)back=false;
        }
        for(int dy=0;dy<p.height;++dy){
            const auto r=PreviewAfPointFromClick(p,p.left+p.width/2,p.top+dy,h);
            if(!r || !r->Plausible()){valid=false;continue;}
            if(r->y-h.frame_height/2<0 || r->y-h.frame_height/2+h.frame_height>h.image_height)inside=false;
            const int shown=p.top+static_cast<int>(std::lround(static_cast<double>(r->y)*p.height/h.image_height));
            if(r->y>h.frame_height/2 && r->y<h.image_height-(h.frame_height-h.frame_height/2) && std::abs(shown-(p.top+dy))>1)back=false;
        }
        CHECK(valid);CHECK(inside);CHECK(monotonic);CHECK(back);
    }
}
// A fake body for the flow: records what is sent, answers as configured, and puts every request through the
// same dedicated gate the camera uses.
struct FakeBody {
    DWORD mode{0};DWORD response{0x2001};bool transport_failure{},command_failure{};
    bool ready_fails{};bool stop{};
    std::optional<PreviewAfBounds> bounds{PreviewAfBounds{1280,852,kT0}};
    Clock::time_point now{kT0+std::chrono::milliseconds(100)};
    Clock::time_point gate_now{kT0+std::chrono::milliseconds(100)};
    unsigned ready_calls{},mode_reads{};
    std::vector<DWORD> sent;std::vector<DWORD> arguments;
    PreviewAfPointHooks Hooks(){
        PreviewAfPointHooks hooks;
        hooks.wait_ready=[this](DWORD* completion){++ready_calls;if(completion)*completion=0x2001;return !ready_fails;};
        hooks.read_focus_mode=[this]{++mode_reads;return mode;};
        hooks.bounds=[this]{return bounds;};
        hooks.now=[this]{return now;};
        hooks.stopping=[this]{return stop;};
        hooks.send=[this](IPortableDeviceValues* request)->WpdLiveViewExchange{
            DWORD opcode{};request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&opcode);sent.push_back(opcode);
            if(!AdmitPreviewAfPointDispatch(request,bounds,gate_now,true,stop,false))throw std::runtime_error("not admitted");
            Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
            request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args);
            DWORD count{};args->GetCount(&count);
            for(DWORD i=0;i<count;++i){PROPVARIANT v{};args->GetAt(i,&v);arguments.push_back(v.ulVal);PropVariantClear(&v);}
            if(transport_failure)return {E_FAIL,nullptr};
            if(command_failure)return CommandFailedReply();
            return FinalReply(response);
        };
        return hooks;
    }
};
void Flow(){
    const PreviewAfPointRequest point{600,400,1280,852};
    // Accepted: one 0x9205 with the two values, readiness before and after, nothing else, and no AF.
    {
        FakeBody body;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::Placed && result.dispatches==1 && result.command.Ok());
        CHECK(body.sent==std::vector<DWORD>{0x9205} && body.arguments==std::vector<DWORD>{600,400});
        CHECK(body.ready_calls==2 && body.mode_reads==1 && result.ready_confirmed);
    }
    // Refused by the body, each with its own code kept: shown, readiness settled, no second try, no AF.
    for(DWORD code:{0xa00bu,0xa004u,0xa002u,0x2002u,0x2019u}){
        FakeBody body;body.response=code;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::RefusedByBody && result.command.mtp_response==code && result.dispatches==1);
        CHECK(body.sent.size()==1 && body.sent[0]==0x9205 && body.ready_calls==2);
    }
    // Transport failure in both shapes: the result is not known, so nothing else is sent.
    {
        FakeBody body;body.transport_failure=true;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::Uncertain && result.dispatches==1 && body.sent.size()==1 && body.ready_calls==1);
    }
    {
        FakeBody body;body.command_failure=true;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::Uncertain && result.dispatches==1 && body.sent.size()==1 && body.ready_calls==1);
    }
    // The focus mode: AF-S and AF-F send; manual focus and the rest do not, and say why.
    for(DWORD mode:{0u,2u}){FakeBody body;body.mode=mode;CHECK(RunPreviewAfPoint(point,body.Hooks()).outcome==PreviewAfPointOutcome::Placed && body.sent.size()==1);}
    for(DWORD mode:{3u,4u}){
        FakeBody body;body.mode=mode;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::Blocked && result.admission==PreviewFocusAdmission::NeedsAfSwitch && result.mode==mode);
        CHECK(body.sent.empty() && result.dispatches==0);
    }
    for(DWORD mode:{1u,5u,99u,0xffffffffu}){
        FakeBody body;body.mode=mode;const auto result=RunPreviewAfPoint(point,body.Hooks());
        CHECK(result.outcome==PreviewAfPointOutcome::Blocked && result.admission==PreviewFocusAdmission::UnsupportedMode && body.sent.empty());
    }
    // Interrupted before anything: no mode read, no send.
    {FakeBody body;body.ready_fails=true;const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.outcome==PreviewAfPointOutcome::NotSent && r.not_sent==PreviewAfPointNotSent::Interrupted && body.mode_reads==0 && body.sent.empty());}
    {FakeBody body;body.stop=true;const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.outcome==PreviewAfPointOutcome::NotSent && r.not_sent==PreviewAfPointNotSent::Interrupted && body.sent.empty());}
    // The bounds: missing, stale, from the future, a different space than the click's.
    {FakeBody body;body.bounds.reset();const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.not_sent==PreviewAfPointNotSent::BoundsUnusable && body.sent.empty());}
    {FakeBody body;body.now=kT0+std::chrono::seconds(2);const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.not_sent==PreviewAfPointNotSent::BoundsUnusable && body.sent.empty());}
    {FakeBody body;body.now=kT0-std::chrono::milliseconds(5);const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.not_sent==PreviewAfPointNotSent::BoundsUnusable && body.sent.empty());}
    {FakeBody body;body.bounds=PreviewAfBounds{1024,852,kT0};const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.not_sent==PreviewAfPointNotSent::BoundsChanged && body.sent.empty());}
    {FakeBody body;body.bounds=PreviewAfBounds{1280,768,kT0};const auto r=RunPreviewAfPoint(point,body.Hooks());
        CHECK(r.not_sent==PreviewAfPointNotSent::BoundsChanged && body.sent.empty());}
    // A position outside its own space is never built into a request.
    for(const PreviewAfPointRequest& bad:{PreviewAfPointRequest{1280,0,1280,852},PreviewAfPointRequest{0,852,1280,852},PreviewAfPointRequest{0,0,0,852},PreviewAfPointRequest{0,0,1280,0}}){
        FakeBody body;const auto r=RunPreviewAfPoint(bad,body.Hooks());CHECK(r.outcome==PreviewAfPointOutcome::NotSent && body.sent.empty() && body.ready_calls==0);
    }
    // The gate at the last boundary refuses what became stale after the flow's own check: the error
    // reaches the caller (the worker stops), and the request is not repeated.
    {FakeBody body;body.gate_now=kT0+std::chrono::seconds(3);bool thrown=false;
        try{RunPreviewAfPoint(point,body.Hooks());}catch(const std::runtime_error&){thrown=true;}
        CHECK(thrown && body.sent.size()==1);}
    // A hundred runs never send anything but one 0x9205 each.
    {
        FakeBody body;unsigned placed{};
        for(int i=0;i<100;++i)if(RunPreviewAfPoint(PreviewAfPointRequest{static_cast<std::uint16_t>(i),static_cast<std::uint16_t>(i),1280,852},body.Hooks()).outcome==PreviewAfPointOutcome::Placed)++placed;
        CHECK(placed==100 && body.sent.size()==100);
        bool only_af_point=true;for(auto opcode:body.sent)only_af_point=only_af_point && opcode==0x9205;CHECK(only_af_point);
    }
}
}
int main(){
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    // An exception inside a section is a failed check, not a crash.
    for(void (*section)():{HeaderWords,Validation,Display,LogThrottle,Gate,Conversion,Flow}){
        try{section();}catch(const std::exception& error){check(false,error.what());}
    }
    CoUninitialize();
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
