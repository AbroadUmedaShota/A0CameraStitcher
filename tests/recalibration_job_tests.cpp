// The job and the confirmation of the recalibration of camera B from one pair (#434 lane B; recalibration_job.hpp, recalibration_cli.hpp).
//
// The computation (the wide search, the six-degree estimate and the check on the new profile; about a minute on a real pair) is replaced by a
// stand-in that returns at once, so the rest runs in seconds: reading the condition, decoding the originals, the sample composite with the
// candidate as the selection, the classification of what was found (checked / not checked / found not to fit), the record in the staging
// folder, and the confirmation that writes into the settings folder. The frames are flat JPEGs of the full sensor size: a flat sheet cannot be
// measured, so the sample comes back "not checked" and the unchecked path (a warning, an acknowledgement) is the one that runs here.
//
// Argument 1 is the shared rig profile v2 fixture folder, 2 the StillReview executable, 3 `on` when StillReview was built with
// A0_BUILD_WPD_RECALIBRATION and `off` otherwise. Argument 4 `full` (not run by ctest) adds a whole run with the real computation on a rendered
// pair in which both cameras were moved (A and B by different amounts). The settings folders are temporary copies.
// Every range, line and tolerance below is a value of this test; the rig and the paper are synthetic.
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/fixed_composite_condition_v3.hpp"
#include "a0/m2/experimental/fixed_composite_selection.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/phase0/experimental/fixed_composite_tool_v3.hpp"
#include "a0/phase0/experimental/pair_review_synthetic.hpp"
#include "a0/phase0/experimental/recalibration_cli.hpp"
#include "a0/phase0/experimental/recalibration_job.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace a0::phase0::experimental;
namespace m2e = a0::m2::experimental;
namespace rc = a0::m2::experimental::recalibration;
namespace cr = a0::m2::experimental::content_registration;
namespace gate = a0::m2::experimental::registration_gate;
namespace render = a0::m2::render;
namespace ps = a0::phase0::experimental::pair_synthetic;
namespace fs = std::filesystem;
namespace json = a0::common::protocol_json;
using a0::m2::RigProfileV2;

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* what, int line) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAILED (line " << line << "): " << what << '\n'; }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool HasLine(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) if (Has(l, part)) return true;
    return false;
}
double Since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

const auto kNow = std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + std::chrono::hours(12)};
const m2e::FixedCompositeSha256 kHash = ps::Hash();
constexpr const char* kMeasuredAt = "2000-01-01T00:00:00Z";

std::string ReadAll(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
std::string HashOf(const std::string& text) { return kHash(std::vector<unsigned char>(text.begin(), text.end())); }

// Every file of a folder, by relative name, with its bytes: two snapshots are equal when nothing was added, removed or changed.
std::map<std::string, std::string> Snapshot(const fs::path& folder) {
    std::map<std::string, std::string> files;
    if (!fs::exists(folder)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file()) files[fs::relative(entry.path(), folder).generic_string()] = ReadAll(entry.path());
    return files;
}
// Every entry of a folder, folders included (Snapshot holds only the files): two listings are equal when nothing was added or removed.
std::vector<std::string> Listing(const fs::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) names.push_back(fs::relative(entry.path(), folder).generic_string());
    std::sort(names.begin(), names.end());
    return names;
}
fs::path root;
fs::path NewFolder(const std::string& name) {
    const auto path = root / name;
    fs::create_directories(path);
    return path;
}
void CopyFolder(const fs::path& from, const fs::path& to) { fs::copy(from, to, fs::copy_options::recursive); }

struct JsonFailure { [[noreturn]] static void Fail(std::string_view code, std::string_view) { throw std::invalid_argument("test json " + std::string(code)); } };
json::JsonValue Tree(const std::string& text) { return json::BasicJsonParser<JsonFailure>(text).Parse(); }
const json::JsonValue& At(const json::JsonValue& v, const char* key) { return v.object.at(key); }

// ---- The rig and the settings (the shared fixture, as in the recalibration test of the computation) ----
std::string fixture;
std::string RigText() {
    using ps::detail::WithMatrix;
    return WithMatrix(WithMatrix(fixture, 0, {10, 0, 100, 0, 10, 100}), 1, {10, 0, -5660, 0, 10, 100});
}
// 180 mm x 160 mm at 254 DPI: 1800 x 1600 pixels, the synthetic rig's seam column 900 (X = 650 mm).
render::OutputRaster Raster() { return ps::Raster(560000, 100000, 740000, 260000, 254); }
void TestRanges(m2e::FixedCompositeConditionV3& c) {
    c.registration.envelope = {5, 1, 0.5, 2};
    c.registration.surface_envelope = m2e::SurfaceEnvelope{10, 2};
    c.registration.limits.convergence_px = 0.05;
    c.registration.limits.minimum_unique_blocks = 2;
}
// A settings folder with the profile and revision r000001 (this test's ranges).
fs::path MakeSettings(const std::string& name) {
    const auto folder = NewFolder(name);
    auto c = ps::SampleCondition(folder, RigText(), m2e::MountOrientation::normal, Raster());
    TestRanges(c);
    m2e::ValidateFixedCompositeConditionV3(c);
    const auto revisions = folder / std::string(m2e::kFixedCompositeRevisionFolderName);
    fs::create_directories(revisions);
    m2e::WriteFixedCompositeConditionV3(revisions, c);
    return folder;
}
std::optional<m2e::LoadedFixedCompositeConditionV3> Active(const fs::path& settings) {
    return m2e::LoadActiveFixedCompositeRevisionV3(settings / std::string(m2e::kFixedCompositeRevisionFolderName), kHash);
}

render::Matrix3 MatrixOf(const render::CameraProjection& c) { return c.document_to_image; }
render::Point Project(const render::Matrix3& h, render::Point x) {
    render::Point p{};
    if (!render::TryTransform(h, x, p)) throw std::runtime_error("projection failed");
    return p;
}
// The pose of B for a camera moved by `shift_mm` along the sheet's x, found on the model.
cr::PoseParameters PoseForShift(const render::CameraProjection& camera_b, double shift_mm) {
    const cr::PoseModel model(camera_b);
    const render::Point x0{934, 236};
    const auto target = Project(MatrixOf(camera_b), {x0.x - shift_mm, x0.y});
    const auto at = [&](double tx) { return Project(model.CameraHomography(cr::PoseParameters{0, 0, 0, tx, 0, 0}), x0).x - target.x; };
    double a = 0, b = 1;
    for (int i = 0; i < 6; ++i) {
        const double fa = at(a), fb = at(b);
        if (fa == fb) break;
        const double c = b - fb * (b - a) / (fb - fa);
        a = b; b = c;
    }
    return cr::PoseParameters{0, 0, 0, b, 0, 0};
}

// The counts of an estimate whose fine stage stopped for want of points (too-few-points) on its first measurement, with nothing counted by
// the coarse stage and no vote (R = 0): the counts on which that reason alone is a lack of marks.
cr::EvidenceCounts StoppedFirstNothingCounted() {
    cr::EvidenceCounts e;
    e.rival_anchor_support = 0;
    return e;
}

// ---- The stand-in for the computation ----
enum class Fake { ready, check_not_trusted, check_doubtful, check_failed, check_out_of_range, displacement, no_overlap, band_uncovered, range_collapsed, tilt, throws, throws_memory, outside_sheet };
constexpr const char* kRecord = "{\"schema\":\"test-record\"}";
RecalibrationComputation FakeComputation(Fake mode, std::vector<std::string>* stages = nullptr) {
    RecalibrationComputation c;
    c.propose = [mode, stages](const rc::Inputs& in) {
        // The stand-in announces the stages the real computation announces; ProposeFromPose announces bake and check itself.
        if (in.progress) for (const auto s : {rc::Stage::wide_search, rc::Stage::fine_fit}) in.progress(s);
        if (stages) stages->push_back("propose");
        if (mode == Fake::throws) throw std::runtime_error("a failure inside the computation (C:\\secret\\path\\x.jpg)");
        if (mode == Fake::throws_memory) throw std::bad_alloc();
        const auto parent = RigProfileV2::Parse(in.profile_bytes).ValidateCalibrationForEvaluation(in.active.raster, render::Resampling::bilinear);
        // outside_sheet: B far enough along the sheet that the new raster leaves the profile's sheet (the real pieces refuse it).
        const auto pose = PoseForShift(parent.camera_b, mode == Fake::outside_sheet ? 500.0 : 30.0);
        const rc::ProfileFit fit{0.1, 0.3, 50};
        rc::Proposal p;
        if (mode == Fake::no_overlap) {
            // The wide search found no overlap: it ran, and gave its reason.
            p.refusals = {rc::Refusal::no_overlap};
            p.refusal_reason[rc::Refusal::no_overlap] = "insufficient-tiles";
            p.wide_performed = true;
            p.wide.determined = false;
            p.wide.reason = "insufficient-tiles";
            p.detail = "wide search: insufficient-tiles";
            return p;
        }
        if (mode == Fake::band_uncovered || mode == Fake::range_collapsed) {
            // The wide search found B (the numbers are this test's), and the raster that follows B's cover could not be made. What the
            // computation had filled by then: the raster before, the place that the wide search saw, and the rows that are missing.
            const auto& r = in.active.raster;
            p.wide_performed = true;
            p.wide.determined = true;
            p.wide.tiles_agreed = 21; p.wide.correspondences = 14;
            p.wide.pose = cr::PoseParameters{0, 0, -0.8, -3.5, 4.3, 0};
            p.wide.residual_rms_mm = 1.25; p.wide.residual_max_mm = 3.5;
            p.display.width_previous_px = r.width_pixels; p.display.height_previous_px = r.height_pixels;
            p.display.right_previous_mm = static_cast<double>(r.region_um.right) / 1000.0;
            p.display.top_previous_mm = static_cast<double>(r.region_um.top) / 1000.0; p.display.bottom_previous_mm = static_cast<double>(r.region_um.bottom) / 1000.0;
            p.display.has_start_motion = true;
            p.display.start_motion.camera_motion_mm = {-3.5, 4.3};
            p.display.start_motion.rotation_clockwise_deg = -0.8;
            if (mode == Fake::band_uncovered) {
                p.refusals = {rc::Refusal::band_uncovered};
                p.refusal_reason[rc::Refusal::band_uncovered] = "bottom";
                auto& s = p.display.target_cover_shortfall;
                s.present = true; s.at_estimate = false; s.rows_top = 0; s.rows_bottom = 26; s.top_mm = 0; s.bottom_mm = 26 * 25.4 / r.dpi;
                p.detail = "no overlap band at the start pose: seam blend needs A and B on every output row";
            } else {
                p.refusals = {rc::Refusal::range_collapsed};
                p.refusal_reason[rc::Refusal::range_collapsed] = "no-width";
                p.detail = "the new raster would have no width";
            }
            return p;
        }
        p = rc::ProposeFromPose(in, pose, fit, kRecord);   // the real pieces after the estimate: the candidate and the creation check
        // What the computation before it leaves in the proposal: the wide search's pose and the raster that the fine stage ran on.
        p.wide_performed = true; p.wide.determined = true; p.wide.tiles_agreed = 21; p.wide.correspondences = 14;
        p.wide.pose = cr::PoseParameters{0, 0, 0.8, 30.0, -1.7, 0}; p.wide.residual_rms_mm = 1.25; p.wide.residual_max_mm = 3.5;
        p.fine_raster = in.active.raster;
        if (in.progress) in.progress(rc::Stage::verify);
        p.estimate.state = cr::RegistrationState::verified;
        p.estimate.has_estimate = true;
        p.verification.performed = true;
        p.verification.limit_px = in.active.registration.limits.convergence_px;
        p.verification.state = cr::RegistrationState::verified;
        p.verification.passed = true;
        switch (mode) {
        case Fake::ready: p.ready = p.candidate.has_value() && p.refusals.empty(); break;
        case Fake::check_not_trusted:
            // What the computation does when the estimator on the new profile cannot be trusted: no candidate, one refusal.
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::reestimate_not_verified};
            p.verification.state = cr::RegistrationState::undetermined; p.verification.passed = false; p.verification.reasons = {"too-few-points"};
            p.verification.evidence = StoppedFirstNothingCounted();
            break;
        case Fake::check_doubtful:
            // Undetermined for want of marks AND for a sign that the placement does not fit: not "too few marks" alone.
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::reestimate_not_verified};
            p.verification.state = cr::RegistrationState::undetermined; p.verification.passed = false; p.verification.reasons = {"unique-blocks", "residual-shape"};
            // The blocks are those of an estimate that V2 failed on one anchor alone (a shortage), so residual-shape is the doubt that is named.
            p.verification.evidence.unique_blocks = 6; p.verification.evidence.consistent_blocks = 6;
            p.verification.evidence.anchor_blocks = 1; p.verification.evidence.consistent_anchor_blocks = 1;
            p.verification.evidence.rival_anchor_support = 0;   // every anchor voted
            break;
        case Fake::check_failed:
            // What VerifyCandidate returns when the estimator refuses its settings.
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::reestimate_not_verified};
            p.verification.state = cr::RegistrationState::undetermined; p.verification.passed = false; p.verification.reasons = {"estimate-failed"};
            break;
        case Fake::check_out_of_range:
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::reestimate_not_verified};
            p.verification.state = cr::RegistrationState::out_of_envelope; p.verification.passed = false;
            break;
        case Fake::displacement:
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::reestimate_displacement};
            p.verification.passed = false; p.verification.seam_displacement_px = 3.0;
            break;
        case Fake::tilt:
            p.candidate.reset(); p.ready = false; p.refusals = {rc::Refusal::estimate_not_verified, rc::Refusal::tilt_exceeded};
            p.estimate.state = cr::RegistrationState::out_of_envelope; p.estimate.reasons = {"residual-shape"};
            break;
        default: break;
        }
        return p;
    };
    return c;
}

