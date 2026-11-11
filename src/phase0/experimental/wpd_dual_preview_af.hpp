// AF frame of the development preview (#426), Camera side. Included after Camera, inside the
// anonymous namespace of wpd_dual_preview_main.cpp: it holds out-of-class members of Camera so that
// the camera struct itself only declares them.

// Worker thread, once per decoded frame. Reads the AF frame words of the frame header and keeps them
// when they are usable as a coordinate space; otherwise the frame is unknown and nothing is drawn.
// Sends no command.
void Camera::UpdateAfFrame(std::span<const unsigned char> payload,unsigned width,unsigned height){
    const auto words=D810PreviewAfWords(payload);
    D810AfReject reason{};
    const auto frame=D810PreviewAfFrame(payload,width,height,&reason);
    const auto now=Clock::now();
    {std::lock_guard lock(mutex);af.frame=frame;af.frame_at=now;af.reject=reason;}
    if(af.log.ShouldLog(words,reason,now))Log(alias+" "+DescribeD810AfWords(words,reason));
}

// Stage 2. The only route for 0x9205: Dispatch() refuses it because AllowedD810PreviewRequest does not
// list it. Same I/O discipline as Dispatch: the shared WPD mutex, session ownership, stop request and
// the deadline are checked under it, and a result that is not clean is logged once.
WpdLiveViewExchange Camera::DispatchAfPoint(IPortableDeviceValues* request){
    std::optional<PreviewAfBounds> bounds;
    {std::lock_guard lock(mutex);bounds=PreviewAfBoundsFrom(af.frame,af.frame_at);}
    std::lock_guard io_lock(wpd_io_mutex);
    const auto now=Clock::now();
    if(!AdmitPreviewAfPointDispatch(request,bounds,now,opened && !closed,stopping,now>=send_deadline)){
        if(stopping)admission_cancelled=true;
        throw std::runtime_error("AF point request not admitted");
    }
    ComPtr<IPortableDevice> current;
    {std::lock_guard lock(mutex);current=device;busy=true;busy_since=Clock::now();cancelled=false;}
    WpdLiveViewExchange reply;reply.status=current->SendCommand(0,request,&reply.results);
    {std::lock_guard lock(mutex);busy=false;}
    const auto status=InspectWpdLiveViewCommand(reply.status,reply.results.Get(),false);
    if(!status.Ok()){
        std::ostringstream event;event<<alias<<" transport_command_failed=1 opcode=0x9205 dispatch=0x"<<std::hex<<static_cast<unsigned long>(status.dispatch_status)
            <<" command=0x"<<static_cast<unsigned long>(status.command_status)<<" mtp=0x"<<status.mtp_response<<" error="<<std::dec<<static_cast<int>(status.error);
        Log(event.str());
    }
    return reply;
}
// One click: at most one 0x9205, never retried, never followed by AF. The flow itself (readiness,
// focus mode, bounds, one send, completion) is RunPreviewAfPoint; this only supplies the camera.
void Camera::ApplyAfPoint(const PreviewAfPointRequest& point){
    struct Complete {PreviewFocusMailbox& mailbox;~Complete(){mailbox.Complete();}} complete{focus_mailbox};
    const auto forget_request=[this]{std::lock_guard lock(mutex);af.request.reset();};
    FocusStatus(L"AF枠を移動しています…");
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    send_deadline=deadline;
    PreviewAfPointHooks hooks;
    hooks.wait_ready=[&](DWORD* completion){send_deadline=deadline;return WaitReady(deadline,completion);};
    hooks.read_focus_mode=[&]{const auto mode=ReadPreviewProperty(0xd061,false,deadline);send_deadline=deadline;return mode;};
    hooks.bounds=[&]{std::lock_guard lock(mutex);return PreviewAfBoundsFrom(af.frame,af.frame_at);};
    hooks.now=[]{return Clock::now();};
    hooks.stopping=[]{return stopping.load();};
    hooks.send=[&](IPortableDeviceValues* request){return DispatchAfPoint(request);};
    const auto result=RunPreviewAfPoint(point,hooks);
    std::ostringstream log;log<<alias<<" af_point x="<<point.x<<" y="<<point.y<<" space="<<point.width<<"x"<<point.height
        <<" dispatches="<<result.dispatches<<" outcome="<<static_cast<int>(result.outcome);
    if(result.dispatches)log<<" error="<<static_cast<int>(result.command.error)<<" dispatch=0x"<<std::hex<<static_cast<unsigned long>(result.command.dispatch_status)
        <<" command=0x"<<static_cast<unsigned long>(result.command.command_status)<<" mtp=0x"<<result.command.mtp_response<<" ready=0x"<<result.completion;
    else if(result.outcome==PreviewAfPointOutcome::Blocked)log<<" focus_mode="<<result.mode;
    Log(log.str());
    switch(result.outcome){
    case PreviewAfPointOutcome::Placed:
        FocusStatus(L"AF枠の移動コマンド完了（緑の枠で位置を確認。AFは押していません）");break;
    case PreviewAfPointOutcome::RefusedByBody:{
        forget_request();
        std::wostringstream text;text<<L"カメラが枠の移動を拒否: 0x"<<std::hex<<std::uppercase<<result.command.mtp_response<<L"（本体のAFエリア設定を確認）";
        FocusStatus(text.str());break;}
    case PreviewAfPointOutcome::Blocked:
        forget_request();
        FocusStatus(result.admission==PreviewFocusAdmission::NeedsAfSwitch?L"本体・レンズのAF/MスイッチをAF側にしてください（MFのため枠は送りません）":
            L"未対応のフォーカス設定です（枠は送りません）");
        break;
    case PreviewAfPointOutcome::NotSent:
        forget_request();
        if(result.not_sent!=PreviewAfPointNotSent::Interrupted)FocusStatus(L"AF枠の位置情報が古いため、枠は送りませんでした");
        break;
    case PreviewAfPointOutcome::Uncertain:
        forget_request();
        FocusStatus(L"AF枠の移動の結果が不明です。再送しません");
        throw std::runtime_error("AF point command completion uncertain; not retried");
    }
}
