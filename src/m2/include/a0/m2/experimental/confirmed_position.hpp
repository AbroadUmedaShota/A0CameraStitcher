#pragma once
#include "a0/m2/experimental/registration_mapping.hpp"
#include "a0/m2/experimental/registration_pose.hpp"
#include "a0/m2/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::m2::experimental {
// The pure parts of the confirmed position of the development preview (#437 stage 2, docs/DECISIONS.md ADR-0046): how a number is
// written in a record, the entered amounts of a hand step and the one expression that turns them into a position, and the
// difference between two positions at the seam. Header only and free of any window, clock, file or thread. The one dependency of
// the arithmetic is registration_pose.hpp (a function that needs more is in confirmed_position_check.hpp).

// ---- The written form of a number ----
// The one function that writes a number of a record: 17 significant digits, the classic locale, a negative zero as 0. The report's
// numbers and the log's numbers are both written with it, so a position reads the same in both. A number that is not finite is not
// written: it throws std::invalid_argument (the report treats this as a fault; the log writes its own word for it before calling).
[[nodiscard]] inline std::string FormatRecordNumber(double value){
    if(!std::isfinite(value))throw std::invalid_argument("a record number is not finite");
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(value==0?0.0:value);
    return out.str();
}

// ---- The two cameras of a position ----
// How a record names them (ADR-0045: the reference camera and the target camera). The report's confirmedPosition and the logs write these.
inline constexpr std::string_view kConfirmedReferenceCamera="CAM-A",kConfirmedTargetCamera="CAM-B";

// ---- The condition's own position ----
// Whether a mapping is the condition's own position: the base pose and no paper surface, compared by value (-0 and +0 are the same,
// NaN is never the same). The machine that holds the position and the capture that is given it both call this one function.
[[nodiscard]] inline bool IsConditionPosition(const RegisteredMapping& mapping,const content_registration::PoseParameters& base_pose){
    return mapping.pose.rx_deg==base_pose.rx_deg && mapping.pose.ry_deg==base_pose.ry_deg && mapping.pose.rz_deg==base_pose.rz_deg &&
        mapping.pose.tx_mm==base_pose.tx_mm && mapping.pose.ty_mm==base_pose.ty_mm && mapping.pose.tz_percent==base_pose.tz_percent && mapping.surface.IsZero();
}

// ---- The entered amounts of a hand step ----
// A person moves the position along and across the seam and about the optical axis. The amounts are counted in the smallest step
// (4 output px along or across the seam, 0.05 degrees about the axis) as whole numbers, so that a sum that comes back to exactly 0 is
// exactly 0 whatever the order of the presses. A value for the equations is made from the numbers once, here: pixels are the number
// times 4, degrees are the number divided by 20 (the nearest double of the decimal, e.g. 4 / 20 is the double 0.2).
inline constexpr double kHandSeamPixelsPerCount=4.0;
inline constexpr double kHandRotationCountsPerDegree=20.0;
// The steps of the buttons, in counts: 12 px and 4 px, 0.2 and 0.05 degrees (the owner's decision recorded in ADR-0046).
inline constexpr std::int32_t kHandCoarseSeamCounts=3;
inline constexpr std::int32_t kHandFineSeamCounts=1;
inline constexpr std::int32_t kHandCoarseRotationCounts=4;
inline constexpr std::int32_t kHandFineRotationCounts=1;

struct HandCounts {
    std::int32_t seam_dx{};      // along the seam's baseline, to the right in the output
    std::int32_t seam_dy{};      // across it, downward in the output
    std::int32_t rotation{};     // about the optical axis
    [[nodiscard]] bool IsZero() const {return seam_dx==0 && seam_dy==0 && rotation==0;}
    [[nodiscard]] friend bool operator==(const HandCounts& a,const HandCounts& b){return a.seam_dx==b.seam_dx && a.seam_dy==b.seam_dy && a.rotation==b.rotation;}
};
// The sum of two entries. Throws std::out_of_range when a component leaves the range of the counts (no wrap-around).
[[nodiscard]] inline HandCounts AddHandCounts(const HandCounts& a,const HandCounts& b){
    const auto add=[](std::int32_t x,std::int32_t y){
        const std::int64_t sum=static_cast<std::int64_t>(x)+y;
        if(sum<std::numeric_limits<std::int32_t>::min() || sum>std::numeric_limits<std::int32_t>::max())throw std::out_of_range("the entered hand amount is out of range");
        return static_cast<std::int32_t>(sum);
    };
    return {add(a.seam_dx,b.seam_dx),add(a.seam_dy,b.seam_dy),add(a.rotation,b.rotation)};
}
// The entered amounts as the equations and the records take them: the displacement at the seam column's middle row (output px) and the
// rotation (degrees). The same members as the condition's manual-adjustment `entered`.
struct HandValues {
    double seam_dx_px{},seam_dy_px{},rotation_deg{};
};
[[nodiscard]] inline HandValues HandValuesOf(const HandCounts& counts){
    return {static_cast<double>(counts.seam_dx)*kHandSeamPixelsPerCount,static_cast<double>(counts.seam_dy)*kHandSeamPixelsPerCount,
        static_cast<double>(counts.rotation)/kHandRotationCountsPerDegree};
}

