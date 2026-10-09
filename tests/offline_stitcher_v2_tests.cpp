#include "a0/m2/offline_stitcher.hpp"
#include "a0/m2/rig_profile_v2.hpp"
#include "a0/m2/stitch_job_manifest.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace a0::m2::detail {
enum class OfflineStitchFaultPoint : std::uint32_t {
    none = 0, encode_failure = 1, partial_short_write = 2, partial_flush_failure = 3,
    disk_full = 4, publish_failure = 5, interrupt_before_encode = 6,
    interrupt_after_partial_write = 7, interrupt_after_flush = 8,
    interrupt_before_publish = 9, interrupt_after_publish = 10,
};
void SetInputLocksHeldTestHook(void (*)()) noexcept;
void SetBeforeEncodeTestHook(void (*)()) noexcept;
void SetBeforePartialFlushTestHook(void (*)()) noexcept;
void SetBeforePublishRenameTestHook(void (*)()) noexcept;
void SetOfflineStitchFaultForTest(OfflineStitchFaultPoint) noexcept;
std::uint32_t GetOfflineStitchFaultTriggerCountForTest() noexcept;
}

namespace {
namespace fs = std::filesystem;
using namespace a0::m2;
int checks = 0, failures = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
void Hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("synthetic WIC operation failed"); }
template<class T> struct Release { void operator()(T* value) const { if (value) value->Release(); } };
template<class T> using Com = std::unique_ptr<T, Release<T>>;
std::vector<std::uint8_t> Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string Text(const fs::path& path) { const auto b = Bytes(path); return {b.begin(), b.end()}; }
void Write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("test synthetic write failed");
}
void WriteText(const fs::path& path, const std::string& text) { Write(path, {text.begin(), text.end()}); }
struct Temp final {
    fs::path root;
    Temp() {
        root = fs::temp_directory_path() / ("a0-native-v2-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
        if (!fs::create_directory(root)) throw std::runtime_error("test root exclusive creation failed");
    }
    ~Temp() { std::error_code ignored; fs::remove_all(root, ignored); }
};

void SyntheticJpeg(const fs::path& path, std::uint32_t width, std::uint32_t height, unsigned camera) {
    fs::create_directories(path.parent_path());
    IWICImagingFactory* factory_raw{};
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_raw)));
    Com<IWICImagingFactory> factory(factory_raw);
    IWICStream* stream_raw{}; Hr(factory->CreateStream(&stream_raw)); Com<IWICStream> stream(stream_raw);
    Hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    IWICBitmapEncoder* encoder_raw{}; Hr(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder_raw));
    Com<IWICBitmapEncoder> encoder(encoder_raw); Hr(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* frame_raw{}; IPropertyBag2* bag_raw{};
    Hr(encoder->CreateNewFrame(&frame_raw, &bag_raw)); Com<IWICBitmapFrameEncode> frame(frame_raw); Com<IPropertyBag2> bag(bag_raw);
    Hr(frame->Initialize(bag.get())); Hr(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR; Hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("synthetic JPEG format rejected");
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3U);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            // Repeatable nonconstant pixels exercise fractional interpolation;
            // these are synthetic camera inputs, never photographs or IDs.
            row[x * 3U] = static_cast<std::uint8_t>(30 + ((x / 3 + y / 7 + camera * 19) % 180));
            row[x * 3U + 1] = static_cast<std::uint8_t>(40 + ((x / 5 + y / 3 + camera * 37) % 170));
            row[x * 3U + 2] = static_cast<std::uint8_t>(50 + ((x / 7 + y / 5 + camera * 53) % 160));
        }
        Hr(frame->WritePixels(1, width * 3U, static_cast<UINT>(row.size()), row.data()));
    }
    Hr(frame->Commit()); Hr(encoder->Commit());
}

