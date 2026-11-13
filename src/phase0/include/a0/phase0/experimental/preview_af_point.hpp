#pragma once
// AF frame of the development Live View preview (#426).
//
// Stage 1 (this part): read the AF frame words of the 384-byte frame header and
// decide whether they can be shown. Nothing here sends a command.
//
// Public source: digiCamControl NikonD600Base.GetAdditionalLiveViewData (D810
// ancestor), fixed commit in third_party/PTP_PROTOCOL_REFERENCES.md. The words
// are signed big-endian (BaseMTPCamera.ToInt16), at the same offsets as the
// level words in d810_preview.hpp. Whether a D810 fills them as that driver
// expects is not established (unverified): every value is validated, and anything
// that does not fit is treated as unknown rather than drawn.
#include "a0/phase0/experimental/d810_preview.hpp"
#include "a0/phase0/experimental/preview_focus.hpp"
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace a0::phase0::experimental {
// Words of the frame header that describe the AF frame. The image size is the
// coordinate space of the frame centre; it is not the JPEG size.
struct D810AfHeader {
    int live_width{},live_height{};   // offsets 8, 10
    int image_width{},image_height{}; // offsets 12, 14
    int frame_width{},frame_height{}; // offsets 24, 26: size of the AF frame
    int center_x{},center_y{};        // offsets 28, 30: centre of the AF frame
    unsigned focus_mark{};            // byte 48; the public driver reads "!= 1" as focused
    unsigned orientation_byte{};      // byte 37; logged only
    bool operator==(const D810AfHeader&) const = default;
    // The body's own report. Not an optical focus decision.
    [[nodiscard]] bool FocusReported() const { return focus_mark != 1; }
};
inline constexpr int D810AfMaxExtent=0x7fff; // the words are signed 16 bit
// Raw words, only when the payload has the D810 envelope. Validity is separate.
inline std::optional<D810AfHeader> D810PreviewAfWords(std::span<const unsigned char> payload){
    try{D810PreviewJpeg(payload);}catch(const std::runtime_error&){return {};}
    const auto word=[&](size_t offset){
        const int value=(static_cast<int>(payload[offset])<<8)|payload[offset+1];
        return value>=32768?value-65536:value;
    };
    D810AfHeader header;
    header.live_width=word(8);header.live_height=word(10);
    header.image_width=word(12);header.image_height=word(14);
    header.frame_width=word(24);header.frame_height=word(26);
    header.center_x=word(28);header.center_y=word(30);
    header.focus_mark=payload[48];header.orientation_byte=payload[37];
    return header;
}
enum class D810AfReject { None, ImageSize, AspectMismatch, FrameSize, Center };
inline const char* D810AfRejectName(D810AfReject reason){
    switch(reason){
    case D810AfReject::None:return "none";
    case D810AfReject::ImageSize:return "image_size";
    case D810AfReject::AspectMismatch:return "aspect_mismatch";
    case D810AfReject::FrameSize:return "frame_size";
    case D810AfReject::Center:return "center";
    }
    return "unknown";
}
// The aspect ratio of the header's image size may differ from the JPEG's by at
// most 2 percent; the live size words are not part of the geometry and are not
// judged. A frame larger than the image, or a centre outside it, is unknown.
inline D810AfReject ValidateD810AfHeader(const D810AfHeader& header,unsigned jpeg_width,unsigned jpeg_height){
    if(header.image_width<1 || header.image_height<1 || header.image_width>D810AfMaxExtent || header.image_height>D810AfMaxExtent)
        return D810AfReject::ImageSize;
    if(!jpeg_width || !jpeg_height)return D810AfReject::ImageSize;
    const std::int64_t left=static_cast<std::int64_t>(header.image_width)*jpeg_height;
    const std::int64_t right=static_cast<std::int64_t>(jpeg_width)*header.image_height;
    if((left>right?left-right:right-left)*50>right)return D810AfReject::AspectMismatch;
    if(header.frame_width<1 || header.frame_height<1 || header.frame_width>header.image_width || header.frame_height>header.image_height)
        return D810AfReject::FrameSize;
    if(header.center_x<0 || header.center_y<0 || header.center_x>=header.image_width || header.center_y>=header.image_height)
        return D810AfReject::Center;
    return D810AfReject::None;
}
// A header that can be drawn and used as a coordinate space, or nothing.
inline std::optional<D810AfHeader> D810PreviewAfFrame(std::span<const unsigned char> payload,unsigned jpeg_width,unsigned jpeg_height,D810AfReject* reason=nullptr){
    const auto words=D810PreviewAfWords(payload);
    const auto verdict=words?ValidateD810AfHeader(*words,jpeg_width,jpeg_height):D810AfReject::ImageSize;
    if(reason)*reason=verdict;
    if(!words || verdict!=D810AfReject::None)return {};
    return words;
}

// A rectangle in display pixels.
struct PreviewAfRect {int left{},top{},width{},height{};};
// Where the header's frame lies inside the picture drawn into `picture`.
// Same space as the click conversion below: header image size -> display.
inline std::optional<PreviewAfRect> PreviewAfFrameOnDisplay(const D810AfHeader& header,const PreviewAfRect& picture){
    if(picture.width<=0 || picture.height<=0 || header.image_width<1 || header.image_height<1)return {};
    const auto scale_x=[&](double value){return picture.left+static_cast<int>(std::lround(value*picture.width/header.image_width));};
    const auto scale_y=[&](double value){return picture.top+static_cast<int>(std::lround(value*picture.height/header.image_height));};
    const double half_w=header.frame_width/2.0,half_h=header.frame_height/2.0;
    const int left=scale_x(header.center_x-half_w),right=scale_x(header.center_x+half_w);
    const int top=scale_y(header.center_y-half_h),bottom=scale_y(header.center_y+half_h);
    return PreviewAfRect{left,top,std::max(1,right-left),std::max(1,bottom-top)};
}

// Diagnostic log throttle: the first frame, a change (at most once a second)
// and a heartbeat every five seconds. The values are on-screen coordinates of
// the body's own frame, not identifiers.
class PreviewAfLogThrottle final {
public:
    using Clock=std::chrono::steady_clock;
    bool ShouldLog(const std::optional<D810AfHeader>& words,D810AfReject verdict,Clock::time_point now){
        const bool same=started_ && words==last_words_ && verdict==last_verdict_;
        if(started_){
            const auto elapsed=now-last_log_;
            if(same ? elapsed<std::chrono::seconds(5) : elapsed<std::chrono::seconds(1))return false;
        }
        started_=true;last_words_=words;last_verdict_=verdict;last_log_=now;return true;
    }
private:
    bool started_{};std::optional<D810AfHeader> last_words_;D810AfReject last_verdict_{};Clock::time_point last_log_{};
};
// The position the operator last asked for, in the header's space, for the red outline.
struct PreviewAfMark {unsigned x{},y{},width{},height{};std::chrono::steady_clock::time_point at{};};
// What one camera knows about its AF frame. The owner guards it with its own
// mutex, except `log`, which only the frame-reading worker touches.
struct PreviewAfState {
    std::optional<D810AfHeader> frame;           // usable header of the latest frame, else unknown
    std::chrono::steady_clock::time_point frame_at{};
    D810AfReject reject{D810AfReject::ImageSize}; // why `frame` is unknown
    std::optional<PreviewAfMark> request;        // stage 2: the last accepted click
    PreviewAfLogThrottle log;
};
inline std::string DescribeD810AfWords(const std::optional<D810AfHeader>& words,D810AfReject verdict){
    if(!words)return "af_header_words=unavailable";
    const auto& h=*words;
    return "af_header_words live="+std::to_string(h.live_width)+"x"+std::to_string(h.live_height)+
        " image="+std::to_string(h.image_width)+"x"+std::to_string(h.image_height)+
        " frame="+std::to_string(h.frame_width)+"x"+std::to_string(h.frame_height)+
        " center="+std::to_string(h.center_x)+","+std::to_string(h.center_y)+
        " mark="+std::to_string(h.focus_mark)+" orientation_byte="+std::to_string(h.orientation_byte)+
        " usable="+(verdict==D810AfReject::None?"1":"0")+" reject="+D810AfRejectName(verdict)+" commands_sent=0";
}

// ---------------------------------------------------------------------------------------------
// Stage 2: moving the AF frame once per click.
//
// ChangeAfArea 0x9205 takes two UINT32 parameters (x, then y) and has no data phase. Public sources
// and their lines are in third_party/PTP_PROTOCOL_REFERENCES.md. That a D810 accepts it, the origin
// of the coordinates and whether the point is the centre or a corner of the frame are not
// established (unverified): the point is the centre because the public driver draws the frame
// around FocusX/FocusY, and the answer of the body is shown back as the green frame.
//
// This request is NOT admitted by AllowedD810PreviewRequest (that allowlist is unchanged and its
// test still expects a refusal). Only the dedicated gate below admits it, and nothing else.
// ---------------------------------------------------------------------------------------------
using PreviewAfClock=std::chrono::steady_clock;
inline constexpr auto PreviewAfBoundsMaxAge=std::chrono::seconds(2);

// The coordinate space of the latest usable frame header, with the time it was read.
struct PreviewAfBounds {unsigned width{},height{};PreviewAfClock::time_point at{};};
inline std::optional<PreviewAfBounds> PreviewAfBoundsFrom(const std::optional<D810AfHeader>& frame,PreviewAfClock::time_point at){
    if(!frame)return {};
    return PreviewAfBounds{static_cast<unsigned>(std::max(0,frame->image_width)),static_cast<unsigned>(std::max(0,frame->image_height)),at};
}
// Missing, zero, wider than a signed 16 bit word, older than two seconds or from the future: unusable.
inline bool PreviewAfBoundsUsable(const std::optional<PreviewAfBounds>& bounds,PreviewAfClock::time_point now){
    return bounds && bounds->width>=1 && bounds->height>=1 && bounds->width<=static_cast<unsigned>(D810AfMaxExtent) &&
        bounds->height<=static_cast<unsigned>(D810AfMaxExtent) && now>=bounds->at && now-bounds->at<PreviewAfBoundsMaxAge;
}
// A click on the picture drawn into `picture`, as a point in the header's space. The picture is the
// part of the pane that shows the camera's frame: a click on the margin is not a point. The point is
// the centre of the frame, so it is moved inside by half the frame size; the frame then lies wholly
// in the image. The header must already be usable (D810PreviewAfFrame).
inline std::optional<PreviewAfPointRequest> PreviewAfPointFromClick(const PreviewAfRect& picture,int click_x,int click_y,const D810AfHeader& header){
    if(picture.width<=0 || picture.height<=0 || header.image_width<1 || header.image_height<1 ||
        header.image_width>D810AfMaxExtent || header.image_height>D810AfMaxExtent)return {};
    if(click_x<picture.left || click_x>=picture.left+picture.width || click_y<picture.top || click_y>=picture.top+picture.height)return {};
    // The middle of the clicked display pixel, in header units.
    const auto convert=[](std::int64_t offset,std::int64_t display,std::int64_t space){return ((2*offset+1)*space)/(2*display);};
    std::int64_t x=convert(click_x-picture.left,picture.width,header.image_width);
    std::int64_t y=convert(click_y-picture.top,picture.height,header.image_height);
    const std::int64_t low_x=header.frame_width/2,high_x=header.image_width-(header.frame_width-header.frame_width/2);
    const std::int64_t low_y=header.frame_height/2,high_y=header.image_height-(header.frame_height-header.frame_height/2);
    if(low_x>high_x || low_y>high_y)return {};
    x=std::clamp(x,low_x,high_x);y=std::clamp(y,low_y,high_y);
    PreviewAfPointRequest request{static_cast<std::uint16_t>(x),static_cast<std::uint16_t>(y),
        static_cast<std::uint16_t>(header.image_width),static_cast<std::uint16_t>(header.image_height)};
    if(!request.Plausible())return {};
    return request;
}

inline WpdValues BuildPreviewAfPointRequest(DWORD x,DWORD y){
    auto request=BuildWpdLiveViewOperation(WpdLiveViewOperation::Start); // EXECUTE_COMMAND_WITHOUT_DATA_PHASE, no parameters yet
    if(request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,0x9205)!=S_OK)throw std::runtime_error("af point opcode");
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> args;
    if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&args)!=S_OK)throw std::runtime_error("af point args");
    PROPVARIANT value{};value.vt=VT_UI4;value.ulVal=x;
    if(args->Add(&value)!=S_OK)throw std::runtime_error("af point x");
    value.ulVal=y;
    if(args->Add(&value)!=S_OK)throw std::runtime_error("af point y");
    return request;
}
// The dedicated allowlist. It admits one thing: operation 0x9205 as a command without a data phase,
// with exactly two UINT32 parameters, x < width and y < height of the bounds read from a frame header
// less than two seconds ago. Every other request, including the ones the Live View allowlist admits,
// is refused here.
inline bool AllowedPreviewAfPointRequest(IPortableDeviceValues* request,const std::optional<PreviewAfBounds>& bounds,PreviewAfClock::time_point now){
    if(!request || !PreviewAfBoundsUsable(bounds,now))return false;
    GUID category{};DWORD id{},opcode{};
    if(request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY,&category)!=S_OK ||
        request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id)!=S_OK)return false;
    const PROPERTYKEY command{category,id};
    if(!IsEqualPropertyKey(command,WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE))return false;
    if(request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&opcode)!=S_OK || opcode!=0x9205)return false;
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    DWORD count{};
    if(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,&parameters)!=S_OK ||
        !parameters || parameters->GetCount(&count)!=S_OK || count!=2)return false;
    PROPVARIANT x{},y{};
    const auto first=parameters->GetAt(0,&x),second=parameters->GetAt(1,&y);
    const bool allowed=first==S_OK && second==S_OK && x.vt==VT_UI4 && y.vt==VT_UI4 && x.ulVal<bounds->width && y.ulVal<bounds->height;
    PropVariantClear(&x);PropVariantClear(&y);return allowed;
}
// Session ownership, stop request and the I/O deadline, as for the other Live View operations: a
// request is never sent for a camera this process does not own, after a stop was asked for, or
// after the deadline.
inline bool AdmitPreviewAfPointDispatch(IPortableDeviceValues* request,const std::optional<PreviewAfBounds>& bounds,
    PreviewAfClock::time_point now,bool owned,bool stop_requested,bool expired){
    return owned && !stop_requested && !expired && AllowedPreviewAfPointRequest(request,bounds,now);
}

