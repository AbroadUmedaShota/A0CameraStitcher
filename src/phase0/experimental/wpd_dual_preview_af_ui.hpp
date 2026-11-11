// AF frame overlay of the development preview (#426). Included by wpd_dual_preview_ui.hpp before Paint,
// inside the anonymous namespace: it uses Camera, Pixels, PixelRect and FitPixels. Drawing only.
//
// Green: the AF frame the body reports in the frame header. The text under the picture says so and says
// that the mark is the body's report, not an optical focus decision.
inline constexpr COLORREF kAfReportedColor=RGB(0,230,0);
inline constexpr COLORREF kAfRequestColor=RGB(255,60,60);
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
        L" / 合焦の印: "+(f.FocusReported()?L"あり":L"なし")+L"（本体の報告）";
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
    if(c.af.frame){
        const auto rect=PreviewAfFrameOnDisplay(*c.af.frame,area);
        drawn=rect && DrawAfRect(dc,*rect,picture,kAfReportedColor);
    }
    const auto text=AfStatusText(c);
    RECT strip{picture.left,std::max<int>(picture.top,picture.top+picture.height-22),picture.left+picture.width,picture.top+picture.height};
    FillRect(dc,&strip,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    DrawTextW(dc,text.c_str(),-1,&strip,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_VCENTER|DT_END_ELLIPSIS);
    return drawn;
}
