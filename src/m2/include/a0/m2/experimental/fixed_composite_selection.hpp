#pragma once
#include "a0/m2/experimental/fixed_composite_condition_v2.hpp"
#include "a0/m2/experimental/still_review.hpp"
#include <filesystem>
#include <optional>
#include <string>

namespace a0::m2::experimental {
// Reading the fixed composite condition file of a setup, whichever version it is (#405 W4,
// shared by the JPEG path and the live C path). The file name is the same for v1 and v2; the
// "schema" member decides, and LoadFixedCompositeConditionAny reads either. An absent file is
// not an error: there is simply no condition. Everything else that is not exact is a reason
// the composite is skipped, never a reason to guess:
//   condition_absent  no file;
//   condition_invalid the record is damaged, of an unknown schema, or unreadable;
//   profile_absent    a v2 record whose profile file is not beside it;
//   profile_differs   a v2 record whose profile is there but is not the one it names or cannot be used;
//   pose_outside_envelope  a v3 record whose hand adjustment put the base pose beyond registration.envelope (the record is intact).
// `detail` carries the reader's own message (English, for logs).
struct FixedCompositeSelection {
    std::optional<LoadedFixedCompositeConditionAny> loaded;
    FixedCompositeSkip reason{FixedCompositeSkip::condition_absent};
    std::string detail;
    [[nodiscard]] bool IsV2() const {return loaded && std::holds_alternative<LoadedFixedCompositeConditionV2>(*loaded);}
    // v3 (#417): the revision in force beside the condition file's name (the revisions folder of the setup). A setup that has
    // a v1 or v2 file as well is condition_invalid, not v3.
    [[nodiscard]] bool IsV3() const {return loaded && std::holds_alternative<LoadedFixedCompositeConditionV3>(*loaded);}
};
[[nodiscard]] inline FixedCompositeSelection SelectFixedComposite(const std::filesystem::path& file,const FixedCompositeSha256& hash){
    FixedCompositeSelection selection;
    try{selection.loaded=LoadFixedCompositeConditionAny(file,hash);}
    catch(const FixedCompositeProfileError& error){
        selection.reason=error.GetKind()==FixedCompositeProfileError::Kind::missing?FixedCompositeSkip::profile_absent:FixedCompositeSkip::profile_differs;
        selection.detail=error.what();
    }
    catch(const FixedCompositePoseOutsideEnvelope& error){selection.reason=FixedCompositeSkip::pose_outside_envelope;selection.detail=error.what();}
    catch(const std::exception& error){selection.reason=FixedCompositeSkip::condition_invalid;selection.detail=error.what();}
    return selection;
}
}
