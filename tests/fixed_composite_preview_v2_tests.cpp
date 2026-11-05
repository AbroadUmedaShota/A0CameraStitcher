// Live C drawn with the lens-aware fixed composite condition v2 (#405 W5): the
// output-to-live-view mapping (lens, S, mount orientation), the coordinate table,
// the grid and seam of C against the full-size mapping, the choice between the
// uncalibrated C, the v1 C, the v2 C and no C, and the standing texts. The v1 C is
// pinned by SHA-256 so that this change leaves its output as it was.
// Camera and display free; files only in a temporary folder.
// Argument 1 is the shared rig profile v2 fixture folder. Argument 2 "--measure"
// prints the time of one frame (for the person who decides about the table).
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v2.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
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
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FixedCompositeBytesSha256(bytes);};
std::string HashOfImage(const render::BgrImage& image){
    std::vector<unsigned char> bytes(image.bgr.begin(),image.bgr.end());
    return FixedCompositeBytesSha256(bytes);
}

std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
fs::path TempParent(){
#ifdef _MSC_VER
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
#endif
    return fs::temp_directory_path();
}
std::string ReplaceAll(std::string text,const std::string& from,const std::string& to){
    for(size_t at=0;(at=text.find(from,at))!=std::string::npos;at+=to.size())text.replace(at,from.size(),to);
    return text;
}

// ---- v1 pin ----
FixedCompositeCondition MakeV1Condition(int tx,int ty,unsigned w,unsigned h){
    FixedCompositeCondition c;
    c.created_utc="2026-11-04T12:00:00Z";
    c.source.review_report_sha256=std::string(64,'a');c.source.pair_id="pair-1";
    c.source.method=FixedCompositeMethod::manual_still_pixels;
    c.orientation=MountOrientation::normal;
    c.source_width=w;c.source_height=h;
    c.mapping.translation_x=tx;c.mapping.translation_y=ty;c.mapping.rotation_degrees=0;c.mapping.scale=1;
    const auto p=SimilarityReviewGeometry(tx,ty,0,1,w,h);
    c.mapping.crop=p.crop;
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    c.output_width=plan.output_width;c.output_height=plan.output_height;
    c.scale_auto_correction_max_percent=1;
    c.seam={ProvisionalSeamColumn(p,w,h),FixedSeamOrigin::band_center_provisional};
    ValidateFixedCompositeCondition(c);
    return c;
}
// Integer-only frames so that the pinned bytes do not depend on a library function.
render::BgrImage Textured(unsigned w,unsigned h,unsigned seed){
    render::BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned base=60+(x*3+y*2+seed*17)%90;
        const bool bar=(x/9+y/13+seed)%5==0;
        for(unsigned c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(std::min(255u,base+(bar?70u:0u)+c*5));
    }
    return image;
}

// ---- fixtures ----
std::string draft,lens;
fs::path root;

constexpr const char* kPolicy="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":1}";
constexpr const char* kRotated="{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"rotated-180\"}";
// 80 mm x 40 mm of the sheet, 800 x 400 pixels at 254 DPI. The fixture cameras overlap from 561 mm to 600 mm
// along the sheet X (a lens moves both edges by a little).
constexpr std::array<long long,4> kRegion{540000,100000,620000,140000};
constexpr double kRegionLeftMm=540,kJpegPitchMm=0.1;

struct Made {fs::path folder;bool made{};std::string message;};
Made MakeFolder(const std::string& name,const std::string& profile,const char* mount_file){
    Made m;m.folder=root/name;fs::create_directories(m.folder);
    WriteAll(m.folder/kMergePolicySettingsName,kPolicy);
    if(mount_file)WriteAll(m.folder/kMountOrientationSettingsName,mount_file);
    const auto source=root/(name+"-profile-source.json");WriteAll(source,profile);
    FixedConditionV2Request r;r.profile=source;r.settings_folder=m.folder;r.region_um=kRegion;r.dpi=254;
    const auto result=MakeFixedCompositeConditionV2File(r,kHash,kNow);
    m.made=result.made;m.message=result.message;
    return m;
}
fs::path ConditionOf(const fs::path& folder){return folder/kFixedCompositeConditionName;}
FixedPreviewSource Load(const fs::path& folder){return LoadFixedPreviewSource(ConditionOf(folder),kHash);}
fs::path CopyOf(const fs::path& folder,const std::string& name){
    const auto copy=root/name;fs::remove_all(copy);fs::copy(folder,copy,fs::copy_options::recursive);return copy;
}

