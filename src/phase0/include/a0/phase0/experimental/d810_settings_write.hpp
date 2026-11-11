#pragma once
// Camera settings write: item table, the fixed write table, write plans and the
// operation record format (#416 stage B, ADR-0038; the ADR-0041 extension, values
// chosen per camera and CAM-A rows, is present as code but disabled by the fixed
// table and by kD810Adr41Accepted).
//
// Pure functions only: no Windows headers, no device access, no file I/O. The
// gate, the session and the files are in wpd_settings_write.hpp.
//
// What can be written is decided at compile time and nowhere else:
//   1. kD810WriteItems: the only items that can ever be written, with their
//      type, fixed values and excluded values. Image quality, image size,
//      focus, recording media and every other property are absent.
//   2. D810WriteTable: one row per item and camera, with the confirmed code and
//      an enabled flag, the plan kinds that are on, and the camera model and
//      firmware version the rows were confirmed on. kD810ProductWriteTable is
//      the table of the product; it is checked by static_assert. Changing it is
//      a reviewed commit. No file, environment variable, registry value or
//      command-line argument can enable a row, confirm a code or turn on a plan
//      kind; run-time inputs can only stop a write.
// Whether the body really offers the item is checked again before every write
// from a description read in the same session (type, GetSet 1, an enumeration
// or a range with a positive step).
#include "a0/phase0/experimental/d810_settings.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0::experimental {
enum class D810Camera : std::uint8_t { A = 0, B = 1 };
inline const char* D810CameraAlias(D810Camera camera) { return camera == D810Camera::A ? "CAM-A" : "CAM-B"; }

// --- Item table ------------------------------------------------------------------
enum class D810WriteItem : std::uint8_t { ExposureMode, IsoAuto, Iso, ShutterSpeed, Aperture, WhiteBalance, Metering, ExposureCompensation };
inline constexpr std::size_t kD810WriteItemCount = 8;
// The decision an item belongs to.
//   Adr38          the five exposure items ADR-0038 accepted.
//   Adr38Revision  white balance and metering mode: the ADR-0038 revision (the owner decided to add them).
//                  A copy plan covers them, and a product table may enable their CAM-B rows
//                  only while kD810Adr38RevisionAccepted is set.
//   Adr41          the rest (Proposed, values chosen by the operator only).
enum class D810ItemScope : std::uint8_t { Adr38, Adr38Revision, Adr41 };
// Set by the reviewed commit that enables the white balance and metering rows. While it is false the
// product table cannot hold an enabled row for an Adr38Revision item (D810ProductTableProblem).
inline constexpr bool kD810Adr38RevisionAccepted = true;
// ADR-0041 (values chosen per camera, CAM-A rows). While it is false the product table can neither hold a
// chosen-values flag nor an enabled CAM-A row (D810ProductTableProblem), so nothing can be written to CAM-A and
// no chosen plan can be made. It is set only by a reviewed commit after the owner accepts ADR-0041.
inline constexpr bool kD810Adr41Accepted = false;
static_assert(!kD810Adr41Accepted, "ADR-0041 is not accepted: the commit that sets this flag replaces this assertion with the set of enabled rows");
// What the target body must read, in the same session and generation, before
// the item may be armed (ADR-0038 decision 4, M-3).
enum class D810WritePrecondition : std::uint8_t { LiveOff, ManualMode, ManualModeIsoAutoOff };
struct D810WriteItemSpec {
    D810WriteItem item;
    const char* name;
    D810ValueType type;
    // Candidate codes from the public sources. Items with two candidates are
    // written only at the code the fixed table confirms; the code is never
    // searched for at run time and never switched (ADR-0038 decision 13, M-7).
    std::array<std::uint16_t, 2> codes;
    std::size_t code_count;
    bool fixed;
    std::int64_t fixed_value;
    D810ItemScope scope;
    D810WritePrecondition precondition;
    // Values that depend on other content of the body (white balance Manual,
    // One-push Automatic, Color Temperature, Preset): never written and never
    // copied, and "unknown" when compared.
    std::array<std::int64_t, 4> excluded;
    std::size_t excluded_count;
    // A closed list of the values that may be written (the values whose meaning
    // is named in third_party/PTP_PROTOCOL_REFERENCES.md). Empty: no list.
    std::array<std::int64_t, 8> allowed;
    std::size_t allowed_count;
};
// Table order is the write order (ADR-0038 decision 5). Picture Control and
// Active D-Lighting are absent on purpose: their scalar type is not settled.
inline constexpr std::array<D810WriteItemSpec, kD810WriteItemCount> kD810WriteItems{{
    {D810WriteItem::ExposureMode, "exposure_mode", D810ValueType::Uint16, {0x500e, 0}, 1, true, 1, D810ItemScope::Adr38, D810WritePrecondition::LiveOff, {}, 0, {}, 0},
    {D810WriteItem::IsoAuto, "iso_auto", D810ValueType::Uint8, {0xd054, 0xd16a}, 2, true, 0, D810ItemScope::Adr38, D810WritePrecondition::ManualMode, {}, 0, {}, 0},
    {D810WriteItem::Iso, "iso", D810ValueType::Uint16, {0x500f, 0}, 1, false, 0, D810ItemScope::Adr38, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0, {}, 0},
    {D810WriteItem::ShutterSpeed, "shutter_speed", D810ValueType::Uint32, {0x500d, 0xd100}, 2, false, 0, D810ItemScope::Adr38, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0, {}, 0},
    {D810WriteItem::Aperture, "aperture", D810ValueType::Uint16, {0x5007, 0}, 1, false, 0, D810ItemScope::Adr38, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0, {}, 0},
    // White balance: the kind only (Automatic, Daylight, Fluorescent, Tungsten, Flash, Cloudy, Shade). Manual (1),
    // One-push Automatic (3), Color Temperature (0x8012) and Preset (0x8013) lean on content held in the body
    // (a measured white, a temperature in another property), which a copy cannot carry.
    {D810WriteItem::WhiteBalance, "white_balance", D810ValueType::Uint16, {0x5005, 0}, 1, false, 0, D810ItemScope::Adr38Revision, D810WritePrecondition::ManualModeIsoAutoOff,
        {1, 3, 0x8012, 0x8013}, 4, {2, 4, 5, 6, 7, 0x8010, 0x8011}, 7},
    {D810WriteItem::Metering, "metering", D810ValueType::Uint16, {0x500b, 0}, 1, false, 0, D810ItemScope::Adr38Revision, D810WritePrecondition::ManualModeIsoAutoOff,
        {}, 0, {1, 2, 3, 4}, 4},
    {D810WriteItem::ExposureCompensation, "exposure_compensation", D810ValueType::Int16, {0x5010, 0}, 1, false, 0, D810ItemScope::Adr41, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0, {}, 0},
}};
inline constexpr std::uint16_t kD810ExposureModeCode = 0x500e;
inline constexpr std::int64_t kD810ExposureModeManual = 1;
inline constexpr std::int64_t kD810IsoAutoOff = 0;
// Longest shutter time that may be written, in 1/10000 s: 1 s. Provisional, but
// never "no limit". Bulb (0xFFFFFFFF), Time (0xFFFFFFFD) and zero are refused
// separately, in the encoding of 0x500D (the only shutter code the fixed table
// may confirm until 0xD100's encoding has a public source).
inline constexpr std::int64_t kD810MaxWritableExposureUnits = 10000;

