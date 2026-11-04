#pragma once
// PC original persistence for one WPD still payload (#408): the step the
// development pair window runs after a body's import. No camera, device,
// window or log is touched here, so the refusals can be tested directly.
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/pc_direct_capture.hpp"
#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/wpd_still_capture.hpp"
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace a0::phase0::experimental {
// JPEG markers, full pixel decode at the reported size, then PersistExactlyOne
// (.partial, atomic rename without replace, reread and SHA-256) under the
// transaction deadline. Throws on any refusal; on success returns the saved
// original's path, size and hash. `steady_now` overrides the deadline clock
// for tests only; production leaves it empty.
inline VerifiedStillOriginal PersistVerifiedWpdStill(EvidenceWriter& evidence,const std::string& alias,WpdStillPayload payload,
    std::chrono::steady_clock::time_point deadline,
    const std::function<std::chrono::steady_clock::time_point()>& steady_now={}){
    if(!IsValidJpeg(payload.jpeg))throw std::runtime_error("captured bytes are not a valid JPEG");
    (void)FullyDecodeJpeg(payload.jpeg,payload.info.width,payload.info.height);
    const auto frame=evidence.PersistExactlyOne("capture",alias,{{"camera-original.jpg",std::move(payload.jpeg),true,{}}},deadline,{},steady_now);
    if(!frame.success)throw std::runtime_error("PC original persistence failed: "+frame.error_category);
    VerifiedStillOriginal original;
    original.success=true;original.path=frame.path;original.bytes=frame.bytes;original.sha256=frame.sha256;
    return original;
}
// After the session closed: a transaction that reached its deadline is not a
// success, whatever was saved before it. The PC bytes are kept for diagnosis.
inline void ApplyStillDeadline(VerifiedStillOriginal& outcome,std::chrono::steady_clock::time_point now,std::chrono::steady_clock::time_point deadline){
    if(now>=deadline){outcome.success=false;outcome.error+="; transaction deadline exceeded; PC bytes retained";}
}
}
