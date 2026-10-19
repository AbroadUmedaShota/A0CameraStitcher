#include "a0/phase0/experimental/wpd_live_view_codec.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace a0::phase0::experimental;
namespace {
int checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void Rejects(F action, const char* message) {
    bool rejected{};
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}
WpdValues Values() {
    WpdValues values;
    if (CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&values)) != S_OK) throw std::runtime_error("COM values unavailable");
    return values;
}
WpdValues Reply(bool mtp = true) {
    auto values = Values();
    values->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, S_OK);
    if (mtp) values->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, 0x2001);
    return values;
}
WpdValues Begin(ULONGLONG total = 8) {
    auto values = Reply(false);
    values->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, L"synthetic-context-a");
    values->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, total);
    return values;
}
DWORD Number(IPortableDeviceValues* values, REFPROPERTYKEY key) {
    DWORD result{};
    Check(values->GetUnsignedIntegerValue(key, &result) == S_OK, "typed unsigned field");
    return result;
}
void Command(IPortableDeviceValues* values, REFPROPERTYKEY expected, DWORD field_count) {
    GUID category{}; DWORD count{};
    Check(values->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category) == S_OK &&
        IsEqualGUID(category, expected.fmtid), "WPD category");
    Check(Number(values, WPD_PROPERTY_COMMON_COMMAND_ID) == expected.pid, "WPD command id");
    Check(values->GetCount(&count) == S_OK && count == field_count, "no extra WPD command fields");
}
void RequestContracts() {
    const std::array operations{WpdLiveViewOperation::Start, WpdLiveViewOperation::Frame, WpdLiveViewOperation::Stop};
    // Expected codes from the independently pinned public Nikon implementation.
    const std::array<DWORD, 3> codes{0x9201, 0x9203, 0x9202};
    for (std::size_t i = 0; i < operations.size(); ++i) {
        auto request = BuildWpdLiveViewOperation(operations[i]);
        if (!request) { Check(false, "operation request exists"); continue; }
        Command(request.Get(), i == 1 ? WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ :
            WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE, 4);
        Check(Number(request.Get(), WPD_PROPERTY_MTP_EXT_OPERATION_CODE) == codes[i], "only selected Live View opcode");
        Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
        Check(request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS,
            &parameters) == S_OK && parameters, "required params collection");
        DWORD count = 99;
        Check(parameters && parameters->GetCount(&count) == S_OK && count == 0, "no camera settings or capture parameters");
    }
    Rejects([] { (void)BuildWpdLiveViewOperation(static_cast<WpdLiveViewOperation>(0x9207)); }, "capture opcode not an operation");
    Rejects([] { (void)BuildWpdLiveViewOperation(static_cast<WpdLiveViewOperation>(-1)); }, "unknown operation rejected");
    for (const auto* context : {L"synthetic-context-a", L"synthetic-context-b"}) {
        auto read = BuildWpdLiveViewRead(context, 8);
        Command(read.Get(), WPD_COMMAND_MTP_EXT_READ_DATA, 5);
        Check(Number(read.Get(), WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ) == 8, "bounded read size");
        LPWSTR actual{};
        Check(read->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &actual) == S_OK &&
            actual && std::wstring_view(actual) == context, "separate camera context preserved");
        CoTaskMemFree(actual);
        BYTE* buffer{}; DWORD size{};
        Check(read->GetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &buffer, &size) == S_OK &&
            buffer && size == 8, "required input read buffer");
        bool zero = size == 8;
        for (DWORD i = 0; buffer && i < size; ++i) zero = zero && buffer[i] == 0;
        Check(zero, "read buffer initialized");
        CoTaskMemFree(buffer);
        auto end = BuildWpdLiveViewEndTransfer(context);
        Command(end.Get(), WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER, 3);
        actual = nullptr;
        Check(end->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &actual) == S_OK &&
            actual && std::wstring_view(actual) == context, "end uses the same camera context");
        CoTaskMemFree(actual);
    }
    Rejects([] { (void)BuildWpdLiveViewRead(L"", 1); }, "empty context");
    Rejects([] { (void)BuildWpdLiveViewRead(std::wstring_view(L"a\0b", 3), 1); }, "embedded null context");
    Rejects([] { (void)BuildWpdLiveViewEndTransfer(std::wstring(WpdLiveViewMaxContextCharacters + 1, L'x')); }, "oversized context");
    Rejects([] { (void)BuildWpdLiveViewRead(L"a", 0); }, "zero read size");
    Rejects([] { (void)BuildWpdLiveViewRead(L"a", WpdLiveViewMaxChunkBytes + 1); }, "oversized read size");
    auto maximum = BuildWpdLiveViewRead(std::wstring(4096, L'x'), 65536);
    Check(Number(maximum.Get(), WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ) == 65536,
        "context/read allocation bounds inclusive");
    auto a = BuildWpdLiveViewRead(L"synthetic-context-a", 3);
    auto b = BuildWpdLiveViewRead(L"synthetic-context-b", 5);
    a->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, L"synthetic-context-a-updated");
    a->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, 1);
    LPWSTR preserved{};
    Check(b->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &preserved) == S_OK &&
        preserved && std::wstring_view(preserved) == L"synthetic-context-b" &&
        Number(b.Get(), WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ) == 5,
        "independent request objects do not share A/B transfer state");
    CoTaskMemFree(preserved);
}
void StatusContracts() {
    auto reply = Reply();
    Check(InspectWpdLiveViewCommand(S_OK, reply.Get(), true).Ok(), "all three status layers succeed");
    Check(InspectWpdLiveViewCommand(E_FAIL, reply.Get(), true).error == WpdLiveViewResultError::DispatchFailed, "dispatch failure not WPD success");
    Check(InspectWpdLiveViewCommand(S_FALSE, reply.Get(), true).error == WpdLiveViewResultError::DispatchFailed, "non-S_OK dispatch");
    Check(InspectWpdLiveViewCommand(S_OK, nullptr, true).error == WpdLiveViewResultError::MissingResults, "missing results");
    auto empty = Values();
    Check(InspectWpdLiveViewCommand(S_OK, empty.Get(), true).error == WpdLiveViewResultError::MissingCommandStatus, "missing command status");
    reply->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, E_NOTIMPL);
    Check(InspectWpdLiveViewCommand(S_OK, reply.Get(), true).error == WpdLiveViewResultError::CommandFailed, "driver rejection despite dispatch success");
    reply->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, S_FALSE);
    Check(!InspectWpdLiveViewCommand(S_OK, reply.Get(), true).Ok(), "non-S_OK command status");
    reply->SetStringValue(WPD_PROPERTY_COMMON_HRESULT, L"S_OK");
    Check(!InspectWpdLiveViewCommand(S_OK, reply.Get(), true).Ok(), "wrong command status type");
    reply = Reply(false);
    Check(InspectWpdLiveViewCommand(S_OK, reply.Get(), true).error == WpdLiveViewResultError::MissingMtpStatus, "final MTP status required");
    Check(InspectWpdLiveViewCommand(S_OK, reply.Get(), false).Ok(), "data phase need not have final response yet");
    reply->SetStringValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, L"8193");
    Check(!InspectWpdLiveViewCommand(S_OK, reply.Get(), false).Ok(), "optional wrong-type response is not ignored");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, 0x2019);
    const auto busy = InspectWpdLiveViewCommand(S_OK, reply.Get(), true);
    Check(busy.error == WpdLiveViewResultError::MtpRejected && busy.mtp_response == 0x2019, "busy retained as rejection without retry");
    Check(!InspectWpdLiveViewCommand(S_OK, reply.Get(), false).Ok(), "early MTP rejection also enforced");
    for (const auto code : {0U, 0x2002U, 0x201eU, 0xffffU, 0xffffffffU}) {
        reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, code);
        Check(InspectWpdLiveViewCommand(S_OK, reply.Get(), true).error == WpdLiveViewResultError::MtpRejected,
            "only explicit MTP OK is accepted");
    }
    reply->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, 0x2001);
    Check(!InspectWpdLiveViewCommand(S_OK, reply.Get(), true).Ok(), "wrong numeric MTP response type");
}
void BeginContracts() {
    auto reply = Begin();
    auto begin = InspectWpdLiveViewTransferBegin(S_OK, reply.Get());
    Check(begin.result.Ok() && begin.total_size_known && begin.total_bytes == 8 &&
        begin.context == L"synthetic-context-a" && begin.chunk_bytes == 65536, "known transfer metadata");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 3);
    Check(InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).chunk_bytes == 3, "smaller driver chunk hint");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 0xffffffff);
    Check(InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).chunk_bytes == 65536, "large hint bounded by allocation cap");
    reply = Begin(0xffffffffULL);
    begin = InspectWpdLiveViewTransferBegin(S_OK, reply.Get());
    Check(begin.result.Ok() && !begin.total_size_known && begin.total_bytes == 0, "unknown transfer size stays unknown");
    for (const auto size : {0ULL, 4194305ULL, 0xffffffffffffffffULL}) {
        reply = Begin(size); begin = InspectWpdLiveViewTransferBegin(S_OK, reply.Get());
        Check(begin.result.error == WpdLiveViewResultError::InvalidTransferSize &&
            begin.context == L"synthetic-context-a", "rejected size retains context for one cleanup");
    }
    reply = Begin(); reply->RemoveValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT);
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "missing context");
    reply = Begin(); reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, L"");
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "empty returned context");
    reply = Begin(); reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT,
        std::wstring(4097, L'x').c_str());
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "oversized returned context");
    reply = Begin(); reply->RemoveValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE);
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "missing total size");
    reply = Begin(); reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 0);
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "zero chunk hint");
    reply->SetStringValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, L"3");
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "wrong hint type");
    reply = Begin(4194304);
    Check(InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "payload allocation limit inclusive");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, 8);
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "total size requires UI8");
    reply = Begin(); reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, 3);
    Check(!InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).result.Ok(), "wrong context type");
    reply = Begin(); reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, std::wstring(4096, L'x').c_str());
    Check(InspectWpdLiveViewTransferBegin(S_OK, reply.Get()).context.size() == 4096, "returned context bound inclusive");
    Check(!InspectWpdLiveViewTransferBegin(E_FAIL, reply.Get()).result.Ok(), "begin preserves dispatch failure");
    Check(!InspectWpdLiveViewTransferBegin(S_OK, nullptr).result.Ok(), "begin missing result does not dereference null");
}
void ChunkContracts() {
    BYTE data[]{1, 2, 3, 0, 0};
    auto reply = Reply(false);
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, 3);
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, 3);
    auto chunk = InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5);
    Check(chunk.result.Ok() && chunk.bytes == std::vector<unsigned char>({1,2,3}), "exact opaque chunk bytes");
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, 5);
    chunk = InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5);
    Check(chunk.result.Ok() && chunk.bytes.size() == 3, "buffer capacity does not count as received bytes");
    Check(InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 2).result.error == WpdLiveViewResultError::InvalidChunk,
        "count/buffer larger than requested rejected");
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, 2);
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).result.Ok(), "count larger than returned buffer rejected");
    reply->RemoveValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA);
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).result.Ok(), "missing data rejected");
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, 3);
    reply->RemoveValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ);
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).result.Ok(), "missing byte count rejected");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, 0);
    Check(InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).bytes.empty(), "zero count never returns padding as a frame");
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 0).result.Ok(), "zero request rejected before parsing");
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 65537).result.Ok(), "oversized request rejected before parsing");
    reply = Reply(); reply->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, E_FAIL);
    chunk = InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5);
    Check(!chunk.result.Ok() && chunk.bytes.empty(), "failed WPD command publishes no bytes");
    Check(!InspectWpdLiveViewReadChunk(S_OK, nullptr, 5).result.Ok(), "read missing results");
    reply = Reply(false);
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, 3);
    reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, L"abc");
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).result.Ok(), "wrong read buffer type");
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, data, 3);
    reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, L"3");
    Check(!InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5).result.Ok(), "wrong read count type");
    reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, 3);
    chunk = InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5);
    BYTE other[]{9, 8, 7};
    reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, other, 3);
    const auto second = InspectWpdLiveViewReadChunk(S_OK, reply.Get(), 5);
    Check(chunk.bytes == std::vector<unsigned char>({1,2,3}) && second.bytes == std::vector<unsigned char>({9,8,7}),
        "read chunks own their bytes, independent of later COM results");
}
} // namespace
int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    try { RequestContracts(); StatusContracts(); BeginContracts(); ChunkContracts(); }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << error.what() << '\n'; }
    CoUninitialize();
    std::cout << "wpd_live_view_codec checks=" << checks << " failures=" << failures << " hardwareOperations=0\n";
    return failures ? 1 : 0;
}
