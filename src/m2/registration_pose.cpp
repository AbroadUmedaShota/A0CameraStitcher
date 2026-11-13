#include "a0/m2/experimental/registration_pose.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace a0::m2::experimental::content_registration {
namespace {
using render::Matrix3;
using render::Point;
using Vec3 = std::array<double,3>;
constexpr double kDegrees = std::numbers::pi/180.0;

void Require(bool condition,const char* message){
    if(!condition)throw std::invalid_argument(message);
}

Matrix3 Multiply(const Matrix3& a,const Matrix3& b){
    Matrix3 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){
        double s=0;
        for(int k=0;k<3;++k)s+=a[3*i+k]*b[3*k+j];
        r[3*i+j]=s;
    }
    return r;
}
Vec3 Apply(const Matrix3& m,const Vec3& v){
    return {m[0]*v[0]+m[1]*v[1]+m[2]*v[2],m[3]*v[0]+m[4]*v[1]+m[5]*v[2],m[6]*v[0]+m[7]*v[1]+m[8]*v[2]};
}
Matrix3 Inverse(const Matrix3& m){
    const double c00=m[4]*m[8]-m[5]*m[7],c01=m[5]*m[6]-m[3]*m[8],c02=m[3]*m[7]-m[4]*m[6];
    const double det=m[0]*c00+m[1]*c01+m[2]*c02;
    Require(std::isfinite(det) && std::abs(det)>1e-300,"registration pose: singular matrix");
    const double s=1.0/det;
    return {c00*s,(m[2]*m[7]-m[1]*m[8])*s,(m[1]*m[5]-m[2]*m[4])*s,
            c01*s,(m[0]*m[8]-m[2]*m[6])*s,(m[2]*m[3]-m[0]*m[5])*s,
            c02*s,(m[1]*m[6]-m[0]*m[7])*s,(m[0]*m[4]-m[1]*m[3])*s};
}
double Norm(const Vec3& v){return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}

// Rotation by the vector w (radians), Rodrigues.
Matrix3 Rotation(double wx,double wy,double wz){
    const double angle=std::sqrt(wx*wx+wy*wy+wz*wz);
    // sin(a)/a and (1-cos(a))/a^2, stable for a small angle.
    const double s=angle<1e-8?1.0-angle*angle/6.0:std::sin(angle)/angle;
    const double c=angle<1e-4?0.5-angle*angle/24.0:(1.0-std::cos(angle))/(angle*angle);
    return {1+c*(-wz*wz-wy*wy),-s*wz+c*wx*wy,s*wy+c*wx*wz,
            s*wz+c*wx*wy,1+c*(-wz*wz-wx*wx),-s*wx+c*wy*wz,
            -s*wy+c*wx*wz,s*wx+c*wy*wz,1+c*(-wy*wy-wx*wx)};
}
Vec3 Cross(const Vec3& a,const Vec3& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Matrix3 Normalized(Matrix3 m,const char* message){
    const double scale=m[8];
    Require(std::isfinite(scale) && std::abs(scale)>0,message);
    for(auto& v:m)v/=scale;
    return m;
}
void RequirePose(const PoseParameters& p){
    for(const double v:{p.rx_deg,p.ry_deg,p.rz_deg,p.tx_mm,p.ty_mm,p.tz_percent})Require(std::isfinite(v),"registration pose: a pose must be finite");
}

// The terms of Pi(pi) (design 11.3). The surface is z = c + gx x + gy y with c = h0 - gx x0 - gy y0. Camera a's ray through
// the table point X = (x, y, 0) is C_a + s (X - C_a) with s = n_a / D(X), where
//   D(X) = H_a + gx (x - x_a) + gy (y - y_a)    (affine in X; the row `d`)
//   n_a  = H_a - (c + gx x_a + gy y_a)           (the height of a over the surface at a's foot point)
// so the paper point is P = C_a + s (X - C_a) at the height H_a (1 - s). Camera b's ray through P meets the table at
//   Y = C_b + H_b ((C_a - C_b) D(X) + n_a (X - C_a)) / Den(X),   Den(X) = (H_b - H_a) D(X) + H_a n_a = D(X) (H_b - P_z).
struct SurfaceTerms {
    Vec3 d{};
    double n_a{};
};
void RequireCentre(const CameraCentre& c){
    Require(std::isfinite(c.x_mm) && std::isfinite(c.y_mm) && std::isfinite(c.height_mm),"registration surface: a camera centre must be finite");
    Require(c.height_mm>0,"registration surface: a camera centre must be above the table");
}
SurfaceTerms TermsOf(const CameraCentre& a,Point origin_mm,const SurfacePlane& s){
    const double c=s.h0_mm-s.slope_x*origin_mm.x-s.slope_y*origin_mm.y;
    return {{s.slope_x,s.slope_y,a.height_mm-s.slope_x*a.x_mm-s.slope_y*a.y_mm},a.height_mm-(c+s.slope_x*a.x_mm+s.slope_y*a.y_mm)};
}
}

