// Contracts of the per-camera value choice (#416): the pickable items, the list of pickable values built from a
// property description, display names, panel cells and the draft of picks. Pure data: no device, no camera, no
// Windows API, and nothing here reaches a plan, a gate or an execution path.
#include "a0/phase0/experimental/d810_settings_choice.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace a0::phase0::experimental;
namespace {
int failures = 0, checks = 0;
void check(bool value, const char* what = "") {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAILED check " << checks << " " << what << "\n";
    }
}
std::string Refusal(const char* token) { return token ? std::string(token) : std::string("(none)"); }

using Values = std::vector<std::int64_t>;
std::uint16_t TypeOf(D810ChoiceItem item) { return static_cast<std::uint16_t>(D810WriteItemOf(D810ChoiceWriteItem(item)).type); }
D810PropDesc EnumDesc(D810ChoiceItem item, std::int64_t current, const Values& values, std::uint8_t get_set = 1) {
    D810PropDesc desc;
    desc.code = D810SettingOf(D810ChoiceSettingId(item)).code;
    desc.data_type = TypeOf(item);
    desc.get_set = get_set;
    desc.form_flag = 2;
    desc.default_value = desc.current_value = current;
    desc.values = values;
    return desc;
}
D810PropDesc RangeDesc(D810ChoiceItem item, std::int64_t current, std::int64_t min, std::int64_t max, std::int64_t step, std::uint8_t get_set = 1) {
    D810PropDesc desc;
    desc.code = D810SettingOf(D810ChoiceSettingId(item)).code;
    desc.data_type = TypeOf(item);
    desc.get_set = get_set;
    desc.form_flag = 1;
    desc.default_value = desc.current_value = current;
    desc.range_min = min;
    desc.range_max = max;
    desc.range_step = step;
    return desc;
}
Values ValuesOf(const D810ChoiceOptions& options) {
    Values out;
    for (const auto& option : options.options) out.push_back(option.value);
    return out;
}
std::string DroppedReason(const D810ChoiceOptions& options, std::int64_t value) {
    for (const auto& entry : options.dropped)
        if (entry.value == value) return Refusal(entry.reason);
    return "(not dropped)";
}
std::string LabelOf(const D810ChoiceOptions& options, std::int64_t value) {
    for (const auto& option : options.options)
        if (option.value == value) return option.label;
    return "(not offered)";
}

// --- Items -------------------------------------------------------------------------------------------
void Items() {
    check(kD810ChoiceItems.size() == 5, "five pickable items");
    // Item table order is the write order: ISO, shutter speed, aperture, white balance, metering.
    const D810WriteItem order[] = {D810WriteItem::Iso, D810WriteItem::ShutterSpeed, D810WriteItem::Aperture, D810WriteItem::WhiteBalance, D810WriteItem::Metering};
    for (std::size_t i = 0; i < kD810ChoiceItems.size(); ++i) {
        check(D810ChoiceWriteItem(kD810ChoiceItems[i]) == order[i], "pickable items follow the item table");
        check(D810ChoiceItemOf(order[i]) == kD810ChoiceItems[i], "write item maps back");
        check(D810ChoiceItemRefusal(order[i]) == nullptr, "pickable item has no refusal");
        check(static_cast<std::size_t>(kD810ChoiceItems[i]) == i, "enum value is the position");
    }
    for (const auto item : {D810WriteItem::ExposureMode, D810WriteItem::IsoAuto, D810WriteItem::ExposureCompensation}) {
        check(!D810ChoiceItemOf(item).has_value(), "exposure mode, ISO auto and compensation are not pickable");
        check(Refusal(D810ChoiceItemRefusal(item)) == "item_not_choosable", "item_not_choosable token");
        D810PropDesc desc = EnumDesc(D810ChoiceItem::Iso, 100, {100});
        check(Refusal(D810ChoosableRefusal(item, desc, 1)) == "item_not_choosable", "overload for a write item refuses a non pickable item");
    }
    // Every pickable item reads from the code the item table has.
    for (const auto item : kD810ChoiceItems) {
        const auto& write = D810WriteItemOf(D810ChoiceWriteItem(item));
        check(D810WriteItemHasCode(write, D810SettingOf(D810ChoiceSettingId(item)).code), "read code is a code of the write item");
    }
}

// --- Advertised values -------------------------------------------------------------------------------------
void Advertised() {
    {
        const auto out = D810AdvertisedValuesOf(EnumDesc(D810ChoiceItem::Iso, 200, {400, 100, 200, 100}));
        check(out.values == Values({400, 100, 200, 100}) && !out.too_many, "an enumeration is listed as it is, in the body's order");
    }
    {
        const auto out = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 100, 100, 400, 100));
        check(out.values == Values({100, 200, 300, 400}), "a range lists from its minimum in steps");
    }
    {
        // A step that does not land on the maximum stops at the last value inside the range.
        const auto out = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 100, 100, 450, 100));
        check(out.values == Values({100, 200, 300, 400}), "a range stops at the last step inside the maximum");
    }
    {
        const auto out = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 5, 5, 5, 1));
        check(out.values == Values({5}), "a one value range lists one value");
    }
    {
        const auto at_limit = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 0, 0, static_cast<std::int64_t>(kD810EnumMaxValues) - 1, 1));
        check(at_limit.values.size() == kD810EnumMaxValues && !at_limit.too_many, "a range of exactly the limit is listed");
        const auto over = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 0, 0, static_cast<std::int64_t>(kD810EnumMaxValues), 1));
        check(over.values.empty() && over.too_many, "a range over the limit is not listed");
        const auto wide = D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::ShutterSpeed, 0, 0, 0xfffffffe, 1));
        check(wide.values.empty() && wide.too_many, "a very wide range is not listed and does not overflow");
    }
    check(D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 100, 100, 400, 0)).values.empty(), "a range without a step lists nothing");
    check(D810AdvertisedValuesOf(RangeDesc(D810ChoiceItem::Iso, 100, 400, 100, 100)).values.empty(), "an inverted range lists nothing");
    {
        D810PropDesc none = EnumDesc(D810ChoiceItem::Iso, 100, {100});
        none.form_flag = 0;
        none.values.clear();
        check(D810AdvertisedValuesOf(none).values.empty(), "no form lists nothing");
    }
}

