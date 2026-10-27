// Camera-free saved JPEG inspector. No discovery, device open or shutter API.
#include "a0/m2/experimental/still_review.hpp"
#include "a0/m2/experimental/review_viewport.hpp"
#include "a0/m2/experimental/review_correspondence.hpp"
#include "a0/m2/experimental/review_session.hpp"
#include "a0/phase0/experimental/preview_back_buffer.hpp"
#include "a0/phase0/experimental/still_alignment_text.hpp"
#include <windowsx.h>
#include <commdlg.h>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <charconv>

namespace {
namespace review=a0::m2::experimental;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
std::wstring Number(double value){
    char buffer[64]{};const auto parsed=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(parsed.ec!=std::errc{})throw std::invalid_argument("number formatting failed");return std::wstring(buffer,parsed.ptr);
}
struct Loaded {review::SavedStillReview record;a0::m2::render::BgrImage image;};
Loaded Load(const fs::path& path){auto record=review::LoadSavedStillReview(path);auto image=review::LoadStillReviewImage(record);return {std::move(record),std::move(image)};}
struct Inspector {
    Loaded current;
    std::array<a0::m2::render::BgrImage,2> sources;
    review::ReviewViewport viewport;
    std::array<HWND,30> buttons{};
    std::array<HWND,4> edits{};
    HWND residual_text{};
    std::future<Loaded> save;
    std::future<review::SavedReviewSession> work_save;
    fs::path last_session_path;
    std::optional<review::ReviewSessionState> last_work_state;
    unsigned sessions_saved{},sessions_loaded{};
    bool resume_smoke{},resume_verified{};
    std::optional<review::ReviewSessionState> resume_expected;
    std::wstring status=L"画像をドラッグして移動できます。調整値は「再Merge・別保存」で反映します。";
    bool smoke{},closing{},failed{},dragging{};
    POINT drag{};
    unsigned smoke_stage{},saved{},pan_events{},verified_paints{};
    std::array<unsigned,3> zoom_paints{};
    std::array<unsigned,5> mode_paints{};
    unsigned conditions_loaded{},expected_rejections{};int mode{};bool expect_rejection{};
    Clock::time_point started=Clock::now();
    int displayed_x{},displayed_y{},draft_x{},draft_y{};
    double draft_degrees{},draft_scale{1};
    a0::m2::render::PairRenderParameters preview_parameters{};
    static constexpr int image_top=294;
    std::vector<review::ReviewCorrespondence> points;
    std::optional<a0::m2::render::Point> pending_a;
    int pick_stage{};
    review::ReviewPointRole next_point_role{review::ReviewPointRole::fitting};
    std::optional<review::ReviewSimilarityProposal> proposal;
    std::array<double,4> proposal_basis{};
    unsigned point_picks{},point_proposals{},point_applications{},point_undos{},point_rejections{},marker_pixels{};
    unsigned point_role_changes{},unverified_displays{},layout_verified{};
    explicit Inspector(Loaded loaded,bool test):current(std::move(loaded)),sources(review::LoadStillReviewSources(current.record)),smoke(test){
        viewport={current.image.width,current.image.height,1,1,0,current.image.width/2.0,current.image.height/2.0};
        displayed_x=current.record.translation_x;displayed_y=current.record.translation_y;
    }
    bool Busy() const {return save.valid() || work_save.valid();}
    void EnableChanges(bool enabled){
        for(size_t i=4;i<buttons.size();++i)EnableWindow(buttons[i],enabled);for(auto edit:edits)EnableWindow(edit,enabled);
    }
    review::ReviewSessionState WorkState() const {
        return {current.record.report,current.record.report_sha256,draft_x,draft_y,draft_degrees,draft_scale,points,next_point_role};
    }
    bool MatchesWork(const review::ReviewSessionState& state) const {
        if(current.record.report!=state.review_report || current.record.report_sha256!=state.review_report_sha256 || draft_x!=state.x || draft_y!=state.y ||
            draft_degrees!=state.degrees || draft_scale!=state.scale || next_point_role!=state.next_point_role || points.size()!=state.points.size() || proposal)return false;
        for(size_t i=0;i<points.size();++i){const auto& a=points[i];const auto& b=state.points[i];
            if(a.camera_a.x!=b.camera_a.x || a.camera_a.y!=b.camera_a.y || a.camera_b.x!=b.camera_b.x || a.camera_b.y!=b.camera_b.y || a.role!=b.role)return false;
        }return true;
    }
    void StartWorkSave(HWND window){
        if(Busy() || closing)return;if(pick_stage)throw std::invalid_argument("finish or cancel the pending point before saving work");ReadDraft();
        const auto state=WorkState();const auto id=std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(sessions_saved);
        const auto destination=current.record.report.parent_path().parent_path()/(L"session-"+id);
        const auto deadline=Clock::now()+std::chrono::seconds(180);
        work_save=std::async(std::launch::async,[state,destination,deadline]{return review::WriteReviewSession(state,destination,deadline);});
        EnableChanges(false);status=L"対応点・用途・調整値を別の作業ファイルへ保存しています…";InvalidateRect(window,nullptr,FALSE);
    }
    void RestoreWork(HWND window,const review::SavedReviewSession& session){
        const auto& b=session.review;
        if(!review::ReviewSessionBelongsToPair(session,current.record))throw std::invalid_argument("work belongs to another original pair");
        auto baseline=Load(b.report);if(baseline.record.report_sha256!=session.state.review_report_sha256)throw std::invalid_argument("work baseline changed");
        const auto& state=session.state;
        (void)review::SimilarityReviewGeometry(state.x,state.y,state.degrees,state.scale,b.source_width,b.source_height);
        review::ValidateReviewCorrespondences(state.points,b.source_width,b.source_height);
        current=std::move(baseline);displayed_x=current.record.translation_x;displayed_y=current.record.translation_y;
        points=state.points;next_point_role=state.next_point_role;proposal.reset();pending_a.reset();pick_stage=0;
        SetWindowTextW(edits[0],std::to_wstring(state.x).c_str());SetWindowTextW(edits[1],std::to_wstring(state.y).c_str());
        SetWindowTextW(edits[2],Number(state.degrees).c_str());SetWindowTextW(edits[3],Number(state.scale).c_str());
        const auto label=std::wstring(L"次の点: ")+RoleName(next_point_role)+L"用";SetWindowTextW(buttons[26],label.c_str());
        mode=1;ApplyView(window);last_session_path=session.file;++sessions_loaded;
        status=L"作業を復元しました。対応点の残差は再計算しています。未適用候補は「候補を計算」で作り直してください。";
    }
    void LoadWork(HWND window){
        fs::path path;
        if(smoke)path=last_session_path;
        else{
            std::wstring selected(32768,L'\0');const auto initial=current.record.report.parent_path().parent_path().wstring();OPENFILENAMEW dialog{};
            dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFilter=L"保存作業 (review-session.json)\0review-session.json\0\0";
            dialog.lpstrFile=selected.data();dialog.nMaxFile=static_cast<DWORD>(selected.size());dialog.lpstrInitialDir=initial.c_str();dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(!GetOpenFileNameW(&dialog)){if(CommDlgExtendedError())throw std::runtime_error("work selection failed");return;}path=selected.c_str();
        }
        RestoreWork(window,review::LoadReviewSession(path));
    }
    void SetDraft(){
        draft_x=current.record.translation_x;draft_y=current.record.translation_y;
        SetWindowTextW(edits[0],std::to_wstring(draft_x).c_str());SetWindowTextW(edits[1],std::to_wstring(draft_y).c_str());
        draft_degrees=current.record.rotation_degrees;draft_scale=current.record.scale;
        SetWindowTextW(edits[2],Number(draft_degrees).c_str());SetWindowTextW(edits[3],Number(draft_scale).c_str());
        preview_parameters=review::SimilarityReviewGeometry(draft_x,draft_y,draft_degrees,draft_scale,current.record.source_width,current.record.source_height);
        proposal.reset();
    }
    // An automatic estimate that was not applied (no policy, over the limit)
    // becomes the starting manual value; the saved JPEG stays displayed.
    void InitialDraft(){
        SetDraft();
        const auto& automatic=current.record.automatic;if(!automatic)return;
        status=a0::phase0::experimental::StillAlignmentLine(*automatic);
        if(!automatic->similarity || automatic->decision==review::StillAlignmentDecision::corrected)return;
        const auto& s=*automatic->similarity;const int x=static_cast<int>(std::llround(s.x)),y=static_cast<int>(std::llround(s.y));
        try{preview_parameters=review::SimilarityReviewGeometry(x,y,s.rotation_degrees,s.scale,current.record.source_width,current.record.source_height);}
        catch(const std::invalid_argument&){return;}
        draft_x=x;draft_y=y;draft_degrees=s.rotation_degrees;draft_scale=s.scale;
        SetWindowTextW(edits[0],std::to_wstring(x).c_str());SetWindowTextW(edits[1],std::to_wstring(y).c_str());
        SetWindowTextW(edits[2],Number(s.rotation_degrees).c_str());SetWindowTextW(edits[3],Number(s.scale).c_str());
        status+=L" 手動調整の初期値に推定値を入れました（未保存）。";
    }
    int Integer(HWND edit){
        wchar_t buffer[32]{};const int length=GetWindowTextLengthW(edit);
        if(length<=0 || length>=31)throw std::invalid_argument("invalid translation text");
        GetWindowTextW(edit,buffer,32);wchar_t* end{};const long value=wcstol(buffer,&end,10);
        if(end==buffer || *end || value<-32767 || value>32767)throw std::invalid_argument("invalid translation integer");return static_cast<int>(value);
    }
    double Decimal(HWND edit){
        wchar_t buffer[64]{};const int length=GetWindowTextLengthW(edit);if(length<=0 || length>=63)throw std::invalid_argument("invalid adjustment number");
        GetWindowTextW(edit,buffer,64);wchar_t* end{};const auto n=wcstod(buffer,&end);
        if(end==buffer || *end || !std::isfinite(n))throw std::invalid_argument("invalid adjustment number");return n;
    }
    void ReadDraft(){
        const int x=Integer(edits[0]),y=Integer(edits[1]);const auto degrees=Decimal(edits[2]),scale=Decimal(edits[3]);
        auto parameters=review::SimilarityReviewGeometry(x,y,degrees,scale,current.record.source_width,current.record.source_height);
        if(draft_x!=x || draft_y!=y || draft_degrees!=degrees || draft_scale!=scale)proposal.reset();
        draft_x=x;draft_y=y;draft_degrees=degrees;draft_scale=scale;preview_parameters=parameters;
        // Direct edit reads also occur while rejecting a stale proposal or
        // starting a save. Keep its displayed canvas in step with the matrix.
        if(mode){const auto plan=a0::m2::render::PlanPairCanvas(sources[0].width,sources[0].height,sources[1].width,sources[1].height,parameters);
            viewport.image_width=plan.output_width;viewport.image_height=plan.output_height;viewport.Constrain();}
        UpdateMeasurements();
    }
    std::array<double,4> DraftBasis() const {return {double(draft_x),double(draft_y),draft_degrees,draft_scale};}
    static const wchar_t* RoleName(review::ReviewPointRole role){return role==review::ReviewPointRole::fitting?L"調整":L"検証";}
    static std::wstring GroupMetrics(review::ReviewPointRole role,const review::ReviewResiduals& measured){
        std::wstring text=std::wstring(RoleName(role))+L"点: "+std::to_wstring(measured.points.size())+L"\r\n";
        if(measured.points.empty())return text+(role==review::ReviewPointRole::validation?L"未検証（検証点なし）\r\n":L"未指定\r\n");
        return text+L"RMS: "+Number(measured.rms)+L"\r\n最大: "+Number(measured.maximum)+L"\r\n";
    }
    void UpdateMeasurements(){
        if(!residual_text)return;
        const auto measured=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height);
        auto groups=[&](const auto& parameters){
            return GroupMetrics(review::ReviewPointRole::fitting,review::MeasureReviewCorrespondences(points,parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting))+L"\r\n"+
                GroupMetrics(review::ReviewPointRole::validation,review::MeasureReviewCorrespondences(points,parameters,sources[0].width,sources[0].height,review::ReviewPointRole::validation));
        };
        std::wstring text=(current.record.automatic?a0::phase0::experimental::StillAlignmentDetails(*current.record.automatic)+L"\r\n":std::wstring())+L"調整値での残差（A原画像px）\r\n全点: "+std::to_wstring(points.size())+L" / 32\r\n\r\n"+groups(preview_parameters);
        if(proposal)text+=L"\r\n候補での残差（未適用）\r\n"+groups(proposal->parameters);
        text+=L"\r\n各点（現在の調整値）\r\n";
        for(size_t i=0;i<measured.points.size();++i){const auto& p=measured.points[i];text+=std::to_wstring(i+1)+L" ["+RoleName(points[i].role)+L"] dx="+Number(p.dx)+L"\r\n   dy="+Number(p.dy)+L"\r\n   距離="+Number(p.distance)+L"\r\n";}
        text+=L"\r\nBの変換位置 − Aの指定位置。\r\n緑がA、橙がBです。\r\nF=調整、V=検証。\r\n検証点は候補計算に使いません。\r\n図面全体の精度保証ではありません。\r\n点はこの画面内だけで保持します。";
        SetWindowTextW(residual_text,text.c_str());
    }
    void PickPoint(HWND window,int x,int y){
        if(Busy() || closing || !pick_stage)return;
        if(x<12 || y<image_top){++point_rejections;return;}
        const auto pixel=viewport.Pixel(static_cast<unsigned>(x-12),static_cast<unsigned>(y-image_top));
        const auto point=pixel?review::PickReviewSourcePoint({double((*pixel)[0]),double((*pixel)[1])},preview_parameters,sources[0].width,sources[0].height,pick_stage==2):std::nullopt;
        if(!point){++point_rejections;status=L"画像のある範囲を選んでください。黒い余白や格子部分は指定できません。";InvalidateRect(window,nullptr,FALSE);return;}
        if(pick_stage==1){
            pending_a=*point;pick_stage=2;mode=4;ApplyView(window);status=L"次にB画像で同じ箇所をクリックしてください。拡大や移動もできます。";
        }else{
            auto next=points;next.push_back({*pending_a,*point,next_point_role});
            review::ValidateReviewCorrespondences(next,sources[0].width,sources[0].height);
            points=std::move(next);pending_a.reset();pick_stage=0;proposal.reset();++point_picks;mode=1;ApplyView(window);
            for(auto edit:edits)EnableWindow(edit,TRUE);
            status=std::wstring(RoleName(next_point_role))+L"点を追加しました。調整点と、候補計算に使わない検証点を離れた箇所に指定してください。";
        }
        InvalidateRect(window,nullptr,FALSE);
    }
    void PointCommand(HWND window,int id){
        if(id==121){
            if(points.size()>=32)throw std::invalid_argument("point limit reached");
            ReadDraft();proposal.reset();pending_a.reset();pick_stage=1;mode=3;ApplyView(window);
            for(auto edit:edits)EnableWindow(edit,FALSE);
            status=std::wstring(RoleName(next_point_role))+L"点: A画像で目印をクリック → B画像で同じ目印をクリック。選択中の移動は右ドラッグです。";
        }else if(id==122 || id==123){
            proposal.reset();if(pick_stage){pending_a.reset();pick_stage=0;}
            else if(id==122 && !points.empty()){points.pop_back();++point_undos;}
            if(id==123)points.clear();status=L"対応点を取り消しました。画像と調整値は変更していません。";
            for(auto edit:edits)EnableWindow(edit,TRUE);UpdateMeasurements();
        }else if(id==124){
            if(pick_stage)throw std::invalid_argument("finish or cancel the pending point first");
            ReadDraft();auto next=review::ProposeReviewSimilarity(points,sources[0].width,sources[0].height);
            const auto before=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting);
            const auto after=review::MeasureReviewCorrespondences(points,next.parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting);
            const auto validation=review::MeasureReviewCorrespondences(points,next.parameters,sources[0].width,sources[0].height,review::ReviewPointRole::validation);
            proposal=next;proposal_basis=DraftBasis();++point_proposals;
            std::cout<<"point_proposal=1 fit_count="<<after.points.size()<<" before_fit_rms="<<before.rms<<" after_fit_rms="<<after.rms<<" after_fit_max="<<after.maximum<<" validation_count="<<validation.points.size();
            if(validation.points.empty())std::cout<<" validation_status=unverified";else std::cout<<" validation_rms="<<validation.rms<<" validation_max="<<validation.maximum;
            std::cout<<" automatically_applied=0\n"<<std::flush;
            status=L"候補: X="+std::to_wstring(next.x)+L" Y="+std::to_wstring(next.y)+L" 回転="+Number(next.degrees)+L"° 倍率="+Number(next.scale)+
                L"。右欄に調整点と検証点の残差を分けて比較しています。確認後「候補を適用」。精度の合格判定ではありません。";
        }else if(id==125){
            ReadDraft();if(!proposal || proposal_basis!=DraftBasis())throw std::invalid_argument("no current point proposal");
            const auto next=*proposal;
            SetWindowTextW(edits[0],std::to_wstring(next.x).c_str());SetWindowTextW(edits[1],std::to_wstring(next.y).c_str());
            SetWindowTextW(edits[2],Number(next.degrees).c_str());SetWindowTextW(edits[3],Number(next.scale).c_str());
            mode=1;ApplyView(window);proposal.reset();++point_applications;
            status=L"対応点の候補をプレビューへ適用しました。画像で確認し、必要なら手動調整してから「再Merge・条件保存」。点の記録はこの画面内のみです。";
        }else if(id==126 || id==127){
            if(pick_stage)throw std::invalid_argument("cancel or finish the pending point before changing its role");
            if(id==126){
                next_point_role=next_point_role==review::ReviewPointRole::fitting?review::ReviewPointRole::validation:review::ReviewPointRole::fitting;
                const auto label=std::wstring(L"次の点: ")+RoleName(next_point_role)+L"用";SetWindowTextW(buttons[26],label.c_str());
                status=std::wstring(L"次に追加する点は")+RoleName(next_point_role)+L"用です。検証用の点は候補の計算から除外します。";
            }else{
                if(points.empty())throw std::invalid_argument("no completed point to change");
                auto& role=points.back().role;role=role==review::ReviewPointRole::fitting?review::ReviewPointRole::validation:review::ReviewPointRole::fitting;
                proposal.reset();++point_role_changes;status=std::wstring(L"直前の点を")+RoleName(role)+L"用に変更しました。候補は再計算してください。";
            }
        }
        UpdateMeasurements();
        InvalidateRect(window,nullptr,FALSE);
    }
    void ApplyView(HWND window){
        if(mode){ReadDraft();const auto plan=a0::m2::render::PlanPairCanvas(sources[0].width,sources[0].height,sources[1].width,sources[1].height,preview_parameters);
            viewport.image_width=plan.output_width;viewport.image_height=plan.output_height;
        }else{viewport.image_width=current.image.width;viewport.image_height=current.image.height;}
        viewport.Constrain();InvalidateRect(window,nullptr,FALSE);
        UpdateMeasurements();
    }
    void LoadConditions(HWND window){
        fs::path path;
        if(smoke)path=current.record.report;
        else{
            std::wstring selected(32768,L'\0');OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;
            dialog.lpstrFilter=L"確認用レポート (uncalibrated-review.json)\0uncalibrated-review.json\0\0";
            dialog.lpstrFile=selected.data();dialog.nMaxFile=static_cast<DWORD>(selected.size());dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(!GetOpenFileNameW(&dialog)){if(CommDlgExtendedError())throw std::runtime_error("condition file selection failed");return;}
            path=selected.c_str();
        }
        const auto condition=review::LoadSavedStillReview(path);const auto& r=current.record;
        if(condition.pair_id!=r.pair_id || condition.camera_a_original!=r.camera_a_original || condition.camera_b_original!=r.camera_b_original ||
            condition.camera_a_sha256!=r.camera_a_sha256 || condition.camera_b_sha256!=r.camera_b_sha256 || condition.source_width!=r.source_width || condition.source_height!=r.source_height ||
            condition.geometry.orientation!=r.geometry.orientation)
            throw std::invalid_argument("conditions belong to another original pair or mounting");
        SetWindowTextW(edits[0],std::to_wstring(condition.translation_x).c_str());SetWindowTextW(edits[1],std::to_wstring(condition.translation_y).c_str());
        SetWindowTextW(edits[2],Number(condition.rotation_degrees).c_str());SetWindowTextW(edits[3],Number(condition.scale).c_str());
        mode=1;ApplyView(window);++conditions_loaded;status=L"同じ原画像組の保存条件を読み込みました。調整プレビューに反映しています。";
    }
    void Layout(HWND window){
        RECT r{};GetClientRect(window,&r);
        for(int i=0;i<4;++i)if(buttons[i])MoveWindow(buttons[i],12+i*130,12,120,30,TRUE);
        for(int i=4;i<8;++i)if(buttons[i])MoveWindow(buttons[i],12+(i-4)*130,52,120,30,TRUE);
        for(int i=0;i<2;++i)if(edits[i])MoveWindow(edits[i],620+i*170,92,100,28,TRUE);
        for(int i=2;i<4;++i)if(edits[i])MoveWindow(edits[i],620+(i-2)*170,132,100,28,TRUE);
        for(int i=4;i<8;++i)if(buttons[i])MoveWindow(buttons[i],12+(i-4)*130,92,120,30,TRUE);
        for(int i=10;i<15;++i)if(buttons[i])MoveWindow(buttons[i],12+(i-10)*140,52,130,30,TRUE);
        for(int i=15;i<19;++i)if(buttons[i])MoveWindow(buttons[i],12+(i-15)*130,132,120,30,TRUE);
        if(buttons[19])MoveWindow(buttons[19],995,12,180,30,TRUE);
        if(buttons[20])MoveWindow(buttons[20],920,132,255,30,TRUE);
        if(buttons[28])MoveWindow(buttons[28],730,52,210,30,TRUE);
        if(buttons[29])MoveWindow(buttons[29],965,52,210,30,TRUE);
        for(int i=21;i<28;++i)if(buttons[i])MoveWindow(buttons[i],12+(i-21)*168,172,160,30,TRUE);
        if(buttons[8])MoveWindow(buttons[8],545,12,170,30,TRUE);
        if(buttons[9])MoveWindow(buttons[9],730,12,250,30,TRUE);
        viewport.view_width=static_cast<unsigned>(std::max(1L,r.right-350));
        viewport.view_height=static_cast<unsigned>(std::max(1L,r.bottom-(image_top+95)));viewport.Constrain();
        if(residual_text)MoveWindow(residual_text,r.right-326,image_top,314,viewport.view_height,TRUE);
    }
    void VerifyControlLayout(HWND window){
        RECT client{};GetClientRect(window,&client);bool valid=true;
        for(size_t i=0;i<buttons.size();++i){
            RECT bounds{},inner{};GetWindowRect(buttons[i],&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(buttons[i],&inner);
            wchar_t text[256]{};const int length=GetWindowTextW(buttons[i],text,static_cast<int>(std::size(text)));
            const auto dc=GetDC(buttons[i]);const auto font=reinterpret_cast<HFONT>(SendMessageW(buttons[i],WM_GETFONT,0,0));
            const auto previous=SelectObject(dc,font?font:GetStockObject(SYSTEM_FONT));SIZE extent{};
            const bool measured=GetTextExtentPoint32W(dc,text,length,&extent)!=FALSE;SelectObject(dc,previous);ReleaseDC(buttons[i],dc);
            if(!measured || extent.cx+12>inner.right || bounds.left<0 || bounds.top<0 || bounds.right>client.right || bounds.bottom>client.bottom){
                valid=false;std::cout<<"button_layout_failure="<<i<<" text_width="<<extent.cx<<" button_width="<<inner.right<<'\n';
            }
        }
        if(valid)++layout_verified;else failed=true;
    }
    void StartSave(HWND window){
        if(Busy() || closing)return;ReadDraft();
        if(pick_stage)throw std::invalid_argument("finish or cancel the pending point first");
        const auto& r=current.record;
        const auto id=std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(saved);
        const auto destination=r.report.parent_path().parent_path()/(L"revision-"+id);
        review::StillReviewRequest request{r.camera_a_original,r.camera_b_original,destination,r.camera_a_sha256,r.camera_b_sha256,r.pair_id,r.geometry,Clock::now()+std::chrono::seconds(180),review::StillReviewRequest::ManualRevision{draft_x,draft_y,r.report,r.report_sha256,draft_degrees,draft_scale}};
        save=std::async(std::launch::async,[request]{auto result=review::WriteUncalibratedStillReview(request);return Load(result.report);});
        EnableChanges(false);
        status=L"保存済みのA/B原画像から再Mergeしています。以前の結果は保持します…";
        std::cout<<"ui_restitch_requested=1 x="<<draft_x<<" y="<<draft_y<<" rotation="<<draft_degrees<<" scale="<<draft_scale<<" shutter_operations=0\n"<<std::flush;
        InvalidateRect(window,nullptr,FALSE);
    }
    void Advance(HWND window){
        if(save.valid() && save.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{
                auto next=save.get();current=std::move(next);++saved;
                displayed_x=current.record.translation_x;displayed_y=current.record.translation_y;
                mode=0;viewport.image_width=current.image.width;viewport.image_height=current.image.height;viewport.Constrain();SetDraft();
                UpdateMeasurements();
                status=L"別保存しました。表示中: "+current.record.jpeg.wstring();
                std::wcout<<L"saved_review_report="<<current.record.report.wstring()<<std::endl;
                std::cout<<"revision_saved=1 width="<<current.record.width<<" height="<<current.record.height<<" product_quality_accepted=0\n"<<std::flush;
            }catch(const std::exception&){status=L"再Mergeに失敗しました。表示中の結果と元画像は保持しています。自動再実行はしません。";if(smoke)failed=true;}
            EnableChanges(!closing);
            InvalidateRect(window,nullptr,FALSE);
        }
        if(work_save.valid() && work_save.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{
                auto session=work_save.get();last_session_path=session.file;last_work_state=std::move(session.state);++sessions_saved;
                status=L"作業を別保存しました。再開用ファイル: "+last_session_path.wstring();
                std::wcout<<L"saved_work_session="<<last_session_path.wstring()<<std::endl;
                std::cout<<"work_session_saved=1 point_count="<<last_work_state->points.size()<<" jpeg_writes=0 camera_operations=0\n"<<std::flush;
            }catch(const std::exception&){status=L"作業を保存できませんでした。現在の画面と以前のファイルは保持しています。";if(smoke)failed=true;}
            EnableChanges(!closing);InvalidateRect(window,nullptr,FALSE);
        }
        if(closing && !Busy()){DestroyWindow(window);return;}
        if(!smoke)return;
        auto click=[&](int index){SendMessageW(window,WM_COMMAND,MAKEWPARAM(100+index,BN_CLICKED),reinterpret_cast<LPARAM>(buttons[index]));};
        if(Clock::now()-started>std::chrono::seconds(210) || failed){failed=true;SendMessageW(window,WM_CLOSE,0,0);return;}
        if(resume_smoke){
            if(mode_paints[1] && (points.empty() || marker_pixels) && verified_paints){
                const auto fitting=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting);
                const auto validation=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height,review::ReviewPointRole::validation);
                // Check the explicitly selected file, not coordinates from a
                // historical fixture. Empty and validation-only work is valid.
                if(resume_expected){
                    const auto& expected=*resume_expected;
                    const auto parameters=review::SimilarityReviewGeometry(expected.x,expected.y,expected.degrees,expected.scale,sources[0].width,sources[0].height);
                    const auto expected_fitting=review::MeasureReviewCorrespondences(expected.points,parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting);
                    const auto expected_validation=review::MeasureReviewCorrespondences(expected.points,parameters,sources[0].width,sources[0].height,review::ReviewPointRole::validation);
                    const auto same=[](const auto& actual,const auto& wanted){return actual.points.size()==wanted.points.size() &&
                        std::abs(actual.rms-wanted.rms)<1e-8 && std::abs(actual.maximum-wanted.maximum)<1e-8;};
                    resume_verified=MatchesWork(expected) && same(fitting,expected_fitting) && same(validation,expected_validation) &&
                        !pending_a && pick_stage==0 && sessions_loaded==1 && !saved && !sessions_saved;
                }
                if(!resume_verified)failed=true;std::cout<<"resume_verified="<<resume_verified<<" fitting_count="<<fitting.points.size()<<" validation_count="<<validation.points.size()<<" validation_rms="<<validation.rms<<" automatically_fitted=0\n"<<std::flush;
                SendMessageW(window,WM_CLOSE,0,0);
            }return;
        }
        if(smoke_stage==0 && zoom_paints[0]){
            wchar_t initial_metrics[16384]{};GetWindowTextW(residual_text,initial_metrics,static_cast<int>(std::size(initial_metrics)));
            if(std::wstring(initial_metrics).find(L"検証点: 0\r\n未検証")==std::wstring::npos)failed=true;else ++unverified_displays;
            SetWindowTextW(edits[2],L"nan");expect_rejection=true;click(20);expect_rejection=false;
            if(mode!=0 || expected_rejections!=1)failed=true;
            SetDraft();++smoke_stage;click(1);
        }
        else if(smoke_stage==1 && zoom_paints[1]){
            ++smoke_stage;SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,300));
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(340,325));SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(340,325));
        }else if(smoke_stage==2 && pan_events && zoom_paints[1]>=2){++smoke_stage;click(2);}
        else if(smoke_stage==3 && zoom_paints[2]){++smoke_stage;click(3);click(11);click(5);click(6);click(16);click(18);}
        else if(smoke_stage==4 && mode_paints[1]){++smoke_stage;click(12);}
        else if(smoke_stage==5 && mode_paints[2]){++smoke_stage;click(13);}
        else if(smoke_stage==6 && mode_paints[3]){++smoke_stage;click(14);}
        else if(smoke_stage==7 && mode_paints[4]){
            auto rejected=[&](int button){const auto count=expected_rejections;expect_rejection=true;click(button);expect_rejection=false;if(expected_rejections!=count+1)failed=true;};
            click(23);rejected(24); // no points cannot produce a candidate
            click(21);SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,image_top+10));
            if(pick_stage!=1 || point_rejections!=1)failed=true;rejected(26);
            if(next_point_role!=review::ReviewPointRole::fitting || pick_stage!=1)failed=true;click(22);
            // The explicit smoke uses known displaced display coordinates to
            // exercise picking and fitting. They are not optical observations.
            viewport.zoom=1;viewport.center_x=(sources[0].width+draft_x)/2.0;viewport.center_y=viewport.image_height/2.0;viewport.Constrain();
            auto pair=[&](int dx,int dy,int shift_x=2,int shift_y=-1){
                click(21);const auto ox=viewport.view_width/2+dx,oy=viewport.view_height/2+dy;
                SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(12+ox,image_top+oy));
                if(pick_stage!=2)failed=true;
                SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(12+ox+shift_x,image_top+oy+shift_y));
                if(pick_stage)failed=true;
            };
            pair(-80,-100);rejected(24);pair(80,0);const auto first_basis=DraftBasis();click(24);
            if(!proposal || DraftBasis()!=first_basis)failed=true;
            wchar_t no_check_metrics[16384]{};GetWindowTextW(residual_text,no_check_metrics,static_cast<int>(std::size(no_check_metrics)));
            const std::wstring no_check=no_check_metrics;const auto first=no_check.find(L"検証点: 0\r\n未検証");
            if(first==std::wstring::npos || no_check.find(L"検証点: 0\r\n未検証",first+1)==std::wstring::npos)failed=true;else ++unverified_displays;
            click(26);pair(-80,100,5,3);click(22);pair(-80,100,5,3);rejected(25); // adding a validation point invalidates the old candidate
            const auto basis=DraftBasis();click(24);
            if(!proposal || DraftBasis()!=basis)failed=true; // calculating is read-only
            click(27);rejected(25);click(27);click(24); // changing an existing role also invalidates its candidate
            SetWindowTextW(edits[0],std::to_wstring(static_cast<int>(basis[0])+1).c_str());
            rejected(25);UpdateWindow(window); // direct input must leave a renderable viewport
            wchar_t measurement[16384]{};GetWindowTextW(residual_text,measurement,static_cast<int>(std::size(measurement)));
            if(std::wstring(measurement).find(L"RMS: 3.162")==std::wstring::npos)failed=true; // known sqrt(3^2+1^2), not the old sqrt(2^2+1^2)
            click(24);const auto expected=*proposal;click(25);
            if(draft_x!=expected.x || draft_y!=expected.y || draft_degrees!=expected.degrees || draft_scale!=expected.scale || proposal)failed=true;
            if(expected.x!=static_cast<int>(basis[0])-2 || expected.y!=static_cast<int>(basis[1])+1 || std::abs(expected.degrees-basis[2])>1e-9 || std::abs(expected.scale-basis[3])>1e-12)failed=true;
            const auto fitted=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height,review::ReviewPointRole::fitting);
            const auto checked=review::MeasureReviewCorrespondences(points,preview_parameters,sources[0].width,sources[0].height,review::ReviewPointRole::validation);
            if(fitted.points.size()!=2 || fitted.rms>1e-8 || checked.points.size()!=1 || std::abs(checked.rms-5)>1e-8 || std::abs(checked.maximum-5)>1e-8)failed=true;
            GetWindowTextW(residual_text,measurement,static_cast<int>(std::size(measurement)));
            if(std::wstring(measurement).find(L"検証点: 1")==std::wstring::npos || std::wstring(measurement).find(L"3 [検証]")==std::wstring::npos)failed=true;
            std::cout<<"validation_excluded_from_fit=1 fitting_rms="<<fitted.rms<<" validation_rms="<<checked.rms<<" validation_max="<<checked.maximum<<'\n'<<std::flush;
            ++smoke_stage;click(28);click(28); // work save does not create another JPEG
        }
        else if(smoke_stage==8 && sessions_saved){
            ++smoke_stage;
            const auto changed=WorkState();const auto valid_path=last_session_path;last_session_path=current.record.report;
            const auto rejects=expected_rejections;expect_rejection=true;click(29);expect_rejection=false;
            if(expected_rejections!=rejects+1 || !MatchesWork(changed))failed=true; // failed load preserves all current work
            last_session_path=valid_path;click(23);click(26);click(15);click(29);
            if(!last_work_state || !MatchesWork(*last_work_state) || sessions_loaded!=1)failed=true;
            click(9);click(9);
        }
        else if(smoke_stage==9 && saved){++smoke_stage;click(15);click(19);
            if(draft_x!=current.record.translation_x || draft_y!=current.record.translation_y || draft_degrees!=current.record.rotation_degrees || draft_scale!=current.record.scale)failed=true;
            click(10);click(0);
        }
        else if(smoke_stage==10 && zoom_paints[0]>=2 && mode_paints[0]>=2 && verified_paints>=9){++smoke_stage;SendMessageW(window,WM_CLOSE,0,0);}
    }
    void Paint(HWND window,HDC target){
        RECT r{};GetClientRect(window,&r);a0::phase0::experimental::PreviewBackBuffer frame(target,r.right,r.bottom);
        const auto dc=frame.DrawingContext();if(!dc)return;
        FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
        TextOutW(dc,550,98,L"X (px)",6);TextOutW(dc,735,98,L"Y (px)",6);TextOutW(dc,550,138,L"回転 (度)",6);TextOutW(dc,735,138,L"倍率 (倍)",6);
        const auto degrees=mode?draft_degrees:current.record.rotation_degrees,scale=mode?draft_scale:current.record.scale;
        std::wstring summary=(mode?L"調整プレビュー（未保存） / ":L"保存JPEG / ")+std::to_wstring(viewport.image_width)+L" × "+std::to_wstring(viewport.image_height)+L" px  / B位置="+std::to_wstring(mode?draft_x:displayed_x)+L", "+std::to_wstring(mode?draft_y:displayed_y)+L"  回転="+std::to_wstring(degrees)+L"° 倍率="+std::to_wstring(scale*100)+L"%";
        summary+=current.record.geometry.orientation==review::MountOrientation::rotated_180?L"  取付: 180°回転（A/Bを回して表示・合成）":L"  取付: 通常";
        TextOutW(dc,12,218,summary.c_str(),static_cast<int>(summary.size()));
        const wchar_t* help=L"未校正・操作検証用 / 回転・倍率で周辺を切り取ります。差分は重なり部分のみ。比較不可の範囲は格子表示。";
        TextOutW(dc,12,242,help,lstrlenW(help));
        const std::wstring point_help=pick_stage==1?std::wstring(RoleName(next_point_role))+L"点: Aの目印を左クリック。移動は右ドラッグ。":pick_stage==2?std::wstring(RoleName(next_point_role))+L"点: Bで同じ目印を左クリック。移動は右ドラッグ。":L"F=調整点、V=検証点（候補計算から除外）。2組以上の調整点に加え、離れた場所に検証点を指定してください。";
        TextOutW(dc,12,266,point_help.c_str(),static_cast<int>(point_help.size()));
        if(r.right>350 && r.bottom>image_top+95){
            auto pixels=mode?review::CompareReviewBgra(sources[0],sources[1],preview_parameters,viewport,static_cast<review::ReviewComparison>(mode-1)):review::ReviewViewportBgra(current.image,viewport);
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=viewport.view_width;
            info.bmiHeader.biHeight=-static_cast<LONG>(viewport.view_height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            const int drawn=StretchDIBits(dc,12,image_top,viewport.view_width,viewport.view_height,0,0,viewport.view_width,viewport.view_height,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            // Compare a real GDI pixel with the source projection, including in
            // smoke mode. This checks the display path, not merely WM_PAINT.
            const auto x=viewport.view_width/2,y=viewport.view_height/2;const auto index=(static_cast<size_t>(y)*viewport.view_width+x)*4;
            const bool pixel_ok=GetPixel(dc,12+x,image_top+y)==RGB(pixels[index+2],pixels[index+1],pixels[index]);
            if(drawn>0 && pixel_ok && frame.Present(target)){++zoom_paints[viewport.zoom==0?0:viewport.zoom==1?1:2];++mode_paints[mode];++verified_paints;}
            if(mode){
                const auto plan=a0::m2::render::PlanPairCanvas(sources[0].width,sources[0].height,sources[1].width,sources[1].height,preview_parameters);
                const auto origin=viewport.Origin();const auto display_scale=viewport.Scale();const int state=SaveDC(dc);
                IntersectClipRect(dc,12,image_top,12+viewport.view_width,image_top+viewport.view_height);SelectObject(dc,GetStockObject(DC_PEN));
                auto marker=[&](a0::m2::render::Point p,bool b,size_t index){
                    if(b)p=a0::m2::render::Transform(preview_parameters.camera_b_to_camera_a,p);
                    const auto px=p.x-plan.origin_x-preview_parameters.crop.left,py=p.y-plan.origin_y-preview_parameters.crop.top;
                    if(px<0 || py<0 || px>=viewport.image_width || py>=viewport.image_height)return;
                    const auto sx=12+origin.x+(px+.5)*display_scale,sy=image_top+origin.y+(py+.5)*display_scale;
                    if(sx<6 || sx>24+viewport.view_width || sy<image_top-6 || sy>image_top+viewport.view_height+12)return;
                    const int mx=static_cast<int>(std::lround(sx)),my=static_cast<int>(std::lround(sy));
                    const auto color=b?RGB(255,165,0):RGB(0,255,96);SetDCPenColor(dc,color);SetTextColor(dc,color);
                    MoveToEx(dc,mx-6,my,nullptr);LineTo(dc,mx+7,my);MoveToEx(dc,mx,my-6,nullptr);LineTo(dc,mx,my+7);
                    if(mx>=12 && mx<12+static_cast<int>(viewport.view_width) && my>=image_top && my<image_top+static_cast<int>(viewport.view_height) && GetPixel(dc,mx,my)==color)++marker_pixels;
                    const auto role=index<points.size()?points[index].role:next_point_role;
                    const auto label=std::wstring(role==review::ReviewPointRole::fitting?L"F":L"V")+std::to_wstring(index+1)+(b?L"B":L"A");TextOutW(dc,mx+7,my+3,label.c_str(),static_cast<int>(label.size()));
                };
                for(size_t i=0;i<points.size();++i){if(mode!=4)marker(points[i].camera_a,false,i);if(mode!=3)marker(points[i].camera_b,true,i);}
                if(pending_a)marker(*pending_a,false,points.size());RestoreDC(dc,state);SetTextColor(dc,RGB(240,240,240));
            }
        }
        RECT status_rect{12,r.bottom-80,r.right-12,r.bottom-8};DrawTextW(dc,status.c_str(),-1,&status_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
        (void)frame.Present(target);
    }
    void Command(HWND window,int id){
        if(id>=100 && id<=103){
            if(id<103)viewport.zoom=id==100?0:id==101?1:2;
            else{
                // Center on the midpoint of the saved overlap for seam review.
                viewport.center_x=(current.record.source_width+current.record.translation_x)/2.0;
                viewport.center_y=current.record.height/2.0;
            }
            viewport.Constrain();InvalidateRect(window,nullptr,FALSE);return;
        }
        if(Busy() || closing)return;
        if(id==128 || id==129){if(pick_stage)throw std::invalid_argument("finish or cancel the pending point");if(id==128)StartWorkSave(window);else LoadWork(window);return;}
        if(id>=121 && id<=127){PointCommand(window,id);return;}
        if(pick_stage)throw std::invalid_argument("finish or cancel the pending correspondence");
        if(id>=110 && id<=114){const int previous=mode;mode=id-110;try{ApplyView(window);}catch(...){mode=previous;throw;}return;}
        if(id==119){LoadConditions(window);return;}
        if(id==120){const auto previous=mode;mode=1;try{ApplyView(window);}catch(...){mode=previous;throw;}status=L"入力した条件を半透明プレビューに反映しました。保存は別操作です。";return;}
        if(id>=115 && id<=118){ReadDraft();double degrees=draft_degrees,scale=draft_scale;
            if(id==115)degrees-=0.05;if(id==116)degrees+=0.05;if(id==117)scale-=0.0001;if(id==118)scale+=0.0001;
            (void)review::SimilarityReviewGeometry(draft_x,draft_y,degrees,scale,current.record.source_width,current.record.source_height);
            SetWindowTextW(edits[2],Number(degrees).c_str());SetWindowTextW(edits[3],Number(scale).c_str());
            if(!mode)mode=1;ApplyView(window);status=L"回転・倍率を調整しました。再Merge・別保存で記録します。";return;
        }
        if(id>=104 && id<=107){
            ReadDraft();int x=draft_x,y=draft_y;
            if(id==104)--x;if(id==105)++x;if(id==106)--y;if(id==107)++y;
            (void)review::SimilarityReviewGeometry(x,y,draft_degrees,draft_scale,current.record.source_width,current.record.source_height);
            SetWindowTextW(edits[0],std::to_wstring(x).c_str());SetWindowTextW(edits[1],std::to_wstring(y).c_str());
            status=L"調整値を変更しました。「再Merge・別保存」で画像に反映します。";InvalidateRect(window,nullptr,FALSE);
            if(mode)ApplyView(window);
        }else if(id==108){SetDraft();ApplyView(window);status=L"調整値を表示中の画像に戻しました。";InvalidateRect(window,nullptr,FALSE);}
        else if(id==109)StartSave(window);
    }
};
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l){
    auto* app=reinterpret_cast<Inspector*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){app=static_cast<Inspector*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(!app)return DefWindowProcW(window,message,w,l);
    try{switch(message){
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={1240,720};return 0;
    case WM_SIZE:app->Layout(window);InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_DPICHANGED:{const auto* suggested=reinterpret_cast<RECT*>(l);SetWindowPos(window,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_COMMAND:
        if(LOWORD(w)>=100 && LOWORD(w)<130 && HIWORD(w)==BN_CLICKED && reinterpret_cast<HWND>(l)==app->buttons[LOWORD(w)-100])app->Command(window,LOWORD(w));return 0;
    case WM_TIMER:app->Advance(window);return 0;
    case WM_LBUTTONDOWN:
        if(app->pick_stage){app->PickPoint(window,GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;}
        [[fallthrough]];
    case WM_RBUTTONDOWN:
        if(GET_X_LPARAM(l)>=12 && GET_X_LPARAM(l)<12+static_cast<int>(app->viewport.view_width) && GET_Y_LPARAM(l)>=app->image_top && GET_Y_LPARAM(l)<app->image_top+static_cast<int>(app->viewport.view_height)){app->dragging=true;app->drag={GET_X_LPARAM(l),GET_Y_LPARAM(l)};SetCapture(window);}return 0;
    case WM_MOUSEMOVE:
        if(app->dragging){const POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};app->viewport.Pan(point.x-app->drag.x,point.y-app->drag.y);app->drag=point;++app->pan_events;InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_LBUTTONUP:case WM_RBUTTONUP:app->dragging=false;ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:app->dragging=false;return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);try{app->Paint(window,dc);}catch(...){EndPaint(window,&paint);throw;}EndPaint(window,&paint);return 0;}
    case WM_CLOSE:app->closing=true;app->status=L"実行中のファイル保存が終わるまで待っています…";if(!app->Busy())DestroyWindow(window);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }}catch(const std::exception&){
        app->status=L"処理できません。作業の読込には同じ画像組と有効な参照先が必要です。未完了の点は取消し、調整値・画像範囲を確認してください。候補は調整点が2組以上必要です。";
        if(app->expect_rejection)++app->expected_rejections;else if(app->smoke)app->failed=true;InvalidateRect(window,nullptr,FALSE);
    }
    return DefWindowProcW(window,message,w,l);
}
}
int wmain(int argc,wchar_t** argv){
    const bool evaluation_inputs=std::any_of(argv+1,argv+argc,[](const wchar_t* value){return std::wstring_view(value)==L"--evaluation-inputs";});
    try{
        if(evaluation_inputs){
            if(argc!=4 || std::wstring_view(argv[1])!=L"--report" || std::wstring_view(argv[3])!=L"--evaluation-inputs")
                throw std::invalid_argument("explicit report required");
            std::cout<<review::DescribeSavedReviewEvaluationInputs(fs::path(argv[2]))<<std::endl;return 0;
        }
        const bool from_session=argc>=3 && std::wstring(argv[1])==L"--session";
        const bool resume_test=argc==4 && std::wstring(argv[3])==L"--resume-smoke";
        if(argc<3 || argc>4 || (!from_session && std::wstring(argv[1])!=L"--report") || (argc==4 && (from_session?!resume_test:std::wstring(argv[3])!=L"--smoke")))
            throw std::invalid_argument("usage: StillReview --report <explicit report> [--smoke] | --session <explicit work file> [--resume-smoke]");
        // Opt out of Windows bitmap scaling so 100% is one physical display
        // pixel per saved image pixel, including on a scaled desktop.
        if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))throw std::runtime_error("per-monitor pixel display awareness unavailable");
        std::optional<review::SavedReviewSession> resumed;if(from_session)resumed=review::LoadReviewSession(fs::path(argv[2]));
        auto initial=Load(from_session?resumed->review.report:fs::path(argv[2]));
        if(resumed && initial.record.report_sha256!=resumed->state.review_report_sha256)throw std::invalid_argument("work baseline changed during open");
        auto app=std::make_unique<Inspector>(std::move(initial),argc==4);app->resume_smoke=resume_test;
        if(resume_test && resumed)app->resume_expected=resumed->state;
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0SavedStillReview";wc.hCursor=LoadCursor(nullptr,IDC_HAND);
        if(!RegisterClassW(&wc))throw std::runtime_error("register inspector failed");
        auto window=CreateWindowExW(0,wc.lpszClassName,L"A0 保存画像の比較・位置/回転/倍率・再Merge（未校正）",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1440,1000,nullptr,nullptr,instance,app.get());
        if(!window)throw std::runtime_error("create inspector failed");
        constexpr std::array<const wchar_t*,30> labels{L"全体表示",L"100%（等倍）",L"200%",L"継ぎ目へ",L"Bを左へ 1px",L"Bを右へ 1px",L"Bを上へ 1px",L"Bを下へ 1px",L"調整値を戻す",L"再Merge・条件保存",L"保存JPEG",L"半透明 50%",L"差分",L"Aのみ",L"Bのみ",L"回転 −0.05°",L"回転 ＋0.05°",L"倍率 −0.01%",L"倍率 ＋0.01%",L"保存条件を読込",L"入力値をプレビュー反映",L"対応点を追加",L"直前の点を取消",L"点を全消去",L"候補を計算",L"候補を適用",L"次の点: 調整用",L"直前の点の用途変更",L"作業を保存",L"作業を読込"};
        for(int i=0;i<30;++i){
            app->buttons[i]=CreateWindowExW(0,L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(100+i)),instance,nullptr);
            if(!app->buttons[i])throw std::runtime_error("create inspector button failed");
        }
        for(int i=0;i<4;++i){app->edits[i]=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(200+i)),instance,nullptr);if(!app->edits[i])throw std::runtime_error("create translation edit failed");}
        app->residual_text=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(300),instance,nullptr);
        if(!app->residual_text)throw std::runtime_error("create residual display failed");
        app->InitialDraft();app->UpdateMeasurements();app->Layout(window);SetTimer(window,1,100,nullptr);
        if(resumed)app->RestoreWork(window,*resumed);
        if(app->smoke)app->VerifyControlLayout(window);
        ShowWindow(window,SW_SHOWNORMAL);if(!IsWindowVisible(window))ShowWindow(window,SW_SHOW);UpdateWindow(window);
        std::cout<<"review_loaded=1 ui_window_visible="<<(IsWindowVisible(window)!=FALSE)<<" camera_operations=0\n"<<std::flush;
        MSG message{};BOOL result{};while((result=GetMessageW(&message,nullptr,0,0))>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(result<0)throw std::runtime_error("inspector message loop failed");
        const bool pass=!app->failed && (!app->smoke || (app->resume_smoke?app->resume_verified && app->layout_verified:
            app->smoke_stage==11 && app->saved==1 && app->conditions_loaded==1 && app->zoom_paints[0]>=2 && app->zoom_paints[1]>=2 && app->zoom_paints[2]>=1 && app->pan_events && app->mode_paints[1] && app->mode_paints[2] && app->mode_paints[3] && app->mode_paints[4] && app->point_picks==4 && app->points.size()==3 && app->point_proposals==4 && app->point_applications==1 && app->point_undos==1 && app->point_rejections==1 && app->marker_pixels && app->point_role_changes==2 && app->unverified_displays==2 && app->layout_verified && app->sessions_saved==1 && app->sessions_loaded==1));
        std::cout<<"sessions_saved="<<app->sessions_saved<<" sessions_loaded="<<app->sessions_loaded<<" resume_verified="<<app->resume_verified<<'\n';
        std::cout<<"point_pairs="<<app->points.size()<<" point_picks="<<app->point_picks<<" point_proposals="<<app->point_proposals<<" point_applications="<<app->point_applications<<" point_undos="<<app->point_undos<<" outside_point_rejections="<<app->point_rejections<<" verified_marker_pixels="<<app->marker_pixels<<" point_role_changes="<<app->point_role_changes<<" unverified_displays="<<app->unverified_displays<<'\n';
        std::cout<<"overlay_paints="<<app->mode_paints[1]<<" difference_paints="<<app->mode_paints[2]<<" a_paints="<<app->mode_paints[3]<<" b_paints="<<app->mode_paints[4]<<" conditions_loaded="<<app->conditions_loaded<<" invalid_input_rejections="<<app->expected_rejections<<'\n';
        std::cout<<"ui_controls_fit="<<app->layout_verified<<'\n';
        std::cout<<"fit_paints="<<app->zoom_paints[0]<<" native_paints="<<app->zoom_paints[1]<<" double_paints="<<app->zoom_paints[2]<<" verified_gdi_paints="<<app->verified_paints<<" pan_events="<<app->pan_events<<" revisions="<<app->saved<<" shutter_operations=0 result="<<(pass?"PASS":"FAIL")<<std::endl;
        return pass?0:1;
    }catch(const std::exception& error){
        if(evaluation_inputs){std::cerr<<"error=review-inputs-failed\n";return 1;}
        std::cerr<<"still review failed: "<<error.what()<<'\n';
        if(!(argc==4 && (std::wstring(argv[3])==L"--smoke" || std::wstring(argv[3])==L"--resume-smoke")))MessageBoxW(nullptr,L"確認用レポート・作業ファイルと画像を開けませんでした。参照先が揃い、内容が変更されていないか確認してください。",L"A0 撮影JPEGの確認",MB_OK|MB_ICONERROR);
        return 1;
    }
}
