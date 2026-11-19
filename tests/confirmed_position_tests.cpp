// The pure parts of the confirmed position of the development preview (#437 stage 2, docs/DECISIONS.md ADR-0046): the one written form of a
// number, the entered amounts of a hand step and the expression that turns them into a position (with a paper surface, and against the
// condition tool's older expression for the table plane), the difference between two positions at the seam, and the checks a hand step has to
// pass (the pose envelope and the cheap cover check). Camera, window, clock and file free. Argument 1 is the shared rig profile v2 fixture
// folder. Every expected value is made by a route of this file (its own matrix code, the renderer's mapping, a copy of the earlier code, a
// number worked out by hand), not by the function that is checked.
#include "a0/m2/experimental/confirmed_position.hpp"
#include "a0/m2/experimental/confirmed_position_check.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include "a0/phase0/experimental/live_placement.hpp"
#include "confirmed_position_rig.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace m2=a0::m2::experimental;
namespace cr=a0::m2::experimental::content_registration;
namespace px=a0::phase0::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;
using a0::tests::confirmed_position_rig::MakeRig;
using a0::tests::confirmed_position_rig::ReadAll;
using a0::tests::confirmed_position_rig::Rig;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what,int line){++checks;if(!value){++failures;std::cerr<<"FAILED line "<<line<<": "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr,__LINE__)
template<class Call> bool ThrowsInvalid(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(...){return false;}
    return false;
}
#define REJECT(expr) Check(ThrowsInvalid([&]{(void)(expr);}),"rejects: " #expr,__LINE__)
template<class Call> bool ThrowsOutOfRange(Call call){
    try{call();}catch(const std::out_of_range&){return true;}catch(...){return false;}
    return false;
}
#define REJECT_RANGE(expr) Check(ThrowsOutOfRange([&]{(void)(expr);}),"rejects (range): " #expr,__LINE__)
bool PressLess(const m2::HandCounts& a,const m2::HandCounts& b){
    return std::array<std::int32_t,3>{a.seam_dx,a.seam_dy,a.rotation}<std::array<std::int32_t,3>{b.seam_dx,b.seam_dy,b.rotation};
}

std::uint64_t Bits(double v){return std::bit_cast<std::uint64_t>(v);}
bool SameBits(double a,double b){return Bits(a)==Bits(b);}
bool SamePoseBits(const cr::PoseParameters& a,const cr::PoseParameters& b){
    return SameBits(a.rx_deg,b.rx_deg) && SameBits(a.ry_deg,b.ry_deg) && SameBits(a.rz_deg,b.rz_deg) && SameBits(a.tx_mm,b.tx_mm) && SameBits(a.ty_mm,b.ty_mm) &&
        SameBits(a.tz_percent,b.tz_percent);
}
bool SameSurfaceBits(const m2::SurfacePlane& a,const m2::SurfacePlane& b){return SameBits(a.h0_mm,b.h0_mm) && SameBits(a.slope_x,b.slope_x) && SameBits(a.slope_y,b.slope_y);}
bool SameMappingBits(const m2::RegisteredMapping& a,const m2::RegisteredMapping& b){
    return SamePoseBits(a.pose,b.pose) && SameSurfaceBits(a.surface,b.surface) && a.parallax.has_value()==b.parallax.has_value();
}

// What a hand step needs of the rig.
struct Geometry {
    cr::PoseModel model_b;
    cr::CameraCentre centre_a;
    render::Point origin;
    cr::PlaneFrame frame;
    std::uint32_t seam;
    std::uint32_t height;
    double middle;
    explicit Geometry(const Rig& rig):model_b(rig.binding.parameters.camera_b),
        centre_a(cr::PoseModel(rig.binding.parameters.camera_a).Centre({})),
        origin(cr::SurfaceOriginMm(rig.binding.parameters.raster,rig.condition.seam.column)),
        frame(cr::FrameOfRaster(rig.binding.parameters.raster)),seam(rig.condition.seam.column),
        height(rig.binding.parameters.raster.height_pixels),middle((static_cast<double>(rig.binding.parameters.raster.height_pixels)-1)/2){}
};

// ---- Another route to the same numbers: this file's own 3x3 code (adjugate over determinant), not render::Invert or the code under test ----
render::Matrix3 Inverse3(const render::Matrix3& m){
    const double c00=m[4]*m[8]-m[5]*m[7],c01=m[5]*m[6]-m[3]*m[8],c02=m[3]*m[7]-m[4]*m[6];
    const double det=m[0]*c00+m[1]*c01+m[2]*c02;
    return {c00/det,(m[2]*m[7]-m[1]*m[8])/det,(m[1]*m[5]-m[2]*m[4])/det,
            c01/det,(m[0]*m[8]-m[2]*m[6])/det,(m[2]*m[3]-m[0]*m[5])/det,
            c02/det,(m[1]*m[6]-m[0]*m[7])/det,(m[0]*m[4]-m[1]*m[3])/det};
}
render::Point ApplyH(const render::Matrix3& g,double x,double y){
    const double w=g[6]*x+g[7]*y+g[8];
    return {(g[0]*x+g[1]*y+g[2])/w,(g[3]*x+g[4]*y+g[5])/w};
}
render::Matrix3 HomographyOf(const Geometry& g,const m2::RegisteredMapping& m){return cr::RegisteredHomography(g.model_b,g.centre_a,g.origin,m.pose,m.surface);}
// Where the content that `from` puts at the output sample (x, y) is put by `to`: the document point of the sample X' with from(X') = to(X), as a
// displacement of the sample (output px). Worked out with this file's inverse.
render::Point DisplacementBetween(const Geometry& g,const m2::RegisteredMapping& from,const m2::RegisteredMapping& to,double x,double y){
    const double px_mm=g.frame.left_mm+(x+0.5)*g.frame.pitch_mm,py_mm=g.frame.top_mm+(y+0.5)*g.frame.pitch_mm;
    const auto target=ApplyH(HomographyOf(g,to),px_mm,py_mm);
    const auto source=ApplyH(Inverse3(HomographyOf(g,from)),target.x,target.y);
    return {(source.x-px_mm)/g.frame.pitch_mm,(source.y-py_mm)/g.frame.pitch_mm};
}
// The camera pixel that B samples at the output sample (x, y), through the product's mapping maker (the lens, the projection, the validity
// checks), the way a rendered composite reads B.
render::Point RawOf(const Rig& rig,const cr::PoseModel& model_b,const m2::RegisteredMapping& m,double x,double y){
    const auto& p=rig.binding.parameters;
    const auto map=m2::RegisteredDocumentOutputToCamera(p.raster,p.camera_a,p.camera_b,model_b,m,rig.condition.seam.column);
    render::Point raw{};
    if(!map({x,y},raw))throw std::runtime_error("the mapping has no point");
    return raw;
}

// ---- The code before this change, kept as the reference (copied from fixed_composite_tool_v3.hpp, SolveManualChange and what it calls) ----
namespace legacy {
render::Point Moved(const render::Matrix3& g,const cr::PlaneFrame& frame,double x,double y){
    const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
    const double w=g[6]*px+g[7]*py+g[8];
    return {((g[0]*px+g[1]*py+g[2])/w-px)/frame.pitch_mm,((g[3]*px+g[4]*py+g[5])/w-py)/frame.pitch_mm};
}
render::Matrix3 Multiply(const render::Matrix3& a,const render::Matrix3& b){
    render::Matrix3 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=a[3*i+k]*b[3*k+j];r[3*i+j]=s;}
    return r;
}
render::Point RelativeDisplacement(const cr::PoseModel& model,const cr::PlaneFrame& frame,const cr::PoseParameters& from,const cr::PoseParameters& to,double x,double y){
    return Moved(Multiply(render::Invert(model.Homography(from)),model.Homography(to)),frame,x,y);
}
cr::PoseParameters Added(const cr::PoseParameters& a,const cr::PoseParameters& b){
    return {a.rx_deg+b.rx_deg,a.ry_deg+b.ry_deg,a.rz_deg+b.rz_deg,a.tx_mm+b.tx_mm,a.ty_mm+b.ty_mm,a.tz_percent+b.tz_percent};
}
cr::PoseParameters SolveManualChange(const cr::PoseModel& model,const cr::PlaneFrame& frame,const cr::PoseParameters& base,
    const m2::FixedCompositeRevisionChangeV3::Entered& entered,double x,double y){
    cr::PoseParameters delta{};delta.rz_deg=entered.rotation_deg;
    const auto at=[&](const cr::PoseParameters& d){return RelativeDisplacement(model,frame,base,Added(base,d),x,y);};
    constexpr double kStepMm=1e-3,kToleranceMm=1e-12;
    for(int i=0;i<12;++i){
        const auto d=at(delta);
        const double ex=entered.seam_dx_px-d.x,ey=entered.seam_dy_px-d.y;
        if(std::hypot(ex,ey)<1e-10)return delta;
        auto px=delta;px.tx_mm+=kStepMm;auto py=delta;py.ty_mm+=kStepMm;
        const auto dx=at(px),dy=at(py);
        const double a=(dx.x-d.x)/kStepMm,c=(dx.y-d.y)/kStepMm,b=(dy.x-d.x)/kStepMm,e=(dy.y-d.y)/kStepMm;
        const double det=a*e-b*c;
        if(!(std::abs(det)>kToleranceMm))throw std::invalid_argument("the seam displacement does not determine a translation");
        delta.tx_mm+=(ex*e-b*ey)/det;delta.ty_mm+=(a*ey-c*ex)/det;
        if(!std::isfinite(delta.tx_mm) || !std::isfinite(delta.ty_mm))throw std::invalid_argument("the seam displacement gives no pose");
    }
    const auto d=at(delta);
    if(std::hypot(entered.seam_dx_px-d.x,entered.seam_dy_px-d.y)>1e-6)throw std::invalid_argument("the seam displacement could not be turned into a pose");
    return delta;
}
// The two ways a number was written before them being one: the report's Num (capture_registration.cpp) and the log's LivePlacementNumber.
std::string ReportNum(double v){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);return out.str();
}
std::string LogNum(double value){
    if(!std::isfinite(value))return std::string("nan");
    if(value==0)return std::string("0");
    std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<value;
    return s.str();
}
}

