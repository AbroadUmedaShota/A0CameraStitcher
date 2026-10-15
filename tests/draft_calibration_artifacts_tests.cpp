#include "a0/m2/draft_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace a0::m2::calibration;
using namespace a0::common::protocol_json;
int checks = 0, failures = 0;
void Check(bool value, const std::string& label) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { throw std::invalid_argument("test JSON"); } };
JsonValue Tree(std::string_view json) { return BasicJsonParser<Failure>(json).Parse(); }
const JsonValue& At(const JsonValue& value, const char* key) { return value.object.at(key); }
std::string Q(const std::string& value) { return '"' + value + '"'; }
std::string N(double value) { char b[64]; const auto r = std::to_chars(b, b + sizeof(b), value); return std::string(b, r.ptr); }
std::string Read(const std::filesystem::path& path) { std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("test file missing"); return {std::istreambuf_iterator<char>(f), {}}; }
void Write(const std::filesystem::path& path, const std::string& value) { std::ofstream f(path, std::ios::binary); f << value; if (!f) throw std::runtime_error("test write"); }
template<class F> void Rejects(F f, const char* label) { bool refused = false; try { f(); } catch (const std::invalid_argument&) { refused = true; } Check(refused, label); }

// Analytic observations written independently of the calibration library.
// They carry D810 raster dimensions, but contain no image or hardware record.
std::string Session(bool reduced_raster = false, bool no_b_reference = false) {
    std::string chart = "[";
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 10; ++c) {
        if (r || c) chart += ',';
        chart += "{\"pointId\":" + Q("P" + std::to_string(r * 10 + c)) + ",\"xMm\":" + N(c * 40.0 - 180) + ",\"yMm\":" + N(r * 40.0 - 140) + '}';
    }
    chart += ']';
    const std::string width = reduced_raster ? "3680" : "7360", height = reduced_raster ? "2456" : "4912";
    std::string views = "[";
    const std::array<std::array<double, 3>, 8> angles{{{.30,.10,.05}, {-.25,.30,-.08}, {.20,-.35,.12}, {-.30,-.20,.20}, {.10,.40,-.15}, {.35,-.15,-.20}, {.15,.20,-.10}, {0,0,0}}};
    for (int camera = 0; camera < 2; ++camera) for (int v = 0; v < 8; ++v) {
        if (no_b_reference && camera == 1 && v == 7) continue;
        if (views.size() > 1) views += ',';
        const std::string prefix = camera == 0 ? "A" : "B";
        const std::string role = v < 6 ? "training" : (v == 6 ? "validation" : "reference");
        const std::string id = prefix + (v == 7 ? "R" : "V" + std::to_string(v));
        char hash[65]; std::snprintf(hash, sizeof(hash), "%064x", camera * 8 + v + 1);
        const auto& a = angles[static_cast<std::size_t>(v)];
        const double sx = std::sin(a[0]), cx = std::cos(a[0]), sy = std::sin(a[1]), cy = std::cos(a[1]), sz = std::sin(a[2]), cz = std::cos(a[2]);
        // Rz Ry Rx; only its first two columns are needed on z=0.
        const std::array<double, 6> rot{cz*cy, cz*sy*sx-sz*cx, sz*cy, sz*sy*sx+cz*cx, -sy, cy*sx};
        std::string ids = "[", pose = "[", measurement = "[", observations = "[";
        const double raster_scale = reduced_raster ? 0.5 : 1.0;
        for (int i = 0; i < 80; ++i) {
            if (i) { ids += ','; observations += ','; }
            const auto point_id = Q("P" + std::to_string(i)); ids += point_id;
            auto& part = i % 2 == 0 ? pose : measurement;
            if (part.size() > 1) part += ','; part += point_id;
            const double x = (i % 10) * 40.0 - 180, y = (i / 10) * 40.0 - 140;
            const double camera_x = rot[0]*x + rot[1]*y + (camera == 0 ? 30 : -30) + 4*v;
            const double camera_y = rot[2]*x + rot[3]*y + 3*v;
            const double z = rot[4]*x + rot[5]*y + 1100 + 10*v;
            const double nx = camera_x/z, ny = camera_y/z, r2 = nx*nx + ny*ny;
            const double radial = 1 - .06*r2 + .01*r2*r2 + .002*r2*r2*r2;
            const double u = raster_scale * (5600*(nx*radial + 2*.0005*nx*ny - .0003*(r2+2*nx*nx)) + 3679.5);
            const double vv = raster_scale * (5700*(ny*radial + .0005*(r2+2*ny*ny) - 2*.0003*nx*ny) + 2455.5);
            observations += "{\"pointId\":" + point_id + ",\"state\":\"detected\",\"pixel\":{\"x\":" + N(u) + ",\"y\":" + N(vv) + "}}";
        }
        ids += ']'; pose += ']'; measurement += ']'; observations += ']';
        views += "{\"viewId\":" + Q(id) + ",\"camera\":" + Q(camera == 0 ? "CAM-A" : "CAM-B") + ",\"imageSha256\":" + Q(hash)
            + ",\"widthPixels\":" + width + ",\"heightPixels\":" + height + ",\"role\":" + Q(role) + ',';
        views += v == 6 ? "\"posePointIds\":" + pose + ",\"measurementPointIds\":" + measurement : "\"usedPointIds\":" + ids;
        views += ",\"observations\":" + observations + '}';
    }
    views += ']';
    const auto group = "{\"widthPixels\":" + width + ",\"heightPixels\":" + height + ",\"lensLabel\":\"synthetic\",\"focalLengthLabel\":\"synthetic\",\"focusLabel\":\"fixed\"}";
    return "{\"schemaVersion\":\"a0.calibration-observation-session.v1\",\"sessionId\":\"synthetic-writer-session\",\"chartId\":\"synthetic-writer-chart\","
        "\"provenance\":{\"measuredAt\":\"2000-01-01T00:00:00Z\",\"tool\":{\"toolId\":\"synthetic-fixture\",\"version\":\"1.0.0\"}},\"chartPoints\":"
        + chart + ",\"cameras\":{\"CAM-A\":" + group + ",\"CAM-B\":" + group + "},\"views\":" + views + '}';
}

