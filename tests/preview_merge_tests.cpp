#include "a0/phase0/experimental/preview_merge.hpp"
#include "a0/phase0/experimental/preview_alignment.hpp"
#include "a0/phase0/experimental/preview_mount_settings.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace a0::phase0::experimental;
using a0::m2::render::BgrImage;
using a0::m2::experimental::OrientRenderInput;
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
