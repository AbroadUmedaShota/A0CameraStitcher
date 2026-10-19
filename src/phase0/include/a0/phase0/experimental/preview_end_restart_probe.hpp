#pragma once
#include <cstdint>
namespace a0::phase0::experimental {
struct PreviewEndRestartObservation {
    bool unexpected_failure{},camera_ended{},joined{},restart_allowed{},live_fresh{},merge_fresh{};
    unsigned sessions{};std::uint64_t merge_paints{};
};
enum class PreviewEndRestartAction {Wait,Restart,Complete,Fail};
// Explicit hardware test only. No timer, transport or settings access. The
// caller must classify an actual camera-ended response and confirm cleanup.
class PreviewEndRestartProbe final {
public:
    PreviewEndRestartAction Next(const PreviewEndRestartObservation& view){
        if(phase_==Phase::Done)return PreviewEndRestartAction::Complete;
        if(phase_==Phase::Failed)return PreviewEndRestartAction::Fail;
        if(view.unexpected_failure || view.sessions==0 || view.sessions>2){phase_=Phase::Failed;return PreviewEndRestartAction::Fail;}
        if(phase_==Phase::End){
            if(view.sessions!=1){phase_=Phase::Failed;return PreviewEndRestartAction::Fail;}
            if(!view.camera_ended || !view.joined || !view.restart_allowed)return PreviewEndRestartAction::Wait;
            baseline_=view.merge_paints;phase_=Phase::Fresh;return PreviewEndRestartAction::Restart;
        }
        if(view.sessions!=2 || !view.live_fresh || !view.merge_fresh || view.merge_paints<baseline_ || view.merge_paints-baseline_<5)return PreviewEndRestartAction::Wait;
        phase_=Phase::Done;return PreviewEndRestartAction::Complete;
    }
    bool Completed()const{return phase_==Phase::Done;}
private:
    enum class Phase {End,Fresh,Done,Failed};Phase phase_{Phase::End};std::uint64_t baseline_{};
};
}