OfflineStitchV2Request Request(const Temp& temp, const fs::path& profile, const std::string& fingerprint,
    const char* name, render::Resampling kernel = render::Resampling::bilinear) {
    return {temp.root / "a" / "original.jpg", temp.root / "b" / "original.jpg", temp.root / name,
        profile, "2026-10-09T00:00:00Z", fingerprint, kernel,
        "00000000000000000000000000000001", "00000000000000000000000000000002", "2026-10-09T00:00:01Z"};
}
template<class F> void RejectBeforeOutput(OfflineStitchV2Request request, F change, const char* label) {
    change(request);
    bool rejected = false;
    try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
    Check(rejected, label); Check(!fs::exists(request.output_job_directory), "rejection creates no output job");
}

// Independent JPEG header walk; neither WIC resolution nor production metadata
// helpers are used to assert the bytes recorded in the published output.
void CheckDpi(const fs::path& path, unsigned dpi) {
    const auto bytes = Bytes(path); unsigned records = 0; bool density = false, no_exif = true;
    std::size_t p = 2;
    while (p + 4 <= bytes.size() && bytes[p] == 255) {
        const auto marker = bytes[p + 1]; if (marker == 218 || marker == 217) break;
        const auto length = static_cast<std::size_t>(bytes[p + 2]) * 256 + bytes[p + 3];
        if (length < 2 || length > bytes.size() - p - 2) break;
        if (marker == 224 && length >= 16 && std::equal(bytes.begin() + p + 4, bytes.begin() + p + 9, "JFIF\0")) {
            ++records; density = bytes[p + 11] == 1 && bytes[p + 12] * 256U + bytes[p + 13] == dpi
                && bytes[p + 14] * 256U + bytes[p + 15] == dpi;
        }
        if (marker == 225 && length >= 8 && std::equal(bytes.begin() + p + 4, bytes.begin() + p + 10, "Exif\0\0")) no_exif = false;
        p += length + 2;
    }
    Check(records == 1 && density, "published JFIF encodes exact two-axis inches density");
    Check(no_exif, "published JPEG has no EXIF orientation");
}

std::array<fs::path, 3> lock_paths;
void VerifyImmutableLocks() {
    for (const auto& path : lock_paths) {
        for (const DWORD access : {GENERIC_WRITE, DELETE}) {
            const auto handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
                "input and profile write/delete excluded by held handles");
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    }
}
fs::path hook_path, hook_destination, hook_manifest, hook_owned_away;
std::vector<std::uint8_t> replacement_jpeg;
const std::vector<std::uint8_t> sentinel{'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'};
void PreexistingPartial() { Write(hook_path, sentinel); }
void CompetingDestination() { Write(hook_destination, sentinel); }
void CompetingManifest() { Write(hook_manifest, sentinel); }
void ReplaceRegularCandidate() {
    if (!MoveFileExW(hook_path.c_str(), hook_owned_away.c_str(), 0))
        throw std::runtime_error("regular replacement test could not retain owned candidate");
    Write(hook_path, replacement_jpeg);
}
void CorruptDpi() {
    auto bytes = Bytes(hook_path);
    if (bytes.size() < 20 || bytes[2] != 255 || bytes[3] != 224) throw std::runtime_error("test JFIF not first");
    bytes[13] = 0; // units byte of canonical JFIF when preceded by SOI
    Write(hook_path, bytes);
}
void ChangeValidJfifOnSameFile() {
    auto bytes = Bytes(hook_path);
    if (bytes.size() < 20 || bytes[2] != 255 || bytes[3] != 224 || bytes[12] != 2)
        throw std::runtime_error("test canonical JFIF minor version unavailable");
    // SOI + APP0 puts the minor version at byte 12. JFIF 1.01 is valid,
    // changes neither raster nor density, and truncating this existing file
    // preserves its identity while changing the encoder-origin byte string.
    bytes[12] = 1;
    Write(hook_path, bytes);
}
void ClearHooks() {
    detail::SetInputLocksHeldTestHook(nullptr); detail::SetBeforeEncodeTestHook(nullptr);
    detail::SetBeforePartialFlushTestHook(nullptr); detail::SetBeforePublishRenameTestHook(nullptr);
    detail::SetOfflineStitchFaultForTest(detail::OfflineStitchFaultPoint::none);
}

