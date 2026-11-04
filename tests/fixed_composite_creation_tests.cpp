// Mappings and creation checks of the fixed composite condition v2 (#405 W6):
// the document point of a raw pixel, the scale centre P, the scaled mapping of
// camera B, the coverage / seam / provisional-column checks of design 3.9 items
// 3 to 5, and MakeFixedCompositeConditionV2 with the real creation check.
// Camera free and file free. Argument 1 is the shared rig profile v2 fixture folder.
//
// The fixture draft is two affine cameras that look at the sheet with their image
// x along the document Y and their image y along the document X (8 px per mm):
// camera A covers document X in (-14, 600] mm and camera B in (561, 1175] mm, so
// they overlap by 39 mm. The checked scales act on B about its centre, document
// (868.0625, 447.4375) mm. All values here are chosen for this fixture.
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include <algorithm>
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
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,("test token absent: "+from).c_str());return text;}
    text.replace(at,from.size(),to);return text;
}
std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}

std::string draft;

// Region in micrometres at a DPI, with the pixel counts the exact ceiling gives.
render::OutputRaster Raster(long long left,long long top,long long right,long long bottom,unsigned dpi){
    const auto pixels=[&](long long um){return static_cast<std::uint32_t>((um*static_cast<long long>(dpi)+25399)/25400);};
    return render::OutputRaster{render::DocumentRegionUm{left,top,right,bottom},dpi,pixels(right-left),pixels(bottom-top)};
}
// Inside the 39 mm overlap at every scale in +-1 %: 18 mm x 8 mm, 180 x 80 pixels at 0.1 mm.
render::OutputRaster InnerRaster(){return Raster(580000,100000,598000,108000,254);}
// Starts 1 mm into the overlap: at B scale 0.99 its first 20 columns lose B.
render::OutputRaster EdgeRaster(){return Raster(562000,100000,598000,108000,254);}

render::DocumentRenderParameters Parameters(const std::string& profile,const render::OutputRaster& raster){
    return a0::m2::RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);
}

FixedCompositeV2Request Request(const std::string& profile,const render::OutputRaster& raster,double limit,std::uint32_t column,FixedSeamOrigin origin){
    FixedCompositeV2Request r;
    r.profile_file="wpd-fixed-composite-profile-test.json";r.profile_bytes=profile;
    r.raster=raster;r.scale_auto_correction_max_percent=limit;r.seam_column=column;r.seam_origin=origin;
    return r;
}
bool Creates(const std::string& profile,const render::OutputRaster& raster,double limit,std::uint32_t column,FixedSeamOrigin origin){
    try{(void)MakeFixedCompositeConditionV2(Request(profile,raster,limit,column,origin),kCreated,kHash,MakeFixedCompositeCreationCheck());return true;}
    catch(const std::invalid_argument&){return false;}
}

void Scales(){
    const auto one=FixedCompositeCheckedScales(1);
    CHECK(one.size()==3 && Near(one[0],0.99,1e-15) && one[1]==1.0 && Near(one[2],1.01,1e-15));
    const auto half=FixedCompositeCheckedScales(2.5);
    CHECK(half.size()==3 && Near(half[0],0.975,1e-15) && Near(half[2],1.025,1e-15));
    const auto none=FixedCompositeCheckedScales(0);
    CHECK(none.size()==1 && none[0]==1.0);
    REJECT(FixedCompositeCheckedScales(-0.1));
    REJECT(FixedCompositeCheckedScales(100));
    REJECT(FixedCompositeCheckedScales(250));
    REJECT(FixedCompositeCheckedScales(std::numeric_limits<double>::quiet_NaN()));
    REJECT(FixedCompositeCheckedScales(std::numeric_limits<double>::infinity()));
}

// A camera with every lens term, an off-centre principal point and a tilted matrix.
render::CameraProjection TiltedCamera(){
    return render::CameraProjection{render::CameraIntrinsics{9800,10100,3700.25,2440.75},render::LensDistortion{-0.09,0.03,-0.004,0.0006,-0.0004},
        {8.2,0.3,-1000,-0.2,8.1,4700,0.00001,-0.000008,1}};
}

