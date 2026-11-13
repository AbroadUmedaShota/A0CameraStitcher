// The command line of StillReview that makes and revises the fixed composite condition v3 (#417): the argument reader, the report it prints,
// the progress notice, and the real executable run on a temporary folder (success, every refusal, no overwrite, no path or identifier in the
// output). Camera free, window free; every file lives in a temporary folder.
// Arguments: 1 the shared rig profile v2 fixture folder, 2 the StillReview executable, 3 the policy file template of docs/examples.
// Every range, line and limit below is a value of this test; the product code has no default for any of them.
#include "a0/phase0/experimental/fixed_composite_cli_v3.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace px=a0::phase0::experimental;
namespace m2=a0::m2::experimental;
namespace fs=std::filesystem;
using namespace std::chrono_literals;
using Kind=m2::FixedCompositeRevisionKind;

namespace {
int checks=0,failures=0;
void Check(bool value,const std::string& what){++checks;if(!value){++failures;std::cerr<<"FAILED: "<<what<<'\n';}}
#define CHECK(...) Check((__VA_ARGS__),#__VA_ARGS__)

const auto kNow=std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026}/11/10}+12h};
const m2::FixedCompositeSha256 kHash=[](const std::vector<unsigned char>& bytes){return m2::FixedCompositeBytesSha256(bytes);};
std::string HashOf(const std::string& text){return m2::FixedCompositeBytesSha256(std::vector<unsigned char>(text.begin(),text.end()));}
std::string ReadAll(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void WriteAll(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
std::string Replace(std::string text,const std::string& from,const std::string& to){
    const auto at=text.find(from);
    if(at==std::string::npos){Check(false,"test token absent: "+from);return text;}
    text.replace(at,from.size(),to);return text;
}
fs::path TempParent(){
    char* value=nullptr;size_t length=0;
    if(_dupenv_s(&value,&length,"A0_TEST_TMPDIR")==0 && value!=nullptr){
        const std::string text(value);std::free(value);
        if(!text.empty())return fs::path(text);
    }
    return fs::temp_directory_path();
}
fs::path NewFolder(const std::string& name){
    const auto root=TempParent()/("a0-still-v3-cli-"+name+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);return root;
}
std::set<std::string> Listing(const fs::path& folder){
    std::set<std::string> names;
    if(fs::exists(folder))for(const auto& e:fs::directory_iterator(folder))names.insert(e.path().filename().string());
    return names;
}
std::vector<std::wstring> W(std::initializer_list<const wchar_t*> items){return std::vector<std::wstring>(items.begin(),items.end());}

fs::path profile_path,executable,example_policy;
std::string draft;

// A policy file of this test. Layer 1 and layer 2 can be switched off; the position range is a parameter.
std::string Policy(double position_mm=5,bool parallax=true,bool surface=true){
    return std::string("{\"schema\":\"a0.wpd-registration-policy.v1\",\"registration\":{\"envelope\":{\"positionMm\":")+std::to_string(position_mm)+
        ",\"rotationDeg\":1.5,\"tiltDeg\":2.5,\"heightPercent\":3},"
        "\"limits\":{\"brownLoweAlpha\":8,\"brownLoweBeta\":0.3,\"minimumUniqueBlocks\":3,\"maximumPredictionSigmaPx\":0.5,\"maximumResidualRmsPx\":0.75,"
        "\"maximumResidualP95Px\":1.5,\"maximumBinBiasPx\":0.4,\"convergencePx\":0.05,\"startAgreementPx\":0.25},"
        "\"geometry\":{\"omegaMarginPx\":2,\"guardPx\":72,\"bandHalfWidthPx\":96},\"surfaceEnvelope\":"+(surface?"{\"heightMm\":10,\"tiltDeg\":4}":"null")+
        ",\"applyBeyondEnvelope\":true,\"surfaceScaleWarnPercent\":null,\"parallax\":"+(parallax?"{\"maxLiftMm\":1}":"null")+",\"live\":null},"
        "\"seamGate\":{\"limitPx\":1,\"guardBandPx\":0.2},\"revisionRules\":{\"manualAdjustmentMaxSeamPx\":null}}";
}
const std::wstring kLayerTwo[]={L"--layer-two-falloff-px",L"64",L"--layer-two-max-strain",L"0.5"};   // this test's design constants

// ---- the real executable ----
struct Run {DWORD code{};std::string output;};
Run RunExe(const std::vector<std::wstring>& args,DWORD timeout_ms=600000){
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE read=nullptr,write=nullptr;
    if(!CreatePipe(&read,&write,&security,0))throw std::runtime_error("cannot create a pipe");
    SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    std::wstring command=L"\""+executable.wstring()+L"\"";
    for(const auto& a:args){
        command+=L" \"";
        for(const wchar_t c:a){if(c==L'"')command+=L'\\';command+=c;}
        command+=L"\"";
    }
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=write;startup.hStdError=write;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){CloseHandle(read);CloseHandle(write);throw std::runtime_error("cannot start the inspector");}
    CloseHandle(write);
    Run result;
    std::thread reader([&]{char buffer[4096];DWORD got=0;while(ReadFile(read,buffer,sizeof buffer,&got,nullptr) && got>0)result.output.append(buffer,got);});
    const auto waited=WaitForSingleObject(process.hProcess,timeout_ms);
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&result.code);else{TerminateProcess(process.hProcess,99);result.code=99;}
    reader.join();
    CloseHandle(read);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    return result;
}
// Arguments of the kinds, with this test's raster (18 mm x 8 mm inside the overlap of the fixture cameras, 180 x 80 pixels at 254 DPI).
std::vector<std::wstring> Created(const fs::path& folder,bool dry_run=false,bool layer_two=true){
    std::vector<std::wstring> a{L"created",L"--settings-folder",folder.wstring(),L"--profile",profile_path.wstring(),L"--region-um",L"580000,100000,598000,108000",L"--dpi",L"254"};
    if(layer_two)a.insert(a.end(),std::begin(kLayerTwo),std::end(kLayerTwo));
    if(dry_run)a.push_back(L"--dry-run");
    return a;
}
std::vector<std::wstring> WithFolder(const std::wstring& kind,const fs::path& folder,std::vector<std::wstring> more={},bool layer_two=true){
    std::vector<std::wstring> a{kind,L"--settings-folder",folder.wstring()};
    a.insert(a.end(),more.begin(),more.end());
    if(layer_two)a.insert(a.end(),std::begin(kLayerTwo),std::end(kLayerTwo));
    return a;
}
std::vector<std::wstring> Exe(std::vector<std::wstring> args){args.insert(args.begin(),L"--make-fixed-composite-condition-v3");return args;}
const fs::path kRevisions=std::string(m2::kFixedCompositeRevisionFolderName);

