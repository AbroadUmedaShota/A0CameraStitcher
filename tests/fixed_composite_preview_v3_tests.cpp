// Live C drawn with the registered fixed composite condition v3 (#417 LC): the pose and the paper surface of camera B in C's own
// grid, the two faces of B's table, the difference that decides a new bake, and the whole chain from a scripted estimate through
// the state machine to the picture. With the condition's fixed mapping and a zero base pose, C is the v2 C byte for byte.
// Camera and display free; files only in a temporary folder.
// Argument 1 is the shared rig profile v2 fixture folder. Argument 2 "--measure" prints the time of a plan and of a bake.
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v2.hpp"
#include "a0/phase0/experimental/live_registration_tracker.hpp"
#include "registration_fixtures.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
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
namespace fx=a0::tests::registration_fixtures;
using namespace std::chrono_literals;
using Pose=content_registration::PoseParameters;

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
constexpr double kNaN=std::numeric_limits<double>::quiet_NaN();

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/4}+12h};
const FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return FixedCompositeBytesSha256(bytes);};
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
// Integer-only frames.
render::BgrImage Textured(unsigned w,unsigned h,unsigned seed){
    render::BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned base=60+(x*3+y*2+seed*17)%90;
        const bool bar=(x/9+y/13+seed)%5==0;
        for(unsigned c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(std::min(255u,base+(bar?70u:0u)+c*5));
    }
    return image;
}

// ---- fixtures (the same rig as the v2 C's test) ----
std::string draft,lens;
fs::path root;
constexpr const char* kPolicy="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":1}";
constexpr const char* kRotated="{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"rotated-180\"}";
constexpr std::array<long long,4> kRegion{540000,100000,620000,140000};

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
FixedPreviewSource Load(const fs::path& folder){return LoadFixedPreviewSource(folder/kFixedCompositeConditionName,kHash);}

// A condition v3 on the rig and raster of a loaded v2 condition, built in memory (the reader of v3 is another lane's; C needs the
// fields only). live: registration.live is set. The ranges are round numbers of this test, not owner values.
LoadedFixedCompositeConditionV3 MakeV3(const LoadedFixedCompositeConditionV2& v2,const Pose& base,bool live){
    const auto& c=v2.condition;
    FixedCompositeConditionV3 v3;
    v3.created_utc="2026-01-01T00:00:00Z";
    v3.revision.sequence=1;
    v3.source.profile_file=c.source.profile_file;
    v3.source.profile_file_sha256=c.source.profile_file_sha256;
    v3.source.profile_fingerprint_sha256=c.source.profile_fingerprint_sha256;
    v3.source.policy_sha256=std::string(64,'c');
    v3.orientation=c.orientation;
    v3.raster=c.raster;
    v3.seam.column=c.seam.column;v3.seam.origin=c.seam.origin;
    v3.base_pose=base;
    v3.registration.envelope.position_mm=10;v3.registration.envelope.rotation_deg=2;v3.registration.envelope.tilt_deg=1;v3.registration.envelope.height_percent=5;
    v3.registration.geometry={8,72,256};
    v3.registration.surface_envelope=SurfaceEnvelope{20,4};
    v3.registration.apply_beyond_envelope=true;
    v3.registration.parallax=FixedCompositeConditionV3::Registration::Parallax{1};
    if(live){
        FixedCompositeConditionV3::Registration::Live l;
        l.limits.brown_lowe_alpha=8;l.limits.brown_lowe_beta=0.3;l.limits.minimum_unique_blocks=3;
        v3.registration.live=l;
    }
    v3.seam_gate.limit_px=1;v3.seam_gate.guard_band_px=0.2;
    return LoadedFixedCompositeConditionV3{v3,std::string(64,'d'),v2.profile};
}

bool SamePoint(const render::Point& a,const render::Point& b){return a.x==b.x && a.y==b.y;}
// The two mappings give the same answer, bit for bit, at every pixel of the grid (including where there is none).
bool SameTables(const FixedPreviewPlan& a,const FixedPreviewPlan& b,bool camera_a,bool camera_b){
    if(!a.mapped || !b.mapped || a.output_width!=b.output_width || a.output_height!=b.output_height)return false;
    for(std::uint32_t y=0;y<a.output_height;++y)for(std::uint32_t x=0;x<a.output_width;++x){
        const render::Point at{double(x),double(y)};
        render::Point pa{},pb{};
        if(camera_a){
            const bool oa=a.mapped->camera_a(at,pa),ob=b.mapped->camera_a(at,pb);
            if(oa!=ob || (oa && !SamePoint(pa,pb)))return false;
        }
        if(camera_b){
            const bool oa=a.mapped->camera_b(at,pa),ob=b.mapped->camera_b(at,pb);
            if(oa!=ob || (oa && !SamePoint(pa,pb)))return false;
        }
    }
    return true;
}
bool SamePlanFields(const FixedPreviewPlan& a,const FixedPreviewPlan& b){
    return a.view_width==b.view_width && a.view_height==b.view_height && a.scale_x==b.scale_x && a.scale_y==b.scale_y && a.output_width==b.output_width &&
        a.output_height==b.output_height && a.capture_seam_column==b.capture_seam_column && a.scaled_seam_column==b.scaled_seam_column && a.seam_shift==b.seam_shift &&
        a.view.seam_column==b.view.seam_column && a.settings.width_pixels==b.settings.width_pixels && a.settings.edge_margin_pixels==b.settings.edge_margin_pixels &&
        a.settings.brightness_half_width_pixels==b.settings.brightness_half_width_pixels && a.settings.brightness_smoothing_rows==b.settings.brightness_smoothing_rows &&
        a.grid.left_mm==b.grid.left_mm && a.grid.top_mm==b.grid.top_mm && a.grid.pitch_mm==b.grid.pitch_mm && a.grid.width==b.grid.width && a.grid.height==b.grid.height &&
        a.lens_aware==b.lens_aware && a.profile_id==b.profile_id && a.seam_origin==b.seam_origin;
}

