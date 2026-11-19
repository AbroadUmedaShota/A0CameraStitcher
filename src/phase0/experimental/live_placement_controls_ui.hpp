// The hand buttons of the live C and what the window does with them (#437 stage 2). Included by wpd_dual_preview_ui.hpp after the state it
// reads (merged, live_runtime, fixed_source), at file scope of the same unit; the pure parts (the counts of a press, the sentences) are in
// live_registration_display.hpp and the machine behind them is live_placement.hpp.
//
// Buttons: along the seam (left, right), across it (up, down), about the optical axis (counter-clockwise, clockwise) and a switch between the
// coarse step (12 px, 0.2 degrees) and the fine one (4 px, 0.05 degrees). They exist only for a condition v3 (a live runtime); then the four
// position buttons of the uncalibrated C stand aside and the button of the automatic estimate is "自動調整".
//
// A press never runs the check on the window's thread: the check of a full-size position scans the full-size output and takes a moment. One check
// runs at a time on a thread of its own, holding no lock. A press that comes while it runs is not dropped: its counts are added to the
// ones that wait, and when the check ends, the sum is checked once (so a burst of presses moves by as many steps as were pressed and costs one
// check more). A check that refuses takes what waited with it, and the position stays as it was. A capture that is requested while a check
// runs does not wait for it: it takes the position that is drawn, and what waited is dropped (the machine refuses the check's result).
inline constexpr int kHandButtonFirstId=320;   // 320 left, 321 right, 322 up, 323 down, 324 counter-clockwise, 325 clockwise, 326 the step switch
inline constexpr int kHandStepButtonId=kHandButtonFirstId+6;
std::array<HWND,kPreviewHandButtonCount> hand_buttons{};
bool hand_fine=false;   // the step switch: false is the coarse step
// What the last press or capture said that the state row keeps saying until something replaces it (UTF-8). A notice that the screen is not drawn
// with the position yet goes once it is (live_notice_until_drawn); every notice goes when a session begins.
std::string live_notice;
bool live_notice_until_drawn=false;
// A check in flight and what waits behind it. Only the UI thread touches these; the thread of the check returns its result by the future.
std::future<LiveHandResult> hand_future;
std::shared_ptr<LiveRegistrationRuntime> hand_runtime;     // the runtime that the check runs on (kept alive until its result is taken)
a0::m2::experimental::HandCounts hand_queued;              // the sum of the presses that came while a check ran
unsigned hand_queued_presses{};
// What was checked last (the sum of the presses of that run) and the run that waits: a run starts only once the position of the run before is drawn (the
// machine refuses a check that the drawing of the position before it overtook), and a check that was overtaken is run again, a few times.
a0::m2::experimental::HandCounts hand_last_delta;
bool hand_run_waits=false;
Clock::time_point hand_run_waits_since{};
inline constexpr std::chrono::seconds kHandRunWaitLimit{5};
unsigned hand_stale_retries{};
inline constexpr unsigned kHandStaleRetries=3;
bool hand_capture_dropped=false;                           // a capture was requested while a check ran: its result is not reported
// True from the request of a pair capture that the machine admitted until its result is in (the window ends it with the result, or with
// EndCapture when there is none).
bool live_capture_held=false;
std::optional<a0::m2::experimental::ConfirmedPosition> pending_confirmed;   // what the capture that waits for the Lives to stop is given
// Smokes only: a check of a hand step and a check of an estimate's position of their own (empty: the real ones), and the counters of the check that runs (how many at a time, at most, and how many in all).
LiveHandChecker live_hand_check_override;
LiveMappingChecker live_mapping_check_override;
std::atomic<int> hand_checks_running{0},hand_checks_peak{0},hand_checks_total{0};
// What passes the gate of the checks (below): how many are inside it now and at most, how many went in and how many wait at it. A smoke may hold a
// check inside the gate (cover_gate_hold) to see that the other one waits.
std::atomic<int> cover_gate_inside{0},cover_gate_peak{0},cover_gate_entered{0},cover_gate_waiting{0};
std::function<void()> cover_gate_hold;