// ---- a scene on the sheet, seen by both the full-size capture and the live view ----
// Bars of a distinct level at known sheet X, painted through the cameras of the profile: a document
// sample is projected to its raw pixel and that pixel (scaled to the live view where needed) is set.
constexpr std::uint8_t kBase=100,kBarLevel=250;
constexpr unsigned kThreshold=175;
struct Bar {double x0,x1;};
constexpr Bar kBarA{548,553};            // camera A only
constexpr std::array<Bar,2> kBarsB{Bar{587,592},Bar{608,616}}; // inside the overlap, right of the seam; beyond camera A
render::BgrImage Flat(unsigned w,unsigned h,std::uint8_t v){return render::BgrImage{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3,v)};}
void PaintBar(render::BgrImage& image,const render::CameraProjection& camera,double su,double sv,const Bar& bar,double step){
    for(double x=bar.x0;x<=bar.x1;x+=step)for(double y=96;y<=144;y+=step){
        render::Point raw{};
        if(!render::TryProjectDocument(camera,{x,y},raw))continue;
        const double u=std::floor(raw.x*su+.5),v=std::floor(raw.y*sv+.5);
        if(u<0 || v<0 || u>=image.width || v>=image.height)continue;
        std::fill_n(image.bgr.begin()+static_cast<std::ptrdiff_t>((static_cast<size_t>(v)*image.width+static_cast<size_t>(u))*3),3,kBarLevel);
    }
}
struct Captures {render::BgrImage a,b;};
Captures FullSize(const render::DocumentRenderParameters& p){
    Captures c{Flat(7360,4912,kBase),Flat(7360,4912,kBase)};
    PaintBar(c.a,p.camera_a,1,1,kBarA,0.025);
    for(const auto& bar:kBarsB)PaintBar(c.b,p.camera_b,1,1,bar,0.025);
    return c;
}
// The live view of the same scene under S: stored order, then turned like the preview turns it.
Captures LiveView(const render::DocumentRenderParameters& p,unsigned w,unsigned h,MountOrientation orientation){
    const double sx=w/7360.0,sy=h/4912.0;
    Captures c{Flat(w,h,kBase),Flat(w,h,kBase)};
    PaintBar(c.a,p.camera_a,sx,sy,kBarA,0.05);
    for(const auto& bar:kBarsB)PaintBar(c.b,p.camera_b,sx,sy,bar,0.05);
    OrientRenderInput(c.a,orientation);OrientRenderInput(c.b,orientation);
    return c;
}
double CentroidMm(const render::BgrImage& image,double left_mm,double pitch_mm,double lo_mm,double hi_mm){
    double sum=0;std::uint64_t n=0;
    for(unsigned x=0;x<image.width;++x){
        const double mm=left_mm+(x+.5)*pitch_mm;
        if(mm<lo_mm || mm>hi_mm)continue;
        for(unsigned y=0;y<image.height;++y)if(image.bgr[(static_cast<size_t>(y)*image.width+x)*3]>kThreshold){sum+=mm;++n;}
    }
    return n?sum/double(n):std::numeric_limits<double>::quiet_NaN();
}
struct Where {double a{},b1{},b2{};};
Where Centroids(const render::BgrImage& image,double left_mm,double pitch_mm){
    return {CentroidMm(image,left_mm,pitch_mm,kBarA.x0-2.5,kBarA.x1+2.5),CentroidMm(image,left_mm,pitch_mm,kBarsB[0].x0-2.5,kBarsB[0].x1+2.5),
        CentroidMm(image,left_mm,pitch_mm,kBarsB[1].x0-2.5,kBarsB[1].x1+2.5)};
}

// ---- tests ----
void V1IsPinned(){
    const auto condition=MakeV1Condition(930,9,1840,1228);
    const auto plan=PlanFixedPreview(condition,MountOrientation::normal,320,214);
    CHECK(!plan.skip && !plan.lens_aware && !plan.mapped);
    const auto drawn=RenderFixedPreview(plan,Textured(320,214,1),Textured(320,214,2));
    // The bytes of the v1 C before the lens-aware C was added (Debug and Release agree).
    CHECK(plan.output_width==481 && plan.output_height==212 && plan.view.seam_column==241);
    CHECK(HashOfImage(drawn.image)=="fbe92c281b6686e4b5ba50e4dd6054d4c7096a96d16cb27674376abeb1441d47");
}

void Grid(){
    const render::OutputRaster raster{render::DocumentRegionUm{540000,100000,620000,140000},254,800,400};
    const double s=640.0/7360;
    const auto grid=MakeFixedPreviewGrid(raster,s);
    CHECK(Near(grid.pitch_mm,0.1/s,1e-12) && grid.left_mm==540 && grid.top_mm==100);
    CHECK(grid.width==69 && grid.height==34); // floor(800 s), floor(400 s)
    // Every sample centre lies inside the region.
    // (a whole cell of the pitch fits for every sample, and one more would not).
    CHECK(grid.left_mm+grid.width*grid.pitch_mm<=620 && grid.top_mm+grid.height*grid.pitch_mm<=140);
    CHECK(grid.left_mm+(grid.width+1)*grid.pitch_mm>620 && grid.top_mm+(grid.height+1)*grid.pitch_mm>140);
    // A region that is an exact multiple of the pitch keeps all of its samples.
    const render::OutputRaster exact{render::DocumentRegionUm{0,0,10000,5000},254,100,50};
    const auto whole=MakeFixedPreviewGrid(exact,0.5);
    CHECK(whole.width==50 && whole.height==25);
    REJECT(MakeFixedPreviewGrid(raster,0));
    REJECT(MakeFixedPreviewGrid(raster,-1));
    REJECT(MakeFixedPreviewGrid(raster,std::numeric_limits<double>::quiet_NaN()));
    REJECT(MakeFixedPreviewGrid(raster,std::numeric_limits<double>::infinity()));
    // A region smaller than one sample, and one that the declared pixel counts do not describe.
    REJECT(MakeFixedPreviewGrid(render::OutputRaster{render::DocumentRegionUm{540000,100000,541000,140000},254,10,400},s));
    REJECT(MakeFixedPreviewGrid(render::OutputRaster{render::DocumentRegionUm{540000,100000,620000,140000},254,799,400},s));
    // More samples than the table may hold, and a large region that still fits.
    REJECT(MakeFixedPreviewGrid(render::OutputRaster{render::DocumentRegionUm{0,0,3000000,3000000},254,30000,30000},s));
    CHECK(!Throws([&]{(void)MakeFixedPreviewGrid(render::OutputRaster{render::DocumentRegionUm{0,0,1000000,1000000},254,10000,10000},s);}));
}

