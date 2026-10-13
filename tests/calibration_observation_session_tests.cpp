#include "a0/m2/calibration_observation_session.hpp"

#include "a0/common/protocol_json.hpp"
#include "calibration_exact_predicates.hpp"
#include "calibration_session_json_scan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Contract test of the calibration observation session reader (Issue #286).
// Arguments: <fixtures-dir> [--limits]. The test writes no file. Every input is
// a hand-written fixture or built here from numbers; no product code, generator,
// profile reader or image is involved.
namespace {
using namespace a0::m2::calibration;
namespace pj = a0::common::protocol_json;

int checks = 0, failures = 0;
void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

struct TestJsonFailure {
    [[noreturn]] static void Fail(std::string_view code, std::string_view) {
        throw std::runtime_error("test JSON: " + std::string(code));
    }
};
using TestParser = pj::BasicJsonParser<TestJsonFailure, 2U * 1024U * 1024U>;
pj::JsonValue ParseDocument(const std::string& text) { return TestParser(text).Parse(); }
std::string Serialize(const pj::JsonValue& value) { return pj::SerializeJsonWith<TestJsonFailure>(value); }

std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Rejection category: the text between "calibration observation session: " and " at ".
// Empty if Parse accepted the document.
std::string Category(const std::string& json) {
    constexpr std::string_view prefix = "calibration observation session: ";
    try {
        (void)CalibrationObservationSession::Parse(json);
        return {};
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        if (message.compare(0, prefix.size(), prefix) != 0) return "<foreign message>";
        const auto at = message.find(" at /", prefix.size());
        if (at == std::string::npos) return "<no pointer>";
        return message.substr(prefix.size(), at - prefix.size());
    } catch (...) {
        return "<foreign exception>";
    }
}
void RejectsWith(const std::string& json, const std::string& category, const std::string& label) {
    const auto actual = Category(json);
    Check(actual == category, label + " (expected " + category + ", got " + (actual.empty() ? "accepted" : actual) + ")");
}
void Accepts(const std::string& json, const std::string& label) {
    const auto actual = Category(json);
    Check(actual.empty(), label + " (rejected as " + actual + ")");
}
// The whole rejection message, or an empty string if Parse accepted the document.
std::string Message(const std::string& json) {
    try {
        (void)CalibrationObservationSession::Parse(json);
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    } catch (...) {
        return "<foreign exception>";
    }
}
void RejectsAt(const std::string& json, const std::string& category, const std::string& pointer, const std::string& label) {
    const auto expected = "calibration observation session: " + category + " at " + pointer;
    const auto actual = Message(json);
    Check(actual == expected, label + " (expected '" + expected + "', got '" + (actual.empty() ? std::string("accepted") : actual) + "')");
}

std::vector<std::string> Split(const std::string& pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) return tokens;
    std::size_t start = 1;
    for (;;) {
        const auto slash = pointer.find('/', start);
        tokens.push_back(pointer.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return tokens;
}

// add / replace / remove on a JSON Pointer. A failing operation is a failure of
// the test fixture, never a rejection by the reader.
void ApplyOperation(pj::JsonValue& root, const std::string& kind, const std::string& pointer, const pj::JsonValue* value) {
    const auto tokens = Split(pointer);
    pj::JsonValue* node = &root;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (node->kind == pj::JsonKind::object) node = &node->object.at(tokens[index]);
        else node = &node->array.at(std::stoul(tokens[index]));
    }
    if (tokens.empty()) throw std::runtime_error("empty pointer");
    const auto& last = tokens.back();
    if (node->kind == pj::JsonKind::object) {
        if (kind == "add") {
            if (!node->object.emplace(last, *value).second) throw std::runtime_error("add onto existing key " + pointer);
        } else if (kind == "replace") {
            auto found = node->object.find(last);
            if (found == node->object.end()) throw std::runtime_error("replace of missing key " + pointer);
            found->second = *value;
        } else if (kind == "remove") {
            if (node->object.erase(last) != 1) throw std::runtime_error("remove of missing key " + pointer);
        } else throw std::runtime_error("unknown operation " + kind);
    } else if (node->kind == pj::JsonKind::array) {
        auto& array = node->array;
        if (kind == "add") {
            const auto position = last == "-" ? array.size() : std::stoul(last);
            if (position > array.size()) throw std::runtime_error("add beyond array end " + pointer);
            array.insert(array.begin() + static_cast<std::ptrdiff_t>(position), *value);
        } else if (kind == "replace") {
            array.at(std::stoul(last)) = *value;
        } else if (kind == "remove") {
            const auto position = std::stoul(last);
            (void)array.at(position); // bounds check before erase
            array.erase(array.begin() + static_cast<std::ptrdiff_t>(position));
        } else throw std::runtime_error("unknown operation " + kind);
    } else throw std::runtime_error("operation parent is a scalar " + pointer);
}

struct Operation { std::string kind, pointer; pj::JsonValue value; };
std::string Mutate(const pj::JsonValue& base, const std::vector<Operation>& operations) {
    auto copy = base;
    for (const auto& operation : operations) ApplyOperation(copy, operation.kind, operation.pointer, &operation.value);
    return Serialize(copy);
}
pj::JsonValue Literal(const std::string& text) { return ParseDocument(text); }
std::string MutateOne(const pj::JsonValue& base, const std::string& kind, const std::string& pointer, const std::string& value_json = "null") {
    return Mutate(base, {{kind, pointer, Literal(value_json)}});
}
std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// ---------------------------------------------------------------------------
// Synthetic session builder used by the diagnostic and limit tests.
std::string Num(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}
std::string Hash(unsigned number) {
    char tail[16];
    std::snprintf(tail, sizeof tail, "%08x", number);
    return std::string(56, '0') + tail;
}

struct ChartPt { std::string id; double x, y; };
struct ViewSpec {
    std::string id;
    std::string camera = "CAM-A";
    std::string role = "training";
    std::vector<std::string> observed;     // observation point ids, input order
    std::vector<std::string> used;         // usedPointIds, or posePointIds for validation
    std::vector<std::string> measurement;  // validation only
    std::map<std::string, std::pair<double, double>> pixel_override;
    std::map<std::string, std::string> state_override; // "missing" or "ambiguous"
};

std::string Join(const std::vector<std::string>& items) {
    std::string text;
    for (std::size_t index = 0; index < items.size(); ++index) text += (index ? "," : "") + items[index];
    return text;
}
std::string IdList(const std::vector<std::string>& ids) {
    std::vector<std::string> quoted;
    for (const auto& id : ids) quoted.push_back(Quote(id));
    return "[" + Join(quoted) + "]";
}
std::string RenderView(const ViewSpec& view, unsigned hash_number) {
    std::vector<std::string> observations;
    for (std::size_t index = 0; index < view.observed.size(); ++index) {
        const auto& id = view.observed[index];
        const auto state = view.state_override.find(id);
        if (state != view.state_override.end()) {
            observations.push_back("{\"pointId\":" + Quote(id) + ",\"state\":" + Quote(state->second) + ",\"pixel\":null}");
            continue;
        }
        auto pixel = std::pair<double, double>{static_cast<double>(index % 60), static_cast<double>(index / 60)};
        const auto override_found = view.pixel_override.find(id);
        if (override_found != view.pixel_override.end()) pixel = override_found->second;
        observations.push_back("{\"pointId\":" + Quote(id) + ",\"state\":\"detected\",\"pixel\":{\"x\":" + Num(pixel.first)
            + ",\"y\":" + Num(pixel.second) + "}}");
    }
    std::string text = "{\"viewId\":" + Quote(view.id) + ",\"camera\":" + Quote(view.camera) + ",\"imageSha256\":" + Quote(Hash(hash_number))
        + ",\"widthPixels\":1024,\"heightPixels\":1024,\"role\":" + Quote(view.role) + ",";
    if (view.role == "validation") {
        text += "\"posePointIds\":" + IdList(view.used) + ",\"measurementPointIds\":" + IdList(view.measurement) + ",";
    } else {
        text += "\"usedPointIds\":" + IdList(view.used) + ",";
    }
    return text + "\"observations\":[" + Join(observations) + "]}";
}
std::string CameraJson(std::uint32_t width, std::uint32_t height, const std::string& focus) {
    return "{\"widthPixels\":" + std::to_string(width) + ",\"heightPixels\":" + std::to_string(height)
        + ",\"lensLabel\":\"synthetic-lens\",\"focalLengthLabel\":\"f50mm\",\"focusLabel\":" + Quote(focus) + "}";
}
std::string ChartJson(const std::vector<ChartPt>& chart) {
    std::vector<std::string> items;
    for (const auto& point : chart) {
        items.push_back("{\"pointId\":" + Quote(point.id) + ",\"xMm\":" + Num(point.x) + ",\"yMm\":" + Num(point.y) + "}");
    }
    return "[" + Join(items) + "]";
}
std::string SessionText(const std::string& chart_json, const std::string& cameras_json, const std::string& views_json) {
    return "{\"schemaVersion\":\"a0.calibration-observation-session.v1\",\"sessionId\":\"synthetic-session\","
        "\"chartId\":\"synthetic-chart\",\"provenance\":{\"measuredAt\":\"2026-01-01T00:00:00Z\","
        "\"tool\":{\"toolId\":\"synthetic-fixture\",\"version\":\"1.0.0\"}},\"chartPoints\":" + chart_json
        + ",\"cameras\":" + cameras_json + ",\"views\":" + views_json + "}";
}
std::string CamerasJson(std::uint32_t width = 1024, std::uint32_t height = 1024) {
    return "{\"CAM-A\":" + CameraJson(width, height, "fixed-a") + ",\"CAM-B\":" + CameraJson(width, height, "fixed-b") + "}";
}

// Test points go first; five filler points far away complete the chart so that
// it is never degenerate, and the default views use only the fillers so that
// they never produce a diagnostic of their own.
struct Fixture {
    std::vector<ChartPt> chart;
    std::vector<ViewSpec> views;
    bool reference_a = true, validation_a = true, reference_b = true, validation_b = true;
};
std::string Build(const Fixture& fixture) {
    auto chart = fixture.chart;
    chart.push_back({"G1", 10000, 10000});
    chart.push_back({"G2", 10010, 10000});
    chart.push_back({"G3", 10000, 10020});
    chart.push_back({"G4", 10030, 10050});
    chart.push_back({"G5", 10007, 10013});
    std::vector<std::string> views_json;
    unsigned hash_number = 1;
    for (const auto& view : fixture.views) views_json.push_back(RenderView(view, hash_number++));
    const std::vector<std::string> fillers{"G1", "G2", "G3", "G4"};
    const auto add_reference = [&](const std::string& camera, const std::string& id) {
        ViewSpec view{id, camera, "reference", {"G1", "G2", "G3", "G4", "G5"}, fillers, {}, {}, {}};
        views_json.push_back(RenderView(view, hash_number++));
    };
    const auto add_validation = [&](const std::string& camera, const std::string& id) {
        ViewSpec view{id, camera, "validation", {"G1", "G2", "G3", "G4", "G5"}, fillers, {"G5"}, {}, {}};
        views_json.push_back(RenderView(view, hash_number++));
    };
    if (fixture.reference_a) add_reference("CAM-A", "DEFAULT-REF-A");
    if (fixture.validation_a) add_validation("CAM-A", "DEFAULT-VAL-A");
    if (fixture.reference_b) add_reference("CAM-B", "DEFAULT-REF-B");
    if (fixture.validation_b) add_validation("CAM-B", "DEFAULT-VAL-B");
    return SessionText(ChartJson(chart), CamerasJson(), "[" + Join(views_json) + "]");
}

std::vector<std::string> Ids(const std::vector<ChartPt>& points) {
    std::vector<std::string> ids;
    for (const auto& point : points) ids.push_back(point.id);
    return ids;
}
ViewSpec TrainingView(const std::string& id, const std::vector<ChartPt>& points, const std::string& camera = "CAM-A") {
    ViewSpec view;
    view.id = id;
    view.camera = camera;
    view.observed = Ids(points);
    view.used = view.observed;
    return view;
}
std::vector<ChartPt> Points(const std::vector<std::pair<double, double>>& coordinates, const std::string& prefix = "T") {
    std::vector<ChartPt> points;
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        points.push_back({prefix + std::to_string(index + 1), coordinates[index].first, coordinates[index].second});
    }
    return points;
}

