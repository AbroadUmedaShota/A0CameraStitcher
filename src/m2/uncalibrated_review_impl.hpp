// Development-only implementation, included inside a0::m2 after its private
// immutable-file/WIC helpers. Product profile validation paths stay unchanged.
namespace experimental {
namespace {
EncodedJpegIdentity EncodeReview(IWICImagingFactory* factory,Image& image,const std::filesystem::path& path){
    ComPtr<NewJpegStream> stream(new NewJpegStream(path));
    IWICBitmapEncoder* raw{};CheckHresult(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&raw),"review encoder");ComPtr<IWICBitmapEncoder> encoder(raw);
    CheckHresult(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache),"review encoder init");
    IWICBitmapFrameEncode* frame_raw{};IPropertyBag2* properties_raw{};
    CheckHresult(encoder->CreateNewFrame(&frame_raw,&properties_raw),"review frame");
    ComPtr<IWICBitmapFrameEncode> frame(frame_raw);ComPtr<IPropertyBag2> properties(properties_raw);
    CheckHresult(frame->Initialize(properties.get()),"review frame init");CheckHresult(frame->SetSize(image.width,image.height),"review dimensions");
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;CheckHresult(frame->SetPixelFormat(&format),"review BGR");
    if(format!=GUID_WICPixelFormat24bppBGR)throw std::runtime_error("review encoder rejected BGR");
    CheckHresult(frame->WritePixels(image.height,image.width*3,static_cast<UINT>(image.bgr.size()),image.bgr.data()),"review pixels");
    CheckHresult(frame->Commit(),"review frame commit");CheckHresult(encoder->Commit(),"review commit");
    frame.reset();properties.reset();encoder.reset();const auto bytes=stream->Snapshot();return stream->Identity(bytes);
}
std::string ReviewPathJson(const std::filesystem::path& path){
    const auto u8=path.generic_u8string();return a0::common::protocol_json::JsonEscape(std::string(reinterpret_cast<const char*>(u8.data()),u8.size()));
}
}
#include "still_review_report_impl.hpp"
StillReviewResult WriteUncalibratedStillReview(const StillReviewRequest& r){
    const auto active=[&]{if(std::chrono::steady_clock::now()>=r.deadline)throw std::runtime_error("review transaction deadline; files retained");};
    active();
    const auto hash_valid=[](const std::string& hash){return hash.size()==64 && hash.find_first_not_of("0123456789abcdef")==std::string::npos;};
    if(!hash_valid(r.camera_a_sha256) || !hash_valid(r.camera_b_sha256) || r.pair_id.empty() || r.pair_id.size()>80 ||
        r.pair_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)
        throw std::invalid_argument("review needs exact source hashes and pair identity");
    if(r.manual_revision && r.automatic_alignment)throw std::invalid_argument("manual revision and automatic alignment are exclusive");
    if(r.automatic_alignment && r.automatic_alignment->policy)ValidateStillMergePolicy(*r.automatic_alignment->policy);
    ValidateCanonicalPath(r.camera_a_original,"CAM-A");ValidateCanonicalPath(r.camera_b_original,"CAM-B");
    const auto a_path=NormalizedAbsolute(r.camera_a_original),b_path=NormalizedAbsolute(r.camera_b_original),job=NormalizedAbsolute(r.output_directory);
    if(a_path.parent_path().filename()!=L"CAM-A" || b_path.parent_path().filename()!=L"CAM-B")throw std::invalid_argument("review A/B original paths are not alias-bound");
    if(std::filesystem::exists(job))throw std::invalid_argument("review directory already exists; never overwrite");
    V2PathPins pins;pins.Pin(a_path);pins.Pin(b_path);pins.Pin(job,true);
    LockedReadFile a_file(a_path,false,true),b_file(b_path,false,true);
    if(a_file.SameFileAs(b_file))throw std::invalid_argument("review originals must be distinct physical files");
    ComApartment apartment;auto factory=CreateFactory();
    auto a=ReadJpegSnapshot(factory.get(),a_file,"review CAM-A",true),b=ReadJpegSnapshot(factory.get(),b_file,"review CAM-B",true);
    if(ToLowerHex(a.sha256)!=r.camera_a_sha256 || ToLowerHex(b.sha256)!=r.camera_b_sha256)throw std::invalid_argument("review source hash differs from capture transaction");
    if(a.image.width!=b.image.width || a.image.height!=b.image.height)throw std::invalid_argument("review source dimensions differ");
    const auto width=a.image.width,height=a.image.height;
    auto parameters=ScalePreviewReviewGeometry(r.geometry,width,height);
    std::unique_ptr<LockedReadFile> parent_file;
    if(r.manual_revision){
        const auto& revision=*r.manual_revision;
        const auto parent=LoadSavedStillReview(revision.parent_report);
        if(parent.report_sha256!=revision.parent_report_sha256 || parent.pair_id!=r.pair_id ||
            parent.camera_a_original!=a_path || parent.camera_b_original!=b_path ||
            parent.camera_a_sha256!=r.camera_a_sha256 || parent.camera_b_sha256!=r.camera_b_sha256 ||
            !SameReviewGeometry(parent.geometry,r.geometry))throw std::invalid_argument("restitch parent differs from original pair");
        // Retain the exact parent report through the new report's persistence.
        // A prior validation followed by an unlocked render would allow a
        // changed parent to make the newly saved lineage unusable.
        pins.Pin(parent.report);parent_file=std::make_unique<LockedReadFile>(parent.report,false,true);
        Sha256Provider sha;
        if(ToLowerHex(sha.Compute(parent_file->ReadAll(256*1024)))!=revision.parent_report_sha256)
            throw std::invalid_argument("restitch parent changed before lock");
        parameters=SimilarityReviewGeometry(revision.x,revision.y,revision.rotation_degrees,revision.scale,width,height);
    }
    active();
    // Only the decoded copies turn; the locked originals stay byte-identical.
    OrientRenderInput(a.image,r.geometry.orientation);OrientRenderInput(b.image,r.geometry.orientation);
    int translation_x=static_cast<int>(parameters.camera_b_to_camera_a[2]),translation_y=static_cast<int>(parameters.camera_b_to_camera_a[5]);
    if(r.manual_revision){translation_x=r.manual_revision->x;translation_y=r.manual_revision->y;}
    std::optional<AutomaticStillAlignment> automatic;double alignment_milliseconds{};
    if(r.automatic_alignment){
        // Starts at the live translation scaled to the still; undetermined
        // keeps exactly that translation and render.
        const auto started=std::chrono::steady_clock::now();
        automatic=DecideStillAlignment(EstimateOverlapSimilarity(a.image,b.image,translation_x,translation_y),r.automatic_alignment->policy);
        alignment_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        if(automatic->similarity){
            // The applied geometry, not the estimate: scale stays 1 unless the
            // policy's scale limit admits it (the estimator checked all fits render).
            const auto applied=AppliedStillAlignment(*automatic);
            parameters=SimilarityReviewGeometry(applied.x,applied.y,applied.rotation_degrees,applied.scale,width,height);
            translation_x=applied.x;translation_y=applied.y;
        }
        active();
    }
    auto rendered=render::RenderPair(a.image,b.image,parameters);a.image={};b.image={};active();
    std::filesystem::create_directories(job.parent_path());pins.Pin(job.parent_path(),true);
    if(!std::filesystem::create_directory(job))throw std::runtime_error("cannot reserve new review directory");pins.Pin(job,true);
    const auto partial=job/L"uncalibrated-review.jpg.partial",final=job/L"uncalibrated-review.jpg";
    const auto encoded=EncodeReview(factory.get(),rendered.image,partial);
    const auto output_width=rendered.image.width,output_height=rendered.image.height;rendered.image={};
    InvokeTestHook(before_partial_flush_hook);active();FlushGeneratedPartial(partial,true,&encoded);active();
    {
        LockedReadFile candidate(partial,true,true);candidate.ValidateIdentity(encoded);
        const auto bytes=candidate.ReadAll();Sha256Provider sha;
        if(sha.Compute(bytes)!=encoded.sha256 || bytes.size()!=encoded.size)throw std::runtime_error("review partial changed");
        const auto dimensions=ValidateJpegDimensions(factory.get(),bytes,"review partial",true);
        if(dimensions.width!=output_width || dimensions.height!=output_height)throw std::runtime_error("review partial dimensions changed");
        active();candidate.RenameToWithoutReplace(final);
    }
    StillReviewResult result;result.jpeg=final;result.report=job/L"uncalibrated-review.json";
    result.width=output_width;result.height=output_height;result.sha256=ToLowerHex(encoded.sha256);
    {
        LockedReadFile saved(final,false,true);auto decoded=ReadJpegSnapshot(factory.get(),saved,"saved review",true);
        if(decoded.sha256!=encoded.sha256 || decoded.compressed.size()!=encoded.size || decoded.image.width!=output_width || decoded.image.height!=output_height)
            throw std::runtime_error("saved review verification failed");
        const auto divisor=std::max(1u,(std::max(output_width,output_height)+1199)/1200);
        result.thumbnail={output_width/divisor,output_height/divisor,{}};
        result.thumbnail.bgr.resize(static_cast<size_t>(result.thumbnail.width)*result.thumbnail.height*3);
        for(unsigned y=0;y<result.thumbnail.height;++y)for(unsigned x=0;x<result.thumbnail.width;++x){
            const auto source=(static_cast<size_t>(y*divisor)*output_width+x*divisor)*3,target=(static_cast<size_t>(y)*result.thumbnail.width+x)*3;
            std::copy_n(decoded.image.bgr.data()+source,3,result.thumbnail.bgr.data()+target);
        }
        ValidateSnapshotHash(decoded,saved,"saved review");
    }
    ValidateSnapshotHash(a,a_file,"review CAM-A");ValidateSnapshotHash(b,b_file,"review CAM-B");active();
    std::ostringstream report;
    report.imbue(std::locale::classic());report<<std::setprecision(17);
    const bool similarity=r.manual_revision && (r.manual_revision->rotation_degrees!=0 || r.manual_revision->scale!=1);
    const char* schema=automatic?"a0.uncalibrated-still-review.v5":similarity?"a0.uncalibrated-still-review.v3":r.manual_revision?"a0.uncalibrated-still-review.v2":"a0.uncalibrated-still-review.v1";
    report<<"{\"schema\":\""<<schema<<"\",\"pairId\":\""<<r.pair_id
        <<"\",\"state\":\"UncalibratedReviewReady\",\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false,\"automaticRetryCount\":0,\"inputs\":["
        <<"{\"alias\":\"CAM-A\",\"path\":\""<<ReviewPathJson(a_path)<<"\",\"sha256\":\""<<r.camera_a_sha256<<"\",\"bytes\":"<<a.compressed.size()<<"},"
        <<"{\"alias\":\"CAM-B\",\"path\":\""<<ReviewPathJson(b_path)<<"\",\"sha256\":\""<<r.camera_b_sha256<<"\",\"bytes\":"<<b.compressed.size()<<"}],"
        <<"\"previewGeometry\":{\"width\":"<<r.geometry.width<<",\"height\":"<<r.geometry.height<<",\"overlap\":"<<r.geometry.overlap<<",\"vertical\":"<<r.geometry.vertical<<",\"revision\":"<<r.geometry.revision<<"},";
    // Absent means normal, so normal reports keep their previous bytes.
    if(r.geometry.orientation!=MountOrientation::normal)report<<"\"mountOrientation\":\""<<MountOrientationName(r.geometry.orientation)<<"\",";
    report<<"\"sourceDimensions\":{\"width\":"<<width<<",\"height\":"<<height<<"},\"translation\":{\"x\":"<<translation_x<<",\"y\":"<<translation_y<<"},"
        <<"\"output\":{\"path\":\"uncalibrated-review.jpg\",\"width\":"<<output_width<<",\"height\":"<<output_height<<",\"bytes\":"<<encoded.size<<",\"sha256\":\""<<result.sha256<<"\"}";
    if(r.manual_revision)report<<",\"geometryBasis\":\""<<(similarity?"manual-still-similarity":"manual-still-pixels")<<"\",\"parentReview\":{\"path\":\""<<ReviewPathJson(NormalizedAbsolute(r.manual_revision->parent_report))<<"\",\"sha256\":\""<<r.manual_revision->parent_report_sha256<<"\"}";
    if(similarity)report<<",\"adjustment\":{\"anchor\":\"image-center\",\"rotationDegrees\":"<<r.manual_revision->rotation_degrees<<",\"scale\":"<<r.manual_revision->scale<<"},\"crop\":{\"left\":"<<parameters.crop.left<<",\"top\":"<<parameters.crop.top<<",\"right\":"<<parameters.crop.right<<",\"bottom\":"<<parameters.crop.bottom<<"}";
    if(automatic){
        const auto& m=*automatic;
        report<<",\"geometryBasis\":\""<<StillAlignmentBasis(m)<<"\"";
        // adjustment: the rotation and scale actually applied; the estimate
        // stays in automaticAlignment.similarity.
        if(m.decision==StillAlignmentDecision::corrected){const auto applied=AppliedStillAlignment(m);
            report<<",\"adjustment\":{\"anchor\":\"image-center\",\"rotationDegrees\":"<<applied.rotation_degrees<<",\"scale\":"<<applied.scale<<"},\"crop\":{\"left\":"<<parameters.crop.left<<",\"top\":"<<parameters.crop.top<<",\"right\":"<<parameters.crop.right<<",\"bottom\":"<<parameters.crop.bottom<<"}";}
        report<<",\"automaticAlignment\":{\"estimator\":\"a0.overlap-similarity.v2\",\"status\":\""<<OverlapEstimateStatusCode(m.status)<<"\",\"reason\":\""<<SimilarityUndeterminedReasonCode(m.reason)
            <<"\",\"initialPosition\":\""<<(m.wide_search?"still-wide-search":"live-preview")<<"\",\"tiles\":{\"searched\":"<<m.tiles_searched<<",\"matched\":"<<m.tiles_matched<<",\"agreed\":"<<m.tiles_agreed<<"},\"similarity\":";
        if(m.similarity)report<<"{\"x\":"<<m.similarity->x<<",\"y\":"<<m.similarity->y<<",\"rotationDegrees\":"<<m.similarity->rotation_degrees<<",\"scale\":"<<m.similarity->scale<<"},\"rigid\":{\"x\":"<<m.rigid->x<<",\"y\":"<<m.rigid->y
            <<"},\"translationOnly\":{\"x\":"<<m.translation_x<<",\"y\":"<<m.translation_y
            <<"},\"residualPixels\":{\"similarityRms\":"<<m.similarity_residual.rms<<",\"similarityMax\":"<<m.similarity_residual.maximum<<",\"rigidRms\":"<<m.rigid_residual.rms<<",\"rigidMax\":"<<m.rigid_residual.maximum
            <<",\"translationOnlyRms\":"<<m.translation_residual.rms<<",\"translationOnlyMax\":"<<m.translation_residual.maximum<<"}";
        else report<<"null,\"rigid\":null,\"translationOnly\":null,\"residualPixels\":null";
        report<<",\"decision\":\""<<StillAlignmentDecisionCode(m.decision)<<"\",\"scaleDecision\":\""<<StillScaleDecisionCode(m.scale_decision)<<"\",\"warnings\":[";
        bool first=true;for(const auto w:StillAlignmentWarnings(m)){report<<(first?"":",")<<'"'<<w<<'"';first=false;}
        report<<"],\"policy\":";
        const auto optional=[&](const std::optional<double>& v){if(v)report<<*v;else report<<"null";};
        if(m.policy){report<<"{\"sha256\":\""<<m.policy->sha256<<"\",\"rotationAutoCorrectionMaxDegrees\":"<<m.policy->rotation_max_degrees<<",\"scaleAutoCorrectionMaxPercent\":";
            optional(m.policy->scale_max_percent);report<<",\"similarityResidualMaxWarningPixels\":";optional(m.policy->residual_max_pixels);report<<"}";
        }else report<<"null";
        report<<"}";
    }
    report<<"}";
    const auto document=report.str();const std::vector<std::uint8_t> report_bytes(document.begin(),document.end());
    const auto report_partial=job/L"uncalibrated-review.json.partial";active();WriteBytesToNewFile(report_partial,report_bytes);
    {LockedReadFile saved(report_partial,true,true);if(saved.ReadAll()!=report_bytes)throw std::runtime_error("review report partial changed");active();saved.RenameToWithoutReplace(result.report);}
    {LockedReadFile saved(result.report,false,true);if(saved.ReadAll()!=report_bytes)throw std::runtime_error("review report reread failed");}
    result.automatic=std::move(automatic);result.alignment_milliseconds=alignment_milliseconds;
    active();return result;
}
}