void Mapping(const FixedPreviewSource& normal,const FixedPreviewSource& rotated){
    const auto& p=normal.loaded_v2->profile.parameters;
    for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{400,267},{640,400}}){
        const auto n=ChoosePreviewC(normal,MountOrientation::normal,view.first,view.second);
        const auto t=ChoosePreviewC(rotated,MountOrientation::rotated_180,view.first,view.second);
        CHECK(n.mode==PreviewCMode::fixed_composite && t.mode==PreviewCMode::fixed_composite);
        if(n.mode!=PreviewCMode::fixed_composite || t.mode!=PreviewCMode::fixed_composite)continue;
        const double sx=view.first/7360.0,sy=view.second/4912.0;
        CHECK(n.plan.lens_aware && n.plan.mapped && n.plan.output_width==n.plan.grid.width && n.plan.output_height==n.plan.grid.height);
        CHECK(n.plan.output_width==t.plan.output_width && n.plan.output_height==t.plan.output_height);
        CHECK(Near(n.plan.scale_x,sx,1e-15) && Near(n.plan.scale_y,sy,1e-15));
        // The table holds what the direct mapping returns, bit for bit; the turned table is the flip of the plain one.
        const auto direct=LiveViewMappedInput(p,n.plan.grid,sx,sy,view.first,view.second,false);
        const auto direct_turned=LiveViewMappedInput(p,t.plan.grid,sx,sy,view.first,view.second,true);
        bool same=true,flipped=true,independent=true;
        for(std::uint32_t y=0;y<n.plan.output_height;++y)for(std::uint32_t x=0;x<n.plan.output_width;++x){
            const render::Point at{double(x),double(y)};
            render::Point da{},db{},ta{},tb{},na{},nb{},ra{},rb{};
            same=same && direct.camera_a(at,da) && direct.camera_b(at,db) && n.plan.mapped->camera_a(at,na) && n.plan.mapped->camera_b(at,nb);
            same=same && da.x==na.x && da.y==na.y && db.x==nb.x && db.y==nb.y;
            flipped=flipped && direct_turned.camera_a(at,ta) && direct_turned.camera_b(at,tb) && t.plan.mapped->camera_a(at,ra) && t.plan.mapped->camera_b(at,rb);
            flipped=flipped && ta.x==ra.x && ta.y==ra.y && tb.x==rb.x && tb.y==rb.y;
            flipped=flipped && ta.x==(view.first-1.0)-da.x && ta.y==(view.second-1.0)-da.y && tb.x==(view.first-1.0)-db.x && tb.y==(view.second-1.0)-db.y;
            // The formula of the design, written out again: document point, lens, S.
            render::Point raw{};
            const render::Point document{n.plan.grid.left_mm+(x+.5)*n.plan.grid.pitch_mm,n.plan.grid.top_mm+(y+.5)*n.plan.grid.pitch_mm};
            independent=independent && render::TryProjectDocument(p.camera_b,document,raw) && Near(db.x,raw.x*sx,1e-9) && Near(db.y,raw.y*sy,1e-9);
        }
        CHECK(same);CHECK(flipped);CHECK(independent);
        // Outside the grid there is no sample.
        render::Point none{};
        CHECK(!n.plan.mapped->camera_a({-1,0},none) && !n.plan.mapped->camera_a({double(n.plan.output_width),0},none) && !n.plan.mapped->camera_b({0,double(n.plan.output_height)},none));
    }
    // A mapping that cannot be represented stops the bake; one that is absent is kept as no sample.
    const auto grid=MakeFixedPreviewGrid(render::OutputRaster{render::DocumentRegionUm{540000,100000,620000,140000},254,800,400},0.087);
    REJECT(LiveViewOutputToCamera(p.camera_a,grid,0,1,640,427,false));
    REJECT(LiveViewOutputToCamera(p.camera_a,grid,1,1,0,427,false));
    auto flat=p.camera_a;flat.document_to_image={0,0,0,0,0,0,0,0,0};
    REJECT(BakeOutputToCamera(LiveViewOutputToCamera(flat,grid,0.087,0.087,640,427,false),grid.width,grid.height));
    const auto none=BakeOutputToCamera([](render::Point,render::Point&){return false;},3,2);
    render::Point out{};
    CHECK(!TableOutputToCamera(none,3,2)({1,1},out));
    REJECT(BakeOutputToCamera([](render::Point,render::Point& r){r={std::nan(""),0};return true;},3,2));
    REJECT(BakeOutputToCamera(nullptr,3,2));
    REJECT(TableOutputToCamera(none,4,2));
}

struct JpegSide {Where where;double seam_mm{};};
JpegSide FullSizeJpeg(const LoadedFixedCompositeConditionV2& loaded,const Captures& full){
    const auto jpeg=RenderDocumentFixedSeam(full.a,full.b,loaded.profile.parameters,loaded.condition.seam.column,kSeamBlendDesign);
    CHECK(jpeg.image.width==loaded.condition.raster.width_pixels && jpeg.image.height==loaded.condition.raster.height_pixels);
    return {Centroids(jpeg.image,kRegionLeftMm,kJpegPitchMm),kRegionLeftMm+(loaded.condition.seam.column+.5)*kJpegPitchMm};
}

