// Private Win32 host for the development preview. Included after Camera.
std::array<std::unique_ptr<Camera>,2> cameras;
std::array<std::jthread,2> camera_workers;
std::jthread merge_worker;
std::array<std::wstring,2> registered_ids;
Clock::time_point run_start;
int run_seconds=60;
bool closing=false,preview_joined=false,all_sessions_passed=true;
std::array<std::array<HWND,3>,2> focus_buttons{};
std::array<HWND,2> capture_buttons{};
std::array<HWND,5> merge_buttons{};
HWND pair_button{},review_button{},inspect_button{},session_button{},seam_button{};
bool focus_smoke=false,capture_smoke=false,merge_smoke=false,pair_smoke=false,level_smoke=false;
bool restart_smoke=false,capture_restart_blocked=false;
bool manual_restart_waiting=false;
unsigned restart_smoke_next{},manual_preview_stops{},manual_preview_restarts{};
bool pair_cycle_smoke=false;
bool camera_end_restart_smoke=false;
PreviewEndRestartProbe camera_end_probe;
bool af_prepare_smoke=false;unsigned af_prepare_next{};
PairCycleProbe pair_cycle_probe;
std::vector<std::filesystem::path> pair_cycle_reports;
size_t smoke_next{},capture_smoke_next{},merge_smoke_next{};
unsigned pair_smoke_next{},pair_reviews{},review_painted{};
unsigned captures_saved{},preview_sessions{};
std::optional<int> capture_once_alias,capture_pending;
// composite_skip: the originals were saved but no composite was made (#400); the text is shown on screen and in the evidence record.
struct CaptureUiOutcome {StillCaptureOutcome capture;std::optional<a0::m2::experimental::StillReviewResult> review;bool preflight_refused{};
    std::optional<a0::m2::experimental::FixedCompositeSkip> composite_skip;std::string composite_detail;std::array<std::filesystem::path,2> originals;};
