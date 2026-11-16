#include "a0/m2/experimental/fixed_composite_recalibration.hpp"
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/fixed_composite_creation.hpp"
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/m2/experimental/seam_blend.hpp"
#include "a0/m2/optical_planner.hpp"
#include "a0/m2/rig_profile_v2.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <sstream>

namespace a0::m2::experimental::recalibration {
namespace {
namespace json=a0::common::protocol_json;
using json::JsonKind;
using json::JsonValue;
using content_registration::PoseModel;
using content_registration::PoseParameters;
using render::Point;

constexpr const char* kPrefix="fixed composite recalibration: ";
[[noreturn]] void Reject(const std::string& what){throw std::invalid_argument(kPrefix+what);}
struct Failure {[[noreturn]] static void Fail(std::string_view code,std::string_view){Reject("JSON "+std::string(code));}};

std::vector<unsigned char> Bytes(std::string_view text){return std::vector<unsigned char>(text.begin(),text.end());}
// 17 significant digits in the classic locale (every double is written exactly), a zero as 0: the notation of the formats document.
std::string Num(double v){
    if(!std::isfinite(v))throw std::invalid_argument("number is not recordable");
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<(v==0?0.0:v);
    return out.str();
}
std::string Quoted(const std::string& s){return "\""+json::JsonEscape(s)+"\"";}
std::string PoseJson(const PoseParameters& p){
    return "{\"rxDeg\":"+Num(p.rx_deg)+",\"ryDeg\":"+Num(p.ry_deg)+",\"rzDeg\":"+Num(p.rz_deg)+",\"txMm\":"+Num(p.tx_mm)+",\"tyMm\":"+Num(p.ty_mm)+
        ",\"tzPercent\":"+Num(p.tz_percent)+"}";
}
std::string StringsJson(const std::vector<std::string>& v){
    std::string s="[";
    for(std::size_t i=0;i<v.size();++i)s+=(i?",":"")+Quoted(v[i]);
    return s+"]";
}

// ---- Coverage of one camera, as the creation check of the condition counts it (the profile fixes the sensor's frame) ----
bool Covered(const render::OutputToCamera& map,double x,double y){
    Point raw{};
    try{if(!map({x,y},raw))return false;}
    catch(const std::invalid_argument&){return false;}   // a document point the projection cannot represent is not covered
    if(!std::isfinite(raw.x) || !std::isfinite(raw.y))return false;
    return raw.x>=0.0 && raw.y>=0.0 && raw.x<static_cast<double>(kD810SensorWidthPixels) && raw.y<static_cast<double>(kD810SensorHeightPixels);
}
constexpr double kScanStepPx=32.0;       // a cover run is far longer than this on any usable rig
constexpr double kScanLimitPx=65535.0;
constexpr int kBisections=18;
// The edge of the covered run that holds x0, in `direction`, to within the doubling step times 2^-kBisections.
double EdgeSearch(const render::OutputToCamera& map,double y,double x0,double direction,double limit){
    double inside=x0,outside=x0,step=1;
    for(;;){
        const double x=x0+direction*step;
        if((direction<0 && x<limit) || (direction>0 && x>limit))return limit;
        if(!Covered(map,x,y)){outside=x;break;}
        inside=x;step*=2;
    }
    for(int i=0;i<kBisections;++i){
        const double mid=(inside+outside)/2;
        if(Covered(map,mid,y))inside=mid;else outside=mid;
    }
    return inside;
}
// A covered position on the row, by a coarse scan from `from` upward; nullopt when there is none before kScanLimitPx.
std::optional<double> FindCovered(const render::OutputToCamera& map,double y,double from){
    for(double x=from;x<kScanLimitPx;x+=kScanStepPx)if(Covered(map,x,y))return x;
    return std::nullopt;
}
struct Run {double left{},right{};};
std::optional<Run> RunOfRow(const render::OutputToCamera& map,double y,double scan_from=0.0){
    const auto start=FindCovered(map,y,scan_from);
    if(!start)return std::nullopt;
    return Run{EdgeSearch(map,y,*start,-1,-kScanLimitPx),EdgeSearch(map,y,*start,+1,kScanLimitPx)};
}

// ---- A small JSON writer over the common parser's value (keys in the parser's order) ----
std::string Dump(const JsonValue& v){
    switch(v.kind){
    case JsonKind::string:return Quoted(v.string);
    case JsonKind::boolean:return v.boolean?"true":"false";
    case JsonKind::number:return v.string;
    case JsonKind::null_value:return "null";
    case JsonKind::array:{
        std::string s="[";
        for(std::size_t i=0;i<v.array.size();++i)s+=(i?",":"")+Dump(v.array[i]);
        return s+"]";
    }
    case JsonKind::object:{
        std::string s="{";bool first=true;
        for(const auto& [key,child]:v.object){s+=(first?"":",")+Quoted(key)+":"+Dump(child);first=false;}
        return s+"}";
    }
    }
    return "null";
}
JsonValue NumberValue(double v){JsonValue j;j.kind=JsonKind::number;j.string=Num(v);return j;}
JsonValue StringValue(const std::string& v){JsonValue j;j.kind=JsonKind::string;j.string=v;return j;}
JsonValue NullValue(){return JsonValue{};}
JsonValue& Member(JsonValue& v,const char* key){
    if(v.kind!=JsonKind::object)Reject(std::string("the profile has no object where ")+key+" is expected");
    const auto found=v.object.find(key);
    if(found==v.object.end())Reject(std::string("the profile has no member ")+key);
    return found->second;
}
double RealOf(const JsonValue& v){
    if(v.kind!=JsonKind::number)Reject("the profile has a value that is not a number");
    double n{};
    const auto r=std::from_chars(v.string.data(),v.string.data()+v.string.size(),n);
    if(r.ec!=std::errc{} || r.ptr!=v.string.data()+v.string.size() || !std::isfinite(n))Reject("the profile has a number that is not finite");
    return n;
}
std::string StringOf(const JsonValue& v){
    if(v.kind!=JsonKind::string)Reject("the profile has a value that is not a string");
    return v.string;
}
render::CameraProjection ProjectionOf(JsonValue& projection){
    auto& k=Member(projection,"intrinsics");auto& d=Member(projection,"distortion");auto& rows=Member(projection,"documentToImage");
    if(rows.kind!=JsonKind::array || rows.array.size()!=3)Reject("documentToImage is not a 3 x 3 matrix");
    render::Matrix3 h{};
    for(std::size_t row=0;row<3;++row){
        if(rows.array[row].kind!=JsonKind::array || rows.array[row].array.size()!=3)Reject("documentToImage is not a 3 x 3 matrix");
        for(std::size_t column=0;column<3;++column)h[row*3+column]=RealOf(rows.array[row].array[column]);
    }
    return render::CameraProjection(render::CameraIntrinsics(RealOf(Member(k,"fxPixels")),RealOf(Member(k,"fyPixels")),RealOf(Member(k,"cxPixels")),RealOf(Member(k,"cyPixels"))),
        render::LensDistortion(RealOf(Member(d,"k1")),RealOf(Member(d,"k2")),RealOf(Member(d,"k3")),RealOf(Member(d,"p1")),RealOf(Member(d,"p2"))),h);
}
JsonValue MatrixValue(const render::Matrix3& h){
    JsonValue rows;rows.kind=JsonKind::array;
    for(std::size_t row=0;row<3;++row){
        JsonValue r;r.kind=JsonKind::array;
        for(std::size_t column=0;column<3;++column)r.array.push_back(NumberValue(row==2 && column==2?1.0:h[row*3+column]));
        rows.array.push_back(std::move(r));
    }
    return rows;
}

// What the parent profile says about itself that the new record and the checks need.
struct ParentFacts {
    double sheet_width_mm{},sheet_height_mm{};
    std::string provenance_id,tool_id;
};
JsonValue ParseProfileTree(std::string_view bytes){
    if(bytes.size()>kMaximumFixedCompositeProfileBytes)Reject("the profile is too large");
    return json::BasicJsonParser<Failure>(bytes).Parse();
}
ParentFacts FactsOf(std::string_view profile_bytes){
    auto tree=ParseProfileTree(profile_bytes);
    ParentFacts f;
    auto& plane=Member(tree,"documentPlane");
    f.sheet_width_mm=RealOf(Member(plane,"sheetWidthMm"));f.sheet_height_mm=RealOf(Member(plane,"sheetHeightMm"));
    auto& calibration=Member(tree,"calibration");
    f.provenance_id=StringOf(Member(calibration,"provenanceId"));
    f.tool_id=StringOf(Member(Member(calibration,"tool"),"toolId"));
    return f;
}

PoseParameters Minus(const PoseParameters& a,const PoseParameters& b){
    return {a.rx_deg-b.rx_deg,a.ry_deg-b.ry_deg,a.rz_deg-b.rz_deg,a.tx_mm-b.tx_mm,a.ty_mm-b.ty_mm,a.tz_percent-b.tz_percent};
}

// The placement that the active revision's changes are counted from: its base pose, or the profile's own (pose 0) when a hand adjustment
// put the base pose beyond the envelope (pose_outside_envelope, the revision is intact). A raster measured at a pose that far out may leave
// camera B no row to cover; and the revision that this makes has base pose 0 whatever the parent's was.
bool BasePoseOutsideEnvelope(const FixedCompositeConditionV3& active){
    try{RequireFixedCompositeBasePoseInEnvelopeV3(active);}
    catch(const FixedCompositePoseOutsideEnvelope&){return true;}
    return false;
}
PoseParameters ReferencePose(const FixedCompositeConditionV3& active){return BasePoseOutsideEnvelope(active)?PoseParameters{}:active.base_pose;}

void ValidateInputs(const Inputs& in,bool images){
    using namespace fixed_composite_detail;
    if(!in.hash)Reject("no SHA-256 provider");
    if(!IsPairId(in.pair_id))Reject("the pair identity is not a pair identity");
    if(!IsUtcTimestamp(in.measured_at_utc))Reject("the time of the shot is not a UTC time");
    if(!IsLowerHex64(in.active_sha256) || !IsLowerHex64(in.original_a_sha256) || !IsLowerHex64(in.original_b_sha256))Reject("a SHA-256 is not 64 lower-case hex digits");
    if(in.thread_count<1 || in.thread_count>256)Reject("the thread count is outside 1..256");
    // The time goes into the revision as its creation time: the clock's epoch would be written as a real time. Layer 2's constants are needed
    // by the creation check when the condition enables layer 2; without them the check would refuse for a reason that is the caller's.
    if(in.now==std::chrono::system_clock::time_point{})Reject("the time of the new revision is not given");
    if(in.active.registration.parallax && !in.layer_two)Reject("the condition enables layer two and the design constants of its strain bound are not given");
    if(images){
        if(!in.camera_a || !in.camera_b)Reject("both originals are required");
        for(const auto* image:{in.camera_a,in.camera_b})
            if(image->width==0 || image->height==0 || image->bgr.size()!=render::PixelCount(image->width,image->height)*3)Reject("an original is not a complete BGR image");
        if(in.camera_a->width!=in.camera_b->width || in.camera_a->height!=in.camera_b->height)Reject("the two originals differ in size");
    }
}
bool Stopped(const Inputs& in){return in.control.stop && in.control.stop();}
void Announce(const Inputs& in,Stage stage){if(in.progress)in.progress(stage);}

void Refuse(Proposal& p,Refusal reason,const std::string& detail){
    if(std::find(p.refusals.begin(),p.refusals.end(),reason)==p.refusals.end())p.refusals.push_back(reason);
    if(!detail.empty())p.detail+=(p.detail.empty()?"":"; ")+detail;
}

render::OutputRaster RasterWithWidth(const render::OutputRaster& old,std::int64_t width){
    if(width<1)throw RecalibrationRefused(Refusal::no_overlap,"the new raster would have no width");
    if(width>static_cast<std::int64_t>(render::kMaximumImageDimension))
        throw RecalibrationRefused(Refusal::raster_too_large,"the new raster is wider than the render limit");
    // The largest extent (micrometres) whose ceiling at this DPI is `width` pixels.
    const std::int64_t extent=width*25400/static_cast<std::int64_t>(old.dpi);
    render::OutputRaster out(render::DocumentRegionUm{old.region_um.left,old.region_um.top,old.region_um.left+extent,old.region_um.bottom},old.dpi,
        static_cast<std::uint32_t>(width),old.height_pixels);
    try{render::ValidateOutputRaster(out);}
    catch(const std::invalid_argument& e){throw RecalibrationRefused(Refusal::raster_too_large,e.what());}
    return out;
}
}

std::string_view RefusalCode(Refusal refusal){
    switch(refusal){
    case Refusal::no_overlap:return "no-overlap";
    case Refusal::estimate_failed:return "estimate-failed";
    case Refusal::estimate_not_verified:return "estimate-not-verified";
    case Refusal::tilt_exceeded:return "tilt-exceeded";
    case Refusal::height_exceeded:return "height-exceeded";
    case Refusal::profile_invalid:return "profile-invalid";
    case Refusal::raster_too_large:return "raster-too-large";
    case Refusal::raster_outside_sheet:return "raster-outside-sheet";
    case Refusal::creation_check_failed:return "creation-check-failed";
    case Refusal::reestimate_not_verified:return "reestimate-not-verified";
    case Refusal::reestimate_displacement:return "reestimate-displacement";
    case Refusal::stopped:return "stopped";
    }
    return {};
}
std::string_view StageCode(Stage stage){
    switch(stage){
    case Stage::wide_search:return "wide-search";
    case Stage::fine_fit:return "fine-fit";
    case Stage::bake:return "bake";
    case Stage::check:return "check";
    case Stage::verify:return "verify";
    }
    return {};
}

// ===== R1 =====================================================================================================================
namespace {
render::BgrImage HalfTurn(const render::BgrImage& image){
    render::BgrImage out{image.width,image.height,std::vector<std::uint8_t>(image.bgr.size())};
    const std::size_t pixels=static_cast<std::size_t>(image.width)*image.height;
    for(std::size_t i=0;i<pixels;++i)
        for(std::size_t c=0;c<3;++c)out.bgr[i*3+c]=image.bgr[(pixels-1-i)*3+c];
    return out;
}

// Gauss-Newton with damping on (tx, ty, rz): the rigid shift of the document plane that carries A's document points onto B's.
struct DocumentPair {Point a,b;};
bool Residuals(const PoseModel& model,const std::vector<DocumentPair>& pairs,const PoseParameters& pose,std::vector<double>& r){
    try{
        const auto g=model.Homography(pose);
        r.clear();
        for(const auto& c:pairs){
            Point y{};
            if(!render::TryTransform(g,c.a,y))return false;
            r.push_back(y.x-c.b.x);r.push_back(y.y-c.b.y);
        }
    }catch(const std::invalid_argument&){return false;}
    return true;
}
double Cost(const std::vector<double>& r){double s=0;for(const double v:r)s+=v*v;return s;}
bool Solve3(std::array<std::array<double,4>,3> m,std::array<double,3>& x){
    for(int col=0;col<3;++col){
        int pivot=col;
        for(int row=col+1;row<3;++row)if(std::abs(m[row][col])>std::abs(m[pivot][col]))pivot=row;
        if(!(std::abs(m[pivot][col])>1e-300))return false;
        std::swap(m[col],m[pivot]);
        for(int row=0;row<3;++row){
            if(row==col)continue;
            const double f=m[row][col]/m[col][col];
            for(int k=col;k<4;++k)m[row][k]-=f*m[col][k];
        }
    }
    for(int i=0;i<3;++i)x[i]=m[i][3]/m[i][i];
    return true;
}
bool FitRigidShift(const PoseModel& model,const std::vector<DocumentPair>& pairs,PoseParameters& pose){
    pose=PoseParameters{};
    std::vector<double> r,rp,rm;
    if(!Residuals(model,pairs,pose,r))return false;
    double cost=Cost(r),damping=1e-6;
    constexpr double kStep[3]{0.01,0.01,0.001};   // mm, mm, degrees: finite differences of the Jacobian
    for(int iteration=0;iteration<40;++iteration){
        std::vector<std::array<double,3>> jacobian(r.size());
        for(int k=0;k<3;++k){
            auto up=pose,down=pose;
            (k==0?up.tx_mm:k==1?up.ty_mm:up.rz_deg)+=kStep[k];
            (k==0?down.tx_mm:k==1?down.ty_mm:down.rz_deg)-=kStep[k];
            if(!Residuals(model,pairs,up,rp) || !Residuals(model,pairs,down,rm))return false;
            for(std::size_t i=0;i<r.size();++i)jacobian[i][static_cast<std::size_t>(k)]=(rp[i]-rm[i])/(2*kStep[k]);
        }
        std::array<std::array<double,4>,3> normal{};
        for(std::size_t i=0;i<r.size();++i)
            for(int a=0;a<3;++a){
                for(int b=0;b<3;++b)normal[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]+=jacobian[i][static_cast<std::size_t>(a)]*jacobian[i][static_cast<std::size_t>(b)];
                normal[static_cast<std::size_t>(a)][3]-=jacobian[i][static_cast<std::size_t>(a)]*r[i];
            }
        bool improved=false;
        for(int attempt=0;attempt<12 && !improved;++attempt){
            auto system=normal;
            for(int a=0;a<3;++a)system[static_cast<std::size_t>(a)][static_cast<std::size_t>(a)]+=damping*(1+normal[static_cast<std::size_t>(a)][static_cast<std::size_t>(a)]);
            std::array<double,3> delta{};
            if(!Solve3(system,delta)){damping*=10;continue;}
            auto trial=pose;trial.tx_mm+=delta[0];trial.ty_mm+=delta[1];trial.rz_deg+=delta[2];
            std::vector<double> tr;
            if(Residuals(model,pairs,trial,tr) && Cost(tr)<=cost){
                const double step=std::max({std::abs(delta[0]),std::abs(delta[1]),std::abs(delta[2])});
                pose=trial;r=std::move(tr);cost=Cost(r);damping=std::max(damping/10,1e-12);improved=true;
                if(step<1e-10)return true;
            }else damping*=10;
        }
        if(!improved)return true;   // a minimum within the arithmetic
    }
    return true;
}

WideStart WideStartOn(const Inputs& in,const render::DocumentRenderParameters& parent){
    WideStart out;
    const auto& a=*in.camera_a;const auto& b=*in.camera_b;
    const bool turned=in.active.orientation==MountOrientation::rotated_180;
    // The overlap estimator takes oriented frames (A left, B right). A rig that is mounted a half turn stores both pictures
    // turned; they are turned back here and the matched points are carried back to the stored order below.
    std::optional<render::BgrImage> turned_a,turned_b;
    if(turned){turned_a=HalfTurn(a);turned_b=HalfTurn(b);}
    const render::BgrImage& oriented_a=turned?*turned_a:a;
    const render::BgrImage& oriented_b=turned?*turned_b:b;

    // The prior: where the calibrated mappings put the middle of the overlap, as a translation of B to A.
    int prior_x=0,prior_y=0;
    {
        const auto input=PoseCorrectedDocumentMappedFixedSeamInput(parent,PoseParameters{});
        const auto rows=MappedFixedSeamBand(a,b,input,in.thread_count);
        const std::size_t height=rows.size();
        std::optional<std::size_t> at;
        for(std::size_t d=0;d<height && !at;++d)
            for(const long long sign:{1LL,-1LL}){
                const long long y=static_cast<long long>(height/2)+sign*static_cast<long long>(d);
                if(y>=0 && y<static_cast<long long>(height) && rows[static_cast<std::size_t>(y)].first<rows[static_cast<std::size_t>(y)].end){at=static_cast<std::size_t>(y);break;}
            }
        if(!at){out.reason="prior-rejected";return out;}
        const auto& row=rows[*at];
        const double x=(static_cast<double>(row.first)+static_cast<double>(row.end)-1)/2,y=static_cast<double>(*at);
        Point raw_a{},raw_b{};
        if(!input.camera_a({x,y},raw_a) || !input.camera_b({x,y},raw_b)){out.reason="prior-rejected";return out;}
        double dx=raw_a.x-raw_b.x,dy=raw_a.y-raw_b.y;
        if(turned){dx=-dx;dy=-dy;}
        prior_x=static_cast<int>(std::lround(dx));prior_y=static_cast<int>(std::lround(dy));
    }
    OverlapSimilarityEstimate estimate;
    try{estimate=EstimateOverlapSimilarity(oriented_a,oriented_b,prior_x,prior_y);}
    catch(const std::invalid_argument&){out.reason="prior-rejected";return out;}
    out.tiles_agreed=estimate.agreed;
    if(estimate.status!=OverlapEstimateStatus::estimated_not_judged){out.reason=std::string(SimilarityUndeterminedReasonCode(estimate.reason));return out;}

    std::vector<DocumentPair> pairs;
    const double w=static_cast<double>(a.width),h=static_cast<double>(a.height);
    for(const auto& tile:estimate.tiles){
        if(!tile.agreed)continue;
        // Pixel-corner coordinates of the oriented frame to the stored order's sample index.
        Point ia{tile.a_x-0.5,tile.a_y-0.5},ib{tile.b_x-0.5,tile.b_y-0.5};
        if(turned){ia={w-1-ia.x,h-1-ia.y};ib={w-1-ib.x,h-1-ib.y};}
        Point da{},db{};
        if(TryDocumentPointOfRawPixel(parent.camera_a,ia,da) && TryDocumentPointOfRawPixel(parent.camera_b,ib,db))pairs.push_back({da,db});
    }
    out.correspondences=static_cast<unsigned>(pairs.size());
    // Three unknowns, two equations a point: fewer than two points cannot determine them.
    if(pairs.size()<2){out.reason="no-correspondence";return out;}
    const PoseModel model(parent.camera_b);
    PoseParameters pose{};
    if(!FitRigidShift(model,pairs,pose)){out.reason="fit-failed";return out;}
    std::vector<double> r;
    if(!Residuals(model,pairs,pose,r)){out.reason="fit-failed";return out;}
    double sum=0,maximum=0;
    for(std::size_t i=0;i+1<r.size();i+=2){const double d=std::hypot(r[i],r[i+1]);sum+=d*d;maximum=std::max(maximum,d);}
    out.pose=pose;out.residual_rms_mm=std::sqrt(sum/static_cast<double>(pairs.size()));out.residual_max_mm=maximum;
    out.determined=true;
    return out;
}
}

WideStart EstimateWideStart(const Inputs& in){
    ValidateInputs(in,true);
    const auto parent=BindFixedCompositeProfileV3(in.active,in.profile_bytes,in.hash);
    return WideStartOn(in,parent.parameters);
}

// ===== R3 =====================================================================================================================
std::string ReanchoredProfileText(std::string_view parent_profile_bytes,const PoseParameters& p_hat,const Provenance& provenance){
    using namespace fixed_composite_detail;
    for(const double v:{p_hat.rx_deg,p_hat.ry_deg,p_hat.rz_deg,p_hat.tx_mm,p_hat.ty_mm,p_hat.tz_percent})
        if(!std::isfinite(v))Reject("the pose is not finite");
    if(!IsUtcTimestamp(provenance.measured_at_utc) || !IsLowerHex64(provenance.input_sha256_camera_b))Reject("the provenance has a time or a hash that is malformed");
    if(!std::isfinite(provenance.fit.rms_px) || provenance.fit.rms_px<0 || !std::isfinite(provenance.fit.max_px) || provenance.fit.max_px<0)Reject("the fit residual is not a measure");
    {
        const auto& id=provenance.provenance_id;
        const auto alnum=[](char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');};
        if(id.empty() || id.size()>128 || !alnum(id.front()) || !std::all_of(id.begin(),id.end(),[&](char c){return alnum(c)||c=='.'||c=='_'||c=='-';}))
            Reject("the provenance identifier is not an identifier");
    }
    auto tree=ParseProfileTree(parent_profile_bytes);
    if(StringOf(Member(tree,"status"))!="draft")Reject("only a draft profile is re-anchored");
    auto& calibration=Member(tree,"calibration");
    if(calibration.kind!=JsonKind::object)Reject("the parent profile is not calibrated");
    auto& projection_b=Member(Member(Member(tree,"cameras"),"CAM-B"),"projection");
    if(projection_b.kind!=JsonKind::object)Reject("the parent profile has no camera B");
    const PoseModel model(ProjectionOf(projection_b));
    Member(projection_b,"documentToImage")=MatrixValue(model.CameraHomography(p_hat));

    Member(tree,"profileId")=NullValue();
    Member(Member(calibration,"tool"),"toolId")=StringValue(std::string(kToolId));
    Member(Member(calibration,"tool"),"version")=StringValue(std::string(kToolVersion));
    Member(calibration,"provenanceId")=StringValue(provenance.provenance_id);
    Member(calibration,"measuredAt")=StringValue(provenance.measured_at_utc);
    Member(Member(calibration,"inputSha256"),"CAM-B")=StringValue(provenance.input_sha256_camera_b);
    {
        JsonValue fit;fit.kind=JsonKind::object;
        fit.object["rms"]=NumberValue(provenance.fit.rms_px);fit.object["max"]=NumberValue(provenance.fit.max_px);
        Member(Member(calibration,"fitResidualPixels"),"CAM-B")=std::move(fit);
    }
    auto& rig=Member(calibration,"rigMeasurements");
    Member(rig,"baselineMm")=NullValue();Member(rig,"overlapMm")=NullValue();
    Member(Member(rig,"cameraToDocumentMm"),"CAM-B")=NullValue();
    auto text=Dump(tree);
    if(text.size()>kMaximumFixedCompositeProfileBytes)Reject("the new profile is too large");
    // The readers take it: a calibrated draft.
    try{
        const auto parsed=RigProfileV2::Parse(text);
        if(parsed.IsApproved() || !parsed.IsCalibrated())Reject("the new profile is not a calibrated draft");
    }catch(const std::invalid_argument& e){
        if(std::string_view(e.what()).starts_with(kPrefix))throw;
        Reject(std::string("the new profile does not pass the rig profile reader: ")+e.what());
    }
    return text;
}

ProfileFit FitInCameraPixels(const content_registration::RegistrationResult& estimate,const render::OutputRaster& raster,const render::CameraProjection& camera_b_after){
    ProfileFit fit;
    const auto map=render::DocumentOutputToCamera(raster,camera_b_after);
    double sum=0;
    for(const auto& o:estimate.diagnostics.observations){
        if(!(o.weight>0) || !std::isfinite(o.residual_px))continue;
        Point right{},left{},down{},up{};
        try{
            if(!map({o.x+0.5,o.y},right) || !map({o.x-0.5,o.y},left) || !map({o.x,o.y+0.5},down) || !map({o.x,o.y-0.5},up))continue;
        }catch(const std::invalid_argument&){continue;}
        // The columns of the Jacobian of the output-to-camera mapping (camera pixels per output pixel).
        const double jxx=right.x-left.x,jyx=right.y-left.y,jxy=down.x-up.x,jyy=down.y-up.y;
        double scale;
        const double n=std::hypot(o.normal.x,o.normal.y);
        if(o.one_directional && n>0){
            const double nx=o.normal.x/n,ny=o.normal.y/n;
            scale=std::hypot(jxx*nx+jxy*ny,jyx*nx+jyy*ny);
        }else scale=std::sqrt(std::abs(jxx*jyy-jxy*jyx));
        const double value=o.residual_px*scale;
        if(!std::isfinite(value))continue;
        sum+=value*value;fit.max_px=std::max(fit.max_px,value);++fit.points;
    }
    if(fit.points)fit.rms_px=std::sqrt(sum/fit.points);
    return fit;
}

// ===== The raster ============================================================================================================
render::OutputRaster ProposeRaster(const FixedCompositeConditionV3& active,const FixedCompositeProfileBinding& parent,const PoseParameters& p_hat,RasterPlacement placement){
    const auto& old=active.raster;
    if(placement==RasterPlacement::keep)return old;
    const PoseModel model_b(parent.parameters.camera_b);
    const auto map_old=PoseCorrectedDocumentOutputToCamera(old,parent.parameters.camera_b,model_b,ReferencePose(active));
    const auto map_new=PoseCorrectedDocumentOutputToCamera(old,parent.parameters.camera_b,model_b,p_hat);
    // The right edge of B's cover on each row; the smallest over the rows is what the raster's right edge follows.
    double smallest_old=std::numeric_limits<double>::infinity(),smallest_new=smallest_old;
    for(std::uint32_t y=0;y<old.height_pixels;++y){
        if(const auto run=RunOfRow(map_old,static_cast<double>(y)))smallest_old=std::min(smallest_old,run->right);
        if(const auto run=RunOfRow(map_new,static_cast<double>(y)))smallest_new=std::min(smallest_new,run->right);
    }
    if(!std::isfinite(smallest_old))Reject("camera B covers no row of the active raster at the active base pose");
    if(!std::isfinite(smallest_new))throw RecalibrationRefused(Refusal::no_overlap,"camera B covers no row of the raster at the estimated pose");
    const auto delta=static_cast<std::int64_t>(std::llround(smallest_new-smallest_old));
    if(delta==0)return old;
    return RasterWithWidth(old,static_cast<std::int64_t>(old.width_pixels)+delta);
}

// ===== The estimate's record =================================================================================================
namespace {
std::string EstimateRecord(const Inputs& in,const ParentFacts& facts,const WideStart& wide,const content_registration::RegistrationResult& estimate,
    const PoseParameters& p_hat,const ProfileFit& fit,const std::string& profile_fingerprint){
    const auto& c=in.active;const auto& d=estimate.diagnostics;
    const auto& env=c.registration.envelope;
    // The prior that pulls the directions the band cannot see towards the base pose: factor x the envelope of each component.
    const double f=in.design.prior_sigma_factor;
    const PoseParameters prior{f*env.tilt_deg,f*env.tilt_deg,f*env.rotation_deg,f*env.position_mm,f*env.position_mm,f*env.height_percent};
    std::string s="{\"schema\":"+Quoted(std::string(kEstimateRecordSchema))+",\"pairId\":"+Quoted(in.pair_id)+",\"measuredAt\":"+Quoted(in.measured_at_utc)+
        ",\"inputs\":{\"cameraASha256\":"+Quoted(in.original_a_sha256)+",\"cameraBSha256\":"+Quoted(in.original_b_sha256)+"}"
        ",\"parent\":{\"revisionSequence\":"+std::to_string(c.revision.sequence)+",\"revisionSha256\":"+Quoted(in.active_sha256)+
        ",\"profileFileSha256\":"+Quoted(c.source.profile_file_sha256)+",\"profileFingerprintSha256\":"+Quoted(profile_fingerprint)+
        ",\"profileProvenanceId\":"+Quoted(facts.provenance_id)+",\"profileToolId\":"+Quoted(facts.tool_id)+
        ",\"reanchoredBefore\":"+(facts.tool_id==kToolId?"true":"false")+"}"
        ",\"mountOrientation\":"+Quoted(MountOrientationName(c.orientation))+
        ",\"wideSearch\":{\"tilesAgreed\":"+std::to_string(wide.tiles_agreed)+",\"correspondences\":"+std::to_string(wide.correspondences)+
        ",\"txMm\":"+Num(wide.pose.tx_mm)+",\"tyMm\":"+Num(wide.pose.ty_mm)+",\"rzDeg\":"+Num(wide.pose.rz_deg)+
        ",\"residualRmsMm\":"+Num(wide.residual_rms_mm)+",\"residualMaxMm\":"+Num(wide.residual_max_mm)+"}"
        ",\"estimate\":{\"state\":"+Quoted(std::string(content_registration::RegistrationStateCode(estimate.state)))+",\"reasons\":"+StringsJson(estimate.reasons)+
        ",\"pose\":"+PoseJson(p_hat)+",\"poseSigma\":"+PoseJson(estimate.pose_sigma)+",\"priorSigma\":"+PoseJson(prior)+
        ",\"informationConditionNumber\":"+Num(d.information_condition_number)+
        ",\"points\":"+std::to_string(d.points)+",\"inliers\":"+std::to_string(d.inliers)+",\"outerIterations\":"+std::to_string(d.outer_iterations)+
        ",\"uniqueBlocks\":"+std::to_string(d.unique_blocks)+",\"anchorBlocks\":"+std::to_string(d.anchor_blocks)+
        ",\"residualRmsPx\":"+Num(d.residual.rms)+",\"residualP95Px\":"+Num(d.residual.p95)+
        ",\"secondStart\":"+Quoted(std::string(content_registration::SecondStartCode(d.second_start)))+
        ",\"cameraBResidualPx\":{\"rms\":"+Num(fit.rms_px)+",\"max\":"+Num(fit.max_px)+",\"points\":"+std::to_string(fit.points)+"}}}";
    return s;
}

// ===== The candidate's geometry =============================================================================================
struct Plan {
    std::string profile_bytes,profile_name;
    render::OutputRaster raster;
    std::uint32_t seam_column{};
    FixedSeamOrigin seam_origin{FixedSeamOrigin::band_center_provisional};
    render::DocumentRenderParameters parameters;
};

void FillMotion(Display& display,const Inputs& in,const FixedCompositeProfileBinding& parent,const PoseParameters& p_hat){
    const auto& active=in.active;
    const auto reference=ReferencePose(active);
    display.previous_outside_envelope=BasePoseOutsideEnvelope(active);
    display.motion=content_registration::OutputFrameOf(parent.parameters.camera_a,parent.parameters.camera_b,active.raster,active.seam.column,reference,p_hat,
        content_registration::SurfacePlane{});
    display.pose=content_registration::DisplayOf(Minus(p_hat,reference),PoseModel(parent.parameters.camera_b));
    display.pose.output_frame=display.motion;
    display.has_motion=true;
}
void FillPrevious(Display& display,const FixedCompositeConditionV3& active){
    const auto& r=active.raster;
    const double pitch=25.4/r.dpi,left=static_cast<double>(r.region_um.left)/1000.0;
    display.seam_column_previous=active.seam.column;
    display.seam_x_previous_mm=left+(static_cast<double>(active.seam.column)+0.5)*pitch;
    display.width_previous_px=r.width_pixels;display.height_previous_px=r.height_pixels;
    display.right_previous_mm=static_cast<double>(r.region_um.right)/1000.0;
}
void FillNew(Display& display,const Plan& plan){
    const auto& r=plan.raster;
    const double pitch=25.4/r.dpi,left=static_cast<double>(r.region_um.left)/1000.0;
    display.seam_column_new=plan.seam_column;
    display.seam_x_new_mm=left+(static_cast<double>(plan.seam_column)+0.5)*pitch;
    display.width_new_px=r.width_pixels;display.height_new_px=r.height_pixels;
    display.right_new_mm=static_cast<double>(r.region_um.right)/1000.0;
    display.has_new=true;
    // Overlap and the seam's distance to the photographs' edges, row by row, with the new mapping and the same coverage rule as the check.
    const auto map_a=render::DocumentOutputToCamera(r,plan.parameters.camera_a);
    const auto map_b=render::DocumentOutputToCamera(r,plan.parameters.camera_b);
    const double right_limit=static_cast<double>(r.width_pixels)-1.0;
    double narrowest=std::numeric_limits<double>::infinity();
    double distance_a=std::numeric_limits<double>::infinity(),distance_b=distance_a;
    for(std::uint32_t y=0;y<r.height_pixels;++y){
        const double row=static_cast<double>(y);
        const auto run_a=RunOfRow(map_a,row),run_b=RunOfRow(map_b,row);
        double width=0;
        if(run_a && run_b){
            const double left_edge=std::max(run_b->left,0.0),right_edge=std::min(run_a->right,right_limit);
            width=std::max(0.0,right_edge-left_edge);
        }
        if(width<narrowest){narrowest=width;display.narrowest_overlap_row=y;}
        Point raw_a{},raw_b{},next_a{},next_b{};
        const Point seam{static_cast<double>(plan.seam_column),row};
        // The stored pixel order is the profile's, so which side of a photograph faces the drawing's right depends on the mounting: one
        // output pixel further along the row says which way the stored x runs (a half-turn mounting runs it backwards). A's edge that
        // matters is the one on the drawing's right, B's the one on the drawing's left.
        const double step=seam.x+1.0<=right_limit?1.0:-1.0;
        const Point probe{seam.x+step,row};
        bool ok=false;
        try{ok=map_a(seam,raw_a) && map_b(seam,raw_b) && map_a(probe,next_a) && map_b(probe,next_b);}catch(const std::invalid_argument&){}
        if(ok && Covered(map_a,seam.x,seam.y) && Covered(map_b,seam.x,seam.y)){
            const double run_a_x=(next_a.x-raw_a.x)*step,run_b_x=(next_b.x-raw_b.x)*step;
            const double last=static_cast<double>(kD810SensorWidthPixels)-1.0;
            if(run_a_x!=0 && run_b_x!=0){
                distance_a=std::min(distance_a,run_a_x>0?last-raw_a.x:raw_a.x);
                distance_b=std::min(distance_b,run_b_x>0?raw_b.x:last-raw_b.x);
            }
        }
    }
    if(std::isfinite(narrowest)){display.narrowest_overlap_mm=narrowest*pitch;display.has_overlap=true;}
    if(std::isfinite(distance_a) && std::isfinite(distance_b)){
        display.edge_distance_a_px=distance_a;display.edge_distance_b_px=distance_b;
        display.edge_share_a=distance_a/static_cast<double>(kD810SensorWidthPixels);display.edge_share_b=distance_b/static_cast<double>(kD810SensorWidthPixels);
        display.has_edge_distance=true;
    }
}

// The pieces that make the candidate; throws RecalibrationRefused.
Plan MakePlan(const Inputs& in,const FixedCompositeProfileBinding& parent,const ParentFacts& facts,const PoseParameters& p_hat,const Provenance& provenance,
    Display& display){
    const auto& active=in.active;
    const auto raster=ProposeRaster(active,parent,p_hat,in.raster);
    const double right_mm=static_cast<double>(raster.region_um.right)/1000.0,bottom_mm=static_cast<double>(raster.region_um.bottom)/1000.0;
    display.width_new_px=raster.width_pixels;display.right_new_mm=right_mm;
    if(right_mm>facts.sheet_width_mm || bottom_mm>facts.sheet_height_mm)
        throw RecalibrationRefused(Refusal::raster_outside_sheet,"the new raster goes beyond the sheet of the profile");
    Announce(in,Stage::bake);
    std::string profile;
    try{profile=ReanchoredProfileText(in.profile_bytes,p_hat,provenance);}
    catch(const std::invalid_argument& e){throw RecalibrationRefused(Refusal::profile_invalid,e.what());}
    std::optional<render::DocumentRenderParameters> parameters;
    try{parameters=RigProfileV2::Parse(profile).ValidateCalibrationForEvaluation(raster,render::Resampling::bilinear);}
    catch(const std::invalid_argument& e){throw RecalibrationRefused(Refusal::profile_invalid,std::string("the new profile cannot be used over the new raster: ")+e.what());}
    std::uint32_t column;FixedSeamOrigin origin;
    if(in.seam==SeamPlacement::keep_previous){
        column=active.seam.column;origin=FixedSeamOrigin::operator_set;
        if(column>=raster.width_pixels)throw RecalibrationRefused(Refusal::creation_check_failed,"the previous seam column is outside the new raster");
    }else{
        try{column=FixedCompositeProvisionalSeamColumn(*parameters);}
        catch(const std::invalid_argument& e){throw RecalibrationRefused(Refusal::no_overlap,std::string("the new profile has no overlap band: ")+e.what());}
        origin=FixedSeamOrigin::band_center_provisional;
    }
    return Plan{profile,FixedCompositeProfileFileName(in.hash(Bytes(profile))),raster,column,origin,*parameters};
}

Provenance ProvenanceOf(const Inputs& in,const std::string& record_sha256,const ProfileFit& fit){
    return Provenance{"pair-reanchor-"+record_sha256.substr(0,16),in.measured_at_utc,in.original_b_sha256,fit};
}

void BuildFromPose(const Inputs& in,Proposal& out,const PoseParameters& p_hat,const ProfileFit& fit,const std::string& record_text){
    const auto parent=BindFixedCompositeProfileV3(in.active,in.profile_bytes,in.hash);
    const auto facts=FactsOf(in.profile_bytes);
    out.has_pose=true;out.pose=p_hat;out.fit=fit;
    out.estimate_record_text=record_text;out.estimate_record_sha256=in.hash(Bytes(record_text));
    FillPrevious(out.display,in.active);
    FillMotion(out.display,in,parent,p_hat);
    std::optional<Plan> plan;
    try{plan=MakePlan(in,parent,facts,p_hat,ProvenanceOf(in,out.estimate_record_sha256,fit),out.display);}
    catch(const RecalibrationRefused& e){Refuse(out,e.GetReason(),e.what());return;}
    catch(const std::invalid_argument& e){Refuse(out,Refusal::profile_invalid,e.what());return;}
    FillNew(out.display,*plan);

    Announce(in,Stage::check);
    FixedCompositeV3Request request;
    request.profile_file=plan->profile_name;request.profile_bytes=plan->profile_bytes;request.policy_sha256=in.active.source.policy_sha256;
    request.sequence=in.active.revision.sequence+1;request.parent_sha256=in.active_sha256;
    request.change.kind=FixedCompositeRevisionKind::recalibrated;request.change.report_sha256=out.estimate_record_sha256;request.change.pair_id=in.pair_id;
    request.orientation=in.active.orientation;request.raster=plan->raster;
    request.seam_column=plan->seam_column;request.seam_origin=plan->seam_origin;
    request.registration=in.active.registration;request.seam_gate=in.active.seam_gate;request.revision_rules=in.active.revision_rules;
    CreationCheckDetailsV3 details;
    std::optional<FixedCompositeConditionV3> condition;
    try{
        condition=MakeFixedCompositeConditionV3(request,in.now,in.hash,MakeFixedCompositeCreationCheckV3(&details,CreationCheckOptionsV3{in.layer_two}));
    }catch(const std::invalid_argument& e){
        std::string what=e.what();
        // A profile or a record that the readers refuse is not the check's failure.
        Refuse(out,what.find("creation check")!=std::string::npos || what.find("fixed composite creation v3")!=std::string::npos?Refusal::creation_check_failed:Refusal::profile_invalid,what);
        return;
    }
    std::string text=FormatFixedCompositeConditionV3(*condition)+"\n";
    std::string sha256=in.hash(Bytes(text));
    try{
        auto binding=BindFixedCompositeProfileV3(*condition,plan->profile_bytes,in.hash);
        out.candidate=Candidate{plan->profile_bytes,plan->profile_name,*condition,std::move(text),std::move(sha256),std::move(binding),std::move(details)};
    }catch(const std::invalid_argument& e){Refuse(out,Refusal::profile_invalid,e.what());}
}
}

Proposal ProposeFromPose(const Inputs& in,const PoseParameters& p_hat,const ProfileFit& fit,std::string estimate_record_text){
    ValidateInputs(in,false);
    Proposal out;
    BuildFromPose(in,out,p_hat,fit,estimate_record_text);
    return out;
}

// ===== The verification =====================================================================================================
Verification VerifyCandidate(const Inputs& in,const Candidate& candidate){
    ValidateInputs(in,true);
    Verification v;
    v.performed=true;
    const auto& c=candidate.condition;
    v.limit_px=c.registration.limits.convergence_px;
    content_registration::RegistrationSettings settings;
    settings.geometry={c.seam.column,kSeamBlendDesign.width_pixels,c.registration.geometry.omega_margin_px,c.registration.geometry.guard_px,c.registration.geometry.band_half_width_px};
    settings.envelope=c.registration.envelope;settings.limits=c.registration.limits;settings.design=in.design;
    settings.surface_envelope=c.registration.surface_envelope;settings.base_pose=c.base_pose;
    const auto& parameters=candidate.binding.parameters;
    content_registration::RegistrationResult r;
    try{
        content_registration::ValidateRegistrationSettings(settings);
        r=content_registration::EstimateRegistration(*in.camera_a,*in.camera_b,parameters,settings,in.thread_count,in.control);
    }catch(const std::invalid_argument&){
        v.state=content_registration::RegistrationState::undetermined;v.reasons={"estimate-failed"};
        return v;
    }
    v.state=r.state;v.reasons=r.reasons;
    v.inliers=r.diagnostics.inliers;v.points=r.diagnostics.points;
    v.evidence=content_registration::EvidenceCountsOf(r.diagnostics);
    if(!r.has_estimate)return v;
    v.pose=r.pose;
    if(r.surface.state==SurfaceState::estimated)v.surface=r.surface.plane;
    try{
        const PoseModel model_b(parameters.camera_b);
        const auto centre_a=PoseModel(parameters.camera_a).Centre(PoseParameters{});
        const auto origin=content_registration::SurfaceOriginMm(parameters.raster,c.seam.column);
        const auto frame=content_registration::FrameOfRaster(parameters.raster);
        for(std::uint32_t y=0;y<parameters.raster.height_pixels;++y){
            const auto total=content_registration::RegisteredDisplacement(model_b,frame,centre_a,origin,r.pose,v.surface,static_cast<double>(c.seam.column),static_cast<double>(y));
            const auto pose_only=content_registration::RegisteredDisplacement(model_b,frame,centre_a,origin,r.pose,content_registration::SurfacePlane{},
                static_cast<double>(c.seam.column),static_cast<double>(y));
            v.seam_displacement_px=std::max(v.seam_displacement_px,std::hypot(total.x,total.y));
            v.seam_displacement_pose_only_px=std::max(v.seam_displacement_pose_only_px,std::hypot(pose_only.x,pose_only.y));
        }
    }catch(const std::invalid_argument&){
        v.reasons.push_back("displacement-failed");
        v.seam_displacement_px=std::numeric_limits<double>::infinity();
        return v;
    }
    v.passed=r.state==content_registration::RegistrationState::verified && v.seam_displacement_px<=v.limit_px;
    return v;
}

// ===== The whole =============================================================================================================
Proposal ProposeRecalibration(const Inputs& in){
    ValidateInputs(in,true);
    Proposal out;
    const auto parent=BindFixedCompositeProfileV3(in.active,in.profile_bytes,in.hash);
    const auto facts=FactsOf(in.profile_bytes);
    const auto joined=[](const std::vector<std::string>& v){std::string s;for(const auto& x:v)s+=(s.empty()?"":",")+x;return s;};

    Announce(in,Stage::wide_search);
    out.wide=WideStartOn(in,parent.parameters);
    if(Stopped(in)){Refuse(out,Refusal::stopped,"stopped after the wide search");return out;}
    if(!out.wide.determined){Refuse(out,Refusal::no_overlap,"wide search: "+out.wide.reason);return out;}

    // The geometry of the fine stage: the new raster and the seam at the start pose.
    render::OutputRaster raster_fine=in.active.raster;
    try{raster_fine=ProposeRaster(in.active,parent,out.wide.pose,in.raster);}
    catch(const RecalibrationRefused& e){Refuse(out,e.GetReason(),e.what());return out;}
    catch(const std::invalid_argument& e){Refuse(out,Refusal::profile_invalid,e.what());return out;}
    render::DocumentRenderParameters parameters_fine(raster_fine,parent.parameters.camera_a,parent.parameters.camera_b,parent.parameters.layout,parent.parameters.resampling);
    std::uint32_t seam_fine;
    if(in.seam==SeamPlacement::keep_previous){
        seam_fine=in.active.seam.column;
        if(seam_fine>=raster_fine.width_pixels){Refuse(out,Refusal::creation_check_failed,"the previous seam column is outside the raster");return out;}
    }else{
        try{
            const auto input=PoseCorrectedDocumentMappedFixedSeamInput(parameters_fine,out.wide.pose);
            seam_fine=FixedCompositeBandCenter(FixedCompositeBandOfRows(MappedFixedSeamBand(*in.camera_a,*in.camera_b,input,in.thread_count)));
        }catch(const std::invalid_argument& e){Refuse(out,Refusal::no_overlap,std::string("no overlap band at the start pose: ")+e.what());return out;}
    }

    Announce(in,Stage::fine_fit);
    content_registration::RegistrationSettings settings;
    const auto& g=in.active.registration.geometry;
    settings.geometry={seam_fine,kSeamBlendDesign.width_pixels,g.omega_margin_px,g.guard_px,g.band_half_width_px};
    settings.envelope=in.active.registration.envelope;settings.limits=in.active.registration.limits;settings.design=in.design;
    settings.surface_envelope=std::nullopt;   // six degrees: the paper is taken to lie on the table
    settings.base_pose=out.wide.pose;
    try{
        content_registration::ValidateRegistrationSettings(settings);
        out.estimate=content_registration::EstimateRegistration(*in.camera_a,*in.camera_b,parameters_fine,settings,in.thread_count,in.control);
    }catch(const std::invalid_argument& e){Refuse(out,Refusal::estimate_failed,e.what());return out;}
    const auto& estimate=out.estimate;
    if(std::find(estimate.reasons.begin(),estimate.reasons.end(),"time-budget")!=estimate.reasons.end() || Stopped(in)){
        Refuse(out,Refusal::stopped,"stopped during the fine stage");return out;
    }
    if(estimate.has_estimate){out.has_pose=true;out.pose=estimate.pose;}
    if(out.has_pose){FillPrevious(out.display,in.active);FillMotion(out.display,in,parent,out.pose);}
    if(estimate.state!=content_registration::RegistrationState::verified || !estimate.has_estimate){
        // A pose beyond the tilt or the height line is the condition's own reason to start again from the chart: say it by name.
        for(const auto component:estimate.exceeded){
            if(component==content_registration::EnvelopeComponent::tilt)Refuse(out,Refusal::tilt_exceeded,"the estimated tilt is over registration.envelope.tiltDeg");
            if(component==content_registration::EnvelopeComponent::height)Refuse(out,Refusal::height_exceeded,"the estimated height change is over registration.envelope.heightPercent");
        }
        Refuse(out,Refusal::estimate_not_verified,"fine stage: "+std::string(content_registration::RegistrationStateCode(estimate.state))+" ["+joined(estimate.reasons)+"]");
        return out;
    }
    const PoseParameters p_hat=estimate.pose;
    {
        const auto& env=in.active.registration.envelope;
        // The camera was moved, not turned: a tilt or a change of height is not what this does (docs: the lens and A are kept, and
        // the pose of B is a shift of the paper plane). The lines are the condition's own.
        if(std::hypot(p_hat.rx_deg,p_hat.ry_deg)>env.tilt_deg)Refuse(out,Refusal::tilt_exceeded,"the estimated tilt is over registration.envelope.tiltDeg");
        if(std::abs(p_hat.tz_percent)>env.height_percent)Refuse(out,Refusal::height_exceeded,"the estimated height change is over registration.envelope.heightPercent");
        if(!out.refusals.empty())return out;
    }

    // The residual in camera B's pixels is measured on the mapping that the new profile will have.
    auto moved_b=parent.parameters.camera_b;
    moved_b.document_to_image=PoseModel(parent.parameters.camera_b).CameraHomography(p_hat);
    out.fit=FitInCameraPixels(estimate,raster_fine,moved_b);
    const auto record=EstimateRecord(in,facts,out.wide,estimate,p_hat,out.fit,parent.profile_fingerprint_sha256);

    BuildFromPose(in,out,p_hat,out.fit,record);
    if(!out.candidate)return out;
    if(Stopped(in)){Refuse(out,Refusal::stopped,"stopped after the creation check");out.candidate.reset();return out;}

    Announce(in,Stage::verify);
    out.verification=VerifyCandidate(in,*out.candidate);
    const auto& v=out.verification;
    if(Stopped(in) || std::find(v.reasons.begin(),v.reasons.end(),"time-budget")!=v.reasons.end()){
        Refuse(out,Refusal::stopped,"stopped during the verification");out.candidate.reset();return out;
    }
    if(v.state!=content_registration::RegistrationState::verified || !v.passed){
        if(v.state!=content_registration::RegistrationState::verified)
            Refuse(out,Refusal::reestimate_not_verified,"verification: "+std::string(content_registration::RegistrationStateCode(v.state))+" ["+joined(v.reasons)+"]");
        else Refuse(out,Refusal::reestimate_displacement,"verification: the estimate on the new profile still moves the seam by "+Num(v.seam_displacement_px)+
            " px (the line is "+Num(v.limit_px)+" px)");
        out.candidate.reset();
        return out;
    }
    out.ready=true;
    return out;
}

FixedCompositeSelection SelectionOfCandidate(const Candidate& candidate){
    FixedCompositeSelection selection;
    selection.loaded=LoadedFixedCompositeConditionAny{LoadedFixedCompositeConditionV3{candidate.condition,candidate.sha256,candidate.binding}};
    return selection;
}
}