// ---- The checks that the runtime is given (the same ones that a capture makes) ----------------------------------------------------------
// The cover check of a full-size position scans the whole output, and a burst of them would starve the Live view: the checks of a hand press and of an
// estimate's position go through one gate, one at a time. `scan_threads` is the number of threads the scan of a check uses; the answer does not depend on
// it. Four: the scan of a full-size position is long on one thread, and threads past the cores of a four-core machine add CPU time for little. The Live
// threads (two cameras and the loop of C) run beside it while it scans.
std::mutex cover_check_gate;
// One check inside the gate, counted while it holds it.
struct CoverGateEntry {
    std::unique_lock<std::mutex> lock;
    CoverGateEntry(){
        ++cover_gate_waiting;lock=std::unique_lock<std::mutex>(cover_check_gate);--cover_gate_waiting;
        const int now=++cover_gate_inside;++cover_gate_entered;
        int peak=cover_gate_peak.load();while(peak<now && !cover_gate_peak.compare_exchange_weak(peak,now)){}
        if(cover_gate_hold)cover_gate_hold();
    }
    ~CoverGateEntry(){--cover_gate_inside;}
    CoverGateEntry(const CoverGateEntry&)=delete;
    CoverGateEntry& operator=(const CoverGateEntry&)=delete;
};
inline constexpr unsigned kWindowCheckScanThreads=4;
LiveHandChecker MakeWindowHandChecker(const a0::m2::experimental::LoadedFixedCompositeConditionV3& loaded,unsigned scan_threads=kWindowCheckScanThreads){
    namespace m2x=a0::m2::experimental;
    const auto parameters=std::make_shared<const a0::m2::render::DocumentRenderParameters>(loaded.profile.parameters);
    const auto seam_column=loaded.condition.seam.column;
    const auto base_pose=loaded.condition.base_pose;
    const auto envelope=loaded.condition.registration.envelope;
    return [parameters,seam_column,base_pose,envelope,scan_threads](const m2x::RegisteredMapping& hand_base,const m2x::HandCounts& total){
        m2x::HandStepInput input;
        input.parameters=parameters.get();input.seam_column=seam_column;input.base_pose=base_pose;input.envelope=envelope;
        input.hand_base=hand_base;input.total=total;input.threads=scan_threads;
        const CoverGateEntry gate;
        const auto verdict=m2x::CheckHandStep(input);
        LiveHandCheck check;
        check.mapping=verdict.mapping;
        check.seam_covered=!verdict.cover_checked || verdict.seam_covered;check.output_covered=!verdict.cover_checked || verdict.output_covered;
        switch(verdict.status){
        case m2x::HandStepStatus::accepted:check.status=LiveHandCheck::Status::accepted;break;
        case m2x::HandStepStatus::refused_envelope:
            check.status=LiveHandCheck::Status::refused_envelope;
            for(const auto& c:verdict.components)if(c.exceeded)check.exceeded.emplace_back(m2x::content_registration::EnvelopeComponentCode(c.component));
            break;
        case m2x::HandStepStatus::refused_cover:check.status=LiveHandCheck::Status::refused_cover;break;
        case m2x::HandStepStatus::refused_unsolvable:check.status=LiveHandCheck::Status::refused_unsolvable;break;
        }
        return check;
    };
}
// The check that an estimate's position passes before it is drawn: the cover check of a capture (profile, seam column, the design's blend width).
LiveMappingChecker MakeWindowMappingChecker(const a0::m2::experimental::LoadedFixedCompositeConditionV3& loaded,unsigned scan_threads=kWindowCheckScanThreads){
    namespace m2x=a0::m2::experimental;
    const auto parameters=std::make_shared<const a0::m2::render::DocumentRenderParameters>(loaded.profile.parameters);
    const auto seam_column=loaded.condition.seam.column;
    return [parameters,seam_column,scan_threads](const m2x::RegisteredMapping& mapping){
        const CoverGateEntry gate;
        const auto cover=m2x::CheckRegisteredMappingCover(*parameters,mapping,seam_column,m2x::kSeamBlendDesign,scan_threads);
        std::vector<std::string> failed;
        if(!cover.seam_covered)failed.emplace_back(kLivePositionSeamOutsideOverlapReason);
        if(!cover.output_covered)failed.emplace_back(kLivePositionOutputUncoveredReason);
        return failed;
    };
}
// Counts how many checks run at once (the window never runs two) around whichever check the runtime was given.
LiveHandChecker CountedHandChecker(LiveHandChecker inner){
    if(!inner)return inner;
    return [inner=std::move(inner)](const a0::m2::experimental::RegisteredMapping& base,const a0::m2::experimental::HandCounts& total){
        const int now=++hand_checks_running;++hand_checks_total;
        int peak=hand_checks_peak.load();while(peak<now && !hand_checks_peak.compare_exchange_weak(peak,now)){}
        struct Done{~Done(){--hand_checks_running;}} done;
        return inner(base,total);
    };
}