using Diagnostics = std::vector<SessionDiagnostic>;
Diagnostics DiagnoseText(const std::string& json) { return DiagnoseObservationSession(CalibrationObservationSession::Parse(json)); }
std::string Describe(const Diagnostics& diagnostics) {
    std::string text = "[";
    for (const auto& item : diagnostics) {
        text += std::string(DiagnosticCode(item.code)) + "/" + std::string(CameraAliasCode(item.camera)) + "/" + item.view_id.value_or("-") + " ";
    }
    return text + "]";
}
void ExpectDiagnostics(const std::string& json, const Diagnostics& expected, const std::string& label) {
    try {
        const auto actual = DiagnoseText(json);
        Check(actual == expected, label + " (expected " + Describe(expected) + ", got " + Describe(actual) + ")");
    } catch (const std::exception& error) {
        Check(false, label + " threw " + error.what());
    }
}
SessionDiagnostic ViewDiag(SessionDiagnosticCode code, const std::string& view, CameraAlias camera = CameraAlias::CamA) {
    return {code, camera, view};
}

// ---------------------------------------------------------------------------
// A: accepted documents
void Accepted(const std::string& valid, const pj::JsonValue& base) {
    Accepts(valid, "A1 valid-minimal parses");
    CalibrationObservationSession first = CalibrationObservationSession::Parse(valid);
    CalibrationObservationSession second = CalibrationObservationSession::Parse(valid);
    Check(DiagnoseObservationSession(first).empty(), "A3 valid-minimal has no diagnostics");

    // A4: two reads agree on every accessor and on the diagnostics.
    Check(first.SessionId() == second.SessionId() && first.ChartId() == second.ChartId(), "A4 ids agree");
    Check(first.Provenance() == second.Provenance(), "A4 provenance agrees");
    Check(std::equal(first.ChartPoints().begin(), first.ChartPoints().end(), second.ChartPoints().begin(), second.ChartPoints().end()), "A4 chart agrees");
    Check(std::equal(first.Views().begin(), first.Views().end(), second.Views().begin(), second.Views().end()), "A4 views agree");
    Check(first.Camera(CameraAlias::CamA) == second.Camera(CameraAlias::CamA)
        && first.Camera(CameraAlias::CamB) == second.Camera(CameraAlias::CamB), "A4 cameras agree");
    Check(DiagnoseObservationSession(first) == DiagnoseObservationSession(second), "A4 diagnostics agree");

    // A5: spot values.
    Check(first.SessionId() == "synthetic-minimal-session" && first.ChartId() == "synthetic-chart-6", "A5 ids");
    Check(first.Views().size() == 6, "A5 six views");
    Check(first.ChartPoints().size() == 6, "A5 six chart points");
    const auto& b_training = first.Views()[3];
    Check(b_training.id == "B-T1" && b_training.camera == CameraAlias::CamB && b_training.role == ViewRole::Training, "A5 B-T1 identity");
    Check(b_training.observations.size() == 6 && b_training.observations[1].point_id == "P2"
        && b_training.observations[1].state == ObservationState::Missing && !b_training.observations[1].pixel, "A5 missing observation has no pixel");
    Check(b_training.observations[4].state == ObservationState::Ambiguous && !b_training.observations[4].pixel, "A5 ambiguous observation has no pixel");
    Check(b_training.observations[0].pixel && b_training.observations[0].pixel->x == 4.5, "A5 detected pixel kept");
    Check(first.Provenance().tool.version == "1.0.0" && first.Provenance().tool.id == "synthetic-fixture", "A5 tool");
    Check(first.Provenance().measured_at_utc == "2026-01-01T00:00:00Z", "A5 measuredAt");
    Check(first.Camera(CameraAlias::CamB).focus_label == "fixed-b" && first.Camera(CameraAlias::CamA).width_pixels == 64, "A5 camera group");
    const auto* p6 = first.FindChartPoint("P6");
    Check(p6 != nullptr && p6->x_mm == 100 && p6->y_mm == 40, "A5 P6");
    Check(first.FindChartPoint("P7") == nullptr, "A5 P7 absent");
    const auto& validation = first.Views()[1];
    Check(validation.role == ViewRole::Validation && validation.pose_point_ids.size() == 4 && validation.measurement_point_ids.size() == 2
        && validation.used_point_ids.empty(), "A5 validation point lists");
    Check(first.Views()[0].pose_point_ids.empty() && first.Views()[0].used_point_ids.size() == 4, "A5 training point list");

    // A6: a negative zero becomes a positive zero.
    {
        const auto session = CalibrationObservationSession::Parse(MutateOne(base, "replace", "/views/0/observations/0/pixel/x", "-0"));
        Check(std::bit_cast<std::uint64_t>(session.Views()[0].observations[0].pixel->x) == 0, "A6 -0 pixel is +0");
        const auto chart = CalibrationObservationSession::Parse(MutateOne(base, "replace", "/chartPoints/0/xMm", "-0"));
        Check(std::bit_cast<std::uint64_t>(chart.ChartPoints()[0].x_mm) == 0, "A6 -0 chart coordinate is +0");
    }
    // A7: leap day.
    Accepts(MutateOne(base, "replace", "/provenance/measuredAt", "\"2028-02-29T00:00:00Z\""), "A7 leap day accepted");
    Accepts(MutateOne(base, "replace", "/provenance/measuredAt", "\"0001-01-01T00:00:00Z\""), "A7 year 1 accepted");
    RejectsWith(MutateOne(base, "replace", "/provenance/measuredAt", "\"2027-02-29T00:00:00Z\""), "utc", "A7 non-leap February 29 rejected");
    RejectsWith(MutateOne(base, "replace", "/provenance/measuredAt", "\"0000-01-01T00:00:00Z\""), "utc", "A7 year 0 rejected");
    // A8: an out-of-range enum value.
    {
        bool threw = false;
        try { (void)first.Camera(static_cast<CameraAlias>(7)); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "A8 out-of-range camera alias throws");
        Check(CameraAliasCode(static_cast<CameraAlias>(7)).empty() && ViewRoleCode(static_cast<ViewRole>(9)).empty()
            && ObservationStateCode(static_cast<ObservationState>(9)).empty() && DiagnosticCode(static_cast<SessionDiagnosticCode>(99)).empty(),
            "A8 out-of-range enum codes are empty");
    }
    // Extremes: a subnormal and the largest finite coordinate are exact numbers, not errors.
    {
        const auto tiny = CalibrationObservationSession::Parse(MutateOne(base, "replace", "/chartPoints/5/yMm", "5e-324"));
        Check(std::bit_cast<std::uint64_t>(tiny.ChartPoints()[5].y_mm) == 1, "R3 subnormal 5e-324 keeps its bits");
        const auto huge = CalibrationObservationSession::Parse(MutateOne(base, "replace", "/chartPoints/5/xMm", "1.7976931348623157e308"));
        Check(huge.ChartPoints()[5].x_mm == std::numeric_limits<double>::max(), "largest finite coordinate accepted");
        Accepts(MutateOne(base, "replace", "/views/0/observations/0/pixel/x", "-0.5"), "D8 pixel x = -0.5");
        Accepts(MutateOne(base, "replace", "/views/0/observations/0/pixel/x", "63.5"), "D8 pixel x = W - 0.5");
        Accepts(MutateOne(base, "replace", "/views/0/observations/0/pixel/y", "47.5"), "D8 pixel y = H - 0.5");
    }
    // Code strings.
    Check(DiagnosticCode(SessionDiagnosticCode::InsufficientObservations) == "insufficient-observations"
        && DiagnosticCode(SessionDiagnosticCode::DegeneratePointConfiguration) == "degenerate-point-configuration"
        && DiagnosticCode(SessionDiagnosticCode::AmbiguousCorrespondence) == "ambiguous-correspondence"
        && DiagnosticCode(SessionDiagnosticCode::ReferenceViewMissing) == "reference-view-missing"
        && DiagnosticCode(SessionDiagnosticCode::ValidationViewMissing) == "validation-view-missing", "diagnostic code strings");
}

