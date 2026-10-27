#pragma once
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace a0::phase0::experimental {
using a0::m2::experimental::StillMergePolicy;
// Administrator-written local policy beside camera-map-wpd.json (#365). An
// absent file means no policy: values are shown, nothing is corrected. Any
// other unreadable or non-exact content refuses startup. No field has a code
// default; similarityResidualMaxWarningPixels may be omitted.
inline constexpr const wchar_t* kMergePolicySettingsName=L"wpd-merge-policy.json";
struct MergePolicyFailure {[[noreturn]] static void Fail(std::string_view,std::string_view){throw std::invalid_argument("invalid merge policy settings");}};
// sha256 is the caller's hash of the exact file bytes, recorded in reports.
inline StillMergePolicy ParseMergePolicySettings(std::string_view text,std::string sha256){
    namespace json=a0::common::protocol_json;
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3); // encoding marker only
    const auto root=json::BasicJsonParser<MergePolicyFailure,4096>(text).Parse();
    const bool residual=root.kind==json::JsonKind::object && root.object.contains("similarityResidualMaxWarningPixels");
    if(root.kind!=json::JsonKind::object || root.object.size()!=2u+(residual?1u:0u) || !root.object.contains("schema") || !root.object.contains("rotationAutoCorrectionMaxDegrees"))MergePolicyFailure::Fail({},{});
    const auto& schema=root.object.at("schema");
    if(schema.kind!=json::JsonKind::string || schema.string!="a0.wpd-merge-policy.v1")MergePolicyFailure::Fail({},{});
    const auto number=[](const json::JsonValue& value){
        if(value.kind!=json::JsonKind::number)MergePolicyFailure::Fail({},{});double n{};
        const auto parsed=std::from_chars(value.string.data(),value.string.data()+value.string.size(),n,std::chars_format::general);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.string.data()+value.string.size() || !std::isfinite(n))MergePolicyFailure::Fail({},{});return n;
    };
    StillMergePolicy policy{std::move(sha256),number(root.object.at("rotationAutoCorrectionMaxDegrees")),{}};
    if(residual)policy.residual_max_pixels=number(root.object.at("similarityResidualMaxWarningPixels"));
    try{a0::m2::experimental::ValidateStillMergePolicy(policy);}catch(const std::invalid_argument&){MergePolicyFailure::Fail({},{});}
    return policy;
}
// hash: SHA-256 lower hex of the bytes read, supplied by the caller's provider.
template<class Hash>
std::optional<StillMergePolicy> LoadMergePolicySettings(const std::filesystem::path& file,Hash hash){
    std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
    if(status.type()==std::filesystem::file_type::not_found)return {};
    if(error || status.type()!=std::filesystem::file_type::regular)throw std::invalid_argument("merge policy settings is not a regular file");
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("merge policy settings unreadable");
    std::string text;text.resize(4097);in.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(in.bad())throw std::runtime_error("merge policy settings unreadable");
    text.resize(static_cast<size_t>(in.gcount()));if(text.size()>4096)throw std::invalid_argument("merge policy settings too large");
    return ParseMergePolicySettings(text,hash(std::vector<unsigned char>(text.begin(),text.end())));
}
}
