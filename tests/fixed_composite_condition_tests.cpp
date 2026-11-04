// Fixed composite condition record (#399): format, strict reader, creation
// from a saved review, the provisional seam column, and the new-file writer.
// Camera free; files go to a temporary folder only.
#include "a0/m2/experimental/fixed_composite_condition.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using namespace std::chrono_literals;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(const std::runtime_error&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)

const std::string kHash(64,'a');
const auto kCreated=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h+34min+56s+789ms};

// Test fixture value only; the product code has no default for the limit.
constexpr double kScaleLimit=1;
const double kInf=std::numeric_limits<double>::infinity();
FixedCompositeCondition MakeCondition(const SavedStillReview& review,std::chrono::system_clock::time_point created){
    return MakeFixedCompositeCondition(review,created,kScaleLimit);
}

fs::path TempParent(){
#ifdef _MSC_VER
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
#else
    if(const char* value=std::getenv("A0_TEST_TMPDIR");value!=nullptr && *value!='\0')return fs::path(value);
#endif
    return fs::temp_directory_path();
}

SavedStillReview Review(unsigned w,unsigned h,int x,int y,double rotation=0,double scale=1,MountOrientation orientation=MountOrientation::normal){
    SavedStillReview r;
    r.report_sha256=kHash;r.pair_id="pair-1";r.source_width=w;r.source_height=h;
    r.geometry.orientation=orientation;
    r.translation_x=x;r.translation_y=y;r.rotation_degrees=rotation;r.scale=scale;
    const auto p=SimilarityReviewGeometry(x,y,rotation,scale,w,h);
    r.crop=p.crop;
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    r.width=plan.output_width;r.height=plan.output_height;
    return r;
}
SavedStillReview Manual(SavedStillReview r){
    r.manual_revision=StillReviewRequest::ManualRevision{r.translation_x,r.translation_y,"parent.json",std::string(64,'b'),r.rotation_degrees,r.scale};
    return r;
}
// A review with an automatic result decided under the given policy, geometry
// taken from what the rule applies.
SavedStillReview Automatic(unsigned w,unsigned h,OverlapSimilarityEstimate e,const std::optional<StillMergePolicy>& policy){
    const auto decided=DecideStillAlignment(e,policy);
    SavedStillReview r;
    if(decided.similarity){
        const auto t=AppliedStillAlignment(decided);r=Review(w,h,t.x,t.y,t.rotation_degrees,t.scale);
    }else r=Review(w,h,32,3);
    r.automatic=decided;
    return r;
}
OverlapSimilarityEstimate Estimate(double x,double y,double rotation,double scale){
    OverlapSimilarityEstimate e;
    e.status=OverlapEstimateStatus::estimated_not_judged;e.reason=SimilarityUndeterminedReason::none;
    e.matched=e.agreed=8;e.tiles.resize(8);
    e.similarity={x,y,rotation,scale};e.rigid={x+0.2,y-0.1,rotation,1};e.translation_x=x+0.4;e.translation_y=y+0.3;
    return e;
}
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,("test token absent: "+from).c_str());return text;}
    text.replace(at,from.size(),to);return text;
}
bool SameCondition(const FixedCompositeCondition& a,const FixedCompositeCondition& b){
    return a.created_utc==b.created_utc && a.source.review_report_sha256==b.source.review_report_sha256 && a.source.pair_id==b.source.pair_id &&
        a.source.method==b.source.method && a.orientation==b.orientation && a.source_width==b.source_width && a.source_height==b.source_height &&
        a.mapping.translation_x==b.mapping.translation_x && a.mapping.translation_y==b.mapping.translation_y &&
        a.mapping.rotation_degrees==b.mapping.rotation_degrees && a.mapping.scale==b.mapping.scale &&
        a.mapping.crop.left==b.mapping.crop.left && a.mapping.crop.top==b.mapping.crop.top && a.mapping.crop.right==b.mapping.crop.right &&
        a.mapping.crop.bottom==b.mapping.crop.bottom && a.output_width==b.output_width && a.output_height==b.output_height && a.scale_auto_correction_max_percent==b.scale_auto_correction_max_percent &&
        a.seam.column==b.seam.column && a.seam.origin==b.seam.origin;
}
std::string FakeHash(const std::vector<unsigned char>& bytes){
    unsigned long long h=1469598103934665603ULL;for(const auto b:bytes){h^=b;h*=1099511628211ULL;}
    char text[17];std::snprintf(text,sizeof text,"%016llx",h);return std::string(48,'0')+text;
}
std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}