// One click, one try. The steps and what each leaves unsent:
//   DeviceReady -> read 0xD061 -> AF-capable value only -> bounds still usable and unchanged ->
//   0x9205 exactly once -> DeviceReady for the completion.
// Nothing is retried, no AF (0x90C1) follows, and a result that is not known ends the flow.
enum class PreviewAfPointOutcome { Placed, RefusedByBody, Uncertain, Blocked, NotSent };
enum class PreviewAfPointNotSent { None, Interrupted, BoundsUnusable, BoundsChanged };
struct PreviewAfPointResult {
    PreviewAfPointOutcome outcome{PreviewAfPointOutcome::NotSent};
    PreviewAfPointNotSent not_sent{PreviewAfPointNotSent::None};
    PreviewFocusAdmission admission{PreviewFocusAdmission::Allow}; // Blocked only
    DWORD mode{0xffffffff};
    unsigned dispatches{};
    WpdLiveViewCommandResult command;
    DWORD completion{};
    bool ready_confirmed{};
};
struct PreviewAfPointHooks {
    std::function<bool(DWORD* completion)> wait_ready;                // DeviceReady; false when interrupted
    std::function<DWORD()> read_focus_mode;                           // 0xD061 now
    std::function<std::optional<PreviewAfBounds>()> bounds;           // from the latest frame header
    std::function<PreviewAfClock::time_point()> now;
    std::function<bool()> stopping;
    std::function<WpdLiveViewExchange(IPortableDeviceValues*)> send;  // the dedicated gate and the transport
};
inline PreviewAfPointResult RunPreviewAfPoint(const PreviewAfPointRequest& point,const PreviewAfPointHooks& hooks){
    PreviewAfPointResult out;
    if(!point.Plausible()){out.not_sent=PreviewAfPointNotSent::BoundsUnusable;return out;}
    if(!hooks.wait_ready(nullptr)){out.not_sent=PreviewAfPointNotSent::Interrupted;return out;}
    out.mode=hooks.read_focus_mode();
    // The same condition as the AF button: AF-S or AF-F. A manual-focus value never reaches the body.
    out.admission=AdmitPreviewFocusMode(PreviewFocusAction::AutoFocus,out.mode);
    if(out.admission!=PreviewFocusAdmission::Allow){out.outcome=PreviewAfPointOutcome::Blocked;return out;}
    const auto bounds=hooks.bounds();
    if(!PreviewAfBoundsUsable(bounds,hooks.now())){out.not_sent=PreviewAfPointNotSent::BoundsUnusable;return out;}
    if(bounds->width!=point.width || bounds->height!=point.height){out.not_sent=PreviewAfPointNotSent::BoundsChanged;return out;}
    if(hooks.stopping()){out.not_sent=PreviewAfPointNotSent::Interrupted;return out;}
    auto request=BuildPreviewAfPointRequest(point.x,point.y);
    auto reply=hooks.send(request.Get());
    out.dispatches=1;
    out.command=InspectWpdLiveViewCommand(reply.status,reply.results.Get(),true);
    // Neither accepted nor refused by the body: the body may or may not have moved the frame. No second try.
    if(!out.command.Ok() && out.command.error!=WpdLiveViewResultError::MtpRejected){out.outcome=PreviewAfPointOutcome::Uncertain;return out;}
    out.outcome=out.command.Ok()?PreviewAfPointOutcome::Placed:PreviewAfPointOutcome::RefusedByBody;
    out.ready_confirmed=hooks.wait_ready(&out.completion);
    return out;
}
}
