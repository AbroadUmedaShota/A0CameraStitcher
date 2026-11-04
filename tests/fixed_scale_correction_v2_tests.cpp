// Per-capture scale of the lens-aware fixed composite (#405 W3): the tile frame of the
// estimator against the profile's raw pixels, the fit of k in document mm, the decision
// against the limit of the condition, and that the drawn correction (the shared scaled
// mapping of fixed_composite_maps.hpp) shrinks the difference across the seam.
// Camera free and file free. Argument 1 is the shared rig profile v2 fixture folder.
//
// The truth in these tests: camera A maps exactly, and a pixel b of camera B shows the
// document point  W = P + scale (DocB(b) - P) + shift,  where DocB is B's fixed mapping and
// P the scale centre. scale 1.005 is a B placed 0.5 % larger about P than its profile says
// (what the correction multiplies into B's mapping). The tiles are built from that truth
// through the same mapping functions the code under test uses, so what is checked is the
// fit, the frame (corner coordinates, orientation) and the decision, not the lens inverse
// (fixed_composite_creation_tests.cpp). The fixture cameras are two affine ones, 8 px per
// mm, which overlap by 39 mm; a variant adds the lens terms to both.
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_scale_correction_v2.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    try{call();}catch(const std::invalid_argument&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}

const auto kCreated=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
std::string FakeHash(const std::vector<unsigned char>& bytes){
    unsigned long long h=1469598103934665603ULL;for(const auto b:bytes){h^=b;h*=1099511628211ULL;}
    char text[17];std::snprintf(text,sizeof text,"%016llx",h);return std::string(48,'0')+text;
}
const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FakeHash(bytes);};

std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}

std::string draft,lens_draft;
constexpr unsigned kWidth=7360,kHeight=4912;

render::OutputRaster Raster(long long left,long long top,long long right,long long bottom,unsigned dpi){
    const auto pixels=[&](long long um){return static_cast<std::uint32_t>((um*static_cast<long long>(dpi)+25399)/25400);};
    return render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},dpi,pixels(right-left),pixels(bottom-top)};
}
// 18 mm x 8 mm inside the 39 mm overlap, 180 x 80 pixels of 0.1 mm.
render::OutputRaster InnerRaster(){return Raster(580000,100000,598000,108000,254);}

struct Setup {FixedCompositeConditionV2 condition;FixedCompositeProfileBinding binding;};
Setup FromRequest(const FixedCompositeV2Request& r,const FixedCompositeCreationCheck& check){
    auto condition=MakeFixedCompositeConditionV2(r,kCreated,kHash,check);
    auto binding=BindFixedCompositeProfile(condition,r.profile_bytes,kHash);
    return Setup{std::move(condition),std::move(binding)};
}
Setup MakeSetup(const std::string& profile,double limit,MountOrientation orientation=MountOrientation::normal,
    const render::OutputRaster& raster=InnerRaster(),std::uint32_t column=90){
    FixedCompositeV2Request r;
    r.profile_file="wpd-fixed-composite-profile-test.json";r.profile_bytes=profile;
    r.orientation=orientation;r.raster=raster;r.scale_auto_correction_max_percent=limit;r.seam_column=column;
    r.seam_origin=FixedSeamOrigin::operator_set;
    return FromRequest(r,MakeFixedCompositeCreationCheck());
}

// A tile coordinate of the estimator for a raw stored-order pixel: pixel-corner coordinates
// of the oriented frame (the centre of pixel i is i + 0.5; the rotation reverses the order).
render::Point TileOfRaw(render::Point raw,MountOrientation orientation){
    if(orientation==MountOrientation::rotated_180)return {(kWidth-1-raw.x)+0.5,(kHeight-1-raw.y)+0.5};
    return {raw.x+0.5,raw.y+0.5};
}

struct Truth {double scale{1};render::Point shift{0,0};};
struct GridPoint {render::Point raw;unsigned row,column;};
// A on a grid of 4 rows (the overlap, document X 570 to 592 mm) by 8 columns across the sheet.
std::vector<GridPoint> Grid(){
    std::vector<GridPoint> g;
    unsigned row=0;
    for(const double v:{60.0,120.0,180.0,240.0}){
        unsigned column=0;
        for(const double u:{600.0,1500.0,2400.0,3300.0,4200.0,5100.0,6000.0,6900.0})g.push_back({{u,v},row,column++});
        ++row;
    }
    return g;
}