// Whether output column `column` is wholly inside A and B on output row `row`.
bool FullyCovered(const render::PairRenderParameters& p,unsigned w,unsigned h,unsigned column,unsigned row){
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    const auto inverse=render::Invert(p.camera_b_to_camera_a);
    const double y=plan.origin_y+p.crop.top+row+0.5,left=plan.origin_x+p.crop.left+column,right=left+1;
    if(y<0 || y>h || left<0 || right>w)return false;
    for(const double x:{left,right}){
        render::Point source{};
        if(!render::TryTransform(inverse,{x,y},source) || source.x<0 || source.x>w || source.y<0 || source.y>h)return false;
    }
    return true;
}
bool ColumnCoveredOnEveryRow(const render::PairRenderParameters& p,unsigned w,unsigned h,unsigned column){
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    for(unsigned row=0;row<plan.output_height;++row)if(!FullyCovered(p,w,h,column,row))return false;
    return true;
}

void Band(){
    // Pure translation: B starts at x, A ends at the width.
    for(const auto& [w,h,x,y]:{std::array<int,4>{64,48,32,4},std::array<int,4>{64,48,17,-3},std::array<int,4>{200,120,100,0}}){
        const auto p=SimilarityReviewGeometry(x,y,0,1,w,h);const auto band=FixedCompositeOverlapBand(p,w,h);
        CHECK(band.first==static_cast<unsigned>(x) && band.last_exclusive==static_cast<unsigned>(w));
        CHECK(ProvisionalSeamColumn(p,w,h)==(static_cast<unsigned>(x)+w)/2);
    }
    // 32 .. 64 -> 48; an odd width rounds down: 17 .. 64 -> 40.
    CHECK(ProvisionalSeamColumn(SimilarityReviewGeometry(32,4,0,1,64,48),64,48)==48);
    CHECK(ProvisionalSeamColumn(SimilarityReviewGeometry(17,-3,0,1,64,48),64,48)==40);
    // Rotated and scaled mappings: every band column is covered on every row,
    // and the columns just outside it are not wholly covered on every row.
    struct Case{unsigned w,h;int x,y;double rotation,scale;};
    for(const auto& c:{Case{400,300,200,6,0.6,1.004},Case{400,300,180,-9,-1.2,0.995},Case{640,480,300,20,2.0,1.02},Case{320,240,120,0,0.3,1}}){
        const auto p=SimilarityReviewGeometry(c.x,c.y,c.rotation,c.scale,c.w,c.h);
        const auto plan=render::PlanPairCanvas(c.w,c.h,c.w,c.h,p);
        const auto band=FixedCompositeOverlapBand(p,c.w,c.h);
        CHECK(band.first<band.last_exclusive && band.last_exclusive<=plan.output_width);
        bool inside=true;for(auto col=band.first;col<band.last_exclusive;++col)inside=inside && ColumnCoveredOnEveryRow(p,c.w,c.h,col);
        CHECK(inside);
        if(band.first>0)CHECK(!ColumnCoveredOnEveryRow(p,c.w,c.h,band.first-1));
        if(band.last_exclusive<plan.output_width)CHECK(!ColumnCoveredOnEveryRow(p,c.w,c.h,band.last_exclusive));
        const auto seam=ProvisionalSeamColumn(p,c.w,c.h);CHECK(seam>=band.first && seam<band.last_exclusive);
    }
    // Refusals: not an affine left/right mapping, or no overlap.
    auto projective=SimilarityReviewGeometry(32,4,0,1,64,48);projective.camera_b_to_camera_a[6]=0.001;
    REJECT(FixedCompositeOverlapBand(projective,64,48));
    auto vertical=SimilarityReviewGeometry(32,4,0,1,64,48);vertical.layout=a0::m2::StitchLayout::camera_a_top_camera_b_bottom;
    REJECT(FixedCompositeOverlapBand(vertical,64,48));
    auto apart=SimilarityReviewGeometry(32,4,0,1,64,48);apart.camera_b_to_camera_a[2]=70;
    REJECT(FixedCompositeOverlapBand(apart,64,48));
}