const std::vector<std::pair<unsigned,unsigned>> kViews{{640,427},{400,267},{640,400}};

// With the condition's fixed mapping and a zero base pose, C is the v2 C: the same tables, the same pictures, byte for byte.
void ByteIdentity(const char* label,const LoadedFixedCompositeConditionV2& v2,MountOrientation orientation){
    const auto v3=MakeV3(v2,Pose{},true);
    for(const auto& view:kViews){
        const auto p2=PlanFixedPreviewV2(v2,orientation,view.first,view.second);
        const auto p3=PlanFixedPreviewV3(v3,orientation,view.first,view.second);
        Check(!p2.skip && !p3.skip,label);
        if(p2.skip || p3.skip)continue;
        CHECK(!p2.live && p3.live && p3.lens_aware);
        CHECK(SamePlanFields(p2,p3));
        CHECK(SameTables(p2,p3,true,true));
        const auto a=Textured(view.first,view.second,1),b=Textured(view.first,view.second,2);
        const auto d2=RenderFixedPreview(p2,a,b),d3=RenderFixedPreview(p3,a,b);
        CHECK(d2.image.width==d3.image.width && d2.image.height==d3.image.height && d2.image.bgr==d3.image.bgr);
        CHECK(d2.record.seam_x==d3.record.seam_x && d2.record.fixed_seam==d3.record.fixed_seam);
        // Baking the same fixed mapping again changes nothing either.
        const auto again=RebakePreviewB(p3,RegisteredMapping{Pose{},{},std::nullopt});
        Check(again.ok,label);
        if(again.ok){
            CHECK(SameTables(p2,again.plan,true,true));
            CHECK(RenderFixedPreview(again.plan,a,b).image.bgr==d2.image.bgr);
        }
    }
}

// B's table through the formula written out again: the document point, the homography of the pose, the lens, S, the turn.
bool IndependentB(const render::DocumentRenderParameters& profile,const FixedPreviewGrid& g,double sx,double sy,unsigned vw,unsigned vh,bool turned,
    const render::Matrix3* h,int x,int y,render::Point& live){
    const render::Point document{g.left_mm+(x+.5)*g.pitch_mm,g.top_mm+(y+.5)*g.pitch_mm};
    render::Point moved=document,raw{};
    if(h && !render::TryTransform(*h,document,moved))return false;
    if(!render::TryProjectDocument(profile.camera_b,moved,raw))return false;
    live={raw.x*sx,raw.y*sy};
    if(turned)live={(vw-1.0)-live.x,(vh-1.0)-live.y};
    return true;
}

void BasePose(const char* label,const LoadedFixedCompositeConditionV2& v2,MountOrientation orientation){
    const Pose base{0.15,-0.1,0.3,2.0,-1.5,0.4};
    const auto v3=MakeV3(v2,base,true);
    const auto& profile=v3.profile.parameters;
    for(const auto& view:kViews){
        const auto p2=PlanFixedPreviewV2(v2,orientation,view.first,view.second);
        const auto p3=PlanFixedPreviewV3(v3,orientation,view.first,view.second);
        Check(!p3.skip && p3.live,label);
        if(p3.skip || !p3.live)continue;
        CHECK(p3.live->fixed.pose.tx_mm==2.0 && p3.live->fixed.surface.IsZero() && !p3.live->fixed.parallax);
        CHECK(p3.live->baked.pose.rz_deg==0.3);
        const content_registration::PoseModel model(profile.camera_b);
        const auto h=model.Homography(base);
        const double sx=view.first/7360.0,sy=view.second/4912.0;
        const bool turned=orientation==MountOrientation::rotated_180;
        bool same=true,different=false;
        for(std::uint32_t y=0;y<p3.output_height;++y)for(std::uint32_t x=0;x<p3.output_width;++x){
            render::Point want{},got{},was{};
            const bool ok=IndependentB(profile,p3.grid,sx,sy,view.first,view.second,turned,&h,int(x),int(y),want);
            const bool shown=p3.mapped->camera_b({double(x),double(y)},got);
            same=same && ok==shown && (!ok || SamePoint(want,got));
            if(ok && p2.mapped->camera_b({double(x),double(y)},was) && std::hypot(was.x-got.x,was.y-got.y)>1e-3)different=true;
        }
        CHECK(same);CHECK(different);
        // A is the calibrated mapping, as in the v2 C.
        CHECK(SameTables(p2,p3,true,false));
        CHECK(SamePlanFields(p2,p3));
    }
}