std::string Job() {
    return R"({"schemaVersion":"a0.draft-calibration-job.v1","profileId":"synthetic-writer-draft","provenanceId":"synthetic-writer-provenance","measuredAt":"2000-01-02T00:00:00Z","documentPlane":{"orientation":"landscape","cameraOrder":"CAM-A-left-CAM-B-right"},"referencePair":{"CAM-A":"AR","CAM-B":"BR"},"referencePlacement":{"angleRadians":0,"translationXMm":-594.5,"translationYMm":-420.5,"coplanarConfirmed":true},"numericalPolicy":{"minimumViews":2,"relativeSingularCutoff":0.01,"maxIterations":100,"relativeCostTolerance":1e-10,"relativeStepTolerance":1e-12,"initialDamping":0.001,"maxUndistortionIterations":40,"undistortionTolerancePixels":1e-9},"rigMeasurements":{"baselineMm":null,"overlapMm":null,"cameraToDocumentMm":{"CAM-A":null,"CAM-B":null},"lensFocalLengthMm":{"CAM-A":null,"CAM-B":null}}})";
}
std::string Serialize(const JsonValue& value) { return SerializeJsonWith<Failure>(value); }

void TestLibrary(const std::string& session, const std::string& job) {
    const auto output = RunDraftCalibrationJob(session, job);
    Check(output.ok && !output.profile_json.empty(), "a two-camera session produces a draft");
    if (!output.ok) {
        const auto diagnostic = Tree(output.report_json);
        std::cerr << "rejection=" << At(diagnostic, "rejection").string << " profile=" << At(diagnostic, "profileValidationFailure").string << '\n';
        for (const auto& camera : At(diagnostic, "cameras").array) {
            if (At(camera, "lens").kind == JsonKind::object) std::cerr << "lens=" << At(At(camera, "lens"), "status").string << '\n';
            if (At(camera, "reference").kind == JsonKind::object) std::cerr << "reference=" << At(At(camera, "reference"), "outcome").string << '\n';
        }
        return;
    }
    const auto profile = a0::m2::RigProfileV2::Parse(output.profile_json);
    Check(profile.IsCalibrated() && !profile.IsApproved(), "existing strict reader accepts a calibrated draft only");
    Check(profile.FingerprintSha256() == output.profile_fingerprint_sha256, "fingerprint is the existing v2 canonical fingerprint");
    {
        // This is the SAME evaluation reader/gate used by the draft stitch
        // executable (#273), followed by the real document renderer. Full
        // D810-sized constant buffers keep the expected feather independent
        // of the projection implementation; only a small crop is rendered.
        using namespace a0::m2::render;
        const OutputRaster raster{{500000, 400000, 503200, 403200}, 254, 32, 32};
        const auto parameters = profile.ValidateCalibrationForEvaluation(raster, Resampling::bilinear);
        const BgrImage a{7360, 4912, std::vector<std::uint8_t>(3ULL * 7360 * 4912, 64)};
        const BgrImage b{7360, 4912, std::vector<std::uint8_t>(3ULL * 7360 * 4912, 128)};
        const auto rendered = RenderDocumentPair(a, b, parameters);
        bool expected = rendered.image.width == 32 && rendered.image.height == 32;
        for (std::size_t y = 0; y < 32; ++y) for (std::size_t x = 0; x < 32; ++x) for (std::size_t c = 0; c < 3; ++c)
            expected = expected && rendered.image.bgr[(y * 32 + x) * 3 + c] == 64 + 2 * x;
        Check(expected && rendered.seam_navigation.available, "generated draft feeds the existing evaluation renderer and blends known D810 buffers correctly");
        Check(a.bgr.front() == 64 && a.bgr.back() == 64 && b.bgr.front() == 128 && b.bgr.back() == 128, "evaluation keeps both input buffers");
    }
    const auto p = Tree(output.profile_json), report = Tree(output.report_json);
    const auto& calibration = At(p, "calibration");
    Check(At(calibration, "provenanceId").string == At(report, "provenanceId").string, "profile and report share provenance ID");
    Check(At(report, "profileFingerprintSha256").string == profile.FingerprintSha256(), "report is bound to the exact profile content");
    Check(At(report, "sessionSha256").string.size() == 64 && At(At(report, "session"), "views").array.size() == 16,
        "report retains every training, validation and reference view and a session hash");
    Check(At(report, "quality").string == "not-assessed", "writer never approves optical quality");
    for (const auto* field : {"outputRaster", "correctionEnvelope", "qualityContract", "approval"}) Check(At(p, field).kind == JsonKind::null_value, "owner-decision block remains null");
    const auto parsed_session = CalibrationObservationSession::Parse(session);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* alias = i == 0 ? "CAM-A" : "CAM-B";
        const auto* reference_id = i == 0 ? "AR" : "BR";
        const SessionView* reference = nullptr;
        for (const auto& view : parsed_session.Views()) if (view.id == reference_id) reference = &view;
        Check(reference && At(At(calibration, "inputSha256"), alias).string == reference->image_sha256, "profile stores reference image hash, never manifest/training hash");
        const auto& camera_report = At(report, "cameras").array[i];
        const auto& residual = At(At(camera_report, "reference"), "residual");
        const auto& profile_residual = At(At(calibration, "fitResidualPixels"), alias);
        Check(At(residual, "rms").string == At(profile_residual, "rms").string
            && At(residual, "max").string == At(profile_residual, "max").string && At(residual, "pointCount").string == "80", "fit residual belongs to reference usedPointIds");
        Check(At(At(camera_report, "lens"), "views").array.size() == 7, "training and held-out per-view results remain in detailed report");
        Check(At(At(p, "cameras"), alias).object.at("physicalIdentity").kind == JsonKind::null_value, "writer does not invent physical identities");
    }
    const auto again = RunDraftCalibrationJob(session, job);
    Check(again.profile_json == output.profile_json && again.report_json == output.report_json, "same inputs produce identical artifact bytes");
    const auto reduced = RunDraftCalibrationJob(Session(true), job);
    Check(!reduced.ok && reduced.profile_json.empty() && At(Tree(reduced.report_json), "rejection").string == "unsupported-image-dimensions", "small calculation cannot masquerade as a D810 profile");
    const auto partial = RunDraftCalibrationJob(Session(false, true), job);
    Check(!partial.ok && partial.profile_json.empty() && At(Tree(partial.report_json), "cameras").array.size() == 2, "missing reference emits diagnostics and no partial profile");
    {
        auto missing_validation = Tree(session);
        std::erase_if(missing_validation.object.at("views").array, [](const auto& view) {
            return At(view, "camera").string == "CAM-A" && At(view, "role").string == "validation";
        });
        const auto failed = RunDraftCalibrationJob(Serialize(missing_validation), job);
        Check(!failed.ok && failed.profile_json.empty() && At(Tree(failed.report_json), "rejection").string == "validation-not-evaluated",
            "missing independent validation cannot be replaced by a successful training/reference fit");
    }
    {
        auto invalid_validation = Tree(session);
        for (auto& view : invalid_validation.object.at("views").array) {
            if (At(view, "camera").string != "CAM-A" || At(view, "role").string != "validation") continue;
            // A view with zero image extent has no homography seed. A merely
            // collinear/noisy view could still be evaluated with a large raw
            // residual, which is NOT a numerical failure or a quality gate.
            for (auto& observation : view.object.at("observations").array) {
                observation.object.at("pixel").object.at("x").string = "100";
                observation.object.at("pixel").object.at("y").string = "100";
            }
        }
        const auto failed = RunDraftCalibrationJob(Serialize(invalid_validation), job);
        Check(!failed.ok && failed.profile_json.empty() && At(Tree(failed.report_json), "rejection").string == "validation-not-evaluated",
            "failed held-out validation cannot be omitted from draft issuance");
    }
    for (const auto* key : {"profileId", "provenanceId", "measuredAt"}) {
        auto bad = Tree(job); bad.object.at(key).string = "invalid/value";
        const auto failed = RunDraftCalibrationJob(session, Serialize(bad));
        Check(!failed.ok && failed.profile_json.empty() && At(Tree(failed.report_json), "rejection").string == "profile-metadata-invalid", "invalid metadata cannot escape as profile");
    }
    {
        auto old = Tree(job); old.object.at("measuredAt").string = "1999-01-01T00:00:00Z";
        Check(!RunDraftCalibrationJob(session, Serialize(old)).ok, "calibration time cannot precede observations");
    }
    {
        auto injected = Tree(job); injected.object["approval"] = JsonValue{};
        Rejects([&] { (void)RunDraftCalibrationJob(session, Serialize(injected)); }, "job cannot inject an approval block");
    }
    Rejects([&] { (void)RunDraftCalibrationJob(session, std::string(65537, ' ')); }, "oversized job rejected");
    Rejects([&] { (void)RunDraftCalibrationJob(session, std::string(100, '[') + std::string(100, ']')); }, "deep job rejected before recursive parser");
    Rejects([&] { (void)RunDraftCalibrationJob("{}", job); }, "invalid observation session is not serialized as evidence");
}