void Time(){
    CHECK(FormatFixedCompositeTime(kCreated)=="2026-11-04T12:34:56Z");
    CHECK(FormatFixedCompositeTime(std::chrono::system_clock::time_point{})=="1970-01-01T00:00:00Z");
    CHECK(FormatFixedCompositeTime(std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2028}/2/29}+23h+59min+59s})=="2028-02-29T23:59:59Z");
    CHECK(FormatFixedCompositeTime(std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{1969}/12/31}+23h+59min+59s})=="1969-12-31T23:59:59Z");
}

void Make(){
    // Every method of the source reviews.
    struct Case{const char* name;SavedStillReview review;FixedCompositeMethod method;};
    StillMergePolicy no_scale{kHash,2,{},{}},with_scale{kHash,2,{},1};
    std::vector<Case> cases;
    cases.push_back({"v1 live",Review(64,48,32,4),FixedCompositeMethod::live_preview});
    cases.push_back({"v2 manual pixels",Manual(Review(64,48,33,3)),FixedCompositeMethod::manual_still_pixels});
    cases.push_back({"v3 manual similarity",Manual(Review(400,300,200,6,1,1.0125)),FixedCompositeMethod::manual_still_similarity});
    {OverlapSimilarityEstimate undetermined;auto r=Review(64,48,32,3);r.automatic=DecideStillAlignment(undetermined,with_scale);cases.push_back({"v5 undetermined",r,FixedCompositeMethod::live_preview});}
    cases.push_back({"v5 rotation exceeded",Automatic(400,300,Estimate(200.4,6.2,3.5,1.003),with_scale),FixedCompositeMethod::auto_still_translation});
    cases.push_back({"v5 rotation only",Automatic(400,300,Estimate(200.4,6.2,0.4,1.003),no_scale),FixedCompositeMethod::auto_still_rotation});
    cases.push_back({"v5 scale over limit",Automatic(400,300,Estimate(200.4,6.2,0.4,1.03),with_scale),FixedCompositeMethod::auto_still_rotation});
    cases.push_back({"v5 similarity",Automatic(400,300,Estimate(200.4,6.2,0.4,1.003),with_scale),FixedCompositeMethod::auto_still_similarity});
    cases.push_back({"v5 no policy",Automatic(400,300,Estimate(200.4,6.2,0.4,1.003),std::nullopt),FixedCompositeMethod::auto_still_translation});
    cases.push_back({"rotated",Review(64,48,32,4,0,1,MountOrientation::rotated_180),FixedCompositeMethod::live_preview});
    for(const auto& c:cases){
        std::cout<<"case: "<<c.name<<'\n';
        const auto condition=MakeCondition(c.review,kCreated);
        CHECK(condition.source.method==c.method);
        CHECK(condition.created_utc=="2026-11-04T12:34:56Z" && condition.source.review_report_sha256==kHash && condition.source.pair_id=="pair-1");
        CHECK(condition.orientation==c.review.geometry.orientation);
        CHECK(condition.source_width==c.review.source_width && condition.source_height==c.review.source_height);
        CHECK(condition.mapping.translation_x==c.review.translation_x && condition.mapping.translation_y==c.review.translation_y);
        CHECK(condition.mapping.rotation_degrees==c.review.rotation_degrees && condition.mapping.scale==c.review.scale);
        CHECK(condition.output_width==c.review.width && condition.output_height==c.review.height);
        // The render parameters are the ones the review rendered with.
        const auto expected=SimilarityReviewGeometry(c.review.translation_x,c.review.translation_y,c.review.rotation_degrees,c.review.scale,c.review.source_width,c.review.source_height);
        const auto actual=FixedCompositeRenderParameters(condition);
        CHECK(actual.camera_b_to_camera_a==expected.camera_b_to_camera_a && actual.layout==expected.layout);
        CHECK(actual.crop.left==expected.crop.left && actual.crop.top==expected.crop.top && actual.crop.right==expected.crop.right && actual.crop.bottom==expected.crop.bottom);
        // The seam starts as the provisional band middle.
        CHECK(condition.seam.origin==FixedSeamOrigin::band_center_provisional);
        CHECK(condition.seam.column==ProvisionalSeamColumn(expected,c.review.source_width,c.review.source_height));
        CHECK(condition.seam.column<condition.output_width);
        // Round trip: write, read, same.
        const auto text=FormatFixedCompositeCondition(condition);
        const auto parsed=ParseFixedCompositeCondition(text);
        CHECK(SameCondition(condition,parsed) && FormatFixedCompositeCondition(parsed)==text);
        CHECK(text.find("\"approval\":\"unapproved\"")!=std::string::npos && text.find("pproved\":true")==std::string::npos);
        CHECK(ParseFixedCompositeCondition(std::string("\xEF\xBB\xBF")+text+"\r\n").seam.column==condition.seam.column);
    }
    {
        const auto text=FormatFixedCompositeCondition(MakeCondition(Review(64,48,32,4,0,1,MountOrientation::rotated_180),kCreated));
        CHECK(text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
        CHECK(ParseFixedCompositeCondition(text).orientation==MountOrientation::rotated_180);
        const auto normal=FormatFixedCompositeCondition(MakeCondition(Review(64,48,32,4),kCreated));
        CHECK(normal.find("\"mountOrientation\":\"normal\"")!=std::string::npos);
    }
    // Format of the first case, as a golden text (the schema is a contract).
    CHECK(FormatFixedCompositeCondition(MakeCondition(Review(64,48,32,4),kCreated))==
        "{\"schema\":\"a0.fixed-composite-condition.v1\",\"approval\":\"unapproved\",\"createdUtc\":\"2026-11-04T12:34:56Z\","
        "\"source\":{\"reviewReportSha256\":\""+kHash+"\",\"pairId\":\"pair-1\",\"method\":\"live-preview\"},"
        "\"mountOrientation\":\"normal\",\"sourceDimensions\":{\"width\":64,\"height\":48},"
        "\"mapping\":{\"translation\":{\"x\":32,\"y\":4},\"rotationDegrees\":0,\"scale\":1,\"anchor\":\"image-center\","
        "\"crop\":{\"left\":0,\"top\":4,\"right\":0,\"bottom\":4}},\"output\":{\"width\":96,\"height\":44},"
        "\"scaleAutoCorrectionMaxPercent\":1,"
        "\"seam\":{\"column\":48,\"origin\":\"band-center-provisional\"}}");
    // No profile or approval field names.
    const auto text=FormatFixedCompositeCondition(MakeCondition(Review(64,48,32,4),kCreated));
    for(const char* name:{"\"approved\"","\"rigProfile\"","\"homography\"","\"path\""})CHECK(text.find(name)==std::string::npos);

    // Inconsistent or invalid reviews are refused.
    {auto r=Manual(Review(64,48,32,4));r.automatic=DecideStillAlignment(OverlapSimilarityEstimate{},std::nullopt);REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.crop.top=3;REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.width=95;REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.height=45;REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.report_sha256=std::string(64,'A');REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.report_sha256.clear();REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.pair_id="a/b";REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.rotation_degrees=1;REJECT(MakeCondition(r,kCreated));} // not manual and not automatic: translation only
    {auto r=Review(64,48,32,4);r.translation_x=64;REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.source_width=0;REJECT(MakeCondition(r,kCreated));}
    {auto r=Review(64,48,32,4);r.geometry.orientation=static_cast<MountOrientation>(7);REJECT(MakeCondition(r,kCreated));}
    {auto r=Manual(Review(400,300,200,6,1,1.0125));r.scale=std::nan("");REJECT(MakeCondition(r,kCreated));}
    REJECT(MakeCondition(Review(64,48,32,4),std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{10000}/1/1}}));
    // Negative zero never reaches the text.
    {auto r=Review(64,48,32,4);r.rotation_degrees=-0.0;CHECK(FormatFixedCompositeCondition(MakeCondition(r,kCreated)).find("\"rotationDegrees\":-0")==std::string::npos);}
}

