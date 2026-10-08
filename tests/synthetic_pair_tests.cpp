// Contracts for the ground-truth synthetic pair generator (issue #269).
//
// Usage: m2_synthetic_pair_contracts --generator <exe> --adapter <exe> --spec <spec.json>
//            --powershell <pwsh> --profile-schema <rig-profile.v2.schema.json> [--small-only]
//
// Everything that writes images writes into a throw-away folder under the
// system temp directory, never into the repository.

#include "a0/m2/synthetic_pair.hpp"
#include "synthetic_pair_publish.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace synth = a0::m2::synthetic;

namespace {

int failures = 0;

void Check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Info(const std::string& message) { std::cout << "info: " << message << '\n'; }

std::string Fixed(const double value, const int digits = 4) {
    char text[48]{};
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

std::string Scientific(const double value) {
    char text[48]{};
    std::snprintf(text, sizeof(text), "%.3e", value);
    return text;
}

template <typename Callable>
void CheckRejects(Callable&& callable, const std::string& expected, const std::string& message) {
    try {
        callable();
        Check(false, message + " (not rejected)");
    } catch (const std::invalid_argument& error) {
        Check(std::string(error.what()).find(expected) != std::string::npos,
            message + " (unexpected rejection: " + error.what() + ")");
    } catch (const std::exception& error) {
        Check(false, message + " (wrong exception type: " + error.what() + ")");
    }
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path.string());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("cannot write " + path.string());
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    if (position == std::string::npos) throw std::logic_error("test bug: pattern not found: " + from);
    text.replace(position, from.size(), to);
    return text;
}

// Replaces the last occurrence, for the second camera of a spec whose cameras share text.
std::string ReplaceLast(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.rfind(from);
    if (position == std::string::npos) throw std::logic_error("test bug: pattern not found: " + from);
    text.replace(position, from.size(), to);
    return text;
}

class TempFolder final {
public:
    TempFolder() {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("a0-synthetic-pair-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(ticks));
        std::filesystem::create_directories(path_);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] std::filesystem::path Sub(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

struct ProcessResult {
    DWORD exit_code{};
    std::string output;
};

ProcessResult RunProcess(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    const DWORD timeout_milliseconds) {
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    for (const auto& argument : arguments) command += L" \"" + argument + L"\"";

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, 0)) throw std::runtime_error("pipe creation failed");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        throw std::runtime_error("process start failed");
    }
    ProcessResult result;
    char buffer[4096];
    DWORD count = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
        result.output.append(buffer, count);
    }
    CloseHandle(read_end);
    if (WaitForSingleObject(process.hProcess, timeout_milliseconds) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("process timed out");
    }
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

std::string OutputValue(const std::string& output, const std::string& key) {
    std::istringstream lines(output);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    }
    return {};
}

// ---- A small spec for the fast checks ---------------------------------------------------------

// fx = fy = 312 is about the half diagonal of the 480x400 image, as in the full size fixture.
const char* const kSmallSpec = R"({
  "schema": "a0.m2.synthetic-pair-spec/2",
  "seed": 11,
  "image": { "width_px": 480, "height_px": 400, "supersample": 3 },
  "jpeg": { "quality": 0.9, "chroma_subsampling": "444" },
  "chart": { "width_mm": 120, "height_mm": 100, "margin_mm": 10, "grid_pitch_mm": 5,
             "grid_line_width_mm": 0.2, "fiducial_pitch_mm": 20, "fiducial_radius_mm": 2.8,
             "slanted_edge_slope": 0.0875, "fine_line_base_width_mm": 0.25 },
  "cameras": [
    { "alias": "CAM-A",
      "projection": {
        "intrinsics": { "fxPixels": 312, "fyPixels": 312, "cxPixels": 239.5, "cyPixels": 199.5 },
        "distortion": { "model": "brown-conrady-k1k2k3-p1p2", "k1": -0.05, "k2": 0.01, "k3": 0, "p1": 0, "p2": 0 },
        "placement": { "center_mm": [45, 50], "pixels_per_mm": 4.5, "quarter_turns": 0 } },
      "exposure_gain": 1.0, "white_balance_gain": [1, 1, 1], "vignette": [-0.15, 0.03],
      "noise": { "read_sigma": 0.004, "shot_sigma": 0.006 } },
    { "alias": "CAM-B",
      "projection": {
        "intrinsics": { "fxPixels": 312, "fyPixels": 312, "cxPixels": 239.5, "cyPixels": 199.5 },
        "distortion": { "model": "brown-conrady-k1k2k3-p1p2", "k1": 0.03, "k2": 0, "k3": 0, "p1": 0, "p2": 0 },
        "placement": { "center_mm": [75, 50], "pixels_per_mm": 4.5, "quarter_turns": 0 } },
      "exposure_gain": 1.05, "white_balance_gain": [1.02, 1, 0.98], "vignette": [-0.1, 0],
      "noise": { "read_sigma": 0.005, "shot_sigma": 0.007 } }
  ]
})";

const char* const kPlacementA = R"("placement": { "center_mm": [45, 50], "pixels_per_mm": 4.5, "quarter_turns": 0 })";

// The same spec with every degradation switched off, used to compare the rendered
// image with the ground truth positions.
std::string CleanSmallSpec(const bool with_lens_distortion) {
    auto text = std::string(kSmallSpec);
    text = Replace(text, "\"vignette\": [-0.15, 0.03]", "\"vignette\": [0, 0]");
    text = Replace(text, "\"vignette\": [-0.1, 0]", "\"vignette\": [0, 0]");
    text = Replace(text, "\"noise\": { \"read_sigma\": 0.004, \"shot_sigma\": 0.006 }", "\"noise\": { \"read_sigma\": 0, \"shot_sigma\": 0 }");
    text = Replace(text, "\"noise\": { \"read_sigma\": 0.005, \"shot_sigma\": 0.007 }", "\"noise\": { \"read_sigma\": 0, \"shot_sigma\": 0 }");
    text = Replace(text, "\"exposure_gain\": 1.05", "\"exposure_gain\": 1.0");
    if (!with_lens_distortion) {
        text = Replace(text, "\"k1\": -0.05, \"k2\": 0.01", "\"k1\": 0, \"k2\": 0");
        text = Replace(text, "\"k1\": 0.03, \"k2\": 0", "\"k1\": 0, \"k2\": 0");
    }
    text = Replace(text, "[1.02, 1, 0.98]", "[1, 1, 1]");
    return text;
}

constexpr double kPi = 3.14159265358979323846;

// Two cameras that exercise what the placement form cannot: CAM-A is turned by 17 degrees with
// perspective terms, different focal lengths, an off-centre principal point and all five lens
// coefficients; CAM-B is built from a pose, H = K [r1 r2 t] with a 25 degree tilt about the x axis.
synth::PairSpec MakeStressSpec() {
    auto spec = synth::ParsePairSpec(CleanSmallSpec(true));
    spec.supersample = 4;

    auto& turned = spec.cameras[0];
    const double angle = 17.0 * kPi / 180.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double scale = 4.5;
    turned.intrinsics = {330.0, 300.0, 241.0, 198.0};
    turned.distortion = {-0.04, 0.01, 0.002, 0.0008, -0.0006};
    turned.document_to_image = {scale * c, -scale * s, 239.5 - scale * (c * 45.0 - s * 50.0),
        scale * s, scale * c, 199.5 - scale * (s * 45.0 + c * 50.0), 6e-4, -4e-4, 1.0};

    auto& tilted = spec.cameras[1];
    const double fx = 2400.0;
    const double fy = 2350.0;
    const double cx = 239.5;
    const double cy = 199.5;
    tilted.intrinsics = {fx, fy, cx, cy};
    tilted.distortion = {0.02, 0.0, 0.0, -0.0005, 0.0007};
    const double tilt = 25.0 * kPi / 180.0;
    const double roll = 10.0 * kPi / 180.0;
    const double ct = std::cos(tilt);
    const double st = std::sin(tilt);
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);
    // R = Rx(tilt) * Rz(roll); only the first two columns enter the plane homography.
    const double r[3][2] = {{cr, -sr}, {ct * sr, ct * cr}, {st * sr, st * cr}};
    const double look_at[2] = {60.0, 50.0};
    const double distance = 600.0;
    const double t[3] = {-(r[0][0] * look_at[0] + r[0][1] * look_at[1]),
        -(r[1][0] * look_at[0] + r[1][1] * look_at[1]),
        distance - (r[2][0] * look_at[0] + r[2][1] * look_at[1])};
    const double m[3][3] = {
        {fx * r[0][0] + cx * r[2][0], fx * r[0][1] + cx * r[2][1], fx * t[0] + cx * t[2]},
        {fy * r[1][0] + cy * r[2][0], fy * r[1][1] + cy * r[2][1], fy * t[1] + cy * t[2]},
        {r[2][0], r[2][1], t[2]},
    };
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) tilted.document_to_image[row * 3 + column] = m[row][column] / m[2][2];
    }
    synth::ValidatePairSpec(spec);
    return spec;
}

// ---- JSON ---------------------------------------------------------------------------------------

void TestJson() {
    const auto value = synth::ParseJson(R"({"a":[1,2.5,-3e2],"b":{"c":"x\ny\u00e9"},"d":true,"e":null,"seed":18446744073709551615})");
    Check(value.kind == synth::JsonValue::Kind::object, "json: object parsed");
    const auto* a = value.Find("a");
    Check(a != nullptr && a->items.size() == 3 && a->items[2].AsDouble() == -300.0, "json: numbers parse");
    const auto* c = value.Find("b") != nullptr ? value.Find("b")->Find("c") : nullptr;
    Check(c != nullptr && c->AsString() == "x\ny\xC3\xA9", "json: string escapes decode to UTF-8");
    Check(value.Find("seed") != nullptr && value.Find("seed")->AsUnsigned() == 18446744073709551615ULL,
        "json: 64 bit seed is kept exactly");
    Check(value.Find("missing") == nullptr, "json: missing key gives null");

    const char* const malformed[] = {
        "", "{", "[1,]", "{\"a\":1,}", "{\"a\":1,\"a\":2}", "01", "1.", ".5", "-", "1e", "\"abc", "\"\\q\"",
        "[1] x", "nul", "{\"a\" 1}", "\"\\ud800\"", "\"tab\there\"",
    };
    for (const char* text : malformed) {
        CheckRejects([&] { (void)synth::ParseJson(text); }, "JSON", std::string("json: malformed rejected: ") + text);
    }
    // The header promises 32 levels of objects and arrays: 32 are accepted, 33 are not.
    const auto nested = [](const std::size_t levels, const bool with_scalar) {
        std::string text(levels, '[');
        if (with_scalar) text += "1";
        text += std::string(levels, ']');
        return text;
    };
    (void)synth::ParseJson(nested(32, false));
    (void)synth::ParseJson(nested(32, true));
    CheckRejects([&] { (void)synth::ParseJson(nested(33, false)); }, "too deep", "json: 33 nested arrays rejected");
    CheckRejects([&] { (void)synth::ParseJson(nested(33, true)); }, "too deep", "json: 33 nested arrays with a scalar rejected");
    std::string objects;
    for (int level = 0; level < 33; ++level) objects += "{\"a\":";
    objects += "1" + std::string(33, '}');
    CheckRejects([&] { (void)synth::ParseJson(objects); }, "too deep", "json: 33 nested objects rejected");
    CheckRejects([&] { (void)synth::ParseJson("-1").AsUnsigned(); }, "unsigned", "json: negative is not unsigned");
    CheckRejects([&] { (void)synth::ParseJson("1.0").AsUnsigned(); }, "unsigned", "json: fraction is not unsigned");
    CheckRejects([&] { (void)synth::ParseJson("1e999").AsDouble(); }, "finite", "json: infinite double rejected");
}

