// Camera-free contract of the Live-open capture probe's window side (#426 stage 4, ADR-0044). Included by
// wpd_dual_preview_ui.hpp after the window procedure, inside the anonymous namespace, and only when
// A0_ENABLE_WPD_LIVE_CAPTURE_PROBE is defined. Synthetic cameras, a synthetic backend and a window of its own:
// no Discover/Open/Run, no hardware lease and no camera command. This is the only place that sends the probe
// button a click (kLiveProbeButtonId); the real button is pressed by a person.
int RunLiveCaptureProbeUiSmoke(){
    Log("live_capture_probe_ui_synthetic=1 camera_operations=0");
    unsigned checks{},failures{};
    const auto check=[&](bool ok,const char* what){++checks;if(!ok){++failures;Log(std::string("live_capture_probe_ui_failed_check=")+what);}};
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0LiveCaptureProbeUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic live capture probe test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    live_probe_button=CreateWindowExW(0,L"BUTTON",LiveProbeButtonLabel(),WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLiveProbeButtonId)),instance,nullptr);
    if(!live_probe_button){DestroyWindow(window);return 2;}
    run_seconds=0;run_start=Clock::now();
    const int target=LiveCaptureProbeCameraIndex(),other=1-target;
    // The synthetic backend plays the probe thread: it borrows the named camera's held session, notes what it saw, and returns it.
    std::atomic<unsigned> backend_calls{};
    std::atomic<int> backend_camera{-1};
    std::atomic<bool> lent_state_seen{},other_held_seen{},block_backend{};
    std::optional<unsigned> backend_live_after=1u;
    bool backend_clean=true;
    live_probe_backend=[&](Camera& camera,Clock::time_point)->LiveProbeThreadOutcome{
        ++backend_calls;backend_camera=camera.alias=="CAM-A"?0:1;
        LiveProbeThreadOutcome result;
        const auto token=camera.owner.Lend();
        if(!token)return result;
        result.lent=true;
        lent_state_seen=camera.owner.State()==PreviewSessionState::Lent && camera.owner.IsLentTo(*token);
        other_held_seen=cameras[other]->owner.State()==PreviewSessionState::Held;
        while(block_backend)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        result.outcome.result=WpdLiveProbeResult::Observed;result.outcome.shutter_sent=true;result.outcome.live_after=backend_live_after;result.outcome.sequence_clean=backend_clean;
        result.outcome.published="original";result.outcome.attributed=true;
        camera.owner.Return(*token);
        return result;
    };
    const auto reset=[&]{
        for(int i=0;i<2;++i){
            cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
            c.alias=i==0?"CAM-A":"CAM-B";c.opened=true;c.start_attempted=true;c.started=true;c.focus_mode=0;
            c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();
        }
        stopping=false;closing=false;preview_joined=false;capture_stop_requested=false;capture_restart_blocked=false;capture_pending.reset();
        live_probe_used=false;live_probe_phase=LiveProbePhase::Idle;live_probe_outcome.reset();live_probe_lent=false;live_probe_frame_result="skipped";
        backend_calls=0;backend_camera=-1;lent_state_seen=false;other_held_seen=false;block_backend=false;backend_live_after=1u;backend_clean=true;
    };
    const auto tick=[&]{
        for(auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();}
        SendMessageW(window,WM_TIMER,1,0);
    };
    const auto click=[&]{SendMessageW(window,WM_COMMAND,MAKEWPARAM(kLiveProbeButtonId,BN_CLICKED),reinterpret_cast<LPARAM>(live_probe_button));};
    const auto wait_for=[&](const std::function<bool()>& done){
        const auto until=Clock::now()+std::chrono::seconds(5);
        while(!done() && Clock::now()<until){tick();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        return done();
    };
    const auto owner_is=[&](int i,PreviewSessionState state){return cameras[i]->owner.State()==state;};
    // Both Live workers are done: the shutdown the existing "ライブ停止" path performs, as far as the window can see it.
    const auto workers_end=[&]{for(auto& camera:cameras){camera->stopped=true;camera->closed=true;camera->done=true;}};

    // 1. The button is enabled only when every condition holds; the same conditions are re-checked by the click.
    reset();
    tick();check(IsWindowEnabled(live_probe_button)!=FALSE && LiveProbeCanPress(),"enabled when every condition holds");
    {
        const auto refused=[&](const char* what){check(!LiveProbeCanPress(),what);};
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now()-std::chrono::seconds(3);}refused("stale Live frame of CAM-A");
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now();cameras[1]->frames=5;}refused("too few frames of CAM-B");
        cameras[1]->frames=25;
        check(cameras[1]->focus_mailbox.TryQueue(PreviewFocusAction::AutoFocus,true),"mailbox taken");refused("an AF request is pending");
        cameras[1]->focus_mailbox.Take();refused("an AF request is running");cameras[1]->focus_mailbox.Complete();
        check(cameras[0]->focus_mailbox.TryQueueAfPoint(PreviewAfPointRequest{10,10,100,100},true),"mailbox taken for the AF frame");refused("an AF frame move is pending");
        cameras[0]->focus_mailbox.TakeAfPoint();cameras[0]->focus_mailbox.Complete();
        {std::promise<CaptureUiOutcome> pending;capture_future=pending.get_future();}refused("a capture future exists");capture_future=std::future<CaptureUiOutcome>();
        {std::promise<SettingsReadOutcome> pending;settings_future=pending.get_future();}refused("a settings read exists");settings_future=std::future<SettingsReadOutcome>();
        capture_pending=0;refused("a capture is pending");capture_pending.reset();
        capture_restart_blocked=true;refused("a failed capture stopped the window");capture_restart_blocked=false;
        closing=true;refused("the window is closing");closing=false;
        stopping=true;refused("Live is stopping");stopping=false;
        live_probe_used=true;refused("the probe was already used");live_probe_used=false;
        // An automatic diagnostic run (it opens the cameras by itself) never offers the button.
        run_seconds=30;refused("an automatic run with --seconds N>0");
        click();check(!live_probe_used && live_probe_phase==LiveProbePhase::Idle,"a click during an automatic run does not use up the press");
        run_seconds=0;
        {
            bool* const flags[]={&focus_smoke,&capture_smoke,&merge_smoke,&pair_smoke,&level_smoke,&restart_smoke,&pair_cycle_smoke,&af_prepare_smoke,&camera_end_restart_smoke};
            for(bool* flag:flags){*flag=true;refused("an automatic smoke or capture flag");*flag=false;}
        }
        capture_once_alias=1;refused("a one-shot command-line capture");capture_once_alias.reset();
        check(LiveProbeCanPress(),"enabled again after the automatic-run conditions are cleared");
        cameras[1]->first=Clock::now()-std::chrono::minutes(6);refused("CAM-B Live is older than the limit");
        cameras[1]->first=Clock::now()-std::chrono::minutes(4);check(LiveProbeCanPress(),"CAM-B Live younger than the limit");
        cameras[1]->first=Clock::now()-std::chrono::seconds(3);
        cameras[other]->first=Clock::now()-std::chrono::minutes(6);check(LiveProbeCanPress(),"the other camera's Live age does not matter");
        cameras[other]->first=Clock::now()-std::chrono::seconds(3);
        for(const DWORD mode:{1u,3u,4u,0xffffffffu}){cameras[1]->focus_mode=mode;refused("focus mode that is not AF-S or AF-F");}
        cameras[1]->focus_mode=2;check(LiveProbeCanPress(),"AF-F is accepted");cameras[1]->focus_mode=0;
        check(cameras[0]->owner.RequestHold(),"hold requested");refused("a session is not the Live worker's");cameras[0]->owner.Release();
        check(LiveProbeCanPress(),"enabled again after every condition is back");
        // A click that finds a condition missing does nothing and does not use up the press.
        cameras[1]->focus_mode=3;click();
        check(!live_probe_used && live_probe_phase==LiveProbePhase::Idle && backend_calls==0 && owner_is(0,PreviewSessionState::Live) && owner_is(1,PreviewSessionState::Live),"a refused click changes nothing");
        cameras[1]->focus_mode=0;
    }

    // 2. A full run: hold, borrow, one frame, stop. One press for the life of the process.
    reset();
    click();
    check(live_probe_used && live_probe_phase==LiveProbePhase::Holding && owner_is(0,PreviewSessionState::HoldRequested) && owner_is(1,PreviewSessionState::HoldRequested),"press asks both workers to hold");
    check(!cameras[0]->focus_mailbox.TryQueue(PreviewFocusAction::AutoFocus,CanFocus(*cameras[0])) && !CanFocus(*cameras[1]),"no focus request is accepted while the probe holds");
    RequestCapture(0);RequestCapture(1);RequestCapture(2);RequestSettingsRead();
    check(!capture_pending && !capture_future.valid() && !settings_future.valid(),"no capture or settings read starts during the probe");
    tick();check(IsWindowEnabled(live_probe_button)==FALSE,"the button is disabled during the probe");
    cameras[other]->owner.AcknowledgeHold();tick();
    check(live_probe_phase==LiveProbePhase::Holding && backend_calls==0,"one answer is not enough");
    cameras[target]->owner.AcknowledgeHold();tick();
    check(live_probe_phase==LiveProbePhase::Running,"both answers start the probe thread");
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::AfterFrame,"the probe thread's result asks for one frame");
    check(backend_calls==1 && backend_camera==target,"the backend ran once, for the named camera");
    check(lent_state_seen && other_held_seen,"the named session was lent while the other stayed held");
    check(owner_is(target,PreviewSessionState::Live) && owner_is(other,PreviewSessionState::Held) && cameras[target]->probe_frame_state==1 && !stopping,"the named camera is back with its worker, the other stays held");
    // The worker reads its one frame and asks to hold again at the next boundary (what the worker loop does).
    cameras[target]->probe_frame_state=2;cameras[target]->owner.RequestHold();cameras[target]->owner.AcknowledgeHold();
    tick();
    check(live_probe_phase==LiveProbePhase::Stopping && stopping && live_probe_frame_result=="ok","the frame result is read and both Live sessions are told to stop");
    check(owner_is(0,PreviewSessionState::Live) && owner_is(1,PreviewSessionState::Live),"both sessions are released so that the workers can Stop and Close");
    workers_end();tick();
    check(preview_joined && live_probe_phase==LiveProbePhase::Done,"the probe ends when both workers are joined");
    check(capture_status.find(L"探りが済みました")!=std::wstring::npos && capture_status.find(L"もう撮らないでください")!=std::wstring::npos,"the screen says the probe is done");
    tick();check(IsWindowEnabled(live_probe_button)==FALSE,"the button stays disabled");
    click();tick();
    check(backend_calls==1 && live_probe_phase==LiveProbePhase::Done,"a second press does nothing");

    // 3. Live View no longer reads on after the shot: no frame is asked for.
    reset();backend_live_after=0u;
    click();for(auto& camera:cameras)camera->owner.AcknowledgeHold();tick();
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::Stopping,"live_after 0 goes straight to the stop");
    check(cameras[target]->probe_frame_state==0 && live_probe_frame_result=="skipped" && stopping,"no frame is requested");
    workers_end();tick();check(live_probe_phase==LiveProbePhase::Done,"done after the stop");

    // 3b. The sequence was not clean (an anomaly): no frame is asked for even though Live View reads on.
    reset();backend_clean=false;
    click();for(auto& camera:cameras)camera->owner.AcknowledgeHold();tick();
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::Stopping,"an unclean sequence goes straight to the stop");
    check(cameras[target]->probe_frame_state==0 && live_probe_frame_result=="skipped" && stopping && owner_is(target,PreviewSessionState::Live),"after an anomaly no frame is requested; both sessions are released for the stop");
    workers_end();tick();check(live_probe_phase==LiveProbePhase::Done,"done after the stop");

    // 3c. The 60-second deadline ran out while the probe thread finished: no frame is asked for.
    reset();block_backend=true;
    click();for(auto& camera:cameras)camera->owner.AcknowledgeHold();tick();
    wait_for([&]{return backend_calls==1;});
    live_probe_deadline=Clock::now()-std::chrono::seconds(1);
    block_backend=false;
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::Stopping,"a run that ends after the deadline goes straight to the stop");
    check(cameras[target]->probe_frame_state==0 && live_probe_frame_result=="skipped" && stopping,"after the deadline no frame is requested");
    workers_end();tick();check(live_probe_phase==LiveProbePhase::Done,"done after the stop");

    // 3d. A frame is still asked for when the run ends just inside the deadline.
    reset();block_backend=true;
    click();for(auto& camera:cameras)camera->owner.AcknowledgeHold();tick();
    wait_for([&]{return backend_calls==1;});
    live_probe_deadline=Clock::now()+std::chrono::seconds(30);
    block_backend=false;
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::AfterFrame && cameras[target]->probe_frame_state==1,"a run that ends inside the deadline still asks for its one frame");
    cameras[target]->probe_frame_state=2;cameras[target]->owner.RequestHold();cameras[target]->owner.AcknowledgeHold();tick();
    check(live_probe_phase==LiveProbePhase::Stopping,"then it stops");
    workers_end();tick();check(live_probe_phase==LiveProbePhase::Done,"done after the stop");

    // 4. A worker that does not answer in time: nothing is sent, the holds are given back, Live runs on.
    reset();
    click();cameras[other]->owner.AcknowledgeHold();tick();
    live_probe_hold_started=Clock::now()-kLiveCaptureProbeHoldWait-std::chrono::seconds(1);tick();
    check(live_probe_phase==LiveProbePhase::Done && backend_calls==0 && owner_is(0,PreviewSessionState::Live) && owner_is(1,PreviewSessionState::Live) && !stopping,"a hold timeout ends without a shutter and keeps Live");
    check(live_probe_used && !LiveProbeCanPress(),"the press stays used");

    // 5. The window closes while the workers are asked to hold.
    reset();
    click();closing=true;stopping=true;capture_stop_requested=true;tick();
    check(live_probe_phase==LiveProbePhase::Done && backend_calls==0 && owner_is(0,PreviewSessionState::Live) && owner_is(1,PreviewSessionState::Live),"closing while holding sends nothing");

    // 6. The window closes while the probe thread runs: the window waits for it, then releases, and asks for no frame.
    reset();block_backend=true;
    click();for(auto& camera:cameras)camera->owner.AcknowledgeHold();tick();
    check(live_probe_phase==LiveProbePhase::Running,"running");
    wait_for([&]{return backend_calls==1;});
    closing=true;stopping=true;capture_stop_requested=true;tick();
    check(live_probe_phase==LiveProbePhase::Running && LiveProbeHoldsWindow(),"the window is held while the probe thread runs");
    block_backend=false;
    check(wait_for([&]{return live_probe_phase!=LiveProbePhase::Running;}) && live_probe_phase==LiveProbePhase::Stopping,"after the thread ends the window stops");
    check(cameras[target]->probe_frame_state==0 && owner_is(0,PreviewSessionState::Live) && owner_is(1,PreviewSessionState::Live),"no frame is asked for and the sessions are released");

    // 7. A worker that was asked to hold sends nothing and waits for the release, even while the window is closing; the
    // Live path refuses to send while the session is held or lent; no focus request is accepted meanwhile.
    {
        ComScope com; // the request below is a WPD object
        reset();
        Camera& camera=*cameras[0];
        camera.owner.RequestHold();
        std::atomic<bool> finished{};
        std::thread worker([&]{camera.HoldAtFrameBoundary();finished=true;});
        const auto until=Clock::now()+std::chrono::seconds(5);
        while(camera.owner.State()!=PreviewSessionState::Held && Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(camera.owner.State()==PreviewSessionState::Held,"the worker answers a hold request");
        const auto refused=[&](const char* what){
            auto request=BuildPreviewDeviceReadyRequest();
            bool thrown=false;
            try{camera.Dispatch(request.Get());}catch(const std::runtime_error&){thrown=true;}
            check(thrown,what);
        };
        refused("the Live path refuses to send while the session is held");
        stopping=true;capture_stop_requested=true; // the window is closing
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(!finished,"a closing window does not release a held worker");
        check(!camera.focus_mailbox.TryQueue(PreviewFocusAction::AutoFocus,CanFocus(camera)) && !camera.focus_mailbox.TryQueueAfPoint(PreviewAfPointRequest{10,10,100,100},CanFocus(camera)),"no focus or AF frame request is accepted while held");
        const auto token=camera.owner.Lend();
        check(token.has_value(),"the held session can be lent");
        refused("the Live path refuses to send while the session is lent");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!finished,"a lent session keeps the worker waiting");
        camera.owner.Return(*token);
        refused("the Live path still refuses after the session is returned but not released");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!finished,"a returned session keeps the worker waiting until it is released");
        camera.owner.Release();
        worker.join();
        check(finished && camera.held_dispatch_refused>=3,"the release lets the worker go, and the refusals were counted");
    }

    live_probe_backend=RunLiveProbeOnCamera;
    live_probe_future=std::future<LiveProbeThreadOutcome>();
    DestroyWindow(window);live_probe_button=nullptr;
    stopping=true;
    Log("live_capture_probe_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");
    return failures?1:0;
}