// ---- The pair ----
struct Pair {ps::Frames frames; fs::path plain_a, plain_b;};
Pair MakePair(const std::string& name) {
    const auto dir = NewFolder(name);
    Pair pair;
    pair.frames = ps::WriteFrames(dir, ps::Flat(128), ps::Flat(131));
    pair.plain_a = dir / "first.jpg"; pair.plain_b = dir / "second.jpg";
    fs::copy_file(pair.frames.a, pair.plain_a); fs::copy_file(pair.frames.b, pair.plain_b);
    return pair;
}
RecalibrationJobRequest Request(const fs::path& settings, const Pair& pair, const std::string& staging, RecalibrationComputation computation, bool plain_names = false) {
    RecalibrationJobRequest r;
    r.original_a = plain_names ? pair.plain_a : pair.frames.a;
    r.original_b = plain_names ? pair.plain_b : pair.frames.b;
    r.pair_id = "pair-0042"; r.measured_at_utc = kMeasuredAt;
    r.settings_folder = settings; r.staging_folder = root / staging;
    r.hash = kHash; r.live_view_stopped = true; r.thread_count = 2; r.now = kNow;
    r.computation = std::move(computation);
    return r;
}
bool Plain(const std::string& text) {
    for (const std::string bad : {root.string(), root.generic_string(), std::string(":\\"), std::string("/Users"), std::string("CAM-A"), std::string("CAM-B"), std::string("original.jpg")})
        if (!bad.empty() && Has(text, bad)) return false;
    return true;
}
// The same check for the text of a record. A record names the cameras by their aliases, as values and as keys, so an alias that is a whole
// quoted string is not a leak there. Only that is taken out: an alias inside a longer string (a path, a file name) stays and is refused as
// before, as are the places, the paths and the file name of an original.
bool PlainRecord(const std::string& text) {
    std::string rest = text;
    for (const std::string alias : {"CAM-A", "CAM-B"}) {
        const std::string quoted = "\"" + alias + "\"";
        for (auto at = rest.find(quoted); at != std::string::npos; at = rest.find(quoted, at + 1)) rest.replace(at, quoted.size(), quoted.size(), '~');
    }
    return Plain(rest);
}
void PlainRecordSelfTest() {
    CHECK(PlainRecord(R"({"referenceCamera":"CAM-A","targetCamera":"CAM-B","originalSha256":{"CAM-A":"1","CAM-B":"2"}})"));
    CHECK(!Plain(R"({"targetCamera":"CAM-B"})"));   // the check of a screen line or a log line is the same as it was
    CHECK(!PlainRecord(R"({"x":"sample-originals/CAM-A/x"})"));
    CHECK(!PlainRecord(R"({"x":"/CAM-B/"})"));
    CHECK(!PlainRecord(R"({"x":"CAM-B\\original.jpg"})"));
    CHECK(!PlainRecord(R"({"x":"original.jpg"})"));
    CHECK(!PlainRecord(R"({"x":"C:\\a\\CAM-B\\original.jpg"})"));
    CHECK(!PlainRecord(R"({"x":"C:\\temp"})"));
    CHECK(!PlainRecord(R"({"x":"/Users/someone"})"));
    CHECK(!PlainRecord(R"({"a":"CAM-A","b":"CAM-A-2"})"));
    CHECK(!PlainRecord(R"({"a":"CAM-A","b":"CAM-B/x"})"));
    CHECK(!PlainRecord("{\"x\":\"" + root.generic_string() + "/a\"}"));
}
// The roles of a record: the reference and the target camera are named by their aliases, after the pair.
void CameraFields(const std::string& text, const json::JsonValue& record) {
    CHECK(At(record, "referenceCamera").string == "CAM-A" && At(record, "targetCamera").string == "CAM-B");
    CHECK(Has(text, "\"referenceCamera\":\"CAM-A\",\"targetCamera\":\"CAM-B\","));
    CHECK(Has(text, "\"pairId\":\"pair-0042\",") && text.find("\"pairId\"") < text.find("\"referenceCamera\""));
}
bool AllPlain(const std::vector<std::string>& lines) {
    for (const auto& l : lines) if (!Plain(l)) return false;
    return true;
}
bool OnlyClass(const RecalibrationJobOutcome& o, FindingClass k) {
    return !o.findings.empty() && std::all_of(o.findings.begin(), o.findings.end(), [&](const RecalibrationFinding& f) { return f.klass == k; });
}
bool HasCode(const RecalibrationJobOutcome& o, const std::string& code) {
    return std::any_of(o.findings.begin(), o.findings.end(), [&](const RecalibrationFinding& f) { return f.code == code; });
}

// The condition's limits of the estimator's first check (alpha 8, beta 0.3: with no outlier, V1 is passed only by 12 points or more) and of
// V2 (3 blocks). Values of this test.
cr::RegistrationLimits Limits() {
    cr::RegistrationLimits limits;
    limits.brown_lowe_alpha = 8.0;
    limits.brown_lowe_beta = 0.3;
    limits.minimum_unique_blocks = 3;
    return limits;
}
const auto kLimits = Limits();
bool Is(const std::vector<RecalibrationFinding>& findings, FindingClass klass, const char* code) {
    return findings.size() == 1 && findings[0].klass == klass && findings[0].code == code;
}

