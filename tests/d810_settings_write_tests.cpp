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
// table for the contract tests; it is a product table only while the revision flag is set (the tests also ask with the flag off).
constexpr D810WriteTable RevisionTable() {
    auto table = Adr38Table();
    for (auto& row : table.rows)
        if (row.camera == D810Camera::B && D810WriteItemOf(row.item).scope == D810ItemScope::Adr38Revision) row.enabled = true;
    return table;
}
constexpr D810WriteTable kRevisionTable = RevisionTable();
static_assert(D810WriteTableProblem(kRevisionTable) == nullptr && D810EnabledRowCount(kRevisionTable) == 7, "seven rows, a valid table");
static_assert(D810ProductTableProblem(kRevisionTable, false) != nullptr && D810ProductTableProblem(kRevisionTable, true) == nullptr, "a product table only while the revision flag is set");
// What the product table enables, written out here and not read from the table: a row opened early, or by mistake, fails
// ProductTable. Table order. Each stage that opens a row changes this list in the same commit.
const std::vector<D810WriteItem> kExpectedProductItems{D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture, D810WriteItem::WhiteBalance, D810WriteItem::Metering};
// The writable items among `differing` (the items that read differently on the two bodies), in table order.
std::vector<D810WriteItem> ExpectedProductSteps(const std::vector<D810WriteItem>& differing) {
    std::vector<D810WriteItem> out;
    for (const auto item : kExpectedProductItems)
        if (std::find(differing.begin(), differing.end(), item) != differing.end()) out.push_back(item);
    return out;
}
// The items a plan writes, in the plan's order, and whether every step targets CAM-B.
std::vector<D810WriteItem> PlanStepItems(const D810PlanResult& plan) {
    std::vector<D810WriteItem> items;
    if (plan.Ok()) for (const auto& step : plan.plan->steps) items.push_back(step.item);
    return items;
}
bool PlanStepsAllCamB(const D810PlanResult& plan) {
    return plan.Ok() && std::all_of(plan.plan->steps.begin(), plan.plan->steps.end(), [](const auto& step) { return step.camera == D810Camera::B; });
}
// On the product bench (PrepareProductBench) these items read differently on the two bodies.
const std::vector<D810WriteItem> kProductBenchDiffering{D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture, D810WriteItem::WhiteBalance, D810WriteItem::Metering};
// The steps a copy makes on the product bench: the enabled items that differ, in table order, without `left_out` (items that are listed for their value instead).
std::vector<D810WriteItem> ExpectedProductStepsWithout(const std::vector<D810WriteItem>& left_out) {
    std::vector<D810WriteItem> differing;
    for (const auto item : kProductBenchDiffering)
        if (std::find(left_out.begin(), left_out.end(), item) == left_out.end()) differing.push_back(item);
    return ExpectedProductSteps(differing);
}
// Everything on, for the ADR-0041 code paths. Not a valid product table.
constexpr D810WriteTable Adr41Table() {
    auto table = Adr38Table();
    for (auto& row : table.rows) row.enabled = true;
    table.chosen_values = true;
    table.verified_firmware_a = "1.00";
    return table;
}
constexpr D810WriteTable kAdr41Table = Adr41Table();
static_assert(D810WriteTableProblem(kAdr41Table) == nullptr && D810ProductTableProblem(kAdr41Table) != nullptr, "ADR-0041 table");
// The table of the per-camera tests: chosen values on and the rows of the five items an operator may pick enabled on both
// cameras, with the firmware version of each camera. What a product table could hold once ADR-0041 is accepted (a test
// table here, because the flag is not set).
constexpr D810WriteTable ChosenTable() {
    auto table = Adr38Table();
    for (auto& row : table.rows) {
        row.enabled = row.item == D810WriteItem::Iso || row.item == D810WriteItem::ShutterSpeed || row.item == D810WriteItem::Aperture ||
            row.item == D810WriteItem::WhiteBalance || row.item == D810WriteItem::Metering;
    }
    table.chosen_values = true;
    table.verified_firmware_a = "1.00";
    return table;
}
constexpr D810WriteTable kChosenTable = ChosenTable();
static_assert(D810WriteTableProblem(kChosenTable) == nullptr, "the per-camera test table is consistent");
static_assert(D810ProductTableProblem(kChosenTable) != nullptr, "it is not a product table while ADR-0041 is not accepted");
static_assert(D810ProductTableProblem(kChosenTable, true, true) == nullptr, "it would be one if ADR-0041 were accepted");
static_assert(D810EnabledRowCount(kChosenTable) == 10, "five items on two cameras");
// A pick of a value, and "same as CAM-A", as the panel draft hands them to the plan.
D810CameraChoice PickFor(D810Camera camera, D810WriteItem item, std::int64_t value) { return {item, camera, D810ChoiceSource::Picked, value}; }
D810CameraChoice FollowA(D810WriteItem item) { return {item, D810Camera::B, D810ChoiceSource::SameAsA, 0}; }
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
    // Which bodies match what the table names for them, as the plan read would report from the fake access.
    D810BodyFlags Verified() const {
        return {D810BodyVerified(table, D810Camera::A, access.model[0], access.firmware[0]), D810BodyVerified(table, D810Camera::B, access.model[1], access.firmware[1])};
    }
    // The way the screen plans picked values: the controller reads both bodies (ReadForPlan) and plans from that read.
    // A body that the table does not name for CAM-B ends the read, so there is no plan then ("read_not_current").
    D810PlanResult Chosen(const std::vector<D810CameraChoice>& choices) {
        const auto read = controller->ReadForPlan(access, options);
        // The read for the plan is not part of the run that follows: the counts of opens and closes start again.
        for (int i = 0; i < 2; ++i) access.opens[i] = access.closes[i] = 0;
        access.overlap = false;
        return controller->PlanChosen(read, choices);
    }
    // The plan builder itself with flags given by the caller (what the controller keeps from a read is tested through Chosen above).
    D810PlanResult Chosen(const std::vector<D810CameraChoice>& choices, const D810BodyFlags& verified) {
        return BuildD810PerCameraPlan(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), verified, choices, table);
    }
    // (camera index, code) of every write begin that reached a body, in the order they arrived. Call after the
    // bodies are set up: assigning a whole body replaces its hook.
    std::vector<std::pair<int, int>> begin_order;
    void TraceBegins() {
        for (int i = 0; i < 2; ++i)
            access.bodies[i]->after = [this, i](const std::string& entry) {
                if (entry.size() == 10 && entry.compare(0, 5, "1016(") == 0) begin_order.emplace_back(i, static_cast<int>(std::stoul(entry.substr(5, 4), nullptr, 16)));
            };
    }
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
    // The enabled set is exactly this: ISO, white balance and metering on CAM-B. Changing the product table makes this check fail on purpose.
    std::set<std::pair<int, int>> enabled;
    for (const auto& row : table.rows)
        if (row.enabled) enabled.insert({static_cast<int>(row.camera), static_cast<int>(row.item)});
    const int cam_b = static_cast<int>(D810Camera::B);
    check(enabled == std::set<std::pair<int, int>>{{cam_b, static_cast<int>(D810WriteItem::Iso)}, {cam_b, static_cast<int>(D810WriteItem::ShutterSpeed)}, {cam_b, static_cast<int>(D810WriteItem::Aperture)},
                         {cam_b, static_cast<int>(D810WriteItem::WhiteBalance)}, {cam_b, static_cast<int>(D810WriteItem::Metering)}},
        "the product table enables exactly ISO, shutter speed, aperture, white balance and metering on CAM-B");
    check(D810EnabledRowCount(table) == 5 && kExpectedProductItems.size() == 5, "five enabled rows");
    check(D810CopyWritableItems(table) == kExpectedProductItems, "the writable items are the expected ones, in table order");
    // The codes the table confirms: ISO auto and shutter speed on both cameras. ISO auto's rows are disabled; shutter speed is enabled on CAM-B only.
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        const auto* iso_auto = table.Row(D810WriteItem::IsoAuto, camera);
        const auto* shutter = table.Row(D810WriteItem::ShutterSpeed, camera);
        check(iso_auto && !iso_auto->enabled && iso_auto->code == 0xd054, "ISO auto: code 0xD054, disabled");
        check(shutter && shutter->enabled == (camera == D810Camera::B) && shutter->code == 0x500d, "shutter speed: code 0x500D, enabled on CAM-B only");
    }
    check(table.Code(D810WriteItem::IsoAuto) == 0xd054 && table.Code(D810WriteItem::ShutterSpeed) == 0x500d, "confirmed codes");
    check(table.copy_a_to_b && !table.chosen_values, "copy only");
    check(std::string(table.verified_model) == "D810" && table.verified_firmware != nullptr && table.verified_firmware[0] != '\0', "model and firmware named");
    check(D810BodyVerified(table, D810Camera::B, "D810", table.verified_firmware), "a body that reports the named model and firmware is verified");
    check(!D810BodyVerified(table, D810Camera::B, "D810", "") && !D810BodyVerified(table, D810Camera::B, "D810", "1.00") && !D810BodyVerified(table, D810Camera::B, "D850", table.verified_firmware) &&
        !D810BodyVerified(table, D810Camera::B, "", table.verified_firmware), "any other model or firmware is not");
    {
        // The two comparisons are reported apart, and an empty text matches nothing on either side.
        const auto both = D810CheckBody(table, D810Camera::B, table.verified_model, table.verified_firmware);
        const auto model_only = D810CheckBody(table, D810Camera::B, table.verified_model, "x");
        const auto firmware_only = D810CheckBody(table, D810Camera::B, "x", table.verified_firmware);
        check(both.model_match && both.firmware_match && model_only.model_match && !model_only.firmware_match && !firmware_only.model_match && firmware_only.firmware_match,
            "the comparisons are apart");
        auto blank = table;
        blank.verified_model = "";
        check(!D810BodyVerified(blank, D810Camera::B, "", table.verified_firmware) && !D810BodyVerified(blank, D810Camera::B, table.verified_model, table.verified_firmware), "an empty model in the table verifies no body");
        blank = table;
        blank.verified_firmware = "";
        check(!D810BodyVerified(blank, D810Camera::B, table.verified_model, "") && !D810BodyVerified(blank, D810Camera::B, table.verified_model, table.verified_firmware), "an empty version in the table verifies no body");
        blank.verified_model = nullptr;
        blank.verified_firmware = nullptr;
        check(!D810BodyVerified(blank, D810Camera::B, "", "") && !D810BodyVerified(blank, D810Camera::B, table.verified_model, "x") && !D810CheckBody(D810DisabledWriteTable(), D810Camera::B, "", "").Verified(), "no entry in the table verifies no body");
    }
    // What the product table may ever hold: CAM-B and the five items, no chosen values.
    std::size_t allowed = 0;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        auto probe = Adr38Table();
        probe.rows[i].enabled = true;
        const bool ok = D810ProductTableProblem(probe) == nullptr;
        allowed += ok;
        check(ok == (probe.rows[i].camera == D810Camera::B && D810ItemInProductScope(D810WriteItemOf(probe.rows[i].item))), "allowed rows");
    }
    check(allowed == 7, "CAM-B x the five and the two revision items");
    // White balance and metering (the ADR-0038 revision): CAM-B's rows are enabled with their codes from the public table, CAM-A's rows
    // are disabled. The product table accepts them only while the revision flag is set (the tests also ask with the flag off).
    check(kD810Adr38RevisionAccepted, "the ADR-0038 revision flag is set");
    for (const auto item : {D810WriteItem::WhiteBalance, D810WriteItem::Metering}) {
        for (const auto camera : {D810Camera::A, D810Camera::B}) {
            const auto* row = table.Row(item, camera);
            check(row && row->enabled == (camera == D810Camera::B) && row->code == (item == D810WriteItem::WhiteBalance ? 0x5005 : 0x500b),
                "revision rows: CAM-B enabled, CAM-A disabled, code from the public table");
        }
        auto probe = Adr38Table();
        for (auto& row : probe.rows) if (row.camera == D810Camera::B && row.item == item) row.enabled = true;
        check(D810ProductTableProblem(probe) == nullptr, "revision row accepted in the product table");
        check(std::string(D810ProductTableProblem(probe, false)) == "extra_item_enabled", "revision row refused without the revision flag");
        check(D810WriteTableProblem(probe) == nullptr, "revision row valid in a test table");
        auto cam_a = Adr38Table();
        for (auto& row : cam_a.rows) if (row.camera == D810Camera::A && row.item == item) row.enabled = true;
        check(std::string(D810ProductTableProblem(cam_a)) == "cam_a_version_unconfirmed", "a CAM-A row needs CAM-A's firmware version in the table");
        cam_a.verified_firmware_a = "1.00";
        check(std::string(D810ProductTableProblem(cam_a)) == "cam_a_row_enabled", "a CAM-A revision row is refused whatever the flag says");
    }
    // The items the next stages open stay disabled in the product table until their own commits.
    for (const auto item : {D810WriteItem::ExposureMode, D810WriteItem::IsoAuto, D810WriteItem::ExposureCompensation})
        for (const auto camera : {D810Camera::A, D810Camera::B}) {
            const auto* row = table.Row(item, camera);
            check(row && !row->enabled, "the other rows are disabled");
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
    cam_a.verified_firmware_a = "1.00";
    check(std::string(D810ControllerTableProblem(cam_a, false)) == "cam_a_row_enabled", "product rules: a CAM-A row refused");
    auto extra = kAdr38Table;
    for (auto& row : extra.rows) if (row.camera == D810Camera::B && row.item == D810WriteItem::ExposureCompensation) row.enabled = true;
    check(std::string(D810ControllerTableProblem(extra, false)) == "extra_item_enabled", "product rules: an extra item refused");
    check(D810ControllerTableProblem(kRevisionTable, false) == nullptr, "product rules: white balance and metering rows pass with the revision flag");
    check(D810ControllerTableProblem(kAdr41Table, true) == nullptr, "test tables: the ADR-0041 paths can be built");
    auto broken = kAdr38Table;
    broken.rows[0].code = 0x1234;
    check(D810ControllerTableProblem(broken, true) != nullptr && D810ControllerTableProblem(broken, false) != nullptr, "an inconsistent table is refused either way");
    {
        // The product entry point holds the product table.
        Bench bench(kProduct);
        check(bench.controller->Status().enabled_rows == 5, "product entry: five rows");
        check(bench.controller->Status().write_allowed && bench.controller->Status().capture_allowed, "rows enabled, no block: copying is offered");
        auto none = kAdr38Table;
        for (auto& row : none.rows) row.enabled = false;
        Bench empty(none);
        check(empty.controller->Status().enabled_rows == 0 && !empty.controller->Status().write_allowed && empty.controller->Status().capture_allowed, "no row enabled: copying is not offered");
        const auto copy = bench.Copy();
        check(copy.Ok() && PlanStepItems(copy) == ExpectedProductSteps({D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}) && PlanStepsAllCamB(copy) && bench.Writes() == 0,
            "product entry: only the enabled items, on CAM-B, can be planned");
        check(bench.Restarted().Status().enabled_rows == 5, "product entry after a restart");
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
    check(copy.Ok() && PlanStepItems(copy) == ExpectedProductSteps({D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}) && PlanStepsAllCamB(copy),
        "product copy: the enabled items on CAM-B and nothing else, whatever the files and the variable say");
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        check(!bench.Chosen({PickFor(camera, D810WriteItem::Iso, 200)}).Ok(), "product chosen refused");
        check(!bench.Chosen({PickFor(camera, D810WriteItem::Iso, 200)}, {true, true}).Ok(), "product chosen refused whatever the body check says");
    }
    check(!bench.Chosen({FollowA(D810WriteItem::Iso)}, {true, true}).Ok(), "product chosen: same as CAM-A refused");
    check(bench.controller->Status().enabled_rows == 5, "status: five rows");
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
        // The product table: the enabled items are planned on CAM-B; every other difference is left out because its row is disabled.
        Bench bench(kProduct);
        const auto plan = bench.Copy();
        check(plan.Ok() && PlanStepItems(plan) == ExpectedProductSteps({D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture}) && PlanStepsAllCamB(plan) &&
                plan.plan->steps[0].value == 64 && plan.plan->steps[0].before == 100 && plan.plan->steps[1].value == 1667 && plan.plan->steps[1].before == 333 &&
                plan.plan->steps[2].value == 800 && plan.plan->steps[2].before == 560,
            "product table: ISO, shutter speed and aperture, with B's value now and A's value");
        const auto skipped = [&](D810WriteItem item) {
            return plan.Ok() && std::any_of(plan.plan->skipped.begin(), plan.plan->skipped.end(), [&](const auto& s) { return s.first == item && s.second == "row_disabled"; });
        };
        check(!skipped(D810WriteItem::ShutterSpeed) && !skipped(D810WriteItem::Aperture), "product table: shutter speed and aperture are steps, not left out for a disabled row");
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
        check(D810BlockItems(kD810ProductWriteTable) == std::vector<Item>{Item::ExposureMode, Item::IsoAuto, Item::Iso, Item::ShutterSpeed, Item::Aperture, Item::WhiteBalance, Item::Metering},
            "the product table: the release compares seven items (the five and white balance and metering)");
    }
}
// --- Plans of the values picked for each camera (ADR-0041) -------------------------------------------------------
bool HasReason(const D810PlanResult& plan, const std::string& reason) {
    return std::find(plan.reasons.begin(), plan.reasons.end(), reason) != plan.reasons.end();
}
// The steps of a plan as "CAM-x item value", in the plan's order.
std::vector<std::string> StepTexts(const D810PlanResult& plan) {
    std::vector<std::string> out;
    if (plan.Ok())
        for (const auto& step : plan.plan->steps)
            out.push_back(std::string(D810CameraAlias(step.camera)) + " " + D810WriteItemOf(step.item).name + " " + std::to_string(step.value));
    return out;
}
bool Observed(const D810PlanResult& plan, D810Camera camera, std::uint16_t code, std::int64_t value) {
    if (!plan.Ok()) return false;
    return std::any_of(plan.plan->observed.begin(), plan.plan->observed.end(),
        [&](const auto& seen) { return seen.camera == camera && seen.code == code && seen.value == value; });
}
bool ObservedCode(const D810PlanResult& plan, D810Camera camera, std::uint16_t code) {
    if (!plan.Ok()) return false;
    return std::any_of(plan.plan->observed.begin(), plan.plan->observed.end(), [&](const auto& seen) { return seen.camera == camera && seen.code == code; });
}
void ChosenPlans() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        // A table with chosen values off, the product table included: no plan for any item, camera, source or value,
        // whatever the body check says.
        const auto sweep = [](Bench& bench) {
            for (const auto& spec : kD810WriteItems)
                for (const auto camera : {D810Camera::A, D810Camera::B})
                    for (const auto value : {0LL, 1LL, 64LL, 800LL, 1667LL, 4LL})
                        for (const auto& flags : {D810BodyFlags{false, false}, D810BodyFlags{true, true}}) {
                            const auto plan = bench.Chosen({PickFor(camera, spec.item, value)}, flags);
                            check(!plan.Ok() && plan.reasons.size() == 1 && plan.reasons[0] == "chosen_values_disabled", "chosen disabled");
                        }
            const auto follow = bench.Chosen({FollowA(Item::Iso)}, {true, true});
            check(!follow.Ok() && follow.reasons.size() == 1 && follow.reasons[0] == "chosen_values_disabled", "same as CAM-A: chosen disabled");
            check(bench.Writes() == 0, "nothing sent");
        };
        Bench adr38;
        sweep(adr38);
        Bench product(kProduct);
        sweep(product);
        // The product table, by the rules a product table must keep.
        check(!kD810Adr41Accepted && !kD810ProductWriteTable.chosen_values && !D810TextPresent(kD810ProductWriteTable.verified_firmware_a), "product table: ADR-0041 is not accepted, no chosen values, no CAM-A version");
        for (const auto& row : kD810ProductWriteTable.rows) check(!row.enabled || row.camera == B, "product table: no CAM-A row is enabled");
    }
    {
        // Order: every CAM-B step first, every CAM-A step last, the item table's order within a camera, whatever order the
        // picks arrive in.
        Bench bench(kChosenTable);
        const std::vector<D810CameraChoice> picks{PickFor(A, Item::Iso, 200), PickFor(B, Item::Aperture, 1100), PickFor(A, Item::Metering, 4),
            PickFor(B, Item::Iso, 200), PickFor(B, Item::WhiteBalance, 4)};
        const std::vector<std::string> expected{"CAM-B iso 200", "CAM-B aperture 1100", "CAM-B white_balance 4", "CAM-A iso 200", "CAM-A metering 4"};
        const auto plan = bench.Chosen(picks);
        check(plan.Ok() && StepTexts(plan) == expected, "B first, A last, item order within a camera");
        auto reversed = picks;
        std::reverse(reversed.begin(), reversed.end());
        check(StepTexts(bench.Chosen(reversed)) == expected, "the order of the picks does not matter");
        if (plan.Ok()) {
            const auto& steps = plan.plan->steps;
            check(plan.plan->source == D810PlanSource::Chosen && plan.plan->Cameras() == std::vector<D810Camera>{B, A}, "Cameras() is CAM-B then CAM-A");
            check(steps[0].before == 100 && steps[0].code == 0x500f && steps[1].before == 560 && steps[1].code == 0x5007 && steps[2].before == 2 && steps[2].code == 0x5005 &&
                steps[3].before == 64 && steps[3].code == 0x500f && steps[4].before == 3 && steps[4].code == 0x500b, "each step carries its own camera's value before and code");
            check(D810PlanProblem(*plan.plan, kChosenTable).empty(), "the plan is well formed");
            // What the plan relies on: the values it changes, and manual mode and ISO auto off, per camera with a step.
            check(Observed(plan, B, 0x500f, 100) && Observed(plan, B, 0x5007, 560) && Observed(plan, B, 0x5005, 2) && Observed(plan, A, 0x500f, 64) && Observed(plan, A, 0x500b, 3),
                "the values to change are observed on their own camera");
            check(Observed(plan, A, 0x500e, 1) && Observed(plan, A, 0xd054, 0) && Observed(plan, B, 0x500e, 1) && Observed(plan, B, 0xd054, 0), "manual mode and ISO auto are observed");
            check(!ObservedCode(plan, B, 0x500b) && !ObservedCode(plan, A, 0x5007) && !ObservedCode(plan, A, 0x5005), "nothing else is observed");
        }
        check(bench.Writes() == 0, "planning sends nothing");
    }
    {
        // Different values on the two cameras: each step carries its own camera's value.
        Bench bench(kChosenTable);
        const auto plan = bench.Chosen({PickFor(A, Item::Iso, 200), PickFor(B, Item::Iso, 400)});
        check(plan.Ok() && StepTexts(plan) == std::vector<std::string>{"CAM-B iso 400", "CAM-A iso 200"}, "A=200 and B=400");
        check(plan.Ok() && plan.plan->steps[0].before == 100 && plan.plan->steps[1].before == 64, "before: B reads 100, A reads 64");
        const auto only_a = bench.Chosen({PickFor(A, Item::Iso, 400)});
        check(only_a.Ok() && StepTexts(only_a) == std::vector<std::string>{"CAM-A iso 400"} && only_a.plan->Cameras() == std::vector<D810Camera>{A}, "CAM-A alone");
        check(!ObservedCode(only_a, B, 0x500f) && !ObservedCode(only_a, B, 0x500e), "CAM-B is not relied on by a CAM-A-only plan");
        const auto only_b = bench.Chosen({PickFor(B, Item::Iso, 400)});
        check(only_b.Ok() && only_b.plan->Cameras() == std::vector<D810Camera>{B} && !ObservedCode(only_b, A, 0x500f) && !ObservedCode(only_b, A, 0x500e), "CAM-B alone");
    }
    {
        // A pick equal to what the camera reads now is not a step. It still has to be a valid pick.
        Bench bench(kChosenTable);
        const auto same = bench.Chosen({PickFor(B, Item::Iso, 100), PickFor(A, Item::Iso, 64)});
        check(same.Ok() && same.plan->steps.empty(), "picks equal to the current values: a plan without steps");
        if (same.Ok()) check(std::string(bench.controller->Confirm(same.plan->id)) == "nothing_to_write", "nothing to write cannot be confirmed");
        const auto mixed = bench.Chosen({PickFor(B, Item::Iso, 100), PickFor(B, Item::Aperture, 1100)});
        check(mixed.Ok() && StepTexts(mixed) == std::vector<std::string>{"CAM-B aperture 1100"}, "only the differing pick is a step");
        bench.b.props[0x500f].values = {64, 200, 400};
        const auto invalid = bench.Chosen({PickFor(B, Item::Iso, 100), PickFor(B, Item::Aperture, 1100)});
        check(!invalid.Ok() && HasReason(invalid, "value_not_advertised:iso:CAM-B"), "a no-op pick is checked too, and the whole plan is refused");
    }
    {
        // "Same as CAM-A" is resolved here, from the read made for the plan.
        Bench bench(kChosenTable);
        bench.a.props[0x500f].current = 200;
        const auto follow = bench.Chosen({FollowA(Item::Iso)});
        check(follow.Ok() && StepTexts(follow) == std::vector<std::string>{"CAM-B iso 200"} && follow.plan->Cameras() == std::vector<D810Camera>{B}, "B follows what A reads");
        check(Observed(follow, A, 0x500f, 200), "the value of CAM-A it came from is an observation, although CAM-A gets no step");
        bench.a.props[0x500f].current = 400;
        check(StepTexts(bench.Chosen({FollowA(Item::Iso)})) == std::vector<std::string>{"CAM-B iso 400"}, "a new read gives the new value");
        // CAM-A's pick wins over what CAM-A reads.
        bench.a.props[0x500f].current = 64;
        const auto picked = bench.Chosen({FollowA(Item::Iso), PickFor(A, Item::Iso, 200)});
        check(picked.Ok() && StepTexts(picked) == std::vector<std::string>{"CAM-B iso 200", "CAM-A iso 200"}, "B follows CAM-A's pick, then CAM-A is set");
        // CAM-A's pick is the value it already reads: B follows it, A gets no step.
        bench.a.props[0x500f].current = 200;
        const auto kept = bench.Chosen({PickFor(A, Item::Iso, 200), FollowA(Item::Iso)});
        check(kept.Ok() && StepTexts(kept) == std::vector<std::string>{"CAM-B iso 200"} && Observed(kept, A, 0x500f, 200), "A keeps its value and B follows");
        // Equal already: no step.
        bench.b.props[0x500f].current = 200;
        const auto equal = bench.Chosen({FollowA(Item::Iso)});
        check(equal.Ok() && equal.plan->steps.empty(), "B reads what A reads: nothing to do");
        // B does not offer A's value.
        Bench other(kChosenTable);
        other.a.props[0x5007].current = 1600;
        other.b.props[0x5007].values = {400, 560, 800, 1100};
        const auto not_offered = other.Chosen({FollowA(Item::Aperture)});
        check(!not_offered.Ok() && HasReason(not_offered, "same_as_a_not_offered:aperture:CAM-B"), "B does not offer A's value");
        // A reads a white balance this application never writes.
        other.a.props[0x5005].current = 0x8013;
        const auto preset = other.Chosen({FollowA(Item::WhiteBalance)});
        check(!preset.Ok() && HasReason(preset, "same_as_a_not_offered:white_balance:CAM-B"), "A on Preset: B cannot follow");
        // "Same as CAM-A" for CAM-A, and CAM-A unreadable.
        const auto on_a = other.Chosen({D810CameraChoice{Item::Iso, A, D810ChoiceSource::SameAsA, 0}});
        check(!on_a.Ok() && HasReason(on_a, "same_as_a_on_cam_a:iso:CAM-A"), "no 'same as CAM-A' for CAM-A");
        other.a.props.erase(0x500f);
        const auto unreadable = other.Chosen({FollowA(Item::Iso)});
        check(!unreadable.Ok() && HasReason(unreadable, "unreadable:iso:CAM-A"), "A's value cannot be read");
        check(bench.Writes() == 0 && other.Writes() == 0, "nothing sent");
    }
    {
        // Values and items that are refused, each with its reason; any one refuses the whole plan. The two bodies
        // advertise more than the application writes, so that the application's rules are what refuse.
        Bench bench(kChosenTable);
        for (auto* body : {&bench.a, &bench.b}) {
            body->props[0x500f].values = {32, 64, 100, 200, 25600};
            body->props[0x5005].values = {1, 2, 3, 4, 5, 6, 7, 0x8010, 0x8011, 0x8012, 0x8013};
            body->props[0x500b].values = {1, 2, 3, 4, 5};
        }
        struct Case { Item item; std::int64_t value; const char* reason; };
        const std::vector<Case> cases{
            {Item::Iso, 320, "value_not_advertised"},     {Item::Iso, 25600, "value_out_of_band"},         {Item::Iso, 32, "value_out_of_band"},
            {Item::Metering, 1, "value_not_named"},       {Item::Metering, 5, "value_not_listed"},          {Item::WhiteBalance, 1, "value_content_dependent"},
            {Item::WhiteBalance, 3, "value_content_dependent"}, {Item::WhiteBalance, 0x8012, "value_content_dependent"}, {Item::WhiteBalance, 0x8013, "value_content_dependent"},
            {Item::WhiteBalance, 8, "value_not_listed"},  {Item::ShutterSpeed, 0xffffffffLL, "bulb_or_time"}, {Item::ShutterSpeed, 0xfffffffdLL, "bulb_or_time"},
            {Item::ShutterSpeed, 20000, "exposure_too_long"}, {Item::ShutterSpeed, 0, "exposure_zero"},    {Item::ShutterSpeed, 400, "value_not_advertised"},
            {Item::Aperture, 700, "value_not_advertised"}, {Item::Aperture, 0, "value_not_advertised"},    {Item::ExposureMode, 1, "item_not_choosable"},
            {Item::ExposureMode, 3, "item_not_choosable"}, {Item::IsoAuto, 0, "item_not_choosable"},       {Item::IsoAuto, 1, "item_not_choosable"},
            {Item::ExposureCompensation, 0, "item_not_choosable"}, {Item::ExposureCompensation, -500, "item_not_choosable"},
        };
        for (const auto camera : {A, B})
            for (const auto& c : cases) {
                const auto plan = bench.Chosen({PickFor(camera, c.item, c.value)});
                const std::string reason = std::string(c.reason) + ":" + D810WriteItemOf(c.item).name + ":" + D810CameraAlias(camera);
                check(!plan.Ok() && HasReason(plan, reason), "refused with its reason");
                // One valid pick next to it does not save the plan.
                const auto companion = c.item == Item::Aperture && camera == B ? PickFor(B, Item::Iso, 200) : PickFor(B, Item::Aperture, 1100);
                const auto both = bench.Chosen({companion, PickFor(camera, c.item, c.value)});
                check(!both.Ok() && HasReason(both, reason), "the whole plan is refused");
            }
        // Values that pass every rule are accepted on either camera.
        for (const auto camera : {A, B}) {
            check(bench.Chosen({PickFor(camera, Item::Iso, 200)}).Ok() && bench.Chosen({PickFor(camera, Item::WhiteBalance, 0x8010)}).Ok() &&
                bench.Chosen({PickFor(camera, Item::Metering, 4)}).Ok() && bench.Chosen({PickFor(camera, Item::ShutterSpeed, 500)}).Ok(), "values that pass every rule");
        }
        // The state of the body.
        Bench body(kChosenTable);
        body.b.props[0x500f].get_set = 0;
        check(HasReason(body.Chosen({PickFor(B, Item::Iso, 200)}), "not_writable:iso:CAM-B"), "GetSet 0");
        body.b.props.erase(0x5007);
        check(HasReason(body.Chosen({PickFor(B, Item::Aperture, 1100)}), "unreadable:aperture:CAM-B"), "not readable");
        check(!body.Chosen({}).Ok() && HasReason(body.Chosen({}), "no_choice"), "no pick");
        const auto twice = body.Chosen({PickFor(B, Item::Iso, 200), PickFor(B, Item::Iso, 400)});
        check(!twice.Ok() && HasReason(twice, "duplicate_choice:iso:CAM-B"), "one pick per camera and item");
        check(bench.Writes() == 0 && body.Writes() == 0, "nothing sent");
    }
    {
        // Rows: the table lets a pick through only where the row is enabled.
        auto table = kChosenTable;
        for (auto& row : table.rows) if (row.camera == A) row.enabled = false;
        Bench bench(table);
        const auto plan = bench.Chosen({PickFor(A, Item::Iso, 200)});
        check(!plan.Ok() && HasReason(plan, "row_disabled:iso:CAM-A"), "CAM-A row disabled");
        check(bench.Chosen({PickFor(B, Item::Iso, 200)}).Ok(), "CAM-B row enabled");
        table = kChosenTable;
        for (auto& row : table.rows) if (row.camera == B && row.item == Item::WhiteBalance) row.enabled = false;
        Bench second(table);
        check(HasReason(second.Chosen({PickFor(B, Item::WhiteBalance, 4)}), "row_disabled:white_balance:CAM-B"), "CAM-B white balance row disabled");
        // A row that is enabled while its code is not confirmed (such a table is not accepted by a controller; the plan checks anyway).
        auto unconfirmed = kChosenTable;
        for (auto& row : unconfirmed.rows) if (row.item == Item::ShutterSpeed) row.code = 0;
        auto a = Body(1667, 800, 64), b = Body(333, 560, 100);
        const auto direct = BuildD810PerCameraPlan(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {true, true}, {PickFor(B, Item::ShutterSpeed, 500)}, unconfirmed);
        check(!direct.Ok() && HasReason(direct, "code_unconfirmed:shutter_speed:CAM-B"), "code not confirmed");
        // Picks outside the item and camera enumerations are refused, not indexed.
        const auto wild = BuildD810PerCameraPlan(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {true, true},
            {D810CameraChoice{static_cast<D810WriteItem>(99), B, D810ChoiceSource::Picked, 1}}, kChosenTable);
        check(!wild.Ok() && HasReason(wild, "item_not_choosable"), "an item outside the table");
        const auto wild_camera = BuildD810PerCameraPlan(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {true, true},
            {D810CameraChoice{Item::Iso, static_cast<D810Camera>(5), D810ChoiceSource::Picked, 200}}, kChosenTable);
        check(!wild_camera.Ok(), "a camera outside A and B");
    }
    {
        // The body check is per camera and covers only the cameras that get a step.
        Bench bench(kChosenTable);
        bench.access.firmware[0] = "other-text";
        check(bench.Verified() == D810BodyFlags{false, true}, "the fake CAM-A reports another version");
        const auto a_step = bench.Chosen({PickFor(A, Item::Iso, 200)});
        check(!a_step.Ok() && HasReason(a_step, "body_not_verified:CAM-A") && !HasReason(a_step, "body_not_verified:CAM-B"), "CAM-A is not verified");
        check(bench.Chosen({PickFor(B, Item::Iso, 200)}).Ok(), "a CAM-B-only plan does not depend on CAM-A's body");
        const auto mixed = bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)});
        check(!mixed.Ok() && HasReason(mixed, "body_not_verified:CAM-A"), "the plan with a CAM-A step is refused");
        check(bench.Chosen({PickFor(A, Item::Iso, 64)}).Ok(), "a CAM-A pick that changes nothing needs no verified CAM-A");
        bench.access.firmware[1] = "other-text";
        // A CAM-B that is not the table's ends the read for the plan, so the controller has no read to plan from; the plan builder, given
        // both flags false, names both bodies.
        const auto both = bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)});
        check(!both.Ok() && HasReason(both, "read_not_current"), "CAM-B not verified: the read does not complete, no plan");
        const auto named = bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)}, bench.Verified());
        check(!named.Ok() && HasReason(named, "body_not_verified:CAM-A") && HasReason(named, "body_not_verified:CAM-B"), "both named");
        Bench flags(kChosenTable);
        check(!flags.Chosen({PickFor(B, Item::Iso, 200)}, {true, false}).Ok() && flags.Chosen({PickFor(B, Item::Iso, 200)}, {false, true}).Ok(), "the flags are read by camera");
        check(!flags.Chosen({PickFor(A, Item::Iso, 200)}, {false, true}).Ok() && flags.Chosen({PickFor(A, Item::Iso, 200)}, {true, false}).Ok(), "the flags are read by camera, CAM-A");
        check(bench.Writes() == 0 && flags.Writes() == 0, "nothing sent");
    }
    {
        // The table names a firmware per camera, and a body is checked against its own camera's.
        auto table = kChosenTable;
        table.verified_firmware = "B-version";
        table.verified_firmware_a = "A-version";
        check(D810BodyVerified(table, A, "D810", "A-version") && !D810BodyVerified(table, B, "D810", "A-version") &&
            D810BodyVerified(table, B, "D810", "B-version") && !D810BodyVerified(table, A, "D810", "B-version"), "each camera has its own version");
        check(D810CheckBody(table, A, "D810", "B-version").model_match && !D810CheckBody(table, A, "D810", "B-version").firmware_match, "the two comparisons are apart, per camera");
        for (const char* none : {static_cast<const char*>(nullptr), ""}) {
            table.verified_firmware_a = none;
            check(!D810BodyVerified(table, A, "D810", "") && !D810BodyVerified(table, A, "D810", "A-version") && !D810BodyVerified(table, A, "D810", "B-version"), "no version for CAM-A: no CAM-A body is verified");
            check(D810BodyVerified(table, B, "D810", "B-version"), "CAM-B is unaffected");
            check(std::string(D810WriteTableProblem(table)) == "cam_a_version_unconfirmed", "a CAM-A row needs CAM-A's version in the table");
            auto without_a_rows = table;
            for (auto& row : without_a_rows.rows) if (row.camera == A) row.enabled = false;
            check(D810WriteTableProblem(without_a_rows) == nullptr, "no CAM-A row, no CAM-A version needed");
        }
        // The product table: no body is verified as CAM-A, whatever it reports.
        const auto& product = kD810ProductWriteTable;
        check(!D810BodyVerified(product, A, product.verified_model, product.verified_firmware) && !D810BodyVerified(product, A, product.verified_model, "") &&
            D810BodyVerified(product, B, product.verified_model, product.verified_firmware), "product table: CAM-A cannot be verified");
        // Product-table rules once ADR-0041 would be accepted: CAM-A rows need the chosen values, never the exposure mode or ISO auto.
        check(D810ProductTableProblem(kChosenTable, true, true) == nullptr, "an accepted ADR-0041 would allow this table");
        auto a_without_chosen = kChosenTable;
        a_without_chosen.chosen_values = false;
        check(std::string(D810ProductTableProblem(a_without_chosen, true, true)) == "cam_a_row_enabled", "a CAM-A row without chosen values");
        auto a_mode = kChosenTable;
        for (auto& row : a_mode.rows) if (row.camera == A && row.item == Item::ExposureMode) row.enabled = true;
        check(std::string(D810ProductTableProblem(a_mode, true, true)) == "cam_a_row_not_choosable", "never the exposure mode on CAM-A");
        check(std::string(D810ProductTableProblem(kChosenTable, true, false)) == "chosen_values_before_adr0041", "chosen values need ADR-0041");
    }
    {
        // Manual mode and ISO auto off are needed on the cameras that get a step, and only on those.
        Bench bench(kChosenTable);
        bench.b.props[0x500e].current = 3;
        const auto b_step = bench.Chosen({PickFor(B, Item::Iso, 200)});
        check(!b_step.Ok() && HasReason(b_step, "exposure_mode_not_manual_set_on_body:CAM-B"), "B is not in M");
        check(bench.Chosen({PickFor(A, Item::Iso, 200)}).Ok(), "B's mode does not matter to a CAM-A-only plan");
        bench.b.props[0x500e].current = 1;
        bench.a.props[0xd054].current = 1;
        const auto a_step = bench.Chosen({PickFor(A, Item::Iso, 200), PickFor(B, Item::Iso, 200)});
        check(!a_step.Ok() && HasReason(a_step, "iso_auto_not_off:CAM-A") && !HasReason(a_step, "iso_auto_not_off:CAM-B"), "A's ISO auto is on");
        check(bench.Chosen({PickFor(B, Item::Iso, 200)}).Ok(), "A's ISO auto does not matter to a CAM-B-only plan");
    }
    {
        // The shape check, on plans built by hand.
        D810WritePlan plan;
        plan.source = D810PlanSource::Chosen;
        plan.iso_auto_code = 0xd054;
        const D810WriteStep b_iso{B, Item::Iso, 0x500f, 100, 200}, b_aperture{B, Item::Aperture, 0x5007, 560, 1100}, a_iso{A, Item::Iso, 0x500f, 64, 200};
        plan.steps = {b_iso, b_aperture, a_iso};
        check(D810PlanProblem(plan, kChosenTable).empty(), "CAM-B steps, then CAM-A steps");
        for (const auto& steps : std::vector<std::vector<D810WriteStep>>{{a_iso, b_iso}, {b_aperture, b_iso}, {b_iso, b_iso}, {b_iso, a_iso, b_aperture}, {a_iso, a_iso}}) {
            plan.steps = steps;
            check(D810PlanProblem(plan, kChosenTable) == "plan_order", "CAM-A before CAM-B, an item out of order, or an item twice");
        }
        plan.steps = {D810WriteStep{B, Item::Iso, 0x5007, 100, 200}};
        check(D810PlanProblem(plan, kChosenTable) == "plan_step_code", "a code that is not the item's");
        plan.steps = {a_iso};
        plan.source = D810PlanSource::CopyAToB;
        check(D810PlanProblem(plan, kChosenTable) == "copy_targets_cam_b_only", "a copy plan never has a CAM-A step");
        plan.source = D810PlanSource::Chosen;
        plan.iso_auto_code = 0;
        check(D810PlanProblem(plan, kChosenTable) == "code_changed:iso_auto", "the table's codes again");
        auto no_a = kChosenTable;
        for (auto& row : no_a.rows) if (row.camera == A) row.enabled = false;
        plan.iso_auto_code = 0xd054;
        check(D810PlanProblem(plan, no_a) == "row_disabled", "the table's rows again");
    }
    {
        // The write stage's time: 60 s as the base, 120 s for a chosen plan of six steps or more, a copy keeps the base.
        D810WritePlan plan;
        plan.source = D810PlanSource::Chosen;
        const std::chrono::milliseconds base{60000};
        plan.steps.resize(5);
        check(D810WriteDeadlineFor(plan, base) == base, "five steps: the base");
        plan.steps.resize(6);
        check(D810WriteDeadlineFor(plan, base) == std::chrono::milliseconds(120000) && D810WriteDeadlineFor(plan, std::chrono::milliseconds(200000)) == std::chrono::milliseconds(200000),
            "six steps: 120 s, never less than the base");
        plan.steps.resize(10);
        check(D810WriteDeadlineFor(plan, base) == std::chrono::milliseconds(120000), "ten steps: 120 s");
        plan.source = D810PlanSource::CopyAToB;
        plan.steps.resize(7);
        check(D810WriteDeadlineFor(plan, base) == base, "a copy keeps the base");
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
// The key to a CAM-A session (ADR-0041): closed by default, and a CAM-A gate arms only the planned (camera, code, value).
void GateCamA() {
    {
        // Closed by default: a CAM-A configuration without the key retires the gate before it can do anything.
        auto config = Config();
        config.camera = D810Camera::A;
        D810SettingsWriteGate gate(config);
        check(gate.Retired() && std::string(gate.LastRefusal()) == "invalid_config", "a CAM-A gate without the key is retired");
        check(std::string(gate.Arm(D810Camera::A, 0x500f, 64)) == "retired", "it arms nothing");
        check(!gate.Admit(Make(SettingsCommandKind::ExecuteWithDataToWrite, 0x1016, {0x500f}).Get()) && gate.WritesSent() == 0, "it admits no write");
        check(!D810WriteGateConfig{}.cam_a_allowed, "the key is closed by default");
        config.cam_a_allowed = true;
        D810SettingsWriteGate open(config);
        check(!open.Retired(), "with the key the gate is built");
        Prepare(open);
        check(open.Arm(D810Camera::A, 0x500f, 64) == nullptr, "and arms the planned write");
        // The key opens nothing else.
        auto b_config = Config();
        b_config.cam_a_allowed = true;
        D810SettingsWriteGate b_gate(b_config);
        Prepare(b_gate);
        check(std::string(b_gate.Arm(D810Camera::A, 0x500f, 64)) == "other_destination" && b_gate.Arm(D810Camera::B, 0x500f, 64) == nullptr, "a CAM-B gate stays a CAM-B gate");
        // A CAM-A gate still checks the same things as any gate.
        D810SettingsWriteGate a_live(config);
        Prepare(a_live, 1);
        check(std::string(a_live.Arm(D810Camera::A, 0x500f, 64)) == "live_not_confirmed_off", "live view on");
        D810SettingsWriteGate a_mode(config);
        Prepare(a_mode, 0, 3);
        check(std::string(a_mode.Arm(D810Camera::A, 0x500f, 64)) == "mode_not_confirmed_manual", "not in M");
        D810SettingsWriteGate a_value(config);
        Prepare(a_value);
        check(std::string(a_value.Arm(D810Camera::A, 0x500f, 100)) == "not_in_plan", "a value that is not planned");
    }
    {
        // Every (camera, code, value) with a gate of each camera: only the planned pair on the gate's own camera arms.
        const std::vector<std::uint16_t> codes{0x500e, 0xd054, 0x500f, 0x500d, 0x5007, 0x5005, 0x500b, 0x5010, 0xd16a, 0xd100};
        const std::vector<std::int64_t> values{0, 1, 2, 64, 100, 200, 560, 800, 1667};
        for (const auto gate_camera : {D810Camera::A, D810Camera::B}) {
            auto config = Config();
            config.camera = gate_camera;
            config.cam_a_allowed = true;
            std::size_t armed = 0;
            for (const auto camera : {D810Camera::A, D810Camera::B})
                for (const auto code : codes)
                    for (const auto value : values) {
                        D810SettingsWriteGate gate(config);
                        Prepare(gate);
                        const bool ok = gate.Arm(camera, code, value) == nullptr;
                        armed += ok;
                        check(ok == (camera == gate_camera && ((code == 0x500f && value == 64) || (code == 0x5007 && value == 800))), "only the planned pair on the gate's own camera");
                    }
            check(armed == 2, "two planned pairs, each on its own camera");
        }
    }
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
        config.cam_a_allowed = true;
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
// A product-table bench whose CAM-B reports the table's model and version. A reads differently from B on every writable item
// (ISO, white balance, metering) and on the two that are not enabled (shutter speed, aperture).
void PrepareProductBench(Bench& bench) {
    bench.access.model[1] = kD810ProductWriteTable.verified_model;
    bench.access.firmware[1] = kD810ProductWriteTable.verified_firmware;
    bench.a.props[0x5005].current = 4;
    bench.a.props[0x500b].current = 2;
}
// The writes the product table makes, in the order it makes them, written out here (not read from the table): the item, its
// code on the body, the value A holds and a value B could answer with instead (to fake a mismatch). Table order.
struct ProductWrite { D810WriteItem item; std::uint16_t code; std::int64_t a_value; std::int64_t other_value; };
const std::vector<ProductWrite> kExpectedProductWrites{
    {D810WriteItem::Iso, 0x500f, 64, 200},
    {D810WriteItem::ShutterSpeed, 0x500d, 1667, 2500},
    {D810WriteItem::Aperture, 0x5007, 800, 1100},
    {D810WriteItem::WhiteBalance, 0x5005, 4, 5},
    {D810WriteItem::Metering, 0x500b, 2, 4},
};
// The product table end to end on the fakes: for every enabled item one begin, data and END, on CAM-B's own code, in the table's
// order; CAM-A is only read; the items whose rows are not enabled are not touched.
void ProductRun() {
    {
        Bench bench(kProduct);
        PrepareProductBench(bench);
        const auto run = bench.CopyRun();
        const auto n = kExpectedProductWrites.size();
        check(run.verdict == D810WriteVerdict::Match && run.sent == n && run.reason.empty(), "product run: matched, one write per enabled item");
        check(bench.b.begins_by_code.count(0xd100) == 0 && bench.a.begins_by_code.count(0xd100) == 0, "product run: nothing is ever sent to 0xD100");
        for (const auto& write : kExpectedProductWrites)
            check(bench.b.props[write.code].current == write.a_value && bench.b.begins_by_code[write.code] == 1, "product run: B reads A's value, written once");
        check(bench.b.write_begins == static_cast<int>(n) && bench.b.write_datas == static_cast<int>(n) && bench.b.write_ends == static_cast<int>(n) && bench.b.begins_by_code.size() == n,
            "product run: one begin, data and END each, and no other code");
        check(bench.a.write_begins == 0 && bench.a.write_datas == 0 && bench.a.write_ends == 0 && bench.a.forbidden == 0 && bench.b.forbidden == 0, "product run: A is only read");
        check(bench.a.begins_by_code.empty() && NoResend(bench.b) && !bench.access.overlap, "product run: nothing to A, no resend, never both open");
        // The order of the writes is the table's order.
        std::vector<std::uint16_t> order;
        for (const auto& entry : bench.b.trace)
            if (entry.size() == 10 && entry.compare(0, 5, "1016(") == 0 && entry[9] == ')') order.push_back(static_cast<std::uint16_t>(std::stoul(entry.substr(5, 4), nullptr, 16)));
        std::vector<std::uint16_t> expected_order;
        for (const auto& write : kExpectedProductWrites) expected_order.push_back(write.code);
        check(order == expected_order, "product run: the write order is the table's");
        const auto journal = bench.Journal();
        check(Contains(journal, "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040") &&
                Contains(journal, "ev=step i=1 alias=CAM-B item=shutter_speed code=0x500d before=0x0000014d value=0x00000683") &&
                Contains(journal, "ev=step i=2 alias=CAM-B item=aperture code=0x5007 before=0x0230 value=0x0320") &&
                Contains(journal, "ev=step i=3 alias=CAM-B item=white_balance code=0x5005 before=0x0002 value=0x0004") &&
                Contains(journal, "ev=step i=4 alias=CAM-B item=metering code=0x500b before=0x0003 value=0x0002"),
            "product run: the record names each step");
        check(Contains(journal, "verdict=match sent=5") && !HasIdentifierShape(journal), "product run: the record ends with a match");
        check(bench.controller->CaptureAllowed() && bench.Restarted().CaptureAllowed(), "product run: nothing held");
    }
    {
        // A failure or an unknown end at any item: nothing after it is sent, nothing is resent, capture is held across a restart.
        for (std::size_t k = 0; k < kExpectedProductWrites.size(); ++k) {
            for (const bool unknown : {true, false}) {
                Bench bench(kProduct);
                PrepareProductBench(bench);
                const auto& failing = kExpectedProductWrites[k];
                if (unknown) bench.b.begin_reject.insert(failing.code);
                else bench.b.apply_as[failing.code] = failing.other_value;
                const auto run = bench.CopyRun();
                const std::string n = std::string("product run, item ") + std::to_string(k) + (unknown ? " unknown" : " mismatch");
                check(run.verdict == (unknown ? D810WriteVerdict::Unknown : D810WriteVerdict::Mismatch) && run.sent == k + 1, (n + ": stops there").c_str());
                bool later_sent = false;
                for (std::size_t later = k + 1; later < kExpectedProductWrites.size(); ++later) later_sent |= bench.b.begins_by_code.count(kExpectedProductWrites[later].code) != 0;
                check(!later_sent && NoResend(bench.b) && bench.a.write_begins == 0, (n + ": nothing after it, no resend, nothing to A").c_str());
                const auto expected = unknown ? D810CaptureBlock::Unknown : D810CaptureBlock::Mismatch;
                check(!bench.controller->CaptureAllowed() && bench.controller->Block() == expected && bench.Restarted().Block() == expected, (n + ": capture held, also after a restart").c_str());
            }
        }
    }
    {
        // A body that reports any other firmware version is not written.
        Bench bench(kProduct);
        const auto run = bench.CopyRun();
        check(run.sent == 0 && run.reason == "body_not_verified:CAM-B" && bench.Writes() == 0, "product run: another firmware, nothing sent");
    }
}
// White balance and metering through the product table: the values a copy cannot carry are never written, and are listed with
// their reason; the rest of the plan stands. The release of a capture block compares seven items.
void ProductRevisionItems() {
    using Item = D810WriteItem;
    const auto skipped_reason = [](const D810PlanResult& plan, Item item) {
        if (!plan.Ok()) return std::string("(no plan)");
        for (const auto& [skipped, reason] : plan.plan->skipped)
            if (skipped == item) return reason;
        return std::string("(not listed)");
    };
    const auto steps_of = [](const D810PlanResult& plan) {
        std::vector<Item> items;
        if (plan.Ok()) for (const auto& step : plan.plan->steps) items.push_back(step.item);
        return items;
    };
    // The kinds that lean on content held in the body: never copied, whatever B reads, and never sent.
    for (const std::int64_t value : {0x0001LL, 0x0003LL, 0x8012LL, 0x8013LL}) {
        for (const std::int64_t on_b : {2LL, value}) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.a.props[0x5005].current = value;
            bench.b.props[0x5005].current = on_b;
            const auto plan = bench.Copy();
            const std::string n = "white balance " + D810Hex(static_cast<std::uint64_t>(value), 4) + " on A" + (on_b == value ? ", the same on B" : "");
            check(plan.Ok() && skipped_reason(plan, Item::WhiteBalance) == "value_content_dependent", (n + ": listed as content dependent").c_str());
            check(steps_of(plan) == ExpectedProductStepsWithout({Item::WhiteBalance}), (n + ": the other items stay in the plan").c_str());
            const auto run = bench.Run(plan);
            check(run.verdict == D810WriteVerdict::Match && bench.b.begins_by_code.count(0x5005) == 0 && bench.b.props[0x5005].current == on_b && bench.a.write_begins == 0,
                (n + ": never written, B keeps its own value").c_str());
        }
    }
    {
        // A value the item does not name.
        for (const std::int64_t value : {0LL, 8LL, 0x8014LL, 0x8016LL, 0xffffLL}) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.a.props[0x5005].current = value;
            const auto plan = bench.Copy();
            check(plan.Ok() && skipped_reason(plan, Item::WhiteBalance) == "value_not_listed" && steps_of(plan) == ExpectedProductStepsWithout({Item::WhiteBalance}), "white balance: a value without a name is listed, not written");
        }
        for (const std::int64_t value : {0LL, 5LL, 0x8010LL, 0xffffLL}) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.a.props[0x500b].current = value;
            const auto plan = bench.Copy();
            check(plan.Ok() && skipped_reason(plan, Item::Metering) == "value_not_listed" && steps_of(plan) == ExpectedProductStepsWithout({Item::Metering}), "metering: a mode outside 1 to 4 is listed, not written");
        }
    }
    {
        // The kinds a copy may carry are all written (B advertises them): Automatic and Fluorescent stay in the list.
        for (const std::int64_t value : {2LL, 4LL, 5LL, 6LL, 7LL, 0x8010LL}) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.b.props[0x5005].current = value == 2 ? 4 : 2;
            bench.a.props[0x5005].current = value;
            const auto run = bench.CopyRun();
            check(run.verdict == D810WriteVerdict::Match && bench.b.props[0x5005].current == value && bench.b.begins_by_code[0x5005] == 1, "white balance: a named kind is written once");
        }
        for (const std::int64_t value : {1LL, 2LL, 3LL, 4LL}) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.b.props[0x500b].current = value == 3 ? 2 : 3;
            bench.a.props[0x500b].current = value;
            const auto run = bench.CopyRun();
            check(run.verdict == D810WriteVerdict::Match && bench.b.props[0x500b].current == value && bench.b.begins_by_code[0x500b] == 1, "metering: modes 1 to 4 are written once");
        }
    }
    {
        // B does not advertise A's value: only that item is left out, with its reason; the rest of the plan stands.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x5005].values = {2, 5};
        bench.b.props[0x500b].values = {1, 3};
        const auto plan = bench.Copy();
        check(plan.Ok() && skipped_reason(plan, Item::WhiteBalance) == "value_not_advertised" && skipped_reason(plan, Item::Metering) == "value_not_advertised" &&
                steps_of(plan) == ExpectedProductStepsWithout({Item::WhiteBalance, Item::Metering}),
            "not advertised by B: left out with the reason, the plan stands");
        // B cannot write them, or answers another type, or cannot be read.
        Bench other(kProduct);
        PrepareProductBench(other);
        other.b.props[0x5005].get_set = 0;
        other.b.props[0x500b].type = 6;
        const auto plan2 = other.Copy();
        check(plan2.Ok() && skipped_reason(plan2, Item::WhiteBalance) == "not_writable" && skipped_reason(plan2, Item::Metering) == "unreadable" && steps_of(plan2) == ExpectedProductStepsWithout({Item::WhiteBalance, Item::Metering}),
            "not writable or another type: left out, the plan stands");
    }
    {
        // The release of a capture block compares seven items: white balance and metering hold a block.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.apply_as[0x5005] = 5;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Mismatch && run.sent == 4 && bench.b.begins_by_code.count(0x500b) == 0, "release: the white balance mismatch stops the run");
        // B reads A's exposure items now, white balance and metering still differ.
        bench.b.props[0x500f].current = 64;
        bench.b.props[0x500d].current = 1667;
        bench.b.props[0x5007].current = 800;
        const auto first = bench.controller->Reread(bench.access, bench.options);
        check(!first.check.released && first.check.differences == std::vector<std::string>{"white_balance", "metering"} && !bench.controller->CaptureAllowed(),
            "release: white balance and metering are compared and hold the block");
        bench.b.props[0x5005].current = 4;
        const auto second = bench.controller->Reread(bench.access, bench.options);
        check(!second.check.released && second.check.differences == std::vector<std::string>{"metering"}, "release: metering alone still holds it");
        bench.b.props[0x500b].current = 2;
        const auto third = bench.controller->Reread(bench.access, bench.options);
        check(third.check.released && bench.controller->CaptureAllowed() && bench.Restarted().CaptureAllowed(), "release: all seven read the same, the block is released");
        // A Preset on A is unknown, never a match: the release stays held while the colors cannot be compared.
        bench.a.props[0x5005].current = 0x8013;
        bench.b.props[0x5005].current = 0x8013;
        bench.b.apply_as.clear();
        const auto preset = bench.controller->Reread(bench.access, bench.options);
        check(!preset.check.released && preset.check.differences == std::vector<std::string>{"white_balance:content_dependent"}, "release: a Preset is unknown, not a match");
    }
}
// Aperture through the product table: a differing aperture is written to CAM-B's own code, and a value CAM-B does not offer
// refuses the whole plan (as for the other exposure items), with nothing sent.
void ProductAperture() {
    const auto has_reason = [](const D810PlanResult& plan, const char* token) {
        return std::find(plan.reasons.begin(), plan.reasons.end(), token) != plan.reasons.end();
    };
    {
        Bench bench(kProduct);
        PrepareProductBench(bench);
        const auto plan = bench.Copy();
        const auto step = plan.Ok() ? std::find_if(plan.plan->steps.begin(), plan.plan->steps.end(), [](const auto& s) { return s.item == D810WriteItem::Aperture; }) : decltype(plan.plan->steps.end()){};
        check(plan.Ok() && step != plan.plan->steps.end() && step->camera == D810Camera::B && step->code == 0x5007 && step->before == 560 && step->value == 800, "aperture: planned on CAM-B's code, B's value now and A's value");
    }
    {
        // The same aperture on both: no step, no write to the code.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x5007].current = 800;
        const auto plan = bench.Copy();
        check(plan.Ok() && PlanStepItems(plan) == ExpectedProductStepsWithout({D810WriteItem::Aperture}), "aperture: equal on both, no step");
        check(bench.Run(plan).verdict == D810WriteVerdict::Match && bench.b.begins_by_code.count(0x5007) == 0, "aperture: equal on both, nothing sent to the code");
    }
    {
        // B does not offer A's value: refused, and nothing is sent.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x5007].values = {400, 560, 1100, 1600};
        const auto plan = bench.Copy();
        check(!plan.Ok() && has_reason(plan, "value_not_advertised:aperture:CAM-B"), "aperture: a value B does not advertise refuses the plan");
        check(bench.Run(plan).sent == 0 && bench.Writes() == 0 && !bench.controller->Status().plan_pending, "aperture: refused, nothing sent");
    }
    {
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x5007].get_set = 0;
        const auto plan = bench.Copy();
        check(!plan.Ok() && has_reason(plan, "not_writable:aperture:CAM-B") && bench.Writes() == 0, "aperture: B cannot write it, the plan is refused");
    }
    {
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x5007].type = 6;
        const auto plan = bench.Copy();
        check(!plan.Ok() && has_reason(plan, "unreadable:aperture:CAM-B") && bench.Writes() == 0, "aperture: B answers another type, the plan is refused");
        Bench from_a(kProduct);
        PrepareProductBench(from_a);
        from_a.a.props[0x5007].type = 6;
        const auto plan_a = from_a.Copy();
        check(!plan_a.Ok() && has_reason(plan_a, "unreadable:aperture:CAM-A") && from_a.Writes() == 0, "aperture: A cannot be read, the plan is refused");
    }
    {
        // B stops offering the value after the plan was made: nothing is sent.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        const auto plan = bench.Copy();
        bench.b.props[0x5007].values = {400, 560, 1100, 1600};
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::NotSent && run.reason.rfind("plan_changed:CAM-B", 0) == 0 && bench.Writes() == 0, "aperture: no longer offered after the plan, nothing sent");
    }
}
// Shutter speed through the product table: written to 0x500D only, never Bulb, Time, zero or longer than 1 s, and nothing is
// ever sent to 0xD100 (its encoding has no source, so it cannot be in the table).
void ProductShutter() {
    const auto has_reason = [](const D810PlanResult& plan, const char* token) {
        return std::find(plan.reasons.begin(), plan.reasons.end(), token) != plan.reasons.end();
    };
    {
        Bench bench(kProduct);
        PrepareProductBench(bench);
        const auto plan = bench.Copy();
        const auto step = plan.Ok() ? std::find_if(plan.plan->steps.begin(), plan.plan->steps.end(), [](const auto& s) { return s.item == D810WriteItem::ShutterSpeed; }) : decltype(plan.plan->steps.end()){};
        check(plan.Ok() && step != plan.plan->steps.end() && step->camera == D810Camera::B && step->code == 0x500d && step->before == 333 && step->value == 1667,
            "shutter speed: planned on CAM-B's code 0x500D, B's value now and A's value");
    }
    {
        // A's shutter speed that must never be copied: the plan is not made and nothing is sent.
        struct Case { const char* what; std::int64_t value; const char* reason; };
        const std::vector<Case> cases{
            {"Bulb", 0xffffffffLL, "a_shutter_bulb_or_time"}, {"Time", 0xfffffffdLL, "a_shutter_bulb_or_time"}, {"zero", 0, "a_shutter_exposure_zero"},
            {"just over 1 s", 10001, "a_shutter_exposure_too_long"}, {"20 s", 200000, "a_shutter_exposure_too_long"},
        };
        for (const auto& c : cases) {
            Bench bench(kProduct);
            PrepareProductBench(bench);
            bench.a.props[0x500d].current = c.value;
            const auto plan = bench.Copy();
            check(!plan.Ok() && has_reason(plan, c.reason) && bench.Writes() == 0 && bench.b.begins_by_code.count(0x500d) == 0, (std::string("shutter speed on A, ") + c.what + ": no plan, nothing sent").c_str());
        }
    }
    {
        // 1 s itself is the longest time that may be written.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.a.props[0x500d].current = 10000;
        const auto run = bench.CopyRun();
        check(run.verdict == D810WriteVerdict::Match && bench.b.props[0x500d].current == 10000 && bench.b.begins_by_code[0x500d] == 1, "shutter speed: 1 s is written once");
    }
    {
        // A time B does not offer: the whole plan is refused (a value is never replaced by another one).
        Bench bench(kProduct);
        PrepareProductBench(bench);
        bench.b.props[0x500d].values = {125, 250, 333, 500, 1000};
        const auto plan = bench.Copy();
        check(!plan.Ok() && has_reason(plan, "value_not_advertised:shutter_speed:CAM-B") && bench.Writes() == 0, "shutter speed: a value B does not advertise refuses the plan");
        Bench other(kProduct);
        PrepareProductBench(other);
        other.b.props[0x500d].get_set = 0;
        check(!other.Copy().Ok() && other.Writes() == 0, "shutter speed: B cannot write it, the plan is refused");
    }
    {
        // A's shutter speed turns into Bulb after the plan was made: nothing is sent.
        Bench bench(kProduct);
        PrepareProductBench(bench);
        const auto plan = bench.Copy();
        bench.a.props[0x500d].current = 0xffffffffLL;
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::NotSent && run.reason.rfind("plan_changed:CAM-A", 0) == 0 && bench.Writes() == 0, "shutter speed: A changed after the plan, nothing sent");
    }
    {
        // The candidate code 0xD100 cannot be confirmed: a product table that names it is invalid, in any camera's row.
        for (const auto camera : {D810Camera::A, D810Camera::B}) {
            auto bad = kD810ProductWriteTable;
            for (auto& row : bad.rows) if (row.item == D810WriteItem::ShutterSpeed && row.camera == camera) row.code = 0xd100;
            check(D810WriteTableProblem(bad) != nullptr && D810ControllerTableProblem(bad, false) != nullptr, "shutter speed: a table with 0xD100 is invalid");
        }
        auto a_row = kD810ProductWriteTable;
        for (auto& row : a_row.rows) if (row.item == D810WriteItem::ShutterSpeed && row.camera == D810Camera::A) row.enabled = true;
        check(std::string(D810ProductTableProblem(a_row)) == "cam_a_version_unconfirmed", "shutter speed: CAM-A's row needs CAM-A's version in the table");
        a_row.verified_firmware_a = "1.00";
        check(std::string(D810ProductTableProblem(a_row)) == "cam_a_row_enabled", "shutter speed: CAM-A's row cannot be enabled");
    }
}
// The check of the body at the plan's read (not only at the send): a CAM-B that is not the model and firmware the
// table names gets no plan and no confirmation. The log says which comparison failed, as booleans only.
void PlanReadChecksTheBody() {
    const auto joined = [](const std::vector<std::string>& log) {
        std::string all;
        for (const auto& line : log) all += line + "\n";
        return all;
    };
    {
        // A matching body: the read completes, and the log says both comparisons held.
        Bench bench;
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(read.complete && read.reason.empty(), "plan read: a matching body completes");
        check(Contains(joined(bench.log), "CAM-B settings_write_body_check model_match=1 firmware_match=1 setting_writes=0"), "plan read: both comparisons held");
    }
    // The table of this bench names the body; the bodies below report that, a different one, or nothing readable.
    const std::string model = kAdr38Table.verified_model, firmware = kAdr38Table.verified_firmware, other = "other-text";
    struct Case { const char* what; std::string model; std::string firmware; const char* expect; };
    const std::vector<Case> cases{
        {"firmware differs", model, other, "model_match=1 firmware_match=0"},
        {"model differs", other, firmware, "model_match=0 firmware_match=1"},
        {"both differ", other, other, "model_match=0 firmware_match=0"},
        {"model could not be read", "", firmware, "model_match=0 firmware_match=1"},
        {"firmware could not be read", model, "", "model_match=1 firmware_match=0"},
    };
    for (const auto& c : cases) {
        Bench bench;
        bench.access.model[1] = c.model;
        bench.access.firmware[1] = c.firmware;
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        const std::string n = c.what;
        check(!read.complete && read.reason == "body_not_verified:CAM-B", (n + ": no plan read, the reason names the body").c_str());
        check(bench.a.read_begins > 0 && bench.b.read_begins == 0 && bench.b.trace.empty(), (n + ": CAM-A was read, nothing reached CAM-B").c_str());
        check(bench.access.opens[1] == 1 && bench.access.closes[1] == 1 && !bench.access.overlap && bench.Writes() == 0, (n + ": CAM-B opened and closed once, nothing written").c_str());
        const auto log = joined(bench.log);
        check(Contains(log, std::string("CAM-B settings_write_body_check ") + c.expect + " setting_writes=0"), (n + ": the log says which comparison failed").c_str());
        // Booleans only: neither the table's strings nor the body's are in the log.
        check(!Contains(log, model) && !Contains(log, firmware) && !Contains(log, other) && !HasIdentifierShape(log), (n + ": the log has no model or version").c_str());
        check(!bench.controller->Status().plan_pending && bench.controller->CaptureAllowed(), (n + ": no plan, no block").c_str());
    }
    {
        // CAM-A is only read: its model and version do not stop the plan's read, and are not logged.
        Bench bench;
        bench.access.model[0] = other;
        bench.access.firmware[0] = other;
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(read.complete && !Contains(joined(bench.log), "CAM-A settings_write_body_check"), "plan read: CAM-A's version does not matter");
    }
    {
        // A table that enables a CAM-A row: the log says how CAM-A's body compared (booleans only), and the read goes on.
        Bench bench(kChosenTable);
        auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(read.complete && read.verified[0] && read.verified[1] &&
                Contains(joined(bench.log), "CAM-A settings_write_body_check model_match=1 firmware_match=1 setting_writes=0"),
            "plan read: CAM-A's comparison held");
        Bench other(kChosenTable);
        other.access.firmware[0] = "other-text";
        read = other.controller->ReadForPlan(other.access, other.options);
        const auto log = joined(other.log);
        check(read.complete && !read.verified[0] && read.verified[1] && other.Writes() == 0 &&
                Contains(log, "CAM-A settings_write_body_check model_match=1 firmware_match=0 setting_writes=0") && !Contains(log, "other-text") && !Contains(log, "1.00"),
            "plan read: CAM-A's version differs, reported and not stopped");
        other.access.model[0] = "";
        other.log.clear();
        other.controller->ReadForPlan(other.access, other.options);
        check(Contains(joined(other.log), "CAM-A settings_write_body_check model_match=0 firmware_match=0 setting_writes=0"), "plan read: CAM-A's model unreadable");
        // No CAM-A row enabled (the product's shape): nothing about CAM-A is reported.
        Bench none(kAdr38Table);
        none.controller->ReadForPlan(none.access, none.options);
        check(!Contains(joined(none.log), "CAM-A settings_write_body_check"), "plan read: no CAM-A row, no CAM-A line");
    }
    {
        // The reread releases a block whatever the bodies report: it is read-only and is not gated by the version.
        Bench bench(kAdr38Table, -1, true);
        bench.b = Body(1667, 800, 64); // the two bodies read the same
        bench.access.firmware[1] = other;
        const auto result = bench.controller->Reread(bench.access, bench.options);
        check(result.complete && result.check.released && bench.b.read_begins > 0, "reread: not gated by the version");
    }
    {
        // The product entry: a body whose version is not the table's gets no plan.
        Bench bench(kProduct);
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(!read.complete && read.reason == "body_not_verified:CAM-B" && bench.b.read_begins == 0, "product entry: another version, no plan read");
        bench.access.firmware[1] = kD810ProductWriteTable.verified_firmware;
        check(bench.controller->ReadForPlan(bench.access, bench.options).complete, "product entry: the table's version completes");
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
// The step lines of a journal, in order, as "CAM-x item".
std::vector<std::string> JournalStepOrder(const std::string& journal) {
    std::vector<std::string> out;
    for (const auto line : SplitD810RecordLines(journal)) {
        const auto fields = SplitD810Fields(line);
        if (fields.size() == 9 && fields[2] == "ev=step") out.push_back(std::string(fields[4].substr(6)) + " " + std::string(fields[5].substr(5)));
    }
    return out;
}
using BeginOrder = std::vector<std::pair<int, int>>;
// The plan of picked values takes the controller's own last read: the read it is handed is only a number.
void ChosenReadToken() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        Bench bench(kChosenTable);
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(read.complete && read.id != 0 && read.verified == D810BodyFlags{true, true}, "token: a complete read has a number");
        check(bench.controller->PlanChosen(read, {PickFor(B, Item::Iso, 200)}).Ok(), "token: the last read plans");
        check(bench.controller->PlanChosen(read, {PickFor(B, Item::Iso, 400)}).Ok(), "token: and plans again");
        // An older read is refused once another is made.
        const auto newer = bench.controller->ReadForPlan(bench.access, bench.options);
        const auto old_plan = bench.controller->PlanChosen(read, {PickFor(B, Item::Iso, 200)});
        check(!old_plan.Ok() && HasReason(old_plan, "read_not_current") && old_plan.reasons.size() == 1, "token: an older read is refused");
        check(!bench.controller->Status().plan_pending, "token: a refused plan leaves nothing pending");
        check(bench.controller->PlanChosen(newer, {PickFor(B, Item::Iso, 200)}).Ok(), "token: the newer read plans");
        // Not a read of this controller: nothing, a number made up, a read of another controller (numbers are unique over the process).
        D810PlanRead blank;
        blank.complete = true;
        check(HasReason(bench.controller->PlanChosen(blank, {PickFor(B, Item::Iso, 200)}), "read_not_current"), "token: a read with no number");
        auto made_up = newer;
        made_up.id += 1000;
        check(HasReason(bench.controller->PlanChosen(made_up, {PickFor(B, Item::Iso, 200)}), "read_not_current"), "token: a made-up number");
        Bench other(kChosenTable);
        const auto elsewhere = other.controller->ReadForPlan(other.access, other.options);
        check(elsewhere.id != newer.id && HasReason(bench.controller->PlanChosen(elsewhere, {PickFor(B, Item::Iso, 200)}), "read_not_current"), "token: a read of another controller");
        // A read that did not complete is not kept (CAM-B is not the table's), and it makes the earlier one old.
        other.access.firmware[1] = "other-text";
        const auto partial = other.controller->ReadForPlan(other.access, other.options);
        check(!partial.complete && HasReason(other.controller->PlanChosen(partial, {PickFor(B, Item::Iso, 200)}), "read_not_current") &&
                HasReason(other.controller->PlanChosen(elsewhere, {PickFor(B, Item::Iso, 200)}), "read_not_current"),
            "token: an incomplete read plans nothing and replaces the last");
        check(bench.Writes() == 0 && other.Writes() == 0, "token: nothing sent");
    }
    {
        // What the caller holds is not trusted: the flags and the snapshots are the controller's. CAM-A's body is not the table's here.
        Bench bench(kChosenTable);
        bench.access.firmware[0] = "other-text";
        auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(read.complete && read.verified == D810BodyFlags{false, true}, "forged: CAM-A is reported as not verified");
        read.verified = {true, true};
        const auto forged = bench.controller->PlanChosen(read, {PickFor(A, Item::Iso, 200)});
        check(!forged.Ok() && HasReason(forged, "body_not_verified:CAM-A"), "forged: flags the caller sets are not used");
        // Snapshots the caller sets are not used: CAM-A is made to read the value that the pick would not change.
        read.snapshots[0] = Snapshot(bench.a, "CAM-A");
        bench.access.firmware[0] = "1.00";
        auto good = bench.controller->ReadForPlan(bench.access, bench.options);
        const auto real = bench.controller->PlanChosen(good, {PickFor(A, Item::Iso, 200)});
        check(real.Ok() && real.plan->steps.size() == 1 && real.plan->steps[0].before == 64, "forged: the plan is made from the controller's read");
        good.snapshots[0] = Snapshot(bench.a, "CAM-A");
        for (auto& reading : good.snapshots[0])
            if (reading.id == D810SettingId::Iso) reading.desc.current_value = 200;
        const auto tampered = bench.controller->PlanChosen(good, {PickFor(A, Item::Iso, 200)});
        check(tampered.Ok() && tampered.plan->steps.size() == 1 && tampered.plan->steps[0].before == 64, "forged: snapshots the caller sets are not used");
    }
    {
        // A run and a reread make a read old: the cameras changed after it.
        Bench bench(kChosenTable);
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        const auto plan = bench.controller->PlanChosen(read, {PickFor(B, Item::Iso, 200)});
        check(plan.Ok() && bench.Run(plan).verdict == D810WriteVerdict::Match, "stale: a run");
        check(HasReason(bench.controller->PlanChosen(read, {PickFor(B, Item::Iso, 400)}), "read_not_current"), "stale: the read before a run is old");
        const auto second = bench.controller->ReadForPlan(bench.access, bench.options);
        check(bench.controller->PlanChosen(second, {PickFor(B, Item::Iso, 400)}).Ok(), "stale: a new read plans");
        bench.controller->Reread(bench.access, bench.options);
        check(HasReason(bench.controller->PlanChosen(second, {PickFor(B, Item::Iso, 400)}), "read_not_current"), "stale: the read before a reread is old");
        // The copy's way in is not changed.
        check(bench.Copy().Ok(), "stale: the copy plan still takes two snapshots");
    }
}
void ChosenRuns() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        // The key to a CAM-A session is an own guard: a plan of picked values, the switch, a CAM-A firmware version and the
        // enabled row of every CAM-A step. The switch alone does not open it.
        Bench bench(kChosenTable);
        const auto made = bench.Chosen({PickFor(B, Item::Iso, 400), PickFor(A, Item::Iso, 200), PickFor(A, Item::Metering, 4)});
        check(made.Ok(), "a plan with two CAM-A steps");
        const auto& plan = *made.plan;
        check(D810CamAKeyOpen(plan, kChosenTable), "the key opens for a plan the table lets through");
        auto no_switch = kChosenTable;
        no_switch.chosen_values = false;
        check(!D810CamAKeyOpen(plan, no_switch), "no key without the switch");
        auto no_version = kChosenTable;
        no_version.verified_firmware_a = nullptr;
        check(!D810CamAKeyOpen(plan, no_version), "no key without a CAM-A version");
        no_version.verified_firmware_a = "";
        check(!D810CamAKeyOpen(plan, no_version), "no key with an empty CAM-A version");
        for (const auto item : {Item::Iso, Item::Metering}) {
            auto row_off = kChosenTable;
            for (auto& row : row_off.rows)
                if (row.camera == A && row.item == item) row.enabled = false;
            check(!D810CamAKeyOpen(plan, row_off), "no key while the row of a CAM-A step is disabled");
        }
        auto other_row_off = kChosenTable;
        for (auto& row : other_row_off.rows)
            if (row.camera == A && row.item == Item::Aperture) row.enabled = false;
        check(D810CamAKeyOpen(plan, other_row_off), "a row nobody plans does not matter");
        auto copy = plan;
        copy.source = D810PlanSource::CopyAToB;
        check(!D810CamAKeyOpen(copy, kChosenTable), "no key for a copy");
        // The two conditions on their own: a plan with no CAM-A step has no row to be refused by, so only the plan's
        // kind and the switch can close the key for it.
        Bench second(kChosenTable); // a plan made on the first bench would replace the one to be run below
        const auto b_only = second.Chosen({PickFor(B, Item::Iso, 400)});
        check(b_only.Ok() && D810CamAKeyOpen(*b_only.plan, kChosenTable), "a plan with no CAM-A step: the key follows the kind, the switch and the version");
        auto b_copy = *b_only.plan;
        b_copy.source = D810PlanSource::CopyAToB;
        check(!D810CamAKeyOpen(b_copy, kChosenTable), "no key for a copy, whatever its steps");
        check(!D810CamAKeyOpen(*b_only.plan, no_switch), "no key without the switch, whatever the steps");
        check(!D810CamAKeyOpen(*b_only.plan, no_version), "no key without a CAM-A version, whatever the steps");
        // And in a run: the CAM-A steps go out with the key.
        const auto run = bench.Run(made);
        check(run.verdict == D810WriteVerdict::Match && run.sent == 3 && bench.a.write_begins == 2, "the run sends the CAM-A steps");
    }
    {
        // Both cameras, different values: every CAM-B item first, CAM-A last, once each, on its own body.
        Bench bench(kChosenTable);
        bench.TraceBegins();
        const auto plan = bench.Chosen({PickFor(A, Item::Iso, 200), PickFor(B, Item::Iso, 400), PickFor(B, Item::WhiteBalance, 4), PickFor(A, Item::Metering, 4), PickFor(B, Item::Aperture, 1100)});
        check(plan.Ok() && plan.plan->steps.size() == 5, "five steps");
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::Match && run.sent == 5 && run.reason.empty(), "all matched");
        check(bench.begin_order == BeginOrder{{1, 0x500f}, {1, 0x5007}, {1, 0x5005}, {0, 0x500f}, {0, 0x500b}}, "the writes reached CAM-B's three items first, then CAM-A's two");
        check(bench.a.props[0x500f].current == 200 && bench.b.props[0x500f].current == 400, "each body got its own ISO");
        check(bench.b.props[0x5007].current == 1100 && bench.b.props[0x5005].current == 4 && bench.a.props[0x500b].current == 4, "and its own other items");
        check(bench.a.props[0x5007].current == 800 && bench.a.props[0x5005].current == 2 && bench.b.props[0x500b].current == 3 && bench.a.props[0x500d].current == 1667 && bench.b.props[0x500d].current == 333,
            "nothing else changed");
        check(!bench.access.overlap && NoResend(bench.a) && NoResend(bench.b) && bench.a.forbidden == 0 && bench.b.forbidden == 0, "one at a time, once, nothing else");
        check(bench.a.write_begins == 2 && bench.b.write_begins == 3 && bench.a.write_datas == 2 && bench.b.write_datas == 3 && bench.a.write_ends == 2 && bench.b.write_ends == 3,
            "a begin, data and END per step");
        check(bench.access.opens[0] == 2 && bench.access.opens[1] == 2, "each camera: one verification read and one write session");
        const auto journal = bench.Journal();
        check(JournalStepOrder(journal) == std::vector<std::string>{"CAM-B iso", "CAM-B aperture", "CAM-B white_balance", "CAM-A iso", "CAM-A metering"}, "the record lists the steps in the write order");
        check(Contains(journal, " source=chosen steps=5\n"), "the record says the plan was chosen");
        const auto state = ParseD810Journal(journal);
        check(state.block == D810CaptureBlock::None && state.operations == 1 && state.malformed_lines == 0, "the record reads clean");
        check(state.expected.size() == 5 && state.chosen_unresolved, "the record keeps what was planned");
        check(bench.controller->CaptureAllowed() && bench.Restarted().CaptureAllowed(), "CAM-A and CAM-B differ and capture is allowed: the cameras are not compared");
    }
    {
        // "Same as CAM-A": CAM-B takes CAM-A's pick; B is written, then A.
        Bench bench(kChosenTable);
        bench.TraceBegins();
        const auto run = bench.Run(bench.Chosen({PickFor(A, Item::Iso, 200), FollowA(Item::Iso)}));
        check(run.verdict == D810WriteVerdict::Match && run.sent == 2, "matched");
        check(bench.a.props[0x500f].current == 200 && bench.b.props[0x500f].current == 200, "both bodies read 200");
        check(bench.begin_order == BeginOrder{{1, 0x500f}, {0, 0x500f}}, "B first");
    }
    {
        // CAM-A alone and CAM-B alone: the other camera is not even read.
        Bench bench(kChosenTable);
        const auto run = bench.Run(bench.Chosen({PickFor(A, Item::Iso, 200)}));
        check(run.verdict == D810WriteVerdict::Match && run.sent == 1 && bench.a.write_begins == 1 && bench.b.write_begins == 0, "CAM-A alone");
        check(bench.access.opens[1] == 0, "CAM-B was not opened");
        Bench second(kChosenTable);
        const auto only_b = second.Run(second.Chosen({PickFor(B, Item::Iso, 200)}));
        check(only_b.verdict == D810WriteVerdict::Match && only_b.sent == 1 && second.b.write_begins == 1 && second.a.write_begins == 0 && second.access.opens[0] == 0, "CAM-B alone, CAM-A not opened");
    }
    {
        // A CAM-B item fails (the body keeps the old value): the rest of CAM-B and all of CAM-A get nothing.
        Bench bench(kChosenTable);
        bench.TraceBegins();
        bench.b.apply_as[0x5007] = 560;
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(B, Item::Aperture, 1100), PickFor(B, Item::WhiteBalance, 4), PickFor(A, Item::Iso, 200)}));
        check(run.verdict == D810WriteVerdict::Mismatch && run.reason == "readback_mismatch" && run.sent == 2, "mismatch on CAM-B's second item");
        check(bench.begin_order == BeginOrder{{1, 0x500f}, {1, 0x5007}}, "nothing after the failed item");
        check(bench.b.begins_by_code.count(0x5005) == 0 && bench.a.write_begins == 0 && bench.access.opens[0] == 1, "CAM-A was only read, before the first send");
        check(NoResend(bench.b) && bench.b.write_begins == 2, "no resend, no write back");
        check(bench.b.props[0x500f].current == 200, "the first item stays written");
        check(!bench.controller->CaptureAllowed() && bench.controller->Block() == D810CaptureBlock::Mismatch && bench.Restarted().Block() == D810CaptureBlock::Mismatch, "capture is held, also after a restart");
    }
    {
        // A CAM-B item ends unknown: nothing more to either camera.
        Bench bench(kChosenTable);
        bench.b.end_reject[0x5007] = 0x2002;
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(B, Item::Aperture, 1100), PickFor(A, Item::Iso, 200)}));
        check(run.verdict == D810WriteVerdict::Unknown && run.sent == 2 && run.reason == "result_unknown", "unknown on CAM-B");
        check(bench.a.write_begins == 0 && bench.access.opens[0] == 1 && bench.b.write_begins == 2, "CAM-A gets nothing");
        check(bench.controller->Block() == D810CaptureBlock::Unknown, "capture is held");
    }
    {
        // CAM-B is written and matches, CAM-A's first item fails: CAM-A's rest gets nothing and CAM-B is not written back.
        Bench bench(kChosenTable);
        bench.TraceBegins();
        bench.a.apply_as[0x500f] = 400;
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(B, Item::Aperture, 1100), PickFor(A, Item::Iso, 200), PickFor(A, Item::Metering, 4)}));
        check(run.verdict == D810WriteVerdict::Mismatch && run.sent == 3 && run.reason == "readback_mismatch", "mismatch on CAM-A's first item");
        check(bench.begin_order == BeginOrder{{1, 0x500f}, {1, 0x5007}, {0, 0x500f}}, "CAM-A's second item was not sent");
        check(bench.b.props[0x500f].current == 200 && bench.b.props[0x5007].current == 1100 && bench.b.write_begins == 2, "CAM-B keeps what it was given, no write back");
        check(NoResend(bench.a) && NoResend(bench.b) && bench.a.write_begins == 1, "no resend");
        check(bench.controller->Block() == D810CaptureBlock::Mismatch && bench.Restarted().Block() == D810CaptureBlock::Mismatch, "capture is held, also after a restart");
    }
    {
        // CAM-A's version stops being the table's between the plan and the run: nothing is sent, CAM-B included.
        Bench bench(kChosenTable);
        const auto plan = bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)});
        check(plan.Ok(), "a plan while both bodies match");
        bench.access.firmware[0] = "other-text";
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason == "body_not_verified:CAM-A" && bench.Writes() == 0, "stopped before the first send");
    }
    {
        // CAM-A changes after the plan, and CAM-A only supplied a value for "same as CAM-A": the run stops before a send.
        Bench bench(kChosenTable);
        bench.a.props[0x500f].current = 200;
        const auto plan = bench.Chosen({FollowA(Item::Iso)});
        check(plan.Ok() && StepTexts(plan) == std::vector<std::string>{"CAM-B iso 200"}, "B follows A");
        bench.a.props[0x500f].current = 400;
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason == "plan_changed:CAM-A:changed:0x500f" && bench.Writes() == 0, "the value B followed changed");
    }
    {
        // The operation record is written first; when it cannot be, nothing is sent (CAM-A's steps included).
        Bench bench(kChosenTable, 2);
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)}));
        check(run.sent == 0 && run.reason == "record_unwritable" && bench.Writes() == 0, "the plan line and the first step line are written, the second step line is not");
    }
}

