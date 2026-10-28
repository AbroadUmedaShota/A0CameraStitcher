#include "a0/m2/planar_calibration_artifacts.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/m2/document_render.hpp"
#include "a0/m2/synthetic_pair.hpp"
#include "a0/common/protocol_json.hpp"
#include "draft_calibration_json.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Planar calibration (#374) on synthetic observations. The pixels come from
// the synthetic pair generator's forward model (a0_m2_synthetic_pair, the
// same model that renders its images) with a known lens and a known H per
// camera; the estimator never sees those values. Every numerical threshold
// below is an input of this test, not an optical or quality acceptance line.

namespace {
using namespace a0::m2::calibration;
using namespace a0::common::protocol_json;
namespace syn = a0::m2::synthetic;
using Matrix3 = std::array<double, 9>;
int checks = 0, failures = 0;
void Check(bool value, const std::string& label) { ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
struct Failure { [[noreturn]] static void Fail(std::string_view, std::string_view) { throw std::invalid_argument("test JSON"); } };
// The report embeds the session, so it can exceed the protocol default (256 KiB).
JsonValue Tree(std::string_view json) { return BasicJsonParser<Failure, 8U * 1024U * 1024U>(json).Parse(); }
const JsonValue& At(const JsonValue& value, const char* key) { return value.object.at(key); }
double Value(const JsonValue& value) { double out{}; std::from_chars(value.string.data(), value.string.data() + value.string.size(), out); return out; }
std::string Q(const std::string& value) { return '"' + value + '"'; }
std::string N(double value) { char b[64]; const auto r = std::to_chars(b, b + sizeof(b), value); return std::string(b, r.ptr); }
std::string Read(const std::filesystem::path& path) { std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("test file missing"); return {std::istreambuf_iterator<char>(f), {}}; }
void Write(const std::filesystem::path& path, const std::string& value) { std::ofstream f(path, std::ios::binary); f << value; if (!f) throw std::runtime_error("test write"); }
template<class F> void Rejects(F f, const char* label) { bool refused = false; try { f(); } catch (const std::invalid_argument&) { refused = true; } Check(refused, label); }

constexpr std::uint32_t kWidth = 7360, kHeight = 4912;
constexpr int kColumns = 11, kRows = 15; // chart nodes, pitch 20 mm: 200 x 280 mm
constexpr double kPitch = 20;
// Nominal values of the job: 50 mm lens, 35.9 x 24.0 mm sensor (D810 data sheet values).
constexpr double kFocal = 50, kSensorWidth = 35.9, kSensorHeight = 24.0;

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 p{};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) for (int k = 0; k < 3; ++k) p[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    return p;
}
Matrix3 Normalize(Matrix3 m) { const double s = m[8]; for (auto& v : m) v /= s; m[8] = 1; return m; }

struct Truth {
    syn::IntrinsicsSpec k;
    syn::DistortionSpec d;
    Matrix3 sheet_to_image; // sheet mm -> undistorted pixel, h22 = 1
    syn::Vec2 field_center; // sheet mm at the image centre (before the small rotation and tilt)
};
syn::CameraSpec Spec(const Truth& truth, const Matrix3& h) {
    syn::CameraSpec spec;
    spec.alias = "truth";
    spec.intrinsics = truth.k;
    spec.distortion = truth.d;
    spec.document_to_image = Normalize(h);
    return spec;
}
// Truth for one camera: quarter-turned, about 7 px/mm, a small in-plane
// rotation and a small tilt of the desk relative to the sensor.
Truth MakeTruth(syn::Vec2 center, double rotation, double tilt_x, double tilt_y, syn::DistortionSpec d, double focal_scale,
    double cx_offset, double cy_offset) {
    Truth t;
    t.k = {kFocal * kWidth / kSensorWidth * focal_scale, kFocal * kHeight / kSensorHeight * focal_scale,
        (kWidth - 1) / 2.0 + cx_offset, (kHeight - 1) / 2.0 + cy_offset};
    t.d = d;
    t.field_center = center;
    const auto base = syn::HomographyFromPlacement({center, 7.0, 1}, kWidth, kHeight);
    const double c = std::cos(rotation), s = std::sin(rotation);
    // Rotation about the field centre and a projective tilt.
    const Matrix3 to_center{1, 0, -center.x, 0, 1, -center.y, 0, 0, 1}, back{1, 0, center.x, 0, 1, center.y, 0, 0, 1};
    const Matrix3 rotate{c, -s, 0, s, c, 0, 0, 0, 1}, tilt{1, 0, 0, 0, 1, 0, tilt_x, tilt_y, 1};
    t.sheet_to_image = Normalize(Multiply(Multiply(base, Multiply(back, Multiply(rotate, to_center))), tilt));
    return t;
}

// sheet = R(angle) * chart + t, chart centre (100, 140) mm at `center`.
struct Placement { double angle; syn::Vec2 center; };
syn::Vec2 ChartToSheet(const Placement& p, double x, double y) {
    const double c = std::cos(p.angle), s = std::sin(p.angle);
    const double dx = x - 100, dy = y - 140;
    return {c * dx - s * dy + p.center.x, s * dx + c * dy + p.center.y};
}

struct ViewSpec { std::string id; std::string role; Placement placement; };
// Deterministic sub-pixel perturbation (no random generator): amplitude in px.
double Wiggle(int view, int point, int axis, double amplitude) {
    return amplitude * std::sin(1.618 * point + 2.39 * view + 1.3 * axis) * std::cos(0.7 * point - 0.9 * axis);
}

std::string ChartId(int c, int r) { char b[32]; std::snprintf(b, sizeof(b), "F-%03d-%03d", c, r); return b; }

struct Scenario {
    std::array<Truth, 2> truth;
    std::array<std::vector<ViewSpec>, 2> views;
    Placement overlap;
    double noise = 0;
};

std::string Session(const Scenario& scenario, bool drop_validation = false) {
    std::string chart = "[";
    for (int r = 0; r < kRows; ++r) for (int c = 0; c < kColumns; ++c) {
        if (chart.size() > 1) chart += ',';
        chart += "{\"pointId\":" + Q(ChartId(c, r)) + ",\"xMm\":" + N(c * kPitch) + ",\"yMm\":" + N(r * kPitch) + '}';
    }
    chart += ']';
    std::string views = "[";
    int view_number = 0;
    for (int camera = 0; camera < 2; ++camera) for (const auto& view : scenario.views[static_cast<std::size_t>(camera)]) {
        if (drop_validation && view.role == "validation") continue;
        ++view_number;
        const auto& truth = scenario.truth[static_cast<std::size_t>(camera)];
        const auto spec = Spec(truth, truth.sheet_to_image);
        std::string ids = "[", pose = "[", measurement = "[", observations = "[";
        int index = 0;
        for (int r = 0; r < kRows; ++r) for (int c = 0; c < kColumns; ++c) {
            const auto sheet = ChartToSheet(view.placement, c * kPitch, r * kPitch);
            const auto pixel = syn::ProjectDocumentPoint(spec, sheet);
            if (!pixel || pixel->x < 20 || pixel->y < 20 || pixel->x > kWidth - 21 || pixel->y > kHeight - 21) continue;
            const auto id = Q(ChartId(c, r));
            if (observations.size() > 1) { observations += ','; ids += ','; }
            ids += id;
            auto& part = index % 2 == 0 ? pose : measurement;
            if (part.size() > 1) part += ',';
            part += id;
            const double u = pixel->x + Wiggle(view_number, index, 0, scenario.noise), v = pixel->y + Wiggle(view_number, index, 1, scenario.noise);
            observations += "{\"pointId\":" + id + ",\"state\":\"detected\",\"pixel\":{\"x\":" + N(u) + ",\"y\":" + N(v) + "}}";
            ++index;
        }
        ids += ']'; pose += ']'; measurement += ']'; observations += ']';
        char hash[65]; std::snprintf(hash, sizeof(hash), "%064x", view_number);
        if (views.size() > 1) views += ',';
        views += "{\"viewId\":" + Q(view.id) + ",\"camera\":" + Q(camera == 0 ? "CAM-A" : "CAM-B") + ",\"imageSha256\":" + Q(hash)
            + ",\"widthPixels\":7360,\"heightPixels\":4912,\"role\":" + Q(view.role) + ',';
        views += view.role == "validation" ? "\"posePointIds\":" + pose + ",\"measurementPointIds\":" + measurement : "\"usedPointIds\":" + ids;
        views += ",\"observations\":" + observations + '}';
    }
    views += ']';
    const std::string group = "{\"widthPixels\":7360,\"heightPixels\":4912,\"lensLabel\":\"synthetic\",\"focalLengthLabel\":\"50mm\",\"focusLabel\":\"fixed\"}";
    return "{\"schemaVersion\":\"a0.calibration-observation-session.v1\",\"sessionId\":\"synthetic-planar-session\",\"chartId\":\"synthetic-planar-chart\","
        "\"provenance\":{\"measuredAt\":\"2000-01-01T00:00:00Z\",\"tool\":{\"toolId\":\"synthetic-fixture\",\"version\":\"1.0.0\"}},\"chartPoints\":"
        + chart + ",\"cameras\":{\"CAM-A\":" + group + ",\"CAM-B\":" + group + "},\"views\":" + views + '}';
}

// chart = R(alpha) * sheet + t of the overlap placement, from sheet = R(angle) * chart + c - R(angle) (100, 140).
std::array<double, 3> OverlapJobPlacement(const Placement& p) {
    const double c = std::cos(p.angle), s = std::sin(p.angle);
    const double tx = p.center.x - (c * 100 - s * 140), ty = p.center.y - (s * 100 + c * 140);
    // chart = R(-angle) (sheet - t)
    return {-p.angle, -(c * tx + s * ty), -(-s * tx + c * ty)};
}

std::string Job(const Scenario& scenario, const std::string& overrides = {}) {
    const auto p = OverlapJobPlacement(scenario.overlap);
    std::string job = R"({"schemaVersion":"a0.planar-calibration-job.v1","profileId":"synthetic-planar-draft","provenanceId":"synthetic-planar-provenance","measuredAt":"2000-01-02T00:00:00Z","documentPlane":{"orientation":"landscape","cameraOrder":"CAM-A-left-CAM-B-right"},"overlapViews":{"CAM-A":"A-overlap","CAM-B":"B-overlap"},"overlapPlacement":{"angleRadians":)"
        + N(p[0]) + ",\"translationXMm\":" + N(p[1]) + ",\"translationYMm\":" + N(p[2])
        + R"(,"coplanarConfirmed":true},"nominalCameras":{"CAM-A":{"focalLengthMm":50,"sensorWidthMm":35.9,"sensorHeightMm":24},"CAM-B":{"focalLengthMm":50,"sensorWidthMm":35.9,"sensorHeightMm":24}},"numericalPolicy":{"minimumTrainingViews":5,"maxIterations":200,"relativeCostTolerance":1e-12,"relativeStepTolerance":1e-12,"initialDamping":0.001,"maxUndistortionIterations":40,"undistortionTolerancePixels":1e-9},"seamOutputDpi":180})";
    if (!overrides.empty()) {
        const auto at = overrides.find('=');
        const auto from = overrides.substr(0, at), to = overrides.substr(at + 1);
        const auto position = job.find(from);
        if (position == std::string::npos) throw std::runtime_error("test job override");
        job.replace(position, from.size(), to);
    }
    return job;
}

// Five flat placements per camera (centre and the four corners of its field),
// one validation placement, and one overlap placement seen by both.
Scenario MakeScenario(double noise, double focal_scale, double cx_offset, double cy_offset) {
    Scenario s;
    s.noise = noise;
    s.truth[0] = MakeTruth({380, 420.5}, 0.007, 1.2e-5, -0.8e-5, {-0.045, 0.09, -0.06, 1.5e-4, -2e-4}, focal_scale, cx_offset, cy_offset);
    s.truth[1] = MakeTruth({810, 420.5}, -0.012, -1.0e-5, 0.9e-5, {-0.052, 0.11, -0.08, -1e-4, 1.2e-4}, focal_scale, -cx_offset, cy_offset);
    s.overlap = {0.02, {595, 420}};
    const std::array<std::array<double, 3>, 5> offsets{{{0, 0, 0.05}, {-250, -390, -0.08}, {250, -390, 0.12}, {-250, 390, -0.03}, {250, 390, 0.09}}};
    for (std::size_t camera = 0; camera < 2; ++camera) {
        const std::string prefix = camera == 0 ? "A" : "B";
        const auto center = s.truth[camera].field_center;
        s.views[camera].push_back({prefix + "-overlap", "reference", s.overlap});
        for (std::size_t i = 0; i < offsets.size(); ++i)
            s.views[camera].push_back({prefix + "-T" + std::to_string(i + 1), "training", {offsets[i][2], {center.x + offsets[i][0], center.y + offsets[i][1]}}});
        s.views[camera].push_back({prefix + "-V1", "validation", {0.2, {center.x - 130, center.y + 200}}});
    }
    return s;
}

struct Comparison { double undistorted_max = 0, raw_max = 0; int samples = 0; };
// Sheet points that the true camera sees inside its image: true vs estimated
// undistorted (H) and raw (H, K, lens) pixel positions.
Comparison Compare(const Truth& truth, const a0::m2::render::CameraProjection& estimated) {
    Comparison out;
    const auto spec = Spec(truth, truth.sheet_to_image);
    for (double y = truth.field_center.y - 520; y <= truth.field_center.y + 520; y += 20)
        for (double x = truth.field_center.x - 345; x <= truth.field_center.x + 345; x += 15) {
            const auto raw = syn::ProjectDocumentPoint(spec, {x, y});
            if (!raw || raw->x < 0 || raw->y < 0 || raw->x > kWidth - 1 || raw->y > kHeight - 1) continue;
            a0::m2::render::Point mine{};
            if (!a0::m2::render::TryProjectDocument(estimated, {x, y}, mine)) { out.raw_max = 1e300; continue; }
            out.raw_max = std::max(out.raw_max, std::hypot(mine.x - raw->x, mine.y - raw->y));
            const auto& h = truth.sheet_to_image;
            const auto& e = estimated.document_to_image;
            const double w = h[6] * x + h[7] * y + 1, we = e[6] * x + e[7] * y + 1;
            const double du = (h[0] * x + h[1] * y + h[2]) / w - (e[0] * x + e[1] * y + e[2]) / we;
            const double dv = (h[3] * x + h[4] * y + h[5]) / w - (e[3] * x + e[4] * y + e[5]) / we;
            out.undistorted_max = std::max(out.undistorted_max, std::hypot(du, dv));
            ++out.samples;
        }
    return out;
}

void Report(const char* label, double value) { std::cout << label << '=' << value << '\n'; }

void TestRecovery(const std::filesystem::path& calibrator) {
    const auto scenario = MakeScenario(0, 1, 0, 0);
    const auto session = Session(scenario), job = Job(scenario);
    const auto output = RunPlanarCalibrationJob(session, job);
    Check(output.ok && !output.profile_json.empty(), "flat placements produce a draft profile");
    const auto report = Tree(output.report_json);
    if (!output.ok) {
        std::cerr << "rejection=" << At(report, "rejection").string << '\n';
        if (At(report, "cameras").kind == JsonKind::array)
            for (const auto& camera : At(report, "cameras").array) std::cerr << "status=" << At(camera, "status").string << '\n';
        return;
    }
    const auto profile = a0::m2::RigProfileV2::Parse(output.profile_json);
    Check(profile.IsCalibrated() && !profile.IsApproved(), "existing strict reader accepts the planar draft as a calibrated draft");
    Check(profile.FingerprintSha256() == output.profile_fingerprint_sha256, "fingerprint is the existing v2 canonical fingerprint");
    const auto p = Tree(output.profile_json);
    const auto& calibration = At(p, "calibration");
    Check(At(At(calibration, "tool"), "toolId").string == "a0-planar-calibrator", "profile names the planar (nominal K) tool");
    Check(At(report, "intrinsicsSource").string == "nominal-not-estimated" && At(report, "quality").string == "not-assessed",
        "report states nominal K and no quality verdict");
    for (const auto* field : {"outputRaster", "correctionEnvelope", "qualityContract", "approval"}) Check(At(p, field).kind == JsonKind::null_value, "owner-decision block remains null");
    Check(At(calibration, "inputSha256").object.at("CAM-A").string == std::string(63, '0') + "1", "profile binds the CAM-A overlap image hash");

    // Recovery of the known lens and H.
    using namespace a0::m2::render;
    const OutputRaster raster{{560000, 380000, 563200, 383200}, 254, 32, 32};
    const auto parameters = profile.ValidateCalibrationForEvaluation(raster, Resampling::bilinear);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& camera = i == 0 ? parameters.camera_a : parameters.camera_b;
        const auto& truth = scenario.truth[i];
        const auto* alias = i == 0 ? "CAM-A" : "CAM-B";
        const auto& d = camera.distortion;
        const std::array<double, 5> error{d.k1 - truth.d.k1, d.k2 - truth.d.k2, d.k3 - truth.d.k3, d.p1 - truth.d.p1, d.p2 - truth.d.p2};
        double coefficient_error = 0;
        for (const auto value : error) coefficient_error = std::max(coefficient_error, std::abs(value));
        std::cout << alias << " k1=" << d.k1 << " k2=" << d.k2 << " k3=" << d.k3 << " p1=" << d.p1 << " p2=" << d.p2 << '\n';
        std::cout << alias << " coefficient errors k1=" << error[0] << " k2=" << error[1] << " k3=" << error[2] << " p1=" << error[3] << " p2=" << error[4] << '\n';
        Check(coefficient_error < 1e-6, std::string(alias) + " five coefficients return to the known values (1e-6, test input)");
        double h_error = 0;
        for (std::size_t j = 0; j < 8; ++j) h_error = std::max(h_error, std::abs(camera.document_to_image[j] - truth.sheet_to_image[j]) / std::max(1e-12, std::abs(truth.sheet_to_image[j])));
        std::cout << alias << " H max relative element error=" << h_error << '\n';
        const auto compared = Compare(truth, camera);
        std::cout << alias << " samples=" << compared.samples << " undistorted max px=" << compared.undistorted_max << " raw max px=" << compared.raw_max << '\n';
        Check(compared.samples > 1000 && compared.undistorted_max < 1e-4 && compared.raw_max < 1e-4,
            std::string(alias) + " H and the full mapping return to the known ones over the field (1e-4 px, test input)");
        Check(camera.intrinsics.fx_pixels == kFocal * kWidth / kSensorWidth && camera.intrinsics.cx_pixels == (kWidth - 1) / 2.0,
            std::string(alias) + " profile carries the nominal K unchanged");
        const auto& camera_report = At(report, "cameras").array[i];
        const auto& held_out = At(camera_report, "heldOutResidual");
        Report((std::string(alias) + " held-out rms px").c_str(), Value(At(held_out, "rms")));
        Check(At(camera_report, "status").string == "planar-calibration-estimated" && Value(At(held_out, "rms")) < 1.0
            && Value(At(held_out, "max")) < 1.0, std::string(alias) + " held-out residual below 1 px (synthetic, test input)");
        const auto& fit = At(At(camera_report, "straightness"), "fit");
        const double raw_rms = Value(At(At(fit, "raw"), "rms")), undistorted_rms = Value(At(At(fit, "undistorted"), "rms"));
        std::cout << alias << " straightness raw rms=" << raw_rms << " undistorted rms=" << undistorted_rms << '\n';
        Check(undistorted_rms < 1e-4 && raw_rms > 1000 * undistorted_rms, std::string(alias) + " rows and columns are straight after removing the estimated lens");
        const auto& held_out_lines = At(At(At(camera_report, "straightness"), "heldOut"), "undistorted");
        Check(held_out_lines.kind == JsonKind::object && Value(At(held_out_lines, "rms")) < 1e-4, std::string(alias) + " validation rows and columns are straight too");
        Check(At(camera_report, "views").array.size() == 7, std::string(alias) + " report lists overlap, training and validation views");
        Check(Value(At(At(camera_report, "distortionCheck"), "minimumRadialSlope")) > 0, std::string(alias) + " physicality is reported");
    }
    {
        // The SAME evaluation reader/gate and renderer as StitchEval. Constant
        // D810-size buffers: inside the overlap both cameras contribute.
        const BgrImage a{kWidth, kHeight, std::vector<std::uint8_t>(3ULL * kWidth * kHeight, 64)};
        const BgrImage b{kWidth, kHeight, std::vector<std::uint8_t>(3ULL * kWidth * kHeight, 128)};
        const auto rendered = RenderDocumentPair(a, b, parameters);
        bool inside = rendered.image.width == 32 && rendered.image.height == 32;
        for (const auto value : rendered.image.bgr) inside = inside && value >= 64 && value <= 128;
        Check(inside && rendered.seam_navigation.available, "planar draft feeds the existing evaluation renderer");
    }