// ---- the argument reader ----
void Arguments(){
    const fs::path folder="C:/settings";
    const auto parse=[](std::vector<std::wstring> a){return px::ParseFixedConditionV3Arguments(a);};
    // Every kind with its arguments.
    auto p=parse(Created(folder));
    CHECK(p.request && p.error.empty() && p.request->kind==Kind::created && p.request->settings_folder==folder && p.request->profile==profile_path);
    CHECK(p.request && p.request->region_um==std::array<long long,4>({580000,100000,598000,108000}) && p.request->dpi==254 && !p.request->dry_run);
    CHECK(p.request && p.request->layer_two && p.request->layer_two->falloff_px==64 && p.request->layer_two->maximum_strain==0.5);
    p=parse(Created(folder,true,false));
    CHECK(p.request && p.request->dry_run && !p.request->layer_two);
    p=parse(WithFolder(L"migrated-from-v2",folder,{L"--v2-condition",L"C:/moved.json"}));
    CHECK(p.request && p.request->kind==Kind::migrated_from_v2 && p.request->v2_condition==fs::path("C:/moved.json"));
    p=parse(WithFolder(L"values",folder));
    CHECK(p.request && p.request->kind==Kind::values);
    p=parse(WithFolder(L"manual-adjustment",folder,{L"--seam-dx-px",L"-12.5",L"--seam-dy-px",L"0",L"--rotation-deg",L"1e-1"}));
    CHECK(p.request && p.request->entered && p.request->entered->seam_dx_px==-12.5 && p.request->entered->seam_dy_px==0 && p.request->entered->rotation_deg==0.1);
    // A very large entry is not refused by the reader (the owner's decision 9: no upper bound on a manual adjustment).
    p=parse(WithFolder(L"manual-adjustment",folder,{L"--seam-dx-px",L"100000",L"--seam-dy-px",L"-100000",L"--rotation-deg",L"90"}));
    CHECK(p.request && p.request->entered->seam_dx_px==100000);
    p=parse(WithFolder(L"rollback",folder,{L"--restore-sequence",L"2"}));
    CHECK(p.request && p.request->kind==Kind::rollback && p.request->restore_sequence==2);
    p=parse(WithFolder(L"saved-estimate",folder,{L"--report",L"C:/report.json"}));
    CHECK(p.request && p.request->kind==Kind::saved_estimate && p.request->report==fs::path("C:/report.json"));
    // Refusals of the reader: each gives a reason and no request.
    const auto refused=[&](std::vector<std::wstring> a){const auto r=parse(std::move(a));return !r.request && !r.error.empty();};
    CHECK(refused({}));
    CHECK(refused(W({L"unknown",L"--settings-folder",L"x"})));
    CHECK(refused(W({L"created"})));
    CHECK(refused(W({L"values"})));                                            // no settings folder
    CHECK(refused(W({L"values",L"--settings-folder"})));                        // a flag without its value
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--settings-folder",L"b"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--dry-run",L"--dry-run"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--profile",L"p"})));   // a flag of another kind
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--bogus",L"1"})));
    auto base=Created(folder);
    for(const std::wstring& flag:{std::wstring(L"--profile"),std::wstring(L"--region-um"),std::wstring(L"--dpi")}){   // each needed flag, removed
        auto a=base;const auto at=std::find(a.begin(),a.end(),flag);a.erase(at,at+2);CHECK(refused(a));
    }
    for(const std::wstring region:{L"1,2,3",L"1,2,3,4,5",L"1,2,3,x",L"1,2,3,",L",1,2,3",L"-1,2,3,4",L"1,2,3,1234567890123",L"1.5,2,3,4"}){
        auto a=base;*(std::find(a.begin(),a.end(),L"--region-um")+1)=region;CHECK(refused(a));
    }
    for(const std::wstring dpi:{L"",L"x",L"-1",L"1.5",L"123456",L"0x10"}){auto a=base;*(std::find(a.begin(),a.end(),L"--dpi")+1)=dpi;CHECK(refused(a));}
    CHECK(refused(WithFolder(L"migrated-from-v2",folder)));                        // needs --v2-condition
    CHECK(refused(WithFolder(L"rollback",folder)));                                // needs --restore-sequence
    for(const std::wstring n:{L"0",L"-1",L"x",L"1.5",L"1234567",L""})CHECK(refused(WithFolder(L"rollback",folder,{L"--restore-sequence",n})));
    CHECK(refused(WithFolder(L"saved-estimate",folder)));                          // needs --report
    // manual-adjustment: all three, each a finite number.
    CHECK(refused(WithFolder(L"manual-adjustment",folder,{L"--seam-dx-px",L"1",L"--seam-dy-px",L"1"})));
    for(const std::wstring n:{L"",L"x",L"nan",L"inf",L"-inf",L"1x",L"+1",L" 1",L"1 ",L"1,5",L"1e999"})
        CHECK(refused(WithFolder(L"manual-adjustment",folder,{L"--seam-dx-px",n,L"--seam-dy-px",L"1",L"--rotation-deg",L"1"})));
    // Layer 2: both or neither, positive numbers only.
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--layer-two-falloff-px",L"64"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--layer-two-max-strain",L"0.5"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--layer-two-falloff-px",L"0",L"--layer-two-max-strain",L"0.5"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--layer-two-falloff-px",L"64",L"--layer-two-max-strain",L"-1"})));
    CHECK(refused(W({L"values",L"--settings-folder",L"a",L"--layer-two-falloff-px",L"64",L"--layer-two-max-strain",L"0.5",L"--layer-two-max-strain",L"0.5"})));
    // Non-ASCII where a number or kind goes.
    CHECK(refused(W({L"\u4f5c\u308b",L"--settings-folder",L"a"})));
}

