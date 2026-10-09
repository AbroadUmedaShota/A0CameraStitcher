#include "a0/m2/rig_profile_v2.hpp"
#include "a0/common/protocol_json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using a0::m2::RigProfileV2;
using namespace a0::m2::render;
using namespace a0::common::protocol_json;
int failures = 0, checks = 0;

void Check(const bool condition, const std::string& message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<class Callable>
void Rejects(Callable action, const std::string& message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}

struct Failure {
    [[noreturn]] static void Fail(std::string_view, std::string_view message) {
        throw std::invalid_argument(std::string(message));
    }
};

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Fixture could not be opened");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string TrimEnd(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

JsonValue ParseTree(const std::string& text) { return BasicJsonParser<Failure>(text).Parse(); }
std::string Serialize(const JsonValue& value) { return SerializeJsonWith<Failure>(value); }
JsonValue& Field(JsonValue& value, const char* name) { return value.object.at(name); }
JsonValue& Camera(JsonValue& value, const char* alias = "CAM-A") { return Field(Field(Field(value, "cameras"), alias), "projection"); }

void SetNumber(JsonValue& value, const std::string& lexeme) {
    value.kind = JsonKind::number; value.string = lexeme;
}
void SetString(JsonValue& value, const std::string& text) {
    value.kind = JsonKind::string; value.string = text;
}

template<class Mutation>
void RejectMutation(const std::string& valid, Mutation mutation, const std::string& message) {
    auto value = ParseTree(valid); mutation(value);
    Rejects([&] { (void)RigProfileV2::Parse(Serialize(value)); }, message);
}

void TestSharedFixtures(const std::filesystem::path& directory) {
    for (const auto* name : {"template", "calibrated-draft", "approved"}) {
        const auto text = Read(directory / (std::string(name) + ".json"));
        const auto profile = RigProfileV2::Parse(text);
        const auto golden = Read(directory / (std::string(name) + ".canonical.txt"));
        const auto hash = TrimEnd(Read(directory / (std::string(name) + ".sha256")));
        Check(profile.CanonicalFingerprintText() == golden, std::string(name) + " independently authored canonical bytes");
        Check(profile.FingerprintSha256() == hash, std::string(name) + " independently authored fingerprint");
        Check(!golden.empty() && golden.back() == '\n' && golden.find('\r') == std::string::npos,
            "Canonical format is LF, including its last line");
        const auto reordered = RigProfileV2::Parse(Serialize(ParseTree(text)));
        Check(reordered.CanonicalFingerprintText() == golden, "JSON property order does not determine fingerprint traversal");
        Check(reordered.FingerprintSha256() == hash, "Whitespace/order variation keeps the same hash");
        Check(profile.IsApproved() == (std::string_view(name) == "approved"), "Status is retained");
        Check(profile.IsCalibrated() == (std::string_view(name) != "template"), "Template and calibrated draft remain distinct");
    }
}

void TestSharedBinary64Cases(const std::filesystem::path& directory, const std::string& draft) {
    const auto cases = ParseTree(Read(directory / "number-cases.json"));
    Check(cases.kind == JsonKind::array && cases.array.size() >= 12, "Independent shared numeric vectors exist");
    for (const auto& entry : cases.array) {
        const auto lexeme = entry.object.at("jsonNumber").string;
        const auto bits = entry.object.at("binary64Hex").string;
        auto value = ParseTree(draft);
        SetNumber(Field(Field(Camera(value), "distortion"), "k1"), lexeme);
        const auto profile = RigProfileV2::Parse(Serialize(value));
        Check(profile.CanonicalFingerprintText().find("/cameras/CAM-A/projection/distortion/k1=f:" + bits + '\n') != std::string::npos,
            "Shared nearest-even binary64 case " + lexeme);
    }
    RejectMutation(draft, [](auto& value) { SetNumber(Field(Field(Camera(value), "distortion"), "k1"), "1e400"); }, "Overflow is rejected, unlike representable rounding underflow");
    auto negative_zero = ParseTree(draft);
    SetNumber(Field(Field(Camera(negative_zero), "distortion"), "k1"), "-0.0");
    Check(RigProfileV2::Parse(Serialize(negative_zero)).FingerprintSha256() == RigProfileV2::Parse(draft).FingerprintSha256(),
        "Negative zero normalizes to positive zero in whole-profile fingerprint");
}

void TestShapeAndLexemes(const std::string& approved, const std::string& draft, const std::string& blank) {
    RejectMutation(approved, [](auto& value) { SetString(Field(value, "schemaVersion"), "1.1.0"); }, "v1 is rejected by v2 reader");
    RejectMutation(approved, [](auto& value) { value.object.erase("qualityContract"); }, "Missing required block rejected");
    RejectMutation(approved, [](auto& value) { value.object.emplace("unexpected", JsonValue{}); }, "Unknown root field rejected");
    RejectMutation(approved, [](auto& value) { Field(Camera(value), "intrinsics").object.emplace("unexpected", JsonValue{}); }, "Unknown nested field rejected");
    RejectMutation(approved, [](auto& value) { Field(Camera(value), "distortion").object.erase("p2"); }, "No default for a missing lens coefficient");
    RejectMutation(approved, [](auto& value) { Field(Field(value, "cameras"), "CAM-A").object.at("physicalIdentity").kind = JsonKind::string; }, "Physical identity is strictly null");
    RejectMutation(approved, [](auto& value) { SetString(Field(value, "profileId"), "contains=separator"); }, "Canonical separator forbidden in identifier");
    RejectMutation(approved, [](auto& value) { SetString(Field(value, "profileId"), "contains\nnewline"); }, "Control character forbidden");
    RejectMutation(approved, [](auto& value) { SetString(Field(value, "profileId"), "nonascii-\xc3\xa9"); }, "Non-ASCII identifier forbidden");
    RejectMutation(approved, [](auto& value) { SetString(Field(Field(Field(value, "calibration"), "inputSha256"), "CAM-A"), std::string(64, 'A')); }, "Calibration hashes are lowercase hex only");
    RejectMutation(approved, [](auto& value) { Field(Field(Camera(value), "intrinsics"), "fxPixels").kind = JsonKind::boolean; }, "Boolean cannot be a coefficient");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(Camera(value), "intrinsics"), "fxPixels"), "0"); }, "Zero focal length rejected");
    RejectMutation(approved, [](auto& value) { Field(Camera(value), "documentToImage").array.pop_back(); }, "Matrix row count is exact");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Camera(value), "documentToImage").array[2].array[2], "2"); }, "H22 normalization is required");
    RejectMutation(draft, [](auto& value) { Field(value, "documentPlane") = JsonValue{}; }, "Partially populated draft rejected");
    RejectMutation(blank, [](auto& value) { SetString(Field(value, "status"), "approved"); }, "Template cannot claim approval");
    RejectMutation(draft, [&](auto& value) { auto owner = ParseTree(approved); Field(value, "outputRaster") = Field(owner, "outputRaster"); }, "Draft cannot hold owner raster");
    auto raw_duplicate = blank;
    raw_duplicate.insert(raw_duplicate.find('{') + 1, "\"schemaVersion\":\"2.0.0\",");
    Rejects([&] { (void)RigProfileV2::Parse(raw_duplicate); }, "Duplicate field is not last-value-wins");
    auto escaped_duplicate = blank;
    escaped_duplicate.insert(escaped_duplicate.find('{') + 1, "\"\\u0073chemaVersion\":\"2.0.0\",");
    Rejects([&] { (void)RigProfileV2::Parse(escaped_duplicate); }, "Escaped duplicate keys are detected after decoding");
    Rejects([&] { (void)RigProfileV2::Parse(blank + " null"); }, "Trailing JSON is rejected");
    for (const auto* lexeme : {"180.0", "180e0"}) RejectMutation(approved, [&](auto& value) { SetNumber(Field(Field(value, "outputRaster"), "dpi"), lexeme); }, "DPI integer lexeme only");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(value, "cameraModel"), "sensorWidthPixels"), "7360e0"); }, "Sensor integer lexeme only");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(value, "outputRaster"), "widthPixels"), "8426.0"); }, "Output width integer lexeme only");
    for (const auto* lexeme : {"0.0010000000000000000001", "0.0009999999999999999999", "1e-400", "-1e-400"}) {
        RejectMutation(approved, [&](auto& value) { SetNumber(Field(Field(Field(value, "outputRaster"), "regionMm"), "left"), lexeme); }, "Micrometre divisibility is exact before binary64 rounding");
    }
    for (const auto* lexeme : {"0.001", "1e-3", "0.0010000000000000000000"}) {
        auto value = ParseTree(approved); SetNumber(Field(Field(Field(value, "outputRaster"), "regionMm"), "left"), lexeme);
        const auto profile = RigProfileV2::Parse(Serialize(value));
        Check(profile.ValidateApprovedForUse("2026-01-01T00:00:00Z", Resampling::bilinear).raster.region_um.left == 1,
            "Exact exponent/decimal region resolves to integer micrometres");
    }
}