// ---- 1. The written form of a number ----
void Numbers(){
    // Worked out with %.17g of another implementation (Python). The negative zero is written as 0 by the report and the log.
    struct Case {double value;const char* text;};
    const Case cases[]={
        {0.0,"0"},{-0.0,"0"},{1.0,"1"},{-1.0,"-1"},{0.1,"0.10000000000000001"},{0.2,"0.20000000000000001"},{0.05,"0.050000000000000003"},
        {0.15000000000000002,"0.15000000000000002"},{12.0,"12"},{4.0,"4"},{1e-5,"1.0000000000000001e-05"},{1e-17,"1.0000000000000001e-17"},
        {1e21,"1e+21"},{1e22,"1e+22"},{123456789.12345679,"123456789.12345679"},{4.9406564584124654e-324,"4.9406564584124654e-324"},
        {1.7976931348623157e308,"1.7976931348623157e+308"},{4503599627370496.0,"4503599627370496"},{-3.5e-7,"-3.4999999999999998e-07"},
        {0.30000000000000004,"0.30000000000000004"},{1.0/3.0,"0.33333333333333331"},{3.141592653589793,"3.1415926535897931"},
        {0.09999999999999999,"0.099999999999999992"},{11.5,"11.5"},{-1.3877787807814457e-17,"-1.3877787807814457e-17"}};
    for(const auto& c:cases){
        CHECK(m2::FormatRecordNumber(c.value)==c.text);
        CHECK(px::LivePlacementNumber(c.value)==c.text);
    }
    // The same text as the two writers had before they were one, over a spread of bit patterns (a fixed generator, no random seed).
    std::uint64_t state=0x9E3779B97F4A7C15ull;
    const auto next=[&]{state^=state<<13;state^=state>>7;state^=state<<17;return state;};
    int tried=0;
    for(int i=0;i<4000;++i){
        const double value=std::bit_cast<double>(next());
        if(!std::isfinite(value))continue;
        ++tried;
        if(m2::FormatRecordNumber(value)!=legacy::ReportNum(value) || px::LivePlacementNumber(value)!=legacy::LogNum(value) ||
            m2::FormatRecordNumber(value)!=px::LivePlacementNumber(value)){CHECK(false);break;}
    }
    CHECK(tried>3900);
    for(int i=0;i<2000;++i){   // round values: the ones a position and its amounts take
        const double value=static_cast<double>(static_cast<std::int64_t>(next()%2000001)-1000000)/static_cast<double>(1+next()%1000);
        CHECK(m2::FormatRecordNumber(value)==legacy::ReportNum(value) && px::LivePlacementNumber(value)==legacy::LogNum(value));
    }
    // A number that is not finite is not written by the one function; the log says nan, as before.
    for(const double bad:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}){
        REJECT(m2::FormatRecordNumber(bad));
        CHECK(px::LivePlacementNumber(bad)=="nan" && legacy::LogNum(bad)=="nan");
    }
    // The log's fields of a position are made of it (the strings the earlier code made are in live_placement_tests; this is the join).
    const m2::RegisteredMapping mapping{{0.1,0,-0.0,0.2,0.05,0},{3,0.01,-0.002},std::nullopt};
    const auto fields=px::LivePlacementFields(mapping);
    CHECK(fields.find("rx_deg="+m2::FormatRecordNumber(0.1)+" ry_deg=0 rz_deg=0 tx_mm="+m2::FormatRecordNumber(0.2))==0);
    CHECK(fields.find(" slope_y="+m2::FormatRecordNumber(-0.002))!=std::string::npos);
}