// ---- the policy template of docs/examples ----
void Template(){
    const auto text=ReadAll(example_policy);
    CHECK(!text.empty() && text.size()<=px::kMaximumRegistrationPolicyBytes);
    px::RegistrationPolicyV1 policy;
    try{policy=px::ParseRegistrationPolicyV1(text);}catch(const std::exception& e){Check(false,std::string("the template does not parse: ")+e.what());return;}
    const auto& r=policy.registration;
    // The owner's values (decisions-417: 1, 2, 3, 6, 9, 10) and the lines of the design.
    CHECK(r.envelope.position_mm==10 && r.envelope.rotation_deg==2 && r.envelope.tilt_deg==1 && r.envelope.height_percent==1);
    CHECK(r.surface_envelope && r.surface_envelope->height_mm==40 && r.surface_envelope->tilt_deg==8);
    CHECK(r.parallax && r.parallax->max_lift_mm==1.0);
    CHECK(r.apply_beyond_envelope && *r.apply_beyond_envelope);
    CHECK(!r.surface_scale_warn_percent && !r.live && !policy.revision_rules.manual_adjustment_max_seam_px);
    CHECK(policy.seam_gate.limit_px==1.0 && policy.seam_gate.guard_band_px==0.2);
    // Strict reader: a changed schema, a missing member, an extra member, a set manual limit are all handled as the reader says.
    CHECK(!Contains(text,"scaleAutoCorrection"));
    bool threw=false;
    try{(void)px::ParseRegistrationPolicyV1(Replace(text,"a0.wpd-registration-policy.v1","a0.wpd-registration-policy.v2"));}catch(const std::exception&){threw=true;}
    CHECK(threw);
    threw=false;
    try{(void)px::ParseRegistrationPolicyV1(Replace(text,"\"live\": null","\"live\": null,\n    \"extra\": 1"));}catch(const std::exception&){threw=true;}
    CHECK(threw);
}

