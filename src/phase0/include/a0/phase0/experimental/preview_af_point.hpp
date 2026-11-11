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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
// What one camera knows about its AF frame. The owner guards it with its own
// mutex, except `log`, which only the frame-reading worker touches.
struct PreviewAfState {
    std::optional<D810AfHeader> frame;           // usable header of the latest frame, else unknown
    std::chrono::steady_clock::time_point frame_at{};
    D810AfReject reject{D810AfReject::ImageSize}; // why `frame` is unknown
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
}
