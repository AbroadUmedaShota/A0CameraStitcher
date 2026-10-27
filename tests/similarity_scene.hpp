#pragma once
// Test-only drawing-like scene (#365) and its A/B views under a known
// similarity. Independent of the estimators: no estimator header is used.
// Raw row pointers keep full-size views affordable in unoptimized builds.
#include "a0/m2/render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace similarity_scene {
using a0::m2::render::BgrImage;
// World pixel (i,j) covers A corner coordinates [i-ox,i-ox+1) x [j-oy,j-oy+1).
struct Scene {int width{},height{},ox{},oy{};std::vector<std::uint8_t> v;};
struct Lcg {std::uint64_t s;unsigned Next(unsigned n){s=s*6364136223846793005ull+1442695040888963407ull;return static_cast<unsigned>((s>>33)%n);}};
// Paper with ruled lines, boxes, diagonal strokes and rows of glyph blocks,
// then a 3x3 box blur so bilinear views are smooth. Covers A and the A-frame
// footprint of B for translations up to max_x and |y| up to max_y.
inline Scene MakeScene(int w,int h,int max_x,int max_y,std::uint64_t seed){
    const int m=h/10+max_y;Scene s{max_x+w+w/10+2*m,h+2*m,m,m,{}};
    std::vector<std::uint8_t> raw(static_cast<size_t>(s.width)*s.height,232);Lcg r{seed};
    const auto box=[&](int x0,int y0,int x1,int y1,std::uint8_t ink){
        x0=std::max(x0,0);y0=std::max(y0,0);x1=std::min(x1,s.width);y1=std::min(y1,s.height);
        if(x0>=x1)return;
        for(int y=y0;y<y1;++y){std::uint8_t* const row=raw.data()+static_cast<size_t>(y)*s.width;std::fill(row+x0,row+x1,ink);}
    };
    const double area=double(s.width)*s.height;
    for(long k=0;k<static_cast<long>(area/9000);++k){
        const int x=static_cast<int>(r.Next(s.width)),y=static_cast<int>(r.Next(s.height)),len=40+static_cast<int>(r.Next(500)),t=1+static_cast<int>(r.Next(3));
        const auto ink=static_cast<std::uint8_t>(30+r.Next(60));
        switch(r.Next(4)){
        case 0:box(x,y,x+len,y+t,ink);break;
        case 1:box(x,y,x+t,y+len,ink);break;
        case 2:box(x,y,x+len,y+t,ink);box(x,y+len/2,x+len,y+len/2+t,ink);box(x,y,x+t,y+len/2+t,ink);box(x+len-t,y,x+len,y+len/2+t,ink);break;
        default:{const double a=r.Next(180)*std::numbers::pi/180;for(int i=0;i<len;++i)box(x+static_cast<int>(i*std::cos(a)),y+static_cast<int>(i*std::sin(a)),x+static_cast<int>(i*std::cos(a))+t+1,y+static_cast<int>(i*std::sin(a))+t+1,ink);}
        }
    }
    for(long k=0;k<static_cast<long>(area/60000);++k){
        int x=static_cast<int>(r.Next(s.width));const int y=static_cast<int>(r.Next(s.height)),size=6+static_cast<int>(r.Next(10)),count=4+static_cast<int>(r.Next(24));
        for(int g=0;g<count;++g){const int gw=size/2+static_cast<int>(r.Next(size)),gh=size+static_cast<int>(r.Next(size/2+1));
            if(r.Next(5))box(x,y,x+gw,y+gh,static_cast<std::uint8_t>(40+r.Next(40)));x+=gw+2+static_cast<int>(r.Next(4));}
    }
    s.v.resize(raw.size());const std::uint8_t* const in=raw.data();std::uint8_t* const out=s.v.data();
    for(int y=0;y<s.height;++y)for(int x=0;x<s.width;++x){
        int sum=0,n=0;
        for(int v=std::max(0,y-1);v<=std::min(s.height-1,y+1);++v){const std::uint8_t* const row=in+static_cast<size_t>(v)*s.width;
            for(int u=std::max(0,x-1);u<=std::min(s.width-1,x+1);++u){sum+=row[u];++n;}}
        out[static_cast<size_t>(y)*s.width+x]=static_cast<std::uint8_t>((sum+n/2)/n);
    }
    return s;
}
inline BgrImage Gray(unsigned w,unsigned h){return {w,h,std::vector<std::uint8_t>(static_cast<size_t>(w)*h*3)};}
inline void Put(std::uint8_t* pixel,double value){
    const auto v=static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));pixel[0]=v;pixel[1]=v;pixel[2]=v;
}
inline void Put(BgrImage& image,unsigned x,unsigned y,double value){Put(image.bgr.data()+(static_cast<size_t>(y)*image.width+x)*3,value);}
inline BgrImage ViewA(const Scene& s,unsigned w,unsigned h){
    auto image=Gray(w,h);
    for(unsigned y=0;y<h;++y){const std::uint8_t* const row=s.v.data()+static_cast<size_t>(y+s.oy)*s.width+s.ox;std::uint8_t* const out=image.bgr.data()+static_cast<size_t>(y)*w*3;
        for(unsigned x=0;x<w;++x)Put(out+x*3,row[x]);}
    return image;
}
// B pixel center b maps to A corner coordinates a = S R (b - c) + c + (x,y),
// c the image center, positive degrees clockwise with y down. gain scales B.
inline BgrImage ViewB(const Scene& s,unsigned w,unsigned h,double x,double y,double degrees,double scale,double gain){
    auto image=Gray(w,h);const double a=degrees*std::numbers::pi/180,p=scale*std::cos(a),q=scale*std::sin(a),cx=w/2.0,cy=h/2.0;
    const std::uint8_t* const world=s.v.data();
    for(unsigned j=0;j<h;++j){std::uint8_t* const out=image.bgr.data()+static_cast<size_t>(j)*w*3;
        for(unsigned i=0;i<w;++i){
            const double bx=i+0.5-cx,by=j+0.5-cy,ax=p*bx-q*by+cx+x+s.ox-0.5,ay=q*bx+p*by+cy+y+s.oy-0.5;
            const int x0=static_cast<int>(std::floor(ax)),y0=static_cast<int>(std::floor(ay));double value=232;
            if(x0>=0 && y0>=0 && x0+1<s.width && y0+1<s.height){
                const double fx=ax-x0,fy=ay-y0;const std::uint8_t* const t=world+static_cast<size_t>(y0)*s.width+x0;const std::uint8_t* const b=t+s.width;
                value=(t[0]*(1-fx)+t[1]*fx)*(1-fy)+(b[0]*(1-fx)+b[1]*fx)*fy;
            }
            Put(out+i*3,value*gain);
        }
    }
    return image;
}
}