constexpr const D810WriteItemSpec& D810WriteItemOf(D810WriteItem item) { return kD810WriteItems[static_cast<std::size_t>(item)]; }
constexpr bool D810WriteItemHasCode(const D810WriteItemSpec& spec, std::uint32_t code) {
    for (std::size_t i = 0; i < spec.code_count; ++i)
        if (spec.codes[i] == code) return true;
    return false;
}
// The item a code belongs to, or nothing for any code outside the table.
constexpr const D810WriteItemSpec* FindD810WriteItemByCode(std::uint32_t code) {
    for (const auto& spec : kD810WriteItems)
        if (D810WriteItemHasCode(spec, code)) return &spec;
    return nullptr;
}
constexpr bool D810WriteValueExcluded(const D810WriteItemSpec& spec, std::int64_t value) {
    for (std::size_t i = 0; i < spec.excluded_count; ++i)
        if (spec.excluded[i] == value) return true;
    return false;
}
// True when the item has no closed list, or the list names `value`.
constexpr bool D810WriteValueListed(const D810WriteItemSpec& spec, std::int64_t value) {
    if (spec.allowed_count == 0) return true;
    for (std::size_t i = 0; i < spec.allowed_count; ++i)
        if (spec.allowed[i] == value) return true;
    return false;
}
// The reason a value cannot be copied from CAM-A at all (before CAM-B is asked), or nullptr.
// "value_content_dependent": the value leans on content held in the body, which a copy cannot carry.
// "value_not_listed": the value is not one of those the item names.
constexpr const char* D810CopyValueRefusal(const D810WriteItemSpec& spec, std::int64_t value) {
    if (D810WriteValueExcluded(spec, value)) return "value_content_dependent";
    if (!D810WriteValueListed(spec, value)) return "value_not_listed";
    return nullptr;
}
// Items a product table may enable: the accepted five, and the revision items once it is accepted.
// `revision_accepted` is the constant by default; the tests pass the other value to keep the refusal under test.
constexpr bool D810ItemInProductScope(const D810WriteItemSpec& spec, bool revision_accepted = kD810Adr38RevisionAccepted) {
    return spec.scope == D810ItemScope::Adr38 || (spec.scope == D810ItemScope::Adr38Revision && revision_accepted);
}

// --- Values ------------------------------------------------------------------------
constexpr bool D810ValueFits(D810ValueType type, std::int64_t value) {
    switch (type) {
    case D810ValueType::Int8: return value >= -128 && value <= 127;
    case D810ValueType::Uint8: return value >= 0 && value <= 0xff;
    case D810ValueType::Int16: return value >= -32768 && value <= 32767;
    case D810ValueType::Uint16: return value >= 0 && value <= 0xffff;
    case D810ValueType::Int32: return value >= INT32_MIN && value <= INT32_MAX;
    case D810ValueType::Uint32: return value >= 0 && value <= 0xffffffffLL;
    }
    return false;
}
// Little endian, exactly D810ValueBytes(type) bytes (two's complement for the signed types).
inline std::vector<unsigned char> EncodeD810Value(D810ValueType type, std::int64_t value) {
    std::vector<unsigned char> out;
    const auto raw = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < D810ValueBytes(type); ++i) out.push_back(static_cast<unsigned char>((raw >> (8 * i)) & 0xff));
    return out;
}
// Writable for this application: GetSet 1 and a form that offers values (an
// enumeration with values, or a range with a positive step). Stricter than
// D810PropDesc::Writable(), which also accepts a description without a form.
inline bool D810DescWritable(const D810PropDesc& desc) {
    if (desc.get_set != 1) return false;
    if (desc.form_flag == 2) return !desc.values.empty();
    if (desc.form_flag == 1) return desc.range_step > 0 && desc.range_min <= desc.range_max;
    return false;
}
// The body offered the value: in the enumeration, or on a step of the range.
// Values are sign-extended for signed types, so the range compares signed.
inline bool D810ValueAdvertised(const D810PropDesc& desc, std::int64_t value) {
    if (desc.form_flag == 2) return std::find(desc.values.begin(), desc.values.end(), value) != desc.values.end();
    if (desc.form_flag == 1)
        return desc.range_step > 0 && value >= desc.range_min && value <= desc.range_max && (value - desc.range_min) % desc.range_step == 0;
    return false;
}
// Shutter values that are never written: Bulb, Time, zero and long exposures.
inline const char* D810ShutterRefusal(std::int64_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    if (raw == 0xffffffffu || raw == 0xfffffffdu) return "bulb_or_time";
    if (value <= 0) return "exposure_zero";
    if (value > kD810MaxWritableExposureUnits) return "exposure_too_long";
    return nullptr;
}
// Why `value` cannot be written to `spec` on a body that returned `desc`, or
// nullptr when it can. Used when a plan is built and again when the gate arms.
inline const char* D810WriteValueRefusal(const D810WriteItemSpec& spec, const D810PropDesc& desc, std::int64_t value) {
    if (desc.data_type != static_cast<std::uint16_t>(spec.type)) return "type_mismatch";
    if (!D810DescWritable(desc)) return "not_writable";
    if (!D810ValueFits(spec.type, value)) return "value_out_of_type";
    if (spec.fixed && value != spec.fixed_value) return "value_not_fixed";
    if (const auto refusal = D810CopyValueRefusal(spec, value)) return refusal;
    if (spec.item == D810WriteItem::ShutterSpeed) {
        if (const auto refusal = D810ShutterRefusal(value)) return refusal;
    }
    if (!D810ValueAdvertised(desc, value)) return "value_not_advertised";
    return nullptr;
}

