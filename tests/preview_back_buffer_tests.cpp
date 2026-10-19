#include "a0/phase0/experimental/preview_back_buffer.hpp"
#include <iostream>
using a0::phase0::experimental::PreviewBackBuffer;
int main(){
    HDC screen=GetDC(nullptr);if(!screen)return 2;
    HDC target=CreateCompatibleDC(screen);HBITMAP image=CreateCompatibleBitmap(screen,8,8);ReleaseDC(nullptr,screen);
    if(!target || !image)return 2;
    auto original=SelectObject(target,image);int checks=0,failures=0;
    auto check=[&](bool value){++checks;if(!value)++failures;};RECT rect{0,0,8,8};
    auto blue=CreateSolidBrush(RGB(0,0,255));auto red=CreateSolidBrush(RGB(255,0,0));
    FillRect(target,&rect,blue);
    const auto resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<100;++i){
        FillRect(target,&rect,blue);
        PreviewBackBuffer frame(target,8,8);check(frame.DrawingContext()!=nullptr);
        FillRect(frame.DrawingContext(),&rect,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        check(GetPixel(target,4,4)==RGB(0,0,255)); // Clearing offscreen never blanks visible content.
        FillRect(frame.DrawingContext(),&rect,red);
        check(GetPixel(target,4,4)==RGB(0,0,255));
        check(frame.Present(target));
        check(GetPixel(target,0,0)==RGB(255,0,0) && GetPixel(target,7,7)==RGB(255,0,0));
        check(!frame.Present(nullptr));
    }
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==resources);
    {PreviewBackBuffer bad(target,0,8);check(!bad.DrawingContext() && !bad.Present(target));}
    {PreviewBackBuffer bad(target,8192,8192);check(!bad.DrawingContext() && !bad.Present(target));}
    DeleteObject(blue);DeleteObject(red);SelectObject(target,original);DeleteObject(image);DeleteDC(target);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
