// Contracts of the camera settings write (#416 stage B, ADR-0038; ADR-0041 paths
// stay off without their approval record). Synthetic bodies only: in-process WPD
// value objects and files in a temporary folder. No device, no camera.
#include "a0/phase0/experimental/wpd_settings_write.hpp"
#include "a0/phase0/experimental/d810_settings_write_screen.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <intrin.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace a0::phase0::experimental;
namespace fs = std::filesystem;
namespace {
int failures = 0, checks = 0;
void check(bool value, const char* what = "") {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAILED check " << checks << " " << what << "\n";
    }
}
// A refusal token as text; "(none)" when there is no refusal, so that a missing refusal fails a check instead of
// reading a null pointer.
std::string Refusal(const char* token) { return token ? std::string(token) : std::string("(none)"); }
using Bytes = std::vector<unsigned char>;
void Put(Bytes& out, std::int64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) out.push_back(static_cast<unsigned char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xff));
}
std::size_t SizeOfType(unsigned type) { return type == 1 || type == 2 ? 1 : type == 3 || type == 4 ? 2 : 4; }
Bytes Desc(unsigned code, unsigned type, unsigned get_set, std::int64_t current, unsigned form, const std::vector<std::int64_t>& values) {
    Bytes out;
    const auto size = SizeOfType(type);
    Put(out, code, 2);
    Put(out, type, 2);
    Put(out, get_set, 1);
    Put(out, current, size);
    Put(out, current, size);
    Put(out, form, 1);
    if (form == 2) Put(out, static_cast<std::int64_t>(values.size()), 2);
    for (const auto value : values) Put(out, value, size);
    return out;
}
bool HasIdentifierShape(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::regex guid("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-");
    return lower.find("\\\\?\\") != std::string::npos || lower.find("usb#") != std::string::npos || lower.find("vid_") != std::string::npos ||
        lower.find('{') != std::string::npos || std::regex_search(lower, guid) || lower.find("ctx-") != std::string::npos;
}

// --- WPD helpers ------------------------------------------------------------------
WpdValues Reply(bool final_reply = false, DWORD mtp = 0x2001) {
    WpdValues result;
    CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result));
    result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, S_OK);
    if (final_reply) result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, mtp);
    return result;
}
WpdLiveViewExchange FinalReply(DWORD mtp = 0x2001) { return {S_OK, Reply(true, mtp)}; }
WpdLiveViewExchange ReadBeginReply(const std::wstring& context, std::size_t total) {
    auto result = Reply();
    result->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.c_str());
    result->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, total);
    result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPTIMAL_TRANSFER_BUFFER_SIZE, 64);
    return {S_OK, result};
}
WpdLiveViewExchange ChunkReply(const Bytes& bytes) {
    static unsigned char none = 0;
    auto result = Reply();
    result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, static_cast<DWORD>(bytes.size()));
    result->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, bytes.empty() ? &none : const_cast<unsigned char*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    return {S_OK, result};
}
WpdLiveViewExchange WriteBeginReply(const std::wstring& context) {
    auto result = Reply();
    result->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.c_str());
    return {S_OK, result};
}
WpdLiveViewExchange WrittenReply(DWORD count) {
    auto result = Reply();
    result->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_WRITTEN, count);
    return {S_OK, result};
}
WpdLiveViewExchange CommandFailedReply() {
    auto result = Reply();
    result->SetErrorValue(WPD_PROPERTY_COMMON_HRESULT, E_FAIL);
    return {S_OK, result};
}

// --- A synthetic body that keeps its settings and applies writes -----------------------
struct Prop {
    unsigned type{};
    unsigned get_set{1};
    std::int64_t current{};
    unsigned form{2};
    std::vector<std::int64_t> values;
};
struct FakeWriteBody {
    std::map<std::uint16_t, Prop> props;
    unsigned live = 0;
    std::set<std::uint16_t> begin_throw, begin_reject, begin_command_failed, data_throw, data_fail, data_short, end_throw, ignore_write;
    std::map<std::uint16_t, DWORD> end_reject;
    std::map<std::uint16_t, std::int64_t> apply_as;
    std::set<std::uint16_t> unreadable_after_write;
    std::map<std::uint16_t, int> begins_by_code;
    // The n-th description read of a code (1-based, every read of the body
    // counted) answers as a real body did in stage A: the begin succeeds, the
    // data is 0 bytes, and the END is refused with 0x200A.
    std::map<std::uint16_t, int> zero_at;
    std::map<std::uint16_t, int> reads_of;
    int write_begins = 0, write_datas = 0, write_ends = 0, read_begins = 0, forbidden = 0;
    std::vector<std::string> trace;
    std::function<void(const std::string&)> after;

    Bytes DescBytes(std::uint16_t code) const {
        const auto& p = props.at(code);
        return Desc(code, p.type, p.get_set, p.current, p.form, p.values);
    }
    WpdLiveViewExchange operator()(IPortableDeviceValues* request) {
        const auto shape = InspectSettingsRequest(request);
        char text[48];
        const auto note = [&](const std::string& entry) { trace.push_back(entry); if (after) after(entry); };
        if (!shape) { ++forbidden; throw std::runtime_error("unreadable request reached the body"); }
        switch (shape->kind) {
        case SettingsCommandKind::ExecuteWithoutData:
            if (shape->opcode != 0x90c8) { ++forbidden; throw std::runtime_error("forbidden operation"); }
            note("90c8");
            return FinalReply();
        case SettingsCommandKind::ExecuteWithDataToRead: {
            if ((shape->opcode != 0x1014 && shape->opcode != 0x1015) || shape->arguments.size() != 1) { ++forbidden; throw std::runtime_error("forbidden read"); }
            ++read_begins;
            const auto code = static_cast<std::uint16_t>(shape->arguments[0]);
            std::snprintf(text, sizeof text, "%04x(%04x)", static_cast<unsigned>(shape->opcode), static_cast<unsigned>(code));
            note(text);
            if (shape->opcode == 0x1015) data_ = Bytes{static_cast<unsigned char>(live)};
            else {
                if (!props.count(code) || (unreadable_after_write.count(code) && begins_by_code[code] > 0)) return FinalReply(0x200a);
                data_ = DescBytes(code);
                if (zero_at.count(code) && ++reads_of[code] == zero_at[code]) {
                    data_.clear();
                    zero_ = true;
                }
            }
            offset_ = 0;
            writing_ = false;
            return ReadBeginReply(L"ctx-r" + std::to_wstring(read_begins), data_.size());
        }
        case SettingsCommandKind::ExecuteWithDataToWrite: {
            if (shape->opcode != 0x1016 || shape->arguments.size() != 1) { ++forbidden; throw std::runtime_error("forbidden write operation"); }
            const auto code = static_cast<std::uint16_t>(shape->arguments[0]);
            ++write_begins;
            ++begins_by_code[code];
            std::snprintf(text, sizeof text, "1016(%04x)", static_cast<unsigned>(code));
            note(text);
            current_ = code;
            pending_.clear();
            writing_ = true;
            if (begin_throw.count(code)) throw std::runtime_error("transport failure at write begin");
            if (begin_reject.count(code)) return FinalReply(0x2002);
            if (begin_command_failed.count(code)) return CommandFailedReply();
            return WriteBeginReply(L"ctx-w" + std::to_wstring(write_begins));
        }
        case SettingsCommandKind::ReadData: {
            note("read");
            DWORD requested{};
            request->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, &requested);
            const auto count = std::min<std::size_t>(requested, data_.size() - offset_);
            Bytes chunk(data_.begin() + static_cast<std::ptrdiff_t>(offset_), data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
            offset_ += count;
            return ChunkReply(chunk);
        }
        case SettingsCommandKind::WriteData: {
            ++write_datas;
            note("write_data");
            const auto data = InspectSettingsWriteData(request);
            if (data_throw.count(current_)) throw std::runtime_error("transport failure at write data");
            if (data_fail.count(current_)) return CommandFailedReply();
            pending_ = data ? data->bytes : Bytes{};
            return WrittenReply(data_short.count(current_) ? 0 : static_cast<DWORD>(pending_.size()));
        }
        case SettingsCommandKind::EndDataTransfer: {
            note(writing_ ? "write_end" : "end");
            if (!writing_ && zero_) { zero_ = false; return FinalReply(0x200a); }
            if (!writing_) return FinalReply();
            ++write_ends;
            writing_ = false;
            if (end_throw.count(current_)) throw std::runtime_error("transport failure at write end");
            if (end_reject.count(current_)) return FinalReply(end_reject[current_]);
            auto& prop = props.at(current_);
            if (!ignore_write.count(current_) && pending_.size() == D810ValueBytes(static_cast<D810ValueType>(prop.type))) {
                const auto type = static_cast<D810ValueType>(prop.type);
                prop.current = apply_as.count(current_) ? apply_as[current_] : D810DecodeValue(type, pending_, 0);
            }
            return FinalReply();
        }
        default: ++forbidden; throw std::runtime_error("other command reached the body");
        }
    }
    std::size_t Count(const std::string& entry) const { return static_cast<std::size_t>(std::count(trace.begin(), trace.end(), entry)); }
private:
    Bytes data_, pending_;
    std::size_t offset_ = 0;
    std::uint16_t current_ = 0;
    bool writing_ = false, zero_ = false;
};
const std::vector<std::int64_t> kShutters{125, 166, 250, 333, 500, 1000, 1667, 2500, 3333, 5000, 10000, 20000, 0xfffffffdLL, 0xffffffffLL};
FakeWriteBody Body(std::int64_t shutter, std::int64_t aperture, std::int64_t iso) {
    FakeWriteBody body;
    body.props[0x500e] = {4, 1, 1, 2, {1, 2, 3, 4}};
    body.props[0x500d] = {6, 1, shutter, 2, kShutters};
    body.props[0x5007] = {4, 1, aperture, 2, {400, 560, 800, 1100, 1600}};
    body.props[0x500f] = {4, 1, iso, 2, {64, 100, 200, 400}};
    body.props[0xd054] = {2, 1, 0, 2, {0, 1}};
    body.props[0x5005] = {4, 1, 2, 2, {2, 4, 5, 6, 7, 0x8010}};
    body.props[0x500b] = {4, 1, 3, 2, {1, 2, 3, 4}};
    body.props[0x5010] = {3, 1, 0, 1, {-3000, 3000, 500}};
    body.props[0xd200] = {2, 1, 1, 2, {1, 2}};
    body.props[0xd14e] = {2, 1, 0, 2, {0, 1}};
    return body;
}
D810SettingsSnapshot Snapshot(FakeWriteBody& body, const char* alias) {
    WpdSettingsReadSession session([&](IPortableDeviceValues* request) { return body(request); }, [] { return true; }, [] { return true; });
    return session.ReadAll(alias, nullptr).readings;
}
// The same read as the write path makes (the table's codes only, as ReadCamera does).
D810SettingsSnapshot WritePathSnapshot(FakeWriteBody& body, const char* alias) {
    WpdSettingsReadSession session([&](IPortableDeviceValues* request) { return body(request); }, [] { return true; }, [] { return true; });
    return session.ReadAll(alias, nullptr, std::span<const D810SettingSpec>(kD810WriteReadSpecs)).readings;
}

// --- Tables ----------------------------------------------------------------------------
// The product table with CAM-B's five rows enabled and a synthetic firmware
// version: the most ADR-0038 allows. The product table itself enables one row.
constexpr D810WriteTable Adr38Table() {
    auto table = kD810ProductWriteTable;
    for (auto& row : table.rows) {
        if (row.item == D810WriteItem::IsoAuto) row.code = 0xd054;
        if (row.item == D810WriteItem::ShutterSpeed) row.code = 0x500d;
        row.enabled = row.camera == D810Camera::B && D810WriteItemOf(row.item).scope == D810ItemScope::Adr38;
    }
    table.verified_firmware = "1.00";
    return table;
}
constexpr D810WriteTable kAdr38Table = Adr38Table();
static_assert(D810ProductTableProblem(kAdr38Table) == nullptr, "the ADR-0038 test table is a valid product table");
static_assert(D810EnabledRowCount(kAdr38Table) == 5, "five rows");
// The ADR-0038 revision (Proposed): CAM-B's rows for white balance and metering on top of the five. A valid
// table for the contract tests, but not a product table while the revision is not accepted.
constexpr D810WriteTable RevisionTable() {
    auto table = Adr38Table();
    for (auto& row : table.rows)
        if (row.camera == D810Camera::B && D810WriteItemOf(row.item).scope == D810ItemScope::Adr38Revision) row.enabled = true;
    return table;
}
constexpr D810WriteTable kRevisionTable = RevisionTable();
static_assert(D810WriteTableProblem(kRevisionTable) == nullptr && D810EnabledRowCount(kRevisionTable) == 7, "seven rows, a valid table");
static_assert(!kD810Adr38RevisionAccepted && D810ProductTableProblem(kRevisionTable) != nullptr, "not a product table until the owner accepts the revision");
// Everything on, for the ADR-0041 code paths. Not a valid product table.
constexpr D810WriteTable Adr41Table() {
    auto table = Adr38Table();
    for (auto& row : table.rows) row.enabled = true;
    table.chosen_values = true;
    return table;
}
constexpr D810WriteTable kAdr41Table = Adr41Table();
static_assert(D810WriteTableProblem(kAdr41Table) == nullptr && D810ProductTableProblem(kAdr41Table) != nullptr, "ADR-0041 table");
// The ADR-0038 table with only some of CAM-B's rows enabled.
D810WriteTable Adr38Only(std::initializer_list<D810WriteItem> items) {
    auto table = kAdr38Table;
    for (auto& row : table.rows) row.enabled = row.camera == D810Camera::B && std::find(items.begin(), items.end(), row.item) != items.end();
    return table;
}
void WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}
fs::path g_root;
fs::path NewFolder() {
    static int n = 0;
    auto folder = g_root / ("case-" + std::to_string(++n));
    fs::create_directories(folder);
    return folder;
}

// The record in memory, for the runs whose subject is not the file (each file
// append is flushed to the disk, which is slow on a busy machine). The tests
// about the file use D810SettingsFileJournal.
class MemoryJournal final: public D810SettingsJournal {
public:
    bool Append(const std::string& line) override { text_ += line + "\n"; return true; }
    D810TextFile Read() const override {
        D810TextFile file;
        file.kind = text_.empty() ? D810TextFile::Kind::Absent : D810TextFile::Kind::Read;
        file.text = text_;
        return file;
    }
private:
    std::string text_;
};
// A journal whose appends start failing after `ok` successful ones.
class FailingJournal final: public D810SettingsJournal {
public:
    FailingJournal(D810SettingsJournal& inner, int ok): inner_(inner), ok_(ok) {}
    bool Append(const std::string& line) override { return ok_-- > 0 && inner_.Append(line); }
    D810TextFile Read() const override { return inner_.Read(); }
private:
    D810SettingsJournal& inner_;
    int ok_;
};

struct FakeAccess final: D810SettingsCameraAccess {
    FakeWriteBody* bodies[2]{};
    int open = -1;
    bool overlap = false;
    int opens[2]{}, closes[2]{};
    bool close_ok[2]{true, true};
    bool open_fail[2]{};
    std::string model[2]{"D810", "D810"}, firmware[2]{"1.00", "1.00"};
    // The flag a watchdog of camera i would raise (shared by all its opens here).
    std::shared_ptr<std::atomic<bool>> cancel[2];
    // Requests that reached a body after its flag was raised.
    int after_cancel[2]{};
    D810OpenedCamera Open(D810Camera camera) override {
        const auto i = static_cast<int>(camera);
        if (open != -1) overlap = true;
        if (open_fail[i]) return {};
        open = i;
        ++opens[i];
        return {[this, i](IPortableDeviceValues* request) {
                    if (cancel[i] && cancel[i]->load()) ++after_cancel[i];
                    return (*bodies[i])(request);
                },
            model[i], firmware[i], cancel[i]};
    }
    bool Close(D810Camera camera) override {
        const auto i = static_cast<int>(camera);
        ++closes[i];
        open = -1;
        return close_ok[i];
    }
};

// A folder with a file journal, two bodies and a controller over a table.
// Bench(kProduct) builds the controller through the product entry point.
struct ProductEntry {};
inline constexpr ProductEntry kProduct{};
struct Bench {
    fs::path folder = NewFolder();
    D810SettingsFileJournal file{folder};
    MemoryJournal memory;
    D810SettingsJournal* journal{};
    std::unique_ptr<FailingJournal> failing;
    D810CameraOwnership owner;
    D810WriteTable table;
    std::unique_ptr<D810SettingsWriteController> controller;
    FakeWriteBody a = Body(1667, 800, 64), b = Body(333, 560, 100);
    FakeAccess access;
    bool go = true;
    std::chrono::milliseconds left{60000};
    std::vector<std::string> log;
    D810WriteRunOptions options;
    bool product = false;
    explicit Bench(ProductEntry): Bench(kD810ProductWriteTable, -1, false, true) {}
    explicit Bench(const D810WriteTable& t = kAdr38Table, int journal_ok = -1, bool on_disk = false, bool product_entry = false): table(t), product(product_entry) {
        journal = on_disk ? static_cast<D810SettingsJournal*>(&file) : &memory;
        if (journal_ok >= 0) failing = std::make_unique<FailingJournal>(*journal, journal_ok);
        auto& used = failing ? static_cast<D810SettingsJournal&>(*failing) : *journal;
        controller = product ? std::make_unique<D810SettingsWriteController>(used, owner) : std::make_unique<D810SettingsWriteController>(table, used, owner);
        access.bodies[0] = &a;
        access.bodies[1] = &b;
        options.may_continue = [this] { return go; };
        options.may_cleanup = [] { return true; };
        options.remaining = [this] { return left; };
        options.log = [this](const std::string& line) { log.push_back(line); };
        options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
    }
    // A fresh controller over the same folder: what a restart sees.
    D810SettingsWriteController Restarted() {
        return product ? D810SettingsWriteController(*journal, owner) : D810SettingsWriteController(table, *journal, owner);
    }
    D810PlanResult Copy(const std::optional<std::vector<D810WriteItem>>& only = std::nullopt) {
        return controller->PlanCopy(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), only);
    }
    D810PlanResult Chosen(const std::vector<D810WriteChoice>& choices) { return controller->PlanChosen(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), choices); }
    D810WriteRun Run(const D810PlanResult& plan) {
        if (!plan.plan) return {};
        check(controller->Confirm(plan.plan->id) == nullptr, "confirm");
        return controller->Execute(plan.plan->id, access, options);
    }
    D810WriteRun CopyRun() { return Run(Copy()); }
    int Writes() const { return a.write_begins + b.write_begins; }
    std::string Journal() const { return journal->Read().text; }
};
bool NoResend(const FakeWriteBody& body) {
    return std::all_of(body.begins_by_code.begin(), body.begins_by_code.end(), [](const auto& entry) { return entry.second <= 1; });
}

// --- Values and the item table ------------------------------------------------------------
void ValuesAndTable() {
    check(EncodeD810Value(D810ValueType::Uint16, 0x0320) == Bytes{0x20, 0x03}, "u16 encoding");
    check(EncodeD810Value(D810ValueType::Int16, -500) == Bytes{0x0c, 0xfe}, "i16 encoding");
    check(EncodeD810Value(D810ValueType::Uint32, 1667) == Bytes{0x83, 0x06, 0, 0}, "u32 encoding");
    check(EncodeD810Value(D810ValueType::Uint8, 0) == Bytes{0}, "u8 encoding");
    check(!D810ValueFits(D810ValueType::Uint16, 0x10000) && !D810ValueFits(D810ValueType::Uint8, -1) && D810ValueFits(D810ValueType::Int16, -32768), "fits");
    D810PropDesc desc;
    desc.data_type = 4;
    desc.get_set = 1;
    desc.form_flag = 2;
    desc.values = {400, 800};
    check(D810ValueAdvertised(desc, 800) && !D810ValueAdvertised(desc, 560), "enum");
    desc.form_flag = 1;
    desc.range_min = -3000; desc.range_max = 3000; desc.range_step = 500;
    check(D810ValueAdvertised(desc, -2500) && !D810ValueAdvertised(desc, -2400) && !D810ValueAdvertised(desc, 3500), "range");
    desc.range_step = 0;
    check(!D810ValueAdvertised(desc, -3000) && !desc.Writable(), "range step 0 offers nothing");
    desc.form_flag = 0;
    check(!D810ValueAdvertised(desc, 0), "no form offers nothing");
    for (const auto value : {0xffffffffLL, 0xfffffffdLL, 0LL, 10001LL, 20000LL})
        check(D810ShutterRefusal(value) != nullptr, "shutter refusal");
    check(D810ShutterRefusal(10000) == nullptr && D810ShutterRefusal(125) == nullptr, "short shutter allowed");
    D810PropDesc shutter;
    shutter.data_type = 6; shutter.get_set = 1; shutter.form_flag = 2; shutter.values = kShutters;
    const auto& shutter_spec = D810WriteItemOf(D810WriteItem::ShutterSpeed);
    check(Refusal(D810WriteValueRefusal(shutter_spec, shutter, 0xffffffffLL)) == "bulb_or_time", "bulb refused although advertised");
    check(Refusal(D810WriteValueRefusal(shutter_spec, shutter, 0xfffffffdLL)) == "bulb_or_time", "time refused although advertised");
    check(Refusal(D810WriteValueRefusal(shutter_spec, shutter, 20000)) == "exposure_too_long", "long exposure refused although advertised");
    check(D810WriteValueRefusal(shutter_spec, shutter, 1667) == nullptr, "advertised shutter");
    shutter.get_set = 0;
    check(Refusal(D810WriteValueRefusal(shutter_spec, shutter, 1667)) == "not_writable", "getset 0");
    D810PropDesc mode;
    mode.data_type = 4; mode.get_set = 1; mode.form_flag = 2; mode.values = {1, 2, 3, 4};
    check(Refusal(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::ExposureMode), mode, 2)) == "value_not_fixed", "exposure mode is M only");
    D810PropDesc iso_auto;
    iso_auto.data_type = 2; iso_auto.get_set = 1; iso_auto.form_flag = 2; iso_auto.values = {0, 1};
    check(Refusal(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::IsoAuto), iso_auto, 1)) == "value_not_fixed", "ISO auto is OFF only");
    iso_auto.data_type = 4;
    check(Refusal(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::IsoAuto), iso_auto, 0)) == "type_mismatch", "type");
    // The table: exactly these codes, nothing else in 0..0xFFFF.
    const std::set<std::uint32_t> expected{0x500e, 0xd054, 0xd16a, 0x500f, 0x500d, 0xd100, 0x5007, 0x5005, 0x500b, 0x5010};
    std::size_t found = 0;
    for (std::uint32_t code = 0; code <= 0xffff; ++code)
        if (FindD810WriteItemByCode(code)) { ++found; check(expected.count(code) == 1, "code in table"); }
    check(found == expected.size(), "table size");
    for (const auto code : {0xd200u, 0xd14eu, 0x5004u, 0x5003u, 0x500au, 0x5008u, 0x501cu, 0xd1a2u, 0xd10bu, 0x9207u, 0x90c2u})
        check(!FindD810WriteItemByCode(code), "not writable: picture control, ADL, quality, size, focus, media, live, vendor ops");
    for (const auto& spec : kD810WriteItems) {
        check(D810WriteItemOf(spec.item).item == spec.item, "table order is the enum order");
        for (std::size_t i = 0; i < spec.code_count; ++i) {
            const auto* read = FindD810Setting(spec.codes[i]);
            check(read && read->type == spec.type, "every write code is a read code of the same type");
        }
    }
    check(D810WriteItemOf(D810WriteItem::ExposureMode).fixed && D810WriteItemOf(D810WriteItem::IsoAuto).fixed, "fixed items");
}