// ---------------------------------------------------------------------------
// B: structural rejections
void Rejected(const std::string& dir, const std::string& valid, const pj::JsonValue& base) {
    const std::set<std::string> expected_items{"unknown-field", "schema-version", "non-finite", "camera-alias", "dimension", "image-hash",
        "chart-point", "undefined-point", "used-list", "detected-pixel", "unresolved-pixel", "used-not-detected", "validation-overlap",
        "validation-shape", "reference-duplicate", "truth", "identifier", "identifier-boundary", "measured-at"};
    const auto table = ParseDocument(ReadFile(dir + "/negative-cases.json"));
    std::set<std::string> items, ids;
    for (const auto& entry : table.array) {
        const auto& id = entry.object.at("id").string;
        Check(ids.insert(id).second, "table id is unique: " + id);
        items.insert(entry.object.at("item").string);
        const auto& schema = entry.object.at("schema").string;
        Check(schema == "reject" || schema == "accept" || schema == "skip", "table schema column is valid: " + id);
        std::vector<Operation> operations;
        for (const auto& operation : entry.object.at("ops").array) {
            const auto value = operation.object.find("value");
            operations.push_back({operation.object.at("op").string, operation.object.at("path").string,
                value == operation.object.end() ? pj::JsonValue{} : value->second});
        }
        try {
            const auto mutated = Mutate(base, operations);
            const auto& category = entry.object.at("category").string;
            RejectsWith(mutated, category, "negative case " + id);
            // The pointer pins the reason: a case that fails elsewhere for the same category is caught.
            const auto pointer = entry.object.find("pointer");
            Check(pointer != entry.object.end(), "table row names the expected pointer: " + id);
            if (pointer != entry.object.end()) RejectsAt(mutated, category, pointer->second.string, "negative case " + id + " pointer");
        } catch (const std::exception& error) {
            Check(false, "negative case " + id + " could not be built: " + error.what());
        }
    }
    Check(items == expected_items, "table covers exactly the structural rejection items");
    Check(table.array.size() >= 70, "table has its expected size");

    // Every truth key at every kind of position.
    const std::array<std::string, 10> truth_keys{"groundTruth", "ground_truth", "intrinsics", "distortion", "projection",
        "documentToImage", "image_px", "document_mm", "reference_points", "generator_version"};
    const std::array<std::string, 6> places{"", "/provenance", "/chartPoints/0", "/cameras/CAM-A", "/views/0", "/views/0/observations/0"};
    for (const auto& key : truth_keys) {
        for (const auto& place : places) {
            RejectsWith(MutateOne(base, "add", place + "/" + key, "1"), "truth-field", "truth key " + key + " at '" + place + "'");
        }
    }
    // The field names of the session never collide with the truth keys.
    Accepts(valid, "session field names are not truth keys");

    // Documents that are not a value of the table.
    const auto insert_after = [&](const std::string& anchor, const std::string& text) {
        auto copy = valid;
        const auto position = copy.find(anchor);
        Check(position != std::string::npos, "anchor present: " + anchor);
        copy.insert(position + anchor.size(), text);
        return copy;
    };
    RejectsWith(insert_after("{\n", "  \"sessionId\": \"dup\",\n"), "json-duplicate-key", "BT1 duplicate key");
    RejectsWith(insert_after("{\n", "  \"session\\u0049d\": \"dup\",\n"), "json-duplicate-key", "BT2 duplicate key after unescaping");
    RejectsWith(insert_after("\"cameras\": {", "\"CAM-A\": {},"), "json-duplicate-key", "BT3 duplicate camera");
    RejectsWith(valid + " null", "json-syntax", "BT4 trailing data");
    RejectsWith(std::string("\xEF\xBB\xBF") + valid, "json-syntax", "BT5 byte order mark");
    {
        auto nan = valid;
        nan.replace(nan.find("\"xMm\": 0"), 8, "\"xMm\": NaN");
        RejectsWith(nan, "json-syntax", "BT6 NaN literal");
        auto infinity = valid;
        infinity.replace(infinity.find("\"xMm\": 0"), 8, "\"xMm\": Infinity");
        RejectsWith(infinity, "json-syntax", "BT6 Infinity literal");
        auto leading_zero = valid;
        leading_zero.replace(leading_zero.find("\"xMm\": 0"), 8, "\"xMm\": 01");
        RejectsWith(leading_zero, "json-syntax", "BT6 leading zero");
    }
    RejectsWith(ReadFile(dir + "/invalid-generator-ground-truth.json"), "truth-field", "BT7 generator ground truth");
    RejectsWith("", "json-byte-limit", "BT8 empty document");
    RejectsWith("[]", "type", "root is not an object");
    RejectsWith("null", "type", "root is null");

    // A message never carries input text.
    {
        const std::string secret = "UnknownKeyZzz";
        try {
            (void)CalibrationObservationSession::Parse(MutateOne(base, "add", "/" + secret, "1"));
            Check(false, "unknown key rejected");
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            Check(message.find(secret) == std::string::npos && message.size() < 120, "message omits an unknown key name");
        }
        try {
            (void)CalibrationObservationSession::Parse(MutateOne(base, "replace", "/sessionId", "\"secret value here\""));
            Check(false, "bad identifier rejected");
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            Check(message.find("secret") == std::string::npos && message == "calibration observation session: identifier at /sessionId",
                "message omits an input value");
        }
        try {
            (void)CalibrationObservationSession::Parse(MutateOne(base, "replace", "/views/0/observations/0/pixel/x", "64"));
            Check(false, "pixel range rejected");
        } catch (const std::invalid_argument& error) {
            Check(std::string(error.what()) == "calibration observation session: pixel-range at /views/0/observations/0/pixel/x",
                "pointer names the offending field");
        }
    }
}

// ---------------------------------------------------------------------------
// C: diagnostics
void Diagnostic() {
    using Code = SessionDiagnosticCode;
    const auto two_chain = [](const std::vector<std::pair<double, double>>& coordinates, bool reverse = false) {
        auto points = Points(coordinates);
        if (reverse) std::reverse(points.begin(), points.end());
        Fixture fixture;
        fixture.chart = points;
        fixture.views.push_back(TrainingView("V1", points));
        return Build(fixture);
    };
    // C1
    ExpectDiagnostics(two_chain({{0, 0}, {1, 0}, {0, 1}}), {ViewDiag(Code::InsufficientObservations, "V1")}, "C1 three points");
    // C2-C6 and their reversals (C7).
    struct Case { const char* name; std::vector<std::pair<double, double>> points; Diagnostics expected; };
    const std::vector<Case> cases{
        {"C2 five collinear points", {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}}, {ViewDiag(Code::DegeneratePointConfiguration, "V1")}},
        {"C3 four collinear plus one", {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}}, {ViewDiag(Code::DegeneratePointConfiguration, "V1")}},
        {"C4 three collinear plus two", {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 2}}, {}},
        {"C5a one ulp off the line", {{0.5, 0.5000000000000001}, {12, 12}, {24, 24}, {36, 36}, {1, 0}}, {}},
        {"C5b exactly on the line", {{0.5, 0.5}, {12, 12}, {24, 24}, {36, 36}, {1, 0}}, {ViewDiag(Code::DegeneratePointConfiguration, "V1")}},
        {"C6 exact collinearity a double formula misses",
            {{0.3619410566761061, 0.599179702575384}, {1.583748231833741, 1.595060556447672}, {4.027362582149011, 3.5868222641922483}, {0, 5}},
            {ViewDiag(Code::DegeneratePointConfiguration, "V1")}},
    };
    for (const auto& item : cases) {
        ExpectDiagnostics(two_chain(item.points), item.expected, item.name);
        ExpectDiagnostics(two_chain(item.points, true), item.expected, std::string(item.name) + " reversed");
    }
    // C8: same pixel for distinct ids.
    const auto square = std::vector<std::pair<double, double>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const auto with_pixels = [&](const std::map<std::string, std::pair<double, double>>& pixels, std::size_t extra_points = 0) {
        auto coordinates = square;
        for (std::size_t index = 0; index < extra_points; ++index) coordinates.push_back({5.0 + static_cast<double>(index), 7.0 * static_cast<double>(index + 1) + 0.5});
        auto points = Points(coordinates);
        Fixture fixture;
        fixture.chart = points;
        auto view = TrainingView("V1", points);
        view.used = {"T1", "T2", "T3", "T4"};
        view.pixel_override = pixels;
        fixture.views.push_back(view);
        return Build(fixture);
    };
    ExpectDiagnostics(with_pixels({{"T1", {10, 10}}, {"T2", {10, 10}}}), {ViewDiag(Code::AmbiguousCorrespondence, "V1")}, "C8a same pixel");
    ExpectDiagnostics(with_pixels({{"T1", {0, 5}}, {"T2", {-0.0, 5}}}), {ViewDiag(Code::AmbiguousCorrespondence, "V1")}, "C8b -0 equals +0");
    ExpectDiagnostics(with_pixels({{"T1", {10, 10}}, {"T2", {10, 10.000000000000002}}}), {}, "C8c one ulp apart is not ambiguous");
    ExpectDiagnostics(with_pixels({{"T5", {20, 20}}, {"T6", {20, 20}}}, 2), {ViewDiag(Code::AmbiguousCorrespondence, "V1")},
        "C8d same pixel on points outside the used list");
    // C9: camera-level diagnostics.
    {
        Fixture fixture;
        fixture.reference_a = false;
        ExpectDiagnostics(Build(fixture), {{Code::ReferenceViewMissing, CameraAlias::CamA, std::nullopt}}, "C9a reference missing for CAM-A");
        fixture = Fixture{};
        fixture.validation_b = false;
        ExpectDiagnostics(Build(fixture), {{Code::ValidationViewMissing, CameraAlias::CamB, std::nullopt}}, "C9b validation missing for CAM-B");
        fixture = Fixture{};
        fixture.reference_a = fixture.validation_a = fixture.reference_b = fixture.validation_b = false;
        ExpectDiagnostics(Build(fixture), {
            {Code::ReferenceViewMissing, CameraAlias::CamA, std::nullopt}, {Code::ValidationViewMissing, CameraAlias::CamA, std::nullopt},
            {Code::ReferenceViewMissing, CameraAlias::CamB, std::nullopt}, {Code::ValidationViewMissing, CameraAlias::CamB, std::nullopt}},
            "C9c no views at all");
    }
    // C10: a validation view counts its pose points only.
    {
        auto points = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 3}});
        Fixture fixture;
        fixture.chart = points;
        ViewSpec view;
        view.id = "VAL1";
        view.role = "validation";
        view.observed = Ids(points);
        view.used = {"T1", "T2", "T3"};
        view.measurement = {"T4", "T5"};
        fixture.views.push_back(view);
        fixture.validation_a = false;
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::InsufficientObservations, "VAL1")}, "C10 three pose points");
        // The same pose points with collinear measurement points raise nothing.
        auto collinear = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {3, 0}});
        fixture.chart = collinear;
        view.observed = Ids(collinear);
        view.used = {"T1", "T2", "T3", "T4"};
        view.measurement = {"T5", "T6"};
        fixture.views = {view};
        ExpectDiagnostics(Build(fixture), {}, "C10 measurement points do not count");
    }
    // C11: a mixture comes out in the documented order.
    {
        auto points = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}});
        Fixture fixture;
        fixture.chart = points;
        auto few = TrainingView("V1", points);
        few.used = {"T1", "T2", "T3"};
        auto same = TrainingView("V2", points);
        same.pixel_override = {{"T1", {10, 10}}, {"T2", {10, 10}}};
        fixture.views = {few, same};
        fixture.reference_a = false;
        ExpectDiagnostics(Build(fixture), {
            {Code::ReferenceViewMissing, CameraAlias::CamA, std::nullopt},
            ViewDiag(Code::InsufficientObservations, "V1"), ViewDiag(Code::AmbiguousCorrespondence, "V2")}, "C11 documented order");
        // C13: one view with both, insufficient first.
        auto both = TrainingView("V3", points);
        both.used = {"T1", "T2", "T3"};
        both.pixel_override = {{"T1", {10, 10}}, {"T2", {10, 10}}};
        fixture.views = {both};
        ExpectDiagnostics(Build(fixture), {
            {Code::ReferenceViewMissing, CameraAlias::CamA, std::nullopt},
            ViewDiag(Code::InsufficientObservations, "V3"), ViewDiag(Code::AmbiguousCorrespondence, "V3")}, "C13 two codes on one view");
    }
    // A view of a different camera keeps its own camera in the diagnostic.
    {
        auto points = Points({{0, 0}, {1, 0}, {0, 1}});
        Fixture fixture;
        fixture.chart = points;
        fixture.views = {TrainingView("VB", points, "CAM-B")};
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::InsufficientObservations, "VB", CameraAlias::CamB)}, "diagnostic carries the camera of the view");
    }
}