// An estimate whose agreed tiles follow the truth. For each A pixel a the matching B pixel b
// is the one with  P + k (DocB(b) - P) + d = DocA(a).
OverlapSimilarityEstimate ModelEstimate(const Setup& s,const Truth& truth,const std::vector<GridPoint>& grid,unsigned radius=64){
    OverlapSimilarityEstimate e;
    e.status=OverlapEstimateStatus::estimated_not_judged;e.reason=SimilarityUndeterminedReason::none;e.search_radius=radius;
    const auto& p=s.binding.parameters;
    const auto center=FixedCompositeScaleCenterMm(p.camera_b);
    for(const auto& point:grid){
        render::Point xa{},braw{};
        const bool solved=TryDocumentPointOfRawPixel(p.camera_a,point.raw,xa);
        CHECK(solved);
        const render::Point xb{center.x+(xa.x-center.x-truth.shift.x)/truth.scale,center.y+(xa.y-center.y-truth.shift.y)/truth.scale};
        CHECK(render::TryProjectDocument(p.camera_b,xb,braw));
        const auto ta=TileOfRaw(point.raw,s.condition.orientation),tb=TileOfRaw(braw,s.condition.orientation);
        SimilarityTile t{};
        t.a_x=ta.x;t.a_y=ta.y;t.b_x=tb.x;t.b_y=tb.y;t.row=point.row;t.column=point.column;t.matched=true;t.agreed=true;
        e.tiles.push_back(t);
    }
    e.matched=e.agreed=static_cast<unsigned>(e.tiles.size());
    return e;
}

FixedScaleCorrectionV2 Decide(const Setup& s,const OverlapSimilarityEstimate& e){
    return DecideFixedScaleV2(s.condition,s.binding,e,kWidth,kHeight);
}

// The largest difference along the seam column, in output pixels, between what A shows and what B
// shows at each row when B is drawn through the scaled mapping with `applied`. The document point
// each camera shows is: A, its fixed mapping; B, the truth above.
double WorstSeamDifference(const Setup& s,const Truth& truth,double applied){
    const auto& p=s.binding.parameters;
    const auto input=ScaledDocumentMappedFixedSeamInput(p,applied);
    const auto center=FixedCompositeScaleCenterMm(p.camera_b);
    const double pitch=25.4/s.condition.raster.dpi;
    double worst=0;
    for(std::uint32_t row=0;row<input.height;++row){
        render::Point ra{},rb{},xa{},xb{};
        const render::Point output{static_cast<double>(s.condition.seam.column),static_cast<double>(row)};
        CHECK(input.camera_a(output,ra) && input.camera_b(output,rb));
        CHECK(TryDocumentPointOfRawPixel(p.camera_a,ra,xa) && TryDocumentPointOfRawPixel(p.camera_b,rb,xb));
        const render::Point shown_by_b{center.x+truth.scale*(xb.x-center.x)+truth.shift.x,center.y+truth.scale*(xb.y-center.y)+truth.shift.y};
        worst=std::max(worst,std::hypot(shown_by_b.x-xa.x,shown_by_b.y-xa.y)/pitch);
    }
    return worst;
}

void Frame(){
    // Corner coordinates: the image centre (w/2, h/2) is the middle of the pixel grid.
    const auto centre=FixedScaleRawPixelOfTile(kWidth/2.0,kHeight/2.0,MountOrientation::normal,kWidth,kHeight);
    CHECK(centre.x==3679.5 && centre.y==2455.5);
    const auto turned_centre=FixedScaleRawPixelOfTile(kWidth/2.0,kHeight/2.0,MountOrientation::rotated_180,kWidth,kHeight);
    CHECK(turned_centre.x==3679.5 && turned_centre.y==2455.5);
    // The first pixel of the oriented frame is the last one of the stored order after a half turn.
    const auto first=FixedScaleRawPixelOfTile(0.5,0.5,MountOrientation::normal,kWidth,kHeight);
    CHECK(first.x==0 && first.y==0);
    const auto last=FixedScaleRawPixelOfTile(0.5,0.5,MountOrientation::rotated_180,kWidth,kHeight);
    CHECK(last.x==kWidth-1 && last.y==kHeight-1);
    const auto off=FixedScaleRawPixelOfTile(1000.5,200.25,MountOrientation::rotated_180,kWidth,kHeight);
    CHECK(off.x==kWidth-1-1000 && off.y==kHeight-1-199.75);
    // And it is the inverse of the test's own conversion.
    for(const auto o:{MountOrientation::normal,MountOrientation::rotated_180}){
        const render::Point raw{1234.25,321.75};
        const auto tile=TileOfRaw(raw,o);
        const auto back=FixedScaleRawPixelOfTile(tile.x,tile.y,o,kWidth,kHeight);
        CHECK(back.x==raw.x && back.y==raw.y);
    }
}