// ---- 1. The decisions, without files ----
void Decisions() {
    // Every refusal of the computation, and how it counts.
    struct Row {rc::Refusal refusal; cr::RegistrationState check; FindingClass klass;};
    const Row rows[] = {
        {rc::Refusal::no_overlap, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::band_uncovered, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::range_collapsed, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::estimate_failed, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::estimate_not_verified, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::stopped, cr::RegistrationState::verified, FindingClass::no_candidate},
        {rc::Refusal::tilt_exceeded, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::height_exceeded, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::profile_invalid, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::raster_too_large, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::raster_outside_sheet, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::creation_check_failed, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::reestimate_displacement, cr::RegistrationState::verified, FindingClass::rejected},
        {rc::Refusal::reestimate_not_verified, cr::RegistrationState::out_of_envelope, FindingClass::rejected},
        {rc::Refusal::reestimate_not_verified, cr::RegistrationState::undetermined, FindingClass::unverified},
    };
    for (const auto& row : rows) {
        rc::Proposal p;
        p.refusals = {row.refusal};
        p.verification.performed = true;
        p.verification.state = row.check;
        p.verification.reasons = {"too-few-points"};
        p.verification.evidence = StoppedFirstNothingCounted();
        const auto f = ClassifyProposal(p, kLimits);
        CHECK(f.size() == 1 && f[0].klass == row.klass && f[0].code == std::string(rc::RefusalCode(row.refusal)));
    }
    {   // The reason that comes with a refusal is the finding's reason (a code of the closed sets), or nothing.
        const struct {rc::Refusal refusal; const char* reason;} given[] = {
            {rc::Refusal::no_overlap, "tied-starts"}, {rc::Refusal::band_uncovered, "top-and-bottom"}, {rc::Refusal::range_collapsed, "no-width"}};
        for (const auto& g : given) {
            rc::Proposal p;
            p.refusals = {g.refusal};
            CHECK(ClassifyProposal(p, kLimits).front().reason.empty());
            p.refusal_reason[g.refusal] = g.reason;
            const auto f = ClassifyProposal(p, kLimits);
            CHECK(f.size() == 1 && f[0].klass == FindingClass::no_candidate && f[0].reason == g.reason && f[0].code == std::string(rc::RefusalCode(g.refusal)));
        }
        // A reason belongs to the refusal it came with: another refusal's reason is not borrowed.
        rc::Proposal p;
        p.refusals = {rc::Refusal::no_overlap, rc::Refusal::range_collapsed};
        p.refusal_reason[rc::Refusal::range_collapsed] = "no-width";
        const auto f = ClassifyProposal(p, kLimits);
        CHECK(f.size() == 2 && f[0].reason.empty() && f[1].reason == "no-width");
    }
    {   // None of the three refusals that say "nothing was made" can be confirmed: not with the warning, not on its own, not beside the one
        // refusal that can (a check that lacked marks), and not by rebuilding the candidate from a pose.
        for (const auto refusal : {rc::Refusal::no_overlap, rc::Refusal::band_uncovered, rc::Refusal::range_collapsed}) {
            rc::Proposal alone;
            alone.refusals = {refusal};
            alone.verification.performed = true; alone.verification.state = cr::RegistrationState::undetermined; alone.verification.reasons = {"too-few-points"};
            alone.verification.evidence = StoppedFirstNothingCounted();
            alone.has_pose = true; alone.estimate_record_text = kRecord;
            CHECK(!OnlyUnverifiedCheck(alone, kLimits));
            auto beside = alone;
            beside.refusals = {rc::Refusal::reestimate_not_verified, refusal};
            CHECK(!OnlyUnverifiedCheck(beside, kLimits));
            beside.refusals = {refusal, rc::Refusal::reestimate_not_verified};
            CHECK(!OnlyUnverifiedCheck(beside, kLimits));
            for (const auto policy : {UnverifiedPolicy::confirm_with_warning, UnverifiedPolicy::refuse_to_confirm}) {
                CHECK(DecideVerdict(ClassifyProposal(alone, kLimits), policy, false) == RecalibrationVerdict::unusable);
                CHECK(DecideVerdict(ClassifyProposal(beside, kLimits), policy, false) == RecalibrationVerdict::unusable);
                // Even if a candidate were there, a finding that is not "unverified" decides.
                CHECK(DecideVerdict(ClassifyProposal(alone, kLimits), policy, true) == RecalibrationVerdict::unusable);
                CHECK(DecideVerdict(ClassifyProposal(beside, kLimits), policy, true) == RecalibrationVerdict::unusable);
            }
        }
    }
    {   // The estimator's first reason sharpens the finding.
        rc::Proposal p;
        p.refusals = {rc::Refusal::estimate_not_verified};
        p.estimate.reasons = {"unique-blocks", "too-few-points"};
        CHECK(ClassifyProposal(p, kLimits).front().reason == "unique-blocks");
        rc::Proposal q;
        q.refusals = {rc::Refusal::reestimate_not_verified};
        q.verification.state = cr::RegistrationState::undetermined;
        q.verification.reasons = {"too-few-points"};
        q.verification.evidence = StoppedFirstNothingCounted();
        CHECK(ClassifyProposal(q, kLimits).front().reason == "too-few-points");
    }
    // The check on the new profile when it is undetermined: told by ALL of its reasons (the table of the ruling, one function in m2).
    {
        // u, c, a, ca, r: the block counts (unique, consistent, anchors, consistent anchors) and the vote's rival anchors that tell the two
        // sources of unique-blocks apart. r not given is "not carried" (the value EvidenceCounts starts with), which is a doubt.
        struct Case {const char* name; std::vector<std::string> reasons; std::uint32_t inliers, points; FindingClass klass; const char* code; const char* reason;
            std::uint32_t u{}, c{}, a{}, ca{}, r{std::numeric_limits<std::uint32_t>::max()};};
        const Case cases[] = {
            {"one reason of the family", {"unique-blocks"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "unique-blocks", 6, 6, 1, 1, 0},
            {"unique-blocks, the coarse stage stopped on the number of blocks", {"unique-blocks"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "unique-blocks", 2, 0, 1, 0, 0},
            {"unique-blocks, S2 and an anchor pointed to another place than the vote's winner", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks", 6, 6, 2, 2, 1},
            {"unique-blocks, S1 and an anchor pointed to another place than the vote's winner", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks", 2, 0, 2, 0, 1},
            {"unique-blocks, S2 and the rival was not carried", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks", 6, 6, 1, 1},
            {"unique-blocks, more than half of the blocks disagree", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks", 8, 3, 8, 3, 0},
            {"unique-blocks, the only anchor was dropped", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks", 6, 5, 1, 0, 0},
            {"unique-blocks, no counts carried", {"unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "unique-blocks"},
            {"every reason of the family", {"coarse-no-unique-blocks", "repeating-only", "too-few-points", "prediction-uncertain", "too-dark"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "coarse-no-unique-blocks", 0, 0, 0, 0, 0},
            {"too-few-points, nothing counted and no vote", {"too-few-points"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "too-few-points", 0, 0, 0, 0, 0},
            {"too-few-points, nothing counted and the rival not carried (counts that were not carried)", {"too-few-points"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "too-few-points"},
            {"too-few-points, the blocks that were counted agree", {"too-few-points"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "too-few-points", 8, 8, 8, 8, 0},
            {"too-few-points, the counts V2 fails on the anchors alone", {"too-few-points"}, 0, 0, FindingClass::unverified, "reestimate-not-verified", "too-few-points", 6, 6, 1, 1, 0},
            {"too-few-points, more than half of the blocks disagree", {"too-few-points"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "too-few-points", 8, 3, 8, 3, 0},
            {"too-few-points, an anchor pointed to another place", {"too-few-points"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "too-few-points", 6, 6, 2, 2, 1},
            {"too-few-points, the rival was not carried", {"too-few-points"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "too-few-points", 6, 6, 2, 2},
            {"too-few-points, an anchor was dropped", {"too-few-points"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "too-few-points", 6, 5, 2, 1, 0},
            {"a shortage and a doubt", {"unique-blocks", "residual-shape"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "residual-shape", 6, 6, 1, 1, 0},
            {"a doubt first", {"residual-shape", "unique-blocks"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "residual-shape", 6, 6, 1, 1, 0},
            {"a code nobody knows", {"a-code-nobody-knows"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "a-code-nobody-knows"},
            {"a shortage and a code nobody knows", {"unique-blocks", "a-code-nobody-knows"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "a-code-nobody-knows", 6, 6, 1, 1, 0},
            {"no reason", {}, 0, 0, FindingClass::rejected, "reestimate-doubtful", ""},
            {"the estimate failed", {"estimate-failed"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "estimate-failed"},
            {"the displacement could not be found", {"displacement-failed"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "displacement-failed"},
            {"brown-lowe, few points (the boundary: 11 points of 11 inliers)", {"brown-lowe"}, 11, 11, FindingClass::unverified, "reestimate-not-verified", "brown-lowe"},
            {"brown-lowe, few points beside a few outliers", {"brown-lowe"}, 5, 11, FindingClass::unverified, "reestimate-not-verified", "brown-lowe"},
            {"brown-lowe, the same inliers among outliers (11 of 40)", {"brown-lowe"}, 11, 40, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, 11 of 12 points: the outlier decides", {"brown-lowe"}, 11, 12, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, 12 of 12", {"brown-lowe"}, 12, 12, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, 12 of 30", {"brown-lowe"}, 12, 30, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, nothing counted", {"brown-lowe"}, 0, 0, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, no inlier", {"brown-lowe"}, 0, 5, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe, more inliers than points", {"brown-lowe"}, 11, 5, FindingClass::rejected, "reestimate-doubtful", "brown-lowe"},
            {"brown-lowe beside a shortage", {"unique-blocks", "brown-lowe"}, 5, 9, FindingClass::unverified, "reestimate-not-verified", "unique-blocks", 6, 6, 1, 1, 0},
            {"brown-lowe beside a doubt", {"brown-lowe", "starts-disagree"}, 5, 9, FindingClass::rejected, "reestimate-doubtful", "starts-disagree"},
        };
        for (const auto& c : cases) {
            rc::Proposal p;
            p.refusals = {rc::Refusal::reestimate_not_verified};
            p.verification.performed = true; p.verification.state = cr::RegistrationState::undetermined;
            p.verification.reasons = c.reasons; p.verification.inliers = c.inliers; p.verification.points = c.points;
            const auto evidence = [&] {
                cr::EvidenceCounts e;
                e.inliers = c.inliers; e.points = c.points; e.unique_blocks = c.u; e.consistent_blocks = c.c; e.anchor_blocks = c.a; e.consistent_anchor_blocks = c.ca; e.rival_anchor_support = c.r;
                return e;
            }();
            p.verification.evidence = evidence;
            const auto f = ClassifyProposal(p, kLimits);
            if (!(f.size() == 1 && f[0].klass == c.klass && f[0].code == c.code && f[0].reason == c.reason)) std::cerr << "case: " << c.name << std::endl;
            CHECK(f.size() == 1 && f[0].klass == c.klass && f[0].code == c.code && f[0].reason == c.reason);
            // The decision to rebuild and offer the calibration as unchecked is the same one.
            p.has_pose = true; p.estimate_record_text = kRecord;
            CHECK(OnlyUnverifiedCheck(p, kLimits) == (c.klass == FindingClass::unverified));
            // The sample's estimate is told by the same function: the same reasons give the same class.
            RecalibrationSampleFacts s;
            s.composed = true; s.estimate = cr::RegistrationState::undetermined; s.gate = gate::GateState::pass;
            s.estimate_reasons = c.reasons; s.estimate_evidence = evidence;
            const auto g = ClassifySample(s, kLimits);
            CHECK(g.size() == 1 && g[0].klass == c.klass && g[0].code == (c.klass == FindingClass::unverified ? "sample-estimate-undetermined" : "sample-estimate-doubtful"));
        }
        // The same inliers are read against the condition's limits: without them brown-lowe is a doubt.
        rc::Proposal p;
        p.refusals = {rc::Refusal::reestimate_not_verified};
        p.verification.performed = true; p.verification.state = cr::RegistrationState::undetermined;
        p.verification.reasons = {"brown-lowe"}; p.verification.inliers = 11; p.verification.points = 11;
        p.verification.evidence.inliers = 11; p.verification.evidence.points = 11;
        CHECK(ClassifyProposal(p, kLimits).front().klass == FindingClass::unverified);
        CHECK(ClassifyProposal(p, cr::RegistrationLimits{}).front().klass == FindingClass::rejected);
    }
    // Only a check that could not be made for want of marks, with a pose to rebuild from, is rebuilt and offered as unchecked.
    {
        rc::Proposal p;
        p.refusals = {rc::Refusal::reestimate_not_verified};
        p.verification.performed = true; p.verification.state = cr::RegistrationState::undetermined; p.verification.reasons = {"too-few-points"};
        p.verification.evidence = StoppedFirstNothingCounted();
        p.has_pose = true; p.estimate_record_text = kRecord;
        CHECK(OnlyUnverifiedCheck(p, kLimits));
        auto no_pose = p; no_pose.has_pose = false;
        auto no_record = p; no_record.estimate_record_text.clear();
        auto wrong = p; wrong.verification.state = cr::RegistrationState::out_of_envelope;
        auto doubt = p; doubt.verification.reasons = {"too-few-points", "fit-failed"};
        auto silent = p; silent.verification.reasons.clear();
        auto more = p; more.refusals.push_back(rc::Refusal::tilt_exceeded);
        auto ready = p; ready.ready = true;
        CHECK(!OnlyUnverifiedCheck(no_pose, kLimits) && !OnlyUnverifiedCheck(no_record, kLimits) && !OnlyUnverifiedCheck(wrong, kLimits) && !OnlyUnverifiedCheck(doubt, kLimits) &&
            !OnlyUnverifiedCheck(silent, kLimits) && !OnlyUnverifiedCheck(more, kLimits) && !OnlyUnverifiedCheck(ready, kLimits));
    }
    // The sample: the gate and the estimate.
    {
        RecalibrationSampleFacts s;
        s.composed = true; s.estimate = cr::RegistrationState::verified; s.gate = gate::GateState::pass;
        CHECK(ClassifySample(s, kLimits).empty());
        auto one = [&](auto edit, SuspectContentPolicy policy = kRecalibrationSuspectContentPolicy) { auto t = s; edit(t); return ClassifySample(t, kLimits, policy); };
        // A gate whose own setting is inconsistent did not check anything: it is never confirmed. One that did not run is the same, whatever
        // the cause; the cause (time-budget, stage-failed, or nothing found) is carried in the reason.
        auto f = one([](auto& t) { t.gate = gate::GateState::undetermined; t.gate_findings = {"gate-undetermined"}; });
        CHECK(Is(f, FindingClass::rejected, "gate-undetermined"));
        f = one([](auto& t) { t.gate = gate::GateState::undetermined; t.gate_findings = {"placement-unverified", "gate-undetermined"}; });
        CHECK(f.size() == 2 && f[0].klass == FindingClass::unverified && f[1].klass == FindingClass::rejected &&
            DecideVerdict(f, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::unusable);
        for (const char* why : {"time-budget", "stage-failed", ""}) {
            f = one([&](auto& t) { t.gate = gate::GateState::not_run; t.gate_skipped_reason = why; });
            CHECK(Is(f, FindingClass::rejected, "gate-not-run") && f[0].reason == why &&
                DecideVerdict(f, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::unusable);
        }
        // Only `pass` passes: a state that is not known here is not trusted.
        f = one([](auto& t) { t.gate = static_cast<gate::GateState>(99); t.gate_findings = {"nothing-measured"}; });
        CHECK(Is(f, FindingClass::rejected, "gate-unknown"));
        // What was not measured stays a doubt that a person may decide (the other states that findings explain).
        f = one([](auto& t) { t.gate = gate::GateState::undetermined; t.gate_findings = {"dx-unmeasured"}; });
        CHECK(Is(f, FindingClass::unverified, "gate-dx-unmeasured"));
        f = one([](auto& t) { t.gate = gate::GateState::fail; t.gate_cause = gate::GateCause::unmeasurable; t.gate_findings = {"nothing-measured"}; });
        CHECK(Is(f, FindingClass::unverified, "gate-nothing-measured"));
        // Over the line (an excess that was measured): by the state and the cause, whatever the findings say.
        for (const auto cause : {gate::GateCause::lift, gate::GateCause::pose, gate::GateCause::displacement, gate::GateCause::none}) {
            f = one([&](auto& t) { t.gate = gate::GateState::fail; t.gate_cause = cause; });
            CHECK(Is(f, FindingClass::rejected, "gate-fail"));
            f = one([&](auto& t) { t.gate = gate::GateState::fail; t.gate_cause = cause; t.gate_findings = {"nothing-measured"}; });
            CHECK(Is(f, FindingClass::rejected, "gate-fail"));
        }
        f = one([](auto& t) { t.estimate = cr::RegistrationState::out_of_envelope; });
        CHECK(Is(f, FindingClass::rejected, "sample-estimate-out-of-envelope"));
        // The blocks of an estimate that V2 failed on one anchor alone: a shortage of marks. More than half of the blocks in disagreement,
        // or an anchor that was dropped from the fit, is a doubt whatever the gate says.
        const auto few_anchors = [](auto& t) {
            t.estimate = cr::RegistrationState::undetermined; t.estimate_reasons = {"unique-blocks"};
            t.estimate_evidence.unique_blocks = 6; t.estimate_evidence.consistent_blocks = 6;
            t.estimate_evidence.anchor_blocks = 1; t.estimate_evidence.consistent_anchor_blocks = 1;
            t.estimate_evidence.rival_anchor_support = 0;   // every anchor voted
        };
        f = one(few_anchors);
        CHECK(Is(f, FindingClass::unverified, "sample-estimate-undetermined"));
        f = one([&](auto& t) {
            few_anchors(t);
            t.estimate_evidence.unique_blocks = 8; t.estimate_evidence.consistent_blocks = 3; t.estimate_evidence.anchor_blocks = 8; t.estimate_evidence.consistent_anchor_blocks = 3;
        });
        CHECK(Is(f, FindingClass::rejected, "sample-estimate-doubtful") && f[0].reason == "unique-blocks");
        f = one([&](auto& t) { few_anchors(t); t.estimate_evidence.consistent_anchor_blocks = 0; });
        CHECK(Is(f, FindingClass::rejected, "sample-estimate-doubtful") && f[0].reason == "unique-blocks");
        // An anchor that pointed to another place than the vote's winner: a doubt, and the same when the rival was not carried.
        f = one([&](auto& t) {
            few_anchors(t);
            t.estimate_evidence.anchor_blocks = 2; t.estimate_evidence.consistent_anchor_blocks = 2; t.estimate_evidence.rival_anchor_support = 1;
        });
        CHECK(Is(f, FindingClass::rejected, "sample-estimate-doubtful") && f[0].reason == "unique-blocks");
        f = one([&](auto& t) { few_anchors(t); t.estimate_evidence.rival_anchor_support = std::numeric_limits<std::uint32_t>::max(); });
        CHECK(Is(f, FindingClass::rejected, "sample-estimate-doubtful") && f[0].reason == "unique-blocks");
        f = one([&](auto& t) { few_anchors(t); t.gate = gate::GateState::fail; });   // a failure beside a doubt stays a failure
        CHECK(f.size() == 2 && f[1].klass == FindingClass::rejected && f[1].code == "gate-fail");
        f = one([](auto& t) { t.composed = false; t.threw = true; });
        CHECK(f.size() == 1 && f[0].code == "sample-failed" && f[0].klass == FindingClass::rejected);
        f = one([](auto& t) { t.composed = false; t.skip = m2e::FixedCompositeSkip::seam_outside_overlap; });
        CHECK(f.size() == 1 && f[0].code == "sample-skipped" && f[0].klass == FindingClass::rejected);
        f = one([](auto& t) { t.composed = false; t.out_of_memory = true; });
        CHECK(f.size() == 1 && f[0].code == "out-of-memory");
        f = one([](auto& t) { t.composed = false; });
        CHECK(f.size() == 1 && f[0].code == "sample-failed");

        // The gate, told by the codes of its own findings. A seam that could not be measured (state fail, cause unmeasurable) and a gate
        // that is undetermined are the two states that findings explain. Each code alone, then together, for both answers to Q1.
        struct GateRow {const char* symbol; FindingClass klass_reject; FindingClass klass_allow; const char* code;};
        const FindingClass R = FindingClass::rejected, U = FindingClass::unverified;
        const GateRow gate_rows[] = {
            {"nothing-measured", U, U, "gate-nothing-measured"},
            {"content-unmeasured", U, U, "gate-content-unmeasured"},
            {"dx-unmeasured", U, U, "gate-dx-unmeasured"},
            {"placement-unverified", U, U, "gate-placement-unverified"},
            {"gate-undetermined", R, R, "gate-undetermined"},
            {"unconfirmed-content", R, U, "gate-unconfirmed-content"},   // Q1: the only two codes that the owner's answer changes
            {"one-sided-content", R, U, "gate-one-sided-content"},
            {"transition-outside-band", R, R, "gate-transition-outside-band"},
            {"a-code-nobody-knows", R, R, "gate-unknown"},
        };
        const std::pair<gate::GateState, gate::GateCause> states[] = {{gate::GateState::fail, gate::GateCause::unmeasurable}, {gate::GateState::undetermined, gate::GateCause::none}};
        for (const auto& state_and_cause : states) {
            const auto state = state_and_cause.first;
            const auto cause = state_and_cause.second;
            for (const auto& row : gate_rows) {
                for (const auto policy : {SuspectContentPolicy::reject, SuspectContentPolicy::allow_with_warning}) {
                    f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; t.gate_findings = {row.symbol}; }, policy);
                    const auto expected = policy == SuspectContentPolicy::reject ? row.klass_reject : row.klass_allow;
                    if (!Is(f, expected, row.code)) std::cerr << "gate code: " << row.symbol << std::endl;
                    CHECK(Is(f, expected, row.code));
                    if (std::string(row.code) == "gate-unknown") CHECK(f[0].reason == row.symbol);
                }
            }
            // Together: one finding that is not confirmable decides the verdict; the others are still told one by one.
            for (const auto policy : {SuspectContentPolicy::reject, SuspectContentPolicy::allow_with_warning}) {
                f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; t.gate_findings = {"unconfirmed-content", "nothing-measured"}; }, policy);
                CHECK(f.size() == 2 && f[0].code == "gate-unconfirmed-content" && f[1].code == "gate-nothing-measured");
                CHECK(DecideVerdict(f, UnverifiedPolicy::confirm_with_warning, true) ==
                    (policy == SuspectContentPolicy::reject ? RecalibrationVerdict::unusable : RecalibrationVerdict::usable_unverified));
            }
            f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; t.gate_findings = {"nothing-measured", "dx-unmeasured", "placement-unverified"}; });
            CHECK(f.size() == 3 && std::all_of(f.begin(), f.end(), [](const RecalibrationFinding& x) { return x.klass == FindingClass::unverified; }) &&
                DecideVerdict(f, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::usable_unverified);
            f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; t.gate_findings = {"nothing-measured", "transition-outside-band"}; });
            CHECK(DecideVerdict(f, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::unusable);
            f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; t.gate_findings = {"nothing-measured", "nothing-measured"}; });   // a code is told once
            CHECK(Is(f, FindingClass::unverified, "gate-nothing-measured"));
            // A gate that is not `pass` and gives nothing to explain it is not trusted.
            f = one([&](auto& t) { t.gate = state; t.gate_cause = cause; });
            CHECK(Is(f, FindingClass::rejected, "gate-unknown"));
        }
        // A finding of the gate is not looked at when the gate passed or was not run (the state is the whole story there).
        f = one([](auto& t) { t.gate = gate::GateState::pass; t.gate_findings = {"unconfirmed-content"}; });
        CHECK(f.empty());
        // The default is the answer in force: Q1 is "reject" until the owner says otherwise (one constant, one place).
        CHECK(kRecalibrationSuspectContentPolicy == SuspectContentPolicy::reject);
        const RecalibrationJobRequest fresh_request;
        CHECK(fresh_request.suspect_content_policy == kRecalibrationSuspectContentPolicy);
        f = one([](auto& t) { t.gate = gate::GateState::fail; t.gate_cause = gate::GateCause::unmeasurable; t.gate_findings = {"one-sided-content"}; });
        CHECK(Is(f, kRecalibrationSuspectContentPolicy == SuspectContentPolicy::reject ? FindingClass::rejected : FindingClass::unverified, "gate-one-sided-content"));
    }
    // The verdict, for each policy. Nothing without a candidate is usable; anything found wrong is not; only a doubt follows the policy.
    {
        const RecalibrationFinding doubt{FindingClass::unverified, "gate-nothing-measured", {}}, wrong{FindingClass::rejected, "gate-fail", {}}, none{FindingClass::no_candidate, "no-overlap", {}};
        for (const auto policy : {UnverifiedPolicy::confirm_with_warning, UnverifiedPolicy::refuse_to_confirm}) {
            CHECK(DecideVerdict({}, policy, true) == RecalibrationVerdict::usable);
            CHECK(DecideVerdict({}, policy, false) == RecalibrationVerdict::unusable);
            CHECK(DecideVerdict({doubt}, policy, false) == RecalibrationVerdict::unusable);
            CHECK(DecideVerdict({wrong}, policy, true) == RecalibrationVerdict::unusable);
            CHECK(DecideVerdict({none}, policy, true) == RecalibrationVerdict::unusable);
            CHECK(DecideVerdict({doubt, wrong}, policy, true) == RecalibrationVerdict::unusable);
        }
        CHECK(DecideVerdict({doubt}, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::usable_unverified);
        CHECK(DecideVerdict({doubt, doubt}, UnverifiedPolicy::confirm_with_warning, true) == RecalibrationVerdict::usable_unverified);
        CHECK(DecideVerdict({doubt}, UnverifiedPolicy::refuse_to_confirm, true) == RecalibrationVerdict::unusable);
    }
    // The three decisions of the owner, in one place each.
    CHECK(kRecalibrationSeamPlacement == rc::SeamPlacement::band_center);
    CHECK(kRecalibrationRasterPlacement == rc::RasterPlacement::follow_camera_b);
    CHECK(kRecalibrationUnverifiedPolicy == UnverifiedPolicy::confirm_with_warning);
    const RecalibrationJobRequest fresh;
    CHECK(fresh.seam == kRecalibrationSeamPlacement && fresh.raster == kRecalibrationRasterPlacement && fresh.unverified_policy == kRecalibrationUnverifiedPolicy);
    CHECK(!fresh.live_view_stopped && fresh.sample_window == kRecalibrationSampleWindow);
}

// ---- 2. Refusals that need no sample ----
void JobRefusals(const Pair& pair) {
    const auto settings = MakeSettings("settings-refusals");
    const auto before = Snapshot(settings);
    {   // The live view must be stopped: the job does not start, and makes nothing.
        auto r = Request(settings, pair, "staging-live", FakeComputation(Fake::ready));
        r.live_view_stopped = false;
        const auto o = RunRecalibrationJob(r);
        CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && !o.candidate && HasCode(o, "live-not-stopped") && !fs::exists(r.staging_folder));
        CHECK(Has(o.screen_lines.front(), "使えません") && HasLine(o.screen_lines, "ライブを止めて"));
    }
    {   // A staging folder that exists is refused, and nothing in it is touched.
        auto r = Request(settings, pair, "staging-taken", FakeComputation(Fake::ready));
        fs::create_directories(r.staging_folder);
        { std::ofstream(r.staging_folder / "keep.txt") << "kept"; }
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "staging-exists") && ReadAll(r.staging_folder / "keep.txt") == "kept" && Snapshot(r.staging_folder).size() == 1);
    }
    {   // A staging folder inside the settings folder (a record folder among the revisions would make the reader refuse the whole row of
        // revisions) is refused before anything is made; the bytes and the listing of the settings folder stay as they were.
        const auto revisions = settings / std::string(m2e::kFixedCompositeRevisionFolderName);
        const auto listing = Listing(settings);
        const struct {const char* name; fs::path staging;} inside[] = {
            {"directly in the settings folder", settings / "staging-inside"},
            {"in the folder of the revisions", revisions / "staging-inside"},
            {"in the revisions, by a path with a parent step", revisions / ".." / std::string(m2e::kFixedCompositeRevisionFolderName) / "staging-inside"},
        };
        for (const auto& c : inside) {
            auto r = Request(settings, pair, "unused", FakeComputation(Fake::ready));
            r.staging_folder = c.staging;
            const auto o = RunRecalibrationJob(r);
            if (!(!o.ready && !o.candidate && HasCode(o, "staging-in-settings"))) std::cerr << "not refused: " << c.name << std::endl;
            CHECK(!o.ready && !o.candidate && HasCode(o, "staging-in-settings") && OnlyClass(o, FindingClass::no_candidate));
            CHECK(!fs::exists(settings / "staging-inside") && !fs::exists(revisions / "staging-inside") && HasLine(o.screen_lines, "設定のフォルダの中にあります"));
            CHECK(Snapshot(settings) == before && Listing(settings) == listing && Active(settings).has_value());
        }
        // A sibling whose name only starts the same way is not inside.
        auto sibling = Request(settings, pair, "staging-sibling", FakeComputation(Fake::no_overlap));
        sibling.staging_folder = settings.parent_path() / (settings.filename().string() + "-record");
        const auto s = RunRecalibrationJob(sibling);
        CHECK(!HasCode(s, "staging-in-settings") && HasCode(s, "no-overlap") && fs::exists(sibling.staging_folder / "recalibration-report.json"));
        // A computation that refused leaves no candidate, and the report still names the two cameras.
        { const auto text = ReadAll(sibling.staging_folder / "recalibration-report.json"); CameraFields(text, Tree(text)); CHECK(PlainRecord(text)); }
    }
    {   // No condition v3 in the settings: refused before the staging folder is made.
        const auto empty = NewFolder("settings-empty");
        auto r = Request(empty, pair, "staging-nov3", FakeComputation(Fake::ready));
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "no-v3-condition") && !fs::exists(r.staging_folder) && HasLine(o.screen_lines, "校正の元になる条件（v3）がありません"));
        // A condition of the older kind (a v1/v2 file beside the revisions) is the same: there is no v3 in force.
        const auto both = NewFolder("settings-both");
        CopyFolder(settings, both / "x");
        auto r2 = Request(both / "x", pair, "staging-nov3b", FakeComputation(Fake::ready));
        { std::ofstream(both / "x" / "wpd-fixed-composite-condition.json") << "{}"; }
        CHECK(HasCode(RunRecalibrationJob(r2), "no-v3-condition") && !fs::exists(r2.staging_folder));
    }
    {   // A hash that is not the file's.
        auto r = Request(settings, pair, "staging-hash", FakeComputation(Fake::ready));
        r.sha_a = std::string(64, 'a');
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "hash-mismatch") && fs::exists(r.staging_folder / "recalibration-report.json") && !fs::exists(r.staging_folder / "review"));
        // The job stopped before it had the second original's hash: the report still names the two cameras, and the value that is not known is empty.
        const auto text = ReadAll(r.staging_folder / "recalibration-report.json");
        const auto report = Tree(text);
        CameraFields(text, report);
        CHECK(PlainRecord(text) && At(At(report, "originalSha256"), "CAM-A").string == std::string(64, 'a') && At(At(report, "originalSha256"), "CAM-B").string.empty());
    }
    {   // A file that is no JPEG, and a JPEG of the wrong size.
        const auto dir = NewFolder("bad-originals");
        ps::Frames bad{dir / "CAM-A" / "original.jpg", dir / "CAM-B" / "original.jpg", dir};
        fs::create_directories(dir / "CAM-A"); fs::create_directories(dir / "CAM-B");
        { std::ofstream(bad.a, std::ios::binary) << "this is not a jpeg"; }
        fs::copy_file(pair.frames.b, bad.b);
        auto r = Request(settings, pair, "staging-nojpeg", FakeComputation(Fake::ready));
        r.original_a = bad.a; r.original_b = bad.b;
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "decode-failed"));
        ps::WriteJpeg(bad.a, render::BgrImage{64, 48, std::vector<std::uint8_t>(64 * 48 * 3, 90)});
        auto r2 = Request(settings, pair, "staging-small", FakeComputation(Fake::ready));
        r2.original_a = bad.a; r2.original_b = bad.b;
        CHECK(HasCode(RunRecalibrationJob(r2), "size-mismatch"));
        auto r3 = Request(settings, pair, "staging-missing", FakeComputation(Fake::ready));
        r3.original_a = dir / "nothing.jpg";
        CHECK(HasCode(RunRecalibrationJob(r3), "decode-failed"));
    }
    {   // A staging folder whose parent is not there cannot be made: an outcome (job-failed), not an exception, and no path on the screen.
        auto r = Request(settings, pair, "unused", FakeComputation(Fake::ready));
        r.staging_folder = root / "no-such-parent" / "staging-orphan";
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && !o.candidate && HasCode(o, "job-failed") && AllPlain(o.screen_lines) && AllPlain(o.log_lines) && !fs::exists(root / "no-such-parent"));
    }
    {   // A stop asked for before the computation: nothing is computed.
        std::vector<std::string> ran;
        auto r = Request(settings, pair, "staging-stop", FakeComputation(Fake::ready, &ran));
        std::stop_source source; source.request_stop(); r.stop = source.get_token();
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "stopped") && ran.empty() && HasLine(o.screen_lines, "中止しました"));
    }
    // What the computation refuses is not offered, whatever the policy; the findings say which kind of refusal it was.
    {
        const struct {Fake mode; const char* name; FindingClass klass; const char* code;} cases[] = {
            {Fake::no_overlap, "staging-noov", FindingClass::no_candidate, "no-overlap"},
            {Fake::band_uncovered, "staging-band", FindingClass::no_candidate, "band-uncovered"},
            {Fake::range_collapsed, "staging-coll", FindingClass::no_candidate, "range-collapsed"},
            {Fake::displacement, "staging-disp", FindingClass::rejected, "reestimate-displacement"},
            {Fake::check_out_of_range, "staging-oor", FindingClass::rejected, "reestimate-not-verified"},
            {Fake::check_doubtful, "staging-dbt", FindingClass::rejected, "reestimate-doubtful"},
            {Fake::check_failed, "staging-fail", FindingClass::rejected, "reestimate-doubtful"},
        };
        for (const auto& c : cases) {
            for (const auto policy : {UnverifiedPolicy::confirm_with_warning, UnverifiedPolicy::refuse_to_confirm}) {
                auto r = Request(settings, pair, std::string(c.name) + (policy == UnverifiedPolicy::confirm_with_warning ? "-c" : "-r"), FakeComputation(c.mode));
                r.unverified_policy = policy;
                const auto o = RunRecalibrationJob(r);
                CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && !o.candidate && HasCode(o, c.code));
                CHECK(std::any_of(o.findings.begin(), o.findings.end(), [&](const RecalibrationFinding& f) { return f.code == c.code && f.klass == c.klass; }));
                CHECK(!fs::exists(r.staging_folder / "review") && !fs::exists(r.staging_folder / "candidate-revision.json") && fs::exists(r.staging_folder / "recalibration-report.json"));
                CHECK(o.screen_lines.front() == "この校正は使えません");
                // Not usable is not confirmable, even with the acknowledgement.
                const auto confirmed = ConfirmRecalibration(o, settings, kHash, {true});
                CHECK(!confirmed.written && confirmed.reason == "not-usable");
            }
        }
        {   // A check that was undetermined for a reason beyond "too few marks" says so, and is told apart from the checks that only lacked marks.
            auto d = Request(settings, pair, "staging-dbt-lines", FakeComputation(Fake::check_doubtful));
            const auto o = RunRecalibrationJob(d);
            CHECK(HasLine(o.screen_lines, "合っていない疑いが見つかりました") && HasLine(o.screen_lines, "浮いているか") && !HasLine(o.screen_lines, "確かな値になりませんでした") && !o.ready);
            CHECK(std::any_of(o.findings.begin(), o.findings.end(), [](const RecalibrationFinding& f) { return f.code == "reestimate-doubtful" && f.reason == "residual-shape"; }));
            CHECK(HasLine(o.log_lines, "recalibration_finding class=rejected code=reestimate-doubtful reason=residual-shape"));
            // The record of the run carries the state and the counts that the decision was made from.
            const auto report = Tree(ReadAll(d.staging_folder / "recalibration-report.json"));
            const auto& verification = At(report, "verification");
            CHECK(At(verification, "state").string == "undetermined" && At(verification, "inliers").kind == json::JsonKind::number && At(verification, "points").kind == json::JsonKind::number);
            CHECK(At(report, "verdict").string == "unusable" && !ConfirmRecalibration(o, settings, kHash, {true}).written);
        }
        auto r = Request(settings, pair, "staging-tilt", FakeComputation(Fake::tilt));
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "tilt-exceeded") && HasCode(o, "estimate-not-verified") && HasLine(o.screen_lines, "チャートでの校正が要ります"));
        // The estimator's own reason (a sheet that is not flat) is a symptom of the tilt here: the line says what comes first, and does not send
        // the person to the paper. The reason itself stays in the finding and in the record.
        CHECK(HasLine(o.screen_lines, "細かい合わせも確かな値になりませんでした") && !HasLine(o.screen_lines, "浮いている") && !HasLine(o.screen_lines, "机に直に"));
        CHECK(std::any_of(o.findings.begin(), o.findings.end(), [](const RecalibrationFinding& f) { return f.code == "estimate-not-verified" && f.reason == "residual-shape"; }));
        CHECK(HasLine(o.log_lines, "recalibration_finding class=no-candidate code=estimate-not-verified reason=residual-shape"));
    }
    {   // A failure inside the computation (also memory) is an outcome, not an exception; the text of the failure does not reach the screen.
        auto r = Request(settings, pair, "staging-throw", FakeComputation(Fake::throws));
        const auto o = RunRecalibrationJob(r);
        CHECK(!o.ready && HasCode(o, "job-failed") && AllPlain(o.screen_lines) && AllPlain(o.log_lines) && !HasLine(o.screen_lines, "secret") && !HasLine(o.log_lines, "secret"));
        auto r2 = Request(settings, pair, "staging-oom", FakeComputation(Fake::throws_memory));
        const auto m = RunRecalibrationJob(r2);
        CHECK(!m.ready && HasCode(m, "out-of-memory") && HasLine(m.screen_lines, "メモリが足りませんでした"));
    }
    {   // Mistakes of the caller are exceptions.
        auto r = Request(settings, pair, "staging-bad", FakeComputation(Fake::ready));
        auto no_hash = r; no_hash.hash = nullptr;
        auto bad_time = r; bad_time.measured_at_utc = "yesterday";
        auto no_path = r; no_path.staging_folder.clear();
        for (const auto* request : {&no_hash, &bad_time, &no_path}) {
            bool threw = false;
            try { (void)RunRecalibrationJob(*request); } catch (const std::invalid_argument&) { threw = true; }
            CHECK(threw);
        }
        CHECK(!fs::exists(r.staging_folder));
    }
    // None of the above wrote into the settings folder.
    CHECK(Snapshot(settings) == before);
}

// ---- The reasons that come with a refusal: on the screen, in the log, in the report and on the command line ----
double NumberOf(const json::JsonValue& v) { return std::stod(v.string); }
bool IsNull(const json::JsonValue& v) { return v.kind == json::JsonKind::null_value; }
// Whether the first occurrences of `names`, each searched from where the one before was found, come in this order.
bool InOrder(const std::string& text, std::initializer_list<const char*> names, std::size_t from = 0) {
    std::size_t at = from;
    for (const char* name : names) {
        const auto found = text.find(name, at);
        if (found == std::string::npos) { std::cerr << "not in order: " << name << std::endl; return false; }
        at = found + 1;
    }
    return true;
}
void SeparateReasons(const Pair& pair) {
    const auto settings = MakeSettings("settings-reasons");
    const auto before = Snapshot(settings);
    const auto wide = [](const fs::path& p) { return p.wstring(); };
    struct Row {Fake mode; const char* name; const char* code; const char* reason; const char* said; const char* not_said;};
    const Row rows[] = {
        {Fake::no_overlap, "staging-r-noov", "no-overlap", "insufficient-tiles", "2 台の写真に、重なっている所が見つかりません", "上下にずれて"},
        {Fake::band_uncovered, "staging-r-band", "band-uncovered", "bottom", "合成の範囲の下の端を B が写していません", "重なっている所が見つかりません"},
        {Fake::range_collapsed, "staging-r-coll", "range-collapsed", "no-width", "合成の範囲の幅が無くなります", "重なっている所が見つかりません"},
    };
    for (const auto& row : rows) {
        for (const auto policy : {UnverifiedPolicy::confirm_with_warning, UnverifiedPolicy::refuse_to_confirm}) {
            auto r = Request(settings, pair, std::string(row.name) + (policy == UnverifiedPolicy::confirm_with_warning ? "-c" : "-r"), FakeComputation(row.mode));
            r.unverified_policy = policy;
            const auto o = RunRecalibrationJob(r);
            CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && !o.candidate && o.findings.size() == 1 && OnlyClass(o, FindingClass::no_candidate));
            CHECK(o.findings.size() == 1 && o.findings[0].code == row.code && o.findings[0].reason == row.reason);
            // The screen says what happened in words (the code and the reason stay off it), and not what another refusal would say.
            CHECK(o.screen_lines.size() >= 2 && o.screen_lines.front() == "この校正は使えません" && Has(o.screen_lines[1], row.said));
            CHECK(!HasLine(o.screen_lines, row.not_said) && !HasLine(o.screen_lines, row.code) && !HasLine(o.screen_lines, row.reason));
            CHECK(HasLine(o.log_lines, std::string("recalibration_finding class=no-candidate code=") + row.code + " reason=" + row.reason));
            CHECK(AllPlain(o.screen_lines) && AllPlain(o.log_lines));
            // Whatever the policy and whatever the person acknowledges, none of the three is confirmed, and nothing is written.
            const auto confirmed = ConfirmRecalibration(o, settings, kHash, {true});
            CHECK(!confirmed.written && confirmed.reason == "not-usable");
            CHECK(!fs::exists(r.staging_folder / "review") && !fs::exists(r.staging_folder / "candidate-revision.json") && fs::exists(r.staging_folder / "recalibration-report.json"));
            const auto text = ReadAll(r.staging_folder / "recalibration-report.json");
            const auto report = Tree(text);
            CHECK(PlainRecord(text) && At(report, "verdict").string == "unusable" && !At(report, "unverified").boolean);
            const auto& findings = At(report, "findings").array;
            CHECK(findings.size() == 1 && At(findings[0], "class").string == "no-candidate" && At(findings[0], "code").string == row.code && At(findings[0], "reason").string == row.reason);
        }
    }
    CHECK(Snapshot(settings) == before);

    // The record of a wide search that found nothing: it ran, it says why, and every number it could not give is null. Nothing after it ran.
    {
        auto r = Request(settings, pair, "staging-r-noov-report", FakeComputation(Fake::no_overlap));
        (void)RunRecalibrationJob(r);
        const auto report = Tree(ReadAll(r.staging_folder / "recalibration-report.json"));
        const auto& search = At(report, "wideSearch");
        CHECK(search.kind == json::JsonKind::object && !At(search, "determined").boolean && At(search, "reason").string == "insufficient-tiles");
        CHECK(NumberOf(At(search, "tilesAgreed")) == 0 && NumberOf(At(search, "correspondences")) == 0);
        for (const char* key : {"txMm", "tyMm", "rzDeg", "residualRmsMm", "residualMaxMm"}) CHECK(IsNull(At(search, key)));
        CHECK(IsNull(At(report, "fineFitRaster")));
        const auto& display = At(report, "display");
        CHECK(!At(display, "hasStartMotion").boolean && IsNull(At(display, "startMotionRightMm")) && IsNull(At(display, "startMotionDownMm")) && IsNull(At(display, "startRotationClockwiseDeg")));
        CHECK(!At(display, "hasNew").boolean && IsNull(At(display, "heightNewPx")) && IsNull(At(display, "topNewMm")) && IsNull(At(display, "bottomNewMm")) && IsNull(At(display, "targetCoverShortfall")));
    }
    // A raster that B does not cover below: the wide search's pose is in the record and on the screen as a guide, the rows that are missing
    // are counted, and the fine stage did not run.
    {
        auto r = Request(settings, pair, "staging-r-band-report", FakeComputation(Fake::band_uncovered));
        const auto o = RunRecalibrationJob(r);
        const auto report = Tree(ReadAll(r.staging_folder / "recalibration-report.json"));
        const auto& search = At(report, "wideSearch");
        CHECK(At(search, "determined").boolean && At(search, "reason").string.empty() && NumberOf(At(search, "tilesAgreed")) == 21 && NumberOf(At(search, "correspondences")) == 14);
        CHECK(NumberOf(At(search, "txMm")) == -3.5 && NumberOf(At(search, "tyMm")) == 4.3 && NumberOf(At(search, "rzDeg")) == -0.8 &&
            NumberOf(At(search, "residualRmsMm")) == 1.25 && NumberOf(At(search, "residualMaxMm")) == 3.5);
        CHECK(IsNull(At(report, "fineFitRaster")));
        const auto& display = At(report, "display");
        CHECK(!At(display, "hasMotion").boolean && At(display, "hasStartMotion").boolean);
        CHECK(NumberOf(At(display, "startMotionRightMm")) == -3.5 && NumberOf(At(display, "startMotionDownMm")) == 4.3 && NumberOf(At(display, "startRotationClockwiseDeg")) == -0.8);
        const auto& shortfall = At(display, "targetCoverShortfall");
        CHECK(shortfall.kind == json::JsonKind::object && shortfall.object.size() == 5 && At(shortfall, "pose").string == "start" && NumberOf(At(shortfall, "rowsTop")) == 0 &&
            NumberOf(At(shortfall, "rowsBottom")) == 26 && NumberOf(At(shortfall, "topMm")) == 0 && std::abs(NumberOf(At(shortfall, "bottomMm")) - 2.6) < 1e-9);
        CHECK(!At(display, "hasNew").boolean && IsNull(At(display, "heightNewPx")) && IsNull(At(display, "topNewMm")) && IsNull(At(display, "bottomNewMm")) && IsNull(At(display, "widthNewPx")));
        CHECK(NumberOf(At(display, "heightPreviousPx")) == 1600 && NumberOf(At(display, "topPreviousMm")) == 100 && NumberOf(At(display, "bottomPreviousMm")) == 260);
        // The screen: the guide of the wide search stands in the place of the line that the fine stage's pose would have, and the rows are said.
        CHECK(HasLine(o.screen_lines, "大まかな探索で見た、A に対する B の位置の変化（目安。傾きと高さは測っていません）: 図面の左へ約 3.5 mm、上下は下へ約 4.3 mm、回り 約 0.8°（画面で見て反時計回り）"));
        CHECK(HasLine(o.screen_lines, "B が写っていない所: 合成の範囲の下の端から約 2.6 mm") && !HasLine(o.screen_lines, "求められませんでした") && !HasLine(o.screen_lines, "合成の画像の大きさ"));
        CHECK(HasLine(o.screen_lines, "「位置の変化」の行"));   // the sentence points at the line above, which is there
        // The same, with the cameras' own numbers, on the command line.
        const std::vector<std::wstring> args{L"--camera-a", wide(pair.frames.a), L"--camera-b", wide(pair.frames.b), L"--settings-folder", wide(settings), L"--staging", wide(root / "staging-r-band-cli")};
        std::ostringstream out;
        CHECK(RunRecalibrationCommand(args, out, kHash, kNow, FakeComputation(Fake::band_uncovered)) == 1);
        const auto text = out.str();
        CHECK(Has(text, "finding class=no-candidate code=band-uncovered reason=bottom"));
        CHECK(Has(text, "wide determined=1 reason=none tiles_agreed=21 correspondences=14 tx_mm=-3.5 ty_mm=4.3 rz_deg=-0.8 residual_rms_mm=1.25"));
        CHECK(Has(text, "start_motion right_mm=-3.5 down_mm=4.3 rotation_clockwise_deg=-0.8"));
        CHECK(Has(text, "target_cover_shortfall pose=start rows_top=0 rows_bottom=26 top_mm=0 bottom_mm=2.6"));
        CHECK(!Has(text, "fine_fit_raster") && !Has(text, "seam previous_mm=") && !Has(text, "motion right_mm=-3.5 down_mm=4.3 rotation_clockwise_deg=-0.8 tilt"));
        CHECK(Plain(text));
    }
    // A raster with no width: the same record without the rows.
    {
        auto r = Request(settings, pair, "staging-r-coll-report", FakeComputation(Fake::range_collapsed));
        const auto o = RunRecalibrationJob(r);
        const auto report = Tree(ReadAll(r.staging_folder / "recalibration-report.json"));
        CHECK(At(At(report, "wideSearch"), "determined").boolean && IsNull(At(report, "fineFitRaster")) && IsNull(At(At(report, "display"), "targetCoverShortfall")));
        CHECK(HasLine(o.screen_lines, "大まかな探索で見た、A に対する B の位置の変化") && !HasLine(o.screen_lines, "B が写っていない所"));
    }
    // The wide search that found nothing, on the command line.
    {
        const std::vector<std::wstring> args{L"--camera-a", wide(pair.frames.a), L"--camera-b", wide(pair.frames.b), L"--settings-folder", wide(settings), L"--staging", wide(root / "staging-r-noov-cli")};
        std::ostringstream out;
        CHECK(RunRecalibrationCommand(args, out, kHash, kNow, FakeComputation(Fake::no_overlap)) == 1);
        const auto text = out.str();
        CHECK(Has(text, "finding class=no-candidate code=no-overlap reason=insufficient-tiles"));
        CHECK(Has(text, "wide determined=0 reason=insufficient-tiles tiles_agreed=0 correspondences=0 tx_mm=none ty_mm=none rz_deg=none residual_rms_mm=none"));
        CHECK(!Has(text, "start_motion") && !Has(text, "target_cover_shortfall") && !Has(text, "fine_fit_raster") && Plain(text));
    }
    CHECK(Snapshot(settings) == before);
}

// A stop asked for after the computation is honoured at the next step: the outcome is "stopped", nothing is offered, and nothing that a
// confirmation would use is written (the settings folder is untouched, the staging folder holds no candidate and no sample).
void StopAfterTheComputation(const Pair& pair) {
    const auto settings = MakeSettings("settings-stop-late");
    const auto before = Snapshot(settings);
    {   // The stop comes when the computation returns: the proposal is dropped before the rebuild, the record and the sample.
        std::stop_source source;
        auto computation = FakeComputation(Fake::ready);
        const auto inner = computation.propose;
        computation.propose = [inner, &source](const rc::Inputs& in) { auto proposal = inner(in); source.request_stop(); return proposal; };
        auto r = Request(settings, pair, "staging-stop-late", computation);
        r.stop = source.get_token();
        std::vector<std::string> stages;
        r.on_stage = [&](RecalibrationStage s, double) { stages.emplace_back(RecalibrationStageCode(s)); };
        const auto o = RunRecalibrationJob(r);
        CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && !o.candidate && o.findings.size() == 1 && HasCode(o, "stopped"));
        // The numbers of the dropped proposal are not shown (the lines that remain are the "not found" lines of an empty result).
        CHECK(o.screen_lines.front() == "この校正は使えません" && HasLine(o.screen_lines, "中止しました") && !HasLine(o.screen_lines, "の位置の変化") && !HasLine(o.screen_lines, "画素"));
        CHECK(!o.facts.display.has_motion && !o.facts.display.has_new && !o.facts.gate.known && !o.facts.creation_checked);
        CHECK(std::find(stages.begin(), stages.end(), "sample") == stages.end() && std::find(stages.begin(), stages.end(), "crops") == stages.end());
        CHECK(Snapshot(settings) == before);
        const auto staged = Snapshot(r.staging_folder);
        CHECK(staged.size() == 1 && staged.count("recalibration-report.json") == 1);   // only the record of the run itself
        CHECK(!fs::exists(r.staging_folder / "review") && !fs::exists(r.staging_folder / "sample-originals"));
        const auto confirmed = ConfirmRecalibration(o, settings, kHash, {true});
        CHECK(!confirmed.written && confirmed.reason == "not-usable" && Snapshot(settings) == before);
    }
    {   // The stop comes while the sample is composed: the crops are not made and nothing is offered.
        std::stop_source source;
        auto r = Request(settings, pair, "staging-stop-sample", FakeComputation(Fake::ready));
        r.stop = source.get_token();
        std::vector<std::string> stages;
        r.on_stage = [&](RecalibrationStage s, double) { stages.emplace_back(RecalibrationStageCode(s)); if (s == RecalibrationStage::sample) source.request_stop(); };
        const auto o = RunRecalibrationJob(r);
        CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && !o.candidate && HasCode(o, "stopped") && o.findings.size() == 1);
        CHECK(std::find(stages.begin(), stages.end(), "crops") == stages.end() && !fs::exists(r.staging_folder / "seam-crops.png") && !o.seam_crops);
        CHECK(Snapshot(settings) == before);
        CHECK(!ConfirmRecalibration(o, settings, kHash, {true}).written);
    }
}