void Inverse(){
    const auto camera=TiltedCamera();
    // Raw pixel -> document -> raw pixel across the frame, including the corners.
    double worst=0;int solved=0,attempts=0;
    for(double u=0;u<=7359;u+=736)for(double v=0;v<=4911;v+=491){
        render::Point document{},back{};
        ++attempts;
        if(!TryDocumentPointOfRawPixel(camera,{u,v},document))continue;
        ++solved;
        CHECK(render::TryProjectDocument(camera,document,back));
        worst=std::max({worst,std::abs(back.x-u),std::abs(back.y-v)});
    }
    CHECK(attempts==110 && solved==attempts);
    CHECK(worst<1e-6);
    // The affine fixture camera inverts in closed form: u = 8 Y + 100, v = -8 X + 4800.
    const auto fixture=Parameters(draft,InnerRaster());
    render::Point at{};
    CHECK(TryDocumentPointOfRawPixel(fixture.camera_a,{500,4000},at) && Near(at.x,100,1e-9) && Near(at.y,50,1e-9));
    // Not finite or not a camera: no point.
    CHECK(!TryDocumentPointOfRawPixel(camera,{std::numeric_limits<double>::quiet_NaN(),1},at));
    auto flat=camera;flat.document_to_image={0,0,0,0,0,0,0,0,1};
    CHECK(!TryDocumentPointOfRawPixel(flat,{100,100},at));
    auto bad_focal=camera;bad_focal.intrinsics.fx_pixels=0;
    CHECK(!TryDocumentPointOfRawPixel(bad_focal,{100,100},at));
}

void ScaleCenter(){
    const auto fixture=Parameters(draft,InnerRaster());
    // The principal point is the grid centre and the lens is flat: the closed form H^-1(c).
    const auto p=FixedCompositeScaleCenterMm(fixture.camera_b);
    CHECK(Near(p.x,868.0625,1e-9) && Near(p.y,447.4375,1e-9));
    render::Point raw{};
    CHECK(render::TryProjectDocument(fixture.camera_b,p,raw) && Near(raw.x,3679.5,1e-9) && Near(raw.y,2455.5,1e-9));
    // With the lens terms and an off-centre principal point: still the point that B puts on the grid centre.
    const auto tilted=TiltedCamera();
    const auto q=FixedCompositeScaleCenterMm(tilted);
    CHECK(render::TryProjectDocument(tilted,q,raw) && Near(raw.x,3679.5,1e-6) && Near(raw.y,2455.5,1e-6));
    // No document point: an unusable camera is an error, not a default.
    auto flat=tilted;flat.document_to_image={0,0,0,0,0,0,0,0,1};
    REJECT(FixedCompositeScaleCenterMm(flat));
}