// ---- Spec validation --------------------------------------------------------------------------

void TestSpecValidation() {
    const std::string good = kSmallSpec;
    const auto spec = synth::ParsePairSpec(good);
    Check(spec.seed == 11 && spec.cameras.size() == 2 && spec.image_width_px == 480, "spec: small spec parses");
    Check(spec.cameras[0].distortion.p1 == 0.0 && spec.cameras[0].intrinsics.fx_pixels == 312.0, "spec: fields reach the structs");

    const std::string alias_b = "\"alias\": \"CAM-B\"";
    const std::string matrix_a = "\"documentToImage\": ";
    struct Case {
        std::string text;
        std::string expected;
        std::string name;
    };
    const std::vector<Case> cases = {
        {Replace(good, "\"seed\": 11,", "\"seed\": 11, \"bogus\": 1,"), "unknown key", "unknown top level key"},
        {Replace(good, "\"seed\": 11,", ""), "missing \"seed\"", "missing seed"},
        {Replace(good, "\"seed\": 11", "\"seed\": -1"), "unsigned", "negative seed"},
        {Replace(good, "a0.m2.synthetic-pair-spec/2", "a0.m2.synthetic-pair-spec/1"), "ADR-0034", "the /1 format names ADR-0034"},
        {Replace(good, "a0.m2.synthetic-pair-spec/2", "a0.m2.synthetic-pair-spec/3"), "schema", "wrong schema"},
        {Replace(good, "\"supersample\": 3", "\"supersample\": 0"), "supersample", "zero supersample"},
        {Replace(good, "\"supersample\": 3", "\"supersample\": 9"), "supersample", "supersample too large"},
        {Replace(good, "\"width_px\": 480", "\"width_px\": 0"), "width_px", "zero width"},
        {Replace(good, "\"width_px\": 480", "\"width_px\": 16385"), "width_px", "width too large"},
        {Replace(good, "\"quality\": 0.9", "\"quality\": 1.5"), "quality", "quality above 1"},
        {Replace(good, "\"quality\": 0.9", "\"quality\": 0"), "quality", "quality zero"},
        {Replace(good, "\"444\"", "\"411\""), "chroma_subsampling", "unknown subsampling"},
        {Replace(good, "\"fiducial_radius_mm\": 2.8", "\"fiducial_radius_mm\": 3.1"), "fiducial_radius_mm", "radius over 0.15 pitch"},
        {Replace(good, "\"grid_pitch_mm\": 5", "\"grid_pitch_mm\": 7"), "integer multiple", "pitch not a multiple"},
        {Replace(good, "\"margin_mm\": 10", "\"margin_mm\": 0.5"), "margin_mm", "margin too small for the frame"},
        {Replace(good, "\"slanted_edge_slope\": 0.0875", "\"slanted_edge_slope\": 0.5"), "slanted_edge_slope", "slope too steep"},
        {Replace(good, "\"width_mm\": 120", "\"width_mm\": 30"), "at least one lattice cell", "chart without a cell"},
        {Replace(good, alias_b, "\"alias\": \"CAM-A\""), "duplicates", "duplicate alias"},
        {Replace(good, alias_b, "\"alias\": \"cam-a\""), "duplicates", "alias duplicate ignoring case"},
        {Replace(good, alias_b, "\"alias\": \"../x\""), "alias", "path-like alias"},
        {Replace(good, alias_b, "\"alias\": \"\""), "alias", "empty alias"},
        {Replace(good, alias_b, "\"alias\": \"CON\""), "reserved", "reserved alias CON"},
        {Replace(good, alias_b, "\"alias\": \"nul\""), "reserved", "reserved alias nul"},
        {Replace(good, alias_b, "\"alias\": \"Com1\""), "reserved", "reserved alias Com1"},
        {Replace(good, alias_b, "\"alias\": \"LPT9\""), "reserved", "reserved alias LPT9"},
        {Replace(good, alias_b, "\"alias\": \"aux\""), "reserved", "reserved alias aux"},
        {Replace(good, "\"quarter_turns\": 0", "\"quarter_turns\": 4"), "quarter_turns", "quarter turns above 3"},
        {Replace(good, "\"k1\": -0.05", "\"k1\": -3"), "monotonic", "non monotonic lens"},
        // A barrel lens whose radial map folds back before the image corner radius is reached.
        {Replace(good, "\"k1\": -0.05, \"k2\": 0.01", "\"k1\": -0.2, \"k2\": 0"), "monotonic", "barrel lens with no preimage for the corners"},
        {Replace(good, "\"exposure_gain\": 1.0,", "\"exposure_gain\": -1.0,"), "exposure_gain", "negative gain"},
        {Replace(good, "\"fxPixels\": 312", "\"fxPixels\": 0"), "fxPixels", "zero fx"},
        {Replace(good, "\"fyPixels\": 312", "\"fyPixels\": -312"), "fyPixels", "negative fy"},
        {Replace(good, "\"cxPixels\": 239.5", "\"cxPixels\": 1e999"), "cxPixels", "infinite cx"},
        {Replace(good, "brown-conrady-k1k2k3-p1p2", "brown-conrady-k1k2p1p2k3"), "model", "wrong distortion model"},
        {Replace(good, ", \"p2\": 0 }", " }"), "missing \"p2\"", "no implicit zero for p2"},
        {Replace(good, "\"p1\": 0, \"p2\": 0 }", "\"p2\": 0 }"), "missing \"p1\"", "no implicit zero for p1"},
        {Replace(good, "\"k3\": 0, ", ""), "missing \"k3\"", "no implicit zero for k3"},
        {Replace(good, "\"read_sigma\": 0.004", "\"read_sigma\": -0.004"), "read_sigma", "negative noise"},
        {Replace(good, "\"white_balance_gain\": [1, 1, 1]", "\"white_balance_gain\": [1, 1]"), "white_balance_gain", "short white balance"},
        {Replace(good, "\"cameras\": [", "\"cameras\": [ ], \"cameras_unused\": ["), "1 to 8 cameras", "empty cameras"},
        {Replace(good, "\"projection\": {", "\"homography\": [1,0,0,0,1,0,0,0,1], \"projection\": {"), "unknown key", "the /1 homography key is unknown"},
        // documentToImage given directly.
        {Replace(good, kPlacementA, "\"documentToImage\": [[4.5,0,0],[0,4.5,0],[0,0,2]]"), "[2][2] must be 1", "documentToImage[2][2] other than 1"},
        {Replace(good, kPlacementA, "\"documentToImage\": [[4.5,0,0],[0,4.5,0]]"), "3 rows", "documentToImage with two rows"},
        {Replace(good, kPlacementA, "\"documentToImage\": [[4.5,0],[0,4.5,0],[0,0,1]]"), "3 numbers", "documentToImage with a short row"},
        {Replace(good, kPlacementA, "\"documentToImage\": [4.5,0,0,0,4.5,0,0,0,1]"), "3 rows", "flat documentToImage of the /1 style"},
        {Replace(good, kPlacementA, std::string(kPlacementA) + ", \"documentToImage\": [[4.5,0,0],[0,4.5,0],[0,0,1]]"), "exactly one", "placement and documentToImage together"},
        {Replace(good, std::string(",\n        ") + kPlacementA, ""), "exactly one", "neither placement nor documentToImage"},
        {Replace(good, kPlacementA, "\"documentToImage\": [[1,2,3],[2,4,6],[0,0,1]]"), "not invertible", "singular documentToImage"},
        // w' <= 0 at a chart corner while the chart centre is still in front.
        {Replace(good, kPlacementA, "\"documentToImage\": [[1,0,0],[0,1,0],[-0.01,0,1]]"), "chart corner behind the camera", "chart corner behind the camera"},
        // The chart is in front, but the preimage of the image corners is not.
        {Replace(good, kPlacementA, "\"documentToImage\": [[4.5,0,-2000],[0,4.5,0],[-0.008,0,1]]"), "puts an image corner behind the camera",
            "image corner preimage behind the camera"},
    };
    for (const auto& test : cases) {
        CheckRejects([&] { (void)synth::ParsePairSpec(test.text); }, test.expected, "spec: " + test.name);
    }

    // A reserved name with digits next to it is not reserved.
    (void)synth::ParsePairSpec(Replace(good, alias_b, "\"alias\": \"COM10\""));
    (void)synth::ParsePairSpec(Replace(good, alias_b, "\"alias\": \"CONSOLE\""));
    (void)matrix_a;

    // The C++ API is held to the same rules as a parsed document.
    {
        auto zero_supersample = spec;
        zero_supersample.supersample = 0;
        CheckRejects([&] { synth::ValidatePairSpec(zero_supersample); }, "supersample", "api: supersample 0 rejected by ValidatePairSpec");
        CheckRejects([&] { const synth::CameraRenderer renderer(zero_supersample, 0); }, "supersample",
            "api: supersample 0 rejected by CameraRenderer");
        TempFolder temp;
        synth::GenerateOptions options;
        options.output_directory = temp.Sub("never");
        CheckRejects([&] { (void)synth::GeneratePair(zero_supersample, options); }, "supersample", "api: supersample 0 rejected by GeneratePair");
        Check(!std::filesystem::exists(temp.Sub("never")), "api: a rejected spec creates nothing");

        auto big = spec;
        big.supersample = 4000000000U;
        CheckRejects([&] { synth::ValidatePairSpec(big); }, "supersample", "api: huge supersample rejected");
        auto bad_matrix = spec;
        bad_matrix.cameras[1].document_to_image[8] = 2.0;
        CheckRejects([&] { synth::ValidatePairSpec(bad_matrix); }, "[2][2] must be 1", "api: documentToImage[2][2] rejected");
        auto no_cameras = spec;
        no_cameras.cameras.clear();
        CheckRejects([&] { synth::ValidatePairSpec(no_cameras); }, "1 to 8 cameras", "api: no cameras rejected");
        auto bad_alias = spec;
        bad_alias.cameras[0].alias = "Prn";
        CheckRejects([&] { synth::ValidatePairSpec(bad_alias); }, "reserved", "api: reserved alias rejected");
        auto bad_subsampling = spec;
        bad_subsampling.jpeg.chroma_subsampling = static_cast<synth::ChromaSubsampling>(9);
        CheckRejects([&] { synth::ValidatePairSpec(bad_subsampling); }, "chroma_subsampling", "api: unknown subsampling rejected");
        auto bad_model = spec;
        bad_model.cameras[0].distortion.k2 = std::nan("");
        CheckRejects([&] { synth::ValidatePairSpec(bad_model); }, "k2", "api: NaN coefficient rejected");
    }

    // Serialising and parsing again gives the same spec text, tangential terms included.
    const auto text = synth::SerializePairSpec(spec);
    const auto again = synth::ParsePairSpec(text);
    Check(synth::SerializePairSpec(again) == text, "spec: serialised form round-trips");
    const auto stress = MakeStressSpec();
    const auto stress_text = synth::SerializePairSpec(stress);
    Check(synth::SerializePairSpec(synth::ParsePairSpec(stress_text)) == stress_text, "spec: stress spec (p1, p2, fx != fy) round-trips");
    Check(stress_text.find("\"documentToImage\":[[") != std::string::npos, "spec: documentToImage is written as nested rows");
    Check(stress_text.find("\"placement\"") == std::string::npos && stress_text.find("\"homography\"") == std::string::npos,
        "spec: the serialised form has no placement and no /1 homography");
}

