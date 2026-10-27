#pragma once
#include "a0/common/protocol_json.hpp"
#include "a0/m2/experimental/mount_orientation.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace a0::phase0::experimental {
using a0::m2::experimental::MountOrientation;
// Local development default beside camera-map-wpd.json. An absent file means
// normal; any other unreadable or non-exact content refuses startup.
inline constexpr const wchar_t* kMountOrientationSettingsName=L"wpd-mount-orientation.json";
struct MountSettingsFailure {[[noreturn]] static void Fail(std::string_view,std::string_view){throw std::invalid_argument("invalid mount orientation settings");}};
inline MountOrientation ParseMountOrientationSettings(std::string_view text){
    namespace json=a0::common::protocol_json;
    if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3); // encoding marker only
    const auto root=json::BasicJsonParser<MountSettingsFailure,4096>(text).Parse();
    if(root.kind!=json::JsonKind::object || root.object.size()!=2 || !root.object.contains("schema") || !root.object.contains("mountOrientation"))MountSettingsFailure::Fail({},{});
    const auto& schema=root.object.at("schema");const auto& value=root.object.at("mountOrientation");
    if(schema.kind!=json::JsonKind::string || schema.string!="a0.wpd-mount-orientation.v1" || value.kind!=json::JsonKind::string)MountSettingsFailure::Fail({},{});
    const auto orientation=a0::m2::experimental::ParseMountOrientation(value.string);
    if(!orientation)MountSettingsFailure::Fail({},{});return *orientation;
}
inline std::optional<MountOrientation> LoadMountOrientationSettings(const std::filesystem::path& file){
    std::error_code error;const auto status=std::filesystem::symlink_status(file,error);
    if(status.type()==std::filesystem::file_type::not_found)return {};
    if(error || status.type()!=std::filesystem::file_type::regular)throw std::invalid_argument("mount orientation settings is not a regular file");
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("mount orientation settings unreadable");
    std::string text;text.resize(4097);in.read(text.data(),static_cast<std::streamsize>(text.size()));
    if(in.bad())throw std::runtime_error("mount orientation settings unreadable");
    text.resize(static_cast<size_t>(in.gcount()));if(text.size()>4096)throw std::invalid_argument("mount orientation settings too large");
    return ParseMountOrientationSettings(text);
}
}
