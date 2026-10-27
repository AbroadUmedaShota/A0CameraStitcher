#include "a0/m2/experimental/seam_blend.hpp"
#include "zero_mean_correlation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace a0::m2::experimental {
namespace {
using render::BgrImage;
double Luminance(const std::array<double,3>& p){return p[0]*0.114+p[1]*0.587+p[2]*0.299;}
// Output pixel (x,y) is global (ox+x,oy+y), exactly as render::RenderPair.
struct Frame {
    render::PairCanvasPlan plan;
    render::Matrix3 inverse{};
    double ox{},oy{};
    bool A(const BgrImage& a,double x,double y,std::array<double,3>& p) const {return render::SampleBilinear(a,ox+x,oy+y,p);}
    bool B(const BgrImage& b,double x,double y,std::array<double,3>& p) const {
        render::Point s{};return render::TryTransform(inverse,{ox+x,oy+y},s) && render::SampleBilinear(b,s.x,s.y,p);
    }
};
Frame Prepare(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& p){
    if(p.layout!=StitchLayout::camera_a_left_camera_b_right)throw std::invalid_argument("seam blend needs A left and B right");
    Frame f;f.inverse=render::Invert(p.camera_b_to_camera_a);
    render::ValidateProjectiveDomain(p.camera_b_to_camera_a,b.width,b.height);
    if(a.bgr.size()!=render::PixelCount(a.width,a.height)*3 || b.bgr.size()!=render::PixelCount(b.width,b.height)*3)
        throw std::invalid_argument("seam blend buffer size does not match its dimensions");
    f.plan=render::PlanPairCanvas(a.width,a.height,b.width,b.height,p);
    f.ox=f.plan.origin_x+p.crop.left;f.oy=f.plan.origin_y+p.crop.top;return f;
}
constexpr float kAbsent=std::numeric_limits<float>::quiet_NaN();
// Log luminance of A and B on the band columns [x0,x0+width) of every output
// row, NaN where either is missing; per row the covered span [c0,c1).
struct Band {
    unsigned x0{},width{},height{};
    std::vector<float> a,b;
    std::vector<unsigned> c0,c1;
    bool Valid(unsigned i,unsigned y) const {return !std::isnan(a[static_cast<size_t>(y)*width+i]);}
    float At(const std::vector<float>& v,unsigned i,unsigned y) const {return v[static_cast<size_t>(y)*width+i];}
    // Forward difference where both neighbours are present, else backward, else 0.
    double Gradient(const std::vector<float>& v,unsigned i,unsigned y) const {
        const double c=At(v,i,y);double gx=0,gy=0;
        if(i+1<width && Valid(i+1,y))gx=At(v,i+1,y)-c;else if(i && Valid(i-1,y))gx=c-At(v,i-1,y);
        if(y+1<height && Valid(i,y+1))gy=At(v,i,y+1)-c;else if(y && Valid(i,y-1))gy=c-At(v,i,y-1);
        return std::sqrt(gx*gx+gy*gy);
    }
};
Band SampleBand(const BgrImage& a,const BgrImage& b,const Frame& f){
    const auto& plan=f.plan;
    const double start=std::max(plan.a_bounds.minimum_x,plan.b_bounds.minimum_x),end=std::min(plan.a_bounds.maximum_x,plan.b_bounds.maximum_x);
    const double first=std::max(0.0,std::floor(start-f.ox)-1),last=std::min(double(plan.output_width),std::ceil(end-f.ox)+1);
    if(!(last>first))throw std::invalid_argument("seam blend has no overlap band");
    Band band{static_cast<unsigned>(first),static_cast<unsigned>(last-first),plan.output_height,{},{},{},{}};
    const auto cells=static_cast<size_t>(band.width)*band.height;
    band.a.assign(cells,kAbsent);band.b.assign(cells,kAbsent);band.c0.assign(band.height,0);band.c1.assign(band.height,0);
    for(unsigned y=0;y<band.height;++y){
        bool any=false;float* const ra=band.a.data()+static_cast<size_t>(y)*band.width;float* const rb=band.b.data()+static_cast<size_t>(y)*band.width;
        for(unsigned i=0;i<band.width;++i){
            std::array<double,3> pa{},pb{};
            if(!f.A(a,band.x0+i,y,pa) || !f.B(b,band.x0+i,y,pb))continue;
            ra[i]=static_cast<float>(std::log1p(Luminance(pa)));rb[i]=static_cast<float>(std::log1p(Luminance(pb)));
            if(!any)band.c0[y]=i;any=true;band.c1[y]=i+1;
        }
        if(!any)throw std::invalid_argument("seam blend needs A and B on every output row");
    }
    return band;
}
// Local mean of log(A)-log(B) over blocks of side `block`, bilinear between
// block centres. Blocks without a covered cell take the mean of all cells.
struct LocalRatio {
    unsigned block{},nx{},ny{};std::vector<double> v;
    double At(unsigned i,unsigned y) const {
        const auto axis=[&](unsigned p,unsigned n,unsigned& k0,unsigned& k1,double& t){
            const double c=std::clamp((p+0.5)/block-0.5,0.0,double(n-1));k0=static_cast<unsigned>(c);k1=std::min(k0+1,n-1);t=c-k0;
        };
        unsigned x0{},x1{},y0{},y1{};double tx{},ty{};axis(i,nx,x0,x1,tx);axis(y,ny,y0,y1,ty);
        const auto g=[&](unsigned x,unsigned yy){return v[static_cast<size_t>(yy)*nx+x];};
        return (g(x0,y0)*(1-tx)+g(x1,y0)*tx)*(1-ty)+(g(x0,y1)*(1-tx)+g(x1,y1)*tx)*ty;
    }
};
LocalRatio MeasureLocalRatio(const Band& band,unsigned block){
    LocalRatio r{block,(band.width+block-1)/block,(band.height+block-1)/block,{}};
    std::vector<double> sum(static_cast<size_t>(r.nx)*r.ny,0);std::vector<std::uint64_t> count(sum.size(),0);double total=0;std::uint64_t n=0;
    for(unsigned y=0;y<band.height;++y)for(unsigned i=band.c0[y];i<band.c1[y];++i){
        if(!band.Valid(i,y))continue;const double d=double(band.At(band.a,i,y))-band.At(band.b,i,y);
        const auto k=static_cast<size_t>(y/block)*r.nx+i/block;sum[k]+=d;++count[k];total+=d;++n;
    }
    r.v.resize(sum.size());for(size_t k=0;k<sum.size();++k)r.v[k]=count[k]?sum[k]/double(count[k]):total/double(n);
    return r;
}
std::vector<std::uint32_t> FindSeam(const Band& band,const LocalRatio& ratio,unsigned margin){
    const unsigned w=band.width,h=band.height;constexpr double inf=std::numeric_limits<double>::infinity();
    std::vector<double> previous(w,inf),current(w,inf);std::vector<std::int8_t> back(static_cast<size_t>(w)*h,0);
    unsigned lo{},hi{};
    for(unsigned y=0;y<h;++y){
        const unsigned span=band.c1[y]-band.c0[y],m=std::min(margin,(span-1)/2);lo=band.c0[y]+m;hi=band.c1[y]-m;
        std::fill(current.begin(),current.end(),inf);bool reachable=false;
        for(unsigned i=lo;i<hi;++i){
            if(!band.Valid(i,y))continue;
            double best=0;std::int8_t d=0;
            if(y){best=previous[i];if(i && previous[i-1]<best){best=previous[i-1];d=-1;}if(i+1<w && previous[i+1]<best){best=previous[i+1];d=1;}}
            if(best==inf)continue;
            const double cost=std::abs(double(band.At(band.a,i,y))-band.At(band.b,i,y)-ratio.At(i,y))+std::abs(band.Gradient(band.a,i,y)-band.Gradient(band.b,i,y));
            current[i]=best+cost;back[static_cast<size_t>(y)*w+i]=d;reachable=true;
        }
        if(!reachable)throw std::invalid_argument("seam blend has no connected path through the overlap");
        std::swap(previous,current);
    }
    // Equal totals: nearest the middle of the last row's allowed span, then left.
    unsigned end=lo;
    for(unsigned i=lo;i<hi;++i){
        const auto distance=[&](unsigned k){return std::abs(2*static_cast<long long>(k)-static_cast<long long>(lo+hi-1));};
        if(previous[i]<previous[end] || (previous[i]==previous[end] && distance(i)<distance(end)))end=i;
    }
    std::vector<std::uint32_t> x(h);unsigned i=end;
    for(unsigned y=h;y-->0;){x[y]=band.x0+i;i=static_cast<unsigned>(static_cast<int>(i)+back[static_cast<size_t>(y)*w+i]);}
    return x;
}
// Box sum over [y-radius,y+radius] clipped to the rows.
std::vector<double> BoxRows(const std::vector<double>& v,unsigned radius){
    std::vector<double> prefix(v.size()+1,0);for(size_t k=0;k<v.size();++k)prefix[k+1]=prefix[k]+v[k];
    std::vector<double> out(v.size());
    for(size_t k=0;k<v.size();++k){const auto a=k>radius?k-radius:0;const auto b=std::min(v.size(),k+radius+1);out[k]=prefix[b]-prefix[a];}
    return out;
}
} // namespace