// ---- What the window says of the position --------------------------------------------------------------------------------------------
// Whether the hand buttons are in the layout: a registered C (a runtime) has them. A layout check forces either way.
std::optional<bool> layout_force_hand;
bool HandControlsInLayout(){return layout_force_hand?*layout_force_hand:live_runtime!=nullptr;}
void RefreshHandLayout();   // lays the buttons out again after the runtime came or went (defined with the layout)
bool merge_buttons_hidden_for_hand=false;
void SetLiveNotice(const std::string& text,bool until_drawn=false){live_notice=text;live_notice_until_drawn=until_drawn && !text.empty();}
bool LiveHandBusy(){return hand_future.valid() || hand_run_waits;}
// The live pixel in the JPEG's pixels (the grid's pitch over the raster's pitch), from the plan that C is drawn with; 0 when there is none.
double LivePixelInOutputPx(const std::shared_ptr<const FixedPreviewPlan>& plan){
    if(!plan || !plan->live || !plan->live->raster.dpi)return 0;
    return plan->grid.pitch_mm/(25.4/plan->live->raster.dpi);
}
std::shared_ptr<const FixedPreviewPlan> DrawnPlanSnapshot(){std::lock_guard lock(merged.mutex);return merged.drawn_plan;}

// ---- The press ------------------------------------------------------------------------------------------------------------------------
void StartLiveHand(const a0::m2::experimental::HandCounts& delta){
    hand_runtime=live_runtime;hand_last_delta=delta;
    const auto drawn=DrawnPlanSnapshot();
    const bool running=!stopping && !closing;
    hand_future=std::async(std::launch::async,[runtime=hand_runtime,delta,drawn,running]{
        try{return runtime->HandStep(delta,drawn,running);}
        catch(const std::exception& error){
            LiveHandResult failed;failed.outcome=LiveHandOutcome::refused_error;failed.detail=error.what();
            Log(std::string("C live_hand_step result=error detail=")+LiveLogSafeText(RedactPaths(error.what()))+" quality_accepted=0");
            return failed;
        }
    });
}
// One press of a hand button (the UI thread). The check is never run here.
void PressLiveHand(LiveHandButton button){
    namespace m2x=a0::m2::experimental;
    if(!live_runtime){Log("C ui_live_hand refused=no-session");return;}
    const auto delta=LiveHandCountsOf(button,hand_fine);
    static constexpr const char* names[]{"left","right","up","down","turn-counter-clockwise","turn-clockwise"};
    std::ostringstream line;line.imbue(std::locale::classic());
    line<<"C ui_live_hand button="<<names[static_cast<int>(button)]<<" step="<<(hand_fine?"fine":"coarse")<<" delta_seam_dx_counts="<<delta.seam_dx<<" delta_seam_dy_counts="<<delta.seam_dy
        <<" delta_rotation_counts="<<delta.rotation<<" check_running="<<(LiveHandBusy()?1:0);
    if(LiveHandBusy()){
        // A check is running: the press waits with the others (their sum is checked once, when this check ends).
        try{hand_queued=m2x::AddHandCounts(hand_queued,delta);++hand_queued_presses;line<<" queued="<<hand_queued_presses;}
        catch(const std::out_of_range&){line<<" queued=refused-range";}
        line<<" quality_accepted=0";Log(line.str());
        return;
    }
    line<<" queued=0 quality_accepted=0";Log(line.str());
    SetLiveNotice({});
    StartLiveHand(delta);
}
// The result of the check that ran (the UI thread, from the timer and before the capture's own steps). An accepted press with a sum that waited
// starts the next check; a refused one takes the waiting sum with it.
void AdvanceLiveHand(){
    // A notice that the screen was not drawn with the position goes once it is.
    if(live_notice_until_drawn && live_runtime && live_runtime->DrawnWithPlacement(DrawnPlanSnapshot()))SetLiveNotice({});
    // A run that waits starts when the position that the run before it accepted has been drawn (nothing waits to be drawn any more).
    if(!hand_future.valid() && hand_run_waits){
        if(!live_runtime || closing || stopping || live_runtime->Status().placement.capturing){
            // the position of the run before will not be drawn now (Live stopped, a capture holds it): what waited is not checked
            if(hand_queued_presses)SetLiveNotice(LiveHandQueueDroppedText(hand_queued_presses));
            hand_run_waits=false;hand_queued={};hand_queued_presses=0;hand_capture_dropped=false;hand_stale_retries=0;return;
        }
        // Drawn: the machine has taken the position (nothing waits) and the picture on the screen was drawn with its table (the check of the next run is
        // made against that picture).
        const auto status=live_runtime->Status();
        if(!status.placement.hand_wanted && live_runtime->DrawnWithPlacement(DrawnPlanSnapshot())){
            const auto next=hand_queued;
            hand_queued={};hand_queued_presses=0;hand_run_waits=false;
            if(!next.IsZero())StartLiveHand(next);
        }else if(Clock::now()-hand_run_waits_since>kHandRunWaitLimit){
            // the position is not drawn in time (a bake that fails): what waited is not checked, and the sentence says what to do
            LiveHandResult not_drawn;not_drawn.outcome=LiveHandOutcome::refused_not_drawn;
            auto text=LiveHandRefusalText(not_drawn,false,status.bake_failed);
            const auto queue=LiveHandQueueDroppedText(hand_queued_presses);
            if(!queue.empty())text+=" "+queue;
            SetLiveNotice(text,!status.bake_failed);
            hand_run_waits=false;hand_queued={};hand_queued_presses=0;
        }
        return;
    }
    if(!hand_future.valid() || hand_future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    LiveHandResult result;
    try{result=hand_future.get();}
    catch(const std::exception& error){result.outcome=LiveHandOutcome::refused_error;result.detail=error.what();}
    const auto runtime=std::move(hand_runtime);hand_runtime.reset();
    bool beyond=false,bake_failed=false;
    if(runtime){const auto status=runtime->Status();beyond=status.placement.beyond_envelope;bake_failed=status.bake_failed;}
    const unsigned waiting=hand_queued_presses;
    const bool dropped_by_capture=hand_capture_dropped;hand_capture_dropped=false;
    if(result.outcome==LiveHandOutcome::accepted){
        if(!dropped_by_capture)SetLiveNotice(result.withdrew_estimate?LiveHandWithdrewEstimateText():result.became_condition_position?LiveHandBecameConditionText():std::string());
        hand_stale_retries=0;
        if(waiting && !hand_queued.IsZero() && runtime==live_runtime && !closing && !dropped_by_capture){hand_run_waits=true;hand_run_waits_since=Clock::now();}   // started by the next call, once drawn
        else{hand_queued={};hand_queued_presses=0;}
        return;
    }
    // Overtaken while it was checked (the position before it was drawn meanwhile): its amount is not lost, it is checked again with what waits.
    if(result.outcome==LiveHandOutcome::refused_stale && !dropped_by_capture && runtime==live_runtime && !closing && hand_stale_retries<kHandStaleRetries){
        try{hand_queued=a0::m2::experimental::AddHandCounts(hand_queued,hand_last_delta);++hand_stale_retries;hand_run_waits=true;hand_run_waits_since=Clock::now();return;}
        catch(const std::out_of_range&){}
    }
    hand_stale_retries=0;
    if(!dropped_by_capture){
        auto text=LiveHandRefusalText(result,beyond,bake_failed);
        const auto queue=LiveHandQueueDroppedText(waiting);
        if(!queue.empty())text+=" "+queue;
        SetLiveNotice(text,result.outcome==LiveHandOutcome::refused_not_drawn && !bake_failed);
    }
    hand_queued={};hand_queued_presses=0;
}
// A session begins or the window closes: the check that is running is waited for (it holds no lock, and the runtime that it ran on is the one
// whose machine the next session carries over), and what waited behind it is dropped.
void DrainLiveHand(){
    hand_queued={};hand_queued_presses=0;hand_run_waits=false;
    if(hand_future.valid()){hand_future.wait();AdvanceLiveHand();}
    hand_capture_dropped=false;hand_stale_retries=0;   // nothing of a press is carried into the next session
}

// ---- The capture ----------------------------------------------------------------------------------------------------------------------
// The report's word for where a position came from: the same code as the live log's (a code that the report does not know is a mistake of the build).
a0::m2::experimental::ConfirmedPositionSource ConfirmedSourceOf(LivePlacementSource source){
    const auto parsed=a0::m2::experimental::ParseConfirmedPositionSource(LivePlacementSourceCode(source));
    if(!parsed)throw std::logic_error("a position source that the report does not know");
    return *parsed;
}
a0::m2::experimental::ConfirmedPosition ConfirmedPositionOfGrant(const LivePlacementGrant& grant){
    namespace m2x=a0::m2::experimental;
    m2x::ConfirmedPosition position;
    position.mapping=grant.mapping;
    position.source=ConfirmedSourceOf(grant.source);
    position.serial=grant.serial;
    position.condition_sha256=grant.condition_sha256;
    if(grant.base_source)position.base_source=ConfirmedSourceOf(*grant.base_source);
    position.manual_entered=grant.hand_entered;
    return position;
}
// The capture is over (with no JPEG that applies, or the shot never started): the position is free again.
void EndLiveCaptureHold(){
    if(!live_capture_held)return;
    live_capture_held=false;pending_confirmed.reset();
    if(live_runtime)live_runtime->EndCapture();
}
// A pair capture is asked for (RequestCapture(2), under merged.mutex): the position that is drawn is taken, and what waited to be drawn is dropped.
// False when the screen is not drawn with the position yet: nothing was changed and the sentence says what to do.
bool BeginLiveCapture(const std::shared_ptr<const FixedPreviewPlan>& drawn){
    if(!live_runtime)return true;
    const bool hand_waited=live_runtime->Status().placement.hand_wanted.has_value();   // a press was accepted and waits to be drawn
    const auto begin=live_runtime->BeginCapture(drawn);
    if(begin.admission!=LiveCaptureAdmission::accepted){
        const bool bake_failed=live_runtime->Status().bake_failed;
        const auto text=LiveCaptureRefusalText(begin.admission,bake_failed);
        capture_status=WidenUtf8(text);SetLiveNotice(text,begin.admission==LiveCaptureAdmission::not_drawn && !bake_failed);
        return false;
    }
    live_capture_held=true;
    pending_confirmed=ConfirmedPositionOfGrant(*begin.grant);
    if(LiveHandBusy() || hand_queued_presses || (begin.dropped_pending && hand_waited)){
        // Only a check that still runs has a result to come; a run that waited to be drawn, or a press that waited, is simply dropped.
        hand_capture_dropped=hand_future.valid();
        hand_queued={};hand_queued_presses=0;hand_run_waits=false;hand_stale_retries=0;
        SetLiveNotice(LiveHandDroppedByCaptureText());
    }else SetLiveNotice({});
    return true;
}

// ---- The buttons ----------------------------------------------------------------------------------------------------------------------
const wchar_t* HandButtonLabel(int index){
    static const wchar_t* labels[]{L"← Bを左へ",L"Bを右へ →",L"↑ Bを上へ",L"Bを下へ ↓",L"↺ 反時計回り",L"時計回り ↻"};
    return labels[index];
}
const wchar_t* HandStepLabel(bool fine){return fine?L"刻み: 細かい（4 px・0.05°）":L"刻み: 粗い（12 px・0.2°）";}
void CreateHandButtons(HWND window,HINSTANCE instance){
    for(int i=0;i<kPreviewHandButtonCount;++i){
        const wchar_t* text=i==kPreviewHandStep?HandStepLabel(hand_fine):HandButtonLabel(i);
        hand_buttons[i]=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHandButtonFirstId+i)),instance,nullptr);
        if(!hand_buttons[i])throw std::runtime_error("create hand button failed");
        EnableWindow(hand_buttons[i],FALSE);
    }
}
// A hand button was clicked. False when the command is not one of theirs.
bool HandleLiveHandCommand(int id,int notify,HWND from){
    if(id<kHandButtonFirstId || id>=kHandButtonFirstId+kPreviewHandButtonCount || notify!=BN_CLICKED || from!=hand_buttons[id-kHandButtonFirstId])return false;
    const int index=id-kHandButtonFirstId;
    if(index==kPreviewHandStep){
        hand_fine=!hand_fine;SetWindowTextW(hand_buttons[index],HandStepLabel(hand_fine));
        Log(std::string("C ui_live_hand_step_size=")+(hand_fine?"fine":"coarse"));
        return true;
    }
    PressLiveHand(static_cast<LiveHandButton>(index));
    return true;
}
// Whether the hand buttons can be pressed now: a registered C is drawn and Live runs. (A capture or a stop disables them; a press that gets
// through anyway is refused by the machine, with its sentence.)
bool HandControlsUsable(){
    if(!live_runtime || stopping || closing)return false;
#if defined(A0_ENABLE_WPD_RECALIBRATION)
    if(RecalibrationHoldsLive())return false;
#endif
    std::lock_guard lock(merged.mutex);
    return merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.fixed_plan.live!=nullptr;
}
// The timer: which buttons can be pressed, which are shown (a condition v3 has them and the four position buttons of the uncalibrated C stand aside).
void UpdateHandControls(){
    const bool usable=HandControlsUsable();
    for(int i=0;i<kPreviewHandButtonCount;++i)if(hand_buttons[i])EnableWindow(hand_buttons[i],usable);
}
