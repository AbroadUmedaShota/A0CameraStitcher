// Window side of the Live-open capture probe (#426 stage 4, ADR-0044 stage P). Included from
// wpd_dual_preview_ui.hpp inside the anonymous namespace, and only when A0_ENABLE_WPD_LIVE_CAPTURE_PROBE is
// defined. A build without that option contains none of this.
//
// One button, one press per process. The press asks both Live workers to hold at a frame boundary; when both
// have, the probe thread borrows the named camera's session for one transaction; afterwards the window lets the
// named camera read at most one frame, then stops both Live sessions through the same path as "ライブ停止". No
// command line, file, environment variable or timer can press the button: the only synthetic click is in the
// camera-free smoke at the end of this file.
bool CanFocus(Camera& camera);
inline constexpr int kLiveProbeButtonId=450;
enum class LiveProbePhase {Idle,Holding,Running,AfterFrame,Stopping,Done};
bool live_probe_used=false; // set at the press and never cleared: one press for the life of the process
LiveProbePhase live_probe_phase=LiveProbePhase::Idle;
HWND live_probe_button{};
Clock::time_point live_probe_pressed{},live_probe_deadline{},live_probe_hold_started{},live_probe_frame_started{};
using LiveProbeBackend=std::function<LiveProbeThreadOutcome(Camera&,Clock::time_point)>;
// Replaced only by --live-capture-probe-ui-smoke, which has no camera.
LiveProbeBackend live_probe_backend=RunLiveProbeOnCamera;
std::future<LiveProbeThreadOutcome> live_probe_future;
std::optional<WpdLiveProbeOutcome> live_probe_outcome;
bool live_probe_lent=false;
// What happened to the one frame after the shot: ok, not_live_view, failed (the read itself failed), timeout and
// interrupted (nothing was read before the wait ran out or the window closed) or skipped (not asked for; the reason
// is in live_probe_frame_skip_reason).
std::string live_probe_frame_result="skipped",live_probe_frame_skip_reason="not_asked";
// While a probe is in progress no AF, fine drive, AF-frame move, capture, pair capture or settings read starts
// (CanFocus is false for both cameras), and the Live restart is not offered.
bool LiveProbeBusy(){return live_probe_phase!=LiveProbePhase::Idle && live_probe_phase!=LiveProbePhase::Done;}
bool LiveProbeHoldsWindow(){return live_probe_future.valid() || live_probe_phase==LiveProbePhase::Holding || live_probe_phase==LiveProbePhase::Running || live_probe_phase==LiveProbePhase::AfterFrame;}
const wchar_t* LiveProbeButtonLabel(){return LiveCaptureProbeCameraIndex()==0?L"探り: Aをライブのまま1回撮る":L"探り: Bをライブのまま1回撮る";}
// Every condition is checked here, and again inside RequestLiveProbe; the button state is not trusted.
// A run that opens the cameras by itself (--seconds N>0, or any smoke or capture flag) is an automatic run: the
// probe's button is never available in it, because a scripted restart or capture could press or follow it.
bool LiveProbeAutomaticRun(){
    return run_seconds!=0 || capture_once_alias.has_value() || focus_smoke || capture_smoke || merge_smoke || pair_smoke || level_smoke || restart_smoke ||
        pair_cycle_smoke || af_prepare_smoke || camera_end_restart_smoke;
}
bool LiveProbeCanPress(){
    if(LiveProbeAutomaticRun())return false;
    if(live_probe_used || cleanup_only || camera_end_restart_smoke || capture_restart_blocked || closing || capture_stop_requested || stopping)return false;
    if(LiveProbeBusy() || preview_joined || !cameras[0] || !cameras[1])return false;
    // Both Live views fresh (frames within 2 s), both focus mailboxes free, no capture, settings read, close or
    // failed-capture stop in progress, and both sessions owned by their Live workers.
    if(!CanFocus(*cameras[0]) || !CanFocus(*cameras[1]))return false;
    Camera& target=*cameras[LiveCaptureProbeCameraIndex()];
    std::lock_guard lock(target.mutex);
    if(target.first==Clock::time_point{} || Clock::now()-target.first>=kLiveCaptureProbeLiveAgeMax)return false; // Live older than the limit
    return target.focus_mode==0 || target.focus_mode==2; // AF-S or AF-F; manual focus is never probed
}
long long LiveProbeElapsedMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-live_probe_pressed).count();}
void LiveProbeReleaseBoth(){for(auto& camera:cameras)if(camera)camera->owner.Release();}
std::wstring LiveProbeStoppedText(const std::string& reason){return L"探りは撮影せずに止まりました（"+WidenUtf8(reason)+L"）。シャッターは送っていません。もう撮らないでください。";}
// The two ways out before any shutter: both holds are given back and Live runs on.
void LiveProbeEndWithoutShutter(const std::string& reason){
    LiveProbeReleaseBoth();
    Log(DescribeLiveCaptureProbeResult(WpdLiveProbeResult::StoppedBeforeShutter,false,LiveProbeElapsedMs(),reason));
    capture_status=LiveProbeStoppedText(reason);
    live_probe_phase=LiveProbePhase::Done;
}
void RequestLiveProbe(){
    if(!LiveProbeCanPress()){Log("ui_live_capture_probe_refused=1 shutter_commands=0");return;}
    live_probe_used=true;
    Camera& target=*cameras[LiveCaptureProbeCameraIndex()];
    live_probe_pressed=Clock::now();live_probe_deadline=live_probe_pressed+kLiveCaptureProbeDeadline;
    {
        std::lock_guard lock(target.mutex);
        std::ostringstream line;
        line<<target.alias<<" live_capture_probe_begin=1 live_age_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(live_probe_pressed-target.first).count()
            <<" focus_mode=0x"<<std::hex<<target.focus_mode<<std::dec<<" deadline_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(kLiveCaptureProbeDeadline).count();
        Log(line.str());
    }
    live_probe_outcome.reset();live_probe_lent=false;live_probe_frame_result="skipped";live_probe_frame_skip_reason="not_asked";review_visible=false;
    const bool first=cameras[0]->owner.RequestHold(),second=cameras[1]->owner.RequestHold();
    if(!first || !second){LiveProbeEndWithoutShutter("hold_not_requested");return;}
    live_probe_hold_started=Clock::now();live_probe_phase=LiveProbePhase::Holding;
    capture_status=L"探り: 両方のライブを止めています…（本体の設定は変えません。ボタンは押さないでください）";
}
// Both workers answered. The probe thread borrows the named camera's session; the other stays held with no I/O.
void LiveProbeStartRun(){
    const int target=LiveCaptureProbeCameraIndex();
    const auto waited=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-live_probe_hold_started).count();
    for(int i=0;i<2;++i)Log(cameras[i]->alias+" live_capture_probe_hold acknowledged=1 waited_ms="+std::to_string(waited)+(i==target?" lent=1":""));
    Log(LiveProbeAfLine(*cameras[target],"before"));
    live_probe_future=std::async(std::launch::async,[backend=live_probe_backend,deadline=live_probe_deadline,target]()->LiveProbeThreadOutcome{
        try{return backend(*cameras[target],deadline);}
        catch(const std::exception&){LiveProbeThreadOutcome failed;failed.outcome.stop_reason="probe_exception";Log("live_capture_probe_exception=1");return failed;}
    });
    live_probe_phase=LiveProbePhase::Running;
    capture_status=L"探り: ライブを開いたまま1回だけ撮っています（本体の設定は変えません。ボタンは押さないでください）…";
}
// The frame result is logged, both Live sessions are stopped like "ライブ停止" (no Start follows), and the
// held workers are released so that they can run their normal Stop and Close.
void LiveProbeStartStop(){
    Camera& target=*cameras[LiveCaptureProbeCameraIndex()];
    {
        std::ostringstream line;
        line<<target.alias<<" live_capture_probe_after_frame result="<<live_probe_frame_result;
        if(live_probe_frame_result=="not_live_view" || live_probe_frame_result=="failed")line<<" mtp=0x"<<std::hex<<target.probe_frame_mtp.load();
        if(live_probe_frame_result=="skipped")line<<" reason="<<live_probe_frame_skip_reason;
        Log(line.str());
    }
    if(live_probe_frame_result=="ok")Log(LiveProbeAfLine(target,"after"));
    else Log(target.alias+" live_capture_probe_af_frame phase=after skipped=1 reason=no_frame");
    stopping=true;
    Log("live_capture_probe_stop=1 start_commands=0");
    LiveProbeReleaseBoth();
    capture_status=L"探りが済みました。両方のライブを停止・切断しています…";
    live_probe_phase=LiveProbePhase::Stopping;
}
std::wstring LiveProbeFinalText(){
    std::wstring text=L"探りが済みました。もう撮らないでください。";
    if(!live_probe_outcome)return text;
    const auto& o=*live_probe_outcome;
    if(o.published=="original")text+=L" 画像は探りの保存先に保存しました。ライブ停止後に開いて、撮る前にわざとぼかしたピントが残っているか見てください（残っていれば撮影の時にAFは走っていません）。";
    else if(o.published=="partial_only")text+=L" 画像は検証できず、非公開の一時ファイルにだけ残しました。";
    else if(o.shutter_sent)text+=L" 画像は取り込めていません。";
    else text+=L" シャッターは送っていません。";
    if(o.shutter_sent && !o.sequence_clean)text+=L" 想定外の事が起きたため、撮影後のライブの状態の読み取りと、ライブの1枚読みは送っていません（原因: "+WidenUtf8(o.anomaly)+L"）。";
    if(o.sdram_may_remain)text+=L" 本体に画像が残っている可能性があります。「ライブ再開」が拒まれたら、本体の電源を入れ直してください。";
    text+=L" 結果: "+WidenUtf8(WpdLiveProbeResultText(o.result));
    return text;
}
// UI thread, once per timer tick (from AdvanceCapture).
void AdvanceLiveProbe(){
    const int target=LiveCaptureProbeCameraIndex();
    switch(live_probe_phase){
    case LiveProbePhase::Holding:{
        if(closing || capture_stop_requested){LiveProbeEndWithoutShutter("window_closing");break;}
        const bool both=cameras[0]->owner.State()==PreviewSessionState::Held && cameras[1]->owner.State()==PreviewSessionState::Held;
        if(both)LiveProbeStartRun();
        else if(Clock::now()-live_probe_hold_started>=kLiveCaptureProbeHoldWait)LiveProbeEndWithoutShutter("hold_timeout");
        break;}
    case LiveProbePhase::Running:{
        if(!live_probe_future.valid() || live_probe_future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)break;
        LiveProbeThreadOutcome got;
        try{got=live_probe_future.get();}catch(const std::exception&){got.outcome.stop_reason="probe_exception";}
        live_probe_outcome=got.outcome;live_probe_lent=got.lent;
        Camera& camera=*cameras[target];
        // One frame through the unchanged Live path, only when the whole sequence stayed clean, Live View still reads
        // on and the 60-second deadline has not run out. The worker holds again right after it, so exactly one frame
        // is read. After anything outside the expected sequence no frame is asked for.
        const bool in_time=Clock::now()<live_probe_deadline;
        const bool want_frame=got.lent && got.outcome.sequence_clean && got.outcome.live_after==1u && in_time &&
            !closing && !capture_stop_requested && !stopping && !camera.failed && !camera.done;
        if(want_frame){
            camera.probe_frame_state=1;live_probe_frame_started=Clock::now();
            camera.owner.Release();
            live_probe_phase=LiveProbePhase::AfterFrame;
            capture_status=L"探り: 撮影の後のライブを1枚だけ確かめています…";
        }else{
            live_probe_frame_result="skipped";
            live_probe_frame_skip_reason=!got.lent?"not_lent":!got.outcome.sequence_clean?"sequence_not_clean":got.outcome.live_after!=1u?"live_not_on":!in_time?"deadline":"window_closing_or_camera_ended";
            LiveProbeStartStop();
        }
        break;}
    case LiveProbePhase::AfterFrame:{
        Camera& camera=*cameras[target];
        const int state=camera.probe_frame_state.load();
        // The wait never outlasts the 60-second deadline either.
        const bool timed_out=Clock::now()-live_probe_frame_started>=std::chrono::seconds(15) || Clock::now()>=live_probe_deadline;
        const bool interrupted=closing || capture_stop_requested || camera.done;
        // state 2: the frame was read and the worker is asking to hold again; wait until it is held (or it ended).
        const bool settled=state==2 && (camera.owner.State()==PreviewSessionState::Held || camera.done);
        if(state==3 || state==4 || interrupted || timed_out || settled){
            // A stop before the read is not a failed read: the camera never answered "failed" in that case.
            live_probe_frame_result=state==2?"ok":state==3?"not_live_view":state==4?"failed":interrupted?"interrupted":"timeout";
            LiveProbeStartStop();
        }
        break;}
    case LiveProbePhase::Stopping:
        // The existing shutdown has run: both workers are joined and closed.
        if(preview_joined){
            const auto& o=live_probe_outcome;
            if(o)Log(DescribeLiveCaptureProbeResult(*o,LiveProbeElapsedMs(),live_probe_frame_result));
            else Log(DescribeLiveCaptureProbeResult(WpdLiveProbeResult::StoppedBeforeShutter,false,LiveProbeElapsedMs(),std::string("probe_exception")));
            capture_status=LiveProbeFinalText();
            live_probe_phase=LiveProbePhase::Done;
        }
        break;
    default:break;
    }
}
// Before the workers are joined at exit: nothing may keep a session held or lent, or a worker would wait forever.
void LiveProbeShutdown(){
    if(live_probe_future.valid()){try{live_probe_future.get();}catch(...){}}
    for(auto& camera:cameras)if(camera)camera->owner.Release();
}
