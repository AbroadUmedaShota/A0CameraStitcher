// AF frame overlay of the development preview (#426). Included by wpd_dual_preview_ui.hpp before Paint,
// inside the anonymous namespace: it uses Camera, Pixels, PixelRect and FitPixels. Drawing only.
//
// Green: the AF frame the body reports in the frame header. The text under the picture says so and says
// that the mark is the body's report, not an optical focus decision.
inline constexpr COLORREF kAfReportedColor=RGB(0,230,0);
inline constexpr COLORREF kAfRequestColor=RGB(255,60,60);
// Red: the position of the last accepted click, drawn for this long so that the move can be compared with
// the green frame the body reports afterwards.
inline constexpr auto kAfRequestShown=std::chrono::seconds(10);
// The request is shown only while the header it was made in is still the current one.
bool AfRequestVisible(const Camera& c,Clock::time_point now){
    if(!c.af.frame || !c.af.request)return false;
    const auto& mark=*c.af.request;
    return now>=mark.at && now-mark.at<kAfRequestShown && mark.width==static_cast<unsigned>(c.af.frame->image_width) &&
        mark.height==static_cast<unsigned>(c.af.frame->image_height);
}
// The bottom rows of a picture carry the status text. They look like a part of the display, not of the
// scene, so a click on them selects nothing.
inline constexpr int kAfStripHeight=22;
// Where Paint puts the picture of camera `index`. Paint calls DrawPixels with the same numbers; the
// click conversion uses this function so that a click and the drawing never disagree.
PixelRect PanePictureRect(const RECT& client,int index,const Pixels& pixels){
    const int pane=client.right/2,left=index*pane+12,top=174;
    return FitPixels(pixels,left,top,pane-24,client.bottom/2-top-8);
}
const wchar_t* AfRejectText(D810AfReject reason){
    switch(reason){
    case D810AfReject::ImageSize:return L"画像サイズ";
    case D810AfReject::AspectMismatch:return L"縦横比がJPEGと合わない";
    case D810AfReject::FrameSize:return L"枠の大きさ";
    case D810AfReject::Center:return L"枠の位置";
    case D810AfReject::None:break;
    }
    return L"";
}
// The line under the picture. The caller holds c.mutex.
std::wstring AfStatusText(const Camera& c){
    if(!c.af.frame){
        if(c.af.frame_at==Clock::time_point{})return L"AF枠: 未取得";
        return std::wstring(L"AF枠: 不明（")+AfRejectText(c.af.reject)+L"が使えません）";
    }
    const auto& f=*c.af.frame;
    return L"AF枠（本体の報告）: 中心 "+std::to_wstring(f.center_x)+L","+std::to_wstring(f.center_y)+
        L" / 枠 "+std::to_wstring(f.frame_width)+L"x"+std::to_wstring(f.frame_height)+
        L" / 合焦の印: "+(f.FocusReported()?L"あり":L"なし")+L"（本体の報告）"+
        (AfRequestVisible(c,Clock::now())?L" / 要求 "+std::to_wstring(c.af.request->x)+L","+std::to_wstring(c.af.request->y):L"");
}
// A two pixel outline, clipped to the picture so that it never spills into the next pane.
bool DrawAfRect(HDC dc,const PreviewAfRect& rect,const PixelRect& picture,COLORREF color){
    const auto saved=SaveDC(dc);if(!saved)return false;
    IntersectClipRect(dc,picture.left,picture.top,picture.left+picture.width,picture.top+picture.height);
    SelectObject(dc,GetStockObject(DC_PEN));SelectObject(dc,GetStockObject(NULL_BRUSH));SetDCPenColor(dc,color);
    bool drawn=Rectangle(dc,rect.left,rect.top,rect.left+rect.width,rect.top+rect.height)!=FALSE;
    drawn=Rectangle(dc,rect.left-1,rect.top-1,rect.left+rect.width+1,rect.top+rect.height+1)!=FALSE && drawn;
    RestoreDC(dc,saved);return drawn;
}
// Over the picture Paint has just drawn for camera `index`. The caller holds c.mutex. Returns whether the
// reported frame was drawn.
bool DrawAfOverlay(HDC dc,const Camera& c,const RECT& client,int index){
    const auto picture=PanePictureRect(client,index,c.pixels);
    if(picture.width<=0 || picture.height<=0)return false;
    const PreviewAfRect area{picture.left,picture.top,picture.width,picture.height};
    bool drawn=false;
    if(AfRequestVisible(c,Clock::now())){
        D810AfHeader asked=*c.af.frame;asked.center_x=static_cast<int>(c.af.request->x);asked.center_y=static_cast<int>(c.af.request->y);
        if(const auto rect=PreviewAfFrameOnDisplay(asked,area))DrawAfRect(dc,*rect,picture,kAfRequestColor);
    }
    if(c.af.frame){
        const auto rect=PreviewAfFrameOnDisplay(*c.af.frame,area);
        drawn=rect && DrawAfRect(dc,*rect,picture,kAfReportedColor);
    }
    const auto text=AfStatusText(c);
    RECT strip{picture.left,std::max<int>(picture.top,picture.top+picture.height-kAfStripHeight),picture.left+picture.width,picture.top+picture.height};
    FillRect(dc,&strip,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    DrawTextW(dc,text.c_str(),-1,&strip,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_VCENTER|DT_END_ELLIPSIS);
    return drawn;
}
// Which camera's selectable part of the picture holds the client point, or -1. Below the size at which
// Paint draws nothing there is no picture, so there is nothing to select either.
int AfPictureAt(const RECT& client,int x,int y){
    if(client.right<200 || client.bottom<240)return -1;
    for(int i=0;i<2;++i){
        PixelRect picture;
        {std::lock_guard lock(cameras[i]->mutex);picture=PanePictureRect(client,i,cameras[i]->pixels);}
        if(picture.width>0 && x>=picture.left && x<picture.left+picture.width && y>=picture.top && y<picture.top+picture.height-kAfStripHeight)return i;
    }
    return -1;
}
// WM_MOUSEACTIVATE. The click that brings the window to the front is not an instruction: over a picture
// it is eaten (MA_ACTIVATEANDEAT), so it activates the window and never reaches HandleAfPointClick.
// Windows also sends the message to the parent for every click on a child control, even when the window is
// in front, so the click is eaten only when the window is not in front and no visible child is under the
// cursor: a button or list laid over a picture still works. Returns 0 when the default handling applies.
LRESULT AfMouseActivate(const RECT& client,WORD hit,WORD message,POINT client_point,bool window_in_front=false,bool over_child=false){
    if(hit==HTCLIENT && message==WM_LBUTTONDOWN && !window_in_front && !over_child && AfPictureAt(client,client_point.x,client_point.y)>=0)return MA_ACTIVATEANDEAT;
    return 0;
}
// Whether a visible child window lies under the client point.
bool AfChildAt(HWND hwnd,POINT client_point){
    const HWND child=ChildWindowFromPointEx(hwnd,client_point,CWP_SKIPINVISIBLE|CWP_SKIPTRANSPARENT);
    return child && child!=hwnd;
}
// The window procedure's side of WM_MOUSEACTIVATE: reads the cursor and the foreground window.
LRESULT AfMouseActivateOf(HWND hwnd,LPARAM l){
    RECT area{};GetClientRect(hwnd,&area);POINT cursor{};GetCursorPos(&cursor);ScreenToClient(hwnd,&cursor);
    return AfMouseActivate(area,LOWORD(l),HIWORD(l),cursor,GetForegroundWindow()==hwnd,AfChildAt(hwnd,cursor));
}
// WM_LBUTTONDOWN. A click on a camera's picture asks for one AF frame move on that camera: nothing else.
// The click is accepted under the conditions of the AF button (CanFocus: Live running, first frames
// shown, picture fresh, nothing pending), only when the header of the shown frame is usable, and the
// mailbox keeps it to one pending request. It never starts AF and never writes a setting. When the
// camera is not in a state to take a click, nothing is written: no status text and no log line.
void HandleAfPointClick(HWND hwnd,int x,int y){
    if(camera_end_restart_smoke)return;
    RECT client{};GetClientRect(hwnd,&client);
    const int index=AfPictureAt(client,x,y);
    if(index<0)return;
    auto& c=*cameras[index];
    if(!CanFocus(c))return;
    PixelRect picture;std::optional<D810AfHeader> frame;
    {std::lock_guard lock(c.mutex);picture=PanePictureRect(client,index,c.pixels);frame=c.af.frame;}
    if(!frame){
        c.FocusStatus(L"AF枠の位置が不明なため、枠は動かしません");
        Log(c.alias+" ui_af_point queued=0 reason=header_unknown");return;
    }
    const auto point=PreviewAfPointFromClick(PreviewAfRect{picture.left,picture.top,picture.width,picture.height},x,y,*frame);
    if(!point){Log(c.alias+" ui_af_point queued=0 reason=no_point");return;}
    // The red mark is set before the request is queued: once queued, the worker may finish and clear it.
    std::optional<PreviewAfMark> previous;
    {std::lock_guard lock(c.mutex);previous=c.af.request;c.af.request=PreviewAfMark{point->x,point->y,point->width,point->height,Clock::now()};}
    const bool queued=c.focus_mailbox.TryQueueAfPoint(*point,true);
    if(!queued){std::lock_guard lock(c.mutex);c.af.request=previous;}
    Log(c.alias+" ui_af_point queued="+std::to_string(queued));
    if(queued)c.FocusStatus(L"AF枠の移動を受け付けました…");
}
