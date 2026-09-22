#include "a0/phase0/worker_preview_dispatcher.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include <Windows.h>
#include <charconv>
#include <iostream>
using namespace a0::phase0;
namespace json = a0::common::protocol_json;
namespace {
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
        if (object.kind != json::JsonKind::object || object.object.size() != 4) return 2;
        const auto string = [&](const char* key) {
            return json::RequireFieldWith<Failure>(object, key, json::JsonKind::string).string;
        };
        const auto& time = json::RequireFieldWith<Failure>(object, "lifetimeMs", json::JsonKind::number).string;
        std::uint32_t lifetime{};
        const auto parsed = std::from_chars(time.data(), time.data() + time.size(), lifetime);
        if (parsed.ec != std::errc{} || parsed.ptr != time.data() + time.size() || !lifetime || lifetime > 600000) return 2;
        NikonSdkTransport transport;
        const auto result = experimental::RunWorkerPreviewNamedPipeServer(string("pipe"), transport,
            string("epoch"), string("capability"), parent, std::chrono::milliseconds(lifetime));
        if (!result.safe_to_exit) {
            std::cerr << "Worker SDK shutdown unconfirmed; retained for human recovery.\n" << std::flush;
            Sleep(INFINITE); // Never kill/restart a worker with unknown SDK closure.
        }
        CloseHandle(parent);
        return result.ipc_exit_code;
    } catch (...) { return 3; }
}
