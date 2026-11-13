// Contract of the pure button layout of the development preview (#437 stage 0). No window, no camera.
#include "a0/phase0/experimental/preview_button_layout.hpp"

#include <cstdio>
#include <initializer_list>
#include <string>

using namespace a0::phase0::experimental;

namespace {
unsigned checks{},failures{};
void Check(bool ok,const std::string& what){
    ++checks;
    if(!ok){++failures;std::printf("FAILED: %s\n",what.c_str());}
}
bool Overlap(const LayoutBox& a,const LayoutBox& b){
    return a.x<b.x+b.width && b.x<a.x+a.width && a.y<b.y+b.height && b.y<a.y+a.height;
}
}  // namespace

int main(){
    // The lists and their enums stay in step: a button added to one and not the other would index out of range.
    Check(LowerButtonItems().size()==static_cast<std::size_t>(kLowerButtonCount),"lower list matches the enum");
    Check(PaneButtonItems().size()==static_cast<std::size_t>(kPaneButtonCount),"pane list matches its count");
    Check(LowerButtonItems(true).size()==static_cast<std::size_t>(kLowerRecal)+1,"the calibration entrance is one more item after the others");
    // With the calibration entrance, every width keeps every button inside the window and clear of the others.
    for(const int width:{1000,1024,1056,1400,1920}){
        const auto plan=PlanPreviewButtons(width,true);
        const std::string tag=" with the calibration entrance, width="+std::to_string(width);
        Check(plan.lower.size()==static_cast<std::size_t>(kLowerRecal)+1,"every button has a place"+tag);
        for(std::size_t a=0;a<plan.lower.size();++a){
            Check(plan.lower[a].x>=12 && plan.lower[a].x+plan.lower[a].width<=width-15,"inside the lower half"+tag);
            for(std::size_t b=a+1;b<plan.lower.size();++b)Check(!Overlap(plan.lower[a],plan.lower[b]),"no overlap"+tag);
        }
    }
    // A wide window gives the places of the fixed numbers this layout replaced.
    for(const int width:{1400,1920,3000}){
        const auto plan=PlanPreviewButtons(width);
        const std::string tag=" width="+std::to_string(width);
        const int pane=width/2;
        for(int i=0;i<2;++i){
            const auto& boxes=plan.pane[static_cast<std::size_t>(i)];
            for(int j=0;j<3;++j)Check(boxes[static_cast<std::size_t>(j)].x==i*pane+12+j*125 && boxes[static_cast<std::size_t>(j)].y==40 && boxes[static_cast<std::size_t>(j)].width==115,"focus button"+tag);
            Check(boxes[3].x==i*pane+387 && boxes[3].y==40 && boxes[3].width==145,"shot button"+tag);
        }
        for(int j=0;j<5;++j)Check(plan.lower[kLowerMerge0+j].x==12+j*145 && plan.lower[kLowerMerge0+j].y==kLowerButtonsTop && plan.lower[kLowerMerge0+j].width==135,"merge button"+tag);
        Check(plan.lower[kLowerPair].x==755 && plan.lower[kLowerPair].y==kLowerButtonsTop && plan.lower[kLowerPair].width==235,"pair button"+tag);
        Check(plan.lower[kLowerReview].x==1005 && plan.lower[kLowerReview].y==kLowerButtonsTop && plan.lower[kLowerReview].width==225,"review button"+tag);
        Check(plan.lower[kLowerSeam].x==1245 && plan.lower[kLowerSeam].y==kLowerButtonsTop && plan.lower[kLowerSeam].width==140,"seam button"+tag);
        Check(plan.lower[kLowerInspect].x==width-215 && plan.lower[kLowerInspect].y==kLowerButtonsTop+kLowerRowPitch,"inspect button"+tag);
        Check(plan.lower[kLowerSession].x==width-430 && plan.lower[kLowerSession].y==kLowerButtonsTop+kLowerRowPitch,"session button"+tag);
        Check(plan.lower[kLowerSettings].x==width-645 && plan.lower[kLowerSettings].y==kLowerButtonsTop+kLowerRowPitch,"settings button"+tag);
        Check(plan.pane_rows==1 && plan.top_shift==0 && plan.lower_rows==2,"two rows of buttons"+tag);
        // The status text (mid+4..mid+86) ends above the first row of buttons, and the title stands on the row of the right-aligned
        // buttons: a wide window moves the content under the buttons down by the one row the status text needed.
        Check(plan.lower_shift==kLowerRowPitch && kLowerSharedTitleRow==kLowerTitleRow+plan.lower_shift,"wide: the content is one row lower than the places before the status text had its own rows"+tag);
    }
    // Narrower windows wrap. Nothing leaves the window, nothing overlaps, and the content under the buttons is pushed below them.
    for(const int width:{900,1000,1056,1100,1240,1300,1384,1399}){
        const auto plan=PlanPreviewButtons(width);
        const std::string tag=" width="+std::to_string(width);
        const int pane=width/2;
        for(int i=0;i<2;++i)for(const auto& box:plan.pane[static_cast<std::size_t>(i)]){
            Check(box.x>=i*pane+12 && box.x+box.width<=i*pane+pane-12,"pane button inside its pane"+tag);
            Check(box.y+box.height<=kPaneButtonsTop+kPaneRowPitch*plan.pane_rows,"pane rows counted"+tag);
        }
        Check(plan.top_shift==(plan.pane_rows-1)*kPaneRowPitch,"top shift"+tag);
        std::vector<LayoutBox> all;
        for(const auto& boxes:plan.pane)all.insert(all.end(),boxes.begin(),boxes.end());
        for(std::size_t a=0;a<all.size();++a)for(std::size_t b=a+1;b<all.size();++b)Check(!Overlap(all[a],all[b]),"pane buttons overlap"+tag);
        int bottom=0;
        for(const auto& box:plan.lower){
            Check(box.x>=12 && box.x+box.width<=width-15,"lower button inside the window"+tag);
            bottom=std::max(bottom,box.y+box.height);
        }
        for(std::size_t a=0;a<plan.lower.size();++a)for(std::size_t b=a+1;b<plan.lower.size();++b)Check(!Overlap(plan.lower[a],plan.lower[b]),"lower buttons overlap"+tag);
        Check(bottom<=kLowerTitleRow+plan.lower_shift,"content starts below the buttons"+tag);
        Check(plan.lower_shift==0 || bottom==kLowerButtonsTop+kLowerRowPitch*plan.lower_rows-(kLowerRowPitch-kLayoutButtonHeight),"shift equals the rows added"+tag);
    }
    // The status text never meets a button at any width: it ends above the first row of buttons.
    for(const int width:{900,1000,1024,1056,1100,1240,1384,1399,1400,1920}){
        const auto plan=PlanPreviewButtons(width);
        const LayoutBox status{12,kLowerStatusTop,width-24,kLowerStatusBottom-kLowerStatusTop,0};
        for(const auto& box:plan.lower)Check(!Overlap(status,box),"status text clear of the buttons width="+std::to_string(width));
        Check(plan.lower_shift>=kLowerRowPitch,"the shift includes the status row width="+std::to_string(width));
    }
    // A narrow screen: the shot button and the pair button wrap, the rest stays in view.
    {
        const auto plan=PlanPreviewButtons(1000);
        Check(plan.pane_rows==2 && plan.top_shift==34,"1000: shot button on a second row");
        Check(plan.pane[0][3].row==1 && plan.pane[0][3].x==12 && plan.pane[1][3].x==512,"1000: shot buttons start their row");
        Check(plan.lower_rows==3 && plan.lower_shift==108,"1000: three rows of buttons");
        Check(plan.lower[kLowerPair].row==1 && plan.lower[kLowerReview].row==1 && plan.lower[kLowerSeam].row==1,"1000: pair, review and seam on the second row");
        Check(plan.lower[kLowerInspect].row==2 && plan.lower[kLowerSettings].row==2,"1000: the right group on the third row");
    }
    // A button list of any length ends: a narrow window gives one button per row, a wide one a single row.
    {
        const auto narrow=FlowButtons({{200},{200},{200,15,true},{200,15,true}},12,100,0,36);
        Check(narrow.rows==4 && narrow.boxes[0].row==0 && narrow.boxes[1].row==1 && narrow.boxes[2].row==2 && narrow.boxes[3].row==3,"narrow flow");
        const auto empty=FlowButtons({},12,100,0,36);
        Check(empty.rows==0 && empty.boxes.empty(),"empty flow");
        const auto tiny=PlanPreviewButtons(100);
        Check(tiny.pane_rows>=1 && tiny.lower_rows>=2,"tiny window");
        // A button added to a group takes the next place in the order and wraps like the others.
        auto items=LowerButtonItems();items.insert(items.begin()+kLowerSeam+1,LayoutButton{100});
        const auto added=FlowButtons(items,12,1385,52,36);
        Check(added.boxes[kLowerSeam+1].x==1245+140+10 || added.boxes[kLowerSeam+1].row==1,"an added button follows the seam button or wraps");
    }
    std::printf("preview_button_layout_checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
