#include "a0/m2/experimental/seam_blend.hpp"
#include "zero_mean_correlation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace a0::m2::experimental {
namespace {
using render::BgrImage;
double Luminance(const std::array<double,3>& p){return p[0]*0.114+p[1]*0.587+p[2]*0.299;}
// The blend reads each camera through a mapping from the output pixel (x,y) to a
// raw position of that camera, sampled bilinearly (coverage [0,w)x[0,h)). A
// mapping is any callable bool(double x,double y,render::Point& raw); false means
// the camera has no sample there. Frame is a template so that the pair mapping of
// render::RenderPair (below) stays a pair of plain functors with no indirect call.
template<class MapA,class MapB> struct Frame {
    unsigned width{},height{};
    MapA map_a;MapB map_b;
    bool A(const BgrImage& a,double x,double y,std::array<double,3>& p) const {render::Point s{};return map_a(x,y,s) && render::SampleBilinear(a,s.x,s.y,p);}
    bool B(const BgrImage& b,double x,double y,std::array<double,3>& p) const {render::Point s{};return map_b(x,y,s) && render::SampleBilinear(b,s.x,s.y,p);}
};
// render::RenderPair as a pair of mappings: output pixel (x,y) is global (ox+x,oy+y);
// A is sampled there, B through the inverse of camera_b_to_camera_a.
struct PairMapA {
    double ox{},oy{};
    bool operator()(double x,double y,render::Point& raw) const {raw={ox+x,oy+y};return true;}
};
struct PairMapB {
    render::Matrix3 inverse{};double ox{},oy{};
    bool operator()(double x,double y,render::Point& raw) const {return render::TryTransform(inverse,{ox+x,oy+y},raw);}
};
using PairFrame=Frame<PairMapA,PairMapB>;
struct Prepared {render::PairCanvasPlan plan;PairFrame frame;};
// The images are checked against their buffers when given (null: only the sizes are known).
Prepared Prepare(unsigned aw,unsigned ah,unsigned bw,unsigned bh,const BgrImage* a,const BgrImage* b,const render::PairRenderParameters& p){
    if(p.layout!=StitchLayout::camera_a_left_camera_b_right)throw std::invalid_argument("seam blend needs A left and B right");
    const auto inverse=render::Invert(p.camera_b_to_camera_a);
    render::ValidateProjectiveDomain(p.camera_b_to_camera_a,bw,bh);
    if(a && b && (a->bgr.size()!=render::PixelCount(a->width,a->height)*3 || b->bgr.size()!=render::PixelCount(b->width,b->height)*3))
        throw std::invalid_argument("seam blend buffer size does not match its dimensions");
    Prepared r;r.plan=render::PlanPairCanvas(aw,ah,bw,bh,p);
    const double ox=r.plan.origin_x+p.crop.left,oy=r.plan.origin_y+p.crop.top;
    r.frame={r.plan.output_width,r.plan.output_height,PairMapA{ox,oy},PairMapB{inverse,ox,oy}};return r;
}
Prepared Prepare(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& p){return Prepare(a.width,a.height,b.width,b.height,&a,&b,p);}
// An arbitrary output-to-camera mapping (render::OutputToCamera): the output pixel is
// passed as its integer index (x,y), exactly as the mappings of the document render
// and of the seam gate expect it. A non-finite result fails instead of being sampled.
struct FunctionMap {
    const render::OutputToCamera* map{};
    bool operator()(double x,double y,render::Point& raw) const {
        if(!(*map)({x,y},raw))return false;
        if(!std::isfinite(raw.x) || !std::isfinite(raw.y))throw std::invalid_argument("output-to-camera mapping returned nonfinite camera coordinates");
        return true;
    }
};
using MappedFrame=Frame<FunctionMap,FunctionMap>;
MappedFrame PrepareMapped(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& in){
    (void)render::PixelCount(in.width,in.height);
    if(!in.camera_a || !in.camera_b)throw std::invalid_argument("seam blend needs both output-to-camera mappings");
    if(a.bgr.size()!=render::PixelCount(a.width,a.height)*3 || b.bgr.size()!=render::PixelCount(b.width,b.height)*3)
        throw std::invalid_argument("seam blend buffer size does not match its dimensions");
    return {in.width,in.height,FunctionMap{&in.camera_a},FunctionMap{&in.camera_b}};
}
void RequireThreadCount(unsigned threads){
    if(threads<1 || threads>256)throw std::invalid_argument("seam blend: thread count must be 1..256");
}
// body(y) for every row y in [0,rows), on up to `threads` workers that take the rows in increasing order.
// The rows must not depend on one another: each writes only its own row of the outputs. A row that throws
// stops the rows after it; the exception of the lowest row that threw is rethrown, as if the rows had run
// in order. With one thread the rows run on the calling thread, in order.
template<class F> void ForEachRow(unsigned rows,unsigned threads,F&& body){
    if(threads<=1 || rows<=1){for(unsigned y=0;y<rows;++y)body(y);return;}
    std::atomic<unsigned> next{0},failed_row{rows};
    std::mutex lock;std::exception_ptr failure;
    const auto work=[&]{
        for(;;){
            const unsigned y=next.fetch_add(1);
            if(y>=rows || y>=failed_row.load())return;
            try{body(y);}
            catch(...){const std::lock_guard guard(lock);if(y<failed_row.load()){failed_row=y;failure=std::current_exception();}}
        }
    };
    const unsigned workers=std::min(threads,rows);
    std::vector<std::thread> pool;
    pool.reserve(workers-1);
    try{for(unsigned t=1;t<workers;++t)pool.emplace_back(work);}
    catch(const std::system_error&){} // fewer workers; the calling thread takes the rest
    work();
    for(auto& t:pool)t.join();
    if(failure)std::rethrow_exception(failure);
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
// The band over the columns [x0,x0+width) of every output row. one_run: the
// covered cells of a row must be one run (no uncovered cell between the first and
// the last covered one); a mapped pair that breaks it fails instead of choosing a side.
template<class MapA,class MapB>
Band SampleBandColumns(const BgrImage& a,const BgrImage& b,const Frame<MapA,MapB>& f,unsigned x0,unsigned width,bool one_run,unsigned threads){
    Band band{x0,width,f.height,{},{},{},{}};
    const auto cells=static_cast<size_t>(band.width)*band.height;
    band.a.assign(cells,kAbsent);band.b.assign(cells,kAbsent);band.c0.assign(band.height,0);band.c1.assign(band.height,0);
    ForEachRow(band.height,threads,[&](unsigned y){
        bool any=false;float* const ra=band.a.data()+static_cast<size_t>(y)*band.width;float* const rb=band.b.data()+static_cast<size_t>(y)*band.width;
        for(unsigned i=0;i<band.width;++i){
            std::array<double,3> pa{},pb{};
            if(!f.A(a,band.x0+i,y,pa) || !f.B(b,band.x0+i,y,pb))continue;
            ra[i]=static_cast<float>(std::log1p(Luminance(pa)));rb[i]=static_cast<float>(std::log1p(Luminance(pb)));
            if(!any)band.c0[y]=i;any=true;band.c1[y]=i+1;
        }
        if(!any)throw std::invalid_argument("seam blend needs A and B on every output row");
        if(one_run)for(unsigned i=band.c0[y];i<band.c1[y];++i)
            if(std::isnan(ra[i]))throw std::invalid_argument("the overlap of A and B is not one run on an output row");
    });
    return band;
}
// The pair mapping of render::RenderPair: the columns come from the bounds of A and of the placed B.
Band SampleBand(const BgrImage& a,const BgrImage& b,const Prepared& prepared,unsigned threads){
    const auto& plan=prepared.plan;const double ox=prepared.frame.map_a.ox;
    const double start=std::max(plan.a_bounds.minimum_x,plan.b_bounds.minimum_x),end=std::min(plan.a_bounds.maximum_x,plan.b_bounds.maximum_x);
    const double first=std::max(0.0,std::floor(start-ox)-1),last=std::min(double(plan.output_width),std::ceil(end-ox)+1);
    if(!(last>first))throw std::invalid_argument("seam blend has no overlap band");
    return SampleBandColumns(a,b,prepared.frame,static_cast<unsigned>(first),static_cast<unsigned>(last-first),false,threads);
}
// Both cameras have a sample at the output pixel (the coverage test of render::SampleBilinear).
bool InsideImage(const BgrImage& image,const render::Point& raw){return raw.x>=0.0 && raw.y>=0.0 && raw.x<double(image.width) && raw.y<double(image.height);}
bool BothCover(const MappedFrame& f,const BgrImage& a,const BgrImage& b,unsigned x,unsigned y){
    render::Point pa{},pb{};
    return f.map_a(x,y,pa) && InsideImage(a,pa) && f.map_b(x,y,pb) && InsideImage(b,pb);
}
// Per output row, the run [first,end) of columns covered by both cameras that
// contains the first covered column found on a coarse scan (every 8th column,
// then every column for a row the coarse scan misses), walked to both ends.
std::vector<MappedOverlapRow> OverlapRows(const MappedFrame& f,const BgrImage& a,const BgrImage& b,unsigned threads){
    std::vector<MappedOverlapRow> rows(f.height);
    ForEachRow(f.height,threads,[&](unsigned y){
        long long seed=-1;
        for(unsigned x=0;x<f.width && seed<0;x+=8)if(BothCover(f,a,b,x,y))seed=x;
        for(unsigned x=0;x<f.width && seed<0;++x)if(BothCover(f,a,b,x,y))seed=x;
        if(seed<0)throw std::invalid_argument("seam blend needs A and B on every output row");
        unsigned lo=static_cast<unsigned>(seed),hi=lo+1;
        while(lo>0 && BothCover(f,a,b,lo-1,y))--lo;
        while(hi<f.width && BothCover(f,a,b,hi,y))++hi;
        rows[y]={lo,hi};
    });
    return rows;
}
// The band of a mapped pair: the columns of the overlap runs of all rows, one cell wider on each side.
Band SampleMappedBand(const BgrImage& a,const BgrImage& b,const MappedFrame& f,unsigned threads){
    const auto rows=OverlapRows(f,a,b,threads);unsigned first=f.width,end=0;
    for(const auto& r:rows){first=std::min(first,r.first);end=std::max(end,r.end);}
    first=first?first-1:0;end=std::min(f.width,end+1);
    return SampleBandColumns(a,b,f,first,end-first,true,threads);
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
// Brightness along a decided seam (one column per output row in record.seam_x,
// inside the band): the A/B ratio of window sums per row, smoothed along rows.
// Fills result.row_ratio and the record's difference and ratio fields. Shared
// by the searched seam (RenderPairSeamCut) and the fixed seam.
void MeasureAlongSeam(const Band& band,const SeamBlendSettings& settings,SeamBlendResult& result){
    auto& record=result.record;
    // Ratio of window sums: paper dominates, and the same ink is inside
    // both windows while the residual is smaller than the window.
    std::vector<double> sum_a(band.height,0),sum_b(band.height,0);
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
// ---- Colour match of B to A (#415) ----
// The gain applied to B is, per channel, global[c] * (residual[y][c] at the seam, fading to 1 at the
// right output edge). See ColorMatch in seam_blend.hpp.
struct ColorPlan {
    std::array<double,3> global{1,1,1};
    std::vector<std::array<double,3>> residual; // per output row, at the seam
};
// Per camera, the lowest log luminance and the highest gradient of a paper pixel.
struct PaperLevels {float floor_a{},floor_b{};double ceiling_a{},ceiling_b{};};
// Value at quantile q of v (v is reordered). Deterministic: it is the value of one order statistic.
template<class T> T QuantileOf(std::vector<T>& v,double q){
    const auto k=static_cast<size_t>(q*static_cast<double>(v.size()-1));
    std::nth_element(v.begin(),v.begin()+static_cast<std::ptrdiff_t>(k),v.end());return v[k];
}
// The levels from a fixed-stride sample of the cells covered by both cameras. A band with no covered
// cell never reaches here (SampleBandColumns rejects a row without one).
// A running count over the cells in row-major order is split by rows: the count of every row is taken on
// its own (rows in parallel), a row then starts from the sum of the rows before it, so the cell that
// carries a given count is the same as in a single pass.
std::vector<std::uint64_t> FirstCountOfRows(const std::vector<std::uint64_t>& per_row,std::uint64_t& total){
    std::vector<std::uint64_t> first(per_row.size());total=0;
    for(size_t y=0;y<per_row.size();++y){first[y]=total;total+=per_row[y];}
    return first;
}
PaperLevels MeasurePaperLevels(const Band& band,std::uint64_t& covered,unsigned threads){
    const auto& d=kPaperSelectionDesign;
    std::vector<std::uint64_t> covered_in_row(band.height,0);
    ForEachRow(band.height,threads,[&](unsigned y){
        std::uint64_t n=0;for(unsigned i=band.c0[y];i<band.c1[y];++i)n+=band.Valid(i,y)?1u:0u;covered_in_row[y]=n;
    });
    const auto first_covered=FirstCountOfRows(covered_in_row,covered);
    const std::uint64_t stride=std::max<std::uint64_t>(1,covered/d.quantile_sample_cells);
    struct Sample {float la,lb;double ga,gb;};
    std::vector<std::vector<Sample>> sampled(band.height);
    ForEachRow(band.height,threads,[&](unsigned y){
        std::uint64_t k=first_covered[y];
        for(unsigned i=band.c0[y];i<band.c1[y];++i){
            if(!band.Valid(i,y) || k++%stride)continue;
            sampled[y].push_back({band.At(band.a,i,y),band.At(band.b,i,y),band.Gradient(band.a,i,y),band.Gradient(band.b,i,y)});
        }
    });
    std::vector<float> la,lb;std::vector<double> ga,gb;
    for(auto& row:sampled){
        for(const auto& s:row){la.push_back(s.la);lb.push_back(s.lb);ga.push_back(s.ga);gb.push_back(s.gb);}
        row=std::vector<Sample>{};
    }
    if(la.empty())throw std::invalid_argument("seam blend has no cell covered by both cameras");
    const auto floor_of=[&](std::vector<float>& v){return static_cast<float>(std::log1p(d.luminance_fraction*std::expm1(static_cast<double>(QuantileOf(v,d.luminance_quantile)))));};
    return {floor_of(la),floor_of(lb),QuantileOf(ga,d.gradient_quantile),QuantileOf(gb,d.gradient_quantile)};
}
bool IsPaper(const Band& band,const PaperLevels& t,unsigned i,unsigned y){
    return band.Valid(i,y) && band.At(band.a,i,y)>=t.floor_a && band.At(band.b,i,y)>=t.floor_b &&
        band.Gradient(band.a,i,y)<=t.ceiling_a && band.Gradient(band.b,i,y)<=t.ceiling_b;
}
// Sums of A and of B per channel over the paper pixels: of the whole band, and per row in the window
// around that row's seam. See PaperSelectionDesign for what a paper pixel is. The median ratio that the
// last criterion compares with is taken from a strided sample of the pixels that meet the others.
struct PaperSums {
    std::array<double,3> a{},b{};std::uint64_t cells{};
    std::array<std::vector<double>,3> row_a,row_b;
};
template<class MapA,class MapB>
PaperSums SumPaper(const BgrImage& a,const BgrImage& b,const Frame<MapA,MapB>& frame,const Band& band,const PaperLevels& levels,
    const std::vector<std::uint32_t>& seam_x,unsigned half_window,unsigned threads){
    const auto& d=kPaperSelectionDesign;
    // A sample of both cameras at a band cell that is not clipped and has a positive value in every channel.
    const auto sample=[&](unsigned i,unsigned y,std::array<double,3>& pa,std::array<double,3>& pb){
        if(!frame.A(a,band.x0+i,y,pa) || !frame.B(b,band.x0+i,y,pb))return false;
        if(std::max({pa[0],pa[1],pa[2],pb[0],pb[1],pb[2]})>=d.clipped_level)return false;
        return std::min({pa[0],pa[1],pa[2],pb[0],pb[1],pb[2]})>0;
    };
    // The three passes below take the rows in parallel. Every floating-point value that is summed is summed
    // in the order of a single pass over the rows, so the result does not depend on the thread count: the
    // per-row window sums only ever add the cells of their own row in column order, and the sums over the
    // whole band are added by one thread, row after row (see the blocks of the last pass).
    std::vector<std::uint64_t> candidates_in_row(band.height,0);
    ForEachRow(band.height,threads,[&](unsigned y){
        std::uint64_t n=0;for(unsigned i=band.c0[y];i<band.c1[y];++i)n+=IsPaper(band,levels,i,y)?1u:0u;candidates_in_row[y]=n;
    });
    std::uint64_t candidates=0;
    const auto first_candidate=FirstCountOfRows(candidates_in_row,candidates);
    const std::uint64_t stride=std::max<std::uint64_t>(1,candidates/d.quantile_sample_cells);
    std::array<std::vector<double>,3> ratio;
    {
        std::vector<std::vector<std::array<double,3>>> sampled(band.height);
        ForEachRow(band.height,threads,[&](unsigned y){
            std::uint64_t k=first_candidate[y];
            for(unsigned i=band.c0[y];i<band.c1[y];++i){
                if(!IsPaper(band,levels,i,y) || k++%stride)continue;
                std::array<double,3> pa{},pb{};
                if(sample(i,y,pa,pb))sampled[y].push_back({pa[0]/pb[0],pa[1]/pb[1],pa[2]/pb[2]});
            }
        });
        for(const auto& row:sampled)for(const auto& r:row)for(int c=0;c<3;++c)ratio[c].push_back(r[c]);
    }
    std::array<double,3> lowest{0,0,0},highest{0,0,0}; // no candidate: nothing is kept
    if(!ratio[0].empty())for(int c=0;c<3;++c){const double median=QuantileOf(ratio[c],0.5);lowest[c]=median/d.ratio_tolerance;highest[c]=median*d.ratio_tolerance;}
    PaperSums sums;for(int c=0;c<3;++c){sums.row_a[c].assign(band.height,0);sums.row_b[c].assign(band.height,0);}
    // A block of rows is sampled in parallel; the samples it keeps (A then B, three channels each, per kept
    // cell) are then added to the sums of the band in the order of the cells. The block bounds the memory.
    constexpr unsigned kBlockRows=128;
    std::vector<std::vector<double>> kept(std::min(kBlockRows,band.height));
    for(unsigned y0=0;y0<band.height;y0+=kBlockRows){
        const unsigned rows=std::min(kBlockRows,band.height-y0);
        ForEachRow(rows,threads,[&](unsigned r){
            const unsigned y=y0+r;auto& out=kept[r];out.clear();
            const unsigned s=seam_x[y]-band.x0,first=s>half_window?s-half_window:0,last=std::min(band.width-1,s+half_window);
            std::array<double,3> window_a{0,0,0},window_b{0,0,0};
            for(unsigned i=band.c0[y];i<band.c1[y];++i){
                if(!IsPaper(band,levels,i,y))continue;
                std::array<double,3> pa{},pb{};
                if(!sample(i,y,pa,pb))continue;
                bool consistent=true;
                for(int c=0;c<3;++c){const double q=pa[c]/pb[c];consistent=consistent && q>=lowest[c] && q<=highest[c];}
                if(!consistent)continue;
                out.insert(out.end(),pa.begin(),pa.end());out.insert(out.end(),pb.begin(),pb.end());
                if(i>=first && i<=last)for(int c=0;c<3;++c){window_a[c]+=pa[c];window_b[c]+=pb[c];}
            }
            for(int c=0;c<3;++c){sums.row_a[c][y]=window_a[c];sums.row_b[c][y]=window_b[c];}
        });
        for(unsigned r=0;r<rows;++r){
            const auto& out=kept[r];
            for(size_t j=0;j<out.size();j+=6){
                ++sums.cells;
                for(int c=0;c<3;++c){sums.a[c]+=out[j+c];sums.b[c]+=out[j+3+c];}
            }
        }
    }
    return sums;
}
// Measures the global and the residual gains of B along record.seam_x (inside the band) and fills the
// record's colour, difference and ratio fields, result.row_ratio and result.row_gain. The residual is
// A over B-after-the-global-gain on the paper pixels of the window, smoothed along rows as the luminance
// ratio is; where a row has no paper pixel in reach it is 1.
// When the global gain cannot be measured it returns no plan and leaves only the counts and the
// fallback mark in record.color: the caller then renders with the luminance ratio, which still ties B
// to A, rather than leave B with no ratio at all.
template<class MapA,class MapB>
std::optional<ColorPlan> MeasureColorMatch(const BgrImage& a,const BgrImage& b,const Frame<MapA,MapB>& frame,const Band& band,const SeamBlendSettings& settings,
    const ColorMatch& color,SeamBlendResult& result,unsigned threads){
    constexpr double weight[3]={0.114,0.587,0.299}; // B, G, R luminance weights, as Luminance()
    auto& record=result.record;auto& cm=record.color;
    cm.mode=ColorMatchMode::rgb_paper;cm.gain_limit_set=color.gain_limit.has_value();cm.gain_limit=color.gain_limit.value_or(0);
    const double limit=color.gain_limit.value_or(0);
    const auto levels=MeasurePaperLevels(band,cm.covered_cells,threads);
    auto sums=SumPaper(a,b,frame,band,levels,record.seam_x,settings.brightness_half_width_pixels,threads);
    cm.paper_cells=sums.cells;
    ColorPlan plan;
    const auto needed=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(std::ceil(kPaperSelectionDesign.minimum_cell_fraction*static_cast<double>(cm.covered_cells))));
    cm.global_measured=sums.cells>=needed && sums.a[0]>0 && sums.a[1]>0 && sums.a[2]>0 && sums.b[0]>0 && sums.b[1]>0 && sums.b[2]>0;
    if(!cm.global_measured){cm.luminance_fallback=true;return std::nullopt;}
    for(int c=0;c<3;++c){
        cm.measured_global[c]=sums.a[c]/sums.b[c];
        plan.global[c]=cm.measured_global[c];
        if(color.gain_limit){const auto kept=std::clamp(plan.global[c],1/limit,limit);if(kept!=plan.global[c]){plan.global[c]=kept;cm.global_limited=true;}}
        cm.global_gain[c]=plan.global[c];
    }
    const auto r=settings.brightness_smoothing_rows;
    for(int c=0;c<3;++c){sums.row_a[c]=BoxRows(BoxRows(sums.row_a[c],r),r);sums.row_b[c]=BoxRows(BoxRows(sums.row_b[c],r),r);}
    plan.residual.assign(band.height,{1,1,1});result.row_gain.resize(band.height);result.row_ratio.resize(band.height);
    cm.local_minimum={1,1,1};cm.local_maximum={1,1,1};double difference=0;
    for(unsigned y=0;y<band.height;++y){
        bool limited=false;double numerator=0,denominator=0;
        for(int c=0;c<3;++c){
            double residual=sums.row_a[c][y]>0 && sums.row_b[c][y]>0?sums.row_a[c][y]/(plan.global[c]*sums.row_b[c][y]):1;
            if(color.gain_limit){const auto kept=std::clamp(residual,1/(limit*plan.global[c]),limit/plan.global[c]);limited=limited || kept!=residual;residual=kept;}
            plan.residual[y][c]=residual;result.row_gain[y][c]=plan.global[c]*residual;
            numerator+=weight[c]*result.row_gain[y][c]*sums.row_b[c][y];denominator+=weight[c]*sums.row_b[c][y];
            cm.local_minimum[c]=y?std::min(cm.local_minimum[c],residual):residual;cm.local_maximum[c]=y?std::max(cm.local_maximum[c],residual):residual;
        }
        cm.local_limited_rows+=limited?1u:0u;
        result.row_ratio[y]=denominator>0?numerator/denominator:weight[0]*result.row_gain[y][0]+weight[1]*result.row_gain[y][1]+weight[2]*result.row_gain[y][2];
        // The difference at the seam pixel: luminance of A against B after the gain applied there.
        std::array<double,3> pa{},pb{};
        if(frame.A(a,record.seam_x[y],y,pa) && frame.B(b,record.seam_x[y],y,pb)){
            const double d=std::abs(Luminance(pa)-(weight[0]*result.row_gain[y][0]*pb[0]+weight[1]*result.row_gain[y][1]*pb[1]+weight[2]*result.row_gain[y][2]*pb[2]));
            difference+=d;record.difference_maximum=std::max(record.difference_maximum,d);
        }
    }
    record.difference_mean=difference/band.height;
    const auto [lo,hi]=std::minmax_element(result.row_ratio.begin(),result.row_ratio.end());record.ratio_minimum=*lo;record.ratio_maximum=*hi;
    return plan;
}
// Mixes A and B along record.seam_x with row_ratio (both set), as described in
// seam_blend.hpp. Takes the partly filled result and returns it with the image
// and the seam summary. With a ColorPlan the gain of B is the per-channel one of the plan; row_ratio
// is then a record only.
template<class MapA,class MapB>
SeamBlendResult ComposeAlongSeam(const BgrImage& a,const BgrImage& b,const Frame<MapA,MapB>& frame,const SeamBlendSettings& settings,SeamBlendResult result,unsigned threads,const ColorPlan* color=nullptr){
    auto& record=result.record;
    record.summary=SummarizeSeam(record.seam_x);
    const unsigned width=frame.width,height=frame.height;
    auto& output=result.image;output={width,height,std::vector<std::uint8_t>(static_cast<size_t>(render::PixelCount(width,height))*3)};
    const double blend=settings.width_pixels;
    // Rows are independent: each writes its own bytes of the image, and the pixels whose gain was lowered
    // are counted per row and added up (an integer sum, so the order does not matter).
    std::atomic<std::uint64_t> limited_pixels{0};
    ForEachRow(height,threads,[&](unsigned y){
        std::uint64_t limited_in_row=0;
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
            if(color){
                // Global gain times the residual, which fades to 1 to the right of the seam. When a channel of B
                // would pass 255 the three gains are lowered together, so the hue of the match stays.
                std::array<double,3> gain{};const auto& residual=color->residual[y];
                for(int c=0;c<3;++c)gain[c]=color->global[c]*(x<=seam?residual[c]:residual[c]+(1-residual[c])*(x-seam)/fade);
                if(w>0){
                    const double peak=std::max({pb[0]*gain[0],pb[1]*gain[1],pb[2]*gain[2]});
                    if(peak>255){const double scale=255/peak;for(auto& g:gain)g*=scale;++limited_in_row;}
                }
                for(int c=0;c<3;++c)row[x*3+c]=static_cast<std::uint8_t>(std::clamp(std::lround((1-w)*pa[c]+w*gain[c]*pb[c]),0L,255L));
                continue;
            }
            double gain=x<=seam?ratio:ratio+(1-ratio)*(x-seam)/fade;
            if(w>0 && gain>1){
                const double peak=std::max({pb[0],pb[1],pb[2]});
                if(peak*gain>255){const double limited=std::max(1.0,255/peak);if(limited<gain){gain=limited;++limited_in_row;}}
            }
            for(int c=0;c<3;++c)row[x*3+c]=static_cast<std::uint8_t>(std::clamp(std::lround((1-w)*pa[c]+w*gain*pb[c]),0L,255L));
        }
        limited_pixels.fetch_add(limited_in_row,std::memory_order_relaxed);
    });
    record.saturation_limited_pixels+=limited_pixels.load();
    return result;
}
// The columns seam-half..seam+half, half = ceil(width_pixels/2)-1 (the columns
// with a blend weight strictly between 0 and 1), must lie in the band and be
// covered by both A and B on every row.
bool SeamInOverlap(const Band& band,std::uint32_t column,unsigned blend_width){
    const std::uint64_t half=(blend_width-1)/2,first=std::uint64_t(band.x0),end=first+band.width;
    if(column<first+half || std::uint64_t(column)+half>=end)return false;
    for(unsigned y=0;y<band.height;++y)for(std::uint64_t c=column-half;c<=column+half;++c)
        if(!band.Valid(static_cast<unsigned>(c-first),y))return false;
    return true;
}
void RequireSeamInOverlap(const Band& band,std::uint32_t column,unsigned blend_width){
    if(!SeamInOverlap(band,column,blend_width))throw std::invalid_argument("seam column is outside the overlap of A and B");
}
// SeamInOverlap(SampleMappedBand(a,b,f,threads),column,blend_width) without the sampling: the same answer, and the same
// std::invalid_argument for the same inputs, from the coverage alone. The band is the columns of the rows' overlap runs, one cell
// wider on each side (SampleMappedBand), and a cell of it is valid when both cameras cover it (BothCover is the coverage test of
// SampleBilinear that Frame::A and Frame::B apply), so the run of a row is its valid cells when it is the row's only run. A row is
// one run when no cell of the band outside the run found by OverlapRows is covered; the cells inside it are covered by
// construction and were evaluated by the walk. Every cell of the band that the sampled form evaluates is evaluated here, in the
// same order within a row, so a mapping that fails (non-finite) fails on the same row; the row's own "not one run" is raised after
// its scan, as the sampled form does. Costs the mapping evaluations of the band once (no bilinear sample, no logarithm, no band
// buffer: the sampled band is 2 floats and 2 unsigned per cell).
bool MappedSeamInOverlap(const MappedFrame& f,const BgrImage& a,const BgrImage& b,std::uint32_t column,unsigned blend_width,unsigned threads){
    const auto rows=OverlapRows(f,a,b,threads);unsigned first=f.width,end=0;
    for(const auto& r:rows){first=std::min(first,r.first);end=std::max(end,r.end);}
    first=first?first-1:0;end=std::min(f.width,end+1);
    ForEachRow(f.height,threads,[&](unsigned y){
        const auto run=rows[y];bool outside=false;
        for(unsigned x=first;x<end;++x){
            if(x>=run.first && x<run.end){x=run.end-1;continue;}
            if(BothCover(f,a,b,x,y))outside=true;
        }
        if(outside)throw std::invalid_argument("the overlap of A and B is not one run on an output row");
    });
    const std::uint64_t half=(blend_width-1)/2;
    if(column<std::uint64_t(first)+half || std::uint64_t(column)+half>=end)return false;
    for(const auto& r:rows)if(column-half<r.first || std::uint64_t(column)+half>=r.end)return false;
    return true;
}
// The render of a fixed seam on a decided band, for the pair mapping and for mapped pairs alike.
template<class MapA,class MapB>
SeamBlendResult RenderFixedOnBand(const BgrImage& a,const BgrImage& b,const Frame<MapA,MapB>& frame,Band band,std::uint32_t seam_column,
    const SeamBlendSettings& settings,const ColorMatch& color,unsigned threads){
    SeamBlendResult result;auto& record=result.record;record.settings=settings;record.fixed_seam=true;
    RequireSeamInOverlap(band,seam_column,settings.width_pixels);
    record.seam_x.assign(band.height,seam_column);
    if(color.mode==ColorMatchMode::rgb_paper){
        const auto plan=MeasureColorMatch(a,b,frame,band,settings,color,result,threads);
        if(plan){
            band=Band{};
            return ComposeAlongSeam(a,b,frame,settings,std::move(result),threads,&*plan);
        }
        // The global gain is not measurable: fall through to the luminance ratio, which sets the
        // difference and ratio fields and row_ratio itself. record.color keeps the counts and the mark.
    }
    MeasureAlongSeam(band,settings,result);
    band=Band{};
    return ComposeAlongSeam(a,b,frame,settings,std::move(result),threads);
}
} // namespace

SeamBlendResult RenderPairSeamCut(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,const SeamBlendSettings& settings){
    ValidateSeamBlendSettings(settings);
    const auto prepared=Prepare(a,b,parameters);
    SeamBlendResult result;auto& record=result.record;record.settings=settings;
    {
        const auto band=SampleBand(a,b,prepared,1);
        record.seam_x=FindSeam(band,MeasureLocalRatio(band,2*settings.brightness_half_width_pixels),settings.edge_margin_pixels);
        MeasureAlongSeam(band,settings,result);
    }
    return ComposeAlongSeam(a,b,prepared.frame,settings,std::move(result),1);
}

SeamBlendResult RenderPairFixedSeam(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color,unsigned thread_count){
    ValidateSeamBlendSettings(settings);ValidateColorMatch(color);RequireThreadCount(thread_count);
    const auto prepared=Prepare(a,b,parameters);
    return RenderFixedOnBand(a,b,prepared.frame,SampleBand(a,b,prepared,thread_count),seam_column,settings,color,thread_count);
}

SeamBlendResult RenderPairFixedSeam(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color){
    return RenderPairFixedSeam(a,b,parameters,seam_column,settings,color,1);
}

bool CanRenderFixedSeam(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,unsigned thread_count){
    ValidateSeamBlendSettings(settings);RequireThreadCount(thread_count);
    const auto prepared=Prepare(a,b,parameters);
    return SeamInOverlap(SampleBand(a,b,prepared,thread_count),seam_column,settings.width_pixels);
}

bool CanRenderFixedSeam(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings){
    return CanRenderFixedSeam(a,b,parameters,seam_column,settings,1);
}

MappedFixedSeamInput PairMappedFixedSeamInput(std::uint32_t camera_a_width,std::uint32_t camera_a_height,std::uint32_t camera_b_width,
    std::uint32_t camera_b_height,const render::PairRenderParameters& parameters){
    const auto prepared=Prepare(camera_a_width,camera_a_height,camera_b_width,camera_b_height,nullptr,nullptr,parameters);
    const PairMapA map_a=prepared.frame.map_a;const PairMapB map_b=prepared.frame.map_b;
    return {prepared.frame.width,prepared.frame.height,
        [map_a](render::Point output,render::Point& raw){return map_a(output.x,output.y,raw);},
        [map_b](render::Point output,render::Point& raw){return map_b(output.x,output.y,raw);}};
}

MappedFixedSeamInput DocumentMappedFixedSeamInput(const render::DocumentRenderParameters& parameters){
    if(parameters.layout!=StitchLayout::camera_a_left_camera_b_right)throw std::invalid_argument("seam blend needs A left and B right");
    if(parameters.resampling!=render::Resampling::bilinear)throw std::invalid_argument("the fixed seam blend samples bilinearly");
    render::ValidateCameraProjection(parameters.camera_a,parameters.raster);
    render::ValidateCameraProjection(parameters.camera_b,parameters.raster);
    return {parameters.raster.width_pixels,parameters.raster.height_pixels,
        render::DocumentOutputToCamera(parameters.raster,parameters.camera_a),render::DocumentOutputToCamera(parameters.raster,parameters.camera_b)};
}

std::vector<MappedOverlapRow> MappedFixedSeamBand(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input,unsigned thread_count){
    RequireThreadCount(thread_count);
    return OverlapRows(PrepareMapped(a,b,input),a,b,thread_count);
}

std::vector<MappedOverlapRow> MappedFixedSeamBand(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input){
    return MappedFixedSeamBand(a,b,input,1);
}

SeamBlendResult RenderMappedFixedSeam(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color,unsigned thread_count){
    ValidateSeamBlendSettings(settings);ValidateColorMatch(color);RequireThreadCount(thread_count);
    const auto frame=PrepareMapped(a,b,input);
    return RenderFixedOnBand(a,b,frame,SampleMappedBand(a,b,frame,thread_count),seam_column,settings,color,thread_count);
}

SeamBlendResult RenderMappedFixedSeam(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color){
    return RenderMappedFixedSeam(a,b,input,seam_column,settings,color,1);
}

bool CanRenderMappedFixedSeam(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings,unsigned thread_count){
    ValidateSeamBlendSettings(settings);RequireThreadCount(thread_count);
    return MappedSeamInOverlap(PrepareMapped(a,b,input),a,b,seam_column,settings.width_pixels,thread_count);
}

bool CanRenderMappedFixedSeam(const BgrImage& a,const BgrImage& b,const MappedFixedSeamInput& input,std::uint32_t seam_column,const SeamBlendSettings& settings){
    return CanRenderMappedFixedSeam(a,b,input,seam_column,settings,1);
}

SeamBlendResult RenderDocumentFixedSeam(const BgrImage& a,const BgrImage& b,const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color,unsigned thread_count){
    return RenderMappedFixedSeam(a,b,DocumentMappedFixedSeamInput(parameters),seam_column,settings,color,thread_count);
}

SeamBlendResult RenderDocumentFixedSeam(const BgrImage& a,const BgrImage& b,const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,const ColorMatch& color){
    return RenderDocumentFixedSeam(a,b,parameters,seam_column,settings,color,1);
}

bool CanRenderDocumentFixedSeam(const BgrImage& a,const BgrImage& b,const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings,unsigned thread_count){
    return CanRenderMappedFixedSeam(a,b,DocumentMappedFixedSeamInput(parameters),seam_column,settings,thread_count);
}

bool CanRenderDocumentFixedSeam(const BgrImage& a,const BgrImage& b,const render::DocumentRenderParameters& parameters,std::uint32_t seam_column,const SeamBlendSettings& settings){
    return CanRenderDocumentFixedSeam(a,b,parameters,seam_column,settings,1);
}

FixedSeamCondition ScaleFixedSeamCondition(const FixedSeamCondition& capture,std::uint32_t capture_width,std::uint32_t capture_height,
    std::uint32_t view_width,std::uint32_t view_height){
    if(!capture_width || !capture_height || !view_width || !view_height)throw std::invalid_argument("empty size for a seam condition");
    const auto& p=capture.parameters;
    if(p.layout!=StitchLayout::camera_a_left_camera_b_right)throw std::invalid_argument("seam condition needs A left and B right");
    const auto from=render::PlanPairCanvas(capture_width,capture_height,capture_width,capture_height,p);
    if(capture.seam_column>=from.output_width)throw std::invalid_argument("seam column is outside the output");
    const double sx=double(view_width)/capture_width,sy=double(view_height)/capture_height;
    const auto& m=p.camera_b_to_camera_a;
    // S*H*S^-1 with S=diag(sx,sy,1): entry (i,j) is S_i*H_ij/S_j.
    render::PairRenderParameters scaled{{m[0],m[1]*sx/sy,m[2]*sx,m[3]*sy/sx,m[4],m[5]*sy,m[6]/sx,m[7]/sy,m[8]},p.layout,{}};
    const auto to=render::PlanPairCanvas(view_width,view_height,view_width,view_height,scaled);
    // Global canvas coordinates of the capture window and of the seam column's left edge.
    const double gx0=from.origin_x+p.crop.left,gy0=from.origin_y+p.crop.top;
    const long long ox=std::llround(to.origin_x),oy=std::llround(to.origin_y);
    // The window edges round inward, so no retained sample lies outside the scaled capture window.
    const auto inward_start=[](double v){return static_cast<long long>(std::ceil(v));};
    const auto inward_end=[](double v){return static_cast<long long>(std::floor(v));};
    long long x0=std::max(inward_start(sx*gx0),ox),x1=std::min(inward_end(sx*(gx0+from.output_width)),ox+to.canvas_width);
    long long y0=std::max(inward_start(sy*gy0),oy),y1=std::min(inward_end(sy*(gy0+from.output_height)),oy+to.canvas_height);
    if(x1<=x0 || y1<=y0)throw std::invalid_argument("scaled crop removes the complete canvas");
    scaled.crop={static_cast<std::uint32_t>(x0-ox),static_cast<std::uint32_t>(y0-oy),
        static_cast<std::uint32_t>(ox+to.canvas_width-x1),static_cast<std::uint32_t>(oy+to.canvas_height-y1)};
    const long long seam=std::llround(sx*(gx0+capture.seam_column))-x0;
    if(seam<0 || seam>=x1-x0)throw std::invalid_argument("scaled seam column is outside the output");
    return {scaled,static_cast<std::uint32_t>(seam)};
}

std::vector<SeamDisplacementProbe> ProbeSeamDisplacement(const BgrImage& a,const BgrImage& b,const render::PairRenderParameters& parameters,
    const std::vector<std::uint32_t>& seam_x,unsigned patch,unsigned search,unsigned row_step){
    const auto prepared=Prepare(a,b,parameters);const auto& frame=prepared.frame;
    const unsigned width=frame.width,height=frame.height;
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