// --- Pickable values -----------------------------------------------------------------------------------------
void IsoOptions() {
    // Inside the band: 64 to 12800. Outside: dropped as out of band, and 0 has no meaning.
    const auto from_enum = BuildD810ChoiceOptions(D810ChoiceItem::Iso, EnumDesc(D810ChoiceItem::Iso, 100, {32, 50, 64, 100, 200, 12800, 25600, 0, 100}));
    check(ValuesOf(from_enum) == Values({64, 100, 200, 12800}), "ISO from an enumeration: the band applies, a repeat is listed once");
    check(DroppedReason(from_enum, 32) == "value_out_of_band" && DroppedReason(from_enum, 50) == "value_out_of_band" &&
          DroppedReason(from_enum, 25600) == "value_out_of_band", "ISO outside the band: value_out_of_band");
    check(DroppedReason(from_enum, 0) == "value_not_named", "ISO 0 has no meaning: value_not_named");
    check(from_enum.dropped.size() == 4, "each dropped value once");
    check(LabelOf(from_enum, 100) == "100" && LabelOf(from_enum, 12800) == "12800", "ISO labels are the number");

    const auto from_range = BuildD810ChoiceOptions(D810ChoiceItem::Iso, RangeDesc(D810ChoiceItem::Iso, 100, 100, 1000, 100));
    check(ValuesOf(from_range) == Values({100, 200, 300, 400, 500, 600, 700, 800, 900, 1000}), "ISO from a range");
    check(from_range.dropped.empty() && !from_range.too_many, "nothing dropped inside the band");

    const auto wide = BuildD810ChoiceOptions(D810ChoiceItem::Iso, RangeDesc(D810ChoiceItem::Iso, 100, 0, 65535, 1));
    check(wide.options.empty() && wide.too_many, "a range over 1024 candidates offers nothing");

    const auto all_outside = BuildD810ChoiceOptions(D810ChoiceItem::Iso, EnumDesc(D810ChoiceItem::Iso, 25600, {25600, 51200}));
    check(all_outside.options.empty() && all_outside.dropped.size() == 2, "an enumeration wholly outside the band offers nothing");
    // The band edges.
    check(D810ChoiceBand(D810ChoiceItem::Iso, 64) && !D810ChoiceBand(D810ChoiceItem::Iso, 63), "ISO lower edge");
    check(D810ChoiceBand(D810ChoiceItem::Iso, 12800) && !D810ChoiceBand(D810ChoiceItem::Iso, 12801), "ISO upper edge");
}

void ShutterOptions() {
    // 167 reads as 1/60, 125 as 1/80, 1250 as 1/8, 10000 is 1 s.
    const auto options = BuildD810ChoiceOptions(D810ChoiceItem::ShutterSpeed,
        EnumDesc(D810ChoiceItem::ShutterSpeed, 167, {0xffffffffLL, 0xfffffffdLL, 0, 20000, 10001, 10000, 1250, 167, 125, 1}));
    check(ValuesOf(options) == Values({10000, 1250, 167, 125, 1}), "shutter speed: Bulb, Time, zero and more than 1 s are dropped");
    check(DroppedReason(options, 0xffffffffLL) == "bulb_or_time" && DroppedReason(options, 0xfffffffdLL) == "bulb_or_time", "Bulb and Time: bulb_or_time");
    check(DroppedReason(options, 0) == "exposure_zero", "zero: exposure_zero");
    check(DroppedReason(options, 20000) == "exposure_too_long" && DroppedReason(options, 10001) == "exposure_too_long", "over 1 s: exposure_too_long");
    check(LabelOf(options, 167) == "1/60" && LabelOf(options, 125) == "1/80" && LabelOf(options, 1250) == "1/8", "fractions without a unit letter");
    check(LabelOf(options, 10000) == "1 秒", "1 s reads as 1 秒");
    check(LabelOf(options, 1) == "1/10000", "the smallest unit reads as a fraction");
    check(D810ChoiceName(D810ChoiceItem::ShutterSpeed, 3000) == "0.3 秒", "a value that is not a clean fraction reads as seconds");
    check(D810ChoiceName(D810ChoiceItem::ShutterSpeed, 3) == "3/10000", "below 10 units the units are shown");
    check(D810ChoiceName(D810ChoiceItem::ShutterSpeed, 0xffffffffLL) == "Bulb", "Bulb keeps its name");

    // Labels that would read the same get the raw value, all of them.
    Values same;
    for (std::int64_t v = 1000; v < 1000 + static_cast<std::int64_t>(kD810EnumMaxValues); ++v) same.push_back(v);
    const auto many = BuildD810ChoiceOptions(D810ChoiceItem::ShutterSpeed, EnumDesc(D810ChoiceItem::ShutterSpeed, 1000, same));
    check(many.options.size() == kD810EnumMaxValues, "all values in 1000..2023 are offered");
    std::set<std::string> labels;
    for (const auto& option : many.options) labels.insert(option.label);
    check(labels.size() == many.options.size(), "labels never repeat within an item");
    check(LabelOf(many, 1000) == "1/10 (0x000003e8)", "a repeated label carries the raw value");
    // Marked exactly when the bare name is shared with another value.
    std::size_t shared = 0, marked = 0;
    bool consistent = true;
    for (const auto& option : many.options) {
        const auto bare = D810ChoiceName(D810ChoiceItem::ShutterSpeed, option.value);
        const auto sharers = std::count_if(many.options.begin(), many.options.end(), [&](const auto& other) {
            return D810ChoiceName(D810ChoiceItem::ShutterSpeed, other.value) == bare;
        });
        const bool has_mark = option.label.find("(0x") != std::string::npos;
        shared += sharers > 1;
        marked += has_mark;
        consistent = consistent && (has_mark == (sharers > 1));
    }
    check(shared >= 2 && shared == marked && consistent, "a label is marked exactly when its name is shared");
    const auto pair = BuildD810ChoiceOptions(D810ChoiceItem::ShutterSpeed, EnumDesc(D810ChoiceItem::ShutterSpeed, 1000, {1000, 1250}));
    check(LabelOf(pair, 1000) == "1/10" && LabelOf(pair, 1250) == "1/8", "distinct labels carry no raw value");
}