// A refusal that comes before the new plan is whole (the new raster leaves the sheet) must not put the "after" side on the screen, in the
// report or on the command line: the width is known, the seam and the height are not.
void RefusedBeforeThePlan(const Pair& pair) {
    const auto settings = MakeSettings("settings-outside");
    const auto before = Snapshot(settings);
    auto r = Request(settings, pair, "staging-outside", FakeComputation(Fake::outside_sheet));
    const auto o = RunRecalibrationJob(r);
    CHECK(!o.ready && !o.candidate && HasCode(o, "raster-outside-sheet") && o.verdict == RecalibrationVerdict::unusable);
    CHECK(!o.facts.display.has_new && o.facts.display.has_motion);
    CHECK(!HasLine(o.screen_lines, "継ぎ目の位置") && !HasLine(o.screen_lines, "合成の画像の大きさ") && !HasLine(o.screen_lines, "画素") && HasLine(o.screen_lines, "A に対する B の位置の変化"));
    const auto report = Tree(ReadAll(r.staging_folder / "recalibration-report.json"));
    const auto& display = At(report, "display");
    CHECK(!At(display, "hasNew").boolean && At(display, "seamXNewMm").kind == json::JsonKind::null_value && At(display, "widthNewPx").kind == json::JsonKind::null_value &&
        At(display, "rightNewMm").kind == json::JsonKind::null_value && At(display, "seamXPreviousMm").kind == json::JsonKind::number);
    CHECK(Snapshot(settings) == before);
    // The command line prints the same facts: no line for the new seam and the new raster.
    const auto wide = [](const fs::path& p) { return p.wstring(); };
    const std::vector<std::wstring> args{L"--camera-a", wide(pair.frames.a), L"--camera-b", wide(pair.frames.b), L"--settings-folder", wide(settings), L"--staging", wide(root / "staging-outside-cli")};
    std::ostringstream out;
    CHECK(RunRecalibrationCommand(args, out, kHash, kNow, FakeComputation(Fake::outside_sheet)) == 1);
    const auto text = out.str();
    CHECK(Has(text, "code=raster-outside-sheet") && Has(text, "motion right_mm=") && !Has(text, "seam previous_mm=") && !Has(text, "raster_width_px") && !Has(text, "screen 合成の画像の大きさ"));
    CHECK(Snapshot(settings) == before);
}