// ---------------------------------------------------------------------------
// D: resource limits that are cheap to build
std::string Repeat(char c, std::size_t count) { return std::string(count, c); }

void Limits(const pj::JsonValue& base) {
    // D2: nesting depth, counted in containers including the root.
    {
        const auto nested = [](int arrays) { return "{\"x\":" + std::string(static_cast<std::size_t>(arrays), '[') + "0" + std::string(static_cast<std::size_t>(arrays), ']') + "}"; };
        RejectsWith(nested(31), "unknown-field", "D2 32 nested containers pass the JSON layer");
        RejectsWith(nested(32), "json-syntax", "D2 33 nested containers fail the JSON layer");
        static_assert(kMaximumObservationSessionJsonDepth == 32);
    }
    // D3: chart points.
    {
        const auto grid = [](std::size_t count) {
            std::vector<ChartPt> chart;
            for (std::size_t index = 0; index < count; ++index) chart.push_back({"G" + std::to_string(index), static_cast<double>(index % 128), static_cast<double>(index / 128)});
            return SessionText(ChartJson(chart), CamerasJson(), "[]");
        };
        Accepts(grid(kMaximumChartPoints), "D3 16384 chart points accepted");
        auto over = std::vector<ChartPt>();
        for (std::size_t index = 0; index < kMaximumChartPoints; ++index) over.push_back({"G" + std::to_string(index), static_cast<double>(index % 128), static_cast<double>(index / 128)});
        over.push_back({"EXTRA", 1000, 1000});
        RejectsWith(SessionText(ChartJson(over), CamerasJson(), "[]"), "limit-chart-points", "D3 16385 chart points rejected");
        RejectsWith(SessionText(ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}}), CamerasJson(), "[]"), "chart-degenerate", "chart of three points rejected");
        Accepts(SessionText(ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}}), CamerasJson(), "[]"), "chart of four points accepted");
    }
    // D4: views per camera.
    {
        const auto views = [](const std::string& camera, std::size_t count) {
            std::vector<std::string> rendered;
            for (std::size_t index = 0; index < count; ++index) {
                ViewSpec view;
                view.id = "V" + std::to_string(index);
                view.camera = camera;
                rendered.push_back(RenderView(view, static_cast<unsigned>(index + 1)));
            }
            return "[" + Join(rendered) + "]";
        };
        const auto chart = ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}});
        for (const std::string camera : {"CAM-A", "CAM-B"}) {
            Accepts(SessionText(chart, CamerasJson(), views(camera, kMaximumViewsPerCamera)), "D4 128 views on " + camera + " accepted");
            RejectsWith(SessionText(chart, CamerasJson(), views(camera, kMaximumViewsPerCamera + 1)), "limit-views-per-camera",
                "D4 129 views on " + camera + " rejected");
        }
    }
    // D5b: one view holding more observations than the per-view limit.
    {
        const auto chart = ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}});
        const auto observations = [](std::size_t count) {
            std::string text = "[";
            for (std::size_t index = 0; index < count; ++index) text += (index ? "," : "") + std::string("{\"pointId\":\"A\",\"state\":\"missing\",\"pixel\":null}");
            return text + "]";
        };
        const auto view = [&](std::size_t count) {
            return "[{\"viewId\":\"V1\",\"camera\":\"CAM-A\",\"imageSha256\":" + Quote(Hash(1)) + ",\"widthPixels\":1024,\"heightPixels\":1024,"
                "\"role\":\"training\",\"usedPointIds\":[],\"observations\":" + observations(count) + "}]";
        };
        RejectsWith(SessionText(chart, CamerasJson(), view(kMaximumObservationsPerView + 1)), "limit-observations", "D5b one view over the per-view limit");
        RejectsWith(SessionText(chart, CamerasJson(), view(2)), "duplicate-id", "D5b the same point twice in a view");
    }
    // D6: image sides.
    {
        const auto chart = ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}});
        const auto session = [&](std::uint32_t width, std::uint32_t height, const std::string& view_width, const std::string& view_height) {
            ViewSpec view{"V1", "CAM-A", "training", {}, {}, {}, {}, {}};
            auto text = RenderView(view, 1);
            text.replace(text.find("\"widthPixels\":1024"), 18, "\"widthPixels\":" + view_width);
            text.replace(text.find("\"heightPixels\":1024"), 19, "\"heightPixels\":" + view_height);
            return SessionText(chart, CamerasJson(width, height), "[" + text + "]");
        };
        Accepts(session(1, 1, "1", "1"), "D6 sides of 1 accepted");
        Accepts(session(32768, 32768, "32768", "32768"), "D6 sides of 32768 accepted");
        RejectsWith(session(0, 1, "1", "1"), "integer", "D6 camera width 0");
        RejectsWith(session(1, 0, "1", "1"), "integer", "D6 camera height 0");
        RejectsWith(session(32769, 1, "1", "1"), "integer", "D6 camera width 32769");
        RejectsWith(session(1, 32769, "1", "1"), "integer", "D6 camera height 32769");
        RejectsWith(session(1, 1, "0", "1"), "integer", "D6 view width 0");
        RejectsWith(session(1, 1, "1", "32769"), "integer", "D6 view height 32769");
        RejectsWith(session(1, 1, "2", "1"), "dimension-mismatch", "D6 view wider than its camera");
    }
    // D7: identifier length.
    {
        const auto at = [&](const std::string& pointer, std::size_t length) { return MutateOne(base, "replace", pointer, Quote(Repeat('a', length))); };
        for (const std::string pointer : {"/sessionId", "/chartId", "/cameras/CAM-A/lensLabel", "/cameras/CAM-A/focalLengthLabel", "/cameras/CAM-A/focusLabel",
                 "/views/0/viewId", "/provenance/tool/version", "/provenance/tool/toolId"}) {
            Accepts(at(pointer, kMaximumIdentifierLength), "D7 128 characters at " + pointer);
            RejectsWith(at(pointer, kMaximumIdentifierLength + 1), "identifier", "D7 129 characters at " + pointer);
        }
        // A point id of 128 characters, referenced from an observation and a point list.
        const auto long_id = Repeat('p', kMaximumIdentifierLength);
        Fixture fixture;
        fixture.chart = {{long_id, 0, 0}, {"T2", 1, 0}, {"T3", 0, 1}, {"T4", 1, 1}};
        fixture.views.push_back(TrainingView("V1", fixture.chart));
        Accepts(Build(fixture), "D7 128-character point id");
        fixture.chart[0].id = Repeat('p', kMaximumIdentifierLength + 1);
        fixture.views.clear();
        RejectsWith(Build(fixture), "identifier", "D7 129-character point id");
        // The tool identifiers may hold a dot; the session identifiers may not.
        Accepts(MutateOne(base, "replace", "/provenance/tool/toolId", "\"tool.v1\""), "D7 dot in toolId");
        RejectsWith(MutateOne(base, "replace", "/cameras/CAM-A/focusLabel", "\"a.b\""), "identifier", "D7 dot in a label");
    }
}

// D1 and D5: the large documents.
void HeavyLimits(const std::string& valid) {
    using clock = std::chrono::steady_clock;
    const auto milliseconds = [](clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - from).count();
    };
    // D1: the byte limit is checked before parsing.
    {
        const auto started = clock::now();
        std::string padded = valid + std::string(kMaximumObservationSessionJsonBytes - valid.size(), ' ');
        Check(padded.size() == kMaximumObservationSessionJsonBytes, "D1 document built at the limit");
        Accepts(padded, "D1 exactly 8 MiB accepted");
        padded.push_back(' ');
        RejectsWith(padded, "json-byte-limit", "D1 8 MiB + 1 rejected (valid content)");
        RejectsWith(std::string(kMaximumObservationSessionJsonBytes + 1, '['), "json-byte-limit", "D1 8 MiB + 1 of '[' rejected before parsing");
        std::cout << "D1 elapsed_ms=" << milliseconds(started) << '\n';
    }
    // D5: the observation total, summed over views.
    {
        const auto started = clock::now();
        std::vector<ChartPt> chart;
        chart.reserve(kMaximumChartPoints);
        for (std::size_t index = 0; index < kMaximumChartPoints; ++index) {
            chart.push_back({"G" + std::to_string(index), static_cast<double>(index % 128), static_cast<double>(index / 128)});
        }
        const auto observations = [&](std::size_t count) {
            std::string text = "[";
            for (std::size_t index = 0; index < count; ++index) {
                text += (index ? "," : "") + std::string("{\"pointId\":\"G") + std::to_string(index) + "\",\"state\":\"missing\",\"pixel\":null}";
            }
            return text + "]";
        };
        const auto view = [&](unsigned number, const std::string& observations_json) {
            return "{\"viewId\":\"V" + std::to_string(number) + "\",\"camera\":\"CAM-A\",\"imageSha256\":" + Quote(Hash(number))
                + ",\"widthPixels\":1024,\"heightPixels\":1024,\"role\":\"training\",\"usedPointIds\":[],\"observations\":" + observations_json + "}";
        };
        static_assert(kMaximumObservationsPerSession % kMaximumObservationsPerView == 0);
        constexpr std::size_t full_views = kMaximumObservationsPerSession / kMaximumObservationsPerView;
        const auto full = observations(kMaximumObservationsPerView);
        std::string views = "[";
        for (unsigned number = 1; number <= full_views; ++number) views += (number > 1 ? "," : "") + view(number, full);
        const auto chart_json = ChartJson(chart);
        const std::string at_limit = SessionText(chart_json, CamerasJson(), views + "]");
        Check(at_limit.size() <= kMaximumObservationSessionJsonBytes, "D5 document at the observation limit fits the byte limit");
        std::cout << "D5 document_bytes=" << at_limit.size() << '\n';
        const auto parsed_at = clock::now();
        Accepts(at_limit, "D5 65536 observations accepted");
        std::cout << "D5 accept_parse_ms=" << milliseconds(parsed_at) << '\n';
        const std::string over = SessionText(chart_json, CamerasJson(),
            views + "," + view(static_cast<unsigned>(full_views + 1), "[{\"pointId\":\"G0\",\"state\":\"missing\",\"pixel\":null}]") + "]");
        Check(over.size() <= kMaximumObservationSessionJsonBytes, "D5 over-limit document fits the byte limit");
        RejectsWith(over, "limit-observations", "D5 65537 observations rejected");
        std::cout << "D5 elapsed_ms=" << milliseconds(started) << '\n';
    }
}