void ApertureOptions() {
    const auto options = BuildD810ChoiceOptions(D810ChoiceItem::Aperture, EnumDesc(D810ChoiceItem::Aperture, 800, {0, 280, 400, 560, 800, 1100}));
    check(ValuesOf(options) == Values({280, 400, 560, 800, 1100}), "aperture: zero is dropped");
    check(DroppedReason(options, 0) == "value_not_named", "aperture 0 has no meaning");
    check(LabelOf(options, 280) == "f/2.8" && LabelOf(options, 400) == "f/4" && LabelOf(options, 560) == "f/5.6" &&
          LabelOf(options, 800) == "f/8" && LabelOf(options, 1100) == "f/11", "aperture labels");
}

void WhiteBalanceOptions() {
    const Values all = {1, 2, 3, 4, 5, 6, 7, 0x8010, 0x8011, 0x8012, 0x8013, 0x8014};
    const auto options = BuildD810ChoiceOptions(D810ChoiceItem::WhiteBalance, EnumDesc(D810ChoiceItem::WhiteBalance, 4, all));
    check(ValuesOf(options) == Values({2, 4, 5, 6, 7, 0x8010, 0x8011}), "white balance: the seven named kinds");
    for (const auto value : {1, 3, 0x8012, 0x8013}) check(DroppedReason(options, value) == "value_content_dependent", "Manual, One-push, Color Temperature, Preset: value_content_dependent");
    check(DroppedReason(options, 0x8014) == "value_not_listed", "a kind the item does not name: value_not_listed");
    // Every value the item table allows is named, and the other way round.
    const auto& spec = D810WriteItemOf(D810WriteItem::WhiteBalance);
    for (std::size_t i = 0; i < spec.allowed_count; ++i) check(D810ChoiceNamed(D810ChoiceItem::WhiteBalance, spec.allowed[i]), "allowed white balance values are named");
    for (const auto& entry : kD810ChoiceWhiteBalanceNames) check(D810WriteValueListed(spec, entry.value) && !D810WriteValueExcluded(spec, entry.value), "named values are allowed");
    check(LabelOf(options, 2) == "オート (0x0002)" && LabelOf(options, 4) == "晴天 (0x0004)" && LabelOf(options, 5) == "蛍光灯 (0x0005)" &&
          LabelOf(options, 6) == "白熱電球 (0x0006)" && LabelOf(options, 7) == "フラッシュ (0x0007)" &&
          LabelOf(options, 0x8010) == "曇天 (0x8010)" && LabelOf(options, 0x8011) == "晴天日陰 (0x8011)", "white balance labels carry the raw value while the names are unconfirmed");
    check(!kD810ChoiceNamesConfirmed, "the names are not confirmed");
    check(D810ChoiceName(D810ChoiceItem::WhiteBalance, 4) == "晴天", "the bare name has no raw value");
    check(D810ChoiceName(D810ChoiceItem::WhiteBalance, 0x8014) == "raw 0x8014", "a value without a name reads as its raw value");
}

void MeteringOptions() {
    const auto options = BuildD810ChoiceOptions(D810ChoiceItem::Metering, EnumDesc(D810ChoiceItem::Metering, 3, {1, 2, 3, 4, 0x8010}));
    check(ValuesOf(options) == Values({2, 3, 4}), "metering: 2, 3 and 4");
    check(DroppedReason(options, 1) == "value_not_named", "metering 1 has no name: value_not_named");
    check(DroppedReason(options, 0x8010) == "value_not_listed", "metering 0x8010 is not listed");
    check(LabelOf(options, 2) == "中央重点 (0x0002)" && LabelOf(options, 3) == "マルチパターン (0x0003)" && LabelOf(options, 4) == "スポット (0x0004)", "metering labels");
    // The copy list of the item table still holds 1; only the pickable list drops it.
    const auto& spec = D810WriteItemOf(D810WriteItem::Metering);
    check(D810WriteValueListed(spec, 1), "the copy list is unchanged");
}

// A value the body did not advertise, or a description that cannot be written, is refused by the same function.
void RefusalReasons() {
    const auto desc = EnumDesc(D810ChoiceItem::Iso, 100, {100, 200});
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, desc, 100)) == "(none)", "an advertised, named, in band value is pickable");
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, desc, 400)) == "value_not_advertised", "a value the body did not advertise");
    auto wrong_type = desc;
    wrong_type.data_type = 6;
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, wrong_type, 100)) == "type_mismatch", "a description of another type");
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, EnumDesc(D810ChoiceItem::Iso, 100, {100}, 0), 100)) == "not_writable", "a read only description");
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, EnumDesc(D810ChoiceItem::Iso, 100, {100, 70000}), 70000)) == "value_out_of_type", "a value that does not fit the type");
    check(Refusal(D810ChoosableRefusal(D810WriteItem::Iso, desc, 200)) == "(none)", "the overload for a write item agrees");
    auto none = desc;
    none.form_flag = 0;
    none.values.clear();
    check(Refusal(D810ChoosableRefusal(D810ChoiceItem::Iso, none, 100)) == "not_writable", "a description without a form offers nothing");
}