// The mapping at the scale of the capture is S1's: the same function for the same pixel, bit for bit.
void SameAsTheMaker(const LoadedFixedCompositeConditionV2& v2){
    const auto v3=MakeV3(v2,Pose{},true);
    const auto& profile=v3.profile.parameters;
    const auto& raster=v3.condition.raster;
    const auto grid=MakeFixedPreviewGrid(raster,1.0);
    CHECK(grid.pitch_mm==25.4/raster.dpi && grid.width==raster.width_pixels && grid.height==raster.height_pixels);
    const RegisteredMapping fixed{Pose{},{},std::nullopt};
    const LiveRegistrationContext context(profile,raster,v3.condition.seam.column,grid,1.0,1.0,7360,4912,false,fixed);
    const content_registration::PoseModel model(profile.camera_b);
    const std::vector<RegisteredMapping> mappings{
        RegisteredMapping{Pose{},{},std::nullopt},
        RegisteredMapping{Pose{0.15,-0.1,0.3,2.0,-1.5,0.4},{},std::nullopt},
        RegisteredMapping{Pose{},SurfacePlane{6.0,0.004,-0.002},std::nullopt},
        RegisteredMapping{Pose{0.15,-0.1,0.3,2.0,-1.5,0.4},SurfacePlane{12.0,-0.01,0.006},std::nullopt}};
    for(const auto& mapping:mappings){
        const auto mine=LiveViewRegisteredOutputToCamera(context,mapping);
        const auto theirs=RegisteredDocumentOutputToCamera(raster,profile.camera_a,profile.camera_b,model,mapping,v3.condition.seam.column);
        bool same=true;int n=0;
        for(std::uint32_t y=0;y<grid.height;y+=7)for(std::uint32_t x=0;x<grid.width;x+=5){
            render::Point a{},b{};
            const bool oa=mine({double(x),double(y)},a),ob=theirs({double(x),double(y)},b);
            same=same && oa==ob && (!oa || SamePoint(a,b));++n;
        }
        Check(same && n>1000,"C's mapping at the capture's scale is the maker's");
    }
    // The same through the document homography that the context gives.
    CHECK(!LiveDocumentHomography(context,fixed));
    CHECK(LiveDocumentHomography(context,mappings[1]).has_value() && LiveDocumentHomography(context,mappings[2]).has_value());
}

// At the scale of the live view, C's pixel (x,y) is the JPEG's output at the same point of the drawing: the S1 maker called at the
// fractional output coordinate, scaled by s. That fixes the convention (centres at +0.5, raw*s, the turn) against the maker's.
void SameConventionAsTheJpeg(const char* label,const LoadedFixedCompositeConditionV2& v2,MountOrientation orientation){
    const auto v3=MakeV3(v2,Pose{},true);
    const auto& profile=v3.profile.parameters;
    const auto& raster=v3.condition.raster;
    const content_registration::PoseModel model(profile.camera_b);
    const std::vector<RegisteredMapping> mappings{
        RegisteredMapping{Pose{0.15,-0.1,0.3,2.0,-1.5,0.4},{},std::nullopt},
        RegisteredMapping{Pose{0.15,-0.1,0.3,2.0,-1.5,0.4},SurfacePlane{9.0,0.005,-0.003},std::nullopt}};
    for(const auto& view:kViews){
        const auto plan=PlanFixedPreviewV3(v3,orientation,view.first,view.second);
        Check(!plan.skip && plan.live,label);
        if(plan.skip || !plan.live)continue;
        const double s=plan.scale_x,pitch_jpeg=25.4/raster.dpi;
        for(const auto& mapping:mappings){
            const auto mine=LiveViewRegisteredOutputToCamera(*plan.live,mapping);
            const auto jpeg=RegisteredDocumentOutputToCamera(raster,profile.camera_a,profile.camera_b,model,mapping,v3.condition.seam.column);
            double worst=0;int n=0;
            for(std::uint32_t y=0;y<plan.output_height;y+=3)for(std::uint32_t x=0;x<plan.output_width;x+=3){
                // The same document point in the JPEG's output coordinates.
                const render::Point at{(plan.grid.left_mm+(x+.5)*plan.grid.pitch_mm-raster.region_um.left/1000.0)/pitch_jpeg-.5,
                    (plan.grid.top_mm+(y+.5)*plan.grid.pitch_mm-raster.region_um.top/1000.0)/pitch_jpeg-.5};
                render::Point raw{},live{};
                if(!jpeg(at,raw) || !mine({double(x),double(y)},live))continue;
                render::Point want{raw.x*s,raw.y*plan.scale_y};
                if(orientation==MountOrientation::rotated_180)want={(view.first-1.0)-want.x,(view.second-1.0)-want.y};
                worst=std::max(worst,std::hypot(want.x-live.x,want.y-live.y));++n;
            }
            Check(n>100 && worst<1e-6,label);
        }
    }
}

