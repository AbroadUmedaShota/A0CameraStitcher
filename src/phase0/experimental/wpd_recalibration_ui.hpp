// Recalibration of camera B from one pair: the entrance, the guide, the progress, the result and the decision of the development preview
// (#434; docs/design/recalibration-from-pair.md 2.2, 2.3 and 2.9). Included by wpd_dual_preview_ui.hpp, inside the anonymous namespace, only in
// a build with A0_BUILD_WPD_RECALIBRATION (A0_ENABLE_WPD_RECALIBRATION). The computation, the sample and the writing are
// recalibration_job.hpp; this file is the window around them.
//
// The flow (phases):
//   idle       the entrance button "校正をやり直す" (offered while Live runs and the pair capture is offered).
//   guide      Live keeps running; the C area holds the guide (when to use it, how to move the cameras, which paper) and two buttons.
//   capturing  the pair capture of the preview (RequestCapture(2): the same admission, the same 180 seconds, the same failure handling);
//              the originals are saved and no composite is made.
//   computing  Live stays stopped (no session is opened). The job runs on its own thread; the screen names the stage and the seconds.
//   result     the verdict, the reasons, the numbers, the sample; the buttons depend on the verdict:
//                usable             "この校正を使う" / "撮り直す" / "やめる（前の校正のまま）"
//                usable_unverified  the warning first; "未確認のまま使う" / "撮り直す" / "やめる（前の校正のまま）"
//                unusable           nothing can be saved; "撮り直す" / "やめる（前の校正のまま）"
// Every decision ends with Live started again (a new session reads the condition again) or, for "撮り直す", with the guide.
//
// What this file never does: touch a camera, the shutter or the autofocus (the pair capture is the preview's own), open a Live session while
// the job runs, write into the settings folder (only ConfirmRecalibration does, from here, when the person has decided), or offer a way back
// to the earlier calibration (a revision with another profile cannot be rolled back to; the screen says so before the decision).
//
// Threads: every state below is read and written on the UI thread, except the stage (a mutex) and the capture record (written by the capture
// worker before its future is read).
// The headers it needs (recalibration_job.hpp, protocol_json.hpp, <ctime>, <stop_token>) are included by wpd_dual_preview_main.cpp.

inline constexpr int kRecalEnterId=460;
inline constexpr int kRecalFlowFirstId=461;
inline constexpr int kRecalFlowCount=7;
// The flow buttons by index (id = kRecalFlowFirstId + index).
inline constexpr int kRecalStart=0,kRecalGuideQuit=1,kRecalAbort=2,kRecalUse=3,kRecalUseUnchecked=4,kRecalRetake=5,kRecalQuit=6;
inline constexpr std::array<int,kRecalFlowCount> kRecalFlowWidths{190,110,110,150,190,110,260};
inline constexpr int kRecalRowTop=124,kRecalRowPitch=34,kRecalButtonHeight=28;
inline constexpr const wchar_t* kRecalEnterLabel=L"校正をやり直す";
inline constexpr std::array<const wchar_t*,kRecalFlowCount> kRecalFlowLabels{
    L"撮影して校正する",L"やめる",L"中止",L"この校正を使う",L"未確認のまま使う",L"撮り直す",L"やめる（前の校正のまま）"};

enum class RecalPhase {idle,guide,capturing,computing,result};
enum class RecalAction {none,use,use_unchecked,retake,quit,abort};

// Written by the capture worker, read after its future.
struct RecalibrationCaptureRecord {
    std::filesystem::path run_root;
    std::array<std::filesystem::path,2> originals;
    std::array<std::string,2> sha;
    std::string measured_at_utc;
    std::string pair_id;                  // the run id of the capture, as the pair review records it
};

struct RecalibrationUi {
    RecalPhase phase{RecalPhase::idle};
    RecalAction action{RecalAction::none};
    std::shared_ptr<RecalibrationCaptureRecord> capture;
    bool abort_requested{};
    std::mutex stage_mutex;
    std::optional<a0::phase0::experimental::RecalibrationStage> stage;
    double stage_at{};
    Clock::time_point job_began{};
    std::optional<a0::phase0::experimental::RecalibrationJobOutcome> outcome;
    Pixels thumbnail,crops;               // the display copies of the sample, with the seam marks drawn in
    std::wstring note;                    // one line under the buttons: why a button did nothing
    int button_rows{1};
    std::string shown_signature;
    // The calibration that was confirmed as unchecked, in this run of the program; and the answer for the revision in force.
    std::string confirmed_unchecked_sha,scanned_sha;
    bool scanned_unchecked{};
    // Replaced only by --recalibration-ui-smoke: a computation that returns at once, and a runner of the job.
    a0::phase0::experimental::RecalibrationComputation computation;
    std::function<a0::phase0::experimental::RecalibrationJobOutcome(const a0::phase0::experimental::RecalibrationJobRequest&)> runner;
    unsigned jobs_started{},confirmations_written{};
    // Last, so that it is destroyed first: a job that is still running never outlives the members it reads.
    std::stop_source stop;
    std::future<a0::phase0::experimental::RecalibrationJobOutcome> job;
};
RecalibrationUi recal_ui;
std::array<HWND,kRecalFlowCount> recal_flow_buttons{};

// ---- Where things are ----
std::filesystem::path RecalibrationSettingsFolder(){
    return recalibration_settings_override?*recalibration_settings_override:DefaultIdentityMapPath().parent_path();
}
std::filesystem::path RecalibrationReviewRoot(){
    return pair_review_root_override?*pair_review_root_override:DefaultIdentityMapPath().parent_path().parent_path()/"wpd-pair-reviews";
}
// Live is stopped and the screen holds the cameras' place: no AF, no capture, no settings read, no restart by hand.
bool RecalibrationHoldsLive(){
    return recal_ui.phase==RecalPhase::capturing || recal_ui.phase==RecalPhase::computing || recal_ui.phase==RecalPhase::result;
}
// A job that is running (or finished and not yet taken) keeps the window open until it ends.
bool RecalibrationHoldsWindow(){return recal_ui.job.valid();}

