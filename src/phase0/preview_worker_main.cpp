#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/preview_worker_timing.hpp"
#include <Windows.h>
#include <charconv>
#include <filesystem>
#include <iostream>
using namespace a0::phase0;
namespace json = a0::common::protocol_json;
namespace {
// Exit codes: 0 explicit close completed; 2 explicit argument, bootstrap or
// delegation rejection; 3 the dispatcher ran but did not complete (it has
// already tried to close the SDK session); 4 an exception reached main before
// the pipe host ran a dispatcher (bootstrap read or JSON, handle arguments,
// delegation check, transport construction, host preconditions), so the SDK
// was never used. No exit at all: safe_to_exit was false (held for recovery).
constexpr int kWorkerMainExceptionExitCode = 4;
struct Failure {
    [[noreturn]] static void Fail(std::string_view a, std::string_view b) { throw TransportError(std::string(a), std::string(b)); }
};
HANDLE HandleArgument(std::string_view value) {
    std::uintptr_t number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (!number || result.ec != std::errc{} || result.ptr != value.data() + value.size())
        throw TransportError("worker_bootstrap", "invalid inherited handle");
    return reinterpret_cast<HANDLE>(number);
}
void ReadExact(HANDLE pipe, void* buffer, DWORD size) {
    DWORD done{};
    while (done < size) {
        DWORD read{};
        if (!ReadFile(pipe, static_cast<char*>(buffer) + done, size - done, &read, nullptr) || !read)
            throw TransportError("worker_bootstrap", "bootstrap channel ended");
        done += read;
    }
}
// Accepts a nonzero decimal that fits in 32 bits. The only upper bound here is
// the uint32 range; the lifetime limits (operation deadline below the serving
// lifetime, serving lifetime at most kPreviewSessionLimit) are enforced by
// LifetimesAccepted alone, which must run after both values are parsed.
bool ParseMilliseconds(const std::string& value, std::uint32_t& milliseconds) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && milliseconds;
}
// 0 < operation deadline < serving lifetime <= kPreviewSessionLimit (180 s).
bool LifetimesAccepted(std::uint32_t operation_deadline, std::uint32_t serving_lifetime) {
    namespace timing = a0::phase0::experimental::preview_worker_timing;
    return operation_deadline < serving_lifetime &&
        serving_lifetime <= static_cast<std::uint64_t>(timing::kPreviewSessionLimit.count());
}
}
int main(int argc, char** argv) {
    if (argc != 4 || std::string_view(argv[1]) != "--delegated-worker") return 2;
    try {
        const auto parent = HandleArgument(argv[2]);
        const auto bootstrap = HandleArgument(argv[3]);
        if (!GetProcessId(parent) || GetProcessId(parent) == GetCurrentProcessId() ||
            WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || GetFileType(bootstrap) != FILE_TYPE_PIPE)
            return 2;
        std::uint32_t size{};
        ReadExact(bootstrap, &size, sizeof(size));
        if (!size || size > 1024) return 2;
        std::string wire(size, '\0');
        ReadExact(bootstrap, wire.data(), size);
        CloseHandle(bootstrap);
        const auto object = json::BasicJsonParser<Failure>(wire).Parse();
        if (object.kind != json::JsonKind::object) return 2;
        // pipe, epoch, capability, lifetimeMs, servingMs (+ leaseName and
        // testMarkerRoot in the SDK-stub test context). Checked before any
        // field is read, and every rejection below happens before the SDK
        // transport is constructed.
#if defined(A0_PREVIEW_STUB_TEST_CONTEXT)
        const bool test_root = object.object.size() == 7;
        if (!test_root && object.object.size() != 5) return 2;
#else
        constexpr bool test_root = false;
        if (object.object.size() != 5) return 2;
#endif
        const auto string = [&](const char* key) {
            return json::RequireFieldWith<Failure>(object, key, json::JsonKind::string).string;
        };
        const auto& pipe_name = string("pipe");
        const auto& epoch = string("epoch");
        const auto& capability = string("capability");
        // lifetimeMs is the operation deadline; servingMs the serving lifetime.
        const auto& operation_time = json::RequireFieldWith<Failure>(object, "lifetimeMs", json::JsonKind::number).string;
        const auto& serving_time = json::RequireFieldWith<Failure>(object, "servingMs", json::JsonKind::number).string;
        std::uint32_t operation_deadline{}, serving_lifetime{};
        if (!ParseMilliseconds(operation_time, operation_deadline) || !ParseMilliseconds(serving_time, serving_lifetime) ||
            !LifetimesAccepted(operation_deadline, serving_lifetime)) return 2;
        std::string lease_name = "A0CameraStitcher.Phase0.CameraControl.v1";
        std::filesystem::path test_marker_root;
        if (test_root) {
            lease_name = string("leaseName");
            const auto& root_utf8 = string("testMarkerRoot");
            if (lease_name.empty() || root_utf8.empty()) return 2;
            test_marker_root = std::filesystem::u8path(root_utf8);
        }
        const auto delegation_is_valid = [parent, epoch = std::string(epoch), lease_name, test_marker_root] {
            return HardwareProcessLease::ValidateWorkerDelegation(parent, epoch, lease_name, test_marker_root);
        };
        // Authenticate the durable parent/worker registration before loading an SDK transport.
        if (!delegation_is_valid()) return 2;
        NikonSdkTransport transport;
        const auto result = experimental::RunWorkerPreviewNamedPipeServer(pipe_name, transport,
            epoch, capability, parent, std::chrono::milliseconds(operation_deadline),
            std::chrono::milliseconds(serving_lifetime), delegation_is_valid);
        if (!result.safe_to_exit) {
            std::cerr << "Worker SDK shutdown unconfirmed; retained for human recovery.\n" << std::flush;
            Sleep(INFINITE); // Never kill/restart a worker with unknown SDK closure.
        }
        CloseHandle(parent);
        return result.ipc_exit_code;
    } catch (...) { return kWorkerMainExceptionExitCode; }
}