void TestUseAndGeometry(const std::string& approved, const std::string& draft, const std::string& blank) {
    const auto profile = RigProfileV2::Parse(approved);
    const auto fingerprint = profile.FingerprintSha256();
    auto applied = profile.ValidateApprovedForUse("2000-01-02T00:00:00Z", Resampling::bilinear);
    Check(applied.raster.width_pixels == 8426 && applied.raster.height_pixels == 5960 && applied.raster.dpi == 180, "Approved raster is supplied explicitly by profile");
    Check(applied.camera_a.intrinsics.fx_pixels == 10000 && applied.camera_a.document_to_image[5] == 4800, "Parsed coefficients are the values applied");
    applied.camera_a.document_to_image[5] = 999;
    Check(profile.ValidateApprovedForUse("2099-12-31T23:59:59Z", Resampling::bicubic_catmull_rom).camera_a.document_to_image[5] == 4800,
        "Returned parameters are copies; caller edits do not mutate the profile snapshot");
    Check(profile.FingerprintSha256() == fingerprint, "Assessment and chosen kernel do not alter profile fingerprint");
    Rejects([&] { (void)profile.ValidateApprovedForUse("2100-01-01T00:00:00Z", Resampling::bilinear); }, "Expiry boundary is exclusive");
    Rejects([&] { (void)profile.ValidateApprovedForUse("2000-01-01T23:59:59Z", Resampling::bilinear); }, "Assessment cannot precede approval");
    Rejects([&] { (void)profile.ValidateApprovedForUse("2026-01-01T00:00:00Z", static_cast<Resampling>(77)); }, "Kernel selection has no fallback");
    const OutputRaster evaluation{{0, 0, 10000, 10000}, 180, 71, 71};
    const auto calibrated = RigProfileV2::Parse(draft);
    Check(calibrated.ValidateCalibrationForEvaluation(evaluation, Resampling::bilinear).raster.width_pixels == 71, "Calibrated draft uses external evaluation raster");
    Rejects([&] { (void)calibrated.ValidateApprovedForUse("2026-01-01T00:00:00Z", Resampling::bilinear); }, "Calibrated draft never passes product-use gate");
    Rejects([&] { (void)profile.ValidateCalibrationForEvaluation(evaluation, Resampling::bilinear); }, "Approved profile is not a draft evaluation snapshot");
    const auto template_profile = RigProfileV2::Parse(blank);
    Rejects([&] { (void)template_profile.ValidateCalibrationForEvaluation(evaluation, Resampling::bilinear); }, "Template has no usable calibration");
    Rejects([&] { (void)calibrated.ValidateCalibrationForEvaluation({{0, 0, 1200000, 10000}, 180, 8504, 71}, Resampling::bilinear); }, "External raster cannot extend beyond sheet");
    Rejects([&] { (void)calibrated.ValidateCalibrationForEvaluation({{0, 0, 10000, 10000}, 180, 72, 71}, Resampling::bilinear); }, "External raster must obey exact ceil arithmetic");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(value, "outputRaster"), "widthPixels"), "8425"); }, "Approved ceil arithmetic checked at parse");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(Field(value, "outputRaster"), "regionMm"), "right"), "1190"); }, "Approved raster stays inside sheet");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(Field(value, "outputRaster"), "regionMm"), "left"), "1189"); }, "Empty or reversed output region rejected");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(Field(value, "correctionEnvelope"), "rotationErrorDegrees"), "targetMax"), "1"); }, "Target must not exceed automatic correction maximum");
    RejectMutation(draft, [](auto& value) { SetNumber(Field(Field(Field(Field(value, "calibration"), "fitResidualPixels"), "CAM-A"), "rms"), "-1e-400"); }, "Raw negative residual cannot hide in rounding underflow");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(value, "correctionEnvelope"), "minimumOverlapMm"), "-1e-400"); }, "Raw negative correction limit cannot hide in rounding underflow");
    RejectMutation(draft, [](auto& value) { for (auto& cell : Field(Camera(value), "documentToImage").array[0].array) SetNumber(cell, "0"); }, "Singular projection rejected even without a draft raster");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Camera(value), "documentToImage").array[2].array[0], "-0.02"); }, "Nonpositive projective domain rejected");
    RejectMutation(approved, [](auto& value) { SetNumber(Field(Field(Camera(value), "distortion"), "k1"), "-1"); }, "Radial fold rejected on actual output domain");
    RejectMutation(approved, [](auto& value) {
        auto& distortion = Field(Camera(value), "distortion");
        SetNumber(Field(distortion, "k1"), "-6.666666666666667");
        SetNumber(Field(distortion, "k2"), "19.8");
    }, "Radial derivative with an interior negative minimum cannot pass on corner values alone");
}