// ---- the report, in process ----
void InProcess(){
    const auto root=NewFolder("inprocess");
    WriteAll(root/"wpd-registration-policy.json",Policy());
    const auto policy_hash=HashOf(Policy());
    const auto run=[&](const std::vector<std::wstring>& a,std::chrono::seconds interval=0s){
        std::ostringstream out;const int code=px::RunFixedConditionV3Command(a,out,kHash,kNow,interval);return std::make_pair(code,out.str());
    };
    // Wrong arguments: exit 2, a reason and the usage, nothing written.
    auto [code,text]=run(W({L"bogus"}));
    CHECK(code==2 && Contains(text,"fixed_composite_condition_v3_made=0") && Contains(text,"error=") && Contains(text,"usage: StillReview --make-fixed-composite-condition-v3") && Listing(root).size()==1);
    // A dry run: exit 0, the facts, nothing written; no record hash because there is no file.
    std::tie(code,text)=run(Created(root,true));
    CHECK(code==0 && Contains(text,"fixed_composite_condition_v3_made=0 checked=1 dry_run=1 camera_operations=0") && Contains(text,"revision=1 kind=created") &&
          Contains(text,"policy_sha256="+policy_hash) && !Contains(text,"condition_sha256=") && !Contains(text,"revision_file=") && Listing(root).size()==1);
    CHECK(Contains(text,"check_started=1") && Contains(text,"check_seconds=") && Contains(text,"k_px_per_mm="));
    // Made: exit 0, the revision, its file name, the hash of the file as written, the hash of the policy.
    std::tie(code,text)=run(Created(root));
    const auto file=root/kRevisions/"r000001.json";
    CHECK(code==0 && Contains(text,"fixed_composite_condition_v3_made=1 checked=1 dry_run=0 camera_operations=0") && Contains(text,"revision=1 kind=created") && Contains(text,"revision_file=r000001.json"));
    CHECK(fs::exists(file) && Contains(text,"condition_sha256="+HashOf(ReadAll(file))) && Contains(text,"policy_sha256="+policy_hash) && !Contains(text,"parent_sha256="));
    // The output carries no path and no identifier of the rig: not the folder, not the temporary parent, not the profile copy's name.
    const auto profile_copy=[&]{for(const auto& n:Listing(root))if(n.starts_with("wpd-fixed-composite-profile-"))return n;return std::string();}();
    CHECK(!profile_copy.empty() && !Contains(text,root.string()) && !Contains(text,TempParent().string()) && !Contains(text,profile_copy) && !Contains(text,profile_path.string()));
    CHECK(!Contains(text,"profile_id") && !Contains(text,"C:\\") && !Contains(text,"Users"));
    // Not overwritten: the same command again is refused (exit 1) and the bytes stay.
    const auto before=ReadAll(file);
    std::tie(code,text)=run(Created(root));
    CHECK(code==1 && Contains(text,"fixed_composite_condition_v3_made=0") && Contains(text,"r000001") && ReadAll(file)==before);
    // values with the same policy has nothing to write; a changed one is revision 2 with the parent's hash.
    std::tie(code,text)=run(WithFolder(L"values",root));
    CHECK(code==1 && Contains(text,"書く改訂がありません"));
    WriteAll(root/"wpd-registration-policy.json",Policy(6));
    std::tie(code,text)=run(WithFolder(L"values",root));
    CHECK(code==0 && Contains(text,"revision=2 kind=values") && Contains(text,"parent_sha256="+HashOf(before)) && Contains(text,"policy_sha256="+HashOf(Policy(6))) && Contains(text,"revision_file=r000002.json"));
    // manual-adjustment: neither the size nor a base pose that leaves the pose range is refused when it is written (the owner's decision).
    // The reader that draws with the condition refuses the latter (checked in the tool tests); here the next revision is a rollback.
    std::tie(code,text)=run(WithFolder(L"manual-adjustment",root,{L"--seam-dx-px",L"12.5",L"--seam-dy-px",L"-7.25",L"--rotation-deg",L"0.1"}));
    CHECK(code==0 && Contains(text,"revision=3 kind=manual-adjustment") && Contains(text,"maximum_seam_displacement_px="));
    // rollback to revision 1; a revision that does not exist, or the one in force, is refused.
    std::tie(code,text)=run(WithFolder(L"rollback",root,{L"--restore-sequence",L"1"}));
    CHECK(code==0 && Contains(text,"revision=4 kind=rollback") && Contains(text,"policy_sha256="+policy_hash));
    for(const wchar_t* n:{L"4",L"5",L"999999"}){
        std::tie(code,text)=run(WithFolder(L"rollback",root,{L"--restore-sequence",n}));
        CHECK(code==1 && Listing(root/kRevisions).size()==4);
    }
    // saved-estimate stays refused, with the reason.
    std::tie(code,text)=run(WithFolder(L"saved-estimate",root,{L"--report",(root/"report.json").wstring()}));
    CHECK(code==1 && Contains(text,"saved-estimate"));
    // The layer-2 design constants are needed when the policy enables layer 2; the reason says so, and nothing is written.
    const auto bare=NewFolder("nolayer2");
    WriteAll(bare/"wpd-registration-policy.json",Policy());
    std::tie(code,text)=run(Created(bare,false,false));
    CHECK(code==1 && Contains(text,"L と s_max") && !fs::exists(bare/kRevisions));
    std::error_code ignored;fs::remove_all(root,ignored);fs::remove_all(bare,ignored);
}