void Recovery(const std::string& name,const std::string& profile){
    std::cout<<name<<'\n';
    for(const auto orientation:{MountOrientation::normal,MountOrientation::rotated_180}){
        const auto s=MakeSetup(profile,1,orientation);
        // (a) B placed 0.5 % larger about P: k is found, nothing is left over.
        for(const double truth:{1.005,0.995,1.0}){
            const auto e=ModelEstimate(s,{truth,{0,0}},Grid());
            const auto r=Decide(s,e);
            std::cout<<"  truth="<<truth<<" estimated="<<(r.estimated_scale?*r.estimated_scale:0.0)<<" shift_mm="<<r.unexplained_shift_mm.x<<","<<r.unexplained_shift_mm.y
                <<" residual_max_mm="<<r.residual_mm.maximum<<'\n';
            CHECK(r.status==FixedScaleStatus::applied && r.reason==FixedScaleReason::none);
            CHECK(r.estimated_scale && Near(*r.estimated_scale,truth,1e-7) && r.applied_scale==*r.estimated_scale);
            CHECK(r.difference_percent && Near(*r.difference_percent,std::abs(truth*100-100),1e-5) && r.limit_percent==1);
            CHECK(Near(r.unexplained_shift_mm.x,0,1e-5) && Near(r.unexplained_shift_mm.y,0,1e-5));
            CHECK(Near(r.unexplained_shift_output_pixels.x,0,1e-4) && Near(r.unexplained_shift_output_pixels.y,0,1e-4));
            CHECK(r.residual_mm.maximum<1e-5 && r.residual_output_pixels.maximum<1e-4);
            CHECK(r.used_tiles==32 && r.used_rows==4 && r.first_used_row==0 && r.last_used_row==3);
        }
        // (b) a shift that is not a scale: k stays 1, the shift is left over (mm and output pixels, 0.1 mm each).
        {
            const auto r=Decide(s,ModelEstimate(s,{1.0,{0.8,-0.5}},Grid()));
            CHECK(r.status==FixedScaleStatus::applied && r.estimated_scale && Near(*r.estimated_scale,1.0,1e-7) && Near(r.applied_scale,1.0,1e-7));
            CHECK(Near(r.unexplained_shift_mm.x,0.8,1e-5) && Near(r.unexplained_shift_mm.y,-0.5,1e-5));
            CHECK(Near(r.unexplained_shift_output_pixels.x,8,1e-4) && Near(r.unexplained_shift_output_pixels.y,-5,1e-4));
            CHECK(r.residual_mm.maximum<1e-5);
        }
        // A scale and a shift together: the scale is found, the shift is left over.
        {
            const auto r=Decide(s,ModelEstimate(s,{1.004,{-0.6,0.9}},Grid()));
            CHECK(r.status==FixedScaleStatus::applied && r.estimated_scale && Near(*r.estimated_scale,1.004,1e-7));
            CHECK(Near(r.unexplained_shift_mm.x,-0.6,1e-5) && Near(r.unexplained_shift_mm.y,0.9,1e-5));
        }
    }
}

