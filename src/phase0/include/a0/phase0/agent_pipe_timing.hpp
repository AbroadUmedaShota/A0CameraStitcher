#pragma once

#include <chrono>

// Stage timeouts of the named-pipe server loop shared by every Camera Agent
// host and the preview worker host (RunNamedPipeServerLoop in
// hardware_camera_agent_pipe.cpp). This header is their single source: the
// loop uses them directly, and the experimental preview worker timing budget
// (preview_worker_timing.hpp) derives its reply-stage terms from them.
namespace a0::phase0::agent_pipe_timing {

// Serve-once hosts: how long the host waits for its one client to connect.
inline constexpr std::chrono::milliseconds kAcceptTimeout{15000};
// Each request read stage (header, then body).
inline constexpr std::chrono::milliseconds kFrameReadTimeout{5000};
// Each of the three post-dispatch stages: response header write, response
// body write, and the client's ACK read.
inline constexpr std::chrono::milliseconds kResponseWriteTimeout{1000};

} // namespace a0::phase0::agent_pipe_timing