// The pickable set is the set the gate's refusal accepts, narrowed only by the names and the band.
void SieveMatchesTheGate() {
    for (const auto item : kD810ChoiceItems) {
        const auto& spec = D810WriteItemOf(D810ChoiceWriteItem(item));
        D810PropDesc desc = item == D810ChoiceItem::ShutterSpeed ? RangeDesc(item, 1, 0, 0x10000, 1) : RangeDesc(item, 1, 0, 0xffff, 1);
        if (item == D810ChoiceItem::ShutterSpeed) desc.range_max = 0xfffffffeLL;
        std::vector<std::int64_t> sweep;
        for (std::int64_t v = -2; v <= 20005; ++v) sweep.push_back(v);
        for (std::int64_t v : {0x8010LL, 0x8011LL, 0x8012LL, 0x8013LL, 0xfffdLL, 0xffffLL, 0xfffffffdLL, 0xffffffffLL}) sweep.push_back(v);
        std::size_t pickable = 0, narrowed = 0;
        for (const auto v : sweep) {
            const auto gate = D810WriteValueRefusal(spec, desc, v);
            const auto sieve = D810ChoosableRefusal(item, desc, v);
            if (!sieve) {
                ++pickable;
                check(gate == nullptr, "a pickable value passes the gate's refusal");
            } else if (!gate) {
                ++narrowed;
                const std::string reason = sieve;
                check(reason == "value_not_named" || reason == "value_out_of_band", "only the names and the band narrow the gate's set");
            } else {
                check(std::string(gate) == sieve, "otherwise the reason is the gate's own");
            }
        }
        check(pickable > 0, "some value of every item is pickable in the sweep");
        if (item == D810ChoiceItem::Metering) check(narrowed == 1, "metering: only value 1 is narrowed");
        if (item == D810ChoiceItem::Iso) check(narrowed > 0, "ISO: the band narrows");
    }
}

// --- Display ------------------------------------------------------------------------------------------------
void DisplayNames() {
    check(D810ChoiceName(D810ChoiceItem::Iso, 100) == "100", "ISO name");
    check(D810ChoiceLabel(D810ChoiceItem::Iso, 100) == "100", "ISO label has no raw value");
    check(D810ChoiceName(D810ChoiceItem::Iso, 0) == "raw 0x0000", "ISO 0 reads as raw");
    check(D810ChoiceName(D810ChoiceItem::Aperture, 560) == "f/5.6", "aperture name");
    check(D810ChoiceName(D810ChoiceItem::Aperture, 0) == "raw 0x0000", "aperture 0 reads as raw");
    check(D810ChoiceName(D810ChoiceItem::ShutterSpeed, 167) == "1/60", "shutter name");
    check(D810ChoiceLabel(D810ChoiceItem::WhiteBalance, 0x8013) == "raw 0x8013", "an unnamed white balance has no raw suffix on top of the raw text");
    check(D810ChoiceName(D810ChoiceItem::Metering, 3) == "マルチパターン", "metering name");
}

// --- Cells ---------------------------------------------------------------------------------------------------
D810SettingReading Reading(D810SettingId id, const D810PropDesc& desc) {
    D810SettingReading reading;
    reading.id = id;
    reading.available = true;
    reading.desc = desc;
    return reading;
}
// A body in M with ISO auto off and a few values on offer for each item.
D810SettingsSnapshot Snapshot(std::int64_t iso = 200, std::int64_t shutter = 167, std::int64_t aperture = 800, std::int64_t wb = 4, std::int64_t metering = 3) {
    D810SettingsSnapshot snapshot;
    D810PropDesc mode;
    mode.code = 0x500e;
    mode.data_type = 4;
    mode.get_set = 1;
    mode.form_flag = 2;
    mode.default_value = mode.current_value = 1;
    mode.values = {1};
    snapshot.push_back(Reading(D810SettingId::ExposureMode, mode));
    D810PropDesc auto_iso;
    auto_iso.code = 0xd054;
    auto_iso.data_type = 2;
    auto_iso.get_set = 1;
    auto_iso.form_flag = 2;
    auto_iso.default_value = auto_iso.current_value = 0;
    auto_iso.values = {0, 1};
    snapshot.push_back(Reading(D810SettingId::IsoAuto, auto_iso));
    snapshot.push_back(Reading(D810SettingId::Iso, EnumDesc(D810ChoiceItem::Iso, iso, {100, 200, 400, 800, 25600})));
    snapshot.push_back(Reading(D810SettingId::ShutterSpeed, EnumDesc(D810ChoiceItem::ShutterSpeed, shutter, {0xffffffffLL, 10000, 167, 125, 100})));
    snapshot.push_back(Reading(D810SettingId::Aperture, EnumDesc(D810ChoiceItem::Aperture, aperture, {400, 560, 800, 1100})));
    snapshot.push_back(Reading(D810SettingId::WhiteBalance, EnumDesc(D810ChoiceItem::WhiteBalance, wb, {1, 2, 4, 5, 0x8013})));
    snapshot.push_back(Reading(D810SettingId::Metering, EnumDesc(D810ChoiceItem::Metering, metering, {1, 2, 3, 4})));
    return snapshot;
}
void Replace(D810SettingsSnapshot& snapshot, D810SettingId id, const D810PropDesc& desc) {
    for (auto& reading : snapshot)
        if (reading.id == id) reading = Reading(id, desc);
}
void Remove(D810SettingsSnapshot& snapshot, D810SettingId id) {
    snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(), [&](const auto& reading) { return reading.id == id; }), snapshot.end());
}
// Every row on, for both cameras, with the codes the product table confirms. A test table only.
D810WriteTable OpenTable() {
    auto table = D810DisabledWriteTable();
    for (auto& row : table.rows) {
        if (row.item == D810WriteItem::IsoAuto) row.code = 0xd054;
        if (row.item == D810WriteItem::ShutterSpeed) row.code = 0x500d;
        row.enabled = true;
    }
    table.copy_a_to_b = true;
    table.chosen_values = true;
    table.verified_model = "D810";
    table.verified_firmware = "V0";
    return table;
}
std::string Locked(const D810PanelCell& cell) { return Refusal(cell.locked_reason); }

