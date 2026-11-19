// Where the buttons of the development preview stand (#437 stage 0). Pure arithmetic: no window, no clock, no file.
//
// A list of buttons goes in and a place for each comes out. Buttons are put left to right from the left margin and move to
// the next row when the next one would end beyond the right margin; buttons marked right-aligned are put from the right
// margin on rows of their own, after the left-aligned ones. A wider window gives the same places as the fixed numbers
// this replaced, except that the buttons of the lower half stand one row lower, so the status text above them keeps its own
// rows. To add a button, add one entry to the list of its group and one line where the window is moved.
//
// Rows that wrap push the content under them down. top_shift is that distance for the two Live panes (status line, level
// gauge and picture), lower_shift for everything under the row of buttons in the lower half (title, notes, C, the saved
// result, the settings table).
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace a0::phase0::experimental {

inline constexpr int kLayoutMargin=12;        // left margin, and the right margin of a pane
inline constexpr int kLayoutRightMargin=15;   // right margin of the lower half
inline constexpr int kLayoutButtonHeight=28;
inline constexpr int kLayoutGap=10;
inline constexpr int kPaneButtonsTop=40;      // from the top of the client area
inline constexpr int kPaneRowPitch=34;
inline constexpr int kLowerStatusTop=4;       // the status text above the buttons, from the middle of the client area
inline constexpr int kLowerStatusBottom=86;
inline constexpr int kLowerButtonsTop=88;     // from the middle of the client area; the status text ends above it
inline constexpr int kLowerRowPitch=36;
inline constexpr int kLowerTitleRow=88;       // where the title and everything under it stand with no shift, from the middle
static_assert(kLowerStatusBottom<=kLowerButtonsTop,"the status text must end above the first row of buttons");
// The title may stand on the last row of buttons when the buttons there are right-aligned and leave room at its left.
inline constexpr int kLowerSharedTitleRow=kLowerButtonsTop+kLowerRowPitch;
// The title may share the last row of buttons only when the right-aligned buttons leave this much room at its left.
inline constexpr int kLowerTitleReserve=700;

struct LayoutButton {
    int width{};
    int gap_before{kLayoutGap};   // to the previous button of the row (to the one at its right, for a right-aligned one)
    bool right_aligned{false};
    bool new_row{false};          // starts a row of its own (left-aligned buttons)
    bool skip{false};             // stands aside in this layout: no place is made for it
};
struct LayoutBox {int x{},y{},width{},height{kLayoutButtonHeight},row{};};
struct FlowResult {
    std::vector<LayoutBox> boxes;   // in the order of the list that went in
    int rows{};                     // rows used (left-aligned rows, then right-aligned rows)
    int right_rows{};               // how many of those are right-aligned
    int right_left_edge{};          // smallest x of the right-aligned buttons (0 when there are none)
};

// area_left..area_right is the room of a row. A button wider than the room stands alone in its row. y of a box is
// top + row * pitch.
inline FlowResult FlowButtons(const std::vector<LayoutButton>& items,int area_left,int area_right,int top,int pitch){
    FlowResult result;result.boxes.assign(items.size(),LayoutBox{});
    int row=-1,cursor=area_left;bool first=true;
    for(std::size_t i=0;i<items.size();++i){
        if(items[i].right_aligned || items[i].skip)continue;
        int x=first?area_left:cursor+items[i].gap_before;
        if(row<0 || (!first && (items[i].new_row || x+items[i].width>area_right))){++row;x=area_left;}
        first=false;
        result.boxes[i]={x,top+row*pitch,items[i].width,kLayoutButtonHeight,row};cursor=x+items[i].width;
    }
    const int left_rows=row+1;
    row=left_rows-1;first=true;int edge=area_right;int smallest=0;bool any=false;
    for(std::size_t i=0;i<items.size();++i){
        if(!items[i].right_aligned || items[i].skip)continue;
        int x=first?area_right-items[i].width:edge-items[i].gap_before-items[i].width;
        if(row<left_rows || (!first && x<area_left)){++row;x=area_right-items[i].width;}
        first=false;
        result.boxes[i]={x,top+row*pitch,items[i].width,kLayoutButtonHeight,row};edge=x;
        smallest=any?std::min(smallest,x):x;any=true;
    }
    result.rows=row+1;result.right_rows=any?result.rows-left_rows:0;result.right_left_edge=any?smallest:0;
    return result;
}

// Buttons of one Live pane: the three focus buttons, then the shot button.
inline constexpr int kPaneButtonCount=4;
inline std::vector<LayoutButton> PaneButtonItems(){
    return {{115},{115},{115},{145}};
}
// Buttons under the middle of the client area, in this order. The first five move C, the last three stand at the right (a condition v3 also has
// the hand buttons, listed after them: PreviewHandButton).
enum PreviewLowerButton {
    kLowerMerge0=0,kLowerMerge1,kLowerMerge2,kLowerMerge3,kLowerMerge4,
    kLowerPair,kLowerReview,kLowerSeam,kLowerInspect,kLowerSession,kLowerSettings,kLowerButtonCount
};
// The hand buttons of a condition v3 (#437 stage 2) stand after the others, on a row of their own: along the seam (left, right), across it (up,
// down), about the optical axis (counter-clockwise, clockwise) and the switch of the step. Then the four position buttons of the uncalibrated C
// stand aside (the button of the automatic estimate stays).
enum PreviewHandButton {
    kPreviewHandLeft=0,kPreviewHandRight,kPreviewHandUp,kPreviewHandDown,kPreviewHandCounterClockwise,kPreviewHandClockwise,kPreviewHandStep,kPreviewHandButtonCount
};
inline constexpr int kLowerHandFirst=kLowerButtonCount;   // index of the first hand button in the plan's list when the hand buttons are in it
inline std::vector<LayoutButton> LowerButtonItems(bool hand_controls=false){
    std::vector<LayoutButton> items{{135},{135},{135},{135},{135}};
    items.push_back({235,28});                // pair shot
    items.push_back({225,15});                // saved result
    items.push_back({140,15});                // seam line
    items.push_back({200,15,true});           // inspect (rightmost)
    items.push_back({200,15,true});           // session
    items.push_back({200,15,true});           // settings
    if(hand_controls){
        for(int i=0;i<4;++i)items[static_cast<std::size_t>(kLowerMerge0+i)].skip=true;
        const int widths[kPreviewHandButtonCount]{110,110,100,100,125,125,210};
        for(int i=0;i<kPreviewHandButtonCount;++i){LayoutButton button{widths[i]};button.new_row=i==0;items.push_back(button);}
    }
    return items;
}

// ---- The rows under the title of C ----
// Under the title stand the note (a few words that wrap), for a registered C the state row (it wraps too) and the row of the cameras'
// direction, then the picture. The room of the note and of the state row is what their text needs at the width of the window, never less
// than the room they had when they were drawn in fixed boxes: a note of three lines used to be cut at the second line and a state row longer
// than the window used to be cut with dots. Pure arithmetic; the heights of the texts come from the caller (it measures them with the font
// it draws with). All y are from the middle of the client area, lower_shift not included.
inline constexpr int kLowerNoteTop=110;
inline constexpr int kLowerNoteMinHeight=28;
inline constexpr int kLowerStateMinHeight=21;
inline constexpr int kLowerOrientationHeight=21;
inline constexpr int kLowerRowGap=2;
inline constexpr int kLowerPictureTop=kLowerNoteTop+kLowerNoteMinHeight+kLowerRowGap;   // 140: where the picture of a C without rows begins
inline constexpr int kLowerPictureBottomMargin=15;                                       // the picture ends this far above the bottom of the client area
struct LowerRowsPlan {
    int note_top{kLowerNoteTop},note_height{kLowerNoteMinHeight};
    int state_top{0},state_height{0};              // 0 when the C has no state row
    int orientation_top{0},orientation_height{0};
    int picture_top{kLowerPictureTop};
};
// `note_text_height` and `state_text_height`: the height that the wrapped text needs (0 for no text). `registered_rows`: the C of a condition v3
// has the state row and the direction row between the note and the picture.
inline LowerRowsPlan PlanLowerRows(int note_text_height,int state_text_height,bool registered_rows){
    LowerRowsPlan plan;
    plan.note_height=std::max(kLowerNoteMinHeight,note_text_height);
    plan.picture_top=plan.note_top+plan.note_height+kLowerRowGap;
    if(!registered_rows)return plan;
    plan.state_top=plan.picture_top;
    plan.state_height=std::max(kLowerStateMinHeight,state_text_height);
    plan.orientation_top=plan.state_top+plan.state_height+kLowerRowGap;
    plan.orientation_height=kLowerOrientationHeight;
    plan.picture_top=plan.orientation_top+plan.orientation_height+kLowerRowGap;
    return plan;
}

struct PreviewButtonPlan {
    std::array<std::vector<LayoutBox>,2> pane;   // per pane, y from the top of the client area
    std::vector<LayoutBox> lower;                // y from the middle of the client area
    int pane_rows{1},top_shift{0};
    int lower_rows{2},lower_shift{0};
};
inline PreviewButtonPlan PlanPreviewButtons(int client_width,bool hand_controls=false){
    PreviewButtonPlan plan;
    const int pane_width=client_width/2;
    for(int i=0;i<2;++i){
        const auto flow=FlowButtons(PaneButtonItems(),i*pane_width+kLayoutMargin,i*pane_width+pane_width-kLayoutMargin,kPaneButtonsTop,kPaneRowPitch);
        plan.pane[static_cast<std::size_t>(i)]=flow.boxes;plan.pane_rows=std::max(plan.pane_rows,flow.rows);
    }
    plan.top_shift=(plan.pane_rows-1)*kPaneRowPitch;
    const auto lower=FlowButtons(LowerButtonItems(hand_controls),kLayoutMargin,client_width-kLayoutRightMargin,kLowerButtonsTop,kLowerRowPitch);
    plan.lower=lower.boxes;plan.lower_rows=lower.rows;
    const bool shares_last_row=lower.rows==2 && lower.right_rows==1 && lower.right_left_edge>=kLowerTitleReserve;
    const int title_row=shares_last_row?kLowerSharedTitleRow:kLowerButtonsTop+kLowerRowPitch*lower.rows;
    plan.lower_shift=title_row-kLowerTitleRow;
    return plan;
}

}  // namespace a0::phase0::experimental