// ---- 2. The entered amounts ----
void Counts(){
    // The owner's steps: 12 px and 4 px, 0.2 and 0.05 degrees. The doubles are the ones the decimals give.
    const auto coarse_dx=m2::HandValuesOf({m2::kHandCoarseSeamCounts,0,0});
    CHECK(SameBits(coarse_dx.seam_dx_px,12.0) && coarse_dx.seam_dy_px==0 && coarse_dx.rotation_deg==0);
    const auto fine_dy=m2::HandValuesOf({0,m2::kHandFineSeamCounts,0});
    CHECK(fine_dy.seam_dx_px==0 && SameBits(fine_dy.seam_dy_px,4.0));
    CHECK(SameBits(m2::HandValuesOf({0,0,m2::kHandCoarseRotationCounts}).rotation_deg,0.2));
    CHECK(SameBits(m2::HandValuesOf({0,0,m2::kHandFineRotationCounts}).rotation_deg,0.05));
    CHECK(SameBits(m2::HandValuesOf({0,0,-3}).rotation_deg,-0.15));
    CHECK(SameBits(m2::HandValuesOf({-5,7,0}).seam_dx_px,-20.0) && SameBits(m2::HandValuesOf({-5,7,0}).seam_dy_px,28.0));
    // One press moves one count along the axes it names and no more than the counts the four steps name.
    CHECK(m2::kHandCoarseSeamCounts==3 && m2::kHandFineSeamCounts==1 && m2::kHandCoarseRotationCounts==4 && m2::kHandFineRotationCounts==1);

    // A sum that comes back to exactly 0 is exactly 0 whatever the order of the presses: 12, 4 px and 0.2, 0.05 degrees up and down, in every order.
    std::vector<m2::HandCounts> presses{
        {m2::kHandCoarseSeamCounts,0,0},{-m2::kHandCoarseSeamCounts,0,0},{m2::kHandFineSeamCounts,0,0},{-m2::kHandFineSeamCounts,0,0},
        {0,0,m2::kHandCoarseRotationCounts},{0,0,-m2::kHandCoarseRotationCounts},{0,0,m2::kHandFineRotationCounts},{0,0,-m2::kHandFineRotationCounts}};
    std::sort(presses.begin(),presses.end(),PressLess);
    std::size_t orders=0,all_zero=0,float_not_zero=0;
    do{
        ++orders;
        m2::HandCounts total;double degrees=0;
        for(const auto& p:presses){total=m2::AddHandCounts(total,p);degrees+=m2::HandValuesOf(p).rotation_deg;}
        if(total.IsZero())++all_zero;
        if(degrees!=0)++float_not_zero;
    }while(std::next_permutation(presses.begin(),presses.end(),PressLess));
    CHECK(orders==40320 && all_zero==orders);
    // Why they are counted: summed as doubles, the same presses do not always come back to 0 (0.2 + 0.05 - 0.2 - 0.05 is -1.39e-17).
    CHECK(float_not_zero>0);
    // A sum of counts is not wrapped around.
    const std::int32_t top=std::numeric_limits<std::int32_t>::max();
    REJECT_RANGE(m2::AddHandCounts({top,0,0},{1,0,0}));
    REJECT_RANGE(m2::AddHandCounts({0,std::numeric_limits<std::int32_t>::min(),0},{0,-1,0}));
    CHECK(m2::AddHandCounts({top,0,0},{-1,0,0}).seam_dx==top-1);
    CHECK((m2::HandCounts{1,2,3}==m2::HandCounts{1,2,3}) && !(m2::HandCounts{1,2,3}==m2::HandCounts{1,2,4}));
}