// ---------------------------------------------------------------------------
// S: exact predicates shared with the geometry preflight
void Predicates() {
    using namespace a0::m2::calibration::exact_detail;
    const Matrix3x3 sequence{1, 2, 3, 4, 5, 6, 7, 8, 9};
    Check(!ExactNonzeroDeterminant(sequence), "S1 singular integer matrix");
    for (const int exponent : {-1060, -500, 500, 900}) {
        Matrix3x3 scaled = sequence;
        for (auto& value : scaled) value = std::ldexp(value, exponent);
        Check(!ExactNonzeroDeterminant(scaled), "S2 scaled singular matrix 2^" + std::to_string(exponent));
    }
    Check(ExactNonzeroDeterminant({1, 2, 3, 4, 5, 6, 7, 8, 10}), "S3 nonsingular matrix");
    const auto naive = [](double ax, double ay, double bx, double by, double cx, double cy) { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); };
    Check(!ExactCollinear(0.5, 0.5000000000000001, 12, 12, 24, 24), "S4 one ulp off the line is not collinear");
    Check(ExactCollinear(0.5, 0.5, 12, 12, 24, 24), "S4 exact line");
    Check(ExactCollinear(0.3619410566761061, 0.599179702575384, 1.583748231833741, 1.595060556447672, 4.027362582149011, 3.5868222641922483),
        "S4 exactly collinear binary64 points");
    Check(naive(0.5, 0.5000000000000001, 12, 12, 24, 24) == 0, "S5 a double formula misses the one-ulp offset");
    Check(naive(0.5, 0.5, 12, 12, 24, 24) == 0, "S5 a double formula agrees on the exact line");
    Check(naive(0.3619410566761061, 0.599179702575384, 1.583748231833741, 1.595060556447672, 4.027362582149011, 3.5868222641922483) != 0,
        "S5 a double formula reports a false offset");
    Check(ExactCollinear(0, 0, 5e-324, 0, 1e-323, 0) && !ExactCollinear(0, 0, 5e-324, 0, 0, 5e-324), "S6 subnormal coordinates");
    Check(ExactCollinear(-1e308, -1e308, 0, 0, 1e308, 1e308) && !ExactCollinear(-1e308, -1e308, 0, 0, 1e308, 9.999999999999998e+307),
        "S7 coordinates near the largest finite value");
    Check(ExactCollinear(1, 1, 1, 1, 7, 9), "S8 coincident points are collinear");
}

// Orientation of three points on a small integer grid, found with integers only,
// against the exact test (a cross check that does not use the code under test).
void GridCrossCheck() {
    std::size_t mismatches = 0, cases = 0;
    for (int ax = 0; ax < 4; ++ax) for (int ay = 0; ay < 4; ++ay)
        for (int bx = 0; bx < 4; ++bx) for (int by = 0; by < 4; ++by)
            for (int cx = 0; cx < 4; ++cx) for (int cy = 0; cy < 4; ++cy) {
                const bool integer_collinear = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) == 0;
                const bool exact = a0::m2::calibration::exact_detail::ExactCollinear(ax, ay, bx, by, cx, cy);
                ++cases;
                if (integer_collinear != exact) ++mismatches;
            }
    Check(cases == 4096 && mismatches == 0, "S9 integer grid agrees with an integer determinant");
}

// The linear-time general-position test against a brute-force search over all
// quadruples, observed through the chart-degenerate rejection of Parse. Points
// come from a 4 x 4 integer grid, where exact collinearity is common.
void GeneralPositionCrossCheck() {
    std::uint32_t state = 12345;
    const auto next = [&state](std::uint32_t bound) {
        state = state * 1664525U + 1013904223U;
        return (state >> 8) % bound;
    };
    const auto cross = [](const ChartPt& a, const ChartPt& b, const ChartPt& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    std::size_t degenerate = 0, general = 0, mismatches = 0;
    for (int round = 0; round < 600; ++round) {
        std::vector<int> cells(16);
        for (int index = 0; index < 16; ++index) cells[static_cast<std::size_t>(index)] = index;
        std::vector<int> chosen;
        if (round % 3 == 1) {
            // A whole row, plus at most one more point: always degenerate.
            const int row = static_cast<int>(next(4));
            for (int column = 0; column < 4; ++column) chosen.push_back(row * 4 + column);
            std::erase_if(cells, [row](int cell) { return cell / 4 == row; });
            if (next(2) == 1) chosen.push_back(cells[next(static_cast<std::uint32_t>(cells.size()))]);
        } else {
            const std::uint32_t size = 4 + next(5); // 4..8 points
            for (std::uint32_t index = 0; index < size; ++index) {
                const auto pick = next(static_cast<std::uint32_t>(cells.size()));
                chosen.push_back(cells[pick]);
                cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(pick));
            }
        }
        std::vector<ChartPt> chart;
        for (std::size_t index = 0; index < chosen.size(); ++index) {
            chart.push_back({"Q" + std::to_string(index), static_cast<double>(chosen[index] % 4), static_cast<double>(chosen[index] / 4)});
        }
        bool brute_force = false; // some four points with no three collinear
        for (std::size_t a = 0; a < chart.size() && !brute_force; ++a)
            for (std::size_t b = a + 1; b < chart.size() && !brute_force; ++b)
                for (std::size_t c = b + 1; c < chart.size() && !brute_force; ++c)
                    for (std::size_t d = c + 1; d < chart.size() && !brute_force; ++d)
                        brute_force = cross(chart[a], chart[b], chart[c]) != 0 && cross(chart[a], chart[b], chart[d]) != 0
                            && cross(chart[a], chart[c], chart[d]) != 0 && cross(chart[b], chart[c], chart[d]) != 0;
        const auto category = Category(SessionText(ChartJson(chart), CamerasJson(), "[]"));
        const bool rejected_as_degenerate = category == "chart-degenerate";
        if (rejected_as_degenerate == brute_force || (!rejected_as_degenerate && !category.empty())) ++mismatches;
        (brute_force ? general : degenerate)++;
    }
    Check(mismatches == 0, "S10 linear-time test agrees with brute force on " + std::to_string(general + degenerate) + " point sets");
    Check(general > 50 && degenerate > 50, "S10 both outcomes occur in the random sets (" + std::to_string(general) + " general, " + std::to_string(degenerate) + " degenerate)");
}
// ---------------------------------------------------------------------------
// X: additions that pin what the table and the cheap limits leave open: the
// pointer of every rejection, the constants themselves, camera-specific image
// sizes, view totals, list rules, scalar spellings and one-ulp geometry.
void PinnedLimits() {
    Check(kMaximumObservationSessionJsonBytes == 8388608U && kMaximumObservationSessionJsonDepth == 32 && kMaximumChartPoints == 16384U
        && kMaximumViewsPerCamera == 128U && kMaximumObservationsPerView == 16384U && kMaximumObservationsPerSession == 65536U
        && kMinimumImageSidePixels == 1U && kMaximumImageSidePixels == 32768U && kMaximumIdentifierLength == 128U && kMinimumMappingPoints == 4U,
        "X0 limit constants keep the documented values");
}

double Lexeme(const std::string& text) {
    double value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ptr != text.data() + text.size()) throw std::runtime_error("bad number lexeme " + text);
    return value;
}

std::string SizedView(const ViewSpec& view, unsigned hash_number, const std::string& width, const std::string& height) {
    auto text = RenderView(view, hash_number);
    text.replace(text.find("\"widthPixels\":1024"), 18, "\"widthPixels\":" + width);
    text.replace(text.find("\"heightPixels\":1024"), 19, "\"heightPixels\":" + height);
    return text;
}

void PixelBoundaries(const pj::JsonValue& base) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const std::string x_pointer = "/views/0/observations/0/pixel/x", y_pointer = "/views/0/observations/0/pixel/y";
    // The table spells the one-ulp neighbours as decimal text; make sure the text really is one ulp away.
    Check(Lexeme("63.50000000000001") == std::nextafter(63.5, inf), "X1 B38 lexeme is one ulp above W - 0.5");
    Check(Lexeme("-0.5000000000000001") == std::nextafter(-0.5, -inf), "X1 B39 lexeme is one ulp below -0.5");
    Check(Lexeme("47.50000000000001") == std::nextafter(47.5, inf), "X1 B40 lexeme is one ulp above H - 0.5");
    const auto at = [&](const std::string& pointer, double value) { return MutateOne(base, "replace", pointer, Num(value)); };
    // The 64 x 48 view: all four edges, both sides.
    Accepts(at(y_pointer, -0.5), "X1 y = -0.5 accepted");
    Accepts(at(x_pointer, std::nextafter(-0.5, 0.0)), "X1 x one ulp inside the low edge accepted");
    Accepts(at(x_pointer, std::nextafter(63.5, 0.0)), "X1 x one ulp inside the high edge accepted");
    Accepts(at(y_pointer, std::nextafter(47.5, 0.0)), "X1 y one ulp inside the high edge accepted");
    RejectsAt(at(y_pointer, std::nextafter(-0.5, -inf)), "pixel-range", y_pointer, "X1 y one ulp below the low edge");
    RejectsAt(at(x_pointer, std::nextafter(-0.5, -inf)), "pixel-range", x_pointer, "X1 x one ulp below the low edge");
    RejectsAt(at(x_pointer, std::nextafter(63.5, inf)), "pixel-range", x_pointer, "X1 x one ulp above the high edge");
    RejectsAt(at(y_pointer, std::nextafter(47.5, inf)), "pixel-range", y_pointer, "X1 y one ulp above the high edge");
    RejectsAt(at(x_pointer, 1e300), "pixel-range", x_pointer, "X1 x far above");
    RejectsAt(at(y_pointer, -1e300), "pixel-range", y_pointer, "X1 y far below");
    // The largest allowed image: the exact edge W - 0.5 = 32767.5.
    const auto points = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    const auto big = [&](double x, double y) {
        auto view = TrainingView("V1", points);
        view.pixel_override = {{"T1", {x, y}}};
        return SessionText(ChartJson(points), CamerasJson(32768, 32768), "[" + SizedView(view, 1, "32768", "32768") + "]");
    };
    Accepts(big(32767.5, 32767.5), "X1 32768 image: pixel at W - 0.5");
    Accepts(big(std::nextafter(32767.5, 0.0), std::nextafter(32767.5, 0.0)), "X1 32768 image: one ulp inside");
    RejectsAt(big(std::nextafter(32767.5, inf), 0), "pixel-range", x_pointer, "X1 32768 image: x one ulp outside");
    RejectsAt(big(0, std::nextafter(32767.5, inf)), "pixel-range", y_pointer, "X1 32768 image: y one ulp outside");
    // A zero is a zero however it is spelled; only a vanishing non-zero value is refused.
    for (const char* zero : {"0", "0.0", "-0.0", "0e5", "0e-400", "-0e400"}) {
        Accepts(MutateOne(base, "replace", x_pointer, zero), std::string("X1 zero spelled ") + zero);
    }
}

