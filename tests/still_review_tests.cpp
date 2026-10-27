#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/review_session.hpp"
#include "a0/m2/offline_stitcher.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/experimental/preview_merge_policy.hpp"
#include "a0/phase0/experimental/still_alignment_text.hpp"
#include "similarity_scene.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <iostream>
using namespace a0::m2::experimental;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace a0::m2::detail {void SetBeforePartialFlushTestHook(void (*)()) noexcept;}
StillReviewRequest* expiring_request{};
void ExpireReview(){expiring_request->deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);}
void FailSessionCommit(){throw std::runtime_error("injected session persistence failure");}
fs::path parent_lock_probe;
bool parent_write_denied{};
void ProbeParentWriteLock(){
    const auto file=CreateFileW(parent_lock_probe.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    parent_write_denied=file==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file); // Probe only, never alter bytes.
}
void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("fixture WIC failure");}
// Left 32 columns use left_bgr, the rest right_bgr; uniform fixtures pass both equal.
void HalfFixture(const fs::path& path,std::array<BYTE,3> left_bgr,std::array<BYTE,3> right_bgr){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(64,48));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));std::vector<BYTE> pixels(64*48*3);
    for(size_t i=0;i<pixels.size();i+=3){const auto& bgr=(i/3)%64<32?left_bgr:right_bgr;std::copy(bgr.begin(),bgr.end(),pixels.begin()+i);}
    Hr(frame->WritePixels(48,64*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
void Fixture(const fs::path& path,unsigned char red,unsigned char blue){HalfFixture(path,{blue,30,red},{blue,30,red});}
void WriteJpeg(const fs::path& path,const a0::m2::render::BgrImage& image){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(image.width,image.height));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));auto pixels=image.bgr;
    Hr(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
}
std::vector<unsigned char> Read(const fs::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    auto reject=[&](auto call){bool threw=false;try{call();}catch(const std::exception&){threw=true;}check(threw);};
    try{
        auto p=ScalePreviewReviewGeometry({16,12,8,1,9},64,48);check(p.camera_b_to_camera_a[2]==32 && p.camera_b_to_camera_a[5]==4 && p.crop.top==4 && p.crop.bottom==4);
        p=ScalePreviewReviewGeometry({16,12,8,-1,9},64,48);check(p.camera_b_to_camera_a[5]==-4 && p.crop.top==4);
        reject([&]{ScalePreviewReviewGeometry({0,12,8,1,0},64,48);});reject([&]{ScalePreviewReviewGeometry({16,12,16,1,0},64,48);});reject([&]{ScalePreviewReviewGeometry({16,12,8,12,0},64,48);});
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        const auto root=fs::current_path()/("still-review-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        fs::create_directories(root/"CAM-A");fs::create_directories(root/"CAM-B");Fixture(root/"CAM-A/original.jpg",220,10);Fixture(root/"CAM-B/original.jpg",10,220);
        const auto a=Read(root/"CAM-A/original.jpg"),b=Read(root/"CAM-B/original.jpg");
        StillReviewRequest r{root/"CAM-A/original.jpg",root/"CAM-B/original.jpg",root/"review",a0::phase0::Sha256Hex(a),a0::phase0::Sha256Hex(b),"synthetic-pair-1",{16,12,8,1,9},std::chrono::steady_clock::now()+std::chrono::seconds(20)};
        const auto output=WriteUncalibratedStillReview(r);check(output.width==96 && output.height==44 && fs::exists(output.jpeg) && fs::exists(output.report));
        check(output.sha256==a0::phase0::Sha256Hex(Read(output.jpeg)));check(output.thumbnail.width==96 && output.thumbnail.height==44);
        check(output.thumbnail.bgr[2]>output.thumbnail.bgr[0]+100);const auto right=(96-1)*3;check(output.thumbnail.bgr[right]>output.thumbnail.bgr[right+2]+100);
        const auto report_bytes=Read(output.report);const std::string report(report_bytes.begin(),report_bytes.end());
        check(report.find("\"productQualityAccepted\":false")!=std::string::npos && report.find("\"liveToStillFieldOfViewVerified\":false")!=std::string::npos && report.find(r.camera_a_sha256)!=std::string::npos && report.find(r.camera_b_sha256)!=std::string::npos);
        check(Read(r.camera_a_original)==a && Read(r.camera_b_original)==b);reject([&]{WriteUncalibratedStillReview(r);});check(Read(output.jpeg).size()>0);
        const auto original_jpeg=Read(output.jpeg);
        const auto saved=LoadSavedStillReview(output.report);const auto full=LoadStillReviewImage(saved);
        check(saved.pair_id==r.pair_id && saved.translation_x==32 && saved.translation_y==4 && !saved.manual_revision);
        // Normal reports keep their earlier bytes: no orientation field, read as normal.
        check(saved.geometry.orientation==MountOrientation::normal && report.find("mountOrientation")==std::string::npos);
        const auto evaluation_inputs=DescribeSavedReviewEvaluationInputs(output.report);
        check(evaluation_inputs.find("\"mountOrientation\":\"normal\"")!=std::string::npos);
        check(evaluation_inputs.find("a0.saved-review-evaluation-inputs.v1")!=std::string::npos);
        check(evaluation_inputs.find(saved.report_sha256)!=std::string::npos && evaluation_inputs.find(r.camera_a_sha256)!=std::string::npos);
        check(evaluation_inputs.find("\"productApproved\":false")!=std::string::npos);
        check(full.width==96 && full.height==44 && saved.report_sha256==a0::phase0::Sha256Hex(report_bytes));
        auto revised=r;revised.output_directory=root/"revision";
        revised.manual_revision=StillReviewRequest::ManualRevision{33,3,output.report,saved.report_sha256};
        parent_lock_probe=output.report;a0::m2::detail::SetBeforePartialFlushTestHook(ProbeParentWriteLock);
        const auto revision=WriteUncalibratedStillReview(revised);a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);
        check(parent_write_denied);const auto child=LoadSavedStillReview(revision.report);
        check(child.width==97 && child.height==45 && child.translation_x==33 && child.translation_y==3);
        check(child.manual_revision && child.manual_revision->parent_report==output.report && child.manual_revision->parent_report_sha256==saved.report_sha256);
        check(DescribeSavedReviewEvaluationInputs(revision.report).find(saved.report_sha256)!=std::string::npos);
        check(child.pair_id==saved.pair_id && child.camera_a_sha256==saved.camera_a_sha256 && child.camera_b_sha256==saved.camera_b_sha256);
        auto next_revision=revised;next_revision.output_directory=root/"revision-2";
        next_revision.manual_revision=StillReviewRequest::ManualRevision{31,-2,revision.report,child.report_sha256};
        const auto grandchild=LoadSavedStillReview(WriteUncalibratedStillReview(next_revision).report);
        check(grandchild.translation_x==31 && grandchild.translation_y==-2 && grandchild.manual_revision->parent_report==revision.report);
        check(Read(output.report)==report_bytes && Read(output.jpeg)==original_jpeg && Read(r.camera_a_original)==a && Read(r.camera_b_original)==b);
        revised.output_directory=root/"bad-parent";revised.manual_revision->parent_report_sha256=std::string(64,'0');
        reject([&]{WriteUncalibratedStillReview(revised);});check(!fs::exists(revised.output_directory));
        revised.output_directory=root/"wrong-pair";revised.manual_revision->parent_report_sha256=saved.report_sha256;revised.pair_id="other-pair";
        reject([&]{WriteUncalibratedStillReview(revised);});check(!fs::exists(revised.output_directory));
        revised=r;revised.output_directory=root/"outside-image";
        revised.manual_revision=StillReviewRequest::ManualRevision{64,0,output.report,saved.report_sha256};
        reject([&]{WriteUncalibratedStillReview(revised);});check(!fs::exists(revised.output_directory));
        // Untrusted reports must fail closed before the inspector displays them.
        const auto damaged=root/"damaged";fs::create_directory(damaged);
        fs::copy_file(output.jpeg,damaged/"uncalibrated-review.jpg");
        auto reject_report=[&](std::string text){
            {std::ofstream f(damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<text;}
            reject([&]{LoadSavedStillReview(damaged/"uncalibrated-review.json");});
            reject([&]{DescribeSavedReviewEvaluationInputs(damaged/"uncalibrated-review.json");});
        };
        reject_report("{\"schema\":\"a0.uncalibrated-still-review.v1\","+report.substr(1));
        reject_report("{\"unknown\":0,"+report.substr(1));
        auto replace_report=[&](std::string from,std::string to){auto text=report;const auto at=text.find(from);if(at==std::string::npos)throw std::runtime_error("test report token absent");text.replace(at,from.size(),to);reject_report(text);};
        replace_report("\"productQualityAccepted\":false","\"productQualityAccepted\":true");
        replace_report("\"alias\":\"CAM-A\"","\"alias\":\"CAM-B\"");
        replace_report(r.camera_a_sha256,std::string(64,'0'));
        replace_report("\"translation\":{\"x\":32","\"translation\":{\"x\":33");
        replace_report("\"width\":96","\"width\":95");
        {std::ofstream f(damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<report;}
        {std::ofstream f(damaged/"uncalibrated-review.jpg",std::ios::binary|std::ios::app);f.put('x');}
        reject([&]{LoadSavedStillReview(damaged/"uncalibrated-review.json");});
        {std::ofstream f(output.report,std::ios::binary|std::ios::app);f.put(' ');}
        reject([&]{LoadSavedStillReview(revision.report);});
        {std::ofstream f(output.report,std::ios::binary|std::ios::trunc);f<<report;}
        check(LoadSavedStillReview(revision.report).pair_id==r.pair_id);
        auto similarity=r;similarity.output_directory=root/"similarity";
        similarity.manual_revision=StillReviewRequest::ManualRevision{32,0,output.report,saved.report_sha256,1,1.125};
        const auto similarity_output=WriteUncalibratedStillReview(similarity);const auto similarity_record=LoadSavedStillReview(similarity_output.report);
        check(similarity_record.rotation_degrees==1 && similarity_record.scale==1.125 && similarity_record.manual_revision->rotation_degrees==1);
        check(DescribeSavedReviewEvaluationInputs(similarity_output.report).find(similarity_record.report_sha256)!=std::string::npos);
        const auto source_images=LoadStillReviewSources(similarity_record);check(source_images[0].width==64 && source_images[1].height==48);
        check(LoadStillReviewImage(similarity_record).width==similarity_output.width);
        const auto similarity_bytes=Read(similarity_output.report);const std::string similarity_text(similarity_bytes.begin(),similarity_bytes.end());
        check(similarity_text.find("a0.uncalibrated-still-review.v3")!=std::string::npos && similarity_text.find("manual-still-similarity")!=std::string::npos);
        fs::copy_file(similarity_output.jpeg,damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
        {std::ofstream f(damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<similarity_text;}
        check(LoadSavedStillReview(damaged/"uncalibrated-review.json").scale==1.125);
        auto damage_similarity=[&](std::string from,std::string to){auto text=similarity_text;const auto at=text.find(from);if(at==std::string::npos)throw std::runtime_error("similarity token absent");text.replace(at,from.size(),to);reject_report(text);};
        damage_similarity("\"scale\":1.125","\"scale\":0");
        damage_similarity("\"rotationDegrees\":1","\"rotationDegrees\":181");
        damage_similarity("\"anchor\":\"image-center\"","\"anchor\":\"origin\"");
        damage_similarity("\"left\":0","\"left\":1");
        damage_similarity("\"productQualityAccepted\":false","\"productQualityAccepted\":true");
        check(Read(output.report)==report_bytes && Read(output.jpeg)==original_jpeg && Read(r.camera_a_original)==a && Read(r.camera_b_original)==b);
        ReviewSessionState work{output.report,saved.report_sha256,32,-1,.1256789012345678,1.0123456789012345,
            {{{37,10},{5,10}},{{47,20},{15,20}},{{41,33},{5,30},ReviewPointRole::validation}},ReviewPointRole::validation};
        parent_write_denied=false;a0::m2::detail::SetBeforePartialFlushTestHook(ProbeParentWriteLock);
        const auto session=WriteReviewSession(work,root/L"session-\u65e5\u672c\u8a9e",std::chrono::steady_clock::now()+std::chrono::seconds(20));
        a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);check(parent_write_denied);
        const auto work_bytes=Read(session.file);const std::string work_text(work_bytes.begin(),work_bytes.end());
        auto loaded_work=LoadReviewSession(session.file);
        check(loaded_work.sha256==a0::phase0::Sha256Hex(work_bytes) && loaded_work.review.pair_id==saved.pair_id && loaded_work.state.review_report_sha256==saved.report_sha256);
        check(loaded_work.state.x==32 && loaded_work.state.y==-1 && loaded_work.state.degrees==work.degrees && loaded_work.state.scale==work.scale);
        check(loaded_work.state.next_point_role==ReviewPointRole::validation && loaded_work.state.points.size()==3 && loaded_work.state.points[2].role==ReviewPointRole::validation && loaded_work.state.points[2].camera_a.x==41);
        check(ReviewSessionBelongsToPair(loaded_work,saved) && ReviewSessionBelongsToPair(loaded_work,child));
        auto other_pair=saved;other_pair.pair_id="different-pair";check(!ReviewSessionBelongsToPair(loaded_work,other_pair));
        other_pair=saved;other_pair.camera_b_sha256=std::string(64,'0');check(!ReviewSessionBelongsToPair(loaded_work,other_pair));
        other_pair=saved;other_pair.geometry.orientation=MountOrientation::rotated_180;check(!ReviewSessionBelongsToPair(loaded_work,other_pair));
        auto empty_work=work;empty_work.points.clear();empty_work.next_point_role=ReviewPointRole::fitting;
        const auto empty_saved=WriteReviewSession(empty_work,root/"work-empty",std::chrono::steady_clock::now()+std::chrono::seconds(20));
        check(LoadReviewSession(empty_saved.file).state.points.empty() && empty_saved.state.next_point_role==ReviewPointRole::fitting);
        empty_work.points.push_back(work.points.front());const auto one_saved=WriteReviewSession(empty_work,root/"work-one",std::chrono::steady_clock::now()+std::chrono::seconds(20));
        check(LoadReviewSession(one_saved.file).state.points.size()==1);
        const auto before_work=MeasureReviewCorrespondences(work.points,SimilarityReviewGeometry(work.x,work.y,work.degrees,work.scale,64,48),64,48,ReviewPointRole::validation);
        const auto after_work=MeasureReviewCorrespondences(loaded_work.state.points,SimilarityReviewGeometry(loaded_work.state.x,loaded_work.state.y,loaded_work.state.degrees,loaded_work.state.scale,64,48),64,48,ReviewPointRole::validation);
        check(before_work.rms==after_work.rms && before_work.maximum==after_work.maximum);
        reject([&]{WriteReviewSession(work,session.file.parent_path(),std::chrono::steady_clock::now()+std::chrono::seconds(20));});check(Read(session.file)==work_bytes);
        auto wrong_work=work;wrong_work.review_report_sha256=std::string(64,'0');
        reject([&]{WriteReviewSession(wrong_work,root/"work-wrong-report",std::chrono::steady_clock::now()+std::chrono::seconds(20));});check(!fs::exists(root/"work-wrong-report"));
        wrong_work=work;wrong_work.points[0].camera_a.x=NAN;reject([&]{WriteReviewSession(wrong_work,root/"work-nan",std::chrono::steady_clock::now()+std::chrono::seconds(20));});
        wrong_work=work;wrong_work.next_point_role=static_cast<ReviewPointRole>(2);reject([&]{WriteReviewSession(wrong_work,root/"work-role",std::chrono::steady_clock::now()+std::chrono::seconds(20));});
        reject([&]{WriteReviewSession(work,root/"work-expired",std::chrono::steady_clock::now()-std::chrono::seconds(1));});check(!fs::exists(root/"work-expired"));
        a0::m2::detail::SetBeforePartialFlushTestHook(FailSessionCommit);
        reject([&]{WriteReviewSession(work,root/"work-partial",std::chrono::steady_clock::now()+std::chrono::seconds(20));});a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);
        check(fs::exists(root/"work-partial/review-session.json.partial") && !fs::exists(root/"work-partial/review-session.json"));
        const auto damaged_work=root/"damaged-work";fs::create_directory(damaged_work);
        auto reject_work=[&](std::string text){
            {std::ofstream f(damaged_work/"review-session.json",std::ios::binary|std::ios::trunc);f<<text;}
            reject([&]{LoadReviewSession(damaged_work/"review-session.json");});
        };
        {std::ofstream f(damaged_work/"review-session.json",std::ios::binary|std::ios::trunc);f<<work_text;}
        check(LoadReviewSession(damaged_work/"review-session.json").state.points.size()==3);
        reject_work("{\"unknown\":0,"+work_text.substr(1));reject_work("{\"schema\":\"a0.still-review-session.v1\","+work_text.substr(1));
        auto damage_work=[&](std::string from,std::string to){auto text=work_text;const auto at=text.find(from);if(at==std::string::npos)throw std::runtime_error("work token absent");text.replace(at,from.size(),to);reject_work(text);};
        damage_work("\"productQualityAccepted\":false","\"productQualityAccepted\":true");damage_work("\"pairId\":\"synthetic-pair-1\"","\"pairId\":\"other-pair\"");
        damage_work(saved.report_sha256,std::string(64,'0'));damage_work(r.camera_a_sha256,std::string(64,'0'));
        damage_work("\"width\":64","\"width\":65");
        auto zero_scale=work_text;const auto scale_at=zero_scale.find("\"scale\":")+8;const auto scale_end=zero_scale.find_first_of(",}",scale_at);
        zero_scale.replace(scale_at,scale_end-scale_at,"0");reject_work(zero_scale);
        damage_work("\"nextPointRole\":\"validation\"","\"nextPointRole\":\"unknown\"");damage_work("\"role\":\"validation\"","\"role\":\"unknown\"");
        damage_work("\"a\":{\"x\":37","\"a\":{\"x\":64");
        damage_work("\"b\":{\"x\":5,\"y\":30}","\"b\":{\"x\":5,\"y\":10}");
        {std::ofstream f(output.report,std::ios::binary|std::ios::app);f.put(' ');}reject([&]{LoadReviewSession(session.file);});
        {std::ofstream f(output.report,std::ios::binary|std::ios::trunc);f<<report;}
        check(LoadReviewSession(session.file).state.points.size()==3);
        check(Read(output.report)==report_bytes && Read(output.jpeg)==original_jpeg && Read(r.camera_a_original)==a && Read(r.camera_b_original)==b && Read(session.file)==work_bytes);
        auto bad=r;bad.output_directory=root/"bad-hash";bad.camera_a_sha256=std::string(64,'0');reject([&]{WriteUncalibratedStillReview(bad);});check(!fs::exists(bad.output_directory));
        bad=r;bad.output_directory=root/"expired";bad.deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);reject([&]{WriteUncalibratedStillReview(bad);});check(!fs::exists(bad.output_directory));
        bad=r;bad.output_directory=root/"wrong-alias";std::swap(bad.camera_a_original,bad.camera_b_original);reject([&]{WriteUncalibratedStillReview(bad);});
        // The development API must not authorize the existing product API.
        a0::m2::OfflineStitchRequest product{};product.camera_a_original=r.camera_a_original;product.camera_b_original=r.camera_b_original;product.output_job_directory=root/"product";
        reject([&]{(void)a0::m2::StitchCanonicalPair(product);});check(!fs::exists(product.output_job_directory));
        bad=r;bad.output_directory=root/"expired-after-encode";expiring_request=&bad;
        a0::m2::detail::SetBeforePartialFlushTestHook(ExpireReview);reject([&]{WriteUncalibratedStillReview(bad);});a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);expiring_request=nullptr;
        check(fs::exists(bad.output_directory/"uncalibrated-review.jpg.partial") && !fs::exists(bad.output_directory/"uncalibrated-review.jpg") && !fs::exists(bad.output_directory/"uncalibrated-review.json"));
        check(Read(r.camera_a_original)==a && Read(r.camera_b_original)==b);
        fs::create_directories(root/"same-file/CAM-B");const auto hardlink=root/"same-file/CAM-B/original.jpg";
        if(!CreateHardLinkW(hardlink.c_str(),r.camera_a_original.c_str(),nullptr))throw std::runtime_error("fixture hardlink creation failed");
        bad=r;bad.output_directory=root/"hardlink-review";bad.camera_b_original=hardlink;bad.camera_b_sha256=r.camera_a_sha256;
        reject([&]{WriteUncalibratedStillReview(bad);});check(!fs::exists(bad.output_directory));
        // rotated-180 mounting: render inputs turn in memory, originals never do.
        const auto turned_root=root/"rotated";fs::create_directories(turned_root/"CAM-A");fs::create_directories(turned_root/"CAM-B");
        HalfFixture(turned_root/"CAM-A/original.jpg",{10,30,220},{10,220,10});   // A: red left, green right
        HalfFixture(turned_root/"CAM-B/original.jpg",{220,30,10},{200,200,200}); // B: blue left, grey right
        const auto ta=Read(turned_root/"CAM-A/original.jpg"),tb=Read(turned_root/"CAM-B/original.jpg");
        StillReviewRequest turned{turned_root/"CAM-A/original.jpg",turned_root/"CAM-B/original.jpg",turned_root/"review",a0::phase0::Sha256Hex(ta),a0::phase0::Sha256Hex(tb),
            "synthetic-pair-turned",{16,12,8,0,9,MountOrientation::rotated_180},std::chrono::steady_clock::now()+std::chrono::seconds(20)};
        auto pixel=[](const a0::m2::render::BgrImage& image,unsigned x,unsigned y){const auto i=(static_cast<size_t>(y)*image.width+x)*3;return std::array<int,3>{image.bgr[i],image.bgr[i+1],image.bgr[i+2]};};
        auto close_to=[](std::array<int,3> actual,std::array<int,3> expected){for(int c=0;c<3;++c)if(std::abs(actual[c]-expected[c])>40)return false;return true;};
        const auto turned_output=WriteUncalibratedStillReview(turned);check(turned_output.width==96 && turned_output.height==48);
        const auto turned_record=LoadSavedStillReview(turned_output.report);
        check(turned_record.geometry.orientation==MountOrientation::rotated_180 && turned_record.translation_x==32 && turned_record.translation_y==0);
        const auto turned_bytes=Read(turned_output.report);const std::string turned_text(turned_bytes.begin(),turned_bytes.end());
        check(turned_text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos && turned_text.find("a0.uncalibrated-still-review.v1")!=std::string::npos);
        const auto turned_image=LoadStillReviewImage(turned_record);
        check(close_to(pixel(turned_image,4,24),{10,220,10}) && close_to(pixel(turned_image,91,24),{220,30,10})); // raw A right / raw B left at the outer edges
        const auto turned_sources=LoadStillReviewSources(turned_record);
        check(close_to(pixel(turned_sources[0],4,24),{10,220,10}) && close_to(pixel(turned_sources[1],60,24),{220,30,10}));
        check(Read(turned.camera_a_original)==ta && Read(turned.camera_b_original)==tb && a0::phase0::Sha256Hex(Read(turned.camera_a_original))==turned.camera_a_sha256);
        check(DescribeSavedReviewEvaluationInputs(turned_output.report).find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
        auto upright=turned;upright.output_directory=turned_root/"upright";upright.geometry.orientation=MountOrientation::normal;
        const auto upright_record=LoadSavedStillReview(WriteUncalibratedStillReview(upright).report);
        check(upright_record.geometry.orientation==MountOrientation::normal && close_to(pixel(LoadStillReviewImage(upright_record),4,24),{10,30,220}));
        // A manual revision inherits the parent's orientation; another one is refused.
        auto turned_revision=turned;turned_revision.output_directory=turned_root/"revision";
        turned_revision.manual_revision=StillReviewRequest::ManualRevision{30,2,turned_output.report,turned_record.report_sha256};
        const auto turned_child_output=WriteUncalibratedStillReview(turned_revision);const auto turned_child=LoadSavedStillReview(turned_child_output.report);
        const auto child_bytes=Read(turned_child_output.report);const std::string child_text(child_bytes.begin(),child_bytes.end());
        check(turned_child.geometry.orientation==MountOrientation::rotated_180 && turned_child.translation_x==30 && child_text.find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
        check(close_to(pixel(LoadStillReviewImage(turned_child),4,24),{10,220,10}));
        auto flipped=turned_revision;flipped.output_directory=turned_root/"flipped";flipped.geometry.orientation=MountOrientation::normal;
        reject([&]{WriteUncalibratedStillReview(flipped);});check(!fs::exists(flipped.output_directory));
        auto turned_from_upright=r;turned_from_upright.output_directory=root/"turned-from-upright";turned_from_upright.geometry.orientation=MountOrientation::rotated_180;
        turned_from_upright.manual_revision=StillReviewRequest::ManualRevision{33,3,output.report,saved.report_sha256};
        reject([&]{WriteUncalibratedStillReview(turned_from_upright);});check(!fs::exists(turned_from_upright.output_directory));
        auto bad_turn=turned;bad_turn.output_directory=turned_root/"unknown";bad_turn.geometry.orientation=static_cast<MountOrientation>(7);
        reject([&]{WriteUncalibratedStillReview(bad_turn);});check(!fs::exists(bad_turn.output_directory));
        // Strict reader: only the canonical rotated value; a child whose field is
        // removed no longer matches its rotated parent.
        auto reject_in=[&](const fs::path& folder,const fs::path& jpeg,const std::string& text,bool valid){
            fs::create_directories(folder);fs::copy_file(jpeg,folder/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
            {std::ofstream f(folder/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<text;}
            if(valid){check(LoadSavedStillReview(folder/"uncalibrated-review.json").geometry.orientation==MountOrientation::rotated_180);return;}
            reject([&]{LoadSavedStillReview(folder/"uncalibrated-review.json");});reject([&]{DescribeSavedReviewEvaluationInputs(folder/"uncalibrated-review.json");});
        };
        const auto field=std::string("\"mountOrientation\":\"rotated-180\"");
        auto with=[&](const std::string& text,const std::string& to){auto changed=text;changed.replace(changed.find(field),field.size(),to);return changed;};
        const auto turned_damaged=turned_root/"damaged";reject_in(turned_damaged,turned_output.jpeg,turned_text,true);
        for(const auto& to:{std::string("\"mountOrientation\":\"normal\""),std::string("\"mountOrientation\":\"rotated-90\""),std::string("\"mountOrientation\":180"),
            std::string("\"mountOrientation\":\"\""),field+","+field,field+",\"extra\":0"})reject_in(turned_damaged,turned_output.jpeg,with(turned_text,to),false);
        const auto child_damaged=turned_root/"damaged-child";reject_in(child_damaged,turned_child_output.jpeg,child_text,true);
        auto stripped=child_text;stripped.erase(stripped.find(field),field.size()+1); // the field and its comma
        check(stripped.find("mountOrientation")==std::string::npos);reject_in(child_damaged,turned_child_output.jpeg,stripped,false);
        check(Read(turned.camera_a_original)==ta && Read(turned.camera_b_original)==tb && Read(turned_output.report)==turned_bytes);
        // #365 merge policy file: strict, no defaults, residual and scale values
        // optional. The 1% scale limit is a test policy file, not a code default.
        namespace px=a0::phase0::experimental;
        const auto sha=[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);};
        const std::string policy_text="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2}";
        const std::string scale_policy_text="{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":1}";
        const auto parsed_policy=px::ParseMergePolicySettings(policy_text,std::string(64,'a'));
        check(parsed_policy.rotation_max_degrees==2 && !parsed_policy.residual_max_pixels && !parsed_policy.scale_max_percent);
        check(px::ParseMergePolicySettings(scale_policy_text,std::string(64,'a')).scale_max_percent==1.0);
        check(px::ParseMergePolicySettings("{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":0}",std::string(64,'a')).scale_max_percent==0.0);
        const auto all_fields=px::ParseMergePolicySettings("\xEF\xBB\xBF{ \"similarityResidualMaxWarningPixels\" : 12.5,\r\n \"scaleAutoCorrectionMaxPercent\":0.5, \"rotationAutoCorrectionMaxDegrees\":0.75, \"schema\":\"a0.wpd-merge-policy.v1\" }\r\n",std::string(64,'b'));
        check(all_fields.residual_max_pixels==12.5 && all_fields.scale_max_percent==0.5 && all_fields.rotation_max_degrees==0.75);
        for(const auto* text:{"{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":null}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":-0.5}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":\"1\"}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMaxPercent\":1,\"scaleAutoCorrectionMaxPercent\":1}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"scaleAutoCorrectionMaxPercent\":1}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"scaleAutoCorrectionMax\":1}"})
            reject([&]{(void)px::ParseMergePolicySettings(text,std::string(64,'a'));});
        for(const auto* text:{"","{}","{\"schema\":\"a0.wpd-merge-policy.v1\"}","{\"rotationAutoCorrectionMaxDegrees\":2}",
            "{\"schema\":\"a0.wpd-merge-policy.v2\",\"rotationAutoCorrectionMaxDegrees\":2}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":\"2\"}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":-1}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":181}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":null}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"similarityResidualMaxWarningPixels\":null}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"similarityResidualMaxWarningPixels\":-3}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"rotationTargetDegrees\":0.1}",
            "{\"schema\":\"a0.wpd-merge-policy.v1\",\"rotationAutoCorrectionMaxDegrees\":2,\"rotationAutoCorrectionMaxDegrees\":3}",
            "[\"a0.wpd-merge-policy.v1\",2]"})reject([&]{(void)px::ParseMergePolicySettings(text,std::string(64,'a'));});
        reject([&]{(void)px::ParseMergePolicySettings(policy_text,"not-a-hash");});
        const auto policy_root=root/"policy";fs::create_directories(policy_root/"folder.json");
        check(!px::LoadMergePolicySettings(policy_root/"absent.json",sha));reject([&]{(void)px::LoadMergePolicySettings(policy_root/"folder.json",sha);});
        {std::ofstream f(policy_root/"large.json",std::ios::binary);f<<std::string(5000,' ');}reject([&]{(void)px::LoadMergePolicySettings(policy_root/"large.json",sha);});
        {std::ofstream f(policy_root/"wpd-merge-policy.json",std::ios::binary);f<<policy_text;}
        const auto loaded_policy=px::LoadMergePolicySettings(policy_root/"wpd-merge-policy.json",sha);
        check(loaded_policy && loaded_policy->sha256==a0::phase0::Sha256Hex(Read(policy_root/"wpd-merge-policy.json")) && loaded_policy->rotation_max_degrees==2 && !loaded_policy->scale_max_percent);
        fs::create_directories(policy_root/"scale");{std::ofstream f(policy_root/"scale/wpd-merge-policy.json",std::ios::binary);f<<scale_policy_text;}
        const auto scale_policy=px::LoadMergePolicySettings(policy_root/"scale/wpd-merge-policy.json",sha);
        check(scale_policy && scale_policy->scale_max_percent==1.0 && scale_policy->sha256!=loaded_policy->sha256);
        // The decision rule: rotation and scale each against their own limit;
        // equal to a limit is inside, either sign; nothing is clamped; the
        // residual warning needs the policy value.
        using S=StillScaleDecision;
        OverlapSimilarityEstimate synthetic;synthetic.status=OverlapEstimateStatus::estimated_not_judged;synthetic.reason=SimilarityUndeterminedReason::none;
        synthetic.similarity={900.4,10.6,2,1.001};synthetic.rigid={898.7,12.2,2,1};synthetic.translation_x=901.2;synthetic.translation_y=9.4;
        synthetic.similarity_residual={1,3};synthetic.rigid_residual={1.5,4};synthetic.translation_residual={2,6};synthetic.agreed=5;
        const auto applied_is=[](const AutomaticStillAlignment& a,int x,int y,double degrees,double scale){const auto g=AppliedStillAlignment(a);return g.x==x && g.y==y && g.rotation_degrees==degrees && g.scale==scale;};
        // No scale limit: rotation corrected, scale kept at 1 with the rigid fit.
        auto decided=DecideStillAlignment(synthetic,*loaded_policy);
        check(decided.decision==StillAlignmentDecision::corrected && decided.scale_decision==S::no_scale_policy && !decided.rotation_exceeded && !decided.scale_exceeded && !decided.residual_exceeded);
        check(applied_is(decided,899,12,2,1) && StillAlignmentBasis(decided)=="auto-still-rotation" && AppliedStillResidual(decided).rms==1.5 && StillAlignmentWarnings(decided).empty());
        check(px::StillAlignmentLine(decided).find(L"倍率 100.10% は上限が未設定のため補正していません")!=std::wstring::npos &&
            px::StillAlignmentLine(decided).find(L"一致タイル 5、残差 RMS 1.5 px")!=std::wstring::npos);
        // A scale limit with the scale inside: the similarity is applied.
        decided=DecideStillAlignment(synthetic,*scale_policy);
        check(decided.scale_decision==S::corrected && applied_is(decided,900,11,2,1.001) && StillAlignmentBasis(decided)=="auto-still-similarity" && AppliedStillResidual(decided).rms==1);
        for(const double inside:{1.01,0.99}){synthetic.similarity.scale=inside;check(DecideStillAlignment(synthetic,*scale_policy).scale_decision==S::corrected);}
        for(const double outside:{std::nextafter(1.01,2.0),std::nextafter(0.99,0.0),1.5}){synthetic.similarity.scale=outside;
            decided=DecideStillAlignment(synthetic,*scale_policy);
            check(decided.decision==StillAlignmentDecision::corrected && decided.scale_decision==S::scale_exceeded && decided.scale_exceeded && applied_is(decided,899,12,2,1));
            check(StillAlignmentWarnings(decided)==std::vector<std::string_view>{"scale-exceeded"} && StillAlignmentBasis(decided)=="auto-still-rotation");}
        check(px::StillAlignmentLine(decided).find(L"警告: 倍率が上限を超えたため倍率は補正していません")!=std::wstring::npos && px::StillAlignmentDetails(decided).find(L"倍率は上限超えのため 1")!=std::wstring::npos);
        synthetic.similarity.scale=1.001;
        synthetic.similarity.rotation_degrees=-2;check(DecideStillAlignment(synthetic,*loaded_policy).decision==StillAlignmentDecision::corrected);
        synthetic.similarity.rotation_degrees=std::nextafter(2.0,3.0);decided=DecideStillAlignment(synthetic,*loaded_policy);
        check(decided.decision==StillAlignmentDecision::rotation_exceeded && decided.rotation_exceeded && applied_is(decided,901,9,0,1) && StillAlignmentBasis(decided)=="auto-still-translation");
        check(px::StillAlignmentLine(decided).find(L"時計回り")!=std::wstring::npos && px::StillAlignmentLine(decided).find(L"反時計回り")==std::wstring::npos);
        // Rotation over its limit: only translation; the scale is still judged.
        check(DecideStillAlignment(synthetic,*scale_policy).scale_decision==S::not_applied && !DecideStillAlignment(synthetic,*scale_policy).scale_exceeded);
        synthetic.similarity.scale=1.02;decided=DecideStillAlignment(synthetic,*scale_policy);
        check(decided.scale_decision==S::scale_exceeded && applied_is(decided,901,9,0,1) && StillAlignmentWarnings(decided)==std::vector<std::string_view>{"rotation-exceeded","scale-exceeded"});
        synthetic.similarity.scale=1.001;
        synthetic.similarity.rotation_degrees=-2.5;check(px::StillAlignmentLine(DecideStillAlignment(synthetic,*loaded_policy)).find(L"反時計回り")!=std::wstring::npos);
        decided=DecideStillAlignment(synthetic,std::nullopt);
        check(decided.decision==StillAlignmentDecision::no_policy && decided.scale_decision==S::no_policy && !decided.rotation_exceeded && applied_is(decided,901,9,0,1));
        auto warn_policy=*loaded_policy;warn_policy.residual_max_pixels=3;check(!DecideStillAlignment(synthetic,warn_policy).residual_exceeded);
        warn_policy.residual_max_pixels=2.5;check(DecideStillAlignment(synthetic,warn_policy).residual_exceeded);
        auto bad_scale=*scale_policy;bad_scale.scale_max_percent=-1;reject([&]{(void)DecideStillAlignment(synthetic,bad_scale);});
        bad_scale.scale_max_percent=INFINITY;reject([&]{(void)DecideStillAlignment(synthetic,bad_scale);});
        synthetic.status=OverlapEstimateStatus::undetermined;synthetic.reason=SimilarityUndeterminedReason::insufficient_tiles;
        decided=DecideStillAlignment(synthetic,*scale_policy);
        check(decided.decision==StillAlignmentDecision::undetermined && decided.scale_decision==S::undetermined && !decided.similarity && !decided.rigid);
        // Automatic still alignment on generated JPEGs. Live geometry 460 x 307
        // scales by 4 to a 924, 12 start; the truth is 930.3, 9.2, +0.4 deg,
        // scale 1.004 (0.4%: inside a 1% limit, over a 0.2% limit).
        const unsigned sw=1840,sh=1228;const auto world=similarity_scene::MakeScene(sw,sh,960,40,17);
        const auto scene_a=similarity_scene::ViewA(world,sw,sh),scene_b=similarity_scene::ViewB(world,sw,sh,930.3,9.2,0.4,1.004,0.95);
        const auto auto_root=root/"auto";fs::create_directories(auto_root/"CAM-A");fs::create_directories(auto_root/"CAM-B");
        WriteJpeg(auto_root/"CAM-A/original.jpg",scene_a);WriteJpeg(auto_root/"CAM-B/original.jpg",scene_b);
        const auto aa=Read(auto_root/"CAM-A/original.jpg"),ab=Read(auto_root/"CAM-B/original.jpg");
        StillReviewRequest automatic{auto_root/"CAM-A/original.jpg",auto_root/"CAM-B/original.jpg",auto_root/"corrected",a0::phase0::Sha256Hex(aa),a0::phase0::Sha256Hex(ab),
            "synthetic-pair-auto",{460,307,229,3,11},std::chrono::steady_clock::now()+std::chrono::seconds(240)};
        // Scale limit present (1%) and the scale inside it: similarity applied.
        automatic.automatic_alignment=StillReviewRequest::AutomaticAlignment{scale_policy};
        const auto corrected=WriteUncalibratedStillReview(automatic);const auto corrected_record=LoadSavedStillReview(corrected.report);
        std::cout<<"auto_alignment_ms="<<corrected.alignment_milliseconds<<" "<<px::StillAlignmentLog(*corrected.automatic,corrected.alignment_milliseconds)<<'\n';
        const auto corrected_bytes=Read(corrected.report);const std::string corrected_text(corrected_bytes.begin(),corrected_bytes.end());
        check(corrected.automatic && corrected.automatic->decision==StillAlignmentDecision::corrected && corrected_record.automatic && corrected_record.automatic->decision==StillAlignmentDecision::corrected &&
            corrected_record.automatic->scale_decision==S::corrected);
        check(corrected_text.find("a0.uncalibrated-still-review.v5")!=std::string::npos && corrected_text.find("\"geometryBasis\":\"auto-still-similarity\"")!=std::string::npos &&
            corrected_text.find("\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false")!=std::string::npos && corrected_text.find("\"warnings\":[]")!=std::string::npos &&
            corrected_text.find("\"estimator\":\"a0.overlap-similarity.v2\"")!=std::string::npos && corrected_text.find("\"scaleDecision\":\"corrected\"")!=std::string::npos &&
            corrected_text.find("\"scaleAutoCorrectionMaxPercent\":1,")!=std::string::npos);
        const auto& fitted=*corrected_record.automatic->similarity;const auto& fitted_rigid=*corrected_record.automatic->rigid;
        check(std::abs(fitted.rotation_degrees-0.4)<0.02 && std::abs(fitted.x-930.3)<1.5 && std::abs(fitted.y-9.2)<1.5 && std::abs(fitted.scale-1.004)<5e-4);
        check(corrected_record.rotation_degrees==fitted.rotation_degrees && corrected_record.scale==fitted.scale && corrected_record.translation_x==std::llround(fitted.x) && corrected_record.translation_y==std::llround(fitted.y));
        check(corrected_record.automatic->similarity_residual.rms<corrected_record.automatic->translation_residual.rms && corrected_record.automatic->policy->sha256==scale_policy->sha256);
        check(Read(automatic.camera_a_original)==aa && Read(automatic.camera_b_original)==ab);
        // No scale limit in the policy: rotation applied, scale 1, rigid fit.
        auto rotation_only=automatic;rotation_only.output_directory=auto_root/"rotation-only";rotation_only.automatic_alignment->policy=loaded_policy;
        const auto rotation_output=WriteUncalibratedStillReview(rotation_only);const auto rotation_record=LoadSavedStillReview(rotation_output.report);const auto& rm=*rotation_record.automatic;
        std::cout<<px::StillAlignmentLog(rm,rotation_output.alignment_milliseconds)<<'\n';
        check(rm.decision==StillAlignmentDecision::corrected && rm.scale_decision==S::no_scale_policy && rotation_record.scale==1 && rotation_record.rotation_degrees==rm.similarity->rotation_degrees &&
            rotation_record.translation_x==std::llround(rm.rigid->x) && rotation_record.translation_y==std::llround(rm.rigid->y) && std::abs(rm.similarity->scale-1.004)<5e-4);
        check(rm.similarity_residual.rms<=rm.rigid_residual.rms && rm.rigid_residual.rms<rm.translation_residual.rms);
        const auto rotation_bytes=Read(rotation_record.report);const std::string rotation_text(rotation_bytes.begin(),rotation_bytes.end());
        check(rotation_text.find("\"geometryBasis\":\"auto-still-rotation\"")!=std::string::npos && rotation_text.find("\"scaleDecision\":\"no-scale-policy\"")!=std::string::npos &&
            rotation_text.find("\"scaleAutoCorrectionMaxPercent\":null")!=std::string::npos && rotation_text.find(",\"scale\":1},\"crop\"")!=std::string::npos && rotation_text.find("\"warnings\":[]")!=std::string::npos);
        // Scale over the limit (0.2%): warned, scale 1, rotation still applied.
        auto scale_over=automatic;scale_over.output_directory=auto_root/"scale-over";scale_over.automatic_alignment->policy=StillMergePolicy{scale_policy->sha256,2,{},0.2};
        const auto scale_over_record=LoadSavedStillReview(WriteUncalibratedStillReview(scale_over).report);const auto& sm=*scale_over_record.automatic;
        check(sm.decision==StillAlignmentDecision::corrected && sm.scale_decision==S::scale_exceeded && sm.scale_exceeded && scale_over_record.scale==1 &&
            scale_over_record.rotation_degrees==sm.similarity->rotation_degrees && scale_over_record.translation_x==std::llround(sm.rigid->x));
        const auto scale_over_bytes=Read(scale_over_record.report);const std::string scale_over_text(scale_over_bytes.begin(),scale_over_bytes.end());
        check(scale_over_text.find("\"warnings\":[\"scale-exceeded\"]")!=std::string::npos && scale_over_text.find("\"scaleDecision\":\"scale-exceeded\"")!=std::string::npos &&
            scale_over_text.find("\"geometryBasis\":\"auto-still-rotation\"")!=std::string::npos && scale_over_text.find("\"scaleAutoCorrectionMaxPercent\":0.20000000000000001")!=std::string::npos);
        // The overlap compares better after the similarity than after the
        // rigid fit, and better after the rigid fit than after the translation
        // alone: A against B (gain restored) on a grid of A pixels.
        const auto sources=LoadStillReviewSources(corrected_record);
        auto seam_error=[&](const a0::m2::render::PairRenderParameters& p){
            const auto inverse=a0::m2::render::Invert(p.camera_b_to_camera_a);double sum=0;
            for(unsigned y=150;y<sh-150;y+=4)for(unsigned x=1000;x<sw-20;x+=4){
                std::array<double,3> va{},vb{};a0::m2::render::Point s{};
                if(a0::m2::render::SampleBilinear(sources[0],x,y,va) && a0::m2::render::TryTransform(inverse,{double(x),double(y)},s) && a0::m2::render::SampleBilinear(sources[1],s.x,s.y,vb))
                    sum+=std::abs(va[0]-vb[0]/0.95);
            }
            return sum;
        };
        const auto t=corrected_record.automatic;
        const auto similarity_error=seam_error(SimilarityReviewGeometry(corrected_record.translation_x,corrected_record.translation_y,fitted.rotation_degrees,fitted.scale,sw,sh));
        const auto rigid_error=seam_error(SimilarityReviewGeometry(static_cast<int>(std::llround(fitted_rigid.x)),static_cast<int>(std::llround(fitted_rigid.y)),fitted_rigid.rotation_degrees,1,sw,sh));
        const auto translation_error=seam_error(SimilarityReviewGeometry(static_cast<int>(std::llround(t->translation_x)),static_cast<int>(std::llround(t->translation_y)),0,1,sw,sh));
        std::cout<<"seam_error similarity="<<similarity_error<<" rigid="<<rigid_error<<" translation="<<translation_error<<'\n';
        check(similarity_error<rigid_error && rigid_error<translation_error);
        // Over the rotation limit: translation only, warned, never clamped. The
        // scale over its limit is warned too; a residual value of zero
        // exercises the residual warning.
        auto exceeded=automatic;exceeded.output_directory=auto_root/"exceeded";
        exceeded.automatic_alignment->policy=StillMergePolicy{loaded_policy->sha256,0.25,0.0,0.2};
        const auto exceeded_record=LoadSavedStillReview(WriteUncalibratedStillReview(exceeded).report);const auto& em=*exceeded_record.automatic;
        check(em.decision==StillAlignmentDecision::rotation_exceeded && em.rotation_exceeded && em.scale_exceeded && em.scale_decision==S::scale_exceeded && em.residual_exceeded &&
            exceeded_record.rotation_degrees==0 && exceeded_record.scale==1);
        check(exceeded_record.translation_x==std::llround(em.translation_x) && exceeded_record.translation_y==std::llround(em.translation_y));
        const auto exceeded_bytes=Read(exceeded_record.report);const std::string exceeded_text(exceeded_bytes.begin(),exceeded_bytes.end());
        check(exceeded_text.find("\"geometryBasis\":\"auto-still-translation\"")!=std::string::npos && exceeded_text.find("\"warnings\":[\"rotation-exceeded\",\"scale-exceeded\",\"fit-residual-exceeded\"]")!=std::string::npos &&
            exceeded_text.find("\"adjustment\"")==std::string::npos && exceeded_text.find("\"similarityResidualMaxWarningPixels\":0")!=std::string::npos);
        // Without a policy file: values recorded, nothing corrected.
        auto unset=automatic;unset.output_directory=auto_root/"no-policy";unset.automatic_alignment->policy.reset();
        const auto unset_record=LoadSavedStillReview(WriteUncalibratedStillReview(unset).report);
        check(unset_record.automatic->decision==StillAlignmentDecision::no_policy && unset_record.automatic->scale_decision==S::no_policy && !unset_record.automatic->policy &&
            unset_record.rotation_degrees==0 && unset_record.scale==1 && std::abs(unset_record.automatic->similarity->rotation_degrees-0.4)<0.02);
        const auto unset_bytes=Read(unset_record.report);const std::string unset_text(unset_bytes.begin(),unset_bytes.end());
        check(unset_text.find("\"policy\":null")!=std::string::npos && unset_text.find("\"decision\":\"no-policy\"")!=std::string::npos && unset_text.find("\"scaleDecision\":\"no-policy\"")!=std::string::npos);
        // Not estimable (uniform originals): the live translation and render
        // are kept; the JPEG equals the earlier v1 result byte for byte.
        auto flat=r;flat.output_directory=root/"auto-flat";flat.automatic_alignment=StillReviewRequest::AutomaticAlignment{loaded_policy};flat.deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        const auto flat_output=WriteUncalibratedStillReview(flat);const auto flat_record=LoadSavedStillReview(flat_output.report);
        check(flat_record.automatic->decision==StillAlignmentDecision::undetermined && !flat_record.automatic->similarity && flat_record.translation_x==32 && flat_record.translation_y==4);
        check(Read(flat_output.jpeg)==original_jpeg && flat_output.width==96 && flat_output.height==44);
        const auto flat_bytes=Read(flat_output.report);const std::string flat_text(flat_bytes.begin(),flat_bytes.end());
        check(flat_text.find("\"geometryBasis\":\"live-preview\"")!=std::string::npos && flat_text.find("\"similarity\":null")!=std::string::npos && flat_text.find("\"status\":\"undetermined\"")!=std::string::npos);
        auto both=automatic;both.output_directory=auto_root/"both";both.manual_revision=StillReviewRequest::ManualRevision{930,9,corrected.report,corrected_record.report_sha256};
        reject([&]{WriteUncalibratedStillReview(both);});check(!fs::exists(both.output_directory));
        auto bad_policy=automatic;bad_policy.output_directory=auto_root/"bad-policy";bad_policy.automatic_alignment->policy->rotation_max_degrees=NAN;
        reject([&]{WriteUncalibratedStillReview(bad_policy);});check(!fs::exists(bad_policy.output_directory));
        // A manual revision of an automatic result keeps the v2/v3 format and
        // its parent link; the estimate is the starting value.
        auto manual_child=automatic;manual_child.automatic_alignment.reset();manual_child.output_directory=auto_root/"manual";
        manual_child.manual_revision=StillReviewRequest::ManualRevision{corrected_record.translation_x,corrected_record.translation_y,corrected.report,corrected_record.report_sha256,fitted.rotation_degrees,fitted.scale};
        const auto manual_record=LoadSavedStillReview(WriteUncalibratedStillReview(manual_child).report);
        check(manual_record.manual_revision && manual_record.manual_revision->parent_report==corrected.report && !manual_record.automatic && manual_record.rotation_degrees==fitted.rotation_degrees);
        // Recorded decisions are recomputed on read; altered reports fail closed.
        const auto auto_damaged=auto_root/"damaged";
        auto reject_auto=[&](const std::string& text,const std::string& from,const std::string& to,const fs::path& jpeg){
            auto changed=text;const auto at=changed.find(from);if(at==std::string::npos)throw std::runtime_error("automatic token absent");changed.replace(at,from.size(),to);
            fs::create_directories(auto_damaged);fs::copy_file(jpeg,auto_damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
            {std::ofstream f(auto_damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<changed;}
            reject([&]{LoadSavedStillReview(auto_damaged/"uncalibrated-review.json");});
        };
        reject_auto(corrected_text,"\"decision\":\"corrected\"","\"decision\":\"no-policy\"",corrected.jpeg);
        reject_auto(corrected_text,"\"warnings\":[]","\"warnings\":[\"rotation-exceeded\"]",corrected.jpeg);
        reject_auto(corrected_text,"\"rotationAutoCorrectionMaxDegrees\":2","\"rotationAutoCorrectionMaxDegrees\":0.1",corrected.jpeg);
        reject_auto(corrected_text,"\"geometryBasis\":\"auto-still-similarity\"","\"geometryBasis\":\"manual-still-similarity\"",corrected.jpeg);
        reject_auto(corrected_text,"\"estimator\":\"a0.overlap-similarity.v2\"","\"estimator\":\"other\"",corrected.jpeg);
        reject_auto(corrected_text,"\"estimator\":\"a0.overlap-similarity.v2\"","\"estimator\":\"a0.overlap-similarity.v1\"",corrected.jpeg);
        reject_auto(corrected_text,"\"schema\":\"a0.uncalibrated-still-review.v5\"","\"schema\":\"a0.uncalibrated-still-review.v4\"",corrected.jpeg);
        reject_auto(corrected_text,"\"productQualityAccepted\":false","\"productQualityAccepted\":true",corrected.jpeg);
        reject_auto(corrected_text,"\"scaleDecision\":\"corrected\"","\"scaleDecision\":\"no-scale-policy\"",corrected.jpeg);
        reject_auto(corrected_text,"\"scaleAutoCorrectionMaxPercent\":1,","\"scaleAutoCorrectionMaxPercent\":0.1,",corrected.jpeg);
        reject_auto(corrected_text,"\"scaleAutoCorrectionMaxPercent\":1,","",corrected.jpeg);
        reject_auto(corrected_text,"\"geometryBasis\":\"auto-still-similarity\"","\"geometryBasis\":\"auto-still-rotation\"",corrected.jpeg);
        // Rigid result: the adjustment must be the applied scale 1, not the
        // estimate; adding a scale limit afterwards changes the decision.
        reject_auto(rotation_text,",\"scale\":1},\"crop\"",",\"scale\":"+std::to_string(rm.similarity->scale)+"},\"crop\"",rotation_record.jpeg);
        reject_auto(rotation_text,"\"scaleAutoCorrectionMaxPercent\":null","\"scaleAutoCorrectionMaxPercent\":1",rotation_record.jpeg);
        reject_auto(rotation_text,"\"geometryBasis\":\"auto-still-rotation\"","\"geometryBasis\":\"auto-still-similarity\"",rotation_record.jpeg);
        reject_auto(rotation_text,"\"rigid\":{\"x\":","\"rigid\":{\"x\":1",rotation_record.jpeg);
        reject_auto(scale_over_text,"\"warnings\":[\"scale-exceeded\"]","\"warnings\":[]",scale_over_record.jpeg);
        reject_auto(exceeded_text,"\"warnings\":[\"rotation-exceeded\",\"scale-exceeded\",\"fit-residual-exceeded\"]","\"warnings\":[\"rotation-exceeded\",\"fit-residual-exceeded\"]",exceeded_record.jpeg);
        reject_auto(exceeded_text,"\"warnings\":[\"rotation-exceeded\",\"scale-exceeded\",\"fit-residual-exceeded\"]","\"warnings\":[\"scale-exceeded\",\"rotation-exceeded\",\"fit-residual-exceeded\"]",exceeded_record.jpeg);
        reject_auto(exceeded_text,"\"geometryBasis\":\"auto-still-translation\"","\"geometryBasis\":\"live-preview\"",exceeded_record.jpeg);
        reject_auto(flat_text,"\"status\":\"undetermined\"","\"status\":\"estimated-not-judged\"",flat_output.jpeg);
        reject_auto(flat_text,"\"reason\":\"no-tiles\"","\"reason\":\"none\"",flat_output.jpeg);
        reject_auto(unset_text,"\"policy\":null","\"policy\":{}",unset_record.jpeg);
        fs::copy_file(corrected.jpeg,auto_damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
        {std::ofstream f(auto_damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<corrected_text;}
        check(LoadSavedStillReview(auto_damaged/"uncalibrated-review.json").automatic->decision==StillAlignmentDecision::corrected);
        fs::copy_file(rotation_record.jpeg,auto_damaged/"uncalibrated-review.jpg",fs::copy_options::overwrite_existing);
        {std::ofstream f(auto_damaged/"uncalibrated-review.json",std::ios::binary|std::ios::trunc);f<<rotation_text;}
        check(LoadSavedStillReview(auto_damaged/"uncalibrated-review.json").automatic->scale_decision==S::no_scale_policy);
        // rotated-180 mounting: the estimate runs on the turned copies; the
        // originals stay as written.
        auto turn=[](a0::m2::render::BgrImage image){Rotate180(image);return image;};
        const auto auto_turned=root/"auto-turned";fs::create_directories(auto_turned/"CAM-A");fs::create_directories(auto_turned/"CAM-B");
        WriteJpeg(auto_turned/"CAM-A/original.jpg",turn(scene_a));WriteJpeg(auto_turned/"CAM-B/original.jpg",turn(scene_b));
        const auto ra=Read(auto_turned/"CAM-A/original.jpg"),rb=Read(auto_turned/"CAM-B/original.jpg");
        auto turned_auto=automatic;turned_auto.camera_a_original=auto_turned/"CAM-A/original.jpg";turned_auto.camera_b_original=auto_turned/"CAM-B/original.jpg";
        turned_auto.camera_a_sha256=a0::phase0::Sha256Hex(ra);turned_auto.camera_b_sha256=a0::phase0::Sha256Hex(rb);turned_auto.output_directory=auto_turned/"review";
        turned_auto.geometry.orientation=MountOrientation::rotated_180;
        const auto turned_auto_record=LoadSavedStillReview(WriteUncalibratedStillReview(turned_auto).report);
        check(turned_auto_record.geometry.orientation==MountOrientation::rotated_180 && turned_auto_record.automatic->decision==StillAlignmentDecision::corrected &&
            std::abs(turned_auto_record.automatic->similarity->rotation_degrees-0.4)<0.02 && Read(turned_auto.camera_a_original)==ra && Read(turned_auto.camera_b_original)==rb);
        check(DescribeSavedReviewEvaluationInputs(turned_auto_record.report).find("\"mountOrientation\":\"rotated-180\"")!=std::string::npos);
        check(Read(output.report)==report_bytes && Read(output.jpeg)==original_jpeg && Read(r.camera_a_original)==a && Read(r.camera_b_original)==b);
        CoUninitialize();
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