// ---- 3. The expression ----
const std::array<m2::HandCounts,7> kEntries{{{3,0,0},{0,3,0},{1,-1,1},{-3,3,-4},{9,-6,12},{-1,0,0},{0,0,-4}}};

void ExpressionOnTheTablePlane(const Rig& rig){
    const Geometry g(rig);
    // The table plane (no surface): the same bits as the condition tool's expression was, and the tool's expression now is.
    const std::array<cr::PoseParameters,4> bases{{{0,0,0,0,0,0},{0,0,0.3,0.4,-0.2,0},{0.2,-0.1,0.3,0.8,-0.6,0.5},{-0.4,0.3,-0.2,-1.1,0.9,-0.7}}};
    const std::array<m2::FixedCompositeRevisionChangeV3::Entered,6> entered{{{12,0,0},{0,-12,0},{4,4,0.05},{-12,12,-0.2},{37.25,-5.5,0.6},{0,0,0.2}}};
    for(const auto& base:bases){
        for(const auto& e:entered){
            const auto reference=legacy::SolveManualChange(g.model_b,g.frame,base,e,g.seam,g.middle);
            const auto tool=px::fixed_tool_v3_detail::SolveManualChange(g.model_b,g.frame,base,e,g.seam,g.middle);
            const m2::RegisteredMapping from{base,{},std::nullopt};
            const auto now=m2::SolveHandChange(g.model_b,g.centre_a,g.origin,g.frame,from,{e.seam_dx_px,e.seam_dy_px,e.rotation_deg},g.seam,g.middle);
            CHECK(SamePoseBits(reference,tool));
            CHECK(SamePoseBits(reference,now));
            // The centre of A and the origin act only for a paper surface.
            const auto decoy=m2::SolveHandChange(g.model_b,cr::CameraCentre{1e6,-1e6,1},render::Point{123,-456},g.frame,from,{e.seam_dx_px,e.seam_dy_px,e.rotation_deg},g.seam,g.middle);
            CHECK(SamePoseBits(reference,decoy));
        }
    }
    // Unsolvable and not-a-request cases throw.
    {
        const m2::RegisteredMapping from{{},{},std::nullopt};
        REJECT(m2::SolveHandChange(g.model_b,g.centre_a,g.origin,g.frame,from,{std::nan(""),0,0},g.seam,g.middle));
        m2::RegisteredMapping with_layer_two=from;with_layer_two.parallax=m2::ParallaxProfile{};
        REJECT(m2::SolveHandChange(g.model_b,g.centre_a,g.origin,g.frame,with_layer_two,{4,0,0},g.seam,g.middle));
        REJECT(m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,with_layer_two,{1,0,0},g.seam,g.middle));
        REJECT(m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,with_layer_two,{0,0,0},g.seam,g.middle));
    }
}