render::Point SurfaceOriginMm(const render::OutputRaster& raster,std::uint32_t seam_column){
    render::ValidateOutputRaster(raster);
    Require(seam_column<raster.width_pixels,"registration surface: the seam column is outside the raster");
    const double pitch=25.4/raster.dpi;
    const double left=static_cast<double>(raster.region_um.left)/1000.0,top=static_cast<double>(raster.region_um.top)/1000.0;
    const double row=(static_cast<double>(raster.height_pixels)-1.0)/2.0;
    return {left+(static_cast<double>(seam_column)+.5)*pitch,top+(row+.5)*pitch};
}

Matrix3 SurfaceHomography(const CameraCentre& a,const CameraCentre& b,Point origin_mm,const SurfacePlane& s){
    RequireCentre(a);RequireCentre(b);
    Require(std::isfinite(origin_mm.x) && std::isfinite(origin_mm.y),"registration surface: the plane origin must be finite");
    Require(std::isfinite(s.h0_mm) && std::isfinite(s.slope_x) && std::isfinite(s.slope_y),"registration surface: the surface plane must be finite");
    const auto t=TermsOf(a,origin_mm,s);
    Require(std::isfinite(t.n_a) && t.n_a>0,"registration surface: the surface is not below camera A");
    const Vec3 den{(b.height_mm-a.height_mm)*t.d[0],(b.height_mm-a.height_mm)*t.d[1],(b.height_mm-a.height_mm)*t.d[2]+a.height_mm*t.n_a};
    const double dx=a.x_mm-b.x_mm,dy=a.y_mm-b.y_mm,hb=b.height_mm;
    const Matrix3 pi{
        b.x_mm*den[0]+hb*(dx*t.d[0]+t.n_a),b.x_mm*den[1]+hb*(dx*t.d[1]),b.x_mm*den[2]+hb*(dx*t.d[2]-t.n_a*a.x_mm),
        b.y_mm*den[0]+hb*(dy*t.d[0]),b.y_mm*den[1]+hb*(dy*t.d[1]+t.n_a),b.y_mm*den[2]+hb*(dy*t.d[2]-t.n_a*a.y_mm),
        den[0],den[1],den[2]};
    for(const double v:pi)Require(std::isfinite(v),"registration surface: Pi is not finite");
    return Normalized(pi,"registration surface: degenerate Pi");
}

bool SurfaceHomographyHoldsAt(const CameraCentre& a,const CameraCentre& b,Point origin_mm,const SurfacePlane& s,Point x){
    for(const double v:{a.x_mm,a.y_mm,a.height_mm,b.x_mm,b.y_mm,b.height_mm,origin_mm.x,origin_mm.y,s.h0_mm,s.slope_x,s.slope_y,x.x,x.y})
        if(!std::isfinite(v))return false;
    if(!(a.height_mm>0) || !(b.height_mm>0))return false;
    const auto t=TermsOf(a,origin_mm,s);
    const double d=t.d[0]*x.x+t.d[1]*x.y+t.d[2];
    const double den=(b.height_mm-a.height_mm)*d+a.height_mm*t.n_a;
    return t.n_a>0 && d>0 && den>0;
}

PlaneFrame FrameOfRaster(const render::OutputRaster& raster){
    render::ValidateOutputRaster(raster);
    return {static_cast<double>(raster.region_um.left)/1000.0,static_cast<double>(raster.region_um.top)/1000.0,25.4/raster.dpi};
}

PoseModel::PoseModel(const render::CameraProjection& camera){
    const auto& i=camera.intrinsics;
    Require(std::isfinite(i.fx_pixels) && std::isfinite(i.fy_pixels) && i.fx_pixels>0 && i.fy_pixels>0,"registration pose: invalid intrinsics");
    k_={i.fx_pixels,0,i.cx_pixels,0,i.fy_pixels,i.cy_pixels,0,0,1};
    const Matrix3 k_inverse={1/i.fx_pixels,0,-i.cx_pixels/i.fx_pixels,0,1/i.fy_pixels,-i.cy_pixels/i.fy_pixels,0,0,1};
    const auto& h=camera.document_to_image;
    const auto raw=Multiply(k_inverse,h);
    const Vec3 c0{raw[0],raw[3],raw[6]},c1{raw[1],raw[4],raw[7]};
    const double n0=Norm(c0),n1=Norm(c1);
    Require(std::isfinite(n0) && std::isfinite(n1) && n0>0 && n1>0,"registration pose: degenerate document matrix");
    // The plane's first two columns have unit length in mm per document mm; the matrix is then metric.
    lambda_=2.0/(n0+n1);
    for(int j=0;j<9;++j)m_[j]=lambda_*raw[j];
    m_inverse_=Inverse(m_);
    // The pivot: the document point seen at the principal point.
    const auto h_inverse=Inverse(h);
    const Vec3 principal=Apply(h_inverse,{i.cx_pixels,i.cy_pixels,1});
    Require(std::isfinite(principal[2]) && std::abs(principal[2])>0,"registration pose: no pivot");
    pivot_document_={principal[0]/principal[2],principal[1]/principal[2]};
    pivot_camera_=Apply(m_,{pivot_document_.x,pivot_document_.y,1});
    Require(pivot_camera_[2]>0,"registration pose: the document plane is not in front of the camera");
}