void ScaledMapping(){
    const auto fixture=Parameters(draft,InnerRaster());
    const auto p=FixedCompositeScaleCenterMm(fixture.camera_b);
    const auto plain=render::DocumentOutputToCamera(fixture.raster,fixture.camera_b);
    const auto unit=ScaledDocumentOutputToCamera(fixture.raster,fixture.camera_b,p,1.0);
    bool identical=true;
    for(unsigned y=0;y<fixture.raster.height_pixels;y+=9)for(unsigned x=0;x<fixture.raster.width_pixels;x+=7){
        render::Point a{},b{};
        plain({double(x),double(y)},a);unit({double(x),double(y)},b);
        identical=identical && a.x==b.x && a.y==b.y;
    }
    CHECK(identical); // scale 1 is the document render's mapping, bit for bit
    // k = 1.01: the document point is scaled about P by 1/k before the projection.
    const auto scaled=ScaledDocumentOutputToCamera(fixture.raster,fixture.camera_b,p,1.01);
    render::Point got{},want{};
    const double pitch=25.4/254;
    const render::Point sample{580.0+(30+.5)*pitch,100.0+(10+.5)*pitch};
    CHECK(scaled({30,10},got));
    CHECK(render::TryProjectDocument(fixture.camera_b,{p.x+(sample.x-p.x)/1.01,p.y+(sample.y-p.y)/1.01},want));
    CHECK(Near(got.x,want.x,1e-9) && Near(got.y,want.y,1e-9));
    render::Point unscaled_raw{};plain({30,10},unscaled_raw);
    CHECK(std::abs(got.x-unscaled_raw.x)>1 || std::abs(got.y-unscaled_raw.y)>1);
    // P is fixed: the sample that lands on P maps to the grid centre whatever k is.
    // 0.1 mm at 1016 DPI is 4 x 4 pixels of 0.025 mm; pixel (2,1) is centred on (868.0625, 447.4375).
    const auto tiny=Raster(868000,447400,868100,447500,1016);
    CHECK(tiny.width_pixels==4 && tiny.height_pixels==4);
    const auto tiny_parameters=Parameters(draft,tiny);
    for(const double k:{0.9,0.99,1.0,1.01,1.2}){
        const auto map=ScaledDocumentOutputToCamera(tiny_parameters.raster,tiny_parameters.camera_b,p,k);
        render::Point centre{};
        CHECK(map({2,1},centre) && Near(centre.x,3679.5,1e-6) && Near(centre.y,2455.5,1e-6));
    }
    // Invalid scales and centres are refused, and the camera is not changed.
    for(const double bad:{0.0,-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        REJECT(ScaledDocumentOutputToCamera(fixture.raster,fixture.camera_b,p,bad));
    REJECT(ScaledDocumentOutputToCamera(fixture.raster,fixture.camera_b,{std::numeric_limits<double>::quiet_NaN(),0},1.01));

    // The pair: A is untouched, B follows the scaled mapping, k = 1 is the document pair.
    const auto doc=DocumentMappedFixedSeamInput(fixture);
    const auto at_one=ScaledDocumentMappedFixedSeamInput(fixture,1.0);
    const auto at_up=ScaledDocumentMappedFixedSeamInput(fixture,1.01);
    render::Point a0{},a1{},a2{},b0{},b1{},b2{};
    doc.camera_a({3,4},a0);at_one.camera_a({3,4},a1);at_up.camera_a({3,4},a2);
    doc.camera_b({3,4},b0);at_one.camera_b({3,4},b1);at_up.camera_b({3,4},b2);
    CHECK(a0.x==a1.x && a0.y==a1.y && a0.x==a2.x && a0.y==a2.y);
    CHECK(b0.x==b1.x && b0.y==b1.y && (b2.x!=b0.x || b2.y!=b0.y));
    CHECK(at_up.width==fixture.raster.width_pixels && at_up.height==fixture.raster.height_pixels);
    REJECT(ScaledDocumentMappedFixedSeamInput(fixture,0));
}

void Bands(){
    const auto rows=[](std::vector<std::pair<unsigned,unsigned>> v){
        std::vector<MappedOverlapRow> r;for(const auto& p:v)r.push_back({p.first,p.second});return r;
    };
    const auto band=FixedCompositeBandOfRows(rows({{2,10},{3,9},{1,12}}));
    CHECK(band.first==3 && band.last_exclusive==9);
    CHECK(FixedCompositeBandCenter(band)==6);
    CHECK(FixedCompositeBandCenter({3,10})==6);   // rounded down: 13/2
    CHECK(FixedCompositeBandCenter({4,10})==7);   // an even width takes the right-hand middle column
    CHECK(FixedCompositeBandCenter({0,1})==0);
    REJECT(FixedCompositeBandOfRows({}));
    REJECT(FixedCompositeBandOfRows(rows({{0,4},{4,8}})));  // touching runs share no column
    REJECT(FixedCompositeBandOfRows(rows({{0,4},{5,8}})));
    REJECT(FixedCompositeBandCenter({5,5}));
    // From the fixture: the whole inner raster is covered by both on every row, at scale 1.
    const auto inner=FixedCompositeBandAtUnitScale(Parameters(draft,InnerRaster()));
    CHECK(inner.first==0 && inner.last_exclusive==180);
    CHECK(FixedCompositeProvisionalSeamColumn(Parameters(draft,InnerRaster()))==90);
    // A raster that starts left of camera B's left edge (561 mm): the band begins where B begins.
    const auto wide=Parameters(draft,Raster(556000,100000,598000,108000,254));
    const auto wide_band=FixedCompositeBandAtUnitScale(wide);
    CHECK(wide_band.first==50 && wide_band.last_exclusive==420); // (561-556) mm at 10 columns per mm
    CHECK(FixedCompositeProvisionalSeamColumn(wide)==235);
    // Where A does not reach, there is no band.
    REJECT(FixedCompositeBandAtUnitScale(Parameters(draft,Raster(700000,100000,710000,108000,254))));
}

void PixelsPerMm(){
    const auto fixture=Parameters(draft,InnerRaster());
    CHECK(Near(FixedCompositeCapturePixelsPerMm(fixture.camera_a,{590,104}),8.0,1e-9));
    CHECK(Near(FixedCompositeCapturePixelsPerMm(fixture.camera_b,{590,104}),8.0,1e-9));
    // Lens terms change the local density (a barrel lens crowds pixels towards the corners).
    const auto lens=Parameters(ReplaceAll(draft,"\"k1\": 0.0","\"k1\": -0.05"),InnerRaster());
    CHECK(!Near(FixedCompositeCapturePixelsPerMm(lens.camera_a,{590,104}),8.0,1e-3));
    REJECT(FixedCompositeCapturePixelsPerMm(fixture.camera_a,{std::numeric_limits<double>::quiet_NaN(),0}));
}

void Coverage(){
    const auto inner=Parameters(draft,InnerRaster());
    for(const double k:{0.99,1.0,1.01}){
        const auto input=ScaledDocumentMappedFixedSeamInput(inner,k);
        CHECK(!Throws([&]{RequireFixedCompositeCoverage(input);}));
        CHECK(!Throws([&]{RequireFixedCompositeSeam(input,90);}));
    }
    // Camera A ends at 600 mm. B starts at 564.07 mm at scale 0.99 but at 622.4 mm at scale 0.8:
    // 590..640 mm is covered at 0.99 and 1, and leaves a gap of A and B at 0.8.
    const auto reach=Parameters(draft,Raster(590000,100000,640000,108000,254));
    for(const double k:{0.99,1.0,1.01})CHECK(!Throws([&]{RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(reach,k));}));
    REJECT(RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(reach,0.8)));
    try{RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(reach,0.8));}
    catch(const std::invalid_argument& e){CHECK(std::string(e.what()).find("neither camera")!=std::string::npos);}
    // A is not the left camera where column 0 is outside A.
    const auto beyond=Parameters(draft,Raster(1000000,100000,1010000,108000,254));
    try{RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(beyond,1.0));Check(false,"B-only raster accepted");}
    catch(const std::invalid_argument& e){CHECK(std::string(e.what()).find("first column")!=std::string::npos);}
    // A alone covering every row is covered (no overlap is the seam check's business).
    const auto only_a=Parameters(draft,Raster(100000,100000,110000,108000,254));
    CHECK(!Throws([&]{RequireFixedCompositeCoverage(ScaledDocumentMappedFixedSeamInput(only_a,1.0));}));
    REJECT(RequireFixedCompositeSeam(ScaledDocumentMappedFixedSeamInput(only_a,1.0),50));
    // Missing mappings or an empty raster.
    REJECT(RequireFixedCompositeCoverage(MappedFixedSeamInput{}));
}