// C against the full-size JPEG of the same condition, at scale 1 (nothing is corrected): the seam and the
// places of A and of B agree within one pixel of C. Compared in sheet millimetres, so the size of the view does not matter.
void Geometry(const char* label,const FixedPreviewSource& source,MountOrientation orientation,const JpegSide& jpeg,unsigned w,unsigned h,bool print){
    const auto choice=ChoosePreviewC(source,orientation,w,h);
    Check(choice.mode==PreviewCMode::fixed_composite,label);
    if(choice.mode!=PreviewCMode::fixed_composite){std::cerr<<"  "<<label<<" "<<w<<"x"<<h<<": "<<choice.plan.detail<<'\n';return;}
    const auto& plan=choice.plan;
    const auto live=LiveView(source.loaded_v2->profile.parameters,w,h,orientation);
    const auto drawn=RenderFixedPreview(plan,live.a,live.b);
    CHECK(drawn.image.width==plan.output_width && drawn.image.height==plan.output_height);
    CHECK(drawn.record.fixed_seam && drawn.record.seam_x.size()==plan.output_height);
    CHECK(std::all_of(drawn.record.seam_x.begin(),drawn.record.seam_x.end(),[&](std::uint32_t x){return x==plan.view.seam_column;}));
    const double pitch=plan.grid.pitch_mm;
    const double seam_mm=plan.grid.left_mm+(plan.view.seam_column+.5)*pitch;
    const auto where=Centroids(drawn.image,plan.grid.left_mm,pitch);
    if(print)std::cout<<label<<' '<<w<<'x'<<h<<": grid "<<plan.output_width<<'x'<<plan.output_height<<" pitch "<<pitch<<" mm, seam shift "<<plan.seam_shift
        <<", seam diff "<<seam_mm-jpeg.seam_mm<<" mm, bar diff A "<<where.a-jpeg.where.a<<" B1 "<<where.b1-jpeg.where.b1<<" B2 "<<where.b2-jpeg.where.b2<<" mm\n";
    // One pixel of C is the tolerance of everything below; the seam also carries the shift of the fit.
    CHECK(std::abs(plan.seam_shift)<=static_cast<int>(kFixedPreviewSeamFitPixels));
    CHECK(Near(seam_mm,jpeg.seam_mm,(std::abs(plan.seam_shift)+0.5)*pitch+1e-9));
    CHECK(!std::isnan(where.a) && !std::isnan(where.b1) && !std::isnan(where.b2));
    CHECK(Near(where.a,jpeg.where.a,pitch) && Near(where.b1,jpeg.where.b1,pitch) && Near(where.b2,jpeg.where.b2,pitch));
    // The bars are on the sides of the seam they were meant for.
    CHECK(jpeg.where.a<jpeg.seam_mm && jpeg.where.b1>jpeg.seam_mm+1 && jpeg.where.b2>jpeg.where.b1);
    const double s=w/7360.0;
    CHECK(plan.settings.width_pixels==ScaleSeamBlendSettings(kSeamBlendDesign,s,s).width_pixels);
    CHECK(plan.settings.brightness_half_width_pixels==ScaleSeamBlendSettings(kSeamBlendDesign,s,s).brightness_half_width_pixels);
    CHECK(plan.settings.brightness_smoothing_rows==ScaleSeamBlendSettings(kSeamBlendDesign,s,s).brightness_smoothing_rows);
}

std::uint64_t Differences(const render::BgrImage& a,const render::BgrImage& b,int tolerance,std::uint64_t& total){
    std::uint64_t n=0;total=a.bgr.size();
    for(size_t i=0;i<a.bgr.size();++i)if(std::abs(int(a.bgr[i])-int(b.bgr[i]))>tolerance)++n;
    return n;
}

void Turned(const FixedPreviewSource& normal,const FixedPreviewSource& rotated){
    // The same scene on a rig mounted turned: the stored live frames are turned in memory, the mapping
    // flips its coordinates, and C shows what it shows for the normal mounting (design 3.7).
    for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{400,267}}){
        const auto n=ChoosePreviewC(normal,MountOrientation::normal,view.first,view.second);
        const auto t=ChoosePreviewC(rotated,MountOrientation::rotated_180,view.first,view.second);
        CHECK(n.mode==PreviewCMode::fixed_composite && t.mode==PreviewCMode::fixed_composite);
        if(n.mode!=PreviewCMode::fixed_composite || t.mode!=PreviewCMode::fixed_composite)continue;
        const auto stored=LiveView(normal.loaded_v2->profile.parameters,view.first,view.second,MountOrientation::normal);
        const auto turned=LiveView(rotated.loaded_v2->profile.parameters,view.first,view.second,MountOrientation::rotated_180);
        auto check=stored;Rotate180(check.a);Rotate180(check.b);
        CHECK(check.a.bgr==turned.a.bgr && check.b.bgr==turned.b.bgr);
        const auto from_stored=RenderFixedPreview(n.plan,stored.a,stored.b);
        const auto from_turned=RenderFixedPreview(t.plan,turned.a,turned.b);
        CHECK(from_stored.image.width==from_turned.image.width && from_stored.image.height==from_turned.image.height);
        std::uint64_t total=0;
        const auto differing=Differences(from_stored.image,from_turned.image,1,total);
        // Only the one-pixel rim that the turned frame covers differently may differ; none of the interior.
        CHECK(differing*200<=total);
        // The standing texts name the turn.
        CHECK(FixedPreviewTitle(MountOrientation::rotated_180,t.plan).find(L"180°回転")!=std::wstring::npos);
    }
}

