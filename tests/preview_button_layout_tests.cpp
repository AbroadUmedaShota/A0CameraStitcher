// Contract of the pure button layout of the development preview (#437 stage 0). No window, no camera.
#include "a0/phase0/experimental/preview_button_layout.hpp"

#include <algorithm>
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
    // A condition v3 has the hand buttons (#437 stage 2): after the other buttons, on a row of their own, in one row at every width the window can have; the
    // four position buttons of the uncalibrated C stand aside and the automatic button stays; nothing is outside the window or on another button.
    {
        Check(LowerButtonItems(true).size()==static_cast<std::size_t>(kLowerButtonCount+kPreviewHandButtonCount),"hand list: the lower buttons and the hand buttons");
        Check(LowerButtonItems(false).size()==static_cast<std::size_t>(kLowerButtonCount),"no hand buttons: the list is the old one");
        for(const int width:{1000,1024,1056,1280,1400,1920,3000}){
            const auto plan=PlanPreviewButtons(width,true);
            const std::string tag=" hand width="+std::to_string(width);
            Check(plan.lower.size()==static_cast<std::size_t>(kLowerButtonCount+kPreviewHandButtonCount),"plan size"+tag);
            for(int j=0;j<4;++j)Check(plan.lower[kLowerMerge0+j].width==0,"a position button of the uncalibrated C has no place"+tag);
            Check(plan.lower[kLowerMerge0+4].width==135 && plan.lower[kLowerMerge0+4].x==12 && plan.lower[kLowerMerge0+4].y==kLowerButtonsTop,"the automatic button is the first of the row"+tag);
            int row_of_others=0;
            for(const int index:std::initializer_list<int>{kLowerMerge0+4,kLowerPair,kLowerReview,kLowerSeam})row_of_others=std::max(row_of_others,plan.lower[static_cast<std::size_t>(index)].row);
            const auto& first=plan.lower[static_cast<std::size_t>(kLowerHandFirst)];
            Check(first.row==row_of_others+1 && first.x==kLayoutMargin,"the hand buttons begin a row of their own"+tag);
            for(int i=0;i<kPreviewHandButtonCount;++i){
                const auto& box=plan.lower[static_cast<std::size_t>(kLowerHandFirst+i)];
                Check(box.row==first.row && box.y==first.y && box.width>0,"the hand buttons are in one row"+tag);
                if(i)Check(box.x>=plan.lower[static_cast<std::size_t>(kLowerHandFirst+i-1)].x+plan.lower[static_cast<std::size_t>(kLowerHandFirst+i-1)].width+kLayoutGap,"in order, with the gap"+tag);
            }
            for(std::size_t a=0;a<plan.lower.size();++a){
                if(plan.lower[a].width==0)continue;
                Check(plan.lower[a].x>=kLayoutMargin && plan.lower[a].x+plan.lower[a].width<=width-kLayoutRightMargin,"inside the window"+tag);
                for(std::size_t c=a+1;c<plan.lower.size();++c)if(plan.lower[c].width!=0)Check(!Overlap(plan.lower[a],plan.lower[c]),"no overlap"+tag);
            }
            int bottom=0;
            for(const auto& box:plan.lower)if(box.width)bottom=std::max(bottom,box.y+box.height);
            Check(bottom<=kLowerTitleRow+plan.lower_shift,"hand: content starts below the buttons"+tag);
            Check(plan.lower_shift>=PlanPreviewButtons(width,false).lower_shift,"the hand row only pushes the content down"+tag);
        }
    }
    // The rows under the title of C: a short text keeps the places the rows had before they were measured; a longer text takes room and pushes the rest down.
    {
        const auto plain=PlanLowerRows(0,0,false),short_registered=PlanLowerRows(10,10,true);
        Check(plain.picture_top==140 && plain.note_height==kLowerNoteMinHeight,"no rows: the picture starts at 140");
        Check(short_registered.state_top==140 && short_registered.orientation_top==163 && short_registered.picture_top==186 && short_registered.state_height==21,"a registered C: 140, 163, 186");
        Check(short_registered.picture_top-plain.picture_top==46,"the two rows take 46");
        const auto long_note=PlanLowerRows(50,10,true);
        Check(long_note.note_height==50 && long_note.state_top==long_note.note_top+50+kLowerRowGap && long_note.picture_top==short_registered.picture_top+(50-kLowerNoteMinHeight),"a note of three lines pushes the rows down by what it needs");
        const auto long_state=PlanLowerRows(10,63,true);
        Check(long_state.state_height==63 && long_state.orientation_top==long_state.state_top+63+kLowerRowGap && long_state.picture_top==short_registered.picture_top+(63-kLowerStateMinHeight),"a state row of three lines pushes the rest down");
        for(const int note:{0,15,28,29,45,90})for(const int state:{0,10,21,22,70}){
            const auto plan=PlanLowerRows(note,state,true);
            Check(plan.note_top+plan.note_height+kLowerRowGap<=plan.state_top && plan.state_top+plan.state_height+kLowerRowGap<=plan.orientation_top &&
                plan.orientation_top+plan.orientation_height+kLowerRowGap<=plan.picture_top,"the rows never overlap: note "+std::to_string(note)+" state "+std::to_string(state));
        }
    }
    std::printf("preview_button_layout_checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