    const auto again = RunPlanarCalibrationJob(session, job);
    Check(again.profile_json == output.profile_json && again.report_json == output.report_json, "same inputs produce identical artifact bytes");

    // Command line: --planar-job writes the same bytes; --job and --planar-job together are refused.
    const auto root = std::filesystem::temp_directory_path() / ("a0-planar-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    Write(root / "session.json", session);
    Write(root / "job.json", job);
    const auto run = [&](const std::wstring& arguments) {
        const std::wstring command = L"\"" + std::filesystem::absolute(calibrator).make_preferred().wstring() + L"\" " + arguments;
        std::vector<wchar_t> line(command.begin(), command.end()); line.push_back(0);
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return -1;
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        return static_cast<int>(code);
    };
    const auto quoted = [](const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; };
    const int code = run(L"--session " + quoted(root / "session.json") + L" --planar-job " + quoted(root / "job.json") + L" --output " + quoted(root / "out"));
    Check(code == 0 && Read(root / "out" / "profile.json") == output.profile_json && Read(root / "out" / "calibration-report.json") == output.report_json,
        "DraftCalibrator --planar-job writes the library bytes");
    Check(run(L"--session " + quoted(root / "session.json") + L" --planar-job " + quoted(root / "job.json") + L" --job " + quoted(root / "job.json")
        + L" --output " + quoted(root / "both")) == 2 && !std::filesystem::exists(root / "both"), "both job kinds together are refused");
    Check(run(L"--session " + quoted(root / "session.json") + L" --job " + quoted(root / "job.json") + L" --output " + quoted(root / "wrong")) == 2,
        "a planar job is not accepted as a reference-pair job");
    std::filesystem::remove_all(root);
}

// Nominal K differs from the true K (focal 1 % off, principal point off by
// 12 / 8 px) and the pixels carry a deterministic +-0.1 px wiggle. The
// coefficients then absorb the scale; only the mapping is compared.
void TestNominalMismatch() {
    const auto scenario = MakeScenario(0.1, 1.01, 12, -8);
    const auto output = RunPlanarCalibrationJob(Session(scenario), Job(scenario));
    Check(output.ok, "mismatched nominal K still produces a draft");
    if (!output.ok) return;
    const auto report = Tree(output.report_json);
    const auto profile = a0::m2::RigProfileV2::Parse(output.profile_json);
    using namespace a0::m2::render;
    const auto parameters = profile.ValidateCalibrationForEvaluation({{560000, 380000, 563200, 383200}, 254, 32, 32}, Resampling::bilinear);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* alias = i == 0 ? "CAM-A" : "CAM-B";
        const auto& camera_report = At(report, "cameras").array[i];
        const double held_out = Value(At(At(camera_report, "heldOutResidual"), "rms"));
        const double fit = Value(At(At(camera_report, "fitResidual"), "rms"));
        const auto compared = Compare(scenario.truth[i], i == 0 ? parameters.camera_a : parameters.camera_b);
        std::cout << alias << " mismatch: fit rms=" << fit << " held-out rms=" << held_out << " raw mapping max px=" << compared.raw_max << '\n';
        Check(held_out < 1.0 && compared.raw_max < 1.0, std::string(alias) + " nominal-K mismatch stays below 1 px (synthetic, test input)");
    }
    const auto& seam = At(report, "seam");
    const double overlap = Value(At(At(seam, "overlap"), "maxMm")), held_out = Value(At(At(seam, "checkerboardHeldOut"), "maxMm"));
    std::cout << "mismatch seam max mm: overlap=" << overlap << " checkerboard=" << held_out << '\n';
    Check(At(At(seam, "checkerboardHeldOut"), "outcome").string == "measured" && overlap > 0 && overlap < 0.1 && held_out > 0 && held_out < 0.1,
        "nominal-K mismatch: seam disagreement is measured and below 0.1 mm (synthetic, test input)");
}

