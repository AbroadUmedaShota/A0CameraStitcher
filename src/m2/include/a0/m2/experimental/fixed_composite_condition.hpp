#pragma once
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace a0::m2::experimental {
// Fixed composite condition (#399, parent #396): the mapping and the seam
// column that every capture of one setup is composed with. Development only
// and always unapproved. No approved rig profile exists yet, so this record is
// the stand-in the development paths of #400 (JPEG) and #401 (C) will read.
// It is not a rig profile, never creates an approval, and has no field that
// could hold one.
//
// The mapping is the one a saved uncalibrated review actually rendered
// (SavedStillReview): B placed against A by a whole-pixel translation plus
// an optional rotation and scale about B's image center, in the pixels of the
// full-size captures after the mount orientation has turned them in memory.
// Its crop is the covered rectangle SimilarityReviewGeometry derives from it.
// The condition is made once, by an explicit person-driven action, from a
// reviewed pair. It is never made per capture.
//
// The seam is one vertical column of the composite, in output pixels (after
// the crop). It starts as the middle of the overlap band, which is only a
// provisional default (band-center-provisional); a person moves it later
// (#403), which is recorded as origin "operator". Whether a column plus the
// blend half-width fits inside the band is decided where the blend is
// rendered (RenderPairFixedSeam, #398), not here.
//
// Calibration (#374) is not a source in this version. A calibration-derived
// condition needs its own source fields and gets a new schema name.
inline constexpr const wchar_t* kFixedCompositeConditionName=L"wpd-fixed-composite-condition.json";
inline constexpr std::string_view kFixedCompositeConditionSchema="a0.fixed-composite-condition.v1";
inline constexpr std::size_t kMaximumFixedCompositeConditionBytes=4096;

// How the mapping of the source review was made. The names are the review
// report's geometryBasis values; "live-preview" also covers a v1 report.
enum class FixedCompositeMethod {
    live_preview,
    manual_still_pixels,
    manual_still_similarity,
    auto_still_translation,
    auto_still_rotation,
    auto_still_similarity
};
inline std::string_view FixedCompositeMethodName(FixedCompositeMethod m){
    switch(m){
    case FixedCompositeMethod::live_preview:return "live-preview";
    case FixedCompositeMethod::manual_still_pixels:return "manual-still-pixels";
    case FixedCompositeMethod::manual_still_similarity:return "manual-still-similarity";
    case FixedCompositeMethod::auto_still_translation:return "auto-still-translation";
    case FixedCompositeMethod::auto_still_rotation:return "auto-still-rotation";
    case FixedCompositeMethod::auto_still_similarity:return "auto-still-similarity";
    }
    throw std::invalid_argument("unknown fixed composite method");
}
inline std::optional<FixedCompositeMethod> ParseFixedCompositeMethod(std::string_view text){
    for(const auto m:{FixedCompositeMethod::live_preview,FixedCompositeMethod::manual_still_pixels,FixedCompositeMethod::manual_still_similarity,
        FixedCompositeMethod::auto_still_translation,FixedCompositeMethod::auto_still_rotation,FixedCompositeMethod::auto_still_similarity})
        if(FixedCompositeMethodName(m)==text)return m;
    return {};
}

// band_center_provisional: the default computed from the mapping and crop;
// the stored column must equal it. operator_set: a person chose the column.
enum class FixedSeamOrigin {band_center_provisional,operator_set};
inline std::string_view FixedSeamOriginName(FixedSeamOrigin o){
    if(o==FixedSeamOrigin::band_center_provisional)return "band-center-provisional";
    if(o==FixedSeamOrigin::operator_set)return "operator";
    throw std::invalid_argument("unknown fixed seam origin");
}
inline std::optional<FixedSeamOrigin> ParseFixedSeamOrigin(std::string_view text){
    if(text=="band-center-provisional")return FixedSeamOrigin::band_center_provisional;
    if(text=="operator")return FixedSeamOrigin::operator_set;
    return {};
}

struct FixedCompositeCondition {
    // UTC, "YYYY-MM-DDTHH:MM:SSZ".
    std::string created_utc;
    struct Source {
        // SHA-256 of the saved review report the mapping was taken from, and
        // its pair identity. Not a path: the report may move.
        std::string review_report_sha256,pair_id;
        FixedCompositeMethod method{FixedCompositeMethod::live_preview};
    } source;
    MountOrientation orientation{MountOrientation::normal};
    // Full-size capture size (A and B are equal), not the size after turning.
    std::uint32_t source_width{},source_height{};
    struct Mapping {
        // B's image center moves to this position relative to A, in capture pixels.
        int translation_x{},translation_y{};
        double rotation_degrees{},scale{1};
        // Output crop of the composite built from this mapping.
        StitchCropPixels crop;
    } mapping;
    // Size of the cropped composite, derived from the mapping and recorded.
    std::uint32_t output_width{},output_height{};
    // Largest per-capture scale difference, in percent, that may be corrected
    // while rotation and translation stay fixed; a larger one fails the gate
    // (#396 decision). Required and without a code default: the caller copies
    // it from the merge policy (scaleAutoCorrectionMaxPercent of
    // wpd-merge-policy.json). The owner decided 1 percent as the starting value
    // (#396), to be reviewed after measuring on the real rig; it is not an
    // approved optical threshold, and the record stays unapproved. Finite and
    // >= 0, the range ValidateStillMergePolicy accepts. The estimation and correction
    // themselves belong to #400.
    double scale_auto_correction_max_percent{};
    struct Seam {
        // Output pixel column (after the crop), 0 <= column < output_width.
        std::uint32_t column{};
        FixedSeamOrigin origin{FixedSeamOrigin::band_center_provisional};
    } seam;
};

// The output columns [first,last_exclusive) that both A and B cover on every
// output row, for the affine mapping of a fixed composite condition. A pixel
// column counts only when it is wholly covered; rows are tested at the first
// and last output pixel centers, where the edges of the convex footprint of B
// are furthest in. Pure: no image is read. Throws std::invalid_argument for
// a non-affine or invalid mapping, or when no column is covered.
struct FixedCompositeBand {std::uint32_t first{},last_exclusive{};};
inline FixedCompositeBand FixedCompositeOverlapBand(const render::PairRenderParameters& p,std::uint32_t width,std::uint32_t height){
    const auto& m=p.camera_b_to_camera_a;
    if(p.layout!=StitchLayout::camera_a_left_camera_b_right || m[6]!=0 || m[7]!=0 || m[8]!=1)throw std::invalid_argument("overlap band needs an affine left/right mapping");
    const auto plan=render::PlanPairCanvas(width,height,width,height,p);
    const double base_x=plan.origin_x+p.crop.left,top=plan.origin_y+p.crop.top,bottom=top+plan.output_height;
    if(top<0 || bottom>height)throw std::invalid_argument("output rows are outside camera A");
    // B's rectangle in A's frame, as a cycle of corners.
    std::array<render::Point,4> quad{};
    const std::array<render::Point,4> corners{{{0,0},{double(width),0},{double(width),double(height)},{0,double(height)}}};
    for(size_t i=0;i<4;++i)quad[i]={m[0]*corners[i].x+m[1]*corners[i].y+m[2],m[3]*corners[i].x+m[4]*corners[i].y+m[5]};
    const auto extent=[&](double y){
        double lo=std::numeric_limits<double>::infinity(),hi=-lo;
        for(size_t i=0;i<4;++i){
            const auto& s=quad[i];const auto& e=quad[(i+1)%4];
            if(s.y==e.y){if(s.y==y){lo=std::min({lo,s.x,e.x});hi=std::max({hi,s.x,e.x});}continue;}
            if((y<std::min(s.y,e.y)) || (y>std::max(s.y,e.y)))continue;
            const auto x=s.x+(e.x-s.x)*(y-s.y)/(e.y-s.y);lo=std::min(lo,x);hi=std::max(hi,x);
        }
        if(lo>hi)throw std::invalid_argument("camera B does not cover an output row");
        return std::pair{lo,hi};
    };
    const auto upper=extent(top+0.5),lower=extent(bottom-0.5);
    // A covers x in [0,width). The left edge of B only moves right and the
    // right edge only moves left going inward, so the two end rows bound it.
    const double left=std::max({0.0,upper.first,lower.first})-base_x,right=std::min({double(width),upper.second,lower.second})-base_x;
    const double first=std::max(0.0,std::ceil(left)),last=std::min(double(plan.output_width),std::floor(right));
    if(!(last>first))throw std::invalid_argument("the mapping has no overlap band");
    return {static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(last)};
}
// Middle of the overlap band, the provisional seam column: the column at
// (first+last_exclusive)/2 rounded down, so a band of an even width takes the
// right one of the two middle columns.
inline std::uint32_t ProvisionalSeamColumn(const render::PairRenderParameters& p,std::uint32_t width,std::uint32_t height){
    const auto band=FixedCompositeOverlapBand(p,width,height);
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(band.first)+band.last_exclusive)/2);
}

