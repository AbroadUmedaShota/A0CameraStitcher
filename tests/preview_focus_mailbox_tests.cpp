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
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
