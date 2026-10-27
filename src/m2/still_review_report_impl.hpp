// Included inside experimental after the shared immutable/WIC helpers.
namespace {
namespace ReviewJson=a0::common::protocol_json;
using ReviewValue=ReviewJson::JsonValue;
[[noreturn]] void ReviewReject(){throw std::invalid_argument("saved uncalibrated review failed validation");}
struct ReviewJsonFailure {[[noreturn]] static void Fail(std::string_view,std::string_view){ReviewReject();}};
void ReviewFields(const ReviewValue& value,std::initializer_list<const char*> fields,const char* optional=nullptr){
    if(value.kind!=ReviewJson::JsonKind::object || value.object.size()!=fields.size()+(optional && value.object.contains(optional)?1u:0u))ReviewReject();
    for(const auto* field:fields)if(!value.object.contains(field))ReviewReject();
}
const ReviewValue& ReviewAt(const ReviewValue& value,const char* field){
    if(value.kind!=ReviewJson::JsonKind::object || !value.object.contains(field))ReviewReject();return value.object.at(field);
}
std::string ReviewString(const ReviewValue& value){if(value.kind!=ReviewJson::JsonKind::string || value.string.empty() || value.string.find('\0')!=std::string::npos)ReviewReject();return value.string;}
std::string ReviewHash(const ReviewValue& value){auto s=ReviewString(value);if(s.size()!=64 || s.find_first_not_of("0123456789abcdef")!=std::string::npos)ReviewReject();return s;}
std::uint64_t ReviewUnsigned(const ReviewValue& value,std::uint64_t maximum){
    if(value.kind!=ReviewJson::JsonKind::number || value.string.empty() || value.string.find_first_not_of("0123456789")!=std::string::npos)ReviewReject();
    std::uint64_t n{};const auto parsed=std::from_chars(value.string.data(),value.string.data()+value.string.size(),n);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.string.data()+value.string.size() || n>maximum)ReviewReject();return n;
}
int ReviewSigned(const ReviewValue& value,int minimum,int maximum){
    if(value.kind!=ReviewJson::JsonKind::number || value.string.empty())ReviewReject();
    int n{};const auto parsed=std::from_chars(value.string.data(),value.string.data()+value.string.size(),n);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.string.data()+value.string.size() || n<minimum || n>maximum)ReviewReject();return n;
}
void ReviewFalse(const ReviewValue& value){if(value.kind!=ReviewJson::JsonKind::boolean || value.boolean)ReviewReject();}
double ReviewDouble(const ReviewValue& value){
    if(value.kind!=ReviewJson::JsonKind::number)ReviewReject();double n{};
    const auto parsed=std::from_chars(value.string.data(),value.string.data()+value.string.size(),n,std::chars_format::general);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.string.data()+value.string.size() || !std::isfinite(n))ReviewReject();return n;
}
std::filesystem::path ReviewPath(const ReviewValue& value){
    const auto text=ReviewString(value);if(text.size()>32768)ReviewReject();
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)ReviewReject();std::wstring wide(size,L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),size)!=size)ReviewReject();
    std::filesystem::path path(wide);if(!path.is_absolute() || path.lexically_normal()!=path)ReviewReject();return path.make_preferred();
}
bool SameReviewGeometry(const PreviewReviewGeometry& a,const PreviewReviewGeometry& b){
    return a.width==b.width && a.height==b.height && a.overlap==b.overlap && a.vertical==b.vertical && a.revision==b.revision && a.orientation==b.orientation;
}
// Every recorded decision is recomputed from the recorded values with the
// writer's rule; a report whose decision or warnings disagree is rejected.
AutomaticStillAlignment ReadAutomaticAlignment(const ReviewValue& value){
    ReviewFields(value,{"estimator","status","reason","initialPosition","tiles","similarity","translationOnly","residualPixels","decision","warnings","policy"});
    if(ReviewString(ReviewAt(value,"estimator"))!="a0.overlap-similarity.v1")ReviewReject();
    OverlapSimilarityEstimate e;const auto status=ReviewString(ReviewAt(value,"status"));
    if(status==OverlapEstimateStatusCode(OverlapEstimateStatus::estimated_not_judged))e.status=OverlapEstimateStatus::estimated_not_judged;else if(status!=OverlapEstimateStatusCode(OverlapEstimateStatus::undetermined))ReviewReject();
    const auto reason=ReviewString(ReviewAt(value,"reason"));bool known=false;
    for(const auto v:{SimilarityUndeterminedReason::none,SimilarityUndeterminedReason::no_tiles,SimilarityUndeterminedReason::insufficient_tiles,SimilarityUndeterminedReason::outside_image,SimilarityUndeterminedReason::tied_starts})
        if(reason==SimilarityUndeterminedReasonCode(v)){e.reason=v;known=true;}
    const auto start=ReviewString(ReviewAt(value,"initialPosition"));
    if(!known || (start!="live-preview" && start!="still-wide-search") || (e.status==OverlapEstimateStatus::estimated_not_judged)!=(e.reason==SimilarityUndeterminedReason::none))ReviewReject();
    e.wide_search=start=="still-wide-search";
    const auto& tiles=ReviewAt(value,"tiles");ReviewFields(tiles,{"searched","matched","agreed"});
    const auto searched=ReviewUnsigned(ReviewAt(tiles,"searched"),4096);e.matched=static_cast<unsigned>(ReviewUnsigned(ReviewAt(tiles,"matched"),searched));e.agreed=static_cast<unsigned>(ReviewUnsigned(ReviewAt(tiles,"agreed"),e.matched));
    e.tiles.resize(static_cast<size_t>(searched));
    const auto& fit=ReviewAt(value,"similarity");const auto& shift=ReviewAt(value,"translationOnly");const auto& residual=ReviewAt(value,"residualPixels");
    const bool estimated=e.status==OverlapEstimateStatus::estimated_not_judged;
    if(!estimated){if(fit.kind!=ReviewJson::JsonKind::null_value || shift.kind!=ReviewJson::JsonKind::null_value || residual.kind!=ReviewJson::JsonKind::null_value)ReviewReject();}
    else{
        ReviewFields(fit,{"x","y","rotationDegrees","scale"});ReviewFields(shift,{"x","y"});ReviewFields(residual,{"similarityRms","similarityMax","translationOnlyRms","translationOnlyMax"});
        e.similarity={ReviewDouble(ReviewAt(fit,"x")),ReviewDouble(ReviewAt(fit,"y")),ReviewDouble(ReviewAt(fit,"rotationDegrees")),ReviewDouble(ReviewAt(fit,"scale"))};
        e.translation_x=ReviewDouble(ReviewAt(shift,"x"));e.translation_y=ReviewDouble(ReviewAt(shift,"y"));
        e.similarity_residual={ReviewDouble(ReviewAt(residual,"similarityRms")),ReviewDouble(ReviewAt(residual,"similarityMax"))};
        e.translation_residual={ReviewDouble(ReviewAt(residual,"translationOnlyRms")),ReviewDouble(ReviewAt(residual,"translationOnlyMax"))};
        if(e.agreed<3 || e.similarity.scale<=0 || std::abs(e.similarity.rotation_degrees)>180)ReviewReject();
        for(const double d:{e.similarity_residual.rms,e.similarity_residual.maximum,e.translation_residual.rms,e.translation_residual.maximum})if(d<0)ReviewReject();
    }
    const auto& policy_value=ReviewAt(value,"policy");std::optional<StillMergePolicy> policy;
    if(policy_value.kind!=ReviewJson::JsonKind::null_value){
        ReviewFields(policy_value,{"sha256","rotationAutoCorrectionMaxDegrees","similarityResidualMaxWarningPixels"});
        StillMergePolicy p{ReviewHash(ReviewAt(policy_value,"sha256")),ReviewDouble(ReviewAt(policy_value,"rotationAutoCorrectionMaxDegrees")),{}};
        const auto& warning=ReviewAt(policy_value,"similarityResidualMaxWarningPixels");
        if(warning.kind!=ReviewJson::JsonKind::null_value)p.residual_max_pixels=ReviewDouble(warning);
        try{ValidateStillMergePolicy(p);}catch(const std::invalid_argument&){ReviewReject();}
        policy=std::move(p);
    }
    auto decided=DecideStillAlignment(e,policy);
    const auto& warnings=ReviewAt(value,"warnings");if(warnings.kind!=ReviewJson::JsonKind::array)ReviewReject();
    std::vector<std::string> expected;if(decided.rotation_exceeded)expected.push_back("rotation-exceeded");if(decided.residual_exceeded)expected.push_back("fit-residual-exceeded");
    if(warnings.array.size()!=expected.size())ReviewReject();
    for(size_t i=0;i<expected.size();++i)if(ReviewString(warnings.array[i])!=expected[i])ReviewReject();
    if(ReviewString(ReviewAt(value,"decision"))!=StillAlignmentDecisionCode(decided.decision))ReviewReject();
    return decided;
}
SavedStillReview ReadReviewMetadata(const std::filesystem::path& path){
    const auto full=NormalizedAbsolute(path);if(full.filename()!=L"uncalibrated-review.json")ReviewReject();
    V2PathPins pins;pins.Pin(full);LockedReadFile file(full,false,true);const auto bytes=file.ReadAll(256*1024);
    const auto root=ReviewJson::BasicJsonParser<ReviewJsonFailure>(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())).Parse();
    const auto schema=ReviewString(ReviewAt(root,"schema"));const bool similarity=schema=="a0.uncalibrated-still-review.v3",manual=similarity || schema=="a0.uncalibrated-still-review.v2";
    const bool automatic=schema=="a0.uncalibrated-still-review.v4";
    const auto basis=automatic?ReviewString(ReviewAt(root,"geometryBasis")):std::string();const bool corrected=basis=="auto-still-similarity";
    if(automatic && !corrected && basis!="auto-still-translation" && basis!="live-preview")ReviewReject();
    // mountOrientation is the only optional field in v1..v4: absent is normal.
    if(automatic && corrected)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","adjustment","crop","automaticAlignment"},"mountOrientation");
    else if(automatic)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","automaticAlignment"},"mountOrientation");
    else if(similarity)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","parentReview","adjustment","crop"},"mountOrientation");
    else if(manual)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","parentReview"},"mountOrientation");
    else{if(schema!="a0.uncalibrated-still-review.v1")ReviewReject();ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output"},"mountOrientation");}
    if(ReviewString(ReviewAt(root,"state"))!="UncalibratedReviewReady" || ReviewUnsigned(ReviewAt(root,"automaticRetryCount"),0)!=0)ReviewReject();
    for(const auto* flag:{"productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified"})ReviewFalse(ReviewAt(root,flag));
    SavedStillReview record;record.report=full;Sha256Provider sha;record.report_sha256=ToLowerHex(sha.Compute(bytes));
    record.pair_id=ReviewString(ReviewAt(root,"pairId"));if(record.pair_id.size()>80 || record.pair_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)ReviewReject();
    const auto& inputs=ReviewAt(root,"inputs");if(inputs.kind!=ReviewJson::JsonKind::array || inputs.array.size()!=2)ReviewReject();
    for(int i=0;i<2;++i){
        const auto& input=inputs.array[i];ReviewFields(input,{"alias","path","sha256","bytes"});const auto alias=i==0?"CAM-A":"CAM-B";
        if(ReviewString(ReviewAt(input,"alias"))!=alias)ReviewReject();auto original=ReviewPath(ReviewAt(input,"path"));
        if(original.filename()!=L"original.jpg" || original.parent_path().filename()!=alias)ReviewReject();
        const auto hash=ReviewHash(ReviewAt(input,"sha256"));const auto size=ReviewUnsigned(ReviewAt(input,"bytes"),kMaximumCompressedJpegBytes);if(!size)ReviewReject();
        if(i==0){record.camera_a_original=original;record.camera_a_sha256=hash;record.camera_a_bytes=size;}else{record.camera_b_original=original;record.camera_b_sha256=hash;record.camera_b_bytes=size;}
    }
    const auto& geometry=ReviewAt(root,"previewGeometry");ReviewFields(geometry,{"width","height","overlap","vertical","revision"});
    record.geometry={static_cast<unsigned>(ReviewUnsigned(ReviewAt(geometry,"width"),640)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(geometry,"height"),640)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(geometry,"overlap"),640)),ReviewSigned(ReviewAt(geometry,"vertical"),-639,639),ReviewUnsigned(ReviewAt(geometry,"revision"),std::numeric_limits<std::uint64_t>::max())};
    if(root.object.contains("mountOrientation")){
        // One state, one report hash: an explicit "normal" is not canonical.
        if(ParseMountOrientation(ReviewString(ReviewAt(root,"mountOrientation")))!=MountOrientation::rotated_180)ReviewReject();
        record.geometry.orientation=MountOrientation::rotated_180;
    }
    const auto& source=ReviewAt(root,"sourceDimensions");ReviewFields(source,{"width","height"});
    record.source_width=static_cast<unsigned>(ReviewUnsigned(ReviewAt(source,"width"),render::kMaximumImageDimension));record.source_height=static_cast<unsigned>(ReviewUnsigned(ReviewAt(source,"height"),render::kMaximumImageDimension));
    const auto base=ScalePreviewReviewGeometry(record.geometry,record.source_width,record.source_height);
    const auto& translation=ReviewAt(root,"translation");ReviewFields(translation,{"x","y"});
    record.translation_x=ReviewSigned(ReviewAt(translation,"x"),1,32767);record.translation_y=ReviewSigned(ReviewAt(translation,"y"),-32767,32767);
    if(!manual && !automatic && (base.camera_b_to_camera_a[2]!=record.translation_x || base.camera_b_to_camera_a[5]!=record.translation_y))ReviewReject();
    if(automatic){
        record.automatic=ReadAutomaticAlignment(ReviewAt(root,"automaticAlignment"));const auto& m=*record.automatic;
        const auto expected=m.decision==StillAlignmentDecision::corrected?"auto-still-similarity":m.similarity?"auto-still-translation":"live-preview";
        if(basis!=expected)ReviewReject();
        if(m.similarity){const auto t=StillAlignmentTranslation(m);if(t[0]!=record.translation_x || t[1]!=record.translation_y)ReviewReject();}
        else if(base.camera_b_to_camera_a[2]!=record.translation_x || base.camera_b_to_camera_a[5]!=record.translation_y)ReviewReject();
    }
    if(similarity || corrected){
        const auto& adjustment=ReviewAt(root,"adjustment");ReviewFields(adjustment,{"anchor","rotationDegrees","scale"});
        if(ReviewString(ReviewAt(adjustment,"anchor"))!="image-center")ReviewReject();
        record.rotation_degrees=ReviewDouble(ReviewAt(adjustment,"rotationDegrees"));record.scale=ReviewDouble(ReviewAt(adjustment,"scale"));
        if(corrected && (record.rotation_degrees!=record.automatic->similarity->rotation_degrees || record.scale!=record.automatic->similarity->scale))ReviewReject();
    }
    const auto parameters=SimilarityReviewGeometry(record.translation_x,record.translation_y,record.rotation_degrees,record.scale,record.source_width,record.source_height);
    record.crop=parameters.crop;
    if(similarity || corrected){
        const auto& crop=ReviewAt(root,"crop");ReviewFields(crop,{"left","top","right","bottom"});
        if(ReviewUnsigned(ReviewAt(crop,"left"),32768)!=parameters.crop.left || ReviewUnsigned(ReviewAt(crop,"top"),32768)!=parameters.crop.top ||
            ReviewUnsigned(ReviewAt(crop,"right"),32768)!=parameters.crop.right || ReviewUnsigned(ReviewAt(crop,"bottom"),32768)!=parameters.crop.bottom)ReviewReject();
    }
    const auto plan=render::PlanPairCanvas(record.source_width,record.source_height,record.source_width,record.source_height,parameters);
    const auto& output=ReviewAt(root,"output");ReviewFields(output,{"path","width","height","bytes","sha256"});
    if(ReviewString(ReviewAt(output,"path"))!="uncalibrated-review.jpg")ReviewReject();record.jpeg=full.parent_path()/L"uncalibrated-review.jpg";
    record.width=static_cast<unsigned>(ReviewUnsigned(ReviewAt(output,"width"),render::kMaximumImageDimension));record.height=static_cast<unsigned>(ReviewUnsigned(ReviewAt(output,"height"),render::kMaximumImageDimension));
    if(record.width!=plan.output_width || record.height!=plan.output_height)ReviewReject();record.jpeg_bytes=ReviewUnsigned(ReviewAt(output,"bytes"),kMaximumCompressedJpegBytes);if(!record.jpeg_bytes)ReviewReject();record.jpeg_sha256=ReviewHash(ReviewAt(output,"sha256"));
    if(manual){
        if(ReviewString(ReviewAt(root,"geometryBasis"))!=(similarity?"manual-still-similarity":"manual-still-pixels"))ReviewReject();const auto& parent=ReviewAt(root,"parentReview");ReviewFields(parent,{"path","sha256"});
        auto parent_path=ReviewPath(ReviewAt(parent,"path"));if(parent_path==record.report || parent_path.filename()!=L"uncalibrated-review.json")ReviewReject();
        record.manual_revision=StillReviewRequest::ManualRevision{record.translation_x,record.translation_y,parent_path,ReviewHash(ReviewAt(parent,"sha256")),record.rotation_degrees,record.scale};
    }
    return record;
}
void VerifyReviewParent(const SavedStillReview& record){
    if(!record.manual_revision)return;
    const auto& link=*record.manual_revision;const auto parent=ReadReviewMetadata(link.parent_report);
    if(parent.report_sha256!=link.parent_report_sha256 || parent.pair_id!=record.pair_id || parent.camera_a_original!=record.camera_a_original ||
        parent.camera_b_original!=record.camera_b_original || parent.camera_a_sha256!=record.camera_a_sha256 || parent.camera_b_sha256!=record.camera_b_sha256 ||
        parent.source_width!=record.source_width || parent.source_height!=record.source_height || !SameReviewGeometry(parent.geometry,record.geometry))ReviewReject();
}
}
SavedStillReview LoadSavedStillReview(const std::filesystem::path& path){
    auto record=ReadReviewMetadata(path);V2PathPins pins;
    for(const auto& p:{record.report,record.jpeg,record.camera_a_original,record.camera_b_original})pins.Pin(p);
    LockedReadFile report(record.report,false,true),a(record.camera_a_original,false,true),b(record.camera_b_original,false,true),output(record.jpeg,false,true);
    if(a.SameFileAs(b) || output.SameFileAs(a) || output.SameFileAs(b))ReviewReject();
    Sha256Provider sha;if(ToLowerHex(sha.Compute(report.ReadAll(256*1024)))!=record.report_sha256)ReviewReject();
    ComApartment apartment;auto factory=CreateFactory();
    for(int i=0;i<3;++i){const auto& file=i==0?a:i==1?b:output;JpegDimensions dimensions{};
        const auto snapshot=ReadJpegSnapshotStructureOnly(factory.get(),file,"saved review binding",true,&dimensions);
        const auto& hash=i==0?record.camera_a_sha256:i==1?record.camera_b_sha256:record.jpeg_sha256;
        const auto size=i==0?record.camera_a_bytes:i==1?record.camera_b_bytes:record.jpeg_bytes;
        if(ToLowerHex(snapshot.sha256)!=hash || snapshot.compressed.size()!=size || dimensions.width!=(i<2?record.source_width:record.width) || dimensions.height!=(i<2?record.source_height:record.height))ReviewReject();
    }
    VerifyReviewParent(record);return record;
}
std::string DescribeSavedReviewEvaluationInputs(const std::filesystem::path& path){
    const auto record=LoadSavedStillReview(path);
    const auto file=[](const std::filesystem::path& p,const std::string& hash,std::uint64_t size){
        const auto utf8=p.generic_u8string();
        return std::string("{\"path\":\"")+ReviewJson::JsonEscape(std::string_view(reinterpret_cast<const char*>(utf8.data()),utf8.size()))+
            "\",\"sha256\":\""+hash+"\",\"encodedSizeBytes\":"+std::to_string(size)+"}";
    };
    // Orientation travels as a record only. The evaluator keeps reading the
    // unrotated originals; its draft profile projection absorbs the mount.
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<"{\"schemaVersion\":\"a0.saved-review-evaluation-inputs.v1\",\"pairId\":\""<<record.pair_id
        <<"\",\"productApproved\":false,\"quality\":\"not-evaluated\",\"sourceDimensions\":{\"width\":"<<record.source_width
        <<",\"height\":"<<record.source_height<<"},\"mountOrientation\":\""<<MountOrientationName(record.geometry.orientation)<<"\",\"reviewReport\":"<<file(record.report,record.report_sha256,std::filesystem::file_size(record.report))
        <<",\"reviewImage\":"<<file(record.jpeg,record.jpeg_sha256,record.jpeg_bytes)<<",\"inputs\":[";
    for(int i=0;i<2;++i){
        auto input=file(i==0?record.camera_a_original:record.camera_b_original,i==0?record.camera_a_sha256:record.camera_b_sha256,
            i==0?record.camera_a_bytes:record.camera_b_bytes);
        if(i)out<<',';out<<"{\"cameraAlias\":\""<<(i==0?"CAM-A":"CAM-B")<<"\","<<input.substr(1);
    }
    out<<"],\"parentReview\":";
    if(record.manual_revision){const auto& parent=*record.manual_revision;
        out<<file(parent.parent_report,parent.parent_report_sha256,std::filesystem::file_size(parent.parent_report));
    }else out<<"null";
    return out.str()+"}";
}
render::BgrImage LoadStillReviewImage(const SavedStillReview& record){
    V2PathPins pins;pins.Pin(record.jpeg);LockedReadFile file(record.jpeg,false,true);ComApartment apartment;auto factory=CreateFactory();
    auto image=ReadJpegSnapshot(factory.get(),file,"review inspection image",true);
    if(ToLowerHex(image.sha256)!=record.jpeg_sha256 || image.compressed.size()!=record.jpeg_bytes || image.image.width!=record.width || image.image.height!=record.height)ReviewReject();
    return std::move(image.image);
}
std::array<render::BgrImage,2> LoadStillReviewSources(const SavedStillReview& record){
    V2PathPins pins;pins.Pin(record.camera_a_original);pins.Pin(record.camera_b_original);
    LockedReadFile a(record.camera_a_original,false,true),b(record.camera_b_original,false,true);if(a.SameFileAs(b))ReviewReject();
    ComApartment apartment;auto factory=CreateFactory();std::array<render::BgrImage,2> images;
    for(int i=0;i<2;++i){auto image=ReadJpegSnapshot(factory.get(),i==0?a:b,"comparison source",true);
        if(ToLowerHex(image.sha256)!=(i==0?record.camera_a_sha256:record.camera_b_sha256) || image.compressed.size()!=(i==0?record.camera_a_bytes:record.camera_b_bytes) || image.image.width!=record.source_width || image.image.height!=record.source_height)ReviewReject();
        images[i]=std::move(image.image);OrientRenderInput(images[i],record.geometry.orientation);
    }return images;
}