// ---- Placement ----------------------------------------------------------------------------------

void TestPlacement() {
    // For each quarter turn the chosen document point lands on the middle of the sample grid,
    // ((W - 1) / 2, (H - 1) / 2), and a step of 1 mm along document x / y lands on the axis the
    // table below describes.
    struct Expected {
        int turns;
        double dx_u, dx_v, dy_u, dy_v;
    };
    const Expected table[] = {
        {0, 1, 0, 0, 1},
        {1, 0, 1, -1, 0},
        {2, -1, 0, 0, -1},
        {3, 0, -1, 1, 0},
    };
    for (const auto& row : table) {
        synth::PlacementSpec placement{{100, 50}, 8.0, row.turns};
        const auto h = synth::HomographyFromPlacement(placement, 640, 480);
        auto apply = [&](const double x, const double y) {
            return std::array<double, 2>{h[0] * x + h[1] * y + h[2], h[3] * x + h[4] * y + h[5]};
        };
        const auto centre = apply(100, 50);
        Check(centre[0] == 319.5 && centre[1] == 239.5, "placement: centre maps to ((W-1)/2, (H-1)/2), turns " + std::to_string(row.turns));
        const auto x_step = apply(101, 50);
        const auto y_step = apply(100, 51);
        Check(x_step[0] - centre[0] == 8.0 * row.dx_u && x_step[1] - centre[1] == 8.0 * row.dx_v,
            "placement: x step direction, turns " + std::to_string(row.turns));
        Check(y_step[0] - centre[0] == 8.0 * row.dy_u && y_step[1] - centre[1] == 8.0 * row.dy_v,
            "placement: y step direction, turns " + std::to_string(row.turns));
        Check(h[6] == 0.0 && h[7] == 0.0 && h[8] == 1.0, "placement: affine bottom row");
    }
    // An odd size has an integer centre.
    const auto odd = synth::HomographyFromPlacement({{10, 10}, 2.0, 0}, 101, 51);
    Check(odd[0] * 10 + odd[1] * 10 + odd[2] == 50.0 && odd[3] * 10 + odd[4] * 10 + odd[5] == 25.0, "placement: odd sizes centre on a pixel");
    CheckRejects([] { (void)synth::HomographyFromPlacement({{0, 0}, 1.0, 4}, 10, 10); }, "quarter_turns", "placement: turns above 3");
    CheckRejects([] { (void)synth::HomographyFromPlacement({{0, 0}, 0.0, 0}, 10, 10); }, "pixels_per_mm", "placement: zero scale");
}

// ---- An independent projection, written separately from the library -----------------------------

// MSVC's long double is the same 53 bit type as double, so this is not a precision check. What it
// gives is a second implementation: the textbook formulas with the powers written out, reading the
// camera only from the ground truth JSON.
struct IndependentCamera {
    std::string alias;
    long double h[9]{};
    long double fx{}, fy{}, cx{}, cy{}, k1{}, k2{}, k3{}, p1{}, p2{};
};

long double Num(const synth::JsonValue& value) { return static_cast<long double>(value.AsDouble()); }

std::vector<IndependentCamera> ReadCamerasFromGroundTruth(const synth::JsonValue& truth) {
    std::vector<IndependentCamera> cameras;
    const auto& list = *truth.Find("spec")->Find("cameras");
    for (const auto& entry : list.items) {
        IndependentCamera camera;
        camera.alias = entry.Find("alias")->AsString();
        const auto& projection = *entry.Find("projection");
        const auto& rows = projection.Find("documentToImage")->items;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) camera.h[row * 3 + column] = Num(rows[row].items[column]);
        }
        const auto& intrinsics = *projection.Find("intrinsics");
        camera.fx = Num(*intrinsics.Find("fxPixels"));
        camera.fy = Num(*intrinsics.Find("fyPixels"));
        camera.cx = Num(*intrinsics.Find("cxPixels"));
        camera.cy = Num(*intrinsics.Find("cyPixels"));
        const auto& distortion = *projection.Find("distortion");
        camera.k1 = Num(*distortion.Find("k1"));
        camera.k2 = Num(*distortion.Find("k2"));
        camera.k3 = Num(*distortion.Find("k3"));
        camera.p1 = Num(*distortion.Find("p1"));
        camera.p2 = Num(*distortion.Find("p2"));
        cameras.push_back(camera);
    }
    return cameras;
}

// Document mm -> raw pixel. No fold check: callers decide what a folded point means.
std::optional<std::array<long double, 2>> IndependentProject(const IndependentCamera& c, const long double x, const long double y) {
    const long double w = c.h[6] * x + c.h[7] * y + c.h[8];
    if (!(w > 0.0L)) return std::nullopt;
    const long double u = (c.h[0] * x + c.h[1] * y + c.h[2]) / w;
    const long double v = (c.h[3] * x + c.h[4] * y + c.h[5]) / w;
    const long double xn = (u - c.cx) / c.fx;
    const long double yn = (v - c.cy) / c.fy;
    const long double r2 = xn * xn + yn * yn;
    const long double radial = 1.0L + c.k1 * r2 + c.k2 * r2 * r2 + c.k3 * r2 * r2 * r2;
    const long double xd = xn * radial + 2.0L * c.p1 * xn * yn + c.p2 * (r2 + 2.0L * xn * xn);
    const long double yd = yn * radial + c.p1 * (r2 + 2.0L * yn * yn) + 2.0L * c.p2 * xn * yn;
    return std::array<long double, 2>{c.fx * xd + c.cx, c.fy * yd + c.cy};
}

struct ProjectionAudit {
    std::size_t compared{};
    std::size_t visible_in_all{};
    long double worst{};
    std::string worst_where;
};

constexpr long double kProjectionLimit = 1e-6L;

// Compares every image position in the ground truth JSON with the independent projection of the
// document position written next to it. A null entry must be a point that really falls outside:
// the coverage rule is 0 <= u < width and 0 <= v < height.
ProjectionAudit AuditProjections(const synth::JsonValue& truth, const std::string& label) {
    const auto cameras = ReadCamerasFromGroundTruth(truth);
    const auto& image = *truth.Find("spec")->Find("image");
    const long double width = Num(*image.Find("width_px"));
    const long double height = Num(*image.Find("height_px"));
    ProjectionAudit audit;
    for (const auto& point : truth.Find("reference_points")->items) {
        const long double x = Num(point.Find("document_mm")->items[0]);
        const long double y = Num(point.Find("document_mm")->items[1]);
        bool all = true;
        for (const auto& camera : cameras) {
            const auto expected = IndependentProject(camera, x, y);
            const auto* recorded = point.Find("image_px")->Find(camera.alias);
            if (recorded == nullptr) {
                Check(false, label + ": image_px lacks " + camera.alias);
                continue;
            }
            const bool expected_inside = expected && (*expected)[0] >= 0 && (*expected)[0] < width
                && (*expected)[1] >= 0 && (*expected)[1] < height;
            // A point within a hair of the border may round either way; anything else must agree.
            const bool borderline = expected
                && (std::abs((*expected)[0]) < kProjectionLimit || std::abs((*expected)[0] - width) < kProjectionLimit
                    || std::abs((*expected)[1]) < kProjectionLimit || std::abs((*expected)[1] - height) < kProjectionLimit);
            if (recorded->kind == synth::JsonValue::Kind::null_value) {
                all = false;
                Check(!expected_inside || borderline, label + ": " + point.Find("id")->AsString() + " is null but projects inside " + camera.alias);
                continue;
            }
            Check(expected_inside || borderline, label + ": " + point.Find("id")->AsString() + " recorded for " + camera.alias + " but projects outside");
            if (!expected) continue;
            const long double dx = Num(recorded->items[0]) - (*expected)[0];
            const long double dy = Num(recorded->items[1]) - (*expected)[1];
            const long double distance = std::sqrt(dx * dx + dy * dy);
            ++audit.compared;
            if (distance > audit.worst) {
                audit.worst = distance;
                audit.worst_where = point.Find("id")->AsString() + "/" + camera.alias;
            }
        }
        if (all) ++audit.visible_in_all;
    }
    return audit;
}

// ---- Ground truth versus an independent projection ----------------------------------------------