void CellsOfAnOpenTable() {
    const auto table = OpenTable();
    check(D810WriteTableProblem(table) == nullptr, "the test table is consistent");
    const auto cell = BuildD810PanelCell(table, D810Camera::B, D810ChoiceItem::Iso, Snapshot(), true);
    check(cell.readable && cell.current == 200 && cell.Editable() && Locked(cell) == "(none)", "an open cell is editable");
    {
        Values offered;
        for (const auto& option : cell.options) offered.push_back(option.value);
        check(offered == Values({100, 200, 400, 800}), "options hold the pickable values only");
    }
    check(cell.dropped.size() == 1 && cell.dropped[0].value == 25600 && Refusal(cell.dropped[0].reason) == "value_out_of_band", "dropped values are kept with the reason");
    check(cell.current_choosable && cell.current_text == "200", "the current value is pickable and reads as its label");

    const auto shutter = BuildD810PanelCell(table, D810Camera::A, D810ChoiceItem::ShutterSpeed, Snapshot(), true);
    check(shutter.Editable() && shutter.current_text == "1/60" && shutter.options.size() == 4, "shutter speed cell (Bulb dropped)");
}

void CellLockReasons() {
    auto table = OpenTable();
    const auto cell_of = [&](const D810WriteTable& t, D810SettingsSnapshot snapshot, bool verified = true, D810Camera camera = D810Camera::B, D810ChoiceItem item = D810ChoiceItem::Iso) {
        return BuildD810PanelCell(t, camera, item, snapshot, verified);
    };
    // Table.
    {
        auto closed = table;
        closed.chosen_values = false;
        const auto cell = cell_of(closed, Snapshot());
        check(Locked(cell) == "chosen_values_disabled" && cell.options.size() == 4, "chosen values off: locked, options still listed");
    }
    {
        auto row_off = table;
        for (auto& row : row_off.rows)
            if (row.item == D810WriteItem::Iso && row.camera == D810Camera::B) row.enabled = false;
        check(Locked(cell_of(row_off, Snapshot())) == "row_disabled", "row off");
        check(Locked(cell_of(row_off, Snapshot(), true, D810Camera::A)) == "(none)", "the other camera's row is its own");
    }
    {
        auto no_code = table;
        for (auto& row : no_code.rows)
            if (row.item == D810WriteItem::Iso && row.camera == D810Camera::B) row.code = 0;
        check(Locked(cell_of(no_code, Snapshot())) == "code_unconfirmed", "row on without a code");
    }
    // Product table: nothing can be picked while chosen values are off.
    {
        const auto product = kD810ProductWriteTable;
        for (const auto camera : {D810Camera::A, D810Camera::B})
            for (const auto item : kD810ChoiceItems) {
                const auto cell = BuildD810PanelCell(product, camera, item, Snapshot(), true);
                check(!cell.Editable() && Locked(cell) == "chosen_values_disabled", "the product table opens no cell");
                check(!cell.options.empty(), "the product table still lists the values");
            }
    }
    check(Locked(cell_of(table, Snapshot(), false)) == "body_not_verified", "body not verified");
    // The body.
    {
        auto manual_off = Snapshot();
        D810PropDesc mode = EnumDesc(D810ChoiceItem::Iso, 2, {1, 2});
        mode.code = 0x500e;
        mode.data_type = 4;
        Replace(manual_off, D810SettingId::ExposureMode, mode);
        check(Locked(cell_of(table, manual_off)) == "exposure_mode_not_manual", "not in M");
        auto no_mode = Snapshot();
        Remove(no_mode, D810SettingId::ExposureMode);
        check(Locked(cell_of(table, no_mode)) == "exposure_mode_not_manual", "mode not read counts as not M");
        auto iso_auto_on = Snapshot();
        D810PropDesc auto_iso = EnumDesc(D810ChoiceItem::Iso, 1, {0, 1});
        auto_iso.code = 0xd054;
        auto_iso.data_type = 2;
        Replace(iso_auto_on, D810SettingId::IsoAuto, auto_iso);
        check(Locked(cell_of(table, iso_auto_on)) == "iso_auto_not_off", "ISO auto on");
        auto no_auto = Snapshot();
        Remove(no_auto, D810SettingId::IsoAuto);
        check(Locked(cell_of(table, no_auto)) == "iso_auto_not_off", "ISO auto not read counts as not off");
    }
    // The item.
    {
        auto unreadable = Snapshot();
        Remove(unreadable, D810SettingId::Iso);
        const auto cell = cell_of(table, unreadable);
        check(!cell.readable && Locked(cell) == "unreadable" && cell.options.empty() && !cell.current_choosable, "item not read");
        auto unavailable = Snapshot();
        for (auto& reading : unavailable)
            if (reading.id == D810SettingId::Iso) reading.available = false;
        check(Locked(cell_of(table, unavailable)) == "unreadable", "item unavailable");
        auto wrong_type = Snapshot();
        auto bad = EnumDesc(D810ChoiceItem::Iso, 200, {100, 200});
        bad.data_type = 6;
        Replace(wrong_type, D810SettingId::Iso, bad);
        check(Locked(cell_of(table, wrong_type)) == "unreadable", "item of another type reads as unreadable");
        auto read_only = Snapshot();
        Replace(read_only, D810SettingId::Iso, EnumDesc(D810ChoiceItem::Iso, 200, {100, 200}, 0));
        check(Locked(cell_of(table, read_only)) == "not_writable", "read only item");
        auto nothing = Snapshot();
        Replace(nothing, D810SettingId::Iso, EnumDesc(D810ChoiceItem::Iso, 25600, {25600}));
        const auto empty = cell_of(table, nothing);
        check(Locked(empty) == "no_options" && empty.options.empty() && !empty.current_choosable, "nothing pickable");
        auto too_wide = Snapshot();
        Replace(too_wide, D810SettingId::Iso, RangeDesc(D810ChoiceItem::Iso, 100, 0, 65535, 1));
        check(Locked(cell_of(table, too_wide)) == "no_options", "a range over 1024 candidates");
    }
    // Precedence follows the order in the contract.
    {
        auto closed = table;
        closed.chosen_values = false;
        auto read_only = Snapshot();
        Replace(read_only, D810SettingId::Iso, EnumDesc(D810ChoiceItem::Iso, 200, {100, 200}, 0));
        check(Locked(cell_of(closed, read_only, false)) == "not_writable", "not_writable comes before the table");
        check(Locked(cell_of(closed, Snapshot(), false)) == "chosen_values_disabled", "the table comes before the body check");
        auto both = Snapshot();
        Remove(both, D810SettingId::ExposureMode);
        Remove(both, D810SettingId::IsoAuto);
        check(Locked(cell_of(table, both)) == "exposure_mode_not_manual", "exposure mode comes before ISO auto");
        auto unreadable = Snapshot();
        Remove(unreadable, D810SettingId::Iso);
        check(Locked(cell_of(closed, unreadable, false)) == "unreadable", "unreadable comes first");
    }
}