// Releasing a capture block after a chosen plan (ADR-0041 decision 1-7): by the planned values, not by comparing the cameras.
void ChosenRelease() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    const auto differences = [](const D810RereadResult& result) { return result.check.differences; };
    {
        // CAM-B reaches its value, CAM-A's write does not take.
        Bench bench(kChosenTable, -1, true); // on disk: the history must come from the record
        bench.a.apply_as[0x500f] = 64;
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::Iso, 400), PickFor(A, Item::Iso, 200)}));
        check(run.verdict == D810WriteVerdict::Mismatch && run.sent == 2 && bench.b.props[0x500f].current == 400 && bench.a.props[0x500f].current == 64, "B written, A did not take");
        const int writes = bench.Writes();
        // 1. Neither camera has its planned value on A.
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.check.released && differences(reread) == std::vector<std::string>{"iso:CAM-A:not_planned_value"} && reread.block == D810CaptureBlock::Mismatch, "CAM-A is not at its planned value");
        // 2. CAM-A and CAM-B read the same, but not the planned values: not released (the cameras are not compared).
        bench.a.props[0x500f].current = 400;
        reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && differences(reread) == std::vector<std::string>{"iso:CAM-A:not_planned_value"} && !bench.controller->CaptureAllowed(), "equal on both cameras is not enough");
        // 3. CAM-B left its planned value.
        bench.a.props[0x500f].current = 200;
        bench.b.props[0x500f].current = 100;
        reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && differences(reread) == std::vector<std::string>{"iso:CAM-B:not_planned_value"}, "CAM-B is not at its planned value");
        // 4. After a restart, with CAM-A and CAM-B at their planned values (different from each other): released, from the record alone.
        bench.b.props[0x500f].current = 400;
        bench.a.props[0x5005].current = 4; // something nobody planned differs between the cameras
        auto restarted = bench.Restarted();
        check(restarted.Block() == D810CaptureBlock::Mismatch && !restarted.CaptureAllowed(), "the restart still blocks");
        reread = restarted.Reread(bench.access, bench.options);
        check(reread.complete && reread.check.released && reread.block == D810CaptureBlock::None && restarted.CaptureAllowed(), "released by the planned values");
        check(bench.Restarted().CaptureAllowed() && bench.Writes() == writes, "the release survives a restart and sent nothing");
        const auto state = ParseD810Journal(bench.Journal());
        check(state.block == D810CaptureBlock::None && !state.chosen_unresolved && state.expected.empty(), "the release closed the history");
        // The release is not a plan: a new chosen plan can be made and starts afresh.
        auto fresh = bench.Restarted();
        const auto fresh_read = fresh.ReadForPlan(bench.access, bench.options);
        check(fresh.PlanChosen(fresh_read, {PickFor(B, Item::Aperture, 1100)}).Ok(), "a new plan can be made");
    }
    {
        // A value that leans on content held in the body is never a match.
        Bench bench(kChosenTable, -1, true);
        bench.b.apply_as[0x5005] = 2;
        const auto run = bench.Run(bench.Chosen({PickFor(B, Item::WhiteBalance, 4)}));
        check(run.verdict == D810WriteVerdict::Mismatch, "white balance did not take");
        bench.b.props[0x5005].current = 0x8013;
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && differences(reread) == std::vector<std::string>{"white_balance:CAM-B:content_dependent"}, "Preset is unknown, not a match");
        bench.b.props[0x5005].current = 4;
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released && bench.controller->CaptureAllowed(), "the planned kind: released");
    }
    {
        // A history that has copies only keeps the old rule: every block item the same on both cameras.
        Bench bench(kChosenTable, -1, true);
        bench.b.apply_as[0x500f] = 200;
        check(bench.CopyRun().verdict == D810WriteVerdict::Mismatch, "a copy that did not take");
        const auto state = ParseD810Journal(bench.Journal());
        check(!state.chosen_unresolved && state.block == D810CaptureBlock::Mismatch, "the history is a copy history");
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && !reread.check.differences.empty() && reread.check.differences[0] == "iso", "the cameras differ");
        bench.b = Body(1667, 800, 64);
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released && bench.controller->CaptureAllowed(), "the same on both: released");
    }
    {
        // A chosen plan whose step lines cannot be read gives no value to check: the strict rule (the cameras equal) applies.
        Bench bench(kChosenTable, -1, true);
        const std::string op = "a0sw1 op=00000000000000aa ";
        WriteFile(bench.file.Path(), op + "ev=plan t=x source=chosen steps=1\n" + op + "ev=step i=0 alias=CAM-C item=iso code=0x500f before=0x0064 value=0x0040\n");
        const auto state = ParseD810Journal(bench.Journal());
        check(state.chosen_unresolved && state.expected.empty() && state.block == D810CaptureBlock::Unconfirmed && state.malformed_lines == 1, "nothing planned could be read");
        bench.b = Body(1667, 800, 100);
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && !reread.check.differences.empty() && reread.check.differences[0] == "iso", "the cameras differ: not released");
        bench.b = Body(1667, 800, 64);
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released, "the cameras agree: released");
    }
    {
        // Part of the record of a chosen history cannot be trusted: the planned values may miss a step that was
        // sent, so the planned values do not release. The cameras are compared with each other, as for copies.
        // Each record leaves one planned value that CAM-B meets (iso 400): by the planned values alone it would be
        // released, while CAM-A reads something else.
        const std::string op1 = "a0sw1 op=00000000000000a1 ", op2 = "a0sw1 op=00000000000000a2 ";
        const auto plan = [](const std::string& op, const char* source, int steps) { return op + "ev=plan t=x source=" + source + " steps=" + std::to_string(steps) + "\n"; };
        const auto step = [](const std::string& op, int i, const char* alias, const char* before, const char* value) {
            return op + "ev=step i=" + std::to_string(i) + " alias=" + alias + " item=iso code=0x500f before=" + before + " value=" + value + "\n";
        };
        const auto result = [](const std::string& op, int i) { return op + "ev=result i=" + std::to_string(i) + " readback=0x0190 verdict=match\n"; };
        const auto end = [](const std::string& op, const char* verdict, int sent) { return op + "ev=end t=x verdict=" + verdict + " sent=" + std::to_string(sent) + " reason=x\n"; };
        const std::string b_step = step(op1, 0, "CAM-B", "0x0064", "0x0190");
        const auto run_case = [&](const char* what, const std::string& text, bool unreadable_line) {
            Bench bench(kChosenTable, -1, true);
            WriteFile(bench.file.Path(), text);
            const auto state = ParseD810Journal(bench.Journal());
            check(state.chosen_unresolved && state.history_damaged && state.block != D810CaptureBlock::None && (state.malformed_lines > 0) == unreadable_line, what);
            check(state.expected.size() == 1 && state.expected[0].camera == D810Camera::B && state.expected[0].value == 400, what);
            bench.a = Body(1667, 800, 64);
            bench.b = Body(1667, 800, 400);
            auto reread = bench.controller->Reread(bench.access, bench.options);
            check(reread.complete && !reread.check.released && !reread.check.differences.empty() && reread.check.differences[0] == "iso" && !bench.controller->CaptureAllowed(), what);
            bench.a = Body(1667, 800, 400);
            reread = bench.controller->Reread(bench.access, bench.options);
            check(reread.check.released && bench.controller->CaptureAllowed() && bench.Restarted().CaptureAllowed(), what);
        };
        // A step line in the middle cannot be read; the result and end lines come after it.
        run_case("a step line in the middle is unreadable",
            plan(op1, "chosen", 3) + b_step + step(op1, 1, "CAM-C", "0x0064", "0x00c8") + step(op1, 2, "CAM-A", "0x0040", "0x00c8") + result(op1, 0) + result(op1, 2) + end(op1, "mismatch", 2), true);
        // The plan line of a later operation cannot be read, so its steps and its end are not taken.
        run_case("the plan line of a later operation is unreadable",
            plan(op1, "chosen", 1) + b_step + end(op1, "mismatch", 1) + plan(op2, "other", 1) + step(op2, 0, "CAM-A", "0x0040", "0x00c8") + end(op2, "mismatch", 1), true);
        // A step line is gone altogether: every line that is left can be read.
        run_case("a step line is missing", plan(op1, "chosen", 2) + b_step + result(op1, 0) + end(op1, "mismatch", 1), false);
        // The last line is a step line cut short; nothing was sent. The two cameras must agree on the block items.
        run_case("the last step line is cut short", plan(op1, "chosen", 2) + b_step + "a0sw1 op=00000000000000a1 ev=step i=1 alias=CAM-A item=iso co", true);
        // An operation without an end line is held to its plan line as well.
        run_case("no end line and a step line is missing", plan(op1, "chosen", 2) + b_step, false);
    }
    {
        // A record that exists but cannot be read releases nothing, whatever the cameras read. A missing record
        // is not that: it compares the cameras, as before.
        Bench bench(kChosenTable, -1, true);
        fs::create_directories(bench.file.Path());
        bench.a = Body(1667, 800, 64);
        bench.b = Body(1667, 800, 64);
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.check.released && reread.check.differences == std::vector<std::string>{"record_unreadable"} && !bench.controller->CaptureAllowed(), "an unreadable record releases nothing");
        Bench absent(kChosenTable, -1, true);
        absent.a = Body(1667, 800, 64);
        absent.b = Body(1667, 800, 64);
        const auto none = absent.controller->Reread(absent.access, absent.options);
        check(none.complete && none.check.released && none.check.differences.empty(), "no record: the cameras are compared");
    }
    {
        // The planned values release only with the camera in M and ISO auto off; a camera with no planned value is not looked at.
        Bench bench(kChosenTable, -1, true);
        bench.b.apply_as[0x500f] = 200;
        check(bench.Run(bench.Chosen({PickFor(B, Item::Iso, 400)})).verdict == D810WriteVerdict::Mismatch, "the pick did not take");
        bench.b.props[0x500f].current = 400;
        bench.b.props[0x500e].current = 3; // P
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.check.released && differences(reread) == std::vector<std::string>{"exposure_mode:CAM-B:not_manual"} && !bench.controller->CaptureAllowed(), "the planned value is there, the mode is not M");
        bench.b.props[0x500e].current = 1;
        bench.b.props[0xd054].current = 1; // ISO auto on
        reread = bench.controller->Reread(bench.access, bench.options);
        check(!reread.check.released && differences(reread) == std::vector<std::string>{"iso_auto:CAM-B:not_off"}, "the planned value is there, ISO auto is not off");
        bench.b.props[0xd054].current = 0;
        bench.a.props[0x500e].current = 3; // CAM-A has no planned value
        bench.a.props[0xd054].current = 1;
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released && bench.controller->CaptureAllowed(), "M and off on the camera with the value: released");
    }
    {
        // The check on its own.
        auto a = Body(1667, 800, 64), b = Body(333, 560, 100);
        const D810ExpectedValue b_iso{B, Item::Iso, 0x500f, 100}, a_iso{A, Item::Iso, 0x500f, 64};
        check(CheckD810ReleasePlanned(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {b_iso, a_iso}, kChosenTable).released, "both at their values");
        check(CheckD810ReleasePlanned(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {D810ExpectedValue{B, Item::Iso, 0x500f, 64}}, kChosenTable).differences == std::vector<std::string>{"iso:CAM-B:not_planned_value"},
            "a camera is compared with its own value, not the other camera's");
        const auto none = CheckD810ReleasePlanned(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {}, kChosenTable);
        check(!none.released && none.differences == std::vector<std::string>{"nothing_planned"}, "nothing planned releases nothing");
        a.props.erase(0x500f);
        check(CheckD810ReleasePlanned(Snapshot(a, "CAM-A"), Snapshot(b, "CAM-B"), {a_iso}, kChosenTable).differences == std::vector<std::string>{"iso:CAM-A:unreadable"}, "unreadable");
        auto c = Body(1667, 800, 64);
        c.props[0x5005].current = 0x8013;
        check(CheckD810ReleasePlanned(Snapshot(c, "CAM-A"), Snapshot(b, "CAM-B"), {D810ExpectedValue{A, Item::WhiteBalance, 0x5005, 0x8013}}, kChosenTable).differences == std::vector<std::string>{"white_balance:CAM-A:content_dependent"},
            "Preset on both sides of the comparison is unknown");
        // The mode and ISO auto of a camera with a planned value: M and off, else not manual / not off, or unreadable.
        auto d = Body(1667, 800, 64);
        d.props[0x500e].current = 2;
        d.props[0xd054].current = 1;
        const D810ExpectedValue d_iso{A, Item::Iso, 0x500f, 64};
        check(CheckD810ReleasePlanned(Snapshot(d, "CAM-A"), Snapshot(b, "CAM-B"), {d_iso}, kChosenTable).differences == std::vector<std::string>{"exposure_mode:CAM-A:not_manual", "iso_auto:CAM-A:not_off"},
            "not M and ISO auto on");
        check(CheckD810ReleasePlanned(Snapshot(d, "CAM-A"), Snapshot(b, "CAM-B"), {D810ExpectedValue{B, Item::Iso, 0x500f, 100}}, kChosenTable).released,
            "a camera with no planned value is not looked at");
        d.props.erase(0x500e);
        d.props.erase(0xd054);
        check(CheckD810ReleasePlanned(Snapshot(d, "CAM-A"), Snapshot(b, "CAM-B"), {d_iso}, kChosenTable).differences == std::vector<std::string>{"exposure_mode:CAM-A:unreadable", "iso_auto:CAM-A:unreadable"},
            "mode and ISO auto not read");
        auto no_code = kChosenTable;
        for (auto& row : no_code.rows)
            if (row.item == D810WriteItem::IsoAuto) row.code = 0;
        check(CheckD810ReleasePlanned(Snapshot(c, "CAM-A"), Snapshot(b, "CAM-B"), {d_iso}, no_code).differences == std::vector<std::string>{"iso_auto:CAM-A:unreadable"}, "no confirmed ISO auto code: unreadable");
    }
}

