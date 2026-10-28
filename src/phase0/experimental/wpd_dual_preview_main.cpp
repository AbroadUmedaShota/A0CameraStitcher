// Development-only WPD A/B preview, private still capture and uncalibrated C.
#include "a0/phase0/experimental/d810_preview.hpp"
#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include "a0/phase0/experimental/preview_back_buffer.hpp"
#include "a0/phase0/experimental/preview_focus.hpp"
#include "a0/phase0/experimental/preview_shutdown.hpp"
#include "a0/phase0/experimental/preview_lifecycle.hpp"
#include "a0/phase0/experimental/wpd_still_capture.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/wpd_transport.hpp"
#include "a0/phase0/pc_direct_capture.hpp"
#include <wincodec.h>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <fstream>
#include <future>
#include <memory>
#include <algorithm>
#include <cmath>
#include <numbers>
#include "a0/phase0/experimental/preview_merge.hpp"
#include "a0/phase0/experimental/preview_mount_settings.hpp"
#include "a0/phase0/experimental/preview_merge_policy.hpp"
#include "a0/phase0/experimental/still_alignment_text.hpp"
#include "a0/m2/experimental/overlap_estimation.hpp"
#include "a0/phase0/experimental/still_pair.hpp"
#include "a0/phase0/experimental/wpd_still_pair.hpp"
#include "a0/phase0/experimental/pair_cycle_probe.hpp"
#include "a0/phase0/experimental/preview_end_restart_probe.hpp"
#include "a0/m2/experimental/still_review.hpp"

