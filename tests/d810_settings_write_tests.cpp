// Contracts of the camera settings write (#416 stage B, ADR-0038; ADR-0041 paths
// stay off without their approval record). Synthetic bodies only: in-process WPD
// value objects and files in a temporary folder. No device, no camera.
#include "a0/phase0/experimental/wpd_settings_write.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
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
    bool writing_ = false;
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

// --- Tables ----------------------------------------------------------------------------
// The product table with the codes confirmed, CAM-B's five rows enabled and a
// verified firmware: the most ADR-0038 allows. It is what a reviewed commit
// after the real read would make the product table look like.
constexpr D810WriteTable Adr38Table() {
    auto table = kD810ProductWriteTable;
    for (auto& row : table.rows) {
        if (row.item == D810WriteItem::IsoAuto) row.code = 0xd054;
        if (row.item == D810WriteItem::ShutterSpeed) row.code = 0x500d;
        row.enabled = row.camera == D810Camera::B && D810WriteItemOf(row.item).accepted_scope;
    }
    table.verified_firmware = "1.00";
    return table;
}
constexpr D810WriteTable kAdr38Table = Adr38Table();
static_assert(D810ProductTableProblem(kAdr38Table) == nullptr, "the ADR-0038 test table is a valid product table");
static_assert(D810EnabledRowCount(kAdr38Table) == 5, "five rows");
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
    D810OpenedCamera Open(D810Camera camera) override {
        const auto i = static_cast<int>(camera);
        if (open != -1) overlap = true;
        if (open_fail[i]) return {};
        open = i;
        ++opens[i];
        return {[this, i](IPortableDeviceValues* request) { return (*bodies[i])(request); }, model[i], firmware[i]};
    }
    bool Close(D810Camera camera) override {
        const auto i = static_cast<int>(camera);
        ++closes[i];
        open = -1;
        return close_ok[i];
    }
};

// A folder with a file journal, two bodies and a controller over a table.
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
    explicit Bench(const D810WriteTable& t = kAdr38Table, int journal_ok = -1, bool on_disk = false): table(t) {
        journal = on_disk ? static_cast<D810SettingsJournal*>(&file) : &memory;
        if (journal_ok >= 0) failing = std::make_unique<FailingJournal>(*journal, journal_ok);
        controller = std::make_unique<D810SettingsWriteController>(table, failing ? static_cast<D810SettingsJournal&>(*failing) : *journal, owner);
        access.bodies[0] = &a;
        access.bodies[1] = &b;
        options.may_continue = [this] { return go; };
        options.may_cleanup = [] { return true; };
        options.remaining = [this] { return left; };
        options.log = [this](const std::string& line) { log.push_back(line); };
        options.now = [] { return std::string("2000-01-01T00:00:00Z"); };
    }
    // A fresh controller over the same folder: what a restart sees.
    D810SettingsWriteController Restarted() { return D810SettingsWriteController(table, *journal, owner); }
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
    check(std::string(D810WriteValueRefusal(shutter_spec, shutter, 0xffffffffLL)) == "bulb_or_time", "bulb refused although advertised");
    check(std::string(D810WriteValueRefusal(shutter_spec, shutter, 0xfffffffdLL)) == "bulb_or_time", "time refused although advertised");
    check(std::string(D810WriteValueRefusal(shutter_spec, shutter, 20000)) == "exposure_too_long", "long exposure refused although advertised");
    check(D810WriteValueRefusal(shutter_spec, shutter, 1667) == nullptr, "advertised shutter");
    shutter.get_set = 0;
    check(std::string(D810WriteValueRefusal(shutter_spec, shutter, 1667)) == "not_writable", "getset 0");
    D810PropDesc mode;
    mode.data_type = 4; mode.get_set = 1; mode.form_flag = 2; mode.values = {1, 2, 3, 4};
    check(std::string(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::ExposureMode), mode, 2)) == "value_not_fixed", "exposure mode is M only");
    D810PropDesc iso_auto;
    iso_auto.data_type = 2; iso_auto.get_set = 1; iso_auto.form_flag = 2; iso_auto.values = {0, 1};
    check(std::string(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::IsoAuto), iso_auto, 1)) == "value_not_fixed", "ISO auto is OFF only");
    iso_auto.data_type = 4;
    check(std::string(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::IsoAuto), iso_auto, 0)) == "type_mismatch", "type");
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

