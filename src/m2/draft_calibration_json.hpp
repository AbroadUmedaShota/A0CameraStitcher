#pragma once

// Internal JSON text helpers of the development-only draft calibration
// writers (#313 reference pair, #374 planar). Fixed field order, shortest
// round-trip numbers, Windows SHA-256. Not installed.

#include "a0/m2/draft_calibration_artifacts.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace a0::m2::calibration::draft_json {

inline std::string Q(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
inline std::string N(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("draft calibration: non-finite number");
    if (value == 0.0) return "0";
    char buffer[64];
    const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (encoded.ec != std::errc{}) throw std::invalid_argument("draft calibration: number encoding");
    return std::string(buffer, encoded.ptr);
}
inline std::string OptionalNumber(const std::optional<double>& value) { return value ? N(*value) : "null"; }
template<std::size_t Size> std::string Numbers(const std::array<double, Size>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < Size; ++i) { if (i) out += ','; out += N(values[i]); }
    return out + ']';
}
inline std::string Residual(const std::optional<ResidualSummary>& value, bool count = true) {
    if (!value) return "null";
    std::string out = "{\"rms\":" + N(value->rms_pixels) + ",\"max\":" + N(value->max_pixels);
    if (count) out += ",\"pointCount\":" + std::to_string(value->point_count);
    return out + '}';
}
inline std::string Solver(const std::optional<SolverRecord>& value) {
    if (!value) return "null";
    return "{\"iterations\":" + std::to_string(value->iterations) + ",\"acceptedSteps\":" + std::to_string(value->accepted_steps)
        + ",\"initialCost\":" + N(value->initial_cost) + ",\"finalCost\":" + N(value->final_cost)
        + ",\"stopReason\":" + Q(SolverStopReasonCode(value->stop_reason)) + '}';
}
inline std::string K(const PinholeIntrinsics& value) {
    return "{\"fxPixels\":" + N(value.fx) + ",\"fyPixels\":" + N(value.fy)
        + ",\"cxPixels\":" + N(value.cx) + ",\"cyPixels\":" + N(value.cy) + '}';
}
inline std::string D(const BrownConradyDistortion& value) {
    return "{\"model\":\"brown-conrady-k1k2k3-p1p2\",\"k1\":" + N(value.k1) + ",\"k2\":" + N(value.k2)
        + ",\"k3\":" + N(value.k3) + ",\"p1\":" + N(value.p1) + ",\"p2\":" + N(value.p2) + '}';
}
inline std::string Matrix(const std::array<double, 9>& h) {
    return '[' + Numbers(std::array<double, 3>{h[0], h[1], h[2]}) + ',' + Numbers(std::array<double, 3>{h[3], h[4], h[5]})
        + ',' + Numbers(std::array<double, 3>{h[6], h[7], h[8]}) + ']';
}
inline std::string PerCameraMeasurements(const std::array<std::optional<double>, 2>& values) {
    return "{\"CAM-A\":" + OptionalNumber(values[0]) + ",\"CAM-B\":" + OptionalNumber(values[1]) + '}';
}
inline std::string Measurements(const CalibrationRigMeasurements& rig) {
    return "{\"baselineMm\":" + OptionalNumber(rig.baseline_mm) + ",\"overlapMm\":" + OptionalNumber(rig.overlap_mm)
        + ",\"cameraToDocumentMm\":" + PerCameraMeasurements(rig.camera_to_document_mm)
        + ",\"lensFocalLengthMm\":" + PerCameraMeasurements(rig.lens_focal_length_mm) + '}';
}
inline std::string Plane(DraftPaperOrientation orientation, DraftCameraOrder camera_order) {
    if (orientation != DraftPaperOrientation::Landscape && orientation != DraftPaperOrientation::Portrait)
        throw std::invalid_argument("draft calibration: orientation");
    if (camera_order != DraftCameraOrder::LeftRight && camera_order != DraftCameraOrder::TopBottom)
        throw std::invalid_argument("draft calibration: camera order");
    const bool landscape = orientation == DraftPaperOrientation::Landscape;
    return "{\"paper\":\"ISO-216-A0\",\"orientation\":" + Q(landscape ? "landscape" : "portrait")
        + ",\"sheetWidthMm\":" + (landscape ? "1189" : "841") + ",\"sheetHeightMm\":" + (landscape ? "841" : "1189")
        + ",\"cameraOrder\":" + Q(camera_order == DraftCameraOrder::LeftRight ? "CAM-A-left-CAM-B-right" : "CAM-A-top-CAM-B-bottom") + '}';
}
// Everything of a rig profile v2 draft before "cameras".
inline std::string ProfileHead(std::string_view profile_id, DraftPaperOrientation orientation, DraftCameraOrder camera_order) {
    return "{\"schemaVersion\":\"2.0.0\",\"status\":\"draft\",\"profileId\":" + Q(profile_id)
        + ",\"conventions\":{\"documentPlane\":\"mm-origin-sheet-top-left-x-right-y-down\",\"cameraPixel\":\"stored-order-sample-at-integer-index\","
          "\"matrix\":\"row-major-column-vector\",\"exifOrientation\":\"ignored\"},"
          "\"cameraModel\":{\"manufacturer\":\"Nikon\",\"model\":\"D810\",\"sensorWidthPixels\":7360,\"sensorHeightPixels\":4912},\"documentPlane\":"
        + Plane(orientation, camera_order) + ",\"cameras\":{";
}
inline std::string CameraProjection(CameraAlias camera, const PinholeIntrinsics& k, const BrownConradyDistortion& d,
    const std::array<double, 9>& h) {
    return Q(CameraAliasCode(camera)) + ":{\"physicalIdentity\":null,\"projection\":{\"intrinsics\":" + K(k)
        + ",\"distortion\":" + D(d) + ",\"documentToImage\":" + Matrix(h) + "}}";
}

inline std::string Sha256(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("draft calibration: SHA-256 provider");
    struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { BCryptCloseAlgorithmProvider(h, 0); } } alg{algorithm};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("draft calibration: SHA-256 create");
    struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { BCryptDestroyHash(h); } } guard{hash};
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0)
        throw std::runtime_error("draft calibration: SHA-256 data");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("draft calibration: SHA-256 finish");
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto c : digest) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}

} // namespace a0::m2::calibration::draft_json
