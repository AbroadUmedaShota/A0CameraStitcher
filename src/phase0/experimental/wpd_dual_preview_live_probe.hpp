// Live-open capture probe, Camera side (#426 stage 4, ADR-0044 stage P). Included after Camera inside the
// anonymous namespace of wpd_dual_preview_main.cpp, and only when A0_ENABLE_WPD_LIVE_CAPTURE_PROBE is defined.
//
// Two jobs. The Live worker hands its session over at a frame boundary (HoldAtFrameBoundary), and the probe
// thread borrows that session for the one transaction (RunLiveProbeOnCamera). One owner sends at any time.

// Worker thread, at the top of each loop turn, before any command of the turn. Answers a hold request from the
// boundary between two frames and sends nothing until the window releases the session.
void Camera::HoldAtFrameBoundary(){
    if(owner.State()!=PreviewSessionState::HoldRequested)return;
    if(!owner.AcknowledgeHold())return;
    const auto since=Clock::now();
    for(;;){
        const auto state=owner.State();
        if(state==PreviewSessionState::Live || state==PreviewSessionState::Retired)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Log(alias+" live_capture_probe_hold released=1 held_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-since).count())+
        " io_during_hold="+std::to_string(held_dispatch_refused.load()));
}

// The AF frame words of the latest frame, for the line before and after the shot. Code and numbers only.
std::string LiveProbeAfLine(Camera& camera,const char* phase){
    std::optional<D810AfHeader> words;D810AfReject reason{};
    {std::lock_guard lock(camera.mutex);words=camera.af_words_last;reason=camera.af_reject_last;}
    return camera.alias+" live_capture_probe_af_frame phase="+phase+" "+DescribeD810AfWords(words,reason);
}

struct LiveProbeThreadOutcome {
    WpdLiveProbeOutcome outcome;
    bool lent{}; // the session was borrowed (a refusal to lend leaves nothing to give back)
};

// The borrowed session of one camera. Every command goes through the probe gate in the base class first; this
// only adds the shared I/O mutex, the busy mark that the stall watchdog reads, and the transport.
class LiveProbeCameraChannel final:public WpdLiveCaptureProbeChannel {
public:
    LiveProbeCameraChannel(Camera& camera,std::function<bool()> lent,std::function<bool()> may_continue):
        WpdLiveCaptureProbeChannel(std::move(lent),std::move(may_continue),[&camera]{std::lock_guard lock(camera.mutex);return camera.opened && !camera.closed;}),camera_(camera){}
private:
    WpdLiveViewExchange Exchange(IPortableDeviceValues* request)override{
        std::lock_guard io_lock(wpd_io_mutex);
        ComPtr<IPortableDevice> current;
        {std::lock_guard lock(camera_.mutex);current=camera_.device;camera_.busy=true;camera_.busy_since=Clock::now();camera_.cancelled=false;}
        if(!current)throw std::runtime_error("probe session is not open");
        WpdLiveViewExchange reply;reply.status=current->SendCommand(0,request,&reply.results);
        {std::lock_guard lock(camera_.mutex);camera_.busy=false;}
        return reply;
    }
    Camera& camera_;
};

// The probe thread. The session of `camera` is held by its worker when this starts. Borrows it, runs steps 3..9
// of the probe against the borrowed session, and gives it back as held; the window releases it afterwards.
LiveProbeThreadOutcome RunLiveProbeOnCamera(Camera& camera,Clock::time_point deadline){
    LiveProbeThreadOutcome result;
    ComScope com; // the requests are WPD objects built on this thread
    const auto token=camera.owner.Lend();
    if(!token){result.outcome.stop_reason="session_not_held";Log(camera.alias+" live_capture_probe_lend refused=1 shutter_commands=0");return result;}
    result.lent=true;
    struct Giveback {Camera& camera;PreviewSessionOwner::Token token;~Giveback(){camera.owner.Return(token);}} giveback{camera,*token};
    EvidenceWriter evidence(DefaultIdentityMapPath().parent_path().parent_path()/"wpd-live-capture-probes",NewRunId(),"wpd-live-capture-probe-development");
    const auto partial_path=evidence.RunRoot()/(camera.alias+"-transport.jpg.partial");
    struct Partial {HANDLE handle{INVALID_HANDLE_VALUE};~Partial(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} partial;
    partial.handle=CreateFileW(partial_path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(partial.handle==INVALID_HANDLE_VALUE){result.outcome.stop_reason="partial_create_failed";Log(camera.alias+" live_capture_probe_partial_create_failed=1 shutter_commands=0");return result;}
    evidence.RecordState("live-capture-probe","LiveSessionBorrowed",camera.alias);
    WpdLiveCaptureProbeChannel* self=nullptr;
    // The same admission as a still lane: a dispatched shutter keeps its import admitted after a stop request; an
    // undispatched one does not. Nothing is admitted after the deadline or once the camera's worker failed (the
    // 12 s stall watchdog sets that).
    const auto admitted=[&camera,deadline,&self]{
        return !camera.failed && Clock::now()<deadline && ((self && self->ShutterAttempted()) || !capture_stop_requested.load());
    };
    LiveProbeCameraChannel channel(camera,[&camera,t=*token]{return camera.owner.IsLentTo(t);},admitted);
    self=&channel;
    WpdLiveProbeHooks hooks;
    hooks.partial_sink=[&](std::span<const unsigned char> bytes){
        DWORD written{};
        if(!WriteFile(partial.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size())throw std::runtime_error("probe partial write failed");
    };
    hooks.save_original=[&](WpdStillPayload&& payload){
        if(!FlushFileBuffers(partial.handle))throw std::runtime_error("probe partial flush failed");
        const auto saved=PersistVerifiedWpdStill(evidence,camera.alias,std::move(payload),deadline);
        Log(camera.alias+" original_saved=1 bytes="+std::to_string(saved.bytes)+" sha256="+saved.sha256);
        std::wcout<<L"saved_probe_original="<<saved.path.wstring()<<std::endl;
        return saved.success;
    };
    hooks.log=[](const std::string& line){Log(line);};
    try{result.outcome=RunLiveCaptureProbe(channel,camera.alias,hooks);}
    catch(...){
        // Whatever was sent before the exception stays true: a shutter that was attempted is reported as sent.
        result.outcome=WpdLiveProbeOutcomeAfterException(channel.ShutterAttempted());
        Log(camera.alias+" live_capture_probe_exception=1 shutter_commands="+(result.outcome.shutter_sent?"1":"0"));
    }
    try{
        if(result.outcome.result==WpdLiveProbeResult::StoppedBeforeShutter)evidence.RecordState("live-capture-probe","StoppedBeforeShutter",camera.alias,result.outcome.stop_reason);
        else evidence.RecordState("live-capture-probe",WpdLiveProbeResultText(result.outcome.result),camera.alias,result.outcome.stop_reason);
    }catch(const std::exception&){}
    return result;
}