std::wstring Wide(const std::string& value) { return {value.begin(), value.end()}; }
std::wstring Quote(const std::wstring& value) {
    std::wstring quoted = L"\""; unsigned slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') quoted.append(slashes * 2 + 1, L'\\'); else quoted.append(slashes, L'\\');
        slashes = 0; quoted += c;
    }
    quoted.append(slashes * 2, L'\\'); return quoted + L"\"";
}
std::vector<std::wstring> Arguments(const OfflineStitchV2Request& r) {
    return {L"stitch-v2", L"--camera-a", r.camera_a_original.native(), L"--camera-b", r.camera_b_original.native(),
        L"--job-directory", r.output_job_directory.native(), L"--profile-file", r.profile_path.native(),
        L"--expected-profile-sha256", Wide(r.expected_profile_sha256), L"--assessed-at", Wide(r.assessed_at_utc),
        L"--resampling", r.resampling == render::Resampling::bilinear ? L"bilinear" : L"bicubic-catmull-rom",
        L"--stitch-job-id", Wide(r.stitch_job_id), L"--capture-transaction-id", Wide(r.capture_transaction_id),
        L"--completed-at", Wide(r.completed_at_utc)};
}
struct CliResult { DWORD exit; std::string output; };
CliResult Cli(const fs::path& executable, const fs::path& log, const std::vector<std::wstring>& args) {
    std::wstring command = Quote(executable.native()); for (const auto& arg : args) command += L" " + Quote(arg);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("test CLI output reservation failed");
    STARTUPINFOW startup{sizeof(STARTUPINFOW)}; startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = output; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(output);
    if (!started) throw std::runtime_error("test adapter launch failed");
    if (WaitForSingleObject(process.hProcess, 60'000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 199); WaitForSingleObject(process.hProcess, 5'000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); throw std::runtime_error("test adapter timeout");
    }
    DWORD code{}; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return {code, Text(log)};
}
}