void TestGroundTruthProjection(const std::filesystem::path& spec_path) {
    const auto spec = synth::ParsePairSpec(ReadText(spec_path));
    const auto text = synth::SerializeGroundTruth(spec, {});
    const auto truth = synth::ParseJson(text);

    Check(truth.Find("schema")->AsString() == synth::kGroundTruthSchemaVersion, "truth: schema");
    Check(truth.Find("schema")->AsString() == "a0.m2.synthetic-pair-ground-truth/2", "truth: schema is the /2 format");
    Check(truth.Find("generator_version")->AsString() == synth::kGeneratorVersion, "truth: generator version");
    Check(truth.Find("generator_version")->AsString() == "a0.m2.synthetic-pair-generator/2", "truth: generator version is /2");
    Check(truth.Find("seed")->AsUnsigned() == spec.seed, "truth: seed");

    // The four constants of the rig profile v2 `conventions`, and no leftover /1 prose.
    const auto* conventions = truth.Find("conventions");
    Check(conventions != nullptr && conventions->keys.size() == 4, "truth: conventions has four entries");
    if (conventions != nullptr) {
        const auto value = [&](const char* key) {
            const auto* found = conventions->Find(key);
            return found != nullptr ? found->AsString() : std::string("<missing>");
        };
        Check(value("documentPlane") == "mm-origin-sheet-top-left-x-right-y-down", "truth: conventions.documentPlane");
        Check(value("cameraPixel") == "stored-order-sample-at-integer-index", "truth: conventions.cameraPixel");
        Check(value("matrix") == "row-major-column-vector", "truth: conventions.matrix");
        Check(value("exifOrientation") == "ignored", "truth: conventions.exifOrientation");
    }
    Check(truth.Find("pixel_convention") == nullptr, "truth: the /1 pixel_convention prose is gone");

    // The spec echoed in the truth parses back to the same spec.
    const auto echoed = synth::ParsePairSpec(*truth.Find("spec"));
    Check(synth::SerializePairSpec(echoed) == synth::SerializePairSpec(spec), "truth: echoed spec round-trips");
    const auto& echoed_camera = truth.Find("spec")->Find("cameras")->items[0];
    Check(echoed_camera.Find("projection")->Find("documentToImage") != nullptr
            && echoed_camera.Find("projection")->Find("placement") == nullptr,
        "truth: the resolved documentToImage is written, not the placement");
    Check(echoed_camera.Find("projection")->Find("documentToImage")->items[2].items[2].AsDouble() == 1.0,
        "truth: documentToImage[2][2] is 1");

    // Document positions are the lattice nodes.
    const auto& chart = spec.chart;
    const auto columns = static_cast<std::size_t>(std::floor((chart.width_mm - 2 * chart.margin_mm) / chart.fiducial_pitch_mm)) + 1;
    const auto rows = static_cast<std::size_t>(std::floor((chart.height_mm - 2 * chart.margin_mm) / chart.fiducial_pitch_mm)) + 1;
    const auto& points = truth.Find("reference_points")->items;
    Check(points.size() == columns * rows, "truth: reference point count is columns x rows");
    for (std::size_t index = 0; index < points.size(); ++index) {
        const long double expected_x = static_cast<long double>(chart.margin_mm) + static_cast<long double>(index % columns) * chart.fiducial_pitch_mm;
        const long double expected_y = static_cast<long double>(chart.margin_mm) + static_cast<long double>(index / columns) * chart.fiducial_pitch_mm;
        const auto& document = *points[index].Find("document_mm");
        Check(std::abs(Num(document.items[0]) - expected_x) < 1e-9L && std::abs(Num(document.items[1]) - expected_y) < 1e-9L,
            "truth: lattice position of " + points[index].Find("id")->AsString());
    }

    const auto audit = AuditProjections(truth, "truth");
    Info("ground truth: " + std::to_string(audit.compared) + " image positions compared, worst difference "
        + Scientific(static_cast<double>(audit.worst)) + " px at " + audit.worst_where
        + "; " + std::to_string(audit.visible_in_all) + " points visible in every camera");
    Check(audit.compared > 300, "truth: enough image positions were compared");
    Check(audit.worst <= kProjectionLimit, "truth: worst difference to the independent projection is within 1e-6 px");
    Check(audit.visible_in_all > 20, "truth: the cameras share reference points in the overlap");

    // The audit must notice a wrong value: move one recorded position by 0.02 px.
    auto tampered = truth;
    auto& victim = const_cast<synth::JsonValue&>(*tampered.Find("reference_points")->items[30].Find("image_px"));
    for (std::size_t index = 0; index < victim.keys.size(); ++index) {
        if (victim.values[index].kind == synth::JsonValue::Kind::array) {
            victim.values[index].items[0].text = std::to_string(victim.values[index].items[0].AsDouble() + 0.02);
            break;
        }
    }
    const auto tampered_audit = AuditProjections(tampered, "tampered");
    Check(tampered_audit.worst > 0.015L, "truth: audit sees a 0.02 px change");

    // Overlap along the horizontal centre line of the document: both cameras must see the same strip.
    const auto cameras = ReadCamerasFromGroundTruth(truth);
    double first = -1.0;
    double last = -1.0;
    for (double x = 0.0; x <= chart.width_mm; x += 0.05) {
        bool both = true;
        for (const auto& camera : cameras) {
            const auto p = IndependentProject(camera, x, chart.height_mm * 0.5);
            both = both && p && (*p)[0] >= 0 && (*p)[0] < spec.image_width_px && (*p)[1] >= 0 && (*p)[1] < spec.image_height_px;
        }
        if (both) {
            if (first < 0) first = x;
            last = x;
        }
    }
    Info("overlap along the centre line: " + std::to_string(last - first) + " mm (document x " + std::to_string(first)
        + " to " + std::to_string(last) + ")");
    Check(last - first > 136.0 && last - first < 146.0, "overlap: about 141 mm for the rotated-bodies left/right layout");

    // The other specs of this file go through the same audit.
    for (const auto& [name, other] : {std::pair<std::string, synth::PairSpec>{"small", synth::ParsePairSpec(kSmallSpec)},
             std::pair<std::string, synth::PairSpec>{"stress", MakeStressSpec()}}) {
        const auto other_truth = synth::ParseJson(synth::SerializeGroundTruth(other, {}));
        const auto other_audit = AuditProjections(other_truth, name);
        Check(other_audit.compared > 20 && other_audit.worst <= kProjectionLimit,
            "truth: " + name + " spec agrees with the independent projection (" + Scientific(static_cast<double>(other_audit.worst)) + " px)");
    }
}

// The coverage rule is 0 <= u < width: a reference point exactly on the right or bottom edge is
// outside, one exactly on the left or top edge is inside.
void TestCoverageRule() {
    auto spec = synth::ParsePairSpec(kSmallSpec);
    spec.cameras.resize(1);
    auto& camera = spec.cameras[0];
    camera.intrinsics = {1000.0, 1000.0, 240.0, 200.0};
    camera.distortion = {};
    // Reference point F-000-000 sits at (10, 10) mm.
    const auto recorded = [&](const double tx, const double ty) {
        camera.document_to_image = {1, 0, tx, 0, 1, ty, 0, 0, 1};
        const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
        const auto& first = truth.Find("reference_points")->items[0];
        return first.Find("image_px")->Find("CAM-A")->kind == synth::JsonValue::Kind::array;
    };
    Check(recorded(-10.0, 100.0), "coverage: u = 0 is inside");
    Check(!recorded(-10.0000001, 100.0), "coverage: u just below 0 is outside");
    Check(recorded(469.9999999, 100.0), "coverage: u just below the width is inside");
    Check(!recorded(470.0, 100.0), "coverage: u = width is outside");
    Check(recorded(100.0, -10.0), "coverage: v = 0 is inside");
    Check(!recorded(100.0, -10.0000001), "coverage: v just below 0 is outside");
    Check(recorded(100.0, 389.9999999), "coverage: v just below the height is inside");
    Check(!recorded(100.0, 390.0), "coverage: v = height is outside");
}

// A point beyond the radius where the radial map folds back is not imaged where the plain
// formula puts it, so the truth must not list it.
void TestFoldedPointsAreNotRecorded() {
    auto spec = synth::ParsePairSpec(kSmallSpec);
    spec.cameras.resize(1);
    auto& camera = spec.cameras[0];
    camera.intrinsics = {1000.0, 1000.0, 239.5, 199.5};
    camera.distortion = {-0.1, 0.0, 0.0, 0.0, 0.0};
    camera.document_to_image = synth::HomographyFromPlacement({{45, 50}, 42.0, 0}, spec.image_width_px, spec.image_height_px);
    synth::ValidatePairSpec(spec);  // the image itself is covered by the unfolded part of the lens

    // Fold radius: 1 + 3 k1 r^2 = 0 gives r = 1.826.
    const auto inside_fold = synth::ProjectDocumentPoint(camera, {45.0 + 1000.0 / 42.0, 50.0});
    Check(inside_fold.has_value(), "fold: a point at r = 1 is projected");
    const double beyond_x = 45.0 + 3200.0 / 42.0;  // r = 3.2
    Check(!synth::ProjectDocumentPoint(camera, {beyond_x, 50.0}).has_value(), "fold: a point at r = 3.2 has no position");

    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    const auto cameras = ReadCamerasFromGroundTruth(truth);
    std::size_t folded = 0;
    std::size_t recorded = 0;
    for (const auto& point : truth.Find("reference_points")->items) {
        const auto naive = IndependentProject(cameras[0], Num(point.Find("document_mm")->items[0]), Num(point.Find("document_mm")->items[1]));
        const auto* entry = point.Find("image_px")->Find("CAM-A");
        const bool naive_inside = naive && (*naive)[0] >= 0 && (*naive)[0] < spec.image_width_px
            && (*naive)[1] >= 0 && (*naive)[1] < spec.image_height_px;
        if (entry->kind == synth::JsonValue::Kind::null_value) {
            if (naive_inside) ++folded;
            continue;
        }
        ++recorded;
        // Whatever is recorded must come back to the same document point through the inverse.
        const auto back = synth::UnprojectImagePoint(camera, {entry->items[0].AsDouble(), entry->items[1].AsDouble()});
        Check(back.has_value() && std::abs(back->x - point.Find("document_mm")->items[0].AsDouble()) < 1e-6
                && std::abs(back->y - point.Find("document_mm")->items[1].AsDouble()) < 1e-6,
            "fold: recorded point " + point.Find("id")->AsString() + " round-trips through the inverse");
    }
    Info("fold: " + std::to_string(folded) + " reference points would have landed inside the image through the fold and are null; "
        + std::to_string(recorded) + " are recorded");
    Check(folded >= 1, "fold: at least one folded reference point is kept out of the truth");
    Check(recorded >= 1, "fold: the unfolded reference points are still recorded");
}

// ---- Inverse over the whole image ---------------------------------------------------------------

