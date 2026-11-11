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
