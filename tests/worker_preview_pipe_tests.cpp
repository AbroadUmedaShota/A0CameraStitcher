// Built only against the gated SDK stub. These children cannot open a camera.
#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "dual_live_test_ipc.hpp"
using namespace a0::phase0::experimental;
int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--pipe-worker") {
        std::uint64_t raw{};
        if (!Number(argv[3], raw)) return 2;
        const auto parent = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
        NikonSdkTransport transport;
        const auto result = RunWorkerPreviewNamedPipeServer(argv[2], transport, "epoch", "capability", parent,
                                                            std::chrono::seconds(3));
        return result.safe_to_exit ? result.ipc_exit_code : 4;
    }
    try {
        Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId()));
        Require(parent.valid(), "parent handle");
        for (const bool valid : {true, false}) {
            const auto name = Unique();
            Handle child(Start(L"--pipe-worker " + Wide(name) + L" " +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.value)), parent.value));
            Handle pipe;
            const auto deadline = GetTickCount64() + 2500;
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
            const std::string request = "{\"schema\":\"a0.preview-worker.v1\",\"epoch\":\"epoch\",\"capability\":\"" +
                std::string(valid ? "capability" : "wrong") +
                "\",\"sequence\":1,\"operation\":\"close\",\"candidate\":\"\"}";
            Require(WriteMessage(pipe.value, request), "request delivered");
            std::string reply;
            Require(ReadMessage(pipe.value, reply), "close reply received");
            Check(reply.find("\"workerPid\":" + std::to_string(server)) != std::string::npos,
                  "close receipt bound to worker PID");
            Check(reply.find(valid ? "\"status\":\"closed\"" : "\"status\":\"failed\"") != std::string::npos,
                  "authenticated close or rejected capability");
            unsigned char ack = 0x06;
            Require(Io(pipe.value, &ack, 1, true), "delivery acknowledgement");
            Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "child exited without kill");
            Check(ExitCode(child.value) == (valid ? 0U : 3U), "OS exit agrees with IPC result");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
    std::cout << "{\"mode\":\"stub-process-ipc\",\"failures\":" << failures << "}\n";
    return failures ? 1 : 0;
}