// The write path reads the table's ten codes and not the two candidate codes (0xD16A, 0xD100). The bodies answer
// those two. A real body answered 0xD16A with a failed begin and an END refused with 0x200A (the size of 0 is an
// inference), which stops a read session; 0xD100 has not been read on a body (unconfirmed). The fake body below answers
// both like that. The plan's read, the check before a send and the reread must complete anyway, and never ask for them.
void WritePathReadSet() {
    static_assert(!D810WriteReadSetHas(0xd16a) && !D810WriteReadSetHas(0xd100) && D810WriteReadSetHas(0xd054) && D810WriteReadSetHas(0x500d), "the read set");
    check(D810WriteReadSetCovers(kD810ProductWriteTable) && D810WriteReadSetCovers(kAdr38Table) && D810WriteReadSetCovers(kRevisionTable) && D810WriteReadSetCovers(kAdr41Table),
        "every code a table confirms is in the read set");
    auto bad = kAdr38Table;
    for (auto& row : bad.rows) if (row.item == D810WriteItem::IsoAuto) row.code = 0xd16a;
    check(!D810WriteReadSetCovers(bad), "a table that confirms a code outside the read set is seen");
    const auto unsupported = [](Bench& bench) {
        for (auto* body : {&bench.a, &bench.b}) {
            body->props[0xd16a] = {2, 1, 0, 2, {0, 1}};
            body->props[0xd100] = {6, 1, 125, 2, kShutters};
            body->zero_at[0xd16a] = 1;
            body->zero_at[0xd100] = 1;
        }
    };
    const auto asked = [](const Bench& bench) {
        return bench.a.Count("1014(d16a)") + bench.a.Count("1014(d100)") + bench.b.Count("1014(d16a)") + bench.b.Count("1014(d100)");
    };
    Bench bench;
    unsupported(bench);
    const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
    check(read.complete && read.reason.empty(), "plan read: complete although 0xD16A and 0xD100 answer as unsupported");
    check(bench.a.Count("1014(500e)") == 1 && bench.a.Count("1014(d054)") == 1 && bench.a.Count("1014(500d)") == 1 && bench.b.Count("1014(5007)") == 1, "plan read: the table's codes are read");
    check(asked(bench) == 0, "plan read: 0xD16A and 0xD100 are not asked for");
    const auto plan = bench.controller->PlanCopy(read.snapshots[0], read.snapshots[1]);
    check(plan.Ok() && plan.plan->steps.size() == 3, "plan read: a plan is made from it");
    if (!plan.Ok()) return;
    check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
    const auto run = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
    check(run.verdict == D810WriteVerdict::Match && run.sent == 3 && run.reason.empty() && bench.Writes() == 3, "check before the send and the read back: complete, matched");
    check(asked(bench) == 0 && bench.a.forbidden + bench.b.forbidden == 0, "run: 0xD16A and 0xD100 are not asked for");
    const auto reread = bench.controller->Reread(bench.access, bench.options);
    check(reread.complete && reread.reason.empty(), "reread: complete");
    check(asked(bench) == 0, "reread: 0xD16A and 0xD100 are not asked for");
}

// FormFlag None, Range step 0 and the excluded white balance values are never written.
void DescriptionRules() {
    D810PropDesc none;
    none.data_type = 4; none.get_set = 1; none.form_flag = 0; none.current_value = 100;
    check(none.Writable() && !D810DescWritable(none), "the stage A Writable() accepts a form of None; the write rule does not");
    check(Refusal(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::Iso), none, 100)) == "not_writable", "FormFlag None refused");
    D810PropDesc range;
    range.data_type = 3; range.get_set = 1; range.form_flag = 1; range.range_min = -3000; range.range_max = 3000; range.range_step = 0;
    check(!D810DescWritable(range), "step 0");
    range.range_step = -500;
    check(!D810DescWritable(range), "negative step");
    range.range_step = 500;
    const auto& compensation = D810WriteItemOf(D810WriteItem::ExposureCompensation);
    check(D810WriteValueRefusal(compensation, range, -2500) == nullptr && D810WriteValueRefusal(compensation, range, -3000) == nullptr, "signed range compares signed");
    check(D810WriteValueRefusal(compensation, range, -3500) != nullptr && D810WriteValueRefusal(compensation, range, 3500) != nullptr, "outside the signed range");
    check(EncodeD810Value(D810ValueType::Int16, -3000) == Bytes{0x48, 0xf4} && EncodeD810Value(D810ValueType::Int16, -1) == Bytes{0xff, 0xff}, "INT16 two's complement");
    // Parsed from the wire: an INT16 description keeps its negative values.
    const auto parsed = ParseD810PropDesc(Desc(0x5010, 3, 1, -500, 1, {-3000, 3000, 500}), *FindD810Setting(0x5010));
    check(parsed.Ok() && parsed.desc.current_value == -500 && parsed.desc.range_min == -3000, "INT16 parsed signed");
    D810PropDesc wb;
    wb.data_type = 4; wb.get_set = 1; wb.form_flag = 2; wb.values = {1, 2, 3, 4, 5, 6, 7, 0x8010, 0x8011, 0x8012, 0x8013, 0x8014, 0x8016};
    const auto& white_balance = D810WriteItemOf(D810WriteItem::WhiteBalance);
    for (const auto value : {1LL, 3LL, 0x8012LL, 0x8013LL})
        check(Refusal(D810WriteValueRefusal(white_balance, wb, value)) == "value_content_dependent", "content-dependent white balance");
    // The kinds whose meaning is named in the public table; the others the body may offer are not written.
    for (const auto value : {2LL, 4LL, 5LL, 6LL, 7LL, 0x8010LL, 0x8011LL})
        check(D810WriteValueRefusal(white_balance, wb, value) == nullptr, "named white balance kind");
    for (const auto value : {0x8014LL, 0x8016LL})
        check(Refusal(D810WriteValueRefusal(white_balance, wb, value)) == "value_not_listed", "advertised but not named");
    wb.values = {2, 4};
    check(Refusal(D810WriteValueRefusal(white_balance, wb, 5)) == "value_not_advertised", "a named kind the body does not offer");
    wb.data_type = 6;
    check(Refusal(D810WriteValueRefusal(white_balance, wb, 4)) == "type_mismatch", "white balance type");
    D810PropDesc metering;
    metering.data_type = 4; metering.get_set = 1; metering.form_flag = 2; metering.values = {1, 2, 3, 4, 5, 0x8001};
    const auto& metering_spec = D810WriteItemOf(D810WriteItem::Metering);
    for (const auto value : {1LL, 2LL, 3LL, 4LL})
        check(D810WriteValueRefusal(metering_spec, metering, value) == nullptr, "named metering mode");
    for (const auto value : {5LL, 0x8001LL})
        check(Refusal(D810WriteValueRefusal(metering_spec, metering, value)) == "value_not_listed", "metering mode not named");
    metering.get_set = 0;
    check(Refusal(D810WriteValueRefusal(metering_spec, metering, 2)) == "not_writable", "metering GetSet 0");
    // The copy asks about CAM-A's value before CAM-B: a value no copy can carry.
    check(Refusal(D810CopyValueRefusal(white_balance, 0x8013)) == "value_content_dependent" && Refusal(D810CopyValueRefusal(white_balance, 0x9999)) == "value_not_listed" &&
        D810CopyValueRefusal(white_balance, 4) == nullptr && D810CopyValueRefusal(D810WriteItemOf(D810WriteItem::Iso), 12345) == nullptr, "copy value rules");
    {
        // The closed lists, pinned over every 16-bit value: adding a value to either list (or to the excluded kinds) fails here.
        std::set<std::int64_t> white_passes, white_content, metering_passes;
        std::size_t other_refused = 0;
        for (std::int64_t value = 0; value <= 0xffff; ++value) {
            if (D810CopyValueRefusal(white_balance, value) == nullptr) white_passes.insert(value);
            else if (Refusal(D810CopyValueRefusal(white_balance, value)) == "value_content_dependent") white_content.insert(value);
            if (D810CopyValueRefusal(metering_spec, value) == nullptr) metering_passes.insert(value);
            for (const auto item : {D810WriteItem::ExposureMode, D810WriteItem::IsoAuto, D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture})
                if (D810CopyValueRefusal(D810WriteItemOf(item), value) != nullptr) ++other_refused;
        }
        check(white_passes == std::set<std::int64_t>{2, 4, 5, 6, 7, 0x8010, 0x8011}, "white balance: the copy passes exactly the named kinds");
        check(white_content == std::set<std::int64_t>{1, 3, 0x8012, 0x8013}, "white balance: the content-dependent kinds are exactly these");
        check(metering_passes == std::set<std::int64_t>{1, 2, 3, 4}, "metering: the copy passes exactly the listed modes");
        check(other_refused == 0, "the five exposure items have no value rule");
    }
}

// --- The fixed table ----------------------------------------------------------------------------
void ProductTable() {
    const auto& table = kD810ProductWriteTable;
    check(D810ProductTableProblem(table) == nullptr, "product table valid");
    // The enabled set is exactly this: ISO on CAM-B. Changing the product table makes this check fail on purpose.
    std::set<std::pair<int, int>> enabled;
    for (const auto& row : table.rows)
        if (row.enabled) enabled.insert({static_cast<int>(row.camera), static_cast<int>(row.item)});
    check(enabled == std::set<std::pair<int, int>>{{static_cast<int>(D810Camera::B), static_cast<int>(D810WriteItem::Iso)}}, "the product table enables exactly ISO on CAM-B");
    check(D810EnabledRowCount(table) == 1, "one enabled row");
    // The codes the table confirms: ISO auto and shutter speed on both cameras, rows disabled.
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        const auto* iso_auto = table.Row(D810WriteItem::IsoAuto, camera);
        const auto* shutter = table.Row(D810WriteItem::ShutterSpeed, camera);
        check(iso_auto && !iso_auto->enabled && iso_auto->code == 0xd054, "ISO auto: code 0xD054, disabled");
        check(shutter && !shutter->enabled && shutter->code == 0x500d, "shutter speed: code 0x500D, disabled");
    }
    check(table.Code(D810WriteItem::IsoAuto) == 0xd054 && table.Code(D810WriteItem::ShutterSpeed) == 0x500d, "confirmed codes");
    check(table.copy_a_to_b && !table.chosen_values, "copy only");
    check(std::string(table.verified_model) == "D810" && table.verified_firmware != nullptr && table.verified_firmware[0] != '\0', "model and firmware named");
    check(D810BodyVerified(table, "D810", table.verified_firmware), "a body that reports the named model and firmware is verified");
    check(!D810BodyVerified(table, "D810", "") && !D810BodyVerified(table, "D810", "1.00") && !D810BodyVerified(table, "D850", table.verified_firmware) &&
        !D810BodyVerified(table, "", table.verified_firmware), "any other model or firmware is not");
    // What the product table may ever hold: CAM-B and the five items, no chosen values.
    std::size_t allowed = 0;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        auto probe = Adr38Table();
        probe.rows[i].enabled = true;
        const bool ok = D810ProductTableProblem(probe) == nullptr;
        allowed += ok;
        check(ok == (probe.rows[i].camera == D810Camera::B && D810ItemInProductScope(D810WriteItemOf(probe.rows[i].item))), "allowed rows");
    }
    check(allowed == 5, "CAM-B x five");
    // White balance and metering (ADR-0038 revision, Proposed): the rows are in the table, disabled, with their codes; the
    // product table cannot enable one until the owner accepts the revision and a reviewed commit sets the constant.
    check(!kD810Adr38RevisionAccepted, "the ADR-0038 revision is not accepted");
    for (const auto item : {D810WriteItem::WhiteBalance, D810WriteItem::Metering}) {
        for (const auto camera : {D810Camera::A, D810Camera::B}) {
            const auto* row = table.Row(item, camera);
            check(row && !row->enabled && row->code == (item == D810WriteItem::WhiteBalance ? 0x5005 : 0x500b), "revision rows: present, disabled, code from the public table");
        }
        auto probe = Adr38Table();
        for (auto& row : probe.rows) if (row.camera == D810Camera::B && row.item == item) row.enabled = true;
        check(std::string(D810ProductTableProblem(probe)) == "extra_item_enabled", "revision row refused in the product table");
        check(D810WriteTableProblem(probe) == nullptr, "revision row valid in a test table");
    }
    check(!D810ItemInProductScope(D810WriteItemOf(D810WriteItem::ExposureCompensation)) && D810WriteItemOf(D810WriteItem::ExposureCompensation).scope == D810ItemScope::Adr41, "compensation stays ADR-0041");
    auto chosen = Adr38Table();
    chosen.chosen_values = true;
    check(std::string(D810ProductTableProblem(chosen)) == "chosen_values_before_adr0041", "chosen values not allowed");
    // Consistency rules.
    auto bad = Adr38Table();
    bad.rows[1].code = 0xd055;
    check(std::string(D810WriteTableProblem(bad)) == "code_not_a_candidate", "candidate");
    bad = Adr38Table();
    for (auto& row : bad.rows) if (row.item == D810WriteItem::ShutterSpeed) row.code = 0xd100;
    check(std::string(D810WriteTableProblem(bad)) == "nikon_shutter_encoding_unsourced", "0xD100 needs a source first");
    bad = Adr38Table();
    for (auto& row : bad.rows) if (row.item == D810WriteItem::IsoAuto && row.camera == D810Camera::A) row.code = 0xd16a;
    check(std::string(D810WriteTableProblem(bad)) == "codes_disagree", "one code per item");
    bad = Adr38Table();
    for (auto& row : bad.rows) if (row.item == D810WriteItem::IsoAuto) row.code = 0;
    check(std::string(D810WriteTableProblem(bad)) == "enabled_without_code", "enabled needs a code");
    bad = D810DisabledWriteTable();
    for (auto& row : bad.rows) if (row.item == D810WriteItem::Iso && row.camera == D810Camera::B) row.enabled = true;
    check(std::string(D810WriteTableProblem(bad)) == "iso_auto_code_unconfirmed", "ISO needs the ISO auto code");
    bad = kD810ProductWriteTable;
    for (auto& row : bad.rows) if (row.item == D810WriteItem::IsoAuto) row.code = 0;
    check(std::string(D810WriteTableProblem(bad)) == "iso_auto_code_unconfirmed", "the product table's ISO row needs the ISO auto code");
    bad = Adr38Table();
    bad.rows[3] = bad.rows[2];
    check(D810WriteTableProblem(bad) != nullptr, "duplicate or missing row");
}
// The controller's table. Built outside this target (no A0_SETTINGS_WRITE_TEST_TABLES)
// it has no constructor that takes a table (static_assert in the header) and checks its
// table against the product rules; that check is D810ControllerTableProblem(table, false).
void ControllerTables() {
    static_assert(kD810ControllerTestTables, "the contract tests build the test-table constructor");
    check(D810ControllerTableProblem(kD810ProductWriteTable, false) == nullptr && D810ControllerTableProblem(kAdr38Table, false) == nullptr, "product rules: product-shaped tables pass");
    check(std::string(D810ControllerTableProblem(kAdr41Table, false)) == "chosen_values_before_adr0041", "product rules: chosen values refused");
    auto cam_a = kAdr38Table;
    for (auto& row : cam_a.rows) if (row.camera == D810Camera::A && row.item == D810WriteItem::Iso) row.enabled = true;
    check(std::string(D810ControllerTableProblem(cam_a, false)) == "cam_a_row_enabled", "product rules: a CAM-A row refused");
    auto extra = kAdr38Table;
    for (auto& row : extra.rows) if (row.camera == D810Camera::B && row.item == D810WriteItem::WhiteBalance) row.enabled = true;
    check(std::string(D810ControllerTableProblem(extra, false)) == "extra_item_enabled", "product rules: an extra item refused");
    check(D810ControllerTableProblem(kAdr41Table, true) == nullptr, "test tables: the ADR-0041 paths can be built");
    auto broken = kAdr38Table;
    broken.rows[0].code = 0x1234;
    check(D810ControllerTableProblem(broken, true) != nullptr && D810ControllerTableProblem(broken, false) != nullptr, "an inconsistent table is refused either way");
    {
        // The product entry point holds the product table.
        Bench bench(kProduct);
        check(bench.controller->Status().enabled_rows == 1, "product entry: one row");
        check(bench.controller->Status().write_allowed && bench.controller->Status().capture_allowed, "one row enabled, no block: copying is offered");
        auto none = kAdr38Table;
        for (auto& row : none.rows) row.enabled = false;
        Bench empty(none);
        check(empty.controller->Status().enabled_rows == 0 && !empty.controller->Status().write_allowed && empty.controller->Status().capture_allowed, "no row enabled: copying is not offered");
        const auto copy = bench.Copy();
        check(copy.Ok() && copy.plan->steps.size() == 1 && copy.plan->steps[0].item == D810WriteItem::Iso && copy.plan->steps[0].camera == D810Camera::B && bench.Writes() == 0,
            "product entry: only ISO on CAM-B can be planned");
        check(bench.Restarted().Status().enabled_rows == 1, "product entry after a restart");
    }
    {
        // The table is held by value: a temporary does not dangle.
        MemoryJournal journal;
        D810CameraOwnership owner;
        D810SettingsWriteController controller(Adr38Table(), journal, owner);
        auto overwrite = Adr41Table();
        check(controller.Status().enabled_rows == 5 && D810EnabledRowCount(overwrite) == 16, "the temporary table was copied");
    }
}
// Nothing at run time can widen the table: files that look like approvals, in
// the record folder or next to it, change nothing.
void FilesEnableNothing() {
    Bench bench(kProduct);
    for (const auto* name : {"capability.txt", "approval.txt", "table.txt", "enable.txt", "settings-write.ini"}) {
        WriteFile(bench.folder / name, "a0-d810-settings-capability 1\nitem=iso alias=CAM-B code=0x500f type=0x0004 getset=1\n"
            "feature=cam_a_write adr=ADR-0041 issue=416\nfeature=chosen_values adr=ADR-0041 issue=416\nenabled=all\nend count=1\n");
        WriteFile(bench.folder.parent_path() / name, "enabled=all\n");
    }
    SetEnvironmentVariableW(L"A0_SETTINGS_WRITE_ENABLE", L"1");
    const auto copy = bench.Copy();
    check(copy.Ok() && copy.plan->steps.size() == 1 && copy.plan->steps[0].item == D810WriteItem::Iso && copy.plan->steps[0].camera == D810Camera::B,
        "product copy: ISO on CAM-B and nothing else, whatever the files and the variable say");
    for (const auto target : {D810WriteTarget::A, D810WriteTarget::B, D810WriteTarget::Both})
        check(!bench.Chosen({{D810WriteItem::Iso, target, 200}}).Ok(), "product chosen refused");
    check(bench.controller->Status().enabled_rows == 1, "status: one row");
    SetEnvironmentVariableW(L"A0_SETTINGS_WRITE_ENABLE", nullptr);
    check(bench.Writes() == 0 && bench.a.write_datas + bench.b.write_datas == 0 && bench.a.forbidden + bench.b.forbidden == 0, "only reads, no write");
}