void States(const fs::path& normal_folder,const fs::path& rotated_folder){
    // No condition: the uncalibrated C stays.
    {
        const auto empty=root/"empty-state";fs::create_directories(empty);
        const auto source=Load(empty);
        CHECK(!source.loaded && !source.loaded_v2 && !source.Present() && source.reason==FixedCompositeSkip::condition_absent);
        const auto choice=ChoosePreviewC(source,MountOrientation::normal,320,214);
        CHECK(choice.mode==PreviewCMode::uncalibrated && !choice.plan.skip && !choice.plan.lens_aware);
    }
    const auto source=Load(normal_folder);
    CHECK(source.loaded_v2 && !source.loaded && source.Present());
    if(!source.loaded_v2)return;
    CHECK(source.loaded_v2->condition.orientation==MountOrientation::normal && !source.loaded_v2->file_sha256.empty());
    const auto rotated=Load(rotated_folder);
    CHECK(rotated.loaded_v2 && rotated.loaded_v2->condition.orientation==MountOrientation::rotated_180);
    // A v2 condition is drawn with the lens-aware C.
    {
        const auto choice=ChoosePreviewC(source,MountOrientation::normal,320,214);
        CHECK(choice.mode==PreviewCMode::fixed_composite && !choice.plan.skip && choice.plan.lens_aware && choice.plan.view_width==320 && choice.plan.view_height==214);
        CHECK(choice.plan.capture_seam_column==source.loaded_v2->condition.seam.column && choice.plan.seam_origin==FixedSeamOrigin::band_center_provisional);
        CHECK(choice.plan.profile_id==source.loaded_v2->profile.profile_id && choice.plan.profile_id.has_value());
        // The plan is cheap to hold and to copy: the table is shared.
        const auto copy=choice.plan;
        CHECK(copy.mapped==choice.plan.mapped);
    }
    // The other mounting, and views that cannot be used.
    {
        const auto other=ChoosePreviewC(source,MountOrientation::rotated_180,320,214);
        CHECK(other.mode==PreviewCMode::unavailable && other.plan.skip && *other.plan.skip==FixedCompositeSkip::orientation_differs);
        const auto other_turned=ChoosePreviewC(rotated,MountOrientation::normal,320,214);
        CHECK(other_turned.mode==PreviewCMode::unavailable && other_turned.plan.skip && *other_turned.plan.skip==FixedCompositeSkip::orientation_differs);
        for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{0,214},{320,0},{641,214},{320,641}}){
            const auto choice=ChoosePreviewC(source,MountOrientation::normal,view.first,view.second);
            CHECK(choice.mode==PreviewCMode::unavailable && choice.plan.skip && *choice.plan.skip==FixedCompositeSkip::size_differs);
        }
        FixedPreviewPlan none;none.skip=FixedCompositeSkip::size_differs;none.lens_aware=true;
        const auto flat=Flat(320,214,128);
        REJECT(RenderFixedPreview(none,flat,flat));
        const auto plan=ChoosePreviewC(source,MountOrientation::normal,320,214).plan;
        REJECT(RenderFixedPreview(plan,Flat(321,214,128),flat));
        REJECT(RenderFixedPreview(plan,flat,Flat(320,213,128)));
        auto broken=flat;broken.bgr.pop_back();
        REJECT(RenderFixedPreview(plan,broken,flat));
        auto unmapped=plan;unmapped.mapped.reset();
        REJECT(RenderFixedPreview(unmapped,flat,flat));
    }
    // A seam that does not lie in the overlap of the shrunk grid, and regions that cannot be drawn.
    {
        auto moved=*source.loaded_v2;
        for(const std::uint32_t column:{std::uint32_t{0},std::uint32_t{10},moved.condition.raster.width_pixels-1}){
            moved.condition.seam={column,FixedSeamOrigin::operator_set};
            const auto plan=PlanFixedPreviewV2(moved,MountOrientation::normal,640,427);
            CHECK(plan.skip && *plan.skip==FixedCompositeSkip::seam_outside_overlap);
        }
        auto tiny=*source.loaded_v2;
        tiny.condition.raster=render::OutputRaster{render::DocumentRegionUm{540000,100000,541000,140000},254,10,400};
        const auto plan=PlanFixedPreviewV2(tiny,MountOrientation::normal,640,427);
        CHECK(plan.skip && *plan.skip==FixedCompositeSkip::size_differs);
        // A raster the pixel counts do not describe is refused too, and C is not drawn.
        auto torn=*source.loaded_v2;torn.condition.raster.width_pixels+=1;
        const auto torn_plan=PlanFixedPreviewV2(torn,MountOrientation::normal,640,427);
        CHECK(torn_plan.skip && *torn_plan.skip==FixedCompositeSkip::size_differs);
        // A region that neither camera covers, one where B alone covers the first column, and a gap between the cameras:
        // C is not drawn, whatever the reason code.
        auto away=*source.loaded_v2;
        away.condition.raster=render::OutputRaster{render::DocumentRegionUm{1180000,100000,1189000,140000},254,90,400};
        CHECK(PlanFixedPreviewV2(away,MountOrientation::normal,640,427).skip.has_value());
        auto beyond=*source.loaded_v2;
        beyond.condition.raster=render::OutputRaster{render::DocumentRegionUm{900000,100000,960000,140000},254,600,400};
        CHECK(PlanFixedPreviewV2(beyond,MountOrientation::normal,640,427).skip.has_value());
        // Camera B moved 100 mm to the right: a gap of 60 mm opens between the cameras.
        auto gap=*source.loaded_v2;
        gap.profile.parameters.camera_b.document_to_image[5]+=800;
        gap.condition.raster=render::OutputRaster{render::DocumentRegionUm{590000,100000,660000,140000},254,700,400};
        gap.condition.seam={50,FixedSeamOrigin::operator_set};
        const auto gap_plan=PlanFixedPreviewV2(gap,MountOrientation::normal,640,427);
        // Pixels in the gap are shown by neither camera: size-differs, the reason the JPEG path gives for the same condition.
        CHECK(gap_plan.skip && *gap_plan.skip==FixedCompositeSkip::size_differs);
        // The cameras overlap, but the region runs on past the end of B: samples covered by neither camera.
        auto past=*source.loaded_v2;
        past.condition.raster=render::OutputRaster{render::DocumentRegionUm{570000,100000,1189000,140000},254,6190,400};
        past.condition.seam={150,FixedSeamOrigin::operator_set};
        const auto past_plan=PlanFixedPreviewV2(past,MountOrientation::normal,640,427);
        CHECK(past_plan.skip && *past_plan.skip==FixedCompositeSkip::size_differs && !past_plan.detail.empty());
    }
    // A damaged condition is not an absent one, and C gives the reason the pair capture gives: the
    // preview reads the file with SelectFixedComposite, so the two are compared on the same folder.
    const auto damaged=[&](const char* what,const fs::path& folder,FixedCompositeSkip reason){
        const auto s=Load(folder);
        Check(!s.loaded && !s.loaded_v2 && s.Present() && s.reason==reason && !s.detail.empty(),what);
        const auto selection=SelectFixedComposite(ConditionOf(folder),kHash);
        Check(!selection.loaded && selection.reason==s.reason && selection.detail==s.detail,what);
        const auto choice=ChoosePreviewC(s,MountOrientation::normal,320,214);
        Check(choice.mode==PreviewCMode::unavailable && choice.plan.skip && *choice.plan.skip==reason,what);
        Check(!FixedPreviewUnavailableText(reason).empty() && FixedPreviewUnavailableText(reason).find(L"Cを描きません")!=std::wstring::npos,what);
    };
    {
        const auto folder=CopyOf(normal_folder,"damaged-profile");
        fs::path profile;
        for(const auto& e:fs::directory_iterator(folder))if(e.path().filename().string().starts_with("wpd-fixed-composite-profile-"))profile=e.path();
        CHECK(!profile.empty());
        const auto original=ReadAll(profile);
        WriteAll(profile,original+" ");                       // one byte more: the SHA-256 differs
        damaged("profile bytes changed",folder,FixedCompositeSkip::profile_differs);
        WriteAll(profile,ReplaceAll(original,"\"k1\": -0.05","\"k1\": 0.5")); // another calibration under the same name
        damaged("profile replaced by another calibration",folder,FixedCompositeSkip::profile_differs);
        WriteAll(profile,original);
        CHECK(Load(folder).loaded_v2.has_value());              // restored: usable again
        fs::remove(profile);
        damaged("profile missing",folder,FixedCompositeSkip::profile_absent);
        WriteAll(profile,"{");
        damaged("profile not a profile",folder,FixedCompositeSkip::profile_differs);
        fs::remove(profile);fs::create_directory(profile);
        damaged("profile is a folder",folder,FixedCompositeSkip::profile_differs);
        fs::remove_all(folder);
    }
    {
        const auto folder=CopyOf(normal_folder,"damaged-condition");
        const auto file=ConditionOf(folder);
        const auto original=ReadAll(file);
        WriteAll(file,ReplaceAll(original,"\"approval\":\"unapproved\"","\"approval\":\"approved\""));
        damaged("approval claimed",folder,FixedCompositeSkip::condition_invalid);
        WriteAll(file,original.substr(0,original.size()/2));
        damaged("condition cut short",folder,FixedCompositeSkip::condition_invalid);
        WriteAll(file,"");
        damaged("condition empty",folder,FixedCompositeSkip::condition_invalid);
        fs::remove(file);fs::create_directory(file);
        damaged("condition is a folder",folder,FixedCompositeSkip::condition_invalid);
        fs::remove_all(folder);
    }
    // A version 1 file is still read as version 1 and still drawn with the earlier C.
    {
        const auto folder=root/"v1-state";fs::create_directories(folder);
        WriteFixedCompositeCondition(ConditionOf(folder),MakeV1Condition(930,9,1840,1228));
        const auto v1=Load(folder);
        CHECK(v1.loaded && !v1.loaded_v2 && v1.Present());
        const auto choice=ChoosePreviewC(v1,MountOrientation::normal,320,214);
        CHECK(choice.mode==PreviewCMode::fixed_composite && !choice.plan.lens_aware && !choice.plan.mapped);
        CHECK(choice.plan.view.seam_column==241);
        CHECK(HashOfImage(RenderFixedPreview(choice.plan,Textured(320,214,1),Textured(320,214,2)).image)=="fbe92c281b6686e4b5ba50e4dd6054d4c7096a96d16cb27674376abeb1441d47");
        fs::remove_all(folder);
    }
}

