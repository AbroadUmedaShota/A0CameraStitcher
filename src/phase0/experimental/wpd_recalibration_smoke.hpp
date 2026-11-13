// --recalibration-ui-smoke <rig profile v2 fixture folder> [<folder for pictures of the screens>] (#434): the calibration button's screens in the
// real window procedure, on synthetic cameras. Included by wpd_dual_preview_ui.hpp after the window procedure, only in a build with
// A0_BUILD_WPD_RECALIBRATION.
//
// What is real: the window procedure, the buttons, Paint, the capture admission and the saving of a calibration's pair (SaveRecalibrationPair
// with a synthetic pair_capture_backend), the job (RunRecalibrationJob) in one run and the confirmation (ConfirmRecalibration) on a temporary
// settings folder. What is not: the computation (a stand-in that returns at once; the real one takes minutes), the cameras (no Discover, no
// Open, no lease, no shutter, no settings read), and the restart of Live (preview_restart_override). The settings folder of the machine is
// never opened: recalibration_settings_override and pair_review_root_override point at temporary folders.
// The window is 1000 pixels wide in the client area (the owner's screen is 1080 pixels wide): every control that this change adds must
// be inside it, whatever the phase. Controls that were there before are listed when they are not (info lines, not failures).
int RunRecalibrationUiSmoke(const std::filesystem::path& fixtures,const std::filesystem::path& pictures){
    Log("recalibration_ui_synthetic=1 camera_operations=0");
    namespace fs=std::filesystem;
    namespace m2e=a0::m2::experimental;
    namespace cr=m2e::content_registration;
    namespace rc=m2e::recalibration;
    namespace ps=a0::phase0::experimental::pair_synthetic;
    namespace render=a0::m2::render;
    using a0::m2::RigProfileV2;
    using namespace std::chrono_literals;
    unsigned checks{},failures{};
    const auto check=[&](bool ok,const char* what){++checks;if(!ok){++failures;Log(std::string("recalibration_ui_failed_check=")+what);}};
    const auto work=fs::temp_directory_path()/("a0-recal-ui-smoke-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    HWND window=nullptr;
    const std::string kRecord="{\"schema\":\"test-record\"}";
    try{
        ComScope com;
        fs::create_directories(work);
        if(!pictures.empty())fs::create_directories(pictures);
        const auto fixture=ps::ReadText(fixtures/"calibrated-draft.json");
        if(fixture.empty())throw std::runtime_error("the rig profile fixture is missing");

        // ---- The rig, the settings and the stand-in for the computation (as the job's test builds them) ----
        const std::string rig=ps::detail::WithMatrix(ps::detail::WithMatrix(fixture,0,{10,0,100,0,10,100}),1,{10,0,-5660,0,10,100});
        const auto raster=ps::Raster(560000,100000,740000,260000,254);
        const auto make_settings=[&](const fs::path& folder){
            auto c=ps::SampleCondition(folder,rig,m2e::MountOrientation::normal,raster);
            c.registration.envelope={5,1,0.5,2};
            c.registration.surface_envelope=m2e::SurfaceEnvelope{10,2};
            c.registration.limits.convergence_px=0.05;
            c.registration.limits.minimum_unique_blocks=2;
            m2e::ValidateFixedCompositeConditionV3(c);
            const auto revisions=folder/std::string(m2e::kFixedCompositeRevisionFolderName);
            fs::create_directories(revisions);
            m2e::WriteFixedCompositeConditionV3(revisions,c);
        };
        const auto project=[](const render::Matrix3& h,render::Point x){render::Point p{};if(!render::TryTransform(h,x,p))throw std::runtime_error("projection failed");return p;};
        const auto pose_for_shift=[&](const render::CameraProjection& camera_b,double shift_mm){
            const cr::PoseModel model(camera_b);
            const render::Point x0{934,236};
            const auto target=project(camera_b.document_to_image,{x0.x-shift_mm,x0.y});
            const auto at=[&](double tx){return project(model.CameraHomography(cr::PoseParameters{0,0,0,tx,0,0}),x0).x-target.x;};
            double a=0,b=1;
            for(int i=0;i<6;++i){const double fa=at(a),fb=at(b);if(fa==fb)break;const double c=b-fb*(b-a)/(fb-fa);a=b;b=c;}
            return cr::PoseParameters{0,0,0,b,0,0};
        };
        RecalibrationComputation fake_computation;
        fake_computation.propose=[&](const rc::Inputs& in){
            if(in.progress)for(const auto s:{rc::Stage::wide_search,rc::Stage::fine_fit})in.progress(s);
            const auto parent=RigProfileV2::Parse(in.profile_bytes).ValidateCalibrationForEvaluation(in.active.raster,render::Resampling::bilinear);
            const auto pose=pose_for_shift(parent.camera_b,30.0);
            auto p=rc::ProposeFromPose(in,pose,rc::ProfileFit{0.1,0.3,50},kRecord);   // the real pieces after the estimate: the candidate and the creation check
            if(in.progress)in.progress(rc::Stage::verify);
            p.estimate.state=cr::RegistrationState::verified;p.estimate.has_estimate=true;
            p.verification.performed=true;p.verification.limit_px=in.active.registration.limits.convergence_px;
            p.verification.state=cr::RegistrationState::verified;p.verification.passed=true;
            p.ready=p.candidate.has_value() && p.refusals.empty();
            return p;
        };
        recal_ui.computation=fake_computation;
        run_seconds=0;run_start=Clock::now();   // the timed run of the real window must not close this one

        // ---- The base settings, the pair, the seams of the capture and of the job ----
        const auto base_settings=work/"settings-base";
        make_settings(base_settings);
        const auto frames=ps::WriteFrames(work/"pair",ps::Flat(128),ps::Flat(131));
        pair_review_root_override=work/"reviews";
        unsigned backend_calls{};bool backend_fails=false,backend_preflight_stop=false,backend_late_image=false;
        pair_capture_backend=[&](const std::array<std::wstring,2>&,Clock::time_point)->VerifiedStillPair{
            ++backend_calls;
            VerifiedStillPair pair;
            if(backend_fails){pair.complete=false;pair.shutter_dispatched={true,false};pair.error="synthetic capture failure";return pair;}   // A's shutter went, B's did not
            if(backend_preflight_stop){   // both bodies still hold a completion notice: stopped before either shutter
                pair.complete=false;pair.error="synthetic preflight stop";
                for(int i=0;i<2;++i){pair.preflight_checked[i]=true;pair.preflight[i].closed=true;pair.preflight[i].kind=StillPreflightKind::Event;pair.preflight[i].event_code=0xc102;}
                return pair;
            }
            if(backend_late_image){pair.complete=false;pair.shutter_dispatched={true,true};pair.error="synthetic late image";pair.originals[1].late_object_event=true;return pair;}
            pair.complete=true;pair.preflight_complete=true;pair.shutter_dispatched={true,true};
            const std::array<fs::path,2> paths{frames.a,frames.b};
            for(size_t i=0;i<2;++i){auto& o=pair.originals[i];o.success=true;o.closed=true;o.path=paths[i];o.bytes=fs::file_size(paths[i]);o.sha256=ps::Sha(paths[i]);}
            return pair;
        };
        // A settings read that must never be asked for (it would open the cameras): a stand-in that counts.
        unsigned settings_reads{};
        settings_backend=[&](int index,const std::wstring&,Clock::time_point,const std::atomic<bool>&){
            ++settings_reads;SettingsCameraResult result;result.read.alias=index==0?"CAM-A":"CAM-B";MarkSettingsUnread(result,"not_started");return result;
        };
        unsigned restarts{};
        std::mutex gate_mutex;std::condition_variable_any gate_cv;bool gate_open=true;
        const auto set_gate=[&](bool open){{std::lock_guard lock(gate_mutex);gate_open=open;}gate_cv.notify_all();};
        struct SeenRequest {bool live_stopped;fs::path staging;bool has_stop_token;};
        std::mutex seen_mutex;std::vector<SeenRequest> seen;
        std::optional<RecalibrationJobOutcome> canned;
        const auto stopped_outcome=[]{
            RecalibrationJobOutcome o;o.findings.push_back({FindingClass::no_candidate,"stopped",""});o.verdict=RecalibrationVerdict::unusable;
            o.screen_lines=RecalibrationScreenLines(o.verdict,o.findings,o.facts);return o;
        };
        recal_ui.runner=[&](const RecalibrationJobRequest& request)->RecalibrationJobOutcome{
            {std::lock_guard lock(seen_mutex);seen.push_back({request.live_view_stopped,request.staging_folder,request.stop.stop_possible()});}
            if(request.on_stage)request.on_stage(RecalibrationStage::wide_search,0.5);
            {std::unique_lock lock(gate_mutex);gate_cv.wait(lock,request.stop,[&]{return gate_open;});}
            if(request.stop.stop_requested())return stopped_outcome();
            if(!canned)return RunRecalibrationJob(request);   // the first run is the real job on the stand-in computation
            std::error_code ec;fs::create_directories(request.staging_folder,ec);
            auto copy=*canned;copy.staging_folder=request.staging_folder;return copy;
        };
        const auto seen_count=[&]{std::lock_guard lock(seen_mutex);return seen.size();};

        // ---- The window: 1000 pixels of client width ----
        auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0RecalibrationUiSmoke";
        if(!RegisterClassW(&wc))return 2;
        const DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
        const auto set_client_size=[&](int width,int height){
            RECT outer{0,0,width,height};AdjustWindowRectEx(&outer,style,FALSE,0);
            SetWindowPos(window,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            LayoutButtons(window);
        };
        window=CreateWindowExW(0,wc.lpszClassName,L"synthetic recalibration test",style,0,0,1000,1000,nullptr,nullptr,instance,nullptr);
        if(!window)return 2;
        preview_window=window;
        // The controls that were there before, with their real labels and positions, to see what the window holds at this width.
        std::vector<std::pair<int,HWND>> old_controls;
        const auto add_old=[&](const wchar_t* label,int id,DWORD extra=BS_PUSHBUTTON){
            HWND h=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|extra,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            if(!h)throw std::runtime_error("create control failed");old_controls.push_back({id,h});return h;
        };
        constexpr std::array<const wchar_t*,3> focus_labels{L"AF",L"近くへ微調整",L"遠くへ微調整"};
        for(int i=0;i<2;++i){
            for(int j=0;j<3;++j)focus_buttons[i][j]=add_old(focus_labels[j],100+i*10+j);
            capture_buttons[i]=add_old(i==0?L"Aを撮影・保存":L"Bを撮影・保存",200+i);
        }
        constexpr std::array<const wchar_t*,5> merge_labels{L"重なり −1px",L"重なり ＋1px",L"Bを上へ 1px",L"Bを下へ 1px",L"自動位置合わせ"};
        for(int j=0;j<5;++j)merge_buttons[j]=add_old(merge_labels[j],300+j);
        pair_button=add_old(L"両台撮影 → 仮Merge保存",400);review_button=add_old(L"撮影結果を見る",401);inspect_button=add_old(L"拡大確認・再Merge",402);
        session_button=add_old(L"ライブ停止",403);seam_button=add_old(L"継ぎ目の線を表示",404,BS_AUTOCHECKBOX);settings_button=add_old(L"両台の設定を読む",405);
        CreateRecalibrationControls(window,instance);
        set_client_size(1000,960);
        RECT client{};GetClientRect(window,&client);
        check(client.right==1000,"the window is 1000 pixels wide in the client area");

        // ---- Synthetic cameras, a pair that can be captured, the restart of Live ----
        const auto make_cameras=[&]{
            for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
                c.alias=i==0?"CAM-A":"CAM-B";c.opened=true;c.start_attempted=true;c.started=true;
                c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
        };
        // C is "not drawn by the condition" (admitted without a picture) and fresh; the cameras' last frames are new.
        const auto fresh=[&]{
            for(auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}
            std::lock_guard lock(merged.mutex);
            merged.decided=true;merged.mode=PreviewCMode::unavailable;merged.rendered_revision=merged.revision;merged.source_a=Clock::now();merged.source_b=Clock::now();
        };
        const auto finish_workers=[&]{for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}};
        preview_restart_override=[&]{++restarts;stopping=false;preview_joined=false;ClearMerge();LoadFixedSource();make_cameras();fresh();++preview_sessions;};
        const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
        const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
        const auto press=[&](int index){click(kRecalFlowFirstId+index,recal_flow_buttons[index]);};
        const auto wait_until=[&](auto ready,int milliseconds){
            for(int waited=0;waited<milliseconds;waited+=10){tick();if(ready())return true;std::this_thread::sleep_for(10ms);}
            return false;
        };
        const auto phase=[&]{return recal_ui.phase;};
        const auto mid=[&]{RECT area{};GetClientRect(window,&area);return static_cast<int>(area.bottom/2);};
        std::optional<RecalibrationJobOutcome> base_unchecked;   // the real job's outcome on the flat pair (an unchecked calibration)
        const auto shown=[&](int index){return RecalibrationButtonShown(recal_flow_buttons[index]);};
        const auto enabled=[&](int index){return IsWindowEnabled(recal_flow_buttons[index])!=FALSE;};
        const auto caption=[&]{wchar_t text[256]{};GetWindowTextW(window,text,256);return std::wstring(text);};
        const auto has=[](const std::wstring& text,const wchar_t* part){return text.find(part)!=std::wstring::npos;};
        const auto snapshot=[](const fs::path& folder){
            std::map<std::string,std::string> files;
            for(const auto& entry:fs::recursive_directory_iterator(folder))
                if(entry.is_regular_file())files[fs::relative(entry.path(),folder).generic_string()]=ps::ReadText(entry.path());
            return files;
        };
        const auto revision_count=[&](const fs::path& folder){
            unsigned count{};for(const auto& entry:fs::directory_iterator(folder/std::string(m2e::kFixedCompositeRevisionFolderName)))
                if(entry.path().extension()==".json")++count;
            return count;
        };
        // A fresh scenario: a copy of the base settings, Live "running" with fresh frames, every state of the button at rest.
        const auto scenario=[&](const std::string& name){
            const auto folder=work/name;
            fs::copy(base_settings,folder,fs::copy_options::recursive);
            recalibration_settings_override=folder;
            recal_ui.phase=RecalPhase::idle;recal_ui.action=RecalAction::none;recal_ui.capture.reset();recal_ui.outcome.reset();recal_ui.note.clear();
            recal_ui.abort_requested=false;recal_ui.shown_signature.clear();recal_ui.confirmed_unchecked_sha.clear();recal_ui.scanned_sha.clear();recal_ui.scanned_unchecked=false;
            recal_ui.thumbnail={};recal_ui.crops={};
            closing=false;stopping=false;preview_joined=false;capture_pending.reset();capture_restart_blocked=false;settings_close_failed=false;
            backend_fails=false;set_gate(true);
            make_cameras();ClearMerge();LoadFixedSource();fresh();tick();
            return folder;
        };
        const auto guard_no_hardware=[&]{
            // A request that the screen must refuse is withdrawn at once, so that a refusal that failed could not reach a camera.
            RequestCapture(0);RequestCapture(1);RequestCapture(2);click(200,capture_buttons[0]);click(201,capture_buttons[1]);click(400,pair_button);
            const bool pending=capture_pending.has_value();capture_pending.reset();
            return !pending;
        };

        // ---- Geometry: every control that this change adds is in the client area, and none lies over another ----
        const auto client_rect=[&](HWND control){RECT r{};GetWindowRect(control,&r);POINT a{r.left,r.top},b{r.right,r.bottom};ScreenToClient(window,&a);ScreenToClient(window,&b);return RECT{a.x,a.y,b.x,b.y};};
        const auto geometry_ok=[&](const char* what){
            RECT area{};GetClientRect(window,&area);
            std::vector<RECT> rects;
            if(RecalibrationButtonShown(recal_enter_button))rects.push_back(client_rect(recal_enter_button));
            for(int i=0;i<kRecalFlowCount;++i)if(shown(i))rects.push_back(client_rect(recal_flow_buttons[i]));
            bool inside=!rects.empty(),apart=true;
            for(const auto& r:rects)inside=inside && r.left>=0 && r.top>=0 && r.right<=area.right && r.bottom<=area.bottom && r.right>r.left && r.bottom>r.top;
            for(size_t i=0;i<rects.size();++i)for(size_t j=i+1;j<rects.size();++j){
                RECT overlap{};if(IntersectRect(&overlap,&rects[i],&rects[j]))apart=false;}
            // The flow row is under the rows of the lower buttons (the title row of the lower half is lower by PreviewLowerShift, and the flow row is 36 under it).
            for(int i=0;i<kRecalFlowCount;++i)if(shown(i))apart=apart && client_rect(recal_flow_buttons[i]).top>=area.bottom/2+kRecalRowTop+PreviewLowerShift(area.right);
            check(inside,what);check(apart,what);
        };
        // What the window held before this change and does not hold at this width (information for the report; not a failure).
        const auto report_old_controls=[&](const char* label){
            RECT area{};GetClientRect(window,&area);
            for(const auto& entry:old_controls){
                const auto r=client_rect(entry.second);
                if(r.right>area.right)Log(std::string("recalibration_ui_old_control_beyond_client at=")+label+" id="+std::to_string(entry.first)+" right="+std::to_string(r.right)+" client_width="+std::to_string(area.right));
            }
        };

        // ---- A picture of the screen: Paint into a bitmap, the controls printed over it ----
        HDC memory=CreateCompatibleDC(nullptr);
        const auto shot=[&](const std::string& name,bool print_controls){
            if(pictures.empty())return;
            RECT area{};GetClientRect(window,&area);
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;info.bmiHeader.biHeight=-area.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            void* bits{};HBITMAP bitmap=CreateDIBSection(memory,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!bitmap)return;
            auto old=SelectObject(memory,bitmap);SetBkMode(memory,TRANSPARENT);SetTextColor(memory,RGB(240,240,240));
            Paint(memory,area);
            if(print_controls){
                const auto print=[&](HWND control){
                    if(!RecalibrationButtonShown(control))return;
                    const auto r=client_rect(control);const auto saved=SaveDC(memory);
                    SetViewportOrgEx(memory,r.left,r.top,nullptr);
                    SendMessageW(control,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
                    RestoreDC(memory,saved);
                };
                for(const auto& entry:old_controls)print(entry.second);
                print(recal_enter_button);for(auto control:recal_flow_buttons)print(control);
            }
            GdiFlush();
            render::BgrImage image{static_cast<unsigned>(area.right),static_cast<unsigned>(area.bottom),{}};
            image.bgr.resize(static_cast<size_t>(image.width)*image.height*3);
            const auto* source=static_cast<const std::uint8_t*>(bits);
            for(size_t i=0,j=0;i<image.bgr.size();i+=3,j+=4)std::copy_n(source+j,3,image.bgr.data()+i);
            try{a0::phase0::experimental::WriteBgrPngNoOverwrite(pictures/(name+".png"),image);}catch(const std::exception&){}
            SelectObject(memory,old);DeleteObject(bitmap);
        };
        // Lit pixels (not black) in a rectangle of a Paint into a bitmap of the window's size.
        const auto lit_pixels=[&](int left,int top,int right,int bottom){
            RECT area{};GetClientRect(window,&area);
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;info.bmiHeader.biHeight=-area.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            void* bits{};HBITMAP bitmap=CreateDIBSection(memory,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!bitmap)return 0u;
            auto old=SelectObject(memory,bitmap);SetBkMode(memory,TRANSPARENT);SetTextColor(memory,RGB(240,240,240));
            Paint(memory,area);GdiFlush();
            unsigned count{};
            const auto* pixels=static_cast<const std::uint32_t*>(bits);
            for(int y=std::max(top,0);y<std::min<int>(bottom,area.bottom);++y)for(int x=std::max(left,0);x<std::min<int>(right,area.right);++x)
                if((pixels[static_cast<size_t>(y)*area.right+x]&0x00FFFFFFu)!=0)++count;
            SelectObject(memory,old);DeleteObject(bitmap);
            return count;
        };

        // ================= 1. At rest: the entrance, and what takes it away =================
        auto settings=scenario("s1");
        check(caption()==PreviewTitleFrame(L"校正あり・改訂 1") && PreviewTitleMarksMatch(caption()),"rest: the caption names the revision in force, with the marks of the build");
        check(RecalibrationButtonShown(recal_enter_button) && IsWindowEnabled(recal_enter_button)!=FALSE,"rest: the entrance is offered");
        check(!shown(kRecalStart) && !shown(kRecalAbort) && !shown(kRecalUse) && !shown(kRecalRetake),"rest: no flow button is shown");
        geometry_ok("rest: the entrance is in the client area");
        report_old_controls("rest-1000");
        shot("1-rest-1000",true);
        set_client_size(1400,960);geometry_ok("rest at 1400: the entrance is in the client area");shot("1-rest-1400",true);set_client_size(1000,960);
        {   // The entrance goes while a pair capture is asked for, while a settings read is open, and when the frames are old.
            capture_pending=2;tick();check(!IsWindowEnabled(recal_enter_button),"rest: not while a capture is pending");
            click(kRecalEnterId,recal_enter_button);check(phase()==RecalPhase::idle,"rest: a click while a capture is pending does nothing");capture_pending.reset();
            settings_close_failed=true;tick();check(!IsWindowEnabled(recal_enter_button),"rest: not after a close that failed");settings_close_failed=false;
            {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now()-std::chrono::seconds(3);}
            tick();check(!IsWindowEnabled(recal_enter_button),"rest: not when a frame is old");
            click(kRecalEnterId,recal_enter_button);check(phase()==RecalPhase::idle,"rest: a click on a frame that is old does nothing");
            fresh();tick();check(IsWindowEnabled(recal_enter_button)!=FALSE,"rest: offered again when the frames are new");
        }

        // ================= 2. The guide, and leaving it =================
        {
            const auto before=snapshot(settings);
            click(kRecalEnterId,recal_enter_button);
            check(phase()==RecalPhase::guide,"guide: the entrance opens it");
            tick();
            check(shown(kRecalStart) && enabled(kRecalStart) && shown(kRecalGuideQuit) && enabled(kRecalGuideQuit) && !shown(kRecalAbort) && !shown(kRecalUse),"guide: the shot and the way out");
            check(!IsWindowEnabled(recal_enter_button),"guide: the entrance is not offered twice");
            check(CanFocus(*cameras[0]) && CanFocus(*cameras[1]),"guide: Live keeps running and AF stays offered");
            geometry_ok("guide: the buttons are in the client area");
            check(lit_pixels(12,mid()+160,988,mid()+470)>1500,"guide: the C area holds the guide");
            shot("2-guide-1000",true);
            set_client_size(500,960);
            check(recal_ui.button_rows==1,"guide at 500: two buttons fit one row");
            set_client_size(1000,960);
            press(kRecalGuideQuit);
            check(phase()==RecalPhase::idle && backend_calls==0 && snapshot(settings)==before,"guide: leaving it takes no shot and changes no file");
            tick();check(!shown(kRecalStart) && IsWindowEnabled(recal_enter_button)!=FALSE,"guide: the entrance is back");
            // No condition v3: the guide says so and the shot is not offered.
            fixed_source={};
            click(kRecalEnterId,recal_enter_button);tick();
            check(phase()==RecalPhase::guide && shown(kRecalStart) && !enabled(kRecalStart),"guide without a condition: the shot is not offered");
            press(kRecalStart);check(phase()==RecalPhase::guide && backend_calls==0 && !capture_pending,"guide without a condition: a direct command does nothing");
            press(kRecalGuideQuit);
            LoadFixedSource();
        }

        // ================= 3. The shot, the computation, an unchecked result, its confirmation =================
        {
            const auto before=snapshot(settings);
            click(kRecalEnterId,recal_enter_button);tick();
            set_gate(false);                                   // the job waits, so that the screen of the computation can be looked at
            fresh();press(kRecalStart);
            check(phase()==RecalPhase::capturing && capture_pending.has_value() && *capture_pending==2 && stopping,"shot: the same request as the pair capture, Live is asked to stop");
            check(backend_calls==0 && !CanFocus(*cameras[0]) && !CanFocus(*cameras[1]),"shot: nothing is taken until Live has stopped");
            tick();check(!IsWindowEnabled(recal_enter_button) && !shown(kRecalStart),"shot: no button while the pair is taken");
            check(lit_pixels(12,mid()+120,988,mid()+300)>300,"shot: the screen says that the pair is being taken");
            finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::computing;},20000),"shot: the pair is taken and the job starts");
            check(backend_calls==1,"shot: the pair backend ran exactly once");
            check(wait_until([&]{return seen_count()>=1;},5000) && seen_count()==1 && [&]{std::lock_guard lock(seen_mutex);return seen[0].live_stopped && seen[0].has_stop_token && seen[0].staging.filename()==L"recalibration";}(),
                "job: it is told that Live is stopped, can be stopped, and works in a folder of the run");
            check(restarts==0 && preview_joined,"computation: Live is not started again by itself");
            tick();tick();
            check(shown(kRecalAbort) && enabled(kRecalAbort) && !shown(kRecalStart) && !shown(kRecalUse),"computation: only the stop button");
            geometry_ok("computation: the stop button is in the client area");
            check(lit_pixels(12,mid()+160,988,mid()+380)>800,"computation: the screen names the stage");
            shot("3-computing-1000",true);
            // While it runs nothing else may start: no AF, no capture, no settings read, no restart by hand.
            check(guard_no_hardware(),"computation: no capture can be asked for");
            check(!CanRestartPreview() && !CanReadSettings() && !CanFocus(*cameras[0]) && !CanFocus(*cameras[1]),"computation: no restart, no settings read, no AF");
            click(405,settings_button);click(403,session_button);
            check(settings_reads==0 && restarts==0 && !settings_future.valid(),"computation: the settings button and the restart button do nothing");
            check(!IsWindowEnabled(session_button) && !IsWindowEnabled(settings_button),"computation: those buttons are off");
            // The real job on the stand-in computation.
            set_gate(true);
            check(wait_until([&]{return phase()==RecalPhase::result;},240000),"job: the result arrives");
            check(recal_ui.outcome.has_value(),"result: there is an outcome");
            if(recal_ui.outcome){
                const auto& o=*recal_ui.outcome;
                check(o.verdict==RecalibrationVerdict::usable_unverified && o.unverified() && o.ready && o.candidate.has_value(),"result: a flat sheet cannot be measured: an unchecked calibration, offered with a warning");
                check(!o.findings.empty() && std::all_of(o.findings.begin(),o.findings.end(),[](const RecalibrationFinding& f){return f.klass==FindingClass::unverified;}),"result: only 'not checked' findings");
                check(!o.screen_lines.empty() && o.screen_lines[0].find("未確認の校正")!=std::string::npos,"result: the first line says it is an unchecked calibration");
                check(fs::exists(o.staging_folder/"recalibration-report.json") && !fs::exists(o.staging_folder/"confirmation.json"),"result: the record is in the run folder and nothing is confirmed");
                base_unchecked=o;canned=o;
            }
            check(snapshot(settings)==before,"result: the settings folder is as it was");
            tick();tick();
            check(shown(kRecalUseUnchecked) && enabled(kRecalUseUnchecked) && !shown(kRecalUse) && shown(kRecalRetake) && shown(kRecalQuit) && !shown(kRecalAbort),"result (unchecked): the warning's button, retake and quit");
            {   // No button is the default one, and the one that uses an unchecked calibration cannot be reached by the Tab key.
                bool plain=(GetWindowLongW(recal_enter_button,GWL_STYLE)&BS_TYPEMASK)==BS_PUSHBUTTON;
                for(auto control:recal_flow_buttons)plain=plain && (GetWindowLongW(control,GWL_STYLE)&BS_TYPEMASK)==BS_PUSHBUTTON;
                check(plain,"result (unchecked): no button is the default button");
                check((GetWindowLongW(recal_flow_buttons[kRecalUseUnchecked],GWL_STYLE)&WS_TABSTOP)==0,"result (unchecked): its button is not on the tab order");
            }
            geometry_ok("result (unchecked): the buttons are in the client area");
            check(lit_pixels(12,176,988,472)>3000,"result: the sample and the crops are drawn in the upper half");
            check(lit_pixels(12,mid()+160,988,940)>1500,"result: the lines are drawn in the lower half");
            report_old_controls("result-1000");
            shot("4-result-unchecked-1000",true);
            set_client_size(1400,960);geometry_ok("result at 1400: the buttons are in the client area");shot("4-result-unchecked-1400",true);
            set_client_size(500,960);
            check(recal_ui.button_rows==2,"result at 500: the buttons go to a second row");
            geometry_ok("result at 500: the buttons are in the client area");
            shot("4b-result-unchecked-500",true);
            set_client_size(1000,960);
            // The checked calibration's button is not there and a direct command for it does nothing.
            click(kRecalFlowFirstId+kRecalUse,recal_flow_buttons[kRecalUse]);tick();
            check(recal_ui.action==RecalAction::none && phase()==RecalPhase::result && snapshot(settings)==before,"result (unchecked): the button for a checked calibration does nothing");
            // The person decides: the warning was seen. The button is off as soon as it was pressed.
            press(kRecalUseUnchecked);
            UpdateRecalibrationControls(window);
            check(!enabled(kRecalUseUnchecked) && !enabled(kRecalRetake) && !enabled(kRecalQuit),"confirm: every button is off once the decision is made");
            tick();
            check(phase()==RecalPhase::idle && restarts==1,"confirm: Live is started again after the decision");
            check(recal_ui.confirmations_written==1 && revision_count(settings)==2 && fs::exists(settings/std::string(m2e::kFixedCompositeRevisionFolderName)/"r000002.json"),"confirm: one revision was written");
            const auto selection=m2e::SelectFixedComposite(settings/m2e::kFixedCompositeConditionName,ps::Hash());
            const auto* now_in_force=selection.loaded?std::get_if<m2e::LoadedFixedCompositeConditionV3>(&*selection.loaded):nullptr;
            check(now_in_force && now_in_force->condition.revision.sequence==2,"confirm: the capture's reader finds the new revision in force");
            check(fixed_source.loaded_v3 && fixed_source.loaded_v3->condition.revision.sequence==2,"confirm: the new session read the new revision");
            check(has(caption(),L"改訂 2") && has(caption(),L"未確認の校正"),"confirm: the caption names the revision and the unchecked mark");
            check(RecalibrationUncheckedInForce(),"confirm: the unchecked mark is on");
            check(has(capture_status,L"未確認の校正として更新しました"),"confirm: the status line says it");
            const auto staging=canned?canned->staging_folder:fs::path{};
            check(fs::exists(staging/"confirmation.json") && ps::ReadText(staging/"confirmation.json").find("\"unverified\":true")!=std::string::npos,"confirm: the record says it is unchecked");
            shot("5-after-unchecked-use-1000",true);
            // One mechanism: what the result screen showed (the sample composed with the candidate), what the live C reads after the decision
            // (the condition of the new session) and what the next saved JPEG uses (the capture's own selection) are the same revision and the
            // same profile, and a composite made the way a capture makes it, on the same originals, is the sample's composite byte for byte.
            if(canned && canned->candidate){
                const auto& candidate=*canned->candidate;
                check(fixed_source.loaded_v3 && fixed_source.loaded_v3->file_sha256==candidate.sha256 &&
                    fixed_source.loaded_v3->profile.profile_file_sha256==candidate.condition.source.profile_file_sha256,"same: the live C reads the candidate that was shown");
                check(now_in_force && now_in_force->file_sha256==candidate.sha256 && now_in_force->profile.profile_file_sha256==candidate.condition.source.profile_file_sha256,
                    "same: the capture's reader reads the candidate that was shown");
                ReviewGeometry geometry{460,307,229,3,11,candidate.condition.orientation};
                m2e::StillReviewRequest request{frames.a,frames.b,work/"post-review",ps::Sha(frames.a),ps::Sha(frames.b),NewRunId(),geometry,Clock::now()+std::chrono::seconds(900)};
                auto composed=ComposePairWithSelection(selection,std::move(request),merge_policy);
                check(composed.review && composed.review->fixed_composite_registration && composed.review->fixed_composite_registration->condition_sha256==candidate.sha256 &&
                    composed.review->fixed_composite_registration->seam_column==candidate.condition.seam.column,"same: a capture after the decision composes with the candidate's revision and seam");
                const auto sample_jpeg=staging/"review"/"uncalibrated-review.jpg";
                check(composed.review && fs::exists(sample_jpeg) && ps::Sha(composed.review->jpeg)==ps::Sha(sample_jpeg),"same: that composite is the sample, byte for byte");
            }
            // After a restart of the program: the mark is read back from the record.
            recal_ui.confirmed_unchecked_sha.clear();recal_ui.scanned_sha.clear();recal_ui.scanned_unchecked=false;
            LoadFixedSource();
            check(RecalibrationUncheckedInForce() && has(caption(),L"未確認の校正"),"restart: the mark comes back from the record");
            // No record to read (or one for another revision): nothing is shown.
            std::error_code ec;fs::rename(staging/"confirmation.json",staging/"confirmation.json.moved",ec);
            recal_ui.scanned_sha.clear();recal_ui.scanned_unchecked=false;
            LoadFixedSource();
            check(!RecalibrationUncheckedInForce() && !has(caption(),L"未確認の校正"),"restart: without the record no mark is shown");
            fs::rename(staging/"confirmation.json.moved",staging/"confirmation.json",ec);
            {   // A record that cannot be trusted is not shown either: not JSON, the wrong schema, a larger file.
                const auto text=ps::ReadText(staging/"confirmation.json");
                const auto bad=[&](const std::string& content){
                    ps::WriteText(staging/"confirmation.json",content);recal_ui.scanned_sha.clear();recal_ui.scanned_unchecked=false;LoadFixedSource();return RecalibrationUncheckedInForce();};
                check(!bad("not json"),"restart: a record that is not JSON shows nothing");
                std::string wrong=text;wrong.replace(wrong.find("a0.pair-recalibration-confirmation.v1"),std::string("a0.pair-recalibration-confirmation.v1").size(),"a0.other.v1");
                check(!bad(wrong),"restart: a record of another schema shows nothing");
                check(!bad(text+std::string(5000,' ')),"restart: a record that is too large shows nothing");
                check(bad(text),"restart: the record, put back, shows the mark again");
            }
            // The earlier revision, if it came back, would not carry the mark (the mark follows the SHA-256 of the revision in force).
            check(recal_ui.scanned_sha==fixed_source.loaded_v3->file_sha256,"restart: the mark is kept for the revision it was looked up for");
        }

        // ================= 4. A checked result: the plain button; quit changes nothing =================
        {
            settings=scenario("s4");
            const auto before=snapshot(settings);
            auto checked=*base_unchecked;
            checked.verdict=RecalibrationVerdict::usable;checked.ready=true;checked.findings.clear();
            checked.screen_lines=RecalibrationScreenLines(checked.verdict,checked.findings,checked.facts);
            canned=checked;
            const unsigned calls_before=backend_calls,restarts_before=restarts,confirmed_before=recal_ui.confirmations_written;
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},60000),"checked: the result arrives");
            tick();tick();
            check(backend_calls==calls_before+1,"checked: one pair was taken");
            check(shown(kRecalUse) && enabled(kRecalUse) && !shown(kRecalUseUnchecked) && shown(kRecalRetake) && shown(kRecalQuit),"checked: the plain button, retake and quit");
            geometry_ok("checked: the buttons are in the client area");
            shot("6-result-checked-1000",true);
            // Quit: nothing written, Live again.
            press(kRecalQuit);tick();
            check(phase()==RecalPhase::idle && restarts==restarts_before+1 && snapshot(settings)==before && recal_ui.confirmations_written==confirmed_before,"quit: nothing is written and Live is started again");
            check(has(capture_status,L"前の校正のまま"),"quit: the status line says the earlier calibration stays");
            // The same result, used: a checked calibration carries no mark.
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},60000),"checked 2: the result arrives");
            tick();press(kRecalUse);tick();
            check(phase()==RecalPhase::idle && revision_count(settings)==2 && fixed_source.loaded_v3 && fixed_source.loaded_v3->condition.revision.sequence==2,"checked 2: one revision, read by the new session");
            check(!RecalibrationUncheckedInForce() && !has(caption(),L"未確認の校正") && has(caption(),L"改訂 2"),"checked 2: no unchecked mark");
            check(has(capture_status,L"校正を更新しました") && !has(capture_status,L"未確認の校正として"),"checked 2: the status line");
            check(recal_ui.confirmations_written==confirmed_before+1,"checked 2: one confirmation");
        }

        // ================= 5. Not usable: nothing can be saved =================
        {
            settings=scenario("s5");
            const auto before=snapshot(settings);
            const unsigned confirmed_before=recal_ui.confirmations_written;
            auto bad=*base_unchecked;
            bad.thumbnail={};bad.seam_crops.reset();   // a result without the sample and the crops: the screen says so and does not fail
            bad.verdict=RecalibrationVerdict::unusable;bad.ready=false;bad.candidate.reset();
            bad.findings={{FindingClass::rejected,"reestimate-displacement",""}};
            bad.screen_lines=RecalibrationScreenLines(bad.verdict,bad.findings,bad.facts);
            canned=bad;
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},60000),"unusable: the result arrives");
            tick();tick();
            check(!shown(kRecalUse) && !shown(kRecalUseUnchecked) && shown(kRecalRetake) && shown(kRecalQuit),"unusable: no button saves it");
            geometry_ok("unusable: the buttons are in the client area");
            click(kRecalFlowFirstId+kRecalUse,recal_flow_buttons[kRecalUse]);click(kRecalFlowFirstId+kRecalUseUnchecked,recal_flow_buttons[kRecalUseUnchecked]);tick();
            check(recal_ui.action==RecalAction::none && phase()==RecalPhase::result && snapshot(settings)==before,"unusable: a direct command for either button does nothing");
            check(lit_pixels(12,mid()+160,988,940)>1500,"unusable: the reason and the next step are drawn");
            shot("7-result-unusable-1000",true);
            // Even if the action were set, the confirmation refuses it and writes nothing.
            recal_ui.action=RecalAction::use_unchecked;tick();
            check(snapshot(settings)==before && recal_ui.confirmations_written==confirmed_before && has(capture_status,L"使えない結果"),"unusable: even a forced decision writes nothing and says why");
            check(phase()==RecalPhase::idle,"unusable: the decision ends the flow");
        }
        {   // A caller's mistake of the job is not shown as the person's: the screen says that nothing was started.
            settings=scenario("s5b");
            auto mistaken=*base_unchecked;
            mistaken.thumbnail={};mistaken.seam_crops.reset();
            mistaken.verdict=RecalibrationVerdict::unusable;mistaken.ready=false;mistaken.candidate.reset();
            mistaken.findings={{FindingClass::no_candidate,"staging-exists",""}};
            mistaken.screen_lines=RecalibrationScreenLines(mistaken.verdict,mistaken.findings,mistaken.facts);
            const auto shown_lines=RecalibrationDisplayLines(mistaken);
            check(shown_lines.lines.size()==2 && shown_lines.lines[1].find("新しいフォルダ")==std::string::npos && shown_lines.lines[1].find("技術担当")!=std::string::npos,"mistake: the sentence for a caller is not shown");
            mistaken.findings={{FindingClass::no_candidate,"live-not-stopped",""}};
            check(RecalibrationDisplayLines(mistaken).lines.size()==2,"mistake: nor is the other one");
            mistaken.findings={{FindingClass::no_candidate,"staging-in-settings",""}};
            check(RecalibrationDisplayLines(mistaken).lines.size()==2,"mistake: nor is the one for a folder among the settings");
        }

        // ================= 6. Retake, and the condition that changed while the result was on the screen =================
        {
            settings=scenario("s6");
            canned=*base_unchecked;
            const unsigned calls_before=backend_calls,restarts_before=restarts,confirmed_before=recal_ui.confirmations_written;
            const auto before=snapshot(settings);
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},60000),"retake: the first result arrives");
            tick();press(kRecalRetake);tick();
            check(phase()==RecalPhase::guide && restarts==restarts_before+1 && snapshot(settings)==before,"retake: Live again and the guide is back, nothing written");
            tick();check(shown(kRecalStart) && enabled(kRecalStart),"retake: the shot is offered again");
            fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},60000),"retake: the second result arrives");
            check(backend_calls==calls_before+2,"retake: a second pair was taken, once");
            tick();
            // While the result is on the screen another revision is made (a hand adjustment, say): the decision is refused, nothing is written.
            {
                const auto revisions=settings/std::string(m2e::kFixedCompositeRevisionFolderName);
                const auto second=ps::SampleCondition(settings,rig,m2e::MountOrientation::normal,raster,2,ps::Sha(revisions/"r000001.json"),m2e::FixedCompositeRevisionKind::values);
                m2e::WriteFixedCompositeConditionV3(revisions,second);
            }
            const auto after_other=snapshot(settings);
            const std::string pressed_candidate=recal_ui.outcome && recal_ui.outcome->candidate?recal_ui.outcome->candidate->sha256:std::string{};
            press(kRecalUseUnchecked);tick();
            check(!pressed_candidate.empty() && recal_ui.confirmed_unchecked_sha==pressed_candidate && !RecalibrationUncheckedInForce(),"changed: the unchecked mark is remembered from the press, and is not shown for a revision that is not that candidate");
            check(recal_ui.confirmations_written==confirmed_before && snapshot(settings)==after_other,"changed: nothing is written over a revision that appeared meanwhile");
            check(phase()==RecalPhase::idle && has(capture_status,L"条件が変わっています") && has(capture_status,L"いま有効な校正は改訂 2 です"),"changed: the status says so and names the revision in force");
            check(fixed_source.loaded_v3 && fixed_source.loaded_v3->condition.revision.sequence==2,"changed: Live read the revision that is in force");
        }

        // ================= 7. Stop during the computation; a shot that fails; a shot that does not start; closing the window =================
        {
            settings=scenario("s7");
            canned=stopped_outcome();
            const auto before=snapshot(settings);
            set_gate(false);
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::computing;},20000),"stop: the job runs");
            const auto restarts_before=restarts;
            press(kRecalAbort);
            check(recal_ui.abort_requested,"stop: asked");
            tick();check(!enabled(kRecalAbort),"stop: the button is off once asked");
            check(wait_until([&]{return phase()==RecalPhase::idle;},20000),"stop: the job ends and the screen leaves the computation");
            check(restarts==restarts_before+1 && snapshot(settings)==before && has(capture_status,L"中止しました") && has(capture_status,L"前の校正のまま"),"stop: Live again, nothing written, the status says so");
            tick();check(IsWindowEnabled(recal_enter_button)!=FALSE,"stop: the entrance is back");
            set_gate(true);
        }
        {   // The shot fails: no job, the failure is handled as the pair capture handles it (no retry, no restart).
            settings=scenario("s8");
            backend_fails=true;
            const auto jobs_before=recal_ui.jobs_started,calls_before=backend_calls;
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::idle && !capture_future.valid() && capture_restart_blocked;},20000),"failed shot: the calibration does not begin and the capture failure is kept");
            check(backend_calls==calls_before+1 && recal_ui.jobs_started==jobs_before && !recal_ui.job.valid(),"failed shot: one try, no job");
            check(has(capture_status,L"再撮影せず停止中"),"failed shot: the status line is the capture's own");
            tick();check(!IsWindowEnabled(recal_enter_button),"failed shot: nothing more is offered until the program is started again");
            backend_fails=false;
        }
        // The same words as the pair capture says for the same failure: one function fills both (RecordIncompletePair), and AdvanceCapture words it.
        for(const int kind:{0,1}){
            settings=scenario(kind==0?"s8b":"s8c");
            backend_preflight_stop=kind==0;backend_late_image=kind==1;
            VerifiedStillPair expected;
            if(kind==0){for(int i=0;i<2;++i){expected.preflight_checked[i]=true;expected.preflight[i].closed=true;expected.preflight[i].kind=StillPreflightKind::Event;expected.preflight[i].event_code=0xc102;}}
            else expected.originals[1].late_object_event=true;
            const std::wstring want=kind==0?DescribeStillPairPreflightRefusal(expected):DescribeStillPairLateObjects(expected);
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::idle && !capture_future.valid() && capture_restart_blocked;},20000),"failed shot with a notice: the failure is kept");
            check(!want.empty() && has(capture_status,want.c_str()),kind==0?"failed shot: the preflight text of the pair capture is shown":"failed shot: the leftover image notice of the pair capture is shown");
            backend_preflight_stop=false;backend_late_image=false;
        }
        {   // Live could not be confirmed closed: the capture does not start and the calibration does not wait for it.
            settings=scenario("s9");
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);
            cameras[0]->started=false; // FinishPreview: not a safe end
            finish_workers();
            const auto calls_before=backend_calls;
            check(wait_until([&]{return phase()==RecalPhase::idle;},5000),"unsafe stop: the calibration does not wait for a capture that was not started");
            check(backend_calls==calls_before && !capture_future.valid(),"unsafe stop: no shutter");
        }
        {   // The run's folder would stand among the settings: nothing is run, and the screen says that nothing was calculated.
            settings=scenario("s11");
            pair_review_root_override=settings/"inside";
            const auto jobs_before=recal_ui.jobs_started;
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::result;},20000),"inside the settings: the result screen comes up");
            tick();tick();
            check(recal_ui.jobs_started==jobs_before && !recal_ui.job.valid() && !shown(kRecalUse) && !shown(kRecalUseUnchecked) && shown(kRecalRetake) && shown(kRecalQuit),"inside the settings: no job, nothing can be saved");
            press(kRecalQuit);tick();
            check(phase()==RecalPhase::idle && revision_count(settings)==1,"inside the settings: leaving changes no revision");
            pair_review_root_override=work/"reviews";
            std::error_code remove_ec;fs::remove_all(settings/"inside",remove_ec);
        }
        {   // Closing the window while the job runs: the job is asked to stop and the window waits for it.
            settings=scenario("s10");
            canned=stopped_outcome();
            set_gate(false);
            click(kRecalEnterId,recal_enter_button);tick();fresh();press(kRecalStart);finish_workers();
            check(wait_until([&]{return phase()==RecalPhase::computing;},20000),"close: the job runs");
            SendMessageW(window,WM_CLOSE,0,0);
            check(closing && recal_ui.job.valid() ,"close: the window waits for the job");
            check(wait_until([&]{return !IsWindow(window);},20000),"close: the job ended and the window went");
            window=nullptr;
        }
        // The settings folder of the machine was never opened; nothing was shot.
        check(settings_reads==0,"no settings read was asked for");
    }catch(const std::exception& error){
        ++failures;Log(std::string("recalibration_ui_exception: ")+error.what());
    }
    if(window && IsWindow(window))DestroyWindow(window);
    preview_window=nullptr;
    recalibration_settings_override.reset();pair_review_root_override.reset();preview_restart_override=nullptr;
    pair_capture_backend=[](const std::array<std::wstring,2>& ids,Clock::time_point deadline){return CaptureStillPairShutterFirst(ids,deadline);};
    settings_backend=RealSettingsBackend;
    recal_ui.runner=nullptr;
    std::error_code ignored;fs::remove_all(work,ignored);
    Log("recalibration_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0 settings_written_to_machine_folder=0");
    return failures?1:0;
}
