// Included after the saved-review reader, inside a0::m2, only in dev builds.
namespace experimental {
namespace {
struct SessionReviewLock {
    SavedStillReview review;
    V2PathPins pins;
    std::vector<std::unique_ptr<LockedReadFile>> files;
    SessionReviewLock(const std::filesystem::path& report,const std::string& expected):review(ReadReviewMetadata(report)){
        for(const auto& path:{review.report,review.jpeg,review.camera_a_original,review.camera_b_original}){
            pins.Pin(path);files.push_back(std::make_unique<LockedReadFile>(path,false,true));
        }
        if(review.manual_revision){const auto& parent=review.manual_revision->parent_report;pins.Pin(parent);files.push_back(std::make_unique<LockedReadFile>(parent,false,true));}
        const auto verified=LoadSavedStillReview(review.report);
        if(verified.report_sha256!=review.report_sha256 || verified.report_sha256!=expected)ReviewReject();
        review=verified;
    }
};
const char* SessionRole(ReviewPointRole role){
    if(role==ReviewPointRole::fitting)return "fitting";if(role==ReviewPointRole::validation)return "validation";ReviewReject();
}
ReviewPointRole ReadSessionRole(const ReviewValue& value){
    const auto role=ReviewString(value);if(role=="fitting")return ReviewPointRole::fitting;if(role=="validation")return ReviewPointRole::validation;ReviewReject();
}
void ValidateSessionState(const ReviewSessionState& state,const SavedStillReview& review){
    (void)SessionRole(state.next_point_role);
    ValidateReviewCorrespondences(state.points,review.source_width,review.source_height);
    (void)SimilarityReviewGeometry(state.x,state.y,state.degrees,state.scale,review.source_width,review.source_height);
}
render::Point ReadSessionPoint(const ReviewValue& value){
    ReviewFields(value,{"x","y"});return {ReviewDouble(ReviewAt(value,"x")),ReviewDouble(ReviewAt(value,"y"))};
}
}
SavedReviewSession LoadReviewSession(const std::filesystem::path& path){
    const auto full=NormalizedAbsolute(path);if(full.filename()!=L"review-session.json")ReviewReject();
    V2PathPins pins;pins.Pin(full);LockedReadFile file(full,false,true);const auto bytes=file.ReadAll(256*1024);
    const auto root=ReviewJson::BasicJsonParser<ReviewJsonFailure>(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())).Parse();
    ReviewFields(root,{"schema","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","review","pairId","inputs","sourceDimensions","adjustment","nextPointRole","points"});
    if(ReviewString(ReviewAt(root,"schema"))!="a0.still-review-session.v1")ReviewReject();
    for(const auto* flag:{"productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified"})ReviewFalse(ReviewAt(root,flag));
    const auto& link=ReviewAt(root,"review");ReviewFields(link,{"path","sha256"});
    ReviewSessionState state;state.review_report=ReviewPath(ReviewAt(link,"path"));state.review_report_sha256=ReviewHash(ReviewAt(link,"sha256"));
    SessionReviewLock locked(state.review_report,state.review_report_sha256);const auto& review=locked.review;
    if(ReviewString(ReviewAt(root,"pairId"))!=review.pair_id)ReviewReject();
    const auto& inputs=ReviewAt(root,"inputs");if(inputs.kind!=ReviewJson::JsonKind::array || inputs.array.size()!=2)ReviewReject();
    for(int i=0;i<2;++i){const auto& input=inputs.array[i];ReviewFields(input,{"alias","sha256","bytes"});
        if(ReviewString(ReviewAt(input,"alias"))!=(i==0?"CAM-A":"CAM-B") || ReviewHash(ReviewAt(input,"sha256"))!=(i==0?review.camera_a_sha256:review.camera_b_sha256) ||
            ReviewUnsigned(ReviewAt(input,"bytes"),kMaximumCompressedJpegBytes)!=(i==0?review.camera_a_bytes:review.camera_b_bytes))ReviewReject();
    }
    const auto& dimensions=ReviewAt(root,"sourceDimensions");ReviewFields(dimensions,{"width","height"});
    if(ReviewUnsigned(ReviewAt(dimensions,"width"),render::kMaximumImageDimension)!=review.source_width || ReviewUnsigned(ReviewAt(dimensions,"height"),render::kMaximumImageDimension)!=review.source_height)ReviewReject();
    const auto& adjustment=ReviewAt(root,"adjustment");ReviewFields(adjustment,{"x","y","anchor","rotationDegrees","scale"});
    if(ReviewString(ReviewAt(adjustment,"anchor"))!="image-center")ReviewReject();
    state.x=ReviewSigned(ReviewAt(adjustment,"x"),1,32767);state.y=ReviewSigned(ReviewAt(adjustment,"y"),-32767,32767);
    state.degrees=ReviewDouble(ReviewAt(adjustment,"rotationDegrees"));state.scale=ReviewDouble(ReviewAt(adjustment,"scale"));
    state.next_point_role=ReadSessionRole(ReviewAt(root,"nextPointRole"));
    const auto& points=ReviewAt(root,"points");if(points.kind!=ReviewJson::JsonKind::array || points.array.size()>32)ReviewReject();
    for(const auto& point:points.array){ReviewFields(point,{"a","b","role"});state.points.push_back({ReadSessionPoint(ReviewAt(point,"a")),ReadSessionPoint(ReviewAt(point,"b")),ReadSessionRole(ReviewAt(point,"role"))});}
    ValidateSessionState(state,review);Sha256Provider sha;return {full,ToLowerHex(sha.Compute(bytes)),review,std::move(state)};
}
SavedReviewSession WriteReviewSession(const ReviewSessionState& state,const std::filesystem::path& directory,std::chrono::steady_clock::time_point deadline){
    const auto active=[&]{if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("review session deadline; files retained");};active();
    SessionReviewLock locked(state.review_report,state.review_report_sha256);const auto& review=locked.review;ValidateSessionState(state,review);active();
    const auto job=NormalizedAbsolute(directory);if(std::filesystem::exists(job))throw std::invalid_argument("session directory exists; never overwrite");
    V2PathPins pins;pins.Pin(job,true);
    std::ostringstream document;document.imbue(std::locale::classic());document<<std::setprecision(17);
    document<<"{\"schema\":\"a0.still-review-session.v1\",\"productQualityAccepted\":false,\"physicalScaleVerified\":false,\"liveToStillFieldOfViewVerified\":false,"
        <<"\"review\":{\"path\":\""<<ReviewPathJson(review.report)<<"\",\"sha256\":\""<<review.report_sha256<<"\"},\"pairId\":\""<<review.pair_id<<"\",\"inputs\":["
        <<"{\"alias\":\"CAM-A\",\"sha256\":\""<<review.camera_a_sha256<<"\",\"bytes\":"<<review.camera_a_bytes<<"},"
        <<"{\"alias\":\"CAM-B\",\"sha256\":\""<<review.camera_b_sha256<<"\",\"bytes\":"<<review.camera_b_bytes<<"}],"
        <<"\"sourceDimensions\":{\"width\":"<<review.source_width<<",\"height\":"<<review.source_height<<"},"
        <<"\"adjustment\":{\"x\":"<<state.x<<",\"y\":"<<state.y<<",\"anchor\":\"image-center\",\"rotationDegrees\":"<<state.degrees<<",\"scale\":"<<state.scale<<"},"
        <<"\"nextPointRole\":\""<<SessionRole(state.next_point_role)<<"\",\"points\":[";
    for(size_t i=0;i<state.points.size();++i){const auto& point=state.points[i];if(i)document<<',';
        document<<"{\"a\":{\"x\":"<<point.camera_a.x<<",\"y\":"<<point.camera_a.y<<"},\"b\":{\"x\":"<<point.camera_b.x<<",\"y\":"<<point.camera_b.y<<"},\"role\":\""<<SessionRole(point.role)<<"\"}";
    }
    document<<"]}";const auto text=document.str();if(text.size()>256*1024)throw std::invalid_argument("session exceeds byte limit");
    const std::vector<std::uint8_t> bytes(text.begin(),text.end());active();
    std::filesystem::create_directories(job.parent_path());pins.Pin(job.parent_path(),true);active();
    if(!std::filesystem::create_directory(job))throw std::runtime_error("cannot reserve session directory");pins.Pin(job,true);
    const auto partial=job/L"review-session.json.partial",final=job/L"review-session.json";
    active();WriteBytesToNewFile(partial,bytes);InvokeTestHook(before_partial_flush_hook);active();
    {LockedReadFile saved(partial,true,true);if(saved.ReadAll(256*1024)!=bytes)throw std::runtime_error("session partial changed");active();saved.RenameToWithoutReplace(final);}
    {LockedReadFile saved(final,false,true);if(saved.ReadAll(256*1024)!=bytes)throw std::runtime_error("session reread failed");}
    active();auto result=LoadReviewSession(final);active();return result;
}
}