// --- The fixed write table ----------------------------------------------------------
constexpr bool D810TextPresent(const char* text) { return text != nullptr && text[0] != '\0'; }
struct D810WriteRow {
    D810WriteItem item;
    D810Camera camera;
    // The confirmed code, or 0 while it is not confirmed (then the row cannot be enabled).
    std::uint16_t code;
    bool enabled;
};
struct D810WriteTable {
    std::array<D810WriteRow, 2 * kD810WriteItemCount> rows;
    // ADR-0038: copy CAM-A's five exposure items to CAM-B (and, once the ADR-0038
    // revision is accepted, white balance and metering).
    bool copy_a_to_b;
    // ADR-0041 (Proposed): values chosen by the operator.
    bool chosen_values;
    // The camera model and firmware version the rows were confirmed on. A body
    // that reports anything else is not written; nullptr means none confirmed.
    // The model is the same for both cameras; the version is per camera:
    // `verified_firmware` is CAM-B's, `verified_firmware_a` is CAM-A's. While CAM-A's
    // is empty no CAM-A body passes the check, so CAM-A is not written.
    const char* verified_model;
    const char* verified_firmware;
    const char* verified_firmware_a;
    constexpr const D810WriteRow* Row(D810WriteItem item, D810Camera camera) const {
        for (const auto& row : rows)
            if (row.item == item && row.camera == camera) return &row;
        return nullptr;
    }
    // The confirmed code of an item, 0 when not confirmed.
    constexpr std::uint16_t Code(D810WriteItem item) const {
        for (const auto& row : rows)
            if (row.item == item && row.code) return row.code;
        return 0;
    }
};
// Every row present and disabled; single-code items carry their code, the two
// items with candidate codes carry none.
constexpr D810WriteTable D810DisabledWriteTable() {
    D810WriteTable table{};
    std::size_t i = 0;
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto& spec : kD810WriteItems)
            table.rows[i++] = {spec.item, camera, spec.code_count == 1 ? spec.codes[0] : std::uint16_t{0}, false};
    table.copy_a_to_b = false;
    table.chosen_values = false;
    table.verified_model = nullptr;
    table.verified_firmware = nullptr;
    table.verified_firmware_a = nullptr;
    return table;
}
// Consistency of any table (also the ones the tests build).
constexpr const char* D810WriteTableProblem(const D810WriteTable& table) {
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        const auto& row = table.rows[i];
        const auto& spec = D810WriteItemOf(row.item);
        if (row.code != 0 && !D810WriteItemHasCode(spec, row.code)) return "code_not_a_candidate";
        if (row.code == 0xd100) return "nikon_shutter_encoding_unsourced";
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = table.rows[j];
            if (other.item == row.item && other.camera == row.camera) return "duplicate_row";
            if (other.item == row.item && other.code != 0 && row.code != 0 && other.code != row.code) return "codes_disagree";
        }
        if (row.enabled) {
            if (row.code == 0) return "enabled_without_code";
            if (spec.precondition == D810WritePrecondition::ManualModeIsoAutoOff && table.Code(D810WriteItem::IsoAuto) == 0) return "iso_auto_code_unconfirmed";
            // A CAM-A row is enabled only together with the firmware version CAM-A was confirmed on.
            if (row.camera == D810Camera::A && !D810TextPresent(table.verified_firmware_a)) return "cam_a_version_unconfirmed";
        }
    }
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto& spec : kD810WriteItems)
            if (!table.Row(spec.item, camera)) return "missing_row";
    return nullptr;
}
// What the product table may hold until ADR-0041 is accepted: rows for CAM-B
// and the accepted items only (the five of ADR-0038, plus white balance and
// metering once the ADR-0038 revision is accepted), no chosen values. ADR-0038
// invariant 2 (CAM-A is never written) holds at compile time.
//
// ADR-0041 (`adr41_accepted`: the constant by default, the tests pass the other value to keep the refusals under
// test): the chosen-values flag and CAM-A rows need it. A CAM-A row is allowed only together with the chosen-values flag
// (a CAM-A row is written by a chosen plan only, never by a copy) and only for the items a chosen plan can write
// (never the exposure mode or ISO auto control).
constexpr const char* D810ProductTableProblem(const D810WriteTable& table, bool revision_accepted = kD810Adr38RevisionAccepted,
    bool adr41_accepted = kD810Adr41Accepted) {
    if (const auto problem = D810WriteTableProblem(table)) return problem;
    if (table.chosen_values && !adr41_accepted) return "chosen_values_before_adr0041";
    for (const auto& row : table.rows) {
        if (!row.enabled) continue;
        if (row.camera == D810Camera::A) {
            if (!(adr41_accepted && table.chosen_values)) return "cam_a_row_enabled";
            if (row.item == D810WriteItem::ExposureMode || row.item == D810WriteItem::IsoAuto) return "cam_a_row_not_choosable";
        }
        if (!D810ItemInProductScope(D810WriteItemOf(row.item), revision_accepted)) return "extra_item_enabled";
    }
    return nullptr;
}
constexpr std::size_t D810EnabledRowCount(const D810WriteTable& table) {
    std::size_t count = 0;
    for (const auto& row : table.rows) count += row.enabled;
    return count;
}
// The items a copy from CAM-A may write to CAM-B with this table: the items whose CAM-B row is enabled, in table
// order. What the screen names as writable comes from here, not from a fixed sentence.
inline std::vector<D810WriteItem> D810CopyWritableItems(const D810WriteTable& table) {
    std::vector<D810WriteItem> items;
    if (!table.copy_a_to_b) return items;
    for (const auto& spec : kD810WriteItems) {
        if (spec.scope == D810ItemScope::Adr41) continue;
        const auto* row = table.Row(spec.item, D810Camera::B);
        if (row && row->enabled && row->code != 0) items.push_back(spec.item);
    }
    return items;
}
// The product table. The stage A read on the real bodies confirmed ISO auto at
// 0xD054 (not 0xD16A) and shutter speed at 0x500D (not 0xD100). ISO auto is
// recorded with its rows disabled: the plan reads it for its preconditions and
// its refusals, but nothing writes it. The enabled rows are CAM-B's ISO,
// shutter speed, aperture, white balance and metering mode (stages 1 to 3), and
// the firmware version is the one the stage A read reported for CAM-B.
// Enabling another row is a reviewed commit to this table, one stage at a time.
inline constexpr D810WriteTable kD810ProductWriteTable = [] {
    auto table = D810DisabledWriteTable();
    for (auto& row : table.rows) {
        if (row.item == D810WriteItem::IsoAuto) row.code = 0xd054;
        if (row.item == D810WriteItem::ShutterSpeed) row.code = 0x500d;
        if (row.item == D810WriteItem::Iso && row.camera == D810Camera::B) row.enabled = true;
        if (row.item == D810WriteItem::ShutterSpeed && row.camera == D810Camera::B) row.enabled = true;
        if (row.item == D810WriteItem::Aperture && row.camera == D810Camera::B) row.enabled = true;
        if (row.item == D810WriteItem::WhiteBalance && row.camera == D810Camera::B) row.enabled = true;
        if (row.item == D810WriteItem::Metering && row.camera == D810Camera::B) row.enabled = true;
    }
    table.copy_a_to_b = true;
    table.verified_model = "D810";
    table.verified_firmware = "V1.14";
    return table;
}();
// The enabled set of the product table and the codes it confirms, written out so that a change to either is a
// compile error here and a reviewed edit of this block.
constexpr std::uint32_t D810ItemBit(D810WriteItem item) { return 1u << static_cast<unsigned>(item); }
// True when the only enabled rows are CAM-B's, for exactly the items in `camera_b_items` (a set of D810ItemBit), and the two
// confirmed codes are the ones above.
constexpr bool D810ProductTableEnablesOnly(const D810WriteTable& table, std::uint32_t camera_b_items) {
    std::uint32_t enabled = 0;
    for (const auto& row : table.rows) {
        if (row.enabled) {
            if (row.camera != D810Camera::B) return false;
            enabled |= D810ItemBit(row.item);
        }
        if (row.item == D810WriteItem::IsoAuto && row.code != 0xd054) return false;
        if (row.item == D810WriteItem::ShutterSpeed && row.code != 0x500d) return false;
    }
    return enabled == camera_b_items;
}
static_assert(D810ProductTableProblem(kD810ProductWriteTable) == nullptr, "the product write table is outside ADR-0038");
static_assert(D810ProductTableEnablesOnly(kD810ProductWriteTable,
        D810ItemBit(D810WriteItem::Iso) | D810ItemBit(D810WriteItem::ShutterSpeed) | D810ItemBit(D810WriteItem::Aperture) | D810ItemBit(D810WriteItem::WhiteBalance) |
            D810ItemBit(D810WriteItem::Metering)),
    "the product table enables exactly ISO, shutter speed, aperture, white balance and metering on CAM-B and confirms ISO auto 0xD054 and shutter speed 0x500D");
static_assert(D810TextPresent(kD810ProductWriteTable.verified_model), "the product table names the camera model it was confirmed on");
static_assert(D810TextPresent(kD810ProductWriteTable.verified_firmware), "the product table names the firmware version it was confirmed on");
static_assert(!D810TextPresent(kD810ProductWriteTable.verified_firmware_a) && !kD810ProductWriteTable.chosen_values,
    "the product table names no firmware version for CAM-A and has no chosen values: CAM-A is not written (ADR-0041 is not accepted)");
static_assert(kD810Adr38RevisionAccepted, "the product table enables white balance and metering, which needs the ADR-0038 revision flag");