using namespace a0::phase0;
using namespace a0::phase0::experimental;
using Microsoft::WRL::ComPtr;
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> stopping{};
std::atomic<bool> camera_end_observed{};
std::atomic<bool> capture_stop_requested{};
std::atomic<unsigned> ready_count{};
bool cleanup_only=false;
std::mutex output_mutex;
// Both camera Live Views remain active. Serialize WPD calls (not sessions or
// whole focus operations) while investigating the shared-driver I/O failures.
std::mutex wpd_io_mutex;
std::mutex recovery_workflow_mutex;
PreviewShutdownStages shutdown_stages;
std::uint64_t log_sequence{};
void Log(const std::string& value){
    // Sample and emit under the same lock so concurrent workers have one
    // receipt order. UTC is for OS-event correlation, never deadline logic.
    std::lock_guard lock(output_mutex);
    const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout<<value<<" log_sequence="<<++log_sequence<<" log_mono_ms="<<GetTickCount64()<<" log_utc_unix_ms="<<utc<<std::endl;
}
void Check(HRESULT hr,const char* step){if(hr!=S_OK){std::ostringstream s;s<<step<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr);throw std::runtime_error(s.str());}}
struct ComScope {ComScope(){Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM initialize");}~ComScope(){CoUninitialize();}};
WpdValues Values(){WpdValues v;Check(CoCreateInstance(CLSID_PortableDeviceValues,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&v)),"values");return v;}
ComPtr<IPortableDevice> Open(const std::wstring& id,DWORD access){
    ComPtr<IPortableDevice> device;Check(CoCreateInstance(CLSID_PortableDeviceFTM,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&device)),"device");
    auto client=Values();Check(client->SetStringValue(WPD_CLIENT_NAME,L"A0 WPD Dual Preview"),"client name");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION,0),"client version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION,1),"client version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION,0),"client version");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS,access),"client access");
    Check(client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE,SECURITY_IMPERSONATION),"client security");
    Check(device->Open(id.c_str(),client.Get()),"open");return device;
}
std::string Identity(IPortableDevice* device){
    ComPtr<IPortableDeviceContent> content;ComPtr<IPortableDeviceProperties> properties;
    ComPtr<IPortableDeviceKeyCollection> keys;WpdValues values;
    Check(device->Content(&content),"content");Check(content->Properties(&properties),"properties");
    Check(CoCreateInstance(CLSID_PortableDeviceKeyCollection,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&keys)),"keys");
    Check(keys->Add(WPD_DEVICE_SERIAL_NUMBER),"identity key");
    Check(properties->GetValues(WPD_DEVICE_OBJECT_ID,keys.Get(),&values),"identity read");
    PWSTR raw{};Check(values->GetStringValue(WPD_DEVICE_SERIAL_NUMBER,&raw),"identity value");
    std::wstring serial=raw?raw:L"";CoTaskMemFree(raw);
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,serial.data(),static_cast<int>(serial.size()),nullptr,0,nullptr,nullptr);
    if(n<=0 || n>512)throw std::runtime_error("invalid identity");
    std::string utf8(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,serial.data(),static_cast<int>(serial.size()),utf8.data(),n,nullptr,nullptr);
    return DeriveWpdStableIdentity(utf8);
}
std::array<std::wstring,2> Discover(){
    auto path=DefaultIdentityMapPath();path.replace_filename("camera-map-wpd.json");IdentityMap map(path);
    if(!map.FindIdentity("CAM-A") || !map.FindIdentity("CAM-B"))throw std::runtime_error("registered A/B map required");
    ComPtr<IPortableDeviceManager> manager;Check(CoCreateInstance(CLSID_PortableDeviceManager,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager)),"manager");
    Check(manager->RefreshDeviceList(),"refresh");DWORD count{};Check(manager->GetDevices(nullptr,&count),"device count");
    if(count>32)throw std::runtime_error("device count limit");
    std::vector<PWSTR> ids(count,nullptr);
    struct Free {std::vector<PWSTR>& ids;~Free(){for(auto id:ids)CoTaskMemFree(id);}} free{ids};
    Check(manager->GetDevices(ids.data(),&count),"devices");std::array<std::wstring,2> result{};unsigned matched=0;
    for(DWORD i=0;i<count;++i){
        DWORD n{};if(manager->GetDeviceFriendlyName(ids[i],nullptr,&n)!=S_OK || n==0 || n>4096)continue;
        std::wstring name(n,L'\0');Check(manager->GetDeviceFriendlyName(ids[i],name.data(),&n),"friendly name");
        if(name.find(L"D810")==std::wstring::npos)continue;
        auto device=Open(ids[i],GENERIC_READ);std::string identity;
        try{identity=Identity(device.Get());}catch(...){device->Close();throw;}
        Check(device->Close(),"discovery close");auto alias=map.FindAlias(identity);
        if(!alias || (*alias!="CAM-A" && *alias!="CAM-B"))throw std::runtime_error("unbound D810");
        auto& slot=result[*alias=="CAM-A"?0:1];if(!slot.empty())throw std::runtime_error("duplicate binding");slot=ids[i];++matched;
    }
    if(matched!=2){Log("inventory bound_A="+std::to_string(!result[0].empty())+" bound_B="+std::to_string(!result[1].empty()));throw std::runtime_error("exactly two registered D810 required");}
    Log("inventory bound_A=1 bound_B=1 SDK=0");return result;
}
using StillCaptureOutcome=VerifiedStillOriginal;
// One body's WPD still session: open, the exposure steps, close. A single
// capture runs them back to back; a pair transaction (#389) interleaves the two
// bodies' steps so both shutters precede either import. Steps do not throw: a
// failure is recorded in the outcome and logged once, and Finish() always closes
// the session and the private partial. Not movable: the channel and watchdog
// refer to this object.
class StillLane final {
public:
    StillLane(std::wstring id,std::string alias,Clock::time_point outer_deadline,bool preflight_only):
        id_(std::move(id)),alias_(std::move(alias)),preflight_only_(preflight_only),
        deadline_(std::min(outer_deadline,Clock::now()+std::chrono::seconds(180))),
        evidence_(std::in_place,DefaultIdentityMapPath().parent_path().parent_path()/(preflight_only?"wpd-preflights":"wpd-captures"),NewRunId(),"wpd-public-ptp-sdram-development"){
        partial_path_=evidence_->RunRoot()/(alias_+"-transport.jpg.partial");
    }
    StillLane(const StillLane&)=delete;
    StillLane& operator=(const StillLane&)=delete;
    ~StillLane(){try{Finish();}catch(...){}}
    bool OpenSession(){
        try{
            device_=Open(id_,GENERIC_READ|GENERIC_WRITE);opened_=true;
            auto map_path=DefaultIdentityMapPath();map_path.replace_filename("camera-map-wpd.json");IdentityMap map(map_path);
            if(map.FindAlias(Identity(device_.Get()))!=std::optional<std::string>(alias_))throw std::runtime_error("still camera alias mismatch");
            if(!preflight_only_){
                partial_=CreateFileW(partial_path_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr);
                if(partial_==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create private capture partial");
            }
            watchdog_=std::jthread([this](std::stop_token stop){
                const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                if(FAILED(hr)){cancelled_=true;Log(alias_+" capture watchdog COM initialization failed");return;}
                while(!stop.stop_requested()){
                    if(busy_ && GetTickCount64()-busy_tick_.load()>12000 && !cancelled_.exchange(true)){
                        Log(alias_+" capture watchdog cancelling stalled I/O; lease retained");device_->Cancel();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                CoUninitialize();
            });
            channel_=std::make_unique<Channel>(*this);
            evidence_->RecordState(preflight_only_?"preflight":"capture","BothLiveViewsClosed",alias_);
            return true;
        }catch(const std::exception& error){Fail(error.what(),dynamic_cast<const WpdStillOutOfFocus*>(&error)!=nullptr);return false;}
    }
    // Non-exposing check: Ready, capture profile and a clean event baseline.
    void RunPreflightOnly(){
        try{
            Log(alias_+" preflight_only=1 shutter_commands=0 setting_writes=0");
            ReadWpdStillPreflight(*channel_,[this]{return Admitted();});outcome_.success=true;
        }catch(const std::exception& error){Fail(error.what(),dynamic_cast<const WpdStillOutOfFocus*>(&error)!=nullptr);}
    }
    // Recheck the capture state of this open body. No exposure.
    StillPairPrepared PrepareForExposure(){
        StillPairPrepared prepared;
        try{
            acquisition_entered_=true;
            run_.emplace(*channel_,[this]{return Admitted();},[this](std::span<const unsigned char> bytes){
                DWORD written{};if(!WriteFile(partial_,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size())throw std::runtime_error("capture partial write failed");
            },&observed_);
            prepared=PrepareWpdStillRun(*run_);
        }catch(const std::exception& error){prepared.error=error.what();}
        if(!prepared.ready)Fail(prepared.error,prepared.out_of_focus);
        return prepared;
    }
    StillShutterOutcome SendShutter(){
        auto outcome=SendWpdStillShutter(*run_);
        // The exchange refuses a stop that raced the admission check before it
        // reaches the transport; the dispatched flag is the truth for NotSent.
        if(!shutter_dispatched_)outcome.state=StillShutterState::NotSent;
        return outcome;
    }
    // After an uncertain shutter: private partial only, never an original.
    void Rescue(){if(run_)run_->RetainUncertainShutterCandidate();}
    StillCaptureOutcome Collect(){
        try{
            auto payload=run_->Collect();
            if(!FlushFileBuffers(partial_))throw std::runtime_error("capture partial flush failed");
            CloseHandle(partial_);partial_=INVALID_HANDLE_VALUE;
            if(!IsValidJpeg(payload.jpeg))throw std::runtime_error("captured bytes are not a valid JPEG");
            (void)FullyDecodeJpeg(payload.jpeg,payload.info.width,payload.info.height);
            const auto frame=evidence_->PersistExactlyOne("capture",alias_,{{"camera-original.jpg",std::move(payload.jpeg),true,{}}},deadline_);
            if(!frame.success)throw std::runtime_error("PC original persistence failed: "+frame.error_category);
            outcome_.success=true;outcome_.path=frame.path;outcome_.bytes=frame.bytes;outcome_.sha256=frame.sha256;
            Log(alias_+" original_saved=1 bytes="+std::to_string(frame.bytes)+" sha256="+frame.sha256);
            std::wcout<<L"saved_original="<<frame.path.wstring()<<std::endl;
        }catch(const std::exception& error){Fail(error.what(),dynamic_cast<const WpdStillOutOfFocus*>(&error)!=nullptr);}
        return outcome_;
    }
    // One body, one shutter, then its import: the single-capture path.
    void CaptureSingle(){
        if(!PrepareForExposure().ready)return;
        const auto shutter=SendShutter();
        if(shutter.state!=StillShutterState::Sent){
            if(shutter.state==StillShutterState::Uncertain)Rescue();
            Fail(shutter.error,false);return;
        }
        Collect();
    }
    const std::string& Error()const{return outcome_.error;}
    // Always closes the session and the partial. `external_error` names a failure
    // decided outside this lane (e.g. its shutter was never sent) so the log and
    // the journal state agree with the pair result.
    StillCaptureOutcome Finish(const std::string& external_error={}){
        if(finished_)return outcome_;
        finished_=true;
        if(!outcome_.success && outcome_.error.empty() && !external_error.empty())Fail(external_error,false);
        // One line per capture, success or failure: was this capture's completion
        // event read before the session closed (#381). Not a success condition.
        if(acquisition_entered_){
            Log(alias_+" capture_completion "+DescribeWpdStillCaptureObservation(observed_));
            if(observed_.shutter_attempted)Log(alias_+" completion_wait "+DescribeWpdStillCompletionWait(observed_));
        }
        if(partial_!=INVALID_HANDLE_VALUE){CloseHandle(partial_);partial_=INVALID_HANDLE_VALUE;}
        if(opened_){
            busy_tick_=GetTickCount64();busy_=true;const auto hr=device_->Close();busy_=false;
            outcome_.closed=hr==S_OK;opened_=false;
            if(!outcome_.closed){outcome_.success=false;outcome_.error+="; still session close failed";}
        }
        if(watchdog_.joinable()){watchdog_.request_stop();watchdog_.join();}
        if(Clock::now()>=deadline_){outcome_.success=false;outcome_.error+="; transaction deadline exceeded; PC bytes retained";}
        if(outcome_.success)evidence_->RecordState(preflight_only_?"preflight":"capture","Complete",alias_);
        else evidence_->RecordState(preflight_only_?"preflight":"capture","FailedPartial",alias_,outcome_.error);
        Log(alias_+(preflight_only_?" preflight_session_closed=":" capture_session_closed=")+std::to_string(outcome_.closed)+
            (preflight_only_?" preflight_ready=":" capture_complete=")+std::to_string(outcome_.success));
        return outcome_;
    }
private:
    class Channel final:public WpdStillCaptureChannel {
    public:
        explicit Channel(StillLane& lane):
            WpdStillCaptureChannel([&lane]{return lane.Admitted();},[&lane]{return lane.opened_;}),lane_(lane){}
    private:
        WpdLiveViewExchange Exchange(IPortableDeviceValues* request)override{return lane_.Send(request);}
        StillLane& lane_;
    };
    // A dispatched shutter keeps its import admitted after a stop request; an
    // undispatched one does not. Nothing is admitted after cancel or deadline.
    bool Admitted()const{return !cancelled_ && Clock::now()<deadline_ && (shutter_dispatched_ || !capture_stop_requested);}
    void Fail(const std::string& error,bool out_of_focus){
        outcome_.error=error;outcome_.success=false;outcome_.out_of_focus=out_of_focus;
        Log(alias_+(preflight_only_?" preflight_failure: ":" capture_failure: ")+error);
    }
    WpdLiveViewExchange Send(IPortableDeviceValues* request){
        if(preflight_only_ && !AllowedWpdStillRequest(request,false,{}))throw std::runtime_error("preflight forbids shutter/object request");
        DWORD code{};request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&code);
        if(code==0x90c0){
            if(capture_stop_requested)throw std::runtime_error("capture stopped before shutter dispatch");
            shutter_dispatched_=true;Log(alias_+" shutter_no_af_once=1 card_write_command=0");
        }
        busy_tick_=GetTickCount64();busy_=true;WpdLiveViewExchange reply;reply.status=device_->SendCommand(0,request,&reply.results);busy_=false;
        if(code==0x90c0){auto status=InspectWpdLiveViewCommand(reply.status,reply.results.Get(),true);std::ostringstream line;
            line<<alias_<<" shutter_result_error="<<static_cast<int>(status.error)<<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(status.dispatch_status)<<" command=0x"<<static_cast<unsigned long>(status.command_status)<<" mtp=0x"<<status.mtp_response;Log(line.str());}
        return reply;
    }
    std::wstring id_;std::string alias_;bool preflight_only_{};Clock::time_point deadline_;
    std::optional<EvidenceWriter> evidence_;
    std::filesystem::path partial_path_;
    ComPtr<IPortableDevice> device_;
    HANDLE partial_{INVALID_HANDLE_VALUE};
    bool opened_{},shutter_dispatched_{},acquisition_entered_{},finished_{};
    std::atomic<bool> cancelled_{},busy_{};std::atomic<ULONGLONG> busy_tick_{};
    WpdStillCaptureObservation observed_;
    StillCaptureOutcome outcome_;
    std::unique_ptr<Channel> channel_;
    std::optional<WpdStillCaptureRun> run_;
    std::jthread watchdog_; // Last: joined first, before the state it reads goes away.
};
StillCaptureOutcome RunStillOnce(const std::wstring& id,const std::string& alias,bool both_live_sessions_closed,
    Clock::time_point outer_deadline,bool preflight_only){
    StillCaptureOutcome outcome;
    if(!both_live_sessions_closed){outcome.error="both Live View sessions must be closed before capture";return outcome;}
    const auto deadline=std::min(outer_deadline,Clock::now()+std::chrono::seconds(180));
    if(Clock::now()>=deadline || capture_stop_requested){outcome.error="capture deadline/stop before open";return outcome;}
    StillLane lane(id,alias,outer_deadline,preflight_only);
    if(lane.OpenSession()){
        if(preflight_only)lane.RunPreflightOnly();else lane.CaptureSingle();
    }
    return lane.Finish();
}
StillCaptureOutcome SaveStillOnce(const std::wstring& id,const std::string& alias,bool both_live_sessions_closed,
    Clock::time_point deadline=Clock::time_point::max()){
    return RunStillOnce(id,alias,both_live_sessions_closed,deadline,false);
}
VerifiedStillPreflight CheckStillOnce(const std::wstring& id,const std::string& alias,Clock::time_point deadline){
    const auto result=RunStillOnce(id,alias,true,deadline,true);
    return {result.success,result.closed,result.error,result.out_of_focus};
}
// A and B as one 180 s transaction (#389): both bodies stay open from the
// exposure recheck until both imports are saved. Closing A or opening B between
// the shutters would put the slow open/preflight/import back into the gap.
VerifiedStillPair CaptureStillPairShutterFirst(const std::array<std::wstring,2>& ids,Clock::time_point deadline){
    std::array<std::unique_ptr<StillLane>,2> lanes;
    const auto alias=[](int i){return i==0?"CAM-A":"CAM-B";};
    StillPairSteps steps;
    steps.preflight=[&](int i,Clock::time_point end){return CheckStillOnce(ids[i],alias(i),end);};
    steps.prepare=[&](int i,Clock::time_point end){
        if(i==0)Log("pair_preflight_both_ready=1 both_closed=1");
        lanes[i]=std::make_unique<StillLane>(ids[i],alias(i),end,false);
        if(!lanes[i]->OpenSession())return StillPairPrepared{false,lanes[i]->Error(),false};
        return lanes[i]->PrepareForExposure();
    };
    steps.shutter=[&](int i){return lanes[i]->SendShutter();};
    steps.collect=[&](int i){return lanes[i]->Collect();};
    steps.rescue=[&](int i){lanes[i]->Rescue();};
    steps.close=[&](int i,VerifiedStillOriginal& original){if(lanes[i])original=lanes[i]->Finish(original.error);};
    steps.log=[](const std::string& line){Log(line);};
    return CaptureStillPairShutterFirstOnce(steps,[]{return !capture_stop_requested.load();},deadline);
}
struct Pixels {UINT width{},height{};std::vector<BYTE> bytes;};
Pixels Decode(IWICImagingFactory* factory,std::span<const unsigned char> jpeg){
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateStream(&stream),"WIC stream");
    Check(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()),static_cast<DWORD>(jpeg.size())),"WIC memory");
    Check(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder),"JPEG decode");
    GUID format{};Check(decoder->GetContainerFormat(&format),"image format");if(format!=GUID_ContainerFormatJpeg)throw std::runtime_error("not JPEG");
    Check(decoder->GetFrame(0,&frame),"JPEG frame");Pixels p;Check(frame->GetSize(&p.width,&p.height),"JPEG dimensions");
    if(!p.width || !p.height || p.width>2048 || p.height>2048)throw std::runtime_error("preview dimensions exceeded");
    Check(factory->CreateFormatConverter(&converter),"WIC converter");
    Check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"WIC convert");
    p.bytes.resize(static_cast<size_t>(p.width)*p.height*4);Check(converter->CopyPixels(nullptr,p.width*4,static_cast<UINT>(p.bytes.size()),p.bytes.data()),"JPEG pixels");return p;
}
struct Camera final:WpdLiveViewCommandChannel {
    std::string alias;std::wstring id;std::mutex mutex;ComPtr<IPortableDevice> device;
    Pixels pixels;std::uint64_t frames{},painted{},changed{},last_hash{},paint_sequence{};
    std::optional<D810LevelSample> level;
    std::uint64_t level_frames{},level_missing{},level_painted{},level_paint_sequence{};
    PreviewFocusMailbox focus_mailbox;
    std::wstring focus_status{L"AF / 微調整: 準備中"};
    std::wstring failure_status{L"映像取得・制御に失敗しました"};
    std::array<unsigned,3> focus_success{};unsigned focus_requests{};
    DWORD focus_mode{0xffffffff};
    Clock::time_point first{},last{},busy_since{};std::atomic<bool> done{},failed{};
    Clock::time_point send_deadline{Clock::time_point::max()};
    bool admission_cancelled{},frame_dispatched{};
    std::optional<WpdLiveViewCommandResult> start_result;
    bool busy{},cancelled{},opened{},start_attempted{},started{},stopped{},closed{},live_off_confirmed{},camera_ended{};
    WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) override {
        if(!AllowedD810PreviewRequest(request))throw std::runtime_error("request rejected by allowlist");
        std::lock_guard io_lock(wpd_io_mutex);
        DWORD operation{};request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&operation);
        if(!AdmitD810PreviewDispatch(request,opened && !closed,stopping,Clock::now()>=send_deadline)){
            if(stopping)admission_cancelled=true;
            throw std::runtime_error("WPD request not admitted after I/O wait");
        }
        ComPtr<IPortableDevice> current;
        {std::lock_guard lock(mutex);current=device;busy=true;busy_since=Clock::now();cancelled=false;}
        if(operation==0x9203)frame_dispatched=true;
        if(operation==0x9201 || operation==0x9202){std::ostringstream event;
            event<<alias<<" transport_control_dispatch_begin=1 opcode=0x"<<std::hex<<operation;Log(event.str());}
        WpdLiveViewExchange r;r.status=current->SendCommand(0,request,&r.results);
        {std::lock_guard lock(mutex);busy=false;}
        // No per-frame success trace or transfer context/payload dump. Capture
        // the first failing dispatch boundary before later cleanup obscures it.
        const auto status=InspectWpdLiveViewCommand(r.status,r.results.Get(),false);
        if(!status.Ok()){
            DWORD command_id{};request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&command_id);
            std::ostringstream event;event<<alias<<" transport_command_failed=1 command_id="<<command_id
                <<" opcode=0x"<<std::hex<<operation<<" dispatch=0x"<<static_cast<unsigned long>(status.dispatch_status)
                <<" command=0x"<<static_cast<unsigned long>(status.command_status)<<" mtp=0x"<<status.mtp_response
                <<" error="<<std::dec<<static_cast<int>(status.error);Log(event.str());
        }
        DWORD opcode{};
        if(request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE,&opcode)==S_OK && (opcode==0x9203 || opcode==0x1015)){
            auto begin=InspectWpdLiveViewTransferBegin(r.status,r.results.Get());
            if(!begin.result.Ok() && r.results){
                PROPVARIANT size{},hint{};
                const auto size_hr=r.results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE,&size);
                const auto hint_hr=r.results->GetValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE,&hint);
                std::ostringstream info;info<<alias<<" read_opcode=0x"<<std::hex<<opcode<<" begin_error="<<std::dec<<static_cast<int>(begin.result.error)<<" has_context="<<!begin.context.empty()
                    <<" size_hr=0x"<<std::hex<<static_cast<unsigned long>(size_hr)<<" size_type="<<std::dec<<size.vt
                    <<" size="<<(size.vt==VT_UI8?size.uhVal.QuadPart:0)<<" hint_hr=0x"<<std::hex<<static_cast<unsigned long>(hint_hr)
                    <<" hint_type="<<std::dec<<hint.vt<<" hint="<<(hint.vt==VT_UI4?hint.ulVal:0);Log(info.str());
                PropVariantClear(&size);PropVariantClear(&hint);
            }
        }
        return r;
    }
    void Control(WpdLiveViewOperation op,bool observe_peer_end=false){
        send_deadline=Clock::now()+std::chrono::seconds(10);
        if(op==WpdLiveViewOperation::Start){
            DWORD completion{};const bool ready=WaitReady(send_deadline,&completion);
            std::ostringstream state;state<<alias<<" start_ready=0x"<<std::hex<<completion;Log(state.str());
            if(!ready && stopping)throw PreviewStartInterrupted();
            if(!ready || !PreviewControlReady(op,completion))throw std::runtime_error("not ready for Live View start");
        }
        if(op==WpdLiveViewOperation::Stop && !WaitReady(send_deadline,nullptr,true))throw std::runtime_error("not ready for Live View stop");
        if(op==WpdLiveViewOperation::Stop && observe_peer_end){
            const auto original_deadline=send_deadline;
            Log(alias+" peer_camera_end_observation_begin=1 scheduling_grace_ms=2000 original_stop_deadline=1");
            try{
                const bool off=ObservePreviewPeerEnd(original_deadline,
                    [&](auto deadline){return ReadPreviewProperty(0xd1a2,true,deadline);},
                    []{return Clock::now();},[](auto duration){std::this_thread::sleep_for(duration);});
                Log(alias+" peer_camera_end_live_off_observed="+std::to_string(off)+" stop_commands=0");
            }catch(const std::exception& error){failed=true;Log(alias+" peer_camera_end_observation_failed: "+error.what());}
            send_deadline=original_deadline;
            if(!WaitReady(send_deadline,nullptr,true))throw std::runtime_error("not ready after peer-end observation");
        }
        auto request=BuildWpdLiveViewOperation(op);auto r=Dispatch(request.Get());
        auto result=InspectWpdLiveViewCommand(r.status,r.results.Get(),true);
        if(op==WpdLiveViewOperation::Start)start_result=result;
        std::ostringstream s;s<<alias<<" control="<<static_cast<int>(op)<<" error="<<static_cast<int>(result.error)
            <<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.dispatch_status)<<" command=0x"
            <<static_cast<unsigned long>(result.command_status)<<" mtp=0x"<<result.mtp_response;Log(s.str());
        if(!result.Ok()){
            if(op==WpdLiveViewOperation::Start){std::lock_guard lock(mutex);
                failure_status=result.error==WpdLiveViewResultError::MtpRejected && result.mtp_response==0x2019?
                    L"カメラが開始を拒否しました（DeviceBusy）。本体・USB接続を確認してください":L"ライブビュー開始に失敗しました";}
            throw std::runtime_error("Live View control rejected");
        }
        // An acknowledged Start/Stop is not yet a settled body. Keep the same
        // deadline and wait for completion before publishing started/stopped
        // or allowing Close. Never resend the control operation.
        DWORD completion{};
        const bool settled_ready=WaitReady(send_deadline,&completion,op==WpdLiveViewOperation::Stop);
        if(!settled_ready && op==WpdLiveViewOperation::Start && stopping)throw PreviewStartInterrupted();
        if(!settled_ready || !PreviewControlReady(op,completion))
            throw std::runtime_error("Live View control did not settle before deadline");
        std::ostringstream settled;settled<<alias<<" control_settled="<<static_cast<int>(op)<<" mtp=0x"<<std::hex<<completion;Log(settled.str());
    }
    void FocusStatus(std::wstring text){std::lock_guard lock(mutex);focus_status=std::move(text);}
    bool WaitReady(Clock::time_point deadline,DWORD* completion=nullptr,bool cleanup=false){
        while((cleanup || !stopping) && Clock::now()<deadline){
            auto request=BuildPreviewDeviceReadyRequest();auto reply=Dispatch(request.Get());
            auto result=InspectWpdLiveViewCommand(reply.status,reply.results.Get(),true);
            if(result.Ok() || (result.error==WpdLiveViewResultError::MtpRejected && result.mtp_response==0xa002)){
                if(completion)*completion=result.mtp_response;
                return (cleanup || !stopping) && Clock::now()<deadline;
            }
            if(result.error!=WpdLiveViewResultError::MtpRejected || result.mtp_response!=0x2019)
                throw std::runtime_error("focus DeviceReady query failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(cleanup || !stopping)throw std::runtime_error("camera readiness deadline exceeded");
        return false;
    }
    void ApplyFocus(PreviewFocusAction action){
        struct Complete {PreviewFocusMailbox& mailbox;~Complete(){mailbox.Complete();}} complete{focus_mailbox};
        FocusStatus(action==PreviewFocusAction::AutoFocus?L"AF 実行中…":L"ピント微調整中…");
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        send_deadline=deadline;
        if(!WaitReady(deadline))return;
        const auto mode=ReadPreviewProperty(0xd061,false,deadline);
        send_deadline=deadline;
        const auto admission=AdmitPreviewFocusMode(action,mode);
        if(admission!=PreviewFocusAdmission::Allow){
            FocusStatus(admission==PreviewFocusAdmission::NeedsAfSwitch?L"本体・レンズのAF/MスイッチをAF側にしてください":
                admission==PreviewFocusAdmission::NeedsAfS?L"微調整にはライブビューのAF-S設定が必要です":L"未対応のフォーカス設定です");
            Log(alias+" focus_blocked mode="+std::to_string(mode));return;
        }
        auto request=BuildPreviewFocusRequest(action);
        if(stopping)return; // Never dispatch a queued drive after close/timeout.
        auto reply=Dispatch(request.Get());
        auto result=InspectWpdLiveViewCommand(reply.status,reply.results.Get(),true);
        {std::lock_guard lock(mutex);++focus_requests;}
        std::ostringstream log;log<<alias<<" focus="<<static_cast<int>(action)<<" error="<<static_cast<int>(result.error)
            <<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.dispatch_status)<<" command=0x"
            <<static_cast<unsigned long>(result.command_status)<<" mtp=0x"<<result.mtp_response;Log(log.str());
        if(!result.Ok() && result.error!=WpdLiveViewResultError::MtpRejected)
            throw std::runtime_error("focus command completion uncertain; not retried");
        DWORD completion{};if(!WaitReady(deadline,&completion))return;
        if(result.Ok() && action==PreviewFocusAction::AutoFocus && completion==0xa002){
            result.error=WpdLiveViewResultError::MtpRejected;result.mtp_response=completion;
        }
        std::ostringstream readiness;readiness<<alias<<" focus_ready=0x"<<std::hex<<completion;Log(readiness.str());
        if(result.Ok()){
            {std::lock_guard lock(mutex);++focus_success[static_cast<size_t>(action)];}
            FocusStatus(action==PreviewFocusAction::AutoFocus?L"AFコマンド完了（合焦は映像で確認）":
                action==PreviewFocusAction::Near?L"近くへ微調整しました":L"遠くへ微調整しました");
        }else{
            std::wostringstream text;text<<L"カメラが操作を拒否: 0x"<<std::hex<<std::uppercase<<result.mtp_response<<L"（AF設定・レンズを確認）";
            FocusStatus(text.str());
        }
    }
    DWORD ReadPreviewProperty(DWORD property,bool cleanup=false,std::optional<Clock::time_point> outer_deadline={}){
        WpdLiveViewTransferOptions options;const auto deadline=outer_deadline.value_or(Clock::now()+std::chrono::seconds(10));
        send_deadline=deadline;
        options.may_continue=[&]{return (cleanup || !stopping) && Clock::now()<deadline;};
        options.may_cleanup=[&]{return opened && !closed;};
        options.begin_request=[&]{return D810PreviewStatusRequest(property);};options.max_payload_bytes=16;options.max_chunk_bytes=16;
        auto result=ReadWpdLiveViewPayload(*this,options);
        if(!result.Ok()){
            std::ostringstream detail;detail<<"Live View diagnostic read failed: "<<static_cast<int>(result.error);
            if(result.command_failure)detail<<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.command_failure->dispatch_status)<<" command=0x"<<static_cast<unsigned long>(result.command_failure->command_status)<<" mtp=0x"<<result.command_failure->mtp_response;
            if(result.end_result)detail<<" end_mtp=0x"<<std::hex<<result.end_result->mtp_response;
            throw std::runtime_error(detail.str());
        }
        if(result.payload.size()!=D810PreviewPropertyBytes(property))throw std::runtime_error("diagnostic size mismatch");
        DWORD number{};for(size_t i=0;i<result.payload.size();++i)number|=static_cast<DWORD>(result.payload[i])<<(8*i);
        std::ostringstream text;text<<alias<<" property=0x"<<std::hex<<property<<" value=0x"<<number;Log(text.str());
        if(property==0xd061){std::lock_guard lock(mutex);focus_mode=number;}
        return number;
    }
    void Run(){
        const HRESULT com_status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try{
            Check(com_status,"worker COM initialize");
            try{
                ComPtr<IPortableDevice> local;
                {std::lock_guard io_lock(wpd_io_mutex);
                Log(alias+" transport_open_begin=1");
                local=Open(id,GENERIC_READ|GENERIC_WRITE);
                Log(alias+" transport_open_complete=1");
                {std::lock_guard lock(mutex);device=local;opened=true;}
                // Revalidate registered identity after opening the actual session.
                auto path=DefaultIdentityMapPath();path.replace_filename("camera-map-wpd.json");IdentityMap map(path);
                const auto actual_alias=map.FindAlias(Identity(local.Get()));
                if(!actual_alias || *actual_alias!=alias)throw std::runtime_error("current camera identity does not match registered alias");
                }
                if(stopping)throw std::runtime_error("stopped before start");
                if(cleanup_only){
                    std::lock_guard recovery_lock(recovery_workflow_mutex);
                    Log(alias+" explicit_cleanup_only=1 no_start=1 no_focus=1");
                    const auto status=ReadPreviewProperty(0xd1a2,true);
                    if(status>1)throw std::runtime_error("unknown Live View status");
                    if(status==1){Control(WpdLiveViewOperation::Stop);stopped=true;}
                    if(ReadPreviewProperty(0xd1a2,true)!=0)throw std::runtime_error("Live View off not confirmed");
                    ReadPreviewProperty(0xd061,true);
                    live_off_confirmed=true;
                }else{
                for(DWORD property:{0xd1a2u,0xd1a4u,0xd061u}){
                    const auto number=ReadPreviewProperty(property);
                    if(property==0xd1a4u && number!=0){
                        {std::lock_guard lock(mutex);failure_status=(number&(1u<<8))?L"電池不足で開始できません。充電済み電池を確認してください":
                            (number&(1u<<17))?L"カメラが温度上昇で開始を禁止しています":L"カメラ本体がライブビュー開始を禁止しています";}
                        throw std::runtime_error("camera reports Live View prohibition");
                    }
                    if(property==0xd1a2u && number!=0)throw std::runtime_error("camera already in Live View; no session takeover");
                }
                ++ready_count;
                const auto ready_deadline=Clock::now()+std::chrono::seconds(10);
                while(ready_count<2 && !stopping && Clock::now()<ready_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if(stopping || ready_count!=2)throw std::runtime_error("both camera preflights must pass before either start");
                start_attempted=true;Control(WpdLiveViewOperation::Start);started=true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                ComPtr<IWICImagingFactory> factory;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
                while(!stopping){
                    if(const auto action=focus_mailbox.Take())ApplyFocus(*action);
                    if(stopping)break;
                    const auto deadline=Clock::now()+std::chrono::seconds(10);
                    send_deadline=deadline;
                    admission_cancelled=false;frame_dispatched=false;
                    WpdLiveViewTransferOptions options;
                    options.may_continue=[&]{return ContinuePreviewFrame(stopping,frame_dispatched,Clock::now()>=deadline);};
                    options.may_cleanup=[&]{return opened && !closed;};
                    auto result=ReadWpdLiveViewPayload(*this,options);
                    if(!result.Ok()){
                        const bool frame_cancelled=result.error==WpdLiveViewTransferError::Interrupted || result.error==WpdLiveViewTransferError::NotAdmitted || admission_cancelled;
                        if(stopping && frame_cancelled && (!frame_dispatched || result.end_confirmed))break;
                        std::ostringstream s;s<<alias<<" transfer_error="<<static_cast<int>(result.error)<<" received="<<result.received_bytes;
                        if(result.command_failure)s<<" status_error="<<static_cast<int>(result.command_failure->error)<<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.command_failure->dispatch_status)<<" command=0x"<<static_cast<unsigned long>(result.command_failure->command_status)<<" mtp=0x"<<result.command_failure->mtp_response;
                        s<<" end_attempted="<<result.end_attempted<<" end_confirmed="<<result.end_confirmed;
                        if(result.end_result)s<<" end_dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.end_result->dispatch_status)<<" end_command=0x"<<static_cast<unsigned long>(result.end_result->command_status)<<" end_mtp=0x"<<result.end_result->mtp_response;
                        Log(s.str());
                        if(PreviewDeviceUnavailable(result)){
                            {std::lock_guard lock(mutex);failure_status=L"Windowsがカメラを利用できないと報告しました（デバイスなし）。接続状態を確認してください";}
                            Log(alias+" communication_device_unavailable=1 cause_confirmed=0");
                        }else if(PreviewSessionUnavailable(result)){
                            {std::lock_guard lock(mutex);failure_status=L"カメラとの通信セッションが切れました。電源・USB接続・電池を確認してください";}
                            Log(alias+" communication_session_unavailable=1 cause_confirmed=0");
                        }else if(PreviewCameraEnded(result)){
                            camera_ended=true;camera_end_observed=true;
                            {std::lock_guard lock(mutex);failure_status=L"カメラ側でライブビューが終了しました。自動終了設定・電池・温度を確認してください";}
                            Log(alias+" camera_live_view_ended=1 cause_confirmed=0 elapsed_seconds="+std::to_string(std::chrono::duration<double>(Clock::now()-first).count()));
                        }
                        throw std::runtime_error("frame transfer failed");
                    }
                    if(stopping)break; // Transfer was drained, do not publish a stopped frame.
                    auto next=Decode(factory.Get(),D810PreviewJpeg(result.payload));
                    const auto next_level=D810PreviewLevel(result.payload);
                    if(!frames){
                        // Diagnostic numeric words only, never image/header dumps.
                        std::ostringstream words;words<<alias<<" level_header_words="<<std::hex;
                        for(size_t offset:{52u,54u,56u,58u})words<<((static_cast<unsigned>(result.payload[offset])<<8)|result.payload[offset+1])<<',';
                        Log(words.str());
                    }
                    std::uint64_t hash=1469598103934665603ull;for(auto byte:next.bytes){hash^=byte;hash*=1099511628211ull;}
                    {std::lock_guard lock(mutex);auto now=Clock::now();if(!frames){first=now;
                        focus_status=(focus_mode==3 || focus_mode==4)?L"本体・レンズのAF/MスイッチをAF側にしてください":
                            focus_mode==2?L"AF可 / 微調整にはAF-S設定が必要です":L"AF / 微調整: 操作できます";
                    }last=now;++frames;if(frames>1 && hash!=last_hash)++changed;last_hash=hash;pixels=std::move(next);
                        level=next_level;if(level)++level_frames;else ++level_missing;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                }
            }catch(const PreviewStartInterrupted&){Log(alias+" start_interrupted_by_stop=1");stopping=true;}
            catch(const std::exception& e){Log(alias+" failure: "+e.what());failed=true;stopping=true;}
        }catch(const std::exception& e){failed=true;stopping=true;Log(alias+" thread failure: "+e.what());}
        focus_mailbox.Disable();
        shutdown_stages.TransfersDrained();
        Log(alias+" shutdown_transfers_drained=1");
        // Exactly one Stop attempt, including ambiguous Start failure, after
        // BOTH workers have ended their current transfer/focus operations.
        if(opened && start_attempted)try{
            const bool await_peer=AwaitPreviewPeerEnd(camera_end_observed.load(),started,camera_ended,failed.load(),Retired());
            Control(WpdLiveViewOperation::Stop,await_peer);stopped=true;}
            catch(const std::exception& e){Log(alias+" stop failure: "+e.what());failed=true;stopping=true;}
        shutdown_stages.StopsAttempted();
        Log(alias+" shutdown_both_stops_attempted=1");
        try{
            if(opened){
                ComPtr<IPortableDevice> local;{std::lock_guard lock(mutex);local=device;busy=true;busy_since=Clock::now();cancelled=false;}
                Log(alias+" transport_close_begin=1");
                HRESULT hr{};{std::lock_guard io_lock(wpd_io_mutex);hr=local->Close();}
                {std::ostringstream event;event<<alias<<" transport_close_complete=1 hr=0x"<<std::hex<<static_cast<unsigned long>(hr);Log(event.str());}
                {std::lock_guard lock(mutex);busy=false;closed=hr==S_OK;device.Reset();}
                if(hr!=S_OK){failed=true;stopping=true;Log(alias+" close failed");}
            }
        }catch(const std::exception& e){failed=true;stopping=true;Log(alias+" close failure: "+e.what());}
        if(SUCCEEDED(com_status))CoUninitialize();
        done=true;
    }
    void CancelStalled(){
        ComPtr<IPortableDevice> current;
        {std::lock_guard lock(mutex);if(busy && !cancelled && Clock::now()-busy_since>std::chrono::seconds(12)){cancelled=true;current=device;}}
        if(current){failed=true;stopping=true;Log(alias+" watchdog cancelling stalled call; lease retained until worker closes");
            Log(alias+" transport_cancel_begin=1");const auto hr=current->Cancel();
            std::ostringstream event;event<<alias<<" transport_cancel_complete=1 hr=0x"<<std::hex<<static_cast<unsigned long>(hr);Log(event.str());}
    }
};
int ReadCaptureDiagnostics(const std::array<std::wstring,2>& ids,bool inspect_events=false,bool require_live_off=false,bool extended_state=false){
    Log("capture_diagnostics=1 shutter_commands=0 setting_writes=0 live_start_commands=0");
    if(inspect_events)Log("event_diagnostics=1 polls_per_camera=4 capture_admission=not_evaluated");
    if(extended_state)Log("camera_state_diagnostics=1 extra_read_only_properties=5 live_start_commands=0");
    for(int i=0;i<2;++i){
        Camera c;c.id=ids[i];c.alias=i==0?"CAM-A":"CAM-B";std::jthread watchdog;
        try{
            c.device=Open(c.id,GENERIC_READ|GENERIC_WRITE);c.opened=true;
            auto map_path=DefaultIdentityMapPath();map_path.replace_filename("camera-map-wpd.json");IdentityMap map(map_path);
            if(map.FindAlias(Identity(c.device.Get()))!=std::optional<std::string>(c.alias))throw std::runtime_error("diagnostic camera alias mismatch");
            watchdog=std::jthread([&](std::stop_token stop){
                const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                if(FAILED(hr)){c.failed=true;stopping=true;return;}
                while(!stop.stop_requested()){c.CancelStalled();std::this_thread::sleep_for(std::chrono::milliseconds(50));}
                CoUninitialize();
            });
            c.send_deadline=Clock::now()+std::chrono::seconds(10);DWORD completion{};
            if(!c.WaitReady(c.send_deadline,&completion))throw std::runtime_error("diagnostic readiness interrupted");
            std::ostringstream ready;ready<<c.alias<<" diagnostic_ready=0x"<<std::hex<<completion;Log(ready.str());
            for(DWORD property:{0x500du,0x500au,0xd061u,0x5001u,0xd1a2u,0xd1a4u}){
                const auto value=c.ReadPreviewProperty(property);
                if((inspect_events || require_live_off) && property==0xd1a2 && value!=0)throw std::runtime_error("diagnostics require Live View off");
            }
            if(extended_state)for(DWORD property:{0xd1b2u,0xd1f0u,0xd1a6u,0xd10bu,0x500eu})c.ReadPreviewProperty(property);
            if(inspect_events){
                const auto deadline=Clock::now()+std::chrono::seconds(10);
                class EventChannel final:public WpdLiveViewCommandChannel {
                public:EventChannel(Camera& camera,Clock::time_point deadline):camera_(camera),deadline_(deadline){}
                private:
                    WpdLiveViewExchange Dispatch(IPortableDeviceValues* request)override{
                        if(!AllowedWpdStillEventDiagnosticRequest(request))throw std::runtime_error("event diagnostic request rejected");
                        std::lock_guard io_lock(wpd_io_mutex);
                        DWORD id{};request->GetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID,&id);
                        const bool cleanup=id==WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid;
                        if(!camera_.opened || camera_.closed || (!cleanup && (camera_.failed || stopping || Clock::now()>=deadline_)))
                            throw std::runtime_error("event diagnostic deadline/ownership");
                        {std::lock_guard lock(camera_.mutex);camera_.busy=true;camera_.busy_since=Clock::now();}
                        WpdLiveViewExchange result;result.status=camera_.device->SendCommand(0,request,&result.results);
                        {std::lock_guard lock(camera_.mutex);camera_.busy=false;}
                        return result;
                    }
                    Camera& camera_;Clock::time_point deadline_;
                } channel(c,deadline);
                for(unsigned poll=0;poll<4;++poll){
                    WpdLiveViewTransferOptions options;options.purpose=WpdPayloadPurpose::StillImage;options.max_payload_bytes=8192;
                    options.may_continue=[&]{return c.opened && !c.closed && !c.failed && !stopping && Clock::now()<deadline;};
                    options.may_cleanup=[&]{return c.opened && !c.closed;};
                    options.begin_request=[]{return BuildWpdStillRequest(WpdStillOperation::Events);};
                    const auto result=ReadWpdLiveViewPayload(channel,options);
                    if(!result.Ok())throw std::runtime_error("event diagnostic transfer failed");
                    const auto events=ParseWpdStillEvents(result.payload);
                    std::ostringstream line;line<<c.alias<<" diagnostic_event_poll="<<poll<<" count="<<events.size()<<" codes="<<std::hex;
                    for(const auto& event:events)line<<event.code<<','; // No handles, storage IDs or payload dump.
                    Log(line.str());
                    if(poll<3)std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        }catch(const std::exception& error){c.failed=true;Log(c.alias+" capture_diagnostic_failure: "+error.what());}
        if(c.opened){
            {std::lock_guard lock(c.mutex);c.busy=true;c.busy_since=Clock::now();}
            const auto hr=c.device->Close();
            {std::lock_guard lock(c.mutex);c.busy=false;c.closed=hr==S_OK;c.device.Reset();}
            if(!c.closed)c.failed=true;
        }
        if(watchdog.joinable()){watchdog.request_stop();watchdog.join();}
        Log(c.alias+" capture_diagnostic_closed="+std::to_string(c.closed)+" failed="+std::to_string(c.failed));
        if(c.failed)return 1;
    }
    return 0;
}
// Explicit fault-isolation only: distinguish an A-specific refusal from
// overlapping startup on B. This is never a fallback for dual Live View.
int RunIsolatedAStartDiagnostics(const std::array<std::wstring,2>& ids,bool inspect_busy_frame=false){
    Log("isolated_a_start_diagnostics=1 max_a_starts=1 b_start_commands=0 shutter_commands=0 focus_commands=0 setting_writes=0");
    if(inspect_busy_frame)Log("isolated_busy_frame_probe=1 max_frame_requests=1 original_start_deadline=1 start_failure_preserved=1");
    if(ReadCaptureDiagnostics(ids,false,true)!=0)return 1; // Both must Close before A opens again.
    Camera c;c.alias="CAM-A";c.id=ids[0];bool frame_decoded=false;std::jthread watchdog;
    try{
        Log(c.alias+" isolated_transport_open_begin=1");
        c.device=Open(c.id,GENERIC_READ|GENERIC_WRITE);c.opened=true;
        Log(c.alias+" isolated_transport_open_complete=1");
        auto map_path=DefaultIdentityMapPath();map_path.replace_filename("camera-map-wpd.json");IdentityMap map(map_path);
        if(map.FindAlias(Identity(c.device.Get()))!=std::optional<std::string>(c.alias))throw std::runtime_error("isolated diagnostic camera alias mismatch");
        watchdog=std::jthread([&](std::stop_token stop){
            const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            if(FAILED(hr)){c.failed=true;stopping=true;return;}
            while(!stop.stop_requested()){c.CancelStalled();std::this_thread::sleep_for(std::chrono::milliseconds(50));}
            CoUninitialize();
        });
        if(c.ReadPreviewProperty(0xd1a2)!=0 || c.ReadPreviewProperty(0xd1a4)!=0)
            throw std::runtime_error("isolated diagnostic requires A Live off and no prohibition");
        c.start_attempted=true;c.Control(WpdLiveViewOperation::Start);c.started=true;
        if(c.ReadPreviewProperty(0xd1a2)!=1)throw std::runtime_error("isolated A Live on not confirmed");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        c.send_deadline=Clock::now()+std::chrono::seconds(10);c.frame_dispatched=false;
        WpdLiveViewTransferOptions options;
        options.may_continue=[&]{return ContinuePreviewFrame(stopping,c.frame_dispatched,Clock::now()>=c.send_deadline);};
        options.may_cleanup=[&]{return c.opened && !c.closed;};
        const auto transfer=ReadWpdLiveViewPayload(c,options);
        if(!transfer.Ok())throw std::runtime_error("isolated A preview transfer failed");
        ComPtr<IWICImagingFactory> factory;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"isolated WIC factory");
        (void)Decode(factory.Get(),D810PreviewJpeg(transfer.payload));frame_decoded=true;
        Log("CAM-A isolated_preview_decoded=1 displayed=0 saved=0 dual_live_accepted=0");
    }catch(const std::exception& error){c.failed=true;Log(c.alias+" isolated_start_failure: "+error.what());
        // Only the explicit, transported Busy response permits this bounded
        // read-only observation. Never turn it into a successful Start or send
        // Start again. Retain the original control deadline.
        if(c.start_result && c.start_result->error==WpdLiveViewResultError::MtpRejected &&
            c.start_result->mtp_response==0x2019 && !stopping && Clock::now()<c.send_deadline){
            const auto deadline=c.send_deadline;
            try{
                DWORD completion{};const bool ready=c.WaitReady(deadline,&completion);
                std::ostringstream state;state<<"CAM-A isolated_post_busy_ready=0x"<<std::hex<<completion;Log(state.str());
                if(ready){
                    if(inspect_busy_frame){
                        c.send_deadline=deadline;c.frame_dispatched=false;
                        WpdLiveViewTransferOptions options;
                        options.may_continue=[&]{return ContinuePreviewFrame(stopping,c.frame_dispatched,Clock::now()>=deadline);};
                        options.may_cleanup=[&]{return c.opened && !c.closed;};
                        Log("CAM-A isolated_busy_frame_dispatch=1 max_frame_requests=1");
                        const auto transfer=ReadWpdLiveViewPayload(c,options);
                        std::ostringstream frame_state;frame_state<<"CAM-A isolated_busy_frame_result="<<static_cast<int>(transfer.error)
                            <<" end_confirmed="<<transfer.end_confirmed<<" received_bytes="<<transfer.received_bytes;
                        if(transfer.command_failure)frame_state<<" mtp=0x"<<std::hex<<transfer.command_failure->mtp_response;
                        if(transfer.end_result)frame_state<<" end_mtp=0x"<<std::hex<<transfer.end_result->mtp_response;
                        frame_state<<" channel_retired="<<transfer.channel_retired;
                        Log(frame_state.str());
                        if(transfer.channel_retired)throw std::runtime_error("busy-frame transfer retired; no further data reads");
                        if(transfer.Ok()){
                            ComPtr<IWICImagingFactory> factory;
                            Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"busy-frame WIC factory");
                            (void)Decode(factory.Get(),D810PreviewJpeg(transfer.payload));
                            Log("CAM-A isolated_busy_frame_decoded=1 displayed=0 saved=0 start_accepted=0 dual_live_accepted=0");
                        }
                        // A rejected or corrupt preview never causes another
                        // frame/Start or turns the original Start into PASS.
                    }
                    const auto observe_until=std::min(deadline,Clock::now()+std::chrono::seconds(3));
                    unsigned polls{};DWORD live{},prohibition{};
                    do{
                        live=c.ReadPreviewProperty(0xd1a2,false,observe_until);
                        prohibition=c.ReadPreviewProperty(0xd1a4,false,observe_until);++polls;
                        if(live!=0 || prohibition!=0 || stopping || Clock::now()+std::chrono::milliseconds(150)>=observe_until)break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }while(Clock::now()<observe_until);
                    std::ostringstream snapshot;snapshot<<"CAM-A isolated_post_busy_live="<<live<<" prohibition=0x"<<std::hex<<prohibition
                        <<" polls="<<std::dec<<polls<<" start_accepted=0";Log(snapshot.str());
                }
            }catch(const std::exception& observation){Log(std::string("CAM-A isolated_post_busy_observation_failed: ")+observation.what());}
        }
    }
    stopping=true;
    // One Stop even if Start was refused or completion is uncertain. No
    // barrier is needed: B has no open session or worker in this experiment.
    if(c.opened && c.start_attempted)try{c.Control(WpdLiveViewOperation::Stop);c.stopped=true;}
        catch(const std::exception& error){c.failed=true;Log(c.alias+" isolated_stop_failure: "+error.what());}
    if(c.opened){
        {std::lock_guard lock(c.mutex);c.busy=true;c.busy_since=Clock::now();c.cancelled=false;}
        Log(c.alias+" isolated_transport_close_begin=1");const auto hr=c.device->Close();
        {std::lock_guard lock(c.mutex);c.busy=false;c.closed=hr==S_OK;c.device.Reset();}
        {std::ostringstream event;event<<c.alias<<" isolated_transport_close_complete=1 hr=0x"<<std::hex<<static_cast<unsigned long>(hr);Log(event.str());}
        if(!c.closed)c.failed=true;
    }
    if(watchdog.joinable()){watchdog.request_stop();watchdog.join();}
    const bool passed=c.started && frame_decoded && c.stopped && c.closed && !c.failed;
    Log(std::string("isolated_a_start_result=")+(passed?"PASS":"FAIL")+" started="+std::to_string(c.started)+
        " frame_decoded="+std::to_string(frame_decoded)+" stopped="+std::to_string(c.stopped)+" closed="+std::to_string(c.closed)+
        " b_start_commands=0 shutter_commands=0 dual_live_accepted=0");
    return passed?0:1;
}
#include "wpd_dual_preview_ui.hpp"
}
int main(int argc,char** argv){
    try{
        if(argc==2 && std::string(argv[1])=="--level-ui-smoke")return RunLevelUiSmoke();
        if(argc==2 && std::string(argv[1])=="--lifecycle-ui-smoke")return RunLifecycleUiSmoke();
        if(argc==2 && std::string(argv[1])=="--isolated-a-start-diagnostics"){
            ComScope com;HardwareProcessLease lease;return RunIsolatedAStartDiagnostics(Discover());
        }
        if(argc==2 && std::string(argv[1])=="--isolated-a-busy-frame-diagnostics"){
            ComScope com;HardwareProcessLease lease;return RunIsolatedAStartDiagnostics(Discover(),true);
        }
        if(argc==2 && std::string(argv[1])=="--camera-state-diagnostics"){
            ComScope com;HardwareProcessLease lease;return ReadCaptureDiagnostics(Discover(),false,true,true);
        }
        if(argc==2 && std::string(argv[1])=="--pair-preflight-diagnostics"){
            ComScope com;HardwareProcessLease lease;const auto ids=Discover();const auto deadline=Clock::now()+std::chrono::seconds(180);
            Log("pair_preflight_diagnostics=1 shutter_commands=0 setting_writes=0");
            for(int i=0;i<2;++i){const auto result=CheckStillOnce(ids[i],i==0?"CAM-A":"CAM-B",deadline);if(!result.ready || !result.closed)return 1;}
            Log("pair_preflight_diagnostics_result=PASS shutter_commands=0");return 0;
        }
        if(argc==2 && (std::string(argv[1])=="--capture-diagnostics" || std::string(argv[1])=="--capture-event-diagnostics")){
            ComScope com;HardwareProcessLease lease;return ReadCaptureDiagnostics(Discover(),std::string(argv[1])=="--capture-event-diagnostics");
        }
        // Optional trailing pair for the timed window only; the earlier forms
        // keep their exact argument counts.
        std::vector<std::string> args(argv,argv+argc);std::optional<MountOrientation> requested_orientation;
        if(args.size()>=5 && args[1]=="--seconds" && args[args.size()-2]=="--mount-orientation"){
            requested_orientation=a0::m2::experimental::ParseMountOrientation(args.back());
            if(!requested_orientation)throw std::runtime_error("invalid mount orientation; use normal or rotated-180");
            args.resize(args.size()-2);
        }
        if((args.size()!=3 && args.size()!=4) || args[1]!="--seconds")throw std::runtime_error("usage: A0CameraStitcher.WpdDualPreview --seconds 0..3600 [--focus-smoke|--af-prepare-smoke|--stop-live-only|--capture-a|--capture-b|--capture-smoke|--merge-smoke|--pair-smoke|--pair-cycle-smoke|--level-smoke|--restart-smoke|--camera-end-restart-smoke] [--mount-orientation normal|rotated-180]; or --capture-diagnostics / --capture-event-diagnostics / --pair-preflight-diagnostics / --isolated-a-start-diagnostics / --isolated-a-busy-frame-diagnostics / --camera-state-diagnostics (no shutter)");
        size_t used{};run_seconds=std::stoi(args[2],&used);if(used!=args[2].size() || run_seconds<0 || run_seconds>3600)throw std::runtime_error("invalid duration");
        if(args.size()==4){
            const std::string mode=args[3];
            if(mode=="--stop-live-only"){cleanup_only=true;closing=true;}
            else if(mode=="--capture-a" || mode=="--capture-b"){cleanup_only=true;closing=true;capture_once_alias=mode=="--capture-a"?0:1;}
            else if(mode=="--focus-smoke" && run_seconds>=30)focus_smoke=true;
            else if(mode=="--capture-smoke" && run_seconds>=60)capture_smoke=true;
            else if(mode=="--merge-smoke" && run_seconds>=15)merge_smoke=true;
            else if(mode=="--pair-smoke" && run_seconds>=40)pair_smoke=true;
            else if(mode=="--level-smoke" && run_seconds>=15)level_smoke=true;
            else if(mode=="--restart-smoke" && run_seconds>=30)restart_smoke=true;
            else if(mode=="--camera-end-restart-smoke" && run_seconds>=620)camera_end_restart_smoke=true;
            else if(mode=="--pair-cycle-smoke" && run_seconds>=60)pair_cycle_smoke=true;
            else if(mode=="--af-prepare-smoke" && run_seconds>=15)af_prepare_smoke=true;
            else throw std::runtime_error("invalid test mode or duration");
        }
        if(!cleanup_only){
            // Settings sit beside camera-map-wpd.json. A rejected file stops
            // startup before the hardware lease or any camera session.
            auto settings=DefaultIdentityMapPath();settings.replace_filename(kMountOrientationSettingsName);
            const auto configured=LoadMountOrientationSettings(settings);
            mount_orientation=requested_orientation.value_or(configured.value_or(MountOrientation::normal));
            Log(std::string("mount_orientation=")+a0::m2::experimental::MountOrientationName(mount_orientation)+" source="+
                (requested_orientation?"argument":configured?"settings":"default")+" originals_rotated=0");
            auto policy_file=DefaultIdentityMapPath();policy_file.replace_filename(kMergePolicySettingsName);
            merge_policy=LoadMergePolicySettings(policy_file,[](const std::vector<unsigned char>& bytes){return a0::phase0::Sha256Hex(bytes);});
            std::ostringstream policy_log;policy_log.imbue(std::locale::classic());policy_log<<"merge_policy=";
            if(merge_policy){policy_log<<"settings sha256="<<merge_policy->sha256<<" rotation_max_degrees="<<merge_policy->rotation_max_degrees<<" scale_max_percent=";
                if(merge_policy->scale_max_percent)policy_log<<*merge_policy->scale_max_percent;else policy_log<<"none";
                policy_log<<" residual_warning_pixels=";
                if(merge_policy->residual_max_pixels)policy_log<<*merge_policy->residual_max_pixels;else policy_log<<"none";
            }else policy_log<<"none rotation_correction=0 scale_correction=0";
            Log(policy_log.str());
        }
        ComScope com;HardwareProcessLease lease;return RunPreviewWindow(Discover());
    }catch(const std::exception& e){Log(std::string("failure: ")+e.what());return 2;}
}
