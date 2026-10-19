#include "a0/phase0/experimental/wpd_live_view_transfer.hpp"
#include "a0/phase0/experimental/d810_preview.hpp"
#include <PortableDevice.h>
#include <WpdMtpExtensions.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace a0::phase0::experimental;
namespace {
int checks{}, failures{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Require(bool value) { if (!value) throw std::runtime_error("fake channel contract mismatch"); }
WpdValues Values(bool final = false) {
    WpdValues values;
    Require(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&values)) == S_OK);
    Require(values->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, S_OK) == S_OK);
    if (final) Require(values->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, 0x2001) == S_OK);
    return values;
}
DWORD Number(IPortableDeviceValues* values, REFPROPERTYKEY key) {
    DWORD value{}; Require(values->GetUnsignedIntegerValue(key, &value) == S_OK); return value;
}
enum class Step { Begin, Read, End };
class Fake final : public WpdLiveViewCommandChannel {
public:
    std::wstring context{L"synthetic-transfer-a"};
    std::vector<unsigned char> data{10,20,30,40,50,60,70,80};
    std::optional<ULONGLONG> advertised;
    bool unknown{}, reject_begin{}, bad_context{}, bad_size{}, bad_read{}, reject_end{}, missing_end_response{};
    std::optional<Step> throw_at;
    std::optional<Step> fail_dispatch_at;
    DWORD hint{3}, short_read_limit{};
    std::size_t begins{}, reads{}, ends{}, offset{};
    std::vector<DWORD> requests;
    std::function<void(Step)> on_reply;
protected:
    WpdLiveViewExchange Dispatch(IPortableDeviceValues* request) override {
        GUID category{};
        Require(request->GetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, &category) == S_OK &&
            IsEqualGUID(category, WPD_CATEGORY_MTP_EXT_VENDOR_OPERATIONS));
        const auto command = Number(request, WPD_PROPERTY_COMMON_COMMAND_ID);
        Step step{};
        WpdValues reply;
        if (command == WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ.pid) {
            ++begins; step = Step::Begin;
            offset = 0;
            Require(Number(request, WPD_PROPERTY_MTP_EXT_OPERATION_CODE) == 0x9203);
            reply = Values();
            if (reject_begin) reply->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, E_NOTIMPL);
            if (!bad_context) reply->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.c_str());
            reply->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE,
                bad_size ? 0 : unknown ? 0xffffffffULL : advertised.value_or(data.size()));
            reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, hint);
        } else {
            LPWSTR actual{};
            Require(request->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &actual) == S_OK && actual);
            const bool matches = context == actual; CoTaskMemFree(actual); Require(matches);
            if (command == WPD_COMMAND_MTP_EXT_READ_DATA.pid) {
                ++reads; step = Step::Read;
                const auto requested = Number(request, WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ);
                Require(requested > 0 && requested <= 65536);
                requests.push_back(requested);
                auto count = std::min<std::size_t>(requested, data.size() - offset);
                if (short_read_limit) count = std::min<std::size_t>(count, short_read_limit);
                // The driver can return capacity as well as a smaller byte count.
                std::vector<BYTE> buffer(std::max<std::size_t>(count, 1), 0);
                std::copy_n(data.begin() + offset, count, buffer.begin()); offset += count;
                reply = Values();
                reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ,
                    bad_read ? requested + 1 : static_cast<DWORD>(count));
                reply->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, buffer.data(), static_cast<DWORD>(buffer.size()));
            } else {
                Require(command == WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER.pid);
                ++ends; step = Step::End;
                reply = Values(!missing_end_response);
                if (reject_end) reply->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, 0x2019);
            }
        }
        if (on_reply) on_reply(step);
        if (throw_at == step) throw std::runtime_error("synthetic dispatch failure");
        return {fail_dispatch_at == step ? E_ACCESSDENIED : S_OK, std::move(reply)};
    }
};
WpdLiveViewTransferOptions Options() {
    WpdLiveViewTransferOptions o;
    o.may_continue = [] { return true; }; o.may_cleanup = [] { return true; };
    return o;
}
void Failed(const WpdLiveViewTransferResult& result, WpdLiveViewTransferError expected, const Fake& fake,
            std::size_t begins, std::size_t reads, std::size_t ends) {
    Check(result.error == expected && result.payload.empty(), "failure has expected reason and no payload");
    Check(fake.begins == begins && fake.reads == reads && fake.ends == ends, "exact no-retry command counts");
    const bool no_dispatch = expected == WpdLiveViewTransferError::ChannelRetired;
    Check(result.read_commands == (no_dispatch ? 0 : reads) &&
        result.begin_dispatched == (!no_dispatch && begins != 0), "dispatch observations belong to this invocation");
}
void SuccessContracts() {
    Fake known; auto result = ReadWpdLiveViewPayload(known, Options());
    Check(result.Ok() && result.payload == known.data && result.received_bytes == 8, "known payload bytes");
    Check(known.begins == 1 && known.reads == 3 && known.ends == 1, "one begin, three chunks, one end");
    Check(result.end_attempted && result.end_confirmed && result.end_result && result.end_result->Ok() &&
        !result.channel_retired && !known.Retired(), "only final confirmed transfer succeeds");
    Check(known.requests == std::vector<DWORD>({3,3,2}), "last known-size request is exact remaining size");
    known.data = {101,102}; known.context = L"synthetic-transfer-a-next-frame";
    result = ReadWpdLiveViewPayload(known, Options());
    Check(result.Ok() && result.payload == known.data && known.begins == 2 && known.reads == 4 && known.ends == 2,
        "successful channel supports next frame with its new transfer context");
    Fake partial; partial.short_read_limit = 1;
    result = ReadWpdLiveViewPayload(partial, Options());
    Check(result.Ok() && result.payload == partial.data && partial.reads == 8, "known size tolerates short nonzero progress");
    Fake unknown; unknown.unknown = true;
    result = ReadWpdLiveViewPayload(unknown, Options());
    Check(result.Ok() && result.payload == unknown.data && unknown.reads == 3, "unknown size ends with short read");
    Fake exact; exact.unknown = true; exact.data = {1,2,3,4,5,6};
    result = ReadWpdLiveViewPayload(exact, Options());
    Check(result.Ok() && result.payload == exact.data && exact.reads == 3, "unknown exact chunk multiple needs terminal zero read");
    Fake limited; limited.unknown = true; auto options = Options(); options.max_payload_bytes = 8;
    result = ReadWpdLiveViewPayload(limited, options);
    Check(result.Ok() && result.payload == limited.data, "unknown payload limit inclusive");
    Check(limited.requests == std::vector<DWORD>({3,3,3}), "unknown capped window includes at most one sentinel byte");
    Fake exact_limit; exact_limit.unknown = true; exact_limit.data = {1,2,3,4,5,6};
    options.max_payload_bytes = 6; result = ReadWpdLiveViewPayload(exact_limit, options);
    Check(result.Ok() && exact_limit.requests == std::vector<DWORD>({3,3,1}), "one-byte EOF probe at exact payload limit");
    Fake chunk_limit; options = Options(); options.max_chunk_bytes = 2;
    result = ReadWpdLiveViewPayload(chunk_limit, options);
    Check(result.Ok() && chunk_limit.requests == std::vector<DWORD>({2,2,2,2}), "owner chunk cap overrides driver hint");
    Fake full; full.data.assign(4194304, 0x5a); full.hint = 65536;
    result = ReadWpdLiveViewPayload(full, Options());
    Check(result.Ok() && result.payload == full.data && full.reads == 64, "maximum known payload stays bounded and byte exact");
}
void FailureContracts() {
    { Fake f;bool stop=false,begun=false;auto o=Options();
      o.may_continue=[&]{return ContinuePreviewFrame(stop,begun,false);};
      f.on_reply=[&](Step step){if(step==Step::Begin)begun=true;if(step==Step::Read)stop=true;};
      auto r=ReadWpdLiveViewPayload(f,o);
      Check(r.Ok() && r.payload==f.data && f.reads==3 && f.ends==1,"graceful stop drains all remaining bytes before END");
      begun=false;auto next=ReadWpdLiveViewPayload(f,o);
      Check(next.error==WpdLiveViewTransferError::NotAdmitted && f.begins==1,"stop never begins another frame"); }
    { Fake f;bool stop=false,begun=false,expired=false;auto o=Options();
      o.may_continue=[&]{return ContinuePreviewFrame(stop,begun,expired);};
      f.on_reply=[&](Step step){if(step==Step::Begin)begun=true;if(step==Step::Read){stop=true;expired=true;}};
      auto r=ReadWpdLiveViewPayload(f,o);
      Check(!r.Ok() && r.payload.empty() && f.reads==1 && f.ends==1,"draining a stop cannot extend the original deadline"); }
    { Fake f; f.reject_begin = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::BeginRejected, f, 1, 0, 0);
      Check(r.channel_retired && f.Retired() && !r.end_attempted, "uncertain begin requires channel close"); }
    { Fake f; f.bad_context = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::BeginRejected, f, 1, 0, 0); }
    { Fake f; f.bad_size = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::BeginRejected, f, 1, 0, 1);
      Check(r.end_confirmed && r.command_failure, "metadata failure still ends acquired context once"); }
    { Fake f; f.hint = 0; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::BeginRejected, f, 1, 0, 1); }
    { Fake f; auto o = Options(); o.max_payload_bytes = 7; auto r = ReadWpdLiveViewPayload(f, o);
      Failed(r, WpdLiveViewTransferError::PayloadLimitExceeded, f, 1, 0, 1); }
    { Fake f; f.unknown = true; auto o = Options(); o.max_payload_bytes = 6; auto r = ReadWpdLiveViewPayload(f, o);
      Failed(r, WpdLiveViewTransferError::PayloadLimitExceeded, f, 1, 3, 1);
      Check(r.received_bytes == 7, "overflow sentinel counted without publishing or appending it"); }
    { Fake f; f.data = {1,2}; f.advertised = 8; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::IncompletePayload, f, 1, 2, 1); }
    { Fake f; f.data.clear(); f.unknown = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::EmptyPayload, f, 1, 1, 1); }
    { Fake f; f.bad_read = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::ReadRejected, f, 1, 1, 1);
      Check(r.command_failure && r.end_confirmed, "read reason distinct from cleanup status"); }
    { Fake f; auto o = Options(); o.max_read_commands = 2; auto r = ReadWpdLiveViewPayload(f, o);
      Failed(r, WpdLiveViewTransferError::ReadLimitExceeded, f, 1, 2, 1); }
    { Fake f; f.reject_end = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::EndRejected, f, 1, 3, 1);
      Check(!r.end_confirmed && r.end_result && r.end_result->mtp_response == 0x2019, "all bytes insufficient without final MTP OK"); }
    { Fake f; f.missing_end_response = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::EndRejected, f, 1, 3, 1); }
    { Fake f; f.bad_read = true; f.reject_end = true; auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::ReadRejected, f, 1, 1, 1);
      Check(r.command_failure && r.end_result && !r.end_confirmed, "original failure and cleanup failure both retained");
      auto again = ReadWpdLiveViewPayload(f, Options());
      Failed(again, WpdLiveViewTransferError::ChannelRetired, f, 1, 1, 1);
      Check(again.channel_retired, "failed channel cannot run another transfer"); }
    { Fake f; f.reject_begin = true; (void)ReadWpdLiveViewPayload(f, Options());
      auto o = Options(); o.max_payload_bytes = 0; const auto r = ReadWpdLiveViewPayload(f, o);
      Check(r.error == WpdLiveViewTransferError::InvalidOptions && r.channel_retired && f.begins == 1,
        "invalid new options do not hide an already retired channel"); }
    for (const auto stage : {Step::Begin, Step::Read, Step::End}) {
        Fake f; f.throw_at = stage; auto r = ReadWpdLiveViewPayload(f, Options());
        Failed(r, WpdLiveViewTransferError::DispatcherException, f, 1, stage == Step::Begin ? 0 : stage == Step::Read ? 1 : 3,
            stage == Step::Begin ? 0 : 1);
        Check(r.channel_retired, "throwing channel is retired");
    }
    for (const auto stage : {Step::Begin, Step::Read, Step::End}) {
        Fake f; f.fail_dispatch_at = stage; auto r = ReadWpdLiveViewPayload(f, Options());
        const auto expected = stage == Step::Begin ? WpdLiveViewTransferError::BeginRejected :
            stage == Step::Read ? WpdLiveViewTransferError::ReadRejected : WpdLiveViewTransferError::EndRejected;
        Failed(r, expected, f, 1, stage == Step::Begin ? 0 : stage == Step::Read ? 1 : 3, stage == Step::Begin ? 0 : 1);
        const auto diagnostic = stage == Step::End ? r.end_result : r.command_failure;
        Check(diagnostic && diagnostic->dispatch_status == E_ACCESSDENIED && r.channel_retired,
            "each stage preserves failed dispatch HRESULT without retry");
    }
    { Fake f; f.unknown = true; f.hint = 65536; f.data.assign(4194305, 0x5a);
      const auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::PayloadLimitExceeded, f, 1, 65, 1);
      Check(r.received_bytes == 4194305 && f.requests.back() == 1 && r.end_confirmed,
        "maximum unknown-size overflow bounded to one sentinel byte"); }
    { Fake f; f.bad_read = true; f.throw_at = Step::End;
      const auto r = ReadWpdLiveViewPayload(f, Options());
      Failed(r, WpdLiveViewTransferError::ReadRejected, f, 1, 1, 1);
      Check(r.end_error == WpdLiveViewTransferError::DispatcherException && !r.end_confirmed && !r.end_result,
        "cleanup exception kept separately from original read rejection"); }
}
void AdmissionContracts() {
    { Fake f; auto o = Options(); o.may_continue = [] { return false; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::NotAdmitted, f, 0, 0, 0);
      Check(!f.Retired(), "unattempted operation does not corrupt channel state"); }
    { Fake f; auto o = Options(); int admissions{}; o.may_continue = [&] { return ++admissions < 2; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::NotAdmitted, f, 0, 0, 0); }
    { Fake f; auto o = Options(); int admissions{}; o.may_continue = [&] { return ++admissions < 5; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::Interrupted, f, 1, 0, 1);
      Check(r.end_confirmed, "recheck immediately before read dispatch prevents late read"); }
    for (const auto stage : {Step::Begin, Step::Read, Step::End}) {
        Fake f; bool allow = true; auto o = Options(); o.may_continue = [&] { return allow; };
        f.on_reply = [&](Step at) { if (at == stage) allow = false; };
        auto r = ReadWpdLiveViewPayload(f, o);
        Failed(r, WpdLiveViewTransferError::Interrupted, f, 1, stage == Step::Begin ? 0 : stage == Step::Read ? 1 : 3, 1);
        Check(r.end_confirmed && r.channel_retired, "late result never succeeds; cleanup allowed after revocation");
    }
    { Fake f; auto o = Options(); o.may_continue = []() -> bool { throw std::runtime_error("synthetic admission error"); };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::AdmissionException, f, 0, 0, 0); }
    { Fake f; auto o = Options(); bool broken = false; f.on_reply = [&](Step) { broken = true; };
      o.may_continue = [&]() -> bool { if (broken) throw std::runtime_error("synthetic admission error"); return true; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::AdmissionException, f, 1, 0, 1); }
    { Fake f; auto o = Options(); o.may_cleanup = [] { return false; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::CleanupNotAdmitted, f, 1, 3, 0);
      Check(!r.end_attempted && !r.end_confirmed && f.Retired(), "ownership loss forbids even END dispatch"); }
    { Fake f; auto o = Options(); int cleanup_admissions{};
      o.may_cleanup = [&] { return ++cleanup_admissions < 2; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::CleanupNotAdmitted, f, 1, 3, 0);
      Check(!r.end_attempted, "cleanup ownership rechecked immediately before dispatch"); }
    { Fake f; auto o = Options(); o.may_cleanup = []() -> bool { throw std::runtime_error("synthetic cleanup admission failure"); };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::AdmissionException, f, 1, 3, 0); }
    { Fake f; f.bad_read = true; auto o = Options(); o.may_cleanup = [] { return false; };
      auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::ReadRejected, f, 1, 1, 0);
      Check(r.end_error == WpdLiveViewTransferError::CleanupNotAdmitted && !r.end_confirmed,
        "read failure and cleanup ownership loss remain distinct"); }
    for (int mutation = 0; mutation < 8; ++mutation) {
        Fake f; auto o = Options();
        switch (mutation) {
        case 0: o.may_continue = {}; break;
        case 1: o.max_payload_bytes = 0; break;
        case 2: o.max_payload_bytes = 4194305; break;
        case 3: o.max_chunk_bytes = 0; break;
        case 4: o.max_chunk_bytes = 65537; break;
        case 5: o.max_read_commands = 0; break;
        case 6: o.max_read_commands = 4097; break;
        case 7: o.may_cleanup = {}; break;
        }
        auto r = ReadWpdLiveViewPayload(f, o); Failed(r, WpdLiveViewTransferError::InvalidOptions, f, 0, 0, 0);
    }
}
void IndependentChannels() {
    Fake a, b; b.context = L"synthetic-transfer-b"; b.data = {91,92,93,94};
    std::mutex mutex; std::condition_variable cv; bool entered{}, release{};
    a.on_reply = [&](Step step) {
        if (step != Step::Read) return;
        std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return release; });
    };
    auto run_a = std::async(std::launch::async, [&] {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) throw std::runtime_error("worker COM initialization failed");
        auto result = ReadWpdLiveViewPayload(a, Options()); CoUninitialize(); return result;
    });
    bool observed{};
    { std::unique_lock lock(mutex); observed = cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }); }
    // Always release the worker, including on assertion failure; no hanging test.
    WpdLiveViewTransferResult busy, rb;
    if (observed) { busy = ReadWpdLiveViewPayload(a, Options()); rb = ReadWpdLiveViewPayload(b, Options()); }
    { std::lock_guard lock(mutex); release = true; } cv.notify_all();
    const auto ra = run_a.get();
    Check(observed, "A reached an explicit wait point");
    Check(busy.error == WpdLiveViewTransferError::ChannelBusy && !busy.channel_retired,
        "same camera overlapping transfer refused without retiring owner");
    Check(ra.Ok() && rb.Ok() && ra.payload == a.data && rb.payload == b.data,
        "B finishes while A waits, independent contexts and payloads");
    Check(a.begins == 1 && a.ends == 1 && b.begins == 1 && b.ends == 1 && !a.Retired() && !b.Retired(),
        "both channels have exactly one final response with no retry");
}
void BeginFactoryContracts() {
    { Fake f; auto o=Options(); int built=0;
      o.begin_request=[&]{++built;return BuildWpdLiveViewOperation(WpdLiveViewOperation::Frame);};
      const auto r=ReadWpdLiveViewPayload(f,o);
      Check(r.Ok() && built==1 && r.payload==f.data,"custom read begin is built once and retains verified transfer semantics"); }
    { Fake f;auto o=Options();o.begin_request=[]{return WpdValues{};};
      const auto r=ReadWpdLiveViewPayload(f,o);
      Check(!r.Ok() && !r.begin_dispatched && f.begins==0 && r.payload.empty() && !f.Retired(),"null begin factory dispatches nothing"); }
    { Fake f;auto o=Options();o.begin_request=[]()->WpdValues{throw std::runtime_error("synthetic factory failure");};
      const auto r=ReadWpdLiveViewPayload(f,o);
      Check(!r.Ok() && !r.begin_dispatched && f.begins==0 && r.payload.empty(),"throwing begin factory dispatches nothing"); }
}
void StillImageContracts(){
    {Fake f;f.data.assign(4194320,0x5a);f.hint=65536;auto o=Options();o.purpose=WpdPayloadPurpose::StillImage;o.max_payload_bytes=f.data.size();
     std::vector<unsigned char> partial;o.on_chunk=[&](auto chunk){partial.insert(partial.end(),chunk.begin(),chunk.end());};
     auto r=ReadWpdLiveViewPayload(f,o);Check(r.Ok() && r.payload==f.data && partial==f.data && r.end_confirmed,"explicit still purpose admits larger JPEG and bounded partial sink");}
    {Fake f;f.reject_end=true;auto o=Options();o.purpose=WpdPayloadPurpose::StillImage;size_t partial=0;o.on_chunk=[&](auto c){partial+=c.size();};
     auto r=ReadWpdLiveViewPayload(f,o);Check(!r.Ok() && r.payload.empty() && partial==f.data.size() && f.ends==1,"failed final response retains only private partial bytes, never a verified image");}
    {Fake f;auto o=Options();o.purpose=WpdPayloadPurpose::StillImage;o.on_chunk=[](auto){throw std::runtime_error("synthetic disk failure");};
     auto r=ReadWpdLiveViewPayload(f,o);Check(!r.Ok() && r.payload.empty() && r.end_confirmed && f.ends==1,"sink failure closes transfer once without publication");}
    {Fake f;auto o=Options();o.on_chunk=[](auto){};auto r=ReadWpdLiveViewPayload(f,o);Check(r.error==WpdLiveViewTransferError::InvalidOptions && f.begins==0,"preview cannot stream unverified bytes to a sink");}
    {Fake f;auto o=Options();o.purpose=static_cast<WpdPayloadPurpose>(9);auto r=ReadWpdLiveViewPayload(f,o);Check(r.error==WpdLiveViewTransferError::InvalidOptions && f.begins==0,"unknown purpose rejected before send");}
    {Fake f;auto o=Options();o.purpose=WpdPayloadPurpose::StillImage;o.max_payload_bytes=WpdStillImageMaxPayloadBytes+1;auto r=ReadWpdLiveViewPayload(f,o);Check(r.error==WpdLiveViewTransferError::InvalidOptions && f.begins==0,"still limit stays finite");}
}
}
int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--success-only")) return 2;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 1;
    try { SuccessContracts(); if (argc == 1) { FailureContracts(); AdmissionContracts(); IndependentChannels(); BeginFactoryContracts(); StillImageContracts(); } }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << e.what() << '\n'; }
    CoUninitialize();
    std::cout << "wpd_live_view_transfer checks=" << checks << " failures=" << failures << " hardwareOperations=0\n";
    return failures ? 1 : 0;
}