std::future<CaptureUiOutcome> capture_future;
using ReviewGeometry=a0::m2::experimental::PreviewReviewGeometry;
ReviewGeometry pending_geometry;
// Fixed before any worker starts; C and the saved Merge use the same frame.
a0::m2::experimental::MountOrientation mount_orientation{};
// Read once at startup (#365). Absent: values are recorded, nothing is corrected.
std::optional<a0::m2::experimental::StillMergePolicy> merge_policy;
Clock::time_point capture_deadline;
Pixels review_pixels;bool review_visible=false;
std::filesystem::path review_path;
std::wstring capture_status=L"撮影すると両Liveを停止 → 選択カメラを撮影・PC保存 → 両Liveを再開";
struct MergeView {
    std::mutex mutex;
    Pixels pixels;
    Clock::time_point source_a{},source_b{};
    std::uint64_t frames{},painted{},paint_sequence{},revision{},rendered_revision{};
    unsigned overlap_pixels{},source_width{},source_height{};int vertical_pixels{};
    bool auto_requested{},auto_busy{};
    // How C is drawn (#401). decided: the merge worker has chosen for the current frame size.
    // fixed_plan is the plan in use (fixed_composite) or the reason C is not drawn (unavailable).
    bool decided{};PreviewCMode mode{PreviewCMode::uncalibrated};FixedPreviewPlan fixed_plan;
    bool show_seam=true;std::uint32_t seam_column{}; // seam_column: in the pixels of `pixels`, set with them
    double ratio_minimum{1},ratio_maximum{1};
    std::wstring position_refusal;
    std::wstring error,alignment_status=L"自動位置合わせで候補を作成 → 手動で微調整（要目視確認）";
} merged;
// The fixed composite condition beside the settings (#401). Written only by LoadFixedSource, on the UI
// thread while no merge worker exists; the merge worker and the UI only read it.
FixedPreviewSource fixed_source;
bool FixedConditionPresent(){return fixed_source.loaded.has_value() || fixed_source.reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent;}
// Read again at every preview session start, so a condition made between sessions is used.
void LoadFixedSource(){
    try{
        auto file=DefaultIdentityMapPath();file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
        fixed_source=LoadFixedPreviewSource(file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
    }catch(const std::exception& error){
        fixed_source={};fixed_source.reason=a0::m2::experimental::FixedCompositeSkip::condition_invalid;fixed_source.detail=error.what();
    }
    std::ostringstream line;line.imbue(std::locale::classic());
    line<<"C fixed_composite_condition="<<(fixed_source.loaded?"present":fixed_source.reason==a0::m2::experimental::FixedCompositeSkip::condition_absent?"absent":"invalid");
    if(fixed_source.loaded){const auto& c=fixed_source.loaded->condition;
        line<<" sha256="<<fixed_source.loaded->file_sha256<<" seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" approval=unapproved";}
    Log(line.str());
}
void LogPreviewChoice(const FixedPreviewChoice& choice){
    std::ostringstream line;line.imbue(std::locale::classic());
    const auto& plan=choice.plan;
    line<<"C composite mode="<<(choice.mode==PreviewCMode::fixed_composite?"fixed-composite":choice.mode==PreviewCMode::unavailable?"unavailable":"uncalibrated")
        <<" view="<<plan.view_width<<"x"<<plan.view_height;
    if(choice.mode==PreviewCMode::fixed_composite)
        line<<" capture_seam_column="<<plan.capture_seam_column<<" scaled_seam_column="<<plan.scaled_seam_column<<" seam_column="<<plan.view.seam_column<<" seam_shift="<<plan.seam_shift
            <<" blend_width="<<plan.settings.width_pixels<<" window_half_width="<<plan.settings.brightness_half_width_pixels<<" smoothing_rows="<<plan.settings.brightness_smoothing_rows
            <<" scale_correction=0 correspondence_verified=0 uncalibrated=1 quality_accepted=0";
    else if(choice.mode==PreviewCMode::unavailable)line<<" reason="<<a0::m2::experimental::FixedCompositeSkipCode(*plan.skip);
    Log(line.str());
}
bool CanFocus(Camera& camera){
    std::lock_guard lock(camera.mutex);
    return !stopping && !closing && !capture_pending && !capture_future.valid() && !camera.done && !camera.failed && camera.frames>=20 &&
        Clock::now()-camera.last<std::chrono::seconds(2) && camera.focus_mailbox.Available();
}
bool CanRestartPreview(){
    if(!preview_joined)return false; // All following non-atomic state is joined.
    std::array<PreviewClosedState,2> state;
    for(size_t i=0;i<2;++i){const auto& c=*cameras[i];state[i]={c.opened,c.start_attempted,c.stopped,c.closed};}
    return PreviewRestartAllowed(preview_joined,closing,cleanup_only,
        capture_pending.has_value() || capture_future.valid() || capture_restart_blocked,state);
}
PairCycleObservation ObservePairCycle(){
    PairCycleObservation view{!all_sessions_passed || capture_restart_blocked,
        !capture_pending && !capture_future.valid() && !closing && !stopping,
        CanFocus(*cameras[0]) && CanFocus(*cameras[1]),false,preview_sessions,pair_reviews,captures_saved,review_painted,0};
    std::array<Clock::time_point,2> first,last;
    for(size_t i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);
        view.failed=view.failed || c.failed;view.live_ready=view.live_ready && c.frames>=20 && c.painted>=10;
        first[i]=c.first;last[i]=c.last;
    }
    view.live_ready=view.live_ready && std::min(last[0],last[1])-std::max(first[0],first[1])>=std::chrono::milliseconds(2500);
    std::lock_guard lock(merged.mutex);view.merge_paints=merged.painted;
    view.merge_ready=!merged.pixels.bytes.empty() && merged.rendered_revision==merged.revision && PreviewPairFresh(Clock::now(),merged.source_a,merged.source_b);
    return view;
}
PreviewEndRestartObservation ObserveCameraEndRestart(){
    PreviewEndRestartObservation view;view.sessions=preview_sessions;view.joined=preview_joined;
    view.restart_allowed=CanRestartPreview();view.live_fresh=!stopping && CanFocus(*cameras[0]) && CanFocus(*cameras[1]);
    for(const auto& camera:cameras){
        std::lock_guard lock(camera->mutex);
        // camera_ended is written before done; only read it after worker join.
        if(preview_joined){view.camera_ended=view.camera_ended || camera->camera_ended;
            view.unexpected_failure=view.unexpected_failure || (camera->failed && !camera->camera_ended) ||
                !camera->closed || (camera->start_attempted && !camera->stopped);}
        view.live_fresh=view.live_fresh && camera->frames>=20 && camera->painted>=10;
    }
    std::lock_guard lock(merged.mutex);view.merge_paints=merged.painted;
    view.merge_fresh=!stopping && merged.rendered_revision==merged.revision && !merged.pixels.bytes.empty() && PreviewPairFresh(Clock::now(),merged.source_a,merged.source_b);
    return view;
}
void ClearMerge(){std::lock_guard lock(merged.mutex);merged.pixels={};merged.source_a={};merged.source_b={};merged.auto_requested=false;merged.auto_busy=false;merged.decided=false;merged.position_refusal.clear();}
Pixels ToPixels(const a0::m2::render::BgrImage& image){
    Pixels next{image.width,image.height,{}};next.bytes.resize(static_cast<size_t>(next.width)*next.height*4);
    for(size_t i=0,j=0;i<image.bgr.size();i+=3,j+=4)std::copy_n(image.bgr.data()+i,3,next.bytes.data()+j);
    return next;
}
a0::m2::render::BgrImage SmallBgr(const Pixels& p){
    // Bound C's CPU work independently of A/B acquisition and display.
    const auto divisor=std::max(1u,(std::max(p.width,p.height)+639)/640);
    a0::m2::render::BgrImage result{p.width/divisor,p.height/divisor,{}};
    result.bgr.resize(static_cast<size_t>(result.width)*result.height*3);
    for(UINT y=0;y<result.height;++y)for(UINT x=0;x<result.width;++x){
        const auto source=(static_cast<size_t>(y*divisor)*p.width+x*divisor)*4;
        const auto target=(static_cast<size_t>(y)*result.width+x)*3;
        std::copy_n(p.bytes.data()+source,3,result.bgr.data()+target);
    }
    return result;
}
// One frame pair of C when a fixed composite condition exists (#401). Display only. fixed_composite draws
// the pair with the scaled condition and measures the brightness ratio from these two frames; unavailable
// draws nothing and leaves the reason in merged.error. The pair still counts as seen (sources and revision
// are published), so the pair capture, which does not depend on C's picture, stays available.
void PublishFixedC(const FixedPreviewChoice& choice,const a0::m2::render::BgrImage& a,const a0::m2::render::BgrImage& b,
    Clock::time_point time_a,Clock::time_point time_b,std::uint64_t revision,const std::stop_token& stop){
    const auto current=[&]{return !stopping && !stop.stop_requested() && merged.revision==revision && PreviewPairFresh(Clock::now(),time_a,time_b);};
    if(choice.mode==PreviewCMode::unavailable){
        std::lock_guard lock(merged.mutex);
        if(current()){merged.pixels={};merged.source_a=time_a;merged.source_b=time_b;merged.rendered_revision=revision;merged.error=FixedPreviewUnavailableText(*choice.plan.skip);}
        return;
    }
    try{
        auto drawn=RenderFixedPreview(choice.plan,a,b);
        auto next=ToPixels(drawn.image);
        std::lock_guard lock(merged.mutex);
        if(current()){
            merged.pixels=std::move(next);merged.source_a=time_a;merged.source_b=time_b;merged.rendered_revision=revision;++merged.frames;merged.error.clear();
            merged.ratio_minimum=drawn.record.ratio_minimum;merged.ratio_maximum=drawn.record.ratio_maximum;merged.seam_column=choice.plan.view.seam_column;
        }
    }catch(const std::exception&){
        std::lock_guard lock(merged.mutex);merged.pixels={};
        merged.error=L"固定の合成条件でCを描けませんでした。A/Bのフレームの大きさを確認してください";
    }
}
void MergeLoop(std::stop_token stop){
    std::uint64_t last_a{},last_b{},last_revision=~std::uint64_t{};
    std::optional<FixedPreviewChoice> choice;unsigned choice_width{},choice_height{};
    while(!stop.stop_requested() && !stopping){
        Pixels a,b;Clock::time_point time_a{},time_b{};std::uint64_t sequence_a{},sequence_b{},revision{};unsigned overlap{};int vertical{};
        {std::lock_guard lock(merged.mutex);overlap=merged.overlap_pixels;vertical=merged.vertical_pixels;revision=merged.revision;}
        // Both snapshots have their own timestamps. Never hold camera locks
        // during rendering or retain a raw pointer across a session restart.
        for(int i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);
            if(i==0){a=c.pixels;time_a=c.last;sequence_a=c.frames;}else{b=c.pixels;time_b=c.last;sequence_b=c.frames;}}
        if(PreviewPairFresh(Clock::now(),time_a,time_b) && !a.bytes.empty() && !b.bytes.empty() &&
            (sequence_a!=last_a || sequence_b!=last_b || revision!=last_revision)){
            try{
                auto small_a=SmallBgr(a),small_b=SmallBgr(b);
                // Proposal, manual shifts and render all share the oriented frame.
                a0::m2::experimental::OrientRenderInput(small_a,mount_orientation);a0::m2::experimental::OrientRenderInput(small_b,mount_orientation);
                // #401: how C is drawn depends on the condition file and the frame size, not on the frame content.
                if(!choice || choice_width!=small_a.width || choice_height!=small_a.height){
                    choice=ChoosePreviewC(fixed_source,mount_orientation,small_a.width,small_a.height);choice_width=small_a.width;choice_height=small_a.height;
                    LogPreviewChoice(*choice);
                }
                bool align=false;
                {std::lock_guard lock(merged.mutex);
                    merged.source_width=small_a.width;merged.source_height=small_a.height;
                    if(!merged.overlap_pixels)merged.overlap_pixels=small_a.width*30/100;
                    overlap=merged.overlap_pixels;
                    merged.decided=true;merged.mode=choice->mode;merged.fixed_plan=choice->plan;
                    // With a fixed condition no alignment request moves C (#401): nothing is estimated or applied.
                    if(choice->mode!=PreviewCMode::uncalibrated){merged.auto_requested=false;merged.auto_busy=false;}
                    else if(merged.auto_requested && merged.revision==revision){merged.auto_requested=false;align=true;}
                }
                if(choice->mode!=PreviewCMode::uncalibrated){
                    PublishFixedC(*choice,small_a,small_b,time_a,time_b,revision,stop);
                    last_a=sequence_a;last_b=sequence_b;last_revision=revision;
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    continue;
                }
                if(align){
                    using namespace a0::m2::experimental;
                    const auto estimate=EstimateOverlapTranslation(small_a,small_b);
                    std::lock_guard lock(merged.mutex);merged.auto_busy=false;
                    if(merged.revision==revision && !stopping && PreviewPairFresh(Clock::now(),time_a,time_b)){
                        // One diagnostic line per estimate; an undetermined result never moves C.
                        std::ostringstream s;s<<"C auto_overlap_v2 status="<<OverlapEstimateStatusCode(estimate.status)<<" reason="<<OverlapUndeterminedReasonCode(estimate.reason)
                            <<" orientation="<<a0::m2::experimental::MountOrientationName(mount_orientation)<<" levels="<<estimate.levels<<" candidates="<<estimate.candidates.size();
                        if(!estimate.candidates.empty()){const auto& c=estimate.candidates[0];
                            s<<" best_overlap_px="<<c.overlap_pixels<<" best_vertical_px="<<c.vertical_pixels<<" best_correlation="<<c.correlation<<" best_evidence="<<c.evidence<<" best_samples="<<c.samples;}
                        if(const auto* runner_up=estimate.RunnerUp()){const auto& c=*runner_up;
                            s<<" second_overlap_px="<<c.overlap_pixels<<" second_vertical_px="<<c.vertical_pixels<<" second_evidence="<<c.evidence<<" evidence_gap="<<estimate.candidates[0].evidence-c.evidence;}
                        if(estimate.reverse)s<<" reverse_overlap_px="<<estimate.reverse->overlap_pixels<<" reverse_vertical_px="<<estimate.reverse->vertical_pixels;
                        s<<" applied="<<(estimate.Applicable()?1:0)<<" quality_accepted=0";Log(s.str());
                        if(const auto* candidate=estimate.Applicable()){
                            merged.overlap_pixels=candidate->overlap_pixels;merged.vertical_pixels=candidate->vertical_pixels;++merged.revision;
                            overlap=merged.overlap_pixels;vertical=merged.vertical_pixels;revision=merged.revision;
                            merged.alignment_status=L"自動位置を適用（基準による判定なし・要目視確認） / 相関 "+std::to_wstring(candidate->correlation);
                            Log("C auto_translation_proposal=1 overlap_px="+std::to_string(overlap)+" vertical_px="+std::to_string(vertical)+" correlation="+std::to_string(candidate->correlation)+" quality_accepted=0");
                        }else merged.alignment_status=std::wstring(L"自動位置を決められません（")+OverlapUndeterminedLabel(estimate.reason)+L"）。Cの位置は変えていません。模様・重なり・取付の向きを確認し手動調整してください";
                    }else if(merged.revision==revision){
                        // A manual change already replaced the status; a stale or
                        // stopping pair must not leave "computing" on screen.
                        Log(std::string("C auto_overlap_v2 discarded=")+(stopping?"stopping":"stale"));
                        merged.alignment_status=stopping?L"停止したため適用していません":L"映像が古くなったため適用していません。もう一度押してください";
                    }
                }
                auto result=RenderPreviewMerge(small_a,small_b,overlap,vertical);
                auto next=ToPixels(result.image);
                std::lock_guard lock(merged.mutex);
                if(!stopping && !stop.stop_requested() && merged.revision==revision && PreviewPairFresh(Clock::now(),time_a,time_b)){
                    merged.pixels=std::move(next);merged.source_a=time_a;merged.source_b=time_b;merged.rendered_revision=revision;++merged.frames;merged.error.clear();
                }
            }catch(const std::exception&){std::lock_guard lock(merged.mutex);merged.pixels={};merged.auto_busy=false;merged.error=L"Cの合成条件が合いません。A/Bの画角・上下位置を確認してください";}
            last_a=sequence_a;last_b=sequence_b;last_revision=revision;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}
void StartPreview(){
    if(capture_future.valid() || camera_workers[0].joinable() || camera_workers[1].joinable() || merge_worker.joinable())throw std::runtime_error("preview workers must be closed before restart");
    stopping=false;camera_end_observed=false;ready_count=0;preview_joined=false;ClearMerge();
    if(!cleanup_only)LoadFixedSource(); // before the merge worker exists (#401)
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();cameras[i]->id=registered_ids[i];cameras[i]->alias=i==0?"CAM-A":"CAM-B";}
    camera_workers[0]=std::jthread([]{cameras[0]->Run();});
    try{camera_workers[1]=std::jthread([]{cameras[1]->Run();});}
    catch(...){stopping=true;shutdown_stages.MissingWorker();camera_workers[0].join();throw;}
    if(!cleanup_only){
        try{merge_worker=std::jthread(MergeLoop);}
        catch(...){stopping=true;camera_workers[0].join();camera_workers[1].join();throw;}
    }
    ++preview_sessions;Log("preview_session="+std::to_string(preview_sessions)+" fresh_channels=1");
}
bool FinishPreview(){
    for(auto& worker:camera_workers)if(worker.joinable())worker.join();
    if(merge_worker.joinable()){merge_worker.request_stop();merge_worker.join();}ClearMerge();preview_joined=true;
    bool passed=true;
    for(const auto& camera:cameras){const auto& c=*camera;std::ostringstream s;
        s<<c.alias<<" frames="<<c.frames<<" painted="<<c.painted<<" changed="<<c.changed<<" started="<<c.started<<" stopped="<<c.stopped<<" closed="<<c.closed<<" failed="<<c.failed;
        if(c.frames>1)s<<" fps="<<(c.frames-1)/std::chrono::duration<double>(c.last-c.first).count();
        s<<" focus_requests="<<c.focus_requests<<" af_ok="<<c.focus_success[0]<<" near_ok="<<c.focus_success[1]<<" far_ok="<<c.focus_success[2]<<" live_off_confirmed="<<c.live_off_confirmed
         <<" level_frames="<<c.level_frames<<" level_missing="<<c.level_missing<<" level_painted="<<c.level_painted;Log(s.str());
        if(cleanup_only)passed=passed && !c.failed && c.closed && c.live_off_confirmed;
        else passed=passed && !c.failed && c.frames>=20 && c.painted>=10 && c.started && c.stopped && c.closed;
        if(focus_smoke)passed=passed && c.focus_success[0]>=1 && c.focus_success[1]>=1 && c.focus_success[2]>=1;
        if(level_smoke)passed=passed && c.level_frames>=20 && c.level_painted>=10;
    }
    const auto overlap=std::chrono::duration<double>(std::min(cameras[0]->last,cameras[1]->last)-std::max(cameras[0]->first,cameras[1]->first)).count();
    if(!cleanup_only)passed=passed && overlap>=2;
    Log("simultaneous_overlap_seconds="+std::to_string(overlap)+" result="+(passed?"PASS":"FAIL"));
    all_sessions_passed=all_sessions_passed && passed;return passed;
}
void LayoutButtons(HWND hwnd){
    RECT rect{};GetClientRect(hwnd,&rect);const int pane=rect.right/2;
    for(int i=0;i<2;++i){
        for(int j=0;j<3;++j)if(focus_buttons[i][j])MoveWindow(focus_buttons[i][j],i*pane+12+j*125,40,115,28,TRUE);
        if(capture_buttons[i])MoveWindow(capture_buttons[i],i*pane+387,40,145,28,TRUE);
    }
    for(int j=0;j<5;++j)if(merge_buttons[j])MoveWindow(merge_buttons[j],12+j*145,rect.bottom/2+52,135,28,TRUE);
    if(pair_button)MoveWindow(pair_button,755,rect.bottom/2+52,235,28,TRUE);
    if(review_button)MoveWindow(review_button,1005,rect.bottom/2+52,225,28,TRUE);
    if(inspect_button)MoveWindow(inspect_button,std::max(12L,rect.right-215),rect.bottom/2+88,200,28,TRUE);
    if(session_button)MoveWindow(session_button,std::max(12L,rect.right-430),rect.bottom/2+88,200,28,TRUE);
    if(seam_button)MoveWindow(seam_button,1245,rect.bottom/2+52,140,28,TRUE);
}
void OpenSavedInspector(){
    if(review_path.empty())return;
    auto report=review_path;report.replace_extension(L".json");
    std::wstring module(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!length || length>=module.size())throw std::runtime_error("inspector executable path unavailable");module.resize(length);
    const auto executable=std::filesystem::path(module).parent_path()/L"A0CameraStitcher.StillReview.exe";
    // Direct CreateProcess, never a shell. Both arguments end in fixed file
    // names, so no trailing-backslash argument quoting ambiguity is possible.
    std::wstring command=L"\""+executable.wstring()+L"\" --report \""+report.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process))throw std::runtime_error("saved inspector launch failed");
    CloseHandle(process.hThread);CloseHandle(process.hProcess);Log("saved_review_inspector_launched=1 camera_operations=0");
}
// Where DrawPixels puts a picture of p inside the given area: centred across, top aligned, keeping its proportions.
struct PixelRect {int left{},top{},width{},height{};};
PixelRect FitPixels(const Pixels& p,int left,int top,int available_width,int available_height){
    if(!p.width || !p.height || available_width<=0 || available_height<=0)return {};
    const double scale=std::min(double(available_width)/p.width,double(available_height)/p.height);
    const int width=static_cast<int>(p.width*scale),height=static_cast<int>(p.height*scale);
    return {left+(available_width-width)/2,top,width,height};
}
bool DrawPixels(HDC dc,const Pixels& p,int left,int top,int available_width,int available_height){
    if(!p.width || !p.height || available_width<=0 || available_height<=0)return false;
    const auto rect=FitPixels(p,left,top,available_width,available_height);
    const int width=rect.width,height=rect.height;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=p.width;info.bmiHeader.biHeight=-static_cast<LONG>(p.height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    return StretchDIBits(dc,left+(available_width-width)/2,top,width,height,0,0,p.width,p.height,p.bytes.data(),&info,DIB_RGB_COLORS,SRCCOPY)>0;
}
// The seam of C: one vertical line over the picture drawn into `rect`, at the middle of output column
// `column` of p, in the pink of the saved result's seam line (#400). Over the display only; the pixels of
// C stay as rendered. Returns whether a line was drawn.
inline constexpr COLORREF kSeamLineColor=RGB(255,0,255);
bool DrawSeamLine(HDC dc,const PixelRect& rect,const Pixels& p,unsigned column){
    if(!p.width || column>=p.width || rect.width<=0 || rect.height<=0)return false;
    const int x=rect.left+static_cast<int>(std::lround((column+0.5)*rect.width/p.width));
    const auto saved=SaveDC(dc);if(!saved)return false;
    SelectObject(dc,GetStockObject(DC_PEN));SetDCPenColor(dc,kSeamLineColor);
    const bool drawn=MoveToEx(dc,x,rect.top,nullptr)!=FALSE && LineTo(dc,x,rect.top+rect.height)!=FALSE;
    RestoreDC(dc,saved);return drawn;
}
bool DrawLevel(HDC dc,int left,int width,const Camera& c){
    const bool active=!stopping && !c.done && !c.failed;
    const auto now=Clock::now();
    const bool visible=D810LevelVisible(c.level,active,c.last,now);
    std::wstring text=L"水準器: ";
    if(visible)text+=L"左右 "+std::to_wstring(c.level->roll_degrees)+L"° / 前後 "+std::to_wstring(c.level->pitch_degrees)+L"°（整数参考値）";
    else if(!active)text+=L"停止中";
    else if(c.frames && now-c.last>=std::chrono::seconds(2))text+=L"更新待ち（数値非表示）";
    else text+=L"データ未取得 / 利用不可";
    RECT label{left,104,left+width-100,126};
    const bool text_drawn=DrawTextW(dc,text.c_str(),-1,&label,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX)>0;
    RECT note{left,128,left+width-100,170};
    DrawTextW(dc,L"精度・有効性は未検証。大きな前後傾斜・真下向きでは使用できない場合があります。",-1,&note,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    if(!visible || width<300)return false;
    // Positive roll rotates clockwise in screen coordinates, matching the
    // public driver's RotateTransform. Amber is a reference, never a pass.
    const auto saved=SaveDC(dc);if(!saved)return false;
    SelectObject(dc,GetStockObject(DC_PEN));SelectObject(dc,GetStockObject(NULL_BRUSH));
    SetDCPenColor(dc,RGB(110,110,110));
    const int x=left+width-48,y=134;
    bool drawn=Ellipse(dc,x-38,y-28,x+39,y+29)!=FALSE;
    drawn=(MoveToEx(dc,x-35,y,nullptr)!=FALSE) && drawn;
    drawn=(LineTo(dc,x+36,y)!=FALSE) && drawn;
    const double radians=c.level->roll_degrees*std::numbers::pi/180.;
    const int dx=static_cast<int>(std::lround(32*std::cos(radians))),dy=static_cast<int>(std::lround(32*std::sin(radians)));
    SetDCPenColor(dc,RGB(255,200,90));
    drawn=(MoveToEx(dc,x-dx,y-dy,nullptr)!=FALSE) && drawn;
    drawn=(LineTo(dc,x+dx,y+dy)!=FALSE) && drawn;
    RestoreDC(dc,saved);return drawn && text_drawn;
}
// Camera-free rendering contract. Inspect only our own memory bitmap; never
// read the desktop or attach to a camera. Real-frame presentation is separate.
int RunLevelUiSmoke(){
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return 2;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=680;info.bmiHeader.biHeight=-180;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};
    auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bitmap){DeleteDC(dc);return 2;}auto old=SelectObject(dc,bitmap);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
    Camera c;c.frames=1;c.last=Clock::now();c.level=D810LevelSample{0,-7};
    // Realize the Japanese fallback font before checking repeated-draw leaks.
    DrawLevel(dc,12,656,c);GdiFlush();
    const auto resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    unsigned checks{},failures{};auto check=[&](bool ok){++checks;if(!ok){++failures;Log("level_ui_failed_check="+std::to_string(checks));}};
    const auto clear=[&]{RECT area{0,0,680,180};FillRect(dc,&area,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));};
    const auto amber=[&]{unsigned count{};for(int y=100;y<170;++y)for(int x=580;x<680;++x)if(GetPixel(dc,x,y)==RGB(255,200,90))++count;return count;};
    clear();check(DrawLevel(dc,12,656,c));check(GetPixel(dc,600,134)==RGB(255,200,90));check(amber()>=60);
    clear();c.level=D810LevelSample{90,0};check(DrawLevel(dc,12,656,c));check(GetPixel(dc,620,110)==RGB(255,200,90));check(GetPixel(dc,600,134)!=RGB(255,200,90));
    clear();c.last=Clock::now()-std::chrono::seconds(3);check(!DrawLevel(dc,12,656,c));check(amber()==0);
    clear();c.last=Clock::now();stopping=true;check(!DrawLevel(dc,12,656,c));check(amber()==0);stopping=false;
    clear();c.level.reset();check(!DrawLevel(dc,12,656,c));check(amber()==0);
    c.level=D810LevelSample{0,0};
    for(int i=0;i<100;++i){c.last=Clock::now();check(DrawLevel(dc,12,656,c));}
    GdiFlush();check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==resources);
    // Default-size layout uses the same actual GDI font as Paint.
    const std::wstring longest=L"水準器: 左右 -180° / 前後 -180°（整数参考値）";
    SIZE extent{};check(GetTextExtentPoint32W(dc,longest.c_str(),static_cast<int>(longest.size()),&extent)!=FALSE && extent.cx<=556 && extent.cy<=22);
    RECT note{0,0,556,0};DrawTextW(dc,L"精度・有効性は未検証。大きな前後傾斜・真下向きでは使用できない場合があります。",-1,&note,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);check(note.bottom<=42);
    Log("level_ui_label_width="+std::to_string(extent.cx)+" height="+std::to_string(extent.cy)+" note_height="+std::to_string(note.bottom));
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    Log("level_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
std::array<std::uint64_t,6> Paint(HDC dc,RECT client){
    std::array<std::uint64_t,6> drawn{};
    FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
    if(client.right<200 || client.bottom<240)return drawn;
    for(int i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);
        const int pane=client.right/2,left=i*pane+12,top=174;
        std::wstring title=i==0?L"CAM-A（左）":L"CAM-B（右）";title+=L"   frames: "+std::to_wstring(c.frames);
        if(c.failed)title+=L"   FAILED";else if(stopping)title+=L"   STOPPED / STOPPING";else if(c.frames)title+=L"   LIVE";else title+=L"   STARTING";
        TextOutW(dc,left,16,title.c_str(),static_cast<int>(title.size()));TextOutW(dc,left,82,c.focus_status.c_str(),static_cast<int>(c.focus_status.size()));
        if(DrawLevel(dc,left,pane-24,c))drawn[4+i]=c.frames;
        if(!c.pixels.bytes.empty() && Clock::now()-c.last<std::chrono::seconds(2) && !stopping &&
            DrawPixels(dc,c.pixels,left,top,pane-24,client.bottom/2-top-8))drawn[i]=c.frames;
    }
    RECT status_rect{12,client.bottom/2+4,client.right-12,client.bottom/2+48};
    DrawTextW(dc,capture_status.c_str(),-1,&status_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    if(review_visible && !review_pixels.bytes.empty()){
        const wchar_t* title=L"撮影JPEGの合成結果（静止画／未校正・確認用）";
        TextOutW(dc,12,client.bottom/2+88,title,lstrlenW(title));
        const wchar_t* text=L"現在のライブ映像とは別の撮影結果です。「ライブCに戻す」で切り替えられます";
        TextOutW(dc,12,client.bottom/2+110,text,lstrlenW(text));
        if(DrawPixels(dc,review_pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155))drawn[3]=1;
        return drawn;
    }
    std::lock_guard lock(merged.mutex);
    // The title and the standing note name the kind of C (#401): drawn with the fixed composite condition,
    // not drawn although a condition exists, or the uncalibrated C when there is none.
    std::wstring title,status;
    if(merged.decided && merged.mode==PreviewCMode::fixed_composite){
        title=FixedPreviewTitle(mount_orientation,merged.fixed_plan);
        status=merged.position_refusal.empty()?FixedPreviewNote(merged.fixed_plan):merged.position_refusal;
    }else if(FixedConditionPresent()){
        title=L"C 固定の合成条件（未校正・未承認・表示専用）";
        status=merged.decided?merged.position_refusal:L"固定の合成条件を確認しています…";
    }else{
        title=PreviewMergeTitle(mount_orientation,merged.overlap_pixels,merged.vertical_pixels);
        status=std::wstring(kUncalibratedPreviewNotice)+L" / "+merged.alignment_status;
    }
    TextOutW(dc,12,client.bottom/2+88,title.c_str(),static_cast<int>(title.size()));
    RECT status_line{12,client.bottom/2+110,client.right-12,client.bottom/2+138};
    DrawTextW(dc,status.c_str(),-1,&status_line,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    if(!stopping && merged.rendered_revision==merged.revision && !merged.pixels.bytes.empty() && PreviewPairFresh(Clock::now(),merged.source_a,merged.source_b)){
        if(DrawPixels(dc,merged.pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155)){
            drawn[2]=merged.frames;
            if(merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.show_seam)
                DrawSeamLine(dc,FitPixels(merged.pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155),merged.pixels,merged.seam_column);
        }
    }else{const auto text=stopping?L"C: ライブビュー停止中です":merged.error.empty()?L"C: 両方の新しいフレームを待っています":merged.error.c_str();TextOutW(dc,12,client.bottom/2+145,text,lstrlenW(text));}
    return drawn;
}
void RequestCapture(int index){
    if(camera_end_restart_smoke)return; // This explicit lifecycle test never exposes.
    if(index<0 || index>2 || !CanFocus(*cameras[0]) || !CanFocus(*cameras[1]))return;
    if(index==2){std::lock_guard lock(merged.mutex);
        // C without a picture because the fixed condition cannot be applied (#401) does not stop the pair capture:
        // the saved pair needs no picture of C, and its own composite step reports the same reasons (#400).
        const bool no_picture_by_condition=merged.decided && merged.mode==PreviewCMode::unavailable;
        if((merged.pixels.bytes.empty() && !no_picture_by_condition) || merged.rendered_revision!=merged.revision || !PreviewPairFresh(Clock::now(),merged.source_a,merged.source_b))return;
        pending_geometry={merged.source_width,merged.source_height,merged.overlap_pixels,merged.vertical_pixels,merged.revision,mount_orientation};
    }
    capture_stop_requested=false;review_visible=false;capture_deadline=Clock::now()+std::chrono::seconds(180);
    capture_pending=index;capture_status=index==0?L"Aを撮影する準備中：両Liveを停止しています…":L"Bを撮影する準備中：両Liveを停止しています…";
    if(index==2)capture_status=L"両台撮影・確認用JPEG Mergeの準備中：両Liveを停止しています…";
    stopping=true;ClearMerge();Log("ui_capture_requested="+std::to_string(index)+" both_live_stop_required=1");
}
CaptureUiOutcome SavePairReview(const std::array<std::wstring,2>& ids,ReviewGeometry geometry,Clock::time_point deadline){
    CaptureUiOutcome result;
    const auto root=DefaultIdentityMapPath().parent_path().parent_path()/"wpd-pair-reviews";
    EvidenceWriter evidence(root,NewRunId(),"wpd-uncalibrated-pair-review");
    try{
        evidence.RecordState("pair","BothLiveViewsClosed");
        // Read first and on every capture, so a condition made between captures is used.
        std::optional<a0::m2::experimental::LoadedFixedCompositeCondition> condition;
        a0::m2::experimental::FixedCompositeSkip condition_skip=a0::m2::experimental::FixedCompositeSkip::condition_absent;
        try{
            auto condition_file=DefaultIdentityMapPath();condition_file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
            condition=a0::m2::experimental::LoadFixedCompositeCondition(condition_file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
        }catch(const std::exception&){condition_skip=a0::m2::experimental::FixedCompositeSkip::condition_invalid;}
        // #389: both shutters precede either import, all under this one deadline.
        auto pair=CaptureStillPairShutterFirst(ids,deadline);
        if(!pair.complete){result.capture.out_of_focus=pair.out_of_focus;result.preflight_refused=!pair.shutter_dispatched[0] && !pair.shutter_dispatched[1];throw std::runtime_error(pair.error);}
        const auto& a=pair.originals[0];const auto& b=pair.originals[1];
        a0::m2::experimental::StillReviewRequest request{a.path,b.path,evidence.RunRoot()/"review",a.sha256,b.sha256,evidence.RunId(),geometry,deadline};
        // #400: geometry and seam come from the fixed composite condition beside the
        // merge policy. Its absence or damage never stops the capture: the originals
        // are kept and only the composite is skipped, with the reason on screen.
        namespace m2e=a0::m2::experimental;
        std::optional<m2e::FixedCompositeSkip> skip;
        if(condition){
            const auto policy_scale=merge_policy?merge_policy->scale_max_percent:std::nullopt;
            request.fixed_composite=m2e::StillReviewRequest::FixedComposite{&condition->condition,condition->file_sha256,
                m2e::ComparePolicyLimit(condition->condition.scale_auto_correction_max_percent,policy_scale,merge_policy?m2e::FixedPolicyCheck::policy_without_scale_limit:m2e::FixedPolicyCheck::policy_absent)};
            try{result.review=m2e::WriteUncalibratedStillReview(request);}
            catch(const m2e::FixedCompositeUnavailable& unavailable){skip=unavailable.Skip();result.composite_detail=unavailable.what();}
        }else skip=condition_skip;
        if(!result.review){
            result.composite_skip=skip;
            result.capture={true,true,a.path,a.bytes,a.sha256,{}};
            result.originals={a.path,b.path};
            evidence.RecordState("pair","OriginalsSavedCompositeSkipped",{},m2e::FixedCompositeSkipCode(*skip));
            Log("pair_composite_skipped=1 reason="+std::string(m2e::FixedCompositeSkipCode(*skip))+" originals_saved=2 calibrated=0");
            return result;
        }
        if(Clock::now()>=deadline)throw std::runtime_error("pair deadline after review; all files retained");
        result.capture={true,true,result.review->jpeg,std::filesystem::file_size(result.review->jpeg),result.review->sha256,{}};
        evidence.RecordState("pair","UncalibratedReviewReady");
        Log("pair_review_saved=1 width="+std::to_string(result.review->width)+" height="+std::to_string(result.review->height)+" mount_orientation="+a0::m2::experimental::MountOrientationName(geometry.orientation)+" originals_rotated=0 calibrated=0");
        if(const auto& fixed=result.review->fixed_composite){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_fixed_composite seam_column="<<fixed->seam_column<<" seam_origin="<<fixed->seam_origin<<" scale_status="<<a0::m2::experimental::FixedScaleStatusCode(fixed->scale.status)
                <<" scale_reason="<<a0::m2::experimental::FixedScaleReasonCode(fixed->scale.reason)<<" applied_scale="<<fixed->scale.applied_scale<<" limit_percent="<<fixed->scale.limit_percent
                <<" policy_check="<<a0::m2::experimental::FixedPolicyCheckCode(fixed->policy_limit.check)<<" estimate_ms="<<result.review->alignment_milliseconds<<" quality_accepted=0";Log(line.str());}
        if(const auto& seam=result.review->blend){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_blend method="<<(result.review->fixed_composite?"fixed-seam":"seam-cut")<<" seam_x="<<seam->summary.x_minimum<<".."<<seam->summary.x_maximum<<" ratio="<<seam->ratio_minimum<<".."<<seam->ratio_maximum
                <<" saturation_limited="<<seam->saturation_limited_pixels<<" ms="<<result.review->blend_milliseconds<<" quality_accepted=0";Log(line.str());}
        std::wcout<<L"saved_review="<<result.review->jpeg.wstring()<<std::endl;
    }catch(const std::exception& error){result.capture.success=false;result.capture.error=error.what();evidence.RecordState("pair","FailedPartial",{},error.what());Log(std::string("pair_review_failure: ")+error.what());}
    return result;
}
void AdvanceCapture(HWND hwnd){
    if(!preview_joined && cameras[0]->done && cameras[1]->done){
        const bool safe=FinishPreview();
        if(capture_pending && !closing){
            const int index=*capture_pending;capture_pending.reset();
            if(safe){
                capture_status=index==2?L"AとBのシャッターを続けて1回ずつ切り、原画像と確認用Merge JPEGを保存しています…":index==0?L"Aを1回撮影してPCへ保存しています…":L"Bを1回撮影してPCへ保存しています…";
                capture_future=std::async(std::launch::async,[ids=registered_ids,index,geometry=pending_geometry,deadline=capture_deadline]{
                    try{ComScope com;if(index==2)return SavePairReview(ids,geometry,deadline);return CaptureUiOutcome{SaveStillOnce(ids[index],index==0?"CAM-A":"CAM-B",true,deadline),{}};}
                    catch(const std::exception& error){CaptureUiOutcome result;result.capture.error=error.what();return result;}
                });
            }else capture_status=L"Liveの安全な終了を確認できません。撮影せず停止しました。";
        }else if(!closing){
            manual_restart_waiting=false;
            capture_status.clear();
            for(const auto& camera:cameras)if(camera->failed){
                if(!capture_status.empty())capture_status+=L" / ";
                capture_status+=camera->alias=="CAM-A"?L"A: ":L"B: ";capture_status+=camera->failure_status;
            }
            if(capture_status.empty())capture_status=L"ライブビューを停止しました。";
            capture_status+=CanRestartPreview()?L" 両台の切断を確認済み。「ライブ再開」で再開できます。":L" 安全な切断を確認できないため再開を無効にしています。";
        }
    }
    if(capture_future.valid() && capture_future.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto result=capture_future.get();
        if(result.capture.success && result.capture.closed){
            captures_saved+=(result.review || result.composite_skip)?2:1;capture_status=L"保存済み: "+result.capture.path.wstring();
            if(result.composite_skip)capture_status=L"原画像2枚を保存しました（A: "+result.originals[0].wstring()+L"）。合成はしていません: "+WidenUtf8(a0::m2::experimental::FixedCompositeSkipText(*result.composite_skip));
            if(result.review && result.review->fixed_composite)capture_status+=L" / "+FixedCompositeLine(*result.review->fixed_composite);
            if(result.review && result.review->automatic)capture_status+=L" / "+StillAlignmentLine(*result.review->automatic);
            if(result.review){
                ++pair_reviews;review_path=result.review->jpeg;const auto& thumbnail=result.review->thumbnail;
                if(pair_cycle_smoke){pair_cycle_reports.push_back(result.review->report);std::wcout<<L"pair_cycle_report="<<result.review->report.wstring()<<std::endl;}
                review_pixels={thumbnail.width,thumbnail.height,{}};review_pixels.bytes.resize(static_cast<size_t>(thumbnail.width)*thumbnail.height*4);
                for(size_t i=0,j=0;i<thumbnail.bgr.size();i+=3,j+=4)std::copy_n(thumbnail.bgr.data()+i,3,review_pixels.bytes.data()+j);
                // The seam is one vertical column (#400): drawn on this display copy only, never in the saved JPEG.
                if(const auto& fixed=result.review->fixed_composite){
                    const auto divisor=std::max(1u,(std::max(result.review->width,result.review->height)+1199)/1200);const auto column=fixed->seam_column/divisor;
                    if(column<thumbnail.width)for(unsigned y=0;y<thumbnail.height;++y){auto* pixel=review_pixels.bytes.data()+(static_cast<size_t>(y)*thumbnail.width+column)*4;pixel[0]=255;pixel[1]=0;pixel[2]=255;}
                }
                review_visible=true;
            }
            if(!closing)StartPreview();
        }else{
            capture_restart_blocked=true;
            all_sessions_passed=false;capture_status=L"撮影・保存に失敗しました。再撮影せず停止中です。";
            if(result.preflight_refused)capture_status=L"撮影前の確認で停止しました。A/Bどちらにもシャッターを送っていません。カメラ・接続状態を確認してください。";
            if(result.capture.out_of_focus)capture_status=result.preflight_refused?
                L"合焦を確認できず撮影前に停止しました。A/Bの撮影は0です。この画面を閉じてライブを再起動し、AFで確認してください。":
                L"合焦できず撮影を完了できませんでした（OutOfFocus）。この画面を閉じてライブを再起動し、AFで確認してください。自動再撮影はしません。";
            Log("ui_capture_failed no_retry=1");
        }
    }
    if(closing && preview_joined && !capture_future.valid())DestroyWindow(hwnd);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    switch(message){
    case WM_SIZE:LayoutButtons(hwnd);return 0;
    case WM_COMMAND:{
        const int id=LOWORD(w),camera_index=(id-100)/10,action=(id-100)%10;
        if(id>=100 && id<=112 && camera_index<2 && action<3 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==focus_buttons[camera_index][action]){
            if(camera_end_restart_smoke)return 0;
            auto& camera=*cameras[camera_index];const bool queued=camera.focus_mailbox.TryQueue(static_cast<PreviewFocusAction>(action),CanFocus(camera));
            Log(camera.alias+" ui_focus="+std::to_string(action)+" queued="+std::to_string(queued));if(queued)camera.FocusStatus(L"操作を受け付けました…");return 0;
        }
        if((id==200 || id==201) && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==capture_buttons[id-200]){RequestCapture(id-200);return 0;}
        if(id==400 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==pair_button){RequestCapture(2);return 0;}
        if(id==401 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==review_button && !review_pixels.bytes.empty()){review_visible=!review_visible;return 0;}
        if(id==404 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==seam_button){
            // Display only: the seam line over C. Nothing about the composite changes.
            const bool shown=SendMessageW(seam_button,BM_GETCHECK,0,0)==BST_CHECKED;
            {std::lock_guard lock(merged.mutex);merged.show_seam=shown;}
            Log(std::string("C ui_seam_line=")+(shown?"1":"0"));return 0;
        }
        if(id==402 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==inspect_button){
            try{OpenSavedInspector();}catch(const std::exception&){capture_status=L"拡大確認画面を開けませんでした。撮影結果は保存されています。";}return 0;
        }
        if(id==403 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==session_button){
            if(CanRestartPreview()){
                try{
                    // Re-resolve registered identities only after old workers
                    // and both sessions have closed; keep the same lease.
                    registered_ids=Discover();review_visible=false;StartPreview();++manual_preview_restarts;manual_restart_waiting=true;
                    capture_status=L"ライブビューを再開しています。撮影・AFの自動実行はありません。";
                    Log("manual_preview_restart=1 fresh_channels=1 previous_sessions_closed=1");
                }catch(const std::exception& error){
                    all_sessions_passed=false;capture_status=L"ライブビューを再開できませんでした。本体・接続状態を確認してください。";
                    Log(std::string("manual_preview_restart_failed: ")+error.what());
                    // Thread-start exceptions can drop a shutdown barrier.
                    // Never reuse that barrier after a host startup failure.
                    if(!preview_joined){
                        closing=true;stopping=true;
                        if(!camera_workers[0].joinable() && !camera_workers[1].joinable() && !merge_worker.joinable())preview_joined=true;
                    }
                }
            }else if(!preview_joined && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
                stopping=true;++manual_preview_stops;capture_status=L"両台のライブビューを停止・切断しています…";
                Log("manual_preview_stop=1 capture_requests=0");
            }
            return 0;
        }
        if(id>=300 && id<=304 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==merge_buttons[id-300]){
            std::lock_guard lock(merged.mutex);
            if(stopping || !merged.source_width)return 0;
            review_visible=false;
            // A fixed composite condition decides C's position (#401): no request moves it, and nothing is estimated.
            if(FixedConditionPresent()){
                merged.position_refusal=FixedPreviewPositionRefusal();
                Log("C ui_position_request_ignored=1 id="+std::to_string(id)+" fixed_composite_condition=1");return 0;
            }
            if(id==304){
                if(!merged.auto_busy){merged.auto_requested=true;merged.auto_busy=true;merged.alignment_status=L"自動位置の候補を計算中…";++merged.revision;}
                Log("C ui_auto_alignment=1");return 0;
            }
            if(merged.auto_requested){merged.auto_requested=false;merged.auto_busy=false;}
            if(id==300)merged.overlap_pixels=std::max((merged.source_width+9)/10,merged.overlap_pixels-1);
            if(id==301)merged.overlap_pixels=std::min(merged.source_width*9/10,merged.overlap_pixels+1);
            if(id==302)merged.vertical_pixels=std::max(-100,merged.vertical_pixels-1);
            if(id==303)merged.vertical_pixels=std::min(100,merged.vertical_pixels+1);
            merged.alignment_status=L"手動調整（要目視確認）";++merged.revision;merged.pixels={};
            Log("C ui_manual_adjustment="+std::to_string(id-300)+" overlap_px="+std::to_string(merged.overlap_pixels)+" vertical_px="+std::to_string(merged.vertical_pixels));return 0;
        }break;}
    case WM_PAINT:{
        PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);PreviewBackBuffer frame(dc,r.right,r.bottom);
        if(auto offscreen=frame.DrawingContext()){
            const auto drawn=Paint(offscreen,r);
            if(frame.Present(dc)){
                for(size_t i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);if(drawn[i]>c.paint_sequence){++c.painted;c.paint_sequence=drawn[i];}
                    if(drawn[4+i]>c.level_paint_sequence){++c.level_painted;c.level_paint_sequence=drawn[4+i];}}
                std::lock_guard lock(merged.mutex);if(drawn[2]>merged.paint_sequence){++merged.painted;merged.paint_sequence=drawn[2];}
                if(drawn[3])++review_painted;
            }
        }EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_KEYDOWN:if(w==VK_ESCAPE)SendMessageW(hwnd,WM_CLOSE,0,0);return 0;
    case WM_CLOSE:closing=true;stopping=true;capture_stop_requested=true;capture_pending.reset();SetWindowTextW(hwnd,L"A0 - stopping both cameras / finishing current save...");return 0;
    case WM_TIMER:
        if(manual_restart_waiting && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
            manual_restart_waiting=false;capture_status=L"ライブビューを再開しました。撮影ボタンで両Liveを停止して撮影・PC保存できます。";
            Log("manual_preview_restart_live=1 both_fresh=1");
        }
        for(int i=0;i<2;++i){const bool enabled=CanFocus(*cameras[i]);for(auto button:focus_buttons[i])if(button)EnableWindow(button,enabled);}
        for(auto button:capture_buttons)if(button)EnableWindow(button,CanFocus(*cameras[0]) && CanFocus(*cameras[1]));
        if(pair_button)EnableWindow(pair_button,CanFocus(*cameras[0]) && CanFocus(*cameras[1]));
        if(review_button){EnableWindow(review_button,!review_pixels.bytes.empty());SetWindowTextW(review_button,review_visible?L"ライブCに戻す":L"撮影結果を見る");}
        if(inspect_button)EnableWindow(inspect_button,!review_path.empty());
        if(session_button){
            EnableWindow(session_button,CanRestartPreview() || (!preview_joined && CanFocus(*cameras[0]) && CanFocus(*cameras[1])));
            SetWindowTextW(session_button,preview_joined?L"ライブ再開":stopping?L"停止・切断中…":L"ライブ停止");
        }
        {std::lock_guard lock(merged.mutex);for(int i=0;i<5;++i)if(merge_buttons[i])EnableWindow(merge_buttons[i],!stopping && merged.source_width>0 && (i!=4 || !merged.auto_busy));
            if(seam_button)EnableWindow(seam_button,!stopping && merged.decided && merged.mode==PreviewCMode::fixed_composite);}
        if(focus_smoke && smoke_next<6 && !stopping && Clock::now()-run_start>std::chrono::seconds(5+static_cast<int>(smoke_next)*3)){
            constexpr std::array<std::array<int,2>,6> plan{{{0,0},{1,0},{0,1},{0,2},{1,1},{1,2}}};const auto item=plan[smoke_next];
            if(CanFocus(*cameras[item[0]])){SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(100+item[0]*10+item[1],BN_CLICKED),reinterpret_cast<LPARAM>(focus_buttons[item[0]][item[1]]));++smoke_next;}
        }
        if(af_prepare_smoke && !stopping){
            if(af_prepare_next<2 && Clock::now()-run_start>std::chrono::seconds(3+2*af_prepare_next) && CanFocus(*cameras[af_prepare_next])){
                const auto i=af_prepare_next++;SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(100+i*10,BN_CLICKED),reinterpret_cast<LPARAM>(focus_buttons[i][0]));
            }
            if(af_prepare_next==2 && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
                bool complete=true,ok=true;
                for(auto& camera:cameras){std::lock_guard lock(camera->mutex);complete=complete && camera->focus_requests==1;ok=ok && camera->focus_success[0]==1;}
                if(complete){all_sessions_passed=all_sessions_passed && ok;closing=true;stopping=true;Log("af_preparation_complete="+std::to_string(ok)+" shutter_commands=0");}
            }
        }
        // Explicit test uses the very same UI command; never enabled by normal launch.
        if(pair_smoke && pair_smoke_next<2 && !stopping && Clock::now()-run_start>std::chrono::seconds(pair_smoke_next==0?3:7) && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
            const int id=pair_smoke_next==0?304:400;const auto button=pair_smoke_next==0?merge_buttons[4]:pair_button;++pair_smoke_next;
            SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));
        }
        if(merge_smoke && merge_smoke_next<5 && !stopping && Clock::now()-run_start>std::chrono::seconds(3+static_cast<int>(merge_smoke_next)*2) && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
            constexpr std::array<int,5> plan{304,300,301,302,303};const int id=plan[merge_smoke_next++];
            SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(merge_buttons[id-300]));
        }
        if(capture_smoke && capture_smoke_next<2 && !stopping && Clock::now()-run_start>std::chrono::seconds(8+static_cast<int>(capture_smoke_next)*18) && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
            const int index=static_cast<int>(capture_smoke_next++);SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(200+index,BN_CLICKED),reinterpret_cast<LPARAM>(capture_buttons[index]));
        }
        if(restart_smoke && restart_smoke_next<4){
            const bool stop_step=restart_smoke_next%2==0;
            const auto elapsed=Clock::now()-run_start;
            if((stop_step && elapsed>std::chrono::seconds(restart_smoke_next==0?5:14) && CanFocus(*cameras[0]) && CanFocus(*cameras[1])) ||
                (!stop_step && CanRestartPreview())){
                ++restart_smoke_next;SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(403,BN_CLICKED),reinterpret_cast<LPARAM>(session_button));
            }
        }
        if(pair_cycle_smoke && !closing){
            const auto action=pair_cycle_probe.Next(ObservePairCycle());
            if(action==PairCycleAction::Capture){
                Log("pair_cycle_capture_request="+std::to_string(pair_cycle_probe.Completed()+1));
                SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(400,BN_CLICKED),reinterpret_cast<LPARAM>(pair_button));
            }else if(action==PairCycleAction::ShowLive){
                Log("pair_cycle_static_review_confirmed="+std::to_string(pair_cycle_probe.Completed()+1));
                SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(401,BN_CLICKED),reinterpret_cast<LPARAM>(review_button));
            }else if(action==PairCycleAction::Complete || action==PairCycleAction::Fail){
                if(action==PairCycleAction::Fail)all_sessions_passed=false;
                Log("pair_cycle_completed="+std::to_string(pair_cycle_probe.Completed())+" no_retry=1");
                closing=true;stopping=true;capture_pending.reset();
            }
        }
        {static auto next_report=Clock::now()+std::chrono::seconds(2);
        if(Clock::now()>=next_report){
            for(const auto& camera:cameras){auto& c=*camera;std::lock_guard lock(c.mutex);
                std::string state=c.alias+" live_frames="+std::to_string(c.frames)+" painted="+std::to_string(c.painted)+" level_frames="+std::to_string(c.level_frames)+" level_painted="+std::to_string(c.level_painted);
                if(D810LevelVisible(c.level,!stopping && !c.done && !c.failed,c.last,Clock::now()))state+=" roll_degrees="+std::to_string(c.level->roll_degrees)+" pitch_degrees="+std::to_string(c.level->pitch_degrees)+" level_reference_only=1";
                Log(state);}
            {std::lock_guard lock(merged.mutex);std::ostringstream line;line.imbue(std::locale::classic());
                line<<"C merged_frames="<<merged.frames<<" painted="<<merged.painted;
                if(merged.decided && merged.mode==PreviewCMode::fixed_composite)line<<" fixed_composite=1 seam_column="<<merged.seam_column<<" brightness_ratio="<<merged.ratio_minimum<<".."<<merged.ratio_maximum<<" scale_correction=0";
                else if(merged.decided && merged.mode==PreviewCMode::unavailable)line<<" fixed_composite=unavailable";
                Log(line.str());}next_report=Clock::now()+std::chrono::seconds(2);
        }}
        if(run_seconds>0 && Clock::now()-run_start>=std::chrono::seconds(run_seconds)){closing=true;stopping=true;capture_stop_requested=true;capture_pending.reset();}
        for(const auto& c:cameras)c->CancelStalled();
        // Interactive failures stay visible after Stop/Close. Finite test
        // runs still terminate and retain a failing result; no auto restart.
        if(run_seconds>0 && !camera_end_restart_smoke && (cameras[0]->failed || cameras[1]->failed))closing=true;
        try{AdvanceCapture(hwnd);}
        catch(const std::exception& error){
            all_sessions_passed=false;closing=true;stopping=true;capture_pending.reset();
            Log(std::string("preview host failure: ")+error.what());
            // StartPreview joins workers on failed startup. Never let an
            // exception escape the Win32 callback or release the lease early.
            if(!camera_workers[0].joinable() && !camera_workers[1].joinable() && !merge_worker.joinable())preview_joined=true;
            capture_status=L"処理を終了しています。画像・ログは保持します。";
        }
        if(camera_end_restart_smoke && !closing){
            const auto action=camera_end_probe.Next(ObserveCameraEndRestart());
            if(action==PreviewEndRestartAction::Restart){
                Log("explicit_camera_end_test_restart=1 expected_end_retained=1");
                SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(403,BN_CLICKED),reinterpret_cast<LPARAM>(session_button));
            }else if(action==PreviewEndRestartAction::Complete || action==PreviewEndRestartAction::Fail){
                Log("camera_end_restart_probe_complete="+std::to_string(action==PreviewEndRestartAction::Complete));closing=true;stopping=true;
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,w,l);
}
int RunLifecycleUiSmoke(){
    // Synthetic failed workers only. No Discover/Open/Run or hardware lease.
    Log("lifecycle_ui_synthetic=1 camera_operations=0");
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.done=true;c.opened=true;c.start_attempted=true;c.stopped=true;c.closed=true;
        c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    cameras[0]->failed=true;cameras[0]->failure_status=L"カメラ側でライブビューが終了しました";
    stopping=true;run_seconds=0;run_start=Clock::now();
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0LifecycleUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic lifecycle test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    session_button=CreateWindowExW(0,L"BUTTON",L"ライブ停止",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(403),instance,nullptr);
    if(!session_button){DestroyWindow(window);return 2;}
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("lifecycle_ui_failed_check="+std::to_string(checks));}};
    SendMessageW(window,WM_TIMER,1,0);
    check(IsWindow(window) && !closing && preview_joined);check(CanRestartPreview());
    check(capture_status.find(L"カメラ側でライブビューが終了")!=std::wstring::npos);
    SendMessageW(window,WM_TIMER,1,0);check(IsWindowEnabled(session_button)!=FALSE);
    wchar_t caption[64]{};GetWindowTextW(session_button,caption,64);check(std::wstring(caption)==L"ライブ再開");
    cameras[1]->closed=false;SendMessageW(window,WM_TIMER,1,0);check(!IsWindowEnabled(session_button));cameras[1]->closed=true;
    cameras[0]->stopped=false;SendMessageW(window,WM_TIMER,1,0);check(!IsWindowEnabled(session_button));cameras[0]->stopped=true;
    capture_restart_blocked=true;SendMessageW(window,WM_TIMER,1,0);check(!IsWindowEnabled(session_button));capture_restart_blocked=false;
    check(preview_sessions==0 && manual_preview_restarts==0 && captures_saved==0 && !capture_future.valid());
    {std::promise<CaptureUiOutcome> pending;capture_future=pending.get_future();CaptureUiOutcome refused;refused.preflight_refused=true;pending.set_value(std::move(refused));}
    SendMessageW(window,WM_TIMER,1,0);
    check(capture_status.find(L"どちらにもシャッターを送っていません")!=std::wstring::npos);
    check(captures_saved==0 && pair_reviews==0 && !capture_future.valid());check(!CanRestartPreview());
    {std::promise<CaptureUiOutcome> pending;capture_future=pending.get_future();CaptureUiOutcome refused;refused.preflight_refused=true;refused.capture.out_of_focus=true;pending.set_value(std::move(refused));}
    SendMessageW(window,WM_TIMER,1,0);check(capture_status.find(L"A/Bの撮影は0")!=std::wstring::npos);
    // The same failure still ends a finite test, so it cannot hang CI.
    run_seconds=20;SendMessageW(window,WM_TIMER,1,0);check(!IsWindow(window) && closing);
    Log("lifecycle_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
// Camera-free contract of C with a fixed composite condition (#401), on synthetic frames. The real merge
// worker, Paint, the position buttons, the seam checkbox and the pair-capture admission run; the condition
// comes from memory, so no settings file is read or written, and no Discover/Open/Run or hardware lease exists.
int RunFixedCUiSmoke(){
    Log("fixed_c_ui_synthetic=1 camera_operations=0");
    namespace m2=a0::m2::experimental;
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("fixed_c_ui_failed_check="+std::to_string(checks));}};
    const auto condition=[](m2::MountOrientation orientation){
        m2::FixedCompositeCondition c;
        c.created_utc="2026-11-04T12:00:00Z";c.source.review_report_sha256=std::string(64,'a');c.source.pair_id="pair-1";
        c.source.method=m2::FixedCompositeMethod::manual_still_pixels;c.orientation=orientation;
        c.source_width=1840;c.source_height=1228;c.mapping.translation_x=930;c.mapping.translation_y=9;c.mapping.rotation_degrees=0;c.mapping.scale=1;
        const auto p=m2::SimilarityReviewGeometry(930,9,0,1,1840,1228);
        c.mapping.crop=p.crop;
        const auto plan=a0::m2::render::PlanPairCanvas(1840,1228,1840,1228,p);
        c.output_width=plan.output_width;c.output_height=plan.output_height;c.scale_auto_correction_max_percent=1;
        c.seam={m2::ProvisionalSeamColumn(p,1840,1228),m2::FixedSeamOrigin::band_center_provisional};
        m2::ValidateFixedCompositeCondition(c);
        return c;
    };
    const auto frame=[](BYTE b,BYTE g,BYTE r){Pixels p{400,267,{}};p.bytes.assign(static_cast<size_t>(p.width)*p.height*4,0);
        for(size_t i=0;i<p.bytes.size();i+=4){p.bytes[i]=b;p.bytes[i+1]=g;p.bytes[i+2]=r;}return p;};
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.pixels=i==0?frame(100,100,100):frame(200,200,200);
        c.frames=25;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    stopping=false;run_seconds=0;run_start=Clock::now();
    const auto refresh=[&]{for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}};
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0FixedCUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic fixed C test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    for(int j=0;j<5;++j)merge_buttons[j]=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,10,10,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(300+j)),instance,nullptr);
    seam_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD|BS_AUTOCHECKBOX,0,0,10,10,window,reinterpret_cast<HMENU>(404),instance,nullptr);
    bool controls=seam_button!=nullptr;for(const auto button:merge_buttons)controls=controls && button!=nullptr;
    if(!controls){DestroyWindow(window);return 2;}
    SendMessageW(seam_button,BM_SETCHECK,BST_CHECKED,0);
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1400;info.bmiHeader.biHeight=-960;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc || !bitmap){DestroyWindow(window);return 2;}
    auto old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
    const RECT client{0,0,1400,960};
    // Pink pixels over the picture of C in the lower half; a picture has none of its own here.
    const auto pink=[&](const PixelRect& rect){unsigned count{};
        for(int y=rect.top;y<rect.top+rect.height;++y)for(int x=rect.left;x<rect.left+rect.width;++x)if(GetPixel(dc,x,y)==kSeamLineColor)++count;return count;};
    const auto paint=[&]{refresh();Paint(dc,client);GdiFlush();};
    const auto start_worker_wait=[&](auto ready){
        for(int i=0;i<100;++i){refresh();std::this_thread::sleep_for(std::chrono::milliseconds(50));{std::lock_guard lock(merged.mutex);if(ready())return true;}}
        return false;
    };
    const auto start_worker=[&](auto ready){merge_worker=std::jthread(MergeLoop);return start_worker_wait(ready);};
    const auto stop_worker=[&]{merge_worker.request_stop();merge_worker.join();};
    const auto press=[&](int id){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(id==404?seam_button:merge_buttons[id-300]));};
    // The pair capture must stay admitted whenever C was decided: the saved pair needs no picture of C.
    const auto capture_admitted=[&]{
        capture_pending.reset();RequestCapture(2);const bool admitted=capture_pending.has_value() && *capture_pending==2;
        capture_pending.reset();return admitted;
    };
    const auto rearm=[&]{stopping=false;capture_stop_requested=false;capture_pending.reset();};
    const auto picture_rect=[&]{std::lock_guard lock(merged.mutex);return FitPixels(merged.pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155);};

    // 1. A condition that applies: C is drawn with it, the seam line is over the picture, and no position request moves C.
    fixed_source={};fixed_source.loaded=m2::LoadedFixedCompositeCondition{condition(mount_orientation),std::string(64,'b')};
    check(FixedConditionPresent());
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.frames>=3 && !merged.pixels.bytes.empty();}));
    {
        std::lock_guard lock(merged.mutex);
        check(merged.mode==PreviewCMode::fixed_composite && merged.error.empty());
        check(merged.pixels.width==merged.fixed_plan.output_width && merged.pixels.height==merged.fixed_plan.output_height && !merged.pixels.bytes.empty());
        check(merged.seam_column==merged.fixed_plan.view.seam_column && merged.fixed_plan.view_width==400 && merged.fixed_plan.view_height==267);
        check(merged.ratio_minimum>0.4 && merged.ratio_maximum<0.6); // A 100 against B 200: B is brought down to A beside the seam
    }
    paint();
    {
        const auto rect=picture_rect();
        check(rect.width>0 && rect.height>0);
        const auto lit=pink(rect);
        check(lit+2>=static_cast<unsigned>(rect.height) && lit<=static_cast<unsigned>(rect.height));
        unsigned expected_column{},picture_width{};{std::lock_guard lock(merged.mutex);expected_column=merged.seam_column;picture_width=merged.pixels.width;}
        const int x=rect.left+static_cast<int>(std::lround((expected_column+0.5)*rect.width/picture_width));
        check(GetPixel(dc,x,rect.top+rect.height/2)==kSeamLineColor);
        // Off: the same picture without the line. On again: the line comes back.
        SendMessageW(seam_button,BM_SETCHECK,BST_UNCHECKED,0);press(404);
        {std::lock_guard lock(merged.mutex);check(!merged.show_seam);}
        paint();check(pink(rect)==0);
        SendMessageW(seam_button,BM_SETCHECK,BST_CHECKED,0);press(404);
        {std::lock_guard lock(merged.mutex);check(merged.show_seam);}
        paint();check(pink(rect)==lit);
    }
    {
        unsigned overlap{};int vertical{};std::uint64_t revision{};
        {std::lock_guard lock(merged.mutex);overlap=merged.overlap_pixels;vertical=merged.vertical_pixels;revision=merged.revision;}
        for(int id=300;id<=304;++id)press(id);
        std::lock_guard lock(merged.mutex);
        check(merged.overlap_pixels==overlap && merged.vertical_pixels==vertical && merged.revision==revision && !merged.auto_requested && !merged.auto_busy);
        check(!merged.position_refusal.empty() && !merged.pixels.bytes.empty());
    }
    paint();check(pink(picture_rect())>0);
    check(capture_admitted());
    stop_worker();rearm();ClearMerge();

    // 2. A condition for the other mounting: C is not drawn, the reason is shown, requests still change nothing, the pair capture stays admitted.
    fixed_source={};fixed_source.loaded=m2::LoadedFixedCompositeCondition{condition(m2::MountOrientation::rotated_180),std::string(64,'b')};
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::unavailable && merged.rendered_revision==merged.revision && merged.source_a.time_since_epoch().count()>0;}));
    {
        std::lock_guard lock(merged.mutex);
        check(merged.pixels.bytes.empty() && merged.error==FixedPreviewUnavailableText(m2::FixedCompositeSkip::orientation_differs));
        check(merged.fixed_plan.skip && *merged.fixed_plan.skip==m2::FixedCompositeSkip::orientation_differs);
    }
    paint();
    {
        PixelRect lower{12,client.bottom/2+140,client.right-24,client.bottom/2-155};
        check(pink(lower)==0);
        std::uint64_t revision{};{std::lock_guard lock(merged.mutex);revision=merged.revision;}
        press(300);press(304);
        std::lock_guard lock(merged.mutex);
        check(merged.revision==revision && !merged.position_refusal.empty());
    }
    check(capture_admitted());
    stop_worker();rearm();ClearMerge();

    // 3. A damaged file: the same, with its own reason.
    fixed_source={};fixed_source.reason=m2::FixedCompositeSkip::condition_invalid;
    check(FixedConditionPresent());
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::unavailable && merged.rendered_revision==merged.revision && merged.source_a.time_since_epoch().count()>0;}));
    {
        std::lock_guard lock(merged.mutex);
        check(merged.pixels.bytes.empty() && merged.error==FixedPreviewUnavailableText(m2::FixedCompositeSkip::condition_invalid));
    }
    check(capture_admitted());
    stop_worker();rearm();ClearMerge();

    // 4. No condition: the uncalibrated C stays, its position requests work, and no seam line is drawn.
    fixed_source={};
    check(!FixedConditionPresent());
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::uncalibrated && !merged.pixels.bytes.empty();}));
    paint();check(pink(picture_rect())==0);
    {
        unsigned overlap{};std::uint64_t revision{};
        {std::lock_guard lock(merged.mutex);check(merged.mode==PreviewCMode::uncalibrated && !merged.pixels.bytes.empty());overlap=merged.overlap_pixels;revision=merged.revision;}
        press(300);
        std::lock_guard lock(merged.mutex);
        check(merged.overlap_pixels+1==overlap && merged.revision>revision && merged.position_refusal.empty());
    }
    // The press cleared C's picture; once drawn again it is admitted for the pair capture as before.
    check(start_worker_wait([]{return !merged.pixels.bytes.empty() && merged.rendered_revision==merged.revision;}));
    check(capture_admitted());
    stop_worker();rearm();ClearMerge();

    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(window);
    fixed_source={};stopping=true;
    Log("fixed_c_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
int RunPreviewWindow(const std::array<std::wstring,2>& ids){
    registered_ids=ids;
    // Allocate before CreateWindow sends initial WM_PAINT/WM_SIZE.
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();cameras[i]->alias=i==0?"CAM-A":"CAM-B";}
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0WpdDualPreview";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc))throw std::runtime_error("register window failed");
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"A0 A/B Live + C 仮Merge（未校正） / 撮影・PC保存",WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)throw std::runtime_error("create window failed");
    constexpr std::array<const wchar_t*,3> labels{L"AF",L"近くへ微調整",L"遠くへ微調整"};
    for(int i=0;i<2;++i){
        for(int j=0;j<3;++j){focus_buttons[i][j]=CreateWindowExW(0,L"BUTTON",labels[j],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(100+i*10+j)),instance,nullptr);if(!focus_buttons[i][j])throw std::runtime_error("create focus button failed");EnableWindow(focus_buttons[i][j],FALSE);}
        capture_buttons[i]=CreateWindowExW(0,L"BUTTON",i==0?L"Aを撮影・保存":L"Bを撮影・保存",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(200+i)),instance,nullptr);if(!capture_buttons[i])throw std::runtime_error("create capture button failed");EnableWindow(capture_buttons[i],FALSE);
    }
    constexpr std::array<const wchar_t*,5> merge_labels{L"重なり −1px",L"重なり ＋1px",L"Bを上へ 1px",L"Bを下へ 1px",L"自動位置合わせ"};
    for(int j=0;j<5;++j){merge_buttons[j]=CreateWindowExW(0,L"BUTTON",merge_labels[j],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(300+j)),instance,nullptr);if(!merge_buttons[j])throw std::runtime_error("create merge button failed");EnableWindow(merge_buttons[j],FALSE);}
    pair_button=CreateWindowExW(0,L"BUTTON",L"両台撮影 → 仮Merge保存",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(400),instance,nullptr);
    review_button=CreateWindowExW(0,L"BUTTON",L"撮影結果を見る",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(401),instance,nullptr);
    inspect_button=CreateWindowExW(0,L"BUTTON",L"拡大確認・再Merge",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(402),instance,nullptr);
    session_button=CreateWindowExW(0,L"BUTTON",L"ライブ停止",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(403),instance,nullptr);
    // The seam line of the fixed composite C (#401): display only, on by default, enabled once C is drawn with a condition.
    seam_button=CreateWindowExW(0,L"BUTTON",L"継ぎ目の線を表示",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,window,reinterpret_cast<HMENU>(404),instance,nullptr);
    if(!pair_button || !review_button || !inspect_button || !session_button || !seam_button)throw std::runtime_error("create pair review buttons failed");EnableWindow(pair_button,FALSE);EnableWindow(review_button,FALSE);EnableWindow(inspect_button,FALSE);EnableWindow(session_button,FALSE);
    SendMessageW(seam_button,BM_SETCHECK,BST_CHECKED,0);EnableWindow(seam_button,FALSE);
    LayoutButtons(window);run_start=Clock::now();StartPreview();SetTimer(window,1,50,nullptr);
    // A hidden console launch can supply STARTF_USESHOWWINDOW/SW_HIDE. This
    // application is an explicitly requested interactive preview: show its
    // own window even when the launcher keeps the helper console hidden.
    ShowWindow(window,SW_SHOWNORMAL);
    if(!IsWindowVisible(window))ShowWindow(window,SW_SHOW);
    UpdateWindow(window);Log("ui_window_visible="+std::to_string(IsWindowVisible(window)!=FALSE));
    MSG message{};BOOL state{};
    while((state=GetMessageW(&message,nullptr,0,0))>0){if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE){SendMessageW(window,WM_CLOSE,0,0);continue;}if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    stopping=true;
    if(!preview_joined)FinishPreview();
    if(capture_future.valid()){auto result=capture_future.get();all_sessions_passed=all_sessions_passed && result.capture.success && result.capture.closed;}
    if(state<0)all_sessions_passed=false;
    if(capture_once_alias && all_sessions_passed){const auto& c=*cameras[*capture_once_alias];auto result=SaveStillOnce(c.id,c.alias,true);return result.success && result.closed?0:1;}
    if(!cleanup_only){std::lock_guard lock(merged.mutex);
        // A C that is not drawn because the fixed condition cannot be applied is the shown behaviour, not a missing picture (#401).
        all_sessions_passed=all_sessions_passed && (merged.painted>=5 || merged.mode==PreviewCMode::unavailable);
        Log("C final_merged_frames="+std::to_string(merged.frames)+" painted="+std::to_string(merged.painted)+" uncalibrated=1");}
    if(capture_smoke)all_sessions_passed=all_sessions_passed && captures_saved==2 && preview_sessions==3;
    if(merge_smoke)all_sessions_passed=all_sessions_passed && merge_smoke_next==5;
    if(pair_smoke)all_sessions_passed=all_sessions_passed && pair_reviews==1 && captures_saved==2 && preview_sessions==2 && review_painted>0;
    if(restart_smoke)all_sessions_passed=all_sessions_passed && restart_smoke_next==4 && manual_preview_stops==2 && manual_preview_restarts==2 && preview_sessions==3 && captures_saved==0;
    if(pair_cycle_smoke)all_sessions_passed=all_sessions_passed && pair_cycle_probe.Completed()==2 && pair_reviews==2 && captures_saved==4 && preview_sessions==3 && pair_cycle_reports.size()==2 && pair_cycle_reports[0]!=pair_cycle_reports[1];
    if(af_prepare_smoke)for(const auto& c:cameras)all_sessions_passed=all_sessions_passed && c->focus_requests==1 && c->focus_success[0]==1 && c->focus_success[1]==0 && c->focus_success[2]==0 && captures_saved==0;
    Log("manual_preview_stops="+std::to_string(manual_preview_stops)+" manual_preview_restarts="+std::to_string(manual_preview_restarts));
    Log("pair_reviews="+std::to_string(pair_reviews)+" static_review_painted="+std::to_string(review_painted));
    Log("saved_captures="+std::to_string(captures_saved)+" preview_sessions="+std::to_string(preview_sessions)+" final_result="+(all_sessions_passed?"PASS":"FAIL"));
    if(camera_end_restart_smoke){
        bool safe=true;for(const auto& camera:cameras)safe=safe && !camera->failed && camera->stopped && camera->closed;
        const bool pass=camera_end_probe.Completed() && manual_preview_restarts==1 && preview_sessions==2 && captures_saved==0 && safe;
        Log(std::string("camera_end_restart_test_result=")+(pass?"PASS":"FAIL")+" acquisition_history_passed="+std::to_string(all_sessions_passed)+" shutter_commands=0");
        return pass?0:1; // Expected-ended acquisition remains failed above.
    }
    return all_sessions_passed?0:1;
}