void TestRejections() {
    const auto scenario = MakeScenario(0, 1, 0, 0);
    const auto session = Session(scenario), job = Job(scenario);
    const auto rejection = [](const DraftCalibrationArtifacts& artifacts) { return At(Tree(artifacts.report_json), "rejection").string; };
    const auto status = [](const DraftCalibrationArtifacts& artifacts, std::size_t camera) {
        return At(At(Tree(artifacts.report_json), "cameras").array[camera], "status").string;
    };
    Rejects([&] { (void)RunPlanarCalibrationJob(session, job.substr(0, job.size() - 1) + ",\"extra\":1}"); }, "unknown job field is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, "\"minimumTrainingViews\":5,=")); }, "missing policy field is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, "planar-calibration-job.v1=draft-calibration-job.v1")); }, "another job schema is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, "\"maxIterations\":200=\"maxIterations\":0")); }, "invalid numerical policy is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, "\"focalLengthMm\":50=\"focalLengthMm\":\"50\"")); }, "string nominal value is refused");
    const auto no_focal = RunPlanarCalibrationJob(session, Job(scenario, "\"focalLengthMm\":50=\"focalLengthMm\":0"));
    Check(!no_focal.ok && no_focal.profile_json.empty() && rejection(no_focal) == "planar-calibration-unavailable"
        && status(no_focal, 0) == "nominal-camera-invalid", "zero nominal focal length gives no profile");
    const auto no_focal_report = Tree(no_focal.report_json);
    const auto& no_focal_seam = At(no_focal_report, "seam");
    Check(At(At(no_focal_seam, "overlap"), "outcome").string == "camera-not-estimated"
        && At(At(no_focal_seam, "checkerboardHeldOut"), "outcome").string == "camera-not-estimated"
        && At(At(no_focal_seam, "overlap"), "meanMm").kind == JsonKind::null_value, "no seam measure without both estimates");
    const auto not_coplanar = RunPlanarCalibrationJob(session, Job(scenario, "\"coplanarConfirmed\":true=\"coplanarConfirmed\":false"));
    Check(!not_coplanar.ok && not_coplanar.profile_json.empty() && rejection(not_coplanar) == "overlap-placement-invalid",
        "an overlap placement not declared coplanar gives no profile");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, ",\"coplanarConfirmed\":true=")); }, "missing coplanarConfirmed is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, "\"coplanarConfirmed\":true=\"coplanarConfirmed\":\"true\"")); }, "string coplanarConfirmed is refused");
    Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, ",\"seamOutputDpi\":180=")); }, "missing seamOutputDpi is refused");
    for (const auto* bad : {"0", "65536", "180.5", "\"180\""})
        Rejects([&] { (void)RunPlanarCalibrationJob(session, Job(scenario, std::string("\"seamOutputDpi\":180=\"seamOutputDpi\":") + bad)); },
            (std::string("seamOutputDpi ") + bad + " is refused").c_str());
    const auto role = RunPlanarCalibrationJob(session, Job(scenario, "\"CAM-A\":\"A-overlap\"=\"CAM-A\":\"A-T1\""));
    Check(!role.ok && status(role, 0) == "overlap-role-mismatch", "overlap view must be the reference view");
    const auto camera = RunPlanarCalibrationJob(session, Job(scenario, "\"CAM-A\":\"A-overlap\"=\"CAM-A\":\"B-overlap\""));
    Check(!camera.ok && status(camera, 0) == "overlap-camera-mismatch", "overlap view of the other camera is refused");
    const auto missing = RunPlanarCalibrationJob(session, Job(scenario, "\"CAM-B\":\"B-overlap\"=\"CAM-B\":\"B-none\""));
    Check(!missing.ok && status(missing, 1) == "overlap-view-missing", "missing overlap view is reported");
    const auto few = RunPlanarCalibrationJob(session, Job(scenario, "\"minimumTrainingViews\":5=\"minimumTrainingViews\":6"));
    Check(!few.ok && status(few, 0) == "insufficient-views", "minimum training views is enforced");
    const auto no_validation = RunPlanarCalibrationJob(Session(scenario, true), job);
    Check(!no_validation.ok && rejection(no_validation) == "validation-not-evaluated", "a camera without validation views gives no profile");
    const auto early = RunPlanarCalibrationJob(session, Job(scenario, "\"measuredAt\":\"2000-01-02T00:00:00Z\"=\"measuredAt\":\"1999-12-31T00:00:00Z\""));
    Check(!early.ok && rejection(early) == "profile-metadata-invalid", "measuredAt before the observations gives no profile");
    const NominalCamera good{50, 35.9, 24};
    const auto k = NominalIntrinsics(good, kWidth, kHeight);
    Check(k && k->fx == 50 * 7360 / 35.9 && k->fy == 50 * 4912 / 24.0 && k->cx == 3679.5 && k->cy == 2455.5, "nominal K: f / pitch and the image centre");
    bool invalid = true;
    for (const auto& bad : {NominalCamera{0, 35.9, 24}, NominalCamera{-50, 35.9, 24}, NominalCamera{50, 0, 24}, NominalCamera{50, 35.9, 10001},
             NominalCamera{std::nan(""), 35.9, 24}}) invalid = invalid && !NominalIntrinsics(bad, kWidth, kHeight);
    Check(invalid && !NominalIntrinsics(good, 0, kHeight), "invalid nominal declarations give no K");
}
// Seam disagreement and coverage (#385 WP-05). Expected values are computed
// here from the session JSON, independently of the estimator's code paths.
const JsonValue& ViewOf(const JsonValue& session, const std::string& id) {
    for (const auto& view : At(session, "views").array) if (At(view, "viewId").string == id) return view;
    throw std::runtime_error("test view");
}
std::vector<std::pair<std::string, std::array<double, 2>>> Detected(const JsonValue& view) {
    std::vector<std::pair<std::string, std::array<double, 2>>> out;
    for (const auto& o : At(view, "observations").array)
        if (At(o, "state").string == "detected") out.push_back({At(o, "pointId").string, {Value(At(At(o, "pixel"), "x")), Value(At(At(o, "pixel"), "y"))}});
    return out;
}
struct Stats { double mean, p95, max; std::size_t n; };
Stats StatsOf(std::vector<double> values) {
    double sum = 0;
    for (const auto value : values) sum += value;
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size())));
    return {sum / static_cast<double>(values.size()), values[rank - 1], values.back(), values.size()};
}
bool Near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

