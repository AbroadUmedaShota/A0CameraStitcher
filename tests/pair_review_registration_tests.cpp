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
#include "a0/phase0/experimental/pair_review_registration.hpp"
#include "a0/phase0/experimental/pair_review_synthetic.hpp"
#include "similarity_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
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

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::invalid_argument("usage: pair_review_registration_tests <rig-profile-v2 fixture folder>");
        pair_synthetic::detail::Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
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
