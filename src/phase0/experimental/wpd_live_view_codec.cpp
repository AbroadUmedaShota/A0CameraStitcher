#include "a0/phase0/experimental/wpd_live_view_codec.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <algorithm>
#include <stdexcept>

namespace a0::phase0::experimental {
namespace {
// Public source for the three operation numbers (no licensed Nikon PDF used):
// digiCamControl NikonBase.cs, commit a95ee52bbc1e192a7d1da038f3af6561b185d312.
// See docs/WPD_LIVE_VIEW_CODEC.md and third_party/digicamcontrol-NOTICE.txt.
constexpr DWORD StartOperation = 0x9201;
constexpr DWORD FrameOperation = 0x9203;
constexpr DWORD StopOperation = 0x9202;
constexpr DWORD MtpOk = 0x2001;

void Require(HRESULT status) {
    if (status != S_OK) throw std::runtime_error("WPD message construction failed");
}
struct Variant final {
    PROPVARIANT value{};
    Variant() = default;
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    ~Variant() { PropVariantClear(&value); }
};
bool Absent(HRESULT status) {
    return status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == TYPE_E_ELEMENTNOTFOUND;
}
void CheckContext(std::wstring_view context) {
    if (context.empty() || context.size() > WpdLiveViewMaxContextCharacters ||
        context.find(L'\0') != std::wstring_view::npos)
        throw std::invalid_argument("invalid WPD transfer context");
}
WpdValues Request(REFPROPERTYKEY command) {
    WpdValues values;
    Require(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&values)));
    Require(values->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, command.fmtid));
    Require(values->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, command.pid));
    return values;
}
WpdLiveViewCommandResult Fail(WpdLiveViewCommandResult result, WpdLiveViewResultError error) {
    result.error = error;
    return result;
}
bool Unsigned32(IPortableDeviceValues* values, REFPROPERTYKEY key, DWORD& number) {
    Variant value;
    if (values->GetValue(key, &value.value) != S_OK || value.value.vt != VT_UI4) return false;
    number = value.value.ulVal;
    return true;
}
} // namespace

WpdValues BuildWpdLiveViewOperation(WpdLiveViewOperation operation) {
    DWORD code{};
    switch (operation) {
    case WpdLiveViewOperation::Start: code = StartOperation; break;
    case WpdLiveViewOperation::Frame: code = FrameOperation; break;
    case WpdLiveViewOperation::Stop: code = StopOperation; break;
    default: throw std::invalid_argument("unsupported Live View operation");
    }
    auto values = Request(operation == WpdLiveViewOperation::Frame ?
        WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ :
        WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE);
    Require(values->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, code));
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> parameters;
    Require(CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&parameters)));
    Require(values->SetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, parameters.Get()));
    return values;
}

WpdValues BuildWpdLiveViewRead(std::wstring_view context, DWORD requested_bytes) {
    CheckContext(context);
    if (requested_bytes == 0 || requested_bytes > WpdLiveViewMaxChunkBytes)
        throw std::invalid_argument("invalid WPD read size");
    auto values = Request(WPD_COMMAND_MTP_EXT_READ_DATA);
    const std::wstring owned_context(context);
    Require(values->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, owned_context.c_str()));
    Require(values->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, requested_bytes));
    std::vector<BYTE> buffer(requested_bytes, 0);
    Require(values->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, buffer.data(), requested_bytes));
    return values;
}

WpdValues BuildWpdLiveViewEndTransfer(std::wstring_view context) {
    CheckContext(context);
    auto values = Request(WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER);
    const std::wstring owned_context(context);
    Require(values->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, owned_context.c_str()));
    return values;
}

