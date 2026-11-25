// The pair capture's composite with a condition v3 and what is said after the shot (#417 J8w): the selection of a setup's
// condition (the revision in force), the registered composite through ComposePairWithSelection, and the status row and log lines of
// DescribeRegistrationReview. The same functions SavePairReview calls, without the window and without a camera.
//
// Everything is synthetic. The frames are full size (7360 x 4912) JPEG files of a drawing-like texture in the strip around the
// overlap, from the rig profile v2 fixture (10 pixels per mm, the cameras side by side), as the JPEG path's own test (J8) builds
// them. Argument 1 is the shared rig profile v2 fixture folder.
//
// What is checked here and not in the window's smoke (--pair-v3-ui-smoke, flat frames): the best level (G1) with real content, the
// stages the time budget drops (the deadline is the only thing that differs), and the sentences made from a real report.
#include "a0/m2/experimental/fixed_composite_maps.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/experimental/confirmed_position.hpp"
#include "a0/phase0/experimental/live_registration_runtime.hpp"
#include "a0/phase0/experimental/pair_review_registration.hpp"
#include "a0/phase0/experimental/pair_review_synthetic.hpp"
#include "similarity_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace a0::phase0::experimental;
using namespace a0::phase0::experimental::pair_synthetic;
namespace m2e = a0::m2::experimental;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* what, int line) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAILED (line " << line << "): " << what << '\n'; }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)
double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// ---- the world and the frames (as the J8 test) ----
struct World {
    similarity_scene::Scene scene;
    static constexpr double kX0 = 546, kY0 = -13, kPerMm = 5;
    explicit World(std::uint64_t seed) : scene(similarity_scene::MakeScene(static_cast<int>((756 - kX0) * kPerMm), static_cast<int>((485 - kY0) * kPerMm), 0, 0, seed)) {}
    double At(double X, double Y) const {
        const double sx = scene.ox + (X - kX0) * kPerMm, sy = scene.oy + (Y - kY0) * kPerMm;
        const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
        if (x0 < 0 || y0 < 0 || x0 + 1 >= scene.width || y0 + 1 >= scene.height) return 232;
        const double fx = sx - x0, fy = sy - y0;
        const auto* t = scene.v.data() + static_cast<size_t>(y0) * scene.width + x0;
        const auto* b = t + scene.width;
        return (t[0] * (1 - fx) + t[1] * fx) * (1 - fy) + (b[0] * (1 - fx) + b[1] * fx) * fy;
    }
};
void FillStrip(render::BgrImage& image, const render::CameraProjection& camera, unsigned u0, unsigned u1, const World& world) {
    constexpr unsigned step = 8;
    const unsigned gw = (u1 - u0 + step - 1) / step + 2, gh = (kHeight + step - 1) / step + 2;
    std::vector<render::Point> grid(static_cast<size_t>(gw) * gh);
    for (unsigned gj = 0; gj < gh; ++gj)
        for (unsigned gi = 0; gi < gw; ++gi) {
            render::Point doc{};
            if (!m2e::TryDocumentPointOfRawPixel(camera, {double(u0 + gi * step), double(gj * step)}, doc)) throw std::runtime_error("fixture grid has no document point");
            grid[static_cast<size_t>(gj) * gw + gi] = doc;
        }
    const auto rows = [&](unsigned v0, unsigned v1) {
        for (unsigned v = v0; v < v1; ++v) {
            const unsigned gj = v / step;
            const double fy = (v % step) / double(step);
            auto* out = image.bgr.data() + (static_cast<size_t>(v) * kWidth + u0) * 3;
            for (unsigned u = u0; u < u1; ++u, out += 3) {
                const unsigned gi = (u - u0) / step;
                const double fx = ((u - u0) % step) / double(step);
                const auto& p00 = grid[static_cast<size_t>(gj) * gw + gi];
                const auto& p10 = grid[static_cast<size_t>(gj) * gw + gi + 1];
                const auto& p01 = grid[static_cast<size_t>(gj + 1) * gw + gi];
                const auto& p11 = grid[static_cast<size_t>(gj + 1) * gw + gi + 1];
                similarity_scene::Put(out, world.At((p00.x * (1 - fx) + p10.x * fx) * (1 - fy) + (p01.x * (1 - fx) + p11.x * fx) * fy,
                    (p00.y * (1 - fx) + p10.y * fx) * (1 - fy) + (p01.y * (1 - fx) + p11.y * fx) * fy));
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < 4; ++t) pool.emplace_back(rows, kHeight * t / 4, kHeight * (t + 1) / 4);
    for (auto& th : pool) th.join();
}

fs::path root;

m2e::StillReviewRequest RequestFor(const Frames& f, const char* output, m2e::MountOrientation orientation, std::chrono::seconds window) {
    m2e::PreviewReviewGeometry geometry{460, 307, 229, 3, 11, orientation};
    return {f.a, f.b, f.root / output, Sha(f.a), Sha(f.b), "synthetic-pair-registered", geometry, Clock::now() + window};
}
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The sentences made from a report never carry a path, a camera identifier or the work folder.
void NoIdentifiers(const RegistrationReviewLines& lines, const char* what) {
    std::string all;
    for (const auto& s : lines.status) all += s + "\n";
    for (const auto& s : lines.warnings) all += s + "\n";
    for (const auto& s : lines.log) all += s + "\n";
    Check(!all.empty() && !Contains(all, root.string()) && !Contains(all, "CAM-A") && !Contains(all, "CAM-B") && !Contains(all, "original.jpg") && !Contains(all, "synthetic-pair"), what, __LINE__);
}

void Sentences(const m2e::StillReviewResult& review, const char* name) {
    const auto lines = DescribeRegistrationReview(review);
    const auto& record = *review.fixed_composite_registration;
    std::cout << name << ": status\n";
    for (const auto& s : lines.status) std::cout << "  " << s << '\n';
    for (const auto& s : lines.log) std::cout << "  log: " << s << '\n';
    CHECK(lines.status.size() >= 4);
    CHECK(Contains(lines.status.front(), std::string("位置合わせを G") + std::string(m2e::GeometryLevelCode(record.registration.level.level)).substr(1)));
    CHECK(lines.warnings.size() == record.warnings.size());
    // The log: the head names the level, the stages, the gate and the assessment; one line per warning with its sentence; the times.
    CHECK(!lines.log.empty() && lines.log.front().rfind("pair_review_registration level=" + std::string(m2e::GeometryLevelCode(record.registration.level.level)) + " ", 0) == 0);
    CHECK(Contains(lines.log.front(), " gate=") && Contains(lines.log.front(), " assessment=") && Contains(lines.log.front(), "quality_accepted=0") && Contains(lines.log.front(), "geometry_hash=" + record.registration.geometry_hash));
    std::size_t warning_lines = 0;
    for (const auto& line : lines.log) if (line.rfind("pair_review_registration_warning ", 0) == 0) {
        CHECK(Contains(line, " text=" + lines.warnings[warning_lines]));
        ++warning_lines;
    }
    CHECK(warning_lines == record.warnings.size());
    CHECK(Contains(lines.log.back(), "pair_review_registration_ms estimate_ms=") && Contains(lines.log.back(), " composite_ms=") && Contains(lines.log.back(), " skipped="));
    CHECK(review.registration_milliseconds.has_value());
    if (review.registration_milliseconds) {
        // A stage that ran has its time in the log; a stage that did not has none.
        for (std::size_t i = 0; i < review.registration_milliseconds->stage.size(); ++i)
            CHECK(review.registration_milliseconds->stage[i].has_value() == Contains(lines.log.back(), std::string(m2e::kReviewStageTexts[i].second) + "_ms="));
    }
    // The assessment is never the word for "passed", and the row never says the composite cannot be saved.
    for (const auto& s : lines.status) CHECK(!Contains(s, "合格") && !Contains(s, "書き出せません") && !Contains(s, "保存できません"));
    NoIdentifiers(lines, "no path and no camera identifier in the sentences");
}
}

// What the sentence after the shot says the live view does, against what the machine does with the same result. The sentences are made from the record
// alone (ConfirmedPositionText); the machine is given the level and the mapping that the record has.
void SentencesAndMachine() {
    const std::string sha(64, '7'), other(64, '8');
    const m2e::content_registration::PoseParameters base{0.05, 0, 0, 0.5, 0, 0};
    const m2e::RegisteredMapping condition{base, {}, std::nullopt};
    const auto record = [&](m2e::ConfirmedPositionSource source, m2e::GeometryLevel level, bool used, const m2e::RegisteredMapping& position, const std::string& position_sha,
                            std::vector<std::string> reasons, const m2e::RegisteredMapping& level_mapping) {
        m2e::SavedFixedCompositeRegistration r;
        r.condition_sha256 = sha;
        r.base_pose = base;
        m2e::ConfirmedPosition p;
        p.mapping = position;
        p.source = source;
        p.serial = 0;
        p.condition_sha256 = position_sha;
        if (source == m2e::ConfirmedPositionSource::manual) { p.base_source = m2e::ConfirmedPositionSource::auto_start; p.manual_entered = m2e::HandCounts{3, 0, 0}; }
        r.registration.confirmed_position = p;
        r.registration.confirmed_check = m2e::ConfirmedPositionCheck{used, std::nullopt};
        r.registration.level.level = level;
        r.registration.level.reasons = std::move(reasons);
        r.registration.level.mapping = level_mapping;
        return r;
    };
    const auto estimate = m2e::RegisteredMapping{{0.15, -0.05, 0.1, 0.9, -0.25, 0.1}, {}, std::nullopt};
    // A machine that holds an automatic position (the first session's estimate: auto-start, as the records below say), or the same position moved
    // by hand from it (the check is the test's own arithmetic).
    const LiveHandChecker check = [](const m2e::RegisteredMapping& hand_base, const m2e::HandCounts& total) {
        LiveHandCheck out;
        out.status = LiveHandCheck::Status::accepted;
        out.mapping = hand_base;
        out.mapping.pose.tx_mm += m2e::HandValuesOf(total).seam_dx_px / 16.0;
        return out;
    };
    const auto machine_at = [&](bool by_hand) {
        LivePlacementMachine machine;
        machine.BeginSession(sha, base, true);
        const auto job = machine.BeginJob();
        LiveEstimateOutcome outcome;
        outcome.usable = true;
        outcome.pose = estimate.pose;
        machine.Accept(*job, outcome);
        machine.Committed();
        if (by_hand) { CHECK(machine.HandStep(m2e::HandCounts{3, 0, 0}, LiveHandContext{true, true}, check).outcome == LiveHandOutcome::accepted); machine.Committed(); }
        return machine;
    };
    const auto apply = [&](LivePlacementMachine& machine, const m2e::SavedFixedCompositeRegistration& r) {
        const auto& c = r.registration;
        const auto grant = machine.BeginCapture(true).grant;
        CHECK(grant.has_value());
        return machine.ApplyCapture(c.level.mapping, std::string(m2e::GeometryLevelCode(c.level.level)), r.condition_sha256, std::nullopt, grant ? std::optional<std::uint64_t>(grant->serial) : std::nullopt);
    };
    // The live position was used (level confirmed): the live view stays where it was.
    {
        auto machine = machine_at(false);
        const auto before = machine.View();
        const auto r = record(m2e::ConfirmedPositionSource::auto_start, m2e::GeometryLevel::confirmed, true, machine.Placement(), sha, {}, machine.Placement());
        CHECK(Contains(ConfirmedPositionText(r), "ライブで決めた位置で作りました"));
        CHECK(apply(machine, r) && LivePlacementSameMapping(machine.Placement(), before.mapping) && machine.Source() == LivePlacementSource::auto_start && machine.Serial() == before.serial);
    }
    // The hand position was used: it stays by hand.
    {
        auto machine = machine_at(true);
        const auto before = machine.View();
        const auto r = record(m2e::ConfirmedPositionSource::manual, m2e::GeometryLevel::confirmed, true, machine.Placement(), sha, {}, machine.Placement());
        CHECK(Contains(ConfirmedPositionText(r), "手で直した位置で JPEG を作りました"));
        CHECK(apply(machine, r) && machine.Source() == LivePlacementSource::manual && LivePlacementSameMapping(machine.Placement(), before.mapping) && machine.Serial() == before.serial);
    }
    // The hand position was refused by the cheap check: the JPEG has the condition's position, the live view stays at the hand position.
    {
        auto machine = machine_at(true);
        const auto before = machine.View();
        const auto r = record(m2e::ConfirmedPositionSource::manual, m2e::GeometryLevel::g4, false, machine.Placement(), sha, {"seam-outside-overlap-at-level"}, condition);
        CHECK(Contains(ConfirmedPositionText(r), "ライブは手で直した位置のままです"));
        CHECK(apply(machine, r) && machine.Source() == LivePlacementSource::manual && LivePlacementSameMapping(machine.Placement(), before.mapping) && machine.Serial() == before.serial &&
              machine.View().hand_entered == (m2e::HandCounts{3, 0, 0}));
    }
    // The live position was refused as well: the JPEG has the condition's position and the live view goes to it.
    {
        auto machine = machine_at(false);
        const auto r = record(m2e::ConfirmedPositionSource::auto_start, m2e::GeometryLevel::g4, false, machine.Placement(), sha, {"output-uncovered-at-level", "output-uncovered-at-level", "output-uncovered-at-level"}, condition);
        CHECK(Contains(ConfirmedPositionText(r), "ライブも条件の位置にしました"));
        CHECK(apply(machine, r) && LivePlacementSameMapping(machine.Placement(), condition) && machine.Source() == LivePlacementSource::after_capture);
    }
    // The condition changed before the shot: the result is not applied to the live view, and the next session goes back to the new condition's position and asks once.
    {
        auto machine = machine_at(true);
        const auto r = record(m2e::ConfirmedPositionSource::manual, m2e::GeometryLevel::g1, false, machine.Placement(), sha, {}, estimate);
        auto changed = r;
        changed.condition_sha256 = other;
        CHECK(Contains(ConfirmedPositionText(changed), "手で直した位置を捨て、ライブは新しい条件の位置に戻し、自動調整を 1 回行います"));
        const auto before = machine.View();
        const auto grant = machine.BeginCapture(true).grant;
        CHECK(grant && !machine.ApplyCapture(estimate, "G1", other, std::nullopt, grant->serial) && LivePlacementSameMapping(machine.Placement(), before.mapping) && machine.Source() == LivePlacementSource::manual);
        machine.BeginSession(other, base, true);
        const auto view = machine.View();
        CHECK(view.source == LivePlacementSource::fixed_condition && LivePlacementSameMapping(view.mapping, condition) && view.hand_discarded_by_condition &&
              view.activity == LivePlacementActivity::requested && view.ask == LiveEstimateAsk::condition_changed && machine.BeginJob().has_value());
    }
}

// A position that the live estimate decided goes through the capture's entrance and the report v10's writer (#434): the estimate's outcome
// is made with the product's live mouth (LiveAppliedSurfaceOf) on the inputs that the product's estimator makes from the plan of this
// condition, then it goes through the machine of the window (session, job, accept, drawn, capture) and the grant is given to
// ComposePairWithSelection, the function the window's SavePairReview calls. The estimated plane and the pose are made up and rounded
// (those of 1b, with a slope across the width). No level and no `used` is asserted: only that the report is written and read, and its
// surface.
//  (1) the live position: the report is v10, and the position read back has no slope across the width and the mouth's height and slope;
//  (2) the same position moved once by hand (the product's SolveHandStep on the same model, centre and origin): the same;
//  (3) the estimated plane itself as the position (the outcome before the mouth): the entrance refuses it and nothing is created.
namespace live_position {
bool SameBits(double a, double b) { std::uint64_t x{}, y{}; std::memcpy(&x, &a, sizeof x); std::memcpy(&y, &b, sizeof y); return x == y; }
m2e::ConfirmedPositionSource SourceOf(LivePlacementSource source) {
    const auto parsed = m2e::ParseConfirmedPositionSource(LivePlacementSourceCode(source));
    if (!parsed) throw std::logic_error("a position source that the report does not know");
    return *parsed;
}
// The fields that the window copies from a grant (live_placement_controls_ui.hpp ConfirmedPositionOfGrant).
m2e::ConfirmedPosition PositionOfGrant(const LivePlacementGrant& grant) {
    m2e::ConfirmedPosition position;
    position.mapping = grant.mapping;
    position.source = SourceOf(grant.source);
    position.serial = grant.serial;
    position.condition_sha256 = grant.condition_sha256;
    if (grant.base_source) position.base_source = SourceOf(*grant.base_source);
    position.manual_entered = grant.hand_entered;
    return position;
}
}

void LivePositionThroughTheWriter(const Settings& settings, const m2e::FixedCompositeSelection& selection, const Frames& frames) {
    namespace cr = m2e::content_registration;
    using live_position::SameBits;
    const auto& condition = settings.loaded.condition;
    // The inputs that the product makes: from the plan of this condition when it has a registered context, else from the context's own constructor.
    const auto plan = std::make_shared<const FixedPreviewPlan>(PlanFixedPreviewV3(settings.loaded, m2e::MountOrientation::normal, 640, 424));
    std::optional<LiveEstimatorInputs> inputs;
    std::optional<LiveRegistrationContext> own_context;
    if (plan->live && plan->mapped) {
        const auto estimator = MakeLiveEstimator([plan] { return plan; }, kLiveTrackerDesignDraft, [&](const LiveEstimatorInputs& in) { inputs = in; return LiveEstimatorNotConnected(in); });
        const render::BgrImage frame{1, 1, {0, 0, 0}};
        LiveJob job;
        job.kind = LiveJobKind::full;
        (void)estimator(job, frame, frame);
        std::cout << "live position: the inputs come from the plan's registered context\n";
    } else {
        std::cout << "live position: the plan has no registered context (" << plan->detail << "); the inputs come from LiveRegistrationContext\n";
        const m2e::RegisteredMapping fixed{condition.base_pose, {}, std::nullopt};
        own_context.emplace(settings.loaded.profile.parameters, condition.raster, condition.seam.column, MakeFixedPreviewGrid(condition.raster, 640.0 / kWidth), 640.0 / kWidth,
            640.0 / kWidth, 640, 424, false, fixed);
        LiveEstimatorInputs in;
        in.raster = condition.raster;
        in.seam_column = condition.seam.column;
        in.model_b = &own_context->model_b;
        in.centre_a = own_context->centre_a;
        in.surface_origin_mm = own_context->origin_mm;
        inputs = in;
    }
    CHECK(inputs.has_value() && inputs->model_b != nullptr);
    if (!inputs || !inputs->model_b) return;
    const auto in = *inputs;
    const m2e::content_registration::PoseParameters pose{0.125, -0.0625, 0.1875, 0.75, -0.375, 0.25};
    const m2e::SurfacePlane estimated{1.5, 0.00390625, -0.001953125};
    const auto applied = LiveAppliedSurfaceOf(in, estimated);
    CHECK(applied.slope_x == 0 && !std::signbit(applied.slope_x) && !SameBits(applied.h0_mm, estimated.h0_mm));
    // The machine of the window, from the session to the grant of a pair capture.
    const auto grant_of = [&](const m2e::SurfacePlane& surface, bool by_hand) {
        LivePlacementMachine machine;
        machine.BeginSession(settings.loaded.file_sha256, condition.base_pose, true);
        const auto job = machine.BeginJob();
        if (!job) throw std::logic_error("the machine began no job");
        LiveEstimateOutcome outcome;
        outcome.usable = true;
        outcome.pose = pose;
        outcome.estimated_surface = estimated;
        outcome.surface = surface;
        if (machine.Accept(*job, outcome) != LivePlacementMachine::AcceptKind::pending_draw) throw std::logic_error("the outcome was not taken");
        machine.Committed();
        if (by_hand) {
            const LiveHandChecker check = [&](const m2e::RegisteredMapping& base, const m2e::HandCounts& total) {
                LiveHandCheck out;
                out.status = LiveHandCheck::Status::accepted;
                out.mapping = m2e::SolveHandStep(*in.model_b, in.centre_a, in.surface_origin_mm, cr::FrameOfRaster(in.raster), base, total, static_cast<double>(in.seam_column),
                    (static_cast<double>(in.raster.height_pixels) - 1) / 2);
                return out;
            };
            if (machine.HandStep(m2e::HandCounts{3, 0, 0}, LiveHandContext{true, true}, check).outcome != LiveHandOutcome::accepted) throw std::logic_error("the hand step was refused");
            machine.Committed();
        }
        const auto begin = machine.BeginCapture(true);
        if (!begin.grant) throw std::logic_error("the capture was given no position");
        return *begin.grant;
    };
    const auto written = [&](const char* name, const LivePlacementGrant& grant, LivePlacementSource source) {
        CHECK(grant.source == source && SameBits(grant.mapping.surface.h0_mm, applied.h0_mm) && SameBits(grant.mapping.surface.slope_y, applied.slope_y));
        auto outcome = ComposePairWithSelection(selection, RequestFor(frames, name, m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt, live_position::PositionOfGrant(grant));
        CHECK(outcome.review.has_value() && !outcome.skip);
        if (!outcome.review || !outcome.review->fixed_composite_registration) return;
        const auto text = ReadText(outcome.review->report);
        CHECK(Contains(text, "\"a0.uncalibrated-still-review.v10\""));
        const auto saved = m2e::LoadSavedStillReview(outcome.review->report);
        CHECK(saved.fixed_composite_registration.has_value() && saved.fixed_composite_registration->registration.confirmed_position.has_value());
        if (!saved.fixed_composite_registration || !saved.fixed_composite_registration->registration.confirmed_position) return;
        const auto& read = saved.fixed_composite_registration->registration;
        const auto& surface = read.confirmed_position->mapping.surface;
        std::cout << name << ": level " << m2e::GeometryLevelCode(read.level.level) << ", position h0 " << surface.h0_mm << " slope_x " << surface.slope_x << " slope_y " << surface.slope_y << '\n';
        CHECK(surface.slope_x == 0);
        CHECK(SameBits(surface.h0_mm, applied.h0_mm) && SameBits(surface.slope_y, applied.slope_y));
    };
    // (1)
    written("review-live", grant_of(applied, false), LivePlacementSource::auto_start);
    // (2)
    written("review-live-hand", grant_of(applied, true), LivePlacementSource::manual);
    // (3)
    {
        const auto grant = grant_of(estimated, false);
        CHECK(grant.mapping.surface.slope_x != 0);
        bool refused = false;
        std::string what;
        try {
            (void)ComposePairWithSelection(selection, RequestFor(frames, "review-live-estimated", m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt,
                live_position::PositionOfGrant(grant));
        } catch (const std::invalid_argument& error) { refused = true; what = error.what(); }
        std::cout << "estimated plane as the position: " << (refused ? what : std::string("not refused")) << '\n';
        CHECK(refused && what.rfind("confirmed position: ", 0) == 0 && Contains(what, "slope across the width"));
        CHECK(!fs::exists(frames.root / "review-live-estimated"));
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::invalid_argument("usage: pair_review_registration_tests <rig-profile-v2 fixture folder>");
        pair_synthetic::detail::Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        SentencesAndMachine();
        const auto base_profile = ReadText(fs::path(argv[1]) / "calibrated-draft.json");
        root = fs::current_path() / ("pair-review-registration-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);
        const auto started = Clock::now();
        const World world(17);
        const auto profile = ProfileText(base_profile, false);
        const auto settings = MakeSettings(root / "settings", profile, m2e::MountOrientation::normal, TallRaster());
        // The pair: A and B see the same drawing in the strip around the overlap.
        auto a = Flat(128), b = Flat(128);
        const auto& p = settings.loaded.profile.parameters;
        FillStrip(a, p.camera_a, 5560, kWidth, world);
        FillStrip(b, p.camera_b, 0, 1800, world);
        const auto frames = WriteFrames(root / "textured", a, b);
        std::cout << "frames written after " << Since(started) << " s\n";

        // The condition of the setup is the revision in force (the entrance SavePairReview reads).
        const auto selection = m2e::SelectFixedComposite(settings.folder / m2e::kFixedCompositeConditionName, Hash());
        CHECK(selection.loaded && selection.IsV3() && !selection.IsV2());

        // 1. The best level: the registered composite and the report v8, read back, with the sentences made from it.
        {
            const auto t = Clock::now();
            auto outcome = ComposePairWithSelection(selection, RequestFor(frames, "review-g1", m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt);
            std::cout << "full time: " << Since(t) << " s\n";
            CHECK(outcome.review.has_value() && !outcome.skip);
            if (outcome.review && outcome.review->fixed_composite_registration) {
                const auto& review = *outcome.review;
                const auto& record = *review.fixed_composite_registration;
                const auto saved = m2e::LoadSavedStillReview(review.report);
                CHECK(saved.fixed_composite_registration.has_value() && !review.fixed_composite_lens && !review.fixed_composite);
                CHECK(record.condition_sha256 == settings.loaded.file_sha256 && record.condition_sequence == 1);
                std::cout << "level " << m2e::GeometryLevelCode(record.registration.level.level) << ", estimate " << m2e::content_registration::RegistrationStateCode(record.registration.registration.state)
                          << ", gate " << m2e::registration_gate::GateStateCode(record.registration.gate.state) << ", skipped stages " << record.registration.time_budget.skipped.size() << '\n';
                CHECK(record.registration.level.level == m2e::GeometryLevel::g1);
                CHECK(record.registration.time_budget.skipped.empty());
                CHECK(record.registration.gate.state != m2e::registration_gate::GateState::not_run);
                CHECK(m2e::FixedCompositeRegistrationJson(*review.fixed_composite_registration) == m2e::FixedCompositeRegistrationJson(*saved.fixed_composite_registration));
                Sentences(review, "G1");
                const auto lines = DescribeRegistrationReview(review);
                CHECK(Contains(lines.status.back(), "警告") || Contains(lines.status[3], "警告"));
                CHECK(Contains(lines.log.back(), " skipped=none"));
            }
        }

        // 1b. A confirmed position set by hand (#437): the window gives it to the composite, the writer records it, and the log line of the capture is made from the
        // report read back from the file (never from the window's own copy). The line and the report's numbers are the same strings; the sentence says whose position it is.
        {
            m2e::ConfirmedPosition given;
            given.mapping = m2e::RegisteredMapping{m2e::content_registration::PoseParameters{0.125, -0.0625, 0.1875, 0.75, -0.375, 0.25}, m2e::SurfacePlane{1.5, 0.0, -0.001953125}, std::nullopt};
            given.source = m2e::ConfirmedPositionSource::manual;
            given.base_source = m2e::ConfirmedPositionSource::auto_button;
            given.manual_entered = m2e::HandCounts{3, 0, -1};
            given.serial = 12;
            given.condition_sha256 = settings.loaded.file_sha256;
            auto outcome = ComposePairWithSelection(selection, RequestFor(frames, "review-hand", m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt, given);
            CHECK(outcome.review.has_value() && !outcome.skip);
            if (outcome.review && outcome.review->fixed_composite_registration) {
                const auto& review = *outcome.review;
                const auto saved = m2e::LoadSavedStillReview(review.report);
                const auto text = ReadText(review.report);
                CHECK(saved.fixed_composite_registration.has_value());
                const auto& written = review.fixed_composite_registration->registration;
                std::cout << "hand position: level " << m2e::GeometryLevelCode(written.level.level) << ", difference "
                          << (written.confirmed_check && written.confirmed_check->seam_difference_px ? std::to_string(*written.confirmed_check->seam_difference_px) : std::string("none")) << '\n';
                CHECK(written.level.level == m2e::GeometryLevel::confirmed && written.confirmed_check && written.confirmed_check->used);
                CHECK(written.confirmed_check && written.confirmed_check->seam_difference_px && *written.confirmed_check->seam_difference_px > 0);   // the estimate hit, and the hand position differs from it
                if (saved.fixed_composite_registration) {
                    const auto& read = saved.fixed_composite_registration->registration;
                    CHECK(read.confirmed_position && read.level.level == m2e::GeometryLevel::confirmed && read.level.mapping.pose.tx_mm == given.mapping.pose.tx_mm);
                    // The line, from the record read back.
                    const auto line = ConfirmedPositionLogLine(*saved.fixed_composite_registration);
                    CHECK(line == ConfirmedPositionLogLine(*review.fixed_composite_registration));
                    CHECK(Contains(line, "pair_review_confirmed_position given=1 serial=12 source=manual base_source=auto-button reference_camera=CAM-A target_camera=CAM-B " + LivePlacementFields(given.mapping) +
                                             " manual_seam_dx_px=12 manual_seam_dy_px=0 manual_rotation_deg=-0.050000000000000003 condition_sha256=" + settings.loaded.file_sha256 + " condition_matches=1 used=1 seam_difference_px="));
                    CHECK(Contains(line, " level=confirmed quality_accepted=0"));
                    // The nine numbers of the line are the strings the report holds (found in the report's own text, not rebuilt from the values).
                    const auto at = text.find("\"confirmedPosition\":{");
                    CHECK(at != std::string::npos);
                    const std::pair<const char*, const char*> keys[] = {{"rxDeg", "rx_deg"}, {"ryDeg", "ry_deg"}, {"rzDeg", "rz_deg"}, {"txMm", "tx_mm"}, {"tyMm", "ty_mm"}, {"tzPercent", "tz_percent"},
                                                                         {"h0Mm", "h0_mm"}, {"slopeX", "slope_x"}, {"slopeY", "slope_y"}};
                    for (const auto& [json_key, log_key] : keys) {
                        const auto from = text.find("\"" + std::string(json_key) + "\":", at) + std::string(json_key).size() + 3;
                        const auto json_number = text.substr(from, text.find_first_of(",}", from) - from);
                        const auto log_at = line.find(" " + std::string(log_key) + "=") + std::string(log_key).size() + 2;
                        const auto log_number = line.substr(log_at, line.find(' ', log_at) - log_at);
                        CHECK(!json_number.empty() && json_number == log_number);
                    }
                    // The difference at the seam: the report's number and the log's.
                    const auto diff_json_at = text.find("\"seamDifferencePx\":", at);
                    const auto diff_from = diff_json_at + std::string("\"seamDifferencePx\":").size();
                    const auto diff_json = text.substr(diff_from, text.find_first_of(",}", diff_from) - diff_from);
                    const auto diff_log_at = line.find(" seam_difference_px=") + std::string(" seam_difference_px=").size();
                    CHECK(diff_json_at != std::string::npos && diff_json == line.substr(diff_log_at, line.find(' ', diff_log_at) - diff_log_at));
                }
                // The status row says it, the warning is a notice, and the log has the line next to the first one.
                const auto lines = DescribeRegistrationReview(review);
                bool said = false;
                for (const auto& s : lines.status) said = said || Contains(s, "手で直した位置で JPEG を作りました（自動で測ると継ぎ目で最大 ");
                CHECK(said);
                CHECK(lines.log.size() >= 3 && Contains(lines.log[1], "pair_review_confirmed_position given=1"));
                bool notice = false;
                for (const auto& w : lines.warnings) notice = notice || Contains(w, "【お知らせ】手で直した位置で作りました");
                CHECK(notice);
                for (const auto& s : lines.status) CHECK(!Contains(s, "CAM-A") && !Contains(s, "CAM-B") && !Contains(s, root.string()));
                for (const auto& s : lines.warnings) CHECK(!Contains(s, "CAM-A") && !Contains(s, "CAM-B"));
            }
            // The codes the live log and the report share are the same words.
            CHECK(m2e::ConfirmedPositionSourceCode(m2e::ConfirmedPositionSource::fixed_condition) == LivePlacementSourceCode(LivePlacementSource::fixed_condition) &&
                  m2e::ConfirmedPositionSourceCode(m2e::ConfirmedPositionSource::auto_start) == LivePlacementSourceCode(LivePlacementSource::auto_start) &&
                  m2e::ConfirmedPositionSourceCode(m2e::ConfirmedPositionSource::auto_button) == LivePlacementSourceCode(LivePlacementSource::auto_button) &&
                  m2e::ConfirmedPositionSourceCode(m2e::ConfirmedPositionSource::after_capture) == LivePlacementSourceCode(LivePlacementSource::after_capture) &&
                  m2e::ConfirmedPositionSourceCode(m2e::ConfirmedPositionSource::manual) == LivePlacementSourceCode(LivePlacementSource::manual));
            // A capture without a position says so in one line, and nothing about cameras.
            const auto none = ConfirmedPositionLogLine(m2e::SavedFixedCompositeRegistration{});
            CHECK(none == "pair_review_confirmed_position given=0 used=0 quality_accepted=0");
        }

        // 1c. A position that the live estimate decided, through the entrance and the report v10's writer.
        try {
            LivePositionThroughTheWriter(settings, selection, frames);
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAILED: exception in LivePositionThroughTheWriter: " << error.what() << '\n';
        }

        // 2. The same pair with less time: the stages the time holds are run and the others are named. The deadline is the only
        // difference; the composite and the report are still made, and the dropped stages are on the row and in the log.
        // 70 s: with the draft costs the first plan then keeps the coarse stage and the gate at most, and drops the fine stage whatever the
        // decoding took (CaptureRegistrationDesignDraft); the stages after it that the time still holds are run.
        {
            const auto t = Clock::now();
            auto outcome = ComposePairWithSelection(selection, RequestFor(frames, "review-short", m2e::MountOrientation::normal, std::chrono::seconds(70)), std::nullopt);
            std::cout << "short window time: " << Since(t) << " s\n";
            CHECK(outcome.review.has_value() && !outcome.skip);
            if (outcome.review && outcome.review->fixed_composite_registration) {
                const auto& review = *outcome.review;
                const auto& budget = review.fixed_composite_registration->registration.time_budget;
                const auto skipped = [&](m2e::ReviewStage s) { return std::any_of(budget.skipped.begin(), budget.skipped.end(), [&](const m2e::SkippedStage& k) { return k.stage == s; }); };
                std::cout << "planned " << budget.planned.size() << ", skipped " << budget.skipped.size() << ", level "
                          << m2e::GeometryLevelCode(review.fixed_composite_registration->registration.level.level) << '\n';
                CHECK(!budget.skipped.empty());
                CHECK(skipped(m2e::ReviewStage::fine_and_fit));
                CHECK(review.fixed_composite_registration->registration.level.level != m2e::GeometryLevel::g1);
                const auto saved = m2e::LoadSavedStillReview(review.report);
                CHECK(saved.fixed_composite_registration.has_value());
                Sentences(review, "short window");
                const auto lines = DescribeRegistrationReview(review);
                CHECK(lines.log.back().find(" skipped=none") == std::string::npos && Contains(lines.log.back(), "fine-and-fit"));
                bool named = false;
                for (const auto& s : lines.status) named = named || Contains(s, "時間が足りなかったため");
                CHECK(named);
            }
        }

        // 3. A condition that cannot be applied is a skip with its reason, never an error; the writer is not reached.
        {
            auto outcome = ComposePairWithSelection(m2e::FixedCompositeSelection{}, RequestFor(frames, "review-none", m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt);
            CHECK(!outcome.review && outcome.skip && *outcome.skip == m2e::FixedCompositeSkip::condition_absent && !fs::exists(frames.root / "review-none"));
            m2e::FixedCompositeSelection invalid;
            invalid.reason = m2e::FixedCompositeSkip::condition_invalid;
            outcome = ComposePairWithSelection(invalid, RequestFor(frames, "review-invalid", m2e::MountOrientation::normal, std::chrono::seconds(900)), std::nullopt);
            CHECK(!outcome.review && outcome.skip && *outcome.skip == m2e::FixedCompositeSkip::condition_invalid && !fs::exists(frames.root / "review-invalid"));
            // The other mounting: the writer refuses with the reason before it creates anything.
            outcome = ComposePairWithSelection(selection, RequestFor(frames, "review-turned", m2e::MountOrientation::rotated_180, std::chrono::seconds(900)), std::nullopt);
            CHECK(!outcome.review && outcome.skip && *outcome.skip == m2e::FixedCompositeSkip::orientation_differs && !outcome.detail.empty() && !fs::exists(frames.root / "review-turned"));
        }
        // A review without a registered record has nothing to say.
        {
            const m2e::StillReviewResult plain;
            const auto lines = DescribeRegistrationReview(plain);
            CHECK(lines.status.empty() && lines.warnings.empty() && lines.log.empty());
        }

        // 4. A composite that threw after the originals were saved (M2): what is left in the review folder, and the words for it.
        {
            const auto write = [](const fs::path& path) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << "x"; };
            const auto base = root / "cleanup";
            // A JPEG without its report is removed; the folder's other files, the `.partial` and a file beside the folder stay.
            write(base / "a" / "review" / "uncalibrated-review.jpg");
            write(base / "a" / "review" / "uncalibrated-review.json.partial");
            write(base / "a" / "original-a.jpg");
            CHECK(RemoveReportlessComposite(base / "a" / "review") == ReportlessCleanup::removed);
            CHECK(!fs::exists(base / "a" / "review" / "uncalibrated-review.jpg") && fs::exists(base / "a" / "review" / "uncalibrated-review.json.partial") && fs::exists(base / "a" / "original-a.jpg"));
            CHECK(RemoveReportlessComposite(base / "a" / "review") == ReportlessCleanup::no_composite);   // nothing left to remove
            // A JPEG with its report is a finished composite and stays.
            write(base / "b" / "review" / "uncalibrated-review.jpg");
            write(base / "b" / "review" / "uncalibrated-review.json");
            CHECK(RemoveReportlessComposite(base / "b" / "review") == ReportlessCleanup::complete_kept);
            CHECK(fs::exists(base / "b" / "review" / "uncalibrated-review.jpg") && fs::exists(base / "b" / "review" / "uncalibrated-review.json"));
            // No folder, or a folder with no composite: nothing to do, nothing created.
            CHECK(RemoveReportlessComposite(base / "absent" / "review") == ReportlessCleanup::no_composite && !fs::exists(base / "absent"));
            fs::create_directories(base / "c" / "review");
            CHECK(RemoveReportlessComposite(base / "c" / "review") == ReportlessCleanup::no_composite);
            // The composite's name taken by a directory: not a plain file, not removed (nor its contents).
            write(base / "d" / "review" / "uncalibrated-review.jpg" / "inside.txt");
            CHECK(RemoveReportlessComposite(base / "d" / "review") == ReportlessCleanup::not_removed && fs::exists(base / "d" / "review" / "uncalibrated-review.jpg" / "inside.txt"));
            // A review folder that is a link is never followed (a symbolic link needs a privilege: skipped when it cannot be made).
            write(base / "e" / "target" / "uncalibrated-review.jpg");
            std::error_code link_error;
            fs::create_directory_symlink(base / "e" / "target", base / "e" / "review", link_error);
            if (!link_error) {
                CHECK(RemoveReportlessComposite(base / "e" / "review") == ReportlessCleanup::not_removed && fs::exists(base / "e" / "target" / "uncalibrated-review.jpg"));
            } else std::cout << "symbolic link case skipped: cannot be created here\n";
        }
        {
            // The reason: memory is memory, a closed window is the time limit, anything else is an error. The exception's text is not read.
            CHECK(ClassifyCompositeFailure(std::bad_alloc(), false) == CompositeFailureReason::out_of_memory);
            CHECK(ClassifyCompositeFailure(std::bad_alloc(), true) == CompositeFailureReason::out_of_memory);
            CHECK(ClassifyCompositeFailure(std::runtime_error("review transaction deadline; files retained"), true) == CompositeFailureReason::time_limit);
            CHECK(ClassifyCompositeFailure(std::runtime_error("registered composite is not the size of the condition's raster"), false) == CompositeFailureReason::other);
            for (const auto reason : {CompositeFailureReason::time_limit, CompositeFailureReason::out_of_memory, CompositeFailureReason::other}) {
                CHECK(!CompositeFailureText(reason).empty() && !CompositeFailureCode(reason).empty() && CompositeFailureText(reason).find(':') == std::string::npos);
            }
            // No path in what is shown: a drive path (quoted or not, with spaces), a UNC path, and a filesystem_error.
            CHECK(RedactPaths("cannot open \"C:\\Users\\some name\\pair.jpg\" now") == "cannot open \"<path>\" now");
            CHECK(RedactPaths("failed at C:/Users/x/pair.jpg denied") == "failed at <path> denied");
            CHECK(RedactPaths("\\\\server\\share\\a.jpg missing").find("server") == std::string::npos);
            CHECK(RedactPaths("size mismatch 7360x4912") == "size mismatch 7360x4912");
            CHECK(RedactPaths("line one\nline two") == "line one line two");
            CHECK(RedactPaths(std::string(500, 'x')).size() <= 203);
            const std::filesystem::filesystem_error with_path("create", fs::path("C:\\Users\\private-name\\folder\\file.jpg"), std::make_error_code(std::errc::permission_denied));
            const auto summary = ErrorSummary(with_path);
            CHECK(summary.find("private-name") == std::string::npos && summary.find("C:") == std::string::npos && summary.find("filesystem error:") == 0);
            CHECK(ErrorSummary(std::runtime_error("plain message")) == "plain message");
        }

        std::cout << "checks=" << checks << " failures=" << failures << " seconds=" << Since(started) << "\n";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "exception: " << error.what() << "\n";
        return 2;
    }
}