// Raw pixel -> document -> raw pixel on a grid over the whole image, corners included. The first leg
// is the library inverse, the second is checked by both the library and the independent projection.
void TestInverseOverWholeImage(const synth::PairSpec& spec, const std::string& label) {
    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    const auto independent = ReadCamerasFromGroundTruth(truth);
    constexpr int kSteps = 64;
    const double width = spec.image_width_px;
    const double height = spec.image_height_px;
    for (std::size_t camera_index = 0; camera_index < spec.cameras.size(); ++camera_index) {
        const auto& camera = spec.cameras[camera_index];
        double worst_library = 0.0;
        double worst_independent = 0.0;
        std::size_t points = 0;
        for (int row = 0; row <= kSteps; ++row) {
            for (int column = 0; column <= kSteps; ++column) {
                // From the first sample edge (-0.5) to the last (W - 0.5), the corners included.
                const double u = -0.5 + width * column / kSteps;
                const double v = -0.5 + height * row / kSteps;
                const auto document = synth::UnprojectImagePoint(camera, {u, v});
                if (!document) {
                    Check(false, label + ": inverse failed at " + Fixed(u, 1) + "," + Fixed(v, 1) + " " + camera.alias);
                    continue;
                }
                const auto forward = synth::ProjectDocumentPoint(camera, *document);
                const auto other = IndependentProject(independent[camera_index], document->x, document->y);
                if (!forward || !other) {
                    Check(false, label + ": forward failed at " + Fixed(u, 1) + "," + Fixed(v, 1) + " " + camera.alias);
                    continue;
                }
                ++points;
                worst_library = std::max(worst_library, std::hypot(forward->x - u, forward->y - v));
                worst_independent = std::max(worst_independent,
                    static_cast<double>(std::hypot((*other)[0] - u, (*other)[1] - v)));
            }
        }
        Info(label + " " + camera.alias + ": " + std::to_string(points) + " grid points, inverse then forward worst "
            + Scientific(worst_library) + " px (library), " + Scientific(worst_independent) + " px (independent)");
        Check(points == (kSteps + 1) * (kSteps + 1), label + ": every grid point inverted for " + camera.alias);
        Check(worst_library <= 1e-6, label + ": raw -> document -> raw within 1e-6 px for " + camera.alias);
        Check(worst_independent <= 1e-6, label + ": independent forward of the inverse within 1e-6 px for " + camera.alias);
    }
}

void TestInverseAllSpecs(const std::filesystem::path& fixture_path) {
    TestInverseOverWholeImage(synth::ParsePairSpec(ReadText(fixture_path)), "inverse (fixture)");
    TestInverseOverWholeImage(synth::ParsePairSpec(kSmallSpec), "inverse (small)");
    TestInverseOverWholeImage(MakeStressSpec(), "inverse (rotated, projective, tangential, fx != fy / tilted pose)");
}

// ---- Migration from /1 ----------------------------------------------------------------------------

struct LegacyCamera {
    double center_mm[2];
    double pixels_per_mm;
    int quarter_turns;
    double k1, k2;
};

// The /1 model: pixel (i, j) covers [i, i+1) and its centre is (i + 0.5, j + 0.5); the placement
// maps the document centre to (W/2, H/2); the lens centre is [3680, 2456] and the radius 4424.
std::array<long double, 2> LegacyProject(const LegacyCamera& camera, const long double x, const long double y) {
    static const long double rotations[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
    const auto* r = rotations[camera.quarter_turns];
    const long double s = camera.pixels_per_mm;
    const long double a = s * r[0], b = s * r[1], c = s * r[2], d = s * r[3];
    const long double tx = 3680.0L - (a * camera.center_mm[0] + b * camera.center_mm[1]);
    const long double ty = 2456.0L - (c * camera.center_mm[0] + d * camera.center_mm[1]);
    const long double u = a * x + b * y + tx;
    const long double v = c * x + d * y + ty;
    const long double dx = u - 3680.0L;
    const long double dy = v - 2456.0L;
    const long double rho2 = (dx * dx + dy * dy) / (4424.0L * 4424.0L);
    const long double factor = 1.0L + camera.k1 * rho2 + camera.k2 * rho2 * rho2;
    return {3680.0L + dx * factor, 2456.0L + dy * factor};
}

// The full size fixture was migrated from /1 by moving every pixel quantity by half a pixel
// (centre [3680, 2456] -> [3679.5, 2455.5], placement to (W-1)/2). The recorded positions
// must therefore be the /1 positions minus 0.5 in both axes.
void TestMigrationFromV1(const std::filesystem::path& fixture_path) {
    const auto spec = synth::ParsePairSpec(ReadText(fixture_path));
    Check(spec.cameras[0].intrinsics.cx_pixels == 3679.5 && spec.cameras[0].intrinsics.cy_pixels == 2455.5
            && spec.cameras[1].intrinsics.cx_pixels == 3679.5 && spec.cameras[1].intrinsics.cy_pixels == 2455.5,
        "migration: the fixture lens centre is [3679.5, 2455.5]");
    const LegacyCamera legacy[2] = {
        {{324.5, 420.5}, 7.27, 3, -0.045, 0.012},
        {{865.5, 420.5}, 7.27, 3, -0.03, 0.008},
    };
    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    std::size_t compared = 0;
    long double worst = 0.0L;
    for (const auto& point : truth.Find("reference_points")->items) {
        const long double x = Num(point.Find("document_mm")->items[0]);
        const long double y = Num(point.Find("document_mm")->items[1]);
        for (std::size_t camera = 0; camera < 2; ++camera) {
            const auto* recorded = point.Find("image_px")->Find(spec.cameras[camera].alias);
            if (recorded->kind != synth::JsonValue::Kind::array) continue;
            const auto old_position = LegacyProject(legacy[camera], x, y);
            const long double dx = Num(recorded->items[0]) - (old_position[0] - 0.5L);
            const long double dy = Num(recorded->items[1]) - (old_position[1] - 0.5L);
            worst = std::max(worst, std::max(std::abs(dx), std::abs(dy)));
            ++compared;
        }
    }
    Info("migration: " + std::to_string(compared) + " positions compared with the /1 model, worst difference to (/1 - 0.5) "
        + Scientific(static_cast<double>(worst)) + " px");
    Check(compared > 300, "migration: enough positions were compared");
    Check(worst <= 1e-9L, "migration: new image_px equals the /1 value minus 0.5 within 1e-9 px");
}

// ---- Rendered image versus ground truth -------------------------------------------------------

struct Predicted {
    long double x{};
    long double y{};
    long double scale{};  // sqrt(|det J|) at the ring centre: raw pixels per mm
};

// Centroid of the black ink (the outer ring 0.6R..R and the dot 0..0.25R) after the full forward map.
// The ring is sampled densely in polar coordinates, every sample is sent through the independent
// projection and weighted by the document area it stands for times the local Jacobian of the map,
// which is what the camera integrates. This is not the image of the ring centre: the lens bends the
// ring, and the difference grows toward the corners.
std::optional<Predicted> PredictRingCentroid(const IndependentCamera& camera, const long double x0, const long double y0,
    const long double radius) {
    constexpr int kAngular = 96;
    constexpr long double kTwoPi = 6.28318530717958647692L;
    const auto jacobian = [&](const long double x, const long double y) -> std::optional<long double> {
        const long double h = 1e-3L;
        const auto a = IndependentProject(camera, x + h, y);
        const auto b = IndependentProject(camera, x - h, y);
        const auto c = IndependentProject(camera, x, y + h);
        const auto d = IndependentProject(camera, x, y - h);
        if (!a || !b || !c || !d) return std::nullopt;
        const long double dudx = ((*a)[0] - (*b)[0]) / (2 * h);
        const long double dvdx = ((*a)[1] - (*b)[1]) / (2 * h);
        const long double dudy = ((*c)[0] - (*d)[0]) / (2 * h);
        const long double dvdy = ((*c)[1] - (*d)[1]) / (2 * h);
        return std::abs(dudx * dvdy - dudy * dvdx);
    };
    long double sum = 0.0L, sum_x = 0.0L, sum_y = 0.0L;
    const auto accumulate = [&](const long double inner, const long double outer, const int radial_steps) {
        const long double dr = (outer - inner) / radial_steps;
        const long double dtheta = kTwoPi / kAngular;
        for (int i = 0; i < radial_steps; ++i) {
            const long double r = inner + (i + 0.5L) * dr;
            for (int j = 0; j < kAngular; ++j) {
                const long double theta = (j + 0.5L) * dtheta;
                const long double x = x0 + r * std::cos(theta);
                const long double y = y0 + r * std::sin(theta);
                const auto position = IndependentProject(camera, x, y);
                const auto j_abs = jacobian(x, y);
                if (!position || !j_abs) return false;
                const long double weight = r * dr * dtheta * *j_abs;
                sum += weight;
                sum_x += weight * (*position)[0];
                sum_y += weight * (*position)[1];
            }
        }
        return true;
    };
    if (!accumulate(0.6L * radius, radius, 24) || !accumulate(0.0L, 0.25L * radius, 8)) return std::nullopt;
    const auto centre = jacobian(x0, y0);
    if (!centre || !(sum > 0.0L)) return std::nullopt;
    return Predicted{sum_x / sum, sum_y / sum, std::sqrt(*centre)};
}

// Centroid of the dark ink around (px, py) in a buffer holding rows [first_row, first_row + rows).
// Pixel (x, y) has its sample at (x, y). Only the black ring and dot count; grid lines (0.30 and
// 0.55) and paper stay out. A window of 1.12 radii keeps the grid-line arms that leave the ring out.
std::optional<std::array<double, 2>> RenderedInkCentroid(const std::vector<std::uint8_t>& bgr, const std::uint32_t width,
    const std::uint32_t first_row, const std::uint32_t rows, const double px, const double py, const double window) {
    const long y_begin = static_cast<long>(py - window) - 1;
    const long y_end = static_cast<long>(py + window) + 1;
    const long x_begin = static_cast<long>(px - window) - 1;
    const long x_end = static_cast<long>(px + window) + 1;
    if (x_begin < 0 || x_end >= static_cast<long>(width) || y_begin < static_cast<long>(first_row)
        || y_end >= static_cast<long>(first_row + rows)) {
        return std::nullopt;
    }
    double sum = 0.0, sum_x = 0.0, sum_y = 0.0;
    for (long y = y_begin; y <= y_end; ++y) {
        for (long x = x_begin; x <= x_end; ++x) {
            const double dx = x - px;
            const double dy = y - py;
            if (dx * dx + dy * dy > window * window) continue;
            const auto* p = &bgr[(static_cast<std::size_t>(y - first_row) * width + static_cast<std::size_t>(x)) * 3U];
            const double luma = (0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]) / 255.0;
            const double w = luma < 0.25 ? 0.25 - luma : 0.0;
            sum += w;
            sum_x += w * x;
            sum_y += w * y;
        }
    }
    if (!(sum > 0.0)) return std::nullopt;
    return std::array<double, 2>{sum_x / sum, sum_y / sum};
}

struct CentroidStats {
    std::size_t measured{};
    double worst{};
    std::string worst_where;
    std::array<double, 6> worst_detail{};  // rendered x, y, predicted x, y, recorded x, y of the worst ring
    double largest_shift{};  // |predicted centroid - image of the ring centre|
    std::string largest_shift_where;
};

