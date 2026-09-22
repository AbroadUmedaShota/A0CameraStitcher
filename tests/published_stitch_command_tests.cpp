#define wmain AdapterEntry
#include "../src/m2/offline_stitcher_adapter_main.cpp"
#undef wmain
#include <sstream>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (L"A0PublishedReview-" + std::to_wstring(GetCurrentProcessId()));
    try {
        if (!fs::create_directory(root)) return 2;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
        GenerateSyntheticJpeg(root / L"original.jpg", 16, 8, {20, 40, 60});
        fs::rename(root / L"original.jpg", root / L"stitched.jpg"); // Exact synthetic fixture, before observation.
        CoUninitialize();
        a0::m2::StitchJobManifest manifest;
        manifest.stitch_job_id = "11111111111111111111111111111111";
        manifest.capture_transaction_id = "22222222222222222222222222222222";
        manifest.inputs = {{{"CAM-A", std::string(64, 'a'), 1024}, {"CAM-B", std::string(64, 'b'), 2048}}};
        manifest.rig_profile = {"synthetic-review", "1.0.0", std::string(64, 'c')};
        manifest.engine = {"a0.m2.offline-stitcher", "1.0.0"};
        manifest.output = {"stitched.jpg", a0::m2::ComputeFileSha256Hex(root / L"stitched.jpg"), 16, 8,
            fs::file_size(root / L"stitched.jpg")};
        manifest.completed_at_utc = "2026-09-22T00:00:00Z";
        a0::m2::PublishAndVerifyStitchJobManifest(root, manifest);
        const auto manifest_path = root / L"stitch-job.manifest.json";
        const auto before = a0::m2::ComputeFileSha256Hex(manifest_path);
        auto invoke = [&](std::wstring job, std::wstring capture, int expected) {
            std::vector<std::wstring> values{L"adapter", L"verify-published-stitch", L"--job-directory", root.wstring(),
                L"--stitch-job-id", job, L"--capture-transaction-id", capture};
            std::vector<wchar_t*> pointers;
            for (auto& value : values) pointers.push_back(value.data());
            std::ostringstream output;
            auto* old = std::cout.rdbuf(output.rdbuf());
            const auto result = AdapterEntry(static_cast<int>(pointers.size()), pointers.data());
            std::cout.rdbuf(old);
            if (result != expected || (expected == 0 && output.str().find("result=verified-published-stitch") == std::string::npos) ||
                (expected != 0 && !output.str().empty())) throw std::runtime_error("verification command result mismatch");
        };
        const std::wstring job(32, L'1'), capture(32, L'2');
        invoke(job, capture, 0);
        invoke(std::wstring(32, L'3'), capture, 2);
        invoke(job, std::wstring(32, L'4'), 2);
        if (a0::m2::ComputeFileSha256Hex(manifest_path) != before ||
            a0::m2::ComputeFileSha256Hex(root / L"stitched.jpg") != manifest.output.sha256)
            throw std::runtime_error("read-only command mutated an artifact");
        { std::ofstream changed(root / L"stitched.jpg", std::ios::binary | std::ios::app); changed.put('x'); }
        invoke(job, capture, 2);
        if (!fs::remove(manifest_path)) return 2; // Exact synthetic fixture only.
        invoke(job, capture, 2);
        if (!fs::remove(root / L"stitched.jpg") || !fs::remove(root)) return 2;
        std::cout << "{\"publishedStitchCommand\":\"passed\",\"hardwareOperations\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