// Camera B's table is baked again for a registration: the new plan shares everything else, the old plan is not disturbed.
void Rebake(const char* label,const LoadedFixedCompositeConditionV2& v2,MountOrientation orientation){
    const auto v3=MakeV3(v2,Pose{},true);
    const auto& profile=v3.profile.parameters;
    const auto plan=PlanFixedPreviewV3(v3,orientation,640,427);
    Check(!plan.skip && plan.live,label);
    if(plan.skip || !plan.live)return;
    const RegisteredMapping wanted{Pose{0.2,-0.1,0.4,2.5,-1.0,0.3},SurfacePlane{3.0,0.004,-0.002},std::nullopt};
    const auto before_a=plan.live->table_a,before_b=plan.live->table_b;
    const auto before_mapped=plan.mapped;
    const auto r=RebakePreviewB(plan,wanted);
    Check(r.ok,label);
    if(!r.ok){std::cerr<<"  "<<r.detail<<'\n';return;}
    // The plan that was given is as it was.
    CHECK(plan.mapped==before_mapped && plan.live->table_b==before_b && plan.live->table_a==before_a && plan.live->baked.pose.tx_mm==0 && plan.live->baked.surface.IsZero());
    // The new plan: A's table is the same object, B's is a new one, and nothing else of the plan moved.
    CHECK(r.plan.live->table_a==before_a && r.plan.live->table_b!=before_b && r.plan.mapped!=before_mapped);
    CHECK(r.plan.live->baked.pose.tx_mm==2.5 && r.plan.live->baked.surface.h0_mm==3.0 && r.plan.live->fixed.pose.tx_mm==0);
    CHECK(SamePlanFields(plan,r.plan) && SameTables(plan,r.plan,true,false) && !SameTables(plan,r.plan,false,true));
    CHECK(r.plan.live->table_b->size()==static_cast<size_t>(plan.output_width)*plan.output_height && r.plan.live->table_b->size()<=kFixedPreviewMaximumOutputPixels);
    // B's table is the maker's mapping for the new registration, scaled to the live view.
    const double sx=plan.scale_x,sy=plan.scale_y;
    const auto h=*LiveDocumentHomography(*r.plan.live,wanted);
    bool same=true;
    for(std::uint32_t y=0;y<plan.output_height;++y)for(std::uint32_t x=0;x<plan.output_width;++x){
        render::Point want{},got{};
        const bool ok=IndependentB(profile,plan.grid,sx,sy,640,427,orientation==MountOrientation::rotated_180,&h,int(x),int(y),want);
        const bool shown=r.plan.mapped->camera_b({double(x),double(y)},got);
        same=same && ok==shown && (!ok || SamePoint(want,got));
    }
    CHECK(same);
    // The picture changes, and baking the old registration back gives the old picture.
    const auto a=Textured(640,427,1),b=Textured(640,427,2);
    const auto old_picture=RenderFixedPreview(plan,a,b).image.bgr;
    CHECK(RenderFixedPreview(r.plan,a,b).image.bgr!=old_picture);
    const auto back=RebakePreviewB(r.plan,RegisteredMapping{Pose{},{},std::nullopt});
    CHECK(back.ok && RenderFixedPreview(back.plan,a,b).image.bgr==old_picture);
    // The same registration baked twice is the same table.
    const auto twice=RebakePreviewB(plan,wanted);
    CHECK(twice.ok && SameTables(r.plan,twice.plan,true,true));

    // What cannot be drawn is not baked and changes nothing.
    const auto refuses=[&](const RegisteredMapping& m,const char* what){
        const auto bad=RebakePreviewB(plan,m);
        Check(!bad.ok && !bad.detail.empty() && bad.plan.mapped==before_mapped && bad.plan.live==plan.live,what);
    };
    RegisteredMapping layer2{Pose{},{},ParallaxProfile{}};
    refuses(layer2,"layer 2 is not drawn by C");
    refuses(RegisteredMapping{Pose{kNaN,0,0,0,0,0},{},std::nullopt},"a pose that is not finite");
    refuses(RegisteredMapping{Pose{},SurfacePlane{0,kNaN,0},std::nullopt},"a surface that is not finite");
    refuses(RegisteredMapping{Pose{0,0,0,100000,0,0},{},std::nullopt},"a pose that moves B off every sample");
    refuses(RegisteredMapping{Pose{},SurfacePlane{1.0e6,0,0},std::nullopt},"a surface above the cameras");
    CHECK(!RebakePreviewB(PlanFixedPreviewV2(v2,orientation,640,427),wanted).ok);
    FixedPreviewPlan refused;refused.skip=FixedCompositeSkip::size_differs;
    CHECK(!RebakePreviewB(refused,wanted).ok);
}

// The difference of two mappings that decides a new bake.
void Difference(const LoadedFixedCompositeConditionV2& v2){
    const auto v3=MakeV3(v2,Pose{},true);
    const auto& profile=v3.profile.parameters;
    const auto plan=PlanFixedPreviewV3(v3,MountOrientation::normal,640,427);
    Check(!plan.skip && plan.live,"plan for the difference");
    if(plan.skip || !plan.live)return;
    const RegisteredMapping zero{Pose{},{},std::nullopt};
    const auto shifted=[](double tx){return RegisteredMapping{Pose{0,0,0,tx,0,0},{},std::nullopt};};
    CHECK(LiveMappingDifferencePx(plan,zero,zero,5)==0);
    const double d1=LiveMappingDifferencePx(plan,zero,shifted(0.1),9),d2=LiveMappingDifferencePx(plan,zero,shifted(0.2),9),d4=LiveMappingDifferencePx(plan,zero,shifted(0.4),9);
    CHECK(d1>0 && d2>d1 && d4>d2 && Near(d2/d1,2,0.06) && Near(d4/d2,2,0.06));
    // Measured on the document plane in C's pixels: against the largest over every pixel of the grid, written out again.
    const content_registration::PoseModel model(profile.camera_b);
    const auto h=model.Homography(Pose{0,0,0,0.4,0,0});
    double full=0;
    for(std::uint32_t y=0;y<plan.output_height;++y)for(std::uint32_t x=0;x<plan.output_width;++x){
        const render::Point document{plan.grid.left_mm+(x+.5)*plan.grid.pitch_mm,plan.grid.top_mm+(y+.5)*plan.grid.pitch_mm};
        render::Point moved{};
        if(!render::TryTransform(h,document,moved))continue;
        render::Point shown{};
        if(!plan.mapped->camera_b({double(x),double(y)},shown))continue;
        full=std::max(full,std::hypot(moved.x-document.x,moved.y-document.y)/plan.grid.pitch_mm);
    }
    CHECK(d4<=full+1e-9 && d4>=0.95*full);
    // Symmetric, the lattice always holds the corners, and a surface counts too.
    CHECK(LiveMappingDifferencePx(plan,shifted(0.4),zero,9)==d4);
    CHECK(LiveMappingDifferencePx(plan,zero,shifted(0.4),2)<=d4 && LiveMappingDifferencePx(plan,zero,shifted(0.4),2)>0);
    CHECK(LiveMappingDifferencePx(plan,zero,RegisteredMapping{Pose{},SurfacePlane{5.0,0,0},std::nullopt},5)>0);
    const double ab=LiveMappingDifferencePx(plan,zero,shifted(0.3),5),bc=LiveMappingDifferencePx(plan,shifted(0.3),RegisteredMapping{Pose{0,0,0.2,0.3,0,0},{},std::nullopt},5),
        ac=LiveMappingDifferencePx(plan,zero,RegisteredMapping{Pose{0,0,0.2,0.3,0,0},{},std::nullopt},5);
    CHECK(ac<=ab+bc+1e-9);
    REJECT(LiveMappingDifferencePx(plan,zero,zero,1));
    REJECT(LiveMappingDifferencePx(PlanFixedPreviewV2(v2,MountOrientation::normal,640,427),zero,zero,5));
    REJECT(LiveMappingDifferencePx(plan,zero,RegisteredMapping{Pose{},{},ParallaxProfile{}},5));
    REJECT(LiveMappingDifferencePx(plan,zero,RegisteredMapping{Pose{kNaN,0,0,0,0,0},{},std::nullopt},5));
}