void CurrentValueNotPickable() {
    const auto table = OpenTable();
    const auto preset = BuildD810PanelCell(table, D810Camera::A, D810ChoiceItem::WhiteBalance, Snapshot(200, 167, 800, 0x8013), true);
    check(preset.readable && !preset.current_choosable && preset.current == 0x8013, "Preset is not pickable");
    check(preset.current_text == "プリセット（このアプリでは選べません）", "Preset text");
    check(preset.Editable() && preset.Offers(4) && !preset.Offers(0x8013), "the other kinds can still be picked");
    const auto metering = BuildD810PanelCell(table, D810Camera::A, D810ChoiceItem::Metering, Snapshot(200, 167, 800, 4, 1), true);
    check(!metering.current_choosable && metering.current_text == "raw 0x0001（このアプリでは選べません）", "metering 1 reads as raw");
    const auto manual = BuildD810PanelCell(table, D810Camera::A, D810ChoiceItem::WhiteBalance, Snapshot(200, 167, 800, 1), true);
    check(!manual.current_choosable && manual.current_text == "Manual（このアプリでは選べません）", "Manual keeps its existing name");
    const auto iso = BuildD810PanelCell(table, D810Camera::A, D810ChoiceItem::Iso, Snapshot(25600), true);
    check(!iso.current_choosable && iso.current_text == "25600（このアプリでは選べません）", "an ISO outside the band");
}

// --- Draft -----------------------------------------------------------------------------------------------------
struct Bench {
    D810WriteTable table = OpenTable();
    D810PanelModel model;
    D810PanelDraft draft;
    explicit Bench(const D810SettingsSnapshot& a = Snapshot(100), const D810SettingsSnapshot& b = Snapshot(400), std::uint64_t generation = 7)
        : model(BuildD810PanelModel(table, a, true, b, true, generation)), draft(generation) {}
};
using A = D810Camera;
using I = D810ChoiceItem;

void DraftPick() {
    Bench bench;
    check(bench.draft.EntryCount() == 0 && bench.draft.Dirty(bench.model) == 0 && bench.draft.Choices().empty(), "an empty draft has no change");
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 800), "pick a pickable value");
    check(bench.draft.Dirty(bench.model) == 1, "one change");
    const auto& entry = bench.draft.Get(A::B, I::Iso);
    check(entry && entry->source == D810ChoiceSource::Picked && entry->value == 800, "the entry holds the picked value");
    check(!bench.draft.Get(A::A, I::Iso).has_value(), "the other camera has no entry");
    check(!bench.draft.Pick(bench.model, A::B, I::Iso, 25600), "a value dropped by the band is not pickable");
    check(!bench.draft.Pick(bench.model, A::B, I::Iso, 300), "a value not on offer is not pickable");
    check(bench.draft.Get(A::B, I::Iso)->value == 800, "a refused pick leaves the entry as it was");
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 200), "pick again");
    check(bench.draft.EntryCount() == 1 && bench.draft.Get(A::B, I::Iso)->value == 200, "one entry per camera and item: the second replaces the first");
    bench.draft.Cancel(A::B, I::Iso);
    check(bench.draft.EntryCount() == 0 && bench.draft.Dirty(bench.model) == 0, "cancel removes the entry");
    bench.draft.Cancel(A::B, I::Iso);
    check(bench.draft.EntryCount() == 0, "cancel of nothing is harmless");

    // Picking the value a camera already has is an entry, but not a change.
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 400), "pick the current value");
    check(bench.draft.EntryCount() == 1 && bench.draft.Dirty(bench.model) == 0 && bench.draft.PlannedWrites(bench.model).empty(), "the current value is not counted and is no planned write");
    check(bench.draft.Choices().size() == 1, "the choice list still holds the entry (the plan decides it is a no-op)");
}

void DraftLockedCell() {
    Bench bench;
    auto closed = bench.table;
    closed.chosen_values = false;
    const auto locked = BuildD810PanelModel(closed, Snapshot(100), true, Snapshot(400), true, 7);
    D810PanelDraft draft(7);
    check(!draft.Pick(locked, A::B, I::Iso, 800), "a locked cell takes no pick");
    check(!draft.PickSameAsA(locked, I::Iso), "a locked cell takes no same-as-A");
    check(draft.EntryCount() == 0, "nothing stored");
    const auto unverified = BuildD810PanelModel(bench.table, Snapshot(100), true, Snapshot(400), false, 7);
    check(!draft.Pick(unverified, A::B, I::Iso, 800) && draft.Pick(unverified, A::A, I::Iso, 800), "only the unverified camera is locked");
}

