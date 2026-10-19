#pragma once
#include <cstdint>

namespace a0::phase0::experimental {
// Explicit development-test driver only; no command or camera access here.
struct PairCycleObservation {
    bool failed{},idle{},live_ready{},merge_ready{};
    unsigned sessions{},reviews{},saved{};
    std::uint64_t review_paints{},merge_paints{};
};
enum class PairCycleAction {Wait,Capture,ShowLive,Complete,Fail};
class PairCycleProbe {
public:
    PairCycleAction Next(const PairCycleObservation& view){
        if(phase_==Phase::Done)return PairCycleAction::Complete;
        if(phase_==Phase::Failed)return PairCycleAction::Fail;
        if(view.failed || !view.sessions || view.sessions>requested_+1 || view.reviews>requested_ || view.saved!=2*view.reviews){
            phase_=Phase::Failed;return PairCycleAction::Fail;
        }
        if(!view.idle || !view.live_ready)return PairCycleAction::Wait;
        if(phase_==Phase::Ready){
            if(view.sessions!=completed_+1 || view.reviews!=completed_){phase_=Phase::Failed;return PairCycleAction::Fail;}
            if(!view.merge_ready || view.merge_paints<5)return PairCycleAction::Wait;
            ++requested_;review_baseline_=view.review_paints;merge_baseline_=view.merge_paints;
            phase_=Phase::Review;return PairCycleAction::Capture;
        }
        if(view.sessions!=requested_+1 || view.reviews!=requested_)return PairCycleAction::Wait;
        if(phase_==Phase::Review){
            if(view.review_paints<review_baseline_ || view.review_paints-review_baseline_<5)return PairCycleAction::Wait;
            phase_=Phase::Live;return PairCycleAction::ShowLive;
        }
        if(!view.merge_ready || view.merge_paints<merge_baseline_ || view.merge_paints-merge_baseline_<5)return PairCycleAction::Wait;
        if(++completed_==2){phase_=Phase::Done;return PairCycleAction::Complete;}
        phase_=Phase::Ready;return PairCycleAction::Wait;
    }
    unsigned Completed()const{return completed_;}
private:
    enum class Phase {Ready,Review,Live,Done,Failed};
    Phase phase_{Phase::Ready};unsigned requested_{},completed_{};
    std::uint64_t review_baseline_{},merge_baseline_{};
};
}