void Limit(){
    const auto s=MakeSetup(draft,1);
    // Over the limit: nothing is applied, the estimate is kept.
    {
        const auto r=Decide(s,ModelEstimate(s,{1.02,{0,0}},Grid()));
        CHECK(r.status==FixedScaleStatus::exceeded && r.applied_scale==1 && r.estimated_scale && Near(*r.estimated_scale,1.02,1e-7));
        CHECK(r.difference_percent && Near(*r.difference_percent,2,1e-5) && r.reason==FixedScaleReason::none);
        const auto below=Decide(s,ModelEstimate(s,{0.98,{0,0}},Grid()));
        CHECK(below.status==FixedScaleStatus::exceeded && below.applied_scale==1);
    }
    // Exactly the limit is inside; the next smaller limit is not. The limit is set to the measured
    // difference itself, so the equality is exact whatever the arithmetic of the fit.
    {
        auto condition=s;
        condition.condition.scale_auto_correction_max_percent=100;
        const auto e=ModelEstimate(s,{1.0078125,{0,0}},Grid());
        const auto wide=Decide(condition,e);
        CHECK(wide.status==FixedScaleStatus::applied && wide.difference_percent);
        const double difference=*wide.difference_percent;
        condition.condition.scale_auto_correction_max_percent=difference;
        const auto exact=Decide(condition,e);
        CHECK(exact.status==FixedScaleStatus::applied && exact.applied_scale==*exact.estimated_scale && exact.limit_percent==difference);
        condition.condition.scale_auto_correction_max_percent=std::nextafter(difference,0.0);
        const auto under=Decide(condition,e);
        CHECK(under.status==FixedScaleStatus::exceeded && under.applied_scale==1 && under.estimated_scale && *under.estimated_scale==*exact.estimated_scale);
        // A zero limit corrects only an exact 1.
        condition.condition.scale_auto_correction_max_percent=0;
        CHECK(Decide(condition,e).status==FixedScaleStatus::exceeded);
    }
}

void Undetermined(){
    const auto s=MakeSetup(draft,1);
    OverlapSimilarityEstimate undetermined;undetermined.status=OverlapEstimateStatus::undetermined;
    const std::pair<SimilarityUndeterminedReason,FixedScaleReason> mapping[]{
        {SimilarityUndeterminedReason::no_tiles,FixedScaleReason::no_tiles},{SimilarityUndeterminedReason::insufficient_tiles,FixedScaleReason::insufficient_tiles},
        {SimilarityUndeterminedReason::outside_image,FixedScaleReason::outside_image},{SimilarityUndeterminedReason::tied_starts,FixedScaleReason::tied_starts},
        {SimilarityUndeterminedReason::none,FixedScaleReason::degenerate_fit}};
    for(const auto& [from,to]:mapping){
        undetermined.reason=from;
        const auto r=Decide(s,undetermined);
        CHECK(r.status==FixedScaleStatus::not_estimated && r.reason==to && !r.estimated_scale && !r.difference_percent && r.applied_scale==1 && r.limit_percent==1 && r.used_tiles==0);
        CHECK(r.center_mm.x==FixedCompositeScaleCenterMm(s.binding.parameters.camera_b).x);
    }
    const auto grid=Grid();
    const auto good=ModelEstimate(s,{1.004,{0,0}},grid);
    const auto not_estimated=[&](const OverlapSimilarityEstimate& e,FixedScaleReason reason,unsigned used){
        const auto r=Decide(s,e);
        return r.status==FixedScaleStatus::not_estimated && r.reason==reason && r.applied_scale==1 && !r.estimated_scale && r.used_tiles==used;
    };
    // Fewer than three tiles, or one row only.
    {
        auto two=good;two.tiles.resize(2);
        CHECK(not_estimated(two,FixedScaleReason::too_few_tiles,2));
        auto one_row=good;one_row.tiles.resize(8);   // the first grid row only
        CHECK(not_estimated(one_row,FixedScaleReason::too_few_tiles,8));
        auto two_rows=good;two_rows.tiles.resize(16);
        const auto r=Decide(s,two_rows);
        CHECK(r.status==FixedScaleStatus::applied && r.used_rows==2 && r.first_used_row==0 && r.last_used_row==1);
    }
    // Tiles outside the consensus are not used.
    {
        auto outsiders=good;
        for(size_t i=0;i<outsiders.tiles.size();++i)outsiders.tiles[i].agreed=i<2;
        CHECK(not_estimated(outsiders,FixedScaleReason::too_few_tiles,2));
    }
    // A pattern one period away: every match is far outside the search window of the fixed mapping.
    {
        auto repeated=good;
        for(auto& t:repeated.tiles)t.a_x+=240;   // 30 mm along the sheet, 240 pixels
        CHECK(not_estimated(repeated,FixedScaleReason::too_few_tiles,0));
        auto mixed=good;                           // one row off by a period: the rest still fits
        for(size_t i=0;i<8;++i)mixed.tiles[i].a_x+=240;
        const auto r=Decide(s,mixed);
        CHECK(r.status==FixedScaleStatus::applied && r.used_tiles==24 && r.used_rows==3 && r.first_used_row==1 && Near(*r.estimated_scale,1.004,1e-7));
        // The window is the estimator's: the same tiles pass with a wider one.
        auto wide=repeated;wide.search_radius=400;
        CHECK(Decide(s,wide).used_tiles==32);
    }
    // Points with no document point are left out; with too many of them there is nothing to fit.
    {
        auto broken=good;
        for(size_t i=0;i<broken.tiles.size();++i)if(i%2==0)broken.tiles[i].b_x=std::numeric_limits<double>::quiet_NaN();
        const auto r=Decide(s,broken);
        CHECK(r.status==FixedScaleStatus::applied && r.used_tiles==16 && Near(*r.estimated_scale,1.004,1e-7));
        for(auto& t:broken.tiles)t.a_y=std::numeric_limits<double>::infinity();
        CHECK(not_estimated(broken,FixedScaleReason::too_few_tiles,0));
    }
    // A camera that cannot be inverted: the matrix is singular, no tile has a document point.
    {
        auto flat=s;
        flat.binding.parameters.camera_a.document_to_image={0,0,0,0,0,0,0,0,1};
        const auto r=Decide(flat,good);
        CHECK(r.status==FixedScaleStatus::not_estimated && r.reason==FixedScaleReason::too_few_tiles && r.used_tiles==0 && r.applied_scale==1);
    }
    // Every tile at one point: no lever arm.
    {
        auto same=good;
        for(auto& t:same.tiles){t.a_x=same.tiles[0].a_x;t.a_y=same.tiles[0].a_y;t.b_x=same.tiles[0].b_x;t.b_y=same.tiles[0].b_y;}
        CHECK(not_estimated(same,FixedScaleReason::degenerate_fit,32));
    }
    // A fit that would flip B: not applied, not an estimate.
    {
        auto flipped=ModelEstimate(s,{-0.5,{0,0}},grid,100000);
        CHECK(not_estimated(flipped,FixedScaleReason::degenerate_fit,32));
    }
}