// --- Plans -----------------------------------------------------------------------------------
void CopyPlans() {
    {
        Bench bench;
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.size() == 3, "three differences");
        if (plan.Ok()) {
            const auto& steps = plan.plan->steps;
            check(steps[0].item == D810WriteItem::Iso && steps[1].item == D810WriteItem::ShutterSpeed && steps[2].item == D810WriteItem::Aperture, "write order");
            check(steps[0].value == 64 && steps[1].value == 1667 && steps[2].value == 800, "A's values");
            check(steps[0].before == 100 && steps[1].before == 333 && steps[2].before == 560, "B's values before");
            check(std::all_of(steps.begin(), steps.end(), [](const auto& s) { return s.camera == D810Camera::B; }), "B only");
        }
        check(bench.Writes() == 0, "planning sends nothing");
    }
    const auto refused = [](Bench& bench, const char* reason) {
        const auto plan = bench.Copy();
        check(!plan.Ok() && bench.Writes() == 0, reason);
        check(std::any_of(plan.reasons.begin(), plan.reasons.end(), [&](const auto& r) { return r.rfind(reason, 0) == 0; }), reason);
    };
    { Bench bench; bench.a.props[0x500e].current = 3; refused(bench, "a_not_manual"); }
    { Bench bench; bench.a.props[0xd054].current = 1; refused(bench, "a_iso_auto_not_off"); }
    { Bench bench; bench.a.props[0x500d].current = 0xffffffffLL; refused(bench, "a_shutter_bulb_or_time"); }
    { Bench bench; bench.a.props[0x500d].current = 0xfffffffdLL; refused(bench, "a_shutter_bulb_or_time"); }
    { Bench bench; bench.a.props[0x500d].current = 20000; refused(bench, "a_shutter_exposure_too_long"); }
    { Bench bench; bench.b.props[0x5007].values = {400, 560, 1100}; refused(bench, "value_not_advertised:aperture:CAM-B"); }
    { Bench bench; bench.b.props[0x5007].form = 0; bench.b.props[0x5007].values.clear(); refused(bench, "not_writable:aperture:CAM-B"); }
    { Bench bench; bench.b.props[0x5007].get_set = 0; refused(bench, "not_writable:aperture:CAM-B"); }
    { Bench bench; bench.b.props[0x5007].type = 6; refused(bench, "unreadable:aperture:CAM-B"); }
    {
        // The product table: ISO on CAM-B is planned; every other difference is left out because its row is disabled.
        Bench bench(kProduct);
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.size() == 1 && plan.plan->steps[0].item == D810WriteItem::Iso && plan.plan->steps[0].value == 64 && plan.plan->steps[0].before == 100, "product table: one step, ISO");
        const auto skipped = [&](D810WriteItem item) {
            return plan.Ok() && std::any_of(plan.plan->skipped.begin(), plan.plan->skipped.end(), [&](const auto& s) { return s.first == item && s.second == "row_disabled"; });
        };
        check(skipped(D810WriteItem::ShutterSpeed) && skipped(D810WriteItem::Aperture), "product table: shutter speed and aperture are left out (row disabled)");
        check(bench.Writes() == 0, "product table: planning sends nothing");
    }
    {
        // A row disabled: listed as skipped, the rest still planned.
        Bench bench(Adr38Only({D810WriteItem::ExposureMode, D810WriteItem::IsoAuto, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}));
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.size() == 2 && plan.plan->skipped.size() == 1 && plan.plan->skipped[0].first == D810WriteItem::Iso, "skipped iso");
    }
    {
        // One item only, chosen by leaving the others out; the value stays A's.
        Bench bench;
        const auto plan = bench.Copy(std::vector<D810WriteItem>{D810WriteItem::Iso});
        check(plan.Ok() && plan.plan->steps.size() == 1 && plan.plan->steps[0].item == D810WriteItem::Iso && plan.plan->steps[0].value == 64, "one item");
        check(plan.Ok() && plan.plan->skipped.size() == 2 && plan.plan->skipped[0].second == "left_out_by_operator", "the others left out");
    }
    {
        // B is in A mode and its mode row is disabled: stop and ask for M on the body.
        Bench bench(Adr38Only({D810WriteItem::IsoAuto, D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}));
        bench.b.props[0x500e].current = 3;
        refused(bench, "exposure_mode_not_manual_set_on_body:CAM-B");
    }
    {
        Bench bench(Adr38Only({D810WriteItem::ExposureMode, D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}));
        bench.b.props[0xd054].current = 1;
        refused(bench, "iso_auto_not_off:CAM-B");
    }
    {
        Bench bench;
        bench.b = Body(1667, 800, 64);
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.empty(), "no difference, nothing to write");
        check(std::string(bench.controller->Confirm(plan.plan->id)) == "nothing_to_write", "empty plan cannot be confirmed");
    }
}
// --- White balance and metering (ADR-0038 revision, Proposed): the tables here are test tables only ------------------
// The step items of a plan, in order.
std::vector<D810WriteItem> StepItems(const D810PlanResult& plan) {
    std::vector<D810WriteItem> items;
    if (plan.plan) for (const auto& step : plan.plan->steps) items.push_back(step.item);
    return items;
}
const std::pair<D810WriteItem, std::string>* SkippedOf(const D810PlanResult& plan, D810WriteItem item) {
    if (!plan.plan) return nullptr;
    for (const auto& entry : plan.plan->skipped)
        if (entry.first == item) return &entry;
    return nullptr;
}
void RevisionCopyPlans() {
    using Item = D810WriteItem;
    {
        // Both differ: they join the plan after the exposure items, in the table's order, with A's values.
        Bench bench(kRevisionTable);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        const auto plan = bench.Copy();
        check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::Iso, Item::ShutterSpeed, Item::Aperture, Item::WhiteBalance, Item::Metering}, "revision: five steps in the table's order");
        if (plan.Ok()) {
            const auto& steps = plan.plan->steps;
            check(steps[3].code == 0x5005 && steps[3].before == 2 && steps[3].value == 4 && steps[4].code == 0x500b && steps[4].before == 3 && steps[4].value == 2, "revision: A's values, B's values before");
            check(std::all_of(steps.begin(), steps.end(), [](const auto& s) { return s.camera == D810Camera::B; }), "revision: B only");
            check(plan.plan->skipped.empty(), "revision: nothing left out");
            const auto seen = [&](D810Camera camera, std::uint16_t code, std::int64_t value) {
                return std::any_of(plan.plan->observed.begin(), plan.plan->observed.end(), [&](const auto& o) { return o.camera == camera && o.code == code && o.value == value; });
            };
            check(seen(D810Camera::A, 0x5005, 4) && seen(D810Camera::B, 0x5005, 2) && seen(D810Camera::A, 0x500b, 2) && seen(D810Camera::B, 0x500b, 3), "revision: both bodies' values are observed");
        }
        check(bench.Writes() == 0, "revision: planning sends nothing");
    }
    {
        // Seven steps: B in A mode with ISO auto on, every item differs.
        Bench bench(kRevisionTable);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        bench.b.props[0x500e].current = 3;
        bench.b.props[0xd054].current = 1;
        const auto plan = bench.Copy();
        check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::ExposureMode, Item::IsoAuto, Item::Iso, Item::ShutterSpeed, Item::Aperture, Item::WhiteBalance, Item::Metering},
            "revision: seven steps, the mode and ISO auto first");
    }
    {
        // Only the two differ.
        Bench bench(kRevisionTable);
        bench.b = Body(1667, 800, 64);
        bench.a.props[0x5005].current = 6;
        const auto plan = bench.Copy();
        check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::WhiteBalance}, "revision: white balance alone");
        bench.a.props[0x500b].current = 4;
        check(bench.Copy().Ok() && StepItems(bench.Copy()) == std::vector<Item>{Item::WhiteBalance, Item::Metering}, "revision: both alone");
        bench.a.props[0x5005].current = 2;
        check(StepItems(bench.Copy()) == std::vector<Item>{Item::Metering}, "revision: metering alone");
    }
    {
        // No difference: nothing for them, and nothing is left out.
        Bench bench(kRevisionTable);
        const auto plan = bench.Copy();
        check(plan.Ok() && StepItems(plan).size() == 3 && plan.plan->skipped.empty(), "revision: no difference, no step, nothing left out");
    }
    {
        // The product-shaped table (their rows disabled): listed as left out with the table's reason, the five still planned.
        Bench bench(kAdr38Table);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        const auto plan = bench.Copy();
        check(plan.Ok() && StepItems(plan).size() == 3, "disabled rows: the five are planned");
        const auto* wb = SkippedOf(plan, Item::WhiteBalance);
        const auto* metering = SkippedOf(plan, Item::Metering);
        check(wb && wb->second == "row_disabled" && metering && metering->second == "row_disabled", "disabled rows: left out with the reason");
        check(bench.Writes() == 0 && std::none_of(plan.plan->steps.begin(), plan.plan->steps.end(), [](const auto& s) { return s.item == Item::WhiteBalance || s.item == Item::Metering; }), "disabled rows: no step");
    }
    {
        // A disabled row is told by the short table reason even when A holds a kind a copy cannot carry; the
        // value-based reason belongs to enabled rows only (the table's own rule is unchanged: see below).
        for (const std::int64_t value : {1LL, 3LL, 0x8012LL, 0x8013LL, 0x9999LL}) {
            Bench bench(kAdr38Table);
            bench.a.props[0x5005].current = value;
            bench.a.props[0x500b].current = 2;
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, Item::WhiteBalance);
            check(plan.Ok() && wb && wb->second == "row_disabled", "disabled row, A on a kind a copy cannot carry: the table reason");
            check(plan.Ok() && StepItems(plan).size() == 3, "disabled row, A on such a kind: the five are still planned");
        }
        {
            // Preset on both bodies and a disabled row: equal numbers, so nothing is listed (as for the five).
            Bench bench(kAdr38Table);
            bench.b = Body(1667, 800, 64);
            bench.a.props[0x5005].current = 0x8013;
            bench.b.props[0x5005].current = 0x8013;
            const auto plan = bench.Copy();
            check(plan.Ok() && SkippedOf(plan, Item::WhiteBalance) == nullptr, "disabled row, equal numbers: not listed");
        }
        {
            // The enabled-row table keeps the value-based reason for the same input.
            Bench bench(kRevisionTable);
            bench.a.props[0x5005].current = 0x8013;
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, Item::WhiteBalance);
            check(plan.Ok() && wb && wb->second == "value_content_dependent", "enabled row, same input: the value-based reason");
        }
    }
    {
        // A value a copy cannot carry is left out whatever B reads; the rest of the plan stands and B's value is untouched.
        for (const std::int64_t value : {1LL, 3LL, 0x8012LL, 0x8013LL}) {
            Bench bench(kRevisionTable);
            bench.a.props[0x5005].current = value;
            bench.a.props[0x500b].current = 2;
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, Item::WhiteBalance);
            check(plan.Ok() && wb && wb->second == "value_content_dependent", "content-dependent white balance is left out");
            check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::Iso, Item::ShutterSpeed, Item::Aperture, Item::Metering}, "the other items are still planned");
            const auto run = bench.Run(plan);
            check(run.verdict == D810WriteVerdict::Match && run.sent == 4 && bench.b.begins_by_code.count(0x5005) == 0 && bench.b.props[0x5005].current == 2, "B's white balance is not written");
            check(bench.controller->CaptureAllowed(), "a left-out white balance holds nothing");
        }
        {
            // Both on Preset: equal numbers, but the white is not compared, so it is listed.
            Bench bench(kRevisionTable);
            bench.b = Body(1667, 800, 64);
            bench.a.props[0x5005].current = 0x8013;
            bench.b.props[0x5005].current = 0x8013;
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, Item::WhiteBalance);
            check(plan.Ok() && plan.plan->steps.empty() && wb && wb->second == "value_content_dependent", "Preset on both: no step, still listed");
        }
        {
            // B on Preset, A on a named kind: the kind is written over it.
            Bench bench(kRevisionTable);
            bench.b = Body(1667, 800, 64);
            bench.a.props[0x5005].current = 4;
            bench.b.props[0x5005].current = 0x8013;
            const auto plan = bench.Copy();
            check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::WhiteBalance} && plan.plan->steps[0].before == 0x8013 && plan.plan->steps[0].value == 4, "B on Preset gets A's named kind");
        }
        {
            // A value the item does not name (a vendor value of another body, a metering mode of another maker).
            Bench bench(kRevisionTable);
            bench.a.props[0x5005].current = 0x8014;
            bench.a.props[0x500b].current = 0x8001;
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, Item::WhiteBalance);
            const auto* metering = SkippedOf(plan, Item::Metering);
            check(plan.Ok() && wb && wb->second == "value_not_listed" && metering && metering->second == "value_not_listed" && StepItems(plan).size() == 3, "values not named are left out");
        }
    }
    {
        // They never refuse the plan: B does not offer A's value, cannot write, answers another type or cannot be read.
        const auto left_out = [](const char* what, const std::function<void(Bench&)>& inject, const char* reason) {
            Bench bench(kRevisionTable);
            bench.a.props[0x5005].current = 4;
            inject(bench);
            const auto plan = bench.Copy();
            const auto* wb = SkippedOf(plan, D810WriteItem::WhiteBalance);
            check(plan.Ok() && wb && wb->second == reason && StepItems(plan).size() == 3 && bench.Writes() == 0, what);
        };
        left_out("not offered", [](Bench& b) { b.b.props[0x5005].values = {2, 5}; }, "value_not_advertised");
        left_out("GetSet 0", [](Bench& b) { b.b.props[0x5005].get_set = 0; }, "not_writable");
        left_out("no form", [](Bench& b) { b.b.props[0x5005].form = 0; b.b.props[0x5005].values.clear(); }, "not_writable");
        left_out("another type", [](Bench& b) { b.b.props[0x5005].type = 6; }, "unreadable");
        left_out("unreadable on B", [](Bench& b) { b.b.props.erase(0x5005); }, "unreadable");
        left_out("unreadable on A", [](Bench& b) { b.a.props.erase(0x5005); }, "unreadable");
        Bench bench(kRevisionTable);
        bench.a.props[0x500b].current = 2;
        bench.b.props[0x500b].values = {1, 3};
        const auto plan = bench.Copy();
        const auto* metering = SkippedOf(plan, D810WriteItem::Metering);
        check(plan.Ok() && metering && metering->second == "value_not_advertised" && StepItems(plan).size() == 3, "metering not offered by B");
    }
    {
        // The operator keeps only white balance.
        Bench bench(kRevisionTable);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        const auto plan = bench.Copy(std::vector<D810WriteItem>{D810WriteItem::WhiteBalance});
        check(plan.Ok() && StepItems(plan) == std::vector<Item>{Item::WhiteBalance} && plan.plan->skipped.size() == 4, "revision: one item kept, the others left out");
        const auto* metering = SkippedOf(plan, Item::Metering);
        check(metering && metering->second == "left_out_by_operator", "revision: left out by the operator");
    }
    {
        // The five keep their rules: A not in M still refuses the whole plan; B's missing value refuses it too.
        Bench bench(kRevisionTable);
        bench.a.props[0x500e].current = 3;
        const auto plan = bench.Copy();
        check(!plan.Ok() && std::find(plan.reasons.begin(), plan.reasons.end(), "a_not_manual") != plan.reasons.end(), "revision: A not in M refuses");
        Bench other(kRevisionTable);
        other.b.props[0x5007].values = {400, 560, 1100};
        check(!other.Copy().Ok(), "revision: an exposure item B does not offer still refuses");
    }
    {
        // Compensation is not a copy item, whatever the table enables.
        Bench bench(kAdr41Table);
        bench.b = Body(1667, 800, 64);
        bench.a.props[0x5010].current = 500;
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.empty() && plan.plan->skipped.empty(), "compensation is not copied");
        check(Refusal(D810RowRefusal(kAdr41Table, Item::ExposureCompensation, D810Camera::B, D810PlanSource::CopyAToB)) == "copy_item_out_of_scope", "compensation: out of the copy's scope");
        check(D810RowRefusal(kRevisionTable, Item::WhiteBalance, D810Camera::B, D810PlanSource::CopyAToB) == nullptr && D810RowRefusal(kRevisionTable, Item::Metering, D810Camera::B, D810PlanSource::CopyAToB) == nullptr, "white balance and metering: in the copy's scope");
        check(Refusal(D810RowRefusal(kAdr38Table, Item::WhiteBalance, D810Camera::B, D810PlanSource::CopyAToB)) == "row_disabled", "white balance with a disabled row");
        check(Refusal(D810RowRefusal(kRevisionTable, Item::WhiteBalance, D810Camera::A, D810PlanSource::CopyAToB)) == "copy_targets_cam_b_only", "never CAM-A");
    }
    {
        // The items that join a capture block's release.
        check(D810BlockItems(kAdr38Table).size() == 5, "five block items");
        const auto seven = D810BlockItems(kRevisionTable);
        check(seven.size() == 7 && std::find(seven.begin(), seven.end(), Item::WhiteBalance) != seven.end() && std::find(seven.begin(), seven.end(), Item::Metering) != seven.end() &&
            std::find(seven.begin(), seven.end(), Item::ExposureCompensation) == seven.end(), "seven block items once the rows are enabled");
        check(D810BlockItems(kD810ProductWriteTable).size() == 5, "the product table: five block items");
    }
}
void ChosenPlans() {
    {
        // The ADR-0038 table: no chosen plan for any item, target or value.
        Bench bench;
        for (const auto& spec : kD810WriteItems)
            for (const auto target : {D810WriteTarget::A, D810WriteTarget::B, D810WriteTarget::Both})
                for (const auto value : {0LL, 1LL, 64LL, 800LL, 1667LL, 4LL}) {
                    const auto plan = bench.Chosen({{spec.item, target, value}});
                    check(!plan.Ok() && plan.reasons.size() == 1 && plan.reasons[0] == "chosen_values_disabled", "chosen disabled");
                }
        check(bench.Writes() == 0, "nothing sent");
    }
    {
        // Even with chosen values on, CAM-A rows and extra items stay refused unless enabled.
        auto table = Adr38Table();
        table.chosen_values = true;
        Bench bench(table);
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}}).Ok(), "CAM-A row disabled");
        check(!bench.Chosen({{D810WriteItem::WhiteBalance, D810WriteTarget::Both, 4}}).Ok(), "extra row disabled");
    }
    {
        Bench bench(kAdr41Table);
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::B, 200}}).Ok(), "one camera only: refused (both cameras, same value)");
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::A, 200}}).Ok(), "CAM-A only: refused");
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 320}}).Ok(), "value not advertised");
        bench.a.props[0x500f].values = {64, 100, 200, 400, 800};
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 800}}).Ok(), "both: B does not advertise");
        const auto both = bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}});
        check(both.Ok() && both.plan->steps.size() == 2 && both.plan->steps[0].camera == D810Camera::A && both.plan->steps[1].camera == D810Camera::B, "A then B");
        check(!bench.Chosen({{D810WriteItem::ExposureMode, D810WriteTarget::Both, 3}}).Ok(), "M only");
        check(!bench.Chosen({{D810WriteItem::IsoAuto, D810WriteTarget::Both, 1}}).Ok(), "ISO auto OFF only");
        check(!bench.Chosen({{D810WriteItem::ShutterSpeed, D810WriteTarget::Both, 0xffffffffLL}}).Ok(), "no Bulb");
        check(!bench.Chosen({{D810WriteItem::WhiteBalance, D810WriteTarget::Both, 0x8013}}).Ok(), "no Preset");
        check(!bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}, {D810WriteItem::Iso, D810WriteTarget::Both, 200}}).Ok(), "duplicate choice");
        check(bench.Chosen({{D810WriteItem::WhiteBalance, D810WriteTarget::Both, 4}, {D810WriteItem::ExposureCompensation, D810WriteTarget::Both, -500}}).Ok(), "extra items");
        check(!bench.Chosen({{D810WriteItem::ExposureCompensation, D810WriteTarget::Both, -400}}).Ok(), "off the range step");
        check(!bench.Chosen({}).Ok(), "no choice");
        check(bench.Writes() == 0, "nothing sent");
    }
}

