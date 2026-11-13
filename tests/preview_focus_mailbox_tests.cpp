#include "a0/phase0/experimental/preview_focus.hpp"
#include <array>
#include <future>
#include <iostream>
using namespace a0::phase0::experimental;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far}){
        check(AdmitPreviewFocusMode(action,0)==PreviewFocusAdmission::Allow);
        check(AdmitPreviewFocusMode(action,3)==PreviewFocusAdmission::NeedsAfSwitch);
        check(AdmitPreviewFocusMode(action,4)==PreviewFocusAdmission::NeedsAfSwitch);
        check(AdmitPreviewFocusMode(action,99)==PreviewFocusAdmission::UnsupportedMode);
    }
    check(AdmitPreviewFocusMode(PreviewFocusAction::AutoFocus,2)==PreviewFocusAdmission::Allow);
    check(AdmitPreviewFocusMode(PreviewFocusAction::Near,2)==PreviewFocusAdmission::NeedsAfS);
    check(AdmitPreviewFocusMode(PreviewFocusAction::Far,2)==PreviewFocusAdmission::NeedsAfS);
    check(AdmitPreviewFocusMode(static_cast<PreviewFocusAction>(9),0)==PreviewFocusAdmission::UnsupportedMode);
    PreviewFocusMailbox a,b;check(!a.Take());check(!a.TryQueue(PreviewFocusAction::AutoFocus,false));
    check(!a.TryQueue(static_cast<PreviewFocusAction>(9),true));
    for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far}){
        check(a.Available());check(a.TryQueue(action,true));check(!a.Available());
        check(!a.TryQueue(action,true));check(a.Take()==action);check(!a.Take());
        check(!a.TryQueue(action,true));check(b.TryQueue(action,true));check(b.Take()==action);
        b.Complete();a.Complete();check(a.Available() && b.Available());
    }
    std::array<std::future<bool>,8> attempts;
    for(auto& attempt:attempts)attempt=std::async(std::launch::async,[&]{return a.TryQueue(PreviewFocusAction::AutoFocus,true);});
    int accepted=0;for(auto& attempt:attempts)if(attempt.get())++accepted;
    check(accepted==1);check(a.Take()==PreviewFocusAction::AutoFocus);a.Disable();a.Complete();
    check(!a.Available() && !a.Take() && !a.TryQueue(PreviewFocusAction::Far,true));
    check(b.TryQueue(PreviewFocusAction::Near,true));b.Disable();check(!b.Take());b.Complete();check(!b.Available());
    // An AF frame move (#426) shares the slot: one request at a time across AF, fine drive and the move.
    {
        const PreviewAfPointRequest point{600,400,1280,852};
        PreviewFocusMailbox m;
        check(point.Plausible() && !PreviewAfPointRequest{1280,0,1280,852}.Plausible() && !PreviewAfPointRequest{0,852,1280,852}.Plausible());
        check(!PreviewAfPointRequest{0,0,0,852}.Plausible() && !PreviewAfPointRequest{0,0,1280,0}.Plausible() && !PreviewAfPointRequest{0,0,0x8000,852}.Plausible());
        check(!m.TryQueueAfPoint(point,false));                                   // not admitted
        check(!m.TryQueueAfPoint(PreviewAfPointRequest{1280,0,1280,852},true));    // not a point of its space
        check(m.Available() && !m.TakeAfPoint());
        check(m.TryQueueAfPoint(point,true) && !m.Available());
        check(!m.Take());                                                         // not an AF or drive action
        check(!m.TryQueueAfPoint(point,true));                                    // one pending request
        check(!m.TryQueue(PreviewFocusAction::AutoFocus,true));                   // nor AF, nor drive, beside it
        check(m.TakeAfPoint()==point);
        check(!m.TakeAfPoint() && !m.Take());                                     // taken once
        check(!m.TryQueueAfPoint(point,true) && !m.TryQueue(PreviewFocusAction::Near,true)); // running
        m.Complete();check(m.Available());
        // The other way: a pending AF or drive keeps the move out, and a move is not mistaken for them.
        for(auto action:{PreviewFocusAction::AutoFocus,PreviewFocusAction::Near,PreviewFocusAction::Far}){
            check(m.TryQueue(action,true));check(!m.TryQueueAfPoint(point,true));check(!m.TakeAfPoint());
            check(m.Take()==action);check(!m.TryQueueAfPoint(point,true));m.Complete();
        }
        // Every value survives the slot unchanged, including the largest.
        for(const PreviewAfPointRequest& value:{PreviewAfPointRequest{0,0,1,1},PreviewAfPointRequest{0x7ffe,0x7ffe,0x7fff,0x7fff},PreviewAfPointRequest{1,2,3,4},PreviewAfPointRequest{255,256,257,258}}){
            check(m.TryQueueAfPoint(value,true));check(m.TakeAfPoint()==value);m.Complete();
        }
        // Eight clicks at once leave one request.
        std::array<std::future<bool>,8> clicks;
        for(auto& click:clicks)click=std::async(std::launch::async,[&]{return m.TryQueueAfPoint(point,true);});
        int queued=0;for(auto& click:clicks)if(click.get())++queued;
        check(queued==1);check(m.TakeAfPoint()==point);check(!m.TakeAfPoint());m.Complete();
        // A request waiting when the session closes is dropped, and closing is sticky.
        check(m.TryQueueAfPoint(point,true));m.Disable();
        check(!m.TakeAfPoint() && !m.Available() && !m.TryQueueAfPoint(point,true));
        m.Complete();check(!m.Available() && !m.TryQueueAfPoint(point,true));
        PreviewFocusMailbox running;check(running.TryQueueAfPoint(point,true) && running.TakeAfPoint()==point);
        running.Disable();running.Complete();check(!running.Available() && !running.TryQueueAfPoint(point,true));
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
