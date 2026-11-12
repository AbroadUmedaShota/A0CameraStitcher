// Private Win32 part of the camera settings write (#416 stage B, ADR-0038) in the development preview.
// Included by wpd_dual_preview_ui.hpp only when the preview is built with A0_BUILD_WPD_SETTINGS_WRITE
// (A0_ENABLE_WPD_SETTINGS_WRITE). The decisions (what each button may do, what is shown) are in
// D810SettingsWriteScreen; this file opens the cameras, draws and hooks the buttons and the capture admission.
//
// The controller comes from the product entry point only: this build has no constructor that takes a
// table, and the fixed product table decides every row (CAM-B rows only; the count is checked below).
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
// The confirmation test build (CMake: a0_preview_settings_write_confirm_tests): the one executable that has the
// table-taking constructor. Its main runs the camera-free confirmation smoke and nothing else.
static_assert(kD810ControllerTestTables, "the confirmation test build has the table-taking constructor");
#else
static_assert(!kD810ControllerTestTables, "the preview never builds the contract tests' table constructor");
static_assert(std::is_constructible_v<D810SettingsWriteController, D810SettingsJournal&, D810CameraOwnership&>, "the product entry point");
static_assert(!std::is_constructible_v<D810SettingsWriteController, const D810WriteTable&, D810SettingsJournal&, D810CameraOwnership&>,
    "the preview cannot hand the controller a table");
#endif

// --- Opening a camera for the settings channel ---------------------------------------------------------------
// The model and firmware version strings of the device object, read with the identity (no other Content,
// Properties or Resources use). They are compared with the fixed table and never logged or recorded.
std::string NarrowSettingsWriteText(const wchar_t* text){
    if(!text)return {};
    const int length=lstrlenW(text);if(length<=0 || length>64)return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,nullptr,0,nullptr,nullptr);
    if(n<=0 || n>256)return {};
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,length,out.data(),n,nullptr,nullptr);return out;
}
std::pair<std::string,std::string> SettingsWriteBodyVersion(IPortableDevice* device){
    std::pair<std::string,std::string> out;
    ComPtr<IPortableDeviceContent> content;ComPtr<IPortableDeviceProperties> properties;ComPtr<IPortableDeviceKeyCollection> keys;WpdValues values;
    if(device->Content(&content)!=S_OK || content->Properties(&properties)!=S_OK ||
        CoCreateInstance(CLSID_PortableDeviceKeyCollection,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&keys))!=S_OK)return out;
    if(keys->Add(WPD_DEVICE_MODEL)!=S_OK || keys->Add(WPD_DEVICE_FIRMWARE_VERSION)!=S_OK)return out;
    if(FAILED(properties->GetValues(WPD_DEVICE_OBJECT_ID,keys.Get(),&values)) || !values)return out;
    const auto text=[&](REFPROPERTYKEY key){PWSTR raw{};std::string s;if(values->GetStringValue(key,&raw)==S_OK)s=NarrowSettingsWriteText(raw);CoTaskMemFree(raw);return s;};
    out.first=text(WPD_DEVICE_MODEL);out.second=text(WPD_DEVICE_FIRMWARE_VERSION);
    return out;
}
ComPtr<IPortableDevice> OpenSettingsWriteDevice(const std::wstring& id){return Open(id,GENERIC_READ|GENERIC_WRITE);}
// The registered camera, opened for the settings channel (ADR-0038 decisions 6, 9 and 12, M-5). On Open the
// identity is matched against the registration again; when it does not match, or anything fails, nothing is
// handed out (an empty transport). The controller gets a transport function, the model and firmware strings
// and the watchdog's flag, never the device. One camera at a time: a second Open before the Close is refused.
// Used only by the screen's worker, under the preview's hardware lease, which outlives it (the window waits for
// the worker before RunPreviewWindow returns and the lease goes).
class WpdSettingsWriteCameraAccess final:public D810SettingsCameraAccess {
public:
    explicit WpdSettingsWriteCameraAccess(std::array<std::wstring,2> ids):ids_(std::move(ids)){}
    WpdSettingsWriteCameraAccess(const WpdSettingsWriteCameraAccess&)=delete;
    WpdSettingsWriteCameraAccess& operator=(const WpdSettingsWriteCameraAccess&)=delete;
    ~WpdSettingsWriteCameraAccess()override{if(open_)CloseSession(*open_);}
    D810OpenedCamera Open(D810Camera camera)override{
        if(open_)return {}; // never two at once
        const auto index=static_cast<std::size_t>(camera);
        if(index>1 || ids_[index].empty())return {};
        auto session=std::make_unique<Session>();
        session->camera=camera;session->alias=D810CameraAlias(camera);session->cancelled=std::make_shared<std::atomic<bool>>(false);
        try{session->device=OpenSettingsWriteDevice(ids_[index]);session->opened=true;}
        catch(const std::exception&){Log(session->alias+" settings_write_open_failed=1 setting_writes=0");return {};}
        std::pair<std::string,std::string> version;bool matched=false;
        try{
            auto map_path=DefaultIdentityMapPath();map_path.replace_filename("camera-map-wpd.json");IdentityMap map(map_path);
            matched=map.FindAlias(Identity(session->device.Get()))==std::optional<std::string>(session->alias);
            if(matched)version=SettingsWriteBodyVersion(session->device.Get());
        }catch(const std::exception&){matched=false;}
        if(!matched){Log(session->alias+" settings_write_identity_mismatch=1 setting_writes=0");CloseSession(*session);return {};}
        auto* s=session.get();
        // The watchdog only raises the flag and cancels the device; the session sees the flag on the I/O thread.
        s->watchdog=std::jthread([s](std::stop_token token){
            if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))){s->cancelled->store(true);return;}
            while(!token.stop_requested()){
                if(s->busy && GetTickCount64()-s->tick.load()>12000 && !s->cancelled->exchange(true)){
                    Log(s->alias+" settings write watchdog cancelling stalled I/O; lease retained");s->device->Cancel();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CoUninitialize();
        });
        open_=std::move(session);
        return {[s](IPortableDeviceValues* request){
                if(s->cancelled->load())throw std::runtime_error("settings session cancelled");
                WpdLiveViewExchange reply;s->tick=GetTickCount64();s->busy=true;
                reply.status=s->device->SendCommand(0,request,&reply.results);s->busy=false;return reply;
            },version.first,version.second,s->cancelled};
    }
    bool Close(D810Camera camera)override{
        if(!open_ || open_->camera!=camera)return false;
        const bool closed=CloseSession(*open_);open_.reset();return closed;
    }
private:
    struct Session {
        D810Camera camera{};std::string alias;ComPtr<IPortableDevice> device;bool opened{};
        std::shared_ptr<std::atomic<bool>> cancelled;std::atomic<bool> busy{};std::atomic<ULONGLONG> tick{};
        std::jthread watchdog; // Last: joined first, before the state it reads goes away.
    };
    // Close under the watchdog (a stalled Close is cancelled too), then stop the watchdog.
    static bool CloseSession(Session& s){
        bool closed=true;
        if(s.opened){s.tick=GetTickCount64();s.busy=true;const auto hr=s.device->Close();s.busy=false;closed=hr==S_OK;s.opened=false;
            Log(s.alias+" settings_write_session_closed="+std::to_string(closed));}
        if(s.watchdog.joinable()){s.watchdog.request_stop();s.watchdog.join();}
        return closed;
    }
    std::array<std::wstring,2> ids_;
    std::unique_ptr<Session> open_;
};

