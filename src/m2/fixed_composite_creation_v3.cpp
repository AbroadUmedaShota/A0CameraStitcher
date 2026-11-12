#include "a0/m2/experimental/fixed_composite_creation_v3.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/optical_planner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace a0::m2::experimental {
namespace {
using content_registration::PlaneFrame;
using content_registration::PoseModel;
using content_registration::PoseParameters;
using render::Matrix3;
using render::OutputToCamera;
using render::Point;

constexpr const char* kPrefix="fixed composite creation v3: ";
[[noreturn]] void Reject(const std::string& what){throw std::invalid_argument(kPrefix+what);}
std::string Text(double v){
    char text[40];const auto r=std::to_chars(text,text+sizeof text,v);
    return std::string(text,r.ptr);
}

// ---- Design constants of this check (an algorithm's, not acceptance values; none comes from an owner) ----
// How far the linear prediction of a margin may differ from the true value at the predicted worst vertex before the block is
// checked by enumerating the vertices of the range (11.23.4), in output pixels.
constexpr double kLinearizationTolerancePx=1.0;
// Rows per block of the worst-vertex verification.
constexpr std::uint32_t kBlockRows=32;
// How far past the output (output pixels, each side) a camera's cover is looked for, so that a margin beyond the output is a number.
constexpr double kEdgeSearchSlackPx=64.0;
// Bisection steps of an edge search: the edge is found to within the doubling step times 2^-kBisections.
constexpr int kBisections=18;

// ---- Cover of one row by one camera ----
bool Covered(const OutputToCamera& map,double x,double y){
    Point raw{};
    if(!map({x,y},raw))return false;
    if(!std::isfinite(raw.x) || !std::isfinite(raw.y))throw std::invalid_argument("output-to-camera mapping returned nonfinite camera coordinates");
    return raw.x>=0.0 && raw.y>=0.0 && raw.x<static_cast<double>(kD810SensorWidthPixels) && raw.y<static_cast<double>(kD810SensorHeightPixels);
}
// The run of covered positions around x0 on the row: [left,right] in output pixels, the outermost covered positions found (to the
// bisection's precision), or the search limit when the cover reaches it. `ok` is false when x0 itself is not covered.
struct Run {bool ok{};double left{},right{};};
double EdgeSearch(const OutputToCamera& map,double y,double x0,double direction,double limit){
    double inside=x0,outside=x0,step=1;
    for(;;){
        const double x=x0+direction*step;
        if((direction<0 && x<limit) || (direction>0 && x>limit))return limit;
        if(!Covered(map,x,y)){outside=x;break;}
        inside=x;step*=2;
    }
    for(int i=0;i<kBisections;++i){
        const double mid=(inside+outside)/2;
        if(Covered(map,mid,y))inside=mid;else outside=mid;
    }
    return inside;
}
Run RunAround(const OutputToCamera& map,double y,double x0,double width){
    if(!Covered(map,x0,y))return {};
    return {true,EdgeSearch(map,y,x0,-1,-kEdgeSearchSlackPx),EdgeSearch(map,y,x0,+1,width+kEdgeSearchSlackPx)};
}

// ---- The margins of a row. Each is >= 0 where the check holds ----
struct RowMargins {
    bool ok{};
    double seam_left{};     // (a) B covers the seam column minus half the blend width: (seam - h) - left edge of B
    double seam_right{};    // (a) B covers the seam column plus half the blend width: right edge of B - (seam + h)
    double output_end{};    // (b) the last output column is covered by A or by B: max(right edge of A, right edge of B) - (w - 1)
};
struct Context {
    const render::DocumentRenderParameters& parameters;
    PoseModel model;
    PlaneFrame frame;
    OutputToCamera map_a;
    std::uint32_t seam{},width{},height{};
    double half{};          // half the blend width: the seam column +- half must be covered
    std::vector<double> a_right;   // right edge of A on each row (A does not move)
    std::vector<bool> a_ok;
};
// B's mapping for a pose and a paper surface: the registered mapping of fixed_composite_maps (the zero surface is the pose-only mapping,
// bit for bit). Layer 2 is not part of it here: its effect is bounded by the lift cap (see CheckPoseRange).
OutputToCamera MapB(const Context& ctx,const PoseParameters& pose,const SurfacePlane& surface={}){
    RegisteredMapping mapping;mapping.pose=pose;mapping.surface=surface;
    return RegisteredDocumentOutputToCamera(ctx.parameters.raster,ctx.parameters.camera_a,ctx.parameters.camera_b,ctx.model,mapping,ctx.seam);
}
RowMargins MarginsOfRow(const Context& ctx,const OutputToCamera& map_b,std::uint32_t y){
    RowMargins m;
    const auto run=RunAround(map_b,static_cast<double>(y),static_cast<double>(ctx.seam),ctx.width);
    if(!run.ok || !ctx.a_ok[y])return m;
    m.ok=true;
    m.seam_left=(ctx.seam-ctx.half)-run.left;
    m.seam_right=run.right-(ctx.seam+ctx.half);
    m.output_end=std::max(ctx.a_right[y],run.right)-(ctx.width-1.0);
    return m;
}
std::vector<RowMargins> MarginsOfRows(const Context& ctx,const PoseParameters& pose,const SurfacePlane& surface={}){
    const auto map_b=MapB(ctx,pose,surface);
    std::vector<RowMargins> rows(ctx.height);
    for(std::uint32_t y=0;y<ctx.height;++y)rows[y]=MarginsOfRow(ctx,map_b,y);
    return rows;
}
enum class Kind {seam_left,seam_right,output_end};
constexpr std::array<Kind,3> kKinds{Kind::seam_left,Kind::seam_right,Kind::output_end};
const char* KindName(Kind k){return k==Kind::seam_left?"the seam and half the blend width inside B's cover (left)":
    k==Kind::seam_right?"the seam and half the blend width inside B's cover (right)":"every output pixel covered by A or B";}
double Value(const RowMargins& m,Kind k){return k==Kind::seam_left?m.seam_left:k==Kind::seam_right?m.seam_right:m.output_end;}

// ---- The range as a set of parameter vectors ----
// Theta without layer 2: p_b + (a disc in (tx, ty)) x (a disc in (rx, ry)) x [-rz, rz] x [-tz, tz], times the paper surface
// [-h0, h0] x (a disc in the two slopes, of radius tan(tilt)). An offset is (tx,ty,rx,ry,rz,tz,h0,gx,gy); the surface is part of the range
// of the seam margins (a) and not of (b), which is made at pi = 0 (design 11.23.2).
constexpr int kAxes=9;
struct Ranges {double position_mm{},tilt_deg{},rotation_deg{},height_percent{},surface_height_mm{},surface_slope{};bool surface{};};
struct Offset {double tx{},ty{},rx{},ry{},rz{},tz{},h0{},gx{},gy{};};
PoseParameters Moved(const PoseParameters& base,const Offset& o){
    return {base.rx_deg+o.rx,base.ry_deg+o.ry,base.rz_deg+o.rz,base.tx_mm+o.tx,base.ty_mm+o.ty,base.tz_percent+o.tz};
}
SurfacePlane SurfaceOf(const Offset& o){SurfacePlane s;s.h0_mm=o.h0;s.slope_x=o.gx;s.slope_y=o.gy;return s;}
struct Lines {
    std::vector<RowMargins> centre;
    // For each row and kind: the signed half-differences (m(+r) - m(-r)) / 2 along each axis (a disc counts as two axes).
    std::vector<std::array<std::array<double,kAxes>,3>> half_difference;
};

// The worst offset of a row for a margin, given the signed half-differences: each scalar to the side that lowers the margin, each disc to
// the point of its circle that lowers it most.
Offset WorstOffset(const std::array<double,kAxes>& d,const Ranges& r,bool with_surface){
    Offset o;
    const auto disc=[](double dx,double dy,double radius,double& ox,double& oy){
        const double n=std::hypot(dx,dy);
        if(n>0){ox=-radius*dx/n;oy=-radius*dy/n;}
    };
    disc(d[0],d[1],r.position_mm,o.tx,o.ty);
    disc(d[2],d[3],r.tilt_deg,o.rx,o.ry);
    o.rz=d[4]>0?-r.rotation_deg:d[4]<0?r.rotation_deg:0;
    o.tz=d[5]>0?-r.height_percent:d[5]<0?r.height_percent:0;
    if(with_surface && r.surface){
        o.h0=d[6]>0?-r.surface_height_mm:d[6]<0?r.surface_height_mm:0;
        disc(d[7],d[8],r.surface_slope,o.gx,o.gy);
    }
    return o;
}
double PredictedWorst(double centre,const std::array<double,kAxes>& d,double bound,bool with_surface){
    double worst=centre-std::hypot(d[0],d[1])-std::hypot(d[2],d[3])-std::abs(d[4])-std::abs(d[5])-bound;
    if(with_surface)worst-=std::abs(d[6])+std::hypot(d[7],d[8]);
    return worst;
}

// The vertices of the range for a row: each disc by the regular octagon that contains it (8 points on a circle larger by 1/cos(pi/8)), each
// scalar by its two ends: 8 x 8 x 2 x 2 = 256 offsets, and with the paper surface 8 x 2 times as many.
std::vector<Offset> Vertices(const Ranges& r,bool with_surface){
    constexpr double kOuter=1.0823922002923940;   // 1 / cos(pi / 8)
    std::vector<Offset> out;
    out.reserve(with_surface?4096:256);
    const bool surface=with_surface && r.surface;
    for(int a=0;a<8;++a)for(int b=0;b<8;++b)for(const int s:{-1,1})for(const int t:{-1,1})
        for(int c=0;c<(surface?8:1);++c)for(const int u:(surface?std::vector<int>{-1,1}:std::vector<int>{0})){
            const double pa=a*std::numbers::pi/4,pb=b*std::numbers::pi/4,pc=c*std::numbers::pi/4;
            Offset o{r.position_mm*kOuter*std::cos(pa),r.position_mm*kOuter*std::sin(pa),r.tilt_deg*kOuter*std::cos(pb),r.tilt_deg*kOuter*std::sin(pb),
                s*r.rotation_deg,t*r.height_percent};
            if(surface){o.h0=u*r.surface_height_mm;o.gx=r.surface_slope*kOuter*std::cos(pc);o.gy=r.surface_slope*kOuter*std::sin(pc);}
            out.push_back(o);
        }
    return out;
}

// ---- Baseline geometry ----
struct Centre {double x{},y{},z{};};
Centre CentreOf(const Matrix3& h,const render::CameraIntrinsics& k){
    // M = K^-1 H, scaled so that the first two columns have unit length on average (the plane's metric), the plane in front of the camera.
    std::array<std::array<double,3>,3> m{};
    for(int j=0;j<3;++j){
        m[0][j]=(h[j]-k.cx_pixels*h[6+j])/k.fx_pixels;
        m[1][j]=(h[3+j]-k.cy_pixels*h[6+j])/k.fy_pixels;
        m[2][j]=h[6+j];
    }
    const auto norm=[&](int j){return std::sqrt(m[0][j]*m[0][j]+m[1][j]*m[1][j]+m[2][j]*m[2][j]);};
    const double n0=norm(0),n1=norm(1);
    if(!(std::isfinite(n0) && std::isfinite(n1) && n0>0 && n1>0))Reject("a camera's document matrix is degenerate");
    double lambda=2.0/(n0+n1);
    if(m[2][2]<0)lambda=-lambda;
    for(auto& row:m)for(auto& v:row)v*=lambda;
    const std::array<double,3> r1{m[0][0],m[1][0],m[2][0]},r2{m[0][1],m[1][1],m[2][1]},t{m[0][2],m[1][2],m[2][2]};
    const std::array<double,3> r3{r1[1]*r2[2]-r1[2]*r2[1],r1[2]*r2[0]-r1[0]*r2[2],r1[0]*r2[1]-r1[1]*r2[0]};
    const auto dot=[](const std::array<double,3>& a,const std::array<double,3>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    if(!(t[2]>0))Reject("the document plane is not in front of a camera");
    return {-dot(r1,t),-dot(r2,t),-dot(r3,t)};
}
}

BaselineGeometryV3 BaselineGeometryOfCondition(const FixedCompositeConditionV3& c,const FixedCompositeProfileBinding& binding){
    const auto& p=binding.parameters;
    const PoseModel model_b(p.camera_b);
    const auto centre_a=CentreOf(p.camera_a.document_to_image,p.camera_a.intrinsics);
    const auto centre_b=CentreOf(model_b.CameraHomography(c.base_pose),p.camera_b.intrinsics);
    const double bx=centre_b.x-centre_a.x,by=centre_b.y-centre_a.y;
    const double baseline=std::hypot(bx,by);
    if(!(baseline>0))Reject("the two cameras are at the same place over the table, so the baseline has no direction");
    const double mean_height=(std::abs(centre_a.z)+std::abs(centre_b.z))/2;
    const double paper=c.registration.surface_envelope?c.registration.surface_envelope->height_mm:0.0;
    if(!(mean_height-paper>0))Reject("the paper range reaches the cameras");
    const double rho=p.raster.dpi/25.4;
    BaselineGeometryV3 g;
    g.e_b={bx/baseline,by/baseline};g.baseline_mm=baseline;g.mean_camera_height_mm=mean_height;
    g.k_px_per_mm=rho*baseline/(mean_height-paper);
    return g;
}

namespace {
// The outer edge of the output: the four sides, each sample covered by A or by B. The number of samples covered by neither.
std::uint32_t CountUncoveredEdge(const MappedFixedSeamInput& input){
    std::uint32_t count=0;
    const auto uncovered=[&](std::uint32_t x,std::uint32_t y){
        return !Covered(input.camera_a,static_cast<double>(x),static_cast<double>(y)) && !Covered(input.camera_b,static_cast<double>(x),static_cast<double>(y));
    };
    const std::uint32_t w=input.width,h=input.height;
    for(std::uint32_t x=0;x<w;++x){
        if(uncovered(x,0))++count;
        if(h>1 && uncovered(x,h-1))++count;
    }
    for(std::uint32_t y=1;y+1<h;++y){
        if(uncovered(0,y))++count;
        if(w>1 && uncovered(w-1,y))++count;
    }
    return count;
}

struct Counters {std::uint32_t blocks=0,enumerated_blocks=0;std::uint32_t rows_verified=0;};

void CheckPoseRange(const Context& ctx,const FixedCompositeConditionV3& c,double lift_cap_px,CreationCheckDetailsV3* details){
    Ranges ranges{c.registration.envelope.position_mm,c.registration.envelope.tilt_deg,c.registration.envelope.rotation_deg,c.registration.envelope.height_percent};
    if(c.registration.surface_envelope){
        ranges.surface=true;ranges.surface_height_mm=c.registration.surface_envelope->height_mm;
        ranges.surface_slope=std::tan(c.registration.surface_envelope->tilt_deg*std::numbers::pi/180);
    }
    const PoseParameters base=c.base_pose;
    const std::uint32_t h=ctx.height;
    Lines lines;
    lines.centre=MarginsOfRows(ctx,base);
    for(std::uint32_t y=0;y<h;++y)
        if(!lines.centre[y].ok)Reject("row "+std::to_string(y)+": the seam column is not covered by both cameras at the base pose");
    lines.half_difference.assign(h,{});
    const auto axis_offset=[&](int axis,double sign){
        Offset o;
        switch(axis){
        case 0:o.tx=sign*ranges.position_mm;break;
        case 1:o.ty=sign*ranges.position_mm;break;
        case 2:o.rx=sign*ranges.tilt_deg;break;
        case 3:o.ry=sign*ranges.tilt_deg;break;
        case 4:o.rz=sign*ranges.rotation_deg;break;
        case 5:o.tz=sign*ranges.height_percent;break;
        case 6:o.h0=sign*ranges.surface_height_mm;break;
        case 7:o.gx=sign*ranges.surface_slope;break;
        default:o.gy=sign*ranges.surface_slope;break;
        }
        return o;
    };
    constexpr const char* kAxisName[kAxes]{"the x position","the y position","the x tilt","the y tilt","the rotation","the height","the paper height","the paper slope in x","the paper slope in y"};
    constexpr const char* kAxisUnit[kAxes]{" mm"," mm"," deg"," deg"," deg"," %"," mm",""," (slope = tan of the tilt)"};
    const std::array<double,kAxes> axis_end{ranges.position_mm,ranges.position_mm,ranges.tilt_deg,ranges.tilt_deg,ranges.rotation_deg,ranges.height_percent,
        ranges.surface_height_mm,ranges.surface_slope,ranges.surface_slope};
    for(int axis=0;axis<(ranges.surface?kAxes:6);++axis){
        const auto up=axis_offset(axis,+1),down=axis_offset(axis,-1);
        const auto plus=MarginsOfRows(ctx,Moved(base,up),SurfaceOf(up)),minus=MarginsOfRows(ctx,Moved(base,down),SurfaceOf(down));
        for(std::uint32_t y=0;y<h;++y){
            // A row that has no cover at the seam at the end of one axis is a failure of the range itself: that point is inside it.
            if(!plus[y].ok || !minus[y].ok){
                const char* sign=!plus[y].ok?"+":"-";
                Reject("row "+std::to_string(y)+": the seam column is not covered by both cameras when "+kAxisName[axis]+" is at the "+sign+" end of its range ("+sign+
                    Text(axis_end[axis])+kAxisUnit[axis]+" from the base pose)");
            }
            for(int k=0;k<3;++k)lines.half_difference[y][k][axis]=(Value(plus[y],kKinds[k])-Value(minus[y],kKinds[k]))/2;
        }
    }
    // The paper-lift cap lowers the seam margins at most by cap (|q w| <= cap anywhere); it does not enter the output-end margin (b),
    // which is made at pi = 0, q = 0, and neither does the paper surface.
    Counters counters;
    const auto vertices_with_surface=Vertices(ranges,true),vertices_pose_only=Vertices(ranges,false);
    for(const Kind kind:kKinds){
        const int k=static_cast<int>(kind);
        const double bound=kind==Kind::output_end?0.0:lift_cap_px;
        const bool with_surface=kind!=Kind::output_end && ranges.surface;
        const auto& vertices=with_surface?vertices_with_surface:vertices_pose_only;
        for(std::uint32_t first=0;first<h;first+=kBlockRows){
            const std::uint32_t last=std::min<std::uint32_t>(h,first+kBlockRows);
            // The block's row with the lowest predicted worst margin, and the edge rows of the output.
            std::uint32_t worst_row=first;double worst=std::numeric_limits<double>::infinity();
            for(std::uint32_t y=first;y<last;++y){
                const double w=PredictedWorst(Value(lines.centre[y],kind),lines.half_difference[y][k],bound,with_surface);
                if(w<worst){worst=w;worst_row=y;}
            }
            std::vector<std::uint32_t> rows{worst_row};
            if(first==0 && worst_row!=0)rows.push_back(0);
            if(last==h && worst_row!=h-1)rows.push_back(h-1);
            for(const auto y:rows){
                ++counters.rows_verified;
                const double predicted=PredictedWorst(Value(lines.centre[y],kind),lines.half_difference[y][k],bound,with_surface);
                const auto offset=WorstOffset(lines.half_difference[y][k],ranges,with_surface);
                const auto truth=MarginsOfRow(ctx,MapB(ctx,Moved(base,offset),SurfaceOf(offset)),y);
                const double value=truth.ok?Value(truth,kind)-bound:-std::numeric_limits<double>::infinity();
                if(value>=0 && std::abs(value-predicted)<=kLinearizationTolerancePx)continue;
                ++counters.enumerated_blocks;
                // The linear prediction does not hold here (or the worst vertex fails): decide by the vertices of the range on this row.
                for(const auto& vertex:vertices){
                    const auto at=MarginsOfRow(ctx,MapB(ctx,Moved(base,vertex),SurfaceOf(vertex)),y);
                    const double margin=at.ok?Value(at,kind)-bound:-std::numeric_limits<double>::infinity();
                    if(!(margin>=0))
                        Reject("row "+std::to_string(y)+": "+KindName(kind)+" does not hold over the range (margin "+(at.ok?Text(margin):std::string("no cover"))+" px) at the vertex "+
                            "x position "+Text(vertex.tx)+" mm, y position "+Text(vertex.ty)+" mm, x tilt "+Text(vertex.rx)+" deg, y tilt "+Text(vertex.ry)+" deg, rotation "+Text(vertex.rz)+
                            " deg, height "+Text(vertex.tz)+" %"+(with_surface?", paper height "+Text(vertex.h0)+" mm, paper slope ("+Text(vertex.gx)+", "+Text(vertex.gy)+")":std::string())+
                            " (offsets from the base pose)");
                }
            }
            ++counters.blocks;
        }
    }
    if(details){
        details->details.push_back(std::string(ranges.surface?"pose_and_surface_range":"pose_range")+"=checked blocks="+std::to_string(counters.blocks)+" enumerated_blocks="+std::to_string(counters.enumerated_blocks)+
            " rows_verified="+std::to_string(counters.rows_verified));
    }
}
}

FixedCompositeCreationCheckV3 MakeFixedCompositeCreationCheckV3(CreationCheckDetailsV3* details){
    return MakeFixedCompositeCreationCheckV3(details,CreationCheckOptionsV3{});
}

FixedCompositeCreationCheckV3 MakeFixedCompositeCreationCheckV3(CreationCheckDetailsV3* details,const CreationCheckOptionsV3& options){
    return [details,options](const FixedCompositeConditionV3& c,const FixedCompositeProfileBinding& binding){
        if(details)*details=CreationCheckDetailsV3{};
        const auto& parameters=binding.parameters;
        if(parameters.layout!=StitchLayout::camera_a_left_camera_b_right)Reject("only the A-left, B-right layout is supported");
        const std::uint32_t width=parameters.raster.width_pixels,height=parameters.raster.height_pixels;
        if(c.seam.column>=width)Reject("the seam column is outside the raster");

        // (d) the base pose lies inside B's range measured from the calibration. ValidateFixedCompositeConditionV3 refuses a record
        // that breaks it; it is repeated here so that the check does not depend on who validated.
        {
            const auto& p=c.base_pose;const auto& e=c.registration.envelope;
            if(std::hypot(p.tx_mm,p.ty_mm)>e.position_mm || std::abs(p.rz_deg)>e.rotation_deg || std::hypot(p.rx_deg,p.ry_deg)>e.tilt_deg || std::abs(p.tz_percent)>e.height_percent)
                Reject("the base pose is outside B's range measured from the calibration; start again from the calibration");
        }

        // (c) the strain bound of layer 2 and the cap on the lift it can express.
        double cap=0;
        if(c.registration.parallax){
            if(!options.layer_two || !std::isfinite(options.layer_two->falloff_px) || !(options.layer_two->falloff_px>0) ||
                !std::isfinite(options.layer_two->maximum_strain) || !(options.layer_two->maximum_strain>0))
                Reject("layer 2 is enabled but the design constants of its strain bound (L and s_max) were not given, so the bound cannot be checked");
            const auto geometry=BaselineGeometryOfCondition(c,binding);
            cap=c.registration.parallax->max_lift_mm*geometry.k_px_per_mm;
            const double strain=std::numbers::pi/2*cap/options.layer_two->falloff_px;
            if(!std::isfinite(cap) || !(strain<=options.layer_two->maximum_strain))
                Reject("the strain of layer 2 at the lift line (pi/2 * "+Text(cap)+" px / L = "+Text(strain)+") is above the bound "+Text(options.layer_two->maximum_strain));
            if(details){
                details->lift_cap_px=cap;details->maximum_strain=strain;
                details->details.push_back("k_px_per_mm="+Text(geometry.k_px_per_mm)+" lift_cap_px="+Text(cap)+" strain="+Text(strain));
            }
        }

        // A provisional seam column is the middle of the band at the calibration (v2's rule, item 5).
        if(c.seam.origin==FixedSeamOrigin::band_center_provisional){
            const auto expected=FixedCompositeProvisionalSeamColumn(parameters);
            if(c.seam.column!=expected)Reject("a provisional seam column is the middle of the band ("+std::to_string(expected)+"), not "+std::to_string(c.seam.column));
        }

        // The whole output at the base pose, by the same scan as v2 makes: A is the left camera, A or B covers every sample, the seam and
        // half the blend width are inside the overlap on every row.
        const auto input=PoseCorrectedDocumentMappedFixedSeamInput(parameters,c.base_pose);
        try{
            RequireFixedCompositeCoverage(input);
            RequireFixedCompositeSeam(input,c.seam.column);
        }catch(const std::invalid_argument& e){
            std::string text=e.what();
            const std::string v2="fixed composite creation: ";
            if(text.starts_with(v2))text.erase(0,v2.size());
            Reject("at the base pose: "+text);
        }

        // (a) and (b) over B's range, by the linearised margins and the worst vertex of each row block (design 11.23.4).
        Context ctx{parameters,PoseModel(parameters.camera_b),content_registration::FrameOfRaster(parameters.raster),input.camera_a,c.seam.column,width,height,
            static_cast<double>((kSeamBlendDesign.width_pixels+1)/2-1),{},{}};
        ctx.a_right.assign(height,0.0);ctx.a_ok.assign(height,false);
        for(std::uint32_t y=0;y<height;++y){
            const auto run=RunAround(ctx.map_a,static_cast<double>(y),static_cast<double>(c.seam.column),width);
            ctx.a_ok[y]=run.ok && run.right>=c.seam.column+ctx.half;   // A covers the seam and half the blend width on the right of it
            ctx.a_right[y]=run.right;
            if(!ctx.a_ok[y])Reject("row "+std::to_string(y)+": camera A does not cover the seam column and half the blend width");
        }
        CheckPoseRange(ctx,c,cap,details);

        // The outer edge of the output under the paper surface's range does not refuse (11.23.3); the largest count of its uncovered samples,
        // over the ends of the surface range at the base pose, is reported.
        if(details){
            std::uint32_t uncovered=0;
            if(c.registration.surface_envelope){
                const double slope=std::tan(c.registration.surface_envelope->tilt_deg*std::numbers::pi/180);
                constexpr double kOuter=1.0823922002923940;
                for(int a=0;a<8;++a)for(const int u:{-1,1}){
                    RegisteredMapping mapping;mapping.pose=c.base_pose;
                    mapping.surface.h0_mm=u*c.registration.surface_envelope->height_mm;
                    mapping.surface.slope_x=slope*kOuter*std::cos(a*std::numbers::pi/4);mapping.surface.slope_y=slope*kOuter*std::sin(a*std::numbers::pi/4);
                    uncovered=std::max(uncovered,CountUncoveredEdge(RegisteredDocumentMappedFixedSeamInput(parameters,mapping,c.seam.column)));
                }
            }
            details->uncovered_edge_pixels=uncovered;
            details->details.push_back(std::string("surface_range_outer_edge_uncovered=")+(c.registration.surface_envelope?std::to_string(uncovered):std::string("disabled")));
        }
    };
}

RegisteredMappingCover CheckRegisteredMappingCover(const render::DocumentRenderParameters& parameters,const RegisteredMapping& mapping,
    std::uint32_t seam_column,const SeamBlendSettings& blend){
    RegisteredMappingCover cover;
    const auto input=RegisteredDocumentMappedFixedSeamInput(parameters,mapping,seam_column);
    render::BgrImage frame;
    frame.width=static_cast<std::uint32_t>(kD810SensorWidthPixels);frame.height=static_cast<std::uint32_t>(kD810SensorHeightPixels);
    frame.bgr.assign(render::PixelCount(frame.width,frame.height)*3,0);
    try{cover.seam_covered=CanRenderMappedFixedSeam(frame,frame,input,seam_column,blend);}
    catch(const std::invalid_argument&){cover.seam_covered=false;}   // a row without an overlap, or an overlap that is not one run
    cover.uncovered_edge_pixels=CountUncoveredEdge(input);
    cover.output_covered=cover.uncovered_edge_pixels==0;
    return cover;
}
}
