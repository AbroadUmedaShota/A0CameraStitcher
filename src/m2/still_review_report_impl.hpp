// Included inside experimental after the shared immutable/WIC helpers.
namespace {
namespace ReviewJson=a0::common::protocol_json;
using ReviewValue=ReviewJson::JsonValue;
[[noreturn]] void ReviewReject(){throw std::invalid_argument("saved uncalibrated review failed validation");}
struct ReviewJsonFailure {[[noreturn]] static void Fail(std::string_view,std::string_view){ReviewReject();}};
void ReviewFields(const ReviewValue& value,std::initializer_list<const char*> fields,std::initializer_list<const char*> optional={}){
    if(value.kind!=ReviewJson::JsonKind::object)ReviewReject();size_t present=0;for(const auto* field:optional)present+=value.object.contains(field)?1u:0u;
    if(value.object.size()!=fields.size()+present)ReviewReject();
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
    ReviewFields(value,{"estimator","status","reason","initialPosition","tiles","similarity","rigid","translationOnly","residualPixels","decision","scaleDecision","warnings","policy"});
    if(ReviewString(ReviewAt(value,"estimator"))!="a0.overlap-similarity.v2")ReviewReject();
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
    const auto& fit=ReviewAt(value,"similarity");const auto& rigid=ReviewAt(value,"rigid");const auto& shift=ReviewAt(value,"translationOnly");const auto& residual=ReviewAt(value,"residualPixels");
    const bool estimated=e.status==OverlapEstimateStatus::estimated_not_judged;
    if(!estimated){
        for(const auto* v:{&fit,&rigid,&shift,&residual})if(v->kind!=ReviewJson::JsonKind::null_value)ReviewReject();
    }else{
        ReviewFields(fit,{"x","y","rotationDegrees","scale"});ReviewFields(rigid,{"x","y"});ReviewFields(shift,{"x","y"});
        ReviewFields(residual,{"similarityRms","similarityMax","rigidRms","rigidMax","translationOnlyRms","translationOnlyMax"});
        e.similarity={ReviewDouble(ReviewAt(fit,"x")),ReviewDouble(ReviewAt(fit,"y")),ReviewDouble(ReviewAt(fit,"rotationDegrees")),ReviewDouble(ReviewAt(fit,"scale"))};
        // The rigid fit keeps the similarity's rotation with scale 1.
        e.rigid={ReviewDouble(ReviewAt(rigid,"x")),ReviewDouble(ReviewAt(rigid,"y")),e.similarity.rotation_degrees,1};
        e.translation_x=ReviewDouble(ReviewAt(shift,"x"));e.translation_y=ReviewDouble(ReviewAt(shift,"y"));
        e.similarity_residual={ReviewDouble(ReviewAt(residual,"similarityRms")),ReviewDouble(ReviewAt(residual,"similarityMax"))};
        e.rigid_residual={ReviewDouble(ReviewAt(residual,"rigidRms")),ReviewDouble(ReviewAt(residual,"rigidMax"))};
        e.translation_residual={ReviewDouble(ReviewAt(residual,"translationOnlyRms")),ReviewDouble(ReviewAt(residual,"translationOnlyMax"))};
        if(e.agreed<3 || e.similarity.scale<=0 || std::abs(e.similarity.rotation_degrees)>180)ReviewReject();
        for(const double d:{e.similarity_residual.rms,e.similarity_residual.maximum,e.rigid_residual.rms,e.rigid_residual.maximum,e.translation_residual.rms,e.translation_residual.maximum})if(d<0)ReviewReject();
    }
    const auto& policy_value=ReviewAt(value,"policy");std::optional<StillMergePolicy> policy;
    if(policy_value.kind!=ReviewJson::JsonKind::null_value){
        ReviewFields(policy_value,{"sha256","rotationAutoCorrectionMaxDegrees","scaleAutoCorrectionMaxPercent","similarityResidualMaxWarningPixels"});
        StillMergePolicy p{ReviewHash(ReviewAt(policy_value,"sha256")),ReviewDouble(ReviewAt(policy_value,"rotationAutoCorrectionMaxDegrees")),{},{}};
        const auto& warning=ReviewAt(policy_value,"similarityResidualMaxWarningPixels");
        if(warning.kind!=ReviewJson::JsonKind::null_value)p.residual_max_pixels=ReviewDouble(warning);
        const auto& scale=ReviewAt(policy_value,"scaleAutoCorrectionMaxPercent");
        if(scale.kind!=ReviewJson::JsonKind::null_value)p.scale_max_percent=ReviewDouble(scale);
        try{ValidateStillMergePolicy(p);}catch(const std::invalid_argument&){ReviewReject();}
        policy=std::move(p);
    }
    auto decided=DecideStillAlignment(e,policy);
    const auto& warnings=ReviewAt(value,"warnings");if(warnings.kind!=ReviewJson::JsonKind::array)ReviewReject();
    const auto expected=StillAlignmentWarnings(decided);
    if(warnings.array.size()!=expected.size())ReviewReject();
    for(size_t i=0;i<expected.size();++i)if(ReviewString(warnings.array[i])!=expected[i])ReviewReject();
    if(ReviewString(ReviewAt(value,"decision"))!=StillAlignmentDecisionCode(decided.decision) ||
        ReviewString(ReviewAt(value,"scaleDecision"))!=StillScaleDecisionCode(decided.scale_decision))ReviewReject();
    return decided;
}
// #368 blend record. Only the seam cut is written; the linear feather is the
// absent field, so one state has one report hash. The summary is recomputed
// from the path on read; the settings are records, read within their limits.
void WriteSeamBlendJson(std::ostream& out,const SeamBlendRecord& b){
    const auto& s=b.settings;
    out<<",\"blend\":{\"method\":\"seam-cut\",\"widthPixels\":"<<s.width_pixels<<",\"edgeMarginPixels\":"<<s.edge_margin_pixels
        <<",\"seam\":{\"startX\":"<<b.seam_x.front()<<",\"steps\":\""<<EncodeSeamSteps(b.seam_x)<<"\",\"xMinimum\":"<<b.summary.x_minimum<<",\"xMaximum\":"<<b.summary.x_maximum
        <<",\"xMean\":"<<b.summary.x_mean<<",\"differenceMean\":"<<b.difference_mean<<",\"differenceMaximum\":"<<b.difference_maximum
        <<"},\"brightness\":{\"windowHalfWidthPixels\":"<<s.brightness_half_width_pixels<<",\"smoothingRows\":"<<s.brightness_smoothing_rows
        <<",\"ratioMinimum\":"<<b.ratio_minimum<<",\"ratioMaximum\":"<<b.ratio_maximum<<",\"saturationLimitedPixels\":"<<b.saturation_limited_pixels<<"}}";
}
SeamBlendRecord ReadSeamBlend(const ReviewValue& value,std::uint32_t width,std::uint32_t height){
    ReviewFields(value,{"method","widthPixels","edgeMarginPixels","seam","brightness"});
    if(ReviewString(ReviewAt(value,"method"))!="seam-cut")ReviewReject();
    const auto& seam=ReviewAt(value,"seam");const auto& light=ReviewAt(value,"brightness");
    ReviewFields(seam,{"startX","steps","xMinimum","xMaximum","xMean","differenceMean","differenceMaximum"});
    ReviewFields(light,{"windowHalfWidthPixels","smoothingRows","ratioMinimum","ratioMaximum","saturationLimitedPixels"});
    SeamBlendRecord b;
    b.settings={static_cast<unsigned>(ReviewUnsigned(ReviewAt(value,"widthPixels"),4096)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(value,"edgeMarginPixels"),32768)),
        static_cast<unsigned>(ReviewUnsigned(ReviewAt(light,"windowHalfWidthPixels"),4096)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(light,"smoothingRows"),32768))};
    // The path may be empty for a one-row output, so it is not a ReviewString.
    const auto& steps=ReviewAt(seam,"steps");if(steps.kind!=ReviewJson::JsonKind::string || steps.string.size()+1!=height)ReviewReject();
    try{ValidateSeamBlendSettings(b.settings);b.seam_x=ExpandSeamSteps(static_cast<std::uint32_t>(ReviewUnsigned(ReviewAt(seam,"startX"),width)),steps.string,width);}
    catch(const std::invalid_argument&){ReviewReject();}
    b.summary=SummarizeSeam(b.seam_x);
    if(ReviewUnsigned(ReviewAt(seam,"xMinimum"),width)!=b.summary.x_minimum || ReviewUnsigned(ReviewAt(seam,"xMaximum"),width)!=b.summary.x_maximum ||
        ReviewDouble(ReviewAt(seam,"xMean"))!=b.summary.x_mean)ReviewReject();
    b.difference_mean=ReviewDouble(ReviewAt(seam,"differenceMean"));b.difference_maximum=ReviewDouble(ReviewAt(seam,"differenceMaximum"));
    b.ratio_minimum=ReviewDouble(ReviewAt(light,"ratioMinimum"));b.ratio_maximum=ReviewDouble(ReviewAt(light,"ratioMaximum"));
    b.saturation_limited_pixels=ReviewUnsigned(ReviewAt(light,"saturationLimitedPixels"),static_cast<std::uint64_t>(width)*height);
    if(b.difference_mean<0 || b.difference_mean>b.difference_maximum || b.ratio_minimum<=0 || b.ratio_minimum>b.ratio_maximum)ReviewReject();
    return b;
}
// #400 v6: the fixed composite. The seam is one vertical column, recorded once
// in fixedComposite.seam; the blend object keeps the measured brightness only.
// Everything derivable is recomputed on read and a disagreement is rejected.
void WriteFixedSeamBlendJson(std::ostream& out,const SeamBlendRecord& b){
    const auto& s=b.settings;
    out<<",\"blend\":{\"method\":\"fixed-seam\",\"widthPixels\":"<<s.width_pixels<<",\"edgeMarginPixels\":"<<s.edge_margin_pixels
        <<",\"differenceMean\":"<<b.difference_mean<<",\"differenceMaximum\":"<<b.difference_maximum
        <<",\"brightness\":{\"windowHalfWidthPixels\":"<<s.brightness_half_width_pixels<<",\"smoothingRows\":"<<s.brightness_smoothing_rows
        <<",\"ratioMinimum\":"<<b.ratio_minimum<<",\"ratioMaximum\":"<<b.ratio_maximum<<",\"saturationLimitedPixels\":"<<b.saturation_limited_pixels<<"}}";
}
void WriteFixedCompositeJson(std::ostream& out,const SavedFixedComposite& f,int fixed_x,int fixed_y,double fixed_rotation,double fixed_scale){
    const auto optional=[&](const std::optional<double>& v){if(v)out<<*v;else out<<"null";};
    const auto text=[&](std::string_view v){out<<'"'<<ReviewJson::JsonEscape(v)<<'"';};
    const auto& s=f.scale;const auto& d=f.diagnostic;
    out<<",\"fixedComposite\":{\"condition\":{\"sha256\":\""<<f.condition_sha256<<"\"},\"seam\":{\"column\":"<<f.seam_column<<",\"origin\":\""<<f.seam_origin
        <<"\"},\"scaleCorrection\":{\"status\":\""<<FixedScaleStatusCode(s.status)<<"\",\"reason\":\""<<FixedScaleReasonCode(s.reason)<<"\",\"limitPercent\":"<<s.limit_percent
        <<",\"estimatedScale\":";optional(s.estimated_scale);out<<",\"differencePercent\":";optional(s.difference_percent);
    out<<",\"appliedScale\":"<<s.applied_scale<<",\"center\":{\"x\":"<<s.center.x<<",\"y\":"<<s.center.y<<"},\"usedTiles\":"<<s.used_tiles<<",\"residualPixels\":";
    if(s.estimated_scale)out<<"{\"rms\":"<<s.residual.rms<<",\"maximum\":"<<s.residual.maximum<<"}";else out<<"null";
    out<<",\"notice\":";text(FixedScaleNotice(s.status));
    out<<"},\"policyLimit\":{\"check\":\""<<FixedPolicyCheckCode(f.policy_limit.check)<<"\",\"policyPercent\":";optional(f.policy_limit.policy_percent);
    out<<",\"conditionPercent\":"<<s.limit_percent<<",\"notice\":";text(FixedPolicyNotice(f.policy_limit.check));
    out<<"},\"diagnostic\":{\"estimator\":\"a0.overlap-similarity.v2\",\"status\":\""<<OverlapEstimateStatusCode(d.status)<<"\",\"reason\":\""<<SimilarityUndeterminedReasonCode(d.reason)
        <<"\",\"initialPosition\":\""<<(d.wide_search?"still-wide-search":"fixed-mapping")<<"\",\"tiles\":{\"searched\":"<<d.tiles_searched<<",\"matched\":"<<d.tiles_matched<<",\"agreed\":"<<d.tiles_agreed<<"},\"similarity\":";
    if(d.similarity){
        const auto delta=*FixedDiagnosticDelta(d,fixed_x,fixed_y,fixed_rotation,fixed_scale);
        out<<"{\"x\":"<<d.similarity->x<<",\"y\":"<<d.similarity->y<<",\"rotationDegrees\":"<<d.similarity->rotation_degrees<<",\"scale\":"<<d.similarity->scale
            <<"},\"residualPixels\":{\"rms\":"<<d.similarity_residual.rms<<",\"maximum\":"<<d.similarity_residual.maximum
            <<"},\"deltaFromFixed\":{\"x\":"<<delta.x<<",\"y\":"<<delta.y<<",\"rotationDegrees\":"<<delta.rotation_degrees<<",\"scaleRatio\":"<<delta.scale_ratio<<"}";
    }else out<<"null,\"residualPixels\":null,\"deltaFromFixed\":null";
    out<<"}}";
}
std::optional<double> ReviewOptionalDouble(const ReviewValue& v){
    if(v.kind==ReviewJson::JsonKind::null_value)return std::nullopt;return ReviewDouble(v);
}
FitResidual ReviewResidual(const ReviewValue& v){
    ReviewFields(v,{"rms","maximum"});const FitResidual r{ReviewDouble(ReviewAt(v,"rms")),ReviewDouble(ReviewAt(v,"maximum"))};
    if(r.rms<0 || r.maximum<r.rms)ReviewReject();return r;
}
void ReviewExactText(const ReviewValue& v,std::string_view expected){
    // Notices may be empty, which ReviewString refuses.
    if(v.kind!=ReviewJson::JsonKind::string || v.string!=expected)ReviewReject();
}
SavedFixedComposite ReadFixedComposite(const ReviewValue& value,int fixed_x,int fixed_y,double fixed_rotation,double fixed_scale){
    ReviewFields(value,{"condition","seam","scaleCorrection","policyLimit","diagnostic"});
    SavedFixedComposite f;
    const auto& condition=ReviewAt(value,"condition");ReviewFields(condition,{"sha256"});f.condition_sha256=ReviewHash(ReviewAt(condition,"sha256"));
    const auto& seam=ReviewAt(value,"seam");ReviewFields(seam,{"column","origin"});
    f.seam_column=static_cast<std::uint32_t>(ReviewUnsigned(ReviewAt(seam,"column"),render::kMaximumImageDimension));
    f.seam_origin=ReviewString(ReviewAt(seam,"origin"));if(!ParseFixedSeamOrigin(f.seam_origin))ReviewReject();
    const auto& s=ReviewAt(value,"scaleCorrection");
    ReviewFields(s,{"status","reason","limitPercent","estimatedScale","differencePercent","appliedScale","center","usedTiles","residualPixels","notice"});
    bool known=false;const auto status=ReviewString(ReviewAt(s,"status"));
    for(const auto v:{FixedScaleStatus::applied,FixedScaleStatus::exceeded,FixedScaleStatus::not_estimated,FixedScaleStatus::seam_outside_overlap})
        if(status==FixedScaleStatusCode(v)){f.scale.status=v;known=true;}
    const auto reason=ReviewString(ReviewAt(s,"reason"));bool reason_known=false;
    for(const auto v:{FixedScaleReason::none,FixedScaleReason::no_tiles,FixedScaleReason::insufficient_tiles,FixedScaleReason::outside_image,FixedScaleReason::tied_starts,FixedScaleReason::too_few_tiles,FixedScaleReason::degenerate_fit})
        if(reason==FixedScaleReasonCode(v)){f.scale.reason=v;reason_known=true;}
    if(!known || !reason_known)ReviewReject();
    auto& c=f.scale;
    c.limit_percent=ReviewDouble(ReviewAt(s,"limitPercent"));c.estimated_scale=ReviewOptionalDouble(ReviewAt(s,"estimatedScale"));
    c.difference_percent=ReviewOptionalDouble(ReviewAt(s,"differencePercent"));c.applied_scale=ReviewDouble(ReviewAt(s,"appliedScale"));
    const auto& center=ReviewAt(s,"center");ReviewFields(center,{"x","y"});c.center={ReviewDouble(ReviewAt(center,"x")),ReviewDouble(ReviewAt(center,"y"))};
    c.used_tiles=static_cast<unsigned>(ReviewUnsigned(ReviewAt(s,"usedTiles"),4096));
    const auto& residual=ReviewAt(s,"residualPixels");
    if(c.limit_percent<0 || c.applied_scale<=0 || c.estimated_scale.has_value()!=c.difference_percent.has_value() || c.estimated_scale.has_value()==(residual.kind==ReviewJson::JsonKind::null_value))ReviewReject();
    if(c.estimated_scale){
        if(*c.estimated_scale<=0 || *c.difference_percent!=StillScaleDifferencePercent(*c.estimated_scale))ReviewReject();
        c.residual=ReviewResidual(residual);
    }
    const bool estimated=c.estimated_scale.has_value();
    switch(c.status){
    case FixedScaleStatus::applied:
        if(!estimated || c.reason!=FixedScaleReason::none || *c.difference_percent>c.limit_percent || c.applied_scale!=*c.estimated_scale)ReviewReject();break;
    case FixedScaleStatus::exceeded:
        if(!estimated || c.reason!=FixedScaleReason::none || *c.difference_percent<=c.limit_percent || c.applied_scale!=1)ReviewReject();break;
    case FixedScaleStatus::seam_outside_overlap:
        if(!estimated || c.reason!=FixedScaleReason::none || *c.difference_percent>c.limit_percent || c.applied_scale!=1)ReviewReject();break;
    case FixedScaleStatus::not_estimated:
        if(estimated || c.reason==FixedScaleReason::none || c.applied_scale!=1)ReviewReject();break;
    }
    ReviewExactText(ReviewAt(s,"notice"),FixedScaleNotice(c.status));
    const auto& policy=ReviewAt(value,"policyLimit");ReviewFields(policy,{"check","policyPercent","conditionPercent","notice"});
    const auto check=ReviewString(ReviewAt(policy,"check"));bool check_known=false;
    for(const auto v:{FixedPolicyCheck::same,FixedPolicyCheck::differs,FixedPolicyCheck::policy_absent,FixedPolicyCheck::policy_unreadable,FixedPolicyCheck::policy_without_scale_limit})
        if(check==FixedPolicyCheckCode(v)){f.policy_limit.check=v;check_known=true;}
    f.policy_limit.policy_percent=ReviewOptionalDouble(ReviewAt(policy,"policyPercent"));
    if(!check_known || ReviewDouble(ReviewAt(policy,"conditionPercent"))!=c.limit_percent || (f.policy_limit.policy_percent && *f.policy_limit.policy_percent<0))ReviewReject();
    // The recorded check must be what the two values give.
    const auto expected_check=ComparePolicyLimit(c.limit_percent,f.policy_limit.policy_percent,f.policy_limit.check);
    if(expected_check.check!=f.policy_limit.check)ReviewReject();
    if(f.policy_limit.policy_percent.has_value()!=(f.policy_limit.check==FixedPolicyCheck::same || f.policy_limit.check==FixedPolicyCheck::differs))ReviewReject();
    ReviewExactText(ReviewAt(policy,"notice"),FixedPolicyNotice(f.policy_limit.check));
    const auto& d=ReviewAt(value,"diagnostic");
    ReviewFields(d,{"estimator","status","reason","initialPosition","tiles","similarity","residualPixels","deltaFromFixed"});
    if(ReviewString(ReviewAt(d,"estimator"))!="a0.overlap-similarity.v2")ReviewReject();
    auto& g=f.diagnostic;const auto d_status=ReviewString(ReviewAt(d,"status"));
    if(d_status==OverlapEstimateStatusCode(OverlapEstimateStatus::estimated_not_judged))g.status=OverlapEstimateStatus::estimated_not_judged;
    else if(d_status!=OverlapEstimateStatusCode(OverlapEstimateStatus::undetermined))ReviewReject();
    const auto d_reason=ReviewString(ReviewAt(d,"reason"));bool d_known=false;
    for(const auto v:{SimilarityUndeterminedReason::none,SimilarityUndeterminedReason::no_tiles,SimilarityUndeterminedReason::insufficient_tiles,SimilarityUndeterminedReason::outside_image,SimilarityUndeterminedReason::tied_starts})
        if(d_reason==SimilarityUndeterminedReasonCode(v)){g.reason=v;d_known=true;}
    const auto start=ReviewString(ReviewAt(d,"initialPosition"));
    if(!d_known || (start!="fixed-mapping" && start!="still-wide-search") || (g.status==OverlapEstimateStatus::estimated_not_judged)!=(g.reason==SimilarityUndeterminedReason::none))ReviewReject();
    g.wide_search=start=="still-wide-search";
    const auto& tiles=ReviewAt(d,"tiles");ReviewFields(tiles,{"searched","matched","agreed"});
    g.tiles_searched=static_cast<unsigned>(ReviewUnsigned(ReviewAt(tiles,"searched"),4096));g.tiles_matched=static_cast<unsigned>(ReviewUnsigned(ReviewAt(tiles,"matched"),g.tiles_searched));
    g.tiles_agreed=static_cast<unsigned>(ReviewUnsigned(ReviewAt(tiles,"agreed"),g.tiles_matched));
    const auto& fit=ReviewAt(d,"similarity");const auto& fit_residual=ReviewAt(d,"residualPixels");const auto& delta=ReviewAt(d,"deltaFromFixed");
    if(g.status!=OverlapEstimateStatus::estimated_not_judged){
        for(const auto* v:{&fit,&fit_residual,&delta})if(v->kind!=ReviewJson::JsonKind::null_value)ReviewReject();
    }else{
        ReviewFields(fit,{"x","y","rotationDegrees","scale"});
        g.similarity=SimilarityFit{ReviewDouble(ReviewAt(fit,"x")),ReviewDouble(ReviewAt(fit,"y")),ReviewDouble(ReviewAt(fit,"rotationDegrees")),ReviewDouble(ReviewAt(fit,"scale"))};
        if(g.similarity->scale<=0 || std::abs(g.similarity->rotation_degrees)>180 || g.tiles_agreed<3)ReviewReject();
        g.similarity_residual=ReviewResidual(fit_residual);
        const auto expected=*FixedDiagnosticDelta(g,fixed_x,fixed_y,fixed_rotation,fixed_scale);
        ReviewFields(delta,{"x","y","rotationDegrees","scaleRatio"});
        if(ReviewDouble(ReviewAt(delta,"x"))!=expected.x || ReviewDouble(ReviewAt(delta,"y"))!=expected.y ||
            ReviewDouble(ReviewAt(delta,"rotationDegrees"))!=expected.rotation_degrees || ReviewDouble(ReviewAt(delta,"scaleRatio"))!=expected.scale_ratio)ReviewReject();
    }
    // The scale-only fit comes from this estimate, so the two must agree.
    if(estimated && g.status!=OverlapEstimateStatus::estimated_not_judged)ReviewReject();
    if(c.status==FixedScaleStatus::not_estimated){
        if(g.status==OverlapEstimateStatus::estimated_not_judged){
            if(c.reason!=FixedScaleReason::too_few_tiles && c.reason!=FixedScaleReason::degenerate_fit)ReviewReject();
        }else{
            const auto mapped=g.reason==SimilarityUndeterminedReason::no_tiles?FixedScaleReason::no_tiles:g.reason==SimilarityUndeterminedReason::insufficient_tiles?FixedScaleReason::insufficient_tiles:
                g.reason==SimilarityUndeterminedReason::outside_image?FixedScaleReason::outside_image:FixedScaleReason::tied_starts;
            if(c.reason!=mapped)ReviewReject();
        }
    }
    return f;
}
SeamBlendRecord ReadFixedSeamBlend(const ReviewValue& value,std::uint32_t seam_column,std::uint32_t width,std::uint32_t height){
    ReviewFields(value,{"method","widthPixels","edgeMarginPixels","differenceMean","differenceMaximum","brightness"});
    if(ReviewString(ReviewAt(value,"method"))!="fixed-seam")ReviewReject();
    const auto& light=ReviewAt(value,"brightness");
    ReviewFields(light,{"windowHalfWidthPixels","smoothingRows","ratioMinimum","ratioMaximum","saturationLimitedPixels"});
    SeamBlendRecord b;
    b.settings={static_cast<unsigned>(ReviewUnsigned(ReviewAt(value,"widthPixels"),4096)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(value,"edgeMarginPixels"),32768)),
        static_cast<unsigned>(ReviewUnsigned(ReviewAt(light,"windowHalfWidthPixels"),4096)),static_cast<unsigned>(ReviewUnsigned(ReviewAt(light,"smoothingRows"),32768))};
    try{ValidateSeamBlendSettings(b.settings);}catch(const std::invalid_argument&){ReviewReject();}
    if(seam_column>=width)ReviewReject();
    b.fixed_seam=true;b.seam_x.assign(height,seam_column);b.summary=SummarizeSeam(b.seam_x);
    b.difference_mean=ReviewDouble(ReviewAt(value,"differenceMean"));b.difference_maximum=ReviewDouble(ReviewAt(value,"differenceMaximum"));
    b.ratio_minimum=ReviewDouble(ReviewAt(light,"ratioMinimum"));b.ratio_maximum=ReviewDouble(ReviewAt(light,"ratioMaximum"));
    b.saturation_limited_pixels=ReviewUnsigned(ReviewAt(light,"saturationLimitedPixels"),static_cast<std::uint64_t>(width)*height);
    if(b.difference_mean<0 || b.difference_mean>b.difference_maximum || b.ratio_minimum<=0 || b.ratio_minimum>b.ratio_maximum)ReviewReject();
    return b;
}
SavedStillReview ReadReviewMetadata(const std::filesystem::path& path){
    const auto full=NormalizedAbsolute(path);if(full.filename()!=L"uncalibrated-review.json")ReviewReject();
    V2PathPins pins;pins.Pin(full);LockedReadFile file(full,false,true);const auto bytes=file.ReadAll(256*1024);
    const auto root=ReviewJson::BasicJsonParser<ReviewJsonFailure>(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())).Parse();
    const auto schema=ReviewString(ReviewAt(root,"schema"));const bool similarity=schema=="a0.uncalibrated-still-review.v3",manual=similarity || schema=="a0.uncalibrated-still-review.v2";
    // v5 is the automatic report. v4 existed only before the scale limit
    // (#365 review) and was never released; it is not read.
    const bool automatic=schema=="a0.uncalibrated-still-review.v5";
    // v6 is the fixed composite (#400); its mapping comes from the condition, not from the live geometry.
    const bool fixed=schema=="a0.uncalibrated-still-review.v6";
    const auto basis=automatic?ReviewString(ReviewAt(root,"geometryBasis")):std::string();
    const bool corrected=basis=="auto-still-similarity" || basis=="auto-still-rotation";
    if(automatic && !corrected && basis!="auto-still-translation" && basis!="live-preview")ReviewReject();
    // Optional in v1..v5: mountOrientation (absent is normal) and blend
    // (#368, absent is the linear feather).
    if(fixed)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","adjustment","crop","fixedComposite","blend"},{"mountOrientation"});
    else if(automatic && corrected)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","adjustment","crop","automaticAlignment"},{"mountOrientation","blend"});
    else if(automatic)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","automaticAlignment"},{"mountOrientation","blend"});
    else if(similarity)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","parentReview","adjustment","crop"},{"mountOrientation","blend"});
    else if(manual)ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output","geometryBasis","parentReview"},{"mountOrientation","blend"});
    else{if(schema!="a0.uncalibrated-still-review.v1")ReviewReject();ReviewFields(root,{"schema","pairId","state","productQualityAccepted","physicalScaleVerified","liveToStillFieldOfViewVerified","automaticRetryCount","inputs","previewGeometry","sourceDimensions","translation","output"},{"mountOrientation","blend"});}
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
    if(!manual && !automatic && !fixed && (base.camera_b_to_camera_a[2]!=record.translation_x || base.camera_b_to_camera_a[5]!=record.translation_y))ReviewReject();
    if(automatic){
        record.automatic=ReadAutomaticAlignment(ReviewAt(root,"automaticAlignment"));const auto& m=*record.automatic;
        if(basis!=StillAlignmentBasis(m))ReviewReject();
        if(m.similarity){const auto t=AppliedStillAlignment(m);if(t.x!=record.translation_x || t.y!=record.translation_y)ReviewReject();}
        else if(base.camera_b_to_camera_a[2]!=record.translation_x || base.camera_b_to_camera_a[5]!=record.translation_y)ReviewReject();
    }
    if(similarity || corrected || fixed){
        const auto& adjustment=ReviewAt(root,"adjustment");ReviewFields(adjustment,{"anchor","rotationDegrees","scale"});
        if(ReviewString(ReviewAt(adjustment,"anchor"))!="image-center")ReviewReject();
        record.rotation_degrees=ReviewDouble(ReviewAt(adjustment,"rotationDegrees"));record.scale=ReviewDouble(ReviewAt(adjustment,"scale"));
        // The recorded adjustment must equal the geometry the rule applies.
        if(corrected){const auto t=AppliedStillAlignment(*record.automatic);if(record.rotation_degrees!=t.rotation_degrees || record.scale!=t.scale)ReviewReject();}
    }
    if(fixed){
        if(ReviewString(ReviewAt(root,"geometryBasis"))!="fixed-composite")ReviewReject();
        record.fixed_composite=ReadFixedComposite(ReviewAt(root,"fixedComposite"),record.translation_x,record.translation_y,record.rotation_degrees,record.scale);
    }
    const auto parameters=fixed?FixedCompositeScaledGeometry(record.translation_x,record.translation_y,record.rotation_degrees,record.scale,record.source_width,record.source_height,
        record.fixed_composite->seam_column,record.fixed_composite->scale.applied_scale):SimilarityReviewGeometry(record.translation_x,record.translation_y,record.rotation_degrees,record.scale,record.source_width,record.source_height);
    record.crop=parameters.crop;
    if(similarity || corrected || fixed){
        const auto& crop=ReviewAt(root,"crop");ReviewFields(crop,{"left","top","right","bottom"});
        if(ReviewUnsigned(ReviewAt(crop,"left"),32768)!=parameters.crop.left || ReviewUnsigned(ReviewAt(crop,"top"),32768)!=parameters.crop.top ||
            ReviewUnsigned(ReviewAt(crop,"right"),32768)!=parameters.crop.right || ReviewUnsigned(ReviewAt(crop,"bottom"),32768)!=parameters.crop.bottom)ReviewReject();
    }
    const auto plan=render::PlanPairCanvas(record.source_width,record.source_height,record.source_width,record.source_height,parameters);
    const auto& output=ReviewAt(root,"output");ReviewFields(output,{"path","width","height","bytes","sha256"});
    if(ReviewString(ReviewAt(output,"path"))!="uncalibrated-review.jpg")ReviewReject();record.jpeg=full.parent_path()/L"uncalibrated-review.jpg";
    record.width=static_cast<unsigned>(ReviewUnsigned(ReviewAt(output,"width"),render::kMaximumImageDimension));record.height=static_cast<unsigned>(ReviewUnsigned(ReviewAt(output,"height"),render::kMaximumImageDimension));
    if(record.width!=plan.output_width || record.height!=plan.output_height)ReviewReject();record.jpeg_bytes=ReviewUnsigned(ReviewAt(output,"bytes"),kMaximumCompressedJpegBytes);if(!record.jpeg_bytes)ReviewReject();record.jpeg_sha256=ReviewHash(ReviewAt(output,"sha256"));
    if(fixed){
        const auto fixed_base=SimilarityReviewGeometry(record.translation_x,record.translation_y,record.rotation_degrees,record.scale,record.source_width,record.source_height);
        const auto& f=*record.fixed_composite;const auto centre=FixedScaleCenter(record.translation_x,record.translation_y,record.rotation_degrees,record.scale,record.source_width,record.source_height,f.seam_column);
        if(f.seam_column>=record.width || f.scale.center.x!=centre.x || f.scale.center.y!=centre.y ||
            (f.seam_origin=="band-center-provisional" && f.seam_column!=ProvisionalSeamColumn(fixed_base,record.source_width,record.source_height)))ReviewReject();
        record.blend=ReadFixedSeamBlend(ReviewAt(root,"blend"),f.seam_column,record.width,record.height);
    }else if(root.object.contains("blend"))record.blend=ReadSeamBlend(ReviewAt(root,"blend"),record.width,record.height);
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