void DraftSameAsA() {
    Bench bench;
    // CAM-A reads 100, CAM-B reads 400.
    check(bench.draft.PickSameAsA(bench.model, I::Iso), "same as A is accepted");
    const auto& entry = bench.draft.Get(A::B, I::Iso);
    check(entry && entry->source == D810ChoiceSource::SameAsA && !entry->value.has_value(), "same as A holds no value");
    check(!bench.draft.Get(A::A, I::Iso).has_value(), "same as A is never an entry of CAM-A");
    auto writes = bench.draft.PlannedWrites(bench.model);
    check(writes.size() == 1 && writes[0].camera == A::B && writes[0].item == I::Iso && writes[0].value == 100 && writes[0].current == 400,
          "with no pick for CAM-A it resolves to what CAM-A reads");
    check(*bench.draft.EffectiveAValue(bench.model, I::Iso) == 100, "effective value of A: its reading");

    // A pick for CAM-A moves the value same-as-A resolves to.
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 800), "pick for CAM-A");
    check(*bench.draft.EffectiveAValue(bench.model, I::Iso) == 800, "effective value of A: its pick");
    writes = bench.draft.PlannedWrites(bench.model);
    check(writes.size() == 2, "CAM-A changes and CAM-B follows");
    check(writes[0].camera == A::B && writes[0].value == 800 && writes[1].camera == A::A && writes[1].value == 800, "CAM-B first, both at the picked value");
    check(bench.draft.Get(A::B, I::Iso)->source == D810ChoiceSource::SameAsA && !bench.draft.Get(A::B, I::Iso)->value, "the same-as-A entry still holds no value");
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 200), "pick another value for CAM-A");
    check(bench.draft.PlannedWrites(bench.model)[0].value == 200, "CAM-B follows the new pick");
    // The pick that leaves CAM-A at what it reads leaves CAM-B following that.
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 100), "pick CAM-A's own value");
    writes = bench.draft.PlannedWrites(bench.model);
    check(writes.size() == 1 && writes[0].camera == A::B && writes[0].value == 100, "CAM-A unchanged, CAM-B follows");
    // Cancelling CAM-A's pick goes back to its reading.
    bench.draft.Cancel(A::A, I::Iso);
    check(*bench.draft.EffectiveAValue(bench.model, I::Iso) == 100, "after cancel A's reading again");
    // A pick for CAM-B replaces same-as-A, and the other way round.
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 800), "pick for CAM-B");
    check(bench.draft.Get(A::B, I::Iso)->source == D810ChoiceSource::Picked && bench.draft.EntryCount() == 1, "a pick replaces same-as-A");
    check(bench.draft.PickSameAsA(bench.model, I::Iso) && bench.draft.Get(A::B, I::Iso)->source == D810ChoiceSource::SameAsA && !bench.draft.Get(A::B, I::Iso)->value,
          "same-as-A replaces a pick and drops its value");

    // CAM-B already equal to CAM-A: not a change.
    Bench equal(Snapshot(200), Snapshot(200));
    check(equal.draft.PickSameAsA(equal.model, I::Iso) && equal.draft.Dirty(equal.model) == 0, "same as A when the cameras are equal is not counted");
}

void DraftSameAsAOffered() {
    // CAM-A reads 800 and CAM-B does not offer it.
    auto b = Snapshot(100);
    Replace(b, D810SettingId::Iso, EnumDesc(D810ChoiceItem::Iso, 100, {100, 200}));
    Bench bench(Snapshot(800), b);
    check(!bench.draft.SameAsAOffered(bench.model, I::Iso) && !bench.draft.PickSameAsA(bench.model, I::Iso), "same as A is refused when CAM-B does not offer CAM-A's value");
    check(bench.draft.EntryCount() == 0, "nothing stored");
    // CAM-A picks a value CAM-B does offer.
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 200) && bench.draft.SameAsAOffered(bench.model, I::Iso) && bench.draft.PickSameAsA(bench.model, I::Iso),
          "same as A is accepted once CAM-A's value is on offer");
    // CAM-A then moves to a value CAM-B lacks: the entry stays and is listed; the plan refuses it.
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 400), "CAM-A picks 400, which CAM-B does not offer");
    check(!bench.draft.SameAsAOffered(bench.model, I::Iso) && bench.draft.Get(A::B, I::Iso)->source == D810ChoiceSource::SameAsA, "the same-as-A entry stays");
    check(bench.draft.PlannedWrites(bench.model).size() == 2 && bench.draft.PlannedWrites(bench.model)[0].value == 400, "and is listed at A's value, for the plan to refuse");
    // CAM-A unreadable.
    auto a_off = Snapshot(100);
    Remove(a_off, D810SettingId::Iso);
    Bench unreadable(a_off, Snapshot(400));
    check(!unreadable.draft.EffectiveAValue(unreadable.model, I::Iso).has_value() && !unreadable.draft.PickSameAsA(unreadable.model, I::Iso), "CAM-A not read: no value to follow");
}

void DraftOrder() {
    Bench bench(Snapshot(100, 167, 800, 4, 3), Snapshot(400, 125, 400, 5, 2));
    // Entered in a scrambled order.
    check(bench.draft.Pick(bench.model, A::A, I::Metering, 4));
    check(bench.draft.Pick(bench.model, A::B, I::WhiteBalance, 2));
    check(bench.draft.Pick(bench.model, A::A, I::Iso, 200));
    check(bench.draft.PickSameAsA(bench.model, I::ShutterSpeed));
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 800));
    check(bench.draft.Pick(bench.model, A::A, I::Aperture, 560));
    check(bench.draft.Pick(bench.model, A::B, I::Metering, 3));
    check(bench.draft.EntryCount() == 7, "seven entries");
    const auto writes = bench.draft.PlannedWrites(bench.model);
    const std::vector<std::pair<A, I>> expected = {
        {A::B, I::Iso}, {A::B, I::ShutterSpeed}, {A::B, I::WhiteBalance}, {A::B, I::Metering},
        {A::A, I::Iso}, {A::A, I::Aperture}, {A::A, I::Metering}};
    check(writes.size() == expected.size(), "all seven are changes");
    for (std::size_t i = 0; i < std::min(writes.size(), expected.size()); ++i)
        check(writes[i].camera == expected[i].first && writes[i].item == expected[i].second, "CAM-B first, the item table's order within a camera");
    check(bench.draft.Dirty(bench.model) == 7, "dirty counts the list");
    // One entry per camera and item.
    std::set<std::pair<int, int>> seen;
    for (const auto& write : writes) seen.insert({static_cast<int>(write.camera), static_cast<int>(write.item)});
    check(seen.size() == writes.size(), "no (camera, item) twice");
    // Shutter: CAM-B follows CAM-A's reading, 1/60.
    for (const auto& write : writes)
        if (write.camera == A::B && write.item == I::ShutterSpeed) check(write.value == 167 && write.current == 125, "same as A resolved");

    const auto choices = bench.draft.Choices();
    check(choices.size() == 7, "seven choices");
    for (std::size_t i = 0; i < std::min(choices.size(), expected.size()); ++i)
        check(choices[i].camera == expected[i].first && choices[i].item == D810ChoiceWriteItem(expected[i].second), "choices in the same order, as write items");
    for (const auto& choice : choices) {
        if (choice.source == D810ChoiceSource::SameAsA) check(choice.camera == A::B && choice.value == 0, "a same-as-A choice is CAM-B's and carries no value");
        else check(choice.value != 0, "a picked choice carries its value");
    }
    // Choices never repeat a (camera, item) either.
    std::set<std::pair<int, int>> pairs;
    for (const auto& choice : choices) pairs.insert({static_cast<int>(choice.camera), static_cast<int>(choice.item)});
    check(pairs.size() == choices.size(), "no choice twice");
}