// A hand adjustment beyond the range (pose_outside_envelope): the reader refuses it ("roll back, or calibrate again"); the calibration may start from it
// (#434). The recalibrated revision has base pose 0 and ends the state. A chain that cannot be read at all is refused as before.
void OutsideEnvelope(const Pair& pair) {
    const auto settings = MakeSettings("settings-outside-range");
    const m2e::LayerTwoStrainDesignV3 layer{64.0, 0.5};
    FixedConditionV3Request beyond;
    beyond.settings_folder = settings; beyond.kind = m2e::FixedCompositeRevisionKind::manual_adjustment; beyond.layer_two = layer;
    beyond.entered = m2e::FixedCompositeRevisionChangeV3::Entered{120.0, 0.0, 0.0};   // 12 mm at 254 DPI: beyond the 5 mm of this test's range
    const auto written = MakeFixedCompositeConditionV3File(beyond, kHash, kNow);
    CHECK(written.made && written.outside_envelope);
    const auto selection = m2e::SelectFixedComposite(settings / m2e::kFixedCompositeConditionName, kHash);
    CHECK(!selection.loaded && selection.reason == m2e::FixedCompositeSkip::pose_outside_envelope);
    const auto active = Active(settings);
    CHECK(active && active->condition.revision.sequence == 2 && active->condition.revision.change.kind == m2e::FixedCompositeRevisionKind::manual_adjustment);
    if (!active) return;
    const auto before = Snapshot(settings);

    auto request = Request(settings, pair, "staging-outside-range", FakeComputation(Fake::ready));
    const auto o = RunRecalibrationJob(request);
    // It went on: the parent is the revision beyond the range, the new revision is number 3, nothing was written into the settings.
    CHECK(!HasCode(o, "no-v3-condition") && o.candidate.has_value() && o.sequence == 3 && o.parent_sha256 == active->file_sha256);
    CHECK(o.candidate && o.candidate->condition.revision.parent_sha256 == std::optional<std::string>(active->file_sha256) && o.candidate->condition.base_pose.IsZero() &&
        o.candidate->condition.revision.change.kind == m2e::FixedCompositeRevisionKind::recalibrated);
    CHECK(Snapshot(settings) == before && HasLine(o.log_lines, "recalibration_condition_outside_envelope=1"));
    // The changes are counted from the profile's own placement: the same 30 mm as for a calibration that was never adjusted, and the screen says so.
    CHECK(o.facts.display.previous_outside_envelope && o.facts.display.has_motion && std::abs(o.facts.display.motion.camera_motion_mm.x - 30.0) < 0.5);
    CHECK(HasLine(o.screen_lines, "前の手直しは範囲の外だったため、校正の位置からの変化を出します") && HasLine(o.screen_lines, "A に対する B の位置の変化: 図面の右へ約 "));
    CHECK(AllPlain(o.screen_lines) && AllPlain(o.log_lines));
    // The usual run does not say it.
    const auto plain_settings = MakeSettings("settings-in-range");
    const auto usual = RunRecalibrationJob(Request(plain_settings, pair, "staging-in-range", FakeComputation(Fake::ready)));
    CHECK(usual.candidate && !usual.facts.display.previous_outside_envelope && !HasLine(usual.screen_lines, "前の手直しは範囲の外だった") && !HasLine(usual.log_lines, "outside_envelope"));
    // Confirmed (an unchecked calibration, with the acknowledgement): revision 3 is written, and the reader that refused revision 2 reads it, inside the range.
    CHECK(ConfirmRecalibration(o, settings, kHash, {}).reason == "not-acknowledged");
    const auto confirmed = ConfirmRecalibration(o, settings, kHash, {true});
    CHECK(confirmed.written && confirmed.sequence == 3);
    const auto after = m2e::SelectFixedComposite(settings / m2e::kFixedCompositeConditionName, kHash);
    const auto* v3 = after.loaded ? std::get_if<m2e::LoadedFixedCompositeConditionV3>(&*after.loaded) : nullptr;
    CHECK(after.IsV3() && v3 && v3->condition.revision.sequence == 3 && v3->file_sha256 == confirmed.revision_sha256 && v3->condition.base_pose.IsZero());

    // What cannot be read at all is refused as before: a file in the revisions folder that is not a revision, a profile that is gone, a v1/v2 file beside.
    const auto revisions = std::string(m2e::kFixedCompositeRevisionFolderName);
    const auto broken = [&](const char* name, const std::function<void(const fs::path&)>& spoil) {
        const auto copy = root / name;
        CopyFolder(settings, copy);
        fs::remove(copy / revisions / "r000003.json");   // back to the revision beyond the range, in force again
        CHECK(m2e::SelectFixedComposite(copy / m2e::kFixedCompositeConditionName, kHash).reason == m2e::FixedCompositeSkip::pose_outside_envelope);
        spoil(copy);
        const auto snapshot = Snapshot(copy);
        auto r = Request(copy, pair, std::string("staging-") + name, FakeComputation(Fake::ready));
        const auto refused = RunRecalibrationJob(r);
        CHECK(!refused.ready && !refused.candidate && HasCode(refused, "no-v3-condition") && !fs::exists(r.staging_folder) && Snapshot(copy) == snapshot);
    };
    broken("settings-outside-foreign", [&](const fs::path& copy) { std::ofstream(copy / revisions / "notes.txt") << "x"; });
    broken("settings-outside-noprofile", [&](const fs::path& copy) {
        const auto name = Active(copy)->condition.source.profile_file;
        fs::remove(copy / name);
    });
    broken("settings-outside-v2", [&](const fs::path& copy) { std::ofstream(copy / "wpd-fixed-composite-condition.json") << "{}"; });
}

