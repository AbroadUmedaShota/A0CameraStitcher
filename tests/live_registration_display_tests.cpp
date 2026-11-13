// What the screen before the shot shows about the cameras (#417 LC): the direction of camera A at calibration found from its
// calibrated projection, B's direction from the live estimate, the sentences made through TXT, and the sentence about the state of
// the live C. The projections are built from known rotations, so the expected values are known without any measured rig.
// Camera and display free.
#include "a0/phase0/experimental/live_registration_display.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

using namespace a0::phase0::experimental;
namespace ex=a0::m2::experimental;
namespace render=a0::m2::render;

namespace {
int checks=0,failures=0;
void Check(bool value,const char* what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(expr) Check((expr),#expr)
bool Near(double a,double b,double tolerance){return std::abs(a-b)<=tolerance;}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
constexpr double kNaN=std::numeric_limits<double>::quiet_NaN();
constexpr double kPi=3.14159265358979323846;

using Mat=std::array<double,9>;
Mat Mul(const Mat& a,const Mat& b){
    Mat r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[i*3+j]+=a[i*3+k]*b[k*3+j];
    return r;
}
Mat Rx(double deg){const double c=std::cos(deg*kPi/180),s=std::sin(deg*kPi/180);return {1,0,0,0,c,-s,0,s,c};}
Mat Ry(double deg){const double c=std::cos(deg*kPi/180),s=std::sin(deg*kPi/180);return {c,0,s,0,1,0,-s,0,c};}
Mat Rz(double deg){const double c=std::cos(deg*kPi/180),s=std::sin(deg*kPi/180);return {c,-s,0,s,c,0,0,0,1};}

constexpr double kFx=6000,kFy=6000,kCx=3680,kCy=2456;
// A pinhole camera over the table: the centre at (x0, y0) mm and `height` mm over the table (the camera's z points to the table),
// turned by R (the table's axes to the camera's: x right, y down, z forward). No lens distortion. image_turned: the image is
// turned by 180 degrees about the principal point, which is a camera mounted upside down.
render::CameraProjection Camera(const Mat& r,double x0,double y0,double height,bool image_turned){
    // camera coordinates = R (P - C), P = (x, y, 0), C = (x0, y0, -height); the homography is K [R e1, R e2, -R C]
    const std::array<double,3> t{-(r[0]*x0+r[1]*y0+r[2]*(-height)),-(r[3]*x0+r[4]*y0+r[5]*(-height)),-(r[6]*x0+r[7]*y0+r[8]*(-height))};
    Mat cam{r[0],r[1],t[0],r[3],r[4],t[1],r[6],r[7],t[2]};
    Mat k{kFx,0,kCx,0,kFy,kCy,0,0,1};
    Mat h=Mul(k,cam);
    if(image_turned)h=Mul(Mat{-1,0,2*kCx,0,-1,2*kCy,0,0,1},h);
    for(auto& v:h)v/=h[8];
    return render::CameraProjection(render::CameraIntrinsics(kFx,kFy,kCx,kCy),render::LensDistortion(0,0,0,0,0),h);
}

void Tilt(){
    // Tilt about the camera's x axis: the lens faces the +y side of the drawing (down on the screen).
    const auto down=CameraOrientationOfCalibratedA(Camera(Rx(1.2),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(down && Near(down->lens_tilt_deg,1.2,1e-7) && Near(down->lens_direction.x,0,1e-7) && Near(down->lens_direction.y,1,1e-7));
    CHECK(down && Near(down->rotation_clockwise_deg,0,1e-7));
    // About y: the lens faces -x (left).
    const auto left=CameraOrientationOfCalibratedA(Camera(Ry(1.0),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(left && Near(left->lens_tilt_deg,1.0,1e-7) && Near(left->lens_direction.x,-1,1e-7) && Near(left->lens_direction.y,0,1e-7));
    // Opposite signs face the opposite sides.
    const auto up=CameraOrientationOfCalibratedA(Camera(Rx(-0.7),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(up && Near(up->lens_tilt_deg,0.7,1e-7) && Near(up->lens_direction.y,-1,1e-7));
    // A diagonal tilt: about 0.9 degrees to the lower right, the angles combine as the axis does.
    const auto diagonal=CameraOrientationOfCalibratedA(Camera(Mul(Ry(-0.5),Rx(0.5)),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(diagonal && diagonal->lens_direction.x>0.6 && diagonal->lens_direction.y>0.6 && Near(diagonal->lens_tilt_deg,0.7,0.02));
    // The tilt does not depend on where the camera stands or how high.
    const auto elsewhere=CameraOrientationOfCalibratedA(Camera(Rx(1.2),-300,900,1500,false),ex::MountOrientation::normal);
    CHECK(elsewhere && Near(elsewhere->lens_tilt_deg,1.2,1e-7) && Near(elsewhere->lens_direction.y,1,1e-7));
    // No tilt: no direction.
    const auto level=CameraOrientationOfCalibratedA(Camera(Rx(0),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(level && Near(level->lens_tilt_deg,0,1e-7) && level->lens_direction.x==0 && level->lens_direction.y==0);
}

void Turn(){
    // Turned about its own axis: positive when clockwise as seen on the screen, which is minus the angle at which the drawing
    // appears turned in the image.
    for(const double deg:{2.0,-2.0,0.4,45.0,-120.0,90.0}){
        const auto o=CameraOrientationOfCalibratedA(Camera(Rz(deg),600,120,1100,false),ex::MountOrientation::normal);
        Check(o && Near(o->rotation_clockwise_deg,-deg,1e-7) && Near(o->lens_tilt_deg,0,1e-7),"a turn about the axis, nothing else");
    }
    // The turn and a tilt together: the tilt is the same wherever the roll is, and the turn is still the roll (to a small coupling).
    const auto both=CameraOrientationOfCalibratedA(Camera(Mul(Rz(2.0),Rx(1.2)),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(both && Near(both->lens_tilt_deg,1.2,1e-7) && Near(both->lens_direction.y,1,1e-7) && Near(both->rotation_clockwise_deg,-2.0,0.05));
    // A half turn is the other side of the wrap.
    const auto half=CameraOrientationOfCalibratedA(Camera(Rz(180),600,120,1100,false),ex::MountOrientation::normal);
    CHECK(half && Near(half->rotation_clockwise_deg,180,1e-7));
}

void Mounting(){
    // A rig mounted turned by 180 degrees has the image upside down in its projection. The same physical setup gives the same
    // value for either mounting, and the half turn is not reported as a turn to correct.
    for(const auto& r:{Rx(1.2),Mul(Rz(2.0),Rx(1.2)),Mul(Rz(-3.5),Ry(0.8)),Rz(0.3),Mat{1,0,0,0,1,0,0,0,1}}){
        const auto normal=CameraOrientationOfCalibratedA(Camera(r,600,120,1100,false),ex::MountOrientation::normal);
        const auto turned=CameraOrientationOfCalibratedA(Camera(r,600,120,1100,true),ex::MountOrientation::rotated_180);
        Check(normal && turned,"both mountings give a value");
        if(!normal || !turned)continue;
        Check(Near(normal->rotation_clockwise_deg,turned->rotation_clockwise_deg,1e-7) && Near(normal->lens_tilt_deg,turned->lens_tilt_deg,1e-7) &&
            Near(normal->lens_direction.x,turned->lens_direction.x,1e-7) && Near(normal->lens_direction.y,turned->lens_direction.y,1e-7),"same value for either mounting");
        // The same sentences.
        const auto a=LiveOrientationTextsOf(normal,std::nullopt),b=LiveOrientationTextsOf(turned,std::nullopt);
        Check(a.a_short==b.a_short && a.a_detail==b.a_detail,"same sentences for either mounting");
    }
    // The image turned and the mount left as normal reads as a half turn of the camera: nothing is flipped twice or hidden.
    const auto upside=CameraOrientationOfCalibratedA(Camera(Rx(1.2),600,120,1100,true),ex::MountOrientation::normal);
    CHECK(upside && Near(std::abs(upside->rotation_clockwise_deg),180,1e-7) && Near(upside->lens_tilt_deg,1.2,1e-7));
}

void Unmeasurable(){
    const auto bad=render::CameraProjection(render::CameraIntrinsics(kFx,kFy,kCx,kCy),render::LensDistortion(0,0,0,0,0),Mat{0,0,0,0,0,0,0,0,0});
    CHECK(!CameraOrientationOfCalibratedA(bad,ex::MountOrientation::normal));
    // Two columns that are the same direction: the plane cannot be decomposed.
    const auto degenerate=render::CameraProjection(render::CameraIntrinsics(kFx,kFy,kCx,kCy),render::LensDistortion(0,0,0,0,0),Mat{1,1,100,1,1,100,0,0,1});
    CHECK(!CameraOrientationOfCalibratedA(degenerate,ex::MountOrientation::normal));
}

// B against A, from the estimator's own relative orientation: the sentences are TXT's, the same for either mounting.
void BAgainstA(){
    namespace cr=ex::content_registration;
    for(const bool turned:{false,true}){
        const auto mount=turned?ex::MountOrientation::rotated_180:ex::MountOrientation::normal;
        const auto a=Camera(Rx(0),300,120,1100,turned);
        const auto b=Camera(Mul(Rz(2.0),Rx(1.2)),900,120,1100,turned);
        const auto relative=cr::RelativeOrientationOf(cr::OrientationOf(a),cr::OrientationOf(b));
        const auto value=CameraOrientationOfBAgainstA(relative);
        Check(value && Near(value->rotation_clockwise_deg,-2.0,0.05) && Near(value->lens_tilt_deg,1.2,0.02) && Near(value->lens_direction.y,1,1e-3),"B against A");
        // A is level, so its value is nothing; B's sentence is the relative one.
        const auto a_value=CameraOrientationOfCalibratedA(a,mount);
        Check(a_value && Near(a_value->rotation_clockwise_deg,0,1e-7) && Near(a_value->lens_tilt_deg,0,1e-7),"A level for either mounting");
        const auto t=LiveOrientationTextsOf(a_value,value);
        Check(t.b_short=="B: 左回りに 2°、図面の下へ 1.2° 傾き" && t.a_short=="A（校正時）: 回転なし、傾きなし","sentences of B against A");
    }
    cr::RelativeOrientation bad;bad.rotation_clockwise_deg=kNaN;
    CHECK(!CameraOrientationOfBAgainstA(bad));
    CHECK(!CameraOrientationOfCalibratedA(cr::CameraOrientation{{0,0},1000,kNaN,{0,0},0},ex::MountOrientation::normal));
}

void Estimate(){
    CHECK(!CameraOrientationOfEstimate(std::nullopt));
    ex::OutputFrameDisplay display;
    display.rotation_clockwise_deg=2.5;display.lens_direction={std::sqrt(0.5),-std::sqrt(0.5)};display.lens_tilt_deg=1.2;
    const auto o=CameraOrientationOfEstimate(display);
    CHECK(o && o->rotation_clockwise_deg==2.5 && o->lens_tilt_deg==1.2 && o->lens_direction.x==display.lens_direction.x);
    auto bad=display;bad.rotation_clockwise_deg=kNaN;
    CHECK(!CameraOrientationOfEstimate(bad));
    bad=display;bad.lens_tilt_deg=std::numeric_limits<double>::infinity();
    CHECK(!CameraOrientationOfEstimate(bad));
    bad=display;bad.lens_direction.y=kNaN;
    CHECK(!CameraOrientationOfEstimate(bad));
}

void Texts(){
    const auto a=CameraOrientationOfCalibratedA(Camera(Mul(Rz(2.0),Rx(1.2)),600,120,1100,false),ex::MountOrientation::normal);
    ex::OutputFrameDisplay display;
    display.rotation_clockwise_deg=2.5;display.lens_direction={std::sqrt(0.5),-std::sqrt(0.5)};display.lens_tilt_deg=1.2;
    const auto b=CameraOrientationOfEstimate(display);
    const auto t=LiveOrientationTextsOf(a,b);
    // The sentences are TXT's, unchanged.
    CHECK(t.a_short==CameraOrientationShortText(OrientationSubject::camera_a_at_calibration,a));
    CHECK(t.b_short==CameraOrientationShortText(OrientationSubject::camera_b_against_a,b));
    CHECK(t.a_detail==CameraOrientationDetailText(OrientationSubject::camera_a_at_calibration,a));
    CHECK(t.b_detail==CameraOrientationDetailText(OrientationSubject::camera_b_against_a,b));
    CHECK(Contains(t.a_short,"A（校正時）") && Contains(t.a_short,"左回りに 2°") && Contains(t.a_short,"図面の下へ 1.2° 傾き"));
    CHECK(Contains(t.b_short,"B: ") && Contains(t.b_short,"右回りに 2.5°") && Contains(t.b_short,"図面の右上へ 1.2° 傾き"));
    CHECK(t.line==t.a_short+" ／ "+t.b_short && t.line.find('\n')==std::string::npos);
    // No estimate for B: the line says so and still shows A.
    const auto none=LiveOrientationTextsOf(a,std::nullopt);
    CHECK(Contains(none.line,"B: 向きを測れていません") && Contains(none.line,"A（校正時）") && Contains(none.b_detail,"向きを測れていません"));
    const auto neither=LiveOrientationTextsOf(std::nullopt,std::nullopt);
    CHECK(Contains(neither.line,"A（校正時）: 向きを測れていません"));
    // The same inputs give the same sentences.
    CHECK(LiveOrientationTextsOf(a,b).line==t.line);
}

void StateText(){
    LiveRegistrationView view;
    const auto fixed_waiting=LiveRegistrationStateText(view,true);
    view.fixed_reason="estimate-failed";
    const auto fixed_failed=LiveRegistrationStateText(view,true);
    view.fixed_reason="hold-expired";
    const auto fixed_expired=LiveRegistrationStateText(view,true);
    view.state=LiveRegistrationState::tracking;
    const auto tracking=LiveRegistrationStateText(view,true);
    view.beyond_envelope=true;
    const auto beyond=LiveRegistrationStateText(view,true);
    view.beyond_envelope=false;view.state=LiveRegistrationState::holding;
    const auto holding=LiveRegistrationStateText(view,true);
    const auto disabled=LiveRegistrationStateText(view,false);
    const std::array<std::string,7> all{fixed_waiting,fixed_failed,fixed_expired,tracking,beyond,holding,disabled};
    for(std::size_t i=0;i<all.size();++i){
        Check(!all[i].empty(),"a sentence for every state");
        for(std::size_t j=i+1;j<all.size();++j)Check(all[i]!=all[j],"different states read differently");
        // The picture is a guide: it never says within criteria, passed, or that the composite cannot be saved.
        Check(!Contains(all[i],"合格") && !Contains(all[i],"基準内") && !Contains(all[i],"書き出せません") && !Contains(all[i],"保存できません"),"no verdict in the sentence");
        Check(all[i].find('\n')==std::string::npos,"one line");
    }
    CHECK(Contains(tracking,"目安") && Contains(beyond,"範囲の外") && Contains(holding,"直前の推定") && Contains(disabled,"無効"));
    CHECK(Contains(fixed_expired,"固定") && Contains(fixed_failed,"固定") && Contains(fixed_waiting,"待って"));
    // The sentence does not depend on anything but the view: the disabled sentence wins over the state.
    view.state=LiveRegistrationState::tracking;
    CHECK(LiveRegistrationStateText(view,false)==disabled);
    // Which components are outside the range: named in the order position, rotation, tilt, height, then the paper's; an unknown code is left out.
    view.beyond_envelope=true;
    view.exceeded_components={"tilt","position"};
    CHECK(Contains(LiveRegistrationStateText(view,true),"範囲を超えた量: 位置・傾き）"));
    view.exceeded_components={"surface-tilt","rotation","height","surface-height","something-else"};
    CHECK(Contains(LiveRegistrationStateText(view,true),"範囲を超えた量: 回転・高さ・紙面の高さ・紙面の傾き）"));
    CHECK(LiveExceededComponentsText({})=="" && LiveExceededComponentsText({"something-else"})=="");
    CHECK(LiveExceededComponentsText({"position"})=="位置");
    for(const auto& codes:{std::vector<std::string>{},std::vector<std::string>{"position"}}){
        view.exceeded_components=codes;
        const auto text=LiveRegistrationStateText(view,true);
        Check(Contains(text,"許容の範囲の外の値で描いています") && text.find('\n')==std::string::npos && !Contains(text,"合格"),"the sentence about the range stays a guide's sentence on one line");
    }
    // Inside the range nothing is named, whatever the view still holds.
    view.beyond_envelope=false;
    CHECK(!Contains(LiveRegistrationStateText(view,true),"範囲を超えた量"));
}
}

// B moved by an estimate, against A: the same construction as the estimator's own orientation, so the zero pose gives B as calibrated.
void BFromPose(){
    namespace cr=ex::content_registration;
    const render::OutputRaster raster{render::DocumentRegionUm{450000,60000,650000,160000},254,2000,1000};
    const render::DocumentRenderParameters profile{raster,Camera(Rx(0),520,120,1100,false),Camera(Mul(Rz(1.0),Rx(0.5)),580,120,1100,false),
        a0::m2::StitchLayout::camera_a_left_camera_b_right,render::Resampling::bilinear};
    const auto calibrated=CameraOrientationOfBFromPose(profile,cr::PoseParameters{});
    const auto direct=cr::RelativeOrientationOf(cr::OrientationOf(profile.camera_a),cr::OrientationOf(profile.camera_b));
    Check(calibrated && Near(calibrated->rotation_clockwise_deg,direct.rotation_clockwise_deg,1e-6) && Near(calibrated->lens_tilt_deg,direct.tilt_deg,1e-6),"zero pose is B as calibrated");
    Check(calibrated && Near(calibrated->lens_tilt_deg,0.5,0.02) && Near(std::abs(calibrated->rotation_clockwise_deg),1.0,0.05),"the rig's own tilt and turn are seen");
    const auto moved=CameraOrientationOfBFromPose(profile,cr::PoseParameters{0,0,3.0,0,0,0});
    Check(moved && moved->rotation_clockwise_deg!=calibrated->rotation_clockwise_deg && Near(std::abs(moved->rotation_clockwise_deg-calibrated->rotation_clockwise_deg),3.0,0.2),"a turn of the pose is a turn of B");
    const auto tilted=CameraOrientationOfBFromPose(profile,cr::PoseParameters{1.5,0,0,0,0,0});
    Check(tilted && tilted->lens_tilt_deg>calibrated->lens_tilt_deg+0.5,"a tilt of the pose is a tilt of B");
    // A pose that is not finite, and a profile that cannot be decomposed, are not measured.
    Check(!CameraOrientationOfBFromPose(profile,cr::PoseParameters{kNaN,0,0,0,0,0}),"a pose that is not finite");
    auto broken=profile;broken.camera_b=render::CameraProjection(render::CameraIntrinsics(kFx,kFy,kCx,kCy),render::LensDistortion(0,0,0,0,0),Mat{1,1,100,1,1,100,0,0,1});
    Check(!CameraOrientationOfBFromPose(broken,cr::PoseParameters{}),"a profile that cannot be decomposed");
    // The row: the explanation of the two values, then TXT's line.
    const auto a=CameraOrientationOfCalibratedA(profile.camera_a,ex::MountOrientation::normal);
    const auto texts=LiveOrientationTextsOf(a,moved);
    const auto row=LiveOrientationRowText(texts);
    CHECK(Contains(row,"Aに対する今の値") && Contains(row,texts.line) && row.find('\n')==std::string::npos);
}

// The two sentences that belong to the connection of the estimator and to a table that could not be made.
void ConnectionTexts(){
    LiveRegistrationView view;
    view.fixed_reason="estimate-failed";
    const auto failed=LiveRegistrationStateText(view,true);
    view.last_failure={std::string(kLiveEstimatorUnavailableReason)};
    const auto unavailable=LiveRegistrationStateText(view,true);
    CHECK(unavailable!=failed && Contains(unavailable,"未接続") && Contains(unavailable,"固定の位置") && unavailable.find('\n')==std::string::npos);
    // Another failure keeps the ordinary sentence; the reason of the connection only counts while no estimate is in force.
    view.last_failure={"too-few-points"};
    CHECK(LiveRegistrationStateText(view,true)==failed);
    view.last_failure={std::string(kLiveEstimatorUnavailableReason)};
    view.fixed_reason="hold-expired";
    CHECK(!Contains(LiveRegistrationStateText(view,true),"未接続"));
    // A table that could not be made for the estimate in force: said, whatever the state, unless C is fixed anyway.
    view.state=LiveRegistrationState::tracking;
    const auto tracking=LiveRegistrationStateText(view,true);
    const auto not_drawn=LiveRegistrationStateText(view,true,true);
    CHECK(not_drawn!=tracking && Contains(not_drawn,"描けませんでした") && Contains(not_drawn,"直前") && not_drawn.find('\n')==std::string::npos);
    CHECK(!Contains(not_drawn,"合格") && !Contains(not_drawn,"基準内") && !Contains(not_drawn,"書き出せません") && !Contains(not_drawn,"保存できません"));
    view.state=LiveRegistrationState::holding;
    CHECK(Contains(LiveRegistrationStateText(view,true,true),"描けませんでした"));
    view.state=LiveRegistrationState::fixed;
    CHECK(!Contains(LiveRegistrationStateText(view,true,true),"描けませんでした"));
    CHECK(Contains(LiveRegistrationStateText(view,false,true),"無効"));
    // A session whose registration could not be set up.
    const auto start_failed=LiveRegistrationStartFailedText();
    CHECK(Contains(start_failed,"始められませんでした") && Contains(start_failed,"固定の位置") && start_failed!=unavailable && start_failed.find('\n')==std::string::npos);
}

int main(){
    try{
        Tilt();
        Turn();
        Mounting();
        Unmeasurable();
        BAgainstA();
        Estimate();
        Texts();
        StateText();
        BFromPose();
        ConnectionTexts();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