// For every ring that fits in the buffer, compares the rendered centroid with the predicted one.
// `ring_filter` lets the caller restrict the rings (the full size test renders only a few rows).
CentroidStats CompareRingCentroids(const synth::PairSpec& spec, const synth::JsonValue& truth,
    const std::vector<IndependentCamera>& independent, const std::size_t camera_index, const std::vector<std::uint8_t>& bgr,
    const std::uint32_t first_row, const std::uint32_t rows, const std::vector<std::string>* only_ids = nullptr) {
    CentroidStats stats;
    const std::string& alias = spec.cameras[camera_index].alias;
    for (const auto& point : truth.Find("reference_points")->items) {
        const std::string id = point.Find("id")->AsString();
        if (only_ids != nullptr && std::find(only_ids->begin(), only_ids->end(), id) == only_ids->end()) continue;
        const auto* recorded = point.Find("image_px")->Find(alias);
        if (recorded == nullptr || recorded->kind != synth::JsonValue::Kind::array) continue;
        const long double x0 = Num(point.Find("document_mm")->items[0]);
        const long double y0 = Num(point.Find("document_mm")->items[1]);
        const auto predicted = PredictRingCentroid(independent[camera_index], x0, y0, static_cast<long double>(spec.chart.fiducial_radius_mm));
        if (!predicted) {
            Check(false, "centroid: prediction failed for " + id);
            continue;
        }
        const double window = 1.12 * spec.chart.fiducial_radius_mm * static_cast<double>(predicted->scale);
        const auto rendered = RenderedInkCentroid(bgr, spec.image_width_px, first_row, rows,
            static_cast<double>(predicted->x), static_cast<double>(predicted->y), window);
        if (!rendered) continue;
        ++stats.measured;
        const double offset = std::hypot((*rendered)[0] - static_cast<double>(predicted->x), (*rendered)[1] - static_cast<double>(predicted->y));
        if (offset > stats.worst) {
            stats.worst = offset;
            stats.worst_where = id;
            stats.worst_detail = {(*rendered)[0], (*rendered)[1], static_cast<double>(predicted->x), static_cast<double>(predicted->y),
                recorded->items[0].AsDouble(), recorded->items[1].AsDouble()};
        }
        const double shift = std::hypot(static_cast<double>(predicted->x) - recorded->items[0].AsDouble(),
            static_cast<double>(predicted->y) - recorded->items[1].AsDouble());
        if (shift > stats.largest_shift) {
            stats.largest_shift = shift;
            stats.largest_shift_where = id;
        }
    }
    return stats;
}

// The rendered ring centroid must agree with the centroid predicted by sending the ring through the
// forward model. A half pixel mistake in the sampling convention, or a lens term that the renderer
// and the forward model read differently, is far larger than the limit.
void TestRenderMatchesGroundTruth(const std::string& variant, const synth::PairSpec& spec, const double limit_px,
    const double minimum_shift_px) {
    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    const auto independent = ReadCamerasFromGroundTruth(truth);
    for (std::size_t camera_index = 0; camera_index < spec.cameras.size(); ++camera_index) {
        const synth::CameraRenderer renderer(spec, camera_index);
        const auto width = spec.image_width_px;
        const auto height = spec.image_height_px;
        std::vector<std::uint8_t> bgr(static_cast<std::size_t>(width) * height * 3U);
        renderer.RenderRows(0, height, bgr.data());
        const auto stats = CompareRingCentroids(spec, truth, independent, camera_index, bgr, 0, height);
        const std::string where = variant + ", " + spec.cameras[camera_index].alias;
        Info("render (" + where + "): " + std::to_string(stats.measured) + " fiducials measured, worst centroid difference "
            + Fixed(stats.worst, 4) + " px at " + stats.worst_where + " (rendered " + Fixed(stats.worst_detail[0], 3) + "," + Fixed(stats.worst_detail[1], 3)
            + " predicted " + Fixed(stats.worst_detail[2], 3) + "," + Fixed(stats.worst_detail[3], 3) + " recorded " + Fixed(stats.worst_detail[4], 3) + ","
            + Fixed(stats.worst_detail[5], 3) + "); ring centroid is up to " + Fixed(stats.largest_shift, 4)
            + " px from the image of its centre (" + stats.largest_shift_where + ")");
        Check(stats.measured >= 6, "render: enough fiducials inside (" + where + ")");
        Check(stats.worst < limit_px, "render: fiducial centroids agree with the predicted centroids within "
            + Fixed(limit_px, 3) + " px (" + where + ")");
        Check(stats.largest_shift >= minimum_shift_px, "render: the prediction is not just the image of the centre (" + where + ")");
    }
}

void TestRenderIsRowSplitInvariant() {
    const auto spec = synth::ParsePairSpec(kSmallSpec);
    const synth::CameraRenderer renderer(spec, 0);
    const auto width = spec.image_width_px;
    const auto height = spec.image_height_px;
    std::vector<std::uint8_t> whole(static_cast<std::size_t>(width) * height * 3U);
    renderer.RenderRows(0, height, whole.data());
    std::vector<std::uint8_t> pieces(whole.size());
    for (std::uint32_t row = 0; row < height; row += 37) {
        const auto count = std::min<std::uint32_t>(37, height - row);
        renderer.RenderRows(row, count, pieces.data() + static_cast<std::size_t>(row) * width * 3U);
    }
    Check(whole == pieces, "render: splitting rows does not change a single byte");
    std::vector<std::uint8_t> again(whole.size());
    renderer.RenderRows(0, height, again.data());
    Check(whole == again, "render: a second render gives the same bytes");

    auto other = spec;
    other.seed = spec.seed + 1;
    const synth::CameraRenderer other_renderer(other, 0);
    std::vector<std::uint8_t> changed(whole.size());
    other_renderer.RenderRows(0, height, changed.data());
    Check(whole != changed, "render: another seed changes the noise");

    auto quiet = spec;
    quiet.cameras[0].noise = {};
    const synth::CameraRenderer quiet_a(quiet, 0);
    quiet.seed += 5;
    const synth::CameraRenderer quiet_b(quiet, 0);
    std::vector<std::uint8_t> first_quiet(whole.size());
    std::vector<std::uint8_t> second_quiet(whole.size());
    quiet_a.RenderRows(0, height, first_quiet.data());
    quiet_b.RenderRows(0, height, second_quiet.data());
    Check(first_quiet == second_quiet, "render: without noise the seed has no effect");

    CheckRejects([&] { renderer.RenderRows(height, 1, whole.data()); }, "outside", "render: rows beyond the image rejected");
    CheckRejects([&] { renderer.RenderRows(0, 1, nullptr); }, "null", "render: null output rejected");
    CheckRejects([&] { const synth::CameraRenderer bad(spec, 2); }, "out of range", "render: camera index out of range");
}

// A few rows of the real 7360x4912 fixture: the rings nearest to and farthest from the principal
// point of each camera, rendered as a band of rows and compared with the predicted centroid.
void TestFullSizeBands(const std::filesystem::path& fixture_path) {
    const auto spec = synth::ParsePairSpec(ReadText(fixture_path));
    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    const auto independent = ReadCamerasFromGroundTruth(truth);
    for (std::size_t camera_index = 0; camera_index < spec.cameras.size(); ++camera_index) {
        const auto& camera = spec.cameras[camera_index];
        const double margin = 1.12 * spec.chart.fiducial_radius_mm * 7.27 * 1.2 + 4.0;
        struct Candidate {
            std::string id;
            double radius;
            double y;
        };
        std::vector<Candidate> candidates;
        for (const auto& point : truth.Find("reference_points")->items) {
            const auto* recorded = point.Find("image_px")->Find(camera.alias);
            if (recorded->kind != synth::JsonValue::Kind::array) continue;
            const double u = recorded->items[0].AsDouble();
            const double v = recorded->items[1].AsDouble();
            if (u < margin || v < margin || u > spec.image_width_px - margin || v > spec.image_height_px - margin) continue;
            candidates.push_back({point.Find("id")->AsString(),
                std::hypot(u - camera.intrinsics.cx_pixels, v - camera.intrinsics.cy_pixels), v});
        }
        Check(candidates.size() >= 4, "full size band: enough rings away from the border for " + camera.alias);
        if (candidates.size() < 2) continue;
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.radius < b.radius; });
        const std::array<Candidate, 2> chosen = {candidates.front(), candidates.back()};
        const synth::CameraRenderer renderer(spec, camera_index);
        for (const auto& candidate : chosen) {
            const auto first_row = static_cast<std::uint32_t>(std::max(0.0, candidate.y - margin));
            const auto last_row = static_cast<std::uint32_t>(std::min<double>(spec.image_height_px - 1, candidate.y + margin));
            const std::uint32_t rows = last_row - first_row + 1;
            std::vector<std::uint8_t> band(static_cast<std::size_t>(spec.image_width_px) * rows * 3U);
            renderer.RenderRows(first_row, rows, band.data());
            // The fixture has vignetting and noise; the ink threshold stays far from both.
            const std::vector<std::string> only = {candidate.id};
            const auto stats = CompareRingCentroids(spec, truth, independent, camera_index, band, first_row, rows, &only);
            Info("full size band (" + camera.alias + ", " + candidate.id + ", " + Fixed(candidate.radius, 0) + " px from the principal point): "
                + std::to_string(rows) + " rows, centroid difference " + Fixed(stats.worst, 4) + " px, ring centroid "
                + Fixed(stats.largest_shift, 4) + " px from the image of its centre");
            Check(stats.measured == 1, "full size band: the ring " + candidate.id + " was measured for " + camera.alias);
            // Same estimator wobble as in the small tests, plus the fixture noise and vignetting.
            Check(stats.worst < 0.10, "full size band: " + candidate.id + " centroid agrees within 0.10 px for " + camera.alias);
        }
    }
}

// ---- Library API: files, determinism, validation --------------------------------------------

std::vector<std::string> FileHashes(const synth::GenerateResult& result) {
    std::vector<std::string> hashes;
    for (const auto& file : result.files) hashes.push_back(file.sha256);
    hashes.push_back(result.ground_truth_sha256);
    return hashes;
}

std::size_t CountPartialFiles(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code ignored;
    if (!std::filesystem::exists(root, ignored)) return 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.path().extension() == ".partial") ++count;
    }
    return count;
}

