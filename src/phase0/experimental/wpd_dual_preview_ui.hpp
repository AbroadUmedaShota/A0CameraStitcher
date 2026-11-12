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
HWND pair_button{},review_button{},inspect_button{},session_button{},seam_button{},settings_button{};
// Read-only camera settings (#416 stage A). The table is touched on the UI thread only. The read runs in
// settings_future (A is read and closed, then B). settings_stop belongs to one read: it is only ever set,
// never cleared, and nothing else uses it. settings_close_failed: a body could not be closed after a read,
// so neither another read nor a Live restart is offered. The backend and the restart override are replaced
// only by --settings-ui-smoke, which runs without a camera.
struct SettingsView {std::vector<D810ComparisonRow> rows;std::wstring status;};
std::optional<SettingsView> settings_view;
std::future<SettingsReadOutcome> settings_future;
std::shared_ptr<std::atomic<bool>> settings_stop;
bool settings_close_failed=false;
unsigned settings_reads_started{};
SettingsBackend settings_backend=RealSettingsBackend;
std::function<void()> preview_restart_override;
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
    std::optional<a0::m2::experimental::FixedCompositeSkip> composite_skip;std::string composite_detail;std::array<std::filesystem::path,2> originals;
    // Both originals are saved and the composite step threw (the 180-second window, memory, a size mismatch, the gate). Set with
    // capture.success: the next shot is allowed exactly as after a skip. composite_cleanup is what became of a JPEG without a report.
    std::optional<CompositeFailureReason> composite_failure;ReportlessCleanup composite_cleanup{};
    // The composite and its report are both saved and the 180-second window closed before the writer returned. The composite is kept;
    // the row says the window was passed.
    bool deadline_passed{};};
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
bool FixedConditionPresent(){return fixed_source.Present();}
// The live registration of C for a condition v3 (#417 LC). live_runtime is replaced only by PrepareLiveRuntime (or the smoke), on the
// UI thread while no merge worker exists; the merge worker, its helper thread and Paint only call the runtime's own thread-safe members.
// live_estimator_override and live_design_override are replaced only by --live-c-ui-smoke, which runs without a camera.
std::shared_ptr<LiveRegistrationRuntime> live_runtime;
LiveEstimator live_estimator_override;
std::optional<LiveTrackerDesign> live_design_override;
std::string live_condition_sha;
unsigned live_captures_seen{};
// Read again at every preview session start, so a condition made between sessions is used.
void LoadFixedSource(){
    try{
        auto file=DefaultIdentityMapPath();file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
        fixed_source=LoadFixedPreviewSource(file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
    }catch(const std::exception& error){
        fixed_source={};fixed_source.reason=a0::m2::experimental::FixedCompositeSkip::condition_invalid;fixed_source.detail=error.what();
    }
    std::ostringstream line;line.imbue(std::locale::classic());
    line<<"C fixed_composite_condition="<<(fixed_source.loaded || fixed_source.loaded_v2 || fixed_source.loaded_v3?"present":fixed_source.reason==a0::m2::experimental::FixedCompositeSkip::condition_absent?"absent":"invalid");
    if(fixed_source.loaded){const auto& c=fixed_source.loaded->condition;
        line<<" version=1 sha256="<<fixed_source.loaded->file_sha256<<" seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" approval=unapproved";}
    if(fixed_source.loaded_v2){const auto& c=fixed_source.loaded_v2->condition;const auto& p=fixed_source.loaded_v2->profile;
        line<<" version=2 sha256="<<fixed_source.loaded_v2->file_sha256<<" profile_sha256="<<p.profile_file_sha256<<" profile_fingerprint_sha256="<<p.profile_fingerprint_sha256
            <<" raster="<<c.raster.width_pixels<<"x"<<c.raster.height_pixels<<"@"<<c.raster.dpi<<"dpi seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" approval=unapproved";}
    else if(fixed_source.loaded_v3){const auto& c=fixed_source.loaded_v3->condition;const auto& p=fixed_source.loaded_v3->profile;
        line<<" version=3 sha256="<<fixed_source.loaded_v3->file_sha256<<" profile_sha256="<<p.profile_file_sha256<<" profile_fingerprint_sha256="<<p.profile_fingerprint_sha256
            <<" raster="<<c.raster.width_pixels<<"x"<<c.raster.height_pixels<<"@"<<c.raster.dpi<<"dpi seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" revision="<<c.revision.sequence<<" registration_live="<<(c.registration.live?1:0)<<" approval=unapproved";}
    else if(!fixed_source.loaded && fixed_source.reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent)line<<" reason="<<a0::m2::experimental::FixedCompositeSkipCode(fixed_source.reason);
    Log(line.str());
}
// The live registration of C (#417 LC): a runtime for a condition v3, made at every preview session start (the condition is read
// again there). A session that follows a capture asks for its first full estimate with the reason after-capture, and a session whose
// condition file differs from the one before with condition-reloaded; the first one is session-start. Without a v3 condition there
// is no runtime, and C is drawn as before. A setup failure leaves no runtime: C is drawn with the fixed mapping and says so.
void PrepareLiveRuntime(){
    live_runtime.reset();
    if(!fixed_source.loaded_v3)return;
    const auto& loaded=*fixed_source.loaded_v3;
    try{
        LiveRegistrationRuntime::Config config;
        if(live_design_override)config.design=*live_design_override;
        config.base_pose=loaded.condition.base_pose;
        config.live_enabled=loaded.condition.registration.live.has_value();
        config.mount=mount_orientation;
        const auto origin=Clock::now();
        config.clock=[origin]{return std::chrono::duration<double>(Clock::now()-origin).count();};
        config.log=[](const std::string& line){Log(line);};
        auto runtime=std::make_shared<LiveRegistrationRuntime>(config,loaded.profile.parameters);
        const bool captured=captures_saved!=live_captures_seen;
        const bool reread=!live_condition_sha.empty() && live_condition_sha!=loaded.file_sha256;
        const auto reason=captured?LiveFullReason::after_capture:reread?LiveFullReason::condition_reloaded:LiveFullReason::session_start;
        if(reason!=LiveFullReason::session_start)runtime->Restart(reason);
        live_captures_seen=captures_saved;live_condition_sha=loaded.file_sha256;
        live_runtime=std::move(runtime);
        const auto& t=config.design.tracker;
        std::ostringstream line;line.imbue(std::locale::classic());
        line<<"C live_registration_session reason="<<LiveFullReasonCode(reason)<<" live_enabled="<<(config.live_enabled?1:0)
            <<" design=draft follow_interval_s="<<t.follow_interval_seconds<<" hold_s="<<t.hold_seconds<<" full_retry_s="<<t.full_retry_seconds
            <<" follow_change_limit_px="<<t.follow_change_limit_px<<" inlier_fraction_floor="<<t.inlier_fraction_floor
            <<" brightness_change_limit="<<t.brightness_change_limit<<" tau_bake_px="<<t.bake_threshold_px
            <<" adopt_threshold_px="<<t.adopt_threshold_px<<" adopt_sigma_factor="<<t.adopt_sigma_factor
            <<" difference_samples_per_axis="<<config.design.difference_samples_per_axis<<" quality_accepted=0";
        Log(line.str());
    }catch(const std::exception& error){
        live_runtime.reset();
        Log(std::string("C live_registration_session setup_failed=1 detail=")+error.what());
    }
}
void LogPreviewChoice(const FixedPreviewChoice& choice){
    std::ostringstream line;line.imbue(std::locale::classic());
    const auto& plan=choice.plan;
    line<<"C composite mode="<<(choice.mode==PreviewCMode::fixed_composite?"fixed-composite":choice.mode==PreviewCMode::unavailable?"unavailable":"uncalibrated")
        <<" view="<<plan.view_width<<"x"<<plan.view_height;
    if(choice.mode==PreviewCMode::fixed_composite){
        line<<(plan.lens_aware?" version=2 grid="+std::to_string(plan.output_width)+"x"+std::to_string(plan.output_height)+" grid_pitch_mm="+std::to_string(plan.grid.pitch_mm)+" coordinate_table=1":" version=1")
            <<" capture_seam_column="<<plan.capture_seam_column<<" scaled_seam_column="<<plan.scaled_seam_column<<" seam_column="<<plan.view.seam_column<<" seam_shift="<<plan.seam_shift
            <<" blend_width="<<plan.settings.width_pixels<<" window_half_width="<<plan.settings.brightness_half_width_pixels<<" smoothing_rows="<<plan.settings.brightness_smoothing_rows
            <<" scale_correction=0 correspondence_verified=0 uncalibrated="<<(plan.lens_aware?0:1)<<" quality_accepted=0";
    }
    else if(choice.mode==PreviewCMode::unavailable)line<<" reason="<<a0::m2::experimental::FixedCompositeSkipCode(*plan.skip);
    Log(line.str());
}
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
bool SettingsWriteBusy(); // #416 stage B (wpd_settings_write_ui.hpp)
bool SettingsPanelOpen(); // the settings panel is in the place of the Live area (#416 stage 3)
#endif
bool CanFocus(Camera& camera){
    std::lock_guard lock(camera.mutex);
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(SettingsWriteBusy() || SettingsPanelOpen())return false; // a settings write or reread holds the cameras, and the panel takes the place of the Live area: no AF, no capture
#endif
    return !stopping && !closing && !capture_pending && !capture_future.valid() && !settings_future.valid() && !camera.done && !camera.failed && camera.frames>=20 &&
        Clock::now()-camera.last<std::chrono::seconds(2) && camera.focus_mailbox.Available();
}
// Both Live sessions are joined and closed, nothing is pending: the state in which a Live restart or a
// settings read may start. Each of them also checks that no settings read is running.
bool LiveStoppedAndClosed(){
    if(!preview_joined)return false; // All following non-atomic state is joined.
    std::array<PreviewClosedState,2> state;
    for(size_t i=0;i<2;++i){const auto& c=*cameras[i];state[i]={c.opened,c.start_attempted,c.stopped,c.closed};}
    return PreviewRestartAllowed(preview_joined,closing,cleanup_only,
        capture_pending.has_value() || capture_future.valid() || capture_restart_blocked,state);
}
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
bool SettingsWriteBusy(); // #416 stage B (wpd_settings_write_ui.hpp): a settings write or reread also holds the cameras
bool SettingsReadBusy(){return settings_future.valid() || SettingsWriteBusy();}
// The settings panel stands where the Live area was: Live is not restarted by hand while it is open (closing it is the way out).
bool PanelHoldsLive(){return SettingsPanelOpen();}
#else
bool SettingsReadBusy(){return settings_future.valid();}
bool PanelHoldsLive(){return false;}
#endif
bool CanRestartPreview(){return !SettingsReadBusy() && !settings_close_failed && !PanelHoldsLive() && LiveStoppedAndClosed();}
// Re-checked inside the handler, not only through the button state.
bool CanReadSettings(){return !SettingsReadBusy() && !settings_close_failed && LiveStoppedAndClosed() && settings_backend;}
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
void DiscardSettingsWriteView();
void DiscardSettingsView(){settings_view.reset();DiscardSettingsWriteView();}
#include "wpd_settings_write_ui.hpp"
#else
void DiscardSettingsView(){settings_view.reset();}
#endif
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
// The frames that the worker of the live registration estimates from (#417 LC): the same reduced and oriented pair that C is drawn
// from. LivePairReady is cheap (timestamps only); SnapshotLivePair copies and reduces. Neither holds a camera lock longer than a copy.
bool LivePairReady(){
    std::array<Clock::time_point,2> time{};std::array<bool,2> present{};
    for(int i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);time[i]=c.last;present[i]=!c.pixels.bytes.empty();}
    return !stopping && present[0] && present[1] && PreviewPairFresh(Clock::now(),time[0],time[1]);
}
bool SnapshotLivePair(a0::m2::render::BgrImage& small_a,a0::m2::render::BgrImage& small_b){
    Pixels a,b;Clock::time_point time_a{},time_b{};
    for(int i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);
        if(i==0){a=c.pixels;time_a=c.last;}else{b=c.pixels;time_b=c.last;}}
    if(a.bytes.empty() || b.bytes.empty() || !PreviewPairFresh(Clock::now(),time_a,time_b))return false;
    small_a=SmallBgr(a);small_b=SmallBgr(b);
    a0::m2::experimental::OrientRenderInput(small_a,mount_orientation);a0::m2::experimental::OrientRenderInput(small_b,mount_orientation);
    return true;
}
// The loop of C. With a live registration (condition v3) a second thread runs for as long as this loop does: the worker that
// estimates, bakes camera B's table again and swaps the plan. This loop only reads the plan in force (a shared_ptr under a mutex), so
// the 80 ms pass never estimates and never bakes.
void MergeLoop(std::stop_token stop){
    std::uint64_t last_a{},last_b{},last_revision=~std::uint64_t{};
    std::optional<FixedPreviewChoice> choice;unsigned choice_width{},choice_height{};
    const auto runtime=live_runtime;
    std::jthread live_worker;
    if(runtime){
        try{
            // The estimator is made here, before the worker exists: what making it throws is logged below and C stays on the fixed
            // mapping. Inside the worker's own function nothing would catch it. The condition's ranges go with it (the plan does not
            // carry them); fixed_source is only read while this loop runs.
            const LiveEstimator estimator=live_estimator_override?live_estimator_override:
                MakeLiveEstimator([runtime]{return runtime->BasePlan();},runtime->Design(),ConnectedLiveEstimatorEntrance(),
                    fixed_source.loaded_v3?std::optional<LiveRegistrationRanges>(LiveRegistrationRangesOf(fixed_source.loaded_v3->condition.registration)):std::nullopt);
            live_worker=std::jthread([runtime,estimator](std::stop_token live_stop){
                LiveFrameSource frames;
                frames.ready=[]{return LivePairReady();};
                frames.snapshot=[](a0::m2::render::BgrImage& a,a0::m2::render::BgrImage& b){return SnapshotLivePair(a,b);};
                runtime->Work(live_stop,[]{return stopping.load();},frames,estimator,std::chrono::milliseconds(50));
            });
        }catch(const std::exception& error){Log("C live_registration worker_start_failed=1 detail="+LiveLogSafeText(error.what()));}
        catch(...){Log("C live_registration worker_start_failed=1 detail=an exception of an unknown kind");}
    }
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
                    // #417: the plan chosen for this size is the fixed mapping; the worker bakes B's table from it.
                    if(runtime){if(choice->mode==PreviewCMode::fixed_composite && choice->plan.live)runtime->PublishBase(choice->plan);else runtime->ClearBase();}
                }
                // The plan in force: the base plan until the worker has baked one for the mapping that it estimated.
                const FixedPreviewChoice* shown=&*choice;FixedPreviewChoice baked_choice;
                if(runtime && choice->mode==PreviewCMode::fixed_composite && choice->plan.live){
                    if(const auto current=runtime->CurrentPlan()){baked_choice.mode=choice->mode;baked_choice.plan=*current;shown=&baked_choice;}
                }
                bool align=false;
                {std::lock_guard lock(merged.mutex);
                    merged.source_width=small_a.width;merged.source_height=small_a.height;
                    if(!merged.overlap_pixels)merged.overlap_pixels=small_a.width*30/100;
                    overlap=merged.overlap_pixels;
                    merged.decided=true;merged.mode=choice->mode;merged.fixed_plan=shown->plan;
                    // With a fixed condition no alignment request moves C (#401): nothing is estimated or applied.
                    if(choice->mode!=PreviewCMode::uncalibrated){merged.auto_requested=false;merged.auto_busy=false;}
                    else if(merged.auto_requested && merged.revision==revision){merged.auto_requested=false;align=true;}
                }
                if(choice->mode!=PreviewCMode::uncalibrated){
                    PublishFixedC(*shown,small_a,small_b,time_a,time_b,revision,stop);
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
    if(capture_future.valid() || settings_future.valid() || camera_workers[0].joinable() || camera_workers[1].joinable() || merge_worker.joinable())throw std::runtime_error("preview workers must be closed before restart");
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(SettingsWriteBusy())throw std::runtime_error("a settings write or reread holds the cameras");
#endif
    stopping=false;camera_end_observed=false;ready_count=0;preview_joined=false;ClearMerge();DiscardSettingsView();
    if(!cleanup_only){LoadFixedSource();PrepareLiveRuntime();} // before the merge worker exists (#401, #417)
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
    if(settings_button)MoveWindow(settings_button,std::max(12L,rect.right-645),rect.bottom/2+88,200,28,TRUE);
    if(seam_button)MoveWindow(seam_button,1245,rect.bottom/2+52,140,28,TRUE);
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    LayoutSettingsWriteButtons(rect);
#endif
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
const wchar_t* SettingLabel(D810SettingId id){
    switch(id){
    case D810SettingId::ExposureMode:return L"露出モード";
    case D810SettingId::ShutterSpeed:return L"シャッター速度";
    case D810SettingId::Aperture:return L"絞り";
    case D810SettingId::Iso:return L"ISO感度";
    case D810SettingId::IsoAuto:return L"ISO感度自動制御";
    case D810SettingId::WhiteBalance:return L"ホワイトバランス";
    case D810SettingId::Metering:return L"測光";
    case D810SettingId::ExposureCompensation:return L"露出補正";
    case D810SettingId::PictureControl:return L"ピクチャーコントロール";
    case D810SettingId::ActiveDLighting:return L"アクティブD-ライティング";
    default:return L"";
    }
}
// The side-by-side table of the two bodies' settings (#416). Display only: nothing here sends a command.
bool DrawSettingsView(HDC dc,RECT client){
    if(!settings_view)return false;
    const int top=client.bottom/2+88,left=12,label_width=236;
    const int column=std::max<int>(100,(client.right-2*left-label_width)/3);
    const wchar_t* title=L"両台のカメラ設定（読み取り専用・カメラの設定は変えていません）";
    TextOutW(dc,left,top,title,lstrlenW(title));
    const auto cell=[&](int x,int y,const std::wstring& text,int width){
        RECT area{x,y,x+width-6,y+22};DrawTextW(dc,text.c_str(),-1,&area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
    };
    int y=top+34;
    cell(left,y,L"項目",label_width);cell(left+label_width,y,L"CAM-A（左）",column);cell(left+label_width+column,y,L"CAM-B（右）",column);cell(left+label_width+2*column,y,L"差",column);
    y+=24;
    for(const auto& row:settings_view->rows){
        const bool differs=row.difference==D810Difference::Different;
        SetTextColor(dc,differs?RGB(255,200,90):RGB(240,240,240));
        cell(left,y,SettingLabel(row.id),label_width);
        cell(left+label_width,y,WidenUtf8(row.a_text),column);cell(left+label_width+column,y,WidenUtf8(row.b_text),column);
        cell(left+label_width+2*column,y,differs?L"差あり":row.difference==D810Difference::Same?L"同じ":L"比較不可",column);
        y+=22;
    }
    SetTextColor(dc,RGB(240,240,240));
    return true;
}
// Folds a finished read into the table and the status line. UI thread only.
void ApplySettingsOutcome(const SettingsReadOutcome& outcome){
    LogSettingsOutcome(outcome);
    settings_close_failed=!outcome.CleanClose();
    SettingsView view;
    view.rows=CompareD810Settings(outcome.cameras[0].read.readings,outcome.cameras[1].read.readings);
    if(settings_close_failed)view.status=L"設定を読んだあとにカメラとの接続を閉じられませんでした。ライブは再開できません。この画面を閉じて、本体・USB接続を確認してください。";
    else if(outcome.Complete())view.status=L"両台の設定を読みました（読み取りのみ）。差のある項目: "+std::to_wstring(CountD810Differences(view.rows))+L" / 「ライブ再開」を押すと表は消えます。";
    else{
        view.status=L"設定の読み取りを途中で止めました。読めた項目だけ表示しています。";
        for(const auto& camera:outcome.cameras)if(!camera.read.Complete())
            view.status+=L" "+WidenUtf8(camera.read.alias)+L": "+WidenUtf8(camera.read.stop_reason);
    }
    capture_status=view.status;settings_view=std::move(view);
}
void RequestSettingsRead(){
    // The button state is not trusted: every condition is checked again here.
    if(!CanReadSettings() || PanelHoldsLive()){Log("ui_settings_read_refused=1 setting_writes=0");return;}
    review_visible=false;DiscardSettingsView();
    auto stop=std::make_shared<std::atomic<bool>>(false);settings_stop=stop;
    const auto deadline=Clock::now()+std::chrono::seconds(30);
    capture_status=L"両台の設定を読んでいます（読み取りのみ。A を閉じてから B を開きます）…";
    ++settings_reads_started;Log("ui_settings_read_requested=1 setting_writes=0 shutter_commands=0");
    settings_future=std::async(std::launch::async,[ids=registered_ids,stop,deadline,backend=settings_backend]{
        try{ComScope com;return ReadBothSettings(ids,deadline,*stop,backend);}
        catch(const std::exception& error){
            Log(std::string("settings_read_failure: ")+error.what());
            SettingsReadOutcome failed;
            for(int i=0;i<2;++i){failed.cameras[i].read.alias=i==0?"CAM-A":"CAM-B";MarkSettingsUnread(failed.cameras[i],"exception");}
            return failed;
        }
    });
}
// The two rows above the picture of a registered C (#417 LC): what C is drawn with, and the direction of the two cameras so that they
// can be adjusted before the shot (B is the value against A now; A is the value at calibration). Both sentences come from
// live_registration_display.hpp. Without a runtime (its setup failed) C is the fixed mapping and the rows say so.
inline constexpr int kLiveRowsHeight=46;
void DrawLiveRows(HDC dc,const RECT& client){
    std::string state,orientation;
    if(live_runtime){const auto status=live_runtime->Status();state=status.state_text;orientation=status.orientation_line;}
    else{state=LiveRegistrationStartFailedText();orientation=LiveOrientationRowText(LiveOrientationTextsOf(std::nullopt,std::nullopt));}
    const auto wide_state=WidenUtf8(state),wide_orientation=WidenUtf8(orientation);
    RECT state_rect{12,client.bottom/2+140,client.right-12,client.bottom/2+161};
    RECT orientation_rect{12,client.bottom/2+163,client.right-12,client.bottom/2+184};
    DrawTextW(dc,wide_state.c_str(),-1,&state_rect,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    // The direction is what the person adjusts by: a warmer colour than the notes.
    SetTextColor(dc,RGB(255,224,160));
    DrawTextW(dc,wide_orientation.c_str(),-1,&orientation_rect,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    SetTextColor(dc,RGB(240,240,240));
}
std::array<std::uint64_t,6> Paint(HDC dc,RECT client){
    std::array<std::uint64_t,6> drawn{};
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    settings_write_paint_serial=0; // only a paint that draws a whole confirmation raises it again
#endif
    FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
    if(client.right<200 || client.bottom<240)return drawn;
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    const bool panel_over_live=preview_joined && SettingsPanelOpen();
#else
    const bool panel_over_live=false;
#endif
    for(int i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);
        const int pane=client.right/2,left=i*pane+12,top=174;
        std::wstring title=i==0?L"CAM-A（左）":L"CAM-B（右）";title+=L"   frames: "+std::to_wstring(c.frames);
        if(c.failed)title+=L"   FAILED";else if(stopping)title+=L"   STOPPED / STOPPING";else if(c.frames)title+=L"   LIVE";else title+=L"   STARTING";
        TextOutW(dc,left,16,title.c_str(),static_cast<int>(title.size()));TextOutW(dc,left,82,c.focus_status.c_str(),static_cast<int>(c.focus_status.size()));
        if(!panel_over_live && DrawLevel(dc,left,pane-24,c))drawn[4+i]=c.frames;
        if(!panel_over_live && !c.pixels.bytes.empty() && Clock::now()-c.last<std::chrono::seconds(2) && !stopping &&
            DrawPixels(dc,c.pixels,left,top,pane-24,client.bottom/2-top-8))drawn[i]=c.frames;
    }
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(panel_over_live)DrawSettingsPanel(dc,client); // the settings panel takes the place of both Live areas
#endif
    RECT status_rect{12,client.bottom/2+4,client.right-12,client.bottom/2+86};
    DrawTextW(dc,capture_status.c_str(),-1,&status_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(preview_joined && settings_write && settings_write->screen->View() && DrawSettingsWrite(dc,client))return drawn; // a confirmation or a result is never under the review
#endif
    if(review_visible && !review_pixels.bytes.empty()){
        const wchar_t* title=L"撮影JPEGの合成結果（静止画／未校正・確認用）";
        TextOutW(dc,12,client.bottom/2+88,title,lstrlenW(title));
        const wchar_t* text=L"現在のライブ映像とは別の撮影結果です。「ライブCに戻す」で切り替えられます";
        TextOutW(dc,12,client.bottom/2+110,text,lstrlenW(text));
        if(DrawPixels(dc,review_pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155))drawn[3]=1;
        return drawn;
    }
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(preview_joined && DrawSettingsWrite(dc,client))return drawn;
#endif
    // Both Live sessions are closed and a read finished: the settings table takes the place of C until Live restarts.
    if(settings_view && preview_joined){DrawSettingsView(dc,client);return drawn;}
    std::lock_guard lock(merged.mutex);
    // The title and the standing note name the kind of C (#401): drawn with the fixed composite condition,
    // not drawn although a condition exists, or the uncalibrated C when there is none.
    std::wstring title,status;
    bool live_rows=false; // #417: two more rows (the state of the registration, the direction of the cameras) for a condition v3
    if(merged.decided && merged.mode==PreviewCMode::fixed_composite){
        title=FixedPreviewTitle(mount_orientation,merged.fixed_plan);
        status=merged.position_refusal.empty()?FixedPreviewNote(merged.fixed_plan):merged.position_refusal;
        live_rows=merged.fixed_plan.live!=nullptr;
    }else if(FixedConditionPresent()){
        title=FixedPreviewUnavailableTitle(fixed_source);
        status=merged.decided?merged.position_refusal:L"固定の合成条件を確認しています…";
    }else{
        title=PreviewMergeTitle(mount_orientation,merged.overlap_pixels,merged.vertical_pixels);
        status=std::wstring(kUncalibratedPreviewNotice)+L" / "+merged.alignment_status;
    }
    TextOutW(dc,12,client.bottom/2+88,title.c_str(),static_cast<int>(title.size()));
    RECT status_line{12,client.bottom/2+110,client.right-12,client.bottom/2+138};
    DrawTextW(dc,status.c_str(),-1,&status_line,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    // The picture of C starts lower by the two rows, only for a registered C; every other C keeps its place.
    const int picture_top=client.bottom/2+140+(live_rows?kLiveRowsHeight:0),picture_height=client.bottom/2-155-(live_rows?kLiveRowsHeight:0);
    if(live_rows && !stopping)DrawLiveRows(dc,client);
    if(!stopping && merged.rendered_revision==merged.revision && !merged.pixels.bytes.empty() && PreviewPairFresh(Clock::now(),merged.source_a,merged.source_b)){
        if(DrawPixels(dc,merged.pixels,12,picture_top,client.right-24,picture_height)){
            drawn[2]=merged.frames;
            if(merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.show_seam)
                DrawSeamLine(dc,FitPixels(merged.pixels,12,picture_top,client.right-24,picture_height),merged.pixels,merged.seam_column);
        }
    }else{const auto text=stopping?L"C: ライブビュー停止中です":merged.error.empty()?L"C: 両方の新しいフレームを待っています":merged.error.c_str();TextOutW(dc,12,picture_top+5,text,lstrlenW(text));}
    return drawn;
}
void RequestCapture(int index){
    if(camera_end_restart_smoke)return; // This explicit lifecycle test never exposes.
    if(settings_future.valid())return; // No exposure while the settings are being read.
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(!SettingsWriteAdmitsCapture())return; // a settings write not confirmed holds every capture (ADR-0038 decision 8)
#endif
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
// Seams that only --pair-v3-ui-smoke replaces (a synthetic pair, a temporary settings folder and review root). The defaults are
// what the preview always did: the real pair capture, the settings beside the identity map, the private review root.
std::function<VerifiedStillPair(const std::array<std::wstring,2>&,Clock::time_point)> pair_capture_backend=
    [](const std::array<std::wstring,2>& ids,Clock::time_point deadline){return CaptureStillPairShutterFirst(ids,deadline);};
std::optional<std::filesystem::path> pair_settings_folder_override,pair_review_root_override;
CaptureUiOutcome SavePairReview(const std::array<std::wstring,2>& ids,ReviewGeometry geometry,Clock::time_point deadline){
    CaptureUiOutcome result;
    const auto root=pair_review_root_override?*pair_review_root_override:DefaultIdentityMapPath().parent_path().parent_path()/"wpd-pair-reviews";
    EvidenceWriter evidence(root,NewRunId(),"wpd-uncalibrated-pair-review");
    const auto review_directory=evidence.RunRoot()/"review";
    // True from the moment both originals are on disk. A throw after that is a composite that was not made, never a lost capture.
    bool originals_saved=false;
    try{
        evidence.RecordState("pair","BothLiveViewsClosed");
        // Read first and on every capture, so a condition made between captures is used. The v1 (similarity) and
        // v2 (lens-aware, with its profile) records share the file name; the schema decides (#405 W4). A v3 record is the
        // revision in force in the revisions folder beside it (#417); a v1 or v2 file together with a revision is condition-invalid.
        auto condition_file=DefaultIdentityMapPath();condition_file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
        if(pair_settings_folder_override)condition_file=*pair_settings_folder_override/a0::m2::experimental::kFixedCompositeConditionName;
        const auto selection=a0::m2::experimental::SelectFixedComposite(condition_file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
        if(!selection.loaded && selection.reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent)Log("pair_fixed_composite_condition_unusable reason="+std::string(a0::m2::experimental::FixedCompositeSkipCode(selection.reason))+" detail="+RedactPaths(selection.detail));
        // #389: both shutters precede either import, all under this one deadline.
        auto pair=pair_capture_backend(ids,deadline);
        if(!pair.complete){result.capture.out_of_focus=pair.out_of_focus;result.preflight_refused=!pair.shutter_dispatched[0] && !pair.shutter_dispatched[1];throw std::runtime_error(pair.error);}
        const auto& a=pair.originals[0];const auto& b=pair.originals[1];
        result.capture={true,true,a.path,a.bytes,a.sha256,{}};result.originals={a.path,b.path};originals_saved=true;
        a0::m2::experimental::StillReviewRequest request{a.path,b.path,review_directory,a.sha256,b.sha256,evidence.RunId(),geometry,deadline};
        // #400: geometry and seam come from the fixed composite condition beside the
        // merge policy. Its absence or damage never stops the capture: the originals
        // are kept and only the composite is skipped, with the reason on screen.
        // v1, v2 or v3 by the selected record (pair_review_registration.hpp). The v3 composite takes the deadline of this capture as
        // the time left for its stages: the 180-second window is the one RequestCapture set and is not extended here.
        namespace m2e=a0::m2::experimental;
        auto composed=ComposePairWithSelection(selection,request,merge_policy);
        result.review=std::move(composed.review);result.composite_detail=composed.detail;
        const auto skip=composed.skip;
        if(!result.review){
            result.composite_skip=skip;
            evidence.RecordState("pair","OriginalsSavedCompositeSkipped",{},m2e::FixedCompositeSkipCode(*skip));
            Log("pair_composite_skipped=1 reason="+std::string(m2e::FixedCompositeSkipCode(*skip))+" originals_saved=2 calibrated=0");
            return result;
        }
        // The composite and its report are both written and verified: a window that closed meanwhile does not undo them (owner's
        // decision). The row says the window was passed; the gate is not_run when it had no time.
        if(result.review->deadline_passed || Clock::now()>=deadline){result.deadline_passed=true;Log("pair_review_after_deadline=1 files_retained=1");}
        result.capture={true,true,result.review->jpeg,std::filesystem::file_size(result.review->jpeg),result.review->sha256,{}};
        evidence.RecordState("pair","UncalibratedReviewReady");
        Log("pair_review_saved=1 width="+std::to_string(result.review->width)+" height="+std::to_string(result.review->height)+" mount_orientation="+a0::m2::experimental::MountOrientationName(geometry.orientation)+" originals_rotated=0 calibrated=0");
        if(const auto& fixed=result.review->fixed_composite){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_fixed_composite seam_column="<<fixed->seam_column<<" seam_origin="<<fixed->seam_origin<<" scale_status="<<a0::m2::experimental::FixedScaleStatusCode(fixed->scale.status)
                <<" scale_reason="<<a0::m2::experimental::FixedScaleReasonCode(fixed->scale.reason)<<" applied_scale="<<fixed->scale.applied_scale<<" limit_percent="<<fixed->scale.limit_percent
                <<" policy_check="<<a0::m2::experimental::FixedPolicyCheckCode(fixed->policy_limit.check)<<" estimate_ms="<<result.review->alignment_milliseconds<<" quality_accepted=0";Log(line.str());}
        if(const auto& lens=result.review->fixed_composite_lens){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_fixed_composite_lens seam_column="<<lens->seam_column<<" seam_origin="<<lens->seam_origin<<" scale_status="<<a0::m2::experimental::FixedScaleStatusCode(lens->scale.status)
                <<" scale_reason="<<a0::m2::experimental::FixedScaleReasonCode(lens->scale.reason)<<" applied_scale="<<lens->scale.applied_scale<<" limit_percent="<<lens->scale.limit_percent
                <<" used_tiles="<<lens->scale.used_tiles<<" policy_check="<<a0::m2::experimental::FixedPolicyCheckCode(lens->policy_limit.check)<<" estimate_ms="<<result.review->alignment_milliseconds<<" quality_accepted=0";Log(line.str());}
        if(const auto& seam=result.review->blend){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_blend method="<<(result.review->fixed_composite || result.review->fixed_composite_lens || result.review->fixed_composite_registration?"fixed-seam":"seam-cut")<<" seam_x="<<seam->summary.x_minimum<<".."<<seam->summary.x_maximum<<" ratio="<<seam->ratio_minimum<<".."<<seam->ratio_maximum
                <<" saturation_limited="<<seam->saturation_limited_pixels<<" ms="<<result.review->blend_milliseconds<<" quality_accepted=0";Log(line.str());}
        // v8 (#417): the level, the pose of B and the paper surface, the gate, every warning and the time of each stage. No path and
        // no camera identifier is in these lines.
        if(result.review->fixed_composite_registration)for(const auto& line:DescribeRegistrationReview(*result.review).log)Log(line);
        std::wcout<<L"saved_review="<<result.review->jpeg.wstring()<<std::endl;
    }catch(const std::exception& error){
        // The screen and the log get a summary with no path in it (a filesystem_error carries full paths); the evidence record keeps the text.
        const auto summary=ErrorSummary(error);
        if(originals_saved){
            // Both originals were written before the composite began. Whatever threw (the 180-second window, memory, a size mismatch,
            // the gate), the capture is complete: no composite, originals kept, the next shot allowed as after a skip. A composite JPEG
            // that has no report is removed so that it cannot pass for a finished one. No retry, and no session is touched here.
            const auto reason=ClassifyCompositeFailure(error,Clock::now()>=deadline);
            result.review.reset();result.composite_skip.reset();
            result.composite_failure=reason;result.composite_cleanup=RemoveReportlessComposite(review_directory);
            result.capture.success=true;result.capture.closed=true;
            const std::string code(CompositeFailureCode(reason));
            try{evidence.RecordState("pair","OriginalsSavedCompositeFailed",{},code);}catch(const std::exception&){}
            Log("pair_composite_failed=1 reason="+code+" originals_saved=2 calibrated=0 report_less_jpeg="+std::string(ReportlessCleanupCode(result.composite_cleanup)));
            Log("pair_composite_failure: "+summary);
        }else{
            result.capture.success=false;result.capture.error=summary;
            try{evidence.RecordState("pair","FailedPartial",{},error.what());}catch(const std::exception&){}
            Log("pair_review_failure: "+summary);
        }
    }
    return result;
}
// The status row of a saved capture. A v8 review adds one sentence per line under the path (#417): the level, the pose of B and
// the paper surface, the gate, the warnings, the stages the time budget dropped. Every other review has the one line it always had.
std::wstring SavedCaptureStatus(const CaptureUiOutcome& result){
    std::wstring status=L"保存済み: "+result.capture.path.wstring();
    if(result.review && result.deadline_passed)status+=L"（制限時間の180秒を過ぎましたが、合成は作れています）";
    if(result.composite_skip)status=L"原画像2枚を保存しました（A: "+result.originals[0].wstring()+L"）。合成はしていません: "+WidenUtf8(a0::m2::experimental::FixedCompositeSkipText(*result.composite_skip));
    if(result.composite_failure){
        // The composite step threw after both originals were saved: not a failed capture. The reason is a few words, never the exception's text.
        status=L"合成は作れませんでした。原画像2枚は保存済みです（A: "+result.originals[0].wstring()+L"）。理由: "+WidenUtf8(CompositeFailureText(*result.composite_failure))+L"。";
        if(result.composite_cleanup==ReportlessCleanup::not_removed)status+=L" 報告のない合成画像が残っているかもしれません。その画像は使わないでください。";
        else if(result.composite_cleanup==ReportlessCleanup::complete_kept)status+=L" 合成画像と報告が保存先に残っていますが、完了を確認できていません。";
    }
    if(result.review && result.review->fixed_composite)status+=L" / "+FixedCompositeLine(*result.review->fixed_composite);
    if(result.review && result.review->fixed_composite_lens)status+=L" / "+FixedCompositeLensLine(*result.review->fixed_composite_lens);
    if(result.review && result.review->fixed_composite_registration)
        for(const auto& line:DescribeRegistrationReview(*result.review).status)status+=std::wstring(L"\n")+WidenUtf8(line);
    if(result.review && result.review->automatic)status+=L" / "+StillAlignmentLine(*result.review->automatic);
    return status;
}
void AdvanceCapture(HWND hwnd){
    if(settings_future.valid() && settings_future.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto outcome=settings_future.get();settings_stop.reset();ApplySettingsOutcome(outcome);
    }
    if(!preview_joined && cameras[0]->done && cameras[1]->done){
        const bool safe=FinishPreview();
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        // The start of the capture worker looks at the write again: a write or reread, or a held capture, drops it.
        if(capture_pending && !closing && !SettingsWriteAdmitsCapture())capture_pending.reset();
        else
#endif
        if(capture_pending && !closing){
            const int index=*capture_pending;capture_pending.reset();
            if(safe){
                capture_status=index==2?L"AとBのシャッターを続けて1回ずつ切り、原画像と確認用Merge JPEGを保存しています…":index==0?L"Aを1回撮影してPCへ保存しています…":L"Bを1回撮影してPCへ保存しています…";
                capture_future=std::async(std::launch::async,[ids=registered_ids,index,geometry=pending_geometry,deadline=capture_deadline]{
                    try{ComScope com;if(index==2)return SavePairReview(ids,geometry,deadline);return CaptureUiOutcome{SaveStillOnce(ids[index],index==0?"CAM-A":"CAM-B",true,deadline),{}};}
                    catch(const std::exception& error){CaptureUiOutcome result;result.capture.error=ErrorSummary(error);return result;}
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
            captures_saved+=(result.review || result.composite_skip || result.composite_failure)?2:1;capture_status=SavedCaptureStatus(result);
            if(result.review){
                ++pair_reviews;review_path=result.review->jpeg;const auto& thumbnail=result.review->thumbnail;
                if(pair_cycle_smoke){pair_cycle_reports.push_back(result.review->report);std::wcout<<L"pair_cycle_report="<<result.review->report.wstring()<<std::endl;}
                review_pixels={thumbnail.width,thumbnail.height,{}};review_pixels.bytes.resize(static_cast<size_t>(thumbnail.width)*thumbnail.height*4);
                for(size_t i=0,j=0;i<thumbnail.bgr.size();i+=3,j+=4)std::copy_n(thumbnail.bgr.data()+i,3,review_pixels.bytes.data()+j);
                // The seam is one vertical column (#400): drawn on this display copy only, never in the saved JPEG.
                const auto seam_column=result.review->fixed_composite?std::optional<std::uint32_t>(result.review->fixed_composite->seam_column):
                    result.review->fixed_composite_lens?std::optional<std::uint32_t>(result.review->fixed_composite_lens->seam_column):
                    result.review->fixed_composite_registration?std::optional<std::uint32_t>(result.review->fixed_composite_registration->seam_column):std::nullopt;
                if(seam_column){
                    const auto divisor=std::max(1u,(std::max(result.review->width,result.review->height)+1199)/1200);const auto column=*seam_column/divisor;
                    if(column<thumbnail.width)for(unsigned y=0;y<thumbnail.height;++y){auto* pixel=review_pixels.bytes.data()+(static_cast<size_t>(y)*thumbnail.width+column)*4;pixel[0]=255;pixel[1]=0;pixel[2]=255;}
                }
                review_visible=true;
            }
            // #417 LC: the live registration is asked for a full pass after a shot here, with the originals of this shot gone from
            // the live view: tracker.RequestFull(now, after_capture). J8w does not call it; the tracker is LC's.
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
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(closing && SettingsWriteHoldsWindow(hwnd))return; // a write or reread is waited for the same way
#endif
    // A settings read in progress is waited for: its Close must finish before the window (and the lease) goes.
    if(closing && preview_joined && !capture_future.valid() && !settings_future.valid())DestroyWindow(hwnd);
}
// --pair-v3-ui-smoke <rig profile v2 fixture folder> (#417 J8w): the pair capture's saving path with the conditions of a setup, through the
// real SavePairReview and the status row, on a synthetic pair. No camera and no window; the settings folder and the review root are
// temporary folders, so nothing of the machine is read or written. The frames are flat, so the estimate of B's pose is not trusted
// and the level is G4 (the textured pair, G1 and the dropped stages are in pair_review_registration_contracts).
int RunPairV3UiSmoke(const std::filesystem::path& fixtures){
    Log("pair_v3_ui_synthetic=1 camera_operations=0");
    namespace ps=a0::phase0::experimental::pair_synthetic;
    namespace m2e=a0::m2::experimental;
    unsigned checks{},failures{};
    const auto check=[&](bool ok,const char* what){++checks;if(!ok){++failures;Log(std::string("pair_v3_ui_failed_check=")+what);}};
    const auto work=std::filesystem::temp_directory_path()/("a0-pair-v3-ui-smoke-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    std::ostringstream captured;
    auto* const original_buffer=std::cout.rdbuf(captured.rdbuf());
    try{
        ComScope com;
        std::filesystem::create_directories(work);
        const auto base_profile=ps::ReadText(fixtures/"calibrated-draft.json");
        if(base_profile.empty())throw std::runtime_error("the rig profile fixture is missing");
        const auto profile=ps::ProfileText(base_profile,false);
        const auto raster=ps::TallRaster();
        // One flat pair for every case: the originals are read, never changed.
        const auto frames=ps::WriteFrames(work/"pair",ps::Flat(128),ps::Flat(128));
        pair_review_root_override=work/"reviews";
        pair_capture_backend=[&](const std::array<std::wstring,2>&,Clock::time_point){
            VerifiedStillPair pair;pair.complete=true;pair.preflight_complete=true;pair.shutter_dispatched={true,true};
            const std::array<std::filesystem::path,2> paths{frames.a,frames.b};
            for(size_t i=0;i<2;++i){auto& o=pair.originals[i];o.success=true;o.closed=true;o.path=paths[i];o.bytes=std::filesystem::file_size(paths[i]);o.sha256=ps::Sha(paths[i]);}
            return pair;
        };
        const std::array<std::wstring,2> ids{L"synthetic-a",L"synthetic-b"};
        const auto good_backend=pair_capture_backend;
        const auto capture=[&](const std::filesystem::path& settings,m2e::MountOrientation orientation=m2e::MountOrientation::normal,std::chrono::milliseconds window=std::chrono::seconds(900)){
            pair_settings_folder_override=settings;
            return SavePairReview(ids,ReviewGeometry{460,307,229,3,11,orientation},Clock::now()+window);
        };
        const auto kept=[&](const CaptureUiOutcome& r){return r.capture.success && r.capture.closed && !r.review && r.composite_skip && std::filesystem::exists(r.originals[0]) && std::filesystem::exists(r.originals[1]);};

        // 1. A condition v3: the registered composite and the report v8, the sentences on the status row, the lines in the log.
        const auto v3=ps::MakeSettings(work/"settings-v3",profile,m2e::MountOrientation::normal,raster);
        const auto first_started=Clock::now();
        const auto first=capture(v3.folder);
        const auto first_took=Clock::now()-first_started;
        check(first.capture.success && first.capture.closed && first.review && !first.composite_skip,"v3: a composite is made");
        if(first.review){
            check(first.review->fixed_composite_registration && !first.review->fixed_composite_lens && !first.review->fixed_composite,"v3: the review is a registered one");
            if(first.review->fixed_composite_registration){
                const auto& record=*first.review->fixed_composite_registration;
                check(record.registration.level.level==m2e::GeometryLevel::g4,"v3: flat frames give G4");
                check(record.condition_sha256==v3.loaded.file_sha256 && record.condition_sequence==1,"v3: the revision in force is the one recorded");
                const auto saved=m2e::LoadSavedStillReview(first.review->report);
                check(saved.fixed_composite_registration.has_value() && ps::ReadText(first.review->report).find("a0.uncalibrated-still-review.v8")!=std::string::npos,"v3: the report is v8 and reads back");
                const auto lines=DescribeRegistrationReview(*first.review);
                check(lines.status.size()>=4 && lines.log.size()>=4,"v3: the status and the log have their lines");
                const auto status=SavedCaptureStatus(first);
                check(status.find(L"保存済み: ")==0 && status.find(L"G4")!=std::wstring::npos && status.find(L"警告")!=std::wstring::npos && status.find(L'\n')!=std::wstring::npos,"v3: the status row names the level and the warnings");
                check(lines.warnings.size()==record.warnings.size(),"v3: every warning has its sentence");
            }
        }
        const auto log_after_first=captured.str();
        check(log_after_first.find("pair_review_registration level=G4 ")!=std::string::npos && log_after_first.find("pair_review_registration_pose ")!=std::string::npos &&
            log_after_first.find("pair_review_registration_gate ")!=std::string::npos && log_after_first.find("pair_review_registration_ms estimate_ms=")!=std::string::npos &&
            log_after_first.find("pair_review_blend method=fixed-seam")!=std::string::npos,"v3: the log names the level, the pose, the gate and the time of each stage");
        check(log_after_first.find("quality_accepted=0")!=std::string::npos && log_after_first.find(work.string())==std::string::npos &&
            log_after_first.find("synthetic-a")==std::string::npos && log_after_first.find("CAM-A")==std::string::npos,"v3: the log has no path and no camera identifier");

        // 2. A revision chain with a gap is a damaged condition: no composite, the originals are kept, the reason is on the row.
        {
            const auto gap=ps::MakeSettings(work/"settings-gap",profile,m2e::MountOrientation::normal,raster);
            const auto revisions=gap.folder/std::string(m2e::kFixedCompositeRevisionFolderName);
            const auto second=ps::SampleCondition(gap.folder,profile,m2e::MountOrientation::normal,raster,2,ps::Sha(revisions/"r000001.json"),m2e::FixedCompositeRevisionKind::values);
            m2e::WriteFixedCompositeConditionV3(revisions,second);
            std::filesystem::remove(revisions/"r000001.json");
            const auto r=capture(gap.folder);
            check(kept(r) && *r.composite_skip==m2e::FixedCompositeSkip::condition_invalid,"gap: condition-invalid, originals kept");
            check(SavedCaptureStatus(r).find(L"合成はしていません")!=std::wstring::npos,"gap: the row says the composite was not made");
        }
        // 3. A v2 file together with a revision is two sources.
        {
            const auto both=ps::MakeSettings(work/"settings-both",profile,m2e::MountOrientation::normal,raster);
            m2e::FixedCompositeV2Request request;
            request.profile_file=m2e::CopyFixedCompositeProfile(both.folder,profile,ps::Hash());request.profile_bytes=profile;request.orientation=m2e::MountOrientation::normal;
            request.raster=raster;request.scale_auto_correction_max_percent=1;request.seam_column=ps::kSeam;request.seam_origin=m2e::FixedSeamOrigin::operator_set;
            const auto v2=m2e::MakeFixedCompositeConditionV2(request,std::chrono::system_clock::now(),ps::Hash(),m2e::FixedCompositeCreationCheck([](const m2e::FixedCompositeConditionV2&,const m2e::FixedCompositeProfileBinding&){}));
            m2e::WriteFixedCompositeConditionV2(both.folder/m2e::kFixedCompositeConditionName,v2);
            const auto r=capture(both.folder);
            check(kept(r) && *r.composite_skip==m2e::FixedCompositeSkip::condition_invalid,"v2 with a revision: condition-invalid, originals kept");
            check(captured.str().find("two sources")!=std::string::npos,"v2 with a revision: the log says why");
            // 4. The v2 file alone is the composite it always was (report v7, no v8 lines on the row).
            std::filesystem::remove_all(both.folder/std::string(m2e::kFixedCompositeRevisionFolderName));
            const auto before=captured.str().size();
            const auto only=capture(both.folder);
            check(only.review && only.review->fixed_composite_lens && !only.review->fixed_composite_registration && !only.composite_skip,"v2 alone: the v7 composite");
            check(captured.str().find("pair_review_fixed_composite_lens ",before)!=std::string::npos && captured.str().find("pair_review_registration",before)==std::string::npos,"v2 alone: its own log lines, none of v8");
            if(only.review)check(SavedCaptureStatus(only).find(L"位置合わせを")==std::wstring::npos && SavedCaptureStatus(only).find(L'\n')==std::wstring::npos,"v2 alone: one line on the row");
        }
        // 5. No condition at all: the originals are kept, the reason is condition-absent, nothing is logged as unusable.
        {
            std::filesystem::create_directories(work/"settings-none");
            const auto before=captured.str().size();
            const auto r=capture(work/"settings-none");
            check(kept(r) && *r.composite_skip==m2e::FixedCompositeSkip::condition_absent,"none: condition-absent");
            check(captured.str().find("pair_fixed_composite_condition_unusable",before)==std::string::npos,"none: not logged as unusable");
        }
        // 6. A condition v3 for the other mounting: the writer refuses before it creates anything, the originals are kept.
        {
            const auto r=capture(v3.folder,m2e::MountOrientation::rotated_180);
            check(kept(r) && *r.composite_skip==m2e::FixedCompositeSkip::orientation_differs,"v3: orientation-differs, originals kept");
        }
        // 7. The composite step throws after both originals are saved (#417 M2). The capture is complete: the originals stay, the row says
        // so and why, no composite is left behind, and the next shot is allowed (success and closed, as after a skip).
        const auto composites_under_reviews=[&]{
            unsigned count=0;std::error_code ec;
            for(const auto& entry:std::filesystem::recursive_directory_iterator(work/"reviews",ec))if(entry.path().filename()==L"uncalibrated-review.jpg")++count;
            return count;
        };
        const auto failed_but_kept=[&](const CaptureUiOutcome& r){
            return r.capture.success && r.capture.closed && !r.review && !r.composite_skip && r.composite_failure && std::filesystem::exists(r.originals[0]) && std::filesystem::exists(r.originals[1]);
        };
        {   // 7a. The 180-second window is over before the composite begins.
            const auto before=captured.str().size();const auto composites=composites_under_reviews();
            const auto r=capture(v3.folder,m2e::MountOrientation::normal,std::chrono::milliseconds(1));
            check(failed_but_kept(r) && *r.composite_failure==CompositeFailureReason::time_limit,"expired window: originals kept, reason time_limit");
            const auto status=SavedCaptureStatus(r);
            check(status.find(L"合成は作れませんでした")!=std::wstring::npos && status.find(L"原画像2枚は保存済みです")!=std::wstring::npos && status.find(L"180秒")!=std::wstring::npos &&
                status.find(L"撮影・保存に失敗")==std::wstring::npos,"expired window: the row says the composite was not made and the originals are saved");
            const auto log=captured.str().substr(before);
            check(log.find("pair_composite_failed=1 reason=time_limit originals_saved=2")!=std::string::npos && log.find("pair_review_failure")==std::string::npos &&
                log.find(work.string())==std::string::npos,"expired window: logged as a composite failure, no path in the log");
            check(composites_under_reviews()==composites,"expired window: no composite left behind");
        }
        {   // 7b. Another failure after the JPEG is renamed and before its report exists: the JPEG would pass for a finished composite.
            static std::filesystem::path reviews_root;reviews_root=work/"reviews";
            a0::m2::detail::SetBeforePartialFlushTestHook([]{
                // Takes the report's partial name, so that writing the report fails after the JPEG is in place.
                std::error_code ec;
                for(const auto& entry:std::filesystem::recursive_directory_iterator(reviews_root,ec))
                    if(entry.path().filename()==L"uncalibrated-review.jpg.partial"){std::ofstream blocker(entry.path().parent_path()/L"uncalibrated-review.json.partial",std::ios::binary);blocker<<"x";}
            });
            const auto before=captured.str().size();const auto composites=composites_under_reviews();
            const auto r=capture(v3.folder);
            a0::m2::detail::SetBeforePartialFlushTestHook(nullptr);
            check(failed_but_kept(r) && *r.composite_failure==CompositeFailureReason::other,"report blocked: originals kept, reason error");
            check(r.composite_cleanup==ReportlessCleanup::removed && composites_under_reviews()==composites,"report blocked: the JPEG without a report is removed");
            const auto status=SavedCaptureStatus(r);
            check(status.find(L"合成は作れませんでした")!=std::wstring::npos && status.find(L"原画像2枚は保存済みです")!=std::wstring::npos && status.find(L"詳細はログ")!=std::wstring::npos,"report blocked: the row says why in a few words");
            const auto log=captured.str().substr(before);
            check(log.find("pair_composite_failed=1 reason=error originals_saved=2 calibrated=0 report_less_jpeg=removed")!=std::string::npos && log.find(work.string())==std::string::npos,"report blocked: logged, no path in the log");
        }
        {   // 7c. The shot after a failed composite is an ordinary one.
            const auto composites=composites_under_reviews();
            const auto r=capture(v3.folder);
            check(r.capture.success && r.capture.closed && r.review && !r.composite_failure && composites_under_reviews()==composites+1,"after a failed composite: the next shot makes its composite");
        }
        // The window of the next two cases. 7d needs every stage planned (the plan keeps all of them from a little over 90 s: the stage
        // costs and the tail reserve of the design), so that the gate is dropped by the closed window and not by the first plan. 7e needs
        // only that the composite and its report are written inside it: four times what the first shot took.
        const Clock::duration window_all_stages=std::chrono::seconds(100);
        const auto closing_window=std::max<Clock::duration>(std::chrono::seconds(15),first_took*4);
        static Clock::time_point window_closes;
        const auto sleep_until_window_closes=[]{std::this_thread::sleep_until(window_closes+std::chrono::milliseconds(50));};
        {   // 7d. The window closes after the composite JPEG is saved and verified, before the gate and the report (the owner's decision): the
            // composite is kept, the gate is not run, the report is written and the row says the window was passed.
            window_closes=Clock::now()+window_all_stages;
            a0::m2::detail::SetAfterReviewJpegTestHook(+sleep_until_window_closes);
            const auto before=captured.str().size();const auto composites=composites_under_reviews();
            const auto r=capture(v3.folder,m2e::MountOrientation::normal,std::chrono::duration_cast<std::chrono::milliseconds>(window_all_stages));
            a0::m2::detail::SetAfterReviewJpegTestHook(nullptr);
            check(r.capture.success && r.capture.closed && r.review && !r.composite_failure && !r.composite_skip,"window closed after the JPEG: the composite is kept");
            check(r.deadline_passed && r.review && r.review->deadline_passed && Clock::now()>window_closes,"window closed after the JPEG: the outcome says the window was passed");
            check(composites_under_reviews()==composites+1 && r.review && std::filesystem::exists(r.review->jpeg) && std::filesystem::exists(r.review->report),"window closed after the JPEG: the JPEG and its report are both there");
            if(r.review){
                bool loads=false;m2e::SavedStillReview saved;
                try{saved=m2e::LoadSavedStillReview(r.review->report);loads=true;}catch(const std::exception&){}
                check(loads && saved.fixed_composite_registration && saved.fixed_composite_registration->registration.gate.state==m2e::registration_gate::GateState::not_run,
                    "window closed after the JPEG: the report reads back with the gate not run");
                bool gate_dropped=false;
                if(loads && saved.fixed_composite_registration)for(const auto& skipped:saved.fixed_composite_registration->registration.time_budget.skipped)gate_dropped=gate_dropped || skipped.stage==m2e::ReviewStage::gate;
                check(gate_dropped,"window closed after the JPEG: the time budget in the report names the gate as dropped");
                check(r.review->fixed_composite_registration && r.review->fixed_composite_registration->registration.gate.state==m2e::registration_gate::GateState::not_run,"window closed after the JPEG: the result has the gate not run");
            }
            const auto status=SavedCaptureStatus(r);
            check(status.find(L"保存済み: ")==0 && status.find(L"制限時間")!=std::wstring::npos && status.find(L"合成は作れています")!=std::wstring::npos &&
                status.find(L"合成は作れませんでした")==std::wstring::npos,"window closed after the JPEG: the row says the window was passed and the composite was made");
            const auto log=captured.str().substr(before);
            check(log.find("pair_review_after_deadline=1 files_retained=1")!=std::string::npos && log.find("pair_composite_failed")==std::string::npos && log.find("pair_review_failure")==std::string::npos,
                "window closed after the JPEG: logged as kept, not as a failure");
        }
        {   // 7e. The window closes after the report is saved and before the writer returns: both files stay and the row says the composite was made
            // (this used to be a failure that left the JPEG and the report in place and told the operator that nothing was made).
            window_closes=Clock::now()+closing_window;
            a0::m2::detail::SetBeforeReviewReturnTestHook(+sleep_until_window_closes);
            const auto before=captured.str().size();const auto composites=composites_under_reviews();
            const auto r=capture(v3.folder,m2e::MountOrientation::normal,std::chrono::duration_cast<std::chrono::milliseconds>(closing_window));
            a0::m2::detail::SetBeforeReviewReturnTestHook(nullptr);
            check(r.capture.success && r.capture.closed && r.review && !r.composite_failure && !r.composite_skip && r.deadline_passed,"window closed after the report: the composite is kept and the window was passed");
            check(composites_under_reviews()==composites+1 && r.review && std::filesystem::exists(r.review->jpeg) && std::filesystem::exists(r.review->report),"window closed after the report: the JPEG and its report are both there");
            if(r.review){
                bool loads=false;m2e::SavedStillReview saved;
                try{saved=m2e::LoadSavedStillReview(r.review->report);loads=true;}catch(const std::exception&){}
                check(loads && saved.fixed_composite_registration,"window closed after the report: the report reads back");
            }
            const auto status=SavedCaptureStatus(r);
            check(status.find(L"制限時間")!=std::wstring::npos && status.find(L"合成は作れています")!=std::wstring::npos && status.find(L"合成は作れませんでした")==std::wstring::npos,
                "window closed after the report: the row says the window was passed and the composite was made");
            check(captured.str().substr(before).find("pair_composite_failed")==std::string::npos,"window closed after the report: not logged as a failure");
        }
        // 8. The originals themselves are not saved: the failure it always was (no composite step, no 'originals saved').
        {
            const auto before=captured.str().size();
            pair_capture_backend=[](const std::array<std::wstring,2>&,Clock::time_point){VerifiedStillPair pair;pair.complete=false;pair.error="synthetic save failure";pair.shutter_dispatched={true,true};return pair;};
            const auto r=capture(v3.folder);
            pair_capture_backend=good_backend;
            check(!r.capture.success && !r.composite_failure && !r.review && !r.composite_skip && r.originals[0].empty(),"original save failure: still a failure, nothing claimed as saved");
            check(captured.str().substr(before).find("pair_review_failure: synthetic save failure")!=std::string::npos && captured.str().substr(before).find("pair_composite_failed")==std::string::npos,"original save failure: logged as before");
        }
        // 9. An exception that carries a path (a filesystem_error) is summarised: no path on the screen's error text or in the log.
        {
            const auto before=captured.str().size();
            pair_capture_backend=[&](const std::array<std::wstring,2>&,Clock::time_point)->VerifiedStillPair{
                throw std::filesystem::filesystem_error("create",work/"private-folder"/"pair.jpg",std::make_error_code(std::errc::permission_denied));
            };
            const auto r=capture(v3.folder);
            pair_capture_backend=good_backend;
            const auto log=captured.str().substr(before);
            check(!r.capture.success && r.capture.error.find("private-folder")==std::string::npos && r.capture.error.find(work.string())==std::string::npos && !r.capture.error.empty(),"filesystem_error: the error text has no path");
            check(log.find("pair_review_failure: filesystem error:")!=std::string::npos && log.find("private-folder")==std::string::npos && log.find(work.string())==std::string::npos,"filesystem_error: the log has no path");
        }
    }catch(const std::exception& error){
        ++failures;captured<<"pair_v3_ui_exception: "<<error.what()<<'\n';
    }
    std::cout.rdbuf(original_buffer);
    std::cout<<captured.str();
    pair_capture_backend=[](const std::array<std::wstring,2>& ids,Clock::time_point deadline){return CaptureStillPairShutterFirst(ids,deadline);};
    pair_settings_folder_override.reset();pair_review_root_override.reset();
    std::error_code ignored;std::filesystem::remove_all(work,ignored);
    Log(std::string("pair_v3_ui_result=")+(failures?"FAIL":"PASS")+" checks="+std::to_string(checks)+" failures="+std::to_string(failures));
    return failures?1:0;
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    switch(message){
    case WM_SIZE:
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        settings_write_drawn_serial=0; // the width of the lines is judged at paint time: a resized window has drawn nothing yet
#endif
        LayoutButtons(hwnd);return 0;
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    case WM_GETMINMAXINFO:if(SettingsWriteMinTrack(reinterpret_cast<MINMAXINFO*>(l)))return 0;break;
#endif
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
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        if(HandleSettingsWriteCommand(id,HIWORD(w),reinterpret_cast<HWND>(l)))return 0;
#endif
        if(id==405 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==settings_button){RequestSettingsRead();return 0;}
        if(id==403 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==session_button){
            if(SettingsReadBusy()){Log("ui_live_restart_refused=1 settings_read_in_progress=1");return 0;}
            if(CanRestartPreview()){
                try{
                    // Re-resolve registered identities only after old workers
                    // and both sessions have closed; keep the same lease.
                    DiscardSettingsView(); // The table describes the stopped state; it does not survive a restart.
                    if(preview_restart_override)preview_restart_override();
                    else{registered_ids=Discover();review_visible=false;StartPreview();}
                    ++manual_preview_restarts;manual_restart_waiting=true;
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
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        settings_write_drawn_serial=0; // no confirmation counts as drawn unless this paint presents it whole
#endif
        if(auto offscreen=frame.DrawingContext()){
            const auto drawn=Paint(offscreen,r);
            if(frame.Present(dc)){
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
                settings_write_drawn_serial=settings_write_paint_serial;
#endif
                for(size_t i=0;i<2;++i){auto& c=*cameras[i];std::lock_guard lock(c.mutex);if(drawn[i]>c.paint_sequence){++c.painted;c.paint_sequence=drawn[i];}
                    if(drawn[4+i]>c.level_paint_sequence){++c.level_painted;c.level_paint_sequence=drawn[4+i];}}
                std::lock_guard lock(merged.mutex);if(drawn[2]>merged.paint_sequence){++merged.painted;merged.paint_sequence=drawn[2];}
                if(drawn[3])++review_painted;
            }
        }EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_KEYDOWN:if(w==VK_ESCAPE)SendMessageW(hwnd,WM_CLOSE,0,0);return 0;
    case WM_CLOSE:closing=true;stopping=true;capture_stop_requested=true;capture_pending.reset();
        if(settings_stop)settings_stop->store(true); // Only set. The read ends, closes its body, then the window goes.
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        if(SettingsWriteBusy())return 0; // the wait notice (SettingsWriteHoldsWindow) owns the title; a repeated request must not bring the first wording back
#endif
        SetWindowTextW(hwnd,L"A0 - stopping both cameras / finishing current save...");return 0;
    case WM_TIMER:
        // Every path that sets `closing` (finite runs, failures) also stops a running settings read.
        if(closing && settings_stop)settings_stop->store(true);
        if(manual_restart_waiting && CanFocus(*cameras[0]) && CanFocus(*cameras[1])){
            manual_restart_waiting=false;capture_status=L"ライブビューを再開しました。撮影ボタンで両Liveを停止して撮影・PC保存できます。";
            Log("manual_preview_restart_live=1 both_fresh=1");
        }
        for(int i=0;i<2;++i){const bool enabled=CanFocus(*cameras[i]);for(auto button:focus_buttons[i])if(button)EnableWindow(button,enabled);}
        for(auto button:capture_buttons)if(button)EnableWindow(button,CanFocus(*cameras[0]) && CanFocus(*cameras[1]));
        if(pair_button)EnableWindow(pair_button,CanFocus(*cameras[0]) && CanFocus(*cameras[1]));
        if(review_button){EnableWindow(review_button,!review_pixels.bytes.empty());SetWindowTextW(review_button,review_visible?L"ライブCに戻す":L"撮影結果を見る");}
        if(inspect_button)EnableWindow(inspect_button,!review_path.empty());
        if(settings_button)EnableWindow(settings_button,CanReadSettings() && !PanelHoldsLive());
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
        SettingsWriteTimer();
#endif
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
// Camera-free contract of the read-only settings table (#416 stage A). The real window procedure, timer,
// buttons, future and close path run; only the backend that opens a body is replaced by a synthetic one,
// and a Live restart is replaced by a counter. No Discover/Open/Run and no hardware lease.
int RunSettingsUiSmoke(){
    Log("settings_ui_synthetic=1 camera_operations=0");
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("settings_ui_failed_check="+std::to_string(checks));}};
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.opened=true;c.start_attempted=true;c.started=true;
        c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    stopping=false;run_seconds=0;run_start=Clock::now();preview_joined=false;
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0SettingsUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic settings test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    session_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(403),instance,nullptr);
    settings_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(405),instance,nullptr);
    capture_buttons[0]=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(200),instance,nullptr);
    if(!session_button || !settings_button || !capture_buttons[0]){DestroyWindow(window);return 2;}
    struct Fake {
        std::mutex mutex;std::condition_variable wake;
        bool release=false,saw_stop=false,fail_close=false;
        std::vector<int> started;int closed=0,active=0,max_active=0;
    } fake;
    const auto reading=[](const D810SettingSpec& spec,int index){
        D810SettingReading r;r.id=spec.id;r.available=true;r.mtp_response=0x2001;r.desc.code=spec.code;r.desc.data_type=static_cast<std::uint16_t>(spec.type);r.desc.get_set=1;
        r.desc.current_value=spec.id==D810SettingId::ShutterSpeed?(index?5000:1667):spec.id==D810SettingId::Iso?(index?125:64):spec.id==D810SettingId::WhiteBalance?(index?2:1):1;
        return r;
    };
    settings_backend=[&](int index,const std::wstring&,Clock::time_point,const std::atomic<bool>& stop){
        SettingsCameraResult result;result.read.alias=index==0?"CAM-A":"CAM-B";
        if(stop.load()){MarkSettingsUnread(result,"not_started");return result;}
        {std::unique_lock lock(fake.mutex);fake.started.push_back(index);++fake.active;fake.max_active=std::max(fake.max_active,fake.active);
            fake.wake.wait(lock,[&]{return fake.release;}); // the read, then the Close, take their time
            if(stop.load())fake.saw_stop=true;
            --fake.active;++fake.closed;}
        result.clean_close=!(fake.fail_close && index==1);
        if(stop.load()){MarkSettingsUnread(result,"interrupted");return result;}
        for(const auto& spec:kD810SettingSpecs)result.read.readings.push_back(reading(spec,index));
        return result;
    };
    unsigned restarts{};preview_restart_override=[&]{++restarts;};
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
    const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
    const auto wait_until=[&](auto ready){for(int i=0;i<400;++i){if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;};
    const auto started_count=[&]{std::lock_guard lock(fake.mutex);return fake.started.size();};
    const auto release=[&]{{std::lock_guard lock(fake.mutex);fake.release=true;}fake.wake.notify_all();};
    const auto rearm=[&]{std::lock_guard lock(fake.mutex);fake.release=false;fake.started.clear();};
    const auto finished=[&]{return settings_future.valid() && settings_future.wait_for(std::chrono::seconds(0))==std::future_status::ready;};
    const auto stop_live=[&]{for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}preview_joined=true;stopping=true;};

    // 1. While Live runs: the button is off and a direct command does nothing.
    tick();check(!IsWindowEnabled(settings_button));
    click(405,settings_button);
    check(!settings_future.valid() && settings_reads_started==0 && started_count()==0);

    // 2. Live stopped and both sessions closed: the read is offered.
    stop_live();tick();
    check(IsWindowEnabled(settings_button)!=FALSE && IsWindowEnabled(session_button)!=FALSE && CanReadSettings());

    // 3. A read starts; A is open and nothing else is.
    click(405,settings_button);
    check(settings_future.valid() && settings_reads_started==1 && settings_stop && !settings_stop->load());
    check(wait_until([&]{return started_count()==1;}));
    check(capture_status.find(L"読んでいます")!=std::wstring::npos);

    // 4. While it runs: no second read, no Live restart, no capture, even if the cameras looked live.
    tick();
    check(!IsWindowEnabled(settings_button) && !IsWindowEnabled(session_button));
    click(405,settings_button);click(403,session_button);
    check(settings_reads_started==1 && restarts==0 && manual_preview_restarts==0 && started_count()==1);
    check(!CanRestartPreview() && !CanReadSettings());
    stopping=false;for(auto& camera:cameras){camera->done=false;camera->last=Clock::now();}
    check(!CanFocus(*cameras[0]) && !CanFocus(*cameras[1]));
    RequestCapture(0);RequestCapture(1);RequestCapture(2);click(200,capture_buttons[0]);
    check(!capture_pending.has_value());
    stop_live();
    release();
    check(wait_until(finished));
    {std::lock_guard lock(fake.mutex);check(fake.max_active==1 && fake.started.size()==2 && fake.started[0]==0 && fake.started[1]==1 && fake.closed==2);}

    // 5. The result: the table with the differences, in place of C.
    tick();
    check(!settings_future.valid() && !settings_close_failed && settings_view.has_value());
    if(settings_view){
        check(settings_view->rows.size()==10 && CountD810Differences(settings_view->rows)==3);
        for(const auto& row:settings_view->rows)
            if(row.id==D810SettingId::ShutterSpeed)check(row.a_text=="1/6 s" && row.b_text=="1/2 s" && row.difference==D810Difference::Different);
            else if(row.id==D810SettingId::Aperture)check(row.difference==D810Difference::Same);
    }
    check(capture_status.find(L"差のある項目: 3")!=std::wstring::npos);
    tick(); // the buttons follow the state of the previous tick's end
    check(IsWindowEnabled(settings_button)!=FALSE && IsWindowEnabled(session_button)!=FALSE);
    {
        HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1400;info.bmiHeader.biHeight=-960;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc || !bitmap)check(false);
        else{
            auto old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
            const RECT client{0,0,1400,960};
            unsigned lit{};for(int y=client.bottom/2+100;y<client.bottom/2+400;++y)for(int x=12;x<1000;++x)if(GetPixel(dc,x,y)!=RGB(0,0,0))++lit;
            check(lit==0);
            Paint(dc,client);GdiFlush();
            unsigned drawn{};for(int y=client.bottom/2+100;y<client.bottom/2+400;++y)for(int x=12;x<1000;++x)if(GetPixel(dc,x,y)!=RGB(0,0,0))++drawn;
            check(drawn>200);
            SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
        }
    }

    // 6. Live restart: the table goes away.
    click(403,session_button);
    check(restarts==1 && manual_preview_restarts==1 && !settings_view.has_value());
    manual_restart_waiting=false;

    // 7. A body that cannot be closed after a read: no further read, no Live restart.
    rearm();fake.fail_close=true;
    click(405,settings_button);
    check(wait_until([&]{return started_count()==1;}));
    release();check(wait_until(finished));tick();tick();
    check(settings_close_failed && settings_view.has_value());
    check(!CanReadSettings() && !CanRestartPreview() && !IsWindowEnabled(settings_button) && !IsWindowEnabled(session_button));
    click(405,settings_button);click(403,session_button);
    check(settings_reads_started==2 && restarts==1);
    settings_close_failed=false;fake.fail_close=false;DiscardSettingsView();tick();
    check(CanReadSettings());

    // 8. WM_CLOSE during a read: the window and the lease wait for the body to be closed.
    rearm();
    click(405,settings_button);
    check(wait_until([&]{return started_count()==1;}));
    SendMessageW(window,WM_CLOSE,0,0);
    check(settings_stop && settings_stop->load());
    for(int i=0;i<3;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    check(IsWindow(window)!=FALSE && settings_future.valid());
    {std::lock_guard lock(fake.mutex);check(fake.closed==4);} // A is still being read and closed
    release();check(wait_until(finished));tick();
    check(!IsWindow(window) && !settings_future.valid());
    {std::lock_guard lock(fake.mutex);check(fake.saw_stop && fake.closed==5 && fake.started.size()==1);} // B was never opened

    if(IsWindow(window))DestroyWindow(window);
    settings_backend=RealSettingsBackend;preview_restart_override=nullptr;stopping=true;
    Log("settings_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
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

    // 1b. A lens-aware condition (v2, #405 W5): the same real worker, Paint, buttons and admission. The profile is two
    // affine synthetic cameras built here (a sheet position per pixel, no file and no hash), the condition is in memory.
    const auto lens_condition=[](m2::MountOrientation orientation){
        namespace render=a0::m2::render;
        const render::OutputRaster raster{render::DocumentRegionUm{540000,100000,620000,140000},254,800,400};
        const render::CameraIntrinsics intrinsics{10000,10000,3679.5,2455.5};const render::LensDistortion no_lens{0,0,0,0,0};
        const render::DocumentRenderParameters parameters{raster,render::CameraProjection{intrinsics,no_lens,{0,8,100,-8,0,4800,0,0,1}},
            render::CameraProjection{intrinsics,no_lens,{0,8,100,-8,0,9400,0,0,1}},a0::m2::StitchLayout::camera_a_left_camera_b_right,render::Resampling::bilinear};
        m2::FixedCompositeConditionV2 c;
        c.created_utc="2026-11-04T12:00:00Z";c.orientation=orientation;c.raster=raster;c.scale_auto_correction_max_percent=1;
        c.seam={400,m2::FixedSeamOrigin::operator_set}; // inside the overlap of 561 to 600 mm
        return m2::LoadedFixedCompositeConditionV2{c,std::string(64,'c'),m2::FixedCompositeProfileBinding{parameters,std::string("smoke-profile"),std::string(64,'d'),std::string(64,'e')}};
    };
    fixed_source={};fixed_source.loaded_v2=lens_condition(mount_orientation);
    check(FixedConditionPresent());
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.frames>=3 && !merged.pixels.bytes.empty();}));
    {
        std::lock_guard lock(merged.mutex);
        check(merged.mode==PreviewCMode::fixed_composite && merged.error.empty() && merged.fixed_plan.lens_aware);
        check(merged.pixels.width==merged.fixed_plan.output_width && merged.pixels.height==merged.fixed_plan.output_height && merged.pixels.width>0);
        check(merged.seam_column==merged.fixed_plan.view.seam_column && merged.fixed_plan.view_width==400 && merged.fixed_plan.view_height==267);
        check(merged.ratio_minimum>0.4 && merged.ratio_maximum<0.6);
    }
    paint();
    {
        const auto rect=picture_rect();
        check(rect.width>0 && rect.height>0);
        const auto lit=pink(rect);
        check(lit+2>=static_cast<unsigned>(rect.height) && lit<=static_cast<unsigned>(rect.height));
        std::uint64_t revision{};unsigned overlap{};int vertical{};
        {std::lock_guard lock(merged.mutex);revision=merged.revision;overlap=merged.overlap_pixels;vertical=merged.vertical_pixels;}
        for(int id=300;id<=304;++id)press(id);
        std::lock_guard lock(merged.mutex);
        check(merged.revision==revision && merged.overlap_pixels==overlap && merged.vertical_pixels==vertical && !merged.auto_requested && !merged.auto_busy && !merged.position_refusal.empty());
    }
    check(capture_admitted());
    stop_worker();rearm();ClearMerge();

    // 1c. The same lens-aware condition made for the other mounting is not drawn, with its reason.
    fixed_source={};fixed_source.loaded_v2=lens_condition(m2::MountOrientation::rotated_180);
    check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::unavailable && merged.rendered_revision==merged.revision && merged.source_a.time_since_epoch().count()>0;}));
    {
        std::lock_guard lock(merged.mutex);
        check(merged.pixels.bytes.empty() && merged.error==FixedPreviewUnavailableText(m2::FixedCompositeSkip::orientation_differs));
        check(FixedPreviewUnavailableTitle(fixed_source).find(L"v2")!=std::wstring::npos);
    }
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

    // 3. A damaged file, a version 2 condition without its profile, and one whose profile differs: the same,
    // each with its own reason, and the profile reasons worded as the pair capture words them.
    for(const auto reason:{m2::FixedCompositeSkip::condition_invalid,m2::FixedCompositeSkip::profile_absent,m2::FixedCompositeSkip::profile_differs}){
        fixed_source={};fixed_source.reason=reason;
        check(FixedConditionPresent());
        check(start_worker([]{return merged.decided && merged.mode==PreviewCMode::unavailable && merged.rendered_revision==merged.revision && merged.source_a.time_since_epoch().count()>0;}));
        {
            std::lock_guard lock(merged.mutex);
            check(merged.pixels.bytes.empty() && merged.error==FixedPreviewUnavailableText(reason));
            check(merged.fixed_plan.skip && *merged.fixed_plan.skip==reason);
        }
        if(reason!=m2::FixedCompositeSkip::condition_invalid){
            const auto capture_text=WidenUtf8(m2::FixedCompositeSkipText(reason));
            check(!capture_text.empty() && FixedPreviewUnavailableText(reason).rfind(capture_text,0)==0);
        }
        check(capture_admitted());
        stop_worker();rearm();ClearMerge();
    }

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
// Camera-free contract of the registered live C (condition v3, #417 LC), on synthetic frames. The real merge worker with its helper
// thread (the worker of the live registration), Paint with its two rows, the position buttons and the pair-capture admission run;
// the condition comes from memory (two pinhole cameras built here), the estimator is a scripted function (live_estimator_override,
// as preview_restart_override is for the settings smoke), and the design constants are shortened so that holding ends within the
// run. No settings file is read or written, and no Discover/Open/Run or hardware lease exists.
int RunLiveCUiSmoke(){
    Log("live_c_ui_synthetic=1 camera_operations=0");
    namespace m2=a0::m2::experimental;
    namespace render=a0::m2::render;
    namespace cr=a0::m2::experimental::content_registration;
    using namespace std::chrono_literals;
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("live_c_ui_failed_check="+std::to_string(checks));}};

    // ---- the rig: A level, B turned by 1 degree and tilted by 0.5, both seeing the whole raster of the condition ----
    using Mat=std::array<double,9>;
    const auto mul=[](const Mat& a,const Mat& b){Mat r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[i*3+j]+=a[i*3+k]*b[k*3+j];return r;};
    const auto rot_x=[](double degrees){const double c=std::cos(degrees*std::numbers::pi/180),s=std::sin(degrees*std::numbers::pi/180);return Mat{1,0,0,0,c,-s,0,s,c};};
    const auto rot_z=[](double degrees){const double c=std::cos(degrees*std::numbers::pi/180),s=std::sin(degrees*std::numbers::pi/180);return Mat{c,-s,0,s,c,0,0,0,1};};
    const auto pinhole=[&](const Mat& r,double x0,double y0,double height){
        constexpr double focal=6000,cx=3680,cy=2456;
        const std::array<double,3> t{-(r[0]*x0+r[1]*y0+r[2]*(-height)),-(r[3]*x0+r[4]*y0+r[5]*(-height)),-(r[6]*x0+r[7]*y0+r[8]*(-height))};
        Mat h=mul(Mat{focal,0,cx,0,focal,cy,0,0,1},Mat{r[0],r[1],t[0],r[3],r[4],t[1],r[6],r[7],t[2]});
        for(auto& v:h)v/=h[8];
        return render::CameraProjection(render::CameraIntrinsics(focal,focal,cx,cy),render::LensDistortion(0,0,0,0,0),h);
    };
    const render::OutputRaster raster{render::DocumentRegionUm{450000,60000,650000,160000},254,2000,1000};
    const auto profile=[&](){
        return m2::FixedCompositeProfileBinding{render::DocumentRenderParameters{raster,pinhole(rot_x(0),520,110,1100),pinhole(mul(rot_z(1.0),rot_x(0.5)),580,110,1100),
            a0::m2::StitchLayout::camera_a_left_camera_b_right,render::Resampling::bilinear},std::string("smoke-profile"),std::string(64,'d'),std::string(64,'e')};
    };
    const auto condition=[&](bool live,const std::string& sha){
        m2::FixedCompositeConditionV3 c;
        c.created_utc="2026-01-01T00:00:00Z";c.revision.sequence=1;
        c.source.profile_file="profile.json";c.source.profile_file_sha256=std::string(64,'a');c.source.profile_fingerprint_sha256=std::string(64,'b');c.source.policy_sha256=std::string(64,'c');
        c.orientation=mount_orientation;c.raster=raster;c.seam.column=1000;c.seam.origin=m2::FixedSeamOrigin::operator_set;
        c.registration.envelope.position_mm=10;c.registration.envelope.rotation_deg=2;c.registration.envelope.tilt_deg=1;c.registration.envelope.height_percent=5;
        c.registration.geometry={8,16,256};c.registration.surface_envelope=m2::SurfaceEnvelope{20,4};c.registration.apply_beyond_envelope=true;
        c.registration.parallax=m2::FixedCompositeConditionV3::Registration::Parallax{1};
        if(live){m2::FixedCompositeConditionV3::Registration::Live l;l.limits.brown_lowe_alpha=8;l.limits.brown_lowe_beta=0.3;l.limits.minimum_unique_blocks=3;
            l.limits.maximum_prediction_sigma_px=0.25;l.limits.maximum_residual_rms_px=0.3;l.limits.maximum_residual_p95_px=0.6;l.limits.maximum_bin_bias_px=0.25;
            l.limits.convergence_px=0.05;l.limits.start_agreement_px=0.25;c.registration.live=l;}
        c.seam_gate.limit_px=1;c.seam_gate.guard_band_px=0.2;
        return m2::LoadedFixedCompositeConditionV3{c,sha,profile()};
    };
    const auto lens_condition=[&](){
        m2::FixedCompositeConditionV2 c;
        c.created_utc="2026-11-04T12:00:00Z";c.orientation=mount_orientation;c.raster=raster;c.scale_auto_correction_max_percent=1;
        c.seam={1000,m2::FixedSeamOrigin::operator_set};
        return m2::LoadedFixedCompositeConditionV2{c,std::string(64,'c'),profile()};
    };

    // ---- the scripted estimator: what it is asked on which thread, and what it answers ----
    struct Script {
        std::atomic<int> mode{0};         // 0: pose A, 1: not usable, 2: a pose that cannot be drawn, 3: pose B
        std::atomic<int> calls{0};
        std::atomic<int> wrong_frames{0};
        std::mutex mutex;
        std::vector<std::thread::id> threads;
        std::vector<LiveJobKind> kinds;
    };
    const auto script=std::make_shared<Script>();
    const cr::PoseParameters pose_a{0.3,-0.2,0.5,1.5,-1.0,0.0},pose_b{0.1,0.1,0.2,5.0,2.0,0.0};
    const auto install_estimator=[&]{
        live_estimator_override=[script,pose_a,pose_b](const LiveJob& job,const render::BgrImage& a,const render::BgrImage& b){
            ++script->calls;
            if(a.width!=400 || a.height!=267 || b.width!=400 || b.height!=267)++script->wrong_frames;
            {std::lock_guard lock(script->mutex);script->threads.push_back(std::this_thread::get_id());script->kinds.push_back(job.kind);}
            LiveEstimateOutcome outcome;
            const int mode=script->mode;
            if(mode==1){outcome.reasons.emplace_back("too-few-points");return outcome;}
            outcome.usable=true;
            outcome.pose=mode==2?cr::PoseParameters{0,0,0.4,3000.0,0,0}:mode==3?pose_b:pose_a;
            outcome.band_brightness=100;
            if(job.kind==LiveJobKind::follow){outcome.change_px=0.1;outcome.inlier_fraction=0.9;}
            return outcome;
        };
    };
    // Shortened lines so that holding ends within the run; the other values are the draft design's. The significance line is off here:
    // the scripted estimator moves between two poses with the change of a small step, and the run checks the plumbing (threads, frames,
    // what the window shows), not the policy of when an estimate replaces the one in force.
    LiveTrackerDesign shortened=kLiveTrackerDesignDraft;
    shortened.tracker.follow_interval_seconds=0.15;shortened.tracker.hold_seconds=0.8;shortened.tracker.full_retry_seconds=0.3;
    shortened.tracker.adopt_threshold_px=0;shortened.tracker.adopt_sigma_factor=0;

    const auto frame=[](BYTE b,BYTE g,BYTE r){Pixels p{400,267,{}};p.bytes.assign(static_cast<size_t>(p.width)*p.height*4,0);
        for(size_t i=0;i<p.bytes.size();i+=4){p.bytes[i]=b;p.bytes[i+1]=g;p.bytes[i+2]=r;}return p;};
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.pixels=i==0?frame(100,100,100):frame(200,200,200);
        c.frames=25;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    stopping=false;run_seconds=0;run_start=Clock::now();
    const auto refresh=[&]{for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}};
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0LiveCUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic live C test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
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
    const auto paint=[&]{refresh();Paint(dc,client);GdiFlush();};
    const auto wait_for=[&](auto ready,int milliseconds=10000){
        for(int i=0;i<milliseconds/50;++i){refresh();std::this_thread::sleep_for(50ms);if(ready())return true;}
        return false;
    };
    const auto press=[&](int id){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(id==404?seam_button:merge_buttons[id-300]));};
    const auto capture_admitted=[&]{
        capture_pending.reset();RequestCapture(2);const bool admitted=capture_pending.has_value() && *capture_pending==2;
        capture_pending.reset();return admitted;
    };
    // The pair must be fresh when the capture is asked for, and a busy machine can make a check or a paint take longer than that:
    // ask again with fresh frames a few times; a refusal is still a failure after that.
    const auto capture_admitted_soon=[&]{
        for(int i=0;i<40;++i){refresh();std::this_thread::sleep_for(60ms);if(capture_admitted())return true;}
        return false;
    };
    const auto rearm=[&]{stopping=false;capture_stop_requested=false;capture_pending.reset();};
    const auto drawn_plan=[&]{std::lock_guard lock(merged.mutex);return merged.fixed_plan;};
    const auto drawn_frames=[&]{std::lock_guard lock(merged.mutex);return merged.frames;};
    const auto picture_ready=[&]{std::lock_guard lock(merged.mutex);return merged.decided && merged.mode==PreviewCMode::fixed_composite && merged.frames>=3 && !merged.pixels.bytes.empty();};
    // The picture of a registered C starts below the two rows.
    const auto picture_rect=[&]{std::lock_guard lock(merged.mutex);
        return FitPixels(merged.pixels,12,client.bottom/2+140+kLiveRowsHeight,client.right-24,client.bottom/2-155-kLiveRowsHeight);};
    const auto pink=[&](const PixelRect& rect){unsigned count{};
        for(int y=rect.top;y<rect.top+rect.height;++y)for(int x=rect.left;x<rect.left+rect.width;++x)if(GetPixel(dc,x,y)==kSeamLineColor)++count;return count;};
    const auto lit=[&](int top,int bottom){unsigned count{};for(int y=top;y<bottom;++y)for(int x=12;x<900;++x)if(GetPixel(dc,x,y)!=RGB(0,0,0))++count;return count;};
    const auto start=[&](bool live,bool scripted){
        fixed_source={};fixed_source.loaded_v3=condition(live,std::string(64,'1'));
        live_design_override=shortened;
        if(scripted)install_estimator();else live_estimator_override=nullptr;
        script->mode=0;script->calls=0;script->wrong_frames=0;{std::lock_guard lock(script->mutex);script->threads.clear();script->kinds.clear();}
        PrepareLiveRuntime();
        merge_worker=std::jthread(MergeLoop);
    };
    const auto finish=[&]{
        merge_worker.request_stop();merge_worker.join();rearm();ClearMerge();
    };
    const auto state_text=[&]{return live_runtime?live_runtime->Status().state_text:std::string();};
    const auto contains=[](const std::string& text,const char* part){return text.find(part)!=std::string::npos;};

    // 1. The connected estimate (the real connection, with the condition's ranges) on frames that are plain grey: there is nothing
    //    to estimate from, and it says so. C is drawn with the condition's fixed mapping, the rows say that nothing could be
    //    estimated (not that C is registered, and not that the estimate is unconnected), A's direction is shown and B's is not
    //    measured; the position requests change nothing and the pair capture stays admitted.
    check(captures_saved==0);
    start(true,false);
    check(live_runtime!=nullptr && live_runtime->Enabled());
    check(wait_for(picture_ready));
    check(wait_for([&]{return contains(state_text(),"推定できない");}));
    {
        const auto plan=drawn_plan();
        check(plan.live!=nullptr && plan.live->baked.pose.IsZero() && plan.lens_aware);
        const auto status=live_runtime->Status();
        check(status.view.state==LiveRegistrationState::fixed && status.view.counters.full_jobs>=1 && status.rebakes==0);
        check(contains(status.state_text,"固定の写像") && !contains(status.state_text,"未接続") && !contains(status.state_text,"目安"));
        check(status.view.fixed_reason=="estimate-failed" && !status.view.last_failure.empty() &&
            std::find(status.view.last_failure.begin(),status.view.last_failure.end(),std::string(kLiveEstimatorUnavailableReason))==status.view.last_failure.end());
        check(contains(status.orientation_line,"B: 向きを測れていません") && contains(status.orientation_line,"A（校正時）: ") && !contains(status.orientation.a_short,"測れていません"));
    }
    paint();
    {
        const auto rect=picture_rect();
        check(rect.width>0 && rect.height>0);
        const auto pink_count=pink(rect);
        check(pink_count+2>=static_cast<unsigned>(rect.height) && pink_count<=static_cast<unsigned>(rect.height));
        check(rect.top>=client.bottom/2+140+kLiveRowsHeight);
        check(lit(client.bottom/2+140,client.bottom/2+161)>100 && lit(client.bottom/2+163,client.bottom/2+184)>100); // both rows have text
    }
    {
        std::uint64_t revision{};{std::lock_guard lock(merged.mutex);revision=merged.revision;}
        for(int id=300;id<=304;++id)press(id);
        std::lock_guard lock(merged.mutex);
        check(merged.revision==revision && !merged.auto_requested && !merged.position_refusal.empty());
    }
    check(capture_admitted_soon());
    finish();
    check(live_runtime->WorkersRunning()==0);
    live_runtime.reset();

    // 2. A scripted estimate: the worker (not the loop of C, not this thread) estimates and bakes, the loop draws with the new plan,
    //    the picture goes on, and the rows follow.
    start(true,true);
    check(wait_for(picture_ready));
    check(wait_for([&]{const auto plan=drawn_plan();return plan.live && plan.live->baked.pose.tx_mm==pose_a.tx_mm;}));
    {
        const auto plan=drawn_plan();
        check(plan.live && plan.live->baked.pose.rx_deg==pose_a.rx_deg && plan.live->baked.surface.IsZero());
        const auto status=live_runtime->Status();
        check(status.view.state==LiveRegistrationState::tracking && contains(status.state_text,"目安") && status.rebakes>=1 && !status.bake_failed);
        check(!contains(status.orientation.b_short,"測れていません") && contains(status.orientation.b_short,"B: "));
        check(plan.output_width>0 && plan.output_width==plan.mapped->width);
        std::lock_guard lock(script->mutex);
        check(!script->threads.empty());
        bool separate=true;
        for(const auto id:script->threads)separate=separate && id!=std::this_thread::get_id() && id!=merge_worker.get_id();
        check(separate);                      // neither this thread nor the loop of C estimates
        check(script->kinds.front()==LiveJobKind::full);
    }
    {
        // The loop of C keeps drawing while the worker follows: the number of drawn frames grows and follow-ups arrive.
        const auto before=drawn_frames();const int calls=script->calls;
        check(wait_for([&]{return drawn_frames()>before+2 && script->calls>calls+1;}));
        check(script->wrong_frames==0);
    }
    paint();
    {
        const auto rect=picture_rect();
        const auto pink_count=pink(rect);
        check(rect.width>0 && pink_count+2>=static_cast<unsigned>(rect.height));
        std::lock_guard lock(merged.mutex);
        check(merged.error.empty() && merged.pixels.width==merged.fixed_plan.output_width && merged.pixels.height==merged.fixed_plan.output_height);
    }
    // The pose moves a long way: a follow-up (a change under the line) is taken and the plan is baked again.
    script->mode=3;
    check(wait_for([&]{const auto plan=drawn_plan();return plan.live && plan.live->baked.pose.tx_mm==pose_b.tx_mm;}));
    check(live_runtime->Status().rebakes>=2 && live_runtime->Status().view.state==LiveRegistrationState::tracking);
    // A pose that cannot be drawn: the sentence says so, the plan in force is kept, and the picture goes on.
    script->mode=2;
    check(wait_for([&]{return live_runtime->Status().bake_failed;}));
    {
        const auto status=live_runtime->Status();
        check(contains(status.state_text,"描けませんでした") && status.rebake_failures>=1);
        const auto plan=drawn_plan();
        check(plan.live && plan.live->baked.pose.tx_mm==pose_b.tx_mm);
        const auto before=drawn_frames();
        check(wait_for([&]{return drawn_frames()>before+2;}));
        paint();
        check(pink(picture_rect())>0);
    }
    // A pose that can be drawn again clears the sentence.
    script->mode=0;
    check(wait_for([&]{const auto plan=drawn_plan();return !live_runtime->Status().bake_failed && plan.live && plan.live->baked.pose.tx_mm==pose_a.tx_mm;}));
    // No estimate can be made: the last one is held, then C goes back to the fixed mapping and B's direction is not measured.
    script->mode=1;
    check(wait_for([&]{return live_runtime->Status().view.state==LiveRegistrationState::holding;}));
    check(contains(live_runtime->Status().state_text,"直前の推定"));
    check(wait_for([&]{return live_runtime->Status().view.state==LiveRegistrationState::fixed;}));
    check(wait_for([&]{const auto plan=drawn_plan();return plan.live && plan.live->baked.pose.IsZero();}));
    check(contains(live_runtime->Status().orientation.b_short,"B: 向きを測れていません"));
    // The estimate comes back.
    script->mode=0;
    check(wait_for([&]{return live_runtime->Status().view.state==LiveRegistrationState::tracking;}));
    check(capture_admitted_soon());
    finish();
    check(live_runtime->WorkersRunning()==0);
    {
        // After the loop has ended nobody estimates any more.
        const int calls=script->calls;
        std::this_thread::sleep_for(300ms);
        check(script->calls==calls);
    }
    live_runtime.reset();

    // 3. The reasons of the first full estimate of a session: a start, a session after a capture, a condition read again.
    fixed_source={};fixed_source.loaded_v3=condition(true,std::string(64,'1'));
    live_captures_seen=captures_saved;live_condition_sha.clear();
    PrepareLiveRuntime();
    check(live_runtime && live_runtime->Status().view.full_reason==LiveFullReason::session_start);
    PrepareLiveRuntime();
    check(live_runtime && live_runtime->Status().view.full_reason==LiveFullReason::session_start);
    ++captures_saved;
    PrepareLiveRuntime();
    check(live_runtime && live_runtime->Status().view.full_reason==LiveFullReason::after_capture);
    fixed_source.loaded_v3=condition(true,std::string(64,'2'));
    PrepareLiveRuntime();
    check(live_runtime && live_runtime->Status().view.full_reason==LiveFullReason::condition_reloaded);
    PrepareLiveRuntime();
    check(live_runtime && live_runtime->Status().view.full_reason==LiveFullReason::session_start);
    captures_saved=0;live_captures_seen=0;live_condition_sha.clear();live_runtime.reset();

    // 4. A condition without registration.live: nothing is estimated, the rows say so, the pair capture stays admitted.
    start(false,true);
    check(live_runtime!=nullptr && !live_runtime->Enabled());
    check(wait_for(picture_ready));
    std::this_thread::sleep_for(300ms);
    paint();
    {
        const auto status=live_runtime->Status();
        check(!status.live_enabled && contains(status.state_text,"無効") && contains(status.orientation.b_short,"測れていません"));
        check(script->calls==0 && live_runtime->CurrentPlan()==nullptr);
        check(lit(client.bottom/2+140,client.bottom/2+161)>100 && lit(client.bottom/2+163,client.bottom/2+184)>100);
        check(pink(picture_rect())>0);
    }
    check(capture_admitted_soon());
    finish();
    live_runtime.reset();

    // 5. A condition v2: no runtime, no rows; the picture starts where it always did.
    fixed_source={};fixed_source.loaded_v2=lens_condition();
    live_estimator_override=nullptr;
    PrepareLiveRuntime();
    check(live_runtime==nullptr);
    merge_worker=std::jthread(MergeLoop);
    check(wait_for(picture_ready));
    paint();
    {
        const auto plan=drawn_plan();
        check(plan.live==nullptr && plan.lens_aware);
        const PixelRect rect = [&]{std::lock_guard lock(merged.mutex);return FitPixels(merged.pixels,12,client.bottom/2+140,client.right-24,client.bottom/2-155);}();
        const auto pink_count=pink(rect);
        check(rect.width>0 && pink_count+2>=static_cast<unsigned>(rect.height) && pink_count<=static_cast<unsigned>(rect.height));
    }
    check(capture_admitted_soon());
    finish();

    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(window);
    fixed_source={};live_runtime.reset();live_estimator_override=nullptr;live_design_override.reset();stopping=true;
    Log("live_c_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
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
    // Read-only settings of both bodies (#416 stage A): enabled only while both Live sessions are stopped and closed.
    settings_button=CreateWindowExW(0,L"BUTTON",L"両台の設定を読む",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(405),instance,nullptr);
    if(!pair_button || !review_button || !inspect_button || !session_button || !seam_button || !settings_button)throw std::runtime_error("create pair review buttons failed");EnableWindow(pair_button,FALSE);EnableWindow(review_button,FALSE);EnableWindow(inspect_button,FALSE);EnableWindow(session_button,FALSE);EnableWindow(settings_button,FALSE);
    SendMessageW(seam_button,BM_SETCHECK,BST_CHECKED,0);EnableWindow(seam_button,FALSE);
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    CreateSettingsWriteControls(window,instance);InitProductSettingsWrite(); // before any session: a block left by an earlier run holds capture
#endif
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
    // The lease is released only after a running settings read has ended and closed its body.
    if(settings_stop)settings_stop->store(true);
    if(settings_future.valid()){auto outcome=settings_future.get();all_sessions_passed=all_sessions_passed && outcome.CleanClose();LogSettingsOutcome(outcome);}
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    FinishSettingsWrite(); // the stop flag, then the write or reread ends and closes its camera, before the lease goes
#endif
    if(settings_close_failed)all_sessions_passed=false;
    if(capture_future.valid()){auto result=capture_future.get();all_sessions_passed=all_sessions_passed && result.capture.success && result.capture.closed;}
    if(state<0)all_sessions_passed=false;
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    // The hold and the shutter are one call: the hold is asked first inside RunCaptureOnce, and nothing else here reaches SaveStillOnce.
    if(capture_once_alias && all_sessions_passed){const auto& c=*cameras[*capture_once_alias];return RunCaptureOnce([&]{auto result=SaveStillOnce(c.id,c.alias,true);return result.success && result.closed?0:1;});}
#else
    if(capture_once_alias && all_sessions_passed){const auto& c=*cameras[*capture_once_alias];auto result=SaveStillOnce(c.id,c.alias,true);return result.success && result.closed?0:1;}
#endif
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