// ---- the progress notice ----
void Progress(){
    std::ostringstream quiet;
    {px::CreationCheckProgress progress(quiet,0s);progress.Start();progress.Start();std::this_thread::sleep_for(50ms);progress.Stop();}
    CHECK(quiet.str().find("check_started=1")!=std::string::npos && quiet.str().find("check_running")==std::string::npos);
    std::ostringstream loud;
    {
        px::CreationCheckProgress progress(loud,1s);progress.Start();
        std::this_thread::sleep_for(2500ms);
        progress.Stop();
        CHECK(progress.Seconds()>=2);
    }
    const auto text=loud.str();
    CHECK(text.find("check_started=1")!=std::string::npos && text.find("check_running seconds=1")!=std::string::npos && text.find("check_running seconds=2")!=std::string::npos);
    // After Stop nothing more is written.
    const auto size=loud.str().size();
    std::this_thread::sleep_for(1200ms);
    CHECK(loud.str().size()==size);
}

// ---- the real executable ----
void Executable(){
    CHECK(fs::exists(executable));
    // No arguments after the command, a bad kind: exit 2 and the usage on the output. No window opens (the run ends by itself).
    auto r=RunExe(Exe({}));
    CHECK(r.code==2 && Contains(r.output,"usage: StillReview --make-fixed-composite-condition-v3"));
    r=RunExe(Exe(W({L"nonsense"})));
    CHECK(r.code==2 && Contains(r.output,"error="));
    // The older entries are still there (their usage text and exit code are not changed).
    r=RunExe(W({L"--make-fixed-composite-condition-v2"}));
    CHECK(r.code==2 && Contains(r.output,"fixed_composite_condition_v2_made=0"));

    const auto root=NewFolder("exe");
    WriteAll(root/"wpd-registration-policy.json",Policy());
    // Refusals before anything is written: no profile, a missing policy (another folder), a broken policy.
    const auto empty=NewFolder("exe-nopolicy");
    r=RunExe(Exe(Created(empty)));
    CHECK(r.code==1 && Contains(r.output,"方針ファイル") && Listing(empty).empty());
    const auto broken=NewFolder("exe-broken");
    WriteAll(broken/"wpd-registration-policy.json",Replace(Policy(),"\"registration\":{","\"registration\":{\"extra\":1,"));
    r=RunExe(Exe(Created(broken)));
    CHECK(r.code==1 && Contains(r.output,"方針ファイル") && Listing(broken)==std::set<std::string>{"wpd-registration-policy.json"});
    WriteAll(broken/"wpd-registration-policy.json",Replace(Policy(),"\"positionMm\":5.000000","\"positionMm\":null"));
    r=RunExe(Exe(Created(broken)));
    CHECK(r.code==1 && Listing(broken)==std::set<std::string>{"wpd-registration-policy.json"});
    WriteAll(broken/"wpd-registration-policy.json",Replace(Policy(),"\"seamGate\":{\"limitPx\":1,\"guardBandPx\":0.2},",""));   // a missing member
    r=RunExe(Exe(Created(broken)));
    CHECK(r.code==1 && Listing(broken)==std::set<std::string>{"wpd-registration-policy.json"});
    auto no_profile=Created(root);*(std::find(no_profile.begin(),no_profile.end(),L"--profile")+1)=(root/"absent-profile.json").wstring();
    r=RunExe(Exe(no_profile));
    CHECK(r.code==1 && Contains(r.output,"rig profile") && !fs::exists(root/kRevisions));
    // A profile that does not fit the region (outside B's cover) is refused with the reason.
    auto outside=Created(root);*(std::find(outside.begin(),outside.end(),L"--region-um")+1)=L"100000,100000,110000,108000";
    r=RunExe(Exe(outside));
    CHECK(r.code==1 && Contains(r.output,"rig profile") && !fs::exists(root/kRevisions));

    // A dry run writes nothing; the real run writes r000001 and says so.
    r=RunExe(Exe(Created(root,true)));
    CHECK(r.code==0 && Contains(r.output,"dry_run=1") && !fs::exists(root/kRevisions) && Listing(root).size()==1);
    r=RunExe(Exe(Created(root)));
    const auto first=root/kRevisions/"r000001.json";
    Check(r.code==0 && fs::exists(first),"created: "+r.output);
    CHECK(Contains(r.output,"revision_file=r000001.json") && Contains(r.output,"condition_sha256="+HashOf(ReadAll(first))) && Contains(r.output,"policy_sha256="+HashOf(Policy())));
    CHECK(!Contains(r.output,root.string()) && !Contains(r.output,profile_path.string()) && !Contains(r.output,"wpd-fixed-composite-profile-"));
    {   // The file is a v3 record that the product reader loads, with the profile bound.
        const auto active=m2::LoadActiveFixedCompositeRevisionV3(root/kRevisions,kHash);
        CHECK(active && active->condition.revision.sequence==1 && active->condition.source.policy_sha256==HashOf(Policy()));
    }
    const auto bytes=ReadAll(first);
    r=RunExe(Exe(Created(root)));
    CHECK(r.code==1 && ReadAll(first)==bytes && Listing(root/kRevisions).size()==1);
    // values, manual adjustment, rollback through the executable.
    r=RunExe(Exe(WithFolder(L"values",root)));
    CHECK(r.code==1 && Listing(root/kRevisions).size()==1);   // nothing changed in the policy
    WriteAll(root/"wpd-registration-policy.json",Policy(6));
    r=RunExe(Exe(WithFolder(L"values",root)));
    CHECK(r.code==0 && Contains(r.output,"revision=2 kind=values") && Contains(r.output,"parent_sha256="+HashOf(bytes)));
    r=RunExe(Exe(WithFolder(L"manual-adjustment",root,{L"--seam-dx-px",L"3",L"--seam-dy-px",L"-2",L"--rotation-deg",L"0.05"})));
    CHECK(r.code==0 && Contains(r.output,"revision=3 kind=manual-adjustment"));
    r=RunExe(Exe(WithFolder(L"rollback",root,{L"--restore-sequence",L"1"})));
    CHECK(r.code==0 && Contains(r.output,"revision=4 kind=rollback") && Contains(r.output,"policy_sha256="+HashOf(Policy())));
    r=RunExe(Exe(WithFolder(L"rollback",root,{L"--restore-sequence",L"9"})));
    CHECK(r.code==1 && Listing(root/kRevisions).size()==4);
    for(int i=1;i<=4;++i)CHECK(!ReadAll(root/kRevisions/m2::FixedCompositeRevisionFileName(i)).empty());
    CHECK(ReadAll(first)==bytes);
    // saved-estimate is refused.
    r=RunExe(Exe(WithFolder(L"saved-estimate",root,{L"--report",(root/"r.json").wstring()})));
    CHECK(r.code==1 && Contains(r.output,"saved-estimate"));

    // A range the cameras cannot hold: refused, and the sentence names the component and the end that failed.
    const auto wide=NewFolder("exe-wide");
    WriteAll(wide/"wpd-registration-policy.json",Policy(60,false,false));
    r=RunExe(Exe(Created(wide,false,false)));
    CHECK(r.code==1 && Contains(r.output,"検査に通りませんでした") && !fs::exists(wide/kRevisions));
    CHECK(Contains(r.output,"end of its range") || Contains(r.output,"at the vertex"));
    CHECK(Contains(r.output,"x position") || Contains(r.output,"y position"));
    CHECK(!Contains(r.output,wide.string()));

    // Migration: a v2 record that a person moved out of the way is the source; the v2 file in place stops it.
    const auto mig=NewFolder("exe-migrate");
    WriteAll(mig/"wpd-registration-policy.json",Policy(5,false,false));
    m2::FixedCompositeV2Request v2_request;
    v2_request.profile_file=m2::FixedCompositeProfileFileName(HashOf(draft));v2_request.profile_bytes=draft;
    v2_request.raster=a0::m2::render::OutputRaster{a0::m2::render::DocumentRegionUm{580000,100000,598000,108000},254,180,80};
    v2_request.scale_auto_correction_max_percent=1;v2_request.seam_column=90;v2_request.orientation=m2::MountOrientation::normal;
    const auto v2=m2::MakeFixedCompositeConditionV2(v2_request,kNow,kHash,[](const m2::FixedCompositeConditionV2&,const m2::FixedCompositeProfileBinding&){});
    const auto v2_text=m2::FormatFixedCompositeConditionV2(v2)+"\n";
    (void)m2::CopyFixedCompositeProfile(mig,draft,kHash);
    WriteAll(mig/m2::kFixedCompositeConditionName,v2_text);
    auto migrate=WithFolder(L"migrated-from-v2",mig,{L"--v2-condition",(mig/"moved-v2.json").wstring()},false);
    r=RunExe(Exe(migrate));
    CHECK(r.code==1 && !fs::exists(mig/kRevisions));                        // the moved file is absent
    r=RunExe(Exe(WithFolder(L"migrated-from-v2",mig,{L"--v2-condition",(mig/m2::kFixedCompositeConditionName).wstring()},false)));
    CHECK(r.code==1 && Contains(r.output,"既にあります") && !fs::exists(mig/kRevisions));   // the v2 file still in place
    fs::rename(mig/m2::kFixedCompositeConditionName,mig/"moved-v2.json");
    r=RunExe(Exe(migrate));
    Check(r.code==0,"migrated: "+r.output);
    CHECK(Contains(r.output,"revision=1 kind=migrated-from-v2") && Contains(r.output,"revision_file=r000001.json") && Contains(r.output,"policy_sha256="+HashOf(Policy(5,false,false))));
    CHECK(ReadAll(mig/"moved-v2.json")==v2_text);   // the moved v2 file is read, never changed
    r=RunExe(Exe(migrate));
    CHECK(r.code==1 && Listing(mig/kRevisions).size()==1);

    std::error_code ignored;
    for(const auto& d:{root,empty,broken,wide,mig})fs::remove_all(d,ignored);
}
}

int main(int argc,char* argv[]){
    try{
        if(argc!=4)throw std::invalid_argument("expected: the rig profile v2 fixture folder, the StillReview executable, the policy file template");
        profile_path=fs::path(argv[1])/"calibrated-draft.json";
        executable=fs::absolute(fs::path(argv[2])).lexically_normal();example_policy=argv[3];
        draft=ReadAll(profile_path);
        if(draft.empty())throw std::runtime_error("fixture profile is missing");
        const auto step=[](const char* name){std::cerr<<"["<<name<<"]"<<std::endl;};
        step("arguments");Arguments();step("template");Template();step("progress");Progress();step("in process");InProcess();step("executable");Executable();
    }catch(const std::exception& e){++failures;std::cerr<<"unexpected exception: "<<e.what()<<'\n';}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