void Frames(const FixedPreviewSource& source){
    const auto choice=ChoosePreviewC(source,MountOrientation::normal,400,267);
    CHECK(choice.mode==PreviewCMode::fixed_composite);
    if(choice.mode!=PreviewCMode::fixed_composite)return;
    const auto& plan=choice.plan;
    // Brightness is measured in each call from the frames; the mapping and the seam never move.
    const auto a=Flat(400,267,100);
    const auto dark=RenderFixedPreview(plan,a,Flat(400,267,50));
    const auto bright=RenderFixedPreview(plan,a,Flat(400,267,200));
    const auto again=RenderFixedPreview(plan,a,Flat(400,267,200));
    CHECK(dark.record.seam_x==bright.record.seam_x && dark.image.width==bright.image.width && dark.image.height==bright.image.height);
    CHECK(Near(dark.record.ratio_maximum,2,0.05) && Near(bright.record.ratio_minimum,0.5,0.05));
    CHECK(bright.image.bgr==again.image.bgr && bright.record.seam_x==again.record.seam_x); // deterministic
    const auto at=[&](const SeamBlendResult& r,unsigned x,unsigned y,unsigned c){return r.image.bgr[(static_cast<size_t>(y)*r.image.width+x)*3+c];};
    const unsigned row=plan.output_height/2,beside=plan.view.seam_column+plan.settings.width_pixels+1;
    // B is brought towards A by the measured ratio, fading linearly back to B's own level at the right edge.
    const auto expected=[&](const SeamBlendResult& r,double b,unsigned x){
        const double ratio=r.row_ratio[row],fade=double(plan.output_width)-plan.view.seam_column;
        return b*(ratio+(1-ratio)*(x-double(plan.view.seam_column))/fade);
    };
    CHECK(Near(dark.row_ratio[row],2,0.05) && Near(bright.row_ratio[row],0.5,0.05));
    CHECK(Near(at(dark,beside,row,0),expected(dark,50,beside),2.5) && Near(at(bright,beside,row,0),expected(bright,200,beside),2.5));
    CHECK(Near(at(dark,plan.view.seam_column+1,row,0),expected(dark,50,plan.view.seam_column+1),2.5) && Near(at(dark,plan.view.seam_column+1,row,0),100,10));
    CHECK(at(dark,plan.output_width-1,row,0)<=60 && at(bright,plan.output_width-1,row,0)>=190);
    CHECK(a.bgr==Flat(400,267,100).bgr); // the inputs are untouched
    // A plan made again from the same file draws the same picture.
    const auto second=ChoosePreviewC(source,MountOrientation::normal,400,267);
    CHECK(second.plan.view.seam_column==plan.view.seam_column && second.plan.output_width==plan.output_width);
    CHECK(RenderFixedPreview(second.plan,a,Flat(400,267,200)).image.bgr==bright.image.bgr);
}