void Seam(){
    const auto edge=Parameters(draft,EdgeRaster()); // 360 x 80, B covers every column at scale 1
    const auto at_one=ScaledDocumentMappedFixedSeamInput(edge,1.0);
    const auto at_low=ScaledDocumentMappedFixedSeamInput(edge,0.99);
    // Half the blend width is 3 columns (kSeamBlendDesign.width_pixels is 8).
    CHECK(!Throws([&]{RequireFixedCompositeSeam(at_one,3);}));
    CHECK(Throws([&]{RequireFixedCompositeSeam(at_one,2);}));
    CHECK(!Throws([&]{RequireFixedCompositeSeam(at_one,356);}));
    CHECK(Throws([&]{RequireFixedCompositeSeam(at_one,357);}));
    // At scale 0.99 camera B loses its first 21 columns: column 10 fits at 1 and not at 0.99.
    CHECK(!Throws([&]{RequireFixedCompositeSeam(at_one,10);}));
    CHECK(Throws([&]{RequireFixedCompositeSeam(at_low,10);}));
    CHECK(!Throws([&]{RequireFixedCompositeSeam(at_low,30);}));
    CHECK(!Throws([&]{RequireFixedCompositeSeam(at_low,180);}));
}

void Creation(){
    const auto inner=InnerRaster();
    // The middle of the band is provisional and passes every check.
    CHECK(Creates(draft,inner,1,90,FixedSeamOrigin::band_center_provisional));
    const auto made=MakeFixedCompositeConditionV2(Request(draft,inner,1,90,FixedSeamOrigin::band_center_provisional),kCreated,kHash,MakeFixedCompositeCreationCheck());
    CHECK(made.seam.column==90 && made.seam.origin==FixedSeamOrigin::band_center_provisional && made.scale_auto_correction_max_percent==1);
    // Item 5: a provisional column that is not the middle.
    CHECK(!Creates(draft,inner,1,91,FixedSeamOrigin::band_center_provisional));
    CHECK(!Creates(draft,inner,1,89,FixedSeamOrigin::band_center_provisional));
    CHECK(Creates(draft,inner,1,91,FixedSeamOrigin::operator_set));   // an operator column is not held to the middle
    CHECK(Creates(draft,inner,1,89,FixedSeamOrigin::operator_set));
    // Item 4: a column at the edge of the band, whatever its origin.
    CHECK(!Creates(draft,inner,1,0,FixedSeamOrigin::operator_set));
    CHECK(!Creates(draft,inner,1,2,FixedSeamOrigin::operator_set));
    CHECK(Creates(draft,inner,1,3,FixedSeamOrigin::operator_set));
    CHECK(!Creates(draft,inner,1,179,FixedSeamOrigin::operator_set));
    CHECK(Creates(draft,inner,1,176,FixedSeamOrigin::operator_set));
    // Item 4 follows the allowed range: column 10 of EdgeRaster fits at scale 1 and not at 0.99.
    const auto edge=EdgeRaster();
    CHECK(Creates(draft,edge,0,10,FixedSeamOrigin::operator_set));    // no correction allowed: only scale 1 is checked
    CHECK(!Creates(draft,edge,1,10,FixedSeamOrigin::operator_set));
    CHECK(Creates(draft,edge,1,30,FixedSeamOrigin::operator_set));
    CHECK(Creates(draft,edge,1,180,FixedSeamOrigin::band_center_provisional)); // band at scale 1 is [0,360)
    CHECK(!Creates(draft,edge,1,181,FixedSeamOrigin::band_center_provisional));
    // Item 3: a limit so large that B stops covering, while the same raster is fine at 1 %.
    const auto reach=Raster(590000,100000,640000,108000,254);
    {
        // Seam inside the overlap of 590..600 mm (columns 0..99): column 50.
        CHECK(Creates(draft,reach,1,50,FixedSeamOrigin::operator_set));
        CHECK(!Creates(draft,reach,20,50,FixedSeamOrigin::operator_set));
    }
    // The limit has to be a scale: 100 % and more cannot be checked.
    CHECK(!Creates(draft,inner,100,90,FixedSeamOrigin::band_center_provisional));
    // A raster that no band exists for.
    CHECK(!Creates(draft,Raster(700000,100000,710000,108000,254),1,50,FixedSeamOrigin::operator_set));
    // The error names the scale that failed.
    try{(void)MakeFixedCompositeConditionV2(Request(draft,edge,1,10,FixedSeamOrigin::operator_set),kCreated,kHash,MakeFixedCompositeCreationCheck());Check(false,"edge seam accepted");}
    catch(const std::invalid_argument& e){CHECK(std::string(e.what()).find("scale 0.99")!=std::string::npos);}
}

