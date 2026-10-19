#include "a0/m2/color_correction.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace a0::m2::correction {
namespace {
using namespace render;
using Outcome=ColorCorrectionOutcome;
void Validate(const ColorCorrectionPolicy& p) {
    if(p.minimum_samples<3 || p.minimum_samples>kMaximumColorSamples ||
        !std::isfinite(p.minimum_sample_dn) || !std::isfinite(p.maximum_sample_dn) ||
        p.minimum_sample_dn<0 || p.maximum_sample_dn>255 || !(p.minimum_sample_dn<p.maximum_sample_dn) ||
        !std::isfinite(p.minimum_stddev_dn) || !(p.minimum_stddev_dn>0))throw std::invalid_argument("color correction: invalid explicit policy");
    for(const auto n:{p.maximum_gain_difference_percent,p.maximum_offset_dn,p.maximum_residual_dn})
        if(!std::isfinite(n) || n<0)throw std::invalid_argument("color correction: invalid explicit budget");
}
void Validate(const BgrImage& p) {
    if(p.bgr.size()!=PixelCount(p.width,p.height)*3)throw std::invalid_argument("color correction: invalid image buffer");
}
} // namespace
ColorCorrectionResult EstimateBoundedColorCorrection(std::span<const ColorCorrespondence> samples,const ColorCorrectionPolicy& policy) {
    Validate(policy);
    ColorCorrectionResult result{Outcome::Estimated,samples.size(),{},{},{}};
    if(samples.size()>kMaximumColorSamples){result.outcome=Outcome::TooManySamples;return result;}
    // Invalid observations never become a subset of valid ones.
    for(std::size_t n=0;n<samples.size();++n)for(std::size_t c=0;c<3;++c)for(const double v:{samples[n].camera_a_bgr[c],samples[n].camera_b_bgr[c]}) {
        const auto reason=!std::isfinite(v) || v<0 || v>255 ? Outcome::InvalidSamples :
            v<policy.minimum_sample_dn || v>policy.maximum_sample_dn ? Outcome::SampleRangeRejected : Outcome::Estimated;
        if(reason!=Outcome::Estimated){result.outcome=reason;result.violations.push_back({reason,c,n});return result;}
    }
    if(samples.size()<policy.minimum_samples){result.outcome=Outcome::InsufficientSamples;return result;}
    const double count=static_cast<double>(samples.size()),root_count=std::sqrt(count);
    const auto violation=[&](Outcome reason,std::size_t c){result.violations.push_back({reason,c,{}});};
    std::array<double,3> gains{},offsets{};
    for(std::size_t c=0;c<3;++c) {
        double mean_a=0,mean_b=0;
        for(const auto& p:samples){mean_a+=p.camera_a_bgr[c];mean_b+=p.camera_b_bgr[c];}mean_a/=count;mean_b/=count;
        double norm_a=0,norm_b=0;
        for(const auto& p:samples){norm_a=std::hypot(norm_a,p.camera_a_bgr[c]-mean_a);norm_b=std::hypot(norm_b,p.camera_b_bgr[c]-mean_b);}
        auto& diagnostic=result.channels[c].emplace(ColorChannelDiagnostics{norm_a/root_count,norm_b/root_count,{},{}});
        if(diagnostic.camera_a_stddev_dn<policy.minimum_stddev_dn || diagnostic.camera_b_stddev_dn<policy.minimum_stddev_dn){violation(Outcome::InsufficientContrast,c);continue;}
        bool identical=true;double correlation=0;
        for(const auto& p:samples){identical=identical&&p.camera_a_bgr[c]==p.camera_b_bgr[c];correlation+=((p.camera_a_bgr[c]-mean_a)/norm_a)*((p.camera_b_bgr[c]-mean_b)/norm_b);}
        const double gain=identical ? 1 : (norm_a/norm_b)*correlation;
        const double offset=identical ? 0 : mean_a-gain*mean_b;
        if(!std::isfinite(gain) || !std::isfinite(offset)){violation(Outcome::NumericalFailure,c);continue;}
        diagnostic.estimate=ChannelAffine{gain,offset};
        if(!(gain>0)){violation(Outcome::NonpositiveGain,c);continue;}
        double residual_norm=0,residual_max=0;
        for(const auto& p:samples){const double error=std::abs((gain*p.camera_b_bgr[c]+offset)-p.camera_a_bgr[c]);residual_norm=std::hypot(residual_norm,error);residual_max=std::max(residual_max,error);}
        const double percent=std::abs(gain-1)*100;
        if(!std::isfinite(residual_norm) || !std::isfinite(percent)){violation(Outcome::NumericalFailure,c);continue;}
        diagnostic.residuals=ColorResiduals{residual_norm/root_count,residual_max};gains[c]=gain;offsets[c]=offset;
        if(percent>policy.maximum_gain_difference_percent)violation(Outcome::GainExceeded,c);
        if(std::abs(offset)>policy.maximum_offset_dn)violation(Outcome::OffsetExceeded,c);
        if(residual_max>policy.maximum_residual_dn)violation(Outcome::ResidualExceeded,c);
    }
    if(!result.violations.empty()){result.outcome=result.violations.front().reason;return result;}
    result.transform.emplace(gains,offsets);return result;
}
ImageColorCorrectionResult EstimateImageColorCorrection(const render::BgrImage& a,const render::BgrImage& b,
    const render::CameraProjection& camera_a,const render::CameraProjection& camera_b,
    const render::OutputRaster& raster,std::span<const render::Point> seeds,const ColorCorrectionPolicy& policy) {
    Validate(policy);Validate(a);Validate(b);render::ValidateCameraProjection(camera_a,raster);render::ValidateCameraProjection(camera_b,raster);
    ImageColorCorrectionResult result{ImageColorOutcome::SamplingFailed,{},{}};
    if(seeds.size()>kMaximumColorSamples){result.outcome=ImageColorOutcome::TooManySeeds;return result;}
    const auto domain=render::DocumentRasterCorners(raster);std::set<std::pair<double,double>> unique;
    std::vector<ColorCorrespondence> samples;bool complete=true;
    for(const auto p:seeds) {
        DocumentColorSample point{p,ColorSampleOutcome::InvalidSeed,{}};
        if(std::isfinite(p.x) && std::isfinite(p.y) && p.x>=domain[0].x && p.x<=domain[2].x && p.y>=domain[0].y && p.y<=domain[2].y && unique.emplace(p.x,p.y).second) {
            render::Point pa{},pb{};ColorCorrespondence values{};point.outcome=ColorSampleOutcome::SampleUnavailable;
            if(render::TryProjectDocument(camera_a,p,pa) && render::TryProjectDocument(camera_b,p,pb) &&
                render::SampleBilinear(a,pa.x,pa.y,values.camera_a_bgr) && render::SampleBilinear(b,pb.x,pb.y,values.camera_b_bgr)) {
                point.outcome=ColorSampleOutcome::Sampled;point.colors=values;samples.push_back(values);
            }
        }
        if(!point.colors)complete=false;result.points.push_back(point);
    }
    if(!complete)return result;
    result.correction=EstimateBoundedColorCorrection(samples,policy);
    result.outcome=result.correction->transform ? ImageColorOutcome::Estimated : ImageColorOutcome::CorrectionRejected;return result;
}
} // namespace a0::m2::correction