SeamBlendResult RenderPairSeamCut(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,const SeamBlendSettings& settings){
    ValidateSeamBlendSettings(settings);
    const auto frame=Prepare(a,b,parameters);
    SeamBlendResult result;auto& record=result.record;record.settings=settings;
    std::vector<double> sum_a,sum_b;
    {
        const auto band=SampleBand(a,b,frame);
        record.seam_x=FindSeam(band,MeasureLocalRatio(band,2*settings.brightness_half_width_pixels),settings.edge_margin_pixels);
        // Ratio of window sums: paper dominates, and the same ink is inside
        // both windows while the residual is smaller than the window.
        sum_a.assign(band.height,0);sum_b.assign(band.height,0);
        for(unsigned y=0;y<band.height;++y){
            const unsigned s=record.seam_x[y]-band.x0,hw=settings.brightness_half_width_pixels;
            for(unsigned i=s>hw?s-hw:0;i<=std::min(band.width-1,s+hw);++i)
                if(band.Valid(i,y)){sum_a[y]+=std::expm1(double(band.At(band.a,i,y)));sum_b[y]+=std::expm1(double(band.At(band.b,i,y)));}
        }
        const auto r=settings.brightness_smoothing_rows;sum_a=BoxRows(BoxRows(sum_a,r),r);sum_b=BoxRows(BoxRows(sum_b,r),r);
        result.row_ratio.resize(band.height);double difference=0;
        for(unsigned y=0;y<band.height;++y){
            const double ratio=sum_a[y]>0 && sum_b[y]>0?sum_a[y]/sum_b[y]:1;result.row_ratio[y]=ratio;
            const unsigned s=record.seam_x[y]-band.x0;
            const double d=std::abs(std::expm1(double(band.At(band.a,s,y)))-ratio*std::expm1(double(band.At(band.b,s,y))));
            difference+=d;record.difference_maximum=std::max(record.difference_maximum,d);
        }
        record.difference_mean=difference/band.height;
        const auto [lo,hi]=std::minmax_element(result.row_ratio.begin(),result.row_ratio.end());record.ratio_minimum=*lo;record.ratio_maximum=*hi;
    }
    record.summary=SummarizeSeam(record.seam_x);
    const unsigned width=frame.plan.output_width,height=frame.plan.output_height;
    auto& output=result.image;output={width,height,std::vector<std::uint8_t>(static_cast<size_t>(render::PixelCount(width,height))*3)};
    const double blend=settings.width_pixels;
    for(unsigned y=0;y<height;++y){
        const double seam=record.seam_x[y],ratio=result.row_ratio[y],fade=width-seam;
        std::uint8_t* const row=output.bgr.data()+static_cast<size_t>(y)*width*3;
        for(unsigned x=0;x<width;++x){
            const double target=std::clamp((x-seam)/blend+0.5,0.0,1.0);
            std::array<double,3> pa{},pb{};bool ha=false,hb=false;
            if(target<=0){ha=frame.A(a,x,y,pa);if(!ha)hb=frame.B(b,x,y,pb);}
            else if(target>=1){hb=frame.B(b,x,y,pb);if(!hb)ha=frame.A(a,x,y,pa);}
            else{ha=frame.A(a,x,y,pa);hb=frame.B(b,x,y,pb);}
            if(!ha && !hb)throw std::invalid_argument("approved crop contains an uncovered output pixel");
            const double w=ha && hb?target:hb?1:0;
            double gain=x<=seam?ratio:ratio+(1-ratio)*(x-seam)/fade;
            if(w>0 && gain>1){
                const double peak=std::max({pb[0],pb[1],pb[2]});
                if(peak*gain>255){const double limited=std::max(1.0,255/peak);if(limited<gain){gain=limited;++record.saturation_limited_pixels;}}
            }
            for(int c=0;c<3;++c)row[x*3+c]=static_cast<std::uint8_t>(std::clamp(std::lround((1-w)*pa[c]+w*gain*pb[c]),0L,255L));
        }
    }
    return result;
}