void ExpressionWithAPaperSurface(const Rig& rig){
    const Geometry g(rig);
    const std::array<m2::RegisteredMapping,3> bases{{
        {{0.2,-0.1,0.3,0.8,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt},
        {{0,0,0,0,0,0},{-4.5,-0.01,0.006},std::nullopt},
        {{-0.4,0.3,-0.2,-1.1,0.9,-0.7},{8.0,0.0,0.012},std::nullopt}}};
    for(const auto& base:bases){
        for(const auto& counts:kEntries){
            const auto result=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,counts,g.seam,g.middle);
            const auto values=m2::HandValuesOf(counts);
            // The position's surface and untouched axes are the base's; the rotation is what was entered.
            CHECK(SameSurfaceBits(result.surface,base.surface) && !result.parallax);
            CHECK(SameBits(result.pose.rx_deg,base.pose.rx_deg) && SameBits(result.pose.ry_deg,base.pose.ry_deg) && SameBits(result.pose.tz_percent,base.pose.tz_percent));
            CHECK(SameBits(result.pose.rz_deg,base.pose.rz_deg+values.rotation_deg));
            // The displacement at the seam's middle row is what was entered, worked out with this file's own matrices.
            const auto d=DisplacementBetween(g,base,result,g.seam,g.middle);
            CHECK(std::abs(d.x-values.seam_dx_px)<1e-6 && std::abs(d.y-values.seam_dy_px)<1e-6);
            // And through the renderer's mapping: B at the new position samples at (x, y) the camera pixel that B at the base position
            // sampled at (x + dx, y + dy).
            const auto moved=RawOf(rig,g.model_b,result,g.seam,g.middle);
            const auto before=RawOf(rig,g.model_b,base,g.seam+values.seam_dx_px,g.middle+values.seam_dy_px);
            CHECK(std::hypot(moved.x-before.x,moved.y-before.y)<1e-4);
        }
    }
    // The paper surface changes the answer: the same amounts from the same pose give another pose with a surface than without one.
    {
        const m2::RegisteredMapping flat{bases[0].pose,{},std::nullopt};
        const auto a=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,flat,{3,0,0},g.seam,g.middle);
        const auto b=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,bases[0],{3,0,0},g.seam,g.middle);
        CHECK(!SamePoseBits(a.pose,b.pose));
    }
    // The sum is solved once from the base: the same sum gives the same bits whatever the order of the presses that made it, and the sum of
    // presses is what one solve of the sum gives.
    {
        const auto& base=bases[0];
        std::vector<m2::HandCounts> presses{{3,0,0},{1,0,0},{0,3,0},{0,0,4},{0,0,1},{-1,0,0},{0,-1,0},{0,0,-1}};
        const auto expected_total=m2::HandCounts{3,2,4};
        const auto once=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,expected_total,g.seam,g.middle);
        std::sort(presses.begin(),presses.end(),PressLess);
        int orders=0;
        do{
            m2::HandCounts total;
            for(const auto& p:presses)total=m2::AddHandCounts(total,p);
            CHECK(total==expected_total);
            if(++orders%97==1)CHECK(SameMappingBits(m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,total,g.seam,g.middle),once));
        }while(std::next_permutation(presses.begin(),presses.end(),PressLess));
        CHECK(orders>1000);
        // Not a chain of earlier results: stepping by +3 from the result of +3 is not the position of +6 from the base (the expression is not linear).
        const auto first=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,{3,0,0},g.seam,g.middle);
        const auto chained=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,first,{3,0,0},g.seam,g.middle);
        const auto direct=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,{6,0,0},g.seam,g.middle);
        CHECK(!SamePoseBits(chained.pose,direct.pose));
        CHECK(std::abs(chained.pose.tx_mm-direct.pose.tx_mm)<1e-3);   // close, which is why a chain would not be noticed on the screen
    }
    // A sum of 0 gives the position before the first press, bit for bit, without solving (pose with a negative zero, a surface, every step).
    {
        const m2::RegisteredMapping base{{-0.0,0.1,0.3,0.8,-0.6,-0.0},{3.0,-0.0,-0.002},std::nullopt};
        const auto result=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,{0,0,0},g.seam,g.middle);
        CHECK(SameMappingBits(result,base));
        // Every combination of the owner's steps, there and back.
        const std::array<m2::HandCounts,6> steps{{{3,0,0},{1,0,0},{0,3,0},{0,1,0},{0,0,4},{0,0,1}}};
        for(const auto& up:steps)for(const auto& more:steps){
            const auto total=m2::AddHandCounts(m2::AddHandCounts(up,more),m2::AddHandCounts(m2::HandCounts{-up.seam_dx,-up.seam_dy,-up.rotation},
                m2::HandCounts{-more.seam_dx,-more.seam_dy,-more.rotation}));
            CHECK(total.IsZero() && SameMappingBits(m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,total,g.seam,g.middle),base));
        }
    }
}

// ---- 4. The difference at the seam ----
void SeamDifference(const Rig& rig){
    const Geometry g(rig);
    const m2::RegisteredMapping a{{0.2,-0.1,0.3,0.8,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt};
    const auto same=m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,a,g.seam,g.height);
    CHECK(same==0.0);
    // Another route: the displacements of B against A that the library's RegisteredDisplacement gives for each mapping, subtracted, on rows 0, 32, ...
    const auto other_route=[&](const m2::RegisteredMapping& x,const m2::RegisteredMapping& y,std::uint32_t height){
        double worst=0;
        for(std::uint32_t row=0;row<height;row+=32){
            const auto dx=cr::RegisteredDisplacement(g.model_b,g.frame,g.centre_a,g.origin,x.pose,x.surface,g.seam,row);
            const auto dy=cr::RegisteredDisplacement(g.model_b,g.frame,g.centre_a,g.origin,y.pose,y.surface,g.seam,row);
            worst=std::max(worst,std::hypot(dx.x-dy.x,dx.y-dy.y));
        }
        return worst;
    };
    const std::array<m2::RegisteredMapping,4> others{{
        {{0.2,-0.1,0.3,0.8,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt},   // the same position
        {{0.2,-0.1,0.3,1.2,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt},   // 0.4 mm along x
        {{0.2,-0.1,0.9,0.8,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt},   // 0.6 degrees of rotation
        {{0.2,-0.1,0.3,0.8,-0.6,0.5},{-2.0,-0.01,0.02},std::nullopt}}};  // another surface
    for(const auto& b:others){
        const auto got=m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,b,g.seam,g.height);
        const auto expected=other_route(a,b,g.height);
        CHECK(std::abs(got-expected)<1e-9*std::max(1.0,expected));
        CHECK(std::abs(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,b,a,g.seam,g.height)-got)<1e-9*std::max(1.0,got));   // symmetric
    }
    CHECK(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,others[0],g.seam,g.height)==0.0);
    CHECK(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,others[1],g.seam,g.height)>3.0);
    // What the screen would say of a hand step is about what was pressed: 12 px along the seam from a position gives 12 px, to a few percent.
    {
        const auto moved=m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,a,{3,0,0},g.seam,g.middle);
        const auto d=m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,moved,g.seam,g.height);
        CHECK(d>11.0 && d<13.0);
    }
    // Only rows 0, 32, ... count, and only below the height.
    for(const std::uint32_t height:{1u,32u,33u,64u,65u}){
        const auto got=m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,others[2],g.seam,height);
        CHECK(std::abs(got-other_route(a,others[2],height))<1e-9*std::max(1.0,got));
    }
    REJECT(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,others[1],g.seam,0));
    m2::RegisteredMapping with_layer_two=others[1];with_layer_two.parallax=m2::ParallaxProfile{};
    REJECT(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,a,with_layer_two,g.seam,g.height));
    REJECT(m2::SeamDifferencePx(g.model_b,g.centre_a,g.origin,g.frame,with_layer_two,a,g.seam,g.height));
}