enum class D810PlanSource : std::uint8_t { CopyAToB, Chosen };
inline const char* D810PlanSourceToken(D810PlanSource source) { return source == D810PlanSource::CopyAToB ? "copy" : "chosen"; }
// Why the table does not let `item` be written to `camera` by a plan of `source`.
inline const char* D810RowRefusal(const D810WriteTable& table, D810WriteItem item, D810Camera camera, D810PlanSource source) {
    const auto& spec = D810WriteItemOf(item);
    if (source == D810PlanSource::CopyAToB) {
        if (!table.copy_a_to_b) return "copy_disabled";
        if (camera != D810Camera::B) return "copy_targets_cam_b_only";
        if (spec.scope == D810ItemScope::Adr41) return "copy_item_out_of_scope";
    } else if (!table.chosen_values) return "chosen_values_disabled";
    const auto* row = table.Row(item, camera);
    if (!row || !row->enabled) return "row_disabled";
    if (row->code == 0) return "code_unconfirmed";
    return nullptr;
}
// The two comparisons of a body's identity with the table, kept apart so that a log can say which one failed
// (booleans only: the strings themselves are never logged). An empty text never matches, on either side: a
// body whose model or version could not be read does not match an empty entry of a table.
struct D810BodyCheck {
    bool model_match{};
    bool firmware_match{};
    bool Verified() const { return model_match && firmware_match; }
};
// The firmware version the table names for `camera`: CAM-B's `verified_firmware`, CAM-A's `verified_firmware_a`.
constexpr const char* D810VerifiedFirmware(const D810WriteTable& table, D810Camera camera) {
    return camera == D810Camera::A ? table.verified_firmware_a : table.verified_firmware;
}
inline D810BodyCheck D810CheckBody(const D810WriteTable& table, D810Camera camera, std::string_view model, std::string_view firmware) {
    D810BodyCheck check;
    const char* named = D810VerifiedFirmware(table, camera);
    check.model_match = D810TextPresent(table.verified_model) && !model.empty() && model == table.verified_model;
    check.firmware_match = D810TextPresent(named) && !firmware.empty() && firmware == named;
    return check;
}
// A body may be written only when it reports the model and the firmware the table names for that camera.
inline bool D810BodyVerified(const D810WriteTable& table, D810Camera camera, std::string_view model, std::string_view firmware) {
    return D810CheckBody(table, camera, model, firmware).Verified();
}
// Which of the two bodies (indexed by camera: CAM-A, CAM-B) matched the model and firmware the table names for it.
using D810BodyFlags = std::array<bool, 2>;
// Items whose equality on both cameras releases a capture block: the five of
// ADR-0038, and every additional item with an enabled row (white balance and
// metering join once their rows are enabled, never before).
inline std::vector<D810WriteItem> D810BlockItems(const D810WriteTable& table) {
    std::vector<D810WriteItem> items;
    for (const auto& spec : kD810WriteItems) {
        bool enabled = false;
        for (const auto camera : {D810Camera::A, D810Camera::B})
            if (const auto* row = table.Row(spec.item, camera)) enabled |= row->enabled;
        if (spec.scope == D810ItemScope::Adr38 || enabled) items.push_back(spec.item);
    }
    return items;
}

// --- Snapshot lookup ---------------------------------------------------------------------
// The reading of `code` in a stage A snapshot (every write code is also a read code).
inline const D810SettingReading* AvailableD810Reading(const D810SettingsSnapshot& snapshot, std::uint16_t code) {
    const auto* read_spec = FindD810Setting(code);
    const auto* reading = read_spec ? FindD810Reading(snapshot, read_spec->id) : nullptr;
    return reading && reading->available ? reading : nullptr;
}
inline std::string D810Hex(std::uint64_t value, int digits) {
    char text[24]{};
    std::snprintf(text, sizeof text, "0x%0*llx", digits, static_cast<unsigned long long>(value));
    return text;
}

// --- Plans -----------------------------------------------------------------------------
struct D810WriteStep {
    D810Camera camera{};
    D810WriteItem item{};
    std::uint16_t code{};
    std::int64_t before{};
    std::int64_t value{};
};
// A value read while the plan was built. Every one must read the same again
// after confirmation, before anything is sent (ADR-0038 decision 3).
struct D810PlanObservation {
    D810Camera camera{};
    std::uint16_t code{};
    std::int64_t value{};
};
struct D810WritePlan {
    std::uint64_t id{};
    D810PlanSource source{};
    std::vector<D810WriteStep> steps;
    std::vector<D810PlanObservation> observed;
    // Items that differ but are not written, with the reason (copy only).
    std::vector<std::pair<D810WriteItem, std::string>> skipped;
    std::uint16_t iso_auto_code{};
    // Cameras with steps, CAM-B first and CAM-A last: CAM-A is the reference body of the stitch and calibration, so
    // it is changed last, after everything planned for CAM-B has been written and read back. The run opens the
    // cameras in this order.
    std::vector<D810Camera> Cameras() const {
        std::vector<D810Camera> cameras;
        for (const auto camera : {D810Camera::B, D810Camera::A})
            if (std::any_of(steps.begin(), steps.end(), [&](const auto& step) { return step.camera == camera; })) cameras.push_back(camera);
        return cameras;
    }
};
struct D810PlanResult {
    std::optional<D810WritePlan> plan;
    // ASCII tokens: why no plan was made. Empty when `plan` is set.
    std::vector<std::string> reasons;
    bool Ok() const { return plan.has_value(); }
};

namespace d810_plan_detail {
inline std::string Reason(const char* what, const D810WriteItemSpec& spec, D810Camera camera) {
    return std::string(what) + ":" + spec.name + ":" + D810CameraAlias(camera);
}
inline void Observe(std::vector<D810PlanObservation>& observed, D810Camera camera, std::uint16_t code, std::int64_t value) {
    for (const auto& seen : observed)
        if (seen.camera == camera && seen.code == code) return;
    observed.push_back({camera, code, value});
}
// The target body must reach manual mode and ISO auto off before any item that
// needs them: it already reads so, or the plan writes it first.
inline void CheckPreconditions(const D810WritePlan& plan, D810Camera camera, const D810SettingsSnapshot& snapshot, std::vector<std::string>& reasons) {
    bool needs_manual = false, needs_iso_off = false, writes_manual = false, writes_iso_off = false;
    for (const auto& step : plan.steps) {
        if (step.camera != camera) continue;
        const auto pre = D810WriteItemOf(step.item).precondition;
        needs_manual |= pre != D810WritePrecondition::LiveOff;
        needs_iso_off |= pre == D810WritePrecondition::ManualModeIsoAutoOff;
        writes_manual |= step.item == D810WriteItem::ExposureMode;
        writes_iso_off |= step.item == D810WriteItem::IsoAuto;
    }
    const auto* mode = AvailableD810Reading(snapshot, kD810ExposureModeCode);
    const auto* iso_auto = AvailableD810Reading(snapshot, plan.iso_auto_code);
    const std::string alias = D810CameraAlias(camera);
    // ADR-0038 decision 14: when the mode cannot be written, stop and ask for M on the body.
    if (needs_manual && !writes_manual && !(mode && mode->desc.current_value == kD810ExposureModeManual))
        reasons.push_back("exposure_mode_not_manual_set_on_body:" + alias);
    if (needs_iso_off && !writes_iso_off && !(iso_auto && iso_auto->desc.current_value == kD810IsoAutoOff))
        reasons.push_back("iso_auto_not_off:" + alias);
}
} // namespace d810_plan_detail

