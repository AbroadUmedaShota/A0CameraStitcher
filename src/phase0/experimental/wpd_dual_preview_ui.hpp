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
bool FixedConditionPresent(){return fixed_source.Present();}
// Read again at every preview session start, so a condition made between sessions is used.
void LoadFixedSource(){
    try{
        auto file=DefaultIdentityMapPath();file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
        fixed_source=LoadFixedPreviewSource(file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
    }catch(const std::exception& error){
        fixed_source={};fixed_source.reason=a0::m2::experimental::FixedCompositeSkip::condition_invalid;fixed_source.detail=error.what();
    }
    std::ostringstream line;line.imbue(std::locale::classic());
    line<<"C fixed_composite_condition="<<(fixed_source.loaded || fixed_source.loaded_v2?"present":fixed_source.reason==a0::m2::experimental::FixedCompositeSkip::condition_absent?"absent":"invalid");
    if(fixed_source.loaded){const auto& c=fixed_source.loaded->condition;
        line<<" version=1 sha256="<<fixed_source.loaded->file_sha256<<" seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" approval=unapproved";}
    if(fixed_source.loaded_v2){const auto& c=fixed_source.loaded_v2->condition;const auto& p=fixed_source.loaded_v2->profile;
        line<<" version=2 sha256="<<fixed_source.loaded_v2->file_sha256<<" profile_sha256="<<p.profile_file_sha256<<" profile_fingerprint_sha256="<<p.profile_fingerprint_sha256
            <<" raster="<<c.raster.width_pixels<<"x"<<c.raster.height_pixels<<"@"<<c.raster.dpi<<"dpi seam_column="<<c.seam.column<<" seam_origin="<<a0::m2::experimental::FixedSeamOriginName(c.seam.origin)
            <<" orientation="<<a0::m2::experimental::MountOrientationName(c.orientation)<<" approval=unapproved";}
    else if(!fixed_source.loaded && fixed_source.reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent)line<<" reason="<<a0::m2::experimental::FixedCompositeSkipCode(fixed_source.reason);
    Log(line.str());
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
int SettingsButtonRowReserve(); // the row of buttons takes room from the bottom of the settings table (defined below)
int SettingsTableBottom(std::size_t count,int client_bottom,int reserved_bottom); // bottom edge of the last row the table draws
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
    if(capture_future.valid() || settings_future.valid() || camera_workers[0].joinable() || camera_workers[1].joinable() || merge_worker.joinable())throw std::runtime_error("preview workers must be closed before restart");
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(SettingsWriteBusy())throw std::runtime_error("a settings write or reread holds the cameras");
#endif
    stopping=false;camera_end_observed=false;ready_count=0;preview_joined=false;ClearMerge();DiscardSettingsView();
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
    case D810SettingId::ColorSpace:return L"色空間";
    case D810SettingId::VignetteControl:return L"ヴィネットコントロール";
    case D810SettingId::AutoDistortionControl:return L"自動ゆがみ補正";
    case D810SettingId::HighIsoNr:return L"高感度ノイズ低減";
    case D810SettingId::LongExposureNr:return L"長秒時ノイズ低減";
    case D810SettingId::HdrMode:return L"HDR";
    default:return L"";
    }
}
// How the rows of the settings table are laid out below the header. One column of rows at the normal
// height when they fit; otherwise the compact height; otherwise two blocks side by side at the compact
// height. Rows that still do not fit are not drawn (`shown` says how many are).
struct SettingsTableLayout {int row_height{22};int blocks{1};int rows_per_block{0};std::size_t shown{0};};
inline constexpr int kSettingsRowHeight=22,kSettingsCompactRowHeight=20;
SettingsTableLayout LayoutSettingsTable(std::size_t count,int available_height){
    SettingsTableLayout layout;
    const int available=std::max(0,available_height);
    const auto fits=[&](int height){return static_cast<long long>(count)*height<=available;};
    if(fits(kSettingsRowHeight)){layout.rows_per_block=static_cast<int>(count);layout.shown=count;return layout;}
    layout.row_height=kSettingsCompactRowHeight;
    if(fits(kSettingsCompactRowHeight)){layout.rows_per_block=static_cast<int>(count);layout.shown=count;return layout;}
    layout.blocks=2;
    layout.rows_per_block=std::min(static_cast<int>((count+1)/2),available/kSettingsCompactRowHeight);
    layout.shown=std::min<std::size_t>(count,static_cast<std::size_t>(layout.rows_per_block)*2);
    return layout;
}
// The room the row of buttons takes from the bottom of the table: the buttons of a build with the settings write path
// start kD810ButtonRowGap above the bottom of the client area. 0 in a build without that path and while none of the
// buttons that stand in that row is shown.
int SettingsButtonRowReserve(){
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(SettingsWriteButtonShown(settings_copy_button) || SettingsWriteButtonShown(settings_reread_button) || SettingsWriteButtonShown(settings_panel_open_button))return kD810ButtonRowGap;
#endif
    return 0;
}
// Top of the first table row, and the layout of the table for a client area whose bottom reserved_bottom is kept free.
int SettingsTableFirstRowY(int client_bottom){return client_bottom/2+88+34+24;}
SettingsTableLayout LayoutSettingsTableIn(std::size_t count,int client_bottom,int reserved_bottom){
    return LayoutSettingsTable(count,client_bottom-SettingsTableFirstRowY(client_bottom)-4-reserved_bottom);
}
// Bottom edge of the last row the table draws.
int SettingsTableBottom(std::size_t count,int client_bottom,int reserved_bottom){
    const auto layout=LayoutSettingsTableIn(count,client_bottom,reserved_bottom);
    return SettingsTableFirstRowY(client_bottom)+layout.rows_per_block*layout.row_height;
}
// The side-by-side table of the two bodies' settings (#416, #427). Display only: nothing here sends a command.
bool DrawSettingsView(HDC dc,RECT client){
    if(!settings_view)return false;
    const int top=client.bottom/2+88,left=12;
    const wchar_t* title=L"両台のカメラ設定（読み取り専用・カメラの設定は変えていません）";
    TextOutW(dc,left,top,title,lstrlenW(title));
    const int head_y=top+34,first_y=SettingsTableFirstRowY(client.bottom);
    const auto layout=LayoutSettingsTableIn(settings_view->rows.size(),client.bottom,SettingsButtonRowReserve());
    const int block_width=(client.right-2*left)/layout.blocks;
    const int label_width=layout.blocks==1?236:std::min(200,block_width/3);
    const int column=std::max<int>(layout.blocks==1?100:60,(block_width-label_width-12)/3);
    const int height=layout.row_height;
    const auto cell=[&](int x,int y,const std::wstring& text,int width){
        RECT area{x,y,x+width-6,y+height};DrawTextW(dc,text.c_str(),-1,&area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
    };
    for(int block=0;block<layout.blocks;++block){
        const int x0=left+block*block_width;
        cell(x0,head_y,L"項目",label_width);cell(x0+label_width,head_y,L"CAM-A（左）",column);cell(x0+label_width+column,head_y,L"CAM-B（右）",column);cell(x0+label_width+2*column,head_y,L"差",column);
    }
    for(std::size_t i=0;i<layout.shown;++i){
        const auto& row=settings_view->rows[i];
        const int block=static_cast<int>(i)/layout.rows_per_block,x0=left+block*block_width;
        const int y=first_y+(static_cast<int>(i)%layout.rows_per_block)*height;
        const bool differs=row.difference==D810Difference::Different;
        SetTextColor(dc,differs?RGB(255,200,90):RGB(240,240,240));
        cell(x0,y,SettingLabel(row.id),label_width);
        cell(x0+label_width,y,WidenUtf8(row.a_text),column);cell(x0+label_width+column,y,WidenUtf8(row.b_text),column);
        cell(x0+label_width+2*column,y,differs?L"差あり":row.difference==D810Difference::Same?L"同じ":L"比較不可",column);
    }
    SetTextColor(dc,RGB(240,240,240));
    return true;
}
// "一致 N／違う M／読めない K". A row that could not be read on either side is counted as not readable,
// never as a match.
std::wstring SettingsCountsText(const D810ComparisonSummary& summary){
    return L"一致 "+std::to_wstring(summary.same)+L"／違う "+std::to_wstring(summary.different)+L"／読めない "+std::to_wstring(summary.unknown);
}
// Folds a finished read into the table and the status line. UI thread only.
void ApplySettingsOutcome(const SettingsReadOutcome& outcome){
    LogSettingsOutcome(outcome);
    settings_close_failed=!outcome.CleanClose();
    SettingsView view;
    view.rows=CompareD810Settings(outcome.cameras[0].read.readings,outcome.cameras[1].read.readings);
    if(settings_close_failed)view.status=L"設定を読んだあとにカメラとの接続を閉じられませんでした。ライブは再開できません。この画面を閉じて、本体・USB接続を確認してください。";
    else if(outcome.Complete()){
        const auto summary=SummarizeD810Comparison(view.rows);
        view.status=L"両台の設定を読みました（読み取りのみ）。"+SettingsCountsText(summary);
        // Said only when no row is different or unreadable. Only the rows of the table were compared.
        if(outcome.EverythingMatches(summary))view.status+=L"。左右が全部一致（表の項目だけ。色温度（K）の値・ピクチャーコントロールの調整値・プリセットWBの中身・WBの微調整は読めず、比べていません）";
        view.status+=L" / 「ライブ再開」を押すと表は消えます。";
    }else{
        view.status=L"設定の読み取りを途中で止めました。読めた項目だけ表示しています。"+SettingsCountsText(SummarizeD810Comparison(view.rows))+L"。";
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
    RECT status_rect{12,client.bottom/2+4,client.right-12,client.bottom/2+48};
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
    if(merged.decided && merged.mode==PreviewCMode::fixed_composite){
        title=FixedPreviewTitle(mount_orientation,merged.fixed_plan);
        status=merged.position_refusal.empty()?FixedPreviewNote(merged.fixed_plan):merged.position_refusal;
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
CaptureUiOutcome SavePairReview(const std::array<std::wstring,2>& ids,ReviewGeometry geometry,Clock::time_point deadline){
    CaptureUiOutcome result;
    const auto root=DefaultIdentityMapPath().parent_path().parent_path()/"wpd-pair-reviews";
    EvidenceWriter evidence(root,NewRunId(),"wpd-uncalibrated-pair-review");
    try{
        evidence.RecordState("pair","BothLiveViewsClosed");
        // Read first and on every capture, so a condition made between captures is used. The v1 (similarity) and
        // v2 (lens-aware, with its profile) records share the file name; the schema decides (#405 W4).
        auto condition_file=DefaultIdentityMapPath();condition_file.replace_filename(a0::m2::experimental::kFixedCompositeConditionName);
        const auto selection=a0::m2::experimental::SelectFixedComposite(condition_file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
        const a0::m2::experimental::LoadedFixedCompositeCondition* condition=selection.loaded?std::get_if<a0::m2::experimental::LoadedFixedCompositeCondition>(&*selection.loaded):nullptr;
        const a0::m2::experimental::LoadedFixedCompositeConditionV2* condition_v2=selection.loaded?std::get_if<a0::m2::experimental::LoadedFixedCompositeConditionV2>(&*selection.loaded):nullptr;
        const auto condition_skip=selection.reason;
        if(!selection.loaded && selection.reason!=a0::m2::experimental::FixedCompositeSkip::condition_absent)Log("pair_fixed_composite_condition_unusable reason="+std::string(a0::m2::experimental::FixedCompositeSkipCode(selection.reason))+" detail="+selection.detail);
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
        }else if(condition_v2){
            const auto policy_scale=merge_policy?merge_policy->scale_max_percent:std::nullopt;
            request.fixed_composite_lens=m2e::StillReviewRequest::FixedCompositeLens{&condition_v2->condition,&condition_v2->profile,condition_v2->file_sha256,
                m2e::ComparePolicyLimit(condition_v2->condition.scale_auto_correction_max_percent,policy_scale,merge_policy?m2e::FixedPolicyCheck::policy_without_scale_limit:m2e::FixedPolicyCheck::policy_absent)};
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
        if(const auto& lens=result.review->fixed_composite_lens){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_fixed_composite_lens seam_column="<<lens->seam_column<<" seam_origin="<<lens->seam_origin<<" scale_status="<<a0::m2::experimental::FixedScaleStatusCode(lens->scale.status)
                <<" scale_reason="<<a0::m2::experimental::FixedScaleReasonCode(lens->scale.reason)<<" applied_scale="<<lens->scale.applied_scale<<" limit_percent="<<lens->scale.limit_percent
                <<" used_tiles="<<lens->scale.used_tiles<<" policy_check="<<a0::m2::experimental::FixedPolicyCheckCode(lens->policy_limit.check)<<" estimate_ms="<<result.review->alignment_milliseconds<<" quality_accepted=0";Log(line.str());}
        if(const auto& seam=result.review->blend){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"pair_review_blend method="<<(result.review->fixed_composite || result.review->fixed_composite_lens?"fixed-seam":"seam-cut")<<" seam_x="<<seam->summary.x_minimum<<".."<<seam->summary.x_maximum<<" ratio="<<seam->ratio_minimum<<".."<<seam->ratio_maximum
                <<" saturation_limited="<<seam->saturation_limited_pixels<<" ms="<<result.review->blend_milliseconds<<" quality_accepted=0";Log(line.str());}
        std::wcout<<L"saved_review="<<result.review->jpeg.wstring()<<std::endl;
    }catch(const std::exception& error){result.capture.success=false;result.capture.error=error.what();evidence.RecordState("pair","FailedPartial",{},error.what());Log(std::string("pair_review_failure: ")+error.what());}
    return result;
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
            if(result.review && result.review->fixed_composite_lens)capture_status+=L" / "+FixedCompositeLensLine(*result.review->fixed_composite_lens);
            if(result.review && result.review->automatic)capture_status+=L" / "+StillAlignmentLine(*result.review->automatic);
            if(result.review){
                ++pair_reviews;review_path=result.review->jpeg;const auto& thumbnail=result.review->thumbnail;
                if(pair_cycle_smoke){pair_cycle_reports.push_back(result.review->report);std::wcout<<L"pair_cycle_report="<<result.review->report.wstring()<<std::endl;}
                review_pixels={thumbnail.width,thumbnail.height,{}};review_pixels.bytes.resize(static_cast<size_t>(thumbnail.width)*thumbnail.height*4);
                for(size_t i=0,j=0;i<thumbnail.bgr.size();i+=3,j+=4)std::copy_n(thumbnail.bgr.data()+i,3,review_pixels.bytes.data()+j);
                // The seam is one vertical column (#400): drawn on this display copy only, never in the saved JPEG.
                const auto seam_column=result.review->fixed_composite?std::optional<std::uint32_t>(result.review->fixed_composite->seam_column):
                    result.review->fixed_composite_lens?std::optional<std::uint32_t>(result.review->fixed_composite_lens->seam_column):std::nullopt;
                if(seam_column){
                    const auto divisor=std::max(1u,(std::max(result.review->width,result.review->height)+1199)/1200);const auto column=*seam_column/divisor;
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
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
    if(closing && SettingsWriteHoldsWindow(hwnd))return; // a write or reread is waited for the same way
#endif
    // A settings read in progress is waited for: its Close must finish before the window (and the lease) goes.
    if(closing && preview_joined && !capture_future.valid() && !settings_future.valid())DestroyWindow(hwnd);
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
        const auto summary=SummarizeD810Comparison(settings_view->rows);
        check(settings_view->rows.size()==16 && CountD810Differences(settings_view->rows)==3);
        check(summary.same==13 && summary.different==3 && summary.unknown==0 && !summary.AllSame());
        for(const auto& row:settings_view->rows)
            if(row.id==D810SettingId::ShutterSpeed)check(row.a_text=="1/6 s" && row.b_text=="1/2 s" && row.difference==D810Difference::Different);
            else if(row.id==D810SettingId::Aperture)check(row.difference==D810Difference::Same);
    }
    check(capture_status.find(L"一致 13／違う 3／読めない 0")!=std::wstring::npos && capture_status.find(L"全部一致")==std::wstring::npos);
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

    // 5b. The counts and the claim that everything matches, from synthetic outcomes (no camera). The claim needs
    // no different row, no unreadable row, both reads complete and both bodies closed.
    {
        const auto outcome_of=[&](bool same){
            SettingsReadOutcome o;
            for(int i=0;i<2;++i){
                o.cameras[i].read.alias=i==0?"CAM-A":"CAM-B";
                for(const auto& spec:kD810SettingSpecs)o.cameras[i].read.readings.push_back(reading(spec,same?0:i));
            }
            return o;
        };
        const auto status_has=[&](const wchar_t* text){return capture_status.find(text)!=std::wstring::npos;};
        auto all_same=outcome_of(true);
        ApplySettingsOutcome(all_same);
        check(status_has(L"一致 16／違う 0／読めない 0") && status_has(L"左右が全部一致") && status_has(L"色温度") && status_has(L"比べていません"));
        auto one_unreadable=outcome_of(true); // an extended row that B could not read
        for(auto& r:one_unreadable.cameras[1].read.readings)if(r.id==D810SettingId::HdrMode)r=UnavailableD810Setting(D810SettingId::HdrMode,"type_mismatch");
        ApplySettingsOutcome(one_unreadable);
        check(status_has(L"一致 15／違う 0／読めない 1") && !status_has(L"全部一致") && CountD810Differences(settings_view->rows)==0);
        auto unread_both=outcome_of(true); // the same row unreadable on both sides is still not a match
        for(auto& camera:unread_both.cameras)for(auto& r:camera.read.readings)if(r.id==D810SettingId::PictureControl)r=UnavailableD810Setting(D810SettingId::PictureControl,"type_mismatch");
        ApplySettingsOutcome(unread_both);
        check(status_has(L"一致 15／違う 0／読めない 1") && !status_has(L"全部一致"));
        auto stopped=outcome_of(true); // every row read, but the read did not end cleanly
        stopped.cameras[1].read.stop_reason="interrupted";
        ApplySettingsOutcome(stopped);
        check(status_has(L"途中で止めました") && status_has(L"一致 16／違う 0／読めない 0") && !status_has(L"全部一致"));
        auto not_closed=outcome_of(true);
        not_closed.cameras[1].clean_close=false;
        ApplySettingsOutcome(not_closed);
        check(settings_close_failed && !status_has(L"全部一致"));
        settings_close_failed=false;
        auto empty_read=outcome_of(true); // nothing read at all: no claim
        for(auto& camera:empty_read.cameras)MarkSettingsUnread(camera,"not_started");
        ApplySettingsOutcome(empty_read);
        check(status_has(L"一致 0／違う 0／読めない 16") && !status_has(L"全部一致"));
        ApplySettingsOutcome(all_same);
        check(settings_view.has_value() && !settings_close_failed);
        {   // The longest status text (it names what was not compared) stays within the two lines of the status area, at the default and at the smallest write-build width.
            HDC dc=CreateCompatibleDC(nullptr);
            if(!dc)check(false);
            else{
                for(const int width:{1400,1240}){
                    RECT need{0,0,width-24,0};DrawTextW(dc,capture_status.c_str(),-1,&need,DT_CALCRECT|DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
                    check(need.bottom<=44);Log("settings_status_width="+std::to_string(width)+" height="+std::to_string(need.bottom));
                }
                DeleteDC(dc);
            }
        }
        // Layout: the sixteen rows fit the test window (1400 x 960) in one column; more rows share two blocks.
        const auto fit=LayoutSettingsTable(settings_view->rows.size(),960-(960/2+88+58)-4);
        check(fit.blocks==1 && fit.shown==16 && fit.row_height==kSettingsCompactRowHeight);
        {   // With no reserve the table keeps its place: the last row ends above the bottom edge, whatever the height.
            bool inside=true;
            for(int bottom=600;bottom<=1400;++bottom){
                const auto layout=LayoutSettingsTableIn(16,bottom,0);
                if(layout.shown==16 && SettingsTableFirstRowY(bottom)+layout.rows_per_block*layout.row_height>bottom-4)inside=false;
            }
            check(inside && SettingsButtonRowReserve()==0);
            const auto same=LayoutSettingsTableIn(16,960,0);
            check(same.blocks==fit.blocks && same.shown==fit.shown && same.row_height==fit.row_height);
        }
        const auto roomy=LayoutSettingsTable(16,16*kSettingsRowHeight);
        check(roomy.blocks==1 && roomy.shown==16 && roomy.row_height==kSettingsRowHeight);
        const auto crowded=LayoutSettingsTable(30,330);
        check(crowded.blocks==2 && crowded.shown==30 && crowded.rows_per_block==15 && crowded.row_height==kSettingsCompactRowHeight);
        const auto clipped=LayoutSettingsTable(30,100);
        check(clipped.blocks==2 && clipped.shown==10 && clipped.rows_per_block==5);
        const auto none=LayoutSettingsTable(0,0);
        check(none.shown==0 && LayoutSettingsTable(5,0).shown==0);
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
