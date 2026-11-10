// Private Win32 part of the camera settings write (#416 stage B, ADR-0038) in the development preview.
// Included by wpd_dual_preview_ui.hpp only when the preview is built with A0_BUILD_WPD_SETTINGS_WRITE
// (A0_ENABLE_WPD_SETTINGS_WRITE). The decisions (what each button may do, what is shown) are in
// D810SettingsWriteScreen; this file opens the cameras, draws and hooks the buttons and the capture admission.
//
// The controller comes from the product entry point only: this build has no constructor that takes a
// table, and the fixed product table decides every row (0 rows enabled until a reviewed commit).
static_assert(!kD810ControllerTestTables, "the preview never builds the contract tests' table constructor");
static_assert(std::is_constructible_v<D810SettingsWriteController, D810SettingsJournal&, D810CameraOwnership&>, "the product entry point");
static_assert(!std::is_constructible_v<D810SettingsWriteController, const D810WriteTable&, D810SettingsJournal&, D810CameraOwnership&>,
    "the preview cannot hand the controller a table");

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
// A confirmation is offered only when every one of its lines is drawn in the window as it is now (fail closed:
// no client area, no confirm button).
bool SettingsWriteViewFits(HWND window){
    const auto* view=settings_write?settings_write->screen->View():nullptr;
    if(!view || !view->confirmation)return true;
    RECT rect{};
    return window && GetClientRect(window,&rect) && D810WriteViewFits(*view,rect.bottom);
}
bool settings_write_wait_shown=false;
// The window was asked to close while a write or reread still runs: it stays until the worker has closed its camera
// (the operator must not unplug or switch off the body meanwhile), and says so.
bool SettingsWriteHoldsWindow(HWND window){
    if(!SettingsWriteBusy())return false;
    if(!settings_write_wait_shown){
        settings_write_wait_shown=true;
        capture_status=L"設定の書き込み・読み直しの終わりを待っています。終わるまで USB を抜かず、本体の電源を切らないでください。";
        SetWindowTextW(window,L"A0 - 設定の書き込みの終わりを待っています（USB を抜かないでください）");
    }
    return true;
}
void DiscardSettingsWriteView(){if(settings_write)settings_write->screen->ClearView();}
void InitSettingsWrite(std::unique_ptr<D810SettingsJournal> journal,D810SettingsWriteScreen::AccessFactory access){
    auto host=std::make_unique<SettingsWriteHost>();
    host->journal=journal?std::move(journal):std::make_unique<UnavailableSettingsWriteJournal>();
    host->controller=std::make_unique<D810SettingsWriteController>(*host->journal,host->owner);
    D810SettingsWriteScreen::Options options;options.log=[](const std::string& line){Log(line);};
    host->screen=std::make_unique<D810SettingsWriteScreen>(*host->controller,std::move(access),std::move(options));
    const auto& status=host->screen->Status();
    Log("settings_write_screen=1 enabled_rows="+std::to_string(status.enabled_rows)+" capture_block="+D810CaptureBlockToken(status.block)+
        " capture_allowed="+std::to_string(status.capture_allowed)+" setting_writes=0");
    // A block left by an earlier run (not confirmed, mismatch, unknown) holds capture from the start.
    if(!status.capture_allowed)capture_status=D810CaptureBlockText(status.block);
    settings_write=std::move(host);
}
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
    const int y=std::max(0L,rect.bottom-40);
    if(settings_copy_button)MoveWindow(settings_copy_button,12,y,230,28,TRUE);
    if(settings_reread_button)MoveWindow(settings_reread_button,252,y,170,28,TRUE);
    if(settings_confirm_button)MoveWindow(settings_confirm_button,432,y,200,28,TRUE);
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
        if(!SettingsWriteViewFits(GetParent(control))){capture_status=L"確認の内容が窓に入りません。窓を大きくしてください。何も書いていません。";return true;}
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
    ShowSettingsWriteButton(settings_confirm_button,shown && confirming,screen.ConfirmOffered(ready) && SettingsWriteViewFits(GetParent(settings_confirm_button)));
    ShowSettingsWriteButton(settings_discard_button,shown && confirming,confirming && !screen.Busy());
    if(screen.Busy() || !screen.Status().capture_allowed){
        for(auto button:capture_buttons)if(button)EnableWindow(button,FALSE);
        if(pair_button)EnableWindow(pair_button,FALSE);
    }
}
// Drawn while both Live sessions are stopped and joined: the line above the buttons, and the confirmation or
// the last result in place of C (it takes the place of the read-only table while it is shown).
bool DrawSettingsWrite(HDC dc,RECT client){
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
    const int top=client.bottom/2+88;
    SetTextColor(dc,view->confirmation?RGB(255,200,90):RGB(240,240,240));
    TextOutW(dc,12,top,view->title.c_str(),static_cast<int>(view->title.size()));
    SetTextColor(dc,RGB(240,240,240));
    int y=top+30;
    const auto visible=std::min(view->lines.size(),D810WriteViewVisibleLines(client.bottom));
    for(std::size_t i=0;i<visible;++i){
        RECT area{12,y,client.right-12,y+22};DrawTextW(dc,view->lines[i].c_str(),-1,&area,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);y+=22;
    }
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
int RunSettingsWriteUiSmoke(){
    Log("settings_write_ui_synthetic=1 camera_operations=0");
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("settings_write_ui_failed_check="+std::to_string(checks));}};
    wchar_t temp[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,temp))return 2;
    const auto folder=std::filesystem::path(temp)/(L"a0-settings-write-ui-smoke-"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;std::filesystem::remove_all(folder,ignored);
    if(const auto product=ProductD810SettingsWriteFolder())check(folder.parent_path()!=product->parent_path() && folder!=*product); // not the product's record
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.opened=true;c.start_attempted=true;c.started=true;
        c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();}
    stopping=false;run_seconds=0;run_start=Clock::now();preview_joined=false;
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0SettingsWriteUiSmoke";
    if(!RegisterClassW(&wc))return 2;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"synthetic settings write test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr);
    if(!window)return 2;
    session_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(403),instance,nullptr);
    settings_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(405),instance,nullptr);
    for(int i=0;i<2;++i)capture_buttons[i]=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(200+i)),instance,nullptr);
    pair_button=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD,0,0,200,28,window,reinterpret_cast<HMENU>(400),instance,nullptr);
    if(!session_button || !settings_button || !capture_buttons[0] || !capture_buttons[1] || !pair_button){DestroyWindow(window);return 2;}
    try{CreateSettingsWriteControls(window,instance);}catch(const std::exception&){DestroyWindow(window);return 2;}
    auto opens=std::make_shared<std::atomic<int>>(0);
    const auto factory=[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);};
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};
    const auto click=[&](int id,HWND button){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));};
    const auto stop_live=[&]{for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;}preview_joined=true;stopping=true;};
    const auto run_live=[&]{stopping=false;preview_joined=false;for(auto& camera:cameras){camera->done=false;camera->stopped=false;camera->closed=false;camera->last=Clock::now();}};
    const auto wait_idle=[&]{for(int i=0;i<400;++i){tick();if(!SettingsWriteBusy())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}return false;};
    const auto drawn_in=[&](int top,int bottom){
        HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1400;info.bmiHeader.biHeight=-960;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        unsigned lit{};
        if(dc && bitmap){auto old=SelectObject(dc,bitmap);Paint(dc,RECT{0,0,1400,960});GdiFlush();
            for(int y=top;y<bottom;++y)for(int x=12;x<1000;++x)if(GetPixel(dc,x,y)!=RGB(0,0,0))++lit;SelectObject(dc,old);}
        if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return lit;
    };
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
    capture_once_alias=0;check(!CaptureOnceHeld());
    all_sessions_passed=false;check(!CaptureOnceHeld());
    all_sessions_passed=passed_before;capture_once_alias.reset();

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
    review_pixels={};

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