void Reject(){
    const auto good=FormatFixedCompositeCondition(MakeCondition(Review(64,48,32,4),kCreated));
    const auto good_similarity=FormatFixedCompositeCondition(MakeCondition(Manual(Review(400,300,200,6,1,1.0125)),kCreated));
    REJECT(ParseFixedCompositeCondition(""));
    REJECT(ParseFixedCompositeCondition("{"));
    REJECT(ParseFixedCompositeCondition("[]"));
    REJECT(ParseFixedCompositeCondition("null"));
    REJECT(ParseFixedCompositeCondition(good.substr(0,good.size()-1)));
    REJECT(ParseFixedCompositeCondition(good+"x"));
    REJECT(ParseFixedCompositeCondition(good+good));
    REJECT(ParseFixedCompositeCondition(std::string(kMaximumFixedCompositeConditionBytes+1,' ')+good));
    // Schema and approval.
    REJECT(ParseFixedCompositeCondition(Replace(good,"fixed-composite-condition.v1","fixed-composite-condition.v2")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"schema\":\"a0.fixed-composite-condition.v1\"","\"schema\":1")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"schema\":\"a0.fixed-composite-condition.v1\",","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\"","\"approval\":\"approved\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\"","\"approval\":true")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\"","\"approval\":\"Unapproved\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\",","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\"","\"approval\":\"unapproved\",\"approved\":true")));
    // Extra, missing and duplicate fields, at every level.
    REJECT(ParseFixedCompositeCondition(Replace(good,"{\"schema\"","{\"extra\":0,\"schema\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"pairId\"","\"extra\":0,\"pairId\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"anchor\"","\"extra\":0,\"anchor\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"left\":0","\"extra\":0,\"left\":0")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"origin\"","\"extra\":0,\"origin\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"createdUtc\":\"2026-11-04T12:34:56Z\",","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"mountOrientation\":\"normal\",","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"sourceDimensions\":{\"width\":64,\"height\":48},","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"anchor\":\"image-center\",","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,",\"seam\":{\"column\":48,\"origin\":\"band-center-provisional\"}","")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"seam\":{\"column\":48,\"origin\":\"band-center-provisional\"}","\"seam\":{\"column\":48}")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":1,\"scale\":1,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"approval\":\"unapproved\",","\"approval\":\"unapproved\",\"approval\":\"unapproved\",")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"column\":48","\"column\":48,\"column\":48")));
    // Wrong kinds.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":\"1\",")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":null,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"rotationDegrees\":0","\"rotationDegrees\":false")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"source\":{","\"source\":[{")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"width\":64","\"width\":\"64\"")));
    // Numbers: not finite, out of double range, not whole, out of range.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":NaN,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":Infinity,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":1e999,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":-1e999,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"rotationDegrees\":0","\"rotationDegrees\":1e999")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":0,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":-1,")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"scale\":1.0125","\"scale\":0")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"rotationDegrees\":1","\"rotationDegrees\":180.5")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"rotationDegrees\":1","\"rotationDegrees\":-181")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":32.0")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":3.2e1")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":0")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":-32")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":64")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"x\":32","\"x\":99999999999999999999")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"y\":4","\"y\":48")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"y\":4","\"y\":-48")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"y\":4","\"y\":4.5")));
    // Sizes: zero, too large, mismatching the derived values.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"width\":64","\"width\":0")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"height\":48","\"height\":0")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"width\":64","\"width\":32769")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"width\":64","\"width\":-64")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"width\":64","\"width\":65")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"output\":{\"width\":96","\"output\":{\"width\":95")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"height\":44}","\"height\":45}")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"output\":{\"width\":96","\"output\":{\"width\":0")));
    // Crop: any value that differs from the derived covered crop.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"left\":0","\"left\":1")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"top\":4","\"top\":3")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"right\":0","\"right\":1")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"bottom\":4","\"bottom\":5")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"left\":0","\"left\":-0")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"left\":","\"left\":9")));
    // A mapping that covers no pixels: B rotated away.
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"rotationDegrees\":1","\"rotationDegrees\":90")));
    // Orientation.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"normal\"","\"rotated-90\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"normal\"","\"Normal\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"normal\"","\"\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"normal\"","1")));
    CHECK(ParseFixedCompositeCondition(Replace(good,"\"normal\"","\"rotated-180\"")).orientation==MountOrientation::rotated_180);
    // Source.
    REJECT(ParseFixedCompositeCondition(Replace(good,kHash,std::string(63,'a'))));
    REJECT(ParseFixedCompositeCondition(Replace(good,kHash,std::string(64,'A'))));
    REJECT(ParseFixedCompositeCondition(Replace(good,kHash,std::string(63,'a')+"g")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"pairId\":\"pair-1\"","\"pairId\":\"\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"pairId\":\"pair-1\"","\"pairId\":\"a/b\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"pairId\":\"pair-1\"","\"pairId\":\""+std::string(81,'a')+"\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"method\":\"live-preview\"","\"method\":\"auto-similarity\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"method\":\"live-preview\"","\"method\":\"calibration\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"method\":\"live-preview\"","\"method\":\"\"")));
    // The method must agree with the mapping it names.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"method\":\"live-preview\"","\"method\":\"manual-still-similarity\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"method\":\"manual-still-similarity\"","\"method\":\"manual-still-pixels\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"method\":\"manual-still-similarity\"","\"method\":\"live-preview\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"method\":\"manual-still-similarity\"","\"method\":\"auto-still-translation\"")));
    REJECT(ParseFixedCompositeCondition(Replace(good_similarity,"\"method\":\"manual-still-similarity\"","\"method\":\"auto-still-rotation\""))); // scale is not 1
    CHECK(ParseFixedCompositeCondition(Replace(good_similarity,"\"method\":\"manual-still-similarity\"","\"method\":\"auto-still-similarity\"")).source.method==FixedCompositeMethod::auto_still_similarity);
    // Time.
    for(const char* bad:{"2026-13-04T12:34:56Z","2026-02-30T12:34:56Z","2027-02-29T12:34:56Z","2026-11-04T24:00:00Z","2026-11-04T12:60:56Z","2026-11-04T12:34:60Z",
        "2026-11-04t12:34:56Z","2026-11-04T12:34:56","2026-11-04T12:34:56+00:00","2026-11-04 12:34:56Z","2026-11-04T12:34:56.1Z","26-11-04T12:34:56Z","2026-11-00T12:34:56Z",
        "2026-1-04T12:34:56Z","","2026-11-04T12:34:5aZ"})
        Check(Throws([&]{(void)ParseFixedCompositeCondition(Replace(good,"2026-11-04T12:34:56Z",bad));}),bad);
    CHECK(ParseFixedCompositeCondition(Replace(good,"2026-11-04T12:34:56Z","2028-02-29T23:59:59Z")).created_utc=="2028-02-29T23:59:59Z");
    // JSON syntax that the shared parser refuses.
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":01,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"scale\":1,","\"scale\":1,,")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"schema\"","'schema'")));
    REJECT(ParseFixedCompositeCondition(Replace(good,"\"schema\"","\"sch\\x65ma\"")));
}

