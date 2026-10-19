#pragma once
#include <Windows.h>

namespace a0::phase0::experimental {
// Draw the entire preview offscreen before presenting it. Never expose the
// intermediate black background between frames. Own only application GDI data.
class PreviewBackBuffer final {
public:
    PreviewBackBuffer(HDC target, int width, int height) noexcept : width_(width),height_(height) {
        if(!target || width<=0 || height<=0 ||
            static_cast<unsigned long long>(width)*height>32ull*1024*1024)return;
        dc_=CreateCompatibleDC(target);
        if(!dc_)return;
        bitmap_=CreateCompatibleBitmap(target,width,height);
        if(!bitmap_)return;
        original_=SelectObject(dc_,bitmap_);
        if(!original_ || original_==HGDI_ERROR)return;
        ready_=true;
    }
    ~PreviewBackBuffer(){
        if(original_ && original_!=HGDI_ERROR)SelectObject(dc_,original_);
        if(bitmap_)DeleteObject(bitmap_);
        if(dc_)DeleteDC(dc_);
    }
    PreviewBackBuffer(const PreviewBackBuffer&)=delete;
    PreviewBackBuffer& operator=(const PreviewBackBuffer&)=delete;
    [[nodiscard]] HDC DrawingContext() const noexcept{return ready_?dc_:nullptr;}
    [[nodiscard]] bool Present(HDC target) const noexcept {
        return ready_ && target && BitBlt(target,0,0,width_,height_,dc_,0,0,SRCCOPY)!=FALSE;
    }
private:
    int width_{},height_{};HDC dc_{};HBITMAP bitmap_{};HGDIOBJ original_{};bool ready_{};
};
}