// ---- Words of this window (the sentences of the result are recalibration_text.hpp) ----
std::wstring RecalibrationMinutesSeconds(double seconds){
    const long total=static_cast<long>(std::max(0.0,seconds));
    return total>=60?std::to_wstring(total/60)+L" 分 "+std::to_wstring(total%60)+L" 秒":std::to_wstring(total)+L" 秒";
}
std::string RecalibrationUtcNow(){
    const auto now=std::time(nullptr);std::tm parts{};gmtime_s(&parts,&now);
    char text[32]{};std::strftime(text,sizeof text,"%Y-%m-%dT%H:%M:%SZ",&parts);return text;
}

// ---- The mark of an unchecked calibration ----
// The condition's format has no place for it (it was not changed). Within the run that confirmed it, the program knows. After a restart the
// record of the confirmation in the run folder (confirmation.json, written by ConfirmRecalibration) is read, when it is there and names the
// revision in force by its SHA-256; when it is not (or cannot be read) nothing is shown and the log says so. A later revision made on top
// of an unchecked one (a hand adjustment) has another SHA-256 and is not marked: the screen cannot know.
struct RecalibrationJsonFailure {[[noreturn]] static void Fail(std::string_view code,std::string_view){throw std::invalid_argument("recalibration record "+std::string(code));}};
// The newest runs first, at most 200 of them; a file larger than 4 KiB is not read.
std::optional<bool> RecalibrationRecordedAsUnchecked(const std::filesystem::path& review_root,const std::string& revision_sha256){
    namespace fs=std::filesystem;
    namespace json=a0::common::protocol_json;
    std::error_code ec;
    if(revision_sha256.size()!=64 || !fs::is_directory(review_root,ec))return std::nullopt;
    std::vector<fs::path> runs;
    for(fs::directory_iterator it(review_root,ec),end;!ec && it!=end;it.increment(ec)){
        std::error_code entry_ec;
        if(it->is_directory(entry_ec) && !entry_ec)runs.push_back(it->path());
    }
    std::sort(runs.begin(),runs.end(),[](const fs::path& a,const fs::path& b){return a.filename().native()>b.filename().native();});
    if(runs.size()>200)runs.resize(200);
    for(const auto& run:runs){
        const auto file=run/"recalibration"/"confirmation.json";
        std::error_code status_ec;
        const auto status=fs::symlink_status(file,status_ec);
        if(status_ec || status.type()!=fs::file_type::regular)continue;
        const auto size=fs::file_size(file,status_ec);
        if(status_ec || size==0 || size>4096)continue;
        std::string text(static_cast<size_t>(size),'\0');
        {std::ifstream in(file,std::ios::binary);if(!in.read(text.data(),static_cast<std::streamsize>(text.size())))continue;}
        try{
            const auto tree=json::BasicJsonParser<RecalibrationJsonFailure>(text).Parse();
            if(tree.kind!=json::JsonKind::object)continue;
            const auto schema=tree.object.find("schema"),revision=tree.object.find("revisionSha256"),unverified=tree.object.find("unverified");
            if(schema==tree.object.end() || revision==tree.object.end() || unverified==tree.object.end())continue;
            if(schema->second.kind!=json::JsonKind::string || schema->second.string!="a0.pair-recalibration-confirmation.v1")continue;
            if(revision->second.kind!=json::JsonKind::string || revision->second.string!=revision_sha256)continue;
            if(unverified->second.kind!=json::JsonKind::boolean)continue;
            return unverified->second.boolean;
        }catch(const std::exception&){continue;}
    }
    return std::nullopt;
}
// The one place that says whether the revision with this SHA-256 is an unchecked calibration: the run that confirmed it knows; after a
// restart the record of the run folder is read. Nothing else on the screen asks anyone else. A later version reads a small record in the
// settings folder instead (named by the first 16 hex digits of the revision's SHA-256); only this function changes then.
// Empty: not known (nothing is shown).
std::optional<bool> RecalibrationUncheckedMarkOf(const std::string& revision_sha256){
    if(revision_sha256==recal_ui.confirmed_unchecked_sha){Log("recalibration_unchecked_mark source=this-run unchecked=1");return true;}
    const auto found=RecalibrationRecordedAsUnchecked(RecalibrationReviewRoot(),revision_sha256);
    if(found)Log(std::string("recalibration_unchecked_mark source=record unchecked=")+(*found?"1":"0"));
    else Log("recalibration_unchecked_mark source=none reason=no-record-found");
    return found;
}
// Called where the condition is read (every session start): asks once per revision.
void RecalibrationRefreshUncheckedMark(){
    auto& r=recal_ui;
    if(!fixed_source.loaded_v3){r.scanned_sha.clear();r.scanned_unchecked=false;return;}
    const auto& sha=fixed_source.loaded_v3->file_sha256;
    if(sha==r.scanned_sha)return;
    r.scanned_sha=sha;
    r.scanned_unchecked=RecalibrationUncheckedMarkOf(sha).value_or(false);
}
bool RecalibrationUncheckedInForce(){
    return fixed_source.loaded_v3 && recal_ui.scanned_unchecked && recal_ui.scanned_sha==fixed_source.loaded_v3->file_sha256;
}
std::wstring RecalibrationTitleNote(){
    return RecalibrationUncheckedInForce()?std::wstring(L"・")+kRecalUncheckedMark:std::wstring{};
}

