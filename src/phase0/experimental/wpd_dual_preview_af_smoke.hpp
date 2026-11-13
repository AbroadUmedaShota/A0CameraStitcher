// Camera-free contract of the AF frame display (#426). Included by wpd_dual_preview_ui.hpp after the
// window procedure, inside the anonymous namespace. Synthetic cameras and our own memory bitmap only:
// no Discover/Open/Run, no hardware lease, no camera command.
int RunAfPointUiSmoke(){
    Log("af_point_ui_synthetic=1 camera_operations=0");
    ComScope com; // the requests below are built with WPD objects; declared first, so it ends last
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

    // 6. Clicks (stage 2), through the real window procedure. No transport exists here: a click can only put
    // one request into the camera's mailbox, and the checks look at the mailbox and at what is drawn.
    auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.lpszClassName=L"A0AfPointUiSmoke";
    HWND window=RegisterClassW(&wc)?CreateWindowExW(0,wc.lpszClassName,L"synthetic AF point test",WS_OVERLAPPEDWINDOW,0,0,1400,960,nullptr,nullptr,instance,nullptr):nullptr;
    if(!window){SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return 2;}
    RECT real{};GetClientRect(window,&real);
    const auto fresh=[&]{for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}};
    const auto pane_of=[&](int i){std::lock_guard lock(cameras[i]->mutex);return PanePictureRect(real,i,cameras[i]->pixels);};
    const auto spot=[&](int i,double fx,double fy){const auto p=pane_of(i);return std::pair<int,int>{p.left+static_cast<int>(p.width*fx),p.top+static_cast<int>(p.height*fy)};};
    const auto expected_for=[&](int i,std::pair<int,int> at){
        const auto p=pane_of(i);std::optional<D810AfHeader> frame;{std::lock_guard lock(cameras[i]->mutex);frame=cameras[i]->af.frame;}
        return PreviewAfPointFromClick(PreviewAfRect{p.left,p.top,p.width,p.height},at.first,at.second,*frame);
    };
    const auto click=[&](std::pair<int,int> at){SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(at.first,at.second));};
    const auto drain=[&](int i){while(cameras[i]->focus_mailbox.TakeAfPoint())cameras[i]->focus_mailbox.Complete();};
    const auto status_of=[&](int i){std::lock_guard lock(cameras[i]->mutex);return cameras[i]->focus_status;};
    const auto mark_of=[&](int i){std::lock_guard lock(cameras[i]->mutex);return cameras[i]->af.request;};
    fresh();drain(0);drain(1);
    // 6a. A click in A's picture: one request for A, none for B, the red mark kept, and the status says so.
    const auto first_spot=spot(0,0.5,0.5);const auto first=expected_for(0,first_spot);
    click(first_spot);
    check(first && !cameras[0]->focus_mailbox.Available() && cameras[1]->focus_mailbox.Available());
    {const auto mark=mark_of(0);check(first && mark && mark->x==first->x && mark->y==first->y && mark->width==first->width && mark->height==first->height);}
    check(status_of(0).find(L"受け付け")!=std::wstring::npos && !mark_of(1));
    // 6b. A second click while the first is waiting is dropped: the first request is the one taken, once.
    const auto second_spot=spot(0,0.2,0.3);
    click(second_spot);
    {const auto mark=mark_of(0);check(mark && first && mark->x==first->x && mark->y==first->y);} // the mark did not move
    const auto taken=cameras[0]->focus_mailbox.TakeAfPoint();
    check(taken==first);check(!cameras[0]->focus_mailbox.TakeAfPoint());
    // 6c. While it runs, no click is taken; once it completes, the next one is.
    click(second_spot);check(!cameras[0]->focus_mailbox.TakeAfPoint());
    cameras[0]->focus_mailbox.Complete();
    fresh();click(second_spot);
    check(cameras[0]->focus_mailbox.TakeAfPoint()==expected_for(0,second_spot));cameras[0]->focus_mailbox.Complete();
    // 6d. The red outline is drawn at the requested place beside the green one, then goes after ten seconds.
    {
        const auto ask=*mark_of(0);D810AfHeader asked=reported;asked.center_x=static_cast<int>(ask.x);asked.center_y=static_cast<int>(ask.y);
        const auto picture=picture_of(0);const auto rect=*PreviewAfFrameOnDisplay(asked,PreviewAfRect{picture.left,picture.top,picture.width,picture.height});
        paint();
        check(GetPixel(dc,rect.left,rect.top+rect.height/2)==kAfRequestColor || GetPixel(dc,rect.left+rect.width/2,rect.top)==kAfRequestColor);
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfRequestColor)>=static_cast<unsigned>(rect.width+rect.height));
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfReportedColor)>0);
        const auto other=picture_of(1);
        check(count_color(PixelRect{other.left,other.top,other.width,other.height},kAfRequestColor)==0); // B asked for nothing
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.request->at=Clock::now()-std::chrono::seconds(11);}
        paint();
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfRequestColor)==0);
        // A request made in another coordinate space than the current header is not drawn.
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.request->at=Clock::now();cameras[0]->af.request->width=999;}
        paint();
        check(count_color(PixelRect{picture.left,picture.top,picture.width,picture.height},kAfRequestColor)==0);
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.request.reset();}
    }
    // 6e. B's picture belongs to B.
    fresh();drain(0);
    {const auto at=spot(1,0.7,0.4);click(at);check(!cameras[1]->focus_mailbox.Available() && cameras[0]->focus_mailbox.Available());
        check(cameras[1]->focus_mailbox.TakeAfPoint()==expected_for(1,at));cameras[1]->focus_mailbox.Complete();}
    // 6f. Not a point: the margin around the picture, the other camera's margin, outside the window.
    {
        const auto p=pane_of(0);
        click({p.left-1,p.top+p.height/2});click({p.left+p.width,p.top+p.height/2});click({p.left+p.width/2,p.top-1});click({p.left+p.width/2,p.top+p.height});
        click({-5,-5});click({5000,5000});click({0,0});
        check(cameras[0]->focus_mailbox.Available() && cameras[1]->focus_mailbox.Available() && !mark_of(0));
    }
    // 6g. An unknown header, a stale picture, a stopping or closing window, a capture request and the
    // restart test: none of them takes a click, and the unknown header says why.
    {
        const auto at=spot(0,0.5,0.5);
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame.reset();cameras[0]->af.reject=D810AfReject::ImageSize;}
        click(at);check(cameras[0]->focus_mailbox.Available() && !mark_of(0) && status_of(0).find(L"不明")!=std::wstring::npos);
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame=reported;}
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now()-std::chrono::seconds(3);}
        click(at);check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        fresh();
        stopping=true;click(at);stopping=false;check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        closing=true;click(at);closing=false;check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        capture_pending=0;click(at);capture_pending.reset();check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        camera_end_restart_smoke=true;click(at);camera_end_restart_smoke=false;check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        cameras[0]->failed=true;click(at);cameras[0]->failed=false;check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        cameras[0]->frames=3;click(at);cameras[0]->frames=25;check(cameras[0]->focus_mailbox.Available() && !mark_of(0)); // too early after the first frame
        fresh();click(at);check(!cameras[0]->focus_mailbox.Available() && mark_of(0)); // and the same click is taken once all is well
        drain(0);
    }

    // 7. What a click may not select, and what a click may not write.
    {
        fresh();drain(0);drain(1);{std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.request.reset();}
        const auto p=pane_of(0);
        // 7a. The bottom rows carry the status text: a click on them selects nothing; one row above does.
        click({p.left+p.width/2,p.top+p.height-1});click({p.left+p.width/2,p.top+p.height-kAfStripHeight});
        check(cameras[0]->focus_mailbox.Available() && !mark_of(0));
        click({p.left+p.width/2,p.top+p.height-kAfStripHeight-1});
        check(!cameras[0]->focus_mailbox.Available() && mark_of(0));
        drain(0);{std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.request.reset();}
        // 7b. A camera that cannot take a click gets no status text and no mark, whatever the header says.
        const auto in_picture=spot(0,0.5,0.5);
        cameras[0]->FocusStatus(L"確認用");
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->last=Clock::now()-std::chrono::seconds(3);}
        click(in_picture);check(status_of(0)==L"確認用" && !mark_of(0) && cameras[0]->focus_mailbox.Available());
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame.reset();}
        click(in_picture);check(status_of(0)==L"確認用");
        {std::lock_guard lock(cameras[0]->mutex);cameras[0]->af.frame=reported;}
        fresh();
        // 7c. The click that raises the window is eaten over a picture and only there.
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_picture.first,in_picture.second})==MA_ACTIVATEANDEAT);
        const auto in_b=spot(1,0.5,0.5);
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_b.first,in_b.second})==MA_ACTIVATEANDEAT);
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{p.left-1,p.top+p.height/2})==0);        // the margin
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_picture.first,p.top+p.height-1})==0); // the status text
        check(AfMouseActivate(real,HTCAPTION,WM_LBUTTONDOWN,POINT{in_picture.first,in_picture.second})==0); // the title bar
        check(AfMouseActivate(real,HTCLIENT,WM_RBUTTONDOWN,POINT{in_picture.first,in_picture.second})==0);
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{-4,-4})==0);
        check(SendMessageW(window,WM_MOUSEACTIVATE,reinterpret_cast<WPARAM>(window),MAKELPARAM(HTCAPTION,WM_LBUTTONDOWN))!=MA_ACTIVATEANDEAT); // the title bar raises the window normally
        // Not when the window is already in front (Windows sends the message for every child click too), and not over a child.
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_picture.first,in_picture.second},true,false)==0);
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_picture.first,in_picture.second},false,true)==0);
        check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,POINT{in_picture.first,in_picture.second},false,false)==MA_ACTIVATEANDEAT);
        {
            // A real child control over the picture: found under its own pixels, not elsewhere, and then the click is not eaten.
            HWND child=CreateWindowExW(0,L"BUTTON",L"x",WS_CHILD|WS_VISIBLE,in_picture.first-20,in_picture.second-10,40,20,window,nullptr,instance,nullptr);
            check(child!=nullptr);
            if(child){
                const POINT on_child{in_picture.first,in_picture.second},beside{in_picture.first+60,in_picture.second};
                check(AfChildAt(window,on_child) && !AfChildAt(window,beside));
                check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,on_child,false,AfChildAt(window,on_child))==0);
                check(AfMouseActivate(real,HTCLIENT,WM_LBUTTONDOWN,beside,false,AfChildAt(window,beside))==MA_ACTIVATEANDEAT);
                ShowWindow(child,SW_HIDE); // an invisible child does not count
                check(!AfChildAt(window,on_child));
                DestroyWindow(child);
            }
        }
        // Eaten clicks never reach the handler, so activation alone queues nothing.
        check(cameras[0]->focus_mailbox.Available() && cameras[1]->focus_mailbox.Available());
        // 7d. Too small a window draws no picture and takes no click.
        RECT tiny{0,0,150,200};check(AfPictureAt(tiny,60,100)==-1 && AfMouseActivate(tiny,HTCLIENT,WM_LBUTTONDOWN,POINT{60,100})==0);
    }
    // 8. Camera::UpdateAfFrame: from the payload to the stored frame. A header that cannot be used leaves no frame.
    {
        std::vector<unsigned char> payload(390,0);payload[384]=0xff;payload[385]=0xd8;payload[388]=0xff;payload[389]=0xd9;
        const auto put=[&](size_t offset,int value){payload[offset]=static_cast<unsigned char>((value>>8)&0xff);payload[offset+1]=static_cast<unsigned char>(value&0xff);};
        put(12,1280);put(14,852);put(24,160);put(26,120);put(28,300);put(30,200);payload[48]=2;
        auto& c=*cameras[0];
        c.UpdateAfFrame(payload,640,426);
        {std::lock_guard lock(c.mutex);check(c.af.frame && c.af.frame->center_x==300 && c.af.frame->center_y==200 && c.af.frame->focus_mark==2 &&
            c.af.reject==D810AfReject::None && Clock::now()-c.af.frame_at<std::chrono::seconds(1));}
        // The JPEG's width comes first: a swapped pair does not fit the header's aspect ratio.
        c.UpdateAfFrame(payload,426,640);
        {std::lock_guard lock(c.mutex);check(!c.af.frame && c.af.reject==D810AfReject::AspectMismatch);}
        c.UpdateAfFrame(payload,640,426);
        put(12,0);c.UpdateAfFrame(payload,640,426);
        {std::lock_guard lock(c.mutex);check(!c.af.frame && c.af.reject==D810AfReject::ImageSize);}
        // The frame of an earlier good header is not kept, and a click is told the position is unknown.
        put(12,1280);c.UpdateAfFrame(payload,640,426);
        put(28,5000);c.UpdateAfFrame(payload,640,426);
        {std::lock_guard lock(c.mutex);check(!c.af.frame && c.af.reject==D810AfReject::Center);}
        fresh();drain(0);{std::lock_guard lock(c.mutex);c.af.request.reset();}
        click(spot(0,0.5,0.5));check(c.focus_mailbox.Available() && !mark_of(0) && status_of(0).find(L"不明")!=std::wstring::npos);
        put(28,300);std::vector<unsigned char> broken(payload.begin(),payload.begin()+200); // no envelope
        c.UpdateAfFrame(payload,640,426);c.UpdateAfFrame(broken,640,426);
        {std::lock_guard lock(c.mutex);check(!c.af.frame);}
        c.UpdateAfFrame(payload,640,426);
        {std::lock_guard lock(c.mutex);check(c.af.frame.has_value());c.af.frame=reported;c.af.frame_at=Clock::now();c.af.reject=D810AfReject::None;}
    }
    // 9. The click conversion uses the rectangle Paint really draws the picture into.
    {
        for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->af.request.reset();}
        paint();
        for(int i=0;i<2;++i){
            const auto p=picture_of(i);int left=100000,top=100000,right=-1;
            for(int y=140;y<480;++y)for(int x=i*700;x<(i+1)*700;++x)if(GetPixel(dc,x,y)==RGB(100,100,100)){left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);}
            check(left==p.left && right==p.left+p.width-1 && top==p.top);
            // The rows under the picture's strip are the last gray ones; below the rectangle nothing is gray.
            bool gray_below=false;for(int x=p.left;x<p.left+p.width;++x)for(int y=p.top+p.height;y<p.top+p.height+4;++y)if(GetPixel(dc,x,y)==RGB(100,100,100))gray_below=true;
            check(!gray_below);
        }
    }
    // 10. The middle of the drawn green frame, clicked, asks for the header's centre; the red outline of a far,
    // asymmetric request is drawn at (x, y) and not at (y, x).
    {
        auto& b=*cameras[1];
        fresh();drain(1);{std::lock_guard lock(b.mutex);b.af.request.reset();b.af.frame=reported;b.af.frame_at=Clock::now();}
        const auto pb=pane_of(1);const auto green=*PreviewAfFrameOnDisplay(reported,PreviewAfRect{pb.left,pb.top,pb.width,pb.height});
        click({green.left+green.width/2,green.top+green.height/2});
        const auto got=b.focus_mailbox.TakeAfPoint();
        check(got && std::abs(static_cast<int>(got->x)-reported.center_x)<=3 && std::abs(static_cast<int>(got->y)-reported.center_y)<=3);
        b.focus_mailbox.Complete();
        auto& a=*cameras[0];
        fresh();drain(0);{std::lock_guard lock(a.mutex);a.af.request.reset();}
        click(spot(0,0.15,0.8));
        const auto ask=mark_of(0);
        check(ask && std::abs(static_cast<int>(ask->x)-static_cast<int>(ask->y))>300);
        if(ask){
            D810AfHeader asked=reported;asked.center_x=static_cast<int>(ask->x);asked.center_y=static_cast<int>(ask->y);
            const auto pa=picture_of(0);const auto rect=*PreviewAfFrameOnDisplay(asked,PreviewAfRect{pa.left,pa.top,pa.width,pa.height});
            paint();
            check(GetPixel(dc,rect.left,rect.top+rect.height/2)==kAfRequestColor && GetPixel(dc,rect.left+rect.width/2,rect.top)==kAfRequestColor);
        }
        drain(0);{std::lock_guard lock(a.mutex);a.af.request.reset();}
    }
    // 11. Camera::DispatchAfPoint refuses before it touches any device: a session this process does not own, a
    // stop request, the deadline, bounds older than two seconds, no usable header. (No device exists here; a
    // refusal that went missing would fail on the null device.)
    {
        auto& c=*cameras[0];
        const auto request=BuildPreviewAfPointRequest(10,10);
        const auto refused=[&]{bool threw=false;try{c.DispatchAfPoint(request.Get());}catch(const std::runtime_error&){threw=true;}return threw;};
        {std::lock_guard lock(c.mutex);c.af.frame=reported;c.af.frame_at=Clock::now();}
        c.send_deadline=Clock::now()+std::chrono::seconds(10);stopping=false;
        c.opened=false;c.closed=false;check(refused());                                   // not owned
        c.opened=true;c.closed=true;check(refused());                                     // already closed
        c.opened=true;c.closed=false;stopping=true;check(refused());stopping=false;c.admission_cancelled=false; // stop requested
        c.send_deadline=Clock::now()-std::chrono::seconds(1);check(refused());           // deadline passed
        c.send_deadline=Clock::now()+std::chrono::seconds(10);
        {std::lock_guard lock(c.mutex);c.af.frame_at=Clock::now()-std::chrono::seconds(3);}check(refused()); // bounds too old
        {std::lock_guard lock(c.mutex);c.af.frame.reset();c.af.frame_at=Clock::now();}check(refused());      // no usable header
        c.opened=false;{std::lock_guard lock(c.mutex);c.af.frame=reported;c.af.frame_at=Clock::now();}
    }
    // 6h. A closed session takes nothing, and a click on a mailbox that was disabled stays dropped.
    {std::lock_guard lock(cameras[1]->mutex);cameras[1]->af.request.reset();}
    cameras[1]->focus_mailbox.Disable();fresh();click(spot(1,0.5,0.5));check(!cameras[1]->focus_mailbox.TakeAfPoint() && !mark_of(1));
    DestroyWindow(window);
    // 12. The picture rectangles and the rest of the window (#417, #427). The pictures stand in the upper half,
    // from the top of the picture area to 8 pixels above the middle. The rows #417 added (the state rows, C and
    // its two rows) and the whole settings table (#427: 16 rows, one block or two) stand below the middle, and in
    // a build with the settings write path so does the row of buttons. Nothing of them reaches a picture, so a
    // click on a picture is never a click on them and the AF frame is never drawn over them.
    {
        for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->af.request.reset();}
        unsigned tables_one_block{},tables_two_blocks{};
        for(const SIZE size:{SIZE{1400,960},SIZE{1280,800},SIZE{1000,700},SIZE{1920,1080},SIZE{1100,500},SIZE{800,560}}){
            const RECT area{0,0,size.cx,size.cy};const int middle=area.bottom/2;
            const auto layout=LayoutSettingsTableIn(16,area.bottom,SettingsButtonRowReserve());
            (layout.blocks==1?tables_one_block:tables_two_blocks)+=1;
            for(int i=0;i<2;++i){
                PixelRect p;{std::lock_guard lock(cameras[i]->mutex);p=PanePictureRect(area,i,cameras[i]->pixels);}
                check(p.width>0 && p.height>0);
                const int bottom=p.top+p.height;
                check(p.top>=174+PreviewTopShift(area.right) && bottom<=middle-8);        // the upper half only, under the rows of buttons of the pane (#437)
                check(bottom<=middle+4);                                                // above the status text
                check(bottom<=middle+kLowerButtonsTop);                                 // above the buttons of C
                check(bottom<=SettingsTableFirstRowY(area.bottom));                     // above the first row of the settings table
                check(SettingsTableBottom(16,area.bottom,SettingsButtonRowReserve())>=SettingsTableFirstRowY(area.bottom));
                check(bottom<=middle+140+kLiveRowsHeight);                              // above C and its two rows
                check(p.left>=i*(area.right/2)+12 && p.left+p.width<=(i+1)*(area.right/2)-12); // inside its own pane
#if defined(A0_ENABLE_WPD_SETTINGS_WRITE)
                check(bottom<=area.bottom-kD810ButtonRowGap);                           // above the row of the settings buttons
#endif
                // The same rectangle answers a click: inside it the camera, outside it (below, in the table) nothing.
                const int cx=p.left+p.width/2,cy=p.top+(p.height-kAfStripHeight)/2;
                check(AfPictureAt(area,cx,cy)==i);
                check(AfPictureAt(area,cx,bottom)==-1 && AfPictureAt(area,cx,middle+90)==-1 && AfPictureAt(area,cx,SettingsTableFirstRowY(area.bottom)+4)==-1);
            }
        }
        check(tables_one_block>0 && tables_two_blocks>0); // both table layouts were covered
        // What is drawn: the green and red outlines and the strip of text never leave the pictures. Paint is run on
        // the full-size bitmap; every green or red pixel of the window lies inside a picture.
        for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->af.request=PreviewAfMark{200,150,1280,852,Clock::now()};camera->af.frame=reported;}
        paint();
        // Counted straight from the bitmap's own pixels (a GetPixel call per pixel of the whole window is slow).
        const auto* surface=static_cast<const std::uint32_t*>(bits);
        const auto count_all=[&](COLORREF color,const PixelRect* inside){
            const std::uint32_t want=(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);unsigned count{};
            for(int y=0;y<client.bottom;++y)for(int x=0;x<client.right;++x){
                if(inside && !(x>=inside->left && x<inside->left+inside->width && y>=inside->top && y<inside->top+inside->height))continue;
                if((surface[static_cast<std::size_t>(y)*client.right+x]&0xffffffu)==want)++count;
            }
            return count;
        };
        for(const COLORREF color:{kAfReportedColor,kAfRequestColor}){
            unsigned in_pictures{};for(int i=0;i<2;++i){const auto p=picture_of(i);in_pictures+=count_all(color,&p);}
            check(in_pictures>0 && count_all(color,nullptr)==in_pictures);
        }
        for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->af.request.reset();}
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);stopping=true;
    CheckButtonLayouts([&](bool ok){check(ok);}); // #437: the buttons at the width of a narrow screen and at a wide one
    Log("af_point_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
// The settings panel (#416) against the AF frame (#426). Only in the confirmation test build, the one build with a
// controller that takes a table and can be put into the panel state without a camera (the preview itself carries none
// of this). Synthetic cameras, a temporary record folder and our own memory bitmap: no Discover/Open/Run, no lease.
#if defined(A0_SETTINGS_WRITE_TEST_TABLES)
int RunAfPanelUiSmoke(){
    Log("af_point_panel_ui_synthetic=1 camera_operations=0");
    ComScope com;
    unsigned checks{},failures{};const auto check=[&](bool ok){++checks;if(!ok){++failures;Log("af_point_panel_ui_failed_check="+std::to_string(checks));}};
    const auto gray=[]{Pixels p{640,426,{}};p.bytes.assign(static_cast<size_t>(p.width)*p.height*4,100);return p;};
    D810AfHeader reported;reported.live_width=640;reported.live_height=426;reported.image_width=1280;reported.image_height=852;
    reported.frame_width=160;reported.frame_height=120;reported.center_x=640;reported.center_y=426;reported.focus_mark=2;
    // 13. The settings panel (#416) takes the place of both Live areas. While it is up no picture is selectable
    // (so the click that raises the window is not eaten and no click is read as a click on a picture) and no AF
    // frame is drawn over it. It is the condition CanFocus refuses on. The panel is opened on a synthetic read:
    // no camera, no lease. The same points answer again once it is closed.
    {
        settings_write_min_size_enabled=false;
        wchar_t temp[MAX_PATH]{};
        const auto folder=std::filesystem::path(GetTempPathW(MAX_PATH,temp)?temp:L".")/(L"a0-af-panel-smoke-"+std::to_wstring(GetCurrentProcessId()));
        std::error_code ignored;std::filesystem::remove_all(folder,ignored);
        HINSTANCE panel_instance{};
        HWND panel_window=CreateSettingsWriteSmokeWindow(L"A0AfPanelUiSmoke",panel_instance); // makes two new synthetic cameras
        check(panel_window!=nullptr);
        if(panel_window){
            ShowWindow(panel_window,SW_SHOWNOACTIVATE);
            for(auto& camera:cameras){camera->done=true;camera->stopped=true;camera->closed=true;camera->pixels=gray();camera->af.frame=reported;camera->af.frame_at=Clock::now();}
            preview_joined=true; // both Live sessions joined, as the panel needs
            unsigned restarts{};const auto saved_override=preview_restart_override;preview_restart_override=[&]{++restarts;};
            auto opens=std::make_shared<std::atomic<int>>(0);
            InitSettingsWrite(std::make_unique<D810SettingsFileJournal>(folder),[opens]{return std::make_unique<NoCameraSettingsAccess>(opens);});
            auto* screen=settings_write->screen.get();
            const auto tick=[&]{SendMessageW(panel_window,WM_TIMER,1,0);};
            RECT area{};GetClientRect(panel_window,&area);
            const auto centre=[&](int i){PixelRect p;{std::lock_guard lock(cameras[i]->mutex);p=PanePictureRect(area,i,cameras[i]->pixels);}
                return POINT{p.left+p.width/2,p.top+(p.height-kAfStripHeight)/2};};
            const auto fresh_cameras=[&]{for(const auto& camera:cameras){std::lock_guard lock(camera->mutex);camera->last=Clock::now();++camera->frames;}};
            // The window's own Paint into a memory bitmap: how many pixels of the reported-frame colour lie in the two picture rectangles.
            const auto green_in_pictures=[&]{
                HDC memory=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;info.bmiHeader.biHeight=-area.bottom;
                info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* raw{};HBITMAP surface=memory?CreateDIBSection(memory,&info,DIB_RGB_COLORS,&raw,nullptr,0):nullptr;
                unsigned green{};
                if(memory && surface){
                    auto before=SelectObject(memory,surface);SetBkMode(memory,TRANSPARENT);
                    stopping=false;fresh_cameras();Paint(memory,area);GdiFlush();stopping=true; // pictures are drawn only while Live is not stopping
                    for(int i=0;i<2;++i){PixelRect p;{std::lock_guard lock(cameras[i]->mutex);p=PanePictureRect(area,i,cameras[i]->pixels);}
                        for(int y=p.top;y<p.top+p.height;++y)for(int x=p.left;x<p.left+p.width;++x)if(GetPixel(memory,x,y)==kAfReportedColor)++green;}
                    SelectObject(memory,before);
                }
                if(surface)DeleteObject(surface);
                if(memory)DeleteDC(memory);
                return green;
            };
            stopping=true;fresh_cameras(); // as in the settings write smokes: both Live sessions joined
            // Closed: the points select the cameras, the activating click is eaten, the frame is drawn.
            check(!SettingsPanelOpen());
            check(AfPictureAt(area,centre(0).x,centre(0).y)==0 && AfPictureAt(area,centre(1).x,centre(1).y)==1);
            check(AfMouseActivate(area,HTCLIENT,WM_LBUTTONDOWN,centre(0))==MA_ACTIVATEANDEAT);
            check(green_in_pictures()>0);
            // Open: the same points select nothing, the activating click is not eaten, no frame is drawn over the panel.
            D810PlanRead read;read.complete=true;read.verified={true,true};
            read.snapshots[0]=SmokeBody(250,800,200,4,3);read.snapshots[1]=SmokeBody(500,560,100,2,2);
            screen->TakePanelReadForTest(read);tick();
            check(SettingsPanelOpen() && AfPanelCoversPictures());
            for(int i=0;i<2;++i){
                const auto at=centre(i);
                check(AfPictureAt(area,at.x,at.y)==-1);
                check(AfMouseActivate(area,HTCLIENT,WM_LBUTTONDOWN,at)==0);
            }
            check(green_in_pictures()==0);
            // A click that reaches the window procedure while the panel is up queues nothing and writes nothing.
            for(int i=0;i<2;++i){const auto at=centre(i);SendMessageW(panel_window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(at.x,at.y));}
            for(int i=0;i<2;++i){std::lock_guard lock(cameras[i]->mutex);check(!cameras[i]->af.request && !cameras[i]->focus_mailbox.TakeAfPoint());}
            // Closed again: the points answer as before. (Live was joined before the panel opened, so nothing restarts.)
            SendMessageW(panel_window,WM_COMMAND,MAKEWPARAM(kPanelCloseButtonId,BN_CLICKED),reinterpret_cast<LPARAM>(settings_panel_close_button));tick();
            check(!SettingsPanelOpen() && !AfPanelCoversPictures() && restarts==0);
            check(AfPictureAt(area,centre(0).x,centre(0).y)==0 && AfPictureAt(area,centre(1).x,centre(1).y)==1);
            check(green_in_pictures()>0);
            preview_restart_override=saved_override;
            FinishSettingsWrite();
            DestroyWindow(panel_window);
            settings_write.reset();
            std::filesystem::remove_all(folder,ignored);
            capture_status.clear();
        }
    }
    Log("af_point_panel_ui_checks="+std::to_string(checks)+" failures="+std::to_string(failures)+" camera_operations=0");return failures?1:0;
}
#endif
