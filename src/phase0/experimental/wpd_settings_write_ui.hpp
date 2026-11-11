// Private Win32 part of the camera settings write (#416 stage B, ADR-0038) in the development preview.
// Included by wpd_dual_preview_ui.hpp only when the preview is built with A0_BUILD_WPD_SETTINGS_WRITE
// (A0_ENABLE_WPD_SETTINGS_WRITE). The decisions (what each button may do, what is shown) are in
// D810SettingsWriteScreen; this file opens the cameras, draws and hooks the buttons and the capture admission.
//
// The controller comes from the product entry point only: this build has no constructor that takes a
// table, and the fixed product table decides every row (0 rows enabled until a reviewed commit).
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
// How long the closing window waits for a worker that does not end before it says what to do (longer than the write's
// 60 s deadline plus the watchdog's 12 s). There is no button that closes the window sooner: the lease would then
// go before the worker's camera.
std::chrono::milliseconds settings_write_stuck_limit{90000};
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
void DiscardSettingsWriteView(){if(settings_write)settings_write->screen->ClearView();}
// The host with its controller made: the screen on top of it, the first status line, the capture hold.
void InstallSettingsWriteHost(std::unique_ptr<SettingsWriteHost> host,D810SettingsWriteScreen::AccessFactory access){
    D810SettingsWriteScreen::Options options;options.log=[](const std::string& line){Log(line);};
    host->screen=std::make_unique<D810SettingsWriteScreen>(*host->controller,std::move(access),std::move(options));
    const auto& status=host->screen->Status();
    Log("settings_write_screen=1 enabled_rows="+std::to_string(status.enabled_rows)+" capture_block="+D810CaptureBlockToken(status.block)+
        " capture_allowed="+std::to_string(status.capture_allowed)+" setting_writes=0");
    // A block left by an earlier run (not confirmed, mismatch, unknown) holds capture from the start.
    if(!status.capture_allowed)capture_status=D810CaptureBlockText(status.block);
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
    if(!settings_write || code!=BN_CLICKED)return false;
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
        if(screen.ConfirmShownPlan(ready))capture_status=L"CAM-B に書いています（項目ごとに1回だけ）…";
        else capture_status=screen.StatusLine().empty()?screen.OfferText(ready):screen.StatusLine();
        return true;
    }
    if(id==408 && control==settings_discard_button){screen.DiscardPlan();capture_status=screen.StatusLine();return true;}
    if(id==409 && control==settings_reread_button){
        if(screen.RequestReread(ready)){review_visible=false;capture_status=screen.OfferText(ready);}
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
    const bool confirming=screen.Step()==D810WriteScreenStep::Confirming;
    ShowSettingsWriteButton(settings_copy_button,shown,screen.CopyOffered(ready));
    ShowSettingsWriteButton(settings_reread_button,shown,screen.RereadOffered(ready));
    ShowSettingsWriteButton(settings_confirm_button,shown && confirming,screen.ConfirmOffered(ready) && SettingsWriteConfirmPressable(GetParent(settings_confirm_button)));
    ShowSettingsWriteButton(settings_discard_button,shown && confirming,confirming && !screen.Busy());
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
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("settings_write_ui_failed_check="+std::to_string(checks));}};
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

    // 1. The product table and an empty record folder: nothing is enabled, capture is free.
    InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),factory);
    auto* screen=settings_write->screen.get();
    check(screen->Status().enabled_rows==0 && !screen->Status().write_allowed && screen->Status().capture_allowed);
    tick();
    check(!SettingsWriteButtonShown(settings_copy_button) && !IsWindowEnabled(settings_copy_button)); // Live runs: not shown
    stop_live();tick();
    check(CanReadSettings() && SettingsWriteButtonShown(settings_copy_button) && !IsWindowEnabled(settings_copy_button));
    check(IsWindowEnabled(settings_reread_button)!=FALSE && !SettingsWriteButtonShown(settings_confirm_button) && !SettingsWriteButtonShown(settings_discard_button));
    const auto offer=screen->OfferText(true);
    check(offer.find(L"押せません")!=std::wstring::npos && offer.find(L"番号（code）")!=std::wstring::npos && offer.find(L"確かめていない")!=std::wstring::npos);
    click(406,settings_copy_button);click(407,settings_confirm_button); // a direct command does nothing either
    check(!SettingsWriteBusy() && opens->load()==0 && screen->Step()==D810WriteScreenStep::Idle && !screen->Status().plan_pending && !screen->Status().plan_confirmed);
    check(capture_status.find(L"押せません")!=std::wstring::npos);
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
    return table;
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

    // 9. Only CAM-B's ISO row is enabled while CAM-A's white balance is on Preset and its metering differs: the shape of a
    //    first write. The window is its smallest size. White balance and metering are left out with the table's short
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
#endif