// ---- A hint on the status row when the cameras seem to have moved a lot (2.9) ----
// Only a build that has the button says it, and only after a pair capture whose composite or whose gate points at the placement.
std::wstring RecalibrationNudgeLine(const CaptureUiOutcome& result){
    namespace m2e=a0::m2::experimental;
    a0::phase0::experimental::RecalibrationNudgeFacts facts;
    facts.seam_outside_overlap=result.composite_skip && *result.composite_skip==m2e::FixedCompositeSkip::seam_outside_overlap;
    if(result.review && result.review->fixed_composite_registration){
        const auto& record=*result.review->fixed_composite_registration;
        for(const auto& warning:record.warnings)
            if(warning.code==m2e::WarningCode::camera_position && warning.warning_class==m2e::WarningClass::exceeded)facts.position_exceeded=true;
        const auto& capture=record.registration;
        facts.undetermined_at_level_g4=capture.level.level==m2e::GeometryLevel::g4 && capture.registration.state==m2e::content_registration::RegistrationState::undetermined;
        facts.gate_cause_pose=capture.gate.state==m2e::registration_gate::GateState::fail && capture.gate.cause==m2e::registration_gate::GateCause::pose;
    }
    if(!a0::phase0::experimental::RecalibrationNudgeApplies(facts))return {};
    Log("recalibration_ui nudge=1 camera_operations=0");
    return L"\n"+WidenUtf8(a0::phase0::experimental::RecalibrationNudgeText());
}

// ---- The buttons ----
bool RecalibrationButtonShown(HWND button){return button && (GetWindowLongW(button,GWL_STYLE)&WS_VISIBLE)!=0;}
void ShowRecalibrationButton(HWND button,bool shown,bool enabled){
    if(!button)return;
    if(RecalibrationButtonShown(button)!=shown)ShowWindow(button,shown?SW_SHOW:SW_HIDE);
    EnableWindow(button,shown && enabled);
}
// The entrance is offered under the conditions of the pair capture (Live is up and fresh, nothing is pending, no settings read or write).
bool RecalibrationCanOpenGuide(){
    return recal_ui.phase==RecalPhase::idle && !closing && !stopping && !SettingsReadBusy() && !settings_close_failed &&
        CanFocus(*cameras[0]) && CanFocus(*cameras[1]);
}
struct RecalibrationFlowButton {int index;bool enabled;};
// The flow buttons that the phase shows, in the order they stand.
std::vector<RecalibrationFlowButton> RecalibrationFlowButtons(){
    using a0::phase0::experimental::RecalibrationVerdict;
    const auto& r=recal_ui;
    const bool idle_action=r.action==RecalAction::none;
    std::vector<RecalibrationFlowButton> shown;
    switch(r.phase){
    case RecalPhase::guide:
        shown.push_back({kRecalStart,idle_action && fixed_source.loaded_v3.has_value() && CanFocus(*cameras[0]) && CanFocus(*cameras[1])});
        shown.push_back({kRecalGuideQuit,idle_action});
        break;
    case RecalPhase::computing:
        shown.push_back({kRecalAbort,!r.abort_requested});
        break;
    case RecalPhase::result:
        if(r.outcome){
            if(r.outcome->verdict==RecalibrationVerdict::usable)shown.push_back({kRecalUse,idle_action});
            else if(r.outcome->verdict==RecalibrationVerdict::usable_unverified)shown.push_back({kRecalUseUnchecked,idle_action});
            shown.push_back({kRecalRetake,idle_action});
            shown.push_back({kRecalQuit,idle_action});
        }
        break;
    case RecalPhase::idle:
    case RecalPhase::capturing:
        break;
    }
    return shown;
}
// The entrance stands at the right end of the status band of the lower half (the same place as the probe's button, placed by the same
// part, FlowButtons), so that it takes no row from the buttons of the lower half. The flow buttons stand in the area that the lower half
// keeps for C (under the rows of buttons, PreviewLowerShift), in one row, and go to the next row when the window is narrow.
void LayoutRecalibrationButtons(const RECT& rect){
    if(recal_enter_button){
        const auto place=FlowButtons({{kRecalEnterWidth,kLayoutGap,true}},kLayoutMargin,rect.right-kLayoutMargin,kLowerStatusTop,kLowerRowPitch).boxes[0];
        MoveWindow(recal_enter_button,place.x,rect.bottom/2+place.y,place.width,place.height,TRUE);
    }
    const int top=rect.bottom/2+kRecalRowTop+PreviewLowerShift(rect.right);
    int x=12,row=0;
    for(const auto& flow:RecalibrationFlowButtons()){
        const int width=kRecalFlowWidths[flow.index];
        if(x>12 && x+width>rect.right-12){x=12;++row;}
        if(recal_flow_buttons[flow.index])MoveWindow(recal_flow_buttons[flow.index],x,top+row*kRecalRowPitch,width,kRecalButtonHeight,TRUE);
        x+=width+10;
    }
    recal_ui.button_rows=row+1;
}
void CreateRecalibrationControls(HWND window,HINSTANCE instance){
    // No button is the default button (BS_PUSHBUTTON), and the one that uses a calibration that was not checked is not on the tab order: the
    // Enter key reaches only a button that has the focus, and that one gets it only by a click.
    const auto make=[&](const wchar_t* label,int id,bool visible,bool tab_stop){
        HWND button=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|BS_PUSHBUTTON|(tab_stop?WS_TABSTOP:0)|(visible?WS_VISIBLE:0),0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if(!button)throw std::runtime_error("create recalibration button failed");
        return button;
    };
    recal_enter_button=make(kRecalEnterLabel,kRecalEnterId,true,true);EnableWindow(recal_enter_button,FALSE);
    for(int i=0;i<kRecalFlowCount;++i)recal_flow_buttons[i]=make(kRecalFlowLabels[i],kRecalFlowFirstId+i,false,i!=kRecalUseUnchecked);
}
// Every tick: the entrance, and the flow buttons of the phase (shown, enabled); the row is laid out again when the set changes.
void UpdateRecalibrationControls(HWND hwnd){
    ShowRecalibrationButton(recal_enter_button,true,RecalibrationCanOpenGuide());
    const auto wanted=RecalibrationFlowButtons();
    std::string signature;
    for(const auto& flow:wanted)signature+=std::to_string(flow.index)+",";
    const bool changed=signature!=recal_ui.shown_signature;
    for(int i=0;i<kRecalFlowCount;++i){
        bool shown=false,enabled=false;
        for(const auto& flow:wanted)if(flow.index==i){shown=true;enabled=flow.enabled;}
        ShowRecalibrationButton(recal_flow_buttons[i],shown,enabled);
    }
    if(changed){
        recal_ui.shown_signature=signature;
        RECT rect{};GetClientRect(hwnd,&rect);LayoutRecalibrationButtons(rect);
        // The safe button has the focus when a result comes up, so that a key press cannot use a calibration that was not checked.
        if(recal_ui.phase==RecalPhase::result && RecalibrationButtonShown(recal_flow_buttons[kRecalQuit]) && IsChild(hwnd,GetFocus()))
            SetFocus(recal_flow_buttons[kRecalQuit]);
    }
}