void CameraSpecificDimensions() {
    const auto points = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    const auto cameras = "{\"CAM-A\":" + CameraJson(64, 48, "fixed-a") + ",\"CAM-B\":" + CameraJson(32, 24, "fixed-b") + "}";
    const auto session = [&](const ViewSpec& a, const char* a_width, const char* a_height, const ViewSpec& b, const char* b_width, const char* b_height) {
        return SessionText(ChartJson(points), cameras, "[" + SizedView(a, 1, a_width, a_height) + "," + SizedView(b, 2, b_width, b_height) + "]");
    };
    const auto view_a = TrainingView("VA", points, "CAM-A");
    const auto view_b = TrainingView("VB", points, "CAM-B");
    Accepts(session(view_a, "64", "48", view_b, "32", "24"), "X2 each view matches its own camera group");
    RejectsAt(session(view_a, "64", "48", view_b, "64", "48"), "dimension-mismatch", "/views/1/widthPixels", "X2 CAM-B view with the CAM-A width");
    RejectsAt(session(view_a, "32", "24", view_b, "32", "24"), "dimension-mismatch", "/views/0/widthPixels", "X2 CAM-A view with the CAM-B width");
    RejectsAt(session(view_a, "64", "47", view_b, "32", "24"), "dimension-mismatch", "/views/0/heightPixels", "X2 CAM-A view one row short");
    RejectsAt(session(view_a, "64", "48", view_b, "32", "25"), "dimension-mismatch", "/views/1/heightPixels", "X2 CAM-B view one row tall");
    RejectsAt(session(view_a, "63", "48", view_b, "32", "24"), "dimension-mismatch", "/views/0/widthPixels", "X2 CAM-A view one column narrow");
    // The pixel range follows the image size of the view's own camera.
    auto wide_a = view_a;
    wide_a.pixel_override = {{"T1", {40, 1}}};
    Accepts(session(wide_a, "64", "48", view_b, "32", "24"), "X2 x = 40 fits the 64 pixel wide CAM-A image");
    auto wide_b = view_b;
    wide_b.pixel_override = {{"T1", {40, 1}}};
    RejectsAt(session(view_a, "64", "48", wide_b, "32", "24"), "pixel-range", "/views/1/observations/0/pixel/x", "X2 x = 40 is outside the 32 pixel wide CAM-B image");
    auto tall_b = view_b;
    tall_b.pixel_override = {{"T1", {1, 24}}};
    RejectsAt(session(view_a, "64", "48", tall_b, "32", "24"), "pixel-range", "/views/1/observations/0/pixel/y", "X2 y = 24 is outside the 24 pixel tall CAM-B image");
}

void ViewTotals() {
    const auto chart = ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}});
    const auto make = [](const std::string& camera, std::size_t first, std::size_t count) {
        std::vector<std::string> rendered;
        for (std::size_t index = 0; index < count; ++index) {
            ViewSpec view;
            view.id = "V" + std::to_string(first + index);
            view.camera = camera;
            rendered.push_back(RenderView(view, static_cast<unsigned>(first + index + 1)));
        }
        return rendered;
    };
    const auto join = [](std::vector<std::string> left, const std::vector<std::string>& right) {
        left.insert(left.end(), right.begin(), right.end());
        return "[" + Join(left) + "]";
    };
    Accepts(SessionText(chart, CamerasJson(), join(make("CAM-A", 0, 128), make("CAM-B", 128, 128))), "X3 128 + 128 views accepted");
    RejectsAt(SessionText(chart, CamerasJson(), join(make("CAM-A", 0, 129), make("CAM-B", 129, 127))), "limit-views-per-camera", "/views/128",
        "X3 129 views on CAM-A in a total of 256");
    RejectsAt(SessionText(chart, CamerasJson(), join(make("CAM-B", 0, 129), make("CAM-A", 129, 127))), "limit-views-per-camera", "/views/128",
        "X3 129 views on CAM-B in a total of 256");
    RejectsAt(SessionText(chart, CamerasJson(), join(make("CAM-A", 0, 128), make("CAM-B", 128, 129))), "limit-views-per-camera", "/views",
        "X3 257 views in total");
}

void ListAndUniquenessRules(const pj::JsonValue& base) {
    const auto change = [&](const std::string& kind, const std::string& pointer, const std::string& value) { return MutateOne(base, kind, pointer, value); };
    RejectsAt(change("replace", "/views/1/viewId", "\"A-T1\""), "duplicate-id", "/views/1/viewId", "X4 duplicate viewId");
    RejectsAt(change("replace", "/views/1/posePointIds", "[\"P1\",\"P1\",\"P3\",\"P4\"]"), "duplicate-id", "/views/1/posePointIds/1", "X4 duplicate pose point");
    RejectsAt(change("replace", "/views/1/measurementPointIds", "[\"P2\",\"P2\"]"), "duplicate-id", "/views/1/measurementPointIds/1", "X4 duplicate measurement point");
    RejectsAt(change("replace", "/views/1/observations/0", "{\"pointId\":\"P1\",\"state\":\"missing\",\"pixel\":null}"), "used-not-detected",
        "/views/1/posePointIds/0", "X4 pose point that is missing");
    RejectsAt(change("replace", "/views/2/observations/0", "{\"pointId\":\"P1\",\"state\":\"ambiguous\",\"pixel\":null}"), "used-not-detected",
        "/views/2/usedPointIds/0", "X4 reference point that is ambiguous");
    RejectsAt(change("remove", "/views/1/observations/0", "null"), "undefined-point", "/views/1/posePointIds/0", "X4 pose point without an observation");
    RejectsAt(change("remove", "/views/1/observations/1", "null"), "undefined-point", "/views/1/measurementPointIds/0", "X4 measurement point without an observation");
    RejectsAt(change("replace", "/views/1/measurementPointIds", "[\"P2\",\"P3\"]"), "validation-overlap", "/views/1/measurementPointIds/1", "X4 measurement point that is a pose point");
    RejectsAt(change("remove", "/cameras/CAM-A", "null"), "missing-field", "/cameras", "X4 CAM-A missing");
    RejectsAt(change("replace", "/views/0/imageSha256", Quote(Repeat('g', 64))), "sha256", "/views/0/imageSha256", "X4 hash with a non-hex digit");
    RejectsAt(change("replace", "/views/0/imageSha256", Quote(Repeat('1', 65))), "sha256", "/views/0/imageSha256", "X4 hash with 65 digits");
    RejectsAt(change("replace", "/views/0/imageSha256", "1"), "type", "/views/0/imageSha256", "X4 hash that is a number");
    RejectsAt(change("replace", "/chartPoints/1/xMm", "\"1\""), "type", "/chartPoints/1/xMm", "X4 coordinate that is a string");
    RejectsAt(change("replace", "/chartPoints", "{}"), "type", "/chartPoints", "X4 chartPoints that is an object");
    RejectsAt(change("replace", "/views", "{}"), "type", "/views", "X4 views that is an object");
    // A name that differs from another only by one ulp of position is a different place.
    Accepts(SessionText(ChartJson({{"A", 0, 0}, {"B", 1, 0}, {"C", 0, 1}, {"D", 1, 1}, {"E", 1, std::nextafter(1.0, 2.0)}}), CamerasJson(), "[]"),
        "X4 chart positions one ulp apart are distinct");
}