void TestTimes(const std::string& approved, const std::string& draft) {
    for (const auto* timestamp : {"0000-01-01T00:00:00Z", "1900-02-29T00:00:00Z", "2001-02-29T00:00:00Z", "2000-04-31T00:00:00Z",
        "2000-01-01T24:00:00Z", "2000-01-01T00:60:00Z", "2000-01-01T00:00:60Z", "2000-01-01T00:00:00.0Z", "2000-01-01T00:00:00+00:00"}) {
        RejectMutation(draft, [&](auto& value) { SetString(Field(Field(value, "calibration"), "measuredAt"), timestamp); }, "Invalid Gregorian whole-second UTC timestamp rejected");
    }
    for (const auto* timestamp : {"0001-01-01T00:00:00Z", "2000-02-29T00:00:00Z", "9999-12-31T23:59:59Z"}) {
        auto value = ParseTree(draft); SetString(Field(Field(value, "calibration"), "measuredAt"), timestamp);
        Check(RigProfileV2::Parse(Serialize(value)).IsCalibrated(), "Valid Gregorian boundary timestamp retained in draft");
    }
    RejectMutation(approved, [](auto& value) { SetString(Field(Field(value, "approval"), "approvedAt"), "1999-12-31T23:59:59Z"); }, "Approval cannot precede measured calibration");
    RejectMutation(approved, [](auto& value) { SetString(Field(Field(value, "approval"), "validUntil"), "2000-01-02T00:00:00Z"); }, "Empty approval interval rejected");
    const auto profile = RigProfileV2::Parse(approved);
    Rejects([&] { (void)profile.ValidateApprovedForUse("2026-02-30T00:00:00Z", Resampling::bilinear); }, "Invalid assessment calendar rejected");
}

} // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc != 2) throw std::invalid_argument("Expected shared fixture directory");
        const std::filesystem::path directory = argv[1];
        const auto approved = Read(directory / "approved.json");
        const auto draft = Read(directory / "calibrated-draft.json");
        const auto blank = Read(directory / "template.json");
        TestSharedFixtures(directory);
        TestSharedBinary64Cases(directory, draft);
        TestShapeAndLexemes(approved, draft, blank);
        TestUseAndGeometry(approved, draft, blank);
        TestTimes(approved, draft);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected rig profile v2 test failure: " << error.what() << '\n';
        return 2;
    }
    std::cout << "rig_profile_v2 checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