// --- The gate ----------------------------------------------------------------------------------
WpdValues Make(SettingsCommandKind kind, DWORD opcode, std::initializer_list<DWORD> arguments, std::uint64_t total = 2) {
    auto request = BuildSettingsExecute(kind == SettingsCommandKind::ExecuteWithDataToRead ? WpdLiveViewOperation::Frame : WpdLiveViewOperation::Start, opcode, arguments);
    if (kind == SettingsCommandKind::ExecuteWithDataToWrite) {
        request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE.pid);
        request->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, total);
    }
    return request;
}
void SetArguments(IPortableDeviceValues* request, const std::vector<std::uint32_t>& arguments) {
    Microsoft::WRL::ComPtr<IPortableDevicePropVariantCollection> collection;
    request->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, &collection);
    collection->Clear();
    for (const auto argument : arguments) {
        PROPVARIANT value{};
        value.vt = VT_UI4;
        value.ulVal = argument;
        collection->Add(&value);
    }
}
// Feed one clean read transfer of `payload` through the gate.
bool FeedRead(D810SettingsWriteGate& gate, WpdValues begin, const Bytes& payload, DWORD end_mtp = 0x2001) {
    static int n = 0;
    const auto context = L"ctx-g" + std::to_wstring(++n);
    if (!gate.Admit(begin.Get())) return false;
    gate.Observe(ReadBeginReply(context, payload.size()));
    if (!gate.Admit(BuildWpdLiveViewRead(context, static_cast<DWORD>(payload.size())).Get())) return false;
    gate.Observe(ChunkReply(payload));
    if (!gate.Admit(BuildWpdLiveViewEndTransfer(context).Get())) return false;
    gate.Observe(FinalReply(end_mtp));
    return gate.State() == D810WriteGateState::Idle;
}
D810WriteGateConfig Config() {
    D810WriteGateConfig config;
    config.iso_auto_code = 0xd054;
    config.entries = {{0x500f, 64}, {0x5007, 800}};
    return config;
}
// Live off, M, ISO auto off and the descriptions of both planned codes, this generation.
void Prepare(D810SettingsWriteGate& gate, unsigned live = 0, std::int64_t mode = 1, std::int64_t iso_auto = 0) {
    check(FeedRead(gate, BuildSettingsLiveState(), Bytes{static_cast<unsigned char>(live)}), "live read");
    check(FeedRead(gate, BuildSettingsPropDesc(0x500e), Desc(0x500e, 4, 1, mode, 2, {1, 2, 3, 4})), "mode read");
    check(FeedRead(gate, BuildSettingsPropDesc(0xd054), Desc(0xd054, 2, 1, iso_auto, 2, {0, 1})), "iso auto read");
    check(FeedRead(gate, BuildSettingsPropDesc(0x500f), Desc(0x500f, 4, 1, 100, 2, {64, 100})), "iso read");
    check(FeedRead(gate, BuildSettingsPropDesc(0x5007), Desc(0x5007, 4, 1, 560, 2, {560, 800})), "aperture read");
}
// Every opcode with every command kind and argument list, each on a copy of `base`.
// Returns how many were admitted and checks each against `expected`.
std::size_t Sweep(const D810SettingsWriteGate& base, const std::vector<std::vector<std::uint32_t>>& argument_sets, const std::vector<std::uint32_t>& opcodes,
    const std::function<bool(SettingsCommandKind, std::uint32_t, const std::vector<std::uint32_t>&)>& expected, std::size_t& writes) {
    std::size_t admitted = 0;
    for (const auto kind : {SettingsCommandKind::ExecuteWithoutData, SettingsCommandKind::ExecuteWithDataToRead, SettingsCommandKind::ExecuteWithDataToWrite}) {
        auto request = Make(kind, 0, {});
        for (const auto& arguments : argument_sets) {
            SetArguments(request.Get(), arguments);
            for (const auto opcode : opcodes) {
                request->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, opcode);
                D810SettingsWriteGate gate = base;
                const bool got = gate.Admit(request.Get());
                admitted += got;
                writes += gate.WritesSent() - base.WritesSent();
                if (got != expected(kind, opcode, arguments)) { check(false, "write gate sweep mismatch"); return admitted; }
            }
        }
    }
    return admitted;
}
bool ReadOnlyShape(SettingsCommandKind kind, std::uint32_t opcode, const std::vector<std::uint32_t>& arguments) {
    if (kind == SettingsCommandKind::ExecuteWithoutData) return opcode == 0x90c8 && arguments.empty();
    if (kind != SettingsCommandKind::ExecuteWithDataToRead || arguments.size() != 1) return false;
    if (opcode == 0x1015) return arguments[0] == 0xd1a2;
    return opcode == 0x1014 && (arguments[0] == 0x500e || arguments[0] == 0xd054 || arguments[0] == 0x500f || arguments[0] == 0x5007);
}
void GateSweep() {
    const std::vector<std::vector<std::uint32_t>> argument_sets{{}, {0x500f}, {0x5007}, {0xd1a2}, {0x500e}, {0x500f, 0x500f}, {0xffffffffu}, {0x1500f}};
    std::vector<std::uint32_t> every_opcode(0x10000);
    for (std::uint32_t opcode = 0; opcode <= 0xffff; ++opcode) every_opcode[opcode] = opcode;
    // The fresh and the prepared gate are swept over the operations the stage is about; Idle decides
    // from the request shape, the arm and the reads, and the armed gate below gets the full sweep.
    // The read operations with a bit above 16 set are other operations and refused.
    const std::vector<std::uint32_t> targeted{0x1016, 0x1014, 0x1015, 0x90c8, 0x9207, 0x90c2, 0x90c0, 0x90c7, 0x1017, 0x9201, 0x1001, 0x1008, 0x100b, 0x9204,
        0x00011014, 0x80001015, 0x000190c8, 0x00011016};
    std::size_t writes = 0;
    // A fresh gate: the read operations only, no write.
    check(Sweep(D810SettingsWriteGate(Config()), argument_sets, targeted, ReadOnlyShape, writes) == 5, "fresh gate reads only");
    check(writes == 0, "fresh gate writes nothing");
    // Prepared but not armed: still no write.
    D810SettingsWriteGate prepared(Config());
    Prepare(prepared);
    check(Sweep(prepared, argument_sets, targeted, ReadOnlyShape, writes) == 5 && writes == 0, "prepared gate without arm writes nothing");
    // Armed for (0x500f, 64): the one write shape, plus the reads.
    D810SettingsWriteGate armed = prepared;
    check(armed.Arm(D810Camera::B, 0x500f, 64) == nullptr, "arm");
    const auto with_write = [](SettingsCommandKind kind, std::uint32_t opcode, const std::vector<std::uint32_t>& arguments) {
        return ReadOnlyShape(kind, opcode, arguments) ||
            (kind == SettingsCommandKind::ExecuteWithDataToWrite && opcode == 0x1016 && arguments == std::vector<std::uint32_t>{0x500f});
    };
    // Full opcode range on the armed gate, with the argument lists that decide a write (none, the armed code,
    // the other planned code, the Live state code, the armed code with a high bit).
    const std::vector<std::vector<std::uint32_t>> write_arguments{{}, {0x500f}, {0x5007}, {0xd1a2}, {0x1500f}};
    check(Sweep(armed, write_arguments, every_opcode, with_write, writes) == 5 && writes == 1, "armed gate: exactly one write shape");
    check(Sweep(armed, argument_sets, targeted, with_write, writes) == 6 && writes == 2, "armed gate: every argument list on the relevant opcodes");
    // The armed write with a wrong declared size is refused.
    for (const std::uint64_t total : {0ULL, 1ULL, 4ULL, 3ULL}) {
        D810SettingsWriteGate gate = armed;
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}, total).Get()) && gate.WritesSent() == 0, "declared size");
    }
    // Bits above 16 change the value.
    for (const auto high : {0x00010000u, 0x80000000u}) {
        D810SettingsWriteGate gate = armed;
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016 | high, {0x500f}).Get()), "opcode high bits");
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f | high}).Get()), "argument high bits");
    }
    // Arm: only the plan's pairs, over every code.
    std::size_t arms = 0;
    for (std::uint32_t code = 0; code <= 0xffff; ++code)
        for (const std::int64_t value : {64LL, 100LL, 800LL}) {
            D810SettingsWriteGate gate = prepared;
            const bool ok = gate.Arm(D810Camera::B, static_cast<std::uint16_t>(code), value) == nullptr;
            arms += ok;
            if (ok != ((code == 0x500f && value == 64) || (code == 0x5007 && value == 800))) { check(false, "arm sweep"); code = 0x10000; break; }
        }
    check(arms == 2, "two pairs can be armed");
    // After the write of 0x500f ended: re-reads done, 0x500f can never be armed again.
    D810SettingsWriteGate after = armed;
    check(after.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}).Get()), "begin");
    after.Observe(WriteBeginReply(L"ctx-w"));
    check(after.Admit(BuildSettingsWriteData(L"ctx-w", EncodeD810Value(D810ValueType::Uint16, 64)).Get()), "data");
    after.Observe(WrittenReply(2));
    check(after.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "end");
    after.Observe(FinalReply());
    check(after.State() == D810WriteGateState::Idle && after.WritesEnded() == 1, "write ended");
    check(!after.FreshDesc(0x500f) && !after.FreshLiveState(), "generation advanced: earlier reads are stale");
    check(std::string(after.Arm(D810Camera::B, 0x5007, 800)) == "live_not_confirmed_off", "next write needs new reads");
    Prepare(after);
    check(std::string(after.Arm(D810Camera::B, 0x500f, 64)) == "already_written", "one write per code");
    check(after.Arm(D810Camera::B, 0x5007, 800) == nullptr, "the other planned code");
    const auto only_aperture = [](SettingsCommandKind kind, std::uint32_t opcode, const std::vector<std::uint32_t>& arguments) {
        return ReadOnlyShape(kind, opcode, arguments) ||
            (kind == SettingsCommandKind::ExecuteWithDataToWrite && opcode == 0x1016 && arguments == std::vector<std::uint32_t>{0x5007});
    };
    writes = 0;
    check(Sweep(after, argument_sets, targeted, only_aperture, writes) == 6 && writes == 1, "after one write only the other code");
    // An invalid configuration retires the gate at once.
    for (const auto& entries : std::vector<std::vector<D810WriteGateEntry>>{{{0xd200, 1}}, {{0x500e, 3}}, {{0xd054, 1}}, {{0x500f, 64}, {0x500f, 100}}, {{0x500f, 0x10000}}, {{0xd16a, 0}}, {{0xd100, 1667}}}) {
        D810WriteGateConfig config;
        config.iso_auto_code = 0xd054;
        config.entries = entries;
        D810SettingsWriteGate gate(config);
        check(gate.Retired(), "invalid configuration");
    }
    D810WriteGateConfig bad_iso;
    bad_iso.iso_auto_code = 0x500f;
    check(D810SettingsWriteGate(bad_iso).Retired(), "ISO auto code must be a candidate");
}
void GateArmConditions() {
    {
        D810SettingsWriteGate gate(Config());
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 64)) == "live_not_confirmed_off", "no reads");
    }
    {
        // The key is (destination, code, value): a CAM-B gate never arms for CAM-A.
        D810SettingsWriteGate gate(Config());
        Prepare(gate);
        check(std::string(gate.Arm(D810Camera::A, 0x500f, 64)) == "other_destination", "other destination");
        check(gate.Arm(D810Camera::B, 0x500f, 64) == nullptr, "own destination");
        auto config = Config();
        config.camera = D810Camera::A;
        D810SettingsWriteGate a_gate(config);
        Prepare(a_gate);
        check(std::string(a_gate.Arm(D810Camera::B, 0x500f, 64)) == "other_destination", "a CAM-A gate refuses CAM-B");
    }
    {
        // The operation latch: a failure in one session stops arming in every session of the operation.
        D810OperationLatch latch;
        auto config = Config();
        config.latch = &latch;
        D810SettingsWriteGate first(config), second(config);
        Prepare(first);
        Prepare(second);
        first.Cancelled();
        check(latch.Tripped(), "a retired gate trips the latch");
        check(std::string(second.Arm(D810Camera::B, 0x500f, 64)) == "operation_latched", "the other session cannot arm");
    }
    {
        D810SettingsWriteGate gate(Config());
        Prepare(gate, 1);
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 64)) == "live_not_confirmed_off", "live on");
    }
    {
        D810SettingsWriteGate gate(Config());
        Prepare(gate, 0, 3);
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 64)) == "mode_not_confirmed_manual", "not M");
    }
    {
        D810SettingsWriteGate gate(Config());
        Prepare(gate, 0, 1, 1);
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 64)) == "iso_auto_not_confirmed_off", "ISO auto on");
    }
    {
        D810SettingsWriteGate gate(Config());
        Prepare(gate);
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 100)) == "not_in_plan", "value not in plan");
        check(gate.Arm(D810Camera::B, 0x500f, 64) == nullptr, "arm");
        check(std::string(gate.Arm(D810Camera::B, 0x5007, 800)) == "already_armed", "one arm at a time");
        // A read in Idle is fine; a refused request in Idle drops the arm.
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x5007}).Get()) && !gate.Armed(), "refusal drops the arm");
    }
    {
        // The description says the value is not offered any more.
        D810WriteGateConfig config = Config();
        D810SettingsWriteGate gate(config);
        check(FeedRead(gate, BuildSettingsLiveState(), Bytes{0}), "live");
        check(FeedRead(gate, BuildSettingsPropDesc(0x500e), Desc(0x500e, 4, 1, 1, 2, {1})), "mode");
        check(FeedRead(gate, BuildSettingsPropDesc(0xd054), Desc(0xd054, 2, 1, 0, 2, {0})), "iso auto");
        check(FeedRead(gate, BuildSettingsPropDesc(0x500f), Desc(0x500f, 4, 1, 100, 2, {100, 200})), "iso");
        check(std::string(gate.Arm(D810Camera::B, 0x500f, 64)) == "value_not_advertised", "not advertised now");
        check(FeedRead(gate, BuildSettingsPropDesc(0x5007), Desc(0x5007, 4, 0, 560, 2, {560, 800})), "aperture getset 0");
        check(std::string(gate.Arm(D810Camera::B, 0x5007, 800)) == "not_writable", "GetSet 0 now");
    }
    {
        // A read the gate did not see end cleanly retires it (no write after).
        D810SettingsWriteGate gate(Config());
        check(!FeedRead(gate, BuildSettingsPropDesc(0x500f), Desc(0x500f, 4, 1, 100, 2, {64, 100}), 0x2002) && gate.Retired(), "refused END of a read retires");
        check(gate.Arm(D810Camera::B, 0x500f, 64) != nullptr, "retired gate cannot arm");
    }
}
// White balance and metering at the gate: the values of the closed lists only, and the same reads first as the other items.
void RevisionGate() {
    const auto config = [](std::int64_t white_balance, std::int64_t metering) {
        D810WriteGateConfig c;
        c.iso_auto_code = 0xd054;
        c.entries = {{0x5005, white_balance}, {0x500b, metering}};
        return c;
    };
    const auto prepare = [](D810SettingsWriteGate& gate, std::int64_t mode = 1) {
        check(FeedRead(gate, BuildSettingsLiveState(), Bytes{0}), "live read");
        check(FeedRead(gate, BuildSettingsPropDesc(0x500e), Desc(0x500e, 4, 1, mode, 2, {1, 2, 3, 4})), "mode read");
        check(FeedRead(gate, BuildSettingsPropDesc(0xd054), Desc(0xd054, 2, 1, 0, 2, {0, 1})), "iso auto read");
        check(FeedRead(gate, BuildSettingsPropDesc(0x5005), Desc(0x5005, 4, 1, 2, 2, {2, 4, 0x8010, 0x8012, 0x8013, 0x8014})), "white balance read");
        check(FeedRead(gate, BuildSettingsPropDesc(0x500b), Desc(0x500b, 4, 1, 3, 2, {1, 2, 3, 4, 5})), "metering read");
    };
    {
        D810SettingsWriteGate gate(config(4, 2));
        check(!gate.Retired(), "white balance and metering are valid entries");
        check(std::string(gate.Arm(D810Camera::B, 0x5005, 4)) == "live_not_confirmed_off", "no reads, no arm");
        prepare(gate);
        check(gate.Arm(D810Camera::B, 0x5005, 4) == nullptr, "named kind armed");
        check(gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x5005}).Get()) && gate.WritesSent() == 1, "the armed begin goes through");
    }
    {
        D810SettingsWriteGate gate(config(4, 2));
        prepare(gate);
        check(gate.Arm(D810Camera::B, 0x500b, 2) == nullptr, "named metering mode armed");
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x5005}).Get()) && gate.WritesSent() == 0, "another code than the armed one is refused");
    }
    {
        // Every code and several values: only the two planned pairs arm.
        D810SettingsWriteGate gate(config(4, 2));
        prepare(gate);
        std::size_t arms = 0;
        for (std::uint32_t code = 0; code <= 0xffff; ++code)
            for (const std::int64_t value : {2LL, 4LL, 5LL, 0x8013LL}) {
                D810SettingsWriteGate copy = gate;
                const bool ok = copy.Arm(D810Camera::B, static_cast<std::uint16_t>(code), value) == nullptr;
                arms += ok;
                if (ok != ((code == 0x5005 && value == 4) || (code == 0x500b && value == 2))) { check(false, "revision arm sweep"); code = 0x10000; break; }
            }
        check(arms == 2, "revision: two pairs can be armed");
    }
    for (const std::int64_t value : {1LL, 3LL, 0x8012LL, 0x8013LL}) {
        D810SettingsWriteGate gate(config(value, 2));
        prepare(gate);
        check(std::string(gate.Arm(D810Camera::B, 0x5005, value)) == "value_content_dependent", "content-dependent kinds are never armed, although the body offers them");
    }
    {
        D810SettingsWriteGate gate(config(0x8014, 5));
        prepare(gate);
        check(std::string(gate.Arm(D810Camera::B, 0x5005, 0x8014)) == "value_not_listed", "an offered but unnamed white balance is never armed");
        check(std::string(gate.Arm(D810Camera::B, 0x500b, 5)) == "value_not_listed", "an offered but unnamed metering mode is never armed");
    }
    {
        // Same reads first as the other non-mode items: M and ISO auto off.
        D810SettingsWriteGate gate(config(4, 2));
        prepare(gate, 3);
        check(std::string(gate.Arm(D810Camera::B, 0x5005, 4)) == "mode_not_confirmed_manual", "not M: no white balance");
    }
}
// Steps of one write in the gate, and what each failure does.
D810SettingsWriteGate ArmedGate() {
    D810SettingsWriteGate gate(Config());
    Prepare(gate);
    check(gate.Arm(D810Camera::B, 0x500f, 64) == nullptr, "armed");
    return gate;
}
WpdValues GoodData(const wchar_t* context = L"ctx-w") { return BuildSettingsWriteData(context, EncodeD810Value(D810ValueType::Uint16, 64)); }
void GateWriteOrder() {
    const auto begin = [] { return Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}); };
    {
        auto gate = ArmedGate();
        const auto generation = gate.Generation();
        check(gate.Admit(begin().Get()), "begin");
        // Consumed and advanced before the transport is called.
        check(!gate.Armed() && gate.Generation() == generation + 1 && gate.WritesSent() == 1, "arm consumed before transport");
        check(!gate.Admit(begin().Get()), "no second begin");
        check(gate.Retired(), "a request while the begin's reply is pending retires");
    }
    {
        // An explicit MTP refusal of the write begin does not return to Idle.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(FinalReply(0x2002));
        check(gate.Retired() && gate.WritesSent() == 1, "refused begin: retired, counted as sent");
        check(!gate.Admit(BuildSettingsLiveState().Get()), "nothing after");
    }
    for (const auto& [what, data] : std::vector<std::pair<const char*, WpdValues>>{
             {"wrong context", GoodData(L"ctx-x")},
             {"wrong bytes", BuildSettingsWriteData(L"ctx-w", EncodeD810Value(D810ValueType::Uint16, 100))},
             {"too long", BuildSettingsWriteData(L"ctx-w", Bytes{64, 0, 0, 0})},
             {"too short", BuildSettingsWriteData(L"ctx-w", Bytes{64})},
         }) {
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(!gate.Admit(data.Get()) && gate.WriteDataSent() == 0, what);
        check(gate.Retired(), what);
        check(gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "the open transfer may be ended once");
        gate.Observe(FinalReply());
        check(!gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "only once");
    }
    {
        // The declared count must match the buffer.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        auto data = GoodData();
        data->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, 4);
        check(!gate.Admit(data.Get()) && gate.Retired(), "declared count");
    }
    {
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(!gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()) && gate.Retired(), "END before data retires");
    }
    {
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(gate.Admit(GoodData().Get()), "data");
        check(!gate.Admit(GoodData().Get()), "second WRITE_DATA refused");
        check(gate.Retired(), "second WRITE_DATA retires");
    }
    for (const DWORD code : {0x2002u, 0x2005u, 0x2019u, 0xa001u, 0u}) {
        // After data, any END refusal closes.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(gate.Admit(GoodData().Get()), "data");
        gate.Observe(WrittenReply(2));
        check(gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "end");
        gate.Observe(code ? FinalReply(code) : WpdLiveViewExchange{S_OK, Reply()});
        check(gate.Retired() && gate.WritesEnded() == 0, "refused END after data closes");
    }
    {
        // Data accepted short: the END still closes the transfer, then retired.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(gate.Admit(GoodData().Get()), "data");
        gate.Observe(WrittenReply(1));
        check(gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "end");
        gate.Observe(FinalReply());
        check(gate.Retired(), "short write retires");
    }
    {
        // Transport failure during data: one END allowed, nothing else.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(gate.Admit(GoodData().Get()), "data");
        gate.TransportFailed();
        check(gate.Retired(), "retired");
        check(!gate.Admit(BuildSettingsLiveState().Get()), "no read");
        check(gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "cleanup END");
        gate.Observe(FinalReply());
        check(!gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "only one");
    }
    {
        // Transport failure at the begin: no context, nothing at all.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.TransportFailed();
        check(gate.Retired() && gate.WritesSent() == 1, "counted as sent");
        check(!gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "no END without a context");
    }
    {
        // Cancel: nothing more, not even an END.
        auto gate = ArmedGate();
        check(gate.Admit(begin().Get()), "begin");
        gate.Observe(WriteBeginReply(L"ctx-w"));
        check(gate.Admit(GoodData().Get()), "data");
        gate.Cancelled();
        check(gate.Retired() && !gate.Admit(BuildWpdLiveViewEndTransfer(L"ctx-w").Get()), "cancel");
    }
}
// A request of each command kind. `context` is used by the transfer kinds (and
// set on the other command too, which must not matter).
WpdValues KindRequest(SettingsCommandKind kind, const std::wstring& context) {
    switch (kind) {
    case SettingsCommandKind::ExecuteWithoutData: return BuildSettingsDeviceReady();
    case SettingsCommandKind::ExecuteWithDataToRead: return BuildSettingsPropDesc(0x500f);
    case SettingsCommandKind::ExecuteWithDataToWrite: return Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f});
    case SettingsCommandKind::ReadData: return BuildWpdLiveViewRead(context, 16);
    case SettingsCommandKind::WriteData: return GoodData(context.c_str());
    case SettingsCommandKind::EndDataTransfer: return BuildWpdLiveViewEndTransfer(context);
    default: {
        WpdValues request;
        CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&request));
        request->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, WPD_COMMAND_COMMON_RESET_DEVICE.fmtid);
        request->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, WPD_COMMAND_COMMON_RESET_DEVICE.pid);
        request->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, context.c_str());
        return request;
    }
    }
}
// Every command kind in every state of the state machine, with the gate's own
// context and another one. Only the pairs of the state table pass; a refusal in
// Idle drops the arm and stays Idle, any other refusal retires the gate and
// leaves at most the one END of the open transfer.
void GateStateSweep() {
    const std::wstring own = L"ctx-own", other = L"ctx-other";
    const auto begin = [] { return Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}); };
    const auto idle = ArmedGate();
    auto read_open = ArmedGate();
    check(read_open.Admit(BuildSettingsPropDesc(0x500f).Get()), "read begin");
    read_open.Observe(ReadBeginReply(own, 16));
    auto write_open = ArmedGate();
    check(write_open.Admit(begin().Get()), "write begin");
    write_open.Observe(WriteBeginReply(own));
    auto write_sent = write_open;
    check(write_sent.Admit(GoodData(own.c_str()).Get()), "data");
    write_sent.Observe(WrittenReply(2));
    auto retired = write_open;
    check(!retired.Admit(GoodData(other.c_str()).Get()), "data of another transfer");
    using S = D810WriteGateState;
    using K = SettingsCommandKind;
    check(idle.State() == S::Idle && idle.Armed() && read_open.State() == S::ReadOpen && write_open.State() == S::WriteOpen &&
        write_sent.State() == S::WriteSent && retired.State() == S::Retired, "the five states");
    const auto passes = [](S state, K kind, bool own_context) {
        switch (state) {
        case S::Idle: return kind == K::ExecuteWithoutData || kind == K::ExecuteWithDataToRead || kind == K::ExecuteWithDataToWrite;
        case S::ReadOpen: return own_context && (kind == K::ReadData || kind == K::EndDataTransfer);
        case S::WriteOpen: return own_context && kind == K::WriteData;
        case S::WriteSent:
        case S::Retired: return own_context && kind == K::EndDataTransfer;
        default: return false;
        }
    };
    const auto next = [](S state, K kind) {
        switch (state) {
        case S::Idle: return kind == K::ExecuteWithoutData ? S::ReadyPending : kind == K::ExecuteWithDataToRead ? S::ReadBegun : S::WriteBegun;
        case S::ReadOpen: return kind == K::ReadData ? S::ReadOpen : S::ReadEnding;
        case S::WriteOpen: return S::WriteSent;
        case S::WriteSent: return S::WriteEnding;
        default: return S::Retired;
        }
    };
    std::size_t cases = 0, admitted = 0;
    const D810SettingsWriteGate* const bases[] = {&idle, &read_open, &write_open, &write_sent, &retired};
    for (const auto* base : bases) {
        for (const auto kind : {K::ExecuteWithoutData, K::ExecuteWithDataToRead, K::ExecuteWithDataToWrite, K::ReadData, K::WriteData, K::EndDataTransfer, K::Other}) {
            for (const bool own_context : {true, false}) {
                ++cases;
                auto gate = *base;
                const auto state = base->State();
                const bool got = gate.Admit(KindRequest(kind, own_context ? own : other).Get());
                admitted += got;
                check(got == passes(state, kind, own_context), "state sweep: admission");
                check(gate.WritesSent() - base->WritesSent() == (got && kind == K::ExecuteWithDataToWrite ? 1u : 0u), "state sweep: only the armed begin counts as sent");
                if (got) {
                    check(gate.State() == next(state, kind), "state sweep: next state");
                } else if (state == S::Idle) {
                    check(gate.State() == S::Idle && !gate.Armed(), "state sweep: an Idle refusal drops the arm");
                } else {
                    check(gate.Retired(), "state sweep: any other refusal retires");
                    // At most the one END of the open transfer, never one of another transfer.
                    check(!gate.Admit(BuildWpdLiveViewEndTransfer(other).Get()), "state sweep: no END of another transfer");
                    check(gate.Admit(BuildWpdLiveViewEndTransfer(own).Get()), "state sweep: the open transfer may be ended");
                    check(!gate.Admit(BuildWpdLiveViewEndTransfer(own).Get()) && !gate.Admit(BuildSettingsLiveState().Get()), "state sweep: once, then nothing");
                }
            }
        }
    }
    // Idle: the three executes with either context (6); ReadOpen: own READ_DATA and END (2);
    // WriteOpen: own WRITE_DATA; WriteSent: own END; Retired: the own cleanup END.
    check(cases == 70 && admitted == 11, "state sweep: 70 cases, 11 admitted");
    {
        // The END of a write before the data's reply was seen is refused.
        auto gate = write_open;
        check(gate.Admit(GoodData(own.c_str()).Get()), "data");
        check(!gate.Admit(BuildWpdLiveViewEndTransfer(own).Get()) && gate.Retired(), "END before the data reply");
    }
}
// Refusals and failures of a read begin and of DeviceReady retire the gate
// (the read-only gate's "an explicit refusal returns to Idle" is not used).
void GateFailureShapes() {
    const std::vector<std::pair<const char*, WpdLiveViewExchange>> failures_of_a_reply{
        {"0x2002", FinalReply(0x2002)},
        {"0x2005", FinalReply(0x2005)},
        {"0xa001", FinalReply(0xa001)},
        {"dispatch HRESULT", WpdLiveViewExchange{E_FAIL, Reply()}},
        {"command HRESULT", CommandFailedReply()},
    };
    const auto nothing_after = [](D810SettingsWriteGate& gate) {
        return !gate.Admit(BuildSettingsPropDesc(0x500f).Get()) && !gate.Admit(BuildSettingsLiveState().Get()) && !gate.Admit(BuildSettingsDeviceReady().Get()) &&
            !gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}).Get()) && gate.WritesSent() == 0;
    };
    for (const auto& [what, reply] : failures_of_a_reply) {
        for (const bool live : {false, true}) {
            auto gate = ArmedGate();
            check(gate.Admit((live ? BuildSettingsLiveState() : BuildSettingsPropDesc(0x500f)).Get()), "read begin");
            gate.Observe(reply);
            check(gate.Retired() && !gate.Armed(), what);
            check(nothing_after(gate), "nothing after a refused read begin");
        }
        D810SettingsWriteGate gate(Config());
        check(gate.Admit(BuildSettingsDeviceReady().Get()) && gate.State() == D810WriteGateState::ReadyPending, "ready");
        gate.Observe(reply);
        check(gate.Retired(), what);
        check(nothing_after(gate), "nothing after a refused ready");
    }
    // 0x2019 (DeviceBusy) is "not yet": back to Idle and asked again. OK: Idle.
    for (const DWORD code : {0x2019u, 0x2001u}) {
        D810SettingsWriteGate gate(Config());
        check(gate.Admit(BuildSettingsDeviceReady().Get()), "ready");
        gate.Observe(FinalReply(code));
        check(gate.State() == D810WriteGateState::Idle && gate.Admit(BuildSettingsDeviceReady().Get()), "busy or ready: Idle");
    }
}
// The other gates still refuse every write shape (none of them is widened).
void OtherGatesStillRefuseWrites() {
    D810SettingsReadGate read;
    check(!read.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}).Get()), "read gate refuses 0x1016");
    check(!read.Admit(GoodData().Get()), "read gate refuses WRITE_DATA");
}