void Disabled(const LoadedFixedCompositeConditionV2& v2){
    const auto live=MakeV3(v2,Pose{},true),off=MakeV3(v2,Pose{},false);
    CHECK(live.condition.registration.live && !off.condition.registration.live);
    const auto p_live=PlanFixedPreviewV3(live,MountOrientation::normal,640,427),p_off=PlanFixedPreviewV3(off,MountOrientation::normal,640,427);
    Check(!p_live.skip && !p_off.skip,"both plans are drawn");
    CHECK(LiveRegistrationDisabledReason(p_live).empty() && LiveRegistrationDisabledReason(p_off)=="live-registration-disabled");
    CHECK(LiveRegistrationDisabledReason(PlanFixedPreviewV2(v2,MountOrientation::normal,640,427)).empty());
    // The disabled plan is drawn with the fixed mapping, and says so; the same pictures otherwise.
    CHECK(SameTables(p_live,p_off,true,true));
    CHECK(FixedPreviewNote(p_off).find(L"無効")!=std::wstring::npos && FixedPreviewNote(p_live).find(L"無効")==std::wstring::npos);
    CHECK(FixedPreviewNote(p_live).find(L"目安")!=std::wstring::npos);
    // The v2 texts are as they were.
    const auto p2=PlanFixedPreviewV2(v2,MountOrientation::normal,640,427);
    CHECK(FixedPreviewTitle(MountOrientation::normal,p2).find(L"v2")!=std::wstring::npos && FixedPreviewTitle(MountOrientation::normal,p_live).find(L"v3")!=std::wstring::npos);
    CHECK(FixedPreviewNote(p2).find(L"校正の写像（draft、未承認）")!=std::wstring::npos && FixedPreviewNote(p2).find(L"目安")==std::wstring::npos);
}

void Choosing(const LoadedFixedCompositeConditionV2& v2,const LoadedFixedCompositeConditionV2& v2_rotated){
    FixedPreviewSource source;
    CHECK(!source.Present());
    source.loaded_v3=MakeV3(v2,Pose{0,0,0,1,0,0},true);
    CHECK(source.Present());
    auto choice=ChoosePreviewC(source,MountOrientation::normal,640,427);
    CHECK(choice.mode==PreviewCMode::fixed_composite && !choice.plan.skip && choice.plan.live && choice.plan.live->fixed.pose.tx_mm==1);
    // The refusals of the v2 C.
    choice=ChoosePreviewC(source,MountOrientation::rotated_180,640,427);
    CHECK(choice.mode==PreviewCMode::unavailable && choice.plan.skip==FixedCompositeSkip::orientation_differs && !choice.plan.mapped);
    choice=ChoosePreviewC(source,MountOrientation::normal,700,427);
    CHECK(choice.mode==PreviewCMode::unavailable && choice.plan.skip==FixedCompositeSkip::size_differs);
    choice=ChoosePreviewC(source,MountOrientation::normal,0,0);
    CHECK(choice.mode==PreviewCMode::unavailable);
    CHECK(FixedPreviewUnavailableTitle(source).find(L"v3")!=std::wstring::npos);
    FixedPreviewSource turned;turned.loaded_v3=MakeV3(v2_rotated,Pose{},true);
    CHECK(ChoosePreviewC(turned,MountOrientation::rotated_180,640,427).mode==PreviewCMode::fixed_composite);
    // A base pose that the cameras cannot show (B off every sample) is a refusal and not an exception.
    FixedPreviewSource far;far.loaded_v3=MakeV3(v2,Pose{0,0,0,100000,0,0},true);
    choice=ChoosePreviewC(far,MountOrientation::normal,640,427);
    CHECK(choice.mode==PreviewCMode::unavailable && choice.plan.skip.has_value());
    // The reader of the capture path still does not return a v3 file, so a v3 file in the folder is an invalid condition and C
    // says why (the reader is switched on with the capture path).
    const auto folder=root/"v3-file";fs::create_directories(folder);
    fx::ConditionV3Options o;
    WriteAll(folder/kFixedCompositeConditionName,fx::ConditionV3File(o));
    const auto loaded=LoadFixedPreviewSource(folder/kFixedCompositeConditionName,kHash);
    CHECK(!loaded.loaded && !loaded.loaded_v2 && !loaded.loaded_v3 && loaded.Present() && loaded.reason==FixedCompositeSkip::condition_invalid);
}