int wmain(int argc, wchar_t* argv[]) {
    const bool replacement_only = argc == 4 && std::wstring_view(argv[3]) == L"--regular-replacement-only";
    if (argc != 3 && !replacement_only) { std::cerr << "fixture directory and adapter executable required\n"; return 2; }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 2;
    try {
        const fs::path fixtures(argv[1]), adapter(argv[2]); Temp temp;
        const auto profile_path = temp.root / "approved.json";
        const auto profile_text = Text(fixtures / "approved-product-crop.json"); WriteText(profile_path, profile_text);
        const auto profile = RigProfileV2::Parse(profile_text);
        const std::string known_hash = "df6d0d8dc23005e28dc14fdcfeadc997fc37e54d910567c145f52ed4863a63c1";
        Check(profile.FingerprintSha256() == known_hash, "shared independently generated approved crop fingerprint");
        SyntheticJpeg(temp.root / "a/original.jpg", 7360, 4912, 0);
        SyntheticJpeg(temp.root / "b/original.jpg", 7360, 4912, 1);
        const auto a_before = ComputeFileSha256Hex(temp.root / "a/original.jpg");
        const auto b_before = ComputeFileSha256Hex(temp.root / "b/original.jpg");
        const auto profile_before = Bytes(profile_path);
        if (replacement_only) {
            const auto reference = StitchCanonicalPairV2(Request(temp, profile_path, known_hash, "reference"));
            replacement_jpeg = Bytes(reference.stitched_jpeg);
            const auto request = Request(temp, profile_path, known_hash, "replacement");
            hook_path = request.output_job_directory / "stitched.jpg.partial";
            hook_owned_away = request.output_job_directory / "owned-encoded.jpg";
            detail::SetBeforePartialFlushTestHook(ReplaceRegularCandidate);
            bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && !fs::exists(request.output_job_directory / "stitched.jpg")
                && !fs::exists(request.output_job_directory / "stitch-job.manifest.json"),
                "regular valid same-DPI replacement refused before terminal publication");
            Check(fs::exists(hook_path) && Bytes(hook_path) == replacement_jpeg && fs::exists(hook_owned_away),
                "replacement and original encoded candidate both retained as evidence");
            ClearHooks(); CoUninitialize();
            std::cout << "regular replacement checks=" << checks << " failures=" << failures << '\n';
            return failures ? 1 : 0;
        }
        lock_paths = {temp.root / "a/original.jpg", temp.root / "b/original.jpg", profile_path};
        detail::SetInputLocksHeldTestHook(VerifyImmutableLocks);
        detail::SetBeforeEncodeTestHook(VerifyImmutableLocks);
        detail::SetBeforePublishRenameTestHook(VerifyImmutableLocks);
        for (const auto kernel : {render::Resampling::bilinear, render::Resampling::bicubic_catmull_rom}) {
            const bool linear = kernel == render::Resampling::bilinear;
            const auto first_request = Request(temp, profile_path, known_hash, linear ? "linear1" : "cubic1", kernel);
            auto second_request = Request(temp, profile_path, known_hash, linear ? "linear2" : "cubic2", kernel);
            if (linear) {
                // Windows accepts ordinary slash and mixed-separator paths;
                // .NET/native entries must agree without admitting namespaces.
                second_request.camera_a_original = second_request.camera_a_original.generic_wstring();
                second_request.camera_b_original = temp.root / "b/original.jpg";
                second_request.profile_path = second_request.profile_path.generic_wstring();
                second_request.output_job_directory = second_request.output_job_directory.generic_wstring();
            }
            const auto first = StitchCanonicalPairV2(first_request), second = StitchCanonicalPairV2(second_request);
            Check(first.width == 4 && first.height == 4, "approved metric crop produces explicit 4x4 raster");
            Check(Bytes(first.stitched_jpeg) == Bytes(second.stitched_jpeg), "repeated kernel output bytes are identical");
            if (linear) Check(second.width == 4 && second.height == 4, "ordinary slash and mixed-separator paths accepted");
            const auto manifest = VerifyPublishedStitchJob(first_request.output_job_directory, first_request.stitch_job_id);
            Check(manifest.rig_profile.sha256 == known_hash && manifest.rig_profile.version == "2.0.0", "manifest records actual native applied profile fingerprint");
            Check(manifest.engine.engine_id == kOfflineStitcherEngineId
                && manifest.engine.version == (linear ? kOfflineStitcherV2BilinearVersion : kOfflineStitcherV2BicubicVersion), "explicit kernel has distinct engine version");
            Check(manifest.inputs[0].sha256 == a_before && manifest.inputs[1].sha256 == b_before, "manifest records exact locked inputs");
            Check(manifest.output.sha256 == ComputeFileSha256Hex(first.stitched_jpeg)
                && manifest.output.encoded_size_bytes == fs::file_size(first.stitched_jpeg), "manifest hash and size describe published bytes");
            Check(manifest.seam_navigation && manifest.seam_navigation->available
                && manifest.seam_navigation->x_pixels < 4 && manifest.seam_navigation->y_pixels < 4, "seam navigation names actual overlap inside output");
            CheckDpi(first.stitched_jpeg, 100);
        }
        ClearHooks();
        Check(a_before == ComputeFileSha256Hex(temp.root / "a/original.jpg")
            && b_before == ComputeFileSha256Hex(temp.root / "b/original.jpg") && profile_before == Bytes(profile_path), "inputs and approved profile remain byte immutable");

        unsigned ordinal = 0;
        const auto fresh = [&] { const auto name = "reject" + std::to_string(++ordinal); return Request(temp, profile_path, known_hash, name.c_str()); };
        RejectBeforeOutput(fresh(), [](auto& r) { r.expected_profile_sha256.assign(64, 'a'); }, "expected fingerprint mismatch refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.expected_profile_sha256.assign(64, 'A'); }, "noncanonical expected fingerprint refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.stitch_job_id = "bad"; }, "bad job identity refused before output");
        RejectBeforeOutput(fresh(), [](auto& r) { r.stitch_job_id.assign(32, '0'); }, "empty GUID job refused before output");
        RejectBeforeOutput(fresh(), [](auto& r) { r.capture_transaction_id.assign(32, '0'); }, "empty GUID capture refused before output");
        RejectBeforeOutput(fresh(), [](auto& r) { r.capture_transaction_id.assign(32, 'A'); }, "bad capture identity refused before output");
        RejectBeforeOutput(fresh(), [](auto& r) { r.completed_at_utc = "2026-02-30T00:00:00Z"; }, "invalid Gregorian completion refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.completed_at_utc = "0000-01-01T00:00:00Z"; }, "year zero completion refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.completed_at_utc = "2026-10-09T00:00:60Z"; }, "leap second completion refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.completed_at_utc = "2026-10-08T00:00:00Z"; }, "completion before assessment refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.assessed_at_utc = "2100-01-01T00:00:00Z"; r.completed_at_utc = r.assessed_at_utc; }, "approval expiry boundary refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.assessed_at_utc = "2026-10-09T00:00:00.0Z"; }, "fractional assessment refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.resampling = static_cast<render::Resampling>(99); }, "unknown native kernel refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path = L"\\\\server\\share\\approved.json"; }, "UNC profile refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path += L":hidden"; }, "profile ADS refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path = L"\\\\?\\C:\\approved.json"; }, "device namespace profile refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path = r.profile_path.parent_path() / L"CON.json"; }, "DOS device component refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path = r.profile_path.parent_path() / L"CONIN$"; }, "console device component refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.profile_path = r.profile_path.parent_path() / L"COM\u00b9.json"; }, "superscript COM device component refused");
        {
            auto request = fresh(); const auto missing_parent = temp.root / "missing-parent";
            request.output_job_directory = missing_parent / "job";
            RejectBeforeOutput(request, [](auto&) {}, "missing v2 output parent refused");
            Check(!fs::exists(missing_parent), "v2 never creates a missing parent tree");
        }
        RejectBeforeOutput(fresh(), [](auto& r) { r.output_job_directory += L"."; }, "ambiguous trailing-dot job refused");
        RejectBeforeOutput(fresh(), [](auto& r) { r.camera_a_original += L":hidden"; }, "input ADS refused");
        const auto draft_path = temp.root / "draft.json"; WriteText(draft_path, Text(fixtures / "calibrated-draft.json"));
        RejectBeforeOutput(fresh(), [&](auto& r) { r.profile_path = draft_path; }, "calibrated DRAFT cannot enter product route");
        const auto template_path = temp.root / "template.json"; WriteText(template_path, Text(fixtures / "template.json"));
        RejectBeforeOutput(fresh(), [&](auto& r) { r.profile_path = template_path; }, "template cannot enter product route");
        const auto v1_path = temp.root / "v1.json"; WriteText(v1_path, "{\"schemaVersion\":\"1.0.0\"}");
        RejectBeforeOutput(fresh(), [&](auto& r) { r.profile_path = v1_path; }, "v1 file refused by v2 reader");
        const auto huge_path = temp.root / "large.json"; WriteText(huge_path, std::string(256U * 1024U + 1, ' '));
        RejectBeforeOutput(fresh(), [&](auto& r) { r.profile_path = huge_path; }, "profile compressed source bound enforced");
        SyntheticJpeg(temp.root / "small/original.jpg", 8, 8, 0);
        RejectBeforeOutput(fresh(), [&](auto& r) { r.camera_a_original = temp.root / "small/original.jpg"; }, "wrong canonical input dimensions refused");
        fs::create_directory(temp.root / "hardlink");
        if (!CreateHardLinkW((temp.root / "hardlink/original.jpg").c_str(), (temp.root / "a/original.jpg").c_str(), nullptr))
            throw std::runtime_error("test input identity hardlink creation failed");
        RejectBeforeOutput(fresh(), [&](auto& r) { r.camera_b_original = temp.root / "hardlink/original.jpg"; }, "same physical canonical input refused");
        const auto linked = temp.root / "profile-link.json";
        if (CreateSymbolicLinkW(linked.c_str(), profile_path.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
            RejectBeforeOutput(fresh(), [&](auto& r) { r.profile_path = linked; }, "reparse profile refused");
        else std::cout << "SKIP optional symlink privilege unavailable\n";

        for (const auto point : {detail::OfflineStitchFaultPoint::encode_failure, detail::OfflineStitchFaultPoint::partial_short_write,
            detail::OfflineStitchFaultPoint::partial_flush_failure, detail::OfflineStitchFaultPoint::disk_full,
            detail::OfflineStitchFaultPoint::publish_failure}) {
            auto request = fresh(); detail::SetOfflineStitchFaultForTest(point); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && detail::GetOfflineStitchFaultTriggerCountForTest() == 1, "shared v2 failure boundary triggers exactly once");
            Check(fs::is_directory(request.output_job_directory), "failed boundary preserves reserved job");
            Check(!fs::exists(request.output_job_directory / "stitch-job.manifest.json")
                && !fs::exists(request.output_job_directory / "stitched.jpg"), "failed JPEG boundary cannot report terminal success");
            if (point != detail::OfflineStitchFaultPoint::encode_failure)
                Check(fs::exists(request.output_job_directory / "stitched.jpg.partial"), "failed JPEG boundary preserves partial evidence");
            ClearHooks();
        }
        {
            auto request = fresh(); hook_path = request.output_job_directory / "stitched.jpg.partial";
            detail::SetBeforeEncodeTestHook(PreexistingPartial); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && Bytes(hook_path) == sentinel, "v2 CREATE_NEW preserves pre-existing partial bytes"); ClearHooks();
        }
        {
            auto request = fresh(); hook_path = request.output_job_directory / "stitched.jpg.partial";
            detail::SetBeforePartialFlushTestHook(CorruptDpi); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && fs::exists(hook_path) && !fs::exists(request.output_job_directory / "stitched.jpg"), "actual corrupted DPI refused before publish and retained"); ClearHooks();
        }
        {
            auto request = fresh(); hook_path = request.output_job_directory / "stitched.jpg.partial";
            detail::SetBeforePartialFlushTestHook(ChangeValidJfifOnSameFile); bool rejected = false;
            std::string error;
            try { (void)StitchCanonicalPairV2(request); }
            catch (const std::exception& exception) { rejected = true; error = exception.what(); }
            Check(rejected && error.find("candidate bytes changed after encoding") != std::string::npos,
                "same-file valid-JFIF mutation rejected by encoder-origin SHA guard");
            const auto retained = fs::exists(hook_path) ? Bytes(hook_path) : std::vector<std::uint8_t>{};
            Check(retained.size() > 12 && retained[12] == 1
                && !fs::exists(request.output_job_directory / "stitched.jpg")
                && !fs::exists(request.output_job_directory / "stitch-job.manifest.json"),
                "same-file mutated partial retained without output or terminal manifest");
            CheckDpi(hook_path, 100); // independent check: valid density and no EXIF
            ClearHooks();
        }
        {
            auto request = fresh(); hook_path = request.output_job_directory / "stitched.jpg.partial";
            hook_owned_away = request.output_job_directory / "owned-encoded.jpg";
            replacement_jpeg = Bytes(temp.root / "linear1/stitched.jpg");
            detail::SetBeforePartialFlushTestHook(ReplaceRegularCandidate); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && !fs::exists(request.output_job_directory / "stitched.jpg")
                && !fs::exists(request.output_job_directory / "stitch-job.manifest.json"),
                "regular valid same-DPI replacement refused before terminal publication");
            Check(fs::exists(hook_path) && Bytes(hook_path) == replacement_jpeg && fs::exists(hook_owned_away),
                "regular replacement preserves both competing and encoded candidate evidence"); ClearHooks();
        }
        {
            auto request = fresh(); hook_destination = request.output_job_directory / "stitched.jpg";
            detail::SetBeforePublishRenameTestHook(CompetingDestination); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && Bytes(hook_destination) == sentinel && fs::exists(request.output_job_directory / "stitched.jpg.partial"), "shared non-replacing JPEG fence preserves competing destination"); ClearHooks();
        }
        {
            auto request = fresh(); hook_manifest = request.output_job_directory / "stitch-job.manifest.json";
            detail::SetBeforePublishRenameTestHook(CompetingManifest); bool rejected = false;
            try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && Bytes(hook_manifest) == sentinel && fs::exists(request.output_job_directory / "stitched.jpg"), "manifest failure after JPEG publish preserves candidate and denies success"); ClearHooks();
        }
        {
            auto request = fresh(); fs::create_directory(request.output_job_directory); Write(request.output_job_directory / "sentinel", sentinel);
            bool rejected = false; try { (void)StitchCanonicalPairV2(request); } catch (const std::exception&) { rejected = true; }
            Check(rejected && Bytes(request.output_job_directory / "sentinel") == sentinel, "pre-reserved job remains untouched");
        }

        const auto cli_request = Request(temp, profile_path, known_hash, "cli");
        const auto cli = Cli(adapter, temp.root / "cli.log", Arguments(cli_request));
        Check(cli.exit == 0, "strict v2 adapter executes approved route");
        const std::string expected = "result=stitched\nwidth=4\nheight=4\nprofileId=synthetic-approved-product-crop\nstitchJobId="
            + cli_request.stitch_job_id + "\nmanifest=stitch-job.manifest.json\n";
        auto normalized = cli.output; normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
        Check(normalized == expected, "adapter retains exactly six existing success lines");
        Check(VerifyPublishedStitchJob(cli_request.output_job_directory, cli_request.stitch_job_id).rig_profile.sha256 == known_hash, "adapter manifest applies own native fingerprint");
        for (unsigned kind = 0; kind < 5; ++kind) {
            auto request = fresh(); auto args = Arguments(request);
            if (kind == 0) { args.push_back(L"--unknown"); args.push_back(L"x"); }
            if (kind == 1) { args.push_back(L"--resampling"); args.push_back(L"bilinear"); }
            if (kind == 2) { args.erase(args.begin() + 13, args.begin() + 15); }
            if (kind == 3) args[14] = L"nearest";
            if (kind == 4) args[10] = std::wstring(64, L'a');
            const auto result = Cli(adapter, temp.root / ("cli-reject" + std::to_string(kind) + ".log"), args);
            Check(result.exit == 2 && result.output.starts_with("error="), "adapter fails invalid strict contract without success");
            Check(!fs::exists(request.output_job_directory), "invalid adapter contract creates no job");
        }
        Check(a_before == ComputeFileSha256Hex(temp.root / "a/original.jpg")
            && b_before == ComputeFileSha256Hex(temp.root / "b/original.jpg") && profile_before == Bytes(profile_path), "all native/CLI/failure tests preserve original/profile bytes");
        ClearHooks(); CoUninitialize();
        std::cout << "native approved-v2 integration checks=" << checks << " failures=" << failures << '\n';
        return failures ? 1 : 0;
    } catch (const std::exception& exception) {
        ClearHooks(); CoUninitialize(); std::cerr << "FAIL exception=" << exception.what() << '\n'; return 1;
    }
}