// ---- 5. What a hand step has to pass ----
const cr::RegistrationEnvelope kEnvelope{5,1.5,2.5,3};   // this test's values (the condition's own, as MakeRig writes them)

m2::HandStepInput InputOf(const Rig& rig,const m2::RegisteredMapping& base,const m2::HandCounts& total,const cr::PoseParameters& p_b={},
    const cr::RegistrationEnvelope& envelope=kEnvelope){
    m2::HandStepInput in;
    in.parameters=&rig.binding.parameters;in.seam_column=rig.condition.seam.column;
    in.base_pose=p_b;in.envelope=envelope;in.hand_base=base;in.total=total;
    return in;
}
m2::RegisteredMapping Solved(const Rig& rig,const m2::RegisteredMapping& base,const m2::HandCounts& total){
    const Geometry g(rig);
    return m2::SolveHandStep(g.model_b,g.centre_a,g.origin,g.frame,base,total,g.seam,g.middle);
}
double PositionOf(const cr::PoseParameters& pose,const cr::PoseParameters& p_b){return std::hypot(pose.tx_mm-p_b.tx_mm,pose.ty_mm-p_b.ty_mm);}

void AcceptedSteps(const Rig& rig){
    const m2::RegisteredMapping bases[]{{{0,0,0,0,0,0},{},std::nullopt},{{0.2,-0.1,0.3,0.8,-0.6,0.5},{3.0,0.004,-0.002},std::nullopt}};
    for(const auto& base:bases){
        for(const m2::HandCounts counts:{m2::HandCounts{3,0,0},m2::HandCounts{0,-3,0},m2::HandCounts{1,1,1},m2::HandCounts{0,0,4},m2::HandCounts{-1,0,-1}}){
            const auto verdict=m2::CheckHandStep(InputOf(rig,base,counts));
            CHECK(verdict.status==m2::HandStepStatus::accepted);
            CHECK(verdict.cover_checked && verdict.seam_covered && verdict.output_covered);
            CHECK(SameMappingBits(verdict.mapping,Solved(rig,base,counts)));
            CHECK(verdict.components.size()==4 && verdict.components[0].component==cr::EnvelopeComponent::position && verdict.components[1].component==cr::EnvelopeComponent::rotation &&
                verdict.components[2].component==cr::EnvelopeComponent::tilt && verdict.components[3].component==cr::EnvelopeComponent::height);
            for(const auto& c:verdict.components)CHECK(!c.exceeded);
        }
    }
    // The paper surface's range is not read: a surface beyond the condition's own surface range (height 10 mm, tilt 4 degrees) does not stop a step
    // whose pose is inside the pose envelope.
    {
        CHECK(rig.condition.registration.surface_envelope && rig.condition.registration.surface_envelope->height_mm==10);
        const m2::RegisteredMapping high{{0,0,0,0.3,0,0},{15.0,0.02,-0.01},std::nullopt};
        const auto verdict=m2::CheckHandStep(InputOf(rig,high,{1,0,0}));
        CHECK(verdict.status==m2::HandStepStatus::accepted && SameSurfaceBits(verdict.mapping.surface,high.surface));
    }
    // A sum of 0 is not a step: the position goes back as it is, without a check, even when it is outside the envelope.
    {
        const m2::RegisteredMapping outside{{0,0,0,6.5,0,0},{3.0,0.0,0.0},std::nullopt};
        const auto verdict=m2::CheckHandStep(InputOf(rig,outside,{0,0,0}));
        CHECK(verdict.status==m2::HandStepStatus::accepted && !verdict.cover_checked && verdict.components.empty() && SameMappingBits(verdict.mapping,outside));
    }
}

