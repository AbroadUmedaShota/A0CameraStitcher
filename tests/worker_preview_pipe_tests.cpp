// Built only against the gated SDK stub. These children cannot open a camera.
#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/preview_worker_reply.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "dual_live_test_ipc.hpp"
using namespace a0::phase0::experimental;
namespace {
struct PipeCase {
    const char* name;
    std::uint64_t operation_ms;
    std::uint64_t serving_ms;
    DWORD delay_ms;            // Wait after the pipe is reachable, before the request.
    const char* operation;
    bool valid_capability;
    PreviewWorkerReplyStatus status;
    const char* category;      // Expected failure category, or "" for closed.
    DWORD exit_code;
};
void RunCase(HANDLE parent, const PipeCase& c) {
    const auto name = Unique();
    Handle child(Start(L"--pipe-worker " + Wide(name) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(parent)) + L" " + std::to_wstring(c.operation_ms) + L" " +
        std::to_wstring(c.serving_ms), parent));
    Handle pipe;
    // Readiness only: process start on this PC has taken longer than 2.5 s.
    const auto deadline = GetTickCount64() + 8000;
    do {
        pipe.value = CreateFileW(PipePath(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.valid()) break;
        if (WaitForSingleObject(child.value, 10) == WAIT_OBJECT_0) break;
    } while (GetTickCount64() < deadline);
    Require(pipe.valid(), "worker pipe available");
    ULONG server{};
    Require(GetNamedPipeServerProcessId(pipe.value, &server) && server == GetProcessId(child.value),
            "server is the spawned child");
    if (c.delay_ms) Sleep(c.delay_ms);
    const std::string request = "{\"schema\":\"a0.preview-worker.v2\",\"epoch\":\"epoch\",\"capability\":\"" +
        std::string(c.valid_capability ? "capability" : "wrong") +
        "\",\"sequence\":1,\"operation\":\"" + c.operation + "\",\"candidate\":\"\"}";
    Require(WriteMessage(pipe.value, request), "request delivered");
    std::string reply;
    Require(ReadMessage(pipe.value, reply), "reply received");
    const auto parsed = ParsePreviewWorkerReply(reply, "epoch", server, c.valid_capability ? 1 : 0, c.operation);
    Check(parsed.status == c.status && parsed.close_receipt.has_value() && parsed.close_receipt->Complete(),
          std::string(c.name) + ": bound IPC reply status and reported receipt");
    Check(parsed.error_category == c.category, std::string(c.name) + ": failure category survives validation");
    // The gated stub transport has no counters: every v2 reply carries an all-zero diag.
    Check(parsed.topology == PreviewTopologyDiag{}, std::string(c.name) + ": v2 reply carries an all-zero diag");
    unsigned char ack = 0x06;
    Require(Io(pipe.value, &ack, 1, true), "delivery acknowledgement");
    CloseHandle(pipe.value); // No further command: the worker must end on its own.
    pipe.value = nullptr;
    Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "child exited without kill");
    Check(ExitCode(child.value) == c.exit_code, std::string(c.name) + ": OS exit agrees with IPC result");
}
}
int main(int argc, char** argv) {
    if (argc == 6 && std::string_view(argv[1]) == "--pipe-worker") {
        std::uint64_t raw{}, operation_ms{}, serving_ms{};
        if (!Number(argv[3], raw) || !Number(argv[4], operation_ms) || !Number(argv[5], serving_ms)) return 2;
        const auto parent = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
        NikonSdkTransport transport;
        const auto result = RunWorkerPreviewNamedPipeServer(argv[2], transport, "epoch", "capability", parent,
            std::chrono::milliseconds(operation_ms), std::chrono::milliseconds(serving_ms),
            [] { return true; }); // Stub-only IPC fixture.
        return result.safe_to_exit ? result.ipc_exit_code : 4;
    }
    try {
        Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId()));
        Require(parent.valid(), "parent handle");
        using Status = PreviewWorkerReplyStatus;
        const PipeCase cases[] = {
            {"explicit close", 3000, 10000, 0, "close", true, Status::closed, "", 0},
            {"wrong capability", 3000, 10000, 0, "close", false, Status::failed, "worker_authority", 3},
            // Operation deadline 300 ms, serving lifetime 10 s, request at ~600 ms.
            {"frame after operation deadline", 300, 10000, 600, "frame", true, Status::failed,
             "worker_authority_expired", 3},
            {"close after operation deadline", 300, 10000, 600, "close", true, Status::closed, "", 0},
        };
        for (const auto& c : cases) RunCase(parent.value, c);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
    std::cout << "{\"mode\":\"stub-process-ipc\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