// The table still lets every step through (checked again before the run).
inline std::string D810PlanTableRefusal(const D810WritePlan& plan, const D810WriteTable& table) {
    if (table.Code(D810WriteItem::IsoAuto) != plan.iso_auto_code) return "code_changed:iso_auto";
    for (const auto& step : plan.steps) {
        if (const auto refusal = D810RowRefusal(table, step.item, step.camera, plan.source)) return refusal;
        if (table.Code(step.item) != step.code) return "code_changed";
    }
    return {};
}
// The shape of a plan, for every kind of plan: the steps of CAM-B come first and those of CAM-A last, the steps of
// one camera follow the item table's order (so no item appears twice for a camera), every step's code is a code of
// its item, and the table still lets every step through. Checked at the end of both plan builders and again at the
// start of a run (the run's time margin counts the steps still to do and relies on this order). Empty when the plan
// is well formed, else an ASCII token.
inline std::string D810PlanProblem(const D810WritePlan& plan, const D810WriteTable& table) {
    const D810WriteStep* previous = nullptr;
    for (const auto& step : plan.steps) {
        const auto* spec = FindD810WriteItemByCode(step.code);
        if (!spec || spec->item != step.item) return "plan_step_code";
        if (previous) {
            if (previous->camera == D810Camera::A && step.camera == D810Camera::B) return "plan_order";
            if (previous->camera == step.camera && static_cast<unsigned>(previous->item) >= static_cast<unsigned>(step.item)) return "plan_order";
        }
        previous = &step;
    }
    return D810PlanTableRefusal(plan, table);
}
// The key to a CAM-A write session (D810WriteGateConfig::cam_a_allowed), an own guard apart from the plan's checks:
// open only for a plan of picked values, under a table that switches picked values on, with a CAM-A firmware version
// named, and when the table enables the row of every CAM-A step of the plan. A switch alone (`chosen_values`) does
// not open it.
inline bool D810CamAKeyOpen(const D810WritePlan& plan, const D810WriteTable& table) {
    if (plan.source != D810PlanSource::Chosen || !table.chosen_values || !D810TextPresent(table.verified_firmware_a)) return false;
    for (const auto& step : plan.steps)
        if (step.camera == D810Camera::A && D810RowRefusal(table, step.item, D810Camera::A, plan.source)) return false;
    return true;
}
// A CAM-A row of the table is enabled: the plan's read then reports how CAM-A's body compared (booleans only).
constexpr bool D810AnyCamARowEnabled(const D810WriteTable& table) {
    for (const auto& row : table.rows)
        if (row.camera == D810Camera::A && row.enabled) return true;
    return false;
}

// The time the write stage of a run may take. The base is the screen's provisional 60 s (ADR-0038 decision 10).
// The run starts a step only when `step_budget` (5 s) per remaining step is left, so a long plan, which also
// spends time on the check before the first send, does not fit in 60 s: a chosen plan with six or more steps
// gets 120 s. Both numbers are provisional (※仮, to be confirmed with the security review like the 60 s); a plan
// that still does not fit stops before sending ("deadline_margin"), which is the safe side. A copy plan keeps the
// base: the product table enables at most five items.
inline constexpr std::size_t kD810LongPlanSteps = 6;
inline constexpr std::chrono::milliseconds kD810LongPlanDeadline{120000};
inline std::chrono::milliseconds D810WriteDeadlineFor(const D810WritePlan& plan, std::chrono::milliseconds base) {
    if (plan.source == D810PlanSource::Chosen && plan.steps.size() >= kD810LongPlanSteps) return std::max(base, kD810LongPlanDeadline);
    return base;
}

// ADR-0038: copy CAM-A's five exposure items to CAM-B, and (ADR-0038 revision, decided by the owner)
// white balance and metering. CAM-A must be in M with ISO auto off and
// a finite, short shutter time. Only items that differ are written. A differing
// item whose row is disabled, or that the operator left out (`only` given and
// the item not in it), is listed in `skipped`; its value is never replaced by
// another one.
//
// A value of CAM-A that a copy cannot carry (white balance Manual, One-push
// Automatic, Color Temperature or Preset, which lean on a measured white or a
// temperature held in the body; or a value the item does not name) is listed in
// `skipped` with its reason whatever CAM-B reads, and CAM-B's value is left as it
// is: the plan for the other items still stands. Equal numbers do not mean equal
// colors for such a value, so it is listed also when CAM-B reads the same number.
inline D810PlanResult BuildD810CopyPlan(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b, const D810WriteTable& table,
    const std::optional<std::vector<D810WriteItem>>& only = std::nullopt) {
    using namespace d810_plan_detail;
    D810PlanResult result;
    auto& reasons = result.reasons;
    if (!table.copy_a_to_b) { reasons.push_back("copy_disabled"); return result; }
    const auto iso_auto_code = table.Code(D810WriteItem::IsoAuto);
    const auto shutter_code = table.Code(D810WriteItem::ShutterSpeed);
    // CAM-A's conditions and CAM-B's ISO auto are read at the confirmed codes,
    // so nothing can be planned before both codes are confirmed.
    if (!iso_auto_code) reasons.push_back("code_unconfirmed:iso_auto");
    if (!shutter_code) reasons.push_back("code_unconfirmed:shutter_speed");
    if (!reasons.empty()) return result;
    const auto* a_mode = AvailableD810Reading(a, kD810ExposureModeCode);
    const auto* a_iso_auto = AvailableD810Reading(a, iso_auto_code);
    const auto* a_shutter = AvailableD810Reading(a, shutter_code);
    if (!a_mode || a_mode->desc.current_value != kD810ExposureModeManual) reasons.push_back("a_not_manual");
    if (!a_iso_auto || a_iso_auto->desc.current_value != kD810IsoAutoOff) reasons.push_back("a_iso_auto_not_off");
    if (!a_shutter) reasons.push_back("a_shutter_unreadable");
    else if (const auto refusal = D810ShutterRefusal(a_shutter->desc.current_value)) reasons.push_back(std::string("a_shutter_") + refusal);
    if (!reasons.empty()) return result;
    D810WritePlan plan;
    plan.source = D810PlanSource::CopyAToB;
    plan.iso_auto_code = iso_auto_code;
    for (const auto& spec : kD810WriteItems) {
        if (spec.scope == D810ItemScope::Adr41) continue;
        const auto code = table.Code(spec.item);
        const auto* from = AvailableD810Reading(a, code);
        const auto* to = AvailableD810Reading(b, code);
        if (spec.scope == D810ItemScope::Adr38Revision && (!from || !to)) {
            // The five exposure items do not wait for white balance or metering: an item this read could not
            // get from either body is left out, and its value on CAM-B stays as it is.
            plan.skipped.emplace_back(spec.item, "unreadable");
            continue;
        }
        if (!from) { reasons.push_back(Reason("unreadable", spec, D810Camera::A)); continue; }
        if (!to) { reasons.push_back(Reason("unreadable", spec, D810Camera::B)); continue; }
        Observe(plan.observed, D810Camera::A, code, from->desc.current_value);
        Observe(plan.observed, D810Camera::B, code, to->desc.current_value);
        const auto value = spec.fixed ? spec.fixed_value : from->desc.current_value;
        // White balance and metering: the value rule is looked at only while the table lets the row through.
        // With the row disabled they behave like the five (listed with the short table reason, and only when
        // the two bodies differ), so a disabled row never shows the long value-based reason.
        const bool value_rule_applies = spec.scope != D810ItemScope::Adr38Revision ||
            D810RowRefusal(table, spec.item, D810Camera::B, plan.source) == nullptr;
        if (const auto refusal = value_rule_applies ? D810CopyValueRefusal(spec, value) : nullptr) {
            plan.skipped.emplace_back(spec.item, refusal);
            continue;
        }
        if (to->desc.current_value == value) continue;
        if (only && std::find(only->begin(), only->end(), spec.item) == only->end()) {
            plan.skipped.emplace_back(spec.item, "left_out_by_operator");
            continue;
        }
        if (const auto refusal = D810RowRefusal(table, spec.item, D810Camera::B, plan.source)) {
            plan.skipped.emplace_back(spec.item, refusal);
            continue;
        }
        if (const auto refusal = D810WriteValueRefusal(spec, to->desc, value)) {
            if (spec.scope == D810ItemScope::Adr38Revision) plan.skipped.emplace_back(spec.item, refusal);
            else reasons.push_back(Reason(refusal, spec, D810Camera::B));
            continue;
        }
        plan.steps.push_back({D810Camera::B, spec.item, code, to->desc.current_value, value});
    }
    CheckPreconditions(plan, D810Camera::B, b, reasons);
    if (const auto problem = D810PlanProblem(plan, table); !problem.empty()) reasons.push_back(problem);
    if (!reasons.empty()) return result;
    result.plan = std::move(plan);
    return result;
}