namespace fixed_composite_detail {
inline bool IsLowerHex64(const std::string& s){return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline bool IsPairId(const std::string& s){
    return !s.empty() && s.size()<=80 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos;
}
// "YYYY-MM-DDTHH:MM:SSZ", a real calendar date and a time of day.
inline bool IsUtcTimestamp(const std::string& s){
    if(s.size()!=20 || s[4]!='-' || s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':' || s[19]!='Z')return false;
    for(const size_t i:{0,1,2,3,5,6,8,9,11,12,14,15,17,18})if(s[i]<'0' || s[i]>'9')return false;
    const auto n=[&](size_t at,size_t len){return std::stoi(s.substr(at,len));};
    const std::chrono::year_month_day ymd{std::chrono::year{n(0,4)},std::chrono::month{static_cast<unsigned>(n(5,2))},std::chrono::day{static_cast<unsigned>(n(8,2))}};
    return ymd.ok() && n(11,2)<24 && n(14,2)<60 && n(17,2)<60;
}
inline void RequireMethodMatchesMapping(FixedCompositeMethod method,double rotation,double scale){
    const bool identity=rotation==0 && scale==1;
    switch(method){
    case FixedCompositeMethod::live_preview:
    case FixedCompositeMethod::manual_still_pixels:
    case FixedCompositeMethod::auto_still_translation:
        if(!identity)throw std::invalid_argument("a translation-only method records rotation 0 and scale 1");
        break;
    case FixedCompositeMethod::auto_still_rotation:
        if(scale!=1)throw std::invalid_argument("a rotation-only method records scale 1");
        break;
    case FixedCompositeMethod::manual_still_similarity:
        if(identity)throw std::invalid_argument("a manual similarity records a rotation or a scale");
        break;
    case FixedCompositeMethod::auto_still_similarity:
        break;
    }
}
}

// Throws std::invalid_argument unless every field is in range and consistent:
// the crop and output size must equal what the mapping derives, and a
// provisional seam column must equal the band middle. There is no approval
// field to validate: a condition is unapproved by construction.
inline void ValidateFixedCompositeCondition(const FixedCompositeCondition& c){
    using namespace fixed_composite_detail;
    if(!IsUtcTimestamp(c.created_utc) || !IsLowerHex64(c.source.review_report_sha256) || !IsPairId(c.source.pair_id))
        throw std::invalid_argument("invalid fixed composite condition source");
    (void)FixedCompositeMethodName(c.source.method);(void)MountOrientationName(c.orientation);(void)FixedSeamOriginName(c.seam.origin);
    (void)render::PixelCount(c.source_width,c.source_height);
    if(!std::isfinite(c.mapping.rotation_degrees) || !std::isfinite(c.mapping.scale))throw std::invalid_argument("fixed composite mapping is not finite");
    RequireMethodMatchesMapping(c.source.method,c.mapping.rotation_degrees,c.mapping.scale);
    // Range of the translation, rotation, scale and covered crop.
    const auto p=SimilarityReviewGeometry(c.mapping.translation_x,c.mapping.translation_y,c.mapping.rotation_degrees,c.mapping.scale,c.source_width,c.source_height);
    const auto& k=p.crop;const auto& r=c.mapping.crop;
    if(k.left!=r.left || k.top!=r.top || k.right!=r.right || k.bottom!=r.bottom)throw std::invalid_argument("fixed composite crop differs from the mapping");
    const auto plan=render::PlanPairCanvas(c.source_width,c.source_height,c.source_width,c.source_height,p);
    if(plan.output_width!=c.output_width || plan.output_height!=c.output_height)throw std::invalid_argument("fixed composite output size differs from the mapping");
    if(!std::isfinite(c.scale_auto_correction_max_percent) || c.scale_auto_correction_max_percent<0)throw std::invalid_argument("fixed composite scale correction limit is not a finite non-negative percent");
    if(c.seam.column>=c.output_width)throw std::invalid_argument("fixed composite seam column is outside the output");
    if(c.seam.origin==FixedSeamOrigin::band_center_provisional && c.seam.column!=ProvisionalSeamColumn(p,c.source_width,c.source_height))
        throw std::invalid_argument("a provisional seam column is the band middle");
}
// The mapping as render parameters at full capture size, the form the
// development renderers take. Validates the condition first. The layout is A
// on the left, B to its right. Scaling to the live view is #401's job
// (ScaleFixedSeamCondition of #398), not this function's.
inline render::PairRenderParameters FixedCompositeRenderParameters(const FixedCompositeCondition& c){
    ValidateFixedCompositeCondition(c);
    return SimilarityReviewGeometry(c.mapping.translation_x,c.mapping.translation_y,c.mapping.rotation_degrees,c.mapping.scale,c.source_width,c.source_height);
}

inline std::string FormatFixedCompositeTime(std::chrono::system_clock::time_point t){
    using namespace std::chrono;
    const auto day=floor<days>(t);const year_month_day ymd{day};
    const auto seconds_of_day=floor<seconds>(t-day).count();
    const int year=int(ymd.year());
    if(year<0 || year>9999)throw std::invalid_argument("time is outside the recordable range");
    char text[32];
    std::snprintf(text,sizeof text,"%04d-%02u-%02uT%02d:%02d:%02dZ",year,unsigned(ymd.month()),unsigned(ymd.day()),int(seconds_of_day/3600),int(seconds_of_day/60%60),int(seconds_of_day%60));
    return text;
}

// Makes the condition from a saved review that LoadSavedStillReview accepted:
// it takes the geometry the report actually rendered (for an automatic
// report the applied similarity, not the estimate; for a manual revision its
// values), and a provisional seam at the band middle. Call it only when a
// person sets up or re-sets a fixed condition, never per capture. It does not
// re-read the report; a SavedStillReview whose fields disagree with each
// other (crop, output size, method) is refused with std::invalid_argument.
// created is a parameter so the time is explicit and testable.
// scale_auto_correction_max_percent is recorded as given (no default; see the
// field).
inline FixedCompositeCondition MakeFixedCompositeCondition(const SavedStillReview& review,std::chrono::system_clock::time_point created,double scale_auto_correction_max_percent){
    if(review.manual_revision && review.automatic)throw std::invalid_argument("a review is either manual or automatic");
    // A lens-aware (v7) report has no similarity to fix; its mapping is a profile's (#405).
    if(review.fixed_composite_lens)throw std::invalid_argument("a lens-aware review has no similarity mapping to make a v1 condition from");
    // A registered (v8) report has none either: its mapping is a profile's with the capture's registration (#417).
    if(review.fixed_composite_registration)throw std::invalid_argument("a registered review has no similarity mapping to make a v1 condition from");
    FixedCompositeCondition c;
    c.created_utc=FormatFixedCompositeTime(created);
    c.source.review_report_sha256=review.report_sha256;c.source.pair_id=review.pair_id;
    if(review.manual_revision)
        c.source.method=(review.rotation_degrees!=0 || review.scale!=1)?FixedCompositeMethod::manual_still_similarity:FixedCompositeMethod::manual_still_pixels;
    else if(review.automatic){
        const auto basis=ParseFixedCompositeMethod(StillAlignmentBasis(*review.automatic));
        if(!basis)throw std::invalid_argument("unknown automatic geometry basis");
        c.source.method=*basis;
    }else c.source.method=FixedCompositeMethod::live_preview;
    c.orientation=review.geometry.orientation;
    c.source_width=review.source_width;c.source_height=review.source_height;
    c.mapping.translation_x=review.translation_x;c.mapping.translation_y=review.translation_y;
    // Negative zero would print as "-0".
    c.mapping.rotation_degrees=review.rotation_degrees==0?0.0:review.rotation_degrees;c.mapping.scale=review.scale;
    const auto p=SimilarityReviewGeometry(c.mapping.translation_x,c.mapping.translation_y,c.mapping.rotation_degrees,c.mapping.scale,c.source_width,c.source_height);
    c.mapping.crop=p.crop;
    const auto plan=render::PlanPairCanvas(c.source_width,c.source_height,c.source_width,c.source_height,p);
    c.output_width=plan.output_width;c.output_height=plan.output_height;
    if(review.width!=c.output_width || review.height!=c.output_height || review.crop.left!=p.crop.left || review.crop.top!=p.crop.top ||
        review.crop.right!=p.crop.right || review.crop.bottom!=p.crop.bottom)
        throw std::invalid_argument("review output differs from its recorded geometry");
    c.scale_auto_correction_max_percent=scale_auto_correction_max_percent;
    c.seam={ProvisionalSeamColumn(p,c.source_width,c.source_height),FixedSeamOrigin::band_center_provisional};
    ValidateFixedCompositeCondition(c);
    return c;
}

// Canonical text of a validated condition (no trailing newline). Always
// carries "approval":"unapproved".
inline std::string FormatFixedCompositeCondition(const FixedCompositeCondition& c){
    ValidateFixedCompositeCondition(c);
    const auto number=[](double v){
        char text[40];const auto r=std::to_chars(text,text+sizeof text,v==0?0.0:v);
        if(r.ec!=std::errc{})throw std::invalid_argument("number is not recordable");
        return std::string(text,r.ptr);
    };
    std::string s="{\"schema\":\"";s+=kFixedCompositeConditionSchema;
    s+="\",\"approval\":\"unapproved\",\"createdUtc\":\""+c.created_utc+"\",\"source\":{\"reviewReportSha256\":\""+c.source.review_report_sha256+
        "\",\"pairId\":\""+c.source.pair_id+"\",\"method\":\""+std::string(FixedCompositeMethodName(c.source.method))+
        "\"},\"mountOrientation\":\""+MountOrientationName(c.orientation)+"\",\"sourceDimensions\":{\"width\":"+std::to_string(c.source_width)+
        ",\"height\":"+std::to_string(c.source_height)+"},\"mapping\":{\"translation\":{\"x\":"+std::to_string(c.mapping.translation_x)+
        ",\"y\":"+std::to_string(c.mapping.translation_y)+"},\"rotationDegrees\":"+number(c.mapping.rotation_degrees)+
        ",\"scale\":"+number(c.mapping.scale)+",\"anchor\":\"image-center\",\"crop\":{\"left\":"+std::to_string(c.mapping.crop.left)+
        ",\"top\":"+std::to_string(c.mapping.crop.top)+",\"right\":"+std::to_string(c.mapping.crop.right)+
        ",\"bottom\":"+std::to_string(c.mapping.crop.bottom)+"}},\"output\":{\"width\":"+std::to_string(c.output_width)+
        ",\"height\":"+std::to_string(c.output_height)+"},\"scaleAutoCorrectionMaxPercent\":"+number(c.scale_auto_correction_max_percent)+",\"seam\":{\"column\":"+std::to_string(c.seam.column)+
        ",\"origin\":\""+std::string(FixedSeamOriginName(c.seam.origin))+"\"}}";
    return s;
}

struct FixedCompositeJsonFailure {[[noreturn]] static void Fail(std::string_view,std::string_view){throw std::invalid_argument("invalid fixed composite condition");}};
// Strict reader. Rejects malformed JSON, duplicate keys, a different schema,
// missing or extra fields, a value of the wrong kind, a number that is not
// finite or not whole where a whole number is required, and any value
// ValidateFixedCompositeCondition refuses. Every failure is the same
// std::invalid_argument.
inline FixedCompositeCondition ParseFixedCompositeCondition(std::string_view text){
    namespace json=a0::common::protocol_json;
    using Failure=FixedCompositeJsonFailure;
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3); // encoding marker only
    const auto root=json::BasicJsonParser<Failure,kMaximumFixedCompositeConditionBytes>(text).Parse();
    const auto object=[](const json::JsonValue& v,std::initializer_list<const char*> fields)->const json::JsonValue&{
        if(v.kind!=json::JsonKind::object || v.object.size()!=fields.size())Failure::Fail({},{});
        for(const auto* f:fields)if(!v.object.contains(f))Failure::Fail({},{});
        return v;
    };
    const auto at=[](const json::JsonValue& v,const char* f)->const json::JsonValue&{return v.object.at(f);};
    const auto str=[](const json::JsonValue& v){if(v.kind!=json::JsonKind::string)Failure::Fail({},{});return v.string;};
    const auto whole=[](const json::JsonValue& v,long long lo,long long hi){
        if(v.kind!=json::JsonKind::number || v.string.empty())Failure::Fail({},{});
        const size_t digits=v.string[0]=='-'?1:0;
        if(v.string.size()==digits || v.string.find_first_not_of("0123456789",digits)!=std::string::npos)Failure::Fail({},{});
        long long n{};const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
        // "-0" is not the canonical zero.
        if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || n<lo || n>hi || (n==0 && digits==1))Failure::Fail({},{});
        return n;
    };
    const auto real=[](const json::JsonValue& v){
        if(v.kind!=json::JsonKind::number)Failure::Fail({},{});double n{};
        const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n,std::chars_format::general);
        if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || !std::isfinite(n))Failure::Fail({},{});
        return n;
    };
    object(root,{"schema","approval","createdUtc","source","mountOrientation","sourceDimensions","mapping","output","scaleAutoCorrectionMaxPercent","seam"});
    // The only state a condition can be in; "approved" and anything else is refused.
    if(str(at(root,"schema"))!=kFixedCompositeConditionSchema || str(at(root,"approval"))!="unapproved")Failure::Fail({},{});
    FixedCompositeCondition c;c.created_utc=str(at(root,"createdUtc"));
    const auto& source=object(at(root,"source"),{"reviewReportSha256","pairId","method"});
    c.source.review_report_sha256=str(at(source,"reviewReportSha256"));c.source.pair_id=str(at(source,"pairId"));
    const auto method=ParseFixedCompositeMethod(str(at(source,"method")));
    const auto orientation=ParseMountOrientation(str(at(root,"mountOrientation")));
    if(!method || !orientation)Failure::Fail({},{});
    c.source.method=*method;c.orientation=*orientation;
    const auto& size=object(at(root,"sourceDimensions"),{"width","height"});
    c.source_width=static_cast<std::uint32_t>(whole(at(size,"width"),1,render::kMaximumImageDimension));
    c.source_height=static_cast<std::uint32_t>(whole(at(size,"height"),1,render::kMaximumImageDimension));
    const auto& mapping=object(at(root,"mapping"),{"translation","rotationDegrees","scale","anchor","crop"});
    const auto& shift=object(at(mapping,"translation"),{"x","y"});
    c.mapping.translation_x=static_cast<int>(whole(at(shift,"x"),1,32767));c.mapping.translation_y=static_cast<int>(whole(at(shift,"y"),-32767,32767));
    c.mapping.rotation_degrees=real(at(mapping,"rotationDegrees"));c.mapping.scale=real(at(mapping,"scale"));
    if(str(at(mapping,"anchor"))!="image-center")Failure::Fail({},{});
    const auto& crop=object(at(mapping,"crop"),{"left","top","right","bottom"});
    const auto crop_value=[&](const char* f){return static_cast<std::uint32_t>(whole(at(crop,f),0,render::kMaximumImageDimension));};
    c.mapping.crop={crop_value("left"),crop_value("top"),crop_value("right"),crop_value("bottom")};
    const auto& output=object(at(root,"output"),{"width","height"});
    c.output_width=static_cast<std::uint32_t>(whole(at(output,"width"),1,render::kMaximumImageDimension));
    c.output_height=static_cast<std::uint32_t>(whole(at(output,"height"),1,render::kMaximumImageDimension));
    c.scale_auto_correction_max_percent=real(at(root,"scaleAutoCorrectionMaxPercent"));
    const auto& seam=object(at(root,"seam"),{"column","origin"});
    c.seam.column=static_cast<std::uint32_t>(whole(at(seam,"column"),0,render::kMaximumImageDimension));
    const auto origin=ParseFixedSeamOrigin(str(at(seam,"origin")));
    if(!origin)Failure::Fail({},{});
    c.seam.origin=*origin;
    ValidateFixedCompositeCondition(c); // invalid_argument as well
    return c;
}

