#include "a0/phase0/experimental/preview_merge.hpp"
#include "a0/phase0/experimental/preview_alignment.hpp"
#include "a0/phase0/experimental/preview_mount_settings.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include <algorithm>
#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace a0::phase0::experimental;
using a0::m2::render::BgrImage;
using a0::m2::experimental::OrientRenderInput;
using namespace a0::m2::experimental;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    auto reject=[&](auto call){bool threw=false;try{call();}catch(const std::invalid_argument&){threw=true;}check(threw);};
    BgrImage a{8,4,std::vector<std::uint8_t>(8*4*3)},b=a;
    for(size_t i=0;i<a.bgr.size();i+=3){a.bgr[i+2]=200;b.bgr[i]=200;}
    const auto merged=RenderPreviewMerge(a,b,4,0);check(merged.image.width==12 && merged.image.height==4);
    auto pixel=[&](unsigned x,unsigned channel){return merged.image.bgr[x*3+channel];};
    check(pixel(0,2)==200 && pixel(0,0)==0);check(pixel(11,0)==200 && pixel(11,2)==0);
    check(pixel(6,0)==100 && pixel(6,2)==100);check(merged.seam_navigation.available);
    for(int dy:{-3,-1,1,3}){const auto r=RenderPreviewMerge(a,b,4,dy);check(r.image.width==12 && r.image.height==4-std::abs(dy));}
    reject([&]{RenderPreviewMerge(a,b,0,0);});reject([&]{RenderPreviewMerge(a,b,8,0);});
    reject([&]{RenderPreviewMerge(a,b,4,4);});reject([&]{RenderPreviewMerge(a,b,4,-4);});
    auto bad=b;bad.width=7;reject([&]{RenderPreviewMerge(a,bad,4,0);});bad=b;bad.bgr.pop_back();reject([&]{RenderPreviewMerge(a,bad,4,0);});
    const auto now=std::chrono::steady_clock::now();using namespace std::chrono_literals;
    check(PreviewPairFresh(now,now-1s,now));check(!PreviewPairFresh(now,now-2s,now));
    check(!PreviewPairFresh(now,now,now-2s));check(!PreviewPairFresh(now,{},now));check(!PreviewPairFresh(now,now+1ms,now));
    // Independent planar scene: each camera crops a different known rectangle.
    auto world=[](int x,int y){unsigned v=static_cast<unsigned>(x+1000)*2654435761u ^ static_cast<unsigned>(y+1000)*2246822519u;v^=v>>13;v*=3266489917u;return static_cast<unsigned char>(v%96);};
    for(const auto geometry:{std::array<int,4>{80,60,32,3},std::array<int,4>{80,60,47,-4},std::array<int,4>{320,200,113,7}}){
        const auto [width,height,dx,dy]=geometry;BgrImage left{static_cast<unsigned>(width),static_cast<unsigned>(height),{}},right=left;
        left.bgr.resize(static_cast<size_t>(width)*height*3);right.bgr.resize(left.bgr.size());
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<3;++c){
            const auto index=(static_cast<size_t>(y)*width+x)*3+c;left.bgr[index]=world(x,y);right.bgr[index]=static_cast<unsigned char>(world(x+dx,y+dy)*2+20);
        }
        const auto proposal=EstimatePreviewTranslation(left,right);
        check(proposal && proposal->overlap_pixels==static_cast<unsigned>(width-dx) && proposal->vertical_pixels==dy && proposal->correlation>0.999);
    }
    check(!EstimatePreviewTranslation(a,b)); // Uniform views carry no alignment evidence.
    reject([&]{EstimatePreviewTranslation(a,bad);});
    // 180-degree turn: output(x,y)=input(w-1-x,h-1-y) for odd and even sizes.
    const auto turn=MountOrientation::rotated_180;
    auto ramp=[](unsigned w,unsigned h){BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(x*17+y*5+c*83);
        return image;};
    auto at=[](const BgrImage& image,unsigned x,unsigned y,unsigned c){return image.bgr[(static_cast<size_t>(y)*image.width+x)*3+c];};
    constexpr std::array<std::array<unsigned,2>,7> sizes{{{1,1},{3,2},{2,3},{4,4},{5,3},{7,5},{6,1}}};
    for(const auto& [w,h]:sizes){
        const auto input=ramp(w,h);auto turned=input;OrientRenderInput(turned,turn);
        bool exact=turned.width==w && turned.height==h && turned.bgr.size()==input.bgr.size();
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<3;++c)exact=exact && at(turned,x,y,c)==at(input,w-1-x,h-1-y,c);
        check(exact);auto twice=turned;OrientRenderInput(twice,turn);check(twice.bgr==input.bgr);
        auto same=input;OrientRenderInput(same,MountOrientation::normal);check(same.bgr==input.bgr);
    }
    {BgrImage empty{};OrientRenderInput(empty,turn);check(empty.bgr.empty());}
    {auto broken=ramp(3,2);broken.bgr.pop_back();reject([&]{OrientRenderInput(broken,turn);});}
    {auto unknown=ramp(2,2);reject([&]{OrientRenderInput(unknown,static_cast<MountOrientation>(7));});}
    // rotated-180 C: turned A stays left, turned B right, with the same renderer.
    {
        auto raw_a=ramp(8,4),raw_b=ramp(8,4);for(auto& v:raw_b.bgr)v=static_cast<std::uint8_t>(255-v);
        auto turned_a=raw_a,turned_b=raw_b;OrientRenderInput(turned_a,turn);OrientRenderInput(turned_b,turn);
        const auto r=RenderPreviewMerge(turned_a,turned_b,4,0);check(r.image.width==12 && r.image.height==4);
        check(at(r.image,0,0,0)==at(raw_a,7,3,0) && at(r.image,3,3,1)==at(raw_a,4,0,1)); // raw A's right side, upside down
        check(at(r.image,11,0,2)==at(raw_b,0,3,2) && at(r.image,8,3,0)==at(raw_b,3,0,0)); // raw B's left side, upside down
        const auto plain=RenderPreviewMerge(raw_a,raw_b,4,0);check(at(plain.image,0,0,0)==at(raw_a,0,0,0) && plain.image.bgr!=r.image.bgr);
        const auto shifted=RenderPreviewMerge(turned_a,turned_b,4,1);check(shifted.image.height==3 && at(shifted.image,0,0,0)==at(raw_a,7,2,0));
    }
    // Hung rig: the physical overlap is raw A's LEFT edge and raw B's RIGHT
    // edge. After both turn, the proposal is the known shift in that frame.
    for(const auto geometry:{std::array<int,4>{80,60,32,3},std::array<int,4>{80,60,47,-4},std::array<int,4>{320,200,113,7}}){
        const auto [width,height,dx,dy]=geometry;BgrImage raw_a{static_cast<unsigned>(width),static_cast<unsigned>(height),{}},raw_b=raw_a;
        raw_a.bgr.resize(static_cast<size_t>(width)*height*3);raw_b.bgr.resize(raw_a.bgr.size());
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<3;++c){
            const auto index=(static_cast<size_t>(y)*width+x)*3+c;
            raw_a.bgr[index]=world(width-1-x,height-1-y);raw_b.bgr[index]=static_cast<unsigned char>(world(width-1-x+dx,height-1-y+dy)*2+20);
        }
        bool raw_left_overlap=true;
        for(int y=std::max(0,-dy);y<std::min(height,height-dy);++y)for(int x=0;x<width-dx;++x)
            raw_left_overlap=raw_left_overlap && at(raw_b,x+dx,y+dy,1)==at(raw_a,x,y,1)*2+20;
        check(raw_left_overlap);
        const auto raw=EstimatePreviewTranslation(raw_a,raw_b);
        check(!raw || raw->overlap_pixels!=static_cast<unsigned>(width-dx) || raw->vertical_pixels!=dy);
        auto turned_a=raw_a,turned_b=raw_b;OrientRenderInput(turned_a,turn);OrientRenderInput(turned_b,turn);
        const auto proposal=EstimatePreviewTranslation(turned_a,turned_b);
        check(proposal && proposal->overlap_pixels==static_cast<unsigned>(width-dx) && proposal->vertical_pixels==dy && proposal->correlation>0.999);
        if(proposal)check(RenderPreviewMerge(turned_a,turned_b,proposal->overlap_pixels,proposal->vertical_pixels).image.width==static_cast<unsigned>(width+dx));
    }
    // Estimator v2 (#364) on the same independent scenes as the baseline.
    auto at_v2=[&](const OverlapEstimate& e,int width,int dx,int dy){
        const auto* p=e.Applicable();return p && p->overlap_pixels==static_cast<unsigned>(width-dx) && p->vertical_pixels==dy;};
    for(const auto geometry:{std::array<int,4>{80,60,32,3},std::array<int,4>{80,60,47,-4},std::array<int,4>{320,200,113,7}}){
        const auto [width,height,dx,dy]=geometry;BgrImage left{static_cast<unsigned>(width),static_cast<unsigned>(height),{}},right=left;
        left.bgr.resize(static_cast<size_t>(width)*height*3);right.bgr.resize(left.bgr.size());
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<3;++c){
            const auto index=(static_cast<size_t>(y)*width+x)*3+c;left.bgr[index]=world(x,y);right.bgr[index]=static_cast<unsigned char>(world(x+dx,y+dy)*2+20);
        }
        const auto keep_left=left,keep_right=right;
        const auto e=EstimateOverlapTranslation(left,right);
        check(at_v2(e,width,dx,dy) && e.reason==OverlapUndeterminedReason::none && e.reverse && e.reverse->overlap_pixels==e.candidates[0].overlap_pixels);
        check(e.candidates[0].correlation>0 && e.candidates[0].samples>3 && (e.candidates.size()<2 || e.candidates[0].evidence>e.candidates[1].evidence));
        check(left.bgr==keep_left.bgr && right.bgr==keep_right.bgr); // inputs are never modified
        // Hung rig: raw frames overlap at A's left edge; only the turned pair is searched.
        auto raw_a=left,raw_b=right;OrientRenderInput(raw_a,turn);OrientRenderInput(raw_b,turn);
        const auto raw=EstimateOverlapTranslation(raw_a,raw_b);
        check(!at_v2(raw,width,dx,dy));
        auto turned_a=raw_a,turned_b=raw_b;OrientRenderInput(turned_a,turn);OrientRenderInput(turned_b,turn);
        check(at_v2(EstimateOverlapTranslation(turned_a,turned_b),width,dx,dy));
    }
    {
        reject([&]{(void)EstimateOverlapTranslation(a,b);}); // 8x4: below the 8 px minimum height
        BgrImage flat{16,16,std::vector<std::uint8_t>(16*16*3,90)};
        const auto f=EstimateOverlapTranslation(flat,flat);check(!f.Applicable() && f.reason==OverlapUndeterminedReason::no_candidate && f.candidates.empty());
        reject([&]{(void)EstimateOverlapTranslation(flat,a);});
        auto broken=flat;broken.bgr.pop_back();reject([&]{(void)EstimateOverlapTranslation(flat,broken);});
        BgrImage tiny{7,16,std::vector<std::uint8_t>(7*16*3)};reject([&]{(void)EstimateOverlapTranslation(tiny,tiny);});
        BgrImage wide{641,16,std::vector<std::uint8_t>(641*16*3)};reject([&]{(void)EstimateOverlapTranslation(wide,wide);});
        // True overlap 95% lies outside the 10..90% range: the best in range sits
        // on the range edge and is not applied (no clamping into success).
        auto soft=[&](int x,int y){const int cx=x/8,cy=y/8;const double fx=(x%8)/8.0,fy=(y%8)/8.0;
            auto v=[&](int i,int j){return static_cast<double>(world(i*977,j*613));};
            return (v(cx,cy)*(1-fx)+v(cx+1,cy)*fx)*(1-fy)+(v(cx,cy+1)*(1-fx)+v(cx+1,cy+1)*fx)*fy;};
        BgrImage near_a{80,60,std::vector<std::uint8_t>(80*60*3)},near_b=near_a;
        for(int y=0;y<60;++y)for(int x=0;x<80;++x)for(int c=0;c<3;++c){
            near_a.bgr[(static_cast<size_t>(y)*80+x)*3+c]=static_cast<std::uint8_t>(soft(x,y)*2);near_b.bgr[(static_cast<size_t>(y)*80+x)*3+c]=static_cast<std::uint8_t>(soft(x+4,y)*2);}
        const auto edge=EstimateOverlapTranslation(near_a,near_b);
        check(!edge.Applicable() && edge.reason==OverlapUndeterminedReason::search_boundary && !edge.candidates.empty() && edge.candidates[0].overlap_pixels==72);
    }
    check(OverlapEstimateStatusCode(OverlapEstimateStatus::estimated_not_judged)=="estimated-not-judged" && OverlapEstimateStatusCode(OverlapEstimateStatus::undetermined)=="undetermined");
    check(OverlapUndeterminedReasonCode(OverlapUndeterminedReason::reverse_mismatch)=="reverse-mismatch" && OverlapUndeterminedReasonCode(OverlapUndeterminedReason::search_boundary)=="search-boundary");
    for(const auto reason:{OverlapUndeterminedReason::no_candidate,OverlapUndeterminedReason::tied_candidates,OverlapUndeterminedReason::search_boundary,
        OverlapUndeterminedReason::refinement_boundary,OverlapUndeterminedReason::reverse_mismatch})
        check(std::wstring(OverlapUndeterminedLabel(reason)).size()>4 && std::wstring(OverlapUndeterminedLabel(reason)).find(L"合格")==std::wstring::npos);
    reject([&]{(void)OverlapUndeterminedLabel(OverlapUndeterminedReason::none);});
    // Negative controls at live size: the scene generator below is independent
    // of both estimators. Truth: overlap 310 px, B 12 px lower (dx=330, dy=12).
    // Each records the baseline's result and requires v2 to return the true
    // position or undetermined, never another applied position.
    {
        constexpr int W=640,H=427,DX=330,DY=12;
        auto hash=[](int x,int y,unsigned seed){unsigned v=static_cast<unsigned>(x+4096)*2654435761u^static_cast<unsigned>(y+4096)*2246822519u^seed*3266489917u;
            v^=v>>15;v*=2246822519u;v^=v>>13;v*=3266489917u;v^=v>>16;return v;};
        auto floor_div=[](int v,int d){return v>=0?v/d:-((-v+d-1)/d);};
        // Paper 230 with 1 px lines of depth `ink`, one optional line per 23x17 cell.
        auto drawing=[&](int x,int y,double ink){
            const int cx=floor_div(x,23),cy=floor_div(y,17);const unsigned h=hash(cx,cy,1);
            const int lx=cx*23+static_cast<int>(h%23),ly=cy*17+static_cast<int>((h>>8)%17);
            const unsigned kind=(h>>16)%4;const bool line=(kind==0 && x==lx) || (kind==1 && y==ly) || (kind==2 && (x==lx || y==ly));
            return line?230-ink:230.0;};
        // Out-of-focus floor: bilinear value noise on a 32 px lattice.
        auto smooth=[&](int x,int y){
            const int cx=floor_div(x,32),cy=floor_div(y,32);const double fx=(x-cx*32)/32.0,fy=(y-cy*32)/32.0;
            auto v=[&](int i,int j){return 60.0+hash(i,j,7)%141;};
            return (v(cx,cy)*(1-fx)+v(cx+1,cy)*fx)*(1-fy)+(v(cx,cy+1)*(1-fx)+v(cx+1,cy+1)*fx)*fy;};
        // A sees scene(x,y); B sees scene_b(x,y), already in B's own pixels.
        auto shot=[&](auto scene,double vignette,unsigned seed,int noise){
            BgrImage image{W,H,std::vector<std::uint8_t>(static_cast<size_t>(W)*H*3)};
            for(int y=0;y<H;++y)for(int x=0;x<W;++x){
                const double ux=(x-(W-1)/2.0)/(W/2.0),uy=(y-(H-1)/2.0)/(H/2.0);
                double v=scene(x,y)*(1-vignette*(ux*ux+uy*uy)/2)+static_cast<int>(hash(x,y,seed)%(2*noise+1))-noise;
                v=std::clamp(v,0.0,255.0);for(int c=0;c<3;++c)image.bgr[(static_cast<size_t>(y)*W+x)*3+c]=static_cast<std::uint8_t>(std::lround(v));
            }
            return image;};
        struct Control {const char* name;BgrImage a,b;};
        std::vector<Control> controls;
        controls.push_back({"vignetting",shot([&](int x,int y){return drawing(x,y,60);},0.8,11,2),shot([&](int x,int y){return drawing(x+DX,y+DY,60);},0.8,12,2)});
        auto grid=[&](int x,int y){return ((x%24)<3 || (y%24)<3)?70.0:150.0;};
        controls.push_back({"periodic-grid",shot([&](int x,int y){return grid(x,y)-(230-drawing(x,y,40));},0,21,2),
            shot([&](int x,int y){return grid(x+DX,y+DY)-(230-drawing(x+DX,y+DY,40));},0,22,2)});
        // Floor rows (world y<150) are farther: B sees them 40 px less shifted.
        auto depth=[&](int x,int y,int floor_shift){return y<150?smooth(x+floor_shift,y):drawing(x,y,60);};
        controls.push_back({"depth-band",shot([&](int x,int y){return depth(x,y,0);},0,31,2),shot([&](int x,int y){return depth(x+DX,y+DY,-40);},0,32,2)});
        // B's left 70 px repeat A's right 70 px 20 px higher: a similar
        // motif exactly where a 70 px overlap (dx=570, dy=20) would align it.
        controls.push_back({"small-overlap-motif",shot([&](int x,int y){return drawing(x,y,60);},0,41,10),
            shot([&](int x,int y){return x<W-570?drawing(x+570,y+20,60):drawing(x+DX,y+DY,60);},0,42,10)});
        for(const auto& control:controls){
            const auto baseline=EstimatePreviewTranslation(control.a,control.b);
            const auto e=EstimateOverlapTranslation(control.a,control.b),again=EstimateOverlapTranslation(control.a,control.b);
            const bool baseline_true=baseline && baseline->overlap_pixels==W-DX && baseline->vertical_pixels==DY;
            check(!baseline_true); // each control is a real failure of the baseline
            check(!e.Applicable() || at_v2(e,W,DX,DY));
            check(again.status==e.status && again.reason==e.reason && again.candidates.size()==e.candidates.size() &&
                std::equal(e.candidates.begin(),e.candidates.end(),again.candidates.begin(),[](const auto& p,const auto& q){
                    return p.overlap_pixels==q.overlap_pixels && p.vertical_pixels==q.vertical_pixels && p.correlation==q.correlation && p.evidence==q.evidence && p.samples==q.samples;}));
            std::cout<<"negative_control="<<control.name<<" baseline=";
            if(baseline)std::cout<<baseline->overlap_pixels<<","<<baseline->vertical_pixels<<(baseline_true?" (true)":" (wrong)");else std::cout<<"none";
            std::cout<<" v2="<<OverlapEstimateStatusCode(e.status)<<"/"<<OverlapUndeterminedReasonCode(e.reason);
            if(!e.candidates.empty())std::cout<<" best="<<e.candidates[0].overlap_pixels<<","<<e.candidates[0].vertical_pixels<<" r="<<e.candidates[0].correlation<<" evidence="<<e.candidates[0].evidence;
            if(e.candidates.size()>1)std::cout<<" second="<<e.candidates[1].overlap_pixels<<","<<e.candidates[1].vertical_pixels<<" evidence="<<e.candidates[1].evidence;
            if(e.reverse)std::cout<<" reverse="<<e.reverse->overlap_pixels<<","<<e.reverse->vertical_pixels;
            std::cout<<" levels="<<e.levels<<"\n";
        }
    }
    check(PreviewMergeTitle(MountOrientation::normal,12,-3)==L"C 仮Merge（未校正） 取付の向き: 通常 / A:左 + B:右 / 重なり 12px / B上下 -3px");
    check(PreviewMergeTitle(turn,12,0).find(L"取付の向き: 180°回転")!=std::wstring::npos);
    reject([&]{PreviewMergeTitle(static_cast<MountOrientation>(7),12,0);});
    // Local settings: exact two fields, known schema and value; absent is normal.
    const std::string valid="{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"rotated-180\"}";
    check(ParseMountOrientationSettings(valid)==turn);
    check(ParseMountOrientationSettings("\xEF\xBB\xBF{ \"mountOrientation\" : \"normal\",\r\n \"schema\":\"a0.wpd-mount-orientation.v1\" }\r\n")==MountOrientation::normal);
    const std::vector<std::string> bad_settings{"","{}","[]","null",valid+"x",valid+valid,
        "{\"schema\":\"a0.wpd-mount-orientation.v1\"}","{\"mountOrientation\":\"normal\"}",
        "{\"schema\":\"a0.wpd-mount-orientation.v2\",\"mountOrientation\":\"normal\"}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"Rotated-180\"}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"rotated-90\"}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":180}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":null}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"normal\",\"extra\":0}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"normal\",\"mountOrientation\":\"rotated-180\"}",
        "{\"schema\":\"a0.wpd-mount-orientation.v1\",\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"normal\"}"};
    for(const auto& text:bad_settings)reject([&]{ParseMountOrientationSettings(text);});
    namespace fs=std::filesystem;
    const auto folder=fs::current_path()/("preview-mount-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);const auto settings=folder/kMountOrientationSettingsName;
    check(!LoadMountOrientationSettings(settings));
    {std::ofstream f(settings,std::ios::binary);f<<valid;}check(LoadMountOrientationSettings(settings)==turn);
    {std::ofstream f(settings,std::ios::binary|std::ios::trunc);f<<valid<<std::string(4096,' ');}reject([&]{LoadMountOrientationSettings(settings);});
    {std::ofstream f(settings,std::ios::binary|std::ios::trunc);f<<"{\"schema\":\"a0.wpd-mount-orientation.v1\",\"mountOrientation\":\"sideways\"}";}reject([&]{LoadMountOrientationSettings(settings);});
    fs::remove(settings);fs::create_directory(settings);reject([&]{LoadMountOrientationSettings(settings);});
    fs::remove_all(folder);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