// The plan still holds against a fresh read: every observation reads the same
// and every step's value is still offered (ADR-0038 decision 3). Empty when it
// holds, else the first difference as a token.
inline std::string D810PlanChange(const D810WritePlan& plan, D810Camera camera, const D810SettingsSnapshot& snapshot) {
    for (const auto& seen : plan.observed) {
        if (seen.camera != camera) continue;
        const auto* reading = AvailableD810Reading(snapshot, seen.code);
        if (!reading) return "unreadable:" + D810Hex(seen.code, 4);
        if (reading->desc.current_value != seen.value) return "changed:" + D810Hex(seen.code, 4);
    }
    for (const auto& step : plan.steps) {
        if (step.camera != camera) continue;
        const auto* reading = AvailableD810Reading(snapshot, step.code);
        if (!reading) return "unreadable:" + D810Hex(step.code, 4);
        if (const auto refusal = D810WriteValueRefusal(D810WriteItemOf(step.item), reading->desc, step.value))
            return std::string(refusal) + ":" + D810Hex(step.code, 4);
    }
    return {};
}
// --- Release of a capture block ----------------------------------------------------
// Both cameras read in full and every block item reads the same on both. An
// item with two candidate codes is compared at the confirmed code; while the
// code is not confirmed, every candidate either side reads must match and at
// least one must be read. A content-dependent value (white balance Preset and
// the like) is "unknown", never a match.
struct D810ReleaseCheck {
    bool released{};
    std::vector<std::string> differences; // item names, "<name>:unreadable" or "<name>:content_dependent"
};
inline D810ReleaseCheck CheckD810Release(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b, const D810WriteTable& table) {
    D810ReleaseCheck check;
    for (const auto item : D810BlockItems(table)) {
        const auto& spec = D810WriteItemOf(item);
        std::vector<std::uint16_t> codes;
        if (const auto confirmed = table.Code(item)) codes.push_back(confirmed);
        else for (std::size_t i = 0; i < spec.code_count; ++i) codes.push_back(spec.codes[i]);
        bool compared = false, same = true, content = false, unreadable = false;
        for (const auto code : codes) {
            const auto* left = AvailableD810Reading(a, code);
            const auto* right = AvailableD810Reading(b, code);
            if (!left && !right && codes.size() > 1) continue;
            if (!left || !right) { unreadable = true; break; }
            compared = true;
            content |= D810WriteValueExcluded(spec, left->desc.current_value) || D810WriteValueExcluded(spec, right->desc.current_value);
            same &= left->desc.current_value == right->desc.current_value;
        }
        if (content) check.differences.push_back(std::string(spec.name) + ":content_dependent");
        else if (unreadable || !compared) check.differences.push_back(std::string(spec.name) + ":unreadable");
        else if (!same) check.differences.push_back(spec.name);
    }
    check.released = check.differences.empty();
    return check;
}
// A value an unfinished operation of the record planned for one item of one camera (the `value` of a step line).
struct D810ExpectedValue {
    D810Camera camera{};
    D810WriteItem item{};
    std::uint16_t code{};
    std::int64_t value{};
};
// Release by the record (ADR-0041 decision 1-7), for a history that holds a chosen plan: every item the unfinished
// operations planned (the last planned value of each camera and item, from the record) reads that value on its own
// camera. CAM-A and CAM-B are not compared with each other. Differences: "<item>:<CAM-x>:unreadable" (not read),
// "<item>:<CAM-x>:content_dependent" (the value leans on content held in the body, white balance Preset and the
// like: unknown, never a match) or "<item>:<CAM-x>:not_planned_value". Nothing planned means nothing is released
// by this check: the caller uses the comparison of the two cameras then.
//
// A planned value is taken by the picture only with the camera in manual mode and ISO auto off (the plan was made
// on that condition, and the copy rule compares both too): for every camera that has an expected value, the
// exposure mode must read M and ISO auto must read Off, else "exposure_mode:<CAM-x>:not_manual" or
// "iso_auto:<CAM-x>:not_off" (":unreadable" when it is not read). ISO auto is read at the table's confirmed code.
inline D810ReleaseCheck CheckD810ReleasePlanned(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b, const std::vector<D810ExpectedValue>& expected,
    const D810WriteTable& table) {
    D810ReleaseCheck check;
    if (expected.empty()) { check.differences.push_back("nothing_planned"); return check; }
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        if (std::none_of(expected.begin(), expected.end(), [&](const auto& entry) { return entry.camera == camera; })) continue;
        const auto& snapshot = camera == D810Camera::A ? a : b;
        const std::string alias = D810CameraAlias(camera);
        const auto* mode = AvailableD810Reading(snapshot, kD810ExposureModeCode);
        if (!mode) check.differences.push_back("exposure_mode:" + alias + ":unreadable");
        else if (mode->desc.current_value != kD810ExposureModeManual) check.differences.push_back("exposure_mode:" + alias + ":not_manual");
        const auto iso_auto_code = table.Code(D810WriteItem::IsoAuto);
        const auto* iso_auto = iso_auto_code ? AvailableD810Reading(snapshot, iso_auto_code) : nullptr;
        if (!iso_auto) check.differences.push_back("iso_auto:" + alias + ":unreadable");
        else if (iso_auto->desc.current_value != kD810IsoAutoOff) check.differences.push_back("iso_auto:" + alias + ":not_off");
    }
    for (const auto& entry : expected) {
        const auto& spec = D810WriteItemOf(entry.item);
        const std::string label = std::string(spec.name) + ":" + D810CameraAlias(entry.camera) + ":";
        const auto* reading = AvailableD810Reading(entry.camera == D810Camera::A ? a : b, entry.code);
        if (!reading) check.differences.push_back(label + "unreadable");
        else if (D810WriteValueExcluded(spec, reading->desc.current_value) || D810WriteValueExcluded(spec, entry.value)) check.differences.push_back(label + "content_dependent");
        else if (reading->desc.current_value != entry.value) check.differences.push_back(label + "not_planned_value");
    }
    check.released = check.differences.empty();
    return check;
}

