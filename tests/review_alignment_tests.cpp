#include "a0/m2/experimental/review_alignment.hpp"
#include "a0/m2/experimental/review_correspondence.hpp"
#include <iostream>
using namespace a0::m2;
using namespace a0::m2::experimental;
int main(){
    int checks=0,failures=0;auto check=[&](bool v){++checks;if(!v)++failures;};
    auto reject=[&](auto fn){bool threw=false;try{fn();}catch(const std::exception&){threw=true;}check(threw);};
    try{
        auto p=SimilarityReviewGeometry(32,4,0,1,64,48);auto plan=render::PlanPairCanvas(64,48,64,48,p);
        check(plan.output_width==96 && plan.output_height==44 && p.crop.top==4 && p.crop.bottom==4);
        p=SimilarityReviewGeometry(32,0,90,1,64,48);auto center=render::Transform(p.camera_b_to_camera_a,{32,24}),right=render::Transform(p.camera_b_to_camera_a,{42,24});
        check(std::abs(center.x-64)<1e-10 && std::abs(center.y-24)<1e-10 && std::abs(right.x-64)<1e-10 && std::abs(right.y-34)<1e-10);
        p=SimilarityReviewGeometry(32,0,0,1.5,64,48);right=render::Transform(p.camera_b_to_camera_a,{42,24});check(right.x==79 && right.y==24);
        render::BgrImage a{64,48,std::vector<std::uint8_t>(64*48*3,20)},b{64,48,std::vector<std::uint8_t>(64*48*3,80)};
        for(double degrees:{-8.0,-0.1,0.0,0.1,8.0})for(double scale:{0.9,1.0,1.1}){
            p=SimilarityReviewGeometry(32,2,degrees,scale,64,48);const auto rendered=render::RenderPair(a,b,p);check(rendered.image.width>64 && rendered.image.height>0);
            // Independently inverse-rotate the planned output corners. B must
            // cover all right-hand output pixels, A all left-hand pixels.
            plan=render::PlanPairCanvas(64,48,64,48,p);
            const auto theta=degrees*std::numbers::pi/180;
            for(unsigned y=0;y<plan.output_height;++y)for(unsigned x=64;x<plan.output_width;++x){
                const auto gx=plan.origin_x+p.crop.left+x,gy=plan.origin_y+p.crop.top+y;
                const auto bx=(std::cos(theta)*(gx-64)+std::sin(theta)*(gy-26))/scale+32;
                const auto by=(-std::sin(theta)*(gx-64)+std::cos(theta)*(gy-26))/scale+24;
                if(bx<0 || bx>=64 || by<0 || by>=48)throw std::runtime_error("independent coverage oracle failed");
            }
        }
        p=SimilarityReviewGeometry(32,0,0,1,64,48);ReviewViewport view{96,48,96,48,1,48,24};
        auto pixels=CompareReviewBgra(a,b,p,view,ReviewComparison::overlay);check(pixels[(24*96+40)*4]==50 && pixels[(24*96+10)*4]==20 && pixels[(24*96+80)*4]==80);
        pixels=CompareReviewBgra(a,b,p,view,ReviewComparison::difference);check(pixels[(24*96+40)*4]==60 && pixels[(24*96+10)*4]!=0);
        pixels=CompareReviewBgra(a,b,p,view,ReviewComparison::camera_a);check(pixels[(24*96+40)*4]==20);
        pixels=CompareReviewBgra(a,b,p,view,ReviewComparison::camera_b);check(pixels[(24*96+40)*4]==80);
        b=a;pixels=CompareReviewBgra(a,b,p,view,ReviewComparison::difference);check(pixels[(24*96+40)*4]==0);
        // Independent coordinate ramps: correct translation, quarter-turn and
        // 2x scale must cancel in the common region; a wrong shift must not.
        render::BgrImage ramp_a{16,16,std::vector<std::uint8_t>(16*16*3)},ramp_b=ramp_a;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(int ch=0;ch<3;++ch)ramp_a.bgr[(y*16+x)*3+ch]=static_cast<std::uint8_t>(32+3*x+2*y);
        for(int transform=0;transform<3;++transform){
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(int ch=0;ch<3;++ch){
                const int value=transform==0?32+3*(x+8)+2*y:transform==1?32+3*(24-y)+2*x:16+6*x+4*y;
                ramp_b.bgr[(y*16+x)*3+ch]=static_cast<std::uint8_t>(value);
            }
            p=SimilarityReviewGeometry(8,0,transform==1?90:0,transform==2?2:1,16,16);
            const auto bounds=render::PlanPairCanvas(16,16,16,16,p);
            ReviewViewport v{bounds.output_width,bounds.output_height,bounds.output_width,bounds.output_height,1,bounds.output_width/2.0,bounds.output_height/2.0};
            auto diff=CompareReviewBgra(ramp_a,ramp_b,p,v,ReviewComparison::difference);bool zero=true;
            for(unsigned y=0;y<v.view_height;++y)for(unsigned x=10;x<15;++x)zero=zero && diff[(y*v.view_width+x)*4]==0;
            check(zero);
            if(transform==0){p=SimilarityReviewGeometry(9,0,0,1,16,16);v.image_width=v.view_width=25;v.center_x=12.5;diff=CompareReviewBgra(ramp_a,ramp_b,p,v,ReviewComparison::difference);check(diff[(8*25+12)*4]==3);}
        }
        // Hand-derived mappings, independent of the fitted matrix implementation.
        std::vector<ReviewCorrespondence> points;
        for(auto bpoint:std::array<render::Point,4>{{{20,40},{70,40},{20,100},{70,100}}})
            points.push_back({{170+.96*(bpoint.x-100)-.28*(bpoint.y-80),76+.28*(bpoint.x-100)+.96*(bpoint.y-80)},bpoint});
        auto proposal=ProposeReviewSimilarity(points,200,160);
        check(proposal.x==70 && proposal.y==-4 && std::abs(proposal.scale-1)<1e-12 && std::abs(proposal.degrees-std::atan2(.28,.96)*180/std::numbers::pi)<1e-10);
        auto residual=MeasureReviewCorrespondences(points,proposal.parameters,200,160);
        check(residual.points.size()==4 && residual.rms<1e-10 && residual.maximum<1e-10);
        auto wrong=SimilarityReviewGeometry(73,0,proposal.degrees,proposal.scale,200,160);
        residual=MeasureReviewCorrespondences(points,wrong,200,160);
        check(std::abs(residual.rms-5)<1e-10 && std::abs(residual.maximum-5)<1e-10 && std::abs(residual.points[0].dx-3)<1e-10 && std::abs(residual.points[0].dy-4)<1e-10);
        points={{{90,10},{10,10}},{{110,10},{30,10}},{{90,30},{10,30}},{{110,34},{30,30}}};
        proposal=ProposeReviewSimilarity(points,200,160);residual=MeasureReviewCorrespondences(points,proposal.parameters,200,160);
        check(proposal.x==81 && proposal.y==8 && std::abs(proposal.scale-std::hypot(1.05,.05))<1e-12);
        check(std::abs(residual.rms-std::sqrt(2.0))<1e-10 && std::abs(residual.maximum-2)<1e-10);
        // Fractional best translation must report the actual integer UI shift.
        points={{{90.4,10.3},{10,10}},{{110.4,30.3},{30,30}}};proposal=ProposeReviewSimilarity(points,200,160);
        residual=MeasureReviewCorrespondences(points,proposal.parameters,200,160);
        check(proposal.x==80 && proposal.y==0 && std::abs(residual.rms-.5)<1e-10);
        auto picked=PickReviewSourcePoint({42,22},SimilarityReviewGeometry(32,2,0,1,64,48),64,48,true);
        check(picked && picked->x==10 && picked->y==22); // cropped y=22 -> global y=24 -> B y=22
        check(!PickReviewSourcePoint({1,22},SimilarityReviewGeometry(32,2,0,1,64,48),64,48,true));
        check(!PickReviewSourcePoint({80,22},SimilarityReviewGeometry(32,2,0,1,64,48),64,48,false));
        p=SimilarityReviewGeometry(32,0,90,1,64,48);plan=render::PlanPairCanvas(64,48,64,48,p);
        picked=PickReviewSourcePoint({64.0-plan.origin_x-p.crop.left,34.0-plan.origin_y-p.crop.top},p,64,48,true);
        check(picked && std::abs(picked->x-42)<1e-10 && std::abs(picked->y-24)<1e-10);
        ReviewViewport picking{96,44,40,20,2,50,22};picking.Constrain();auto clicked=picking.Pixel(20,10);
        picked=PickReviewSourcePoint({double((*clicked)[0]),double((*clicked)[1])},SimilarityReviewGeometry(32,2,0,1,64,48),64,48,true);
        check(picked && picked->x==18 && picked->y==22); // 2x zoom and pan center, cropped original grid
        check(!PickReviewSourcePoint({NAN,0},p,64,48,true));
        reject([&]{ProposeReviewSimilarity({},200,160);});reject([&]{ProposeReviewSimilarity({points[0]},200,160);});
        reject([&]{ProposeReviewSimilarity({points[0],points[0]},200,160);});
        reject([&]{ProposeReviewSimilarity({{{90,10},{10,10}},{{90,10},{20,20}}},200,160);});
        reject([&]{ProposeReviewSimilarity({{{-1,10},{10,10}},points[1]},200,160);});
        reject([&]{ProposeReviewSimilarity({{{90,10},{NAN,10}},points[1]},200,160);});
        reject([&]{ProposeReviewSimilarity({{{1,1},{100,100}},{{2,2},{101,101}}},200,160);});
        reject([&]{ProposeReviewSimilarity(std::vector<ReviewCorrespondence>(33,points[0]),200,160);});
        reject([&]{ProposeReviewSimilarity({{{100,10},{10,10}},{{80,10},{30,10}},{{100,30},{10,30}},{{80,30},{30,30}}},200,160);}); // reflection is not a rotation/scale
        // Validation observations are withheld from fitting. A 3-4-5 error
        // remains visible even though the two fitting points match exactly.
        std::vector<ReviewCorrespondence> separated={{{90,10},{10,10}},{{110,10},{30,10}},{{94,33},{10,30},ReviewPointRole::validation}};
        auto fitted_only=ProposeReviewSimilarity(separated,200,160);
        check(fitted_only.x==80 && fitted_only.y==0 && std::abs(fitted_only.degrees)<1e-10 && std::abs(fitted_only.scale-1)<1e-12);
        auto fit_metrics=MeasureReviewCorrespondences(separated,fitted_only.parameters,200,160,ReviewPointRole::fitting);
        auto check_metrics=MeasureReviewCorrespondences(separated,fitted_only.parameters,200,160,ReviewPointRole::validation);
        check(fit_metrics.points.size()==2 && fit_metrics.rms<1e-10 && fit_metrics.maximum<1e-10);
        check(check_metrics.points.size()==1 && check_metrics.rms==5 && check_metrics.maximum==5 && check_metrics.points[0].dx==-4 && check_metrics.points[0].dy==-3);
        separated.back().camera_a={100,45};auto changed_check=ProposeReviewSimilarity(separated,200,160);
        check(changed_check.x==fitted_only.x && changed_check.y==fitted_only.y && changed_check.degrees==fitted_only.degrees && changed_check.scale==fitted_only.scale);
        check_metrics=MeasureReviewCorrespondences(separated,changed_check.parameters,200,160,ReviewPointRole::validation);
        check(std::abs(check_metrics.rms-std::sqrt(325.0))<1e-10);
        auto role_changed=separated;role_changed.back().role=ReviewPointRole::fitting;auto new_fit=ProposeReviewSimilarity(role_changed,200,160);
        check(new_fit.x!=80 || new_fit.y!=0 || std::abs(new_fit.degrees)>1e-10 || std::abs(new_fit.scale-1)>1e-12);
        check(MeasureReviewCorrespondences(role_changed,new_fit.parameters,200,160,ReviewPointRole::validation).points.empty());
        role_changed=separated;role_changed.front().role=ReviewPointRole::validation;
        reject([&]{ProposeReviewSimilarity(role_changed,200,160);}); // three points, only one fitting point
        role_changed[1].role=ReviewPointRole::validation;reject([&]{ProposeReviewSimilarity(role_changed,200,160);});
        role_changed=separated;role_changed.back().camera_b=role_changed.front().camera_b;
        reject([&]{ProposeReviewSimilarity(role_changed,200,160);}); // same B coordinate across roles
        reject([&]{MeasureReviewCorrespondences(role_changed,fitted_only.parameters,200,160,ReviewPointRole::fitting);});
        role_changed=separated;role_changed.back().camera_a.x=NAN;reject([&]{ProposeReviewSimilarity(role_changed,200,160);});
        role_changed=separated;role_changed.back().role=static_cast<ReviewPointRole>(2);reject([&]{ProposeReviewSimilarity(role_changed,200,160);});
        reject([&]{MeasureReviewCorrespondences(separated,fitted_only.parameters,200,160,static_cast<ReviewPointRole>(2));});
        reject([&]{SimilarityReviewGeometry(32,0,0,0,64,48);});reject([&]{SimilarityReviewGeometry(32,0,181,1,64,48);});
        reject([&]{SimilarityReviewGeometry(32,0,0,std::numeric_limits<double>::infinity(),64,48);});
        reject([&]{SimilarityReviewGeometry(63,47,45,0.1,64,48);});
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