// ---- 3. A whole job with a sample, and the confirmation ----
void Reports(const RecalibrationJobOutcome& o, const fs::path& staging, const std::string& verdict, const std::string& policy, const Pair& pair) {
    const auto report = Tree(ReadAll(staging / "recalibration-report.json"));
    CHECK(At(report, "schema").string == "a0.pair-recalibration-report.v1" && At(report, "pairId").string == "pair-0042");
    {
        const auto text = ReadAll(staging / "recalibration-report.json");
        CameraFields(text, report);
        // The hashes of the two originals, keyed by alias, in the alias order.
        const auto& originals = At(report, "originalSha256");
        CHECK(originals.kind == json::JsonKind::object && originals.object.size() == 2);
        CHECK(At(originals, "CAM-A").string == HashOf(ReadAll(pair.frames.a)) && At(originals, "CAM-B").string == HashOf(ReadAll(pair.frames.b)));
        CHECK(Has(text, "\"originalSha256\":{\"CAM-A\":\"" + HashOf(ReadAll(pair.frames.a)) + "\",\"CAM-B\":\"" + HashOf(ReadAll(pair.frames.b)) + "\"}"));
        // The distances of the seam to the photographs' edges, keyed by alias; no field names a camera by a letter.
        const auto& share = At(At(report, "display"), "edgeShare");
        CHECK(share.kind == json::JsonKind::object && share.object.size() == 2);
        CHECK(std::abs(std::stod(At(share, "CAM-A").string) - o.facts.display.edge_share_a) < 1e-6 && std::abs(std::stod(At(share, "CAM-B").string) - o.facts.display.edge_share_b) < 1e-6);
        CHECK(Has(text, "\"edgeShare\":{\"CAM-A\":") && Has(text, ",\"CAM-B\":"));
        for (const char* old : {"\"originals\"", "cameraASha256", "cameraBSha256", "edgeShareA", "edgeShareB"}) CHECK(!Has(text, old));
    }
    CHECK(At(report, "verdict").string == verdict && At(report, "unverifiedPolicy").string == policy);
    CHECK(At(report, "unverified").boolean == (verdict == "usable-unverified"));
    CHECK(At(report, "findings").array.size() == o.findings.size());
    CHECK(At(report, "parent").object.at("revisionSha256").string == o.parent_sha256);
    CHECK(At(At(report, "sample"), "gateKnown").boolean == o.facts.gate.known);
    if (o.facts.gate.known) CHECK(At(At(report, "sample"), "gateState").string == std::string(gate::GateStateCode(o.facts.gate.state)));
    if (o.candidate) CHECK(At(At(report, "candidate"), "revisionSha256").string == o.candidate->sha256);
    // The report holds codes and numbers: no path, no machine identifier, no sentence.
    const auto text = ReadAll(staging / "recalibration-report.json");
    CHECK(PlainRecord(text) && !Has(text, "使えません") && !Has(text, "未確認"));
    // The members that were there are where they were, in their order, and the new ones stand beside the ones they belong to.
    CHECK(InOrder(text, {"\"schema\":", "\"pairId\":", "\"referenceCamera\":", "\"targetCamera\":", "\"verdict\":", "\"unverified\":", "\"unverifiedPolicy\":", "\"findings\":",
        "\"originalSha256\":", "\"parent\":", "\"candidate\":", "\"wideSearch\":", "\"fineFitRaster\":", "\"estimate\":{", "\"sample\":", "\"display\":"}));
    const auto display_at = text.find("\"display\":");
    CHECK(display_at != std::string::npos && InOrder(text, {"\"hasMotion\":", "\"motionRightMm\":", "\"motionDownMm\":", "\"rotationClockwiseDeg\":", "\"tiltDeg\":", "\"heightPercent\":",
        "\"hasStartMotion\":", "\"startMotionRightMm\":", "\"startMotionDownMm\":", "\"startRotationClockwiseDeg\":", "\"hasOverlap\":", "\"narrowestOverlapMm\":", "\"hasNew\":",
        "\"seamXPreviousMm\":", "\"seamXNewMm\":", "\"widthPreviousPx\":", "\"widthNewPx\":", "\"heightPreviousPx\":", "\"heightNewPx\":", "\"rightPreviousMm\":", "\"rightNewMm\":",
        "\"topPreviousMm\":", "\"bottomPreviousMm\":", "\"topNewMm\":", "\"bottomNewMm\":", "\"hasEdgeDistance\":", "\"edgeShare\":", "\"targetCoverShortfall\":"}, display_at));
    // What the stand-in's computation left: the wide search, the raster of the fine stage, and the sizes and edges of the raster before and after.
    const auto& wide = At(report, "wideSearch");
    CHECK(wide.kind == json::JsonKind::object && At(wide, "determined").boolean && At(wide, "reason").string.empty() && NumberOf(At(wide, "tilesAgreed")) == 21 && NumberOf(At(wide, "correspondences")) == 14);
    CHECK(NumberOf(At(wide, "txMm")) == 30 && NumberOf(At(wide, "tyMm")) == -1.7 && NumberOf(At(wide, "rzDeg")) == 0.8 && NumberOf(At(wide, "residualRmsMm")) == 1.25 && NumberOf(At(wide, "residualMaxMm")) == 3.5);
    const auto& fine = At(report, "fineFitRaster");
    CHECK(fine.kind == json::JsonKind::object && fine.object.size() == 5 && NumberOf(At(fine, "widthPx")) == 1800 && NumberOf(At(fine, "heightPx")) == 1600 &&
        NumberOf(At(fine, "topMm")) == 100 && NumberOf(At(fine, "bottomMm")) == 260 && NumberOf(At(fine, "rightMm")) == 740);
    const auto& display = At(report, "display");
    CHECK(!At(display, "hasStartMotion").boolean && IsNull(At(display, "startMotionRightMm")) && IsNull(At(display, "startMotionDownMm")) && IsNull(At(display, "startRotationClockwiseDeg")));
    CHECK(IsNull(At(display, "targetCoverShortfall")));
    CHECK(At(display, "hasNew").boolean && NumberOf(At(display, "heightPreviousPx")) == 1600 && NumberOf(At(display, "heightNewPx")) == o.facts.display.height_new_px);
    CHECK(NumberOf(At(display, "topPreviousMm")) == 100 && NumberOf(At(display, "bottomPreviousMm")) == 260 && NumberOf(At(display, "topNewMm")) == 100 && NumberOf(At(display, "bottomNewMm")) == 260);
}