void Seam(){
    const auto base=MakeCondition(Review(64,48,32,4),kCreated); // output 96, band [32,64), middle 48
    CHECK(base.seam.column==48 && base.output_width==96);
    // Provisional: only the band middle.
    for(const unsigned column:{0u,31u,47u,49u,63u,95u}){
        auto c=base;c.seam.column=column;
        REJECT(FormatFixedCompositeCondition(c));
        REJECT(ParseFixedCompositeCondition(Replace(FormatFixedCompositeCondition(base),"\"column\":48","\"column\":"+std::to_string(column))));
    }
    // Operator: any column inside the output; band fit is the renderer's check (#398).
    for(const unsigned column:{0u,1u,31u,40u,48u,63u,95u}){
        auto c=base;c.seam.column=column;c.seam.origin=FixedSeamOrigin::operator_set;
        const auto text=FormatFixedCompositeCondition(c);
        const auto parsed=ParseFixedCompositeCondition(text);
        CHECK(parsed.seam.column==column && parsed.seam.origin==FixedSeamOrigin::operator_set && FormatFixedCompositeCondition(parsed)==text);
        CHECK(text.find("\"origin\":\"operator\"")!=std::string::npos);
        CHECK(FixedCompositeRenderParameters(parsed).camera_b_to_camera_a==FixedCompositeRenderParameters(base).camera_b_to_camera_a); // the mapping is untouched
    }
    for(const unsigned column:{96u,97u,1000u,0xFFFFFFFFu}){
        auto c=base;c.seam.column=column;c.seam.origin=FixedSeamOrigin::operator_set;
        REJECT(FormatFixedCompositeCondition(c));
    }
    {auto c=base;c.seam.column=48;c.seam.origin=static_cast<FixedSeamOrigin>(9);REJECT(FormatFixedCompositeCondition(c));}
    const auto text=FormatFixedCompositeCondition(base);
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"column\":48","\"column\":96")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"column\":48","\"column\":-1")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"column\":48","\"column\":48.0")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"column\":48","\"column\":\"48\"")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"band-center-provisional","provisional")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"band-center-provisional","Operator")));
    CHECK(ParseFixedCompositeCondition(Replace(Replace(text,"band-center-provisional","operator"),"\"column\":48","\"column\":50")).seam.column==50);
    // The seam follows the mapping: the same condition re-made with another mapping has another middle.
    CHECK(MakeCondition(Review(64,48,16,4),kCreated).seam.column==40);
}