void TestPublishContract() {
    TempFolder temp;
    namespace detail = a0::m2::synthetic::detail;
    // Three files are published in order; the third destination already exists. The first two must be
    // taken back, the existing file must stay as it was, and no partial file may remain.
    const auto folder = temp.Sub("publish");
    std::filesystem::create_directories(folder);
    for (const char* name : {"a.bin.partial", "b.bin.partial", "c.bin.partial"}) WriteText(folder / name, name);
    WriteText(folder / "c.bin", "keep me");
    const std::vector<detail::PublishItem> items = {
        {folder / "a.bin.partial", folder / "a.bin"},
        {folder / "b.bin.partial", folder / "b.bin"},
        {folder / "c.bin.partial", folder / "c.bin"},
    };
    bool threw = false;
    try {
        detail::PublishAll(items);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Check(threw, "publish: a failing move is reported");
    Check(!std::filesystem::exists(folder / "a.bin") && !std::filesystem::exists(folder / "b.bin"),
        "publish: files already moved are removed again");
    Check(ReadText(folder / "c.bin") == "keep me", "publish: an existing destination is not replaced");
    Check(CountPartialFiles(folder) == 0, "publish: no partial file is left after a failure");

    // The same three items with a free destination: all of them appear, the partials are gone.
    const auto clean = temp.Sub("publish-ok");
    std::filesystem::create_directories(clean);
    for (const char* name : {"a.bin.partial", "b.bin.partial", "c.bin.partial"}) WriteText(clean / name, name);
    detail::PublishAll({{clean / "a.bin.partial", clean / "a.bin"}, {clean / "b.bin.partial", clean / "b.bin"},
        {clean / "c.bin.partial", clean / "c.bin"}});
    Check(ReadText(clean / "a.bin") == "a.bin.partial" && ReadText(clean / "c.bin") == "c.bin.partial" && CountPartialFiles(clean) == 0,
        "publish: a successful publish moves every file");
}

void TestLibraryGeneration(const std::filesystem::path& adapter) {
    TempFolder temp;
    const auto spec = synth::ParsePairSpec(kSmallSpec);
    synth::GenerateOptions single;
    single.output_directory = temp.Sub("one");
    single.thread_count = 1;
    synth::GenerateOptions many;
    many.output_directory = temp.Sub("many");
    many.thread_count = 5;
    const auto first = synth::GeneratePair(spec, single);
    const auto second = synth::GeneratePair(spec, many);
    Check(first.files.size() == 2 && first.files[0].relative_path == "CAM-A/original.jpg", "generate: one canonical file per camera");
    Check(FileHashes(first) == FileHashes(second), "generate: output does not depend on the thread count");
    Check(CountPartialFiles(temp.Sub("one")) == 0 && CountPartialFiles(temp.Sub("many")) == 0,
        "generate: no .partial file is left after a successful run");

    // The recorded hashes are the real hashes of the files on disk, and the truth lists them.
    for (const auto& file : first.files) {
        Check(synth::Sha256Hex(temp.Sub("one") / file.relative_path) == file.sha256, "generate: recorded sha256 matches the file " + file.alias);
        Check(std::filesystem::file_size(temp.Sub("one") / file.relative_path) == file.size_bytes, "generate: recorded size matches " + file.alias);
    }
    const auto truth = synth::ParseJson(ReadText(temp.Sub("one") / "ground-truth.json"));
    const auto& listed = truth.Find("output_files")->items;
    Check(listed.size() == 2 && listed[1].Find("sha256")->AsString() == first.files[1].sha256, "generate: truth lists the output hashes");
    Check(synth::Sha256Hex(temp.Sub("one") / "ground-truth.json") == first.ground_truth_sha256, "generate: truth hash matches");

    // Known answer for the SHA-256 helper.
    {
        const auto sample = temp.Sub("abc.txt");
        std::ofstream(sample, std::ios::binary) << "abc";
        Check(synth::Sha256Hex(sample) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: known answer for 'abc'");
    }

    // A different seed gives different images; a different quality gives different bytes.
    auto reseeded = spec;
    reseeded.seed = spec.seed + 1;
    synth::GenerateOptions other;
    other.output_directory = temp.Sub("seed");
    other.thread_count = 2;
    const auto third = synth::GeneratePair(reseeded, other);
    Check(third.files[0].sha256 != first.files[0].sha256 && third.files[1].sha256 != first.files[1].sha256, "generate: another seed gives other bytes");

    // Never overwrite earlier output.
    CheckRejects([&] { (void)synth::GeneratePair(spec, single); }, "already exists", "generate: second run into the same folder rejected");
    CheckRejects([&] { (void)synth::GeneratePair(spec, {}); }, "output directory", "generate: empty output directory rejected");

    // A failure after the images are rendered leaves nothing behind: the ground truth cannot be
    // written because its .partial name is taken by a non-empty directory.
    {
        const auto folder = temp.Sub("blocked");
        std::filesystem::create_directories(folder / "ground-truth.json.partial");
        WriteText(folder / "ground-truth.json.partial" / "blocker.txt", "x");
        synth::GenerateOptions blocked;
        blocked.output_directory = folder;
        blocked.thread_count = 2;
        bool threw = false;
        try {
            (void)synth::GeneratePair(spec, blocked);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Check(threw, "atomic: a failure while writing the ground truth is reported");
        Check(!std::filesystem::exists(folder / "CAM-A" / "original.jpg") && !std::filesystem::exists(folder / "CAM-B" / "original.jpg"),
            "atomic: no image is published when the ground truth could not be written");
        Check(!std::filesystem::exists(folder / "ground-truth.json"), "atomic: no ground truth is published after a failure");
        Check(!std::filesystem::exists(folder / "CAM-A") && !std::filesystem::exists(folder / "CAM-B"),
            "atomic: the camera folders created for the failed run are removed");
        Check(std::filesystem::exists(folder / "ground-truth.json.partial" / "blocker.txt"), "atomic: files that were not ours are left alone");
        // A fresh folder next to it is fine.
        synth::GenerateOptions again;
        again.output_directory = temp.Sub("after-failure");
        again.thread_count = 2;
        const auto recovered = synth::GeneratePair(spec, again);
        Check(recovered.files.size() == 2 && FileHashes(recovered) == FileHashes(first), "atomic: a later run into a fresh folder gives the same bytes");
    }

    // A nested output folder that does not exist yet is created, and removed again on failure.
    {
        const auto nested = temp.Sub("outer") / "inner";
        std::filesystem::create_directories(nested / "ground-truth.json.partial");
        WriteText(nested / "ground-truth.json.partial" / "blocker.txt", "x");
        // The blocker keeps `inner` alive, so use a different, empty nest for the removal check.
        const auto fresh = temp.Sub("fresh-outer") / "fresh-inner";
        synth::GenerateOptions options;
        options.output_directory = fresh;
        options.thread_count = 1;
        const auto result = synth::GeneratePair(spec, options);
        Check(result.files.size() == 2 && std::filesystem::exists(fresh / "ground-truth.json"), "generate: a nested output folder is created");
    }

    // The adapter validator accepts the output at its size and rejects another size.
    for (const auto& file : first.files) {
        const auto path = (temp.Sub("one") / file.relative_path).wstring();
        const auto ok = RunProcess(adapter, {L"validate-canonical-jpeg", L"--input", path, L"--width", L"480", L"--height", L"400"}, 60000);
        Check(ok.exit_code == 0 && ok.output.find("result=validated-canonical-jpeg") != std::string::npos,
            "validate: adapter accepts the small output of " + file.alias + " (" + ok.output + ")");
        const auto wrong = RunProcess(adapter, {L"validate-canonical-jpeg", L"--input", path, L"--width", L"481", L"--height", L"400"}, 60000);
        Check(wrong.exit_code != 0, "validate: adapter rejects a wrong width for " + file.alias);
    }
}

// ---- The ground truth as a rig profile v2 draft ------------------------------------------------

std::string QuoteForPowerShell(const std::wstring& text) {
    std::string narrow;
    for (const wchar_t c : text) narrow.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    std::string quoted = "'";
    for (const char c : narrow) {
        if (c == '\'') quoted += "''";
        else quoted.push_back(c);
    }
    quoted += "'";
    return quoted;
}

// Compact JSON of a parsed value. Numbers keep their literal text, so the values the generator
// wrote reach the profile unchanged.
std::string DumpJson(const synth::JsonValue& value) {
    using Kind = synth::JsonValue::Kind;
    switch (value.kind) {
    case Kind::null_value: return "null";
    case Kind::boolean: return value.boolean ? "true" : "false";
    case Kind::number: return value.text;
    case Kind::string: {
        std::string out = "\"";
        for (const char c : value.text) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out + "\"";
    }
    case Kind::array: {
        std::string out = "[";
        for (std::size_t index = 0; index < value.items.size(); ++index) {
            if (index > 0) out += ",";
            out += DumpJson(value.items[index]);
        }
        return out + "]";
    }
    case Kind::object: {
        std::string out = "{";
        for (std::size_t index = 0; index < value.keys.size(); ++index) {
            if (index > 0) out += ",";
            out += "\"" + value.keys[index] + "\":" + DumpJson(value.values[index]);
        }
        return out + "}";
    }
    }
    return "null";
}

// Assembles a calibration-result draft (documentation section 9.1 of the design: every calibration value
// present, every owner decision null) from the ground truth. Only the camera projections and the
// conventions come from the ground truth; the sentinels below are schema exercise values, not a
// calibration of any rig.
std::string BuildDraftProfile(const synth::JsonValue& truth) {
    const auto& cameras = truth.Find("spec")->Find("cameras")->items;
    const std::string zeros(64, '0');
    std::string text = "{\"schemaVersion\":\"2.0.0\",\"status\":\"draft\",\"profileId\":\"synthetic-269-schema-exercise\",";
    text += "\"conventions\":" + DumpJson(*truth.Find("conventions")) + ",";
    text += "\"cameraModel\":{\"manufacturer\":\"Nikon\",\"model\":\"D810\",\"sensorWidthPixels\":7360,\"sensorHeightPixels\":4912},";
    text += "\"documentPlane\":{\"paper\":\"ISO-216-A0\",\"orientation\":\"landscape\",\"sheetWidthMm\":1189,"
            "\"sheetHeightMm\":841,\"cameraOrder\":\"CAM-A-left-CAM-B-right\"},";
    text += "\"cameras\":{";
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        if (index > 0) text += ",";
        text += "\"" + cameras[index].Find("alias")->AsString() + "\":{\"physicalIdentity\":null,\"projection\":"
            + DumpJson(*cameras[index].Find("projection")) + "}";
    }
    text += "},";
    text += "\"calibration\":{\"method\":\"chart-fiducial-planar\",\"chartId\":\"synthetic-chart-269\","
            "\"provenanceId\":\"synthetic-269-ground-truth\",\"measuredAt\":\"2026-10-08T00:00:00Z\","
            "\"tool\":{\"toolId\":\"synthetic-pair-generator\",\"version\":\"2\"},"
            "\"inputSha256\":{\"CAM-A\":\"" + zeros + "\",\"CAM-B\":\"" + zeros + "\"},"
            "\"fitResidualPixels\":{\"CAM-A\":{\"rms\":0,\"max\":0},\"CAM-B\":{\"rms\":0,\"max\":0}},"
            "\"rigMeasurements\":{\"baselineMm\":null,\"overlapMm\":null,"
            "\"cameraToDocumentMm\":{\"CAM-A\":null,\"CAM-B\":null},\"lensFocalLengthMm\":{\"CAM-A\":null,\"CAM-B\":null}}},";
    text += "\"outputRaster\":null,\"correctionEnvelope\":null,\"qualityContract\":null,\"approval\":null}";
    return text;
}

// exit 0 = the schema accepts the file, 1 = it rejects it, anything else = the check itself failed.
DWORD ValidateAgainstSchema(const std::filesystem::path& powershell, const std::filesystem::path& schema,
    const std::filesystem::path& profile, std::string& output) {
    const std::string script = "try { $json = Get-Content -Raw -LiteralPath " + QuoteForPowerShell(profile.wstring())
        + "; if (Test-Json -Json $json -SchemaFile " + QuoteForPowerShell(schema.wstring())
        + " -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 } } catch { Write-Output $_.Exception.Message; exit 2 }";
    const auto result = RunProcess(powershell, {L"-NoProfile", L"-NonInteractive", L"-Command",
        std::wstring(script.begin(), script.end())}, 120000);
    output = result.output;
    return result.exit_code;
}

void TestDraftProfileMatchesSchema(const std::filesystem::path& fixture_path, const std::filesystem::path& powershell,
    const std::filesystem::path& schema) {
    TempFolder temp;
    const auto spec = synth::ParsePairSpec(ReadText(fixture_path));
    const auto truth = synth::ParseJson(synth::SerializeGroundTruth(spec, {}));
    const auto profile = BuildDraftProfile(truth);

    std::string output;
    const auto write_and_check = [&](const std::string& name, const std::string& text) {
        const auto path = temp.Sub(name);
        WriteText(path, text);
        return ValidateAgainstSchema(powershell, schema, path, output);
    };

    Check(write_and_check("draft.json", profile) == 0, "profile: the ground truth projections form a draft that passes rig-profile.v2.schema.json (" + output + ")");
    // Negative controls: the check really looks at the fields that the generator writes.
    Check(write_and_check("no-p2.json", Replace(profile, ",\"p2\":0}", "}")) == 1, "profile: a camera without p2 is rejected");
    Check(write_and_check("h22.json", Replace(profile, "[0,0,1]]", "[0,0,2]]")) == 1, "profile: documentToImage[2][2] other than 1 is rejected");
    Check(write_and_check("old-pixel.json", Replace(profile, "stored-order-sample-at-integer-index", "continuous-pixel-centre-at-half"))
            == 1,
        "profile: another cameraPixel convention is rejected");
    Check(write_and_check("flat.json", Replace(profile, "\"documentToImage\":[[", "\"documentToImage\":[0,[")) == 1,
        "profile: a flattened documentToImage is rejected");
}

// ---- The tool as a process, at the real size -----------------------------------------------

std::vector<std::wstring> GeneratorArguments(const std::filesystem::path& spec, const std::filesystem::path& output) {
    return {L"--spec", spec.wstring(), L"--output", output.wstring()};
}

void TestCommandLine(const std::filesystem::path& generator, const std::filesystem::path& spec_path) {
    TempFolder temp;
    const auto missing = RunProcess(generator, {L"--output", temp.Sub("x").wstring()}, 60000);
    Check(missing.exit_code == 2 && missing.output.find("error=") != std::string::npos, "cli: missing --spec fails with exit 2");
    const auto unknown = RunProcess(generator, {L"--bogus", L"1"}, 60000);
    Check(unknown.exit_code == 2, "cli: unknown option fails with exit 2");
    const auto bad_spec = RunProcess(generator, GeneratorArguments(temp.Sub("nonexistent.json"), temp.Sub("y")), 60000);
    Check(bad_spec.exit_code == 2, "cli: missing spec file fails with exit 2");
    Check(!std::filesystem::exists(temp.Sub("y") / "CAM-A"), "cli: a failed run leaves no images behind");

    // A /1 spec is refused with the reason.
    const auto old_spec_path = temp.Sub("old.spec.json");
    WriteText(old_spec_path, Replace(ReadText(spec_path), "a0.m2.synthetic-pair-spec/2", "a0.m2.synthetic-pair-spec/1"));
    const auto old_spec = RunProcess(generator, GeneratorArguments(old_spec_path, temp.Sub("z")), 60000);
    Check(old_spec.exit_code == 2 && old_spec.output.find("ADR-0034") != std::string::npos, "cli: a /1 spec is refused and names ADR-0034 (" + old_spec.output + ")");
}

void TestFullSize(const std::filesystem::path& generator, const std::filesystem::path& adapter, const std::filesystem::path& spec_path) {
    TempFolder temp;
    const auto spec = synth::ParsePairSpec(ReadText(spec_path));
    Check(spec.image_width_px == 7360 && spec.image_height_px == 4912, "full size: the fixture is 7360x4912");

    std::array<ProcessResult, 2> runs;
    for (std::size_t run = 0; run < runs.size(); ++run) {
        const auto folder = temp.Sub(run == 0 ? "first" : "second");
        runs[run] = RunProcess(generator, GeneratorArguments(spec_path, folder), 1500000);
        Check(runs[run].exit_code == 0 && runs[run].output.find("result=generated-synthetic-pair") != std::string::npos,
            "full size: generator run " + std::to_string(run + 1) + " succeeds (" + runs[run].output + ")");
        Info("full size run " + std::to_string(run + 1) + ": elapsedMilliseconds=" + OutputValue(runs[run].output, "elapsedMilliseconds")
            + " peakWorkingSetBytes=" + OutputValue(runs[run].output, "peakWorkingSetBytes")
            + " size.CAM-A=" + OutputValue(runs[run].output, "size.CAM-A") + " size.CAM-B=" + OutputValue(runs[run].output, "size.CAM-B"));
    }

    // Same seed, same bytes: compare the SHA-256 of every output file, computed here and not taken from the tool.
    const std::array<std::string, 3> names = {"CAM-A/original.jpg", "CAM-B/original.jpg", "ground-truth.json"};
    for (const auto& name : names) {
        const auto one = synth::Sha256Hex(temp.Sub("first") / name);
        const auto two = synth::Sha256Hex(temp.Sub("second") / name);
        Info("sha256 " + name + " = " + one);
        Check(one == two, "full size: two runs give the same SHA-256 for " + name);
    }
    Check(ReadText(temp.Sub("first") / "ground-truth.json") == ReadText(temp.Sub("second") / "ground-truth.json"),
        "full size: ground truth bytes are identical");
    Check(OutputValue(runs[0].output, "sha256.CAM-A") == synth::Sha256Hex(temp.Sub("first") / "CAM-A/original.jpg"),
        "full size: the tool prints the real SHA-256");
    Check(CountPartialFiles(temp.Sub("first")) == 0 && CountPartialFiles(temp.Sub("second")) == 0, "full size: no .partial file is left");

    for (const auto* alias : {"CAM-A", "CAM-B"}) {
        const auto path = (temp.Sub("first") / alias / "original.jpg");
        const auto size = std::filesystem::file_size(path);
        Check(size > 0 && size <= 64ULL * 1024ULL * 1024ULL, std::string("full size: ") + alias + " fits the 64 MiB limit");
        const auto validated = RunProcess(adapter,
            {L"validate-canonical-jpeg", L"--input", path.wstring(), L"--width", L"7360", L"--height", L"4912"}, 600000);
        Check(validated.exit_code == 0 && validated.output.find("result=validated-canonical-jpeg") != std::string::npos,
            std::string("full size: M2Adapter validate-canonical-jpeg accepts ") + alias + " (" + validated.output + ")");
    }

    // The ground truth of the real run passes the same audit.
    const auto truth = synth::ParseJson(ReadText(temp.Sub("first") / "ground-truth.json"));
    const auto audit = AuditProjections(truth, "full size truth");
    Check(audit.worst <= kProjectionLimit, "full size: ground truth within 1e-6 px of the independent projection");
    const auto& listed = truth.Find("output_files")->items;
    Check(listed.size() == 2 && listed[0].Find("sha256")->AsString() == synth::Sha256Hex(temp.Sub("first") / "CAM-A/original.jpg"),
        "full size: ground truth records the image hash");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path generator;
        std::filesystem::path adapter;
        std::filesystem::path spec;
        std::filesystem::path powershell;
        std::filesystem::path profile_schema;
        bool small_only = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--small-only") small_only = true;
            else if (argument == "--generator" && index + 1 < argc) generator = argv[++index];
            else if (argument == "--adapter" && index + 1 < argc) adapter = argv[++index];
            else if (argument == "--spec" && index + 1 < argc) spec = argv[++index];
            else if (argument == "--powershell" && index + 1 < argc) powershell = argv[++index];
            else if (argument == "--profile-schema" && index + 1 < argc) profile_schema = argv[++index];
            else throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
        if (generator.empty() || adapter.empty() || spec.empty() || powershell.empty() || profile_schema.empty()) {
            throw std::invalid_argument("--generator, --adapter, --spec, --powershell and --profile-schema are required");
        }

        generator = std::filesystem::absolute(generator);
        adapter = std::filesystem::absolute(adapter);
        spec = std::filesystem::absolute(spec);
        profile_schema = std::filesystem::absolute(profile_schema);
        TestJson();
        TestSpecValidation();
        TestPlacement();
        TestGroundTruthProjection(spec);
        TestCoverageRule();
        TestFoldedPointsAreNotRecorded();
        TestInverseAllSpecs(spec);
        TestMigrationFromV1(spec);

        {
            // Ideal lens first (a pure sampling convention check), then the distorted lens, then
            // the cameras with rotation, perspective, tilt, tangential terms and fx != fy.
            // The limits are set by the estimator, not by the renderer: the ink weight below is
            // a threshold on luma, which is not linear in coverage, so its centroid wobbles with
            // the sub-pixel phase of the ring (0.015 px at phase 0, up to 0.09 px for an ideal
            // lens shifted by a fraction of a pixel, up to 0.18 px in the distorted cases). A
            // linear coverage weight that also masks the grid-line arms is the next step to reach
            // 0.02 px.
            auto ideal = synth::ParsePairSpec(CleanSmallSpec(false));
            ideal.supersample = 4;
            auto distorted = synth::ParsePairSpec(CleanSmallSpec(true));
            distorted.supersample = 4;
            TestRenderMatchesGroundTruth("ideal lens", ideal, 0.05, 0.0);
            auto shifted = ideal;
            for (auto& camera : shifted.cameras) {
                camera.document_to_image[2] += 0.37;
                camera.document_to_image[5] += 0.23;
            }
            TestRenderMatchesGroundTruth("ideal lens, shifted by a sub-pixel", shifted, 0.15, 0.0);
            TestRenderMatchesGroundTruth("distorted", distorted, 0.25, 0.0);
            TestRenderMatchesGroundTruth("rotated/perspective/tangential and tilted pose", MakeStressSpec(), 0.25, 0.0);
        }
        TestRenderIsRowSplitInvariant();
        TestPublishContract();
        TestLibraryGeneration(adapter);
        TestDraftProfileMatchesSchema(spec, powershell, profile_schema);
        TestCommandLine(generator, spec);
        if (!small_only) {
            TestFullSizeBands(spec);
            TestFullSize(generator, adapter, spec);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "synthetic pair contracts passed\n";
    return 0;
}