struct LoadedFixedCompositeCondition {
    FixedCompositeCondition condition;
    // SHA-256 of the exact file bytes, for reports that record which condition was used.
    std::string file_sha256;
};
// Reads the condition beside wpd-merge-policy.json. An absent file means there
// is no fixed composite condition (std::nullopt). Any other unreadable or
// non-exact content throws, so a damaged file is never mistaken for absence.
// hash: SHA-256 lower hex of the bytes read, supplied by the caller's provider.
template<class Hash>
std::optional<LoadedFixedCompositeCondition> LoadFixedCompositeCondition(const std::filesystem::path& file,Hash hash){
    std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
    if(status.type()==std::filesystem::file_type::not_found)return {};
    if(error || status.type()!=std::filesystem::file_type::regular)throw std::invalid_argument("fixed composite condition is not a regular file");
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("fixed composite condition unreadable");
    std::string text;text.resize(kMaximumFixedCompositeConditionBytes+1);in.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(in.bad())throw std::runtime_error("fixed composite condition unreadable");
    text.resize(static_cast<size_t>(in.gcount()));
    if(text.size()>kMaximumFixedCompositeConditionBytes)throw std::invalid_argument("fixed composite condition too large");
    return LoadedFixedCompositeCondition{ParseFixedCompositeCondition(text),hash(std::vector<unsigned char>(text.begin(),text.end()))};
}

// Writes the condition as a NEW file: validates and formats it, writes
// <file>.partial exclusively (an existing partial is refused), reads it back
// and parses it, then renames it to <file> without replacing. An existing
// <file> is never overwritten; the parent folder must exist. On a failure the
// partial this call created is removed. Throws std::invalid_argument for an
// invalid condition or an existing target, std::runtime_error for I/O errors.
// Changing a stored condition (for example the seam column, #403) is the
// caller's decision to make as a new file; this function does not do it.
void WriteFixedCompositeCondition(const std::filesystem::path& file,const FixedCompositeCondition& condition);
}