// FormFlag None, Range step 0 and the excluded white balance values are never written.
void DescriptionRules() {
    D810PropDesc none;
    none.data_type = 4; none.get_set = 1; none.form_flag = 0; none.current_value = 100;
    check(none.Writable() && !D810DescWritable(none), "the stage A Writable() accepts a form of None; the write rule does not");
    check(std::string(D810WriteValueRefusal(D810WriteItemOf(D810WriteItem::Iso), none, 100)) == "not_writable", "FormFlag None refused");
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
    wb.data_type = 4; wb.get_set = 1; wb.form_flag = 2; wb.values = {1, 2, 4, 0x8012, 0x8013};
    const auto& white_balance = D810WriteItemOf(D810WriteItem::WhiteBalance);
    for (const auto value : {1LL, 0x8012LL, 0x8013LL})
        check(std::string(D810WriteValueRefusal(white_balance, wb, value)) == "value_content_dependent", "content-dependent white balance");
    check(D810WriteValueRefusal(white_balance, wb, 4) == nullptr, "daylight");
}

// --- The fixed table ----------------------------------------------------------------------------
void ProductTable() {
    const auto& table = kD810ProductWriteTable;
    check(D810ProductTableProblem(table) == nullptr, "product table valid");
    // The enabled set is exactly this (empty until the codes are confirmed on a
    // body). Changing the product table makes this check fail on purpose.
    std::set<std::pair<int, int>> enabled;
    for (const auto& row : table.rows)
        if (row.enabled) enabled.insert({static_cast<int>(row.camera), static_cast<int>(row.item)});
    check(enabled.empty(), "no row enabled in the product table");
    check(table.Code(D810WriteItem::IsoAuto) == 0 && table.Code(D810WriteItem::ShutterSpeed) == 0, "ISO auto and shutter codes not confirmed");
    check(table.copy_a_to_b && !table.chosen_values && table.verified_firmware == nullptr, "copy only, no firmware verified");
    check(!D810BodyVerified(table, "D810", "1.00") && !D810BodyVerified(table, "D810", ""), "no body is verified");
    // What the product table may ever hold: CAM-B and the five items, no chosen values.
    std::size_t allowed = 0;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        auto probe = Adr38Table();
        probe.rows[i].enabled = true;
        const bool ok = D810ProductTableProblem(probe) == nullptr;
        allowed += ok;
        check(ok == (probe.rows[i].camera == D810Camera::B && D810WriteItemOf(probe.rows[i].item).accepted_scope), "allowed rows");
    }
    check(allowed == 5, "CAM-B x five");
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
    bad = kD810ProductWriteTable;
    for (auto& row : bad.rows) if (row.item == D810WriteItem::Iso && row.camera == D810Camera::B) row.enabled = true;
    check(std::string(D810WriteTableProblem(bad)) == "iso_auto_code_unconfirmed", "ISO needs the ISO auto code");
    bad = Adr38Table();
    bad.rows[3] = bad.rows[2];
    check(D810WriteTableProblem(bad) != nullptr, "duplicate or missing row");
}
// Nothing at run time can widen the table: files that look like approvals, in
// the record folder or next to it, change nothing.
void FilesEnableNothing() {
    Bench bench(kD810ProductWriteTable);
    for (const auto* name : {"capability.txt", "approval.txt", "table.txt", "enable.txt", "settings-write.ini"}) {
        WriteFile(bench.folder / name, "a0-d810-settings-capability 1\nitem=iso alias=CAM-B code=0x500f type=0x0004 getset=1\n"
            "feature=cam_a_write adr=ADR-0041 issue=416\nfeature=chosen_values adr=ADR-0041 issue=416\nenabled=all\nend count=1\n");
        WriteFile(bench.folder.parent_path() / name, "enabled=all\n");
    }
    SetEnvironmentVariableW(L"A0_SETTINGS_WRITE_ENABLE", L"1");
    const auto copy = bench.Copy();
    check(!copy.Ok() && copy.reasons.size() == 2 && copy.reasons[0] == "code_unconfirmed:iso_auto", "product copy refused");
    for (const auto target : {D810WriteTarget::A, D810WriteTarget::B, D810WriteTarget::Both})
        check(!bench.Chosen({{D810WriteItem::Iso, target, 200}}).Ok(), "product chosen refused");
    check(bench.controller->Status().enabled_rows == 0, "status: no row");
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
    { Bench bench(kD810ProductWriteTable); refused(bench, "code_unconfirmed:iso_auto"); }
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
    const std::vector<std::uint32_t> targeted{0x1016, 0x1014, 0x1015, 0x90c8, 0x9207, 0x90c2, 0x90c0, 0x90c7, 0x1017, 0x9201, 0x1001, 0x1008, 0x100b, 0x9204};
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
    for (const auto& entries : std::vector<std::vector<D810WriteGateEntry>>{{{0xd200, 1}}, {{0x500e, 3}}, {{0xd054, 1}}, {{0x500f, 64}, {0x500f, 100}}, {{0x500f, 0x10000}}, {{0xd16a, 0}}}) {
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
}
void JournalStates() {
    const auto op = std::string("a0sw1 op=00000000000000aa ");
    check(ParseD810Journal("").block == D810CaptureBlock::None, "empty");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n").block == D810CaptureBlock::Unconfirmed, "plan without end");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=end t=x verdict=not_sent sent=0 reason=x\n").block == D810CaptureBlock::None, "not sent");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=end t=x verdict=not_sent sent=1 reason=x\n").block == D810CaptureBlock::Unconfirmed, "not_sent with sends is malformed");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=end t=x verdict=unknown sent=1 reason=x\n").block == D810CaptureBlock::Unknown, "unknown");
    check(ParseD810Journal("a0sw1 op=00000000000000aa ev=pl").block == D810CaptureBlock::Unconfirmed, "torn line");
    check(ParseD810Journal("a0sw1 op=00000000000000aa ev=pl\na0sw1 ev=reread t=x verdict=match\n").block == D810CaptureBlock::None, "torn line released");
    check(ParseD810Journal(op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=end t=x verdict=mismatch sent=1 reason=x\na0sw1 ev=reread t=x verdict=mismatch\n").block == D810CaptureBlock::Mismatch, "mismatch stays");
    {
        // Crash after the plan line: the next start is blocked as unconfirmed.
        Bench bench(kAdr38Table, -1, true);
        WriteFile(bench.file.Path(), op + "ev=plan t=x source=copy steps=1 digest=0\n" + op + "ev=step i=0 alias=CAM-B item=iso code=0x500f before=0x0064 value=0x0040\n");
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
    run("DescriptionRules", DescriptionRules);
    run("ProductTable", ProductTable);
    run("FilesEnableNothing", FilesEnableNothing);
    run("CopyPlans", CopyPlans);
    run("ChosenPlans", ChosenPlans);
    run("GateSweep", GateSweep);
    run("GateArmConditions", GateArmConditions);
    run("GateWriteOrder", GateWriteOrder);
    run("OtherGatesStillRefuseWrites", OtherGatesStillRefuseWrites);
    run("HappyCopy", HappyCopy);
    run("BodyAndConfirmation", BodyAndConfirmation);
    run("RecordMustBeWrittenFirst", RecordMustBeWrittenFirst);
    run("PlanChangedAfterConfirm", PlanChangedAfterConfirm);
    run("FailuresStopAndBlock", FailuresStopAndBlock);
    run("RereadReleases", RereadReleases);
    run("JournalStates", JournalStates);
    run("ChosenRuns", ChosenRuns);
    run("StopsBeforeSending", StopsBeforeSending);
    run("OwnershipIsCompareAndSwap", OwnershipIsCompareAndSwap);
    std::cout << checks << " checks, " << failures << " failures, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms\n";
    std::error_code ignored;
    fs::remove_all(g_root, ignored);
    CoUninitialize();
    return failures ? 1 : 0;
}
