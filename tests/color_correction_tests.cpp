#include "a0/m2/color_correction.hpp"
#include "a0/m2/raw_document_correction.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
using namespace a0::m2;
using namespace a0::m2::render;
using namespace a0::m2::correction;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
template<class F> void Throws(F f,const char* why){bool threw=false;try{f();}catch(const std::invalid_argument&){threw=true;}Check(threw,why);}
bool Has(const ColorCorrectionResult& r,ColorCorrectionOutcome reason){return std::any_of(r.violations.begin(),r.violations.end(),[&](const auto& v){return v.reason==reason;});}
ColorCorrectionPolicy Policy(){return {3,0,255,.1,150,64,1e-10};}
std::vector<ColorCorrespondence> Samples(){
    std::vector<ColorCorrespondence> samples;
    for(unsigned i=0;i<16;++i){const std::array<double,3> b{20.+4*i,40.+3*i,50.+2*i};samples.push_back({{2*b[0]+8,.5*b[1]-4,1.25*b[2]+12},b});}return samples;
}
void Pure(){
    const auto samples=Samples();const auto r=EstimateBoundedColorCorrection(samples,Policy());
    Check(r.outcome==ColorCorrectionOutcome::Estimated&&r.transform&&r.violations.empty(),"known diagonal color transform estimated");if(!r.transform)return;
    const std::array<double,3> gain{2,.5,1.25},offset{8,-4,12};
    for(unsigned c=0;c<3;++c){Check(std::abs(r.transform->gain[c]-gain[c])<1e-12&&std::abs(r.transform->offset_dn[c]-offset[c])<1e-12,"independent known per-channel coefficients");Check(r.channels[c]&&r.channels[c]->residuals&&r.channels[c]->residuals->maximum_dn<1e-10,"all color fit diagnostics recorded");}
    const auto again=EstimateBoundedColorCorrection(samples,Policy());Check(again.transform&&again.transform->gain==r.transform->gain&&again.transform->offset_dn==r.transform->offset_dn,"deterministic color estimation");
    auto identity=samples;for(auto& p:identity)p.camera_a_bgr=p.camera_b_bgr;
    auto zero=Policy();zero.maximum_gain_difference_percent=zero.maximum_offset_dn=zero.maximum_residual_dn=0;
    const auto same=EstimateBoundedColorCorrection(identity,zero);Check(same.transform&&same.transform->gain==std::array<double,3>{1,1,1}&&same.transform->offset_dn==std::array<double,3>{0,0,0},"exact identity honors zero budgets");
    auto many=samples;while(many.size()<kMaximumColorSamples)many.push_back(samples[many.size()%samples.size()]);Check(EstimateBoundedColorCorrection(many,Policy()).transform.has_value(),"4096 samples accepted");many.push_back(samples[0]);Check(EstimateBoundedColorCorrection(many,Policy()).outcome==ColorCorrectionOutcome::TooManySamples,"4097 samples refused before fitting");
    for(unsigned count=0;count<3;++count){const auto bad=EstimateBoundedColorCorrection(std::span(samples).first(count),Policy());Check(bad.outcome==ColorCorrectionOutcome::InsufficientSamples&&!bad.transform,"missing samples have no correction");}
    for(unsigned c=0;c<3;++c)for(const double value:{-1.,256.,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        auto bad=samples;bad[5].camera_b_bgr[c]=value;const auto rejected=EstimateBoundedColorCorrection(bad,Policy());Check(rejected.outcome==ColorCorrectionOutcome::InvalidSamples&&!rejected.transform&&rejected.violations[0].sample_index==5&&rejected.violations[0].channel==c,"bad sample index/channel retained without pruning");
    }
    auto range=Policy();range.minimum_sample_dn=21;Check(EstimateBoundedColorCorrection(samples,range).outcome==ColorCorrectionOutcome::SampleRangeRejected,"lower signal range rejects whole set");
    range=Policy();range.maximum_sample_dn=160;Check(EstimateBoundedColorCorrection(samples,range).outcome==ColorCorrectionOutcome::SampleRangeRejected,"upper signal range rejects whole set");
    range=Policy();range.minimum_sample_dn=16;range.maximum_sample_dn=168;Check(EstimateBoundedColorCorrection(samples,range).transform.has_value(),"sample range endpoints inclusive");range.minimum_sample_dn=std::nextafter(16.,17.);Check(!EstimateBoundedColorCorrection(samples,range).transform,"sample below lower range by one step refused");range.minimum_sample_dn=16;range.maximum_sample_dn=std::nextafter(168.,167.);Check(!EstimateBoundedColorCorrection(samples,range).transform,"sample above upper range by one step refused");
    for(unsigned c=0;c<3;++c)for(bool flat_a:{false,true}){auto flat=samples;for(auto& p:flat)(flat_a?p.camera_a_bgr:p.camera_b_bgr)[c]=100;const auto bad=EstimateBoundedColorCorrection(flat,Policy());Check(!bad.transform&&Has(bad,ColorCorrectionOutcome::InsufficientContrast)&&bad.channels[c]&&!bad.channels[c]->estimate,"constant channel cannot define gain/offset");}
    auto anti=samples;for(auto& p:anti)for(unsigned c=0;c<3;++c)p.camera_a_bgr[c]=255-p.camera_b_bgr[c];const auto negative=EstimateBoundedColorCorrection(anti,Policy());Check(!negative.transform&&Has(negative,ColorCorrectionOutcome::NonpositiveGain)&&negative.channels[0]->estimate->gain<0,"negative response is not clamped");
    auto outlier=samples;outlier.back().camera_a_bgr[0]-=10;auto noisy=Policy();noisy.maximum_residual_dn=20;const auto fit=EstimateBoundedColorCorrection(outlier,noisy);Check(fit.transform.has_value(),"explicit residual allowance accepts noisy samples");
    auto nonlinear=samples;for(auto& v:nonlinear)v.camera_a_bgr[0]=v.camera_b_bgr[0]*v.camera_b_bgr[0]/32;Check(Has(EstimateBoundedColorCorrection(nonlinear,Policy()),ColorCorrectionOutcome::ResidualExceeded),"nonlinear response cannot masquerade as affine fit");
    auto saturated=samples;saturated[5].camera_a_bgr[1]=255;auto guarded=Policy();guarded.maximum_sample_dn=254;Check(EstimateBoundedColorCorrection(saturated,guarded).outcome==ColorCorrectionOutcome::SampleRangeRejected,"explicit saturation margin rejects entire sample set");
    for(unsigned kind=0;kind<4;++kind){
        auto p=noisy;double boundary=kind==3?std::numeric_limits<double>::infinity():0;
        for(const auto& c:fit.channels){if(kind==0)boundary=std::max(boundary,std::abs(c->estimate->gain-1)*100);if(kind==1)boundary=std::max(boundary,std::abs(c->estimate->offset_dn));if(kind==2)boundary=std::max(boundary,c->residuals->maximum_dn);if(kind==3)boundary=std::min({boundary,c->camera_a_stddev_dn,c->camera_b_stddev_dn});}
        double* limit=kind==0?&p.maximum_gain_difference_percent:kind==1?&p.maximum_offset_dn:kind==2?&p.maximum_residual_dn:&p.minimum_stddev_dn;
        *limit=boundary;Check(EstimateBoundedColorCorrection(outlier,p).transform.has_value(),"exact computed limit inclusive");*limit=std::nextafter(boundary,kind==3?std::numeric_limits<double>::infinity():0.);Check(!EstimateBoundedColorCorrection(outlier,p).transform,"one representable step over limit rejected");
    }
    auto strict=Policy();strict.maximum_gain_difference_percent=strict.maximum_offset_dn=strict.maximum_residual_dn=0;const auto bad=EstimateBoundedColorCorrection(outlier,strict);Check(!bad.transform&&bad.sample_count==outlier.size()&&Has(bad,ColorCorrectionOutcome::GainExceeded)&&Has(bad,ColorCorrectionOutcome::OffsetExceeded)&&Has(bad,ColorCorrectionOutcome::ResidualExceeded)&&bad.channels[0]->estimate&&bad.channels[0]->residuals,"simultaneous violations and estimate retained");
    for(unsigned field=0;field<6;++field)for(const double value:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        auto p=Policy();double* address=field==0?&p.minimum_sample_dn:field==1?&p.maximum_sample_dn:field==2?&p.minimum_stddev_dn:field==3?&p.maximum_gain_difference_percent:field==4?&p.maximum_offset_dn:&p.maximum_residual_dn;*address=value;
        Throws([&]{(void)EstimateBoundedColorCorrection(samples,p);},"invalid explicit policy rejected");
    }
    for(auto count:{std::size_t{0},std::size_t{2},kMaximumColorSamples+1}){auto p=Policy();p.minimum_samples=count;Throws([&]{(void)EstimateBoundedColorCorrection(samples,p);},"invalid sample minimum rejected");}
    auto p=Policy();p.maximum_sample_dn=p.minimum_sample_dn;Throws([&]{(void)EstimateBoundedColorCorrection(samples,p);},"empty signal interval rejected");p=Policy();p.minimum_stddev_dn=0;Throws([&]{(void)EstimateBoundedColorCorrection(samples,p);},"zero contrast floor rejected");
    auto tiny=samples;for(unsigned i=0;i<tiny.size();++i)for(unsigned c=0;c<3;++c)tiny[i].camera_b_bgr[c]=(i+1)*1e-320;p=Policy();p.minimum_stddev_dn=std::numeric_limits<double>::denorm_min();Check(Has(EstimateBoundedColorCorrection(tiny,p),ColorCorrectionOutcome::NumericalFailure),"unrepresentable coefficient ratio rejected");
    const auto original=Samples();for(unsigned i=0;i<samples.size();++i)Check(samples[i].camera_a_bgr==original[i].camera_a_bgr&&samples[i].camera_b_bgr==original[i].camera_b_bgr,"source colors unchanged");
}
CameraProjection Camera(){return {{100,100,16,16},{0,0,0,0,0},{1,0,0,0,1,0,0,0,1}};}
const OutputRaster raster{{0,0,20000,16000},25,20,16};
std::vector<Point> Seeds(){return {{3,3},{7,3},{11,3},{3,7},{7,7},{11,7}};}
std::pair<BgrImage,BgrImage> Images(){
    BgrImage a{32,32,std::vector<std::uint8_t>(32*32*3)},b=a;
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){const auto n=(y*32+x)*3;
        b.bgr[n]=static_cast<std::uint8_t>(40+x+y);b.bgr[n+1]=static_cast<std::uint8_t>(50+2*x+y);b.bgr[n+2]=static_cast<std::uint8_t>(80+2*x+2*y);
        a.bgr[n]=static_cast<std::uint8_t>(88+2*x+2*y);a.bgr[n+1]=static_cast<std::uint8_t>(62+2*x+y);a.bgr[n+2]=static_cast<std::uint8_t>(56+x+y);
    }return {a,b};
}
double Error(const BgrImage& a,const BgrImage& b){double e=0;for(std::size_t n=0;n<a.bgr.size();++n)e+=std::abs(static_cast<int>(a.bgr[n])-b.bgr[n]);return e/a.bgr.size();}
void ImagesAndRender(){
    const auto [a,b]=Images();const auto original_a=a.bgr,original_b=b.bgr;const auto seeds=Seeds();
    const auto r=EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,seeds,Policy());Check(r.outcome==ImageColorOutcome::Estimated&&r.correction&&r.correction->transform&&r.points.size()==seeds.size(),"aligned document seeds sampled then fit");if(!r.correction||!r.correction->transform)return;
    for(const auto& point:r.points)Check(point.outcome==ColorSampleOutcome::Sampled&&point.colors.has_value(),"all original paired colors retained");
    for(auto kernel:{Resampling::bilinear,Resampling::bicubic_catmull_rom})for(auto layout:{StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}){
        const DocumentRenderParameters parameters{raster,Camera(),Camera(),layout,kernel};const auto expected=RenderDocumentPair(a,a,parameters),before=RenderDocumentPair(a,b,parameters),after=RenderDocumentPair(a,b,parameters,*r.correction->transform);
        const double old_error=Error(before.image,expected.image),new_error=Error(after.image,expected.image);Check(old_error>10&&new_error<.1,"corrected render restores unmodified source oracle");
        const auto center=(4*20+5)*3;Check(after.image.bgr[center]==108&&after.image.bgr[center+1]==78&&after.image.bgr[center+2]==66,"independent scalar linear-ramp output pixels");
        Check(RenderDocumentPair(a,b,parameters,*r.correction->transform).image.bgr==after.image.bgr,"color render repeat bytes agree");Check(RenderDocumentPair(a,b,parameters,BgrAffineTransform{{1,1,1},{0,0,0}}).image.bgr==before.image.bgr,"identity preserves old renderer bytes");
        Check(before.seam_navigation.available==after.seam_navigation.available&&before.seam_navigation.x==after.seam_navigation.x&&before.seam_navigation.y==after.seam_navigation.y,"color preserves seam and coverage");
        std::cout<<"color_render before="<<old_error<<" after="<<new_error<<'\n';
    }
    auto invalid=seeds;invalid[1]=invalid[0];const auto duplicate=EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,invalid,Policy());Check(duplicate.outcome==ImageColorOutcome::SamplingFailed&&!duplicate.correction&&duplicate.points.size()==seeds.size()&&duplicate.points[1].outcome==ColorSampleOutcome::InvalidSeed&&duplicate.points.back().colors,"duplicate seed refused without dropping point or diagnostics");
    for(auto point:{Point{-1,3},Point{100,3},Point{std::numeric_limits<double>::quiet_NaN(),3}}){invalid=seeds;invalid[0]=point;Check(!EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,invalid,Policy()).correction,"invalid document seed refused");}
    auto outside=Camera();outside.document_to_image[2]=100;const auto uncovered=EstimateImageColorCorrection(a,b,Camera(),outside,raster,seeds,Policy());Check(uncovered.outcome==ImageColorOutcome::SamplingFailed&&!uncovered.correction&&uncovered.points[0].outcome==ColorSampleOutcome::SampleUnavailable,"uncovered source prevents color fitting");
    const std::vector<Point> none;Check(EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,none,Policy()).correction->outcome==ColorCorrectionOutcome::InsufficientSamples,"empty seeds diagnosed");
    auto extended=seeds;extended[0]={20.25,3};Check(EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,extended,Policy()).outcome==ImageColorOutcome::Estimated,"ceil-raster extension is included in document domain");
    const std::vector<Point> too_many(kMaximumColorSamples+1,Point{3,3});Check(EstimateImageColorCorrection(a,b,Camera(),Camera(),raster,too_many,Policy()).outcome==ImageColorOutcome::TooManySeeds,"sampling work bounded");
    auto broken=a;broken.bgr.pop_back();Throws([&]{(void)EstimateImageColorCorrection(broken,b,Camera(),Camera(),raster,seeds,Policy());},"bad image buffer rejected");
    auto bad_camera=Camera();bad_camera.intrinsics.fx_pixels=0;Throws([&]{(void)EstimateImageColorCorrection(a,b,bad_camera,Camera(),raster,seeds,Policy());},"invalid base projection rejected");auto bad_raster=raster;++bad_raster.width_pixels;Throws([&]{(void)EstimateImageColorCorrection(a,b,Camera(),Camera(),bad_raster,seeds,Policy());},"inconsistent raster rejected");
    Check(a.bgr==original_a&&b.bgr==original_b,"source buffers unchanged");
    const MappedRenderParameters mapped{4,2,[](Point p,Point& raw){raw=p;return true;},[](Point p,Point& raw){raw=p;return true;},StitchLayout::camera_a_left_camera_b_right,Resampling::bilinear};
    for(double value:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})Throws([&]{(void)RenderMappedPair(a,b,mapped,BgrAffineTransform{{value,1,1},{0,0,0}});},"renderer refuses invalid gain");
    Throws([&]{(void)RenderMappedPair(a,b,mapped,BgrAffineTransform{{1,1,1},{0,std::numeric_limits<double>::infinity(),0}});},"renderer refuses nonfinite offset");
    Throws([&]{(void)RenderMappedPair(a,b,mapped,BgrAffineTransform{{std::numeric_limits<double>::max(),1,1},{0,0,0}});},"unrepresentable correction rejected before quantization");
    const auto huge=RenderMappedPair(a,b,mapped,BgrAffineTransform{{1e100,1,1},{0,0,0}});Check(huge.image.bgr[3]==255,"large finite DN quantizes without long overflow");
    auto only_a=mapped;only_a.camera_b=[](Point,Point& raw){raw={100,100};return true;};Check(RenderMappedPair(a,b,only_a,BgrAffineTransform{{1,1,1},{100,100,100}}).image.bgr==RenderMappedPair(a,b,only_a).image.bgr,"A-only pixels receive no B offset");
    auto only_b=mapped;only_b.camera_a=[](Point,Point& raw){raw={100,100};return true;};Check(RenderMappedPair(a,b,only_b,BgrAffineTransform{{2,1,1},{8,0,0}}).image.bgr[3]==90,"B-only pixels receive full affine color transform");
    auto flat_a=a,flat_b=b;std::fill(flat_a.bgr.begin(),flat_a.bgr.end(),std::uint8_t{100});std::fill(flat_b.bgr.begin(),flat_b.bgr.end(),std::uint8_t{200});const auto unclipped=RenderMappedPair(flat_a,flat_b,mapped,BgrAffineTransform{{2,2,2},{0,0,0}});Check(unclipped.image.bgr[3]==175,"corrected B overshoot kept through feathering until final quantization");
}
void GeometryAndColor(){
    const auto [a,unshifted]=Images();auto b=unshifted;
    // Independent stored B translation (+2,-1), and known analytic colors.
    for(int y=0;y<32;++y)for(int x=0;x<32;++x){const int u=x-2,v=y+1;const auto n=static_cast<std::size_t>(y*32+x)*3;b.bgr[n]=static_cast<std::uint8_t>(40+u+v);b.bgr[n+1]=static_cast<std::uint8_t>(50+2*u+v);b.bgr[n+2]=static_cast<std::uint8_t>(80+2*u+2*v);}
    std::vector<RawPixelCorrespondence> matches;for(auto p:Seeds())matches.push_back({p,{p.x+2,p.y-1}});
    const auto geometry=EstimateRawPairDocumentCorrection(Camera(),Camera(),raster,{32,32},{32,32},matches,{40,32,1e-9},{3,.1,.001,1e-8,10,1,1});
    Check(geometry.correction&&geometry.correction->corrected_camera,"geometric correction available before color");if(!geometry.correction||!geometry.correction->corrected_camera)return;
    const auto& aligned=*geometry.correction->corrected_camera;const auto color=EstimateImageColorCorrection(a,b,Camera(),aligned,raster,Seeds(),Policy());Check(color.correction&&color.correction->transform,"aligned colors fit after geometry");if(!color.correction||!color.correction->transform)return;
    for(auto kernel:{Resampling::bilinear,Resampling::bicubic_catmull_rom})for(auto layout:{StitchLayout::camera_a_left_camera_b_right,StitchLayout::camera_a_top_camera_b_bottom}){
        const DocumentRenderParameters p{raster,Camera(),aligned,layout,kernel};const auto combined=RenderDocumentPair(a,b,p,*color.correction->transform);
        const auto oracle=RenderDocumentPair(a,a,{raster,Camera(),Camera(),layout,kernel});Check(Error(combined.image,oracle.image)<.1,"H*T and color restore oracle with one original-image sampling");Check(Error(RenderDocumentPair(a,b,p).image,oracle.image)>10,"negative control exposes missing color correction");
    }
}
}
int main(){
    static_assert(!std::is_default_constructible_v<ColorCorrectionPolicy>);static_assert(!std::is_default_constructible_v<BgrAffineTransform>);
    try{Pure();ImagesAndRender();GeometryAndColor();}catch(const std::exception& e){std::cerr<<"unexpected: "<<e.what()<<'\n';return 2;}
    std::cout<<"color_correction checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