// ---- C follows the camera ----
constexpr std::uint8_t kBase=140,kBarLevel=250;
constexpr unsigned kThreshold=175;
struct Bar {double x0,x1;};
constexpr Bar kBarA{548,553};
constexpr std::array<Bar,2> kBarsB{Bar{587,592},Bar{608,616}};
render::BgrImage Flat(unsigned w,unsigned h,std::uint8_t v){return render::BgrImage{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3,v)};}
// A bar of the sheet seen by a camera. g: B's document homography (the pose B really has); null for A and for an unmoved B.
void PaintBar(render::BgrImage& image,const render::CameraProjection& camera,const render::Matrix3* g,double su,double sv,const Bar& bar,double step){
    for(double x=bar.x0;x<=bar.x1;x+=step)for(double y=96;y<=144;y+=step){
        render::Point document{x,y},moved=document,raw{};
        if(g && !render::TryTransform(*g,document,moved))continue;
        if(!render::TryProjectDocument(camera,moved,raw))continue;
        const double u=std::floor(raw.x*su+.5),v=std::floor(raw.y*sv+.5);
        if(u<0 || v<0 || u>=image.width || v>=image.height)continue;
        std::fill_n(image.bgr.begin()+static_cast<std::ptrdiff_t>((static_cast<size_t>(v)*image.width+static_cast<size_t>(u))*3),3,kBarLevel);
    }
}
struct Frames {render::BgrImage a,b;};
// The live view of the sheet when camera B really has the pose `truth`: stored order, then turned like the preview turns it.
Frames LiveView(const render::DocumentRenderParameters& p,unsigned w,unsigned h,MountOrientation orientation,const Pose& truth){
    const double sx=w/7360.0,sy=h/4912.0;
    Frames f{Flat(w,h,kBase),Flat(w,h,kBase)};
    const content_registration::PoseModel model(p.camera_b);
    const render::Matrix3 g=model.Homography(truth);
    PaintBar(f.a,p.camera_a,nullptr,sx,sy,kBarA,0.05);
    for(const auto& bar:kBarsB)PaintBar(f.b,p.camera_b,truth.IsZero()?nullptr:&g,sx,sy,bar,0.05);
    OrientRenderInput(f.a,orientation);OrientRenderInput(f.b,orientation);
    return f;
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
Where Draw(const FixedPreviewPlan& plan,const render::DocumentRenderParameters& p,MountOrientation orientation,const Pose& truth){
    const auto frames=LiveView(p,plan.view_width,plan.view_height,orientation,truth);
    return Centroids(RenderFixedPreview(plan,frames.a,frames.b).image,plan.grid.left_mm,plan.grid.pitch_mm);
}

LiveTrackerSettings TrackerSettings(){
    LiveTrackerSettings s;   // values of this test, not proposals
    s.follow_interval_seconds=0.5;s.hold_seconds=3.0;s.full_retry_seconds=1.0;
    s.follow_change_limit_px=0.4;s.inlier_fraction_floor=0.6;s.brightness_change_limit=20;s.bake_threshold_px=0.5;
    s.adopt_threshold_px=0;s.adopt_sigma_factor=0;
    return s;
}
LiveEstimateOutcome Estimate(const Pose& pose,bool follow,double change=0.1){
    LiveEstimateOutcome o;o.usable=true;o.pose=pose;o.band_brightness=100;
    if(follow){o.change_px=change;o.inlier_fraction=0.9;}
    return o;
}

// The camera moves and C follows: a scripted estimate goes through the state machine, the table is baked when the difference is
// over the threshold, and the bars of B are where they are on the sheet. Without it (the fixed mapping) they are not.
void FollowsTheCamera(const char* label,const LoadedFixedCompositeConditionV2& v2,MountOrientation orientation){
    const auto v3=MakeV3(v2,Pose{},true);
    const auto& profile=v3.profile.parameters;
    const unsigned w=640,h=427;
    auto plan=PlanFixedPreviewV3(v3,orientation,w,h);
    Check(!plan.skip && plan.live,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    if(plan.skip || !plan.live)return;
    const double pitch=plan.grid.pitch_mm;
    const Pose none{};
    const auto reference=Draw(plan,profile,orientation,none);
    Check(!std::isnan(reference.a) && !std::isnan(reference.b1) && !std::isnan(reference.b2),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    auto agrees=[&](const Where& got,double tolerance){
        return std::abs(got.a-reference.a)<=tolerance && std::abs(got.b1-reference.b1)<=tolerance && std::abs(got.b2-reference.b2)<=tolerance;
    };

    // Camera B moved by `first`: the fixed mapping shows the bars of B displaced.
    const Pose first{0,0,0.2,-1.0,8.0,0.2};
    {
        const content_registration::PoseModel model(profile.camera_b);
        const auto g=model.Homography(first);
        render::Point moved{};
        Check(render::TryTransform(g,{590,120},moved) && std::abs(moved.x-590)>3*pitch,"the move is more than three pixels of C");
    }
    const auto lost=Draw(plan,profile,orientation,first);
    Check(!std::isnan(lost.b1) && !agrees(lost,2*pitch) && std::abs(lost.b1-reference.b1)>2*pitch,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());

    LiveRegistrationTracker tracker(TrackerSettings(),Pose{});
    std::deque<LiveEstimateOutcome> script;
    std::vector<LiveJob> jobs;
    const LiveEstimator estimator=[&](const LiveJob& job,const render::BgrImage& a,const render::BgrImage& b){
        jobs.push_back(job);
        Check(a.width==w && a.height==h && b.width==w && b.height==h,"the estimator gets the live frames");
        auto o=script.front();script.pop_front();return o;
    };
    const auto frames=LiveView(profile,w,h,orientation,first);
    int rebakes=0;
    const auto settle=[&](double now){
        const LiveMappingDifference difference=[&](const RegisteredMapping& baked,const RegisteredMapping& wanted){
            return LiveMappingDifferencePx(plan,baked,wanted,9);
        };
        const auto decision=tracker.DecideRebake(now,difference);
        if(!decision.rebake)return false;
        const auto result=RebakePreviewB(plan,decision.wanted);
        Check(result.ok,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
        if(!result.ok)return false;
        plan=result.plan;tracker.MarkBaked(decision.wanted);++rebakes;
        return true;
    };

    // Start: nothing is estimated, C is the fixed mapping; no rebake is wanted.
    CHECK(!settle(0));
    script={Estimate(first,false)};
    auto step=RunLiveStep(tracker,0,frames.a,frames.b,estimator);
    Check(step.ran && step.accept.kind==LiveAcceptKind::applied && tracker.View(0).state==LiveRegistrationState::tracking,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    // The estimate is far from the fixed mapping: the table is baked, and C shows the bars where they are on the sheet.
    Check(settle(0),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(agrees(Draw(plan,profile,orientation,first),pitch),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());

    // A small change of the estimate (a follow-up) is under tau_bake: not baked, and the picture is still right to a pixel.
    Pose nudged=first;nudged.tx_mm+=0.05;
    script={Estimate(nudged,true,0.05)};
    step=RunLiveStep(tracker,0.6,frames.a,frames.b,estimator);
    Check(step.ran && step.job->kind==LiveJobKind::follow && step.accept.kind==LiveAcceptKind::applied,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(!settle(0.6) && rebakes==1,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(agrees(Draw(plan,profile,orientation,first),pitch),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());

    // The camera moves again, further: the follow-up sees a change over its line and a full estimate is asked for at once; when it
    // comes, C follows.
    const Pose second{0,0,-0.2,1.0,-6.0,-0.2};
    const auto frames2=LiveView(profile,w,h,orientation,second);
    script={Estimate(first,true,0.9),Estimate(second,false)};
    step=RunLiveStep(tracker,1.2,frames2.a,frames2.b,estimator);
    Check(step.ran && step.accept.full_requested && step.accept.changes==std::vector<LiveChange>{LiveChange::change_over_limit},(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(tracker.View(1.2).state==LiveRegistrationState::holding,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(!settle(1.2),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());   // the estimate in force is held and nothing new is baked
    step=RunLiveStep(tracker,1.2,frames2.a,frames2.b,estimator);
    Check(step.ran && step.job->kind==LiveJobKind::full && step.job->full_reason==LiveFullReason::change_detected && step.accept.kind==LiveAcceptKind::applied,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(settle(1.2) && rebakes==2,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(agrees(Draw(plan,profile,orientation,second),pitch),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    // Held old table would have been wrong for the new scene.
    Check(!agrees(Draw(RebakePreviewB(plan,RegisteredMapping{first,{},std::nullopt}).plan,profile,orientation,second),2*pitch),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());

    // No estimate for longer than the holding time: back to the fixed mapping, baked back, and C is the reference again for a
    // camera that is where it was calibrated.
    script={Estimate(second,true,0.9)};
    step=RunLiveStep(tracker,1.8,frames2.a,frames2.b,estimator);
    Check(step.ran && step.accept.full_requested,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    script={LiveEstimateOutcome{}};
    const auto frames0=LiveView(profile,w,h,orientation,none);
    step=RunLiveStep(tracker,1.8,frames0.a,frames0.b,estimator);   // the full estimate fails
    Check(step.ran && step.accept.kind==LiveAcceptKind::kept_previous,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(tracker.View(2.0).state==LiveRegistrationState::holding && !settle(2.0),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(tracker.View(4.9).state==LiveRegistrationState::fixed,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());   // 3 s after the last verification at 1.2
    Check(settle(4.9) && rebakes==3,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(plan.live->baked.pose.IsZero() && plan.live->baked.surface.IsZero(),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(agrees(Draw(plan,profile,orientation,none),pitch),(std::string(label)+" line "+std::to_string(__LINE__)).c_str());

    // The number of bakes over a drift of small steps: every step is under tau_bake from the previous one, and the table is baked
    // only when the drift from the baked table is over it, so the picture is never further off than tau_bake and one step.
    LiveRegistrationTracker drift(TrackerSettings(),Pose{});
    script={Estimate(none,false)};
    (void)RunLiveStep(drift,0,frames0.a,frames0.b,estimator);
    int baked=0,steps=0;double worst=0;
    auto drift_plan=PlanFixedPreviewV3(v3,orientation,w,h);
    for(int i=1;i<=24;++i){
        Pose p{};p.tx_mm=0.04*i;
        script={Estimate(p,true,0.03)};
        (void)RunLiveStep(drift,0.5*i,frames0.a,frames0.b,estimator);
        ++steps;
        const LiveMappingDifference difference=[&](const RegisteredMapping& a,const RegisteredMapping& b){return LiveMappingDifferencePx(drift_plan,a,b,9);};
        const auto decision=drift.DecideRebake(0.5*i,difference);
        if(decision.rebake){
            const auto result=RebakePreviewB(drift_plan,decision.wanted);
            Check(result.ok,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
            if(!result.ok)break;
            drift_plan=result.plan;drift.MarkBaked(decision.wanted);++baked;
        }
        worst=std::max(worst,LiveMappingDifferencePx(drift_plan,drift_plan.live->baked,drift.View(0.5*i).mapping,9));
    }
    Check(baked>=1 && baked<steps/2,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
    Check(worst<=0.5,(std::string(label)+" line "+std::to_string(__LINE__)).c_str());
}

void Determinism(const LoadedFixedCompositeConditionV2& v2){
    const auto v3=MakeV3(v2,Pose{0.1,0,0.1,1,0,0},true);
    const auto a=PlanFixedPreviewV3(v3,MountOrientation::normal,640,427),b=PlanFixedPreviewV3(v3,MountOrientation::normal,640,427);
    CHECK(SamePlanFields(a,b) && SameTables(a,b,true,true));
    const RegisteredMapping m{Pose{0.2,0.1,0.1,2,1,0.2},SurfacePlane{2,0.001,0.001},std::nullopt};
    const auto ra=RebakePreviewB(a,m),rb=RebakePreviewB(b,m);
    CHECK(ra.ok && rb.ok && SameTables(ra.plan,rb.plan,true,true));
    const auto frame_a=Textured(640,427,1),frame_b=Textured(640,427,2);
    CHECK(RenderFixedPreview(ra.plan,frame_a,frame_b).image.bgr==RenderFixedPreview(rb.plan,frame_a,frame_b).image.bgr);
    CHECK(LiveMappingDifferencePx(a,m,RegisteredMapping{},9)==LiveMappingDifferencePx(b,m,RegisteredMapping{},9));
}

double MillisecondsSince(std::chrono::steady_clock::time_point t0){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();}
void Measure(const LoadedFixedCompositeConditionV2& v2){
    // The size of the whole sheet (the fixture cameras reach from 0 to 1175 mm across): the grid of a real rig.
    auto sheet=v2;
    sheet.condition.raster=render::OutputRaster{render::DocumentRegionUm{20000,20000,1150000,800000},200,8898,6142};
    sheet.condition.seam={4409,FixedSeamOrigin::operator_set};
    for(const auto& view:std::vector<std::pair<unsigned,unsigned>>{{640,427},{400,267}}){
        const auto v3=MakeV3(sheet,Pose{},true);
        auto t0=std::chrono::steady_clock::now();
        const auto plan=PlanFixedPreviewV3(v3,MountOrientation::normal,view.first,view.second);
        const double plan_ms=MillisecondsSince(t0);
        if(plan.skip){std::cout<<"measure sheet "<<view.first<<'x'<<view.second<<": no plan ("<<plan.detail<<")\n";continue;}
        const RegisteredMapping m{Pose{0.2,-0.1,0.3,2.5,-1.0,0.3},SurfacePlane{3.0,0.004,-0.002},std::nullopt};
        std::vector<double> bake_ms,difference_ms;
        for(int i=0;i<5;++i){
            t0=std::chrono::steady_clock::now();
            const auto r=RebakePreviewB(plan,m);
            bake_ms.push_back(MillisecondsSince(t0));
            if(!r.ok){std::cout<<"measure sheet: bake refused ("<<r.detail<<")\n";break;}
            t0=std::chrono::steady_clock::now();
            (void)LiveMappingDifferencePx(plan,RegisteredMapping{},m,9);
            difference_ms.push_back(MillisecondsSince(t0));
        }
        std::sort(bake_ms.begin(),bake_ms.end());std::sort(difference_ms.begin(),difference_ms.end());
        if(!bake_ms.empty())std::cout<<"measure sheet "<<view.first<<'x'<<view.second<<": grid "<<plan.output_width<<'x'<<plan.output_height<<" ("<<plan.output_width*plan.output_height
            <<" px), plan "<<plan_ms<<" ms, bake of B (median of 5) "<<bake_ms[bake_ms.size()/2]<<" ms, difference over 81 samples "<<difference_ms[difference_ms.size()/2]<<" ms\n";
    }
}
}

int main(int argc,char* argv[]){
    try{
        if(argc<2)throw std::invalid_argument("expected the shared rig profile v2 fixture folder");
        const bool measure=argc>=3 && std::string(argv[2])=="--measure";
        draft=ReadAll(fs::path(argv[1])/"calibrated-draft.json");
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        lens=ReplaceAll(draft,"\"k1\": 0.0","\"k1\": -0.05");
        lens=ReplaceAll(lens,"\"k2\": 0.0","\"k2\": 0.02");
        lens=ReplaceAll(lens,"\"p1\": 0.0","\"p1\": 0.0005");
        root=TempParent()/("fixed-composite-preview-v3-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);

        const auto normal=MakeFolder("normal",lens,nullptr);
        const auto rotated=MakeFolder("rotated",lens,kRotated);
        Check(normal.made,("normal condition made: "+normal.message).c_str());
        Check(rotated.made,("rotated condition made: "+rotated.message).c_str());
        if(normal.made && rotated.made){
            const auto source=Load(normal.folder),turned=Load(rotated.folder);
            CHECK(source.loaded_v2 && turned.loaded_v2);
            if(source.loaded_v2 && turned.loaded_v2){
                const auto& n=*source.loaded_v2;const auto& t=*turned.loaded_v2;
                ByteIdentity("ByteIdentity normal",n,MountOrientation::normal);
                ByteIdentity("ByteIdentity rotated-180",t,MountOrientation::rotated_180);
                BasePose("BasePose normal",n,MountOrientation::normal);
                BasePose("BasePose rotated-180",t,MountOrientation::rotated_180);
                SameAsTheMaker(n);
                SameConventionAsTheJpeg("SameConventionAsTheJpeg normal",n,MountOrientation::normal);
                SameConventionAsTheJpeg("SameConventionAsTheJpeg rotated-180",t,MountOrientation::rotated_180);
                Rebake("Rebake normal",n,MountOrientation::normal);
                Rebake("Rebake rotated-180",t,MountOrientation::rotated_180);
                Difference(n);
                Disabled(n);
                Choosing(n,t);
                FollowsTheCamera("FollowsTheCamera normal",n,MountOrientation::normal);
                FollowsTheCamera("FollowsTheCamera rotated-180",t,MountOrientation::rotated_180);
                Determinism(n);
                if(measure)Measure(n);
            }
        }
        std::error_code ignored;fs::remove_all(root,ignored);
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