void SeamAfterCorrection(const std::string& name,const std::string& profile){
    const auto s=MakeSetup(profile,1);
    struct Case {Truth truth;bool exact;};
    for(const auto& test:{Case{{1.005,{0,0}},true},Case{{0.995,{0,0}},true},Case{{1.004,{0.3,-0.2}},false},Case{{1.0,{0.4,0.1}},false}}){
        const auto r=Decide(s,ModelEstimate(s,test.truth,Grid()));
        CHECK(r.status==FixedScaleStatus::applied);
        const double before=WorstSeamDifference(s,test.truth,1.0),after=WorstSeamDifference(s,test.truth,r.applied_scale);
        std::cout<<"  "<<name<<" truth="<<test.truth.scale<<" shift="<<test.truth.shift.x<<","<<test.truth.shift.y<<" seam difference px: uncorrected="<<before<<" corrected="<<after<<'\n';
        if(test.truth.scale==1.0){
            // A shift alone: k stays 1, so the drawn composite does not change and the shift is reported.
            CHECK(Near(after,before,1e-6) && Near(r.applied_scale,1.0,1e-7));
            continue;
        }
        CHECK(after<before);
        if(test.exact)CHECK(after<1e-4);
        else{
            // The scale part is gone; what is left is the shift the scale could not explain (0.1 mm per output pixel).
            const double left_over=std::hypot(test.truth.shift.x,test.truth.shift.y)/0.1;
            CHECK(after<=left_over*1.05 && before>left_over*1.5);
        }
    }
    // The drawn mapping is the shared one: the pair of the applied scale is the pair of that scale.
    const auto r=Decide(s,ModelEstimate(s,{1.005,{0,0}},Grid()));
    const auto a=ScaledDocumentMappedFixedSeamInput(s.binding.parameters,r.applied_scale);
    const auto b=ScaledDocumentMappedFixedSeamInput(s.binding.parameters,*r.estimated_scale);
    render::Point pa{},pb{};
    CHECK(a.camera_b({7,9},pa) && b.camera_b({7,9},pb) && pa.x==pb.x && pa.y==pb.y);
    // The output raster is not planned again: width and height are the condition's for every scale.
    CHECK(a.width==s.condition.raster.width_pixels && a.height==s.condition.raster.height_pixels);
}