Matrix3 PoseModel::MovedPlane(const PoseParameters& p) const{
    const auto rotation=Rotation(p.rx_deg*kDegrees,p.ry_deg*kDegrees,p.rz_deg*kDegrees);
    const Vec3 translation{p.tx_mm,p.ty_mm,p.tz_percent/100.0*pivot_camera_[2]};
    Matrix3 moved{};
    for(int j=0;j<2;++j){
        const auto column=Apply(rotation,{m_[j],m_[3+j],m_[6+j]});
        for(int r=0;r<3;++r)moved[3*r+j]=column[r];
    }
    // The translation column turns about the pivot and then moves.
    const Vec3 offset{m_[2]-pivot_camera_[0],m_[5]-pivot_camera_[1],m_[8]-pivot_camera_[2]};
    const auto turned=Apply(rotation,offset);
    for(int r=0;r<3;++r)moved[3*r+2]=turned[r]+pivot_camera_[r]+translation[r];
    return moved;
}
Matrix3 PoseModel::Homography(const PoseParameters& p) const{
    auto g=Multiply(m_inverse_,MovedPlane(p));
    const double scale=g[8];
    Require(std::isfinite(scale) && std::abs(scale)>0,"registration pose: degenerate homography");
    for(auto& v:g)v/=scale;
    return g;
}
CameraCentre PoseModel::Centre(const PoseParameters& p) const{
    RequirePose(p);
    const Matrix3 plane=p.IsZero()?m_:MovedPlane(p);
    const Vec3 c0{plane[0],plane[3],plane[6]},c1{plane[1],plane[4],plane[7]},t{plane[2],plane[5],plane[8]};
    // n: the unit normal of the plane, c0 x c1, which points away from a camera that sees the document the right way round.
    Vec3 n=Cross(c0,c1);
    const double length=Norm(n);
    Require(std::isfinite(length) && length>0,"registration pose: degenerate plane");
    for(auto& v:n)v/=length;
    // The point at the height h over (x, y) is c0 x + c1 y + t - h n; the centre is the one at the camera origin.
    const auto v=Apply(Inverse({c0[0],c1[0],n[0],c0[1],c1[1],n[1],c0[2],c1[2],n[2]}),{-t[0],-t[1],-t[2]});
    const double height=-v[2];
    Require(std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(height) && height>0,"registration pose: the camera is not above the table");
    return {v[0],v[1],height};
}
Matrix3 PoseModel::CameraHomography(const PoseParameters& p) const{
    auto h=Multiply(k_,MovedPlane(p));
    const double scale=h[8];
    Require(std::isfinite(scale) && std::abs(scale)>0,"registration pose: degenerate homography");
    for(auto& v:h)v/=scale;
    return h;
}

Point PoseDisplacement(const PoseModel& model,const PlaneFrame& frame,const PoseParameters& p,double x,double y){
    const auto g=model.Homography(p);
    const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
    const Vec3 moved=Apply(g,{px,py,1});
    return {(moved[0]/moved[2]-px)/frame.pitch_mm,(moved[1]/moved[2]-py)/frame.pitch_mm};
}

Matrix3 RegisteredHomography(const PoseModel& model_b,const CameraCentre& a,Point origin_mm,const PoseParameters& p,const SurfacePlane& surface){
    RequirePose(p);
    if(surface.IsZero())return model_b.Homography(p);
    const auto pi=SurfaceHomography(a,model_b.Centre(p),origin_mm,surface);
    if(p.IsZero())return pi;
    return Normalized(Multiply(model_b.Homography(p),pi),"registration surface: degenerate G Pi");
}

Point RegisteredDisplacement(const PoseModel& model_b,const PlaneFrame& frame,const CameraCentre& a,Point origin_mm,const PoseParameters& p,
    const SurfacePlane& surface,double x,double y){
    if(surface.IsZero())return PoseDisplacement(model_b,frame,p,x,y);
    const auto h=RegisteredHomography(model_b,a,origin_mm,p,surface);
    const double px=frame.left_mm+(x+0.5)*frame.pitch_mm,py=frame.top_mm+(y+0.5)*frame.pitch_mm;
    const Vec3 moved=Apply(h,{px,py,1});
    return {(moved[0]/moved[2]-px)/frame.pitch_mm,(moved[1]/moved[2]-py)/frame.pitch_mm};
}
}