int Run(const std::filesystem::path& exe, const std::filesystem::path& session, const std::filesystem::path& job, const std::filesystem::path& output) {
    std::wstring command = L"\"" + exe.wstring() + L"\" --session \"" + session.wstring() + L"\" --job \"" + job.wstring() + L"\" --output \"" + output.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return -1;
    if (WaitForSingleObject(child.hProcess, 600000) != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 2); WaitForSingleObject(child.hProcess, 10000); }
    DWORD code{}; GetExitCodeProcess(child.hProcess, &code); CloseHandle(child.hProcess); CloseHandle(child.hThread);
    return static_cast<int>(code);
}
void TestExecutable(const std::filesystem::path& exe, const std::filesystem::path& temp, const std::string& session, const std::string& job) {
    const auto session_file = temp / "session.json", job_file = temp / "job.json";
    Write(session_file, session); Write(job_file, job);
    const auto expected = RunDraftCalibrationJob(session, job);
    const auto output = temp / "completed";
    Check(Run(exe, session_file, job_file, output) == 0, "CLI publishes a completed artifact directory");
    Check(Read(output / "profile.json") == expected.profile_json && Read(output / "calibration-report.json") == expected.report_json, "CLI reread bytes equal library artifacts");
    const auto before = Read(output / "profile.json");
    Check(Run(exe, session_file, job_file, output) == 2 && Read(output / "profile.json") == before, "existing completed directory is never overwritten");
    const auto empty = temp / "preexisting"; std::filesystem::create_directory(empty);
    Check(Run(exe, session_file, job_file, empty) == 2 && std::filesystem::is_empty(empty), "even an existing empty directory is not reused");
    const auto failed_session = temp / "partial-session.json"; Write(failed_session, Session(false, true));
    const auto failure_output = temp / "no-profile";
    Check(Run(exe, failed_session, job_file, failure_output) == 3
        && !std::filesystem::exists(failure_output / "profile.json") && std::filesystem::exists(failure_output / "calibration-report.json"), "mapping failure publishes a report without a profile");
    const auto bad_job = temp / "bad-job.json"; Write(bad_job, "{}");
    const auto bad_output = temp / "bad-output";
    Check(Run(exe, session_file, bad_job, bad_output) == 2 && !std::filesystem::exists(bad_output), "malformed job creates no output directory");
    const auto absent_parent = temp / "absent-parent";
    Check(Run(exe, session_file, job_file, absent_parent / "output") == 2 && !std::filesystem::exists(absent_parent),
        "IO failure does not create parent directories or a completed artifact");
    const auto race = temp / "race";
    auto first = std::async(std::launch::async, [&] { return Run(exe, session_file, job_file, race); });
    auto second = std::async(std::launch::async, [&] { return Run(exe, session_file, job_file, race); });
    const int a = first.get(), b = second.get();
    Check((a == 0 && b == 2) || (a == 2 && b == 0), "concurrent publication admits exactly one writer");
    Check(Read(race / "profile.json") == expected.profile_json && Read(race / "calibration-report.json") == expected.report_json,
        "concurrent loser cannot overwrite the completed profile/report pair");
    Check(Read(session_file) == session && Read(job_file) == job, "input files are unchanged");
}
} // namespace

