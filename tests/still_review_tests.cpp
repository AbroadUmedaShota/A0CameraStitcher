#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/review_session.hpp"
#include "a0/m2/offline_stitcher.hpp"
#include "a0/phase0/phase0.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
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
void Fixture(const fs::path& path,unsigned char red,unsigned char blue){
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream));Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;Hr(encoder->CreateNewFrame(&frame,&props));Hr(frame->Initialize(props.Get()));Hr(frame->SetSize(64,48));
    auto format=GUID_WICPixelFormat24bppBGR;Hr(frame->SetPixelFormat(&format));std::vector<BYTE> pixels(64*48*3);
    for(size_t i=0;i<pixels.size();i+=3){pixels[i]=blue;pixels[i+1]=30;pixels[i+2]=red;}
    Hr(frame->WritePixels(48,64*3,static_cast<UINT>(pixels.size()),pixels.data()));Hr(frame->Commit());Hr(encoder->Commit());
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
        check(full.width==96 && full.height==44 && saved.report_sha256==a0::phase0::Sha256Hex(report_bytes));
        auto revised=r;revised.output_directory=root/"revision";
        revised.manual_revision=StillReviewRequest::ManualRevision{33,3,output.report,saved.report_sha256};
        parent_lock_probe=output.report;a0::m2::detail::SetBeforePartialFlushTestHook(ProbeParentWriteLock);
        const auto revision=WriteUncalibratedStillReview(revised);a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);
        check(parent_write_denied);const auto child=LoadSavedStillReview(revision.report);
        check(child.width==97 && child.height==45 && child.translation_x==33 && child.translation_y==3);
        check(child.manual_revision && child.manual_revision->parent_report==output.report && child.manual_revision->parent_report_sha256==saved.report_sha256);
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
        CoUninitialize();
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