std::vector<SeamDisplacementProbe> ProbeSeamDisplacement(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,
    const std::vector<std::uint32_t>& seam_x,unsigned patch,unsigned search,unsigned row_step){
    const auto frame=Prepare(a,b,parameters);
    const unsigned width=frame.plan.output_width,height=frame.plan.output_height;
    if(seam_x.size()!=height || patch<4 || patch>512 || !search || search>256 || !row_step)throw std::invalid_argument("invalid seam probe");
    const unsigned half=patch/2,reach=half+search,side=patch+2*search;
    std::vector<SeamDisplacementProbe> probes;
    for(unsigned y=reach;y+reach+1<height;y+=row_step){
        SeamDisplacementProbe probe{y,0,0,0,false};const unsigned xs=seam_x[y];
        if(xs<reach || xs+reach+1>=width){probes.push_back(probe);continue;}
        // Gradient magnitude of luminance on side x side cells, from side+1 samples.
        const auto plane=[&](bool camera_b,std::vector<double>& g){
            std::vector<double> l(static_cast<size_t>(side+1)*(side+1));
            for(unsigned v=0;v<=side;++v)for(unsigned u=0;u<=side;++u){
                std::array<double,3> p{};const double gx=double(xs)-reach+u,gy=double(y)-reach+v;
                if(!(camera_b?frame.B(b,gx,gy,p):frame.A(a,gx,gy,p)))return false;
                l[static_cast<size_t>(v)*(side+1)+u]=Luminance(p);
            }
            g.resize(static_cast<size_t>(side)*side);
            for(unsigned v=0;v<side;++v)for(unsigned u=0;u<side;++u){
                const auto k=static_cast<size_t>(v)*(side+1)+u;const double dx=l[k+1]-l[k],dy=l[k+side+1]-l[k];
                g[static_cast<size_t>(v)*side+u]=std::sqrt(dx*dx+dy*dy);
            }
            return true;
        };
        std::vector<double> ga,gb;
        if(!plane(false,ga) || !plane(true,gb)){probes.push_back(probe);continue;}
        std::vector<double> t(static_cast<size_t>(patch)*patch);
        for(unsigned v=0;v<patch;++v)for(unsigned u=0;u<patch;++u)t[static_cast<size_t>(v)*patch+u]=ga[static_cast<size_t>(v+search)*side+u+search];
        const double norm=correlation_detail::Center(t);
        if(!(norm>0)){probes.push_back(probe);continue;}
        // Strict best of a template over every window offset in [0,2*search]^2.
        const auto find=[&](const std::vector<double>& tt,double tn,const std::vector<double>& plane,int& fx,int& fy,double& best){
            double second=-2;best=-2;
            for(unsigned v=0;v<=2*search;++v)for(unsigned u=0;u<=2*search;++u){
                double r{};if(!correlation_detail::WindowCorrelation(tt,tn,patch,patch,plane.data()+static_cast<size_t>(v)*side+u,side,r))continue;
                if(r>best){second=best;best=r;fx=static_cast<int>(u)-static_cast<int>(search);fy=static_cast<int>(v)-static_cast<int>(search);}
                else if(r>second)second=r;
            }
            return best>second;
        };
        int bx=0,by=0;double best{};const bool unique=find(t,norm,gb,bx,by,best);
        probe.dx=bx;probe.dy=by;probe.correlation=best;
        // Round trip, as the similarity tiles: B's matched patch searched back
        // in A must return to A's patch. Blank paper matches noise and fails it.
        if(unique && std::abs(bx)<static_cast<int>(search) && std::abs(by)<static_cast<int>(search)){
            std::vector<double> back(t.size());
            for(unsigned v=0;v<patch;++v)for(unsigned u=0;u<patch;++u)back[static_cast<size_t>(v)*patch+u]=gb[static_cast<size_t>(static_cast<int>(v+search)+by)*side+static_cast<unsigned>(static_cast<int>(u+search)+bx)];
            const double back_norm=correlation_detail::Center(back);int rx=0,ry=0;double reverse{};
            probe.valid=back_norm>0 && find(back,back_norm,ga,rx,ry,reverse) && rx==0 && ry==0;
        }
        probes.push_back(probe);
    }
    return probes;
}
}