int main(int argc, char* argv[]) {
    const auto parent = std::filesystem::temp_directory_path();
    const auto temp = parent / ("a0_draft_calibration_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    bool owned = false;
    try {
        if (argc != 2 && argc != 3) throw std::invalid_argument("expected executable and optional new retained-artifact directory");
        owned = std::filesystem::create_directory(temp);
        if (!owned) throw std::runtime_error("test directory was not exclusively created");
        const auto session = Session(), job = Job();
        TestLibrary(session, job);
        TestExecutable(argv[1], temp, session, job);
        if (argc == 3) {
            const std::filesystem::path retained = argv[2];
            if (!std::filesystem::create_directory(retained)) throw std::runtime_error("retained artifact directory must be new");
            for (const auto* name : {"profile.json", "calibration-report.json"})
                std::filesystem::copy_file(temp / "completed" / name, retained / name);
            for (const auto* name : {"session.json", "job.json"}) std::filesystem::copy_file(temp / name, retained / name);
        }
    } catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << error.what() << '\n'; }
    // Only this invocation's exclusively created, direct temp child is removed.
    std::error_code error;
    if (owned && temp.is_absolute() && temp.parent_path() == parent) std::filesystem::remove_all(temp, error);
    std::cout << "draft_calibration_artifacts checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