void ScaleLimit(){
    const auto review=Review(64,48,32,4);
    // The limit is the caller's value, recorded as given, with the range the merge policy accepts.
    for(const double limit:{0.0,0.2,1.0,12.5,1e6}){
        const auto c=MakeFixedCompositeCondition(review,kCreated,limit);
        CHECK(c.scale_auto_correction_max_percent==limit);
        const auto text=FormatFixedCompositeCondition(c);
        CHECK(text.find("\"scaleAutoCorrectionMaxPercent\":")!=std::string::npos);
        const auto parsed=ParseFixedCompositeCondition(text);
        CHECK(parsed.scale_auto_correction_max_percent==limit && FormatFixedCompositeCondition(parsed)==text);
        CHECK(parsed.seam.column==48 && parsed.mapping.translation_x==32); // nothing else moves
    }
    for(const double limit:{-0.1,-1.0,std::nan(""),kInf,-kInf}){
        REJECT(MakeFixedCompositeCondition(review,kCreated,limit));
        auto c=MakeCondition(review,kCreated);c.scale_auto_correction_max_percent=limit;REJECT(FormatFixedCompositeCondition(c));
    }
    // Same acceptance as ValidateStillMergePolicy for scaleAutoCorrectionMaxPercent.
    for(const double limit:{0.0,0.2,1.0,1e6,-0.1,std::nan(""),kInf}){
        bool policy_accepts=true;
        try{ValidateStillMergePolicy(StillMergePolicy{kHash,2,{},limit});}catch(const std::invalid_argument&){policy_accepts=false;}
        CHECK(Throws([&]{(void)MakeFixedCompositeCondition(review,kCreated,limit);})==!policy_accepts);
    }
    const auto text=FormatFixedCompositeCondition(MakeCondition(review,kCreated));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1,","")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":-0.5")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":\"0.5\"")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":null")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":1e999")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":NaN")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":1,\"scaleAutoCorrectionMaxPercent\":1")));
    REJECT(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":{\"value\":0.5}")));
    CHECK(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":0")).scale_auto_correction_max_percent==0);
    CHECK(ParseFixedCompositeCondition(Replace(text,"\"scaleAutoCorrectionMaxPercent\":1","\"scaleAutoCorrectionMaxPercent\":2.5e0")).scale_auto_correction_max_percent==2.5);
}