void TheEdgeOfTheEnvelope(const Rig& rig){
    const auto edge_down=[](double v){return std::nextafter(v,0.0);};
    // Position: exactly at the limit is inside, one count further is not, a limit one ulp lower is not. Measured from p_b, not from zero.
    for(const cr::PoseParameters p_b:{cr::PoseParameters{},cr::PoseParameters{0,0,0.1,0.2,-0.1,0}}){
        const m2::RegisteredMapping base{p_b,{},std::nullopt};
        const auto three=Solved(rig,base,{3,0,0});
        const double at_three=PositionOf(three.pose,p_b);
        CHECK(at_three>0.5 && at_three<5);
        auto envelope=kEnvelope;envelope.position_mm=at_three;
        auto verdict=m2::CheckHandStep(InputOf(rig,base,{3,0,0},p_b,envelope));
        CHECK(verdict.status==m2::HandStepStatus::accepted && verdict.components[0].value==at_three && verdict.components[0].limit==at_three && !verdict.components[0].exceeded);
        verdict=m2::CheckHandStep(InputOf(rig,base,{4,0,0},p_b,envelope));
        CHECK(verdict.status==m2::HandStepStatus::refused_envelope && verdict.components[0].exceeded && !verdict.components[1].exceeded && !verdict.cover_checked);
        CHECK(SameMappingBits(verdict.mapping,Solved(rig,base,{4,0,0})));
        envelope.position_mm=edge_down(at_three);
        verdict=m2::CheckHandStep(InputOf(rig,base,{3,0,0},p_b,envelope));
        CHECK(verdict.status==m2::HandStepStatus::refused_envelope && verdict.components[0].exceeded);
        // Not rounded: the refused step's position is its own, not the edge.
        CHECK(verdict.mapping.pose.tx_mm==three.pose.tx_mm && verdict.mapping.pose.ty_mm==three.pose.ty_mm);
        // The other directions and axes: the step is measured whichever way it goes.
        for(const m2::HandCounts counts:{m2::HandCounts{-3,0,0},m2::HandCounts{0,3,0},m2::HandCounts{0,-3,0}}){
            const double v=PositionOf(Solved(rig,base,counts).pose,p_b);
            auto e=kEnvelope;e.position_mm=v;
            CHECK(m2::CheckHandStep(InputOf(rig,base,counts,p_b,e)).status==m2::HandStepStatus::accepted);
            e.position_mm=edge_down(v);
            CHECK(m2::CheckHandStep(InputOf(rig,base,counts,p_b,e)).status==m2::HandStepStatus::refused_envelope);
        }
    }
    // Rotation: 0.2 degrees exactly at the limit is inside; 0.25 is not; a limit one ulp lower is not. The position that the step moves along
    // has the room of the envelope, so it is the rotation that is measured.
    {
        const cr::PoseParameters p_b{0,0,0.1,0.2,-0.1,0};
        const m2::RegisteredMapping base{p_b,{},std::nullopt};
        const auto four=Solved(rig,base,{0,0,4});
        const double at_four=std::abs(four.pose.rz_deg-p_b.rz_deg);
        CHECK(at_four>0.19 && at_four<0.21);
        auto envelope=kEnvelope;envelope.rotation_deg=at_four;
        auto verdict=m2::CheckHandStep(InputOf(rig,base,{0,0,4},p_b,envelope));
        CHECK(verdict.status==m2::HandStepStatus::accepted && !verdict.components[1].exceeded);
        verdict=m2::CheckHandStep(InputOf(rig,base,{0,0,5},p_b,envelope));
        CHECK(verdict.status==m2::HandStepStatus::refused_envelope && verdict.components[1].exceeded && !verdict.components[0].exceeded);
        envelope.rotation_deg=edge_down(at_four);
        CHECK(m2::CheckHandStep(InputOf(rig,base,{0,0,4},p_b,envelope)).status==m2::HandStepStatus::refused_envelope);
        envelope.rotation_deg=at_four;
        CHECK(m2::CheckHandStep(InputOf(rig,base,{0,0,-4},p_b,envelope)).status==m2::HandStepStatus::accepted);
    }
}

void AnOutsidePosition(const Rig& rig){
    // The position is already outside the envelope (an estimate applied beyond it): only a step whose result is inside is accepted.
    const cr::PoseParameters p_b{};
    for(const double tx:{5.2,5.9}){
        const m2::RegisteredMapping base{{0,0,0,tx,0,0},{},std::nullopt};
        // Which way is toward the envelope: a step across the seam (dy) moves tx, a step along it (dx) moves ty.
        const int toward=Solved(rig,base,{0,1,0}).pose.tx_mm<tx?1:-1;
        CHECK(std::abs(Solved(rig,base,{0,toward,0}).pose.tx_mm-tx)>0.2);
        const auto reducing=m2::CheckHandStep(InputOf(rig,base,{0,toward,0},p_b));
        const auto away=m2::CheckHandStep(InputOf(rig,base,{0,-toward,0},p_b));
        const double after=PositionOf(Solved(rig,base,{0,toward,0}).pose,p_b);
        if(after<=kEnvelope.position_mm){
            CHECK(reducing.status==m2::HandStepStatus::accepted && !reducing.components[0].exceeded);
        }else{
            // It reduces the position that is outside, and it is still outside: refused, not rounded.
            CHECK(reducing.status==m2::HandStepStatus::refused_envelope && reducing.components[0].exceeded && reducing.components[0].value<tx);
        }
        CHECK(away.status==m2::HandStepStatus::refused_envelope && away.components[0].exceeded);
    }
    // The nearer position can step in; the farther one, where one count still leaves it outside, cannot.
    {
        const m2::RegisteredMapping near{{0,0,0,5.2,0,0},{},std::nullopt},far{{0,0,0,5.9,0,0},{},std::nullopt};
        const int toward=Solved(rig,near,{0,1,0}).pose.tx_mm<5.2?1:-1;
        CHECK(m2::CheckHandStep(InputOf(rig,near,{0,toward,0},p_b)).status==m2::HandStepStatus::accepted);
        CHECK(m2::CheckHandStep(InputOf(rig,far,{0,toward,0},p_b)).status==m2::HandStepStatus::refused_envelope);
    }
    // A tilt or a height outside the envelope: a hand step moves neither, so no step leads inside. Every step is refused, in both directions, the
    // coarse and the fine ones; the controls (the same position with the tilt or height inside) are accepted.
    const std::array<m2::HandCounts,8> steps{{{3,0,0},{-3,0,0},{0,3,0},{0,-3,0},{0,0,4},{0,0,-4},{1,0,0},{0,0,-1}}};
    struct Outside {cr::PoseParameters pose;std::size_t component;};
    const std::array<Outside,4> outsides{{{{3.0,0,0,0,0,0},2},{{0,-2.6,0,0.3,0,0},2},{{0,0,0,0,0,3.5},3},{{0.5,0.5,0.2,0.4,0,-3.2},3}}};
    for(const auto& o:outsides){
        const m2::RegisteredMapping base{o.pose,{},std::nullopt};
        for(const auto& step:steps){
            const auto verdict=m2::CheckHandStep(InputOf(rig,base,step,p_b));
            CHECK(verdict.status==m2::HandStepStatus::refused_envelope && verdict.components[o.component].exceeded);
        }
        auto inside=o.pose;inside.rx_deg=o.pose.rx_deg*0.5;inside.ry_deg=o.pose.ry_deg*0.5;inside.tz_percent=o.pose.tz_percent*0.5;
        CHECK(m2::CheckHandStep(InputOf(rig,m2::RegisteredMapping{inside,{},std::nullopt},{3,0,0},p_b)).status==m2::HandStepStatus::accepted);
    }
}