void Texts(const FixedPreviewSource& source){
    auto plan=ChoosePreviewC(source,MountOrientation::normal,320,214).plan;
    CHECK(!plan.skip && plan.lens_aware);
    const auto title=FixedPreviewTitle(MountOrientation::normal,plan);
    CHECK(title.find(L"固定の合成条件 v2（レンズの歪みを補正、未承認・表示専用）")!=std::wstring::npos);
    CHECK(title.find(L"未校正")==std::wstring::npos && title.find(L"通常")!=std::wstring::npos);
    CHECK(title.find(std::to_wstring(plan.capture_seam_column))!=std::wstring::npos && title.find(L"帯の中央の仮の値")!=std::wstring::npos && title.find(L"出力ラスタの画素")!=std::wstring::npos);
    CHECK(FixedPreviewTitle(MountOrientation::rotated_180,plan).find(L"180°回転")!=std::wstring::npos);
    plan.seam_origin=FixedSeamOrigin::operator_set;
    CHECK(FixedPreviewTitle(MountOrientation::normal,plan).find(L"人が決めた値")!=std::wstring::npos);
    REJECT(FixedPreviewTitle(static_cast<MountOrientation>(7),plan));
    auto note=FixedPreviewNote(plan);
    CHECK(note.find(L"※未検証")!=std::wstring::npos && note.find(L"#309")!=std::wstring::npos && note.find(L"（S）")!=std::wstring::npos);
    CHECK(note.find(L"校正の写像（draft、未承認）")!=std::wstring::npos && note.find(L"倍率は補正しません")!=std::wstring::npos && note.find(L"保存しません")!=std::wstring::npos);
    CHECK(plan.profile_id && note.find(std::wstring(plan.profile_id->begin(),plan.profile_id->end()))!=std::wstring::npos);
    CHECK(note.find(L"寄せています")==std::wstring::npos);
    plan.seam_shift=-2;
    CHECK(FixedPreviewNote(plan).find(L"2px 左へ寄せています")!=std::wstring::npos);
    plan.seam_shift=1;
    CHECK(FixedPreviewNote(plan).find(L"1px 右へ寄せています")!=std::wstring::npos);
    plan.profile_id.reset();
    CHECK(FixedPreviewNote(plan).find(L"校正の写像（draft、未承認）")!=std::wstring::npos);
    plan.profile_id=std::string("a\xe3\x81\x82\x01z");
    CHECK(FixedPreviewNote(plan).find(L"a????z")!=std::wstring::npos); // only plain characters are shown
    // The v1 texts are as they were.
    FixedPreviewPlan v1;v1.capture_seam_column=7;
    CHECK(FixedPreviewTitle(MountOrientation::normal,v1).find(L"未校正・未承認・表示専用")!=std::wstring::npos);
    CHECK(FixedPreviewNote(v1).find(L"ライブビューと撮影画像の対応は ※未検証（#309）")!=std::wstring::npos && FixedPreviewNote(v1).find(L"校正の写像")==std::wstring::npos);
    // While a condition exists and C is not drawn with it.
    CHECK(FixedPreviewUnavailableTitle(source).find(L"v2")!=std::wstring::npos && FixedPreviewUnavailableTitle(source).find(L"未承認")!=std::wstring::npos);
    CHECK(FixedPreviewUnavailableTitle(FixedPreviewSource{}).find(L"v2")==std::wstring::npos);
    const std::vector<std::wstring> standing{title,note,FixedPreviewPositionRefusal()};
    for(const auto& text:standing)
        CHECK(text.find(L"同期")==std::wstring::npos && text.find(L"同時")==std::wstring::npos && text.find(L"合格")==std::wstring::npos);
}