void ScalarRules(const pj::JsonValue& base) {
    const auto change = [&](const std::string& kind, const std::string& pointer, const std::string& value) { return MutateOne(base, kind, pointer, value); };
    // Integer spellings.
    for (const char* spelling : {"-1", "4294967296", "4294967297", "18446744073709551616", "1E1", "1.5", "0", "1e0"}) {
        RejectsAt(change("replace", "/cameras/CAM-A/heightPixels", spelling), "integer", "/cameras/CAM-A/heightPixels", std::string("X5 height ") + spelling);
        RejectsAt(change("replace", "/views/0/widthPixels", spelling), "integer", "/views/0/widthPixels", std::string("X5 view width ") + spelling);
    }
    for (const char* spelling : {"true", "null", "[]"}) {
        RejectsAt(change("replace", "/cameras/CAM-A/heightPixels", spelling), "type", "/cameras/CAM-A/heightPixels", std::string("X5 height ") + spelling);
    }
    // UTC.
    const std::string at = "/provenance/measuredAt";
    for (const char* text : {"2026-01-01t00:00:00Z", "2026-01-01T00:00:00z", "2026-01-01 00:00:00Z", "2026-01-01T00:60:00Z", "2026-01-01T00:00:60Z",
             "2026-01-00T00:00:00Z", "2026-00-01T00:00:00Z", "2100-02-29T00:00:00Z", "2026-04-31T00:00:00Z", "2026-06-31T00:00:00Z",
             "2026-01-01T0a:00:00Z", "2026-01-01T00:00:00Z ", "2026-01-01T00:00:00", "", "2026-01-01"}) {
        RejectsAt(change("replace", at, Quote(text)), "utc", at, std::string("X5 measuredAt '") + text + "'");
    }
    for (const char* text : {"2000-02-29T00:00:00Z", "2026-12-31T23:59:59Z", "2026-04-30T00:00:00Z", "9999-12-31T23:59:59Z", "2026-01-31T00:00:00Z"}) {
        Accepts(change("replace", at, Quote(text)), std::string("X5 measuredAt '") + text + "'");
    }
    RejectsAt(change("replace", at, "20260101"), "type", at, "X5 measuredAt that is a number");
    // Identifiers.
    RejectsAt(change("replace", "/sessionId", "\"P1\\n\""), "identifier", "/sessionId", "X5 identifier with a trailing newline");
    RejectsAt(change("replace", "/sessionId", "5"), "type", "/sessionId", "X5 sessionId that is a number");
    RejectsAt(change("replace", "/provenance/tool/version", "\".1\""), "identifier", "/provenance/tool/version", "X5 version starting with a dot");
    RejectsAt(change("replace", "/cameras/CAM-A/focalLengthLabel", "\"f 50\""), "identifier", "/cameras/CAM-A/focalLengthLabel", "X5 focal label with a space");
    RejectsAt(change("replace", "/cameras/CAM-A/focusLabel", "\"\""), "identifier", "/cameras/CAM-A/focusLabel", "X5 empty focus label");
    RejectsAt(change("replace", "/cameras/CAM-B/lensLabel", "\"a/b\""), "identifier", "/cameras/CAM-B/lensLabel", "X5 CAM-B lens label with a slash");
    RejectsAt(change("replace", "/chartId", "\"chart\\u00e9\""), "identifier", "/chartId", "X5 chartId with a non-ASCII letter");
    for (const char* good : {"A", "9", "a-", "a_b-c", "Z9"}) {
        Accepts(change("replace", "/sessionId", Quote(good)), std::string("X5 identifier '") + good + "' accepted");
    }
    {
        // 129 characters in an observation or a point list, where the chart holds the 128 character id.
        const auto long_id = Repeat('p', kMaximumIdentifierLength);
        const auto too_long = Repeat('p', kMaximumIdentifierLength + 1);
        Fixture fixture;
        fixture.chart = {{long_id, 0, 0}, {"T2", 1, 0}, {"T3", 0, 1}, {"T4", 1, 1}};
        auto view = TrainingView("V1", fixture.chart);
        fixture.views = {view};
        Accepts(Build(fixture), "X5 128 character point id in all places");
        fixture.views[0].observed[0] = too_long;
        RejectsAt(Build(fixture), "identifier", "/views/0/observations/0/pointId", "X5 129 character id in an observation");
        fixture.views[0] = view;
        fixture.views[0].used[0] = too_long;
        RejectsAt(Build(fixture), "identifier", "/views/0/usedPointIds/0", "X5 129 character id in a point list");
    }
    // Truth keys are keys only: the same words as values or as identifiers are ordinary text.
    for (const char* word : {"image_px", "document_mm", "generator_version", "groundTruth", "intrinsics", "reference_points"}) {
        Accepts(change("replace", "/sessionId", Quote(word)), std::string("X5 sessionId '") + word + "' is a value, not a key");
        Accepts(change("replace", "/views/0/viewId", Quote(word)), std::string("X5 viewId '") + word + "' is a value, not a key");
    }
    // Number spellings that JSON allows all reach the same value.
    for (const char* spelling : {"4E1", "4e+1", "40.0", "0.4e2", "4.0E+1", "400e-1"}) {
        const auto session = CalibrationObservationSession::Parse(change("replace", "/chartPoints/5/yMm", spelling));
        Check(session.ChartPoints()[5].y_mm == 40.0, std::string("X5 number spelled ") + spelling);
    }
    // No chart at all is a degenerate chart, not a crash.
    RejectsAt(change("replace", "/chartPoints", "[]"), "chart-degenerate", "/chartPoints", "X5 empty chart");
    // Pixel and observation shapes.
    const std::string pixel = "/views/0/observations/0/pixel";
    RejectsAt(change("remove", pixel + "/x", "null"), "missing-field", pixel, "X5 pixel without x");
    RejectsAt(change("remove", pixel + "/y", "null"), "missing-field", pixel, "X5 pixel without y");
    RejectsAt(change("replace", pixel, "5"), "type", pixel, "X5 detected pixel that is a number");
    RejectsAt(change("replace", pixel + "/x", "\"5\""), "type", pixel + "/x", "X5 pixel x that is a string");
    RejectsAt(change("replace", pixel + "/y", "null"), "type", pixel + "/y", "X5 pixel y that is null");
    for (const char* shape : {"5", "{}", "[]", "false", "\"\""}) {
        RejectsAt(change("replace", "/views/3/observations/1/pixel", shape), "state-pixel", "/views/3/observations/1/pixel", std::string("X5 missing observation with pixel ") + shape);
        RejectsAt(change("replace", "/views/3/observations/4/pixel", shape), "state-pixel", "/views/3/observations/4/pixel", std::string("X5 ambiguous observation with pixel ") + shape);
    }
    RejectsAt(change("replace", "/views/0/observations/0/state", "\"Detected\""), "state", "/views/0/observations/0/state", "X5 state in capital letters");
    RejectsAt(change("replace", "/views/0/observations/0/state", "\"lost\""), "state", "/views/0/observations/0/state", "X5 unknown state");
    RejectsAt(change("replace", "/views/0/observations/0/state", "1"), "type", "/views/0/observations/0/state", "X5 state that is a number");
    RejectsAt(change("remove", "/views/0/observations/0/state", "null"), "missing-field", "/views/0/observations/0", "X5 observation without state");
    RejectsAt(change("replace", "/views/0/observations/0/pointId", "5"), "type", "/views/0/observations/0/pointId", "X5 pointId that is a number");
}

// Exactly collinear points, and the same points with one coordinate moved by one
// ulp, at several magnitudes. Moving one coordinate of a point on a line by any
// non-zero amount takes the point off the line, so the expected outcome needs
// no second implementation of the predicate.
void OneUlpMatrix() {
    using Code = SessionDiagnosticCode;
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (const int exponent : {-1000, -600, 0, 600, 1000}) {
        const double s = std::ldexp(1.0, exponent);
        for (int variant = 0; variant < 5; ++variant) {
            ChartPt c{"T3", 2 * s, 2 * s};
            if (variant == 1) c.x = std::nextafter(c.x, inf);
            if (variant == 2) c.x = std::nextafter(c.x, -inf);
            if (variant == 3) c.y = std::nextafter(c.y, inf);
            if (variant == 4) c.y = std::nextafter(c.y, -inf);
            const std::vector<ChartPt> points{{"T1", 0, 0}, {"T2", s, s}, c, {"T4", 4 * s, 4 * s}, {"T5", s, 0}};
            const std::string label = "X6 2^" + std::to_string(exponent) + " variant " + std::to_string(variant);
            const std::string expected_chart = variant == 0 ? "chart-degenerate" : "";
            const auto chart_category = Category(SessionText(ChartJson(points), CamerasJson(), "[]"));
            Check(chart_category == expected_chart, label + " as a whole chart (got '" + chart_category + "')");
            Fixture fixture;
            fixture.chart = points;
            fixture.views.push_back(TrainingView("V1", points));
            const Diagnostics expected = variant == 0 ? Diagnostics{ViewDiag(Code::DegeneratePointConfiguration, "V1")} : Diagnostics{};
            ExpectDiagnostics(Build(fixture), expected, label + " as a view");
        }
    }
}

void MoreDiagnostics() {
    using Code = SessionDiagnosticCode;
    // A validation view is judged by its pose points: four collinear pose points are degenerate
    // although the measurement point is off the line.
    {
        const auto points = Points({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {0, 1}});
        Fixture fixture;
        fixture.chart = points;
        ViewSpec view;
        view.id = "VAL1";
        view.role = "validation";
        view.observed = Ids(points);
        view.used = {"T1", "T2", "T3", "T4"};
        view.measurement = {"T5"};
        fixture.views = {view};
        fixture.validation_a = false;
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::DegeneratePointConfiguration, "VAL1")}, "X7 validation view with collinear pose points");
        // Same view with pose points in general position: nothing, even though T5 is collinear with T1-T3.
        const auto square = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 2}});
        fixture.chart = square;
        fixture.views[0].observed = Ids(square);
        fixture.views[0].used = {"T1", "T2", "T3", "T4"};
        fixture.views[0].measurement = {"T5"};
        ExpectDiagnostics(Build(fixture), {}, "X7 validation view with pose points in general position");
        // Two measurement points on one pixel are still an ambiguous correspondence.
        fixture.views[0].pixel_override = {{"T1", {20, 20}}, {"T5", {20, 20}}};
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::AmbiguousCorrespondence, "VAL1")}, "X7 shared pixel between a pose point and a measurement point");
    }
    // A reference view is judged like a training view.
    {
        Fixture fixture;
        fixture.reference_a = false;
        const auto three = Points({{0, 0}, {1, 0}, {0, 1}});
        fixture.chart = three;
        auto reference = TrainingView("REF1", three);
        reference.role = "reference";
        fixture.views = {reference};
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::InsufficientObservations, "REF1")}, "X7 reference view with three points");
        const auto line = Points({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}});
        fixture.chart = line;
        reference = TrainingView("REF1", line);
        reference.role = "reference";
        fixture.views = {reference};
        ExpectDiagnostics(Build(fixture), {ViewDiag(Code::DegeneratePointConfiguration, "REF1")}, "X7 reference view on one line");
    }
    // The same pixel in two different views is not ambiguous; only a view's own observations count.
    {
        const auto square = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}});
        Fixture fixture;
        fixture.chart = square;
        auto first = TrainingView("V1", square);
        first.pixel_override = {{"T1", {10, 10}}};
        auto second = TrainingView("V2", square);
        second.pixel_override = {{"T2", {10, 10}}};
        fixture.views = {first, second};
        ExpectDiagnostics(Build(fixture), {}, "X7 the same pixel in two views");
        // Missing and ambiguous observations carry no pixel and never collide.
        const auto six = Points({{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 3}, {3, 5}});
        fixture.chart = six;
        auto third = TrainingView("V3", six);
        third.used = {"T1", "T2", "T3", "T4"};
        third.state_override = {{"T5", "missing"}, {"T6", "ambiguous"}};
        fixture.views = {third};
        ExpectDiagnostics(Build(fixture), {}, "X7 observations without a pixel never collide");
    }
}

// V: the value count that is bounded on the text, before any tree is built.
// The most values a legal session can hold, from the limit constants. The terms:
//   22                      root, schemaVersion, sessionId, chartId, provenance (5 values),
//                           cameras (13 values)
//   1 + 4 * charts          the chartPoints array, and per point the object and 3 members
//   1 + 8 * views           the views array, and per view the object, 6 scalars and the
//                           observations array
//   2 * views               up to two point-list arrays per view (validation)
//   6 * observations        per detected observation: object, pointId, state, pixel object, x, y
//   1 * observations        point-list elements: each names a distinct detected observation of
//                           its view, and a validation view's two lists are disjoint
constexpr std::size_t kLegalMaximumValues = 22
    + (1 + 4 * kMaximumChartPoints)
    + (1 + 8 * (2 * kMaximumViewsPerCamera))
    + 2 * (2 * kMaximumViewsPerCamera)
    + 6 * kMaximumObservationsPerSession
    + kMaximumObservationsPerSession;
static_assert(kLegalMaximumValues == 526872);
static_assert(kLegalMaximumValues <= kMaximumObservationSessionJsonValues);
// The scan counts one more for each empty container. Every view frame can carry
// at most 11 counted slots whatever its role (a validation view with an empty
// posePointIds keeps all its elements in measurementPointIds; a training view with
// empty observations and an empty point list adds two), so the legal maximum of the
// bound is 256 above the legal maximum of the values.
constexpr std::size_t kLegalMaximumBound = 22
    + (1 + 4 * kMaximumChartPoints)
    + (1 + 11 * (2 * kMaximumViewsPerCamera))
    + 7 * kMaximumObservationsPerSession;