void DraftDiscardAndStale() {
    Bench bench;
    check(bench.draft.Pick(bench.model, A::B, I::Iso, 800) && bench.draft.Pick(bench.model, A::A, I::Aperture, 560) && bench.draft.Pick(bench.model, A::B, I::Metering, 3));
    check(bench.draft.Pick(bench.model, A::A, I::Metering, 3), "an entry equal to the reading");
    // Both bodies read metering 3, so only ISO on CAM-B and aperture on CAM-A are changes.
    check(bench.draft.EntryCount() == 4 && bench.draft.Dirty(bench.model) == 2, "four entries, two changes");
    const auto dropped = bench.draft.Discard(bench.model);
    check(dropped == 2, "discard returns the number of changes dropped");
    check(bench.draft.EntryCount() == 0 && bench.draft.Dirty(bench.model) == 0 && bench.draft.Choices().empty(), "discard empties the draft");

    // A draft of another read is not used.
    Bench other;
    check(other.draft.Pick(other.model, A::B, I::Iso, 800), "pick");
    const auto next_model = BuildD810PanelModel(other.table, Snapshot(100), true, Snapshot(400), true, 8);
    check(other.draft.Dirty(next_model) == 0 && other.draft.PlannedWrites(next_model).empty(), "a stale draft counts nothing");
    check(!other.draft.Pick(next_model, A::B, I::Iso, 800) && !other.draft.PickSameAsA(next_model, I::Iso), "a stale draft takes no pick");
    check(other.draft.Discard(next_model) == 0 && other.draft.EntryCount() == 0 && other.draft.ReadGeneration() == 8, "discard of a stale draft counts 0, empties it and adopts the new read");
    check(other.draft.Pick(next_model, A::B, I::Iso, 800) && other.draft.Dirty(next_model) == 1, "the draft is usable again for the new read");
}

// A change in the reading under an existing draft changes what it stands for, not what it holds.
void DraftFollowsTheReading() {
    Bench bench(Snapshot(100), Snapshot(400));
    check(bench.draft.PickSameAsA(bench.model, I::Iso) && bench.draft.Dirty(bench.model) == 1);
    // The same generation re-evaluated against a model where CAM-A has moved: SameAsA resolves to the new value.
    const auto moved = BuildD810PanelModel(bench.table, Snapshot(200), true, Snapshot(400), true, 7);
    const auto writes = bench.draft.PlannedWrites(moved);
    check(writes.size() == 1 && writes[0].value == 200, "same as A resolves against the reading it is given, not the one it was entered under");
    // A pick stays what it was.
    Bench picked(Snapshot(100), Snapshot(400));
    check(picked.draft.Pick(picked.model, A::B, I::Iso, 800));
    const auto changed = BuildD810PanelModel(picked.table, Snapshot(100), true, Snapshot(800), true, 7);
    check(picked.draft.Dirty(changed) == 0, "a pick equal to the reading it meets is no change");
}

// Nothing in the header can send: it includes no gate, session or plan builder.
void NoWritePath() {
    // A draft is data. Building every cell of the product table and an empty draft leaves nothing to execute.
    const auto model = BuildD810PanelModel(kD810ProductWriteTable, Snapshot(), true, Snapshot(), true, 1);
    std::size_t editable = 0;
    for (const auto camera : {A::A, A::B})
        for (const auto item : kD810ChoiceItems) editable += model.At(camera, item).Editable();
    check(editable == 0, "no cell of the product table is editable");
    D810PanelDraft draft(1);
    for (const auto camera : {A::A, A::B})
        for (const auto item : kD810ChoiceItems) check(!draft.Pick(model, camera, item, 100), "no pick on the product table");
    check(!draft.PickSameAsA(model, I::Iso) && draft.EntryCount() == 0, "no entry on the product table");
}
} // namespace

int main() {
    const auto run = [](const char* name, void (*test)()) {
        try { test(); }
        catch (const std::exception& error) { ++failures; std::cerr << name << " threw: " << error.what() << "\n"; }
        catch (...) { ++failures; std::cerr << name << " threw\n"; }
    };
    run("Items", Items);
    run("Advertised", Advertised);
    run("IsoOptions", IsoOptions);
    run("ShutterOptions", ShutterOptions);
    run("ApertureOptions", ApertureOptions);
    run("WhiteBalanceOptions", WhiteBalanceOptions);
    run("MeteringOptions", MeteringOptions);
    run("RefusalReasons", RefusalReasons);
    run("SieveMatchesTheGate", SieveMatchesTheGate);
    run("DisplayNames", DisplayNames);
    run("CellsOfAnOpenTable", CellsOfAnOpenTable);
    run("CellLockReasons", CellLockReasons);
    run("CurrentValueNotPickable", CurrentValueNotPickable);
    run("DraftPick", DraftPick);
    run("DraftLockedCell", DraftLockedCell);
    run("DraftSameAsA", DraftSameAsA);
    run("DraftSameAsAOffered", DraftSameAsAOffered);
    run("DraftOrder", DraftOrder);
    run("DraftDiscardAndStale", DraftDiscardAndStale);
    run("DraftFollowsTheReading", DraftFollowsTheReading);
    run("NoWritePath", NoWritePath);
    std::cout << "d810_settings_choice checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
