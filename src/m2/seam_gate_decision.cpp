#include "a0/m2/seam_gate.hpp"
#include "a0/common/protocol_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>

namespace a0::m2::seam_gate {
namespace {
bool ValidLimits(const SeamGateLimits& l) {
    return std::isfinite(l.limit_px) && l.limit_px > 0 && std::isfinite(l.guard_band_px) && l.guard_band_px >= 0 &&
        l.guard_band_px < l.limit_px;
}
// The owner's rule (#385 O1): measured maximum + guard band <= limit passes.
bool Over(double value, const SeamGateLimits& l) { return value+l.guard_band_px > l.limit_px; }

std::string Q(std::string_view s) { return "\""+a0::common::protocol_json::JsonEscape(s)+"\""; }
std::string N(double v) {
    if (!std::isfinite(v)) return "null";
    char text[64]; const auto p = std::to_chars(text,text+sizeof(text),v);
    if (p.ec != std::errc{}) throw std::invalid_argument("seam gate report: number not representable");
    return std::string(text,p.ptr);
}
std::string N(const std::optional<double>& v) { return v ? N(*v) : "null"; }
std::string U(std::uint64_t v) { return std::to_string(v); }
std::string B(bool v) { return v ? "true" : "false"; }
std::string P(Point p) { return "["+N(p.x)+","+N(p.y)+"]"; }
std::string_view KindCode(PointKind k) { return k == PointKind::one_directional ? "one-directional" : "two-directional"; }
std::string_view ConfirmationCode(Confirmation c) {
    return c == Confirmation::confirmed ? "confirmed" : c == Confirmation::unconfirmed ? "unconfirmed" : "not-required";
}
} // namespace

std::string_view SeamGateStateCode(SeamGateState s) {
    switch (s) {
    case SeamGateState::pass: return "pass";
    case SeamGateState::fail: return "fail";
    case SeamGateState::undetermined: return "undetermined";
    }
    throw std::invalid_argument("unknown seam gate state");
}

SeamGateResult UndeterminedSeamGate(std::string reason, const SeamGateLimits& limits) {
    SeamGateResult r;
    r.state = SeamGateState::undetermined;
    r.limits = limits;
    r.findings.push_back({std::move(reason),"check-inputs"});
    return r;
}

SeamGateResult DecideSeamGate(const SeamMeasurement& m, const SeamGateLimits& limits) {
    if (!ValidLimits(limits)) return UndeterminedSeamGate("invalid-limits",limits);
    // Remeasurement was triggered with the measurement's limits; a decision
    // with other limits would read points that were never confirmed.
    if (m.limits.limit_px != limits.limit_px || m.limits.guard_band_px != limits.guard_band_px)
        return UndeterminedSeamGate("limits-differ-from-measurement",limits);
    SeamGateResult r;
    r.limits = limits;
    std::vector<double> values;
    const SeamPoint* top = nullptr;
    double sum = 0;
    for (const auto& p : m.points) {
        if (p.status == PointStatus::valid && p.confirmation == Confirmation::unconfirmed) ++r.unconfirmed_points;
        if (!p.Counted()) continue;
        ++r.counted_points;
        if (p.confirmation == Confirmation::confirmed) ++r.confirmed_points;
        values.push_back(p.visible_px); sum += p.visible_px;
        if (!top || p.visible_px > top->visible_px) top = &p;
    }
    if (top) {
        r.measured_maximum_px = top->visible_px;
        std::sort(values.begin(),values.end());
        if (values.size() > 1) r.second_px = values[values.size()-2];
        // Nearest rank.
        const auto rank = static_cast<std::size_t>(std::ceil(0.95*static_cast<double>(values.size())));
        r.p95_px = values[std::max<std::size_t>(rank,1)-1];
        r.mean_px = sum/static_cast<double>(values.size());
        r.maximum_px = top->visible_px;
        r.maximum_at = SeamGateLocation{top->x,top->y,top->block_column,top->block_row};
    }
    const SeamBlock* predicted_top = nullptr;
    for (const auto& b : m.blocks) {
        switch (b.block_class) {
        case BlockClass::measured: ++r.measured_blocks; break;
        case BlockClass::unmeasurable_content: ++r.unmeasurable_blocks; break;
        case BlockClass::empty:
            ++r.empty_blocks;
            if (!b.predicted_px) ++r.unpredicted_empty_blocks;
            else if (!predicted_top || *b.predicted_px > *predicted_top->predicted_px) predicted_top = &b;
            break;
        case BlockClass::outside: break;
        }
    }
    if (predicted_top) {
        r.predicted_maximum_px = predicted_top->predicted_px;
        if (!r.maximum_px || *predicted_top->predicted_px > *r.maximum_px) {
            r.maximum_px = predicted_top->predicted_px;
            r.maximum_at = SeamGateLocation{predicted_top->x+predicted_top->width/2,predicted_top->y+predicted_top->height/2,
                predicted_top->column,predicted_top->row};
            r.maximum_is_prediction = true;
        }
    }
    if (r.measured_maximum_px && Over(*r.measured_maximum_px,limits)) r.findings.push_back({"seam-misalignment","retake-then-recalibrate"});
    if (r.predicted_maximum_px && Over(*r.predicted_maximum_px,limits)) r.findings.push_back({"predicted-misalignment","retake-then-recalibrate"});
    if (r.unmeasurable_blocks) r.findings.push_back({"unmeasurable-content","shift-paper-and-retake"});
    if (!r.measured_blocks) r.findings.push_back({"no-measurable-block","place-lines-on-seam-and-retake"});
    // Draft 3.5: a block without content and without a calibration
    // prediction has no value, and is not counted as within the limit.
    if (r.unpredicted_empty_blocks) r.findings.push_back({"unpredicted-empty-block","use-calibrated-prediction"});
    r.state = r.findings.empty() ? SeamGateState::pass : SeamGateState::fail;
    return r;
}

std::string SeamGateReportJson(const SeamGateResult& r, const SeamMeasurement* m) {
    std::string s = "{\"schema\":"+Q(kSeamGateReportSchema)+",\"state\":"+Q(SeamGateStateCode(r.state))+",\"findings\":[";
    for (std::size_t i = 0; i < r.findings.size(); ++i)
        s += (i ? "," : "")+std::string("{\"reason\":")+Q(r.findings[i].reason)+",\"remedy\":"+Q(r.findings[i].remedy)+"}";
    s += "],\"limits\":{\"limitPx\":"+N(r.limits.limit_px)+",\"guardBandPx\":"+N(r.limits.guard_band_px)+
        ",\"source\":"+Q(r.limits.source)+",\"rule\":\"maximum-plus-guard-band-at-most-limit\"}";
    s += ",\"statistics\":{\"maximumPx\":"+N(r.maximum_px)+",\"maximumAt\":";
    if (r.maximum_at) s += "{\"x\":"+U(r.maximum_at->x)+",\"y\":"+U(r.maximum_at->y)+",\"blockColumn\":"+U(r.maximum_at->block_column)+
        ",\"blockRow\":"+U(r.maximum_at->block_row)+"}";
    else s += "null";
    s += ",\"maximumIsPrediction\":"+B(r.maximum_is_prediction)+",\"measuredMaximumPx\":"+N(r.measured_maximum_px)+
        ",\"secondPx\":"+N(r.second_px)+",\"p95Px\":"+N(r.p95_px)+",\"meanPx\":"+N(r.mean_px)+
        ",\"predictedMaximumPx\":"+N(r.predicted_maximum_px)+",\"countedPoints\":"+U(r.counted_points)+
        ",\"confirmedPoints\":"+U(r.confirmed_points)+",\"unconfirmedPoints\":"+U(r.unconfirmed_points)+"}";
    s += ",\"blocks\":{\"measured\":"+U(r.measured_blocks)+",\"empty\":"+U(r.empty_blocks)+",\"unmeasurableContent\":"+
        U(r.unmeasurable_blocks)+",\"emptyWithoutPrediction\":"+U(r.unpredicted_empty_blocks)+
        ",\"emptyWithoutPredictionPasses\":false}";
    if (!m) return s+",\"measurement\":null}";
    const auto& d = m->design;
    s += ",\"measurement\":{\"design\":{\"pointSpacingPx\":"+U(d.point_spacing_px)+",\"patchPx\":"+U(d.patch_px)+
        ",\"blockPx\":"+U(d.block_px)+",\"searchRadiusPx\":"+U(d.search_radius_px)+",\"maximumSearchRadiusPx\":"+
        U(d.maximum_search_radius_px)+",\"refinementLevels\":"+U(d.refinement_levels)+",\"contentGradientDn\":"+
        N(d.content_gradient_dn)+",\"contentMinimumSamples\":"+U(d.content_minimum_samples)+",\"oneDirectionRatio\":"+
        N(d.one_direction_ratio)+",\"secondaryPeakRatio\":"+N(d.secondary_peak_ratio)+",\"roundTripPx\":"+N(d.round_trip_px)+
        ",\"uncertaintyFloorPx\":"+N(d.uncertainty_floor_px)+",\"maximumUncertaintyPx\":"+N(d.maximum_uncertainty_px)+
        ",\"remeasureShiftPx\":"+U(d.remeasure_shift_px)+",\"remeasurePatchPx\":"+U(d.remeasure_patch_px)+
        ",\"agreementFactor\":"+N(d.agreement_factor)+",\"agreementFloorPx\":"+N(d.agreement_floor_px)+
        ",\"blockNeighborPx\":"+N(d.block_neighbor_px)+",\"stripRows\":"+U(d.strip_rows)+"}";
    s += ",\"grid\":{\"width\":"+U(m->width)+",\"height\":"+U(m->height)+",\"originX\":"+U(m->origin_x)+",\"originY\":"+
        U(m->origin_y)+",\"blockColumns\":"+U(m->block_columns)+",\"blockRows\":"+U(m->block_rows)+"}";
    s += ",\"prediction\":{\"supplied\":"+B(m->prediction_supplied)+",\"mismatchPx\":"+N(m->prediction_mismatch_px)+"}";
    std::map<std::string_view,std::uint64_t> counts;
    for (const auto& p : m->points) ++counts[PointStatusCode(p.status)];
    s += ",\"pointStatusCounts\":{";
    bool first = true;
    for (const auto& [code,count] : counts) { s += (first ? "" : ",")+Q(code)+":"+U(count); first = false; }
    s += "},\"blockList\":[";
    first = true;
    for (const auto& b : m->blocks) {
        if (b.block_class == BlockClass::outside) continue;
        s += (first ? "" : ",")+std::string("{\"column\":")+U(b.column)+",\"row\":"+U(b.row)+",\"x\":"+U(b.x)+",\"y\":"+U(b.y)+
            ",\"width\":"+U(b.width)+",\"height\":"+U(b.height)+",\"class\":"+Q(BlockClassCode(b.block_class))+
            ",\"points\":"+U(b.points)+",\"contentPoints\":"+U(b.content_points)+",\"validPoints\":"+U(b.valid_points)+
            ",\"countedPoints\":"+U(b.counted_points)+",\"confirmedPoints\":"+U(b.confirmed_points)+
            ",\"unconfirmedPoints\":"+U(b.unconfirmed_points)+",\"maximumPx\":"+N(b.maximum_px)+",\"uncertaintyPx\":"+
            N(b.uncertainty_px)+",\"neighborInconsistent\":"+B(b.neighbor_inconsistent)+",\"predictedPx\":"+N(b.predicted_px)+"}";
        first = false;
    }
    s += "],\"remeasuredPoints\":[";
    first = true;
    for (const auto& p : m->points) {
        if (p.remeasurements.empty()) continue;
        s += (first ? "" : ",")+std::string("{\"x\":")+U(p.x)+",\"y\":"+U(p.y)+",\"blockColumn\":"+U(p.block_column)+
            ",\"blockRow\":"+U(p.block_row)+",\"kind\":"+Q(KindCode(p.kind))+",\"normal\":"+P(p.normal)+",\"displacementPx\":"+
            P(p.displacement)+",\"visiblePx\":"+N(p.visible_px)+",\"uncertaintyPx\":"+N(p.uncertainty_px)+
            ",\"confirmation\":"+Q(ConfirmationCode(p.confirmation))+",\"variants\":[";
        for (std::size_t i = 0; i < p.remeasurements.size(); ++i) {
            const auto& v = p.remeasurements[i];
            s += (i ? "," : "")+std::string("{\"variant\":")+Q(VariantCode(v.variant))+",\"status\":"+Q(PointStatusCode(v.status))+
                ",\"kind\":"+Q(KindCode(v.kind))+",\"displacementPx\":"+P(v.displacement)+",\"uncertaintyPx\":"+N(v.uncertainty_px)+
                ",\"agrees\":"+B(v.agrees)+"}";
        }
        s += "]}";
        first = false;
    }
    return s+"]}}";
}

} // namespace a0::m2::seam_gate
