#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <exception>

namespace a0::phase0::experimental {
struct VerifiedStillOriginal {
    bool success{},closed{};
    std::filesystem::path path;
    std::uintmax_t bytes{};
    std::string sha256,error;
    bool out_of_focus{};
};
struct VerifiedStillPair {std::array<VerifiedStillOriginal,2> originals;bool complete{};std::string error;bool out_of_focus{},preflight_complete{};};
struct VerifiedStillPreflight {bool ready{},closed{};std::string error;bool out_of_focus{};};
// One A followed by one B, bound by the caller to registered aliases. No
// retry, fallback, stale original selection or separate per-camera deadline.
template<class Capture,class Admission>
VerifiedStillPair CaptureStillPairOnce(Capture capture,Admission admission,std::chrono::steady_clock::time_point deadline){
    VerifiedStillPair result;
    for(int i=0;i<2;++i){
        if(!admission() || std::chrono::steady_clock::now()>=deadline){result.error="pair transaction deadline/admission";return result;}
        try{result.originals[i]=capture(i,deadline);}catch(const std::exception& error){result.error=error.what();return result;}
        const auto& original=result.originals[i];
        if(!original.success || !original.closed || original.path.empty() || !original.bytes || original.sha256.size()!=64 ||
            original.sha256.find_first_not_of("0123456789abcdef")!=std::string::npos){
            result.out_of_focus=original.out_of_focus;
            result.error=original.error.empty()?"pair original not fully verified and closed":original.error;return result;
        }
    }
    if(!admission() || std::chrono::steady_clock::now()>=deadline){result.error="pair completed after deadline/admission";return result;}
    result.complete=true;return result;
}
// Do not expose A before B's existing state/events have been checked. This
// earlier observation is not a reservation: capture must preflight again.
template<class Preflight,class Capture,class Admission>
VerifiedStillPair CapturePreflightedStillPairOnce(Preflight preflight,Capture capture,Admission admission,std::chrono::steady_clock::time_point deadline){
    VerifiedStillPair result;
    for(int i=0;i<2;++i){
        if(!admission() || std::chrono::steady_clock::now()>=deadline){result.error="pair preflight deadline/admission";return result;}
        VerifiedStillPreflight status;
        try{status=preflight(i,deadline);}catch(const std::exception& error){result.error=error.what();return result;}
        if(!status.ready || !status.closed){
            result.out_of_focus=status.out_of_focus;
            result.error=std::string(i==0?"CAM-A": "CAM-B")+" preflight: "+(status.error.empty()?"readiness/close unconfirmed":status.error);
            return result;
        }
    }
    auto captured=CaptureStillPairOnce(capture,admission,deadline);captured.preflight_complete=true;return captured;
}
}