WpdLiveViewCommandResult InspectWpdLiveViewCommand(
    HRESULT dispatch_status, IPortableDeviceValues* results, bool require_mtp_response) {
    WpdLiveViewCommandResult result;
    result.dispatch_status = dispatch_status;
    if (dispatch_status != S_OK) return Fail(result, WpdLiveViewResultError::DispatchFailed);
    if (!results) return Fail(result, WpdLiveViewResultError::MissingResults);
    Variant status;
    if (results->GetValue(WPD_PROPERTY_COMMON_HRESULT, &status.value) != S_OK || status.value.vt != VT_ERROR)
        return Fail(result, WpdLiveViewResultError::MissingCommandStatus);
    result.command_status = status.value.scode;
    if (result.command_status != S_OK) return Fail(result, WpdLiveViewResultError::CommandFailed);
    Variant response;
    const auto response_status = results->GetValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, &response.value);
    if (!require_mtp_response && Absent(response_status)) return result;
    if (response_status != S_OK || response.value.vt != VT_UI4)
        return Fail(result, WpdLiveViewResultError::MissingMtpStatus);
    result.mtp_response = response.value.ulVal;
    if (result.mtp_response != MtpOk) return Fail(result, WpdLiveViewResultError::MtpRejected);
    return result;
}

WpdLiveViewTransferBegin InspectWpdLiveViewTransferBegin(HRESULT dispatch_status, IPortableDeviceValues* results,
    std::uint64_t maximum_payload_bytes) {
    WpdLiveViewTransferBegin begin;
    begin.result = InspectWpdLiveViewCommand(dispatch_status, results, false);
    if (!begin.result.Ok()) return begin;
    Variant context;
    if (results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &context.value) != S_OK ||
        context.value.vt != VT_LPWSTR || !context.value.pwszVal) {
        begin.result = Fail(begin.result, WpdLiveViewResultError::InvalidContext); return begin;
    }
    const auto length = wcsnlen_s(context.value.pwszVal, WpdLiveViewMaxContextCharacters + 1);
    if (length == 0 || length > WpdLiveViewMaxContextCharacters) {
        begin.result = Fail(begin.result, WpdLiveViewResultError::InvalidContext); return begin;
    }
    begin.context.assign(context.value.pwszVal, length);
    Variant size;
    if (results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, &size.value) != S_OK || size.value.vt != VT_UI8) {
        begin.result = Fail(begin.result, WpdLiveViewResultError::InvalidTransferSize); return begin;
    }
    const auto bytes = size.value.uhVal.QuadPart;
    begin.total_size_known = bytes != 0xffffffffULL;
    if (maximum_payload_bytes==0 || maximum_payload_bytes>WpdStillImageMaxPayloadBytes ||
        (begin.total_size_known && (bytes == 0 || bytes > maximum_payload_bytes))) {
        begin.result = Fail(begin.result, WpdLiveViewResultError::InvalidTransferSize); return begin;
    }
    begin.total_bytes = begin.total_size_known ? bytes : 0;
    Variant hint;
    const auto hint_status = results->GetValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, &hint.value);
    if (!Absent(hint_status)) {
        if (hint_status != S_OK || hint.value.vt != VT_UI4 || hint.value.ulVal == 0) {
            begin.result = Fail(begin.result, WpdLiveViewResultError::InvalidChunk); return begin;
        }
        begin.chunk_bytes = std::min(WpdLiveViewMaxChunkBytes, hint.value.ulVal);
    }
    return begin;
}

WpdLiveViewReadChunk InspectWpdLiveViewReadChunk(HRESULT dispatch_status, IPortableDeviceValues* results, DWORD requested_bytes) {
    WpdLiveViewReadChunk chunk;
    if (requested_bytes == 0 || requested_bytes > WpdLiveViewMaxChunkBytes) {
        chunk.result = Fail(chunk.result, WpdLiveViewResultError::InvalidChunk); return chunk;
    }
    chunk.result = InspectWpdLiveViewCommand(dispatch_status, results, false);
    if (!chunk.result.Ok()) return chunk;
    DWORD received{};
    Variant buffer;
    if (!Unsigned32(results, WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, received) ||
        results->GetValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &buffer.value) != S_OK ||
        buffer.value.vt != (VT_VECTOR | VT_UI1) || received > requested_bytes ||
        received > buffer.value.caub.cElems || buffer.value.caub.cElems > requested_bytes ||
        (buffer.value.caub.cElems != 0 && !buffer.value.caub.pElems)) {
        chunk.result = Fail(chunk.result, WpdLiveViewResultError::InvalidChunk); return chunk;
    }
    if (received) chunk.bytes.assign(buffer.value.caub.pElems, buffer.value.caub.pElems + received);
    return chunk;
}
} // namespace a0::phase0::experimental
