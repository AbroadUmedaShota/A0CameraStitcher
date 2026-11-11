// Camera-free contract of the AF frame display (#426). Included by wpd_dual_preview_ui.hpp after the
// window procedure, inside the anonymous namespace. Synthetic cameras and our own memory bitmap only:
// no Discover/Open/Run, no hardware lease, no camera command.
int RunAfPointUiSmoke(){
    Log("af_point_ui_synthetic=1 camera_operations=0");
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("af_point_ui_failed_check="+std::to_string(checks));}};
    const auto gray=[]{Pixels p{640,426,{}};p.bytes.assign(static_cast<size_t>(p.width)*p.height*4,100);return p;};
    D810AfHeader reported;reported.live_width=640;reported.live_height=426;reported.image_width=1280;reported.image_height=852;
    reported.frame_width=160;reported.frame_height=120;reported.center_x=640;reported.center_y=426;reported.focus_mark=2;
    for(int i=0;i<2;++i){cameras[i]=std::make_unique<Camera>();auto& c=*cameras[i];
        c.alias=i==0?"CAM-A":"CAM-B";c.pixels=gray();c.frames=25;c.painted=15;c.first=Clock::now()-std::chrono::seconds(3);c.last=Clock::now();
        c.af.frame=reported;c.af.frame_at=Clock::now();c.af.reject=D810AfReject::None;}
    stopping=false;
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=1400;info.bmiHeader.biHeight=-960;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc || !bitmap)return 2;
    auto old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
    const RECT client{0,0,1400,960};
    const auto count_color=[&](const PixelRect& area,COLORREF color){
        unsigned count{};
        for(int y=area.top;y<area.top+area.height;++y)for(int x=area.left;x<area.left+area.width;++x)if(GetPixel(dc,x,y)==color)++count;
        return count;
    };
    const auto picture_of=[&](int i){std::lock_guard lock(cameras[i]->mutex);return PanePictureRect(client,i,cameras[i]->pixels);};
    const auto frame_on_display=[&](int i){
        const auto picture=picture_of(i);
        return *PreviewAfFrameOnDisplay(reported,PreviewAfRect{picture.left,picture.top,picture.width,picture.height});
    };
    const auto paint=[&]{
        for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}
        Paint(dc,client);GdiFlush();
    };
    Paint(dc,client);GdiFlush(); // realize the fallback font before the leak check
    const auto resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);

    // 1. A usable frame: a green outline at the place the header says, in each pane, and not filled.
    paint();
    for(int i=0;i<2;++i){
        const auto picture=picture_of(i);
        check(picture.width>0 && picture.height>0 && picture.left>=i*700+12 && picture.left+picture.width<=(i+1)*700-12);
        const auto rect=frame_on_display(i);
        check(rect.width>10 && rect.height>10 && rect.left>=picture.left && rect.top>=picture.top);
        check(GetPixel(dc,rect.left,rect.top+rect.height/2)==kAfReportedColor);                  // left edge
        check(GetPixel(dc,rect.left+rect.width/2,rect.top)==kAfReportedColor);                   // top edge
        check(GetPixel(dc,rect.left+rect.width/2,rect.top+rect.height/2)!=kAfReportedColor);     // the inside is the picture
        const PixelRect outline{rect.left-1,rect.top-1,rect.width+3,rect.height+3};
        check(count_color(outline,kAfReportedColor)>=static_cast<unsigned>(2*(rect.width+rect.height)));
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfRequestColor)==0); // nothing was requested
    }
    // 2. The text under the picture is the body's report and names no optical focus.
    {
        std::lock_guard lock(cameras[0]->mutex);
        const auto text=AfStatusText(*cameras[0]);
        check(text.find(L"本体の報告")!=std::wstring::npos && text.find(L"中心 640,426")!=std::wstring::npos && text.find(L"合焦の印: あり")!=std::wstring::npos);
        check(text.find(L"ピントが合")==std::wstring::npos);
    }
    {
        std::lock_guard lock(cameras[1]->mutex);cameras[1]->af.frame->focus_mark=1;
        check(AfStatusText(*cameras[1]).find(L"合焦の印: なし")!=std::wstring::npos);
        cameras[1]->af.frame->focus_mark=2;
    }
    // 3. An unknown frame draws no outline and says why; one never received says so.
    {
        const auto rect=frame_on_display(0);
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame.reset();cameras[0]->af.reject=D810AfReject::AspectMismatch;}
        paint();
        check(GetPixel(dc,rect.left,rect.top+rect.height/2)!=kAfReportedColor);
        const auto picture=picture_of(0);
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfReportedColor)==0);
        {std::lock_guard lock(cameras[0]->mutex);check(AfStatusText(*cameras[0]).find(L"AF枠: 不明")!=std::wstring::npos);
            cameras[0]->af.frame_at={};check(AfStatusText(*cameras[0]).find(L"未取得")!=std::wstring::npos);}
        // Camera B is unaffected.
        check(GetPixel(dc,frame_on_display(1).left,frame_on_display(1).top+frame_on_display(1).height/2)==kAfReportedColor);
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame=reported;cameras[0]->af.frame_at=Clock::now();cameras[0]->af.reject=D810AfReject::None;}
    }
    // 4. No picture is drawn when the frame is stale or Live is stopping; then no outline either.
    {
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now()-std::chrono::seconds(3);}
        Paint(dc,client);GdiFlush();
        const auto picture=picture_of(0);
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfReportedColor)==0);
        stopping=true;paint();
        check(count_color(PixelRect{picture_of(1).left,picture_of(1).top,picture_of(1).width,picture_of(1).height},kAfReportedColor)==0);
        stopping=false;
    }
    // 5. Drawing again and again does not leak GDI objects.
    for(int i=0;i<100;++i)paint();
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==resources);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);stopping=true;
    Log("af_point_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
