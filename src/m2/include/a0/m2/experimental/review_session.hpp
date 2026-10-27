#pragma once
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/review_correspondence.hpp"

namespace a0::m2::experimental {
struct ReviewSessionState {
    std::filesystem::path review_report;
    std::string review_report_sha256;
    int x{},y{};double degrees{},scale{1};
    std::vector<ReviewCorrespondence> points;
    ReviewPointRole next_point_role{ReviewPointRole::fitting};
};
struct SavedReviewSession {
    std::filesystem::path file;
    std::string sha256;
    SavedStillReview review;
    ReviewSessionState state;
};
inline bool ReviewSessionBelongsToPair(const SavedReviewSession& session,const SavedStillReview& current){
    const auto& source=session.review;
    return source.pair_id==current.pair_id && source.camera_a_original==current.camera_a_original && source.camera_b_original==current.camera_b_original &&
        source.camera_a_sha256==current.camera_a_sha256 && source.camera_b_sha256==current.camera_b_sha256 && source.camera_a_bytes==current.camera_a_bytes &&
        source.camera_b_bytes==current.camera_b_bytes && source.source_width==current.source_width && source.source_height==current.source_height &&
        source.geometry.orientation==current.geometry.orientation; // points live in the oriented input frame
}
// Explicit private work files, separate from merged JPEGs and calibration.
SavedReviewSession LoadReviewSession(const std::filesystem::path& file);
SavedReviewSession WriteReviewSession(const ReviewSessionState& state,const std::filesystem::path& new_directory,
    std::chrono::steady_clock::time_point deadline);
}
