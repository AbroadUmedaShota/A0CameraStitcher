#include "a0/m2/experimental/review_viewport.hpp"
#include <iostream>
using a0::m2::experimental::ReviewViewport;
int main(){
    int checks=0,failures=0;auto check=[&](bool value){++checks;if(!value)++failures;};
    ReviewViewport v{100,80,40,20,1,50,40};v.Constrain();
    check(v.Scale()==1 && v.Pixel(0,0)==std::array<unsigned,2>{30,30} && v.Pixel(39,19)==std::array<unsigned,2>{69,49});
    v.Pan(10,-5);check(v.Pixel(0,0)==std::array<unsigned,2>{20,35});
    v.Pan(999,999);check(v.Pixel(0,0)==std::array<unsigned,2>{0,0});
    v.Pan(-999,-999);check(v.Pixel(39,19)==std::array<unsigned,2>{99,79});
    v.zoom=2;v.center_x=50;v.center_y=40;v.Constrain();
    check(v.Pixel(0,0)==std::array<unsigned,2>{40,35} && v.Pixel(1,1)==v.Pixel(0,0) && v.Pixel(2,2)==std::array<unsigned,2>{41,36});
    v.zoom=0;v.Constrain();check(v.Scale()==0.25 && !v.Pixel(0,0) && !v.Pixel(39,19) && v.Pixel(8,0).has_value());
    v.Pan(999,-999);check(v.center_x==50 && v.center_y==40);check(!v.Pixel(40,0));
    v={4,2,10,8,1,2,1};v.Constrain();check(!v.Pixel(2,3) && v.Pixel(3,3)==std::array<unsigned,2>{0,0});
    a0::m2::render::BgrImage image{4,2,{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24}};
    auto pixels=a0::m2::experimental::ReviewViewportBgra(image,v);
    check(pixels[(3*10+3)*4]==1 && pixels[(4*10+6)*4+2]==24 && pixels[0]==0);
    v={4,2,8,4,2,2,1};pixels=a0::m2::experimental::ReviewViewportBgra(image,v);
    check(pixels[0]==1 && pixels[4]==1 && pixels[8]==4 && pixels[(3*8+7)*4+2]==24);
    v.zoom=3;bool rejected=false;try{v.Scale();}catch(const std::invalid_argument&){rejected=true;}check(rejected);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