namespace hand_detail {
namespace cr=content_registration;
// Document-plane homography applied to an output sample: where the sample goes, in output pixels, relative to where it was.
inline render::Point Moved(const render::Matrix3& g,const cr::PlaneFrame& frame,double x,double y){
    const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
    const double w=g[6]*px+g[7]*py+g[8];
    return {((g[0]*px+g[1]*py+g[2])/w-px)/frame.pitch_mm,((g[3]*px+g[4]*py+g[5])/w-py)/frame.pitch_mm};
}
inline render::Matrix3 Multiply(const render::Matrix3& a,const render::Matrix3& b){
    render::Matrix3 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=a[3*i+k]*b[3*k+j];r[3*i+j]=s;}
    return r;
}
inline cr::PoseParameters Added(const cr::PoseParameters& a,const cr::PoseParameters& b){
    return {a.rx_deg+b.rx_deg,a.ry_deg+b.ry_deg,a.rz_deg+b.rz_deg,a.tx_mm+b.tx_mm,a.ty_mm+b.ty_mm,a.tz_percent+b.tz_percent};
}
}

// The pose change (rz as entered, tx and ty found) that puts camera B's content at the seam column's middle row (x, y) at the entered
// displacement (output px, x to the right and y down), with the paper surface of `base` kept. The displacement is that of
// RegisteredHomography(base.pose + change, surface) against RegisteredHomography(base.pose, surface), measured on the document plane as
// the condition's manual adjustment does. Newton on two unknowns with a finite-difference Jacobian; the displacement is almost linear in
// them, so a few steps are enough. Throws std::invalid_argument when it does not converge, when the displacement does not determine a
// translation, or for a mapping that carries layer 2 (a confirmed position has none).
// With the table plane for the surface (all three components 0) this is, bit for bit, what the condition tool's SolveManualChange was
// before it called this: the same homographies, the same step, tolerance and iteration count. centre_a and origin_mm act only for a
// paper surface.
[[nodiscard]] inline content_registration::PoseParameters SolveHandChange(const content_registration::PoseModel& model_b,
    const content_registration::CameraCentre& centre_a,render::Point origin_mm,const content_registration::PlaneFrame& frame,
    const RegisteredMapping& base,const HandValues& entered,double x,double y){
    namespace cr=content_registration;
    if(base.parallax)throw std::invalid_argument("a hand step starts from a position without layer 2");
    cr::PoseParameters delta{};delta.rz_deg=entered.rotation_deg;
    const auto inverse_from=render::Invert(cr::RegisteredHomography(model_b,centre_a,origin_mm,base.pose,base.surface));
    const auto at=[&](const cr::PoseParameters& d){
        const auto to=cr::RegisteredHomography(model_b,centre_a,origin_mm,hand_detail::Added(base.pose,d),base.surface);
        return hand_detail::Moved(hand_detail::Multiply(inverse_from,to),frame,x,y);
    };
    constexpr double kStepMm=1e-3,kToleranceMm=1e-12;
    for(int i=0;i<12;++i){
        const auto d=at(delta);
        const double ex=entered.seam_dx_px-d.x,ey=entered.seam_dy_px-d.y;
        if(std::hypot(ex,ey)<1e-10)return delta;
        auto px=delta;px.tx_mm+=kStepMm;auto py=delta;py.ty_mm+=kStepMm;
        const auto dx=at(px),dy=at(py);
        const double a=(dx.x-d.x)/kStepMm,c=(dx.y-d.y)/kStepMm,b=(dy.x-d.x)/kStepMm,e=(dy.y-d.y)/kStepMm;   // [a b; c e] (tx, ty) = (ex, ey)
        const double det=a*e-b*c;
        if(!(std::abs(det)>kToleranceMm))throw std::invalid_argument("the seam displacement does not determine a translation");
        delta.tx_mm+=(ex*e-b*ey)/det;delta.ty_mm+=(a*ey-c*ex)/det;
        if(!std::isfinite(delta.tx_mm) || !std::isfinite(delta.ty_mm))throw std::invalid_argument("the seam displacement gives no pose");
    }
    const auto d=at(delta);
    if(std::hypot(entered.seam_dx_px-d.x,entered.seam_dy_px-d.y)>1e-6)throw std::invalid_argument("the seam displacement could not be turned into a pose");
    return delta;
}