// The history a record gives: the planned values, in text.
void JournalPlanned() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    const std::string op1 = "a0sw1 op=00000000000000a1 ", op2 = "a0sw1 op=00000000000000a2 ", op3 = "a0sw1 op=00000000000000a3 ";
    const auto plan = [](const std::string& op, const char* source, int steps) { return op + "ev=plan t=x source=" + source + " steps=" + std::to_string(steps) + "\n"; };
    const auto step = [](const std::string& op, int i, const char* alias, const char* item, const char* code, const char* before, const char* value) {
        return op + "ev=step i=" + std::to_string(i) + " alias=" + alias + " item=" + item + " code=" + code + " before=" + before + " value=" + value + "\n";
    };
    const auto end = [](const std::string& op, const char* verdict, int sent) { return op + "ev=end t=x verdict=" + verdict + " sent=" + std::to_string(sent) + " reason=x\n"; };
    const std::string reread_match = "a0sw1 ev=reread t=x verdict=match\n", reread_mismatch = "a0sw1 ev=reread t=x verdict=mismatch\n";
    const auto value_of = [](const D810JournalState& state, D810Camera camera, Item item) -> std::optional<std::int64_t> {
        for (const auto& entry : state.expected)
            if (entry.camera == camera && entry.item == item) return entry.value;
        return std::nullopt;
    };
    {
        // What a failed chosen plan leaves: every planned value, per camera.
        const auto text = plan(op1, "chosen", 3) + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + step(op1, 1, "CAM-B", "shutter_speed", "0x500d", "0x0000014d", "0x000001f4") +
            step(op1, 2, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + end(op1, "mismatch", 2);
        const auto state = ParseD810Journal(text);
        check(state.block == D810CaptureBlock::Mismatch && state.chosen_unresolved && !state.history_damaged && state.malformed_lines == 0 && state.expected.size() == 3, "a failed chosen plan");
        check(value_of(state, B, Item::Iso) == 400 && value_of(state, A, Item::Iso) == 200 && value_of(state, B, Item::ShutterSpeed) == 500, "the planned value of each camera and item");
        check(state.expected[0].code == 0x500f && state.expected[1].code == 0x500d, "with the code written");
        // A matching reread closes the history; a mismatching one does not.
        const auto mismatch = ParseD810Journal(text + reread_mismatch);
        check(mismatch.block == D810CaptureBlock::Mismatch && mismatch.chosen_unresolved && mismatch.expected.size() == 3, "a mismatching reread keeps it");
        const auto cleared = ParseD810Journal(text + reread_match);
        check(cleared.block == D810CaptureBlock::None && !cleared.chosen_unresolved && cleared.expected.empty(), "a matching reread closes it");
    }
    {
        // A plan that finds no block starts afresh: an earlier operation's values do not stay for the next one.
        const auto first = plan(op1, "chosen", 1) + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + end(op1, "match", 1);
        const auto after_match = ParseD810Journal(first);
        check(after_match.block == D810CaptureBlock::None && after_match.chosen_unresolved && after_match.expected.size() == 1, "a matched chosen plan leaves its values, with no block");
        const auto next = ParseD810Journal(first + plan(op2, "copy", 1) + step(op2, 0, "CAM-B", "aperture", "0x5007", "0x0230", "0x0320") + end(op2, "mismatch", 1));
        check(next.block == D810CaptureBlock::Mismatch && !next.chosen_unresolved && next.expected.size() == 1 && value_of(next, B, Item::Aperture) == 800 && !value_of(next, B, Item::Iso),
            "the next operation starts afresh: a copy history, only its own values");
        const auto chosen_next = ParseD810Journal(first + plan(op2, "chosen", 1) + step(op2, 0, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + end(op2, "unknown", 1));
        check(chosen_next.block == D810CaptureBlock::Unknown && chosen_next.expected.size() == 1 && value_of(chosen_next, A, Item::Iso) == 200 && !value_of(chosen_next, B, Item::Iso), "and a chosen one: only its own values");
    }
    {
        // While a block stands, later operations add to the history, and the last value of a camera and item wins.
        const auto first = plan(op1, "chosen", 2) + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + step(op1, 1, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + end(op1, "mismatch", 1);
        const auto second = plan(op2, "chosen", 2) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0190", "0x0064") + step(op2, 1, "CAM-B", "aperture", "0x5007", "0x0230", "0x0320") + end(op2, "match", 2);
        const auto state = ParseD810Journal(first + second);
        check(state.block == D810CaptureBlock::Mismatch && state.expected.size() == 3, "the history keeps growing while a block stands");
        check(value_of(state, B, Item::Iso) == 100 && value_of(state, A, Item::Iso) == 200 && value_of(state, B, Item::Aperture) == 800, "the last planned value of each camera and item");
        // A copy joins the history of a chosen plan; the history stays a chosen one.
        const auto mixed = ParseD810Journal(first + plan(op2, "copy", 1) + step(op2, 0, "CAM-B", "metering", "0x500b", "0x0003", "0x0002") + end(op2, "match", 1));
        check(mixed.chosen_unresolved && mixed.expected.size() == 3 && value_of(mixed, B, Item::Metering) == 2, "a copy after a failed chosen plan");
    }
    {
        // An operation that sent nothing planned nothing that counts.
        const auto first = plan(op1, "chosen", 1) + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + end(op1, "mismatch", 1);
        const auto state = ParseD810Journal(first + plan(op2, "chosen", 1) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0190", "0x00c8") + end(op2, "not_sent", 0));
        check(state.block == D810CaptureBlock::Mismatch && state.expected.size() == 1 && value_of(state, B, Item::Iso) == 400, "a not_sent operation leaves the earlier value");
        const auto alone = ParseD810Journal(plan(op1, "chosen", 1) + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + end(op1, "not_sent", 0));
        check(alone.block == D810CaptureBlock::None && !alone.chosen_unresolved && alone.expected.empty(), "a chosen plan that sent nothing leaves no history");
    }
    {
        // An operation without its end line may have sent: its values count, and capture is held.
        const auto state = ParseD810Journal(plan(op1, "chosen", 1) + step(op1, 0, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8"));
        check(state.block == D810CaptureBlock::Unconfirmed && state.chosen_unresolved && value_of(state, A, Item::Iso) == 200, "a crash during a write");
        const auto later = ParseD810Journal(plan(op1, "chosen", 1) + step(op1, 0, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + plan(op2, "copy", 0) + end(op2, "not_sent", 0));
        check(later.block == D810CaptureBlock::Unconfirmed && later.expected.size() == 1 && later.chosen_unresolved, "the next plan line closes it as unconfirmed and keeps it");
    }
    {
        // Values by type: the signed type is sign-extended, the unsigned types are not.
        const auto state = ParseD810Journal(plan(op1, "chosen", 3) + step(op1, 0, "CAM-B", "exposure_compensation", "0x5010", "0x0000", "0xfe0c") +
            step(op1, 1, "CAM-B", "iso_auto", "0xd054", "0x01", "0x00") + step(op1, 2, "CAM-B", "shutter_speed", "0x500d", "0x00000001", "0xfffffffd") + end(op1, "mismatch", 1));
        check(value_of(state, B, Item::ExposureCompensation) == -500 && value_of(state, B, Item::IsoAuto) == 0 && value_of(state, B, Item::ShutterSpeed) == 0xfffffffdLL, "signed and unsigned values");
    }
    {
        // A line that cannot be read holds capture; its values are not taken.
        const std::vector<std::string> bad{
            step(op1, 0, "CAM-C", "iso", "0x500f", "0x0064", "0x0190"),     step(op1, 0, "CAM-B", "isoo", "0x500f", "0x0064", "0x0190"),
            step(op1, 0, "CAM-B", "iso", "0x5007", "0x0064", "0x0190"),     step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x190"),
            step(op1, 0, "CAM-B", "iso", "0x500f", "0x64", "0x0190"),       step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x01G0"),
            step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x01C0"),     step(op1, 0, "CAM-B", "iso", "0X500f", "0x0064", "0x0190"),
            step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x00000190"), step(op1, 0, "CAM-B", "shutter_speed", "0x500d", "0x0001", "0x00000001"),
            op1 + "ev=step i=x alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0190\n",
            op1 + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064\n",
            op1 + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0190 x=1\n",
        };
        for (const auto& line : bad) {
            const auto state = ParseD810Journal(plan(op1, "chosen", 1) + line + end(op1, "match", 1));
            check(state.block == D810CaptureBlock::Unconfirmed && state.malformed_lines >= 1 && state.expected.empty(), "an unreadable step line");
        }
        const auto source = ParseD810Journal(plan(op1, "other", 1) + end(op1, "not_sent", 0));
        check(source.block == D810CaptureBlock::Unconfirmed && source.malformed_lines >= 1, "an unknown source");
        const auto torn = ParseD810Journal(plan(op1, "chosen", 1) + "a0sw1 op=00000000000000a1 ev=step i=0 alias=CAM-B item=iso co");
        check(torn.block == D810CaptureBlock::Unconfirmed && torn.expected.empty(), "a torn step line");
        // An operation's lines of another operation, and a step before any plan, are not read as steps.
        const auto stray = ParseD810Journal(step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + plan(op2, "copy", 0) + end(op2, "not_sent", 0));
        check(stray.block == D810CaptureBlock::Unconfirmed && stray.expected.empty(), "a step with no plan");
        // The three steps of the operations of one record are all kept, in the order first seen.
        const auto three = ParseD810Journal(plan(op1, "chosen", 1) + step(op1, 0, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + end(op1, "mismatch", 1) +
            plan(op2, "chosen", 1) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + end(op2, "mismatch", 1) +
            plan(op3, "chosen", 1) + step(op3, 0, "CAM-A", "iso", "0x500f", "0x00c8", "0x0064") + end(op3, "unknown", 1));
        check(three.expected.size() == 2 && three.expected[0].camera == A && three.expected[0].value == 100 && three.expected[1].camera == B && three.expected[1].value == 400, "folded in place");
    }
    {
        // history_damaged: set by a line that cannot be read or by steps that do not fit the plan line of an
        // operation that may have sent; cleared by what clears the history.
        const auto b_step = step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190");
        const auto a_step = step(op1, 1, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8");
        const auto damaged = [](const std::string& text) { return ParseD810Journal(text).history_damaged; };
        check(!damaged(plan(op1, "chosen", 2) + b_step + a_step + end(op1, "mismatch", 2)), "a whole record is not damaged");
        check(!damaged(plan(op1, "chosen", 2) + b_step + a_step + end(op1, "match", 2) + plan(op2, "copy", 0) + end(op2, "match", 0)), "operations that fit their plan lines");
        check(!damaged(plan(op1, "chosen", 0) + end(op1, "not_sent", 0)), "no steps and none announced");
        check(damaged(plan(op1, "chosen", 2) + b_step + end(op1, "mismatch", 1)), "a step line is missing");
        check(damaged(plan(op1, "chosen", 1) + b_step + a_step + end(op1, "mismatch", 2)), "a step line too many");
        check(damaged(plan(op1, "chosen", 2) + b_step + step(op1, 0, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + end(op1, "mismatch", 2)), "a repeated number");
        check(damaged(plan(op1, "chosen", 2) + step(op1, 1, "CAM-A", "iso", "0x500f", "0x0040", "0x00c8") + step(op1, 0, "CAM-B", "iso", "0x500f", "0x0064", "0x0190") + end(op1, "mismatch", 2)),
            "numbers out of order");
        check(damaged(plan(op1, "chosen", 2) + b_step + a_step + end(op1, "unknown", 2) + plan(op2, "chosen", 2) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0190", "0x0064") + end(op2, "mismatch", 1)),
            "a later operation does not fit");
        check(damaged(op1 + "ev=plan t=x source=chosen steps=x\n" + b_step + end(op1, "match", 1)), "the number of steps cannot be read");
        check(damaged(op1 + "ev=plan t=x source=chosen\n" + b_step + end(op1, "match", 1)), "a plan line cut short");
        check(damaged(plan(op1, "chosen", 1) + "garbage\n" + b_step + end(op1, "match", 1)), "a line that is not a line of the record");
        check(damaged(plan(op1, "chosen", 2) + b_step), "no end line and a step line is missing");
        check(damaged(plan(op1, "chosen", 1) + b_step + plan(op2, "chosen", 1)), "an operation closed by the next plan line is held to its plan line too");
        // An operation that sent nothing is not held to its plan line: no step of it counts.
        check(!damaged(plan(op1, "chosen", 2) + b_step + end(op1, "not_sent", 0)), "not_sent without all its step lines");
        check(damaged(plan(op1, "chosen", 2) + b_step + end(op1, "not_sent", 1)), "not_sent that says it sent");
        // A start of the history clears it: a matching reread, and a plan line that finds no block.
        const auto broken = plan(op1, "chosen", 2) + b_step + end(op1, "mismatch", 1);
        check(damaged(broken + reread_mismatch), "a mismatching reread keeps it");
        check(!damaged(broken + reread_match), "a matching reread clears it");
        check(!damaged(broken + reread_match + plan(op2, "chosen", 1) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0190", "0x0064") + end(op2, "mismatch", 1)), "and a later whole operation does not set it again");
        check(damaged(broken + plan(op2, "chosen", 1) + step(op2, 0, "CAM-B", "iso", "0x500f", "0x0190", "0x0064") + end(op2, "mismatch", 1)), "a later whole operation does not clear it while the block stands");
        check(!damaged("a0sw1 op=00000000000000a1 ev=pl\n" + reread_match), "a line cut short before the reread");
        check(damaged("a0sw1 op=00000000000000a1 ev=pl\n" + plan(op1, "chosen", 1) + b_step + end(op1, "match", 1)), "a line cut short at the start of the file stays");
        // Copies alone: the mark exists but nothing rests on it (chosen_unresolved is false).
        const auto copy_only = ParseD810Journal(plan(op1, "copy", 2) + b_step + end(op1, "mismatch", 1));
        check(!copy_only.chosen_unresolved && copy_only.block == D810CaptureBlock::Mismatch && copy_only.malformed_lines == 0, "the record of copies reads as before");
    }
}
// --- Stops across the two cameras (#416) -------------------------------------------------------
// The camera access with a fault put into the write session of one camera (its second open: the first is the check
// before the send). A test double: it changes what the fake connection answers and nothing else.
using Fault = std::function<void(D810OpenedCamera&)>;
struct FaultAccess final: D810SettingsCameraAccess {
    FakeAccess& in;
    D810Camera target;
    Fault fault;
    int target_opens = 0;
    FaultAccess(FakeAccess& access, D810Camera camera, Fault f): in(access), target(camera), fault(std::move(f)) {}
    D810OpenedCamera Open(D810Camera camera) override {
        auto opened = in.Open(camera);
        if (camera == target && opened.transport && ++target_opens == 2) fault(opened);
        return opened;
    }
    bool Close(D810Camera camera) override { return in.Close(camera); }
};
// The first request of the write session that `match` accepts is answered with `reply`; the others reach the body.
Fault AnswerFirst(std::function<bool(const SettingsRequestShape&)> match, std::function<WpdLiveViewExchange()> reply) {
    return [match, reply](D810OpenedCamera& opened) {
        auto fired = std::make_shared<bool>(false);
        auto inner = std::move(opened.transport);
        opened.transport = [match, reply, fired, inner](IPortableDeviceValues* request) {
            const auto shape = InspectSettingsRequest(request);
            if (!*fired && shape && match(*shape)) { *fired = true; return reply(); }
            return inner(request);
        };
    };
}
bool IsRead(const SettingsRequestShape& shape, std::uint32_t opcode, std::uint32_t code = 0) {
    return shape.kind == SettingsCommandKind::ExecuteWithDataToRead && shape.opcode == opcode && shape.arguments.size() == 1 && (code == 0 || shape.arguments[0] == code);
}
D810WriteRun RunThrough(Bench& bench, const D810PlanResult& plan, D810SettingsCameraAccess& access) {
    if (!plan.plan) return {};
    check(bench.controller->Confirm(plan.plan->id) == nullptr, "confirm");
    return bench.controller->Execute(plan.plan->id, access, bench.options);
}
std::size_t CountIn(const std::string& text, const std::string& what) {
    std::size_t count = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
    return count;
}

// CAM-B writes its one item (iso 200), then CAM-A has two (iso, then metering). One condition stops CAM-A in its first
// item: the second item is not sent (it would be if the run went on to the next step), CAM-B keeps what it was
// given (no write back, no second write), and capture is held, in memory and after a restart.
void ChosenStopsOnA() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    const std::vector<D810CameraChoice> picks{PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200), PickFor(A, Item::Metering, 4)};
    struct Case {
        const char* what;
        std::string reason;              // the start of the run's reason
        int a_sent;                      // writes that reached CAM-A
        std::function<void(Bench&)> setup;
        Fault fault;                     // put into CAM-A's write session
        int journal_ok;                  // appends the record accepts (-1: all)
    };
    const auto ms = [](int n) { return std::chrono::milliseconds(n); };
    const std::vector<Case> cases{
        {"CAM-A moved after CAM-B was written", "changed_before_write", 0,
            [](Bench& bench) { bench.b.after = [&bench](const std::string& e) { if (e == "write_end") bench.a.props[0x500f].current = 400; }; }, nullptr, -1},
        {"live view on for the first item only", "live_view_on", 0,
            [](Bench& bench) {
                bench.a.after = [&bench, n = 0](const std::string& e) mutable {
                    if (e.compare(0, 10, "1015(d1a2)") == 0 && bench.access.opens[0] >= 2) bench.a.live = (++n == 1) ? 1 : 0;
                };
            }, nullptr, -1},
        {"the live state cannot be read", "live_state_unreadable", 0, [](Bench&) {},
            AnswerFirst([](const auto& shape) { return IsRead(shape, 0x1015); }, [] { return CommandFailedReply(); }), -1},
        {"the exposure mode description cannot be read", "desc_unreadable", 0, [](Bench&) {},
            AnswerFirst([](const auto& shape) { return IsRead(shape, 0x1014, 0x500e); }, [] { return FinalReply(0x200a); }), -1},
        {"the ISO auto description cannot be read", "desc_unreadable", 0, [](Bench&) {},
            AnswerFirst([](const auto& shape) { return IsRead(shape, 0x1014, 0xd054); }, [] { return FinalReply(0x200a); }), -1},
        {"the item's description cannot be read", "desc_unreadable", 0, [](Bench&) {},
            AnswerFirst([](const auto& shape) { return IsRead(shape, 0x1014, 0x500f); }, [] { return FinalReply(0x200a); }), -1},
        {"stopped by the operator before the first item", "interrupted", 0,
            [](Bench& bench) { bench.a.after = [&bench](const std::string& e) { if (e == "90c8" && bench.access.opens[0] >= 2) bench.go = false; }; }, nullptr, -1},
        {"stopped by the operator after the reads, before the begin", "refused_interrupted", 0,
            [](Bench& bench) {
                // The stop comes between the end of the reads and the begin: the second look at the time left, in CAM-A's session.
                bench.options.remaining = [&bench, looks = 0]() mutable {
                    if (bench.access.opens[0] >= 2 && ++looks == 2) bench.go = false;
                    return bench.left;
                };
            }, nullptr, -1},
        {"no time left for CAM-A's items (checked first)", "deadline_margin", 0,
            [ms](Bench& bench) { bench.b.after = [&bench, ms](const std::string& e) { if (e == "write_end") bench.left = ms(9999); }; }, nullptr, -1},
        {"no time left once CAM-A's reads are done (checked again)", "deadline_margin", 0,
            [ms](Bench& bench) {
                bench.a.after = [&bench, ms](const std::string& e) { if (e == "1014(500f)" && bench.access.opens[0] >= 2) bench.left = ms(9999); };
            }, nullptr, -1},
        {"the gate refuses the value", "refused_", 0,
            [](Bench& bench) {
                bench.a.after = [&bench, done = false](const std::string&) mutable {
                    if (done || bench.access.opens[0] < 2) return;
                    done = true;
                    bench.a.props[0x500f].values = {64, 100}; // 200 is no longer offered
                };
            }, nullptr, -1},
        {"the record cannot take the result line", "record_unwritable", 1, [](Bench&) {}, nullptr, 1 + 3 + 1},
        {"the first item ends unknown", "result_unknown", 1, [](Bench& bench) { bench.a.end_reject[0x500f] = 0x2002; }, nullptr, -1},
        {"the first item reads back another value", "readback_mismatch", 1, [](Bench& bench) { bench.a.apply_as[0x500f] = 100; }, nullptr, -1},
        {"CAM-A cannot be opened for the write", "open_failed:CAM-A", 0,
            [](Bench& bench) { bench.b.after = [&bench](const std::string& e) { if (e == "write_end") bench.access.open_fail[0] = true; }; }, nullptr, -1},
        {"CAM-A's write session sees another version", "body_not_verified:CAM-A", 0, [](Bench&) {},
            [](D810OpenedCamera& opened) { opened.firmware = "other-text"; }, -1},
        {"CAM-A is not ready", "device_not_ready", 0, [](Bench&) {},
            [](D810OpenedCamera& opened) { opened.transport = [](IPortableDeviceValues*) -> WpdLiveViewExchange { throw std::runtime_error("dead"); }; }, -1},
    };
    for (const auto& c : cases) {
        const std::string n = c.what;
        Bench bench(kChosenTable, c.journal_ok);
        c.setup(bench);
        const auto plan = bench.Chosen(picks);
        check(plan.Ok() && plan.plan->steps.size() == 3, (n + ": the plan").c_str());
        FaultAccess faulty(bench.access, A, c.fault ? c.fault : Fault([](D810OpenedCamera&) {}));
        D810SettingsCameraAccess& access = c.fault ? static_cast<D810SettingsCameraAccess&>(faulty) : bench.access;
        const auto run = RunThrough(bench, plan, access);
        check(run.reason.compare(0, c.reason.size(), c.reason) == 0 && !run.reason.empty(), (n + ": the reason (" + run.reason + ")").c_str());
        check(run.sent == 1 + c.a_sent && bench.a.write_begins == c.a_sent && bench.a.begins_by_code.count(0x500b) == 0, (n + ": the next CAM-A item is not sent").c_str());
        check(bench.b.write_begins == 1 && bench.b.begins_by_code[0x500f] == 1 && bench.b.props[0x500f].current == 200, (n + ": CAM-B keeps its value, nothing written back").c_str());
        check(run.block != D810CaptureBlock::None && !bench.controller->CaptureAllowed() && !bench.Restarted().CaptureAllowed(), (n + ": capture is held").c_str());
        check(NoResend(bench.a) && NoResend(bench.b) && bench.a.forbidden + bench.b.forbidden == 0, (n + ": nothing sent twice").c_str());
        if (c.journal_ok < 0) check(CountIn(bench.Journal(), " ev=end ") == 1, (n + ": one end line").c_str());
    }
}

// CAM-B has two items (iso, aperture) and CAM-A one. One condition stops CAM-B; CAM-A is not opened for writing (it is
// opened once, for the check before the send), nothing is sent to it, and a stop after something was sent holds capture.
void ChosenStopsOnB() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    const std::vector<D810CameraChoice> picks{PickFor(B, Item::Iso, 200), PickFor(B, Item::Aperture, 1100), PickFor(A, Item::Iso, 200)};
    struct Case {
        const char* what;
        std::string reason;
        int b_sent;
        std::function<void(Bench&)> setup;
        Fault fault;                     // put into CAM-B's write session
    };
    const std::vector<Case> cases{
        {"CAM-B cannot be closed after its items", "close_failed:CAM-B", 2,
            [](Bench& bench) { bench.b.after = [&bench](const std::string& e) { if (e.compare(0, 5, "1016(") == 0) bench.access.close_ok[1] = false; }; }, nullptr},
        {"the gate refuses CAM-B's first item", "refused_", 0,
            [](Bench& bench) {
                bench.b.after = [&bench, done = false](const std::string&) mutable {
                    if (done || bench.access.opens[1] < 2) return;
                    done = true;
                    bench.b.props[0x500f].values = {64, 100};
                };
            }, nullptr},
        {"the gate refuses CAM-B's second item", "refused_", 1,
            [](Bench& bench) { bench.b.after = [&bench](const std::string& e) { if (e == "write_end") bench.b.props[0x5007].values = {400, 560, 800}; }; }, nullptr},
        {"CAM-B's write session sees another version", "body_not_verified:CAM-B", 0, [](Bench&) {},
            [](D810OpenedCamera& opened) { opened.firmware = "other-text"; }},
        {"CAM-B is not ready", "device_not_ready", 0, [](Bench&) {},
            [](D810OpenedCamera& opened) { opened.transport = [](IPortableDeviceValues*) -> WpdLiveViewExchange { throw std::runtime_error("dead"); }; }},
        {"an exception after CAM-B's first item was sent", "exception", 1,
            [](Bench& bench) { bench.options.log = [](const std::string& line) { if (Contains(line, "settings_write code=")) throw std::runtime_error("log"); }; }, nullptr},
        {"stopped by the operator before the first item", "interrupted", 0,
            [](Bench& bench) { bench.b.after = [&bench](const std::string& e) { if (e == "90c8" && bench.access.opens[1] >= 2) bench.go = false; }; }, nullptr},
    };
    for (const auto& c : cases) {
        const std::string n = c.what;
        Bench bench(kChosenTable);
        c.setup(bench);
        const auto plan = bench.Chosen(picks);
        check(plan.Ok() && plan.plan->steps.size() == 3, (n + ": the plan").c_str());
        FaultAccess faulty(bench.access, B, c.fault ? c.fault : Fault([](D810OpenedCamera&) {}));
        D810SettingsCameraAccess& access = c.fault ? static_cast<D810SettingsCameraAccess&>(faulty) : bench.access;
        const auto run = RunThrough(bench, plan, access);
        check(run.reason.compare(0, c.reason.size(), c.reason) == 0 && !run.reason.empty(), (n + ": the reason (" + run.reason + ")").c_str());
        check(run.sent == c.b_sent && bench.b.write_begins == c.b_sent, (n + ": what reached CAM-B").c_str());
        check(bench.a.write_begins == 0 && bench.access.opens[0] == 1 && bench.a.props[0x500f].current == 64, (n + ": CAM-A is not opened for writing and is not written").c_str());
        check(NoResend(bench.b) && bench.a.forbidden + bench.b.forbidden == 0, (n + ": nothing sent twice").c_str());
        if (c.b_sent > 0) check(run.block != D810CaptureBlock::None && !bench.controller->CaptureAllowed() && !bench.Restarted().CaptureAllowed(), (n + ": capture is held").c_str());
        else check(run.verdict == D810WriteVerdict::NotSent && run.block == D810CaptureBlock::None && Contains(bench.Journal(), "verdict=not_sent sent=0"), (n + ": nothing was sent, no block").c_str());
        check(CountIn(bench.Journal(), " ev=end ") == 1, (n + ": one end line").c_str());
    }
}

// Every subset of the ten picks (two cameras, five items), in three input orders: the plan is CAM-B's steps first, then
// CAM-A's, the item table's order inside a camera, no (camera, item) twice.
void ChosenPlanSubsets() {
    using Item = D810WriteItem;
    auto a = Body(1667, 800, 64), b = Body(333, 560, 100);
    const auto sa = Snapshot(a, "CAM-A"), sb = Snapshot(b, "CAM-B");
    const std::vector<D810CameraChoice> all{
        PickFor(D810Camera::A, Item::Iso, 200), PickFor(D810Camera::A, Item::ShutterSpeed, 500), PickFor(D810Camera::A, Item::Aperture, 1100), PickFor(D810Camera::A, Item::WhiteBalance, 4),
        PickFor(D810Camera::A, Item::Metering, 4), PickFor(D810Camera::B, Item::Iso, 400), PickFor(D810Camera::B, Item::ShutterSpeed, 1000), PickFor(D810Camera::B, Item::Aperture, 1600),
        PickFor(D810Camera::B, Item::WhiteBalance, 5), PickFor(D810Camera::B, Item::Metering, 2)};
    std::size_t plans = 0, bad = 0;
    for (unsigned mask = 1; mask < (1u << all.size()); ++mask)
        for (int variant = 0; variant < 3; ++variant) {
            std::vector<D810CameraChoice> picks;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (mask & (1u << i)) picks.push_back(all[i]);
            if (variant == 1) std::reverse(picks.begin(), picks.end());
            if (variant == 2 && picks.size() > 1) std::rotate(picks.begin(), picks.begin() + 1, picks.end());
            const auto result = BuildD810PerCameraPlan(sa, sb, {true, true}, picks, kChosenTable);
            ++plans;
            std::vector<std::pair<int, int>> want, got;
            for (const auto camera : {D810Camera::B, D810Camera::A})
                for (const auto& pick : all)
                    if ((mask & (1u << (&pick - &all[0]))) && pick.camera == camera) want.emplace_back(static_cast<int>(camera), static_cast<int>(pick.item));
            if (result.plan)
                for (const auto& step : result.plan->steps) got.emplace_back(static_cast<int>(step.camera), static_cast<int>(step.item));
            if (!result.Ok() || got != want || !D810PlanProblem(*result.plan, kChosenTable).empty()) ++bad;
        }
    check(plans == 3 * 1023 && bad == 0, "every subset of the ten picks, in three input orders: CAM-B first, CAM-A last, the item table's order");
}

// All ten steps (five items on each camera) through a run: the order of the sends, the record, the time margin.
void ChosenTenItems() {
    using Item = D810WriteItem;
    const std::vector<D810CameraChoice> all{
        PickFor(D810Camera::A, Item::Iso, 200), PickFor(D810Camera::A, Item::ShutterSpeed, 500), PickFor(D810Camera::A, Item::Aperture, 1100), PickFor(D810Camera::A, Item::WhiteBalance, 4),
        PickFor(D810Camera::A, Item::Metering, 4), PickFor(D810Camera::B, Item::Iso, 400), PickFor(D810Camera::B, Item::ShutterSpeed, 1000), PickFor(D810Camera::B, Item::Aperture, 1600),
        PickFor(D810Camera::B, Item::WhiteBalance, 5), PickFor(D810Camera::B, Item::Metering, 2)};
    {
        Bench bench(kChosenTable);
        bench.TraceBegins();
        bench.left = std::chrono::milliseconds(50000); // 5 s for each of the ten steps, exactly
        const auto plan = bench.Chosen(all);
        check(plan.Ok() && plan.plan->steps.size() == 10, "ten steps");
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::Match && run.sent == 10 && run.reason.empty(), "all ten matched");
        BeginOrder want;
        for (const auto& step : plan.plan->steps) want.emplace_back(static_cast<int>(step.camera), static_cast<int>(step.code));
        check(bench.begin_order == want, "the sends follow the plan");
        check(std::all_of(want.begin(), want.begin() + 5, [](const auto& entry) { return entry.first == 1; }) &&
                std::all_of(want.begin() + 5, want.end(), [](const auto& entry) { return entry.first == 0; }),
            "the five CAM-B sends come before the five CAM-A sends");
        check(bench.a.write_begins == 5 && bench.b.write_begins == 5 && NoResend(bench.a) && NoResend(bench.b) && !bench.access.overlap, "five on each, once each");
        const auto journal = bench.Journal();
        check(CountIn(journal, " ev=step ") == 10 && CountIn(journal, " ev=result ") == 10 && Contains(journal, "verdict=match sent=10"), "the record has the ten steps and their results");
        const auto state = ParseD810Journal(journal);
        check(state.expected.size() == 10 && state.chosen_unresolved && !state.history_damaged && state.block == D810CaptureBlock::None && bench.controller->CaptureAllowed(),
            "ten expected values in the record, no block");
    }
    {
        // One millisecond less and not a single send: 4999 ms for the first of ten steps.
        Bench bench(kChosenTable);
        bench.left = std::chrono::milliseconds(49999);
        const auto run = bench.Run(bench.Chosen(all));
        check(run.reason == "deadline_margin" && run.sent == 0 && bench.Writes() == 0 && run.verdict == D810WriteVerdict::NotSent && bench.access.opens[0] == 1,
            "49999 ms: stopped before the first send, CAM-A never opened for writing");
    }
}

// While capture is held by the record (an operation without its end, an unknown result, a record that cannot be read)
// nothing is planned and nothing is sent; a block that only says "mismatch" lets a new plan through (it is the way to fix
// it). A plan confirmed before the block appeared (another instance left it) is stopped at the run, with nothing sent.
void WritesAreHeldWhileCaptureIsBlocked() {
    using Item = D810WriteItem;
    const std::vector<D810CameraChoice> pick{PickFor(D810Camera::B, Item::Iso, 200)};
    const std::string op = "a0sw1 op=00000000000000b1 ";
    const std::string open_op = op + "ev=plan t=x source=copy steps=1\n" + op + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0190\n";
    struct Kind { const char* name; std::string text; bool holds; bool unreadable; };
    const std::vector<Kind> kinds{
        {"unconfirmed", open_op, true, false},
        {"unknown", open_op + op + "ev=end t=x verdict=unknown sent=1 reason=x\n", true, false},
        {"unreadable record", "", true, true},
        {"mismatch", open_op + op + "ev=end t=x verdict=mismatch sent=1 reason=x\n", false, false},
    };
    const auto leave_block = [](Bench& bench, const Kind& kind) {
        if (kind.unreadable) fs::create_directories(bench.file.Path());
        else WriteFile(bench.file.Path(), kind.text);
    };
    for (const auto& kind : kinds) {
        const std::string n = kind.name;
        {
            // Planning: PlanChosen and PlanCopy.
            Bench bench(kChosenTable, -1, true);
            leave_block(bench, kind);
            const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
            check(read.complete, (n + ": the read itself is not held").c_str());
            const auto chosen = bench.controller->PlanChosen(read, pick);
            const auto copy = bench.Copy();
            if (kind.holds) {
                check(!chosen.Ok() && HasReason(chosen, "reread_first"), (n + ": no plan of picked values").c_str());
                check(!copy.Ok() && HasReason(copy, "reread_first") && !bench.controller->Status().plan_pending, (n + ": no copy plan").c_str());
            } else {
                check(chosen.Ok() && copy.Ok(), (n + ": plans are made, to fix the mismatch").c_str());
            }
        }
        for (const bool use_copy : {false, true}) {
            // The run of a plan confirmed before the block appeared.
            Bench bench(kChosenTable, -1, true);
            const auto plan = use_copy ? bench.Copy() : bench.Chosen(pick);
            check(plan.Ok(), (n + ": a plan on a clean record").c_str());
            if (!plan.Ok()) continue;
            leave_block(bench, kind);
            const auto run = bench.Run(plan);
            const std::string what = n + (use_copy ? " (copy)" : " (picked values)");
            if (kind.holds) {
                check(run.sent == 0 && run.reason == "reread_first" && bench.Writes() == 0, (what + ": the run sends nothing").c_str());
                check(bench.access.opens[0] == 0 && bench.access.opens[1] == 0, (what + ": no camera is opened").c_str());
                check(bench.controller->Execute(plan.plan->id, bench.access, bench.options).reason == "plan_not_confirmed", (what + ": the plan is consumed").c_str());
            } else {
                check(run.sent >= 1 && bench.Writes() >= 1, (what + ": under a mismatch the run goes on").c_str());
            }
        }
    }
}

// A record changed after it was written, within one operation: the same (camera, item) twice, or a CAM-A step before a
// CAM-B one (a plan holds neither). A step that is hidden that way may have been sent, so the planned values do not
// release: the two cameras are compared, as for a damaged record.
void RecordRewrittenWithinAnOperation() {
    const std::string op = "a0sw1 op=00000000000000c1 ";
    const std::string plan = op + "ev=plan t=x source=chosen steps=2\n";
    const std::string end = op + "ev=end t=x verdict=mismatch sent=1 reason=x\n";
    const std::string a_iso = op + "ev=step i=0 alias=CAM-A item=iso code=0x500f before=0x0040 value=0x00c8\n";
    const std::string a_iso_again = op + "ev=step i=1 alias=CAM-A item=iso code=0x500f before=0x0040 value=0x00c8\n";
    const std::string b_aperture_second = op + "ev=step i=1 alias=CAM-B item=aperture code=0x5007 before=0x0230 value=0x0320\n";
    const std::string b_iso = op + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0190\n";
    const std::string a_iso_second = op + "ev=step i=1 alias=CAM-A item=iso code=0x500f before=0x0040 value=0x00c8\n";
    // Untouched: CAM-B first, then CAM-A.
    check(!ParseD810Journal(plan + b_iso + a_iso_second + end).history_damaged, "CAM-B before CAM-A is a plan");
    struct Case { const char* what; std::string text; std::size_t expected; };
    const std::vector<Case> cases{
        {"the same item of a camera twice", plan + a_iso + a_iso_again + end, 1},
        {"a CAM-B step after a CAM-A step", plan + a_iso + b_aperture_second + end, 2},
    };
    for (const auto& c : cases) {
        const std::string n = c.what;
        const auto state = ParseD810Journal(c.text);
        check(state.chosen_unresolved && state.history_damaged && state.malformed_lines == 0 && state.block == D810CaptureBlock::Mismatch && state.expected.size() == c.expected, (n + ": the record reads, and is marked damaged").c_str());
        Bench bench(kChosenTable, -1, true);
        WriteFile(bench.file.Path(), c.text);
        // Every planned value that can be seen is there (CAM-A's ISO 200, and for the second record CAM-B's aperture 800),
        // while the two cameras differ: by the planned values alone this would release.
        bench.a = Body(1667, 800, 200);
        bench.b = Body(333, 800, 100);
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.check.released && !reread.check.differences.empty() && reread.check.differences[0] == "iso" && !bench.controller->CaptureAllowed(), (n + ": not released while the cameras differ").c_str());
        bench.b = Body(1667, 800, 200);
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.check.released && bench.controller->CaptureAllowed(), (n + ": released when the cameras agree").c_str());
    }
}

// "Same as CAM-A" asks nothing of CAM-A's mode or ISO auto (only CAM-B, which is written, must be in M with ISO auto off):
// CAM-A is only read. What it relies on is the value read: if CAM-A's value moves before the run, the run stops.
void SameAsADoesNotAskCamAToBeInM() {
    using Item = D810WriteItem;
    const auto setup = [](Bench& bench) {
        bench.a.props[0x500e].current = 3; // not M
        bench.a.props[0xd054].current = 1; // ISO auto on
    };
    {
        Bench bench(kChosenTable);
        setup(bench);
        const auto plan = bench.Chosen({FollowA(Item::Iso)});
        check(plan.Ok() && plan.plan->steps.size() == 1 && plan.plan->steps[0].camera == D810Camera::B && plan.plan->steps[0].value == 64, "the plan follows CAM-A's ISO although CAM-A is not in M");
        const auto run = bench.Run(plan);
        check(run.verdict == D810WriteVerdict::Match && run.sent == 1 && bench.a.write_begins == 0 && bench.b.props[0x500f].current == 64, "and runs: CAM-B written, CAM-A only read");
    }
    {
        Bench bench(kChosenTable);
        setup(bench);
        const auto plan = bench.Chosen({FollowA(Item::Iso)});
        bench.a.props[0x500f].current = 100; // CAM-A moves after the plan
        const auto run = bench.Run(plan);
        check(run.sent == 0 && run.reason.rfind("plan_changed:CAM-A", 0) == 0 && bench.Writes() == 0, "CAM-A's value moved: the run stops before a send");
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
        // The operation latch drops: CAM-B's read before its first write fails (CAM-B is written first),
        // so CAM-A is never opened for writing (a table with both cameras' rows).
        Bench bench(kChosenTable);
        bench.b.zero_at[0x500f] = 3; // 1: the plan's read, 2: the verification read, 3: the read before the write
        const auto plan = bench.Chosen({PickFor(D810Camera::B, D810WriteItem::Iso, 200), PickFor(D810Camera::A, D810WriteItem::Iso, 200)});
        check(plan.Ok() && plan.plan->Cameras() == std::vector<D810Camera>{D810Camera::B, D810Camera::A}, "a plan for both");
        const auto run = bench.Run(plan);
        check(bench.b.reads_of[0x500f] == 3, "zero read on B happened");
        check(run.sent == 0 && run.verdict == D810WriteVerdict::NotSent && bench.Writes() == 0, "zero read on B: not sent");
        check(bench.access.opens[0] == 1 && bench.a.write_begins == 0, "the latch is down: A opened for the verification read only");
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
        check(s.screen->Status().enabled_rows == 5 && s.screen->CopyOffered(true) && !s.screen->CopyOffered(false), "screen: product table, copy offered when ready");
        const auto text = s.screen->OfferText(true);
        check(text.find(L"書ける項目がありません") == std::wstring::npos && text.find(L"番号（code）") == std::wstring::npos, "screen: product table, not the no-row reason");
        // What is read and what is written are told apart: the writable items are named from the table.
        check(text.find(L"読みます") != std::wstring::npos && text.find(L"CAM-B に書くのは ISO感度・シャッター速度・絞り・ホワイトバランス・測光 だけで、ほかは読むだけです") != std::wstring::npos &&
                text.find(L"確定の前に表示します") != std::wstring::npos,
            "screen: product table, the offer names the writable items");
        check(s.screen->Status().writable_items == kExpectedProductItems, "screen: product table, writable items");
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
// The body is checked when the plan is read: on the screen no confirmation is put up for a CAM-B the table does not name.
void ScreenBodyNotVerified() {
    {
        ScreenBench s;
        s.bench.access.firmware[1] = "other-text";
        check(s.Copy() && s.screen->Step() == D810WriteScreenStep::Idle && !s.screen->ConfirmationShown(), "screen: no confirmation for a body the table does not name");
        check(s.screen->View() && !s.screen->View()->confirmation && Shows(s.screen->View(), L"機種・版が表と違います") && Shows(s.screen->View(), L"body_not_verified:CAM-B"),
            "screen: the reason is shown");
        check(!s.screen->ConfirmOffered(true) && !s.screen->Status().plan_pending && !s.screen->ConfirmShownPlan(true) && s.bench.Writes() == 0, "screen: nothing to confirm, nothing written");
        check(s.bench.b.read_begins == 0 && s.screen->AdmitCapture(), "screen: nothing reached CAM-B, capture is not held");
        check(std::any_of(s.log.begin(), s.log.end(), [](const auto& line) { return line.find("settings_write_body_check model_match=1 firmware_match=0 setting_writes=0") != std::string::npos; }),
            "screen: the check is logged as booleans");
        check(std::any_of(s.log.begin(), s.log.end(), [](const auto& line) { return line.find("settings_write_ui_plan_read_incomplete=1 reason=body_not_verified:CAM-B") != std::string::npos; }),
            "screen: the failed read is logged");
    }
    {
        // The same screen with the matching version puts the confirmation up.
        ScreenBench s;
        check(s.Copy() && s.screen->ConfirmationShown(), "screen: a matching body gets its confirmation");
    }
}
// The confirmation of the product table: ISO, white balance and metering, the notice of what a white balance copy does not
// carry on the white balance line, and no more lines than the window's limit.
void ScreenProductRows() {
    const std::wstring note = L"（種類だけを写します。微調整・自動の型・蛍光灯の型は写りません）";
    const auto count_note = [&](const D810WriteScreenView* view) {
        std::size_t n = 0;
        if (view) for (const auto& line : view->lines) n += line.find(note) != std::wstring::npos;
        return n;
    };
    {
        ScreenBench s(kProduct);
        PrepareProductBench(s.bench);
        check(s.Copy() && s.screen->ConfirmationShown(), "product screen: the confirmation is up");
        const auto* view = s.screen->View();
        // Three notices, the five steps, the closing line; nothing is left out.
        check(view && view->lines.size() == 9 && view->lines.size() <= D810WriteViewMaxLines(), "product screen: nine lines, within the window's limit");
        check(Shows(view, L"ISO感度: CAM-B の今の値 100 → 書く値 64") && Shows(view, L"シャッター速度: CAM-B の今の値 1/30 s → 書く値 1/6 s") && Shows(view, L"絞り: CAM-B の今の値 F5.6 → 書く値 F8") &&
                Shows(view, L"ホワイトバランス: CAM-B の今の値 Automatic → 書く値 Daylight") && Shows(view, L"測光: CAM-B の今の値 Multi-pattern → 書く値 Center-weighted"),
            "product screen: the five items with B's value now and the value to write");
        check(count_note(view) == 1 && view && view->lines.size() > 6 && view->lines[6].find(L"ホワイトバランス:") != std::wstring::npos && view->lines[6].find(note) != std::wstring::npos,
            "product screen: the notice is on the white balance line, once");
        check(!Shows(view, L"外した項目") && !Shows(view, L"表で無効の項目です"), "product screen: no item is left out for a disabled row");
        check(s.Confirm() && Shows(s.screen->View(), L"一致") && s.bench.Writes() == 5 && NoResend(s.bench.b) && s.bench.a.write_begins == 0, "product screen: five items written once");
        check(s.bench.b.props[0x5005].current == 4 && s.bench.b.props[0x500b].current == 2 && s.bench.b.props[0x5007].current == 800 && s.bench.b.props[0x500d].current == 1667,
            "product screen: B reads A's values for the five");
        check(s.screen->OfferText(true).find(L"CAM-B に書くのは ISO感度・シャッター速度・絞り・ホワイトバランス・測光 だけで") != std::wstring::npos, "product screen: the offer names the five items");
    }
    {
        // A kind a copy cannot carry: no step and no notice for it, listed with what to do; the rest is confirmable.
        ScreenBench s(kProduct);
        PrepareProductBench(s.bench);
        s.bench.a.props[0x5005].current = 0x8013;
        check(s.Copy() && s.screen->ConfirmationShown(), "product screen, Preset: the confirmation is up");
        const auto* view = s.screen->View();
        check(view && view->lines.size() == 10 && count_note(view) == 0, "product screen, Preset: no white balance step, so no notice");
        check(Shows(view, L"ホワイトバランス: 本体に登録した内容") && Shows(view, L"B の本体で合わせてください") && !Shows(view, L"ホワイトバランス: CAM-B の今の値"), "product screen, Preset: listed with the reason");
        check(s.Confirm() && s.bench.Writes() == 4 && s.bench.b.begins_by_code.count(0x5005) == 0, "product screen, Preset: written without the white balance");
    }
}
// --- The settings panel (the screen's panel model, the drop-down picks and the draft) --------------------------------
bool OpenPanel(ScreenBench& s) { return s.screen->RequestPanelOpen(true) && s.WaitIdle() && s.screen->PanelOpen(); }
bool PanelAllLocked(const D810SettingsWriteScreen& screen, const char* reason) {
    const auto* model = screen.Panel();
    if (!model) return false;
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto item : kD810ChoiceItems) {
            const auto& cell = model->At(camera, item);
            if (cell.Editable() || !cell.locked_reason || std::string(cell.locked_reason) != reason) return false;
        }
    return true;
}
bool PanelAllEditable(const D810SettingsWriteScreen& screen) {
    const auto* model = screen.Panel();
    if (!model) return false;
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto item : kD810ChoiceItems)
            if (!model->At(camera, item).Editable()) return false;
    return true;
}
void ScreenPanelProductTable() {
    // The product table: the panel opens and shows both cameras, and no cell can be picked.
    ScreenBench s(kProduct);
    PrepareProductBench(s.bench);
    auto& screen = *s.screen;
    check(screen.PanelOpenOffered(true) && !screen.PanelOpenOffered(false) && !screen.PanelOpen() && !screen.Panel(), "panel, product: offered when ready, not open");
    check(!screen.RequestPanelOpen(false) && s.bench.access.opens[0] + s.bench.access.opens[1] == 0, "panel, product: not ready, nothing read");
    check(screen.RequestPanelOpen(true) && screen.Busy() && !screen.PanelEditable() && !screen.PanelOpenOffered(true), "panel, product: a read is running");
    check(screen.OfferText(true).find(L"読んでいます") != std::wstring::npos, "panel, product: the offer says it reads");
    check(s.WaitIdle() && screen.PanelOpen() && screen.Panel() && screen.Step() == D810WriteScreenStep::Idle && !screen.View(), "panel, product: open, no view");
    check(s.bench.access.opens[0] == 1 && s.bench.access.opens[1] == 1 && !s.bench.access.overlap && s.bench.Writes() == 0, "panel, product: read only, one body at a time");
    check(PanelAllLocked(screen, "chosen_values_disabled") && screen.PanelEditable(), "panel, product: every cell is locked by the table");
    // Nothing can be picked, not even a value the cell offers.
    std::size_t tried = 0;
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto item : kD810ChoiceItems)
            for (const auto& option : screen.Panel()->At(camera, item).options) {
                ++tried;
                check(!screen.PanelPick(camera, item, option.value), "panel, product: a pick is refused");
            }
    for (const auto item : kD810ChoiceItems) check(!screen.PanelPickSameAsA(item) && !screen.PanelSameAsAOffered(item), "panel, product: same-as-A is refused");
    check(tried > 20 && screen.PanelEntryCount() == 0 && screen.PanelDirty() == 0 && screen.PanelPlanned().empty(), "panel, product: no pick, no change");
    const auto rows = screen.PanelRows();
    check(rows.size() == 7 && !rows[0].choosable && !rows[1].choosable && rows[0].label == L"露出モード" && rows[1].label == L"ISO感度自動制御", "panel, product: two read-only rows and five to pick");
    bool noted = true;
    for (std::size_t i = 2; i < rows.size(); ++i) noted = noted && rows[i].choosable && rows[i].note[0] == L"この版では選べません" && rows[i].note[1] == L"この版では選べません";
    check(noted, "panel, product: each cell says why it cannot be picked");
    check(screen.PanelSummaryText().find(L"値を選んで書くことはできません") != std::wstring::npos, "panel, product: the line under the panel says it is read only");
    check(screen.PanelCancelAll() == 0 && screen.PanelCloseOffered(), "panel, product: nothing to cancel, can close");
    check(screen.ClosePanel() == 0 && !screen.PanelOpen() && !screen.Panel() && s.bench.Writes() == 0, "panel, product: closed, nothing written");
}
void ScreenPanelPicks() {
    ScreenBench s(kChosenTable);
    auto& screen = *s.screen;
    check(OpenPanel(s), "panel: open");
    const auto serial = screen.PanelSerial();
    check(PanelAllEditable(screen) && screen.PanelEditable(), "panel: the cells can be picked");
    const auto* model = screen.Panel();
    const auto& iso_b = model->At(D810Camera::B, D810ChoiceItem::Iso);
    check(iso_b.readable && iso_b.current == 100 && iso_b.Offers(200) && !iso_b.Offers(999) && model->At(D810Camera::A, D810ChoiceItem::Iso).current == 64, "panel: the model holds what each camera reads and offers");
    // Rows: the two that are only read, then the five to pick, with the difference of the two cameras.
    {
        const auto rows = screen.PanelRows();
        check(rows.size() == 7 && rows[0].value[0] == L"M" && rows[0].value[1] == L"M" && rows[0].difference == L"=" && rows[1].value[0] == L"OFF" && rows[1].difference == L"=", "panel: read-only rows");
        check(rows[2].item == D810ChoiceItem::Iso && rows[2].difference == L"≠" && rows[5].item == D810ChoiceItem::WhiteBalance && rows[5].difference == L"=", "panel: the difference column");
        check(rows[2].note[0].empty() && rows[2].note[1].empty(), "panel: no note without a pick or a lock");
    }
    // A pick changes the draft and nothing else.
    check(screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200) && screen.PanelDirty() == 1 && screen.PanelEntryCount() == 1, "panel: a pick");
    check(screen.PanelShownValue(D810Camera::B, D810ChoiceItem::Iso) == 200 && screen.PanelShownValue(D810Camera::A, D810ChoiceItem::Iso) == 64, "panel: the cell shows the pick");
    check(screen.PanelSerial() == serial, "panel: a pick does not replace the model");
    check(!screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 999) && !screen.PanelPick(D810Camera::A, D810ChoiceItem::Aperture, 12345) && screen.PanelDirty() == 1, "panel: a value the cell does not offer is refused");
    check(screen.PanelRows()[2].note[1] == L"＊変更予定" && screen.PanelRows()[2].note[0].empty(), "panel: the pick is marked");
    check(screen.PanelSummaryText().find(L"変更予定 1 件（まだ何も書いていません）") != std::wstring::npos, "panel: the line under the panel counts the changes");
    // Same as CAM-A: B follows A's pick.
    check(screen.PanelSameAsAOffered(D810ChoiceItem::Iso) && screen.PanelPickSameAsA(D810ChoiceItem::Iso) && screen.PanelShownValue(D810Camera::B, D810ChoiceItem::Iso) == 64, "panel: B the same as A");
    check(screen.PanelRows()[2].note[1] == L"＊変更予定（A と同じ）", "panel: the follow is marked");
    check(screen.PanelPick(D810Camera::A, D810ChoiceItem::Iso, 200) && screen.PanelShownValue(D810Camera::B, D810ChoiceItem::Iso) == 200 && screen.PanelDirty() == 2, "panel: B follows A's pick");
    {
        const auto planned = screen.PanelPlanned();
        check(planned.size() == 2 && planned[0].camera == D810Camera::B && planned[0].value == 200 && planned[0].current == 100 && planned[1].camera == D810Camera::A && planned[1].value == 200 && planned[1].current == 64,
            "panel: the changes are CAM-B first, then CAM-A");
    }
    check(screen.PanelCancelAll() == 2 && screen.PanelDirty() == 0 && screen.PanelEntryCount() == 0 && screen.PanelShownValue(D810Camera::B, D810ChoiceItem::Iso) == 100, "panel: cancel drops every pick");
    check(s.bench.Writes() == 0 && s.bench.access.opens[0] == 1 && s.bench.access.opens[1] == 1, "panel: picks and cancels sent nothing and opened nothing");
}
void ScreenPanelDiscards() {
    {
        // A reread with picks up: the count is said, the picks are gone, the model is the new read.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200) && screen.PanelPick(D810Camera::A, D810ChoiceItem::Aperture, 560) && screen.PanelDirty() == 2, "discard: two changes");
        const auto before = screen.Panel()->read_generation;
        s.bench.b.props[0x500f].current = 400;
        check(screen.RequestPanelReread(true) && screen.Busy() && !screen.PanelEditable(), "discard: the reread runs");
        check(s.WaitIdle(), "discard: the reread ended");
        check(Shows(screen.View(), L"書いていない変更 2 件を捨てました") && screen.PanelOpen() && screen.PanelDirty() == 0 && screen.PanelEntryCount() == 0, "discard: the count is said and the picks are gone");
        check(screen.StatusLine().find(L"書いていない変更 2 件を捨てました") != std::wstring::npos, "discard: the status line says the count");
        check(screen.Panel()->read_generation != before && screen.Panel()->At(D810Camera::B, D810ChoiceItem::Iso).current == 400, "discard: the new model holds the new read");
        // The panel was read once, the reread read each camera once, and the panel was read once more.
        check(screen.PanelEditable() && s.bench.Writes() == 0 && s.bench.access.opens[0] == 3 && s.bench.access.opens[1] == 3 && !s.bench.access.overlap, "discard: read again, one body at a time, nothing written");
        // A reread with no picks says nothing about them.
        check(screen.RequestPanelReread(true) && s.WaitIdle() && !Shows(screen.View(), L"捨てました"), "discard: no picks, nothing said");
    }
    {
        // Closing with picks up.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(D810Camera::B, D810ChoiceItem::Metering, 2) && screen.PanelDirty() == 1, "close: one change");
        check(screen.ClosePanel() == 1 && !screen.PanelOpen() && !screen.Panel() && screen.StatusLine().find(L"書いていない変更 1 件を捨てて") != std::wstring::npos, "close: the count is said");
        check(screen.PanelDirty() == 0 && screen.PanelEntryCount() == 0 && !screen.PanelPick(D810Camera::B, D810ChoiceItem::Metering, 2), "close: nothing is left of the picks");
        check(screen.ClosePanel() == 0 && s.bench.Writes() == 0, "close: twice does nothing");
        // Opened again, the panel starts from a new read with no picks.
        check(OpenPanel(s) && screen.PanelDirty() == 0 && screen.PanelEntryCount() == 0, "close: opened again with no picks");
    }
    {
        // A Live restart or a new stage A read (ClearView) takes the panel with it.
        ScreenBench s(kChosenTable);
        check(OpenPanel(s) && s.screen->PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200), "clear: picks up");
        s.screen->ClearView();
        check(!s.screen->PanelOpen() && !s.screen->Panel() && s.screen->PanelDirty() == 0, "clear: the panel is gone");
    }
}
void ScreenPanelLocksAndFailures() {
    {
        // CAM-A not in M: its cells say so, and so does the read-only row; CAM-B can still be picked.
        ScreenBench s(kChosenTable);
        s.bench.a.props[0x500e].current = 3;
        check(OpenPanel(s), "locks: open");
        const auto rows = s.screen->PanelRows();
        check(rows[0].value[0] == L"A" && rows[0].note[0] == L"本体を M に" && rows[0].note[1].empty(), "locks: the read-only row says A is not in M");
        check(rows[2].note[0] == L"本体を M に" && rows[2].note[1].empty() && !s.screen->PanelPick(D810Camera::A, D810ChoiceItem::Iso, 200) && s.screen->PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200), "locks: A cannot be picked, B can");
    }
    {
        // CAM-A's firmware is not the one the table names: A's cells are locked, B's are not.
        ScreenBench s(kChosenTable);
        s.bench.access.firmware[0] = "9.99";
        check(OpenPanel(s), "version: open");
        const auto rows = s.screen->PanelRows();
        check(rows[2].note[0] == L"本体が未確認です" && rows[2].note[1].empty() && s.screen->Panel()->At(D810Camera::B, D810ChoiceItem::Iso).Editable(), "version: only A is locked");
    }
    {
        // CAM-B's firmware is not the one the table names: no read for a plan completes, so the panel does not open.
        ScreenBench s(kChosenTable);
        s.bench.access.firmware[1] = "9.99";
        check(s.screen->RequestPanelOpen(true) && s.WaitIdle() && !s.screen->PanelOpen() && !s.screen->Panel(), "version: B not verified, no panel");
        check(Shows(s.screen->View(), L"両台の設定を読めませんでした") && Shows(s.screen->View(), L"本体が未確認") && s.bench.Writes() == 0, "version: the reason is said");
    }
    {
        // A camera that cannot be opened.
        ScreenBench s(kChosenTable);
        s.bench.access.open_fail[0] = true;
        check(s.screen->RequestPanelOpen(true) && s.WaitIdle() && !s.screen->PanelOpen() && Shows(s.screen->View(), L"読めませんでした"), "open failure: no panel");
    }
    {
        // A copy while the panel is open: the picks wait behind the confirmation, the run makes the panel old.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(D810Camera::A, D810ChoiceItem::Aperture, 560), "copy: one pick");
        check(s.Copy() && screen.ConfirmationShown(), "copy: the confirmation is up over the open panel");
        check(!screen.PanelEditable() && !screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200) && !screen.PanelCloseOffered() && !screen.PanelOpenOffered(true), "copy: no pick, no close, no second panel meanwhile");
        check(s.Confirm() && screen.PanelOpen() && screen.PanelStale() && !screen.PanelEditable() && screen.PanelDirty() == 0, "copy: the panel is old after the run");
        check(Shows(screen.View(), L"書いていない変更 1 件は捨てました") && screen.PanelSummaryText().find(L"書き込みのあと") != std::wstring::npos, "copy: the dropped pick is said");
        check(!screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200), "copy: an old panel takes no pick");
        check(screen.RequestPanelReread(true) && s.WaitIdle() && !screen.PanelStale() && screen.PanelEditable() && screen.Panel()->At(D810Camera::B, D810ChoiceItem::Iso).current == 64,
            "copy: a reread shows what was written");
    }
}
// --- The plan of the picks, its confirmation and its result, through the screen -------------------------------------
std::size_t LineIndex(const D810WriteScreenView* view, const std::wstring& part) {
    if (!view) return static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < view->lines.size(); ++i)
        if (view->lines[i].find(part) != std::wstring::npos) return i;
    return static_cast<std::size_t>(-1);
}
void ScreenChosenFlow() {
    using Item = D810ChoiceItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        // One change on CAM-B: read again, planned, shown, confirmed, written, read back.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200), "chosen: a pick");
        check(!screen.ChosenOffered(false) && screen.ChosenOffered(true) && screen.ChosenOfferText(true).find(L"1 件を書き込む…") != std::wstring::npos, "chosen: offered when ready, and the text says what it does");
        const int opens_a = s.bench.access.opens[0];
        check(screen.RequestChosen(true) && screen.Busy() && !screen.ChosenOffered(true) && !screen.PanelEditable(), "chosen: the read runs");
        check(screen.OfferText(true).find(L"もう一度読んでいます") != std::wstring::npos, "chosen: the offer says it reads again");
        check(s.WaitIdle() && screen.Step() == D810WriteScreenStep::Confirming && screen.ConfirmationShown(), "chosen: the confirmation is up");
        check(s.bench.access.opens[0] == opens_a + 1 && s.bench.Writes() == 0 && !s.bench.access.overlap && screen.Status().plan_pending && !screen.Status().plan_confirmed, "chosen: a new read, nothing written, not confirmed");
        const auto* view = screen.View();
        check(view && view->lines.size() == 6 && view->lines.size() <= D810WriteViewMaxLines(), "chosen: six lines");
        check(Shows(view, L"自動では元に戻りません") && Shows(view, L"1 回だけ") && Shows(view, L"再送しません") && Shows(view, L"書く順: CAM-B の項目を先に、CAM-A の項目を最後に書きます") &&
                Shows(view, L"項目: CAM-B の今の値 → 書く値 ／ CAM-A の今の値 → 書く値"),
            "chosen: the notices and the order");
        check(Shows(view, L"  ISO感度: CAM-B 100 → 200 ／ CAM-A 変えない") && !Shows(view, L"基準のカメラ") && !Shows(view, L"本体の値が変わった"), "chosen: the item, B's value now and to write, A not changed");
        check(Shows(view, L"よければ「書き込む（1回だけ）」") && screen.PanelOpen() && !screen.PanelEditable() && !screen.PanelPick(B, Item::Metering, 2) && !screen.PanelCloseOffered(), "chosen: the panel waits behind the confirmation");
        check(s.Confirm(), "chosen: confirmed and run");
        check(screen.Step() == D810WriteScreenStep::Idle && s.bench.b.props[0x500f].current == 200 && s.bench.a.write_begins == 0 && s.bench.Writes() == 1 && NoResend(s.bench.b), "chosen: B written once, A untouched");
        check(Shows(screen.View(), L"選んだ値を書きました: 一致（1 項目・1 件、読み戻した値が書いた値と同じ）") && Shows(screen.View(), L"  ISO感度: CAM-B 一致（読み戻し 200） ／ CAM-A 変えない") &&
                Shows(screen.View(), L"再送も、前の値への書き戻しもしていません。"),
            "chosen: the result");
        check(screen.PanelStale() && screen.PanelDirty() == 0 && !screen.ChosenOffered(true) && screen.ChosenOfferText(true).find(L"書き込みのあと") != std::wstring::npos, "chosen: the panel is old after the run");
        check(s.bench.Journal().find("source=chosen") != std::string::npos && s.bench.Journal().find("source=copy") == std::string::npos, "chosen: the record says chosen");
        const bool logged = std::any_of(s.log.begin(), s.log.end(), [](const auto& line) { return line.find("settings_write_ui_chosen_requested=1 changes=1") != std::string::npos; });
        check(logged && std::none_of(s.log.begin(), s.log.end(), [](const auto& line) { return HasIdentifierShape(line); }), "chosen: logged without identifiers");
        check(screen.RequestPanelReread(true) && s.WaitIdle() && !screen.PanelStale() && screen.Panel()->At(B, Item::Iso).current == 200, "chosen: a reread shows the written value");
    }
    {
        // Both cameras: the rows follow the steps (CAM-B's shutter speed first, then CAM-A's ISO), the writes go B then A.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::ShutterSpeed, 1000) && screen.PanelPick(A, Item::Iso, 200), "both: two picks");
        s.bench.TraceBegins();
        check(screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "both: the confirmation is up");
        const auto* view = screen.View();
        check(view && view->lines.size() == 8, "both: eight lines");
        const auto shutter = LineIndex(view, L"  シャッター速度: CAM-B 1/30 → 1/10 ／ CAM-A 変えない");
        const auto iso = LineIndex(view, L"  ISO感度: CAM-B 変えない ／ CAM-A 64 → 200");
        check(shutter != static_cast<std::size_t>(-1) && iso != static_cast<std::size_t>(-1) && shutter < iso, "both: the rows follow the plan's steps, not the item table");
        check(Shows(view, L"CAM-A は合成と校正の基準のカメラです。承認済みの撮影の設定と違う値になると、撮影の前の照合で止まります。"), "both: the note about CAM-A");
        check(s.Confirm() && s.bench.begin_order == std::vector<std::pair<int, int>>{{1, 0x500d}, {0, 0x500f}} && NoResend(s.bench.a) && NoResend(s.bench.b), "both: written CAM-B first, CAM-A last");
        const auto* result = screen.View();
        check(Shows(result, L"  シャッター速度: CAM-B 一致（読み戻し 1/10） ／ CAM-A 変えない") && Shows(result, L"  ISO感度: CAM-B 変えない ／ CAM-A 一致（読み戻し 200）") &&
                Shows(result, L"選んだ値を書きました: 一致（2 項目"),
            "both: the result per item");
        check(LineIndex(result, L"シャッター速度") < LineIndex(result, L"ISO感度"), "both: the result lines follow the run");
    }
    {
        // CAM-B's write does not take: CAM-A is not written, the result says so, and the capture block is told in terms of the values.
        ScreenBench s(kChosenTable, true);
        auto& screen = *s.screen;
        s.bench.b.apply_as[0x500f] = 400;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200) && screen.PanelPick(A, Item::Iso, 200), "failure: two picks");
        check(screen.RequestChosen(true) && s.WaitIdle() && s.Confirm(), "failure: run");
        check(s.bench.a.write_begins == 0 && s.bench.b.write_begins == 1, "failure: CAM-A was not written");
        check(Shows(screen.View(), L"  ISO感度: CAM-B 不一致（読み戻し 400） ／ CAM-A 書いていません") && Shows(screen.View(), L"選んだ値の書き込み: 不一致（送った 1 項目・1 件）") &&
                Shows(screen.View(), L"止めた理由: 読み戻した値が書いた値と違いました（不一致）"),
            "failure: the result says what was and was not written");
        check(screen.Status().block == D810CaptureBlock::Mismatch && screen.Status().chosen_history && !screen.Status().capture_allowed, "failure: capture is held, the history is a chosen plan's");
        check(Shows(screen.View(), L"書いた値が、読み戻しで確かめられていません") && !Shows(screen.View(), L"CAM-A と合っていません") && screen.CaptureRefusalText().find(L"書いた値が、読み戻しで確かめられていません") != std::wstring::npos &&
                screen.CaptureRefusalText().find(L"CAM-A と合っていません") == std::wstring::npos,
            "failure: the block text is about the values written");
        // The reread is told in terms of the values written, camera by camera; the two cameras are not compared.
        check(screen.RequestPanelReread(true) && s.WaitIdle(), "failure: reread");
        check(Shows(screen.View(), L"読み直しました: 書いた値と違う項目があります。") && Shows(screen.View(), L"  ISO感度: CAM-B は、書いた値になっていません") && Shows(screen.View(), L"  ISO感度: CAM-A は、書いた値になっていません") &&
                !Shows(screen.View(), L"CAM-A と CAM-B で違う"),
            "failure: reread, not at the written values");
        s.bench.a.props[0x500f].current = 200;
        s.bench.b.props[0x500f].current = 200;
        check(screen.RequestPanelReread(true) && s.WaitIdle() && Shows(screen.View(), L"読み直しました: 書いた値は、各台でそのまま読めました。") && screen.Status().capture_allowed, "failure: reread at the written values releases the block");
        check(!screen.PanelStale() && screen.Panel()->At(A, Item::Iso).current == 200, "failure: the panel shows the values read");
    }
    {
        // The product table: nothing can be picked, so nothing can be asked.
        ScreenBench s(kProduct);
        PrepareProductBench(s.bench);
        auto& screen = *s.screen;
        check(OpenPanel(s) && !screen.ChosenOffered(true) && !screen.RequestChosen(true) && !screen.Busy(), "product: nothing to write is not asked");
        check(screen.ChosenOfferText(true).find(L"変更予定がありません") != std::wstring::npos && s.bench.access.opens[0] == 1 && s.bench.access.opens[1] == 1 && s.bench.Writes() == 0, "product: nothing was read again or written");
        check(screen.Step() == D810WriteScreenStep::Idle && !screen.Status().plan_pending, "product: no plan");
    }
    {
        // "やめる" returns to the panel with the picks.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200), "discard: a pick");
        check(screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "discard: confirmation");
        screen.DiscardPlan();
        check(screen.Step() == D810WriteScreenStep::Idle && !s.bench.controller->Status().plan_pending && Shows(screen.View(), L"やめました") && screen.PanelEditable() && screen.PanelDirty() == 1 && s.bench.Writes() == 0,
            "discard: back to the panel with the pick");
        check(!screen.ConfirmShownPlan(true) && s.bench.Writes() == 0, "discard: nothing to confirm");
    }
}
void ScreenChosenRefusals() {
    using Item = D810ChoiceItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        // CAM-B left manual mode after the panel was read: the new read finds it, no confirmation, the picks stay.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200), "refusal: a pick");
        s.bench.b.props[0x500e].current = 3;
        check(screen.RequestChosen(true) && s.WaitIdle() && screen.Step() == D810WriteScreenStep::Idle && !screen.ConfirmationShown(), "refusal: no confirmation");
        check(Shows(screen.View(), L"書けません。何も書いていません。") && Shows(screen.View(), L"CAM-B が M ではないため、露出モードは書けません。CAM-B の本体で M にしてください"), "refusal: the reason names CAM-B");
        check(screen.PanelOpen() && screen.PanelDirty() == 1 && screen.PanelEditable() && s.bench.Writes() == 0 && !s.bench.controller->Status().plan_pending, "refusal: the pick stays, nothing was written");
    }
    {
        // CAM-A is not in manual mode after the panel was read: the words are about CAM-A.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(A, Item::Iso, 200), "refusal A: a pick");
        s.bench.a.props[0x500e].current = 3;
        check(screen.RequestChosen(true) && s.WaitIdle() && Shows(screen.View(), L"CAM-A が M ではないため、露出モードは書けません。CAM-A の本体で M にしてください") && !Shows(screen.View(), L"CAM-B"), "refusal A: the reason names CAM-A");
    }
    {
        // The flags of the new read are the ones the plan takes: CAM-A's body is not the one the table names any more.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(A, Item::Iso, 200), "version: a pick");
        s.bench.access.firmware[0] = "9.99";
        check(screen.RequestChosen(true) && s.WaitIdle() && !screen.ConfirmationShown(), "version: no confirmation");
        check(Shows(screen.View(), L"書く相手の本体の機種・版が表と違います") && Shows(screen.View(), L"CAM-A") && s.bench.Writes() == 0 && s.bench.a.write_begins == 0, "version: CAM-A is refused");
    }
    {
        // A pick that the new read already holds is not a step.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200), "same: a pick");
        s.bench.b.props[0x500f].current = 200;
        check(screen.RequestChosen(true) && s.WaitIdle() && !screen.ConfirmationShown() && Shows(screen.View(), L"書くものはありません") && !s.bench.controller->Status().plan_pending, "same: nothing to write");
    }
    {
        // A value that changed after the panel was read is said, and so is a value the application cannot set again.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        s.bench.a.props[0x5005].current = 0x8013;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200) && screen.PanelPick(A, Item::WhiteBalance, 4), "changed: two picks");
        s.bench.b.props[0x500f].current = 400;
        check(screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "changed: confirmation");
        const auto* view = screen.View();
        check(Shows(view, L"  ISO感度: CAM-B 400 → 200 ／ CAM-A 変えない") && Shows(view, L"設定の画面を開いた後に、本体の値が変わった項目があります。"), "changed: the change since the panel is said");
        check(Shows(view, L"ホワイトバランス: CAM-B 変えない ／ CAM-A Preset → 晴天 (0x0004)（元の値は本体でしか戻せません）"), "changed: a value that cannot be set again says so");
        check(view->lines.size() == 9, "changed: nine lines");
    }
    {
        // The most lines: five items on both cameras, and the note of a change: within the window's limit and its smallest height.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s), "most: open");
        const std::vector<std::tuple<D810Camera, Item, std::int64_t>> picks{{B, Item::Iso, 400}, {B, Item::ShutterSpeed, 1000}, {B, Item::Aperture, 800}, {B, Item::WhiteBalance, 4}, {B, Item::Metering, 2},
            {A, Item::Iso, 200}, {A, Item::ShutterSpeed, 333}, {A, Item::Aperture, 560}, {A, Item::WhiteBalance, 5}, {A, Item::Metering, 2}};
        bool all = true;
        for (const auto& [camera, item, value] : picks) all = screen.PanelPick(camera, item, value) && all;
        check(all && screen.PanelDirty() == 10, "most: ten changes");
        s.bench.b.props[0x500f].current = 64; // the value of one item changed since the panel was read
        s.bench.TraceBegins();
        check(screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "most: confirmation");
        const auto* view = screen.View();
        check(view && view->lines.size() == 12 && view->lines.size() <= D810WriteViewMaxLines() && D810WriteViewFits(*view, kD810WriteMinClientHeight), "most: twelve lines, in the window's limit and its smallest height");
        check(D810ConfirmPressable(view, screen.ViewSerial(), screen.ViewSerial(), kD810WriteMinClientWidth, kD810WriteMinClientHeight), "most: pressable once drawn whole, in the smallest window");
        check(s.Confirm() && s.bench.Writes() == 10 && NoResend(s.bench.a) && NoResend(s.bench.b), "most: ten writes, once each");
        bool order = s.bench.begin_order.size() == 10;
        for (std::size_t i = 0; order && i < 10; ++i) order = s.bench.begin_order[i].first == (i < 5 ? 1 : 0);
        check(order, "most: five on CAM-B, then five on CAM-A");
        check(Shows(screen.View(), L"選んだ値を書きました: 一致（5 項目・10 件") && screen.View()->lines.size() == 7 && screen.View()->lines.size() <= D810WriteViewMaxLines(), "most: the result in seven lines");
    }
    {
        // The symbols of a release by the values written, as lines; a symbol that is not known is shown as it is.
        const auto has = [](const char* symbol, const wchar_t* part) { return D810ChosenDifferenceText(symbol).find(part) != std::wstring::npos; };
        check(has("record_unreadable", L"操作の記録を読めないので、解けません。記録のファイルは消さない・動かさないで、人が確かめてください"), "difference: record unreadable");
        check(has("exposure_mode:CAM-B:not_manual", L"CAM-B の露出モードが M ではありません") && has("exposure_mode:CAM-A:unreadable", L"CAM-A の露出モードを読めませんでした"), "difference: exposure mode");
        check(has("iso_auto:CAM-A:not_off", L"CAM-A の ISO 自動が OFF ではありません") && has("iso_auto:CAM-B:unreadable", L"CAM-B の ISO 自動を読めませんでした"), "difference: ISO auto");
        check(has("iso:CAM-B:not_planned_value", L"ISO感度: CAM-B は、書いた値になっていません") && has("iso:CAM-A:unreadable", L"ISO感度: CAM-A は読めません") &&
                has("white_balance:CAM-B:content_dependent", L"ホワイトバランス: CAM-B の値は、本体の別の中身に頼るため確かめられません") && has("nothing_planned", L"書く予定だった値が記録に残っていません"),
            "difference: the symbols of the planned values");
        check(has("iso:CAM-B:something_new", L"差の符号: iso:CAM-B:something_new") && has("something_new", L"差の符号: something_new") && has("iso", L"差の符号: iso"), "difference: an unknown symbol is shown as it is");
    }
    {
        // The words of a flow of picked values are not those of a copy.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(D810Camera::B, D810ChoiceItem::Iso, 200) && screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "words: a confirmation of picks");
        const auto confirming = screen.OfferText(true);
        check(confirming.find(L"書く内容を確かめて") != std::wstring::npos && confirming.find(L"写す") == std::wstring::npos && screen.StatusLine().find(L"書く内容") != std::wstring::npos, "words: the confirmation says write, not copy");
        screen.DiscardPlan();
        check(Shows(screen.View(), L"書くのをやめました") && !Shows(screen.View(), L"写す"), "words: and so does the discard");
        check(D810PlanReasonText("plan_changed:CAM-A:changed", true).find(L"N 件を書き込む…") != std::wstring::npos && D810PlanReasonText("plan_changed:CAM-A:changed", true).find(L"「写す」") == std::wstring::npos &&
                D810WriteReasonText("plan_changed:CAM-B:changed").find(L"「写す」") != std::wstring::npos,
            "words: the reason of a changed body names the button of the flow");
    }
    {
        // A read that returns early (no options, cameras busy) also makes the earlier read old.
        Bench bench(kChosenTable);
        const auto read = bench.controller->ReadForPlan(bench.access, bench.options);
        check(bench.controller->PlanChosen(read, {PickFor(D810Camera::B, D810WriteItem::Iso, 200)}).Ok(), "early return: the read plans");
        D810WriteRunOptions none;
        const auto early = bench.controller->ReadForPlan(bench.access, none);
        check(!early.complete && early.reason == "invalid_options" && HasReason(bench.controller->PlanChosen(read, {PickFor(D810Camera::B, D810WriteItem::Iso, 200)}), "read_not_current"), "early return: an earlier read is gone after invalid options");
        const auto again = bench.controller->ReadForPlan(bench.access, bench.options);
        check(bench.owner.TryAcquire(D810CameraUse::Live), "early return: the cameras are taken");
        const auto busy = bench.controller->ReadForPlan(bench.access, bench.options);
        check(busy.reason == "cameras_busy" && HasReason(bench.controller->PlanChosen(again, {PickFor(D810Camera::B, D810WriteItem::Iso, 200)}), "read_not_current"), "early return: an earlier read is gone after cameras_busy");
        bench.owner.Release(D810CameraUse::Live);
    }
    {
        // Words that are not about CAM-B alone, and the block text of a copy stay as they were.
        check(D810PlanReasonText("exposure_mode_not_manual_set_on_body:CAM-B").find(L"CAM-B が M ではなく、露出モードは書けません。B の本体で M にしてください") != std::wstring::npos, "words: the copy's reason");
        check(D810PlanReasonText("value_not_advertised:iso:CAM-A", true).find(L"この台が、その値を受け付けると答えていません") != std::wstring::npos &&
                D810PlanReasonText("value_not_advertised:iso:CAM-B").find(L"CAM-B がその値を受け付けると答えていません") != std::wstring::npos,
            "words: a plan of picks does not say CAM-B for CAM-A");
        check(D810PlanReasonText("chosen_values_disabled", true).find(L"この版では、選んだ値を書くことはできません") != std::wstring::npos && D810PlanReasonText("no_choice", true).find(L"書く値が選ばれていません") != std::wstring::npos,
            "words: the plan-wide reasons");
        check(D810CaptureBlockText(D810CaptureBlock::Mismatch).find(L"CAM-A と合っていません") != std::wstring::npos && D810CaptureBlockText(D810CaptureBlock::Mismatch, true).find(L"CAM-A と合っていません") == std::wstring::npos &&
                D810CaptureBlockText(D810CaptureBlock::Unknown, true) == D810CaptureBlockText(D810CaptureBlock::Unknown),
            "words: only a mismatch of picked values is told differently");
        check(D810CaptureBlockText(D810CaptureBlock::Unreadable).find(L"消さない・動かさない") != std::wstring::npos && D810CaptureBlockText(D810CaptureBlock::Unreadable).find(L"退避") == std::wstring::npos &&
                D810ChosenDifferenceText("record_unreadable").find(L"消さない・動かさない") != std::wstring::npos,
            "words: an unreadable record has one instruction: do not delete or move the file");
        check(D810PlanReasonText("plan_changed:CAM-A:changed", true).find(L"「A と同じ」の元にした CAM-A") != std::wstring::npos, "words: a changed body is not taken to be the one written");
        check(std::wstring(kD810RecordDamagedAdvice).find(L"計画と合わない") != std::wstring::npos, "words: the damaged record is not only a line that cannot be read");
    }
}
// An access that throws once told to: the exception comes out of the controller call that runs on the worker.
struct ThrowingAccess final: D810SettingsCameraAccess {
    ThrowingAccess(D810SettingsCameraAccess& inner, std::shared_ptr<std::atomic<bool>> throwing): inner_(inner), throwing_(std::move(throwing)) {}
    D810OpenedCamera Open(D810Camera camera) override {
        if (throwing_->load()) throw std::runtime_error("open");
        return inner_.Open(camera);
    }
    bool Close(D810Camera camera) override { return inner_.Close(camera); }
private:
    D810SettingsCameraAccess& inner_;
    std::shared_ptr<std::atomic<bool>> throwing_;
};
void ScreenChosenWorkerFailure() {
    using Item = D810ChoiceItem;
    ScreenBench s(kChosenTable);
    auto throwing = std::make_shared<std::atomic<bool>>(false);
    D810SettingsWriteScreen::Options options;
    options.log = [&s](const std::string& line) { s.log.push_back(line); };
    options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
    s.screen = std::make_unique<D810SettingsWriteScreen>(*s.bench.controller, [&s, throwing] { return std::make_unique<ThrowingAccess>(s.bench.access, throwing); }, options);
    auto& screen = *s.screen;
    check(OpenPanel(s) && screen.PanelPick(D810Camera::B, Item::Iso, 200) && screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "worker failure: a confirmation is up");
    check(screen.Status().plan_pending, "worker failure: a plan waits");
    throwing->store(true);
    check(screen.ConfirmShownPlan(true) && s.WaitIdle(), "worker failure: confirmed, the worker ends with an exception");
    throwing->store(false);
    check(screen.Step() == D810WriteScreenStep::Idle && Shows(screen.View(), L"設定の処理を最後まで終えられませんでした") && Shows(screen.View(), L"書いたかもしれないので、「設定を読み直す」で確かめてください"),
        "worker failure: it says that it may have written");
    check(!screen.Status().plan_pending && !screen.Status().plan_confirmed && !screen.ConfirmOffered(true), "worker failure: no plan stays up");
    check(screen.PanelStale() && screen.PanelDirty() == 0 && screen.PanelEntryCount() == 0 && !screen.ChosenOffered(true), "worker failure: the panel is old and the picks are used up");
    const auto summary = screen.PanelSummaryText();
    check(summary.find(L"書き込みのあと") != std::wstring::npos && summary.find(L"まだ何も書いていません") == std::wstring::npos, "worker failure: the panel does not say that nothing was written");
    check(summary.find(L"書き込みのあと") != std::wstring::npos && !screen.PanelCamerasWereFine(), "worker failure: and the cameras are not taken to have answered");
    check(Shows(screen.View(), L"選んでいた変更 1 件の下書きは捨てました"), "worker failure: the picks dropped are counted");
    // A read that fails does not make the panel old: nothing was sent.
    check(screen.RequestPanelReread(true) && s.WaitIdle() && !screen.PanelStale(), "worker failure: a reread makes the panel current again");
    check(screen.PanelPick(D810Camera::B, Item::Iso, 400), "worker failure: a pick");
    throwing->store(true);
    check(screen.RequestChosen(true) && s.WaitIdle() && screen.PanelOpen() && !screen.PanelStale() && screen.PanelDirty() == 1, "worker failure: a read that fails leaves the panel and its pick");
    throwing->store(false);
}
void RereadRuleIsTold() {
    using Item = D810WriteItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    {
        // The controller says which rule the reread used: the values written, then the cameras once the record is damaged.
        Bench bench(kChosenTable, -1, true);
        bench.b.apply_as[0x500f] = 400;
        bench.Run(bench.Chosen({PickFor(B, Item::Iso, 200), PickFor(A, Item::Iso, 200)}));
        auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && reread.by_planned_values && !reread.history_damaged, "rule: the values written");
        check(bench.file.Append("this is not a line of the record"), "rule: damage");
        reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.by_planned_values && reread.history_damaged, "rule: the cameras, the record is damaged");
        check(ParseD810Journal(bench.Journal()).history_damaged && !D810ReleasesByPlannedValues(ParseD810Journal(bench.Journal())), "rule: one condition for both");
    }
    {
        Bench bench(kAdr38Table, -1, true);
        bench.CopyRun();
        const auto reread = bench.controller->Reread(bench.access, bench.options);
        check(reread.complete && !reread.by_planned_values && !reread.history_damaged, "rule: a copy history compares the cameras");
    }
}
void ScreenRereadUsesTheRuleOfTheCheck() {
    using Item = D810ChoiceItem;
    // The record changes outside the application after the screen last looked at it: the reread is told by the rule that was used.
    ScreenBench s(kChosenTable, true);
    auto& screen = *s.screen;
    s.bench.b.apply_as[0x500f] = 400;
    check(OpenPanel(s) && screen.PanelPick(D810Camera::B, Item::Iso, 200) && screen.RequestChosen(true) && s.WaitIdle() && s.Confirm(), "check rule: a chosen run that did not take");
    check(screen.Status().chosen_history, "check rule: the screen has seen a chosen history");
    check(s.bench.file.Append("this is not a line of the record"), "check rule: the record is damaged behind the screen's back");
    check(screen.RequestPanelReread(true) && s.WaitIdle(), "check rule: reread");
    check(Shows(screen.View(), L"CAM-A と CAM-B で違う項目があります") && !Shows(screen.View(), L"書いた値と違う項目") && Shows(screen.View(), kD810RecordDamagedAdvice), "check rule: told by the rule the check used");
}
void ScreenPanelFollowAndCount() {
    using Item = D810ChoiceItem;
    {
        // "Same as CAM-A" is taken away, with a note, when CAM-A is then given a value that CAM-B does not offer.
        ScreenBench s(kChosenTable);
        s.bench.b.props[0x500f] = {4, 1, 100, 2, {64, 100, 200}};
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPickSameAsA(Item::Iso) && screen.PanelShownValue(D810Camera::B, Item::Iso) == 64 && screen.PanelDirty() == 1, "follow: B follows A");
        check(screen.PanelPick(D810Camera::A, Item::Iso, 200) && screen.PanelShownValue(D810Camera::B, Item::Iso) == 200 && screen.PanelDirty() == 2, "follow: a value B offers is followed");
        check(screen.PanelPick(D810Camera::A, Item::Iso, 400) && !screen.PanelDraftHas(D810Camera::B, Item::Iso) && screen.PanelShownValue(D810Camera::B, Item::Iso) == 100 && screen.PanelDirty() == 1,
            "follow: a value B does not offer takes the follow away");
        check(screen.PanelSummaryText().find(L"CAM-B はその値を受け付けないため、「A と同じ」を外しました") != std::wstring::npos, "follow: and says so");
        check(screen.PanelPick(D810Camera::A, Item::Iso, 100) && screen.PanelSummaryText().find(L"外しました") == std::wstring::npos, "follow: the note goes with the next pick");
    }
    {
        // The number on the button counts changes, not picks: a pick of the value a camera reads is an entry and not a change.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(D810Camera::B, Item::Iso, 100) && screen.PanelEntryCount() == 1 && screen.PanelDirty() == 0, "count: an entry that changes nothing");
        check(screen.PanelPick(D810Camera::B, Item::Iso, 200) && screen.PanelPick(D810Camera::A, Item::Iso, 64) && screen.PanelEntryCount() == 2 && screen.PanelDirty() == 1, "count: two entries, one change");
check(screen.PanelSummaryText().find(L"変更予定 1 件（") != std::wstring::npos, "count: the line under the panel counts the change, not the picks");
        check(screen.ChosenOffered(true), "count: offered with one change");
        check(screen.PanelCancelAll() == 1 && screen.PanelEntryCount() == 0, "count: cancel counts the changes");
    }
}
// An access that stops opening cameras after a number of opens (an Open that gives nothing, as a camera that is gone).
struct FailingAfterAccess final: D810SettingsCameraAccess {
    FailingAfterAccess(D810SettingsCameraAccess& inner, std::shared_ptr<std::atomic<int>> opens_left): inner_(inner), left_(std::move(opens_left)) {}
    D810OpenedCamera Open(D810Camera camera) override { return left_->fetch_sub(1) > 0 ? inner_.Open(camera) : D810OpenedCamera{}; }
    bool Close(D810Camera camera) override { return inner_.Close(camera); }
private:
    D810SettingsCameraAccess& inner_;
    std::shared_ptr<std::atomic<int>> left_;
};
// The panel's "the cameras answered" mark goes down for each way an operation with the cameras can end without them answering, and comes
// back when a read finishes; closing the panel then does not start Live (the window's smoke checks that part).
void ScreenPanelCamerasFlag() {
    using Item = D810ChoiceItem;
    const auto B = D810Camera::B;
    {
        // A run that was stopped before it sent anything, for a reason.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelCamerasWereFine() && screen.PanelPick(B, Item::Iso, 200) && screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown(), "flag: a confirmation");
        s.bench.b.props[0x500f].current = 400; // CAM-B changes after the confirmation: the run stops before it sends
        check(s.Confirm() && s.bench.Writes() == 0 && !screen.PanelCamerasWereFine(), "flag: a run stopped before sending, for a reason, brings it down");
        check(screen.RequestPanelReread(true) && s.WaitIdle() && screen.PanelCamerasWereFine(), "flag: a read that finishes brings it back");
    }
    {
        // A reread that did not finish, then one that did; a reread whose second read (for the panel) did not finish.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s), "flag reread: open");
        s.bench.access.open_fail[0] = true;
        check(screen.RequestPanelReread(true) && s.WaitIdle() && screen.PanelOpen() && !screen.PanelCamerasWereFine(), "flag: a reread that did not finish brings it down");
        s.bench.access.open_fail[0] = false;
        check(screen.RequestPanelReread(true) && s.WaitIdle() && screen.PanelCamerasWereFine(), "flag: and a reread that finishes brings it back");
        auto left = std::make_shared<std::atomic<int>>(100);
        D810SettingsWriteScreen::Options options;
        options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
        s.screen = std::make_unique<D810SettingsWriteScreen>(*s.bench.controller, [&s, left] { return std::make_unique<FailingAfterAccess>(s.bench.access, left); }, options);
        auto& second = *s.screen;
        check(OpenPanel(s) && &second == s.screen.get() && second.PanelCamerasWereFine(), "flag panel read: open");
        left->store(2); // the reread opens both cameras, the read for the panel then finds none
        check(second.RequestPanelReread(true) && s.WaitIdle() && !second.PanelCamerasWereFine(), "flag: the read for the panel that did not finish brings it down");
        check(Shows(second.View(), L"設定の画面の値は読み直せていません"), "flag: and it says so");
    }
    {
        // The read of a copy that did not finish, with the panel open; the read for picked values that did not finish.
        ScreenBench s(kChosenTable);
        auto& screen = *s.screen;
        check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200), "flag read: open and a pick");
        s.bench.access.open_fail[0] = true;
        check(screen.RequestCopy(true) && s.WaitIdle() && screen.PanelOpen() && !screen.PanelCamerasWereFine(), "flag: the read of a copy that did not finish brings it down");
        s.bench.access.open_fail[0] = false;
        check(screen.RequestPanelReread(true) && s.WaitIdle() && screen.PanelCamerasWereFine() && screen.PanelPick(B, Item::Iso, 200), "flag: back by a reread");
        s.bench.access.open_fail[0] = true;
        check(screen.RequestChosen(true) && s.WaitIdle() && !screen.PanelCamerasWereFine(), "flag: the read for picked values that did not finish brings it down");
    }
}
void ScreenChosenDeadline() {
    using Item = D810ChoiceItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    // The write time of the screen is shortened to 30 s: ten steps need 5 s each in hand, so they would not be started with it. A plan of picks
    // with six steps or more gets 120 s (D810WriteDeadlineFor), so it runs; with the screen's own write time it would stop before sending.
    ScreenBench s(kChosenTable);
    D810SettingsWriteScreen::Options options;
    options.write_deadline = std::chrono::milliseconds(30000);
    options.log = [&s](const std::string& line) { s.log.push_back(line); };
    options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
    s.screen = std::make_unique<D810SettingsWriteScreen>(*s.bench.controller, [&s] { return std::make_unique<BorrowedAccess>(s.bench.access); }, options);
    auto& screen = *s.screen;
    check(OpenPanel(s), "deadline: open");
    const std::vector<std::tuple<D810Camera, Item, std::int64_t>> picks{{B, Item::Iso, 400}, {B, Item::ShutterSpeed, 1000}, {B, Item::Aperture, 800}, {B, Item::WhiteBalance, 4}, {B, Item::Metering, 2},
        {A, Item::Iso, 200}, {A, Item::ShutterSpeed, 333}, {A, Item::Aperture, 560}, {A, Item::WhiteBalance, 5}, {A, Item::Metering, 2}};
    for (const auto& [camera, item, value] : picks) check(screen.PanelPick(camera, item, value), "deadline: pick");
    check(screen.RequestChosen(true) && s.WaitIdle() && screen.ConfirmationShown() && s.Confirm(), "deadline: run");
    check(s.bench.Writes() == 10 && Shows(screen.View(), L"選んだ値を書きました: 一致（5 項目・10 件"), "deadline: ten steps run in the longer time");
    // Five steps are within the screen's own 30 s.
    check(screen.RequestPanelReread(true) && s.WaitIdle() && screen.PanelEditable(), "deadline: reread");
    check(screen.PanelPick(B, Item::Iso, 100) && screen.PanelPick(B, Item::ShutterSpeed, 500) && screen.RequestChosen(true) && s.WaitIdle() && s.Confirm() && Shows(screen.View(), L"選んだ値を書きました: 一致（2 項目"), "deadline: two steps");
}
void ScreenChosenDamagedRecord() {
    using Item = D810ChoiceItem;
    const auto A = D810Camera::A, B = D810Camera::B;
    const std::wstring advice = kD810RecordDamagedAdvice;
    ScreenBench s(kChosenTable, true); // on disk: the record is a file that can be damaged
    auto& screen = *s.screen;
    s.bench.b.apply_as[0x500f] = 400; // CAM-B's write does not take: a block stands
    check(OpenPanel(s) && screen.PanelPick(B, Item::Iso, 200) && screen.PanelPick(A, Item::Iso, 200) && screen.RequestChosen(true) && s.WaitIdle() && s.Confirm(), "damaged: a chosen run that did not take");
    check(screen.Status().block == D810CaptureBlock::Mismatch && screen.Status().chosen_history && !screen.Status().history_damaged, "damaged: the record is whole, the history is a chosen plan's");
    check(screen.CaptureRefusalText().find(L"書いた値が、読み戻しで確かめられていません") != std::wstring::npos && screen.CaptureRefusalText().find(advice) == std::wstring::npos, "damaged: whole record, the words of the values written");
    // A line that cannot be read joins the record.
    check(s.bench.file.Append("this is not a line of the record"), "damaged: the record is damaged");
    screen.Refresh();
    check(!screen.Status().chosen_history && screen.Status().history_damaged && !screen.Status().capture_allowed, "damaged: not a chosen history any more, damaged");
    const auto held = screen.CaptureRefusalText();
    check(held.find(advice) != std::wstring::npos && held.find(L"書いた値が、読み戻しで確かめられていません") == std::wstring::npos, "damaged: the block text is the comparison's and gives the advice");
    // The reread compares the cameras (CAM-A reads 64, CAM-B 400 on the block items), item by item, and gives the advice; it does not tell the written values.
    check(screen.RequestPanelReread(true) && s.WaitIdle(), "damaged: reread");
    check(Shows(screen.View(), L"読み直しました: CAM-A と CAM-B で違う項目があります。") && Shows(screen.View(), L"  ISO感度") && Shows(screen.View(), advice) &&
            !Shows(screen.View(), L"書いた値になっていません") && !Shows(screen.View(), L"書いた値と違う項目"),
        "damaged: the reread compares the two cameras and gives the advice");
    check(!screen.Status().capture_allowed, "damaged: not released");
    // Both cameras the same on every block item: released, whatever was written.
    s.bench.b.props[0x500f].current = 64;
    s.bench.b.props[0x500d].current = s.bench.a.props[0x500d].current;
    s.bench.b.props[0x5007].current = s.bench.a.props[0x5007].current;
    check(screen.RequestPanelReread(true) && s.WaitIdle() && Shows(screen.View(), L"読み直しました: 止めの対象の項目は CAM-A と CAM-B で同じです。") && screen.Status().capture_allowed, "damaged: released when the cameras are the same");
    // A record that cannot be read at all releases nothing and says so.
    check(D810CaptureBlockText(D810CaptureBlock::Mismatch, false, true).find(advice) != std::wstring::npos && D810CaptureBlockText(D810CaptureBlock::Unconfirmed, false, true).find(advice) != std::wstring::npos &&
            D810CaptureBlockText(D810CaptureBlock::Unreadable, false, true).find(advice) == std::wstring::npos && D810CaptureBlockText(D810CaptureBlock::None, false, true).empty() &&
            D810CaptureBlockText(D810CaptureBlock::Mismatch, false, false).find(advice) == std::wstring::npos,
        "damaged: the advice is for a block that the cameras' comparison releases");
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
                view->lines[8].find(L"種類だけを写します") != std::wstring::npos && view->lines[8].find(L"微調整・自動の型・蛍光灯の型は写りません") != std::wstring::npos, "revision screen: white balance, the kind only");
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
        check(s.screen->OfferText(true).find(L"CAM-B に書くのは 露出モード・ISO感度自動制御・ISO感度・シャッター速度・絞り だけで") != std::wstring::npos,
            "the offer text names the five writable items of this table");
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
    run("GateCamA", GateCamA);
    run("GateArmConditions", GateArmConditions);
    run("RevisionGate", RevisionGate);
    run("GateWriteOrder", GateWriteOrder);
    run("GateStateSweep", GateStateSweep);
    run("GateFailureShapes", GateFailureShapes);
    run("OtherGatesStillRefuseWrites", OtherGatesStillRefuseWrites);
    run("HappyCopy", HappyCopy);
    run("ProductRun", ProductRun);
    run("ProductRevisionItems", ProductRevisionItems);
    run("ProductAperture", ProductAperture);
    run("ProductShutter", ProductShutter);
    run("PlanReadChecksTheBody", PlanReadChecksTheBody);
    run("BodyAndConfirmation", BodyAndConfirmation);
    run("RecordMustBeWrittenFirst", RecordMustBeWrittenFirst);
    run("PlanChangedAfterConfirm", PlanChangedAfterConfirm);
    run("FailuresStopAndBlock", FailuresStopAndBlock);
    run("RereadReleases", RereadReleases);
    run("JournalStates", JournalStates);
    run("ChosenReadToken", ChosenReadToken);
    run("ChosenRuns", ChosenRuns);
    run("ChosenStopsOnA", ChosenStopsOnA);
    run("ChosenStopsOnB", ChosenStopsOnB);
    run("ChosenPlanSubsets", ChosenPlanSubsets);
    run("ChosenTenItems", ChosenTenItems);
    run("WritesAreHeldWhileCaptureIsBlocked", WritesAreHeldWhileCaptureIsBlocked);
    run("RecordRewrittenWithinAnOperation", RecordRewrittenWithinAnOperation);
    run("SameAsADoesNotAskCamAToBeInM", SameAsADoesNotAskCamAToBeInM);
    run("ChosenRelease", ChosenRelease);
    run("JournalPlanned", JournalPlanned);
    run("RevisionRuns", RevisionRuns);
    run("StopsBeforeSending", StopsBeforeSending);
    run("WatchdogCancel", WatchdogCancel);
    run("ZeroByteReadsInTheWriteSession", ZeroByteReadsInTheWriteSession);
    run("ReadOnlySessionsAfterCancel", ReadOnlySessionsAfterCancel);
    run("ExecuteChecksThePlanNumber", ExecuteChecksThePlanNumber);
    run("BudgetSweep", BudgetSweep);
    run("ScreenCopyFlow", ScreenCopyFlow);
    run("ScreenRefusals", ScreenRefusals);
    run("ScreenBodyNotVerified", ScreenBodyNotVerified);
    run("ScreenProductRows", ScreenProductRows);
    run("ScreenPanelProductTable", ScreenPanelProductTable);
    run("ScreenPanelPicks", ScreenPanelPicks);
    run("ScreenPanelDiscards", ScreenPanelDiscards);
    run("ScreenPanelLocksAndFailures", ScreenPanelLocksAndFailures);
    run("ScreenChosenFlow", ScreenChosenFlow);
    run("ScreenChosenRefusals", ScreenChosenRefusals);
    run("ScreenChosenWorkerFailure", ScreenChosenWorkerFailure);
    run("RereadRuleIsTold", RereadRuleIsTold);
    run("ScreenRereadUsesTheRuleOfTheCheck", ScreenRereadUsesTheRuleOfTheCheck);
    run("ScreenPanelFollowAndCount", ScreenPanelFollowAndCount);
    run("ScreenPanelCamerasFlag", ScreenPanelCamerasFlag);
    run("ScreenChosenDeadline", ScreenChosenDeadline);
    run("ScreenChosenDamagedRecord", ScreenChosenDamagedRecord);
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
