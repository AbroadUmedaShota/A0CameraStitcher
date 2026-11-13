// Contracts of the fast seam measurer (#417, docs/design/fixed-composite-registration.md 11.31): the measurement on a synthetic sheet
// moved by a known amount, the label of a repeating feature, the selection and its two stops, a placement of layer 1 that is off by
// more than the period less the search window, and the independence from the thread count. Camera and file free.
#include "a0/m2/experimental/fast_seam_measurer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsm=a0::m2::experimental::fast_seam_measurer;
namespace render=a0::m2::render;

namespace {
int failures=0,checks=0;
void Check(bool ok,const char* what,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL line "<<line<<": "<<what<<'\n';}}
#define CHECK(x) Check((x),#x,__LINE__)
template<class F>bool Throws(F f){try{f();}catch(const std::exception&){return true;}return false;}

constexpr double kPi=3.14159265358979323846;
constexpr unsigned kW=720,kH=420;

double Frac(double v){return v-std::floor(v);}
// A sum of plane waves with fixed wavelengths, directions and phases: no lattice, no repeat, no random numbers.
double Texture(double x,double y){
    double s=128;
    for(int k=0;k<14;++k){
        const double wavelength=7.0+23.0*Frac(0.6180339887*(k+1)),angle=2*kPi*Frac(0.7548776662*(k+1)),phase=2*kPi*Frac(0.5698402910*(k+1));
        s+=26.0*std::cos(2*kPi/wavelength*(std::cos(angle)*x+std::sin(angle)*y)+phase);
    }
    return s;
}
double Edge(double u){return 1.0/(1.0+std::exp(-u/1.1));}
// Dashes: bars of width 7 px every `period` px along x, each broken into dashes of 24 px every 36 px along y.
double Dashes(double x,double y,double period){
    double cx=std::fmod(x,period);if(cx<0)cx+=period;cx-=period/2;
    double cy=std::fmod(y,36.0);if(cy<0)cy+=36.0;cy-=18.0;
    return 60.0+150.0*Edge(3.5-std::abs(cx))*Edge(12.0-std::abs(cy));
}
// The sheet: dashes left of `split`, texture right of it (a sheet that has both repeating and unique content).
double Sheet(double x,double y,double split,double period){
    const double w=Edge(x-split);   // 0 on the dashes' side, 1 on the texture's
    return (1-w)*Dashes(x,y,period)+w*Texture(x,y);
}
// B shows A's feature moved by (dx, dy).
render::BgrImage Image(double dx,double dy,double split,double period){
    render::BgrImage im{kW,kH,std::vector<std::uint8_t>(static_cast<std::size_t>(kW)*kH*3)};
    for(unsigned v=0;v<kH;++v)for(unsigned u=0;u<kW;++u){
        const auto value=static_cast<std::uint8_t>(std::clamp(std::lround(Sheet(u-dx,v-dy,split,period)),0L,255L));
        const std::size_t at=(static_cast<std::size_t>(v)*kW+u)*3;im.bgr[at]=im.bgr[at+1]=im.bgr[at+2]=value;
    }
    return im;
}
render::OutputToCamera Identity(){return [](render::Point p,render::Point& out){out=p;return true;};}

struct Scene {
    render::BgrImage a,b;
    fsm::Request request;
};
// The lattice: x from 160 to 560, y from 120 to 296, every 16 px (the band's spacing); the patch 32 px; e_b along x.
Scene MakeScene(double dx,double dy,double split,double period,unsigned threads){
    Scene s;
    s.a=Image(0,0,split,period);s.b=Image(dx,dy,split,period);
    auto& r=s.request;
    r.map_a=Identity();r.map_b=Identity();r.output_height=kH;
    for(std::uint32_t y=120;y<=296;y+=16)for(std::uint32_t x=160;x<=560;x+=16)r.lattice.push_back({x,y});
    r.lattice_spacing_px=16;r.patch_px=32;r.baseline={1,0};r.minimum_baseline_component=0.5;r.thread_count=threads;
    return s;
}
void Attach(Scene& s){s.request.camera_a=&s.a;s.request.camera_b=&s.b;}

bool Same(const std::vector<fsm::Point>& x,const std::vector<fsm::Point>& y){
    if(x.size()!=y.size())return false;
    for(std::size_t i=0;i<x.size();++i){
        const auto& p=x[i];const auto& q=y[i];
        if(p.x!=q.x || p.y!=q.y || p.status!=q.status || p.content!=q.content || p.one_directional!=q.one_directional || p.periodic!=q.periodic || p.usable!=q.usable)return false;
        if(std::memcmp(&p.displacement,&q.displacement,sizeof p.displacement)!=0 || std::memcmp(&p.normal,&q.normal,sizeof p.normal)!=0)return false;
        if(std::memcmp(&p.uncertainty_px,&q.uncertainty_px,sizeof p.uncertainty_px)!=0 || std::memcmp(&p.score,&q.score,sizeof p.score)!=0)return false;
        if(std::memcmp(&p.baseline_component_px,&q.baseline_component_px,sizeof p.baseline_component_px)!=0)return false;
    }
    return true;
}

void Design(){
    std::cout<<"the design\n";
    CHECK(!Throws([]{fsm::ValidateDesign(fsm::kDesignDraft);}));
    const auto bad=[](auto&& change){auto d=fsm::kDesignDraft;change(d);return Throws([&]{fsm::ValidateDesign(d);});};
    CHECK(bad([](fsm::Design& d){d.search_radius_px=0;}));
    CHECK(bad([](fsm::Design& d){d.search_radius_px=10;}));
    CHECK(bad([](fsm::Design& d){d.second_peak_ratio=0;}));
    CHECK(bad([](fsm::Design& d){d.round_trip_px=-1;}));
    CHECK(bad([](fsm::Design& d){d.uncertainty_floor_px=d.maximum_uncertainty_px;}));
    CHECK(bad([](fsm::Design& d){d.periodic_reach_px=30;}));
    CHECK(bad([](fsm::Design& d){d.neighbour_wide_radius_px=d.neighbour_radius_px-1;}));
    CHECK(bad([](fsm::Design& d){d.edge_share_line=1.5;}));
    CHECK(bad([](fsm::Design& d){d.strip_margin_px=0;}));
    CHECK(bad([](fsm::Design& d){d={};}));
    // A strip margin below what a point's windows need is refused when the request is known.
    auto s=MakeScene(0,0,330,21,1);Attach(s);
    auto d=fsm::kDesignDraft;d.strip_margin_px=40;
    CHECK(Throws([&]{(void)fsm::Measure(s.request,d);}));
    auto missing=s.request;missing.camera_b=nullptr;
    CHECK(Throws([&]{(void)fsm::Measure(missing,fsm::kDesignDraft);}));
    auto threads=s.request;threads.thread_count=0;
    CHECK(Throws([&]{(void)fsm::Measure(threads,fsm::kDesignDraft);}));
    auto patch=s.request;patch.patch_px=30;
    CHECK(Throws([&]{(void)fsm::Measure(patch,fsm::kDesignDraft);}));
}

// A known shift of a sheet with no repeat: the displacement comes back within a tenth of a pixel, and the result does not depend on
// the thread count.
void KnownShift(){
    std::cout<<"a known shift on a sheet without a repeat\n";
    for(const auto& shift:{std::pair<double,double>{0,0},{2.3,0.7},{-5.6,3.1},{5.0,-4.4}}){
        auto s1=MakeScene(shift.first,shift.second,-1000,21,1);Attach(s1);
        const auto p1=fsm::Measure(s1.request,fsm::kDesignDraft);
        std::size_t valid=0,two=0;double worst=0;
        for(const auto& p:p1){
            if(!p.Valid())continue;
            ++valid;
            // One-directional points are right across their line only.
            const double ex=p.displacement.x-shift.first,ey=p.displacement.y-shift.second;
            const double err=p.one_directional?std::abs(p.normal.x*ex+p.normal.y*ey):std::hypot(ex,ey);
            two+=!p.one_directional;worst=std::max(worst,err);
        }
        std::cout<<"  shift ("<<shift.first<<", "<<shift.second<<"): valid "<<valid<<"/"<<p1.size()<<", two-directional "<<two<<", worst error "<<worst<<" px\n";
        CHECK(valid*10>=p1.size()*8 && worst<0.1);
        auto s4=MakeScene(shift.first,shift.second,-1000,21,4);Attach(s4);
        CHECK(Same(p1,fsm::Measure(s4.request,fsm::kDesignDraft)));
        auto s3=MakeScene(shift.first,shift.second,-1000,21,3);Attach(s3);
        CHECK(Same(p1,fsm::Measure(s3.request,fsm::kDesignDraft)));
    }
    // The shift beyond the window: the points end at the search edge or are not valid, none is valid and wrong.
    {
        auto s=MakeScene(13,0,-1000,21,2);Attach(s);
        const auto p=fsm::Measure(s.request,fsm::kDesignDraft);
        std::size_t edge=0,content=0;bool wrong=false;
        for(const auto& q:p){content+=q.content;edge+=q.status==fsm::Status::search_boundary;if(q.Valid() && std::abs(q.displacement.x-13)>1 && !q.one_directional)wrong=true;}
        std::cout<<"  shift 13 px (window 8): content "<<content<<", ended at the edge "<<edge<<'\n';
        CHECK(!wrong && content>0 && edge*2>=content);
    }
}

// A sheet with dashes repeating every 21 px: the points on it are labelled periodic and, inside the window, valid and right; a repeat
// shorter than the window puts a second peak inside it.
void Dashed(){
    std::cout<<"dashes\n";
    auto s=MakeScene(2.6,-0.8,5000,21,2);Attach(s);   // the whole sheet is dashes
    auto p=fsm::Measure(s.request,fsm::kDesignDraft);
    std::size_t content=0,periodic=0,valid=0,valid_periodic=0;double worst=0;
    for(const auto& q:p){
        content+=q.content;periodic+=q.content && q.periodic;valid+=q.Valid();valid_periodic+=q.Valid() && q.periodic;
        if(q.Valid()){const double ex=q.displacement.x-2.6,ey=q.displacement.y+0.8;worst=std::max(worst,q.one_directional?std::abs(q.normal.x*ex+q.normal.y*ey):std::hypot(ex,ey));}
    }
    std::cout<<"  period 21: content "<<content<<", labelled periodic "<<periodic<<", valid "<<valid<<" (periodic "<<valid_periodic<<"), worst error "<<worst<<" px\n";
    CHECK(content>0 && periodic*10>=content*7 && valid_periodic>0 && worst<0.15);
    // Nothing on this sheet has a non-periodic neighbour: every usable periodic point is left out (nothing can check it).
    const auto sel=fsm::Select(p,s.request,fsm::kDesignDraft);
    std::size_t usable_periodic=0;for(const auto& q:p)usable_periodic+=q.usable && q.periodic;
    std::cout<<"  selection: kept "<<sel.points_kept<<", dropped unchecked "<<sel.dropped_unchecked<<", usable periodic points "<<usable_periodic<<'\n';
    CHECK(sel.dropped_unchecked==usable_periodic && usable_periodic>0);
    for(const auto& q:p)CHECK(!(q.periodic && q.disposition==fsm::Disposition::kept));
    // A repeat of 12 px, less than the window's 16: the second peak is inside the window.
    auto t=MakeScene(2.6,-0.8,5000,12,2);Attach(t);
    const auto pt=fsm::Measure(t.request,fsm::kDesignDraft);
    std::size_t ambiguous=0,content12=0,valid12=0;
    for(const auto& q:pt){content12+=q.content;ambiguous+=q.status==fsm::Status::ambiguous;valid12+=q.Valid();}
    std::cout<<"  period 12: content "<<content12<<", ambiguous "<<ambiguous<<", valid "<<valid12<<'\n';
    CHECK(content12>0 && ambiguous*2>=content12);
}

fsm::Point Make(std::uint32_t x,std::uint32_t y,double z,bool periodic,bool usable=true,fsm::Status status=fsm::Status::valid,bool content=true){
    fsm::Point p;p.x=x;p.y=y;p.status=usable?fsm::Status::valid:status;p.content=content;p.periodic=periodic;p.usable=usable;
    p.baseline_component_px=z;p.displacement={z,0};p.along_baseline=1;p.uncertainty_px=0.03;
    return p;
}
fsm::Request SelectionRequest(){
    fsm::Request r;r.lattice_spacing_px=16;r.patch_px=32;r.baseline={1,0};r.minimum_baseline_component=0.5;return r;
}

// The selection on points made by hand: the neighbour check, the point with nothing to check it against, the wide radius, the edge
// share and the empty case.
void Selection(){
    std::cout<<"the selection\n";
    const auto r=SelectionRequest();
    const auto d=fsm::kDesignDraft;
    {   // A periodic point against four non-periodic neighbours (z 2.0): kept within a pixel, left out beyond it, the edge excluded.
        std::vector<fsm::Point> pts;
        for(const auto& at:{std::pair<std::uint32_t,std::uint32_t>{176,200},{208,200},{192,184},{192,216}})pts.push_back(Make(at.first,at.second,2.0,false));
        pts.push_back(Make(192,200,2.9,true));    // 0.9 px from the neighbours: kept
        pts.push_back(Make(192,232,3.1,true));    // 1.1 px from its neighbour at (192,216) (and the others in reach) : left out
        auto sel=fsm::Select(pts,r,d);
        CHECK(sel.use && sel.points.size()==5 && sel.dropped_by_neighbours==1 && sel.dropped_unchecked==0 && sel.points_kept==5);
        CHECK(pts[4].disposition==fsm::Disposition::kept && pts[5].disposition==fsm::Disposition::dropped_by_neighbours);
        // Exactly the line: kept (more than the line leaves a point out).
        pts[5].baseline_component_px=3.0;
        sel=fsm::Select(pts,r,d);
        CHECK(pts[5].disposition==fsm::Disposition::kept && sel.dropped_by_neighbours==0);
        // The median of the neighbours, not their mean: one wild neighbour does not move it.
        pts.push_back(Make(176,232,40.0,false));
        sel=fsm::Select(pts,r,d);
        CHECK(pts[4].disposition==fsm::Disposition::kept);
    }
    {   // No non-periodic neighbour within 64 px but one within 128 px: the wide radius is used. None within 128 px: left out.
        std::vector<fsm::Point> pts;
        pts.push_back(Make(100,100,1.0,false));
        pts.push_back(Make(180,100,1.4,true));      // 80 px away: only the wide radius reaches the first point
        pts.push_back(Make(400,100,1.0,true));      // 300 px away from every other point
        pts.push_back(Make(180,116,5.0,true));      // beside the periodic point, which is not a neighbour to check against
        auto sel=fsm::Select(pts,r,d);
        CHECK(pts[1].disposition==fsm::Disposition::kept && pts[2].disposition==fsm::Disposition::dropped_unchecked);
        CHECK(pts[3].disposition==fsm::Disposition::dropped_by_neighbours && sel.dropped_unchecked==1);
        // The neighbours must be usable: a non-usable point is no neighbour.
        pts[0]=Make(100,100,1.0,false,false,fsm::Status::flat,true);
        sel=fsm::Select(pts,r,d);
        CHECK(pts[1].disposition==fsm::Disposition::dropped_unchecked);
    }
    {   // The share of content points at the search edge: over the line, none is used; at the line, they are.
        std::vector<fsm::Point> pts;
        for(int i=0;i<12;++i)pts.push_back(Make(160+16*static_cast<std::uint32_t>(i),100,0.5,false));
        for(int i=0;i<4;++i)pts.push_back(Make(160+16*static_cast<std::uint32_t>(i),116,0,false,false,fsm::Status::search_boundary,true));
        auto sel=fsm::Select(pts,r,d);   // 4 of 16 content points: the share 0.25 is not over the line
        CHECK(sel.content_points==16 && sel.edge_points==4 && sel.edge_share==0.25 && sel.use && sel.points.size()==12 && sel.fallback_reason.empty());
        pts.push_back(Make(160,132,0,false,false,fsm::Status::search_boundary,true));   // 5 of 17: over
        sel=fsm::Select(pts,r,d);
        CHECK(!sel.use && sel.points.empty() && sel.fallback_reason=="edge-share" && sel.edge_share>0.25 && sel.points_kept==12);
        // Points without content do not count in the share.
        for(int i=0;i<40;++i)pts.push_back(Make(160+16*static_cast<std::uint32_t>(i%20),400+16*static_cast<std::uint32_t>(i/20),0,false,false,fsm::Status::no_content,false));
        sel=fsm::Select(pts,r,d);
        CHECK(!sel.use && sel.content_points==17 && sel.lattice_points==57);
        auto loose=d;loose.edge_share_line=1.0;
        sel=fsm::Select(pts,r,loose);
        CHECK(sel.use && sel.points.size()==12);
    }
    {   // No point is left for layer 2.
        std::vector<fsm::Point> none;
        none.push_back(Make(160,100,0,false,false,fsm::Status::flat,true));
        auto sel=fsm::Select(none,r,d);
        CHECK(!sel.use && sel.fallback_reason=="no-points" && sel.points.empty());
        std::vector<fsm::Point> empty;
        sel=fsm::Select(empty,r,d);
        CHECK(!sel.use && sel.fallback_reason=="no-points" && sel.edge_share==0);
        // Only periodic points that nothing can check: nothing is left either.
        std::vector<fsm::Point> lonely{Make(160,100,0,true),Make(176,100,0.1,true)};
        sel=fsm::Select(lonely,r,d);
        CHECK(!sel.use && sel.fallback_reason=="no-points" && sel.dropped_unchecked==2);
    }
    {   // The points handed to layer 2 are those of the kept points, in the lattice's order, with the patch's footprint.
        std::vector<fsm::Point> pts{Make(176,200,2.0,false),Make(192,200,2.5,true),Make(208,200,2.0,false)};
        pts[0].one_directional=true;pts[0].normal={1,0};pts[0].displacement={2.0,5};
        auto sel=fsm::Select(pts,r,d);
        CHECK(sel.use && sel.points.size()==3);
        CHECK(sel.points[0].kind==a0::m2::seam_gate::PointKind::one_directional && sel.points[0].normal.x==1 && sel.points[0].displacement.y==5);
        CHECK(sel.points[1].kind==a0::m2::seam_gate::PointKind::two_directional && sel.points[1].normal.x==0 && sel.points[1].position.x==192);
        const auto f=a0::m2::experimental::parallax_correction::PointFootprint(192,200,32);
        CHECK(sel.points[1].footprint.x0==f.x0 && sel.points[1].footprint.y1==f.y1);
    }
}

// Layer 1 is off by more than the period less the window: B's dashes at 16 px (a period of 21 less the window of 8 is 13) look like a
// shift of -5 px to a measurer with a window of +-8, a shift the points show as valid and wrong. The two stops hold: the periodic
// points that nothing can check are left out, and the share of points at the search edge is over the line, so none is used.
void Misplaced(){
    std::cout<<"a placement of layer 1 that is off by more than the period less the window\n";
    auto d=fsm::kDesignDraft;
    for(const double shift:{16.0,19.0}){
        auto s=MakeScene(shift,0,330,21,2);Attach(s);   // dashes left of x = 330, a sheet without a repeat right of it
        auto p=fsm::Measure(s.request,d);
        std::size_t wrong_valid_periodic=0,edge=0,content=0;
        for(const auto& q:p){
            content+=q.content;edge+=q.status==fsm::Status::search_boundary;
            // The dashes at the true shift of 16 show a displacement of -5 (21 less 16) in the window.
            if(q.Valid() && q.periodic && std::abs(q.baseline_component_px-shift)>1)++wrong_valid_periodic;
        }
        std::cout<<"  shift "<<shift<<" px: points valid, periodic and wrong "<<wrong_valid_periodic<<", content "<<content<<", at the edge "<<edge<<'\n';
        CHECK(wrong_valid_periodic>0);   // the trap exists in this scene
        // Without the edge stop, the periodic stop alone: no wrong periodic point reaches layer 2.
        auto only_a=d;only_a.edge_share_line=1.0;
        auto q=p;
        const auto sel_a=fsm::Select(q,s.request,only_a);
        std::size_t wrong_kept=0;
        for(const auto& pt:q)if(pt.periodic && pt.disposition==fsm::Disposition::kept && std::abs(pt.baseline_component_px-shift)>1)++wrong_kept;
        std::cout<<"    periodic stop only: kept "<<sel_a.points_kept<<", wrong points kept "<<wrong_kept<<", dropped unchecked "<<sel_a.dropped_unchecked<<", by neighbours "<<sel_a.dropped_by_neighbours<<'\n';
        CHECK(wrong_kept==0);
        // The edge stop: the share of content points at the search edge is over the line, none is used.
        auto r=p;
        const auto sel=fsm::Select(r,s.request,d);
        std::cout<<"    edge share "<<sel.edge_share<<", used "<<sel.use<<", reason "<<sel.fallback_reason<<'\n';
        CHECK(!sel.use && sel.fallback_reason=="edge-share" && sel.edge_share>d.edge_share_line && sel.points.empty());
    }
    // Without any misplacement, the same sheet is used, and its periodic points agree with the unique ones.
    {
        auto s=MakeScene(1.5,0.5,330,21,2);Attach(s);
        auto p=fsm::Measure(s.request,d);
        const auto sel=fsm::Select(p,s.request,d);
        std::size_t wrong=0;
        for(const auto& pt:p)if(pt.disposition==fsm::Disposition::kept){const double e=pt.one_directional?std::abs(pt.normal.x*(pt.displacement.x-1.5)+pt.normal.y*(pt.displacement.y-0.5)):std::hypot(pt.displacement.x-1.5,pt.displacement.y-0.5);if(e>0.15)++wrong;}
        std::cout<<"  no misplacement: used "<<sel.use<<", kept "<<sel.points_kept<<", edge share "<<sel.edge_share<<", wrong "<<wrong<<'\n';
        CHECK(sel.use && sel.points_kept>0 && sel.edge_share<=d.edge_share_line && wrong==0);
    }
    // Inside the window nothing is taken for another peak: a shift of 7 px.
    {
        auto s=MakeScene(7,0,330,21,2);Attach(s);
        auto p=fsm::Measure(s.request,d);
        const auto sel=fsm::Select(p,s.request,d);
        std::size_t wrong=0;
        for(const auto& pt:p)if(pt.disposition==fsm::Disposition::kept && std::abs(pt.baseline_component_px-7)>1)++wrong;
        std::cout<<"  shift 7 px: used "<<sel.use<<", kept "<<sel.points_kept<<", wrong "<<wrong<<'\n';
        CHECK(wrong==0);
    }
}

// The same points for any thread count, whole path (measure, then select).
void Threads(){
    std::cout<<"threads\n";
    auto s1=MakeScene(3.2,-1.1,330,21,1);Attach(s1);
    auto s4=MakeScene(3.2,-1.1,330,21,4);Attach(s4);
    auto p1=fsm::Measure(s1.request,fsm::kDesignDraft),p4=fsm::Measure(s4.request,fsm::kDesignDraft);
    CHECK(Same(p1,p4));
    const auto a=fsm::Select(p1,s1.request,fsm::kDesignDraft),b=fsm::Select(p4,s4.request,fsm::kDesignDraft);
    bool same=a.use==b.use && a.points.size()==b.points.size() && a.points_kept==b.points_kept && a.edge_points==b.edge_points;
    for(std::size_t i=0;same && i<a.points.size();++i)
        same=std::memcmp(&a.points[i].displacement,&b.points[i].displacement,sizeof a.points[i].displacement)==0 &&
            std::memcmp(&a.points[i].uncertainty_px,&b.points[i].uncertainty_px,sizeof a.points[i].uncertainty_px)==0 && a.points[i].position.x==b.points[i].position.x;
    CHECK(same && a.use);
    // The strip's extent does not change a point: the same lattice measured as part of a larger one gives the same points.
    auto wide=MakeScene(3.2,-1.1,330,21,2);
    wide.request.lattice.insert(wide.request.lattice.begin(),{{100,60},{660,360}});
    Attach(wide);
    const auto pw=fsm::Measure(wide.request,fsm::kDesignDraft);
    bool same_points=pw.size()==p1.size()+2;
    for(std::size_t i=0;same_points && i<p1.size();++i){
        const auto& x=pw[i+2];const auto& y=p1[i];
        same_points=x.status==y.status && std::memcmp(&x.displacement,&y.displacement,sizeof x.displacement)==0 && x.periodic==y.periodic;
    }
    CHECK(same_points);
}
}

int main(){
    try{
        Design();
        KnownShift();
        Dashed();
        Selection();
        Misplaced();
        Threads();
    }catch(const std::exception& error){std::cerr<<"unexpected exception: "<<error.what()<<'\n';return 1;}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