// A hand step: the position before the first hand press (`base`, with its paper surface and no layer 2) plus the sum of the amounts
// entered since, solved once. Each press does not build on the one before; the same sum from the same base is the same position,
// whatever the order of the presses. A sum that is 0 in all three counts returns `base` itself without solving, so the position goes
// back to exactly what it was. The surface is `base`'s. Throws what SolveHandChange throws.
[[nodiscard]] inline RegisteredMapping SolveHandStep(const content_registration::PoseModel& model_b,const content_registration::CameraCentre& centre_a,
    render::Point origin_mm,const content_registration::PlaneFrame& frame,const RegisteredMapping& base,const HandCounts& total,double x,double y){
    if(base.parallax)throw std::invalid_argument("a hand step starts from a position without layer 2");
    if(total.IsZero())return base;
    const auto delta=SolveHandChange(model_b,centre_a,origin_mm,frame,base,HandValuesOf(total),x,y);
    RegisteredMapping result;
    result.pose=hand_detail::Added(base.pose,delta);
    result.surface=base.surface;
    return result;
}

// ---- The difference between two positions at the seam ----
// The one measure of how far two positions of camera B are apart at the seam: for the output samples of the seam column on rows 0, 32,
// 64, ... (the estimator's rows) and on the last row (a difference grows towards the ends of the seam, so the end is always a sample),
// the distance between the document points that the two mappings give for the same sample, in output pixels; the largest of them. The
// screen's sentence after a shot, the live log's seam_difference_px and the report's `seamDifferencePx` are all made from this function. Neither
// mapping may carry layer 2 (std::invalid_argument). origin_mm is content_registration::SurfaceOriginMm of the raster and the seam column.
[[nodiscard]] inline double SeamDifferencePx(const content_registration::PoseModel& model_b,const content_registration::CameraCentre& centre_a,
    render::Point origin_mm,const content_registration::PlaneFrame& frame,const RegisteredMapping& a,const RegisteredMapping& b,
    std::uint32_t seam_column,std::uint32_t height){
    namespace cr=content_registration;
    if(a.parallax || b.parallax)throw std::invalid_argument("the difference at the seam is measured without layer 2");
    if(height==0)throw std::invalid_argument("the output has no rows");
    const auto ha=cr::RegisteredHomography(model_b,centre_a,origin_mm,a.pose,a.surface);
    const auto hb=cr::RegisteredHomography(model_b,centre_a,origin_mm,b.pose,b.surface);
    const auto apply=[](const render::Matrix3& g,double px,double py){
        const double w=g[6]*px+g[7]*py+g[8];
        return render::Point{(g[0]*px+g[1]*py+g[2])/w,(g[3]*px+g[4]*py+g[5])/w};
    };
    double worst=0;
    const auto measure=[&](std::uint32_t row){
        const double px=frame.left_mm+(static_cast<double>(seam_column)+0.5)*frame.pitch_mm,py=frame.top_mm+(static_cast<double>(row)+0.5)*frame.pitch_mm;
        const auto pa=apply(ha,px,py),pb=apply(hb,px,py);
        const double distance=std::hypot(pa.x-pb.x,pa.y-pb.y)/frame.pitch_mm;
        if(!std::isfinite(distance))throw std::invalid_argument("the difference at the seam is not finite");
        worst=std::max(worst,distance);
    };
    for(std::uint32_t row=0;row<height;row+=32)measure(row);
    if((height-1)%32!=0)measure(height-1);
    return worst;
}
}