void WithLens(){
    // Both cameras with lens terms (a barrel k1, k2 and a small tangential term): the
    // checks run through the lens, and the record is still made.
    auto lens=ReplaceAll(draft,"\"k1\": 0.0","\"k1\": -0.05");
    lens=ReplaceAll(lens,"\"k2\": 0.0","\"k2\": 0.02");
    lens=ReplaceAll(lens,"\"p1\": 0.0","\"p1\": 0.0005");
    const auto inner=InnerRaster();
    const auto parameters=Parameters(lens,inner);
    CHECK(parameters.camera_b.distortion.k1==-0.05 && parameters.camera_b.distortion.p1==0.0005);
    // The scale centre is the principal point's document point: unchanged by the lens.
    const auto p=FixedCompositeScaleCenterMm(parameters.camera_b);
    CHECK(Near(p.x,868.0625,1e-6) && Near(p.y,447.4375,1e-6));
    const auto band=FixedCompositeBandAtUnitScale(parameters);
    CHECK(band.first<band.last_exclusive && band.last_exclusive<=180);
    CHECK(Creates(lens,inner,1,FixedCompositeBandCenter(band),FixedSeamOrigin::band_center_provisional));
    CHECK(!Creates(lens,inner,1,FixedCompositeBandCenter(band)+1,FixedSeamOrigin::band_center_provisional));
    // The lens moves camera B's left edge (561 mm without a lens): the band of a raster that starts left of it moves too.
    const auto wide=Raster(556000,100000,598000,108000,254);
    const auto edge_band=FixedCompositeBandAtUnitScale(Parameters(lens,wide));
    const auto flat_band=FixedCompositeBandAtUnitScale(Parameters(draft,wide));
    CHECK(edge_band.first<flat_band.first && edge_band.last_exclusive==flat_band.last_exclusive); // a barrel lens widens B's footprint
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        Scales();Inverse();ScaleCenter();ScaledMapping();Bands();PixelsPerMm();Coverage();Seam();Creation();WithLens();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
