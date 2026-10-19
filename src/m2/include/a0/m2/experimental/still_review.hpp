#pragma once
#include "a0/m2/render.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <cmath>
#include <stdexcept>
#include <optional>
#include "a0/m2/experimental/review_alignment.hpp"

namespace a0::m2::experimental {
struct PreviewReviewGeometry {
    std::uint32_t width{},height{},overlap{};
    int vertical{};
    std::uint64_t revision{};
};
// Approximate normalized-frame translation only. Optical field-of-view
// equivalence has NOT been measured. Never produces a rig profile or approval.
inline render::PairRenderParameters ScalePreviewReviewGeometry(
    PreviewReviewGeometry g,std::uint32_t width,std::uint32_t height){
    if(g.width<2 || g.width>640 || !g.height || g.height>640 || !g.overlap || g.overlap>=g.width ||
        g.vertical<=-static_cast<int>(g.height) || g.vertical>=static_cast<int>(g.height))
        throw std::invalid_argument("invalid review preview geometry");
    (void)render::PixelCount(width,height);
    const auto dx=std::llround(double(g.width-g.overlap)*width/g.width);
    const auto dy=std::llround(double(g.vertical)*height/g.height);
    if(dx<=0 || dx>=width || std::abs(dy)>=height)throw std::invalid_argument("review translation is outside image");
    const auto trim=static_cast<std::uint32_t>(std::abs(dy));
    render::PairRenderParameters p{{1,0,double(dx),0,1,double(dy),0,0,1},StitchLayout::camera_a_left_camera_b_right,{0,trim,0,trim}};
    (void)render::PlanPairCanvas(width,height,width,height,p);return p;
}
struct StillReviewRequest {
    std::filesystem::path camera_a_original,camera_b_original,output_directory;
    std::string camera_a_sha256,camera_b_sha256,pair_id;
    PreviewReviewGeometry geometry;
    std::chrono::steady_clock::time_point deadline;
    struct ManualRevision {
        int x{},y{};
        std::filesystem::path parent_report;
        std::string parent_report_sha256;
        double rotation_degrees{},scale{1};
    };
    std::optional<ManualRevision> manual_revision;
};
struct StillReviewResult {
    std::filesystem::path jpeg,report;
    std::string sha256;
    std::uint32_t width{},height{};
    render::BgrImage thumbnail;
};
// Exists only in the explicitly enabled development preview build. A separate
// uncalibrated review report, never a successful product StitchJob manifest.
StillReviewResult WriteUncalibratedStillReview(const StillReviewRequest& request);
struct SavedStillReview {
    std::filesystem::path report,jpeg,camera_a_original,camera_b_original;
    std::string report_sha256,jpeg_sha256,camera_a_sha256,camera_b_sha256,pair_id;
    std::uint64_t jpeg_bytes{},camera_a_bytes{},camera_b_bytes{};
    std::uint32_t source_width{},source_height{},width{},height{};
    PreviewReviewGeometry geometry;
    int translation_x{},translation_y{};
    std::optional<StillReviewRequest::ManualRevision> manual_revision;
    double rotation_degrees{},scale{1};
    StitchCropPixels crop;
};
inline render::PairRenderParameters StillPixelReviewGeometry(int x,int y,std::uint32_t width,std::uint32_t height){
    (void)render::PixelCount(width,height);
    if(x<=0 || x>=static_cast<int>(width) || y<=-static_cast<int>(height) || y>=static_cast<int>(height))
        throw std::invalid_argument("manual review translation is outside image");
    const auto trim=static_cast<std::uint32_t>(std::abs(y));
    render::PairRenderParameters p{{1,0,double(x),0,1,double(y),0,0,1},StitchLayout::camera_a_left_camera_b_right,{0,trim,0,trim}};
    (void)render::PlanPairCanvas(width,height,width,height,p);return p;
}
// Camera-free, explicit-report loading. Verifies schema, false quality flags,
// aliases, immutable input/output hashes/dimensions and immediate parent link.
SavedStillReview LoadSavedStillReview(const std::filesystem::path& report);
render::BgrImage LoadStillReviewImage(const SavedStillReview& review);
std::array<render::BgrImage,2> LoadStillReviewSources(const SavedStillReview& review);
}