void WholeJob(const Pair& pair) {
    const auto settings = MakeSettings("settings-whole");
    const auto before = Snapshot(settings);
    std::vector<std::string> stages;
    std::vector<double> stage_seconds;
    auto request = Request(settings, pair, "staging-whole", FakeComputation(Fake::ready));
    request.on_stage = [&](RecalibrationStage s, double seconds) { stages.emplace_back(RecalibrationStageCode(s)); stage_seconds.push_back(seconds); };
    const auto began = std::chrono::steady_clock::now();
    const auto o = RunRecalibrationJob(request);
    std::cerr << "whole job: " << Since(began) << " s, verdict " << RecalibrationVerdictCode(o.verdict) << ", gate " << (o.facts.gate.known ? gate::GateStateCode(o.facts.gate.state) : "unknown")
              << ", findings";
    for (const auto& f : o.findings) std::cerr << ' ' << f.code << '(' << FindingClassCode(f.klass) << ')';
    std::cerr << '\n';

    // The stages arrive in order, with seconds that do not go back.
    const std::vector<std::string> expected{"read-condition", "decode", "wide-search", "fine-fit", "bake", "check", "verify", "sample", "crops"};
    if (stages != expected) { std::cerr << "stages:"; for (const auto& n : stages) std::cerr << ' ' << n; std::cerr << std::endl; }
    CHECK(stages == expected);
    CHECK(std::is_sorted(stage_seconds.begin(), stage_seconds.end()));
    // The sample came back with something to show, and the flat sheet could not be measured: an unchecked calibration, with nothing found wrong.
    CHECK(o.candidate.has_value() && o.facts.gate.known && o.facts.creation_checked);
    CHECK(o.verdict == RecalibrationVerdict::usable_unverified && o.ready && o.unverified());
    CHECK(OnlyClass(o, FindingClass::unverified));
    // A flat sheet: the estimate lacks marks and the gate measured nothing. Told by the codes of the reasons and of the gate's findings, not by the state.
    CHECK(HasCode(o, "sample-estimate-undetermined") && HasCode(o, "gate-nothing-measured") && !HasCode(o, "sample-estimate-doubtful") && !HasCode(o, "gate-unknown"));
    CHECK(Has(o.screen_lines.front(), "未確認の校正"));
    CHECK(HasLine(o.screen_lines, "継ぎ目のずれ: 測れ"));   // the gate's result is on the screen, as a fact (what was measured)
    CHECK(HasLine(o.screen_lines, "継ぎ目のずれを確かめられませんでした"));   // and the finding says that it was not checked
    CHECK(HasLine(o.screen_lines, "A に対する B の位置の変化: 図面の右へ約 ") && HasLine(o.screen_lines, "範囲の検査: 通りました"));
    CHECK(o.facts.display.has_motion && std::abs(o.facts.display.motion.camera_motion_mm.x - 30.0) < 0.5);
    CHECK(o.thumbnail.width > 0 && o.thumbnail.height > 0);
    CHECK(AllPlain(o.screen_lines) && AllPlain(o.log_lines));
    CHECK(HasLine(o.log_lines, "recalibration_verdict verdict=usable-unverified unverified=1") && HasLine(o.log_lines, "settings_written=0") && HasLine(o.log_lines, "camera_operations=0"));
    CHECK(HasLine(o.log_lines, "recalibration_finding class=unverified"));
    // The raster that the candidate has, against the one in force: one line with the four numbers (the width follows B here, the height stays).
    {
        const auto& d = o.facts.display;
        CHECK(d.has_new && d.width_new_px != d.width_previous_px && d.height_new_px == d.height_previous_px);
        CHECK(HasLine(o.log_lines, "recalibration_raster width_px=" + std::to_string(d.width_previous_px) + "->" + std::to_string(d.width_new_px) +
            " height_px=" + std::to_string(d.height_previous_px) + "->" + std::to_string(d.height_new_px) + " top_mm="));
    }
    // The job read the settings and wrote nothing there.
    CHECK(Snapshot(settings) == before);
    // The record in the staging folder: what a confirmation would write, by its hashes; the sample; the report.
    const auto& s = request.staging_folder;
    CHECK(fs::exists(s / "review" / "uncalibrated-review.json") && fs::exists(s / "review" / "uncalibrated-review.jpg") && fs::exists(s / "recalibration-report.json"));
    CHECK(!fs::exists(s / "sample-originals") && !fs::exists(s / "confirmation.json"));   // canonical originals are used where they are
    if (o.candidate) {
        const auto& c = *o.candidate;
        CHECK(HashOf(ReadAll(s / "estimate-record.json")) == c.condition.revision.change.report_sha256.value_or(""));
        CHECK(HashOf(ReadAll(s / "candidate-profile.json")) == c.condition.source.profile_file_sha256 && ReadAll(s / "candidate-profile.json") == c.profile_bytes);
        CHECK(HashOf(ReadAll(s / "candidate-revision.json")) == c.sha256 && ReadAll(s / "candidate-revision.json") == c.text);
        CHECK(c.condition.revision.sequence == 2 && o.sequence == 2 && c.condition.revision.change.kind == m2e::FixedCompositeRevisionKind::recalibrated);
        CHECK(c.condition.revision.change.pair_id == std::optional<std::string>("pair-0042") && o.parent_sha256 == Active(settings)->file_sha256);
        CHECK(c.condition.seam.origin == m2e::FixedSeamOrigin::band_center_provisional && c.condition.raster.width_pixels > Active(settings)->condition.raster.width_pixels);   // the owner's Q1 and Q2
    }
    // The sample is the composite of the candidate: a v8 report that names the candidate's revision.
    {
        const auto saved = m2e::LoadSavedStillReview(s / "review" / "uncalibrated-review.json");
        CHECK(saved.fixed_composite_registration.has_value());
        if (saved.fixed_composite_registration && o.candidate) {
            CHECK(saved.fixed_composite_registration->condition_sha256 == o.candidate->sha256 && saved.fixed_composite_registration->condition_sequence == 2);
            CHECK(saved.fixed_composite_registration->profile_file_sha256 == o.candidate->condition.source.profile_file_sha256);
        }
    }
    Reports(o, s, "usable-unverified", "confirm-with-warning", pair);

    // ---- The confirmation ----
    const auto fresh = [&](const std::string& name) { const auto copy = root / name; CopyFolder(settings, copy); return copy; };
    {   // An unchecked calibration needs the person's acknowledgement.
        const auto copy = fresh("confirm-noack");
        const auto b = Snapshot(copy);
        for (const auto& options : {RecalibrationConfirmOptions{}, RecalibrationConfirmOptions{false}}) {
            const auto r = ConfirmRecalibration(o, copy, kHash, options);
            CHECK(!r.written && r.reason == "not-acknowledged" && Has(r.message, "警告を見たうえで") && Snapshot(copy) == b);
        }
    }
    const auto confirmed = fresh("confirm-ack");
    {
        const auto b = Snapshot(confirmed);
        const auto r = ConfirmRecalibration(o, confirmed, kHash, {true});
        CHECK(r.written && r.reason.empty() && r.unverified && r.sequence == 2 && r.revision_sha256 == o.candidate->sha256);
        CHECK(Has(r.message, "未確認の校正として更新しました（改訂 2）") && HasLine(r.log_lines, "recalibration_confirmed=1 sequence=2 unverified=1 acknowledged=1"));
        // Exactly a profile copy and a revision were added; nothing that was there changed.
        const auto after = Snapshot(confirmed);
        std::vector<std::string> added;
        for (const auto& [name, bytes] : after) {
            const auto it = b.find(name);
            if (it == b.end()) added.push_back(name);
            else CHECK(it->second == bytes);
        }
        CHECK(after.size() == b.size() + 2 && added.size() == 2);
        CHECK(std::find(added.begin(), added.end(), "wpd-fixed-composite-revisions/r000002.json") != added.end());
        CHECK(std::find(added.begin(), added.end(), o.candidate->profile_name) != added.end());
        // The revision in force is the candidate, read the way a capture reads it (by the setup's condition name), with the new profile.
        const auto selection = m2e::SelectFixedComposite(confirmed / m2e::kFixedCompositeConditionName, kHash);
        const auto* v3 = selection.loaded ? std::get_if<m2e::LoadedFixedCompositeConditionV3>(&*selection.loaded) : nullptr;
        CHECK(selection.IsV3() && v3 && v3->file_sha256 == r.revision_sha256 && v3->condition.revision.sequence == 2 && v3->profile.profile_file_sha256 == o.candidate->condition.source.profile_file_sha256);
        // The mark of an unchecked calibration is beside the report.
        const auto mark = Tree(ReadAll(request.staging_folder / "confirmation.json"));
        CHECK(At(mark, "schema").string == "a0.pair-recalibration-confirmation.v1" && At(mark, "unverified").boolean && At(mark, "acknowledged").boolean &&
            At(mark, "verdict").string == "usable-unverified" && At(mark, "revisionSha256").string == r.revision_sha256 && PlainRecord(ReadAll(request.staging_folder / "confirmation.json")));
        // It names the two cameras, and it stays far below what the reader of the mark takes (4 KiB).
        CameraFields(ReadAll(request.staging_folder / "confirmation.json"), mark);
        CHECK(ReadAll(request.staging_folder / "confirmation.json").size() < 1024);
        // The same outcome a second time: the revision in force is no longer its parent. Refused, nothing written.
        const auto again_before = Snapshot(confirmed);
        const auto again = ConfirmRecalibration(o, confirmed, kHash, {true});
        CHECK(!again.written && again.reason == "condition-changed" && Has(again.message, "撮り直してください") && Snapshot(confirmed) == again_before);
    }
    {   // The condition changed after the job (another revision came in): refused, and nothing is written.
        const auto copy = fresh("confirm-moved");
        m2e::LayerTwoStrainDesignV3 layer{64.0, 0.5};
        FixedConditionV3Request tool;
        tool.settings_folder = copy; tool.kind = m2e::FixedCompositeRevisionKind::manual_adjustment; tool.layer_two = layer;
        tool.entered = m2e::FixedCompositeRevisionChangeV3::Entered{0.5, 0.0, 0.0};
        CHECK(MakeFixedCompositeConditionV3File(tool, kHash, kNow).made);
        const auto b = Snapshot(copy);
        const auto r = ConfirmRecalibration(o, copy, kHash, {true});
        CHECK(!r.written && r.reason == "condition-changed" && Snapshot(copy) == b && Active(copy)->condition.revision.sequence == 2);
    }
    {   // A partial file of the next revision (a writer that died): the write is refused and the earlier revision stays in force.
        const auto copy = fresh("confirm-partial");
        { std::ofstream(copy / std::string(m2e::kFixedCompositeRevisionFolderName) / "r000002.json.partial") << "x"; }
        const auto r = ConfirmRecalibration(o, copy, kHash, {true});
        CHECK(!r.written && r.reason == "write-failed" && Has(r.message, "前の校正のまま"));
        const auto active = Active(copy);
        CHECK(active && active->condition.revision.sequence == 1 && active->file_sha256 == o.parent_sha256 && !fs::exists(copy / std::string(m2e::kFixedCompositeRevisionFolderName) / "r000002.json"));
        // The profile copy may be there (named by its content, harmless); it is the candidate's own bytes.
        if (fs::exists(copy / o.candidate->profile_name)) CHECK(ReadAll(copy / o.candidate->profile_name) == o.candidate->profile_bytes);
    }
    {   // A file of the profile's name with other bytes is never replaced; no revision is written.
        const auto copy = fresh("confirm-collision");
        { std::ofstream(copy / o.candidate->profile_name, std::ios::binary) << "other bytes"; }
        const auto b = Snapshot(copy);
        const auto r = ConfirmRecalibration(o, copy, kHash, {true});
        CHECK(!r.written && r.reason == "write-failed" && Snapshot(copy) == b && Active(copy)->condition.revision.sequence == 1);
    }
    {   // What is written is what was checked: an outcome whose bytes were altered is not written.
        const auto copy = fresh("confirm-altered");
        const auto b = Snapshot(copy);
        auto altered = o;
        altered.candidate->profile_bytes += " ";
        CHECK(ConfirmRecalibration(altered, copy, kHash, {true}).reason == "write-failed");
        auto text = o;
        text.candidate->text += " ";
        CHECK(ConfirmRecalibration(text, copy, kHash, {true}).reason == "write-failed");
        auto other_parent = o;
        other_parent.parent_sha256 = std::string(64, 'f');
        CHECK(ConfirmRecalibration(other_parent, copy, kHash, {true}).reason == "not-usable");
        CHECK(Snapshot(copy) == b);
    }
    {   // An outcome that is not usable, or empty, writes nothing.
        const auto copy = fresh("confirm-empty");
        const auto b = Snapshot(copy);
        CHECK(ConfirmRecalibration(RecalibrationJobOutcome{}, copy, kHash, {true}).reason == "not-usable");
        auto unusable = o;
        unusable.verdict = RecalibrationVerdict::unusable; unusable.ready = false;
        CHECK(ConfirmRecalibration(unusable, copy, kHash, {true}).reason == "not-usable");
        bool threw = false;
        try { (void)ConfirmRecalibration(o, copy, nullptr, {true}); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw && Snapshot(copy) == b);
    }

    // ---- The way back ----
    // The recalibrated revision is a revision like the others: a hand adjustment on top of it works, and a rollback within its profile works.
    // A rollback to the revision before it is refused by the tool (the profile differs): the way back from a recalibration is to recalibrate again.
    {
        m2e::LayerTwoStrainDesignV3 layer{64.0, 0.5};
        FixedConditionV3Request back;
        back.settings_folder = confirmed; back.kind = m2e::FixedCompositeRevisionKind::rollback; back.restore_sequence = 1; back.layer_two = layer;
        const auto b = Snapshot(confirmed);
        const auto refused = MakeFixedCompositeConditionV3File(back, kHash, kNow);
        CHECK(!refused.made && Has(refused.message, "別の rig") && Snapshot(confirmed) == b);
        FixedConditionV3Request adjust;
        adjust.settings_folder = confirmed; adjust.kind = m2e::FixedCompositeRevisionKind::manual_adjustment; adjust.layer_two = layer;
        adjust.entered = m2e::FixedCompositeRevisionChangeV3::Entered{0.5, 0.0, 0.0};
        CHECK(MakeFixedCompositeConditionV3File(adjust, kHash, kNow).made && Active(confirmed)->condition.revision.sequence == 3);
        FixedConditionV3Request within;
        within.settings_folder = confirmed; within.kind = m2e::FixedCompositeRevisionKind::rollback; within.restore_sequence = 2; within.layer_two = layer;
        const auto made = MakeFixedCompositeConditionV3File(within, kHash, kNow);
        const auto active = Active(confirmed);
        CHECK(made.made && active && active->condition.revision.sequence == 4 && active->condition.revision.change.kind == m2e::FixedCompositeRevisionKind::rollback &&
            active->condition.source.profile_file == o.candidate->condition.source.profile_file && active->condition.base_pose.IsZero());
    }
}

// The refuse policy end to end: the same unchecked calibration is not offered, and the screen still shows what was measured.
void RefusePolicy(const Pair& pair) {
    const auto settings = MakeSettings("settings-refuse");
    const auto before = Snapshot(settings);
    auto request = Request(settings, pair, "staging-refuse", FakeComputation(Fake::ready));
    request.unverified_policy = UnverifiedPolicy::refuse_to_confirm;
    const auto o = RunRecalibrationJob(request);
    CHECK(o.verdict == RecalibrationVerdict::unusable && !o.ready && o.candidate.has_value() && OnlyClass(o, FindingClass::unverified));
    CHECK(o.screen_lines.front() == "この校正は使えません" && HasLine(o.screen_lines, "確かめられませんでした") && HasLine(o.screen_lines, "継ぎ目のずれ:"));
    const auto r = ConfirmRecalibration(o, settings, kHash, {true});
    CHECK(!r.written && r.reason == "not-usable" && Snapshot(settings) == before);
    Reports(o, request.staging_folder, "unusable", "refuse-to-confirm", pair);
}

