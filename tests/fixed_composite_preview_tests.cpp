// Live C drawn with the fixed composite condition (#401): the condition scaled to
// view pixels, the blend width and brightness window at view scale, the fit of
// the seam column, the choice between the uncalibrated C, the fixed C and no C,
// and the standing texts. Camera and display free; files only in a temporary
// folder.
#include "a0/phase0/experimental/fixed_composite_preview.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
using namespace a0::m2::experimental;
namespace render=a0::m2::render;
namespace fs=std::filesystem;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
template<class Call> bool Throws(Call call){
    try{call();}catch(const std::invalid_argument&){return true;}catch(const std::runtime_error&){return true;}
    return false;
}
#define REJECT(expr) Check(Throws([&]{(void)(expr);}),"rejects: " #expr)

FixedCompositeCondition MakeCondition(int tx,int ty,unsigned w,unsigned h,MountOrientation orientation=MountOrientation::normal,double rotation=0,double scale=1){
    FixedCompositeCondition c;
    c.created_utc="2026-11-04T12:00:00Z";
    c.source.review_report_sha256=std::string(64,'a');c.source.pair_id="pair-1";
    c.source.method=(rotation==0 && scale==1)?FixedCompositeMethod::manual_still_pixels:FixedCompositeMethod::manual_still_similarity;
    c.orientation=orientation;
    c.source_width=w;c.source_height=h;
    c.mapping.translation_x=tx;c.mapping.translation_y=ty;c.mapping.rotation_degrees=rotation;c.mapping.scale=scale;
    const auto p=SimilarityReviewGeometry(tx,ty,rotation,scale,w,h);
    c.mapping.crop=p.crop;
    const auto plan=render::PlanPairCanvas(w,h,w,h,p);
    c.output_width=plan.output_width;c.output_height=plan.output_height;
    c.scale_auto_correction_max_percent=1;
    c.seam={ProvisionalSeamColumn(p,w,h),FixedSeamOrigin::band_center_provisional};
    ValidateFixedCompositeCondition(c);
    return c;
}
FixedCompositeCondition WithSeam(FixedCompositeCondition c,std::uint32_t column){
    c.seam={column,FixedSeamOrigin::operator_set};ValidateFixedCompositeCondition(c);return c;
}
render::BgrImage Flat(unsigned w,unsigned h,std::array<std::uint8_t,3> bgr){
    render::BgrImage image{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    for(size_t i=0;i<image.bgr.size();i+=3)std::copy(bgr.begin(),bgr.end(),image.bgr.begin()+static_cast<std::ptrdiff_t>(i));
    return image;
}
void Bar(render::BgrImage& image,unsigned x0,unsigned width){
    for(unsigned y=0;y<image.height;++y)for(unsigned x=x0;x<x0+width && x<image.width;++x)
        std::fill_n(image.bgr.begin()+static_cast<std::ptrdiff_t>((static_cast<size_t>(y)*image.width+x)*3),3,std::uint8_t{255});
}
// Area average of the source over each destination pixel: the live view of the capture under the assumed correspondence.
render::BgrImage Downscale(const render::BgrImage& source,unsigned w,unsigned h){
    render::BgrImage out{w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};
    const double fx=double(source.width)/w,fy=double(source.height)/h;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned x0=static_cast<unsigned>(std::floor(x*fx)),x1=std::max(x0+1,static_cast<unsigned>(std::ceil((x+1)*fx)));
        const unsigned y0=static_cast<unsigned>(std::floor(y*fy)),y1=std::max(y0+1,static_cast<unsigned>(std::ceil((y+1)*fy)));
        for(unsigned c=0;c<3;++c){
            double sum=0;unsigned n=0;
            for(unsigned v=y0;v<std::min(y1,source.height);++v)for(unsigned u=x0;u<std::min(x1,source.width);++u){sum+=source.bgr[(static_cast<size_t>(v)*source.width+u)*3+c];++n;}
            out.bgr[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(std::lround(sum/n));
        }
    }
    return out;
}
// Mean output column (pixel centers) of the pixels whose blue exceeds `level`, over all rows.
double BarCentroid(const render::BgrImage& image,unsigned level){
    double sum=0;std::uint64_t n=0;
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x)
        if(image.bgr[(static_cast<size_t>(y)*image.width+x)*3]>level){sum+=x+0.5;++n;}
    return n?sum/double(n):-1;
}
// Global column of output column 0: the origin of the canvas plus the crop.
double GlobalOrigin(const render::PairRenderParameters& p,unsigned w,unsigned h){
    return render::PlanPairCanvas(w,h,w,h,p).origin_x+p.crop.left;
}
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}
}