void TestSeam() {
    const auto scenario = MakeScenario(0, 1, 0, 0);
    const auto session_json = Session(scenario);
    const auto output = RunPlanarCalibrationJob(session_json, Job(scenario));
    Check(output.ok, "seam scenario produces a draft");
    if (!output.ok) return;
    const auto report = Tree(output.report_json);
    const auto tree = Tree(session_json);
    const auto& seam = At(report, "seam");
    Check(Value(At(seam, "outputDpi")) == 180 && At(At(report, "overlapPlacement"), "coplanarConfirmed").boolean, "report carries the seam DPI and the coplanar declaration");

    // Chart points detected in both overlap views, and their checkerboard colour.
    std::map<std::string, std::array<double, 2>> chart;
    for (const auto& p : At(tree, "chartPoints").array) chart[At(p, "pointId").string] = {Value(At(p, "xMm")), Value(At(p, "yMm"))};
    const auto a = Detected(ViewOf(tree, "A-overlap")), b = Detected(ViewOf(tree, "B-overlap"));
    std::vector<std::string> common, odd;
    for (const auto& [id, pixel] : a) {
        if (std::none_of(b.begin(), b.end(), [&](const auto& other) { return other.first == id; })) continue;
        common.push_back(id);
        const auto c = std::lround(chart[id][0] / kPitch), r = std::lround(chart[id][1] / kPitch);
        if ((c + r) % 2 == 1) odd.push_back(id);
    }
    const auto& overlap = At(seam, "overlap");
    const auto& held_out = At(seam, "checkerboardHeldOut");
    std::cout << "seam overlap n=" << Value(At(overlap, "pointCount")) << " mean mm=" << Value(At(overlap, "meanMm")) << " max mm=" << Value(At(overlap, "maxMm"))
              << " | checkerboard n=" << Value(At(held_out, "pointCount")) << " mean mm=" << Value(At(held_out, "meanMm")) << " max mm=" << Value(At(held_out, "maxMm")) << '\n';
    Check(At(overlap, "outcome").string == "measured" && Value(At(overlap, "pointCount")) == static_cast<double>(common.size()) && common.size() > 50,
        "overlap seam counts every point detected in both overlap views");
    Check(Value(At(overlap, "maxMm")) < 1e-6 && Value(At(overlap, "meanMm")) <= Value(At(overlap, "p95Mm")) + 1e-15
        && Value(At(overlap, "p95Mm")) <= Value(At(overlap, "maxMm")), "known mapping: overlap seam is about zero (1e-6 mm, test input)");
    Check(At(held_out, "outcome").string == "measured" && Value(At(held_out, "pointCount")) == static_cast<double>(odd.size())
        && Value(At(held_out, "maxMm")) < 1e-6, "known mapping: checkerboard held-out seam on the odd half is about zero (1e-6 mm, test input)");
    Check(Value(At(overlap, "maxOutputPixels")) == Value(At(overlap, "maxMm")) * 180 / 25.4, "output pixels are mm * dpi / 25.4");

    // Known disagreement: replace CAM-B's mapping by H_B * M, so that CAM-B
    // maps each point to M^-1(p). A shift gives |delta| at every point; a
    // scale about the chart origin gives |p| (1 - 1 / s).
    const auto session = CalibrationObservationSession::Parse(session_json);
    const PlanarCalibrationRequest request{{"A-overlap", "B-overlap"}, {NominalCamera{50, 35.9, 24}, NominalCamera{50, 35.9, 24}}};
    const PlanarCalibrationPolicy policy{5, 200, 1e-12, 1e-12, 0.001, 40, 1e-9};
    const auto result = EstimatePlanarCalibration(session, request, policy);
    const auto shifted = [&](const Matrix3& m) {
        auto changed = result;
        changed.cameras[1].document_to_image = Normalize(Multiply(*result.cameras[1].document_to_image, m));
        return MeasureOverlapSeam(session, request, policy, changed);
    };
    const auto shift = shifted({1, 0, 0.3, 0, 1, -0.4, 0, 0, 1});
    Check(shift.outcome == PlanarSeamOutcome::Measured && shift.disagreement && Near(shift.disagreement->mean_mm, 0.5, 1e-6)
        && Near(shift.disagreement->p95_mm, 0.5, 1e-6) && Near(shift.disagreement->max_mm, 0.5, 1e-6), "a known 0.5 mm shift of CAM-B reads 0.5 mm");
    constexpr double scale = 1.001;
    std::vector<double> expected;
    for (const auto& id : common) expected.push_back(std::hypot(chart[id][0], chart[id][1]) * (1 - 1 / scale));
    const auto want = StatsOf(expected);
    const auto scaled = shifted({scale, 0, 0, 0, scale, 0, 0, 0, 1});
    Check(scaled.disagreement && scaled.disagreement->point_count == want.n && Near(scaled.disagreement->mean_mm, want.mean, 1e-6)
        && Near(scaled.disagreement->p95_mm, want.p95, 1e-6) && Near(scaled.disagreement->max_mm, want.max, 1e-6),
        "a known 0.1 % scale of CAM-B reads the expected mean, p95 and max");
    if (scaled.disagreement)
        std::cout << "known scale: mean=" << scaled.disagreement->mean_mm << " p95=" << scaled.disagreement->p95_mm << " max=" << scaled.disagreement->max_mm
                  << " expected " << want.mean << ' ' << want.p95 << ' ' << want.max << '\n';
    else std::cout << "known scale: " << PlanarSeamOutcomeCode(scaled.outcome) << '\n';
    auto unestimated = result;
    unestimated.cameras[0].status = PlanarCameraStatus::SolverNonconvergence;
    Check(MeasureOverlapSeam(session, request, policy, unestimated).outcome == PlanarSeamOutcome::CameraNotEstimated, "no seam without both estimates");

    // Coverage: 4 x 4 cells of the fitted points and the image corner r^2.
    for (std::size_t i = 0; i < 2; ++i) {
        const std::string prefix = i == 0 ? "A-" : "B-";
        std::array<double, 16> cells{};
        for (const auto& view : At(tree, "views").array) {
            const auto& id = At(view, "viewId").string;
            if (id.rfind(prefix, 0) != 0 || (At(view, "role").string != "training" && id != prefix + "overlap")) continue;
            const auto detected = Detected(view);
            for (const auto& used : At(view, "usedPointIds").array)
                for (const auto& [point, pixel] : detected) if (point == used.string) {
                    const auto cell = [](double value, double size) { return std::clamp(static_cast<int>(std::floor(4 * (value + 0.5) / size)), 0, 3); };
                    ++cells[static_cast<std::size_t>(cell(pixel[1], kHeight) * 4 + cell(pixel[0], kWidth))];
                }
        }
        const auto& coverage = At(At(report, "cameras").array[i], "coverage");
        bool same = true;
        double total = 0;
        std::string row_text;
        for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
            const double value = Value(At(coverage, "fitPointCells").array[row].array[column]);
            same = same && value == cells[row * 4 + column];
            total += value;
            row_text += (column ? " " : row ? " | " : "") + std::to_string(static_cast<int>(value));
        }
        const double fx = kFocal * kWidth / kSensorWidth, fy = kFocal * kHeight / kSensorHeight;
        const double corner = std::pow((kWidth - 1) / 2.0 / fx, 2) + std::pow((kHeight - 1) / 2.0 / fy, 2);
        const double reported = Value(At(coverage, "imageCornerRadiusSquared"));
        std::cout << (i == 0 ? "CAM-A" : "CAM-B") << " fit cells " << row_text << " corner r2=" << reported
                  << " fit max r2=" << Value(At(At(At(report, "cameras").array[i], "distortionCheck"), "maxRadiusSquared")) << '\n';
        Check(same && total == Value(At(At(At(report, "cameras").array[i], "fitResidual"), "pointCount")),
            std::string(i == 0 ? "CAM-A" : "CAM-B") + " fit point cells match the session and sum to the fitted points");
        Check(Near(reported, corner, 1e-15), std::string(i == 0 ? "CAM-A" : "CAM-B") + " image corner r^2 from the nominal K");
    }
}

