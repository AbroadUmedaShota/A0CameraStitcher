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
// #405 W4. The expected B-to-A translation of the profile's fixed mapping, in the pixels of the
// oriented frames the tile estimator works in (A left, B right): B's grid centre goes to the document
// by B's fixed mapping (the centre of the scale correction) and from there to A's raw pixel. A shift is
// the same in both pixel conventions; a half turn only reverses its sign. False where either mapping
// has no point or the position is not one the estimator accepts as a start.
bool LensEstimatorPrior(const FixedCompositeProfileBinding& profile,MountOrientation orientation,unsigned width,unsigned height,int& x,int& y){
    try{
        const auto& p=profile.parameters;
        const auto document=FixedCompositeScaleCenterMm(p.camera_b);
        render::Point on_a{};
        if(!render::TryProjectDocument(p.camera_a,document,on_a))return false;
        double dx=on_a.x-(static_cast<double>(width)-1)/2,dy=on_a.y-(static_cast<double>(height)-1)/2;
        if(orientation==MountOrientation::rotated_180){dx=-dx;dy=-dy;}
        if(!std::isfinite(dx) || !std::isfinite(dy) || std::abs(dx)>=width || std::abs(dy)>=height)return false;
        x=static_cast<int>(std::llround(dx));y=static_cast<int>(std::llround(dy));
        return x>0 && x<static_cast<int>(width) && std::abs(y)<static_cast<int>(height);
    }catch(const std::invalid_argument&){return false;}
}
bool SameLensRaster(const render::OutputRaster& a,const render::OutputRaster& b){
    return a.region_um.left==b.region_um.left && a.region_um.top==b.region_um.top && a.region_um.right==b.region_um.right && a.region_um.bottom==b.region_um.bottom &&
        a.dpi==b.dpi && a.width_pixels==b.width_pixels && a.height_pixels==b.height_pixels;
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
    if(r.blend!=BlendMethod::linear_feather && r.blend!=BlendMethod::seam_cut)throw std::invalid_argument("unknown review blend");
    if(r.automatic_alignment && r.automatic_alignment->policy)ValidateStillMergePolicy(*r.automatic_alignment->policy);
    // #400: the fixed composite owns geometry and seam, so nothing else may set them.
    const FixedCompositeCondition* fixed_condition{};
    if(r.fixed_composite){
        if(r.manual_revision || r.automatic_alignment || r.blend!=BlendMethod::linear_feather || !r.fixed_composite->condition || !hash_valid(r.fixed_composite->condition_sha256))
            throw std::invalid_argument("fixed composite review is exclusive and needs the condition and its hash");
        fixed_condition=r.fixed_composite->condition;
        try{ValidateFixedCompositeCondition(*fixed_condition);}
        catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::condition_invalid,error.what());}
    }
    // #405 W4: the lens-aware fixed composite owns geometry and seam too; its profile is the mapping.
    const FixedCompositeConditionV2* lens_condition{};const FixedCompositeProfileBinding* lens_profile{};
    if(r.fixed_composite_lens){
        const auto& lens=*r.fixed_composite_lens;
        if(r.fixed_composite || r.manual_revision || r.automatic_alignment || r.blend!=BlendMethod::linear_feather || !lens.condition || !lens.profile || !hash_valid(lens.condition_sha256))
            throw std::invalid_argument("lens-aware fixed composite review is exclusive and needs the condition, its profile and the condition hash");
        lens_condition=lens.condition;lens_profile=lens.profile;
        try{ValidateFixedCompositeConditionV2(*lens_condition);}
        catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::condition_invalid,error.what());}
        // The binding must be the profile this condition names, for this condition's raster.
        if(lens_profile->profile_file_sha256!=lens_condition->source.profile_file_sha256 || lens_profile->profile_fingerprint_sha256!=lens_condition->source.profile_fingerprint_sha256 ||
            !SameLensRaster(lens_profile->parameters.raster,lens_condition->raster))
            throw FixedCompositeUnavailable(FixedCompositeSkip::profile_differs,"the profile binding is not the one the condition names");
    }
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
    // Checked before anything is created: a mismatch keeps only the originals.
    if(fixed_condition){
        if(fixed_condition->orientation!=r.geometry.orientation)
            throw FixedCompositeUnavailable(FixedCompositeSkip::orientation_differs,std::string("condition ")+std::string(MountOrientationName(fixed_condition->orientation))+", capture "+std::string(MountOrientationName(r.geometry.orientation)));
        if(fixed_condition->source_width!=width || fixed_condition->source_height!=height)
            throw FixedCompositeUnavailable(FixedCompositeSkip::size_differs,"condition "+std::to_string(fixed_condition->source_width)+"x"+std::to_string(fixed_condition->source_height)+", capture "+std::to_string(width)+"x"+std::to_string(height));
    }
    if(lens_condition){
        if(lens_condition->orientation!=r.geometry.orientation)
            throw FixedCompositeUnavailable(FixedCompositeSkip::orientation_differs,std::string("condition ")+std::string(MountOrientationName(lens_condition->orientation))+", capture "+std::string(MountOrientationName(r.geometry.orientation)));
        // The profile's camera model fixes the frame its mapping is defined on.
        if(width!=kD810SensorWidthPixels || height!=kD810SensorHeightPixels)
            throw FixedCompositeUnavailable(FixedCompositeSkip::size_differs,"profile frame "+std::to_string(kD810SensorWidthPixels)+"x"+std::to_string(kD810SensorHeightPixels)+", capture "+std::to_string(width)+"x"+std::to_string(height));
    }
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
    // The mapped composite works on the stored pixel order (design 3.7). The cheap check that the seam can be drawn
    // comes first: a seam that cannot be drawn needs no estimate. Nothing but sizes is read from the images here.
    // Before the seam, every output pixel must be shown by A or B (a condition edited by hand can ask for more than the
    // cameras see); the live C refuses the same condition as size-differs, so the JPEG says the same.
    std::optional<MappedFixedSeamInput> lens_pair;
    if(lens_condition){
        try{lens_pair=ScaledDocumentMappedFixedSeamInput(lens_profile->parameters,1.0);}
        catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::seam_outside_overlap,error.what());}
        try{RequireFixedCompositeCoverage(*lens_pair);}
        catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::size_differs,std::string("output not covered by A or B: ")+error.what());}
        try{
            if(!CanRenderMappedFixedSeam(a.image,b.image,*lens_pair,lens_condition->seam.column,kSeamBlendDesign))
                throw FixedCompositeUnavailable(FixedCompositeSkip::seam_outside_overlap,"seam column "+std::to_string(lens_condition->seam.column));
        }catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::seam_outside_overlap,error.what());}
    }
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
    // #405 W4: the scale of this capture on the lens-aware composite. The tiles of the similarity estimator
    // (run on the oriented frames, from the profile's own mapping) are moved to the document and fitted there;
    // the decision is DecideFixedScaleV2's. Then the images go back to the stored order the mapping is defined on.
    std::optional<SavedFixedCompositeLens> lens_record;
    if(lens_condition){
        const auto& c=*lens_condition;const auto& profile=*lens_profile;
        const auto started=std::chrono::steady_clock::now();
        OverlapSimilarityEstimate estimate;int prior_x{},prior_y{};
        if(LensEstimatorPrior(profile,c.orientation,width,height,prior_x,prior_y))estimate=EstimateOverlapSimilarity(a.image,b.image,prior_x,prior_y);
        else{estimate.status=OverlapEstimateStatus::undetermined;estimate.reason=SimilarityUndeterminedReason::outside_image;}
        active();
        OrientRenderInput(a.image,r.geometry.orientation);OrientRenderInput(b.image,r.geometry.orientation);
        SavedFixedCompositeLens record;
        record.condition_sha256=r.fixed_composite_lens->condition_sha256;record.profile_file=c.source.profile_file;
        record.profile_file_sha256=profile.profile_file_sha256;record.profile_fingerprint_sha256=profile.profile_fingerprint_sha256;
        if(profile.profile_id && !profile.profile_id->empty())record.profile_id=profile.profile_id;
        record.raster=c.raster;record.seam_column=c.seam.column;record.seam_origin=std::string(FixedSeamOriginName(c.seam.origin));
        record.policy_limit=r.fixed_composite_lens->policy_limit;
        record.estimate_status=estimate.status;record.estimate_reason=estimate.reason;record.wide_search=estimate.wide_search;
        record.tiles_searched=static_cast<unsigned>(estimate.tiles.size());record.tiles_matched=estimate.matched;record.tiles_agreed=estimate.agreed;
        record.scale=DecideFixedScaleV2(c,profile,estimate,width,height);
        if(record.scale.status==FixedScaleStatus::applied){
            // The correction must leave every output pixel shown and the seam (and half the blend) inside the
            // overlap; otherwise it is not applied, and the status says which of the two failed.
            bool covered=true,fits=false;MappedFixedSeamInput scaled;
            try{
                scaled=ScaledDocumentMappedFixedSeamInput(profile.parameters,record.scale.applied_scale);
                try{RequireFixedCompositeCoverage(scaled);}catch(const std::invalid_argument&){covered=false;}
                if(covered)fits=CanRenderMappedFixedSeam(a.image,b.image,scaled,c.seam.column,kSeamBlendDesign);
            }
            catch(const std::invalid_argument&){fits=false;}
            if(fits)lens_pair=std::move(scaled);
            else{record.scale.status=covered?FixedScaleStatus::seam_outside_overlap:FixedScaleStatus::output_uncovered;record.scale.applied_scale=1;}
        }
        alignment_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        lens_record=std::move(record);
    }
    // #400: geometry and seam from the fixed condition. The similarity estimate
    // is kept as a diagnostic; only its scale-only reduction may change B, and
    // only inside the condition's limit (the policy file never decides).
    std::optional<SavedFixedComposite> fixed_record;
    if(fixed_condition){
        const auto& c=*fixed_condition;
        // The cheap check first: a seam that cannot be drawn needs no estimate.
        parameters=FixedCompositeRenderParameters(c);
        if(!CanRenderFixedSeam(a.image,b.image,parameters,c.seam.column,kSeamBlendDesign))
            throw FixedCompositeUnavailable(FixedCompositeSkip::seam_outside_overlap,"seam column "+std::to_string(c.seam.column));
        const auto started=std::chrono::steady_clock::now();
        const auto estimate=EstimateOverlapSimilarity(a.image,b.image,c.mapping.translation_x,c.mapping.translation_y);
        active();
        SavedFixedComposite record;
        record.condition_sha256=r.fixed_composite->condition_sha256;record.seam_column=c.seam.column;
        record.seam_origin=std::string(FixedSeamOriginName(c.seam.origin));record.policy_limit=r.fixed_composite->policy_limit;
        record.scale=DecideFixedScale(c,estimate);record.diagnostic=MakeFixedSimilarityDiagnostic(estimate);
        alignment_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        if(record.scale.status==FixedScaleStatus::applied){
            bool fits=false;render::PairRenderParameters scaled{};
            try{scaled=FixedCompositeScaledGeometry(c,record.scale.applied_scale);fits=CanRenderFixedSeam(a.image,b.image,scaled,c.seam.column,kSeamBlendDesign);}
            catch(const std::invalid_argument&){fits=false;}
            if(fits)parameters=scaled;
            else{record.scale.status=FixedScaleStatus::seam_outside_overlap;record.scale.applied_scale=1;}
        }
        translation_x=c.mapping.translation_x;translation_y=c.mapping.translation_y;
        fixed_record=std::move(record);
    }
    // #368: the seam cut replaces only the mixing; geometry and crop are the same.
    std::optional<SeamBlendRecord> seam;double blend_milliseconds{};render::PairRenderResult rendered;
    if(lens_record){
        const auto started=std::chrono::steady_clock::now();
        SeamBlendResult fixed;
        // Nothing is created before this point, so a refusal here leaves only the originals.
        try{fixed=RenderMappedFixedSeam(a.image,b.image,*lens_pair,lens_condition->seam.column,kSeamBlendDesign);}
        catch(const std::invalid_argument& error){throw FixedCompositeUnavailable(FixedCompositeSkip::size_differs,error.what());}
        blend_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        // The raster is fixed by the condition: the output is never another size.
        if(fixed.image.width!=lens_condition->raster.width_pixels || fixed.image.height!=lens_condition->raster.height_pixels)throw std::runtime_error("lens-aware composite is not the size of the condition's raster");
        rendered.image=std::move(fixed.image);seam=std::move(fixed.record);
    }else if(fixed_record){
        const auto started=std::chrono::steady_clock::now();
        auto fixed=RenderPairFixedSeam(a.image,b.image,parameters,fixed_record->seam_column,kSeamBlendDesign);
        blend_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        rendered.image=std::move(fixed.image);seam=std::move(fixed.record);
    }else if(r.blend==BlendMethod::seam_cut){
        const auto started=std::chrono::steady_clock::now();auto cut=RenderPairSeamCut(a.image,b.image,parameters,kSeamBlendDesign);
        blend_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        rendered.image=std::move(cut.image);seam=std::move(cut.record);
    }else rendered=render::RenderPair(a.image,b.image,parameters);
    a.image={};b.image={};active();
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
    const char* schema=lens_record?"a0.uncalibrated-still-review.v7":fixed_record?"a0.uncalibrated-still-review.v6":automatic?"a0.uncalibrated-still-review.v5":similarity?"a0.uncalibrated-still-review.v3":r.manual_revision?"a0.uncalibrated-still-review.v2":"a0.uncalibrated-still-review.v1";
    report<<"{\"schema\":\""<<schema<<"\",\"pairId\":\""<<r.pair_id
        <<"\",\"state\":\"UncalibratedReviewReady\",\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false,\"automaticRetryCount\":0,\"inputs\":["
        <<"{\"alias\":\"CAM-A\",\"path\":\""<<ReviewPathJson(a_path)<<"\",\"sha256\":\""<<r.camera_a_sha256<<"\",\"bytes\":"<<a.compressed.size()<<"},"
        <<"{\"alias\":\"CAM-B\",\"path\":\""<<ReviewPathJson(b_path)<<"\",\"sha256\":\""<<r.camera_b_sha256<<"\",\"bytes\":"<<b.compressed.size()<<"}],"
        <<"\"previewGeometry\":{\"width\":"<<r.geometry.width<<",\"height\":"<<r.geometry.height<<",\"overlap\":"<<r.geometry.overlap<<",\"vertical\":"<<r.geometry.vertical<<",\"revision\":"<<r.geometry.revision<<"},";
    // Absent means normal, so normal reports keep their previous bytes.
    if(r.geometry.orientation!=MountOrientation::normal)report<<"\"mountOrientation\":\""<<MountOrientationName(r.geometry.orientation)<<"\",";
    report<<"\"sourceDimensions\":{\"width\":"<<width<<",\"height\":"<<height<<"},";
    // The lens-aware mapping is the profile's: no translation (v7).
    if(!lens_record)report<<"\"translation\":{\"x\":"<<translation_x<<",\"y\":"<<translation_y<<"},";
    report<<"\"output\":{\"path\":\"uncalibrated-review.jpg\",\"width\":"<<output_width<<",\"height\":"<<output_height<<",\"bytes\":"<<encoded.size<<",\"sha256\":\""<<result.sha256<<"\"}";
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
    if(lens_record){report<<",\"geometryBasis\":\"fixed-composite-lens\"";WriteFixedCompositeLensJson(report,*lens_record);WriteFixedSeamBlendJson(report,*seam);}
    else if(fixed_record){report<<",\"geometryBasis\":\"fixed-composite\",\"adjustment\":{\"anchor\":\"image-center\",\"rotationDegrees\":"<<fixed_condition->mapping.rotation_degrees<<",\"scale\":"<<fixed_condition->mapping.scale<<"},\"crop\":{\"left\":"<<parameters.crop.left<<",\"top\":"<<parameters.crop.top<<",\"right\":"<<parameters.crop.right<<",\"bottom\":"<<parameters.crop.bottom<<"}";
        WriteFixedCompositeJson(report,*fixed_record,fixed_condition->mapping.translation_x,fixed_condition->mapping.translation_y,fixed_condition->mapping.rotation_degrees,fixed_condition->mapping.scale);WriteFixedSeamBlendJson(report,*seam);}
    else if(seam)WriteSeamBlendJson(report,*seam);
    report<<"}";
    const auto document=report.str();const std::vector<std::uint8_t> report_bytes(document.begin(),document.end());
    const auto report_partial=job/L"uncalibrated-review.json.partial";active();WriteBytesToNewFile(report_partial,report_bytes);
    {LockedReadFile saved(report_partial,true,true);if(saved.ReadAll()!=report_bytes)throw std::runtime_error("review report partial changed");active();saved.RenameToWithoutReplace(result.report);}
    {LockedReadFile saved(result.report,false,true);if(saved.ReadAll()!=report_bytes)throw std::runtime_error("review report reread failed");}
    result.automatic=std::move(automatic);result.alignment_milliseconds=alignment_milliseconds;
    result.blend=std::move(seam);result.fixed_composite=std::move(fixed_record);result.fixed_composite_lens=std::move(lens_record);result.blend_milliseconds=blend_milliseconds;
    active();return result;
}
StillBlendComparison CompareStillReviewBlends(const SavedStillReview& review,const std::filesystem::path& prefix,std::uint32_t crop_width,std::uint32_t crop_height){
    if(!crop_width || !crop_height)throw std::invalid_argument("comparison crop is empty");
    if(review.fixed_composite_lens)throw std::invalid_argument("the blend comparison re-renders a similarity placement; a lens-aware review has none");
    const auto sources=LoadStillReviewSources(review);
    const auto parameters=SavedStillReviewRenderParameters(review);
    StillBlendComparison c;const auto since=[](auto started){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();};
    auto started=std::chrono::steady_clock::now();auto feather=render::RenderPair(sources[0],sources[1],parameters).image;c.feather_milliseconds=since(started);
    started=std::chrono::steady_clock::now();auto cut=RenderPairSeamCut(sources[0],sources[1],parameters,kSeamBlendDesign);c.seam_milliseconds=since(started);
    if(feather.width!=review.width || feather.height!=review.height)throw std::runtime_error("comparison canvas differs from the saved review");
    c.probes=ProbeSeamDisplacement(sources[0],sources[1],parameters,cut.record.seam_x,128,32,std::max(1u,feather.height/24));
    const auto name=[&](const wchar_t* suffix){auto path=prefix;path+=suffix;return path;};
    ComApartment apartment;auto factory=CreateFactory();
    c.feather_matches_saved=ToLowerHex(EncodeReview(factory.get(),feather,name(L"-feather.jpg")).sha256)==review.jpeg_sha256;
    (void)EncodeReview(factory.get(),cut.image,name(L"-seam.jpg"));
    c.crop_width=std::min(crop_width,feather.width);c.crop_height=std::min(crop_height,feather.height);
    const auto middle=feather.height/2,center=cut.record.seam_x[middle];
    c.crop_left=std::min(center>c.crop_width/2?center-c.crop_width/2:0u,feather.width-c.crop_width);c.crop_top=std::min(middle>c.crop_height/2?middle-c.crop_height/2:0u,feather.height-c.crop_height);
    const auto crop=[&](const Image& from){
        Image out{c.crop_width,c.crop_height,std::vector<std::uint8_t>(static_cast<size_t>(c.crop_width)*c.crop_height*3)};
        for(unsigned y=0;y<c.crop_height;++y)std::copy_n(from.bgr.data()+(static_cast<size_t>(c.crop_top+y)*from.width+c.crop_left)*3,static_cast<size_t>(c.crop_width)*3,out.bgr.data()+static_cast<size_t>(y)*c.crop_width*3);
        return out;
    };
    auto feather_crop=crop(feather),seam_crop=crop(cut.image);
    (void)EncodeReview(factory.get(),feather_crop,name(L"-feather-crop.jpg"));(void)EncodeReview(factory.get(),seam_crop,name(L"-seam-crop.jpg"));
    c.seam=std::move(cut.record);return c;
}
}