// --- The screen's state (UI thread only) ------------------------------------------------------------------------
// When the product folder cannot be resolved there is no record: nothing can be written and capture is held.
class UnavailableSettingsWriteJournal final:public D810SettingsJournal {
public:
    bool Append(const std::string&)override{return false;}
    D810TextFile Read()const override{return {};} // kind Error: the block is "unreadable"
};
struct SettingsWriteHost {
    std::unique_ptr<D810SettingsJournal> journal;
    D810CameraOwnership owner;
    std::unique_ptr<D810SettingsWriteController> controller;
    std::unique_ptr<D810SettingsWriteScreen> screen;
};
std::unique_ptr<SettingsWriteHost> settings_write;
// Set by the real window: without the host, capture is then refused. The camera-free smokes leave it unset.
bool settings_write_required=false;
unsigned settings_write_refresh_ticks{};
HWND settings_copy_button{},settings_reread_button{},settings_confirm_button{},settings_discard_button{};
bool SettingsWriteBusy(){return settings_write && settings_write->screen->Busy();}
// The confirm button may be pressed only when the confirmation has been drawn whole by the last present of the window
// (settings_write_drawn_serial = the serial of that view, set in WM_PAINT after the present succeeded, 0 for any other
// paint), every line fits the window as it is now and the button lies inside it (D810ConfirmPressable). The button
// state and the button's handler both ask this one function; with no client area it is false (fail closed).
std::uint64_t settings_write_paint_serial{},settings_write_drawn_serial{};
unsigned settings_write_confirm_refusals{};
bool SettingsWriteConfirmPressable(HWND window){
    if(!settings_write || !window)return false;
    RECT rect{};
    if(!GetClientRect(window,&rect))return false;
    const auto& screen=*settings_write->screen;
    return D810ConfirmPressable(screen.View(),screen.ViewSerial(),settings_write_drawn_serial,rect.right,rect.bottom);
}
// The window never gets smaller than the one that holds the longest confirmation (kD810WriteMinClientHeight) and the
// button row (kD810WriteMinClientWidth), but never more than the work area of the screen it is on. A screen too
// small for it cannot confirm: the button stays disabled (no row of the confirmation is cut and confirmed).
bool settings_write_min_size_enabled=true;
bool SettingsWriteMinTrack(MINMAXINFO* info){
    if(!info || !settings_write_min_size_enabled)return false;
    RECT frame{0,0,kD810WriteMinClientWidth,kD810WriteMinClientHeight};
    if(!AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,0))return false;
    int width=frame.right-frame.left,height=frame.bottom-frame.top;
    RECT work{};
    if(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)){width=std::min<int>(width,work.right-work.left);height=std::min<int>(height,work.bottom-work.top);}
    info->ptMinTrackSize.x=width;info->ptMinTrackSize.y=height;return true;
}
bool settings_write_wait_shown=false,settings_write_stuck_shown=false;
Clock::time_point settings_write_wait_since{};
// How long the closing window waits for a worker that does not end before it says what to do (longer than the longest write time,
// 120 s for a plan of picked values, plus the watchdog's 12 s). There is no button that closes the window sooner: the lease would then
// go before the worker's camera.
std::chrono::milliseconds settings_write_stuck_limit{150000};
unsigned settings_write_stuck_notices{};
// The window was asked to close while a write or reread still runs: it stays until the worker has closed its camera
// (the operator must not unplug or switch off the body meanwhile), and says so. Past the limit it says what to do.
bool SettingsWriteHoldsWindow(HWND window){
    if(!SettingsWriteBusy())return false;
    const auto now=Clock::now();
    if(!settings_write_wait_shown){
        settings_write_wait_shown=true;settings_write_wait_since=now;
        capture_status=L"設定の書き込み・読み直しの終わりを待っています。終わるまで USB を抜かず、本体の電源を切らないでください。";
        SetWindowTextW(window,L"A0 - 設定の書き込みの終わりを待っています（USB を抜かないでください）");
    }else if(!settings_write_stuck_shown && now-settings_write_wait_since>=settings_write_stuck_limit){
        settings_write_stuck_shown=true;++settings_write_stuck_notices;
        capture_status=L"応答がありません。タスクマネージャーでこのプログラムを終わらせてから、本体の電源を入れ直してください。"
            L"次の起動では撮影が止まったまま始まる事があります。読み直して一致を確かめるまでは撮らないでください。";
        SetWindowTextW(window,L"A0 - 応答がありません（設定の書き込み・読み直しが終わりません）");
        Log("settings_write_window_stuck=1 waited_seconds="+std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now-settings_write_wait_since).count())+" forced_close=0");
    }
    return true;
}
// The panel stops Live when it is opened while Live runs, and gives it back when it is closed (#416, ADR-0041).
// `settings_panel_open_pending`: the operator asked for the panel while Live runs; the panel reads the cameras when Live is
// stopped and closed (the timer), and is given up when a capture, a close, a failed close or a long wait gets in the way.
// `settings_panel_stopped_live`: the panel stopped Live itself. Only then does closing it start Live again; a Live stopped by hand
// stays stopped. Both are cleared by any restart of Live and by the panel's own end.
bool settings_panel_open_pending=false,settings_panel_stopped_live=false;
Clock::time_point settings_panel_pending_since{};
unsigned settings_panel_auto_stops{},settings_panel_auto_restarts{};
// How long Live may take to stop for the panel before the panel gives up (the stop of a session is a few seconds).
constexpr std::chrono::seconds kSettingsPanelPendingLimit{30};
void ClearSettingsPanelLiveFlags(){settings_panel_open_pending=false;settings_panel_stopped_live=false;}
void DiscardSettingsWriteView(){ClearSettingsPanelLiveFlags();if(settings_write)settings_write->screen->ClearView();} // a restart of Live (or a new read) ends the panel's hold on it
// The host with its controller made: the screen on top of it, the first status line, the capture hold.
void InstallSettingsWriteHost(std::unique_ptr<SettingsWriteHost> host,D810SettingsWriteScreen::AccessFactory access){
    D810SettingsWriteScreen::Options options;options.log=[](const std::string& line){Log(line);};
    host->screen=std::make_unique<D810SettingsWriteScreen>(*host->controller,std::move(access),std::move(options));
    const auto& status=host->screen->Status();
    Log("settings_write_screen=1 enabled_rows="+std::to_string(status.enabled_rows)+" capture_block="+D810CaptureBlockToken(status.block)+
        " capture_allowed="+std::to_string(status.capture_allowed)+" setting_writes=0");
    // A block left by an earlier run (not confirmed, mismatch, unknown) holds capture from the start.
    if(!status.capture_allowed)capture_status=D810CaptureBlockText(status.block,status.chosen_history,status.history_damaged);
    settings_write=std::move(host);
}
void InitSettingsWrite(std::unique_ptr<D810SettingsJournal> journal,D810SettingsWriteScreen::AccessFactory access){
    auto host=std::make_unique<SettingsWriteHost>();
    host->journal=journal?std::move(journal):std::make_unique<UnavailableSettingsWriteJournal>();
    host->controller=std::make_unique<D810SettingsWriteController>(*host->journal,host->owner);
    InstallSettingsWriteHost(std::move(host),std::move(access));
}
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
// The confirmation test build only: a table of the test's own.
void InitSettingsWriteWithTable(const D810WriteTable& table,std::unique_ptr<D810SettingsJournal> journal,D810SettingsWriteScreen::AccessFactory access){
    auto host=std::make_unique<SettingsWriteHost>();
    host->journal=journal?std::move(journal):std::make_unique<UnavailableSettingsWriteJournal>();
    host->controller=std::make_unique<D810SettingsWriteController>(table,*host->journal,host->owner);
    InstallSettingsWriteHost(std::move(host),std::move(access));
}
#endif
void InitProductSettingsWrite(){
    settings_write_required=true;
    InitSettingsWrite(OpenProductD810SettingsJournal(),[]{return std::make_unique<WpdSettingsWriteCameraAccess>(registered_ids);});
}
// --- The settings panel (#416, ADR-0041): controls, layout and drawing -------------------------------------------
// The panel takes the place of both Live areas while it is open (the preview stops Live first). One row per item, one
// drop-down per camera and item; the decisions (what can be picked, what a pick does) are in D810SettingsWriteScreen.
// A pick changes the draft only: nothing in this part sends to a camera.
constexpr int kPanelTitleY=104,kPanelHeaderY=132,kPanelRowHeight=30;
constexpr int kPanelLabelX=12,kPanelLabelWidth=170,kPanelAX=190,kPanelComboWidth=210,kPanelANoteX=406,kPanelNoteWidth=190;
constexpr int kPanelDiffX=600,kPanelDiffWidth=36,kPanelBX=640,kPanelBNoteX=856,kPanelSameX=1052,kPanelSameWidth=140;
constexpr int kPanelComboBaseId=430,kPanelSameBaseId=420;
constexpr int kPanelOpenButtonId=410,kPanelCloseButtonId=411,kPanelCancelButtonId=412,kPanelWriteButtonId=413;
constexpr int kPanelFirstChoiceRow=3; // the heading, then the two read-only rows
std::array<std::array<HWND,kD810ChoiceItemCount>,2> settings_panel_combos{};
std::array<HWND,kD810ChoiceItemCount> settings_panel_same_buttons{};
HWND settings_panel_open_button{},settings_panel_close_button{},settings_panel_cancel_button{},settings_panel_write_button{};
// The value behind each entry of a drop-down; nullopt for an entry that only shows a value that cannot be picked.
std::array<std::array<std::vector<std::optional<std::int64_t>>,kD810ChoiceItemCount>,2> settings_panel_values;
std::uint64_t settings_panel_built_serial=~std::uint64_t{},settings_panel_synced_serial=~std::uint64_t{};
unsigned settings_panel_picks{};
// The panel stops Live when it is opened while Live runs and starts it again when it is closed (#416, ADR-0041). The stop and the
// restart are the ones of the "ライブ停止" / "ライブ再開" button, asked for by the panel; nothing about a capture or a camera changes.
// Both Live sessions run and answer: the state in which the "ライブ停止" button stops them.
bool SettingsPanelLiveStoppable(){return !preview_joined && !closing && CanFocus(*cameras[0]) && CanFocus(*cameras[1]);}
// The panel may stop Live by itself only when nothing holds capture or has failed: a held capture, a window that is closing, a failed
// capture and a camera that could not be closed leave Live to the operator's own buttons.
bool SettingsPanelMayStopLive(){
    if(!settings_write || settings_panel_open_pending)return false;
    const auto& screen=*settings_write->screen;
    return SettingsPanelLiveStoppable() && screen.Status().capture_allowed && !capture_restart_blocked && !settings_close_failed && !capture_pending && !capture_future.valid() && screen.PanelOpenOffered(true);
}
// Closing the panel starts Live again only when: the panel stopped Live itself, nothing holds capture or has failed, the window is not
// closing, and the last thing the panel did with the cameras ended (a read that did not finish, or a write that was stopped before it
// sent, is not followed by starting the cameras). Live and the cameras are in the state a restart needs (the panel being open is not
// counted: it is closed first).
bool SettingsPanelWillRestartLive(){
    if(!settings_write || !settings_panel_stopped_live)return false;
    const auto& screen=*settings_write->screen;
    return !closing && screen.Status().capture_allowed && screen.PanelCamerasWereFine() && !capture_restart_blocked && !settings_close_failed && !SettingsReadBusy() && LiveStoppedAndClosed();
}
bool SettingsPanelOpen(){return settings_write && settings_write->screen->PanelOpen();}
int SettingsPanelRowY(int row){return kPanelHeaderY+row*kPanelRowHeight;}
D810Camera SettingsPanelCamera(int camera){return camera==0?D810Camera::A:D810Camera::B;}
// A cell whose drop-down could not be filled whole: it is locked (its entries cannot be trusted to match the values).
std::array<std::array<bool,kD810ChoiceItemCount>,2> settings_panel_cell_broken{};
// Fills the drop-downs from the panel's model (when it is replaced). The entries of a cell: the value it reads now
// when that cannot be picked (it only stands there), then the values it offers. Each entry carries the index of its value in
// `settings_panel_values` as its item data, so a pick is read from the entry that was chosen and never from its place in the list.
// An entry that cannot be added locks the cell.
void RebuildSettingsPanelCombos(){
    if(!settings_write)return;
    const auto* model=settings_write->screen->Panel();
    for(int camera=0;camera<2;++camera)for(std::size_t item=0;item<kD810ChoiceItemCount;++item){
        HWND combo=settings_panel_combos[camera][item];auto& values=settings_panel_values[camera][item];values.clear();
        settings_panel_cell_broken[camera][item]=false;
        if(!combo)continue;
        SendMessageW(combo,CB_RESETCONTENT,0,0);
        if(!model)continue;
        const auto& cell=model->At(SettingsPanelCamera(camera),static_cast<D810ChoiceItem>(item));
        const auto add=[&](const std::wstring& text,std::optional<std::int64_t> value){
            const LRESULT index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
            if(index==CB_ERR || index==CB_ERRSPACE || SendMessageW(combo,CB_SETITEMDATA,static_cast<WPARAM>(index),static_cast<LPARAM>(values.size()))==CB_ERR){
                settings_panel_cell_broken[camera][item]=true;Log("settings_write_ui_panel_drop_down_entry_failed=1 setting_writes=0");return;
            }
            values.push_back(value);
        };
        if(!cell.readable){add(L"読めません",std::nullopt);continue;}
        if(!cell.current_choosable)add(WidenUtf8(cell.current_text),std::nullopt);
        for(const auto& option:cell.options)add(WidenUtf8(option.label),option.value);
    }
}
// The value behind the entry `index` of a drop-down, by its item data; nullopt for an entry that only stands for a value that cannot be picked.
std::optional<std::int64_t> SettingsPanelValueAt(int camera,int item,int index){
    const LRESULT data=SendMessageW(settings_panel_combos[camera][item],CB_GETITEMDATA,static_cast<WPARAM>(index),0);
    const auto& values=settings_panel_values[camera][item];
    if(data==CB_ERR || data<0 || static_cast<std::size_t>(data)>=values.size())return std::nullopt;
    return values[static_cast<std::size_t>(data)];
}
// Shows the value of each cell (the pick, else the value read). A drop-down the operator has open is left alone
// unless `force`.
void SyncSettingsPanelSelection(bool force){
    if(!settings_write)return;
    const auto& screen=*settings_write->screen;
    for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
        HWND combo=settings_panel_combos[camera][item];
        if(!combo || (!force && SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)))continue;
        const auto shown=screen.PanelShownValue(SettingsPanelCamera(camera),static_cast<D810ChoiceItem>(item));
        const int count=static_cast<int>(SendMessageW(combo,CB_GETCOUNT,0,0));
        int index=-1,first_unpickable=-1;
        for(int k=0;k<count;++k){
            const auto value=SettingsPanelValueAt(camera,item,k);
            if(!value){if(first_unpickable<0)first_unpickable=k;continue;}
            if(shown && *value==*shown){index=k;break;}
        }
        if(index<0)index=first_unpickable; // the entry that only shows a value that cannot be picked
        if(static_cast<int>(SendMessageW(combo,CB_GETCURSEL,0,0))!=index)SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(index),0);
    }
}
std::wstring SettingsPanelWriteButtonText(){
    const auto dirty=settings_write?settings_write->screen->PanelDirty():0;
    return std::to_wstring(dirty)+L" 件を書き込む…";
}
void LayoutSettingsPanel(const RECT& rect){
    for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)
        if(settings_panel_combos[camera][item])MoveWindow(settings_panel_combos[camera][item],camera==0?kPanelAX:kPanelBX,SettingsPanelRowY(kPanelFirstChoiceRow+item),kPanelComboWidth,240,TRUE);
    for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)
        if(settings_panel_same_buttons[item])MoveWindow(settings_panel_same_buttons[item],kPanelSameX,SettingsPanelRowY(kPanelFirstChoiceRow+item),kPanelSameWidth,kPanelRowHeight-4,TRUE);
    const int y=std::max(0L,rect.bottom-kD810ButtonRowGap);
    if(settings_panel_open_button)MoveWindow(settings_panel_open_button,432,y,170,28,TRUE);
    if(settings_panel_close_button)MoveWindow(settings_panel_close_button,432,y,170,28,TRUE);
    if(settings_panel_cancel_button)MoveWindow(settings_panel_cancel_button,612,y,150,28,TRUE);
    if(settings_panel_write_button)MoveWindow(settings_panel_write_button,772,y,220,28,TRUE);
}
// The words and the grid of the panel (the drop-downs and buttons are windows of their own).
void DrawSettingsPanel(HDC dc,const RECT& client){
    if(!settings_write)return;
    const auto& screen=*settings_write->screen;
    if(!screen.PanelOpen())return;
    const auto text=[&](int x,int y,int width,const std::wstring& value){
        RECT area{x,y,x+width,y+22};DrawTextW(dc,value.c_str(),-1,&area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
    };
    const COLORREF body=RGB(240,240,240),amber=RGB(255,200,90),quiet=RGB(168,168,168);
    SetTextColor(dc,body);
    text(kPanelLabelX,kPanelTitleY,client.right-24,L"カメラの設定（ライブは止めています。値を選んだだけでは、カメラには何も送りません）");
    const int header=SettingsPanelRowY(0);
    text(kPanelLabelX,header,kPanelLabelWidth,L"項目");text(kPanelAX,header,kPanelComboWidth,L"CAM-A（左・基準）");text(kPanelDiffX,header,kPanelDiffWidth,L"差");
    text(kPanelBX,header,kPanelComboWidth,L"CAM-B（右）");text(kPanelSameX,header,kPanelSameWidth,L"B を A に揃える");
    const auto rows=screen.PanelRows();
    int row=1;
    for(const auto& view:rows){
        const int y=SettingsPanelRowY(row++);
        SetTextColor(dc,body);
        text(kPanelLabelX,y+4,kPanelLabelWidth,view.label);
        if(!view.choosable){
            SetTextColor(dc,quiet);
            text(kPanelAX+4,y+4,kPanelComboWidth,view.value[0]+L"（読むだけ）");text(kPanelBX+4,y+4,kPanelComboWidth,view.value[1]+L"（読むだけ）");
        }
        SetTextColor(dc,view.difference==L"≠"?amber:body);
        text(kPanelDiffX+8,y+4,kPanelDiffWidth,view.difference);
        SetTextColor(dc,amber);
        text(kPanelANoteX,y+4,kPanelNoteWidth,view.note[0]);text(kPanelBNoteX,y+4,kPanelNoteWidth,view.note[1]);
    }
    SetTextColor(dc,body);
    text(kPanelLabelX,SettingsPanelRowY(row)+4,client.right-24,screen.PanelSummaryText());
}
void CreateSettingsWriteControls(HWND window,HINSTANCE instance){
    const auto button=[&](const wchar_t* text,int id){
        HWND h=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if(!h)throw std::runtime_error("create settings write button failed");EnableWindow(h,FALSE);return h;
    };
    // Beside the read-only table: shown while both Live sessions are stopped and joined.
    settings_copy_button=button(L"A の設定を B に写す",406);
    settings_confirm_button=button(L"書き込む（1回だけ）",407);
    settings_discard_button=button(L"やめる",408);
    settings_reread_button=button(L"設定を読み直す",409);
    // The settings panel (#416, ADR-0041): its buttons share the bottom row with the confirmation's (they are never shown together).
    settings_panel_open_button=button(L"カメラの設定",kPanelOpenButtonId);
    settings_panel_close_button=button(L"設定の画面を閉じる",kPanelCloseButtonId);
    settings_panel_cancel_button=button(L"変更を取り消す",kPanelCancelButtonId);
    settings_panel_write_button=button(L"0 件を書き込む…",kPanelWriteButtonId);
    for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
        settings_panel_same_buttons[item]=button(L"A と同じにする",kPanelSameBaseId+item);
        for(int camera=0;camera<2;++camera){
            HWND combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,0,0,0,0,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanelComboBaseId+camera*static_cast<int>(kD810ChoiceItemCount)+item)),instance,nullptr);
            if(!combo)throw std::runtime_error("create settings panel drop-down failed");EnableWindow(combo,FALSE);
            settings_panel_combos[camera][item]=combo;
        }
    }
    // A build that contains the write path says so in its title.
    std::wstring title(512,L'\0');title.resize(static_cast<std::size_t>(GetWindowTextW(window,title.data(),static_cast<int>(title.size()))));
    SetWindowTextW(window,(title+L" ／ 設定の書き込みあり（開発用・#416 段B）").c_str());
}
void LayoutSettingsWriteButtons(const RECT& rect){
    const int y=std::max(0L,rect.bottom-kD810ButtonRowGap);
    if(settings_copy_button)MoveWindow(settings_copy_button,12,y,230,28,TRUE);
    if(settings_reread_button)MoveWindow(settings_reread_button,252,y,170,28,TRUE);
    if(settings_confirm_button)MoveWindow(settings_confirm_button,kD810ConfirmButtonLeft,y,kD810ConfirmButtonWidth,kD810ConfirmButtonHeight,TRUE);
    if(settings_discard_button)MoveWindow(settings_discard_button,642,y,110,28,TRUE);
    LayoutSettingsPanel(rect);
}
// The button's own WS_VISIBLE (IsWindowVisible also depends on the parent, which a smoke never shows).
bool SettingsWriteButtonShown(HWND button){return button && (GetWindowLongW(button,GWL_STYLE)&WS_VISIBLE)!=0;}
void ShowSettingsWriteButton(HWND button,bool shown,bool enabled){
    if(!button)return;
    if(SettingsWriteButtonShown(button)!=shown)ShowWindow(button,shown?SW_SHOW:SW_HIDE);
    EnableWindow(button,shown && enabled);
}
// A capture start point: the capture buttons and RequestCapture. The record is read again here.
bool SettingsWriteAdmitsCapture(){
    if(!settings_write){
        if(!settings_write_required)return true; // camera-free smokes only: no write path in this process
        capture_status=L"撮影を止めています: 設定の書き込みの状態を確かめられません。";Log("ui_capture_refused_settings_write=1 reason=no_screen shutter_commands=0");return false;
    }
    auto& screen=*settings_write->screen;
    if(screen.AdmitCapture())return true;
    capture_status=screen.CaptureRefusalText();
    Log(std::string("ui_capture_refused_settings_write=1 block=")+D810CaptureBlockToken(screen.Status().block)+" busy="+std::to_string(screen.Busy())+" shutter_commands=0");
    return false;
}
// The one-shot capture of --capture-a / --capture-b, after the window ended: held while the record holds capture.
bool CaptureOnceHeld(){return capture_once_alias && all_sessions_passed && !SettingsWriteAdmitsCapture();}
// The one-shot capture as a whole: the hold is asked first, and the shutter is reached only through this call (the
// preview's caller passes the capture as `shutter`; a caller that skips this call takes no picture). 1 when held.
template<class Shutter>int RunCaptureOnce(Shutter&& shutter){
    if(CaptureOnceHeld())return 1;
    return shutter();
}
// The buttons. Each handler checks the conditions again (the button state is not trusted).
bool HandleSettingsWriteCommand(int id,WORD code,HWND control){
    if(!settings_write)return false;
    // A pick in a drop-down of the panel: the draft only. A refused pick goes back to the value shown.
    if(settings_write && code==CBN_SELCHANGE && id>=kPanelComboBaseId && id<kPanelComboBaseId+2*static_cast<int>(kD810ChoiceItemCount)){
        const int slot=id-kPanelComboBaseId,camera=slot/static_cast<int>(kD810ChoiceItemCount),item=slot%static_cast<int>(kD810ChoiceItemCount);
        if(control!=settings_panel_combos[camera][item])return false;
        auto& screen=*settings_write->screen;
        const int index=static_cast<int>(SendMessageW(control,CB_GETCURSEL,0,0));
        bool accepted=false;
        if(!settings_panel_cell_broken[camera][item]){
            const auto value=SettingsPanelValueAt(camera,item,index);
            if(value)accepted=screen.PanelPick(SettingsPanelCamera(camera),static_cast<D810ChoiceItem>(item),*value);
        }
        if(accepted)++settings_panel_picks;
        Log(std::string("settings_write_ui_pick item=")+D810WriteItemOf(D810ChoiceWriteItem(static_cast<D810ChoiceItem>(item))).name+" camera="+D810CameraAlias(SettingsPanelCamera(camera))+
            " accepted="+std::to_string(accepted)+" setting_writes=0 camera_operations=0");
        SyncSettingsPanelSelection(true);
        capture_status=screen.PanelSummaryText();
        return true;
    }
    auto& screen=*settings_write->screen;
    const bool ready=CanReadSettings();
    if(id==406 && control==settings_copy_button){
        if(screen.RequestCopy(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
        else capture_status=screen.OfferText(ready);
        return true;
    }
    // The confirm button is the only caller of the controller's Confirm.
    if(id==407 && control==settings_confirm_button){
        if(!SettingsWriteConfirmPressable(GetParent(control))){
            ++settings_write_confirm_refusals;Log("settings_write_ui_confirm_refused=1 reason=not_shown setting_writes=0");
            // A confirmation is up but not drawn whole: say so. With none up, the usual reason.
            capture_status=screen.ConfirmationShown()?L"確認の内容を窓に描けていません。窓を大きくしてください。何も書いていません。":
                (screen.StatusLine().empty()?screen.OfferText(ready):screen.StatusLine());
            return true;
        }
        if(screen.ConfirmShownPlan(ready))capture_status=screen.RunIsChosen()?screen.OfferText(ready):L"CAM-B に書いています（項目ごとに1回だけ）…";
        else capture_status=screen.StatusLine().empty()?screen.OfferText(ready):screen.StatusLine();
        return true;
    }
    if(id==408 && control==settings_discard_button){screen.DiscardPlan();capture_status=screen.StatusLine();return true;}
    if(id==409 && control==settings_reread_button){
        if(screen.PanelOpen() ? screen.RequestPanelReread(ready) : screen.RequestReread(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
        return true;
    }
    if(id==kPanelOpenButtonId && control==settings_panel_open_button){
        if(ready){
            // Live is stopped and closed already (stopped by hand): read both cameras for the panel.
            if(screen.RequestPanelOpen(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
            else capture_status=screen.OfferText(ready);
        }else if(SettingsPanelMayStopLive()){
            // Live runs: stop it by the "ライブ停止" button's own path; the timer opens the panel when both sessions are closed.
            const auto stops_before=manual_preview_stops;
            SendMessageW(GetParent(control),WM_COMMAND,MAKEWPARAM(403,BN_CLICKED),reinterpret_cast<LPARAM>(session_button));
            if(manual_preview_stops==stops_before+1){
                settings_panel_open_pending=true;settings_panel_stopped_live=true;settings_panel_pending_since=Clock::now();++settings_panel_auto_stops;
                capture_status+=L"（止まったら、両台の設定を読んで、設定の画面を開きます）";
                Log("settings_write_ui_panel_live_stop=1 capture_requests=0 setting_writes=0");
            }
        }else if(!screen.Status().capture_allowed){
            capture_status=L"撮影を止めている間は、設定の画面を開くために、ライブを自動では止めません。先に「ライブ停止」を押してください。";
        }else capture_status=L"今は設定の画面を開けません（撮影・読み取りの途中、または両台の切断を確かめられていません）。";
        return true;
    }
    if(id==kPanelCloseButtonId && control==settings_panel_close_button){
        if(screen.PanelCloseOffered()){
            const bool restart=SettingsPanelWillRestartLive(); // before the panel goes: it knows how its last camera operation ended
            const bool stopped_by_panel=settings_panel_stopped_live;
            screen.ClosePanel();
            const std::wstring closed=screen.StatusLine();
            if(restart){
                // The panel stopped Live and took its place: Live starts again, by the "ライブ再開" button's own path.
                ++settings_panel_auto_restarts;
                Log("settings_write_ui_panel_live_restart=1 setting_writes=0");
                SendMessageW(GetParent(control),WM_COMMAND,MAKEWPARAM(403,BN_CLICKED),reinterpret_cast<LPARAM>(session_button));
                capture_status+=L" "+closed;
            }else{
                ClearSettingsPanelLiveFlags();
                // Why Live stays stopped, and how to get it back; the "ライブ再開" button is named only where it can be pressed.
                const wchar_t* why=settings_close_failed?L" カメラとの接続を閉じられなかったため、ライブは再開できません。この画面を閉じて、本体・USB接続を確認してください。":
                    capture_restart_blocked?L" 撮影に失敗したあとのため、ライブは再開できません。この画面を閉じて、もう一度起動してください。":
                    !screen.Status().capture_allowed?L" 撮影を止めているため、ライブは自動では再開しません。「ライブ再開」で再開できます。":
                    !stopped_by_panel?L" ライブは止まったままです（この画面を開く前から止まっていたため、自動では再開しません）。「ライブ再開」で再開できます。":
                    L" ライブは止まったままです。「ライブ再開」で再開できます。";
                capture_status=closed+why;
            }
        }
        return true;
    }
    // "N 件を書き込む…": reads both cameras again and puts up the confirmation of the picks. It sends nothing itself; the
    // confirm button (above) is the only way on.
    if(id==kPanelWriteButtonId && control==settings_panel_write_button){
        if(screen.RequestChosen(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
        else capture_status=screen.ChosenOfferText(ready);
        return true;
    }
    if(id==kPanelCancelButtonId && control==settings_panel_cancel_button){
        const auto dropped=screen.PanelCancelAll();SyncSettingsPanelSelection(true);
        capture_status=dropped?L"変更予定 "+std::to_wstring(dropped)+L" 件を取り消しました。何も書いていません。":screen.PanelSummaryText();
        return true;
    }
    if(id>=kPanelSameBaseId && id<kPanelSameBaseId+static_cast<int>(kD810ChoiceItemCount) && control==settings_panel_same_buttons[id-kPanelSameBaseId]){
        const auto item=static_cast<D810ChoiceItem>(id-kPanelSameBaseId);
        const bool accepted=screen.PanelPickSameAsA(item);
        Log(std::string("settings_write_ui_pick_same_as_a item=")+D810WriteItemOf(D810ChoiceWriteItem(item)).name+" accepted="+std::to_string(accepted)+" setting_writes=0 camera_operations=0");
        if(accepted){++settings_panel_picks;SyncSettingsPanelSelection(true);}
        capture_status=screen.PanelSummaryText();
        return true;
    }
    return false;
}
// Every timer tick, after the preview's own button states: take a finished result, follow the state, and hold the
// capture buttons while capture is held or a write runs.
void SettingsWriteTimer(){
    if(!settings_write)return;
    auto& screen=*settings_write->screen;
    if(closing)screen.Stop(); // Only set. The worker ends, closes its camera; the lease waits for it.
    if(screen.Poll()){
        review_visible=false; // a finished plan or result is shown, not a picture asked for while it was being made
        capture_status=screen.StatusLine();
        if(screen.CloseFailed())settings_close_failed=true; // as stage A: no further read, write or Live restart
    }else if(!screen.Busy() && ++settings_write_refresh_ticks>=40){settings_write_refresh_ticks=0;screen.Refresh();}
    const bool ready=CanReadSettings(),shown=preview_joined && !closing;
    if(settings_panel_open_pending){
        // Live was stopped for the panel: read when both sessions are closed, give up when something else got in the way.
        const bool too_long=Clock::now()-settings_panel_pending_since>kSettingsPanelPendingLimit;
        if(closing || capture_pending || capture_future.valid() || capture_restart_blocked || settings_close_failed || !screen.Status().capture_allowed || too_long){
            ClearSettingsPanelLiveFlags();
            capture_status=too_long?L"設定の画面は開きませんでした。ライブの停止が終わらなかったためです。ライブが止まったら「両台の設定を読む」か「カメラの設定」を、動いていれば「ライブ停止」を押してください。":
                L"設定の画面は開きませんでした。撮影の止めが掛かっているか、撮影の失敗・窓を閉じる途中・切断の失敗のためです。ライブは「ライブ停止」「ライブ再開」で操作できます。";
            Log("settings_write_ui_panel_open_given_up=1 setting_writes=0");
        }else if(ready){
            settings_panel_open_pending=false;
            if(screen.RequestPanelOpen(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
            else capture_status=screen.OfferText(ready);
        }
    }
    if(!screen.PanelOpen() && !settings_panel_open_pending && !screen.Busy())settings_panel_stopped_live=false; // the panel did not open: it holds no Live
    const bool confirming=screen.Step()==D810WriteScreenStep::Confirming;
    ShowSettingsWriteButton(settings_copy_button,shown,screen.CopyOffered(ready));
    ShowSettingsWriteButton(settings_reread_button,shown,screen.RereadOffered(ready));
    ShowSettingsWriteButton(settings_confirm_button,shown && confirming,screen.ConfirmOffered(ready) && SettingsWriteConfirmPressable(GetParent(settings_confirm_button)));
    ShowSettingsWriteButton(settings_discard_button,shown && confirming,confirming && !screen.Busy());
    {   // The settings panel: its buttons (never together with the confirmation's), its drop-downs and the picks they show.
        const bool panel=screen.PanelOpen(),panel_shown=shown && panel;
        // Offered also while Live runs (the panel then stops it first), not while it is being stopped for the panel.
        ShowSettingsWriteButton(settings_panel_open_button,!panel && !closing && !confirming && (shown || settings_panel_open_pending || SettingsPanelLiveStoppable()),!settings_panel_open_pending && (screen.PanelOpenOffered(ready) || SettingsPanelMayStopLive()));
        ShowSettingsWriteButton(settings_panel_close_button,panel_shown && !confirming,screen.PanelCloseOffered());
        if(panel_shown){
            // The name says what closing does: only a panel that stopped Live itself gives it back.
            const wchar_t* wanted=SettingsPanelWillRestartLive()?L"閉じてライブに戻る":L"設定の画面を閉じる";
            wchar_t named[64]{};GetWindowTextW(settings_panel_close_button,named,64);
            if(std::wstring(named)!=wanted)SetWindowTextW(settings_panel_close_button,wanted);
        }
        ShowSettingsWriteButton(settings_panel_cancel_button,panel_shown && !confirming,screen.PanelEditable() && screen.PanelEntryCount()>0);
        // "N 件を書き込む…" is offered by the screen: a change is planned and nothing else holds the cameras.
        ShowSettingsWriteButton(settings_panel_write_button,panel_shown && !confirming,screen.ChosenOffered(ready));
        if(panel_shown){
            if(screen.PanelSerial()!=settings_panel_built_serial){RebuildSettingsPanelCombos();settings_panel_built_serial=screen.PanelSerial();SyncSettingsPanelSelection(true);settings_panel_synced_serial=screen.DraftSerial();}
            else if(screen.DraftSerial()!=settings_panel_synced_serial){SyncSettingsPanelSelection(false);settings_panel_synced_serial=screen.DraftSerial();}
            const std::wstring write_text=SettingsPanelWriteButtonText();
            wchar_t shown_text[64]{};GetWindowTextW(settings_panel_write_button,shown_text,64);
            if(write_text!=shown_text)SetWindowTextW(settings_panel_write_button,write_text.c_str());
        }
        const auto* model=screen.Panel();
        for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
            const bool editable=panel_shown && model && !settings_panel_cell_broken[camera][item] && screen.PanelEditable() && model->At(SettingsPanelCamera(camera),static_cast<D810ChoiceItem>(item)).Editable();
            ShowSettingsWriteButton(settings_panel_combos[camera][item],panel_shown,editable);
        }
        for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)
            ShowSettingsWriteButton(settings_panel_same_buttons[item],panel_shown,panel_shown && screen.PanelSameAsAOffered(static_cast<D810ChoiceItem>(item)));
    }
    if(review_button && screen.View())EnableWindow(review_button,FALSE); // the view (confirmation or result) is drawn before a picture
    if(screen.Busy() || !screen.Status().capture_allowed){
        for(auto button:capture_buttons)if(button)EnableWindow(button,FALSE);
        if(pair_button)EnableWindow(pair_button,FALSE);
    }
}
// True when `text` laid out with `flags` in `area` needs no more room than `area` has: nothing of it is cut.
bool SettingsWriteTextFits(HDC dc,const std::wstring& text,const RECT& area,UINT flags){
    RECT need=area;
    if(!DrawTextW(dc,text.c_str(),-1,&need,flags|DT_CALCRECT))return false;
    return need.right-need.left<=area.right-area.left && need.bottom-need.top<=area.bottom-area.top;
}
// Drawn while both Live sessions are stopped and joined: the line above the buttons, and the confirmation or
// the last result in place of C (it takes the place of the read-only table while it is shown). A confirmation
// that is drawn whole (the line above the buttons, the title and every line, none cut at the right edge or the
// bottom) leaves its serial in settings_write_paint_serial; the window's present makes it the drawn one.
bool DrawSettingsWrite(HDC dc,RECT client){
    settings_write_paint_serial=0;
    if(!settings_write)return false;
    auto& screen=*settings_write->screen;
    auto offer=screen.OfferText(CanReadSettings());
    const auto* view=screen.View();
    if(view && view->confirmation && !D810WriteViewFits(*view,client.bottom))
        offer=L"確認の内容が窓に入りません。窓を大きくすると「書き込む（1回だけ）」を押せます。";
    RECT offer_rect{12,client.bottom-80,client.right-12,client.bottom-44};
    SetTextColor(dc,screen.Status().capture_allowed?RGB(240,240,240):RGB(255,200,90));
    DrawTextW(dc,offer.c_str(),-1,&offer_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX|DT_END_ELLIPSIS);
    SetTextColor(dc,RGB(240,240,240));
    if(!view)return false;
    bool whole=view->confirmation && SettingsWriteTextFits(dc,offer,offer_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    const int top=client.bottom/2+88;
    SetTextColor(dc,view->confirmation?RGB(255,200,90):RGB(240,240,240));
    TextOutW(dc,12,top,view->title.c_str(),static_cast<int>(view->title.size()));
    whole=whole && SettingsWriteTextFits(dc,view->title,RECT{12,top,client.right-12,top+30},DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    SetTextColor(dc,RGB(240,240,240));
    int y=top+30;
    const auto visible=std::min(view->lines.size(),D810WriteViewVisibleLines(client.bottom));
    for(std::size_t i=0;i<visible;++i){
        RECT area{12,y,client.right-12,y+22};DrawTextW(dc,view->lines[i].c_str(),-1,&area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);y+=22;
        whole=whole && SettingsWriteTextFits(dc,view->lines[i],area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    }
    if(whole && visible==view->lines.size())settings_write_paint_serial=screen.ViewSerial();
    return true;
}
// After the message loop: the stop flag, the worker's end (its camera closed), then the lease may go.
void FinishSettingsWrite(){
    if(!settings_write)return;
    auto& screen=*settings_write->screen;
    screen.Stop();screen.Wait();
    if(screen.CloseFailed())all_sessions_passed=false;
    if(screen.PanelOpen())Log("settings_write_panel_dropped_on_exit=1 changes="+std::to_string(screen.PanelDirty())+" setting_writes=0");
    Log("settings_write_finished=1 close_failed="+std::to_string(screen.CloseFailed())+" capture_block="+D810CaptureBlockToken(screen.Status().block));
}

// --- Camera-free contract of the screen in the product build (#416 stage B) --------------------------------------
// The real window procedure, timer, buttons, Paint and capture admission run with the fixed product table (no row
// enabled) and a record in a temporary folder (never %LOCALAPPDATA%\A0CameraStitcher). The camera access opens
// nothing: it counts the calls and hands out no transport. No Discover, no device, no hardware lease.
LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
void RequestCapture(int index);
void StartPreview();
std::array<std::uint64_t,6> Paint(HDC dc,RECT client);
class NoCameraSettingsAccess final:public D810SettingsCameraAccess {
public:
    explicit NoCameraSettingsAccess(std::shared_ptr<std::atomic<int>> opens):opens_(std::move(opens)){}
    D810OpenedCamera Open(D810Camera)override{++*opens_;return {};}
    bool Close(D810Camera)override{return false;}
private:
    std::shared_ptr<std::atomic<int>> opens_;
};
// An access whose Open waits (at most 20 s) until released: a reread that has not ended. Opens nothing.
class BlockingSettingsAccess final:public D810SettingsCameraAccess {
public:
    struct Gate {std::atomic<int> entered{};std::atomic<bool> release{};};
    explicit BlockingSettingsAccess(std::shared_ptr<Gate> gate):gate_(std::move(gate)){}
    D810OpenedCamera Open(D810Camera)override{
        ++gate_->entered;
        for(int i=0;i<4000 && !gate_->release.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return {};
    }
    bool Close(D810Camera)override{return false;}
private:
    std::shared_ptr<Gate> gate_;
};
// The preview's window procedure on a window nobody sees, with the buttons the screen reaches and two synthetic Live
// sessions (no camera, no lease). Null when anything could not be made.
HWND CreateSettingsWriteSmokeWindow(const wchar_t* class_name,HINSTANCE& instance){
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.opened=true;c.start_attempted=true;c.started=true;
        c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    stopping=false;run_seconds=0;run_start=Clock::now();preview_joined=false;
    instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=class_name;
    if(!RegisterClassW(&wc))return nullptr;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic settings write test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return nullptr;
    session_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(403),instance,nullptr);
    settings_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(405),instance,nullptr);
    for(int i=0;i<2;++i)capture_buttons[i]=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(200+i)),instance,nullptr);
    pair_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(400),instance,nullptr);
    if(!session_button || !settings_button || !capture_buttons[0] || !capture_buttons[1] || !pair_button){DestroyWindow(window);return nullptr;}
    try{CreateSettingsWriteControls(window,instance);}catch(const std::exception&){DestroyWindow(window);return nullptr;}
    return window;
}
// The window's own Paint into a memory bitmap, and the pixels it left (no window, no present).
struct SettingsWriteSmokeFrame {
    int width{},height{};HDC dc{};HBITMAP bitmap{};HGDIOBJ old{};const std::uint32_t* bits{};std::array<std::uint64_t,6> drawn{};
    SettingsWriteSmokeFrame(int w,int h):width(w),height(h){
        dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* raw{};
        if(dc)bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&raw,nullptr,0);
        if(dc && bitmap && raw){old=SelectObject(dc,bitmap);bits=static_cast<const std::uint32_t*>(raw);drawn=Paint(dc,RECT{0,0,w,h});GdiFlush();}
    }
    ~SettingsWriteSmokeFrame(){if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
    SettingsWriteSmokeFrame(const SettingsWriteSmokeFrame&)=delete;
    SettingsWriteSmokeFrame& operator=(const SettingsWriteSmokeFrame&)=delete;
    // Pixels in the rows [top,bottom) of the columns [12,1000) that are not black.
    unsigned Lit(int top,int bottom)const{
        unsigned lit{};
        if(bits)for(int y=std::max(0,top);y<std::min(height,bottom);++y)for(int x=12;x<std::min(width,1000);++x)if((bits[static_cast<std::size_t>(y)*width+x]&0xffffffu)!=0)++lit;
        return lit;
    }
    // Pixels of exactly `color` in the rows [top,bottom) of the whole width.
    unsigned Count(int top,int bottom,COLORREF color)const{
        unsigned count{};const std::uint32_t want=(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
        if(bits)for(int y=std::max(0,top);y<std::min(height,bottom);++y)for(int x=0;x<width;++x)if((bits[static_cast<std::size_t>(y)*width+x]&0xffffffu)==want)++count;
        return count;
    }
};
int RunSettingsWriteUiSmoke(){
    Log("settings_write_ui_synthetic=1 camera_operations=0");
    unsigned checks{},failures{};const auto check=[&](bool ok,int line=__builtin_LINE()){++checks;if(!ok){++failures;Log("settings_write_ui_failed_check="+std::to_string(checks)+" source_line="+std::to_string(line));}};
    wchar_t temp[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,temp))return 2;
    const auto folder=std::filesystem::path(temp)/(L"a0-settings-write-ui-smoke-"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;std::filesystem::remove_all(folder,ignored);
    if(const auto product=ProductD810SettingsWriteFolder())check(folder.parent_path()!=product->parent_path() && folder!=*product); // not the product's record
    HINSTANCE instance{};
    HWND window=CreateSettingsWriteSmokeWindow(L"A0SettingsWriteUiSmoke",instance);
    if(!window)return 2;
    auto opens=std::make_shared<std::atomic<int>>(0);
    const auto factory=[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);};
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
    const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
    const auto stop_live=[&]{for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}preview_joined=true;stopping=true;};
    const auto run_live=[&]{stopping=false;preview_joined=false;for(auto& camera:cameras){camera->done=false;camera->stopped=false;camera->closed=false;camera->last=Clock::now();}};
    const auto wait_idle=[&]{for(int i=0;i<400;++i){tick();if(!SettingsWriteBusy())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;};
    const auto drawn_in=[&](int top,int bottom){return SettingsWriteSmokeFrame(1400,960).Lit(top,bottom);};
    wchar_t caption[256]{};GetWindowTextW(window,caption,256);
    check(std::wstring(caption).find(L"設定の書き込みあり")!=std::wstring::npos); // the ON build's mark

    // 1. The product table and an empty record folder: five rows are enabled (ISO, shutter speed, aperture, white balance and metering on CAM-B), capture is free, and copying
    //    is offered once both Live sessions are joined. The smoke opens no camera (the access counts every Open).
    InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
    auto* screen=settings_write->screen.get();
    check(screen->Status().enabled_rows==5 && screen->Status().write_allowed && screen->Status().capture_allowed);
    tick();
    check(!SettingsWriteButtonShown(settings_copy_button) && !IsWindowEnabled(settings_copy_button)); // Live runs: not shown
    stop_live();tick();
    check(CanReadSettings() && SettingsWriteButtonShown(settings_copy_button) && IsWindowEnabled(settings_copy_button)!=FALSE); // rows enabled: copying is offered
    check(IsWindowEnabled(settings_reread_button)!=FALSE && !SettingsWriteButtonShown(settings_confirm_button) && !SettingsWriteButtonShown(settings_discard_button));
    const auto offer=screen->OfferText(true);
    // The "no row" text belongs to a table with no enabled row; the product table does not reach it.
    check(offer.find(L"書ける項目がありません")==std::wstring::npos && offer.find(L"番号（code）")==std::wstring::npos && offer.find(L"確定の前に表示します")!=std::wstring::npos);
    click(407,settings_confirm_button); // a confirm with no plan up does nothing (copy itself is not pressed: it would start a read)
    check(!SettingsWriteBusy() && opens->load()==0 && screen->Step()==D810WriteScreenStep::Idle && !screen->Status().plan_pending && !screen->Status().plan_confirmed);
    check(capture_status==offer);
    check(drawn_in(960-80,960-44)>50); // the reason line above the buttons
    // Capture is admitted while nothing is held.
    run_live();tick();
    check(CanFocus(*cameras[0]) && CanFocus(*cameras[1]));
    capture_pending.reset();RequestCapture(0);
    check(capture_pending.has_value());
    capture_pending.reset();stopping=false;
    // The one-shot capture (--capture-a / --capture-b) is held only by the record, and only when it was asked for.
    const bool passed_before=all_sessions_passed;
    int shutters{};const auto count_shutter=[&]{++shutters;return 7;};
    capture_once_alias=0;check(!CaptureOnceHeld());
    check(RunCaptureOnce(count_shutter)==7 && shutters==1); // nothing holds: the shutter is reached once and its result returned
    all_sessions_passed=false;check(!CaptureOnceHeld());
    all_sessions_passed=passed_before;capture_once_alias.reset();
    {   // The window's minimum size: the longest confirmation and the button row, within the screen's work area.
        MINMAXINFO limits{};SendMessageW(window,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&limits));
        RECT need{0,0,kD810WriteMinClientWidth,kD810WriteMinClientHeight};AdjustWindowRectEx(&need,WS_OVERLAPPEDWINDOW,FALSE,0);
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        check(limits.ptMinTrackSize.x==std::min<int>(need.right-need.left,work.right-work.left) && limits.ptMinTrackSize.y==std::min<int>(need.bottom-need.top,work.bottom-work.top));
        RECT client{};GetClientRect(window,&client);
        check(need.bottom-need.top>work.bottom-work.top || client.bottom>=kD810WriteMinClientHeight); // the window was made at least that large
    }

    // 2. A restart after a write that never wrote its end line: capture is held from the start.
    settings_write.reset();
    {
        D810SettingsFileJournal journal(folder);
        check(journal.Append("a0sw1 op=00000000000000a1 ev=plan t=2000-01-01T00:00:00Z source=copy steps=1") &&
            journal.Append("a0sw1 op=00000000000000a1 ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040"));
    }
    capture_status.clear();
    InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
    screen=settings_write->screen.get();
    check(screen->Status().block==D810CaptureBlock::Unconfirmed && !screen->Status().capture_allowed);
    check(capture_status.find(L"未確認")!=std::wstring::npos);
    {   // The same record with a line that cannot be read: the text at start-up gives the advice for the comparison of the two cameras.
        {D810SettingsFileJournal journal(folder);check(journal.Append("this is not a line of the record"));}
        settings_write.reset();capture_status.clear();
        InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
        screen=settings_write->screen.get();
        check(!screen->Status().capture_allowed && screen->Status().history_damaged && capture_status.find(kD810RecordDamagedAdvice)!=std::wstring::npos);
    }
    capture_once_alias=1;check(CaptureOnceHeld());                    // --capture-b: held by the record
    shutters=0;check(RunCaptureOnce(count_shutter)==1 && shutters==0); // held: 1 returned, the shutter never reached
    all_sessions_passed=false;check(!CaptureOnceHeld());              // a run that already failed does not reach it
    all_sessions_passed=passed_before;capture_once_alias.reset();check(!CaptureOnceHeld());
    run_live();tick();
    check(!IsWindowEnabled(capture_buttons[0]) && !IsWindowEnabled(capture_buttons[1]) && !IsWindowEnabled(pair_button));
    for(int index=0;index<3;++index){capture_pending.reset();RequestCapture(index);check(!capture_pending.has_value());}
    click(200,capture_buttons[0]);click(400,pair_button);
    check(!capture_pending.has_value() && !stopping && capture_status.find(L"撮影を止めています")!=std::wstring::npos);

    // 3. The reread with no camera: nothing opens, the block stays.
    stop_live();tick();
    check(IsWindowEnabled(settings_reread_button)!=FALSE && !IsWindowEnabled(settings_copy_button));
    click(409,settings_reread_button);
    check(wait_idle());
    check(opens->load()==2 && screen->Status().block==D810CaptureBlock::Unconfirmed && !screen->AdmitCapture()); // A and B asked, neither opened
    check(screen->View() && screen->View()->title.find(L"読み直しを最後まで終えられませんでした")!=std::wstring::npos);
    check(drawn_in(960/2+88,960/2+130)>50); // the result in place of C

    // 3b. The settings panel with no camera: it asks for the cameras, none opens, no panel stands, and the buttons that
    //     only a panel has are not shown. (The product table's panel itself is drawn by the confirmation test build.)
    {
        check(SettingsWriteButtonShown(settings_panel_open_button) && IsWindowEnabled(settings_panel_open_button)!=FALSE && !SettingsWriteButtonShown(settings_panel_close_button) &&
            !SettingsWriteButtonShown(settings_panel_write_button) && !SettingsWriteButtonShown(settings_panel_cancel_button) && !SettingsWriteButtonShown(settings_panel_combos[0][0]));
        const int opens_then=opens->load();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(wait_idle());
        check(opens->load()==opens_then+1 && !SettingsPanelOpen() && !screen->Panel() && screen->View() && screen->View()->title.find(L"設定の画面は開きません")!=std::wstring::npos);
        tick();
        check(SettingsWriteButtonShown(settings_panel_open_button) && !SettingsWriteButtonShown(settings_panel_close_button) && !SettingsWriteButtonShown(settings_panel_combos[1][0]) && CanRestartPreview());
        // A pick notification with no panel does nothing.
        const auto picks=settings_panel_picks;
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kPanelComboBaseId,CBN_SELCHANGE),reinterpret_cast<LPARAM>(settings_panel_combos[0][0]));
        click(kPanelWriteButtonId,settings_panel_write_button);click(kPanelCancelButtonId,settings_panel_cancel_button);
        check(settings_panel_picks==picks && !SettingsWriteBusy() && opens->load()==opens_then+1);
        // While Live runs and capture is held (this record is unconfirmed): the button is there, but the panel does not stop Live by itself.
        run_live();tick();
        const auto stops=manual_preview_stops;
        check(CanFocus(*cameras[0]) && SettingsWriteButtonShown(settings_panel_open_button) && !IsWindowEnabled(settings_panel_open_button));
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(!stopping && manual_preview_stops==stops && !settings_panel_open_pending && !SettingsWriteBusy() && opens->load()==opens_then+1 &&
            capture_status.find(L"自動では止めません")!=std::wstring::npos);
        stop_live();tick();
    }

    // 3c. With capture free, the panel stops Live by itself, reads when both sessions are closed, and gives up when a capture failed.
    {
        settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
        InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
        screen=settings_write->screen.get();
        run_live();preview_joined=false;tick();
        const auto stops=manual_preview_stops;const int opens_then=opens->load();
        check(CanFocus(*cameras[0]) && CanFocus(*cameras[1]) && SettingsWriteButtonShown(settings_panel_open_button) && IsWindowEnabled(settings_panel_open_button)!=FALSE &&
            !SettingsWriteButtonShown(settings_copy_button));
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending && stopping && manual_preview_stops==stops+1 && opens->load()==opens_then && !SettingsWriteBusy() && !SettingsPanelOpen());
        tick();
        check(!IsWindowEnabled(settings_panel_open_button) && opens->load()==opens_then); // being stopped: nothing is read yet
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(manual_preview_stops==stops+1);
        // The Live sessions end (the synthetic cameras): the tick joins them, the next tick reads for the panel.
        for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}
        tick();
        check(preview_joined);
        tick();
        check(wait_idle());
        check(!settings_panel_open_pending && opens->load()==opens_then+1 && !SettingsPanelOpen() && screen->View() && screen->View()->title.find(L"設定の画面は開きません")!=std::wstring::npos);
        // A capture that failed while it was being stopped: given up, nothing is read.
        run_live();preview_joined=false;capture_restart_blocked=false;tick();
        const int opens_before=opens->load();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending);
        capture_restart_blocked=true;
        tick();
        check(!settings_panel_open_pending && capture_status.find(L"設定の画面は開きませんでした")!=std::wstring::npos && opens->load()==opens_before);
        capture_restart_blocked=false;
        // The window closes while it is being stopped: given up too.
        run_live();preview_joined=false;tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending);
        closing=true;tick();
        check(!settings_panel_open_pending && opens->load()==opens_before);
        closing=false;
        // After a failed capture the panel does not stop Live by itself: the button is not offered and a click stops nothing.
        run_live();preview_joined=false;capture_restart_blocked=true;tick();
        {
            const auto stops_then=manual_preview_stops;
            check(CanFocus(*cameras[0]) && !SettingsPanelMayStopLive() && SettingsWriteButtonShown(settings_panel_open_button) && !IsWindowEnabled(settings_panel_open_button));
            click(kPanelOpenButtonId,settings_panel_open_button);
            check(!stopping && manual_preview_stops==stops_then && !settings_panel_open_pending && !settings_panel_stopped_live);
        }
        capture_restart_blocked=false;
        // The stop takes too long: given up with a word on what to do, and the panel holds no Live afterwards.
        run_live();preview_joined=false;tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending && settings_panel_stopped_live);
        settings_panel_pending_since=Clock::now()-kSettingsPanelPendingLimit-std::chrono::seconds(1);
        tick();
        check(!settings_panel_open_pending && !settings_panel_stopped_live && capture_status.find(L"ライブの停止が終わらなかった")!=std::wstring::npos && opens->load()==opens_before);
        // A restart of Live (any path) ends the wait: nothing opens when Live stops again later.
        run_live();preview_joined=false;tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending && settings_panel_stopped_live);
        DiscardSettingsView();
        check(!settings_panel_open_pending && !settings_panel_stopped_live);
        // With a capture under way nothing is stopped.
        run_live();preview_joined=false;tick();
        capture_pending=0;
        check(!SettingsPanelMayStopLive());
        capture_pending.reset();
        stop_live();tick();
    }

    // 3d. The mark "the panel stopped Live" follows the real path: set with the wait, kept while the read for the panel runs, gone when the
    //     panel did not open; never set when Live was stopped by hand; and a capture hold that arrives during the wait ends the wait.
    {
        settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
        auto read_gate=std::make_shared<BlockingSettingsAccess::Gate>();
        InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),[read_gate]{return std::make_unique<BlockingSettingsAccess>(read_gate);});
        screen=settings_write->screen.get();
        const auto view_has=[&](const wchar_t* part){if(!screen->View())return false;for(const auto& line:screen->View()->lines)if(line.find(part)!=std::wstring::npos)return true;return false;};
        const auto entered=[&]{for(int i=0;i<1000 && read_gate->entered.load()<1;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));return read_gate->entered.load()>=1;};
        check(settings_write_stuck_limit>kD810LongPlanDeadline+std::chrono::seconds(12)); // the closing window waits longer than the longest write
        run_live();preview_joined=false;tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending && settings_panel_stopped_live);
        for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}
        tick();check(preview_joined);
        tick(); // the wait ends, the read for the panel starts and waits inside the camera's Open
        check(entered() && SettingsWriteBusy() && !settings_panel_open_pending && settings_panel_stopped_live);
        tick();tick();
        check(SettingsWriteBusy() && settings_panel_stopped_live); // kept while the read runs
        read_gate->release.store(true);
        check(wait_idle());tick();
        check(!SettingsPanelOpen() && !settings_panel_stopped_live && screen->View() && view_has(L"「ライブ再開」で再開できます"));
        // Live stopped by hand: the button reads, and the mark is never set.
        read_gate->release.store(false);read_gate->entered.store(0);
        stop_live();tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(SettingsWriteBusy() && !settings_panel_stopped_live && !settings_panel_open_pending);
        check(entered());tick();
        check(!settings_panel_stopped_live);
        read_gate->release.store(true);
        check(wait_idle());tick();
        check(!settings_panel_stopped_live && !SettingsPanelOpen());
        // A capture hold that arrives during the wait for the stop ends the wait.
        read_gate->release.store(false);
        run_live();preview_joined=false;tick();
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(settings_panel_open_pending && settings_panel_stopped_live);
        {
            D810SettingsFileJournal journal(folder);
            check(journal.Append("a0sw1 op=00000000000000b1 ev=plan t=2000-01-01T00:00:00Z source=copy steps=1") &&
                journal.Append("a0sw1 op=00000000000000b1 ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040"));
        }
        screen->Refresh(); // the status is read again every two seconds; the smoke does not wait for that
        tick();
        check(!settings_panel_open_pending && !settings_panel_stopped_live && !screen->Status().capture_allowed && capture_status.find(L"設定の画面は開きませんでした")!=std::wstring::npos);
        read_gate->release.store(true);
        stop_live();tick();
    }

    // 4. A reread that has not ended (its Open waits): the cameras are held against everything that could ask for
    //    them. The backend and the restart are counting stand-ins, so that a missing guard cannot reach a camera.
    settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
    auto gate=std::make_shared<BlockingSettingsAccess::Gate>();
    InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),[gate]{return std::make_unique<BlockingSettingsAccess>(gate);});
    screen=settings_write->screen.get();
    const auto saved_backend=settings_backend;const auto saved_override=preview_restart_override;
    unsigned backend_calls{},restarts{};
    settings_backend=[&](int index,const std::wstring&,Clock::time_point,const std::atomic<bool>&){
        ++backend_calls;SettingsCameraResult result;result.read.alias=index==0?"CAM-A":"CAM-B";MarkSettingsUnread(result,"not_started");return result;};
    preview_restart_override=[&]{++restarts;};
    const auto wait_entered=[&]{for(int i=0;i<1000 && gate->entered.load()<1;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));return gate->entered.load()>=1;};
    const auto release_gate=[&]{gate->release.store(true);};
    stop_live();tick();
    check(CanReadSettings() && screen->RereadOffered(true) && !SettingsWriteBusy());
    const auto sessions_before=preview_sessions,reads_before=settings_reads_started,restarts_before=manual_preview_restarts;
    click(409,settings_reread_button);
    check(wait_entered() && SettingsWriteBusy());
    tick();
    check(!CanReadSettings() && !CanRestartPreview());
    click(403,session_button);click(405,settings_button);
    check(restarts==0 && manual_preview_restarts==restarts_before && settings_reads_started==reads_before && backend_calls==0 && !settings_future.valid());
    {   // StartPreview itself refuses (cleanup_only: a missing guard would start no camera and read no condition file)
        const bool was_cleanup_only=cleanup_only;cleanup_only=true;
        bool refused=false;
        try{StartPreview();}catch(const std::runtime_error& error){refused=std::string(error.what()).find("settings write")!=std::string::npos;}
        cleanup_only=was_cleanup_only;
        check(refused && preview_sessions==sessions_before);
    }
    run_live();tick();   // the cameras look alive: still no AF, no capture
    check(!CanFocus(*cameras[0]) && !CanFocus(*cameras[1]));
    for(int index=0;index<3;++index){capture_pending.reset();RequestCapture(index);check(!capture_pending.has_value());}
    check(!IsWindowEnabled(capture_buttons[0]) && !IsWindowEnabled(capture_buttons[1]) && !IsWindowEnabled(pair_button));
    {   // A capture asked for before the write, whose Live sessions have just been joined: the worker is not started.
        const bool passed_then=all_sessions_passed;
        stop_live();preview_joined=false;capture_stop_requested=true;capture_pending=0;
        tick();
        check(preview_joined && !capture_pending.has_value() && !capture_future.valid() && capture_status.find(L"途中")!=std::wstring::npos);
        capture_stop_requested=false;all_sessions_passed=passed_then;
    }
    stop_live();
    review_pixels={1,1,{0,0,0,255}};review_visible=true; // a picture asked for while the reread runs
    release_gate();
    check(wait_idle());
    check(!review_visible && screen->View() && drawn_in(960/2+88,960/2+130)>50); // the result is shown, not the picture
    {   // A picture asked for while a result is up is drawn behind it, not over it (a green picture shows no green).
        review_pixels={8,8,std::vector<BYTE>(8*8*4)};
        for(std::size_t i=0;i<review_pixels.bytes.size();i+=4){review_pixels.bytes[i+1]=255;review_pixels.bytes[i+3]=255;}
        review_visible=true;
        SettingsWriteSmokeFrame frame(1400,960);
        check(frame.drawn[3]==0 && frame.Count(960/2+80,960,RGB(0,255,0))==0 && frame.Lit(960/2+88,960/2+130)>50);
    }
    review_visible=false;review_pixels={};

    // 5. The window is asked to close while a reread runs: it stays until the worker has closed its camera.
    gate->release.store(false);gate->entered.store(0);
    tick();
    check(CanReadSettings() && screen->RereadOffered(true));
    click(409,settings_reread_button);
    check(wait_entered() && SettingsWriteBusy());
    SendMessageW(window,WM_CLOSE,0,0);
    for(int i=0;i<10;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    GetWindowTextW(window,caption,256);
    check(closing && IsWindow(window) && SettingsWriteBusy() && std::wstring(caption).find(L"終わりを待っています")!=std::wstring::npos);
    check(capture_status.find(L"USB を抜かず")!=std::wstring::npos);
    SendMessageW(window,WM_CLOSE,0,0);tick();   // a repeated request keeps the wait wording (it used to bring the first wording back)
    GetWindowTextW(window,caption,256);
    check(std::wstring(caption).find(L"終わりを待っています")!=std::wstring::npos && settings_write_stuck_notices==0 && !settings_write_stuck_shown);
    settings_write_stuck_limit=std::chrono::milliseconds(1);   // the limit passes while the worker does not end
    for(int i=0;i<10;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    GetWindowTextW(window,caption,256);
    check(IsWindow(window) && SettingsWriteBusy() && settings_write_stuck_shown && settings_write_stuck_notices==1);
    check(std::wstring(caption).find(L"応答がありません")!=std::wstring::npos && capture_status.find(L"タスクマネージャー")!=std::wstring::npos &&
        capture_status.find(L"撮影が止まったまま")!=std::wstring::npos && capture_status.find(L"読み直して一致を確かめるまでは")!=std::wstring::npos);
    SendMessageW(window,WM_CLOSE,0,0);tick();   // another request: the notice stays, once, and the window still waits for the worker
    GetWindowTextW(window,caption,256);
    check(IsWindow(window) && SettingsWriteBusy() && settings_write_stuck_notices==1 && std::wstring(caption).find(L"応答がありません")!=std::wstring::npos);
    release_gate();
    bool window_gone=false;
    for(int i=0;i<400 && !window_gone;++i){tick();window_gone=!IsWindow(window);if(!window_gone)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    check(window_gone && !SettingsWriteBusy());
    check(closing && !SettingsWriteBusy());
    FinishSettingsWrite();
    if(IsWindow(window))DestroyWindow(window);
    settings_backend=saved_backend;preview_restart_override=saved_override;
    settings_write.reset();
    std::filesystem::remove_all(folder,ignored);
    stopping=true;
    Log("settings_write_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0 camera_opens_attempted="+std::to_string(opens->load()));
    return failures?1:0;
}
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
// --- Camera-free contract of the confirmation screen (#416 stage B): the window's own code draws a confirmation ---
// Only the confirmation test build (CMake a0_preview_settings_write_confirm_tests) has a controller that takes a table,
// so only here can a confirmation be put up. The table enables CAM-B's rows; the bodies are two synthetic snapshots (no
// transport exists: the camera access opens nothing); the plan is taken as the worker's result would be. Everything after
// that is the preview's: Paint, the present in WM_PAINT, the timer's button state and the button's handler.
D810SettingReading SmokeReading(std::uint16_t code,std::int64_t current,std::vector<std::int64_t> values){
    const auto* spec=FindD810Setting(code);
    D810SettingReading reading;reading.id=spec->id;reading.available=true;
    reading.desc.code=code;reading.desc.data_type=static_cast<std::uint16_t>(spec->type);reading.desc.get_set=1;reading.desc.form_flag=2;
    reading.desc.default_value=current;reading.desc.current_value=current;reading.desc.values=std::move(values);return reading;
}
D810SettingsSnapshot SmokeBody(std::int64_t shutter,std::int64_t aperture,std::int64_t iso,std::int64_t white_balance,std::int64_t metering){
    return {SmokeReading(0x500e,1,{1,2,3,4}),SmokeReading(0xd054,0,{0,1}),SmokeReading(0x500d,shutter,{125,250,500,1000}),
        SmokeReading(0x5007,aperture,{400,560,800,1100}),SmokeReading(0x500f,iso,{100,200,400}),
        SmokeReading(0x5005,white_balance,{2,4,5,6,7}),SmokeReading(0x500b,metering,{1,2,3,4})};
}
D810WriteTable SmokeConfirmTable(){
    auto table=kD810ProductWriteTable;
    for(auto& row:table.rows){
        if(row.item==D810WriteItem::IsoAuto)row.code=0xd054;
        if(row.item==D810WriteItem::ShutterSpeed)row.code=0x500d;
        row.enabled=row.camera==D810Camera::B && D810WriteItemOf(row.item).scope!=D810ItemScope::Adr41;
    }
    table.chosen_values=false;table.verified_firmware_a=nullptr; // the copy table of the confirmation smoke: picked values and CAM-A stay off whatever the product table opens
    return table;
}
// How many buttons (and drop-downs) that are shown overlap the window rectangle of `target`, other than `target` itself. A
// confirmation is only safe to press when nothing else is drawn over its button; the smokes press by message and cannot see this.
int ShownSiblingsOverlapping(HWND target){
    RECT own{};
    if(!target || !GetWindowRect(target,&own))return -1;
    int count{};
    for(HWND child=GetWindow(GetParent(target),GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        if(child==target || !SettingsWriteButtonShown(child))continue;
        RECT other{},common{};
        if(GetWindowRect(child,&other) && IntersectRect(&common,&own,&other))++count;
    }
    return count;
}
// How many pairs of shown children of `parent` overlap, whichever they are. A confirmation, the open panel and a result are each
// drawn with several buttons and drop-downs; no shown one may lie over another.
int ShownChildPairsOverlapping(HWND parent){
    std::vector<RECT> rects;
    for(HWND child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        RECT r{};
        if(SettingsWriteButtonShown(child) && GetWindowRect(child,&r))rects.push_back(r);
    }
    int count{};
    for(std::size_t i=0;i<rects.size();++i)for(std::size_t j=i+1;j<rects.size();++j){RECT common{};if(IntersectRect(&common,&rects[i],&rects[j]))++count;}
    return count;
}
// Only CAM-B's ISO row is enabled (the codes the plan reads are set): every other item is left out with the table's reason.
D810WriteTable SmokeIsoOnlyTable(){
    auto table=SmokeConfirmTable();
    for(auto& row:table.rows)row.enabled=row.camera==D810Camera::B && row.item==D810WriteItem::Iso;
    return table;
}
// The widest line, measured as the window measures it (DT_CALCRECT, one line, the default font), that any confirmation can
// show: the title and the lines of `view` (the fixed notices, the headings and the closing line come from a real one), the
// step line of every item with the widest values it can name, and the line of every item with every reason a plan
// carries. `which` receives the text of the widest.
int WidestPossibleConfirmationLine(HDC dc,const D810WriteScreenView& view,std::wstring& which){
    int widest=0;
    const auto width_of=[&](const std::wstring& text){RECT r{0,0,0,0};DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);return static_cast<int>(r.right);};
    const auto measure=[&](const std::wstring& text){const int w=width_of(text);if(w>widest){widest=w;which=text;}};
    measure(view.title);
    for(const auto& line:view.lines)measure(line);
    static constexpr const char* reasons[]={"unreadable","value_content_dependent","value_not_listed","left_out_by_operator","row_disabled","copy_disabled",
        "copy_targets_cam_b_only","copy_item_out_of_scope","code_unconfirmed","type_mismatch","not_writable","value_out_of_type","value_not_advertised"};
    for(const auto& spec:kD810WriteItems){
        if(spec.scope==D810ItemScope::Adr41)continue;
        for(const char* reason:reasons)measure(D810WriteSkippedLine(spec.item,reason)); // already indented as the view has it
        // The value on CAM-B can be anything its type holds (and the shutter's special values); the value written is
        // from the item's closed list, or anything of the type when it has none.
        const std::int64_t top=spec.type==D810ValueType::Uint8?0xff:0xffff;
        std::vector<std::int64_t> before,after;
        for(std::int64_t v=0;v<=top;++v)before.push_back(v);
        if(spec.type==D810ValueType::Uint32){before.push_back(0xfffffffdLL);before.push_back(0xffffffffLL);}
        if(spec.allowed_count)after.assign(spec.allowed.begin(),spec.allowed.begin()+static_cast<std::ptrdiff_t>(spec.allowed_count));
        else for(std::int64_t v=0;v<=top;++v)after.push_back(v);
        const auto widest_value=[&](const std::vector<std::int64_t>& values){
            // Only the longest texts (by characters) are measured: a text two characters shorter is never wider here.
            std::size_t longest=0;
            std::vector<std::pair<std::int64_t,std::wstring>> texts;
            for(const auto v:values){auto text=D810WriteValueText(spec.codes[0],v);longest=std::max(longest,text.size());texts.emplace_back(v,std::move(text));}
            std::int64_t best=values.front();int best_width=-1;
            for(const auto& [v,text]:texts){
                if(text.size()+2<longest)continue;
                const int w=width_of(text);if(w>best_width){best_width=w;best=v;}
            }
            return best;
        };
        D810WriteStep step;step.camera=D810Camera::B;step.item=spec.item;step.code=spec.codes[0];
        step.before=widest_value(before);step.value=widest_value(after);
        measure(L"  "+D810WriteStepLine(step));
    }
    return widest;
}
int RunSettingsWriteConfirmSmoke(){
    Log("settings_write_confirm_smoke=1 camera_operations=0 test_table=1");
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("settings_write_confirm_failed_check="+std::to_string(checks));}};
    wchar_t temp[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,temp))return 2;
    const auto folder=std::filesystem::path(temp)/(L"a0-settings-write-confirm-smoke-"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;std::filesystem::remove_all(folder,ignored);
    settings_write_min_size_enabled=false; // the windows below are made smaller than the real window may be
    HINSTANCE instance{};
    HWND window=CreateSettingsWriteSmokeWindow(L"A0SettingsWriteConfirmSmoke",instance);
    if(!window)return 2;
    review_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(401),instance,nullptr);
    // Shown without taking the focus: the present of a paint is a BitBlt to the window's DC, which a window that was
    // never shown can refuse on a remote session, and the checks below need "drawn whole" to mean a successful present.
    ShowWindow(window,SW_SHOWNOACTIVATE);
    auto opens=std::make_shared<std::atomic<int>>(0);
    InitSettingsWriteWithTable(SmokeConfirmTable(),std::make_unique<D810SettingsFileJournal>(folder),[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);});
    auto* screen=settings_write->screen.get();
    check(screen->Status().enabled_rows==7 && screen->Status().write_allowed && screen->Status().capture_allowed);
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
    const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
    const auto paint=[&]{SendMessageW(window,WM_PAINT,0,0);};
    const auto wait_idle=[&]{for(int i=0;i<400;++i){tick();if(!SettingsWriteBusy())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;};
    const auto resize=[&](int client_width,int client_height){
        RECT frame{0,0,client_width,client_height};AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,0);
        SetWindowPos(window,nullptr,0,0,frame.right-frame.left,frame.bottom-frame.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        RECT client{};GetClientRect(window,&client);return client.right==client_width && client.bottom==client_height;
    };
    // Five steps (iso, shutter, aperture, white balance, metering), or four when `more_alike` (the aperture reads the same).
    const auto put_up_plan=[&](bool more_alike){
        D810PlanRead read;read.complete=true;
        read.snapshots[0]=SmokeBody(250,800,200,4,3);
        read.snapshots[1]=more_alike?SmokeBody(125,800,400,2,2):SmokeBody(500,560,100,2,2);
        screen->TakePlanReadForTest(read);
    };
    const auto confirm_enabled=[&]{return SettingsWriteButtonShown(settings_confirm_button) && IsWindowEnabled(settings_confirm_button)!=FALSE;};
    // The width at which every line (and the title) of the view is not cut, measured as the window draws them.
    const auto widest=[&]{
        int widest_text=0;HDC dc=CreateCompatibleDC(nullptr);
        const auto* view=screen->View();
        if(dc && view){
            const auto measure=[&](const std::wstring& text){RECT r{0,0,0,0};DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);widest_text=std::max<int>(widest_text,r.right);};
            measure(view->title);for(const auto& line:view->lines)measure(line);
        }
        if(dc)DeleteDC(dc);return widest_text;
    };
    const auto green_picture=[&]{
        review_pixels={8,8,std::vector<BYTE>(8*8*4)};
        for(std::size_t i=0;i<review_pixels.bytes.size();i+=4){review_pixels.bytes[i+1]=255;review_pixels.bytes[i+3]=255;}
    };
    for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}preview_joined=true;stopping=true; // both Live sessions joined
    check(resize(1400,1000));
    tick();
    check(CanReadSettings() && !screen->View() && SettingsWriteButtonShown(settings_copy_button));

    // 1. A confirmation is up and has not been drawn: the button is shown but not pressable, and the handler refuses.
    put_up_plan(false);
    const auto* view=screen->View();
    const int needed=view?D810WriteViewMinClientBottom(view->lines.size()):0;
    check(screen->ConfirmationShown() && view && view->confirmation && view->lines.size()==9 && screen->ViewSerial()!=0);
    tick();
    check(SettingsWriteButtonShown(settings_confirm_button) && !IsWindowEnabled(settings_confirm_button));
    const auto refusals_before=settings_write_confirm_refusals;
    click(407,settings_confirm_button);
    check(settings_write_confirm_refusals==refusals_before+1 && opens->load()==0 && screen->Step()==D810WriteScreenStep::Confirming &&
        !screen->Status().plan_confirmed && capture_status.find(L"何も書いていません")!=std::wstring::npos);

    // 2. Drawn whole by a present of the window: pressable.
    paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled() && settings_write_confirm_refusals==refusals_before+1);
    // Nothing is drawn over the confirm button: the panel's button is not shown while a confirmation is up, and no shown button overlaps it.
    check(!SettingsWriteButtonShown(settings_panel_open_button) && ShownSiblingsOverlapping(settings_confirm_button)==0 && ShownSiblingsOverlapping(settings_discard_button)==0 && ShownChildPairsOverlapping(window)==0);

    // 3. The window gets one row too short: not pressable by the height alone; a paint there draws nothing whole and
    //    leaves the flag down, so growing the window back is not enough until it has been drawn again.
    check(resize(1400,needed-1));tick();
    check(!confirm_enabled());
    paint();
    check(settings_write_drawn_serial==0);
    check(resize(1400,needed));tick();
    check(!confirm_enabled());
    paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());

    // 4. The window gets too narrow for the longest line (cut with an ellipsis): the same.
    const int line_width=widest();
    check(line_width+24>kD810ConfirmButtonLeft+kD810ConfirmButtonWidth); // the lines, not the button, decide the width here
    check(resize(1400,1000));
    check(resize(line_width+24-2,1000));paint();tick();
    check(settings_write_drawn_serial==0 && !confirm_enabled());
    click(407,settings_confirm_button);
    check(settings_write_confirm_refusals==refusals_before+2 && opens->load()==0 && screen->Step()==D810WriteScreenStep::Confirming);
    check(resize(line_width+24,1000));paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());

    // 5. Too small for Paint to draw anything: the flag is down after that paint, whatever the window grows to.
    check(resize(150,150));paint();
    check(settings_write_drawn_serial==0);
    check(resize(1400,1000));tick();
    check(!confirm_enabled());
    paint();tick();
    check(confirm_enabled());

    // 6. The view is replaced (the plan discarded, another put up) and not drawn yet: the earlier drawing does not count.
    const auto drawn_before=settings_write_drawn_serial;
    screen->DiscardPlan();put_up_plan(true);
    check(screen->ConfirmationShown() && screen->View()->lines.size()==8 && screen->ViewSerial()!=drawn_before && settings_write_drawn_serial==drawn_before);
    tick();
    check(!confirm_enabled());
    click(407,settings_confirm_button);
    check(opens->load()==0 && screen->Step()==D810WriteScreenStep::Confirming);
    paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());

    // 7. A picture asked for during a confirmation is drawn behind it: the confirmation is first (Paint's order), and
    //    the picture's button is off while a view is up.
    green_picture();review_visible=true;
    {
        SettingsWriteSmokeFrame frame(1400,960);
        check(frame.drawn[3]==0 && frame.Count(960/2+80,960,RGB(0,255,0))==0 && frame.Lit(960/2+88,960/2+150)>50);
    }
    paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled() && !IsWindowEnabled(review_button));
    review_visible=false;
    // The same for a result view (a discarded plan, a reread that did not end): a picture does not cover it.
    screen->DiscardPlan();
    check(screen->View() && !screen->View()->confirmation && screen->View()->title.find(L"写すのをやめました")!=std::wstring::npos);
    review_visible=true;
    {
        SettingsWriteSmokeFrame frame(1400,960);
        check(frame.drawn[3]==0 && frame.Count(960/2+80,960,RGB(0,255,0))==0 && frame.Lit(960/2+88,960/2+130)>50);
    }
    tick();
    check(!IsWindowEnabled(review_button) && !SettingsWriteButtonShown(settings_confirm_button));
    review_visible=false;review_pixels={};

    // 8. Drawn whole and pressed: the handler lets it through, once (the worker opens nothing, so nothing is written).
    put_up_plan(false);
    paint();tick();
    check(confirm_enabled());
    const auto refusals_now=settings_write_confirm_refusals;
    click(407,settings_confirm_button);
    check(wait_idle() && settings_write_confirm_refusals==refusals_now && opens->load()>=1 && screen->Step()==D810WriteScreenStep::Idle);
    click(407,settings_confirm_button);   // the plan was used: a second press does not run it again
    const int opens_after=opens->load();
    check(wait_idle() && opens->load()==opens_after);

    // 9. One row is enabled (CAM-B's ISO) while CAM-A's white balance is a kind a copy cannot carry and the metering modes
    //    differ. The window is its smallest size. White balance and metering are left out with the table's short
    //    reason (not the long one about the content held in the body), and the confirmation is drawn whole and pressable.
    settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
    InitSettingsWriteWithTable(SmokeIsoOnlyTable(),std::make_unique<D810SettingsFileJournal>(folder),[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);});
    screen=settings_write->screen.get();
    check(screen->Status().enabled_rows==1 && screen->Status().write_allowed && screen->Status().capture_allowed);
    {
        D810PlanRead read;read.complete=true;
        read.snapshots[0]=SmokeBody(250,800,200,0x8013,3);
        read.snapshots[1]=SmokeBody(250,800,100,2,2);
        screen->TakePlanReadForTest(read);
    }
    check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));
    tick();
    const auto* first_view=screen->View();
    check(screen->ConfirmationShown() && first_view && first_view->lines.size()==8); // 3 notices, the ISO step, the heading, 2 left out, the closing line
    const auto line_with=[&](const wchar_t* needle){
        if(!screen->View())return false;
        for(const auto& line:screen->View()->lines)if(line.find(needle)!=std::wstring::npos)return true;
        return false;
    };
    check(line_with(L"ホワイトバランス: 表で無効の項目です") && line_with(L"測光: 表で無効の項目です") && !line_with(L"value_content_dependent") && !line_with(L"B の本体で合わせてください"));
    paint();tick();
    check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());
    // The widest line any confirmation can show fits the smallest window (none of them is cut at its width).
    {
        std::wstring which;int widest_possible=0;
        HDC dc=CreateCompatibleDC(nullptr);
        if(dc && first_view){widest_possible=WidestPossibleConfirmationLine(dc,*first_view,which);DeleteDC(dc);}
        std::string which_utf8;
        if(!which.empty()){
            const int bytes=WideCharToMultiByte(CP_UTF8,0,which.c_str(),static_cast<int>(which.size()),nullptr,0,nullptr,nullptr);
            if(bytes>0){which_utf8.assign(static_cast<std::size_t>(bytes),'\0');WideCharToMultiByte(CP_UTF8,0,which.c_str(),static_cast<int>(which.size()),which_utf8.data(),bytes,nullptr,nullptr);}
        }
        Log("settings_write_confirm_widest_possible_line_px="+std::to_string(widest_possible)+" window_line_px="+std::to_string(kD810WriteMinClientWidth-24)+
            " widest_chars="+std::to_string(which.size())+" widest_text="+which_utf8);
        check(widest_possible>0 && widest_possible<=kD810WriteMinClientWidth-24);
    }
    // The window gets narrower than the longest line and no paint has happened yet: not pressable (the drawing is gone).
    {
        const int own_width=widest();
        check(own_width+24>kD810ConfirmButtonLeft+kD810ConfirmButtonWidth);
        check(resize(own_width+24-2,kD810WriteMinClientHeight));
        tick();
        check(settings_write_drawn_serial==0 && !confirm_enabled());
        click(407,settings_confirm_button);
        check(opens->load()==opens_after && screen->Step()==D810WriteScreenStep::Confirming);
        check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));paint();tick();
        check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());
    }

    FinishSettingsWrite();
    if(IsWindow(window))DestroyWindow(window);
    settings_write.reset();
    std::filesystem::remove_all(folder,ignored);
    stopping=true;
    Log("settings_write_confirm_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0 camera_opens_attempted="+std::to_string(opens->load()));
    return failures?1:0;
}
// --- Camera-free contract of the settings panel (#416, ADR-0041): the window's own code, with a table of the test's own ---
// Like the confirmation smoke: only this test build has a controller that takes a table. The bodies are synthetic
// snapshots taken as the panel's read; the camera access opens nothing. Everything after that is the preview's: the
// timer's button state, the drop-down notifications, Paint and the buttons' handlers.
// CAM-B and CAM-A rows of the five items an operator may pick, with chosen values on.
D810WriteTable SmokeChosenTable(){
    auto table=SmokeConfirmTable();
    for(auto& row:table.rows)
        row.enabled=row.item==D810WriteItem::Iso || row.item==D810WriteItem::ShutterSpeed || row.item==D810WriteItem::Aperture ||
            row.item==D810WriteItem::WhiteBalance || row.item==D810WriteItem::Metering;
    table.chosen_values=true;table.verified_firmware_a="V0";
    return table;
}
// The text of the entry `index` of a drop-down.
std::wstring SmokeComboText(HWND combo,int index){
    if(index<0)return {};
    const int length=static_cast<int>(SendMessageW(combo,CB_GETLBTEXTLEN,static_cast<WPARAM>(index),0));
    if(length<=0)return {};
    std::wstring text(static_cast<std::size_t>(length)+1,L'\0');
    SendMessageW(combo,CB_GETLBTEXT,static_cast<WPARAM>(index),reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));return text;
}
// The widest line, measured as the window measures it (DT_CALCRECT, one line, the default font), that a confirmation of picks or
// its result can show beyond its fixed lines: the line of an item with the widest values it can name on both cameras and the
// note about a value that cannot be set again, and the line of a result with the widest read-back. `which` receives its text.
int WidestPossibleChosenLine(HDC dc,std::wstring& which){
    int widest=0;
    const auto width_of=[&](const std::wstring& text){RECT r{0,0,0,0};DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);return static_cast<int>(r.right);};
    const auto note=[&](const std::wstring& text){const int w=width_of(text);if(w>widest){widest=w;which=text;}};
    // The widest of a set of texts: only the ones with the most characters are measured (two characters fewer is never wider here).
    const auto widest_text=[&](const std::vector<std::wstring>& texts){
        std::size_t longest=0;
        for(const auto& text:texts)longest=std::max(longest,text.size());
        std::wstring best;int best_width=-1;
        for(const auto& text:texts){
            if(text.size()+2<longest)continue;
            const int w=width_of(text);if(w>best_width){best_width=w;best=text;}
        }
        return best;
    };
    for(const auto choice:kD810ChoiceItems){
        const auto item=D810ChoiceWriteItem(choice);const auto& spec=D810WriteItemOf(item);
        const std::int64_t top=spec.type==D810ValueType::Uint8?0xff:0xffff;
        std::vector<std::wstring> can_set,as_read,as_read_back;
        std::vector<std::int64_t> domain;
        for(std::int64_t v=0;v<=top;++v)domain.push_back(v);
        if(spec.type==D810ValueType::Uint32){domain.push_back(0xfffffffdLL);domain.push_back(0xffffffffLL);}
        for(const auto v:domain){
            if(D810ChoiceNamed(choice,v) && D810ChoiceBand(choice,v))can_set.push_back(D810ChoiceValueText(item,spec.codes[0],v));
            as_read.push_back(D810WriteValueText(spec.codes[0],v));
            as_read_back.push_back(D810ChoiceValueText(item,spec.codes[0],v));
        }
        const auto set=widest_text(can_set);
        const auto read=widest_text(as_read);
        const auto back=widest_text(as_read_back);
        const std::wstring label=std::wstring(D810WriteItemLabel(item));
        // Both values can be set again: no note. A value that cannot be set again is shown as the camera reports it, with the note.
        note(L"  "+label+L": CAM-B "+set+L" → "+set+L" ／ CAM-A "+set+L" → "+set);
        note(L"  "+label+L": CAM-B "+read+L" → "+set+L" ／ CAM-A "+read+L" → "+set+L"（元の値は本体でしか戻せません）");
        note(L"  "+label+L": CAM-B 不一致（読み戻し "+back+L"） ／ CAM-A 不一致（読み戻し "+back+L"）");
    }
    return widest;
}
int RunSettingsWriteChosenSmoke(){
    Log("settings_write_chosen_smoke=1 camera_operations=0 test_table=1");
    unsigned checks{},failures{};const auto check=[&](bool ok,int line=__builtin_LINE()){++checks;if(!ok){++failures;Log("settings_write_chosen_failed_check="+std::to_string(checks)+" source_line="+std::to_string(line));}};
    wchar_t temp[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,temp))return 2;
    const auto folder=std::filesystem::path(temp)/(L"a0-settings-write-chosen-smoke-"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;std::filesystem::remove_all(folder,ignored);
    settings_write_min_size_enabled=false;
    HINSTANCE instance{};
    HWND window=CreateSettingsWriteSmokeWindow(L"A0SettingsWriteChosenSmoke",instance);
    if(!window)return 2;
    review_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(401),instance,nullptr);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    auto opens=std::make_shared<std::atomic<int>>(0);
    const auto factory=[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);};
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
    const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
    const auto resize=[&](int client_width,int client_height){
        RECT frame{0,0,client_width,client_height};AdjustWindowRectEx(&frame,WS_OVERLAPPEDWINDOW,FALSE,0);
        SetWindowPos(window,nullptr,0,0,frame.right-frame.left,frame.bottom-frame.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        RECT client{};GetClientRect(window,&client);return client.right==client_width && client.bottom==client_height;
    };
    for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}preview_joined=true;stopping=true; // both Live sessions joined
    check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));
    const auto shown=[&](HWND control){return SettingsWriteButtonShown(control);};
    const auto enabled=[&](HWND control){return shown(control) && IsWindowEnabled(control)!=FALSE;};
    const auto combo_ok=[&](int camera,int item){return settings_panel_combos[camera][item];};
    // Picks a value of a drop-down by its text, as the operator does: the selection, then the notification.
    const auto choose=[&](int camera,int item,const std::wstring& text){
        HWND combo=combo_ok(camera,item);
        const int count=static_cast<int>(SendMessageW(combo,CB_GETCOUNT,0,0));
        for(int i=0;i<count;++i)if(SmokeComboText(combo,i)==text){
            SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(i),0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(kPanelComboBaseId+camera*static_cast<int>(kD810ChoiceItemCount)+item,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
            return true;
        }
        return false;
    };
    const auto selected=[&](int camera,int item){HWND combo=combo_ok(camera,item);return SmokeComboText(combo,static_cast<int>(SendMessageW(combo,CB_GETCURSEL,0,0)));};
    const auto read_of=[&](bool b_verified,bool a_verified){
        D810PlanRead read;read.complete=true;read.verified={a_verified,b_verified};
        read.snapshots[0]=SmokeBody(250,800,200,4,3);
        read.snapshots[1]=SmokeBody(500,560,100,2,2);
        return read;
    };
    constexpr int kIso=static_cast<int>(D810ChoiceItem::Iso),kShutter=static_cast<int>(D810ChoiceItem::ShutterSpeed),kWb=static_cast<int>(D810ChoiceItem::WhiteBalance);
    D810SettingsWriteScreen* screen=nullptr;
    unsigned restarts{};const auto saved_override=preview_restart_override;preview_restart_override=[&]{++restarts;}; // a Live restart is a counter here

    // 1. The product table: the panel opens on a read and shows both cameras. CAM-A's body is read as not verified, so none of CAM-A's
    //    drop-downs can be used (the table's row or the body check locks them); CAM-B's follow the table's rows.
    InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
    screen=settings_write->screen.get();
    tick();
    check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight) && CanReadSettings());
    check(shown(settings_panel_open_button) && enabled(settings_panel_open_button) && !shown(settings_panel_close_button) && !shown(settings_panel_write_button) && !shown(combo_ok(0,kIso)));
    click(kPanelOpenButtonId,settings_panel_open_button); // a read with no camera: it fails and no panel opens
    for(int i=0;i<400 && SettingsWriteBusy();++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    check(!SettingsPanelOpen() && opens->load()>=1 && screen->View() && screen->View()->title.find(L"設定の画面は開きません")!=std::wstring::npos);
    check(!shown(combo_ok(0,kIso)) && !shown(settings_panel_close_button) && enabled(settings_panel_open_button));
    screen->TakePanelReadForTest(read_of(true,false)); // CAM-A's body is not verified
    tick();
    check(SettingsPanelOpen() && screen->Panel() && PanelHoldsLive());
    check(!CanRestartPreview() && !shown(settings_panel_open_button) && enabled(settings_panel_close_button) && shown(settings_panel_write_button) && !enabled(settings_panel_write_button));
    {
        bool all_shown=true,a_locked=true,b_follows_table=true;
        for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
            const bool b_row_on=D810RowRefusal(kD810ProductWriteTable,D810ChoiceWriteItem(static_cast<D810ChoiceItem>(item)),D810Camera::B,D810PlanSource::Chosen)==nullptr;
            all_shown=all_shown && shown(combo_ok(camera,item));
            if(camera==0)a_locked=a_locked && !enabled(combo_ok(camera,item));
            else b_follows_table=b_follows_table && enabled(combo_ok(camera,item))==b_row_on;
        }
        check(all_shown && a_locked && b_follows_table);
        // Same-as-A makes a pick on CAM-B: it is offered only where CAM-B's cell can be picked.
        for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)check(shown(settings_panel_same_buttons[item]) && (!enabled(settings_panel_same_buttons[item]) || enabled(combo_ok(1,item))));
    }
    check(selected(0,kIso)==L"200" && selected(1,kIso)==L"100" && selected(0,kShutter).find(L"1/40")!=std::wstring::npos);
    // A notification of a locked drop-down (a stand-in for a key or the wheel): nothing is picked and the value comes back.
    {
        const auto picks=settings_panel_picks;
        check(choose(0,kIso,L"400") && settings_panel_picks==picks && screen->PanelDirty()==0 && selected(0,kIso)==L"200");
        click(kPanelWriteButtonId,settings_panel_write_button);
        check(!SettingsWriteBusy() && screen->Step()==D810WriteScreenStep::Idle && opens->load()<=2);
    }
    {   // The panel is drawn: the title, the grid, the notes (amber) and the line under it.
        check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));
        SettingsWriteSmokeFrame frame(kD810WriteMinClientWidth,kD810WriteMinClientHeight);
        check(frame.Lit(kPanelTitleY,kPanelTitleY+22)>100 && frame.Lit(SettingsPanelRowY(1),SettingsPanelRowY(8))>400);
        check(frame.Count(SettingsPanelRowY(3),SettingsPanelRowY(8),RGB(255,200,90))>200); // the cells say why they are locked
    }
    check(!enabled(settings_confirm_button) && !CanRestartPreview());
    {   // Live was stopped by hand before the panel was put up: the button keeps its plain name.
        wchar_t named[64]{};GetWindowTextW(settings_panel_close_button,named,64);
        check(std::wstring(named)==L"設定の画面を閉じる");
    }
    click(kPanelCloseButtonId,settings_panel_close_button);
    tick();
    check(!SettingsPanelOpen() && !screen->Panel() && !shown(combo_ok(0,kIso)) && !shown(settings_panel_close_button) && enabled(settings_panel_open_button) && CanRestartPreview());
    check(restarts==0 && manual_preview_restarts==0 && settings_panel_auto_restarts==0 && capture_status.find(L"この画面を開く前から止まっていたため、自動では再開しません")!=std::wstring::npos); // Live was stopped by hand: closing does not start it
    check(capture_status.find(L"設定の画面を閉じました")!=std::wstring::npos);

    // 2. A table of the test's own: the drop-downs work, a pick is a draft and sends nothing.
    settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
    InitSettingsWriteWithTable(SmokeChosenTable(),std::make_unique<D810SettingsFileJournal>(folder),factory);
    screen=settings_write->screen.get();
    tick();
    screen->TakePanelReadForTest(read_of(true,true));
    tick();
    check(SettingsPanelOpen() && screen->Panel() && screen->PanelEditable());
    {
        bool all_enabled=true;
        for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)all_enabled=all_enabled && enabled(combo_ok(camera,item));
        check(all_enabled);
    }
    {   // Each drop-down stands in the column of its camera and the row of its item: the control id says which cell it is, the window says where it is.
        bool placed=true,rows_in_order=true;
        for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
            HWND control=combo_ok(camera,item);RECT r{};POINT want{camera==0?kPanelAX:kPanelBX,SettingsPanelRowY(kPanelFirstChoiceRow+item)};
            ClientToScreen(window,&want);
            placed=placed && control && GetWindowRect(control,&r) && GetDlgCtrlID(control)==kPanelComboBaseId+camera*static_cast<int>(kD810ChoiceItemCount)+item && r.left==want.x && r.top==want.y;
            if(item>0){RECT above{};rows_in_order=rows_in_order && GetWindowRect(combo_ok(camera,item-1),&above) && above.top<r.top;}
        }
        check(placed && rows_in_order);
        RECT a{},b{};
        check(GetWindowRect(combo_ok(0,kIso),&a) && GetWindowRect(combo_ok(1,kIso),&b) && a.left<b.left); // CAM-A is the left column
        bool same_rows=true;
        for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item){
            RECT button{},cell{};
            same_rows=same_rows && GetWindowRect(settings_panel_same_buttons[item],&button) && GetWindowRect(combo_ok(1,item),&cell) && GetDlgCtrlID(settings_panel_same_buttons[item])==kPanelSameBaseId+item && button.left>cell.left && button.top<=cell.top+4 && button.top>=cell.top-4;
        }
        check(same_rows); // the "A と同じにする" button of an item is in that item's row, right of CAM-B's cell
        check(ShownChildPairsOverlapping(window)==0); // an open panel: no shown button or drop-down lies over another
    }
    check(!enabled(settings_panel_cancel_button) && !enabled(settings_panel_write_button));
    wchar_t text[64]{};GetWindowTextW(settings_panel_write_button,text,64);check(std::wstring(text)==L"0 件を書き込む…");
    // The entries of a drop-down: what the camera offers, the one it reads selected.
    {
        HWND combo=combo_ok(1,kIso);
        check(static_cast<int>(SendMessageW(combo,CB_GETCOUNT,0,0))==3 && SmokeComboText(combo,0)==L"100" && selected(1,kIso)==L"100");
    }
    {
        const auto picks=settings_panel_picks;
        check(choose(1,kIso,L"400") && settings_panel_picks==picks+1 && screen->PanelDirty()==1 && selected(1,kIso)==L"400");
        tick();
        GetWindowTextW(settings_panel_write_button,text,64);check(std::wstring(text)==L"1 件を書き込む…");
        check(enabled(settings_panel_cancel_button) && !SettingsWriteBusy() && screen->Step()==D810WriteScreenStep::Idle && opens->load()<=2 && screen->Status().plan_pending==false);
        check(capture_status.find(L"変更予定 1 件（まだ何も書いていません）")!=std::wstring::npos);
        // CAM-B follows CAM-A in the shutter speed (A reads 1/40, B reads 1/20).
        check(enabled(settings_panel_same_buttons[kShutter]));
        click(kPanelSameBaseId+kShutter,settings_panel_same_buttons[kShutter]);
        tick();
        check(screen->PanelDirty()==2 && selected(1,kShutter)==selected(0,kShutter) && settings_panel_picks==picks+2);
        // The same value as the one read is not a change.
        check(choose(0,kWb,selected(0,kWb)) && screen->PanelDirty()==2);
        // The number on the button and in the line under the panel counts changes: three entries (two that change, one that picks the value read now) read "2".
        tick();
        GetWindowTextW(settings_panel_write_button,text,64);
        check(screen->PanelEntryCount()==3 && screen->PanelDirty()==2 && std::wstring(text)==L"2 件を書き込む…" && screen->PanelSummaryText().find(L"変更予定 2 件（")!=std::wstring::npos);
        // CAM-A picks an ISO: CAM-B's pick stays its own.
        check(choose(0,kIso,L"100") && screen->PanelDirty()==3 && selected(0,kIso)==L"100" && selected(1,kIso)==L"400");
    }
    {   // Paint with picks up: the picks are marked.
        SettingsWriteSmokeFrame frame(kD810WriteMinClientWidth,kD810WriteMinClientHeight);
        check(frame.Count(SettingsPanelRowY(3),SettingsPanelRowY(4)+kPanelRowHeight,RGB(255,200,90))>100);
    }
    // The grid fits its columns: every note, label and heading at the width the window gives it.
    {
        HDC dc=CreateCompatibleDC(nullptr);
        int widest_note=0,widest_label=0;std::wstring which_note;
        if(dc){
            const auto width_of=[&](const std::wstring& value){RECT r{0,0,0,0};DrawTextW(dc,value.c_str(),-1,&r,DT_CALCRECT|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);return static_cast<int>(r.right);};
            for(const char* reason:{"unreadable","not_writable","chosen_values_disabled","row_disabled","code_unconfirmed","body_not_verified","exposure_mode_not_manual","iso_auto_not_off","no_options"}){
                const auto note=D810PanelLockText(reason);const int w=width_of(note);if(w>widest_note){widest_note=w;which_note=note;}
            }
            for(const auto& note:{std::wstring(L"＊変更予定"),std::wstring(L"＊変更予定（A と同じ）"),std::wstring(L"本体を M に"),std::wstring(L"本体で OFF に")})
                widest_note=std::max(widest_note,width_of(note));
            for(const auto& row:screen->PanelRows())widest_label=std::max(widest_label,width_of(row.label));
            // The heading of each column and the cell of a read-only row.
            int widest_cell=0;
            for(const auto& value:{std::wstring(L"CAM-A（左・基準）"),std::wstring(L"CAM-B（右）"),std::wstring(L"M（読むだけ）"),std::wstring(L"読めません（読むだけ）"),std::wstring(L"Automatic（読むだけ）")})widest_cell=std::max(widest_cell,width_of(value));
            std::string utf8;
            if(!which_note.empty()){
                const int bytes=WideCharToMultiByte(CP_UTF8,0,which_note.c_str(),static_cast<int>(which_note.size()),nullptr,0,nullptr,nullptr);
                if(bytes>0){utf8.assign(static_cast<std::size_t>(bytes),'\0');WideCharToMultiByte(CP_UTF8,0,which_note.c_str(),static_cast<int>(which_note.size()),utf8.data(),bytes,nullptr,nullptr);}
            }
            Log("settings_write_panel_widest_note_px="+std::to_string(widest_note)+" column_px="+std::to_string(kPanelNoteWidth)+" widest_label_px="+std::to_string(widest_label)+
                " label_column_px="+std::to_string(kPanelLabelWidth)+" widest_cell_px="+std::to_string(widest_cell)+" cell_column_px="+std::to_string(kPanelComboWidth)+" widest_note="+utf8);
            check(widest_note<=kPanelNoteWidth && widest_label<=kPanelLabelWidth && widest_cell<=kPanelComboWidth);
            DeleteDC(dc);
        }else check(false);
        // The columns lie inside the smallest window, and the heights inside the Live area of its smallest height.
        check(kPanelSameX+kPanelSameWidth<=kD810WriteMinClientWidth-12 && kPanelBNoteX+kPanelNoteWidth<=kPanelSameX);
        check(SettingsPanelRowY(kPanelFirstChoiceRow+static_cast<int>(kD810ChoiceItemCount))+kPanelRowHeight+24<=kD810WriteMinClientHeight/2-8);
    }
    // Cancel, and closing with picks up: the count is said, nothing is left.
    click(kPanelCancelButtonId,settings_panel_cancel_button);
    check(screen->PanelDirty()==0 && screen->PanelEntryCount()==0 && selected(1,kIso)==L"100" && capture_status.find(L"3 件を取り消しました")!=std::wstring::npos);
    tick();
    check(!enabled(settings_panel_cancel_button));
    check(choose(1,kIso,L"200") && screen->PanelDirty()==1);
    // The panel stopped Live itself (as its open button does): the button says it gives Live back, and closing does.
    settings_panel_stopped_live=true;tick();
    {
        wchar_t named[64]{};GetWindowTextW(settings_panel_close_button,named,64);
        check(std::wstring(named)==L"閉じてライブに戻る");
    }
    click(kPanelCloseButtonId,settings_panel_close_button);tick();
    check(!SettingsPanelOpen() && capture_status.find(L"書いていない変更 1 件を捨てて")!=std::wstring::npos && screen->PanelDirty()==0);
    check(restarts==1 && manual_preview_restarts==1 && settings_panel_auto_restarts==1 && !settings_panel_stopped_live && capture_status.find(L"ライブビューを再開しています")!=std::wstring::npos); // closing gave Live back, by the restart button's own path
    check(opens->load()<=2); // the panel was put up with a read made without a camera: nothing was opened for any pick

    // 3. "N 件を書き込む…" and the confirmation of the picks: the window's own code draws it, and the confirm button keeps the
    //    rule of the copy (drawn whole, every line in the window, the button inside it).
    const auto paint=[&]{SendMessageW(window,WM_PAINT,0,0);};
    const auto wait_idle=[&]{for(int i=0;i<400;++i){tick();if(!SettingsWriteBusy())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;};
    const auto choose_value=[&](int camera,int item,std::int64_t value){
        HWND combo=combo_ok(camera,item);
        const int count=static_cast<int>(SendMessageW(combo,CB_GETCOUNT,0,0));
        for(int k=0;k<count;++k)if(SettingsPanelValueAt(camera,item,k)==std::optional<std::int64_t>(value)){
            SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(k),0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(kPanelComboBaseId+camera*static_cast<int>(kD810ChoiceItemCount)+item,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
            return true;
        }
        return false;
    };
    constexpr int kAperture=static_cast<int>(D810ChoiceItem::Aperture),kMetering=static_cast<int>(D810ChoiceItem::Metering);
    const auto confirm_enabled=[&]{return shown(settings_confirm_button) && IsWindowEnabled(settings_confirm_button)!=FALSE;};
    settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
    InitSettingsWriteWithTable(SmokeChosenTable(),std::make_unique<D810SettingsFileJournal>(folder),factory);
    screen=settings_write->screen.get();
    screen->TakePanelReadForTest(read_of(true,true));
    check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));
    tick();
    check(!enabled(settings_panel_write_button) && screen->ChosenOfferText(true).find(L"変更予定がありません")!=std::wstring::npos);
    {   // Ten changes: every item on both cameras.


        bool all=true;
        all=choose_value(1,kIso,400) && all;all=choose_value(1,kShutter,1000) && all;all=choose_value(1,kAperture,800) && all;
        all=choose_value(1,kWb,4) && all;all=choose_value(1,kMetering,3) && all;
        all=choose_value(0,kIso,100) && all;all=choose_value(0,kShutter,500) && all;all=choose_value(0,kAperture,560) && all;
        all=choose_value(0,kWb,5) && all;all=choose_value(0,kMetering,2) && all;
        check(all && screen->PanelDirty()==10);
    }
    tick();
    GetWindowTextW(settings_panel_write_button,text,64);
    check(std::wstring(text)==L"10 件を書き込む…" && enabled(settings_panel_write_button) && enabled(settings_panel_cancel_button));
    // Pressed with no camera behind it: the read cannot complete, nothing is planned, and the picks stay.
    {
        const int opens_then=opens->load();
        click(kPanelWriteButtonId,settings_panel_write_button);
        check(wait_idle());
        check(opens->load()==opens_then+1 && screen->Step()==D810WriteScreenStep::Idle && !screen->ConfirmationShown() && screen->PanelDirty()==10 && screen->PanelOpen() &&
            screen->View() && screen->View()->title.find(L"書く前の読み取りを最後まで終えられませんでした")!=std::wstring::npos && !screen->Status().plan_pending);
        tick();
        check(shown(settings_panel_write_button) && !shown(settings_confirm_button));
    }
    // The plan from a read that differs from the panel's in one value (CAM-B's ISO): twelve lines, the longest a confirmation has.
    {
        D810PlanRead read=read_of(true,true);
        read.snapshots[1]=SmokeBody(500,560,64,2,2);
        screen->TakeChosenReadForTest(read);
        const auto* view=screen->View();
        check(screen->ConfirmationShown() && view && view->confirmation && view->lines.size()==12 && view->lines.size()<=D810WriteViewMaxLines() && D810WriteViewFits(*view,kD810WriteMinClientHeight));
        check(screen->Status().plan_pending && !screen->Status().plan_confirmed && screen->PanelOpen());
        tick();
        // Shown, not drawn: the button is not pressable and the handler refuses.
        check(shown(settings_confirm_button) && !confirm_enabled() && !shown(settings_panel_write_button) && !shown(settings_panel_close_button) && !shown(settings_panel_cancel_button) && !shown(settings_panel_open_button));
        {
            bool combos_off=true;
            for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)combos_off=combos_off && shown(combo_ok(camera,item)) && !enabled(combo_ok(camera,item));
            check(combos_off && !CanRestartPreview());
        }
        const auto refusals=settings_write_confirm_refusals;
        const int opens_then=opens->load();
        click(407,settings_confirm_button);
        check(settings_write_confirm_refusals==refusals+1 && opens->load()==opens_then && screen->Step()==D810WriteScreenStep::Confirming && !screen->Status().plan_confirmed);
        // Drawn whole by a present of the window: pressable. The window one row too short: not.
        paint();tick();
        check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());
        check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight-1));tick();
        check(!confirm_enabled());
        paint();
        check(settings_write_drawn_serial==0);
        check(resize(kD810WriteMinClientWidth,kD810WriteMinClientHeight));paint();tick();
        check(settings_write_drawn_serial==screen->ViewSerial() && confirm_enabled());
        // The lines are not cut at the smallest width.
        {
            HDC dc=CreateCompatibleDC(nullptr);int widest=0;
            if(dc){
                const auto width_of=[&](const std::wstring& value){RECT r{0,0,0,0};DrawTextW(dc,value.c_str(),-1,&r,DT_CALCRECT|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);return static_cast<int>(r.right);};
                widest=width_of(view->title);for(const auto& line:view->lines)widest=std::max(widest,width_of(line));
                DeleteDC(dc);
            }
            check(widest>0 && widest<=kD810WriteMinClientWidth-24);
        }
        // Drawn: the confirmation under the panel, which stays in the upper part.
        {
            SettingsWriteSmokeFrame frame(kD810WriteMinClientWidth,kD810WriteMinClientHeight);
            check(frame.Lit(SettingsPanelRowY(1),SettingsPanelRowY(8))>400 && frame.Lit(kD810WriteMinClientHeight/2+88,kD810WriteMinClientHeight/2+130)>50);
        }
        // "やめる": back to the panel with the picks; a plan is made again from the picks.
        click(408,settings_discard_button);tick();
        check(screen->Step()==D810WriteScreenStep::Idle && screen->PanelDirty()==10 && screen->PanelEditable() && !shown(settings_confirm_button) && enabled(settings_panel_write_button));
        screen->TakeChosenReadForTest(read);
        paint();tick();
        check(screen->ConfirmationShown() && confirm_enabled());
        check(!shown(settings_panel_open_button) && ShownSiblingsOverlapping(settings_confirm_button)==0 && ShownSiblingsOverlapping(settings_discard_button)==0 && ShownChildPairsOverlapping(window)==0);
        // Confirmed: the run (with no camera it sends nothing), then the panel is old and the picks are used up.
        const auto refusals_now=settings_write_confirm_refusals;
        click(407,settings_confirm_button);
        check(wait_idle() && settings_write_confirm_refusals==refusals_now && screen->Step()==D810WriteScreenStep::Idle && screen->PanelStale() && screen->PanelDirty()==0 && screen->View() && !screen->View()->confirmation);
        check(screen->View() && screen->View()->title.find(L"何も書いていません")!=std::wstring::npos);
        click(407,settings_confirm_button); // the plan was used
        check(!SettingsWriteBusy() && screen->Step()==D810WriteScreenStep::Idle);
        tick();
        check(shown(settings_panel_close_button) && enabled(settings_panel_close_button) && !enabled(settings_panel_write_button));
        {
            bool combos_off=true;
            for(int camera=0;camera<2;++camera)for(int item=0;item<static_cast<int>(kD810ChoiceItemCount);++item)combos_off=combos_off && !enabled(combo_ok(camera,item));
            check(combos_off);
        }
    }
    // The result of a run that wrote all ten and read them back: seven lines, in the window, drawn.
    {
        D810WriteRun run;
        run.verdict=D810WriteVerdict::Match;run.sent=10;
        for(const auto camera:{D810Camera::B,D810Camera::A})
            for(const auto item:kD810ChoiceItems){
                const auto write_item=D810ChoiceWriteItem(item);
                D810StepOutcome outcome;
                outcome.step={camera,write_item,D810WriteItemOf(write_item).codes[0],100,200};
                outcome.sent=true;outcome.readback=200;outcome.verdict=D810WriteVerdict::Match;
                run.steps.push_back(outcome);
            }
        screen->TakeRunForTest(run,D810PlanSource::Chosen);
        const auto* view=screen->View();
        check(view && !view->confirmation && view->lines.size()==7 && view->lines.size()<=D810WriteViewMaxLines() && D810WriteViewFits(*view,kD810WriteMinClientHeight));
        check(view && view->title.find(L"選んだ値を書きました: 一致（5 項目・10 件")!=std::wstring::npos);
        paint();tick();
        SettingsWriteSmokeFrame frame(kD810WriteMinClientWidth,kD810WriteMinClientHeight);
        check(frame.Lit(kD810WriteMinClientHeight/2+88,kD810WriteMinClientHeight/2+130)>50 && !confirm_enabled());
        check(ShownChildPairsOverlapping(window)==0); // the result: no shown button or drop-down lies over another
    }
    // The widest line that a confirmation of picks or its result can have fits the smallest window.
    {
        HDC dc=CreateCompatibleDC(nullptr);
        std::wstring which;int widest=0;
        if(dc){widest=WidestPossibleChosenLine(dc,which);DeleteDC(dc);}
        std::string which_utf8;
        if(!which.empty()){
            const int bytes=WideCharToMultiByte(CP_UTF8,0,which.c_str(),static_cast<int>(which.size()),nullptr,0,nullptr,nullptr);
            if(bytes>0){which_utf8.assign(static_cast<std::size_t>(bytes),'\0');WideCharToMultiByte(CP_UTF8,0,which.c_str(),static_cast<int>(which.size()),which_utf8.data(),bytes,nullptr,nullptr);}
        }
        Log("settings_write_chosen_widest_possible_line_px="+std::to_string(widest)+" window_line_px="+std::to_string(kD810WriteMinClientWidth-24)+" widest_chars="+std::to_string(which.size())+" widest_text="+which_utf8);
        check(widest>0 && widest<=kD810WriteMinClientWidth-24);
    }

    // 4. Closing the panel gives Live back only when nothing holds capture and nothing has failed.
    {
        const auto picks_ready=[&](bool record_unconfirmed,bool stopped_by_panel=true){
            settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
            if(record_unconfirmed){
                D810SettingsFileJournal journal(folder);
                check(journal.Append("a0sw1 op=00000000000000a1 ev=plan t=2000-01-01T00:00:00Z source=copy steps=1") &&
                    journal.Append("a0sw1 op=00000000000000a1 ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040"));
            }
            InitSettingsWriteWithTable(SmokeChosenTable(),std::make_unique<D810SettingsFileJournal>(folder),factory);
            screen=settings_write->screen.get();
            screen->TakePanelReadForTest(read_of(true,true));
            tick();
            check(SettingsPanelOpen() && enabled(settings_panel_close_button));
            settings_panel_stopped_live=stopped_by_panel; // what the panel's open button sets when it stops Live itself
        };
        const auto closed_without_restart=[&](const wchar_t* said,bool restart_button_named=true){
            const auto before=restarts,auto_before=settings_panel_auto_restarts,manual_before=manual_preview_restarts;
            click(kPanelCloseButtonId,settings_panel_close_button);
            check(!SettingsPanelOpen() && restarts==before && settings_panel_auto_restarts==auto_before && manual_preview_restarts==manual_before && capture_status.find(said)!=std::wstring::npos);
            // The restart button is named only where it can be pressed (not after a failed capture or a camera that could not be closed).
            check((capture_status.find(L"「ライブ再開」で再開できます")!=std::wstring::npos)==restart_button_named);
        };
        // a. The record holds capture (a write with no end line).
        picks_ready(true);
        check(!screen->Status().capture_allowed);
        closed_without_restart(L"撮影を止めているため、ライブは自動では再開しません");
        // b. A capture failed.
        picks_ready(false);
        capture_restart_blocked=true;
        closed_without_restart(L"撮影に失敗したあとのため、ライブは再開できません",false);
        capture_restart_blocked=false;
        // c. The window is closing.
        picks_ready(false);
        closing=true;
        closed_without_restart(L"ライブは止まったままです");
        closing=false;
        // d. A camera could not be closed.
        picks_ready(false);
        settings_close_failed=true;
        closed_without_restart(L"カメラとの接続を閉じられなかったため、ライブは再開できません",false);
        settings_close_failed=false;
        // e. Nothing in the way: it is given back.
        picks_ready(false);
        const auto before=restarts;
        click(kPanelCloseButtonId,settings_panel_close_button);
        check(restarts==before+1 && !SettingsPanelOpen());
        // f. Live was stopped by hand before the panel was opened: closing leaves it stopped.
        picks_ready(false,false);
        closed_without_restart(L"この画面を開く前から止まっていたため、自動では再開しません");
        // g. The last thing the panel did with the cameras did not finish (the read for "N 件を書き込む…" could not complete): closing does not start them.
        picks_ready(false);
        check(screen->PanelPick(D810Camera::B,D810ChoiceItem::Iso,400));
        {D810PlanRead incomplete=read_of(true,true);incomplete.complete=false;screen->TakeChosenReadForTest(incomplete);}
        check(!screen->PanelCamerasWereFine() && SettingsPanelOpen());
        tick();
        {wchar_t named[64]{};GetWindowTextW(settings_panel_close_button,named,64);check(std::wstring(named)==L"設定の画面を閉じる");}
        closed_without_restart(L"ライブは止まったままです");
        // g2. The read for "A の設定を B に写す" with the panel open did not finish: closing does not start the cameras either.
        picks_ready(false);
        {D810PlanRead incomplete=read_of(true,true);incomplete.complete=false;screen->TakePlanReadForTest(incomplete);}
        check(!screen->PanelCamerasWereFine() && SettingsPanelOpen());
        closed_without_restart(L"ライブは止まったままです");
        // g3. A run that was stopped before it sent anything, for a reason, with the panel open: the same.
        picks_ready(false);
        {D810WriteRun stopped;stopped.reason="open_failed";screen->TakeRunForTest(stopped,D810PlanSource::Chosen);}
        check(!screen->PanelCamerasWereFine() && screen->PanelStale());
        closed_without_restart(L"ライブは止まったままです");
        // g4. A run that sent and ended in a result leaves the cameras as the panel found them: closing gives Live back.
        picks_ready(false);
        {D810WriteRun done;done.verdict=D810WriteVerdict::Match;done.sent=1;
            D810StepOutcome outcome;outcome.step={D810Camera::B,D810WriteItem::Iso,0x500f,100,200};outcome.sent=true;outcome.readback=200;outcome.verdict=D810WriteVerdict::Match;done.steps.push_back(outcome);
            screen->TakeRunForTest(done,D810PlanSource::Chosen);}
        check(screen->PanelCamerasWereFine());
        {const auto before_g4=restarts;click(kPanelCloseButtonId,settings_panel_close_button);check(restarts==before_g4+1);}
        // The way out of an open panel is its own button: the restart button does nothing, and neither does "両台の設定を読む".
        picks_ready(false);
        const auto restarts_open=restarts,manual_open=manual_preview_restarts;const auto reads=settings_reads_started;
        click(403,session_button);click(405,settings_button);
        check(restarts==restarts_open && manual_preview_restarts==manual_open && settings_reads_started==reads && SettingsPanelOpen() && !CanRestartPreview());
        tick();
        check(!IsWindowEnabled(session_button) && !IsWindowEnabled(settings_button));
    }

    // 5. The way Live is stopped, by the real button, decides whether closing gives it back; and a value that cannot be picked still shows.
    {
        settings_write.reset();std::filesystem::remove_all(folder,ignored);capture_status.clear();
        InitSettingsWriteWithTable(SmokeChosenTable(),std::make_unique<D810SettingsFileJournal>(folder),factory);
        screen=settings_write->screen.get();
        tick();
        // Live is stopped already (by hand): the button reads the cameras, and the panel holds no Live.
        check(enabled(settings_panel_open_button));
        click(kPanelOpenButtonId,settings_panel_open_button);
        check(!settings_panel_stopped_live && !settings_panel_open_pending && SettingsWriteBusy());
        check(wait_idle() && !SettingsPanelOpen());
        screen->TakePanelReadForTest(read_of(true,true)); // what the read would have given
        tick();
        const auto before=restarts;
        click(kPanelCloseButtonId,settings_panel_close_button);
        check(restarts==before && !SettingsPanelOpen() && capture_status.find(L"この画面を開く前から止まっていたため、自動では再開しません")!=std::wstring::npos);
        // A value that the camera reads and the application cannot pick (a white balance Preset): the cell shows it, and the other values can be picked.
        D810PlanRead preset=read_of(true,true);
        preset.snapshots[0]=SmokeBody(250,800,200,0x8013,3);
        screen->TakePanelReadForTest(preset);
        tick();
        check(selected(0,kWb).find(L"プリセット")!=std::wstring::npos && enabled(combo_ok(0,kWb)) && SmokeComboText(combo_ok(0,kWb),0).find(L"プリセット")!=std::wstring::npos);
        check(choose_value(0,kWb,4) && screen->PanelDirty()==1 && selected(0,kWb).find(L"プリセット")==std::wstring::npos);
        click(kPanelCancelButtonId,settings_panel_cancel_button);
        check(selected(0,kWb).find(L"プリセット")!=std::wstring::npos && screen->PanelDirty()==0); // taken back, the value read shows again
        click(kPanelCloseButtonId,settings_panel_close_button);
    }
    preview_restart_override=saved_override;

    FinishSettingsWrite();
    if(IsWindow(window))DestroyWindow(window);
    settings_write.reset();
    std::filesystem::remove_all(folder,ignored);
    stopping=true;
    Log("settings_write_chosen_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0 camera_opens_attempted="+std::to_string(opens->load()));
    return failures?1:0;
}
#endif