// --- Operation record (journal) ---------------------------------------------------------
// One line per event, appended and flushed before the next step:
//   a0sw1 op=<16 hex> ev=plan t=<utc> source=copy|chosen steps=<n>
//   a0sw1 op=<16 hex> ev=step i=<n> alias=<CAM-x> item=<name> code=0x.... before=0x... value=0x...
//   a0sw1 op=<16 hex> ev=result i=<n> readback=0x...|none verdict=match|mismatch|unknown
//   a0sw1 op=<16 hex> ev=end t=<utc> verdict=match|mismatch|unknown|not_sent sent=<n> reason=<token>
//   a0sw1 ev=reread t=<utc> verdict=match|mismatch
// No device identity, serial, digest, model, firmware, transfer context or raw
// transfer bytes (ADR-0038 decision 11). `op` is a random operation id.
enum class D810WriteVerdict : std::uint8_t { NotSent, Match, Mismatch, Unknown };
inline const char* D810WriteVerdictToken(D810WriteVerdict verdict) {
    switch (verdict) {
    case D810WriteVerdict::NotSent: return "not_sent";
    case D810WriteVerdict::Match: return "match";
    case D810WriteVerdict::Mismatch: return "mismatch";
    case D810WriteVerdict::Unknown: return "unknown";
    }
    return "unknown";
}
// Why capture is held. Unconfirmed: an operation has no end line (it may have
// sent), or a line cannot be read. Unreadable: the record cannot be read at all.
enum class D810CaptureBlock : std::uint8_t { None, Mismatch, Unknown, Unconfirmed, Unreadable };
inline const char* D810CaptureBlockToken(D810CaptureBlock block) {
    switch (block) {
    case D810CaptureBlock::None: return "none";
    case D810CaptureBlock::Mismatch: return "mismatch";
    case D810CaptureBlock::Unknown: return "unknown";
    case D810CaptureBlock::Unconfirmed: return "unconfirmed";
    case D810CaptureBlock::Unreadable: return "unreadable";
    }
    return "unreadable";
}
// The enum order is the strength order (None weakest, Unreadable strongest):
// StrongerD810Block compares the values. A new value goes where its strength
// is, and the static_assert below is updated with it.
inline D810CaptureBlock StrongerD810Block(D810CaptureBlock left, D810CaptureBlock right) {
    return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
}
static_assert(static_cast<int>(D810CaptureBlock::None) == 0 && static_cast<int>(D810CaptureBlock::Mismatch) == 1 &&
        static_cast<int>(D810CaptureBlock::Unknown) == 2 && static_cast<int>(D810CaptureBlock::Unconfirmed) == 3 &&
        static_cast<int>(D810CaptureBlock::Unreadable) == 4,
    "capture block strength order");
inline constexpr std::string_view kD810JournalPrefix = "a0sw1";
inline std::string D810ValueField(const D810WriteItemSpec& spec, std::int64_t value) {
    const auto bytes = D810ValueBytes(spec.type);
    const auto mask = bytes == 4 ? 0xffffffffULL : (1ULL << (8 * bytes)) - 1;
    return D810Hex(static_cast<std::uint64_t>(value) & mask, static_cast<int>(2 * bytes));
}
inline std::string D810JournalOp(std::uint64_t op) { return std::string(kD810JournalPrefix) + " op=" + D810Hex(op, 16).substr(2); }
inline std::string FormatD810JournalPlan(std::uint64_t op, const std::string& time, const D810WritePlan& plan) {
    return D810JournalOp(op) + " ev=plan t=" + time + " source=" + D810PlanSourceToken(plan.source) + " steps=" + std::to_string(plan.steps.size());
}
inline std::string FormatD810JournalStep(std::uint64_t op, std::size_t index, const D810WriteStep& step) {
    const auto& spec = D810WriteItemOf(step.item);
    return D810JournalOp(op) + " ev=step i=" + std::to_string(index) + " alias=" + D810CameraAlias(step.camera) + " item=" + spec.name +
        " code=" + D810Hex(step.code, 4) + " before=" + D810ValueField(spec, step.before) + " value=" + D810ValueField(spec, step.value);
}
inline std::string FormatD810JournalResult(std::uint64_t op, std::size_t index, const D810WriteStep& step, std::optional<std::int64_t> readback,
    D810WriteVerdict verdict) {
    return D810JournalOp(op) + " ev=result i=" + std::to_string(index) + " readback=" +
        (readback ? D810ValueField(D810WriteItemOf(step.item), *readback) : std::string("none")) + " verdict=" + D810WriteVerdictToken(verdict);
}
inline std::string FormatD810JournalEnd(std::uint64_t op, const std::string& time, D810WriteVerdict verdict, std::size_t sent, const std::string& reason) {
    std::string token = reason.empty() ? std::string("none") : reason;
    std::replace(token.begin(), token.end(), ' ', '_');
    return D810JournalOp(op) + " ev=end t=" + time + " verdict=" + D810WriteVerdictToken(verdict) + " sent=" + std::to_string(sent) + " reason=" + token;
}
inline std::string FormatD810JournalReread(const std::string& time, bool match) {
    return std::string(kD810JournalPrefix) + " ev=reread t=" + time + " verdict=" + (match ? "match" : "mismatch");
}

// --- Reading the record ---------------------------------------------------------------
inline std::vector<std::string_view> SplitD810RecordLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}
inline std::vector<std::string_view> SplitD810Fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < line.size()) {
        const auto end = line.find(' ', start);
        fields.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}