void Files(){
    const auto root=TempParent()/("a0-fixed-composite-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto file=root/kFixedCompositeConditionName;
    const auto partial=fs::path(file.native()+L".partial");
    const auto hash=[](const std::vector<unsigned char>& bytes){return FakeHash(bytes);};
    // Absent: no condition, not an error.
    CHECK(!LoadFixedCompositeCondition(file,hash));
    const auto condition=MakeCondition(Manual(Review(400,300,200,6,1,1.0125)),kCreated);
    WriteFixedCompositeCondition(file,condition);
    CHECK(fs::exists(file) && !fs::exists(partial));
    const auto bytes=ReadAll(file);
    CHECK(bytes==FormatFixedCompositeCondition(condition)+"\n");
    const auto loaded=LoadFixedCompositeCondition(file,hash);
    CHECK(loaded && SameCondition(loaded->condition,condition) && loaded->file_sha256==FakeHash(std::vector<unsigned char>(bytes.begin(),bytes.end())));
    // Never overwrite: the existing bytes survive, even for an identical condition.
    REJECT(WriteFixedCompositeCondition(file,condition));
    auto other=condition;other.seam.column=condition.seam.column+1;other.seam.origin=FixedSeamOrigin::operator_set;
    REJECT(WriteFixedCompositeCondition(file,other));
    CHECK(ReadAll(file)==bytes && !fs::exists(partial));
    // A partial that is already there is not reused or removed.
    const auto second=root/"second.json";const auto second_partial=fs::path(second.native()+L".partial");
    WriteAll(second_partial,"someone else");
    REJECT(WriteFixedCompositeCondition(second,condition));
    CHECK(ReadAll(second_partial)=="someone else" && !fs::exists(second));
    fs::remove(second_partial);
    WriteFixedCompositeCondition(second,other);
    CHECK(LoadFixedCompositeCondition(second,hash)->condition.seam.origin==FixedSeamOrigin::operator_set);
    // An invalid condition writes nothing.
    auto invalid=condition;invalid.source.pair_id="bad/id";
    const auto third=root/"third.json";
    REJECT(WriteFixedCompositeCondition(third,invalid));
    CHECK(!fs::exists(third) && !fs::exists(fs::path(third.native()+L".partial")));
    // A missing folder is not created.
    REJECT(WriteFixedCompositeCondition(root/"missing-folder"/"x.json",condition));
    CHECK(!fs::exists(root/"missing-folder"));
    // A directory in the way.
    fs::create_directories(root/"as-directory.json");
    REJECT(WriteFixedCompositeCondition(root/"as-directory.json",condition));
    REJECT(LoadFixedCompositeCondition(root/"as-directory.json",hash));
    // Damaged and oversize files fail; they are not read as absent.
    const auto damaged=root/"damaged.json";
    WriteAll(damaged,"{\"schema\":\"a0.fixed-composite-condition.v1\"}");REJECT(LoadFixedCompositeCondition(damaged,hash));
    WriteAll(damaged,"");REJECT(LoadFixedCompositeCondition(damaged,hash));
    WriteAll(damaged,std::string(kMaximumFixedCompositeConditionBytes+1,' '));REJECT(LoadFixedCompositeCondition(damaged,hash));
    WriteAll(damaged,Replace(std::string(bytes),"unapproved","approved"));REJECT(LoadFixedCompositeCondition(damaged,hash));
    WriteAll(damaged,std::string(bytes)+"x");REJECT(LoadFixedCompositeCondition(damaged,hash));
    WriteAll(damaged,std::string("\xEF\xBB\xBF")+bytes);CHECK(LoadFixedCompositeCondition(damaged,hash).has_value());
    std::error_code ignored;fs::remove_all(root,ignored);
}
}

int main(){
    try{
        Time();Band();Make();Reject();Seam();ScaleLimit();Files();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