// --write-fixture <new directory>: the inputs of the recovery scenario for a
// manual end-to-end run (DraftCalibrator, SyntheticPairGenerator, StitchEval):
// session.json, job.json, pair-spec.json (the true cameras on an A0 chart) and
// truth-profile.json (the estimated draft with the true K, lens and H).
void WriteFixture(const std::filesystem::path& directory) {
    const auto scenario = MakeScenario(0, 1, 0, 0);
    const auto session = Session(scenario), job = Job(scenario);
    const auto output = RunPlanarCalibrationJob(session, job);
    if (!output.ok || !std::filesystem::create_directory(directory)) throw std::runtime_error("fixture");
    Write(directory / "session.json", session);
    Write(directory / "job.json", job);
    std::string truth = draft_json::ProfileHead("synthetic-planar-truth", DraftPaperOrientation::Landscape, DraftCameraOrder::LeftRight);
    std::string spec = R"({"schema":"a0.m2.synthetic-pair-spec/2","seed":374374,"image":{"width_px":7360,"height_px":4912,"supersample":2},"jpeg":{"quality":0.95,"chroma_subsampling":"444"},"chart":{"width_mm":1189,"height_mm":841,"margin_mm":20,"grid_pitch_mm":10,"grid_line_width_mm":0.3,"fiducial_pitch_mm":50,"fiducial_radius_mm":7,"slanted_edge_slope":0.0875,"fine_line_base_width_mm":0.2},"cameras":[)";
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& t = scenario.truth[i];
        const PinholeIntrinsics k{t.k.fx_pixels, t.k.fy_pixels, t.k.cx_pixels, t.k.cy_pixels};
        const BrownConradyDistortion d{t.d.k1, t.d.k2, t.d.k3, t.d.p1, t.d.p2};
        if (i) { truth += ','; spec += ','; }
        truth += draft_json::CameraProjection(i == 0 ? CameraAlias::CamA : CameraAlias::CamB, k, d, t.sheet_to_image);
        spec += std::string("{\"alias\":") + (i == 0 ? "\"CAM-A\"" : "\"CAM-B\"") + ",\"projection\":{\"intrinsics\":" + draft_json::K(k)
            + ",\"distortion\":" + draft_json::D(d) + ",\"documentToImage\":" + draft_json::Matrix(t.sheet_to_image)
            + R"(},"exposure_gain":1.0,"white_balance_gain":[1.0,1.0,1.0],"vignette":[0,0],"noise":{"read_sigma":0.004,"shot_sigma":0.006}})";
    }
    const auto calibration = output.profile_json.find("},\"calibration\":");
    Write(directory / "truth-profile.json", truth + output.profile_json.substr(calibration));
    Write(directory / "pair-spec.json", spec + "]}");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--write-fixture") { WriteFixture(argv[2]); return 0; }
        if (argc != 2) throw std::invalid_argument("usage: <DraftCalibrator.exe> | --write-fixture <new directory>");
        TestRecovery(argv[1]);
        TestNominalMismatch();
        TestSeam();
        TestRejections();
    } catch (const std::exception& error) {
        std::cerr << "exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