// Time of one frame of C, for the decision about the coordinate table. Printed, never judged.
double MedianMilliseconds(std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];}
double MillisecondsSince(std::chrono::steady_clock::time_point t){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();}
void Measure(const FixedPreviewSource& source){
    for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{400,267}}){
        const auto begin=std::chrono::steady_clock::now();
        const auto choice=ChoosePreviewC(source,MountOrientation::normal,view.first,view.second);
        const double plan_ms=MillisecondsSince(begin);
        if(choice.mode!=PreviewCMode::fixed_composite){std::cout<<"measure: no plan for "<<view.first<<"x"<<view.second<<'\n';continue;}
        const auto& plan=choice.plan;
        const auto live=LiveView(source.loaded_v2->profile.parameters,view.first,view.second,MountOrientation::normal);
        const double sx=view.first/7360.0,sy=view.second/4912.0;
        const auto direct=LiveViewMappedInput(source.loaded_v2->profile.parameters,plan.grid,sx,sy,view.first,view.second,false);
        std::vector<double> table_ms,direct_ms;
        for(int i=0;i<15;++i){
            auto t0=std::chrono::steady_clock::now();
            (void)RenderFixedPreview(plan,live.a,live.b);
            table_ms.push_back(MillisecondsSince(t0));
            t0=std::chrono::steady_clock::now();
            (void)RenderMappedFixedSeam(live.a,live.b,direct,plan.view.seam_column,plan.settings);
            direct_ms.push_back(MillisecondsSince(t0));
        }
        std::cout<<"measure "<<view.first<<'x'<<view.second<<": grid "<<plan.output_width<<'x'<<plan.output_height<<" ("<<plan.output_width*plan.output_height<<" px), plan with table "
            <<plan_ms<<" ms, frame with table "<<MedianMilliseconds(table_ms)<<" ms, frame with direct mapping "<<MedianMilliseconds(direct_ms)
            <<" ms, table "<<(plan.output_width*plan.output_height*sizeof(render::Point)*2)/1024<<" KiB\n";
    }
    // A region of the size of the whole sheet (the fixture cameras reach from 0 to 1175 mm across): the grid of a real rig.
    {
        auto sheet=*source.loaded_v2;
        sheet.condition.raster=render::OutputRaster{render::DocumentRegionUm{20000,20000,1150000,800000},200,8898,6142};
        sheet.condition.seam={4409,FixedSeamOrigin::operator_set};
        for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{400,267}}){
            const auto begin=std::chrono::steady_clock::now();
            const auto plan=PlanFixedPreviewV2(sheet,MountOrientation::normal,view.first,view.second);
            const double plan_ms=MillisecondsSince(begin);
            if(plan.skip){std::cout<<"measure sheet "<<view.first<<'x'<<view.second<<": no plan ("<<plan.detail<<")\n";continue;}
            const auto live=LiveView(sheet.profile.parameters,view.first,view.second,MountOrientation::normal);
            const double sx=view.first/7360.0,sy=view.second/4912.0;
            const auto direct=LiveViewMappedInput(sheet.profile.parameters,plan.grid,sx,sy,view.first,view.second,false);
            std::vector<double> table_ms,direct_ms;
            for(int i=0;i<7;++i){
                auto t0=std::chrono::steady_clock::now();
                (void)RenderFixedPreview(plan,live.a,live.b);
                table_ms.push_back(MillisecondsSince(t0));
                t0=std::chrono::steady_clock::now();
                (void)RenderMappedFixedSeam(live.a,live.b,direct,plan.view.seam_column,plan.settings);
                direct_ms.push_back(MillisecondsSince(t0));
            }
            std::cout<<"measure sheet "<<view.first<<'x'<<view.second<<": grid "<<plan.output_width<<'x'<<plan.output_height<<" ("<<plan.output_width*plan.output_height<<" px), plan with table "
                <<plan_ms<<" ms, frame with table "<<MedianMilliseconds(table_ms)<<" ms, frame with direct mapping "<<MedianMilliseconds(direct_ms)
                <<" ms, table "<<(plan.output_width*plan.output_height*sizeof(render::Point)*2)/1024<<" KiB, seam shift "<<plan.seam_shift<<'\n';
        }
    }
    // The earlier C for reference.
    const auto v1=PlanFixedPreview(MakeV1Condition(930,9,1840,1228),MountOrientation::normal,640,427);
    if(!v1.skip){
        const auto a=Textured(640,427,1),b=Textured(640,427,2);
        std::vector<double> ms;
        for(int i=0;i<15;++i){const auto t0=std::chrono::steady_clock::now();(void)RenderFixedPreview(v1,a,b);ms.push_back(MillisecondsSince(t0));}
        std::cout<<"measure v1 640x427: output "<<v1.output_width<<'x'<<v1.output_height<<", frame "<<MedianMilliseconds(ms)<<" ms\n";
    }
}
}

int main(int argc,char* argv[]){
    try{
        if(argc<2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const bool measure=argc>=3 && std::string(argv[2])=="--measure";
        const bool print=measure || (argc>=3 && std::string(argv[2])=="--print");
        draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        // Both cameras get a lens: radial terms and a small tangential one (a barrel-like distortion of about 1 %).
        lens=ReplaceAll(draft,"\"k1\": 0.0","\"k1\": -0.05");
        lens=ReplaceAll(lens,"\"k2\": 0.0","\"k2\": 0.02");
        lens=ReplaceAll(lens,"\"p1\": 0.0","\"p1\": 0.0005");
        root=TempParent()/("fixed-composite-preview-v2-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);

        V1IsPinned();
        Grid();
        const auto normal=MakeFolder("normal",lens,nullptr);
        const auto rotated=MakeFolder("rotated",lens,kRotated);
        Check(normal.made,("normal condition made: "+normal.message).c_str());
        Check(rotated.made,("rotated condition made: "+rotated.message).c_str());
        if(normal.made && rotated.made){
            const auto source=Load(normal.folder),turned=Load(rotated.folder);
            CHECK(source.loaded_v2 && turned.loaded_v2);
            if(source.loaded_v2 && turned.loaded_v2){
                CHECK(source.loaded_v2->condition.seam.column==turned.loaded_v2->condition.seam.column);
                Mapping(source,turned);
                const auto full=FullSize(source.loaded_v2->profile.parameters);
                const auto jpeg=FullSizeJpeg(*source.loaded_v2,full);
                if(print)std::cout<<"jpeg: seam "<<jpeg.seam_mm<<" mm, bars A "<<jpeg.where.a<<" B1 "<<jpeg.where.b1<<" B2 "<<jpeg.where.b2<<" mm, seam column "<<source.loaded_v2->condition.seam.column<<'\n';
                for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{512,342},{400,267},{320,214},{640,400},{480,427}}){
                    Geometry("normal",source,MountOrientation::normal,jpeg,view.first,view.second,print);
                    Geometry("rotated-180",turned,MountOrientation::rotated_180,jpeg,view.first,view.second,print);
                }
                Turned(source,turned);
                States(normal.folder,rotated.folder);
                Frames(source);
                Texts(source);
                if(measure)Measure(source);
            }
        }
        std::error_code ignored;fs::remove_all(root,ignored);
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