// Two sentences of the job are for a caller that made a mistake, not for the person: this window cannot make them (the folder is new every
// time, Live is stopped before the job is asked), and if one ever came back the screen would say that nothing was started.
struct RecalibrationLines {std::vector<std::string> lines;size_t reasons_end{};};
RecalibrationLines RecalibrationDisplayLines(const a0::phase0::experimental::RecalibrationJobOutcome& outcome){
    using namespace a0::phase0::experimental;
    RecalibrationLines result;
    for(const auto& finding:outcome.findings)
        if(finding.code=="staging-exists" || finding.code=="live-not-stopped" || finding.code=="staging-in-settings"){
            result.lines={RecalibrationVerdictLine(RecalibrationVerdict::unusable),"校正を始められませんでした。技術担当へ知らせてください。前の校正のままです。"};
            result.reasons_end=2;return result;
        }
    result.lines=outcome.screen_lines;
    result.reasons_end=std::min(result.lines.size(),1+outcome.findings.size());
    return result;
}
// ---- The screens ----
// Draws `text` wrapped in the width, returns the y under it. Nothing is drawn below `limit`.
int DrawRecalibrationText(HDC dc,const std::wstring& text,int left,int top,int width,int limit,COLORREF color){
    if(text.empty() || top>=limit || width<=20)return top;
    RECT measure{left,top,left+width,top};
    DrawTextW(dc,text.c_str(),-1,&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    RECT area{left,top,left+width,std::min<LONG>(measure.bottom,limit)};
    SetTextColor(dc,color);
    DrawTextW(dc,text.c_str(),-1,&area,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    return static_cast<int>(measure.bottom)+4;
}
inline constexpr COLORREF kRecalTextColor=RGB(240,240,240),kRecalHeadColor=RGB(255,224,160),kRecalOkColor=RGB(120,230,120),
    kRecalWarnColor=RGB(255,200,90),kRecalNgColor=RGB(255,110,110);
// The seam marks on the display copy of the sample: new seam in the pink of the saved results, the earlier one grey and dotted.
void RecalibrationMarkColumn(Pixels& pixels,long column,bool dotted,BYTE blue,BYTE green,BYTE red){
    if(pixels.width==0 || pixels.height==0)return;
    for(long x=column-1;x<=column+1;++x){
        if(x<0 || x>=static_cast<long>(pixels.width))continue;
        for(UINT y=0;y<pixels.height;++y){
            if(dotted && ((y/10)%2)==1)continue;
            auto* pixel=pixels.bytes.data()+(static_cast<size_t>(y)*pixels.width+static_cast<size_t>(x))*4;
            pixel[0]=blue;pixel[1]=green;pixel[2]=red;
        }
    }
}
// The sample and the crops of the outcome as display pictures (done once, when the result arrives).
void PrepareRecalibrationPictures(){
    auto& r=recal_ui;
    r.thumbnail={};r.crops={};
    if(!r.outcome)return;
    const auto& outcome=*r.outcome;
    if(!outcome.thumbnail.bgr.empty()){
        r.thumbnail=ToPixels(outcome.thumbnail);
        const auto& d=outcome.facts.display;
        if(d.has_new && d.width_new_px>0 && outcome.candidate){
            const double to_thumbnail=static_cast<double>(r.thumbnail.width)/static_cast<double>(d.width_new_px);
            const double pitch_mm=25.4/static_cast<double>(outcome.candidate->condition.raster.dpi);
            const double left_mm=d.right_new_mm-static_cast<double>(d.width_new_px)*pitch_mm;
            const double previous_column=(d.seam_x_previous_mm-left_mm)/pitch_mm-0.5;
            const double new_column=static_cast<double>(d.seam_column_new);
            if(std::abs(d.seam_x_new_mm-d.seam_x_previous_mm)>=0.05 && std::isfinite(previous_column) && previous_column>=0 && previous_column<d.width_new_px)
                RecalibrationMarkColumn(r.thumbnail,std::lround(previous_column*to_thumbnail),true,160,160,160);
            RecalibrationMarkColumn(r.thumbnail,std::lround(new_column*to_thumbnail),false,255,0,255);
        }
    }
    if(outcome.seam_crops && !outcome.seam_crops->sheet.bgr.empty())r.crops=ToPixels(outcome.seam_crops->sheet);
}
// Where the picture of the sample and the crops go: the upper half, which Live leaves empty while it is stopped. Side by side when the
// area is low, one above the other when it is tall.
struct RecalibrationPictureLayout {RECT thumbnail{},crops{};int captions_y{};};
RecalibrationPictureLayout LayoutRecalibrationPictures(const RECT& client){
    RecalibrationPictureLayout layout;
    const int top=176+PreviewTopShift(client.right),bottom=client.bottom/2-8,left=12,right=client.right-12;   // under the texts that the stopped panes still draw (the level note ends at y=170, lower by the wrapped rows of buttons)
    layout.captions_y=top;
    const int picture_top=top+24;
    const int height=std::max(0,bottom-picture_top),width=std::max(0,right-left);
    if(height>=560){
        const int thumbnail_height=height*55/100;
        layout.thumbnail={left,picture_top,right,picture_top+thumbnail_height};
        layout.crops={left,picture_top+thumbnail_height+28,right,bottom};
    }else{
        const int split=left+width*40/100;
        layout.thumbnail={left,picture_top,split-8,bottom};
        layout.crops={split+8,picture_top,right,bottom};
    }
    return layout;
}
void DrawRecalibrationPictures(HDC dc,const RECT& client){
    const auto& r=recal_ui;
    const auto layout=LayoutRecalibrationPictures(client);
    if(r.thumbnail.bytes.empty() && r.crops.bytes.empty()){
        // The sample could not be composed (or this result has none): one line, no empty frames.
        const std::wstring none=L"見本は合成できませんでした（この結果には見本の画像がありません）";
        SetTextColor(dc,kRecalHeadColor);TextOutW(dc,12,layout.captions_y,none.c_str(),static_cast<int>(none.size()));SetTextColor(dc,kRecalTextColor);
        return;
    }
    SetTextColor(dc,kRecalHeadColor);
    const std::wstring thumbnail_caption=L"見本（新しい校正で合成した縮小画像。桃色＝新しい継ぎ目、灰色の点線＝前の継ぎ目）";
    const std::wstring crops_caption=L"継ぎ目の切り出し（原寸・左から上／中／下）";
    if(layout.crops.top>layout.thumbnail.top){
        TextOutW(dc,12,layout.captions_y,thumbnail_caption.c_str(),static_cast<int>(thumbnail_caption.size()));
        TextOutW(dc,12,layout.crops.top-24,crops_caption.c_str(),static_cast<int>(crops_caption.size()));
    }else{
        RECT a{layout.thumbnail.left,layout.captions_y,layout.thumbnail.right,layout.captions_y+22};
        DrawTextW(dc,thumbnail_caption.c_str(),-1,&a,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        RECT b{layout.crops.left,layout.captions_y,layout.crops.right,layout.captions_y+22};
        DrawTextW(dc,crops_caption.c_str(),-1,&b,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    SetTextColor(dc,kRecalTextColor);
    if(!r.thumbnail.bytes.empty())DrawPixels(dc,r.thumbnail,layout.thumbnail.left,layout.thumbnail.top,layout.thumbnail.right-layout.thumbnail.left,layout.thumbnail.bottom-layout.thumbnail.top);
    else{const std::wstring none=L"見本は合成できませんでした";TextOutW(dc,layout.thumbnail.left,layout.thumbnail.top+4,none.c_str(),static_cast<int>(none.size()));}
    if(!r.crops.bytes.empty())DrawPixels(dc,r.crops,layout.crops.left,layout.crops.top,layout.crops.right-layout.crops.left,layout.crops.bottom-layout.crops.top);
}
bool DrawRecalibrationScreen(HDC dc,const RECT& client){
    using namespace a0::phase0::experimental;
    const auto& r=recal_ui;
    if(r.phase==RecalPhase::idle)return false;
    const int left=12,width=client.right-24;
    const int limit=client.bottom-6-SettingsButtonRowReserve();
    // The text starts under the row of buttons; while the shot is taken there is no button.
    int y=client.bottom/2+kRecalRowTop+PreviewLowerShift(client.right)+(r.phase==RecalPhase::capturing?0:r.button_rows*kRecalRowPitch)+6;
    const auto line=[&](const std::wstring& text,COLORREF color=kRecalTextColor){y=DrawRecalibrationText(dc,text,left,y,width,limit,color);};
    const auto utf8=[](const std::string& text){return WidenUtf8(text);};
    switch(r.phase){
    case RecalPhase::guide:{
        line(L"校正をやり直す",kRecalHeadColor);
        for(const auto& text:RecalibrationGuideLines())line(utf8(text));
        if(!fixed_source.loaded_v3){
            // A condition that is there and cannot be used (a hand adjustment beyond the range, a damaged chain) is told in the words C uses
            // for it; no condition at all, or an older kind, has the sentence of the job. The shot is not offered in either case.
            if(fixed_source.Present() && !fixed_source.loaded && !fixed_source.loaded_v2)line(FixedPreviewUnavailableText(fixed_source.reason),kRecalNgColor);
            else{
                const auto sentence=RecalibrationFindingSentence({FindingClass::no_candidate,"no-v3-condition",""});
                line(utf8(sentence.reason+"。"+sentence.remedy),kRecalNgColor);
            }
        }
        line(L"撮影の後、計算に数分かかります。その間、ライブは止まります。「撮影して校正する」を押すと、両台のシャッターを 1 回ずつ切ります（「両台撮影」と同じ手順です）。");
        if(!r.note.empty())line(r.note,kRecalNgColor);
        break;
    }
    case RecalPhase::capturing:
        line(L"校正用の撮影中",kRecalHeadColor);
        line(L"両台のシャッターを 1 回ずつ切って、原画像を保存しています（最長 180 秒）。終わるまで、カメラやアプリに触らないでください。");
        break;
    case RecalPhase::computing:{
        line(L"校正を計算しています",kRecalHeadColor);
        line(L"ライブは止めています。終わるまで数分かかります。");
        std::optional<RecalibrationStage> stage;double at=0;
        {std::lock_guard lock(recal_ui.stage_mutex);stage=r.stage;at=r.stage_at;}
        const double total=std::chrono::duration<double>(Clock::now()-r.job_began).count();
        if(stage)line(L"いま: "+utf8(RecalibrationStageName(*stage))+L"（この段は "+RecalibrationMinutesSeconds(total-at)+L" 目）");
        line(L"ここまでの時間: "+RecalibrationMinutesSeconds(total));
        line(r.abort_requested?L"中止しています。今の段が終わるまで待ちます（何も書き換えません）。":L"途中で止める時は「中止」を押してください。何も書き換えません。");
        break;
    }
    case RecalPhase::result:{
        if(!r.outcome)break;
        const auto& outcome=*r.outcome;
        DrawRecalibrationPictures(dc,client);
        const auto display=RecalibrationDisplayLines(outcome);
        const auto& lines=display.lines;
        const size_t reasons_end=display.reasons_end;
        const COLORREF verdict_color=outcome.verdict==RecalibrationVerdict::usable?kRecalOkColor:outcome.verdict==RecalibrationVerdict::usable_unverified?kRecalWarnColor:kRecalNgColor;
        if(!lines.empty())line(utf8(lines[0]),verdict_color);
        for(size_t i=1;i<reasons_end;++i)line(utf8(lines[i]),outcome.verdict==RecalibrationVerdict::usable?kRecalTextColor:kRecalWarnColor);
        if(outcome.verdict==RecalibrationVerdict::unusable)line(L"この校正は保存できません。前の校正のままです。",kRecalNgColor);
        else{
            line(L"確定すると、前の校正へは今はボタンで戻せません（もう一度校正が要ります）。",kRecalHeadColor);
            if(outcome.unverified())line(std::wstring(L"「未確認のまま使う」を押すと、画面の題と C の欄に「")+kRecalUncheckedMark+L"」と出し続けます。",kRecalWarnColor);
        }
        for(size_t i=reasons_end;i<lines.size();++i)line(utf8(lines[i]));
        if(!r.note.empty())line(r.note,kRecalNgColor);
        break;
    }
    case RecalPhase::idle:break;
    }
    SetTextColor(dc,kRecalTextColor);
    return true;
}

// ---- The pair capture for a calibration ----
// Same admission as the pair capture (RequestCapture(2)), same pair_capture_backend and same deadline. After the originals are saved nothing
// is composed: the originals and the record go to the job. A failure is handled by the code of the pair capture (no retry).
CaptureUiOutcome SaveRecalibrationPair(const std::array<std::wstring,2>& ids,Clock::time_point deadline,const std::shared_ptr<RecalibrationCaptureRecord>& record){
    CaptureUiOutcome result;
    try{
        EvidenceWriter evidence(RecalibrationReviewRoot(),NewRunId(),"wpd-recalibration-pair");
        evidence.RecordState("pair","BothLiveViewsClosed");
        auto pair=pair_capture_backend(ids,deadline);
        if(!pair.complete){RecordIncompletePair(result,pair);throw std::runtime_error(pair.error);}
        const auto& a=pair.originals[0];const auto& b=pair.originals[1];
        result.capture={true,true,a.path,a.bytes,a.sha256,{}};result.originals={a.path,b.path};
        record->run_root=evidence.RunRoot();record->originals={a.path,b.path};record->sha={a.sha256,b.sha256};record->measured_at_utc=RecalibrationUtcNow();
        record->pair_id=evidence.RunId();
        try{evidence.RecordState("pair","OriginalsSavedForRecalibration");}catch(const std::exception&){}
        Log("recalibration_capture_saved=1 originals_saved=2 composite=0 settings_written=0");
    }catch(const std::exception& error){
        const auto summary=ErrorSummary(error);
        result.capture.success=false;result.capture.error=summary;
        Log("recalibration_capture_failure: "+summary);
    }
    return result;
}
// The capture the next AdvanceCapture starts is a calibration's when the guide's button asked for it.
std::shared_ptr<RecalibrationCaptureRecord> RecalibrationCaptureFor(int index){
    if(index!=2 || recal_ui.phase!=RecalPhase::capturing || !recal_ui.capture)return nullptr;
    capture_status=L"校正用に、AとBのシャッターを続けて1回ずつ切り、原画像を保存しています…";
    return recal_ui.capture;
}

// ---- The job ----
// An outcome for a run that could not be made at all (a failure of this window's, not of the computation). Nothing is offered.
a0::phase0::experimental::RecalibrationJobOutcome RecalibrationFailedOutcome(const char* code){
    using namespace a0::phase0::experimental;
    RecalibrationJobOutcome outcome;
    outcome.verdict=RecalibrationVerdict::unusable;
    outcome.findings.push_back({FindingClass::rejected,code,""});
    outcome.screen_lines=RecalibrationScreenLines(outcome.verdict,outcome.findings,outcome.facts);
    return outcome;
}
a0::phase0::experimental::RecalibrationJobRequest MakeRecalibrationRequest(const RecalibrationCaptureRecord& record){
    a0::phase0::experimental::RecalibrationJobRequest request;
    request.original_a=record.originals[0];request.original_b=record.originals[1];
    request.sha_a=record.sha[0];request.sha_b=record.sha[1];   // the capture's own hashes: the job checks the files against them
    request.pair_id=record.pair_id;
    request.measured_at_utc=record.measured_at_utc;
    request.settings_folder=RecalibrationSettingsFolder();
    request.staging_folder=record.run_root/"recalibration";
    request.hash=[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);};
    request.live_view_stopped=LiveStoppedAndClosed(); // the job refuses to run otherwise
    request.now=std::chrono::system_clock::now();
    request.computation=recal_ui.computation;
    request.on_stage=[](a0::phase0::experimental::RecalibrationStage stage,double seconds){
        std::lock_guard lock(recal_ui.stage_mutex);recal_ui.stage=stage;recal_ui.stage_at=seconds;
    };
    return request;
}
void StartRecalibrationJob(const std::shared_ptr<RecalibrationCaptureRecord>& record){
    auto& r=recal_ui;
    auto request=MakeRecalibrationRequest(*record);
    r.stop=std::stop_source{};request.stop=r.stop.get_token();
    r.abort_requested=false;r.action=RecalAction::none;r.note.clear();r.outcome.reset();
    {std::lock_guard lock(r.stage_mutex);r.stage.reset();r.stage_at=0;}
    r.job_began=Clock::now();++r.jobs_started;r.phase=RecalPhase::computing;
    capture_status=L"校正を計算しています…（ライブは止めています）";
    Log(std::string("recalibration_ui job_started=1 live_stopped=")+(request.live_view_stopped?"1":"0")+" camera_operations=0 settings_written=0");
    // Everything that can come out of the job is caught on its own thread: the job reports its failures as outcomes, and what it throws for
    // a caller's mistake (no hash provider, a time that is not a time, an empty path) becomes an outcome that offers nothing.
    r.job=std::async(std::launch::async,[request=std::move(request),runner=r.runner]() mutable{
        try{return runner?runner(request):a0::phase0::experimental::RunRecalibrationJob(request);}
        catch(const std::exception& error){
            auto outcome=RecalibrationFailedOutcome("job-failed");
            outcome.log_lines.push_back("recalibration_ui job_exception="+ErrorSummary(error)+" settings_written=0");
            return outcome;
        }
        catch(...){
            auto outcome=RecalibrationFailedOutcome("job-failed");
            outcome.log_lines.push_back("recalibration_ui job_exception=unknown settings_written=0");
            return outcome;
        }
    });
}
// Called from AdvanceCapture when a capture ended. Returns true when it was the calibration's capture and the job was started; false
// leaves the capture to the code of the pair capture (a failure keeps its handling: no retry, no restart).
bool RecalibrationTakeCapture(const CaptureUiOutcome& result){
    auto& r=recal_ui;
    if(r.phase!=RecalPhase::capturing || !r.capture)return false;
    const auto record=std::move(r.capture);r.capture.reset();
    if(!(result.capture.success && result.capture.closed)){
        r.phase=RecalPhase::idle;r.note.clear();
        Log("recalibration_ui capture_failed=1 job_started=0 settings_written=0");
        return false;
    }
    if(closing){r.phase=RecalPhase::idle;return false;}
    review_visible=false;
    if(a0::phase0::experimental::recalibration_job_detail::IsInsideFolder(record->run_root,RecalibrationSettingsFolder())){
        // The record of a run must not stand among the settings (a reader of the revisions would refuse the whole chain). Nothing is run.
        r.outcome=RecalibrationFailedOutcome("job-failed");r.thumbnail={};r.crops={};r.note.clear();r.action=RecalAction::none;
        r.phase=RecalPhase::result;
        capture_status=L"校正を計算できませんでした。前の校正のままです。";
        Log("recalibration_ui staging_inside_settings=1 job_started=0 settings_written=0");
        return true;
    }
    StartRecalibrationJob(record);
    return true;
}
// Called from WM_CLOSE: a job in progress is asked to stop (the window waits for it to end).
void RecalibrationOnClose(){
    if(recal_ui.job.valid())recal_ui.stop.request_stop();
}
void RecalibrationResumeLive(){
    if(closing)return;
    if(preview_restart_override)preview_restart_override();else StartPreview();
}

// ---- The buttons' commands (each checks its conditions again; the button's state is not trusted) ----
void RecalibrationOpenGuide(){
    auto& r=recal_ui;
    if(!RecalibrationCanOpenGuide()){Log("recalibration_ui guide_refused=1");return;}
    review_visible=false;r.phase=RecalPhase::guide;r.note.clear();r.action=RecalAction::none;
    capture_status=L"校正をやり直す: 案内を読んで、紙を置いてから「撮影して校正する」を押してください。";
    Log("recalibration_ui guide_opened=1 camera_operations=0 settings_written=0");
}
void RecalibrationStartCapture(){
    auto& r=recal_ui;
    if(r.phase!=RecalPhase::guide || r.action!=RecalAction::none)return;
    if(!fixed_source.loaded_v3){r.note=L"校正の元になる条件（v3）がありません。";return;}
    if(!(CanFocus(*cameras[0]) && CanFocus(*cameras[1]))){r.note=L"いまは撮影できません。両方のライブの映像が出てから、もう一度押してください。";return;}
    r.note.clear();
    if(capture_pending){r.note=L"いまは撮影できません（ほかの撮影が終わってから、もう一度押してください）。";Log("recalibration_ui capture_refused=1");return;}
    RequestCapture(2);
    if(!capture_pending){r.note=L"いまは撮影できません（Cの映像が新しくなるまで待って、もう一度押してください）。";Log("recalibration_ui capture_refused=1");return;}
    r.capture=std::make_shared<RecalibrationCaptureRecord>();
    r.phase=RecalPhase::capturing;
    capture_status=L"校正用の撮影の準備中：両Liveを停止しています…";
    Log("recalibration_ui capture_requested=1 both_live_stop_required=1");
}
bool HandleRecalibrationCommand(int id,WORD code,HWND control){
    if(code!=BN_CLICKED)return false;
    auto& r=recal_ui;
    if(id==kRecalEnterId && control==recal_enter_button){RecalibrationOpenGuide();return true;}
    if(id<kRecalFlowFirstId || id>=kRecalFlowFirstId+kRecalFlowCount)return false;
    const int index=id-kRecalFlowFirstId;
    if(control!=recal_flow_buttons[index])return false;
    using a0::phase0::experimental::RecalibrationVerdict;
    switch(index){
    case kRecalStart:RecalibrationStartCapture();break;
    case kRecalGuideQuit:
        if(r.phase==RecalPhase::guide){r.phase=RecalPhase::idle;r.note.clear();capture_status=L"校正をやめました。前の校正のままです。";Log("recalibration_ui guide_closed=1");}
        break;
    case kRecalAbort:
        if(r.phase==RecalPhase::computing && !r.abort_requested){r.abort_requested=true;r.stop.request_stop();Log("recalibration_ui abort_requested=1");}
        break;
    case kRecalUse:
        if(r.phase==RecalPhase::result && r.outcome && r.outcome->verdict==RecalibrationVerdict::usable && r.action==RecalAction::none)r.action=RecalAction::use;
        break;
    case kRecalUseUnchecked:
        if(r.phase==RecalPhase::result && r.outcome && r.outcome->verdict==RecalibrationVerdict::usable_unverified && r.action==RecalAction::none)r.action=RecalAction::use_unchecked;
        break;
    case kRecalRetake:
        if(r.phase==RecalPhase::result && r.action==RecalAction::none)r.action=RecalAction::retake;
        break;
    case kRecalQuit:
        if(r.phase==RecalPhase::result && r.action==RecalAction::none)r.action=RecalAction::quit;
        break;
    default:return false;
    }
    return true;
}

// ---- Every tick: the job's end, and the decision ----
void RecalibrationEndOfJob(){
    using namespace a0::phase0::experimental;
    auto& r=recal_ui;
    RecalibrationJobOutcome outcome;
    try{outcome=r.job.get();}
    catch(const std::exception& error){
        // The job reports its failures as outcomes; an exception here is a programming error. Nothing was written.
        outcome=RecalibrationJobOutcome{};
        outcome.findings.push_back({FindingClass::rejected,"job-failed",""});
        outcome.verdict=RecalibrationVerdict::unusable;
        outcome.screen_lines=RecalibrationScreenLines(outcome.verdict,outcome.findings,outcome.facts);
        Log("recalibration_ui job_exception="+ErrorSummary(error)+" settings_written=0");
    }
    for(const auto& text:outcome.log_lines)Log(text);
    if(closing){r.phase=RecalPhase::idle;return;}
    if(r.abort_requested){
        // The person stopped it: whatever the job came to is not shown and nothing is kept.
        r.phase=RecalPhase::idle;r.abort_requested=false;r.outcome.reset();
        Log("recalibration_ui aborted=1 settings_written=0");
        RecalibrationResumeLive();
        capture_status=L"校正を中止しました。何も書き換えていません。前の校正のままです。ライブを再開しました。";
        return;
    }
    r.outcome=std::move(outcome);r.note.clear();
    PrepareRecalibrationPictures();
    r.phase=RecalPhase::result;
    Log(std::string("recalibration_ui result verdict=")+std::string(RecalibrationVerdictCode(r.outcome->verdict))+" offered="+(r.outcome->ready?"1":"0")+
        " camera_operations=0 settings_written=0");
    capture_status=L"校正の結果です。内容を見て、使うか、撮り直すか、やめるかを決めてください。";
}
void RecalibrationDecide(){
    using namespace a0::phase0::experimental;
    auto& r=recal_ui;
    const auto action=r.action;r.action=RecalAction::none;
    if(action==RecalAction::use || action==RecalAction::use_unchecked){
        if(!r.outcome){r.phase=RecalPhase::idle;return;}
        // Only the button for an unchecked calibration, pressed after the warning, carries the acknowledgement.
        RecalibrationConfirmOptions options;options.acknowledge_unverified=action==RecalAction::use_unchecked;
        RecalibrationConfirmResult confirmed;
        bool state_unknown=false;
        // The unchecked mark is remembered from the press on, not from a confirmation that came back: a write that ends in a failed read-back or
        // an exception may still have put this candidate in force, and the mark must then show. It only shows for a revision with this SHA-256.
        if(action==RecalAction::use_unchecked && r.outcome->candidate)r.confirmed_unchecked_sha=r.outcome->candidate->sha256;
        try{
            confirmed=ConfirmRecalibration(*r.outcome,RecalibrationSettingsFolder(),
                [](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);},options);
        }catch(const std::exception& error){
            // Something threw (memory, for one) possibly after the revision was written: what is in force is read again, not assumed.
            state_unknown=true;
            Log("recalibration_ui confirm_exception="+ErrorSummary(error)+" state_read_again=1");
        }
        for(const auto& text:confirmed.log_lines)Log(text);
        std::wstring message=state_unknown?std::wstring(L"校正の更新の途中で失敗しました。技術担当へ知らせてください。"):WidenUtf8(confirmed.message);
        const bool written=confirmed.written && !state_unknown;
        if(written){
            ++r.confirmations_written;
            r.confirmed_unchecked_sha=confirmed.unverified?confirmed.revision_sha256:std::string{};
        }
        r.outcome.reset();r.thumbnail={};r.crops={};r.phase=RecalPhase::idle;
        RecalibrationResumeLive();   // a new session reads the condition again: the screen then shows what is really in force
        message+=L" ライブを再開しました。";
        if(!written){
            // Not written, or not known to be: say which revision the session read.
            message+=fixed_source.loaded_v3?L"いま有効な校正は改訂 "+std::to_wstring(fixed_source.loaded_v3->condition.revision.sequence)+L" です。":
                std::wstring(L"いま有効な校正を読めませんでした。技術担当へ知らせてください。");
        }
        capture_status=message;
        return;
    }
    if(action==RecalAction::retake){
        r.outcome.reset();r.thumbnail={};r.crops={};r.phase=RecalPhase::guide;r.note.clear();
        RecalibrationResumeLive();
        capture_status=L"校正をやり直す: 紙を替えるか置き直して、もう一度「撮影して校正する」を押してください。";
        Log("recalibration_ui retake=1");
        return;
    }
    if(action==RecalAction::quit){
        r.outcome.reset();r.thumbnail={};r.crops={};r.phase=RecalPhase::idle;
        RecalibrationResumeLive();
        capture_status=L"校正をやめました。何も書き換えていません。前の校正のままです。ライブを再開しました。";
        Log("recalibration_ui quit=1 settings_written=0");
    }
}
void AdvanceRecalibration(){
    auto& r=recal_ui;
    if(r.job.valid() && r.job.wait_for(std::chrono::seconds(0))==std::future_status::ready){RecalibrationEndOfJob();return;}
    if(r.phase==RecalPhase::capturing && preview_joined && !capture_pending && !capture_future.valid() && !closing){
        // The capture was not started (a settings write held it, or the window was closing): the calibration did not begin.
        r.phase=RecalPhase::idle;r.capture.reset();
        Log("recalibration_ui capture_not_started=1 settings_written=0");
        return;
    }
    if(r.phase==RecalPhase::result && r.action!=RecalAction::none && !closing)RecalibrationDecide();
}