// A check on the new profile that could not be made: the computation lets the candidate go, the job makes it again from the pose; the
// originals have names the review writer does not take, so the job copies them beside the record.
void UncheckedProfile(const Pair& pair) {
    const auto settings = MakeSettings("settings-unchecked");
    const auto before = Snapshot(settings);
    auto request = Request(settings, pair, "staging-unchecked", FakeComputation(Fake::check_not_trusted), true);
    const auto o = RunRecalibrationJob(request);
    CHECK(o.candidate.has_value() && o.verdict == RecalibrationVerdict::usable_unverified && OnlyClass(o, FindingClass::unverified));
    CHECK(HasCode(o, "reestimate-not-verified") && HasLine(o.screen_lines, "新しい校正での確かめが、確かな値になりませんでした"));
    CHECK(fs::exists(request.staging_folder / "sample-originals" / "CAM-A" / "original.jpg") && fs::exists(request.staging_folder / "sample-originals" / "CAM-B" / "original.jpg"));
    CHECK(Snapshot(settings) == before && AllPlain(o.screen_lines) && AllPlain(o.log_lines));
    CHECK(o.candidate && o.candidate->condition.revision.sequence == 2 && o.facts.display.has_motion);
    Reports(o, request.staging_folder, "usable-unverified", "confirm-with-warning", pair);
    // It confirms like any other unchecked calibration, with the acknowledgement.
    const auto copy = root / "settings-unchecked-copy";
    CopyFolder(settings, copy);
    CHECK(ConfirmRecalibration(o, copy, kHash, {}).reason == "not-acknowledged");
    const auto done = ConfirmRecalibration(o, copy, kHash, {true});
    CHECK(done.written && done.unverified && Active(copy)->file_sha256 == done.revision_sha256);
}

// ---- 4. The command line ----
void CommandLine(const Pair& pair, const std::string& exe, bool on) {
    const auto wide = [](const fs::path& p) { return p.wstring(); };
    const auto settings = MakeSettings("settings-cli");
    const auto before = Snapshot(settings);
    const std::vector<std::wstring> good{L"--camera-a", wide(pair.frames.a), L"--camera-b", wide(pair.frames.b), L"--settings-folder", wide(settings), L"--staging", wide(root / "staging-cli")};
    // Parsing: the defaults are the owner's decisions; every mistake is refused.
    {
        const auto p = ParseRecalibrationArguments(good);
        CHECK(p.request && p.error.empty() && p.request->seam == kRecalibrationSeamPlacement && p.request->raster == kRecalibrationRasterPlacement && p.request->unverified_policy == kRecalibrationUnverifiedPolicy);
        auto more = good;
        for (const auto* part : {L"--seam", L"keep", L"--raster", L"keep", L"--unverified", L"refuse", L"--pair-id", L"pair-7", L"--measured-at", L"2000-02-03T04:05:06Z"}) more.emplace_back(part);
        const auto q = ParseRecalibrationArguments(more);
        CHECK(q.request && q.request->seam == rc::SeamPlacement::keep_previous && q.request->raster == rc::RasterPlacement::keep && q.request->unverified_policy == UnverifiedPolicy::refuse_to_confirm &&
            q.request->pair_id == "pair-7" && q.request->measured_at_utc == "2000-02-03T04:05:06Z");
        const std::vector<std::vector<std::wstring>> bad{
            {}, {L"--camera-a"}, {L"--camera-a", L"x"},
            {L"--camera-a", L"a", L"--camera-a", L"b", L"--camera-b", L"c", L"--settings-folder", L"d", L"--staging", L"e"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--unknown", L"x"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--seam", L"middle"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--raster", L"wide"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--unverified", L"maybe"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--pair-id", L"not a pair"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d", L"--measured-at", L"tomorrow"},
            {L"--camera-a", L"", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d"},
            // A value that is the next flag (a forgotten value) is refused, not taken as a path.
            {L"--camera-a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"d"},
            {L"--camera-a", L"a", L"--camera-b", L"b", L"--settings-folder", L"c", L"--staging", L"--seam", L"keep"},
        };
        for (const auto& args : bad) {
            const auto refused = ParseRecalibrationArguments(args);
            CHECK(!refused.request && !refused.error.empty());
            std::ostringstream out;
            CHECK(RunRecalibrationCommand(args, out, kHash, kNow) == 2 && Has(out.str(), "recalibration_done=0") && Has(out.str(), "settings_written=0") && Has(out.str(), "usage: StillReview --recalibrate-from-pair"));
        }
    }
    // A run: a calibration that could not be checked ends with exit code 3 (a person would confirm it after the warning), and says so.
    {
        std::ostringstream out;
        const auto code = RunRecalibrationCommand(good, out, kHash, kNow, FakeComputation(Fake::ready));
        const auto text = out.str();
        CHECK(code == 3);
        CHECK(Has(text, "recalibration_done=1 verdict=usable-unverified unverified=1 confirmed=0 camera_operations=0 settings_written=0"));
        CHECK(Has(text, "finding class=unverified code=gate-") && Has(text, "gate state=") && Has(text, "motion right_mm=") && Has(text, "screen この校正は使えます") == false);
        CHECK(Has(text, "wide determined=1 reason=none tiles_agreed=21 correspondences=14 tx_mm=30 ty_mm=-1.7 rz_deg=0.8 residual_rms_mm=1.25"));
        CHECK(Has(text, "fine_fit_raster width_px=1800 height_px=1600 top_mm=100 bottom_mm=260"));
        CHECK(Has(text, "raster_height_px=1600->1600 raster_top_mm=100->100 raster_bottom_mm=260->260") && !Has(text, "start_motion") && !Has(text, "target_cover_shortfall"));
        CHECK(Has(text, "screen 未確認の校正です"));
        CHECK(Plain(text) && !Has(text, "staging-cli") && !Has(text, "pair-0042"));
        CHECK(Snapshot(settings) == before && fs::exists(root / "staging-cli" / "recalibration-report.json"));
    }
    {   // Refused by the computation: exit code 1. A staging folder that exists: also 1, and nothing in it is touched.
        auto args = good;
        args[7] = wide(root / "staging-cli-2");
        std::ostringstream out;
        CHECK(RunRecalibrationCommand(args, out, kHash, kNow, FakeComputation(Fake::no_overlap)) == 1 && Has(out.str(), "verdict=unusable") && Has(out.str(), "finding class=no-candidate code=no-overlap"));
        std::ostringstream again;
        CHECK(RunRecalibrationCommand(args, again, kHash, kNow, FakeComputation(Fake::no_overlap)) == 1 && Has(again.str(), "code=staging-exists"));
        CHECK(Snapshot(settings) == before && Plain(out.str()) && Plain(again.str()));
    }
    // StillReview itself: with the option on it has the entrance (wrong arguments are refused with the usage and code 2, without a window);
    // with the option off the command is unknown to it (not run here: an unknown command ends in a message box).
    if (on) {
        const auto out_file = root / "exe-out.txt";
        const std::string command = "\"\"" + exe + "\" --recalibrate-from-pair > \"" + out_file.string() + "\" 2>&1\"";
        const int code = std::system(command.c_str());
        const auto text = ReadAll(out_file);
        CHECK(code == 2 && Has(text, "recalibration_done=0") && Has(text, "usage: StillReview --recalibrate-from-pair"));
    } else {
        std::cout << "note: StillReview was built without A0_BUILD_WPD_RECALIBRATION; its entrance is absent and was not run\n";
    }
}

// ---- 5. (not run by ctest) the whole path with the real computation, both cameras moved ----
struct Texture {
    static constexpr double kLeft = -30, kTop = -30;
    struct Octave {double spacing, amplitude; int columns, rows; std::vector<float> nodes;};
    std::vector<Octave> octaves;
    explicit Texture(std::uint64_t seed) {
        const double widths[]{10.0, 4.0, 1.6};
        const double amplitudes[]{70.0, 55.0, 45.0};
        std::uint64_t state = seed;
        const auto next = [&] { state += 0x9e3779b97f4a7c15ull; std::uint64_t z = state; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; z ^= z >> 31; return z; };
        for (int k = 0; k < 3; ++k) {
            Octave o{widths[k], amplitudes[k], static_cast<int>(1400 / widths[k]) + 3, static_cast<int>(560 / widths[k]) + 3, {}};
            o.nodes.resize(static_cast<std::size_t>(o.columns) * static_cast<std::size_t>(o.rows));
            for (auto& v : o.nodes) v = static_cast<float>(static_cast<double>(next() >> 11) / 9007199254740992.0 * 2.0 - 1.0);
            octaves.push_back(std::move(o));
        }
    }
    [[nodiscard]] double operator()(double x, double y) const {
        double value = 128;
        for (const auto& o : octaves) {
            const double gx = (x - kLeft) / o.spacing, gy = (y - kTop) / o.spacing;
            const int ix = std::clamp(static_cast<int>(std::floor(gx)), 0, o.columns - 2), iy = std::clamp(static_cast<int>(std::floor(gy)), 0, o.rows - 2);
            const double tx = std::clamp(gx - ix, 0.0, 1.0), ty = std::clamp(gy - iy, 0.0, 1.0);
            const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
            const auto node = [&](int cx, int cy) { return static_cast<double>(o.nodes[static_cast<std::size_t>(cy) * static_cast<std::size_t>(o.columns) + static_cast<std::size_t>(cx)]); };
            const double top = node(ix, iy) * (1 - sx) + node(ix + 1, iy) * sx, bottom = node(ix, iy + 1) * (1 - sx) + node(ix + 1, iy + 1) * sx;
            value += o.amplitude * (top * (1 - sy) + bottom * sy);
        }
        return value;
    }
};
render::BgrImage RenderFrame(const render::Matrix3& h, const Texture& paper) {
    render::BgrImage image{ps::kWidth, ps::kHeight, std::vector<std::uint8_t>(static_cast<std::size_t>(ps::kWidth) * ps::kHeight * 3)};
    const auto inverse = render::Invert(h);
    std::vector<std::thread> threads;
    for (unsigned w = 0; w < 2; ++w) threads.emplace_back([&, w] {
        for (unsigned v = w; v < ps::kHeight; v += 2)
            for (unsigned u = 0; u < ps::kWidth; ++u) {
                render::Point sheet{};
                const auto value = render::TryTransform(inverse, {static_cast<double>(u), static_cast<double>(v)}, sheet) ? paper(sheet.x, sheet.y) : 128.0;
                auto* at = &image.bgr[(static_cast<std::size_t>(v) * ps::kWidth + u) * 3];
                at[0] = at[1] = at[2] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
    });
    for (auto& t : threads) t.join();
    return image;
}
render::Matrix3 Multiply(const render::Matrix3& a, const render::Matrix3& b) {
    render::Matrix3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[static_cast<std::size_t>(3 * i + j)] += a[static_cast<std::size_t>(3 * i + k)] * b[static_cast<std::size_t>(3 * k + j)];
    return r;
}
// A moved by `a_mm` and B by `b_mm` along the sheet's x: the drawing is seen at other places by both, and A is no longer where the profile has it.
void BothMoved(double a_mm, double b_mm) {
    const Texture paper(7919317);
    const render::Matrix3 a0{10, 0, 100, 0, 10, 100, 0, 0, 1}, b0{10, 0, -5660, 0, 10, 100, 0, 0, 1};
    const auto moved = [](double mm) { return render::Matrix3{1, 0, -mm, 0, 1, 0, 0, 0, 1}; };
    const auto began = std::chrono::steady_clock::now();
    const auto dir = NewFolder("both-moved");
    const auto frames = ps::WriteFrames(dir, RenderFrame(Multiply(a0, moved(a_mm)), paper), RenderFrame(Multiply(b0, moved(b_mm)), paper));
    const auto settings = MakeSettings("settings-both-moved");
    const auto before = Snapshot(settings);
    Pair pair{frames, {}, {}};
    auto request = Request(settings, pair, "staging-both-moved", RecalibrationComputation{});
    request.thread_count = 4;
    request.on_stage = [&](RecalibrationStage s, double seconds) { std::cerr << "  stage " << RecalibrationStageCode(s) << " at " << seconds << " s\n"; };
    const auto o = RunRecalibrationJob(request);
    std::cerr << "both moved (A " << a_mm << " mm, B " << b_mm << " mm): " << Since(began) << " s, verdict " << RecalibrationVerdictCode(o.verdict) << '\n';
    for (const auto& l : o.screen_lines) std::cerr << "  " << l << '\n';
    for (const auto& f : o.findings) std::cerr << "  finding " << FindingClassCode(f.klass) << ' ' << f.code << ' ' << f.reason << '\n';
    CHECK(o.ready && o.candidate.has_value() && Snapshot(settings) == before);
    // The place of B against A is what changed: b - a, to the right.
    CHECK(o.facts.display.has_motion && std::abs(o.facts.display.motion.camera_motion_mm.x - (b_mm - a_mm)) < 1.0 && std::abs(o.facts.display.motion.camera_motion_mm.y) < 1.0);
    CHECK(HasLine(o.screen_lines, "図面の右へ約"));
    CHECK(o.verdict == RecalibrationVerdict::usable);
    CHECK(o.facts.gate.known && o.facts.gate.state == gate::GateState::pass && o.findings.empty());
    const auto r = ConfirmRecalibration(o, settings, kHash, {});
    CHECK(r.written && !r.unverified && Active(settings)->condition.revision.sequence == 2);
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 4) throw std::invalid_argument("usage: a0_recalibration_job_tests <rig-profile-v2 fixture folder> <StillReview exe> <on|off> [full]");
        ps::detail::Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        fixture = ReadAll(fs::path(argv[1]) / "calibrated-draft.json");
        if (fixture.empty()) throw std::invalid_argument("the fixture is missing");
        fs::path parent = fs::temp_directory_path();
        {
            char* value = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&value, &length, "A0_TEST_TMPDIR") == 0 && value != nullptr) {
                if (*value != 0) parent = fs::path(value);
                std::free(value);
            }
        }
        root = parent / ("recalibration-job-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);
        const auto started = std::chrono::steady_clock::now();
        const auto lap = [&](const char* name) { std::cerr << name << " done at " << Since(started) << " s\n"; };
        PlainRecordSelfTest(); lap("check of a record's text");
        Decisions(); lap("decisions");
        const auto pair = MakePair("pair");
        lap("frames");
        JobRefusals(pair); lap("refusals");
        SeparateReasons(pair); lap("separate reasons");
        StopAfterTheComputation(pair); lap("stop after the computation");
        RefusedBeforeThePlan(pair); lap("refused before the plan");
        OutsideEnvelope(pair); lap("outside the envelope");
        WholeJob(pair); lap("whole job and confirmation");
        RefusePolicy(pair); lap("refuse policy");
        UncheckedProfile(pair); lap("unchecked profile");
        CommandLine(pair, argv[2], std::string(argv[3]) == "on"); lap("command line");
        if (argc >= 5 && std::string(argv[4]) == "full") { BothMoved(20.0, 50.0); lap("both moved"); }
        std::error_code ignored;
        fs::remove_all(root, ignored);
    } catch (const std::exception& e) {
        std::cerr << "FAILED: an exception: " << e.what() << '\n';
        return 1;
    }
    std::cout << "recalibration job: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