// --- Runs ----------------------------------------------------------------------------------------
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
void HappyCopy() {
    Bench bench(kAdr38Table, -1, true);
    const auto run = bench.CopyRun();
    check(run.verdict == D810WriteVerdict::Match && run.sent == 3 && run.reason.empty(), "copy matched");
    check(bench.b.props[0x500f].current == 64 && bench.b.props[0x500d].current == 1667 && bench.b.props[0x5007].current == 800, "B now reads A's values");
    check(bench.a.write_begins == 0 && bench.a.write_datas == 0 && bench.a.write_ends == 0, "A received no write");
    check(bench.b.write_begins == 3 && bench.b.write_datas == 3 && bench.b.write_ends == 3 && NoResend(bench.b), "one begin, data, END per item");
    check(bench.a.forbidden == 0 && bench.b.forbidden == 0, "no other operation");
    check(!bench.access.overlap, "never both open");
    check(bench.controller->CaptureAllowed() && bench.controller->Block() == D810CaptureBlock::None, "capture allowed");
    const auto journal = bench.Journal();
    check(Contains(journal, "ev=plan") && Contains(journal, "ev=step i=2 alias=CAM-B item=aperture code=0x5007 before=0x0230 value=0x0320"), "journal plan and steps");
    check(Contains(journal, "ev=result i=0 readback=0x0040 verdict=match") && Contains(journal, "ev=end t=2000-01-01T00:00:00Z verdict=match sent=3"), "journal results");
    check(!HasIdentifierShape(journal), "journal has no identifiers or contexts");
    // ADR-0038 decision 11: no digest; the plan line has six fields.
    check(!Contains(journal, "digest") && Contains(journal, " ev=plan t=2000-01-01T00:00:00Z source=copy steps=3\n"), "plan line without a digest");
    for (const auto line : SplitD810RecordLines(journal))
        if (Contains(std::string(line), "ev=plan")) check(SplitD810Fields(line).size() == 6, "six plan fields");
    for (const auto& line : bench.log) check(!HasIdentifierShape(line), "log has no identifiers or contexts");
    // Order: for each item Live, the preconditions and the code, then the write, then the read back.
    const auto& trace = bench.b.trace;
    const auto first_write = std::find(trace.begin(), trace.end(), "1016(500f)");
    check(first_write != trace.end(), "iso written");
    if (first_write != trace.end()) {
        const std::vector<std::string> before(trace.begin(), first_write);
        const std::vector<std::string> expected_tail{"1015(d1a2)", "read", "end", "1014(500e)", "read", "end", "1014(d054)", "read", "end", "1014(500f)", "read", "end"};
        check(before.size() >= expected_tail.size() && std::equal(expected_tail.begin(), expected_tail.end(), before.end() - static_cast<std::ptrdiff_t>(expected_tail.size())), "reads before the write");
        check(std::vector<std::string>(first_write, first_write + 6) == std::vector<std::string>{"1016(500f)", "write_data", "write_end", "1014(500f)", "read", "end"}, "write then read back");
    }
    // The plan was consumed.
    check(bench.controller->Execute(0, bench.access, bench.options).reason == "plan_not_confirmed", "plan consumed");
}
// The product table end to end on the fakes: one begin, data and END, all on CAM-B's ISO code; the other items are not touched.
void ProductRun() {
    {
        Bench bench(kProduct);
        bench.access.firmware[1] = kD810ProductWriteTable.verified_firmware;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Match && run.sent == 1 && run.reason.empty(), "product run: matched with one write");
        check(bench.b.props[0x500f].current == 64 && bench.b.props[0x500d].current == 333 && bench.b.props[0x5007].current == 560, "product run: only ISO changed on B");
        check(bench.b.write_begins == 1 && bench.b.write_datas == 1 && bench.b.write_ends == 1 && bench.b.begins_by_code.size() == 1 && bench.b.begins_by_code.count(0x500f) == 1, "product run: one write, to the ISO code");
        check(bench.a.write_begins == 0 && bench.a.write_datas == 0 && bench.a.write_ends == 0 && bench.a.forbidden == 0 && bench.b.forbidden == 0, "product run: A is only read");
    }
    {
        // A body that reports any other firmware version is not written.
        Bench bench(kProduct);
        const auto run = bench.CopyRun();
        check(run.sent == 0 && run.reason == "body_not_verified:CAM-B" && bench.Writes() == 0, "product run: another firmware, nothing sent");
    }
}
void BodyAndConfirmation() {
    {
        // A target body that is not the verified model or firmware: nothing sent.
        Bench bench;
        bench.access.firmware[1] = "1.01";
        const auto run = bench.CopyRun();
        check(run.sent == 0 && run.reason == "body_not_verified:CAM-B" && bench.Writes() == 0, "firmware differs");
    }
    {
        Bench bench;
        bench.access.model[1] = "D850";
        check(bench.CopyRun().sent == 0 && bench.Writes() == 0, "model differs");
    }
    {
        // CAM-A is only read in a copy, so its version does not matter there.
        Bench bench;
        bench.access.firmware[0] = "9.99";
        check(bench.CopyRun().verdict == D810WriteVerdict::Match, "source camera only read");
    }
    {
        // An unknown plan id, a plan never confirmed.
        Bench bench;
        const auto plan = bench.Copy();
        check(bench.controller->Execute(plan.plan->id, bench.access, bench.options).reason == "plan_not_confirmed", "not confirmed");
        check(std::string(bench.controller->Confirm(plan.plan->id + 1)) == "plan_not_pending", "unknown id");
        check(bench.Writes() == 0, "nothing sent");
    }
    {
        // A table that fails its own checks refuses everything.
        auto table = Adr38Table();
        table.rows[0].code = 0x1234;
        Bench bench(table);
        check(!bench.Copy().Ok() && bench.Copy().reasons[0] == "invalid_table" && bench.Writes() == 0, "invalid table");
    }
}
void RecordMustBeWrittenFirst() {
    for (const int ok : {0, 1, 3}) {
        Bench bench(kAdr38Table, ok);
        const auto plan = bench.Copy();
        const auto reads = bench.b.read_begins;
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::NotSent && run.reason == "record_unwritable" && bench.Writes() == 0, "record unwritable: nothing sent");
        check(bench.b.read_begins == reads, "not even read");
    }
    {
        // The record folder is not trusted (a file where a folder must be): nothing sent.
        Bench bench(kAdr38Table, -1, true);
        fs::remove_all(bench.folder);
        WriteFile(bench.folder, "not a folder");
        const auto plan = bench.Copy();
        check(!plan.Ok() && plan.reasons[0] == "reread_first", "unreadable record blocks planning");
        check(bench.Writes() == 0, "nothing sent");
        D810SettingsFileJournal relative(fs::path(L"relative-folder"));
        check(!relative.Append("a0sw1 ev=reread t=x verdict=match") && !fs::exists(L"relative-folder"), "no relative folder");
    }
    {
        // The product folder is the known LocalAppData folder (resolved, not created here).
        const auto folder = ProductD810SettingsWriteFolder();
        check(folder && folder->is_absolute() && folder->filename() == L"settings-write" && folder->parent_path().filename() == L"A0CameraStitcher", "product folder");
    }
}
void PlanChangedAfterConfirm() {
    {
        Bench bench;
        const auto plan = bench.Copy();
        check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
        bench.a.props[0x5007].current = 1100;
        const auto run = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
        check(run.verdict == D810WriteVerdict::NotSent && run.reason.rfind("plan_changed:CAM-A", 0) == 0 && bench.Writes() == 0, "A changed");
        check(bench.controller->CaptureAllowed(), "nothing sent, no block");
        check(Contains(bench.Journal(), "verdict=not_sent sent=0"), "recorded as not sent");
    }
    {
        Bench bench;
        const auto plan = bench.Copy();
        bench.b.props[0x500e].current = 3;
        check(bench.Run(plan).reason.rfind("plan_changed:CAM-B", 0) == 0 && bench.Writes() == 0, "B changed");
    }
    {
        Bench bench;
        const auto plan = bench.Copy();
        bench.b.props[0x5007].values = {560, 1100};
        check(bench.Run(plan).sent == 0 && bench.Writes() == 0, "value no longer offered");
    }
    {
        // A changes after the verification read: the in-session check before
        // each write stops before the first send.
        Bench bench;
        const auto plan = bench.Copy();
        int seen = 0;
        bench.b.after = [&](const std::string& entry) { if (entry == "1014(500f)" && ++seen == 2) bench.b.props[0x500f].current = 200; };
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason == "changed_before_write" && bench.Writes() == 0, "changed between verification and write");
    }
}
void FailuresStopAndBlock() {
    struct Case { const char* what; std::function<void(FakeWriteBody&)> inject; D810WriteVerdict verdict; std::size_t ends; };
    const std::vector<Case> cases{
        {"begin throws", [](FakeWriteBody& b) { b.begin_throw.insert(0x500d); }, D810WriteVerdict::Unknown, 1},
        {"begin refused", [](FakeWriteBody& b) { b.begin_reject.insert(0x500d); }, D810WriteVerdict::Unknown, 1},
        {"begin command failed", [](FakeWriteBody& b) { b.begin_command_failed.insert(0x500d); }, D810WriteVerdict::Unknown, 1},
        {"data fails", [](FakeWriteBody& b) { b.data_fail.insert(0x500d); }, D810WriteVerdict::Unknown, 2},
        {"data throws", [](FakeWriteBody& b) { b.data_throw.insert(0x500d); }, D810WriteVerdict::Unknown, 2},
        {"data short", [](FakeWriteBody& b) { b.data_short.insert(0x500d); }, D810WriteVerdict::Unknown, 2},
        {"END refused", [](FakeWriteBody& b) { b.end_reject[0x500d] = 0x2005; }, D810WriteVerdict::Unknown, 2},
        {"END throws", [](FakeWriteBody& b) { b.end_throw.insert(0x500d); }, D810WriteVerdict::Unknown, 2},
        {"read back unreadable", [](FakeWriteBody& b) { b.unreadable_after_write.insert(0x500d); }, D810WriteVerdict::Unknown, 2},
        {"applied another value", [](FakeWriteBody& b) { b.apply_as[0x500d] = 2500; }, D810WriteVerdict::Mismatch, 2},
        {"not applied", [](FakeWriteBody& b) { b.ignore_write.insert(0x500d); }, D810WriteVerdict::Mismatch, 2},
    };
    for (const auto& c : cases) {
        Bench bench;
        c.inject(bench.b);
        const auto run = bench.CopyRun();
        check(run.verdict == c.verdict, c.what);
        check(run.sent == 2, c.what); // iso, then the failing shutter; aperture never sent
        check(bench.b.begins_by_code.count(0x5007) == 0 && NoResend(bench.b), "nothing after the failure, no resend");
        check(static_cast<std::size_t>(bench.b.write_ends) <= c.ends, "at most the one END of the open transfer");
        check(!bench.controller->CaptureAllowed(), "capture blocked");
        const auto expected = c.verdict == D810WriteVerdict::Unknown ? D810CaptureBlock::Unknown : D810CaptureBlock::Mismatch;
        check(bench.controller->Block() == expected, c.what);
        check(bench.Restarted().Block() == expected, "the block survives a restart");
        check(!HasIdentifierShape(bench.Journal()), "journal clean");
    }
    {
        // Unknown blocks new plans until a reread.
        Bench bench;
        bench.b.begin_throw.insert(0x500f);
        check(bench.CopyRun().verdict == D810WriteVerdict::Unknown, "unknown");
        const auto again = bench.Copy();
        check(!again.Ok() && again.reasons[0] == "reread_first", "no new plan while unknown");
    }
    {
        // A mismatch still lets a new copy run, but its match does not release.
        Bench bench;
        bench.b.apply_as[0x500f] = 200;
        check(bench.CopyRun().verdict == D810WriteVerdict::Mismatch, "mismatch");
        bench.b.apply_as.clear();
        const auto second = bench.CopyRun();
        check(second.verdict == D810WriteVerdict::Match, "second copy matched");
        check(bench.controller->Block() == D810CaptureBlock::Mismatch, "a later match does not release");
    }
}
void RereadReleases() {
    Bench bench(kAdr38Table, -1, true); // on disk: the block must survive a restart through the file
    bench.b.apply_as[0x5007] = 1100;
    check(bench.CopyRun().verdict == D810WriteVerdict::Mismatch, "mismatch");
    auto rerun = bench.controller->Reread(bench.access, bench.options);
    check(rerun.complete && !rerun.check.released && rerun.block == D810CaptureBlock::Mismatch, "reread finds the difference");
    check(rerun.check.differences == std::vector<std::string>{"aperture"}, "which item");
    check(bench.Writes() == 3, "reread sends no write");
    // The body is brought to A's value by other means; a restart still blocks until a reread.
    bench.b.props[0x5007].current = 800;
    auto restarted = bench.Restarted();
    check(restarted.Block() == D810CaptureBlock::Mismatch, "still blocked after a restart");
    rerun = restarted.Reread(bench.access, bench.options);
    check(rerun.check.released && rerun.block == D810CaptureBlock::None && restarted.CaptureAllowed(), "released by the reread");
    check(bench.Restarted().CaptureAllowed(), "release survives a restart");
    {
        // Unknown becomes a known mismatch after a complete reread.
        Bench other;
        other.b.end_throw.insert(0x500f);
        check(other.CopyRun().verdict == D810WriteVerdict::Unknown, "unknown");
        check(other.controller->Reread(other.access, other.options).block == D810CaptureBlock::Mismatch, "known after reread");
        check(other.Copy().Ok(), "a plan can be made again");
    }
    {
        // An incomplete reread changes nothing.
        Bench other;
        other.b.end_throw.insert(0x500f);
        other.CopyRun();
        other.b.live = 1;
        const auto result = other.controller->Reread(other.access, other.options);
        check(!result.complete && result.block == D810CaptureBlock::Unknown, "incomplete reread");
    }
    {
        // An item one side cannot read is named as unreadable, not as a difference.
        auto a = Body(1667, 800, 64), b = Body(1667, 800, 64);
        b.props.erase(0x5007);
        const auto release = CheckD810Release(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), kAdr38Table);
        check(!release.released && release.differences == std::vector<std::string>{"aperture:unreadable"}, "unreadable on one side");
    }
}
void JournalStates() {
    const auto op = std::string("a0sw1 op=00000000000000aa ");
    check(ParseD810Journal("").block == D810CaptureBlock::None, "empty");
    using B = D810CaptureBlock;
    check(StrongerD810Block(B::None, B::Mismatch) == B::Mismatch && StrongerD810Block(B::Unknown, B::Mismatch) == B::Unknown &&
        StrongerD810Block(B::Unknown, B::Unconfirmed) == B::Unconfirmed && StrongerD810Block(B::Unreadable, B::Unconfirmed) == B::Unreadable, "block strength");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1\n").block == D810CaptureBlock::Unconfirmed, "plan without end");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1\n" + op + "ev=end t=x verdict=not_sent sent=0 reason=x\n").block == D810CaptureBlock::None, "not sent");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1\n" + op + "ev=end t=x verdict=not_sent sent=1 reason=x\n").block == D810CaptureBlock::Unconfirmed, "not_sent with sends is malformed");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1\n" + op + "ev=end t=x verdict=unknown sent=1 reason=x\n").block == D810CaptureBlock::Unknown, "unknown");
    {
        // A plan line with an extra field (the earlier digest) is malformed: it holds capture, it is not ignored.
        const auto extra = ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=end t=x verdict=not_sent sent=0 reason=x\n");
        check(extra.block == D810CaptureBlock::Unconfirmed && extra.malformed_lines >= 1 && extra.operations == 0, "seven plan fields are malformed");
    }
    check(ParseD810Journal("a0sw1 op=00000000000000aa ev=pl").block == D810CaptureBlock::Unconfirmed, "torn line");
    check(ParseD810Journal("a0sw1 op=00000000000000aa ev=pl\na0sw1 ev=reread t=x verdict=match\n").block == D810CaptureBlock::None, "torn line released");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1\n" + op + "ev=end t=x verdict=mismatch sent=1 reason=x\na0sw1 ev=reread t=x verdict=mismatch\n").block == D810CaptureBlock::Mismatch, "mismatch stays");
    // An operation without its end line, then a mismatching reread: the block
    // becomes a known mismatch, it is not dropped.
    const auto open_then_reread = op + "ev=plan t=x source=copy steps=1\n" + op +
        "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040\na0sw1 ev=reread t=x verdict=mismatch\n";
    check(ParseD810Journal(open_then_reread).block == D810CaptureBlock::Mismatch, "unended operation then mismatching reread stays blocked");
    check(ParseD810Journal(open_then_reread + "a0sw1 ev=reread t=x verdict=match\n").block == D810CaptureBlock::None, "a matching reread releases it");
    {
        // A crash during a write, a restart, then a reread that finds A and B
        // still different: capture stays blocked, also after another restart.
        Bench bench(kAdr38Table, -1, true);
        WriteFile(bench.file.Path(), op + "ev=plan t=x source=copy steps=1\n" + op + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040\n");
        auto restarted = bench.Restarted();
        check(restarted.Block() == D810CaptureBlock::Unconfirmed, "restart: unconfirmed");
        const auto reread = restarted.Reread(bench.access, bench.options);
        check(reread.complete && !reread.check.released, "reread finds the difference");
        check(reread.block == D810CaptureBlock::Mismatch && restarted.Block() == D810CaptureBlock::Mismatch && !restarted.CaptureAllowed(), "unconfirmed becomes mismatch, still blocked");
        check(bench.Restarted().Block() == D810CaptureBlock::Mismatch && !bench.Restarted().CaptureAllowed(), "still blocked after another restart");
        check(bench.Writes() == 0, "reread sends no write");
        bench.b = Body(1667, 800, 64);
        auto again = bench.Restarted();
        const auto released = again.Reread(bench.access, bench.options);
        check(released.check.released && released.block == D810CaptureBlock::None && again.CaptureAllowed(), "no difference: released");
        check(bench.Restarted().CaptureAllowed(), "release survives a restart");
    }
    {
        // Crash after the plan line: the next start is blocked as unconfirmed.
        Bench bench(kAdr38Table, -1, true);
        WriteFile(bench.file.Path(), op + "ev=plan t=x source=copy steps=1\n" + op + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040\n");
        check(bench.controller->Block() == D810CaptureBlock::Unconfirmed && !bench.Copy().Ok(), "unconfirmed blocks plans");
        // A torn last line is closed before the next line is appended.
        WriteFile(bench.file.Path(), "a0sw1 op=00000000000000aa ev=pl");
        check(bench.file.Append("a0sw1 ev=reread t=x verdict=match"), "append after torn line");
        check(ParseD810Journal(bench.Journal()).block == D810CaptureBlock::None && ParseD810Journal(bench.Journal()).malformed_lines == 1, "torn line kept separate");
    }
    {
        // A record that cannot be read blocks capture.
        Bench bench(kAdr38Table, -1, true);
        fs::create_directories(bench.file.Path());
        check(bench.controller->Block() == D810CaptureBlock::Unreadable && !bench.controller->CaptureAllowed(), "unreadable record");
        check(!bench.Copy().Ok(), "no plan");
    }
    {
        // The end line cannot be written: unconfirmed, also in memory.
        Bench bench(kAdr38Table, 1 + 3 + 3);
        const auto run = bench.CopyRun();
        check(run.sent == 3 && run.block == D810CaptureBlock::Unconfirmed && !bench.controller->CaptureAllowed(), "end line unwritable");
        check(bench.Restarted().Block() == D810CaptureBlock::Unconfirmed, "disk says unconfirmed");
    }
    {
        // A result line cannot be written: stop, unknown.
        Bench bench(kAdr38Table, 4);
        const auto run = bench.CopyRun();
        check(run.sent == 1 && run.verdict == D810WriteVerdict::Unknown && bench.b.begins_by_code.count(0x500d) == 0, "result line unwritable");
    }
}
void ChosenRuns() {
    {
        Bench bench(kAdr41Table);
        const auto run = bench.Run(bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}, {D810WriteItem::WhiteBalance, D810WriteTarget::Both, 4}}));
        check(run.verdict == D810WriteVerdict::Match && run.sent == 4, "both matched");
        check(bench.a.props[0x500f].current == 200 && bench.b.props[0x500f].current == 200 && bench.a.props[0x5005].current == 4 && bench.b.props[0x5005].current == 4, "values");
        check(!bench.access.overlap && NoResend(bench.a) && NoResend(bench.b), "one at a time, once");
    }
    {
        // A fails: B gets nothing.
        Bench bench(kAdr41Table);
        bench.a.apply_as[0x500f] = 400;
        const auto run = bench.Run(bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}}));
        check(run.verdict == D810WriteVerdict::Mismatch && bench.b.write_begins == 0 && bench.access.opens[1] == 1, "A failed: B read once before, never opened for writing");
        check(!bench.controller->CaptureAllowed(), "blocked");
    }
    {
        // A matches, B fails: only one camera changed, capture blocked.
        Bench bench(kAdr41Table);
        bench.b.end_reject[0x500f] = 0x2002;
        const auto run = bench.Run(bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}}));
        check(run.verdict == D810WriteVerdict::Unknown && bench.a.props[0x500f].current == 200 && run.steps[0].verdict == D810WriteVerdict::Match, "one side only");
        check(!bench.controller->CaptureAllowed() && bench.Restarted().Block() == D810CaptureBlock::Unknown, "blocked across restart");
        // The release needs every block item equal on both, the approved extra items included.
        bench.b.props[0x500f].current = 200;
        bench.b.props[0x5005].current = 5;
        bench.b.props[0x500d].current = 1667;
        bench.b.props[0x5007].current = 800;
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && reread.check.differences == std::vector<std::string>{"white_balance"}, "extra item in the release check");
        // Both on Preset: equal numbers, but the content is not compared, so not released.
        bench.a.props[0x5005].current = 0x8013;
        bench.b.props[0x5005].current = 0x8013;
        const auto preset = bench.controller->Reread(bench.access, bench.options);
        check(!preset.check.released && preset.check.differences == std::vector<std::string>{"white_balance:content_dependent"}, "Preset is unknown, not a match");
    }
}
// White balance and metering through the whole run (test table): the order, the one send, the read back, the block.
void RevisionRuns() {
    const auto index_of = [](const FakeWriteBody& body, const std::string& entry) {
        const auto found = std::find(body.trace.begin(), body.trace.end(), entry);
        return found == body.trace.end() ? -1 : static_cast<int>(found - body.trace.begin());
    };
    {
        Bench bench(kRevisionTable, -1, true);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Match && run.sent == 5 && run.reason.empty(), "revision run: five items matched");
        check(bench.b.props[0x5005].current == 4 && bench.b.props[0x500b].current == 2 && bench.b.props[0x500f].current == 64, "revision run: B now reads A's values");
        check(bench.a.write_begins == 0 && bench.a.write_datas == 0 && bench.a.write_ends == 0, "revision run: A received no write");
        check(bench.b.write_begins == 5 && bench.b.write_datas == 5 && bench.b.write_ends == 5 && NoResend(bench.b) && bench.a.forbidden + bench.b.forbidden == 0, "revision run: one begin, data, END per item");
        const int iso = index_of(bench.b, "1016(500f)"), shutter = index_of(bench.b, "1016(500d)"), aperture = index_of(bench.b, "1016(5007)");
        const int wb = index_of(bench.b, "1016(5005)"), metering = index_of(bench.b, "1016(500b)");
        check(iso >= 0 && iso < shutter && shutter < aperture && aperture < wb && wb < metering, "revision run: the table's order");
        check(bench.b.trace.size() > static_cast<std::size_t>(wb) + 6 &&
            std::vector<std::string>(bench.b.trace.begin() + wb, bench.b.trace.begin() + wb + 6) == std::vector<std::string>{"1016(5005)", "write_data", "write_end", "1014(5005)", "read", "end"},
            "revision run: write, then read back");
        const auto journal = bench.Journal();
        check(Contains(journal, "ev=step i=3 alias=CAM-B item=white_balance code=0x5005 before=0x0002 value=0x0004") &&
            Contains(journal, "ev=step i=4 alias=CAM-B item=metering code=0x500b before=0x0003 value=0x0002"), "revision run: journal steps");
        check(Contains(journal, "ev=result i=3 readback=0x0004 verdict=match") && Contains(journal, "verdict=match sent=5") && !HasIdentifierShape(journal), "revision run: journal results");
        check(bench.controller->CaptureAllowed(), "revision run: capture allowed");
        check(bench.Restarted().CaptureAllowed(), "revision run: nothing held after a restart");
    }
    {
        Bench bench(kRevisionTable);
        bench.b = Body(1667, 800, 64);
        bench.a.props[0x5005].current = 0x8010;
        bench.a.props[0x500b].current = 1;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Match && run.sent == 2 && bench.Writes() == 2 && bench.b.props[0x5005].current == 0x8010 && bench.b.props[0x500b].current == 1, "revision run: the two alone");
    }
    {
        // The readback of the white balance differs: the rest is never sent, capture is held across a restart.
        Bench bench(kRevisionTable, -1, true);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        bench.b.apply_as[0x5005] = 5;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Mismatch && run.sent == 4, "white balance mismatch: stops after it");
        check(bench.b.begins_by_code.count(0x500b) == 0 && NoResend(bench.b), "white balance mismatch: metering never sent, nothing resent");
        check(!bench.controller->CaptureAllowed() && bench.controller->Block() == D810CaptureBlock::Mismatch && bench.Restarted().Block() == D810CaptureBlock::Mismatch, "white balance mismatch: capture held");
        check(Contains(bench.Journal(), "ev=result i=3 readback=0x0005 verdict=mismatch"), "white balance mismatch: recorded");
        // The reread compares seven items; it releases only when all read the same.
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && reread.check.differences == std::vector<std::string>{"white_balance", "metering"}, "reread: white balance and metering differ");
        bench.b.props[0x5005].current = 4;
        bench.b.props[0x500b].current = 2;
        const auto again = bench.controller->Reread(bench.access, bench.options);
        check(again.check.released && bench.controller->CaptureAllowed() && bench.Restarted().CaptureAllowed(), "reread: seven items equal, released");
    }
    {
        // Metering refused at its END: unknown, held.
        Bench bench(kRevisionTable);
        bench.a.props[0x500b].current = 2;
        bench.b.end_reject[0x500b] = 0x2005;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Unknown && run.sent == 4 && bench.controller->Block() == D810CaptureBlock::Unknown, "metering END refused: unknown, held");
    }
    {
        // The write of the white balance is refused at its begin: unknown, metering never sent.
        Bench bench(kRevisionTable);
        bench.a.props[0x5005].current = 4;
        bench.a.props[0x500b].current = 2;
        bench.b.begin_reject.insert(0x5005);
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Unknown && run.sent == 4 && bench.b.begins_by_code.count(0x500b) == 0 && !bench.controller->CaptureAllowed(), "white balance begin refused: unknown, stops");
    }
    {
        // The body changes A's white balance to a content-dependent kind after the plan: nothing is sent.
        Bench bench(kRevisionTable);
        bench.a.props[0x5005].current = 4;
        const auto plan = bench.Copy();
        bench.a.props[0x5005].current = 0x8013;
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::NotSent && run.reason.rfind("plan_changed:CAM-A", 0) == 0 && bench.Writes() == 0, "A's white balance changed after the plan: nothing sent");
    }
    {
        // The product-shaped table: a white balance difference does not hold capture, and a Preset on A does not hold a release.
        Bench bench(kAdr38Table);
        bench.a.props[0x5005].current = 0x8013;
        bench.a.props[0x500b].current = 4;
        bench.b.apply_as[0x5007] = 1100;
        check(bench.CopyRun().verdict == D810WriteVerdict::Mismatch, "disabled rows: a mismatch on the aperture");
        bench.b.apply_as.clear();
        bench.b.props[0x5007].current = 800;
        bench.b.props[0x500f].current = 64;
        bench.b.props[0x500d].current = 1667;
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released && bench.controller->CaptureAllowed(), "disabled rows: Preset on A and a metering difference do not hold the release");
    }
    {
        // Enabled rows, both on Preset after a failure: equal numbers are not equal whites, so the release stays held.
        // ※ Owner decision pending (ADR-0038 revision): this pins the fail-closed behavior that exists today.
        Bench bench(kRevisionTable);
        bench.b.end_throw.insert(0x500f);
        check(bench.CopyRun().verdict == D810WriteVerdict::Unknown, "preset release: unknown first");
        bench.b = Body(1667, 800, 64);
        bench.a.props[0x5005].current = 0x8013;
        bench.b.props[0x5005].current = 0x8013;
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && reread.check.differences == std::vector<std::string>{"white_balance:content_dependent"} && !bench.controller->CaptureAllowed(), "preset release: held, not a match");
    }
}
void StopsBeforeSending() {
    {
        // Live turned on after the verification read: the in-session read stops the write.
        Bench bench;
        const auto plan = bench.Copy();
        check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
        int reads = 0;
        bench.b.after = [&](const std::string& entry) { if (entry == "1015(d1a2)" && ++reads == 2) bench.b.live = 1; };
        const auto run = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
        check(run.sent == 0 && run.reason == "live_view_on" && bench.Writes() == 0, "Live on at the write");
    }
    {
        Bench bench;
        bench.left = std::chrono::milliseconds(1000);
        const auto run = bench.CopyRun();
        check(run.sent == 0 && run.reason == "deadline_margin" && bench.Writes() == 0, "not enough time left");
    }
    for (const int missing : {0, 1, 2}) {
        // The time left is required (and the budget must be positive): without
        // it nothing is read and nothing is sent.
        Bench bench;
        if (missing == 0) bench.options.remaining = nullptr;
        if (missing == 1) bench.options.step_budget = std::chrono::milliseconds(0);
        if (missing == 2) bench.options.step_budget = std::chrono::milliseconds(-1);
        const auto plan = bench.Copy();
        const auto reads = bench.a.read_begins + bench.b.read_begins;
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && run.reason == "invalid_options", "no time left given: refused");
        check(bench.Writes() == 0 && bench.a.read_begins + bench.b.read_begins == reads && bench.controller->CaptureAllowed(), "not even read, no block");
    }
    {
        // The margin at its boundary: three steps of 5000 ms need 15000 ms.
        Bench exact;
        exact.left = std::chrono::milliseconds(15000);
        check(exact.CopyRun().sent == 3, "margin: exactly steps x budget passes");
        Bench short_by_one;
        short_by_one.left = std::chrono::milliseconds(14999);
        const auto run = short_by_one.CopyRun();
        check(run.sent == 0 && run.reason == "deadline_margin" && short_by_one.Writes() == 0, "margin: one ms short stops");
    }
    {
        // A budget so large that budget x steps would wrap round to a small
        // number (3 x 0x5555555555555556 is 2 in 64 bits): it must stop, not write.
        Bench bench;
        bench.options.step_budget = std::chrono::milliseconds(0x5555555555555556LL);
        const auto run = bench.CopyRun();
        check(run.sent == 0 && run.reason == "deadline_margin" && bench.Writes() == 0, "a huge budget stops, it does not wrap");
    }
    {
        // The largest budget with two steps (2 x max wraps to -2).
        Bench bench(Adr38Only({D810WriteItem::Iso, D810WriteItem::Aperture}));
        bench.options.step_budget = std::chrono::milliseconds::max();
        const auto plan = bench.Copy();
        check(plan.Ok() && plan.plan->steps.size() == 2, "two steps");
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason == "deadline_margin" && bench.Writes() == 0, "the largest budget stops, it does not wrap");
    }
    {
        // The time left drops while the step's reads run: the check right
        // before the begin stops it.
        Bench bench;
        const auto plan = bench.Copy();
        check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
        int seen = 0;
        bench.b.after = [&](const std::string& entry) { if (entry == "1014(500f)" && ++seen == 2) bench.left = std::chrono::milliseconds(1000); };
        const auto run = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
        check(seen == 2 && run.sent == 0 && run.reason == "deadline_margin" && bench.Writes() == 0, "margin checked again after the reads");
    }
    {
        // The stop flag during the first write: its read back is not made, the
        // rest is not sent, the result is unknown.
        Bench bench;
        bench.b.after = [&](const std::string& entry) { if (entry == "write_end") bench.go = false; };
        const auto run = bench.CopyRun();
        check(run.sent == 1 && run.verdict == D810WriteVerdict::Unknown && run.reason == "result_unknown", "stopped during one");
        check(bench.b.write_ends == 1 && !bench.controller->CaptureAllowed(), "the END was still sent; blocked");
    }
    {
        // Stop before the run: nothing at all.
        Bench bench;
        const auto plan = bench.Copy();
        bench.go = false;
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason == "reread_incomplete:CAM-A" && bench.controller->CaptureAllowed(), "stopped before");
    }
    {
        Bench bench;
        check(bench.owner.TryAcquire(D810CameraUse::Live), "Live owns the cameras");
        const auto plan = bench.Copy();
        const auto reads = bench.a.read_begins;
        const auto run = bench.Run(plan);
        check(run.reason == "cameras_busy" && bench.Writes() == 0 && bench.a.read_begins == reads, "busy: not even read");
        check(bench.owner.Current() == D810CameraUse::Live, "owner unchanged");
    }
    {
        Bench bench;
        bench.CopyRun();
        check(bench.owner.Current() == D810CameraUse::None, "owner released after the run");
    }
    {
        Bench bench;
        bench.access.open_fail[1] = true;
        check(bench.CopyRun().sent == 0 && bench.Writes() == 0, "B not opened");
    }
    {
        // Close fails after the writes matched: not trusted, unknown.
        Bench bench;
        const auto plan = bench.Copy();
        check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
        bench.b.after = [&](const std::string& entry) { if (entry == "1016(500f)") bench.access.close_ok[1] = false; };
        const auto run = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
        check(run.verdict == D810WriteVerdict::Unknown && run.sent == 3, "close failure");
    }
}
// The I/O watchdog (ADR-0038 decision 10, M3): its thread only raises the
// flag; the session sees it on the I/O thread, sends nothing more (not even
// the END of the open transfer) and the result is unknown.
void WatchdogCancel() {
    {
        Bench bench;
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        bench.access.cancel[1] = flag;
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false, released = false;
        // WRITE_DATA hangs in the transport until the "watchdog" has acted.
        bench.b.after = [&](const std::string& entry) {
            if (entry != "write_data") return;
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return released; });
        };
        std::thread watchdog([&] {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return entered; });
            flag->store(true); // all the watchdog does, besides IPortableDevice::Cancel
            released = true;
            wake.notify_all();
        });
        const auto run = bench.CopyRun();
        watchdog.join();
        const auto& trace = bench.b.trace;
        const auto data = std::find(trace.begin(), trace.end(), "write_data");
        check(data != trace.end() && data + 1 == trace.end(), "cancel: nothing after the stuck WRITE_DATA, not even its END");
        check(bench.access.after_cancel[1] == 0 && bench.b.write_ends == 0 && bench.b.write_datas == 1, "cancel: no request reached the body after the flag");
        check(run.sent == 1 && run.verdict == D810WriteVerdict::Unknown && run.reason == "result_unknown", "cancel: the result is unknown");
        check(bench.b.begins_by_code.count(0x500d) == 0 && NoResend(bench.b), "cancel: the rest is not sent, nothing resent");
        check(!bench.controller->CaptureAllowed() && bench.controller->Block() == D810CaptureBlock::Unknown, "cancel: capture held");
        check(bench.Restarted().Block() == D810CaptureBlock::Unknown, "cancel: held after a restart");
    }
    {
        // The flag raised during a read in the write session, before the first
        // write: nothing is sent and nothing more reaches the body.
        Bench bench;
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        bench.access.cancel[1] = flag;
        int seen = 0;
        // 1: the plan's read, 2: the verification read, 3: the write session.
        bench.b.after = [&](const std::string& entry) { if (entry == "1015(d1a2)" && ++seen == 3) flag->store(true); };
        const auto run = bench.CopyRun();
        check(seen == 3 && !bench.b.trace.empty() && bench.b.trace.back() == "1015(d1a2)", "cancel in a read: that request is the last, no END");
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && bench.Writes() == 0 && bench.access.after_cancel[1] == 0, "cancel in a read: nothing sent");
        check(bench.controller->CaptureAllowed(), "cancel in a read: nothing sent, no block");
    }
}
// A read in the write session answered as a real body answered stage A: begin
// accepted, 0 bytes, END refused with 0x200A.
void ZeroByteReadsInTheWriteSession() {
    {
        // The stop falls on the last row of the read set: no row is left to report "not read", yet the session is
        // spent, so the read is not complete and no plan is made from it.
        Bench bench;
        const auto without = bench.controller->ReadForPlan(bench.access, bench.options);
        check(without.complete, "last row control: a read of a healthy body is complete");
        Bench stopped;
        stopped.b.zero_at[0xd14e] = 1;
        const auto read = stopped.controller->ReadForPlan(stopped.access, stopped.options);
        check(stopped.b.reads_of[0xd14e] == 1 && !read.complete && read.reason == "read_incomplete:CAM-B", "last row: stopped there, the read is not complete");
        check(stopped.Writes() == 0 && stopped.controller->CaptureAllowed(), "last row: nothing written, no block");
    }
    {
        // Before the first write: not sent, nothing written, no block.
        Bench bench;
        bench.b.zero_at[0x500f] = 3; // 1: the plan's read, 2: the verification read, 3: the read before the write
        const auto run = bench.CopyRun();
        check(bench.b.reads_of[0x500f] == 3, "zero read before the write happened");
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && run.reason == "desc_unreadable" && bench.Writes() == 0, "zero read before the write: not sent");
        check(bench.controller->CaptureAllowed(), "zero read before the write: no block");
    }
    {
        // At the read back: unknown, capture held, nothing after.
        Bench bench;
        bench.b.zero_at[0x500f] = 4; // 4: the read back after the write
        const auto run = bench.CopyRun();
        check(run.sent == 1 && run.verdict == D810WriteVerdict::Unknown && run.reason == "result_unknown" && bench.b.write_begins == 1, "zero read back: unknown");
        check(run.steps[0].verdict == D810WriteVerdict::Unknown && !run.steps[0].readback, "zero read back: no value read back");
        check(!bench.controller->CaptureAllowed() && bench.Restarted().Block() == D810CaptureBlock::Unknown, "zero read back: capture held");
    }
    {
        // The operation latch drops: CAM-A's read before its first write fails,
        // so CAM-B is never opened for writing (an ADR-0041 table, both cameras).
        Bench bench(kAdr41Table);
        bench.a.zero_at[0x500f] = 3; // 1: the plan's read, 2: the verification read, 3: the read before the write
        const auto plan = bench.Chosen({{D810WriteItem::Iso, D810WriteTarget::Both, 200}});
        check(plan.Ok() && plan.plan->Cameras().size() == 2, "a plan for both");
        const auto run = bench.Run(plan);
        check(bench.a.reads_of[0x500f] == 3, "zero read on A happened");
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && bench.Writes() == 0, "zero read on A: not sent");
        check(bench.access.opens[1] == 1 && bench.b.write_begins == 0, "the latch is down: B opened for the verification read only");
    }
}
// A read-only session (the read for a plan, the verification read of a run, the reread) does not trust its
// answers once the watchdog's flag is up, even when the flag comes with the very last answer.
void ReadOnlySessionsAfterCancel() {
    FakeWriteBody probe = Body(333, 560, 100);
    WritePathSnapshot(probe, "CAM-B"); // what the write path's reads send, request by request
    check(!probe.trace.empty(), "probe: a read sends requests");
    const std::string last = probe.trace.back();
    const int per_read = static_cast<int>(probe.Count(last));
    // The answer to the last request of the next read of CAM-B raises the flag.
    const auto arm = [&](Bench& bench, const std::shared_ptr<std::atomic<bool>>& flag) {
        auto seen = std::make_shared<int>(0);
        bench.access.cancel[1] = flag;
        bench.b.after = [flag, seen, last, per_read](const std::string& entry) { if (entry == last && ++*seen == per_read) flag->store(true); };
    };
    // A write left unknown, and the bodies equal again: a reread would release the block.
    const auto held = [](Bench& bench) {
        bench.b.end_reject[0x500d] = 0x2005;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Unknown && !bench.controller->CaptureAllowed(), "reread set up: a write left unknown");
        bench.b.end_reject.clear();
        for (const auto& [code, prop] : bench.a.props) bench.b.props[code].current = prop.current;
    };
    {
        Bench bench;
        held(bench);
        const auto result = bench.controller->Reread(bench.access, bench.options);
        check(result.complete && result.check.released && bench.controller->CaptureAllowed(), "reread control: equal bodies release the block");
    }
    {
        Bench bench;
        held(bench);
        const int writes = bench.Writes();
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        arm(bench, flag);
        const auto result = bench.controller->Reread(bench.access, bench.options);
        check(flag->load() && bench.b.trace.back() == last, "reread: the flag came with the last answer, nothing was sent after it");
        check(!result.complete && result.reason == "reread_incomplete" && !result.check.released, "reread: not complete after the flag");
        check(bench.access.after_cancel[1] == 0 && bench.Writes() == writes, "reread: no request reached the body after the flag, no write");
        check(bench.controller->Block() == D810CaptureBlock::Unknown && bench.Restarted().Block() == D810CaptureBlock::Unknown, "reread: the block is not changed");
    }
    {
        Bench bench;
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        arm(bench, flag);
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(flag->load() && bench.b.trace.back() == last, "read for a plan: the flag came with the last answer");
        check(!read.complete && read.reason == "read_incomplete:CAM-B", "read for a plan: not complete after the flag");
        check(bench.access.after_cancel[1] == 0 && bench.Writes() == 0, "read for a plan: nothing after the flag, no write");
    }
    {
        Bench bench;
        const auto plan = bench.Copy(); // CAM-B is read directly here, not through the access
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        arm(bench, flag);
        const auto run = bench.Run(plan);
        check(flag->load() && bench.b.trace.back() == last, "verification read: the flag came with the last answer");
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && run.reason == "reread_incomplete:CAM-B", "verification read: not complete after the flag, nothing sent");
        check(bench.Writes() == 0 && bench.access.after_cancel[1] == 0 && bench.controller->CaptureAllowed(), "verification read: nothing after the flag, no block");
    }
}
// Execute runs the plan that was confirmed and no other.
void ExecuteChecksThePlanNumber() {
    Bench bench;
    const auto plan = bench.Copy();
    check(plan.Ok() && bench.controller->Confirm(plan.plan->id) == nullptr, "execute: confirmed");
    const auto other = bench.controller->Execute(plan.plan->id ^ 1, bench.access, bench.options);
    check(other.reason == "plan_not_confirmed" && other.sent == 0 && other.verdict == D810WriteVerdict::NotSent, "execute: another number is refused");
    check(bench.Writes() == 0 && bench.access.opens[0] + bench.access.opens[1] == 0, "execute: another number opens nothing and writes nothing");
    const auto own = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
    check(own.verdict == D810WriteVerdict::Match && own.sent == 3 && bench.Writes() == 3, "execute: the confirmed number still runs the plan, once");
    const auto again = bench.controller->Execute(plan.plan->id, bench.access, bench.options);
    check(again.reason == "plan_not_confirmed" && bench.Writes() == 3, "execute: and not twice");
}
// The time margin for the steps left against every budget, remaining time and step count around the 64-bit wrap,
// compared with the exact product (budget x steps), not with a product that may wrap.
void BudgetSweep() {
    using ms = std::chrono::milliseconds;
    const std::vector<std::vector<D810WriteItem>> subsets = {{D810WriteItem::Iso}, {D810WriteItem::Iso, D810WriteItem::Aperture},
        {D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}};
    const std::vector<long long> budgets = {1, 4999, 5000, 5001, 20000, 20001, 60000, 60001, 0x1fffffffffffffffLL, 0x2aaaaaaaaaaaaaaaLL, 0x2aaaaaaaaaaaaaabLL,
        0x3fffffffffffffffLL, 0x5555555555555555LL, 0x5555555555555556LL, 0x7fffffffffffffffLL};
    const std::vector<long long> remainings = {0, 1, 59999, 60000, 60001, std::numeric_limits<long long>::max(), -1, std::numeric_limits<long long>::min()};
    int cases = 0, wrong = 0, sent_all = 0;
    for (const auto& items : subsets)
        for (const auto budget : budgets)
            for (const auto remaining : remainings) {
                auto table = kAdr38Table;
                for (auto& row : table.rows) row.enabled = row.camera == D810Camera::B && std::find(items.begin(), items.end(), row.item) != items.end();
                Bench bench(table);
                bench.options.step_budget = ms(budget);
                bench.left = ms(remaining);
                const auto plan = bench.Copy();
                if (!plan.Ok()) { ++wrong; continue; }
                const auto steps = static_cast<unsigned long long>(plan.plan->steps.size());
                unsigned long long high = 0;
                const auto low = _umul128(static_cast<unsigned long long>(budget), steps, &high);
                const bool enough = remaining >= 0 && high == 0 && low <= static_cast<unsigned long long>(remaining);
                const auto run = bench.Run(plan);
                ++cases;
                if (enough) {
                    ++sent_all;
                    if (run.sent != static_cast<int>(steps)) ++wrong;
                } else if (run.sent != 0 || bench.Writes() != 0 || run.reason != "deadline_margin") ++wrong;
            }
    check(cases == 360 && wrong == 0, "budget sweep: every case as the exact product says");
    check(sent_all > 0 && sent_all < cases, "budget sweep: some cases send everything and some send nothing");
}
// --- The screen (D810SettingsWriteScreen), on a worker thread, with the tables of this target -----------------------
struct BorrowedAccess final: D810SettingsCameraAccess {
    explicit BorrowedAccess(D810SettingsCameraAccess& inner): inner_(inner) {}
    D810OpenedCamera Open(D810Camera camera) override { return inner_.Open(camera); }
    bool Close(D810Camera camera) override { return inner_.Close(camera); }
private:
    D810SettingsCameraAccess& inner_;
};
struct ScreenBench {
    Bench bench;
    std::vector<std::string> log;
    std::unique_ptr<D810SettingsWriteScreen> screen; // after bench: it is gone (and its worker joined) first
    explicit ScreenBench(const D810WriteTable& table = kAdr38Table, bool on_disk = false): bench(table, -1, on_disk) { Make(); }
    explicit ScreenBench(ProductEntry entry): bench(entry) { Make(); }
    void Make() {
        D810SettingsWriteScreen::Options options;
        options.log = [this](const std::string& line) { log.push_back(line); };
        options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
        screen = std::make_unique<D810SettingsWriteScreen>(*bench.controller, [this] { return std::make_unique<BorrowedAccess>(bench.access); }, options);
    }
    bool WaitIdle() {
        for (int i = 0; i < 4000; ++i) {
            screen->Poll();
            if (!screen->Busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool Copy() { return screen->RequestCopy(true) && WaitIdle(); }
    bool Confirm() { return screen->ConfirmShownPlan(true) && WaitIdle(); }
    bool Reread() { return screen->RequestReread(true) && WaitIdle(); }
};
bool Shows(const D810WriteScreenView* view, const std::wstring& part) {
    if (!view) return false;
    if (view->title.find(part) != std::wstring::npos) return true;
    return std::any_of(view->lines.begin(), view->lines.end(), [&](const auto& line) { return line.find(part) != std::wstring::npos; });
}
void ScreenCopyFlow() {
    ScreenBench s;
    auto& screen = *s.screen;
    check(screen.CopyOffered(true) && !screen.CopyOffered(false) && screen.Step() == D810WriteScreenStep::Idle, "screen: copy offered when the preview is ready");
    // Copy: both read (A closed before B), the plan shown, nothing written, nothing confirmed.
    check(s.Copy(), "screen: copy read");
    check(screen.Step() == D810WriteScreenStep::Confirming && screen.View() && screen.View()->confirmation, "screen: the confirmation is shown");
    check(s.bench.Writes() == 0 && s.bench.access.opens[0] == 1 && s.bench.access.opens[1] == 1 && !s.bench.access.overlap, "screen: read only, one at a time");
    check(screen.Status().plan_pending && !screen.Status().plan_confirmed, "screen: the plan waits for the confirm button");
    const auto* view = screen.View();
    check(Shows(view, L"ISO感度: CAM-B の今の値 100 → 書く値 64"), "screen: item, B's value now, the value to write");
    check(Shows(view, L"シャッター速度: CAM-B の今の値 1/30 s → 書く値 1/6 s") && Shows(view, L"絞り: CAM-B の今の値 F5.6 → 書く値 F8"), "screen: every step");
    check(Shows(view, L"自動では元に戻りません") && Shows(view, L"1 回だけ") && Shows(view, L"再送しません"), "screen: no undo, once, no resend");
    check(screen.ConfirmOffered(true) && !screen.ConfirmOffered(false) && !screen.CopyOffered(true), "screen: only confirm or discard now");
    // The confirm button: written once, read back, matched.
    check(s.Confirm(), "screen: confirmed and run");
    check(screen.Step() == D810WriteScreenStep::Idle && Shows(screen.View(), L"一致") && s.bench.Writes() == 3 && NoResend(s.bench.b), "screen: matched");
    check(s.bench.b.props[0x500f].current == 64 && s.bench.a.write_begins == 0, "screen: B now reads A's value, A untouched");
    check(screen.AdmitCapture() && screen.Status().capture_allowed, "screen: capture admitted after a match");
    check(!screen.ConfirmShownPlan(true) && s.bench.Writes() == 3, "screen: the plan was used once");
    const bool logged = std::any_of(s.log.begin(), s.log.end(), [](const auto& line) { return line.find("settings_write_ui_result verdict=match sent=3") != std::string::npos; });
    check(logged && std::none_of(s.log.begin(), s.log.end(), [](const auto& line) { return HasIdentifierShape(line); }), "screen: log without identifiers");
}
void ScreenRefusals() {
    {
        // The preview is not ready (Live, a capture, a read): nothing is read.
        ScreenBench s;
        check(!s.screen->RequestCopy(false) && !s.screen->RequestReread(false) && s.bench.access.opens[0] + s.bench.access.opens[1] == 0, "screen: not ready");
    }
    {
        // A table with no enabled row: copy cannot be pressed, and the reason says why. The product table has a row, so
        // this text is not reached by it (next block).
        auto none = kAdr38Table;
        for (auto& row : none.rows) row.enabled = false;
        ScreenBench s(none);
        check(s.screen->Status().enabled_rows == 0 && !s.screen->CopyOffered(true) && !s.screen->RequestCopy(true), "screen: no enabled row, no copy");
        const auto text = s.screen->OfferText(true);
        check(text.find(L"押せません") != std::wstring::npos && text.find(L"番号（code）") != std::wstring::npos && text.find(L"確かめていない") != std::wstring::npos,
            "screen: the reason names the unconfirmed codes and body");
        check(s.bench.access.opens[0] + s.bench.access.opens[1] == 0 && s.bench.Writes() == 0, "screen: no enabled row, nothing opened");
    }
    {
        // The product table has one row: copy is offered, and the no-row reason is not the text shown.
        ScreenBench s(kProduct);
        check(s.screen->Status().enabled_rows == 1 && s.screen->CopyOffered(true) && !s.screen->CopyOffered(false), "screen: product table, copy offered when ready");
        const auto text = s.screen->OfferText(true);
        check(text.find(L"書ける項目がありません") == std::wstring::npos && text.find(L"番号（code）") == std::wstring::npos, "screen: product table, not the no-row reason");
        check(s.bench.access.opens[0] + s.bench.access.opens[1] == 0 && s.bench.Writes() == 0, "screen: product table, nothing opened by looking");
    }
    {
        // Discard: nothing written, the plan gone.
        ScreenBench s;
        check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Confirming, "screen: plan shown");
        s.screen->DiscardPlan();
        check(s.screen->Step() == D810WriteScreenStep::Idle && !s.bench.controller->Status().plan_pending && Shows(s.screen->View(), L"やめました"), "screen: discarded");
        check(!s.screen->ConfirmShownPlan(true) && s.bench.Writes() == 0, "screen: nothing to confirm after a discard");
    }
    {
        // A Live restart or a new read clears a confirmation that was shown.
        ScreenBench s;
        check(s.Copy(), "screen: plan shown");
        s.screen->ClearView();
        check(!s.screen->View() && !s.bench.controller->Status().plan_pending && !s.screen->ConfirmShownPlan(true) && s.bench.Writes() == 0, "screen: cleared");
    }
    {
        // CAM-A not in M: nothing to confirm, the operator is told what to do.
        ScreenBench s;
        s.bench.a.props[0x500e].current = 3;
        check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Idle && Shows(s.screen->View(), L"写せません") && Shows(s.screen->View(), L"A の本体で M に"),
            "screen: A not in M");
        check(s.bench.Writes() == 0 && !s.bench.controller->Status().plan_pending, "screen: nothing planned");
    }
    {
        // No difference: nothing to write.
        ScreenBench s;
        s.bench.b = Body(1667, 800, 64);
        check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Idle && Shows(s.screen->View(), L"書くものはありません") && s.bench.Writes() == 0, "screen: no difference");
    }
    {
        // A camera that does not open (the identity does not match): nothing written.
        ScreenBench s;
        s.bench.access.open_fail[1] = true;
        check(s.Copy() && Shows(s.screen->View(), L"読めませんでした") && s.bench.Writes() == 0, "screen: B not opened");
    }
}
void ScreenSkippedShown() {
    // A row the table does not enable is listed as left out, with the reason, before confirming.
    ScreenBench s(Adr38Only({D810WriteItem::ExposureMode, D810WriteItem::IsoAuto, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}));
    check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Confirming, "screen: plan with a row left out");
    check(Shows(s.screen->View(), L"外した項目") && Shows(s.screen->View(), L"ISO感度: 表で無効の項目です"), "screen: skipped and why");
    check(!Shows(s.screen->View(), L"ISO感度: CAM-B の今の値"), "screen: the skipped item is not a step");
    check(s.Confirm() && s.bench.b.props[0x500f].current == 100 && s.bench.Writes() == 2, "screen: the skipped item was not written");
}
void ScreenBlockAndReread() {
    ScreenBench s(kAdr38Table, true); // the record on disk
    s.bench.b.apply_as[0x5007] = 1100;
    check(s.Copy() && s.Confirm(), "screen: run");
    check(Shows(s.screen->View(), L"不一致") && !s.screen->AdmitCapture() && s.screen->CaptureRefusalText().find(L"不一致") != std::wstring::npos, "screen: mismatch holds capture");
    check(s.screen->CopyOffered(true) && s.screen->RereadOffered(true), "screen: after a mismatch a new copy or a reread may be made");
    check(s.Reread() && Shows(s.screen->View(), L"違う項目") && Shows(s.screen->View(), L"絞り") && !s.screen->AdmitCapture(), "screen: reread finds the difference, still held");
    check(s.bench.Writes() == 3, "screen: reread writes nothing");
    s.bench.b.props[0x5007].current = 800;
    check(s.Reread() && Shows(s.screen->View(), L"同じです") && s.screen->AdmitCapture(), "screen: equal on both: released");
    check(s.bench.Restarted().CaptureAllowed(), "screen: released also after a restart");
}
void ScreenPartialFailure() {
    // The first item matches, the second is refused at its END: unknown, the third never sent, capture held.
    ScreenBench s;
    s.bench.b.end_reject[0x500d] = 0x2005;
    check(s.Copy() && s.Confirm(), "screen: run");
    check(Shows(s.screen->View(), L"不明") && Shows(s.screen->View(), L"ISO感度: CAM-B の今の値 100 → 書く値 64 / 結果: 一致"), "screen: per item results");
    check(Shows(s.screen->View(), L"シャッター速度: CAM-B の今の値 1/30 s → 書く値 1/6 s / 結果: 不明"), "screen: the failed item is unknown");
    check(s.bench.b.begins_by_code.count(0x5007) == 0 && NoResend(s.bench.b), "screen: the rest not sent");
    check(!s.screen->AdmitCapture() && !s.screen->CopyOffered(true) && s.screen->RereadOffered(true), "screen: unknown: only the reread is offered");
}
void ScreenCancelAndStop() {
    {
        // The watchdog's flag during WRITE_DATA, from another thread: unknown, no END, held.
        ScreenBench s;
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        s.bench.access.cancel[1] = flag;
        s.bench.b.after = [flag](const std::string& entry) { if (entry == "write_data") flag->store(true); };
        check(s.Copy() && s.Confirm(), "screen: run");
        check(s.bench.b.write_ends == 0 && s.bench.access.after_cancel[1] == 0 && Shows(s.screen->View(), L"不明") && !s.screen->AdmitCapture(), "screen: cancel: unknown, held");
    }
    {
        // The window closes while the write runs: the stop flag ends it after the open write; capture held,
        // no capture admitted and no other operation offered while it runs.
        ScreenBench s;
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false, released = false;
        s.bench.b.after = [&](const std::string& entry) {
            if (entry != "write_end") return;
            std::unique_lock lock(mutex);
            if (entered) return;
            entered = true;
            wake.notify_all();
            wake.wait(lock, [&] { return released; });
        };
        check(s.Copy() && s.screen->ConfirmShownPlan(true), "screen: run started");
        {
            std::unique_lock lock(mutex);
            check(wake.wait_for(lock, std::chrono::seconds(30), [&] { return entered; }), "screen: the write reached its END");
        }
        check(s.screen->Busy() && !s.screen->AdmitCapture() && !s.screen->CopyOffered(true) && !s.screen->RereadOffered(true), "screen: busy: nothing else");
        check(!s.screen->CaptureRefusalText().empty(), "screen: busy: the capture refusal says why");
        s.screen->Stop();
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        wake.notify_all();
        check(s.WaitIdle(), "screen: the worker ended");
        check(s.bench.Writes() == 1 && s.bench.b.write_ends == 1 && Shows(s.screen->View(), L"不明") && !s.screen->AdmitCapture(), "screen: stopped after the open write");
    }
}
// The confirmation: the two notices come first (a window too small for every line cuts the end), the confirm
// button's lines must all fit, and the line above the buttons repeats that nothing is undone.
void ScreenConfirmationOrder() {
    ScreenBench s;
    check(!s.screen->ConfirmationShown(), "confirmation: none before a copy");
    check(s.Copy() && s.screen->ConfirmationShown(), "confirmation: shown after a copy");
    const auto* view = s.screen->View();
    check(view && view->lines.size() == 7, "confirmation: two notices, the header, three items and the closing line");
    if (view && view->lines.size() >= 3) {
        check(view->lines[0].find(L"自動では元に戻りません") == 0, "confirmation: the first line is that nothing is undone");
        check(view->lines[1].find(L"項目ごとに 1 回だけです") != std::wstring::npos && view->lines[1].find(L"再送しません") != std::wstring::npos, "confirmation: the second is once, no resend");
        check(view->lines[2].find(L"項目:") == 0, "confirmation: the items follow the notices");
    }
    check(s.screen->OfferText(true).find(L"自動では元に戻りません") != std::wstring::npos, "confirmation: the line above the buttons says it too");
    check(D810WriteViewVisibleLines(100) == 0 && D810WriteViewVisibleLines(700) == 6 && D810WriteViewVisibleLines(960) == 12, "confirmation: lines that fit");
    check(view && D810WriteViewFits(*view, 960) && !D810WriteViewFits(*view, 700), "confirmation: fits a 960 px client, not a 700 px one");
    s.screen->DiscardPlan();
    check(!s.screen->ConfirmationShown(), "confirmation: gone after the plan is discarded");
    check(s.Copy() && s.Confirm() && !s.screen->ConfirmationShown(), "confirmation: gone after the run");
}
// The smallest client height (px) at which every line of `view` fits, scanning the geometry that D810WriteViewVisibleLines documents:
// the lines start 30 px under the title (the middle of the window plus 88 px) and end 84 px above the bottom, 22 px each.
int SmallestClientThatFits(const D810WriteScreenView& view) {
    for (int bottom = 0; bottom <= 4000; ++bottom)
        if (D810WriteViewFits(view, bottom)) {
            for (int more = bottom; more <= 4000; ++more)
                if (!D810WriteViewFits(view, more)) return -1; // must stay true above the boundary
            return bottom;
        }
    return -1;
}
// A line count n fits when (client - 84) - (client / 2 + 118) >= 22 n.
int ExpectedSmallestClient(std::size_t lines) {
    for (int bottom = 0; bottom <= 4000; ++bottom)
        if ((bottom - 84) - (bottom / 2 + 118) >= 22 * static_cast<int>(lines)) return bottom;
    return -1;
}
// Whether the confirm button may be pressed (D810ConfirmPressable): a confirmation is presented, the last present drew
// this one whole, every line fits the height and the button is inside the window. Anything else is not pressable.
void ScreenConfirmPressable() {
    ScreenBench s;
    const auto& screen = *s.screen;
    check(!D810ConfirmPressable(screen.View(), screen.ViewSerial(), screen.ViewSerial(), 1400, 960), "pressable: no view presented");
    check(!D810ConfirmPressable(nullptr, 1, 1, 1400, 960), "pressable: no view");
    check(s.Copy() && screen.ConfirmationShown(), "pressable: a confirmation is up");
    const auto* view = screen.View();
    const auto serial = screen.ViewSerial();
    check(view && serial != 0, "pressable: the view has a serial");
    if (!view) return;
    // Presented, drawn whole, fits, button inside: pressable.
    check(D810ConfirmPressable(view, serial, serial, 1400, 960), "pressable: presented, drawn, fits");
    // Presented and fits but not drawn: a serial of nothing, of an earlier view, or of a later one.
    check(!D810ConfirmPressable(view, serial, 0, 1400, 960), "not drawn: serial 0 (a paint that drew nothing, a failed present)");
    check(!D810ConfirmPressable(view, serial, serial - 1, 1400, 960) && !D810ConfirmPressable(view, serial, serial + 1, 1400, 960), "not drawn: another view's serial");
    check(!D810ConfirmPressable(view, 0, 0, 1400, 960), "serial 0 never counts as drawn");
    // Drawn but no longer fitting the height of the window as it is now.
    const int need = ExpectedSmallestClient(view->lines.size());
    check(need > 0 && D810ConfirmPressable(view, serial, serial, 1400, need) && !D810ConfirmPressable(view, serial, serial, 1400, need - 1), "does not fit: one pixel short");
    check(!D810ConfirmPressable(view, serial, serial, 1400, 0) && !D810ConfirmPressable(view, serial, serial, 0, 0), "does not fit: no client area");
    // The confirm button must lie inside the window.
    check(kD810ConfirmButtonLeft + kD810ConfirmButtonWidth == 632 && D810ConfirmButtonInside(632, 960) && !D810ConfirmButtonInside(631, 960), "button inside: the right edge");
    check(D810ConfirmButtonInside(1400, kD810ButtonRowGap) && !D810ConfirmButtonInside(1400, kD810ButtonRowGap - 1), "button inside: the bottom row");
    check(!D810ConfirmPressable(view, serial, serial, 631, 960), "button outside: not pressable");
    // A result is not a confirmation, drawn or not.
    s.screen->DiscardPlan();
    check(screen.View() && !screen.View()->confirmation && screen.ViewSerial() != serial, "pressable: a result replaces the confirmation with a new serial");
    check(!D810ConfirmPressable(screen.View(), screen.ViewSerial(), screen.ViewSerial(), 1400, 960), "a result is not pressable");
    // Every replacement or clearing of the view changes the serial.
    check(s.Copy(), "pressable: the second copy");
    const auto second = screen.ViewSerial();
    check(second != serial && second != 0, "a new confirmation has a new serial");
    s.screen->ClearView();
    check(!screen.View() && screen.ViewSerial() != second, "clearing the view changes the serial");
    check(!D810ConfirmPressable(screen.View(), screen.ViewSerial(), second, 1400, 960), "a cleared view is not pressable");
    // The window's smallest size holds the longest confirmation: the same arithmetic as the lines count and the boundary.
    check(D810WriteViewMaxLines() == 12, "the longest confirmation has 12 lines");
    for (std::size_t lines = 0; lines <= 16; ++lines) check(D810WriteViewMinClientBottom(lines) == (lines == 0 ? 0 : ExpectedSmallestClient(lines)), "min client height matches the visible lines");
    check(kD810WriteMinClientHeight == 931 && D810WriteViewVisibleLines(kD810WriteMinClientHeight) == 12 && D810WriteViewVisibleLines(kD810WriteMinClientHeight - 1) == 11,
        "the window's minimum height shows 12 lines, one pixel less shows 11");
    check(kD810WriteMinClientWidth >= kD810ConfirmButtonLeft + kD810ConfirmButtonWidth, "the window's minimum width holds the confirm button");
}
// White balance and metering on the screen (test table): the lines of the confirmation, where the confirm button's
// "everything fits" boundary falls with seven steps, and what is said about a white balance that cannot be copied.
void ScreenRevision() {
    {
        // Seven steps: the mode, ISO auto, ISO, shutter, aperture, white balance, metering.
        ScreenBench s(kRevisionTable);
        s.bench.a.props[0x5005].current = 4;
        s.bench.a.props[0x500b].current = 2;
        s.bench.b.props[0x500e].current = 3;
        s.bench.b.props[0xd054].current = 1;
        check(s.Copy() && s.screen->ConfirmationShown(), "revision screen: seven steps shown");
        const auto* view = s.screen->View();
        check(view && view->lines.size() == 11, "revision screen: two notices, the header, seven items and the closing line");
        if (view && view->lines.size() == 11) {
            check(view->lines[0].find(L"自動では元に戻りません") == 0 && view->lines[1].find(L"再送しません") != std::wstring::npos && view->lines[2].find(L"項目:") == 0, "revision screen: the notices come first");
            check(view->lines[8].find(L"ホワイトバランス: CAM-B の今の値 Automatic → 書く値 Daylight") != std::wstring::npos &&
                view->lines[8].find(L"種類だけを写します") != std::wstring::npos && view->lines[8].find(L"微調整は写りません") != std::wstring::npos, "revision screen: white balance, the kind only");
            check(view->lines[9].find(L"測光: CAM-B の今の値 Multi-pattern → 書く値 Center-weighted") != std::wstring::npos, "revision screen: metering with its name");
            check(view->lines[3].find(L"露出モード:") != std::wstring::npos, "revision screen: the table's order");
        }
        // The boundary of "every line fits" (the confirm button is offered only then) with seven steps.
        check(view && SmallestClientThatFits(*view) == ExpectedSmallestClient(11) && ExpectedSmallestClient(11) == 887, "revision screen: eleven lines fit from a 887 px client");
        check(view && D810WriteViewFits(*view, 960) && D810WriteViewFits(*view, 887) && !D810WriteViewFits(*view, 886) && !D810WriteViewFits(*view, 700), "revision screen: the boundary at 887/886");
        check(s.screen->ConfirmOffered(true), "revision screen: the confirm button is offered when the plan is up");
        check(s.Confirm() && Shows(s.screen->View(), L"一致") && s.bench.Writes() == 7 && NoResend(s.bench.b), "revision screen: seven written once");
        check(Shows(s.screen->View(), L"ホワイトバランス: CAM-B の今の値 Automatic → 書く値 Daylight") && Shows(s.screen->View(), L"測光: CAM-B の今の値 Multi-pattern → 書く値 Center-weighted / 結果: 一致"), "revision screen: the results name the two");
        check(s.bench.b.props[0x5005].current == 4 && s.bench.b.props[0x500b].current == 2 && s.bench.a.write_begins == 0, "revision screen: B now reads A's, A untouched");
        check(s.screen->AdmitCapture(), "revision screen: capture admitted after a match");
    }
    {
        // A white balance a copy cannot carry: listed after the steps, with the reason and what to do, and the rest is confirmable.
        ScreenBench s(kRevisionTable);
        s.bench.a.props[0x5005].current = 0x8013;
        s.bench.a.props[0x500b].current = 2;
        s.bench.b.props[0x500e].current = 3;
        s.bench.b.props[0xd054].current = 1;
        check(s.Copy() && s.screen->ConfirmationShown(), "preset screen: the plan is up");
        const auto* view = s.screen->View();
        check(view && view->lines.size() == 12, "preset screen: six steps, the left-out header and its line, the closing line");
        check(Shows(view, L"外した項目") && Shows(view, L"ホワイトバランス: 本体に登録した内容") && Shows(view, L"色が揃わない事があります") && Shows(view, L"B の本体で合わせてください"), "preset screen: the reason and what to do");
        check(Shows(view, L"value_content_dependent") && !Shows(view, L"ホワイトバランス: CAM-B の今の値"), "preset screen: the token is kept, no step for white balance");
        check(view && SmallestClientThatFits(*view) == ExpectedSmallestClient(12) && ExpectedSmallestClient(12) == 931, "preset screen: twelve lines fit from a 931 px client");
        check(view && D810WriteViewFits(*view, 931) && !D810WriteViewFits(*view, 930), "preset screen: the boundary at 931/930");
        check(s.Confirm() && s.bench.Writes() == 6 && s.bench.b.begins_by_code.count(0x5005) == 0 && s.bench.b.props[0x5005].current == 2, "preset screen: written without the white balance");
        check(s.screen->AdmitCapture(), "preset screen: capture admitted");
    }
    {
        // Only the Preset differs from nothing: no step, but it is said that the colors are not compared.
        ScreenBench s(kRevisionTable);
        s.bench.b = Body(1667, 800, 64);
        s.bench.a.props[0x5005].current = 0x8013;
        s.bench.b.props[0x5005].current = 0x8013;
        check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Idle && Shows(s.screen->View(), L"書くものはありません") && Shows(s.screen->View(), L"ホワイトバランス: 本体に登録した内容"), "preset on both: nothing to write, listed");
        check(Shows(s.screen->View(), L"CAM-B の設定は、書ける項目について CAM-A と同じです") && s.bench.Writes() == 0, "preset on both: the wording");
    }
    {
        // The product-shaped table: a difference of the two is listed with the table's reason; the offer text names them.
        ScreenBench s;
        s.bench.a.props[0x5005].current = 4;
        check(s.Copy() && s.screen->ConfirmationShown(), "disabled rows: plan");
        check(Shows(s.screen->View(), L"ホワイトバランス: 表で無効の項目です") && s.bench.Writes() == 0, "disabled rows: left out with the table's reason");
        s.screen->DiscardPlan();
        check(s.screen->OfferText(true).find(L"ホワイトバランス・測光") != std::wstring::npos, "the offer text names white balance and metering");
    }
    {
        // Out of A's reach: the words for the two new tokens.
        check(D810WriteReasonText("value_content_dependent").find(L"登録した内容") != std::wstring::npos && D810WriteReasonText("value_not_listed").find(L"一覧にない値") != std::wstring::npos &&
            D810WriteReasonText("value_not_advertised").find(L"受け付けると答えていません") != std::wstring::npos && D810WriteReasonText("copy_item_out_of_scope").find(L"対象の項目ではありません") != std::wstring::npos,
            "the words of the tokens");
    }
}
// The watchdog's flag at several points of a run: whatever was already sent stays unknown, nothing is sent after the
// flag (not even an END), nothing is resent, and capture stays held also after a restart.
void CancelAtThe(const char* name, const std::function<void(ScreenBench&, std::shared_ptr<std::atomic<bool>>)>& arm, int sent) {
    ScreenBench s;
    const auto flag = std::make_shared<std::atomic<bool>>(false);
    s.bench.access.cancel[1] = flag;
    arm(s, flag);
    const std::string n = name;
    check(s.Copy() && s.Confirm(), (n + ": run").c_str());
    check(flag->load(), (n + ": the flag was raised").c_str());
    check(s.bench.access.after_cancel[1] == 0, (n + ": nothing reached the body after the flag").c_str());
    check(s.bench.Writes() == sent && s.bench.b.write_ends <= sent && NoResend(s.bench.b), (n + ": writes sent, no END after the flag, no resend").c_str());
    check(Shows(s.screen->View(), L"不明") && !s.screen->AdmitCapture(), (n + ": unknown and held").c_str());
    check(s.bench.controller->Block() == D810CaptureBlock::Unknown && s.bench.Restarted().Block() == D810CaptureBlock::Unknown, (n + ": held after a restart").c_str());
}
void ScreenCancelAtSevenPoints() {
    CancelAtThe("mid WRITE_DATA of the first item", [](ScreenBench& s, auto flag) { s.bench.b.after = [flag](const std::string& e) { if (e == "write_data") flag->store(true); }; }, 1);
    CancelAtThe("mid read back of the first item", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [&s, flag](const std::string& e) { if (e == "read" && s.bench.b.write_ends == 1) flag->store(true); }; }, 1);
    CancelAtThe("begin of the first read back", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [&s, flag](const std::string& e) { if (e == "1014(500f)" && s.bench.b.write_ends == 1) flag->store(true); }; }, 1);
    CancelAtThe("between items (END of the first read back)", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [&s, flag](const std::string& e) { if (e == "end" && s.bench.b.write_ends == 1) flag->store(true); }; }, 1);
    CancelAtThe("begin of the second item", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [flag](const std::string& e) { if (e == "1016(500d)") flag->store(true); }; }, 2);
    CancelAtThe("END of the second item", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [&s, flag](const std::string& e) { if (e == "write_end" && s.bench.b.write_ends == 1) flag->store(true); }; }, 2);
    CancelAtThe("END of the last item", [](ScreenBench& s, auto flag) {
        s.bench.b.after = [&s, flag](const std::string& e) { if (e == "write_end" && s.bench.b.write_ends == 2) flag->store(true); }; }, 3);
    {
        // CAM-A's flag during its verification read: nothing is sent, no block.
        ScreenBench s;
        const auto flag = std::make_shared<std::atomic<bool>>(false);
        s.bench.access.cancel[0] = flag;
        int seen = 0;
        s.bench.a.after = [&](const std::string& e) { if (e == "1014(500f)" && ++seen == 2) flag->store(true); }; // 1: the read for the plan, 2: the verification read
        check(s.Copy() && s.Confirm(), "cancel of A: run");
        check(flag->load() && s.bench.access.after_cancel[0] == 0 && s.bench.Writes() == 0, "cancel of A in its read: nothing sent, nothing after the flag");
        check(s.screen->AdmitCapture(), "cancel of A in its read: nothing sent, no block");
    }
}
// A record with a byte order mark is not read as a record: capture is held from the start (someone edited it by
// hand). The same record with CRLF line ends is read as it is.
void RecordWithByteOrderMark() {
    const std::string op = "op=00000000000000a1";
    const std::string plan = "a0sw1 " + op + " ev=plan t=2000-01-01T00:00:00Z source=copy steps=1";
    const std::string step = "a0sw1 " + op + " ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040";
    const std::string end = "a0sw1 " + op + " ev=end t=2000-01-01T00:00:00Z verdict=match sent=1 reason=none";
    struct Row { const char* name; std::string text; D810CaptureBlock expect; };
    const std::vector<Row> rows = {
        {"with a byte order mark", std::string("\xEF\xBB\xBF") + plan + "\n" + step + "\n" + end + "\n", D810CaptureBlock::Unconfirmed},
        {"with CRLF line ends", plan + "\r\n" + step + "\r\n" + end + "\r\n", D810CaptureBlock::None},
    };
    for (const auto& row : rows) {
        Bench bench(kAdr38Table, -1, true);
        WriteFile(bench.folder / "marker", "");
        check(bench.file.Append("a0sw1 ev=reread t=2000-01-01T00:00:00Z verdict=match"), "record: the journal made its file");
        fs::path record;
        for (const auto& entry : fs::directory_iterator(bench.folder))
            if (entry.path().filename() != "marker") record = entry.path();
        check(!record.empty(), "record: found the journal's file");
        WriteFile(record, row.text);
        auto restarted = bench.Restarted();
        D810SettingsWriteScreen screen(restarted, [&bench] { return std::make_unique<BorrowedAccess>(bench.access); }, {});
        const std::string n = std::string("record ") + row.name;
        check(screen.Status().block == row.expect, (n + ": the block").c_str());
        check(screen.AdmitCapture() == (row.expect == D810CaptureBlock::None), (n + ": capture admission").c_str());
        check(row.expect == D810CaptureBlock::None || !screen.CaptureRefusalText().empty(), (n + ": a refusal says why").c_str());
    }
}
void OwnershipIsCompareAndSwap() {
    D810CameraOwnership owner;
    check(owner.TryAcquire(D810CameraUse::Settings) && !owner.TryAcquire(D810CameraUse::Live) && !owner.TryAcquire(D810CameraUse::Capture), "one owner");
    check(!owner.Release(D810CameraUse::Live) && owner.Current() == D810CameraUse::Settings, "only the owner releases");
    check(owner.Release(D810CameraUse::Settings) && owner.Current() == D810CameraUse::None, "released");
    check(!owner.TryAcquire(D810CameraUse::None), "None is not an owner");
}
} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_root = fs::path(temp) / (L"a0-settings-write-tests-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    const auto start = std::chrono::steady_clock::now();
    const auto run = [](const char* name, void (*test)()) {
        const auto begin = std::chrono::steady_clock::now();
        try { test(); }
        catch (const std::exception& error) { ++failures; std::cerr << name << " threw: " << error.what() << "\n"; }
        catch (...) { ++failures; std::cerr << name << " threw\n"; }
        std::cout << name << " " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count() << " ms" << std::endl;
    };
    run("ValuesAndTable", ValuesAndTable);
    run("WritePathReadSet", WritePathReadSet);
    run("DescriptionRules", DescriptionRules);
    run("ProductTable", ProductTable);
    run("ControllerTables", ControllerTables);
    run("FilesEnableNothing", FilesEnableNothing);
    run("CopyPlans", CopyPlans);
    run("RevisionCopyPlans", RevisionCopyPlans);
    run("ChosenPlans", ChosenPlans);
    run("GateSweep", GateSweep);
    run("GateArmConditions", GateArmConditions);
    run("RevisionGate", RevisionGate);
    run("GateWriteOrder", GateWriteOrder);
    run("GateStateSweep", GateStateSweep);
    run("GateFailureShapes", GateFailureShapes);
    run("OtherGatesStillRefuseWrites", OtherGatesStillRefuseWrites);
    run("HappyCopy", HappyCopy);
    run("ProductRun", ProductRun);
    run("BodyAndConfirmation", BodyAndConfirmation);
    run("RecordMustBeWrittenFirst", RecordMustBeWrittenFirst);
    run("PlanChangedAfterConfirm", PlanChangedAfterConfirm);
    run("FailuresStopAndBlock", FailuresStopAndBlock);
    run("RereadReleases", RereadReleases);
    run("JournalStates", JournalStates);
    run("ChosenRuns", ChosenRuns);
    run("RevisionRuns", RevisionRuns);
    run("StopsBeforeSending", StopsBeforeSending);
    run("WatchdogCancel", WatchdogCancel);
    run("ZeroByteReadsInTheWriteSession", ZeroByteReadsInTheWriteSession);
    run("ReadOnlySessionsAfterCancel", ReadOnlySessionsAfterCancel);
    run("ExecuteChecksThePlanNumber", ExecuteChecksThePlanNumber);
    run("BudgetSweep", BudgetSweep);
    run("ScreenCopyFlow", ScreenCopyFlow);
    run("ScreenRefusals", ScreenRefusals);
    run("ScreenSkippedShown", ScreenSkippedShown);
    run("ScreenBlockAndReread", ScreenBlockAndReread);
    run("ScreenPartialFailure", ScreenPartialFailure);
    run("ScreenCancelAndStop", ScreenCancelAndStop);
    run("ScreenConfirmationOrder", ScreenConfirmationOrder);
    run("ScreenConfirmPressable", ScreenConfirmPressable);
    run("ScreenRevision", ScreenRevision);
    run("ScreenCancelAtSevenPoints", ScreenCancelAtSevenPoints);
    run("RecordWithByteOrderMark", RecordWithByteOrderMark);
    run("OwnershipIsCompareAndSwap", OwnershipIsCompareAndSwap);
    std::cout << checks << " checks, " << failures << " failures, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms\n";
    std::error_code ignored;
    fs::remove_all(g_root, ignored);
    CoUninitialize();
    return failures ? 1 : 0;
}