// An independent reading of the same tiles with the closed forms of the affine fixture
// (camera A: u = 8 Y + 100, v = 4800 - 8 X; camera B: u = 8 Y + 100, v = 9400 - 8 X) and plain
// arithmetic on the corner coordinates, on tiles that carry measurement noise. The code under test
// has to give the same k and the same left-over shift.
void IndependentFit(){
    for(const auto orientation:{MountOrientation::normal,MountOrientation::rotated_180}){
        const auto s=MakeSetup(draft,1,orientation);
        auto e=ModelEstimate(s,{1.003,{0.2,-0.1}},Grid());
        const double noise[]{0.7,-1.1,0.3,1.4,-0.6,0.9,-1.3,0.2};
        for(size_t i=0;i<e.tiles.size();++i){
            e.tiles[i].a_x+=noise[i%8];e.tiles[i].a_y+=noise[(i+3)%8];
            e.tiles[i].b_x+=noise[(i+5)%8];e.tiles[i].b_y+=noise[(i+1)%8];
        }
        const auto raw=[&](double cx,double cy){
            return orientation==MountOrientation::rotated_180?render::Point{kWidth-1-(cx-0.5),kHeight-1-(cy-0.5)}:render::Point{cx-0.5,cy-0.5};
        };
        const render::Point p{868.0625,447.4375};
        double mqx=0,mqy=0,mtx=0,mty=0;
        std::vector<std::array<double,4>> pairs;
        for(const auto& t:e.tiles){
            const auto ra=raw(t.a_x,t.a_y),rb=raw(t.b_x,t.b_y);
            pairs.push_back({(9400-rb.y)/8-p.x,(rb.x-100)/8-p.y,(4800-ra.y)/8-p.x,(ra.x-100)/8-p.y});
        }
        for(const auto& u:pairs){mqx+=u[0];mqy+=u[1];mtx+=u[2];mty+=u[3];}
        const double n=static_cast<double>(pairs.size());
        mqx/=n;mqy/=n;mtx/=n;mty/=n;
        double spread=0,cross=0;
        for(const auto& u:pairs){spread+=(u[0]-mqx)*(u[0]-mqx)+(u[1]-mqy)*(u[1]-mqy);cross+=(u[0]-mqx)*(u[2]-mtx)+(u[1]-mqy)*(u[3]-mty);}
        const double k=cross/spread;
        const auto r=Decide(s,e);
        std::cout<<"  independent fit: k="<<k<<" code="<<(r.estimated_scale?*r.estimated_scale:0.0)<<" used="<<r.used_tiles<<'\n';
        CHECK(r.used_tiles==pairs.size() && r.estimated_scale);
        CHECK(Near(*r.estimated_scale,k,1e-9));
        CHECK(Near(r.unexplained_shift_mm.x,mtx-k*mqx,1e-7) && Near(r.unexplained_shift_mm.y,mty-k*mqy,1e-7));
        CHECK(r.residual_mm.maximum>0.05); // the noise is left over, not fitted away
        CHECK(std::abs(k-1.003)>1e-6 && std::abs(k-1.003)<5e-3);
    }
}

