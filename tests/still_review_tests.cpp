#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/review_session.hpp"
#include "a0/m2/offline_stitcher.hpp"
#include "a0/phase0/phase0.hpp"
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
        CoUninitialize();
    }catch(const std::exception& e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