// `key=value`, nothing when the key differs or the value is empty.
inline std::optional<std::string_view> D810FieldValue(std::string_view field, std::string_view key) {
    if (field.size() <= key.size() + 1 || field.substr(0, key.size()) != key || field[key.size()] != '=') return {};
    return field.substr(key.size() + 1);
}
inline std::optional<std::uint32_t> ParseD810Decimal(std::string_view text, std::uint32_t max_value) {
    if (text.empty() || text.size() > 9) return {};
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return {};
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value > max_value) return {};
    return value;
}
// `0x` and exactly `digits` lower-case hexadecimal digits (as D810Hex writes them), nothing otherwise.
inline std::optional<std::uint64_t> ParseD810HexField(std::string_view text, std::size_t digits) {
    if (digits == 0 || digits > 8 || text.size() != 2 + digits || text[0] != '0' || text[1] != 'x') return {};
    std::uint64_t value = 0;
    for (const char c : text.substr(2)) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
        else return {};
        value = (value << 4) | digit;
    }
    return value;
}
// The value of a field written by D810ValueField, as the body reads it (sign-extended for the signed types).
inline std::int64_t D810ValueFromField(D810ValueType type, std::uint64_t raw) {
    switch (type) {
    case D810ValueType::Int8: return static_cast<std::int8_t>(raw);
    case D810ValueType::Int16: return static_cast<std::int16_t>(raw);
    case D810ValueType::Int32: return static_cast<std::int32_t>(raw);
    default: return static_cast<std::int64_t>(raw);
    }
}
// A step line: ev=step i=<n> alias=<CAM-x> item=<name> code=0x.... before=0x... value=0x... Nothing when a field
// cannot be read: an unknown camera or item, a code the item does not have, or a value of the wrong width.
inline std::optional<D810ExpectedValue> ParseD810JournalStep(const std::vector<std::string_view>& fields) {
    if (fields.size() != 9) return {};
    const auto index = D810FieldValue(fields[3], "i");
    const auto alias = D810FieldValue(fields[4], "alias");
    const auto item = D810FieldValue(fields[5], "item");
    const auto code = D810FieldValue(fields[6], "code");
    const auto before = D810FieldValue(fields[7], "before");
    const auto value = D810FieldValue(fields[8], "value");
    if (!index || !ParseD810Decimal(*index, 1000) || !alias || !item || !code || !before || !value) return {};
    D810ExpectedValue out;
    if (*alias == "CAM-A") out.camera = D810Camera::A;
    else if (*alias == "CAM-B") out.camera = D810Camera::B;
    else return {};
    const D810WriteItemSpec* spec = nullptr;
    for (const auto& candidate : kD810WriteItems)
        if (*item == candidate.name) spec = &candidate;
    if (!spec) return {};
    const auto code_value = ParseD810HexField(*code, 4);
    if (!code_value || !D810WriteItemHasCode(*spec, static_cast<std::uint32_t>(*code_value))) return {};
    const auto digits = 2 * D810ValueBytes(spec->type);
    const auto before_value = ParseD810HexField(*before, digits);
    const auto raw = ParseD810HexField(*value, digits);
    if (!before_value || !raw) return {};
    out.item = spec->item;
    out.code = static_cast<std::uint16_t>(*code_value);
    out.value = D810ValueFromField(spec->type, *raw);
    return out;
}
struct D810JournalState {
    D810CaptureBlock block{D810CaptureBlock::None};
    std::size_t operations{};
    std::size_t malformed_lines{};
    // The history since the capture was last free of a block holds a chosen plan (a plan of values the operator
    // picked, not a copy), so that a block is released by the planned values and not by comparing the two cameras.
    bool chosen_unresolved{};
    // The last planned value of each camera and item over that history, from the step lines. Steps of an
    // operation that sent nothing (end verdict not_sent) are not in it: nothing was written for them.
    std::vector<D810ExpectedValue> expected;
    // Part of that history cannot be trusted: a line of it cannot be read, or an operation that may have sent holds
    // other step lines than its plan line announced (a step line missing, repeated or out of order). The planned
    // values then miss steps that may have been sent, so a release does not rest on them: it compares the two
    // cameras (the rule of a history of copies), as for a history that gave no planned value.
    bool history_damaged{};
};
// The capture block the record implies. Lines that cannot be read hold capture
// as Unconfirmed until a later reread line releases it; an operation without
// its end line does the same. A later match never releases an earlier block;
// only a reread line with verdict=match does.
//
// The history for a release: a plan line that finds no block starts it afresh (no old expectation stays for the
// next operation), a reread line with verdict=match ends it, and while a block stands every operation adds its
// planned values (a later value of the same camera and item replaces the earlier one). The record is the only
// source: nothing is kept in memory, so a restart sees the same history.
//
// history_damaged is set by a line that cannot be read, anywhere from the last start of the history (a start
// clears it, as it clears the rest of the history), and by an operation that may have sent (any end other than
// `not_sent sent=0`, or none) whose step lines are not the steps= of its plan line, numbered i=0, 1, 2 ... in order.
inline D810JournalState ParseD810Journal(std::string_view text) {
    D810JournalState state;
    std::optional<std::string> open_op;
    bool open_chosen = false;
    std::vector<D810ExpectedValue> op_steps;
    std::optional<std::size_t> op_declared; // steps= of the open operation's plan line
    bool op_in_order = true;                // its step lines came as i=0, 1, 2 ...
    const auto hold = [&](D810CaptureBlock block) { state.block = StrongerD810Block(state.block, block); };
    // The open operation's planned values join the history (it may have sent), or are dropped when it sent nothing.
    const auto commit = [&] {
        if (!op_in_order || op_declared != op_steps.size()) state.history_damaged = true;
        if (open_chosen) state.chosen_unresolved = true;
        for (const auto& step : op_steps) {
            const auto same = std::find_if(state.expected.begin(), state.expected.end(),
                [&](const auto& seen) { return seen.camera == step.camera && seen.item == step.item; });
            if (same != state.expected.end()) *same = step;
            else state.expected.push_back(step);
        }
        op_steps.clear();
        open_chosen = false;
        op_declared.reset();
        op_in_order = true;
    };
    const auto drop = [&] { op_steps.clear(); open_chosen = false; op_declared.reset(); op_in_order = true; };
    const auto close_open = [&] { if (open_op) { hold(D810CaptureBlock::Unconfirmed); open_op.reset(); commit(); } };
    const auto hex16 = [](std::string_view text) {
        return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    for (const auto line : SplitD810RecordLines(text)) {
        const auto fields = SplitD810Fields(line);
        const auto malformed = [&] { ++state.malformed_lines; state.history_damaged = true; close_open(); hold(D810CaptureBlock::Unconfirmed); };
        if (fields.size() < 3 || fields[0] != kD810JournalPrefix) { malformed(); continue; }
        if (fields[1] == "ev=reread") {
            const auto verdict = fields.size() == 4 ? D810FieldValue(fields[3], "verdict") : std::nullopt;
            if (!verdict || !D810FieldValue(fields[2], "t") || (*verdict != "match" && *verdict != "mismatch")) { malformed(); continue; }
            // An operation without its end line counts as unconfirmed first, so
            // a mismatching reread leaves a mismatch, never no block.
            close_open();
            if (*verdict == "match") {
                state.block = D810CaptureBlock::None;
                state.chosen_unresolved = false;
                state.history_damaged = false;
                state.expected.clear();
            } else if (state.block != D810CaptureBlock::None) state.block = D810CaptureBlock::Mismatch;
            continue;
        }
        const auto op = D810FieldValue(fields[1], "op");
        const auto event = D810FieldValue(fields[2], "ev");
        if (!op || !hex16(*op) || !event) { malformed(); continue; }
        if (*event == "plan") {
            const auto source = fields.size() == 6 ? D810FieldValue(fields[4], "source") : std::nullopt;
            if (!source || (*source != "copy" && *source != "chosen")) { malformed(); continue; }
            close_open();
            if (state.block == D810CaptureBlock::None) {
                state.chosen_unresolved = false;
                state.history_damaged = false;
                state.expected.clear();
            }
            open_op = std::string(*op);
            open_chosen = *source == "chosen";
            op_steps.clear();
            op_in_order = true;
            op_declared.reset();
            if (const auto declared = D810FieldValue(fields[5], "steps")) {
                if (const auto count = ParseD810Decimal(*declared, 1000)) op_declared = static_cast<std::size_t>(*count);
            }
            ++state.operations;
        } else if (*event == "step" || *event == "result") {
            if (!open_op || *open_op != *op || fields.size() != (*event == "step" ? 9u : 6u)) { malformed(); continue; }
            if (*event == "step") {
                const auto step = ParseD810JournalStep(fields);
                if (!step) { malformed(); continue; }
                const auto index = ParseD810Decimal(*D810FieldValue(fields[3], "i"), 1000);
                if (!index || *index != op_steps.size()) op_in_order = false;
                op_steps.push_back(*step);
            }
        } else if (*event == "end") {
            const auto verdict = fields.size() == 7 ? D810FieldValue(fields[4], "verdict") : std::nullopt;
            const auto sent = fields.size() == 7 ? D810FieldValue(fields[5], "sent") : std::nullopt;
            const auto count = sent ? ParseD810Decimal(*sent, 1000) : std::nullopt;
            if (!open_op || *open_op != *op || !verdict || !count) { malformed(); continue; }
            open_op.reset();
            if (*verdict == "mismatch") { hold(D810CaptureBlock::Mismatch); commit(); }
            else if (*verdict == "unknown") { hold(D810CaptureBlock::Unknown); commit(); }
            else if (*verdict == "not_sent") {
                if (*count != 0) { malformed(); commit(); }
                else drop();
            } else if (*verdict == "match") commit();
            else { malformed(); commit(); }
        } else malformed();
    }
    close_open();
    return state;
}
} // namespace a0::phase0::experimental