void Center(){
    const auto s=MakeSetup(draft,1);
    const auto r=Decide(s,ModelEstimate(s,{1.003,{0,0}},Grid()));
    const auto p=FixedCompositeScaleCenterMm(s.binding.parameters.camera_b);
    CHECK(r.center_mm.x==p.x && r.center_mm.y==p.y && Near(p.x,868.0625,1e-9) && Near(p.y,447.4375,1e-9));
    // In the output raster (left 580 mm, top 100 mm, 0.1 mm pixels): the sample whose centre is P.
    CHECK(Near(r.center_output_pixels.x,(868.0625-580)/0.1-0.5,1e-9) && Near(r.center_output_pixels.y,(447.4375-100)/0.1-0.5,1e-9));
    // And it is the sample that the scaled mapping sends to the grid centre of B for every scale:
    // 1016 DPI, 4 x 4 pixels from (868.0, 447.4) mm, so P is the centre of pixel (2,1).
    const auto tiny=Raster(868000,447400,868100,447500,1016);
    FixedCompositeV2Request request;
    request.profile_file="wpd-fixed-composite-profile-test.json";request.profile_bytes=draft;request.raster=tiny;
    request.scale_auto_correction_max_percent=1;request.seam_column=2;request.seam_origin=FixedSeamOrigin::operator_set;
    const auto t=FromRequest(request,[](const FixedCompositeConditionV2&,const FixedCompositeProfileBinding&){});
    const auto tiny_result=Decide(t,ModelEstimate(t,{1.003,{0,0}},Grid()));
    CHECK(Near(tiny_result.center_output_pixels.x,2,1e-9) && Near(tiny_result.center_output_pixels.y,1,1e-9));
    for(const double k:{0.99,1.0,1.01}){
        const auto map=ScaledDocumentOutputToCamera(t.binding.parameters.raster,t.binding.parameters.camera_b,p,k);
        render::Point raw{};
        CHECK(map({tiny_result.center_output_pixels.x,tiny_result.center_output_pixels.y},raw) && Near(raw.x,3679.5,1e-6) && Near(raw.y,2455.5,1e-6));
    }
}

void Contracts(){
    const auto s=MakeSetup(draft,1);
    const auto e=ModelEstimate(s,{1.003,{0,0}},Grid());
    // Deterministic: the same inputs give the same bits.
    const auto first=Decide(s,e),second=Decide(s,e);
    CHECK(first.estimated_scale && second.estimated_scale && *first.estimated_scale==*second.estimated_scale);
    CHECK(first.unexplained_shift_mm.x==second.unexplained_shift_mm.x && first.residual_mm.rms==second.residual_mm.rms);
    // The estimate must come from frames of the size the profile fixes.
    REJECT(DecideFixedScaleV2(s.condition,s.binding,e,kWidth/2,kHeight/2));
    REJECT(DecideFixedScaleV2(s.condition,s.binding,e,kWidth,kHeight-1));
    // The binding must be the one the condition names.
    {
        auto other=s;
        other.binding.profile_fingerprint_sha256=std::string(64,'1');
        REJECT(Decide(other,e));
    }
    // An invalid record is refused (the limit has no default).
    {
        auto bad=s;
        bad.condition.scale_auto_correction_max_percent=std::numeric_limits<double>::quiet_NaN();
        REJECT(Decide(bad,e));
    }
    // Camera B without a scale centre.
    {
        auto flat=s;
        flat.binding.parameters.camera_b.document_to_image={0,0,0,0,0,0,0,0,1};
        REJECT(Decide(flat,e));
    }
    // A mount orientation that is not one of the two.
    REJECT(FixedScaleRawPixelOfTile(1,1,static_cast<MountOrientation>(7),kWidth,kHeight));
}

void LensMatters(){
    // The same tiles read with a profile that has no lens terms give a different k: the lens inverse
    // is what keeps the distortion out of the scale.
    const auto with_lens=MakeSetup(lens_draft,1),without=MakeSetup(draft,1);
    const auto e=ModelEstimate(with_lens,{1.005,{0,0}},Grid());
    const auto right=Decide(with_lens,e),wrong=Decide(without,e);
    CHECK(right.estimated_scale && Near(*right.estimated_scale,1.005,1e-7));
    CHECK(wrong.estimated_scale || wrong.status==FixedScaleStatus::not_estimated);
    std::cout<<"  lens ignored: estimated="<<(wrong.estimated_scale?*wrong.estimated_scale:0.0)<<" used="<<wrong.used_tiles<<'\n';
    CHECK(!wrong.estimated_scale || std::abs(*wrong.estimated_scale-1.005)>1e-5);
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        // Both cameras with a barrel k1, k2 and a small tangential term.
        lens_draft=ReplaceAll(draft,"\"k1\": 0.0","\"k1\": -0.05");
        lens_draft=ReplaceAll(lens_draft,"\"k2\": 0.0","\"k2\": 0.02");
        lens_draft=ReplaceAll(lens_draft,"\"p1\": 0.0","\"p1\": 0.0005");
        Frame();
        Recovery("affine cameras",draft);
        Recovery("cameras with lens terms",lens_draft);
        Limit();Undetermined();
        SeamAfterCorrection("affine",draft);
        SeamAfterCorrection("lens",lens_draft);
        Center();IndependentFit();Contracts();LensMatters();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