void RefusedByTheCoverCheck(const Rig& rig){
    // A range that does not stop anything, so that it is the cheap check that refuses. Each direction is stepped further until the mapping leaves the
    // overlap or the output; what is refused carries which of the two it was, and the same answer as the check itself gives for that mapping.
    const cr::RegistrationEnvelope huge{1e9,1e9,1e9,1e9};
    const m2::RegisteredMapping base{{},{},std::nullopt};
    const m2::HandCounts directions[]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    bool saw_seam=false,saw_refusal=false;
    for(const auto& d:directions){
        for(std::int32_t k=8;k<=(1<<17);k*=2){
            const m2::HandCounts counts{d.seam_dx*k,d.seam_dy*k,0};
            const auto verdict=m2::CheckHandStep(InputOf(rig,base,counts,{},huge));
            if(verdict.status==m2::HandStepStatus::accepted){CHECK(verdict.seam_covered && verdict.output_covered);continue;}
            if(verdict.status==m2::HandStepStatus::refused_unsolvable)break;
            CHECK(verdict.status==m2::HandStepStatus::refused_cover && verdict.cover_checked && !(verdict.seam_covered && verdict.output_covered));
            const auto direct=m2::CheckRegisteredMappingCover(rig.binding.parameters,verdict.mapping,rig.condition.seam.column,m2::kSeamBlendDesign);
            CHECK(direct.seam_covered==verdict.seam_covered && direct.output_covered==verdict.output_covered);
            saw_refusal=true;saw_seam=saw_seam || !verdict.seam_covered;
            std::cerr<<"  cover refusal at "<<k<<" counts (dx "<<d.seam_dx<<", dy "<<d.seam_dy<<"): seam_covered="<<verdict.seam_covered<<" output_covered="<<verdict.output_covered<<'\n';
            break;
        }
    }
    CHECK(saw_refusal);
    CHECK(saw_seam);
}

void StepsThatCannotBeTaken(const Rig& rig){
    // A position whose camera would be at the table cannot be turned into a homography: unsolvable, not an exception and not an acceptance.
    {
        const m2::RegisteredMapping base{{0,0,0,0,0,-100.0},{},std::nullopt};
        const auto verdict=m2::CheckHandStep(InputOf(rig,base,{1,0,0}));
        CHECK(verdict.status==m2::HandStepStatus::refused_unsolvable && !verdict.cover_checked && verdict.components.empty() && SameMappingBits(verdict.mapping,base));
    }
    // A surface that is not below the cameras cannot be drawn either.
    {
        const m2::RegisteredMapping base{{0,0,0,0,0,0},{5000.0,0,0},std::nullopt};
        const auto verdict=m2::CheckHandStep(InputOf(rig,base,{1,0,0}));
        CHECK(verdict.status==m2::HandStepStatus::refused_unsolvable && SameMappingBits(verdict.mapping,base));
    }
    // Not a request: no profile, a seam column outside the raster, layer 2, a position that is not finite, an envelope that is not set.
    {
        const m2::RegisteredMapping base{{},{},std::nullopt};
        auto in=InputOf(rig,base,{1,0,0});in.parameters=nullptr;
        REJECT(m2::CheckHandStep(in));
        in=InputOf(rig,base,{1,0,0});in.seam_column=rig.binding.parameters.raster.width_pixels;
        REJECT(m2::CheckHandStep(in));
        m2::RegisteredMapping with_layer_two=base;with_layer_two.parallax=m2::ParallaxProfile{};
        REJECT(m2::CheckHandStep(InputOf(rig,with_layer_two,{1,0,0})));
        REJECT(m2::CheckHandStep(InputOf(rig,with_layer_two,{0,0,0})));
        m2::RegisteredMapping not_finite=base;not_finite.pose.tx_mm=std::numeric_limits<double>::infinity();
        REJECT(m2::CheckHandStep(InputOf(rig,not_finite,{1,0,0})));
        REJECT(m2::CheckHandStep(InputOf(rig,base,{1,0,0},{},cr::RegistrationEnvelope{})));
    }
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const auto draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        const auto step=[](const char* name){std::cerr<<"["<<name<<"]"<<std::endl;};
        step("numbers");Numbers();
        step("counts");Counts();
        const auto rig=MakeRig(draft);
        step("expression on the table plane");ExpressionOnTheTablePlane(rig);
        step("expression with a paper surface");ExpressionWithAPaperSurface(rig);
        step("seam difference");SeamDifference(rig);
        step("accepted steps");AcceptedSteps(rig);
        step("the edge of the envelope");TheEdgeOfTheEnvelope(rig);
        step("an outside position");AnOutsidePosition(rig);
        step("cover refusals");RefusedByTheCoverCheck(rig);
        step("steps that cannot be taken");StepsThatCannotBeTaken(rig);
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