static_assert(kLegalMaximumBound == kLegalMaximumValues + 2 * kMaximumViewsPerCamera);
static_assert(kLegalMaximumBound == 527128);
static_assert(kLegalMaximumBound <= kMaximumObservationSessionJsonValues);
static_assert(kMaximumObservationSessionJsonValues / 2 < kLegalMaximumBound, "the limit is the smallest power of two that holds the legal maximum");
static_assert((kMaximumObservationSessionJsonValues & (kMaximumObservationSessionJsonValues - 1)) == 0);

// {"x":[0,0,...,0]} whose upper bound is exactly `values`: the root, one more for each of the
// two containers, and one for each comma between the (values - 2) zeros.
std::string WideDocument(std::size_t values) {
    const std::size_t zeros = values - 2;
    std::string text = "{\"x\":[";
    text.reserve(2 * zeros + 8);
    for (std::size_t index = 0; index < zeros; ++index) {
        if (index != 0) text += ',';
        text += '0';
    }
    return text + "]}";
}

void CountNodes(const pj::JsonValue& value, std::size_t& nodes, std::size_t& empty_containers) {
    ++nodes;
    if (value.kind == pj::JsonKind::object) {
        if (value.object.empty()) ++empty_containers;
        for (const auto& member : value.object) CountNodes(member.second, nodes, empty_containers);
    } else if (value.kind == pj::JsonKind::array) {
        if (value.array.empty()) ++empty_containers;
        for (const auto& element : value.array) CountNodes(element, nodes, empty_containers);
    }
}

void ValueBudget(const std::string& valid, const pj::JsonValue& base) {
    using detail::JsonValueCountUpperBound;
    const std::size_t limit = kMaximumObservationSessionJsonValues;
    Check(limit == 1048576U, "V0 value limit keeps its documented value");
    // The count on small texts: containers and commas count, string contents and whitespace do not.
    struct Sample { const char* text; std::size_t expected; };
    const Sample samples[] = {
        {"0", 1}, {"[]", 2}, {"{}", 2}, {"[0,0]", 3}, {"{\"a\":[1,2]}", 4}, {"[ 1 ,\n 2 ]", 3},
        {R"(["a,[{","b"])", 3},      // , [ { inside a string
        {R"(["a\",[{",1])", 3},      // an escaped quote does not end the string
        {R"(["a\\",1])", 3},         // an escaped backslash does, so the comma after it counts
        {R"(["abc,1)", 2},           // unterminated string: the scan ends
        {R"("a\)", 1},               // a backslash at the very end
    };
    for (const auto& sample : samples) {
        Check(JsonValueCountUpperBound(sample.text) == sample.expected,
            std::string("V1 count of ") + sample.text + " is " + std::to_string(sample.expected));
    }
    // For a well-formed text the bound is the node count of the parsed tree plus the empty containers.
    {
        std::vector<std::string> texts{valid, Serialize(base), "[]", "[[],{}]", "{\"a\":[],\"b\":{\"c\":[1,[2,3]]}}"};
        texts.push_back(MutateOne(base, "replace", "/views/1/measurementPointIds", "[]"));
        texts.push_back(MutateOne(base, "replace", "/views/0/usedPointIds", "[]"));
        for (const auto& text : texts) {
            std::size_t nodes = 0, empty_containers = 0;
            CountNodes(ParseDocument(text), nodes, empty_containers);
            Check(JsonValueCountUpperBound(text) == nodes + empty_containers,
                "V2 bound = nodes + empty containers for " + text.substr(0, 24) + " (nodes " + std::to_string(nodes) + ")");
        }
    }
    // Boundary: exactly N is within the budget, N + 1 is not. A text of N '[' reaches the JSON
    // layer (which refuses the depth), a text of N + 1 never does.
    Check(JsonValueCountUpperBound(WideDocument(limit)) == limit, "V3 wide document at the limit counts N");
    Check(JsonValueCountUpperBound(WideDocument(limit + 1)) == limit + 1, "V3 wide document over the limit counts N + 1");
    RejectsAt(std::string(limit - 1, '['), "json-syntax", "/", "V3 N values pass the budget (then fail on depth)");
    RejectsAt(std::string(limit, '['), "limit-json-values", "/", "V3 N + 1 values are over the budget");
    // The byte limit is still checked first.
    RejectsAt(std::string(kMaximumObservationSessionJsonBytes + 1, '['), "json-byte-limit", "/", "V3 byte limit comes first");
    // N + 1 values in a text that would have been a 1M-node tree, and a text at the byte limit that
    // would have been about 4M nodes: both are refused without building the tree, and quickly.
    using clock = std::chrono::steady_clock;
    for (const std::size_t values : {limit + 1, (kMaximumObservationSessionJsonBytes - 7) / 2 + 2}) {
        const auto text = WideDocument(values);
        Check(text.size() <= kMaximumObservationSessionJsonBytes, "V4 wide document fits the byte limit");
        const auto started = clock::now();
        RejectsAt(text, "limit-json-values", "/", "V4 " + std::to_string(values) + " values rejected before the tree is built");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count();
        std::cout << "V4 values=" << values << " bytes=" << text.size() << " reject_ms=" << elapsed << '\n';
        Check(elapsed < 2000, "V4 the rejection of " + std::to_string(values) + " values is fast (" + std::to_string(elapsed) + " ms)");
    }
}

// The slowest input the limits allow, built to make the exact predicate work: a chart
// of 16384 points, nearly all exactly collinear over a wide exponent range, two points
// one ulp off the line, and four views that use almost all of it. It takes about 1.3 s in a
// Release build but 25-45 s and 550 MB in a Debug build (the JSON tree dominates), so only
// Release builds run it.
[[maybe_unused]] void WorstCase() {
    using clock = std::chrono::steady_clock;
    const auto milliseconds = [](clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - from).count();
    };
    const auto started = clock::now();
    const std::size_t count = kMaximumChartPoints;
    std::vector<ChartPt> chart;
    chart.reserve(count);
    for (std::size_t index = 0; index + 2 < count; ++index) {
        const double a = std::ldexp(1.0 + static_cast<double>(index) * std::ldexp(1.0, -20), (static_cast<int>(index % 21) - 10) * 40);
        chart.push_back({"G" + std::to_string(index), a, a});
    }
    chart.push_back({"G" + std::to_string(count - 2), 1.5, std::nextafter(1.5, std::numeric_limits<double>::infinity())});
    chart.push_back({"G" + std::to_string(count - 1), 3.0, std::nextafter(3.0, 0.0)});
    const auto first_ids = [&](std::size_t n) {
        std::vector<std::string> ids;
        ids.reserve(n);
        for (std::size_t index = 0; index < n; ++index) ids.push_back(chart[index].id);
        return ids;
    };
    const auto view = [&](unsigned number, const char* camera, std::size_t used) {
        ViewSpec spec;
        spec.id = "V" + std::to_string(number);
        spec.camera = camera;
        spec.observed = first_ids(count);
        spec.used = first_ids(used);
        return RenderView(spec, number);
    };
    const std::string text = SessionText(ChartJson(chart), CamerasJson(),
        "[" + view(1, "CAM-A", count) + "," + view(2, "CAM-B", count - 1) + "," + view(3, "CAM-A", count - 2) + "," + view(4, "CAM-B", count) + "]");
    Check(text.size() <= kMaximumObservationSessionJsonBytes, "W document fits the byte limit");
    const auto values = detail::JsonValueCountUpperBound(text);
    Check(values <= kLegalMaximumValues, "W document stays within the legal maximum of values");
    std::cout << "W document_bytes=" << text.size() << " json_values=" << values << '\n';
    const auto parse_started = clock::now();
    std::optional<CalibrationObservationSession> session;
    try {
        session.emplace(CalibrationObservationSession::Parse(text));
    } catch (const std::exception& error) {
        Check(false, std::string("W worst-case document rejected: ") + error.what());
        return;
    }
    std::cout << "W parse_ms=" << milliseconds(parse_started) << '\n';
    const auto diagnose_started = clock::now();
    const auto diagnostics = DiagnoseObservationSession(*session);
    std::cout << "W diagnose_ms=" << milliseconds(diagnose_started) << '\n';
    using Code = SessionDiagnosticCode;
    const Diagnostics expected{
        {Code::ReferenceViewMissing, CameraAlias::CamA, std::nullopt}, {Code::ValidationViewMissing, CameraAlias::CamA, std::nullopt},
        {Code::ReferenceViewMissing, CameraAlias::CamB, std::nullopt}, {Code::ValidationViewMissing, CameraAlias::CamB, std::nullopt},
        ViewDiag(Code::DegeneratePointConfiguration, "V2", CameraAlias::CamB), ViewDiag(Code::DegeneratePointConfiguration, "V3", CameraAlias::CamA)};
    Check(diagnostics == expected, "W worst-case diagnostics " + Describe(diagnostics));
    std::cout << "W elapsed_ms=" << milliseconds(started) << '\n';
}

// The largest tree the value budget lets through: exactly N values. The reader builds it, then
// refuses its shape; nothing may throw std::bad_alloc on the way. Release only, for the same
// reason as the worst case above (the tree costs several hundred MB in a Debug build).
[[maybe_unused]] void FullTreeAtLimit() {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    const auto text = WideDocument(kMaximumObservationSessionJsonValues);
    RejectsAt(text, "unknown-field", "/", "D9 N values: the tree is built and the shape refused");
    std::cout << "D9 document_bytes=" << text.size() << " elapsed_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    const bool heavy = argc > 2 && std::string(argv[2]) == "--limits";
    try {
        if (argc < 2) throw std::runtime_error("usage: <fixtures-dir> [--limits]");
        const std::string dir = argv[1];
        auto valid = ReadFile(dir + "/valid-minimal.json");
        valid.erase(std::remove(valid.begin(), valid.end(), '\r'), valid.end()); // tolerate a CRLF checkout
        const auto base = ParseDocument(valid);
        if (heavy) {
            HeavyLimits(valid);
#ifdef NDEBUG
            FullTreeAtLimit();
            WorstCase();
#else
            std::cout << "W and D9 skipped in a Debug build; the Release build runs them" << std::endl;
#endif
        } else {
            Accepted(valid, base);
            Rejected(dir, valid, base);
            Diagnostic();
            Limits(base);
            Predicates();
            GridCrossCheck();
            GeneralPositionCrossCheck();
            PinnedLimits();
            PixelBoundaries(base);
            CameraSpecificDimensions();
            ViewTotals();
            ListAndUniquenessRules(base);
            ScalarRules(base);
            OneUlpMatrix();
            MoreDiagnostics();
            ValueBudget(valid, base);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "Unexpected: " << error.what() << '\n';
    }
    std::cout << (heavy ? "calibration_observation_session_limits" : "calibration_observation_session")
        << " checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 && checks > 0 ? 0 : 1;
}