int main(){
    try{
        const unsigned w=1840,h=1228;
        const auto base=MakeCondition(930,9,w,h);
        const auto capture=FixedCompositeRenderParameters(base);
        const auto design=kSeamBlendDesign;

        // --- Blend width and brightness window at view scale.
        {
            const auto same=ScaleSeamBlendSettings(design,1,1);
            CHECK(same.width_pixels==8 && same.edge_margin_pixels==64 && same.brightness_half_width_pixels==32 && same.brightness_smoothing_rows==128);
            const auto small=ScaleSeamBlendSettings(design,160.0/1840,107.0/1228);
            CHECK(small.width_pixels==1);               // 8 px is 0.7 view px: the hard switch of one pixel
            CHECK(small.brightness_half_width_pixels==3); // 32 * 0.087 = 2.8
            CHECK(small.brightness_smoothing_rows==11);   // 128 * 0.087 = 11.2
            CHECK(small.edge_margin_pixels==6);
            const auto half=ScaleSeamBlendSettings(design,0.5,0.5);
            CHECK(half.width_pixels==4 && half.brightness_half_width_pixels==16 && half.brightness_smoothing_rows==64);
            const auto tiny=ScaleSeamBlendSettings(design,0.001,0.001);
            CHECK(tiny.width_pixels==1 && tiny.brightness_half_width_pixels==1 && tiny.brightness_smoothing_rows==1 && tiny.edge_margin_pixels==0);
            REJECT(ScaleSeamBlendSettings(design,0,1));
            REJECT(ScaleSeamBlendSettings(design,1,-1));
            REJECT(ScaleSeamBlendSettings(design,std::nan(""),1));
        }

        // --- Geometry: the view pair is placed where the capture places it, scaled.
        struct View {unsigned w,h;};
        const std::vector<View> views{{640,427},{512,384},{400,267},{320,214},{160,107}};
        {
            auto a=Flat(w,h,{100,100,100}),b=Flat(w,h,{50,200,100});
            Bar(b,700,24);
            const auto original=RenderPairFixedSeam(a,b,capture,base.seam.column,kSeamBlendDesign);
            const double origin=GlobalOrigin(capture,w,h);
            const double bar_global=BarCentroid(original.image,110)+origin;
            const double seam_global=origin+base.seam.column;
            CHECK(bar_global>seam_global+20); // the bar is on B's side
            CHECK(original.record.seam_x.front()==base.seam.column && original.record.fixed_seam);
            for(const auto& v:views){
                const auto plan=PlanFixedPreview(base,MountOrientation::normal,v.w,v.h);
                CHECK(!plan.skip);
                if(plan.skip)continue;
                const double sx=double(v.w)/w,sy=double(v.h)/h;
                const auto& m=plan.view.parameters.camera_b_to_camera_a;const auto& n=capture.camera_b_to_camera_a;
                CHECK(Near(m[2],n[2]*sx,1e-9) && Near(m[5],n[5]*sy,1e-9) && m[0]==n[0] && m[4]==n[4] && m[1]==0 && m[3]==0);
                CHECK(plan.output_width>0 && plan.output_height>0);
                const auto a_view=Downscale(a,v.w,v.h),b_view=Downscale(b,v.w,v.h);
                const auto drawn=RenderFixedPreview(plan,a_view,b_view);
                CHECK(drawn.image.width==plan.output_width && drawn.image.height==plan.output_height);
                CHECK(drawn.record.fixed_seam && drawn.record.seam_x.size()==plan.output_height);
                CHECK(std::all_of(drawn.record.seam_x.begin(),drawn.record.seam_x.end(),[&](std::uint32_t x){return x==plan.view.seam_column;}));
                const double view_origin=GlobalOrigin(plan.view.parameters,v.w,v.h);
                // The seam: the scaled global column, to the rounding of the scaled condition plus the fit shift.
                const double seam_view_global=view_origin+plan.view.seam_column;
                CHECK(Near(seam_view_global,sx*seam_global,0.5+std::abs(plan.seam_shift)+0.01));
                CHECK(std::abs(plan.seam_shift)<=static_cast<int>(kFixedPreviewSeamFitPixels));
                // B's placement: the bar lands at the scaled global column.
                const double bar_view=BarCentroid(drawn.image,110)+view_origin;
                CHECK(Near(bar_view,sx*bar_global,1.0));
                // The output size follows the scale to the rounding of the crop edges.
                CHECK(Near(plan.output_width,sx*base.output_width,4) && Near(plan.output_height,sy*base.output_height,4));
                // Brightness is measured in the call from the frames: the flat ratio is A/B luminance either way.
                CHECK(Near(drawn.record.ratio_minimum,original.record.ratio_minimum,0.02) && Near(drawn.record.ratio_maximum,original.record.ratio_maximum,0.02));
                CHECK(plan.settings.width_pixels==ScaleSeamBlendSettings(design,sx,sy).width_pixels);
            }
        }

        // --- A column that fits the capture also fits in C (within the documented shift).
        const auto sweep=[&](const FixedCompositeCondition& base_condition,const char* name){
            const auto cw=base_condition.source_width,ch=base_condition.source_height;
            const auto cap=FixedCompositeRenderParameters(base_condition);
            const auto band=FixedCompositeOverlapBand(cap,cw,ch);
            const render::BgrImage blank{cw,ch,std::vector<std::uint8_t>(static_cast<size_t>(cw)*ch*3)};
            const auto fits_capture=[&](std::uint32_t column){return CanRenderFixedSeam(blank,blank,cap,column,kSeamBlendDesign);};
            std::uint32_t lowest=band.first,highest=band.last_exclusive-1;
            while(lowest<highest && !fits_capture(lowest))++lowest;
            while(highest>lowest && !fits_capture(highest))--highest;
            CHECK(lowest<highest && fits_capture(lowest) && fits_capture(highest) && !fits_capture(lowest-1) && !fits_capture(highest+1));
            const int half=(static_cast<int>(kSeamBlendDesign.width_pixels)-1)/2;
            CHECK(lowest>=band.first+half && highest+half<band.last_exclusive); // the capture itself keeps the half blend width inside
            int worst=0;unsigned planned=0;
            std::vector<std::uint32_t> columns;
            for(std::uint32_t c=lowest;c<lowest+6;++c)columns.push_back(c);
            for(std::uint32_t c=highest-5;c<=highest;++c)columns.push_back(c);
            columns.push_back((lowest+highest)/2);
            for(const auto& v:views)for(const auto column:columns){
                const auto condition=WithSeam(base_condition,column);
                const auto plan=PlanFixedPreview(condition,MountOrientation::normal,v.w,v.h);
                CHECK(!plan.skip);
                if(plan.skip){std::cerr<<"  column "<<column<<" view "<<v.w<<"x"<<v.h<<": "<<plan.detail<<'\n';continue;}
                ++planned;worst=std::max(worst,std::abs(plan.seam_shift));
                CHECK(CanRenderFixedSeam(Flat(v.w,v.h,{0,0,0}),Flat(v.w,v.h,{0,0,0}),plan.view.parameters,plan.view.seam_column,plan.settings));
            }
            std::cout<<"fit sweep ("<<name<<"): "<<planned<<" plans, largest seam shift "<<worst<<" view pixels\n";
            CHECK(planned==views.size()*columns.size() && worst<=static_cast<int>(kFixedPreviewSeamFitPixels));
            // The band middle (the provisional column) needs no shift.
            for(const auto& v:views)CHECK(PlanFixedPreview(base_condition,MountOrientation::normal,v.w,v.h).seam_shift==0);
            // A column far outside the overlap is refused at every scale; C is not drawn.
            for(const auto& v:views)for(const std::uint32_t column:{std::uint32_t{0},band.first/2,base_condition.output_width-1}){
                const auto plan=PlanFixedPreview(WithSeam(base_condition,column),MountOrientation::normal,v.w,v.h);
                CHECK(plan.skip && *plan.skip==FixedCompositeSkip::seam_outside_overlap);
                if(!plan.skip || *plan.skip!=FixedCompositeSkip::seam_outside_overlap)std::cerr<<"  ("<<name<<") column "<<column<<" of "<<base_condition.output_width<<" (band "<<band.first<<".."<<band.last_exclusive<<") view "<<v.w<<"x"<<v.h<<": "<<(plan.skip?std::string(FixedCompositeSkipCode(*plan.skip)):std::string("drawn"))<<" "<<plan.detail<<'\n';
            }
        };
        sweep(base,"translation only");
        sweep(MakeCondition(1480,-14,2400,1600,MountOrientation::normal,0.4,1.004),"rotation and scale in the mapping");

        // --- Refusals of the plan.
        {
            const auto turned=PlanFixedPreview(base,MountOrientation::rotated_180,320,214);
            CHECK(turned.skip && *turned.skip==FixedCompositeSkip::orientation_differs);
            const auto both=PlanFixedPreview(MakeCondition(930,9,w,h,MountOrientation::rotated_180),MountOrientation::rotated_180,320,214);
            CHECK(!both.skip);
            for(const auto& v:{View{0,214},View{320,0},View{641,214},View{320,641}}){
                const auto plan=PlanFixedPreview(base,MountOrientation::normal,v.w,v.h);
                CHECK(plan.skip && *plan.skip==FixedCompositeSkip::size_differs);
            }
            FixedPreviewPlan none;none.skip=FixedCompositeSkip::size_differs;
            const auto flat=Flat(320,214,{1,2,3});
            REJECT(RenderFixedPreview(none,flat,flat));
            const auto plan=PlanFixedPreview(base,MountOrientation::normal,320,214);
            REJECT(RenderFixedPreview(plan,Flat(321,214,{1,2,3}),flat));
            REJECT(RenderFixedPreview(plan,flat,Flat(320,213,{1,2,3})));
            auto broken=flat;broken.bgr.pop_back();
            REJECT(RenderFixedPreview(plan,broken,flat));
        }

        // --- The frames change the brightness ratio and nothing else.
        {
            const auto plan=PlanFixedPreview(base,MountOrientation::normal,400,267);
            CHECK(!plan.skip);
            const auto a=Flat(400,267,{100,100,100});
            const auto dark=RenderFixedPreview(plan,a,Flat(400,267,{50,50,50}));
            const auto bright=RenderFixedPreview(plan,a,Flat(400,267,{200,200,200}));
            const auto again=RenderFixedPreview(plan,a,Flat(400,267,{200,200,200}));
            CHECK(dark.record.seam_x==bright.record.seam_x && dark.image.width==bright.image.width && dark.image.height==bright.image.height);
            CHECK(Near(dark.record.ratio_maximum,2,0.05) && Near(bright.record.ratio_minimum,0.5,0.05));
            CHECK(bright.image.bgr==again.image.bgr && bright.record.seam_x==again.record.seam_x); // deterministic
            // B is matched to A in both cases, beside the seam and, since the global gain stays (#415), at the far edge.
            const auto at=[&](const SeamBlendResult& r,unsigned x,unsigned y,unsigned c){return r.image.bgr[(static_cast<size_t>(y)*r.image.width+x)*3+c];};
            const unsigned row=plan.output_height/2,beside=plan.view.seam_column+plan.settings.width_pixels+1;
            CHECK(Near(at(dark,beside,row,0),100,6) && Near(at(bright,beside,row,0),100,6));
            CHECK(Near(at(dark,plan.output_width-1,row,0),100,6) && Near(at(bright,plan.output_width-1,row,0),100,6));
            // The inputs are untouched.
            CHECK(a.bgr==Flat(400,267,{100,100,100}).bgr);
        }

        // --- What the host reads and chooses.
        {
            const auto hash=[](const std::vector<unsigned char>&){return std::string(64,'b');};
            const auto folder=fs::current_path()/("fixed-composite-preview-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(folder);
            const auto file=folder/kFixedCompositeConditionName;
            const auto absent=LoadFixedPreviewSource(file,hash);
            CHECK(!absent.loaded && absent.reason==FixedCompositeSkip::condition_absent);
            CHECK(ChoosePreviewC(absent,MountOrientation::normal,320,214).mode==PreviewCMode::uncalibrated);
            CHECK(ChoosePreviewC(absent,MountOrientation::normal,320,214).plan.view_width==320 && !ChoosePreviewC(absent,MountOrientation::normal,320,214).plan.skip);
            WriteFixedCompositeCondition(file,base);
            const auto present=LoadFixedPreviewSource(file,hash);
            CHECK(present.loaded && present.loaded->condition.seam.column==base.seam.column && present.loaded->file_sha256==std::string(64,'b'));
            const auto fixed=ChoosePreviewC(present,MountOrientation::normal,320,214);
            CHECK(fixed.mode==PreviewCMode::fixed_composite && !fixed.plan.skip && fixed.plan.view_width==320);
            const auto other=ChoosePreviewC(present,MountOrientation::rotated_180,320,214);
            CHECK(other.mode==PreviewCMode::unavailable && other.plan.skip && *other.plan.skip==FixedCompositeSkip::orientation_differs);
            CHECK(ChoosePreviewC(present,MountOrientation::normal,641,214).mode==PreviewCMode::unavailable);
            // The condition is read again for each session: a new file is picked up, a damaged one is not mistaken for absence.
            for(const char* text:{"","{","not json","{\"schema\":\"a0.fixed-composite-condition.v2\"}"}){
                {std::ofstream f(file,std::ios::binary|std::ios::trunc);f<<text;}
                const auto damaged=LoadFixedPreviewSource(file,hash);
                CHECK(!damaged.loaded && damaged.reason==FixedCompositeSkip::condition_invalid && !damaged.detail.empty());
                const auto choice=ChoosePreviewC(damaged,MountOrientation::normal,320,214);
                CHECK(choice.mode==PreviewCMode::unavailable && choice.plan.skip && *choice.plan.skip==FixedCompositeSkip::condition_invalid);
            }
            fs::remove(file);fs::create_directory(file);
            const auto directory=LoadFixedPreviewSource(file,hash);
            CHECK(!directory.loaded && directory.reason==FixedCompositeSkip::condition_invalid);
            fs::remove_all(folder);
        }

        // --- Texts: the unverified correspondence is always named; nothing claims approval, sync or quality.
        {
            auto plan=PlanFixedPreview(base,MountOrientation::normal,320,214);
            CHECK(!plan.skip);
            const auto title=FixedPreviewTitle(MountOrientation::normal,plan);
            CHECK(title.find(L"未校正")!=std::wstring::npos && title.find(L"未承認")!=std::wstring::npos && title.find(L"表示専用")!=std::wstring::npos && title.find(L"通常")!=std::wstring::npos);
            CHECK(title.find(std::to_wstring(base.seam.column))!=std::wstring::npos && title.find(L"帯の中央の仮の値")!=std::wstring::npos);
            CHECK(FixedPreviewTitle(MountOrientation::rotated_180,plan).find(L"180°回転")!=std::wstring::npos);
            plan.seam_origin=FixedSeamOrigin::operator_set;
            CHECK(FixedPreviewTitle(MountOrientation::normal,plan).find(L"人が決めた値")!=std::wstring::npos);
            REJECT(FixedPreviewTitle(static_cast<MountOrientation>(7),plan));
            auto note=FixedPreviewNote(plan);
            CHECK(note.find(L"※未検証")!=std::wstring::npos && note.find(L"#309")!=std::wstring::npos && note.find(L"倍率は補正しません")!=std::wstring::npos);
            CHECK(note.find(L"寄せています")==std::wstring::npos);
            plan.seam_shift=-2;
            note=FixedPreviewNote(plan);
            CHECK(note.find(L"2px 左へ寄せています")!=std::wstring::npos);
            plan.seam_shift=1;
            CHECK(FixedPreviewNote(plan).find(L"1px 右へ寄せています")!=std::wstring::npos);
            const auto refusal=FixedPreviewPositionRefusal();
            CHECK(refusal.find(L"変えません")!=std::wstring::npos && refusal.find(L"※未検証")!=std::wstring::npos);
            std::vector<std::wstring> reasons;
            for(const auto skip:{FixedCompositeSkip::condition_invalid,FixedCompositeSkip::orientation_differs,FixedCompositeSkip::size_differs,FixedCompositeSkip::seam_outside_overlap,FixedCompositeSkip::profile_absent,FixedCompositeSkip::profile_differs}){
                const auto text=FixedPreviewUnavailableText(skip);
                CHECK(text.find(L"Cを描きません")!=std::wstring::npos);
                CHECK(std::find(reasons.begin(),reasons.end(),text)==reasons.end());
                reasons.push_back(text);
            }
            CHECK(std::wstring(kUncalibratedPreviewNotice).find(L"固定の合成条件がありません")!=std::wstring::npos && std::wstring(kUncalibratedPreviewNotice).find(L"未校正の仮の C")!=std::wstring::npos);
            CHECK(FixedPreviewUnavailableText(FixedCompositeSkip::condition_absent)==kUncalibratedPreviewNotice);
            const std::vector<std::wstring> standing{title,note,refusal};
            for(const auto& text:standing)
                CHECK(text.find(L"同期")==std::wstring::npos && text.find(L"同時")==std::wstring::npos && text.find(L"合格")==std::wstring::npos);
        }
    }catch(const std::exception& error){
        std::cerr<<"unexpected exception: "<<error.what()<<'\n';++failures;
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
