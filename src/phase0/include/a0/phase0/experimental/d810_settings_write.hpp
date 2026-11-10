#pragma once
// Camera settings write: item table, the fixed write table, write plans and the
// operation record format (#416 stage B, ADR-0038; the ADR-0041 extension is
// present as code but disabled by the fixed table).
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
    // One of the five ADR-0038 items. The others belong to ADR-0041.
    bool accepted_scope;
    D810WritePrecondition precondition;
    // Values that depend on other content of the body (white balance Manual,
    // Color Temperature, Preset): never written, and "unknown" when compared.
    std::array<std::int64_t, 3> excluded;
    std::size_t excluded_count;
};
// Table order is the write order (ADR-0038 decision 5). Picture Control and
// Active D-Lighting are absent on purpose: their scalar type is not settled.
inline constexpr std::array<D810WriteItemSpec, kD810WriteItemCount> kD810WriteItems{{
    {D810WriteItem::ExposureMode, "exposure_mode", D810ValueType::Uint16, {0x500e, 0}, 1, true, 1, true, D810WritePrecondition::LiveOff, {}, 0},
    {D810WriteItem::IsoAuto, "iso_auto", D810ValueType::Uint8, {0xd054, 0xd16a}, 2, true, 0, true, D810WritePrecondition::ManualMode, {}, 0},
    {D810WriteItem::Iso, "iso", D810ValueType::Uint16, {0x500f, 0}, 1, false, 0, true, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0},
    {D810WriteItem::ShutterSpeed, "shutter_speed", D810ValueType::Uint32, {0x500d, 0xd100}, 2, false, 0, true, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0},
    {D810WriteItem::Aperture, "aperture", D810ValueType::Uint16, {0x5007, 0}, 1, false, 0, true, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0},
    {D810WriteItem::WhiteBalance, "white_balance", D810ValueType::Uint16, {0x5005, 0}, 1, false, 0, false, D810WritePrecondition::ManualModeIsoAutoOff, {1, 0x8012, 0x8013}, 3},
    {D810WriteItem::Metering, "metering", D810ValueType::Uint16, {0x500b, 0}, 1, false, 0, false, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0},
    {D810WriteItem::ExposureCompensation, "exposure_compensation", D810ValueType::Int16, {0x5010, 0}, 1, false, 0, false, D810WritePrecondition::ManualModeIsoAutoOff, {}, 0},
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
    if (D810WriteValueExcluded(spec, value)) return "value_content_dependent";
    if (spec.item == D810WriteItem::ShutterSpeed) {
        if (const auto refusal = D810ShutterRefusal(value)) return refusal;
    }
    if (!D810ValueAdvertised(desc, value)) return "value_not_advertised";
    return nullptr;
}

// --- The fixed write table ----------------------------------------------------------
struct D810WriteRow {
    D810WriteItem item;
    D810Camera camera;
    // The confirmed code, or 0 while it is not confirmed (then the row cannot be enabled).
    std::uint16_t code;
    bool enabled;
};
struct D810WriteTable {
    std::array<D810WriteRow, 2 * kD810WriteItemCount> rows;
    // ADR-0038: copy CAM-A's five exposure items to CAM-B.
    bool copy_a_to_b;
    // ADR-0041 (Proposed): values chosen by the operator.
    bool chosen_values;
    // The camera model and firmware version the rows were confirmed on. A body
    // that reports anything else is not written; nullptr means none confirmed.
    const char* verified_model;
    const char* verified_firmware;
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
        }
    }
    for (const auto camera : {D810Camera::A, D810Camera::B})
        for (const auto& spec : kD810WriteItems)
            if (!table.Row(spec.item, camera)) return "missing_row";
    return nullptr;
}
// What the product table may hold until ADR-0041 is accepted: rows for CAM-B
// and the five ADR-0038 items only, no chosen values. ADR-0038 invariant 2
// (CAM-A is never written) holds at compile time.
constexpr const char* D810ProductTableProblem(const D810WriteTable& table) {
    if (const auto problem = D810WriteTableProblem(table)) return problem;
    if (table.chosen_values) return "chosen_values_before_adr0041";
    for (const auto& row : table.rows) {
        if (!row.enabled) continue;
        if (row.camera == D810Camera::A) return "cam_a_row_enabled";
        if (!D810WriteItemOf(row.item).accepted_scope) return "extra_item_enabled";
    }
    return nullptr;
}
constexpr std::size_t D810EnabledRowCount(const D810WriteTable& table) {
    std::size_t count = 0;
    for (const auto& row : table.rows) count += row.enabled;
    return count;
}
// The product table. ISO auto (0xD054 or 0xD16A) and shutter speed (0x500D or
// 0xD100) are not confirmed on a body yet, so every row is disabled and the
// default build writes nothing. Enabling rows, confirming codes and naming the
// verified firmware is a reviewed commit to this table after the stage A read
// on the real bodies.
inline constexpr D810WriteTable kD810ProductWriteTable = [] {
    auto table = D810DisabledWriteTable();
    table.copy_a_to_b = true;
    table.verified_model = "D810";
    return table;
}();
static_assert(D810ProductTableProblem(kD810ProductWriteTable) == nullptr, "the product write table is outside ADR-0038");
static_assert(D810EnabledRowCount(kD810ProductWriteTable) == 0, "no row is enabled before the codes are confirmed on a body");

enum class D810PlanSource : std::uint8_t { CopyAToB, Chosen };
inline const char* D810PlanSourceToken(D810PlanSource source) { return source == D810PlanSource::CopyAToB ? "copy" : "chosen"; }
// Why the table does not let `item` be written to `camera` by a plan of `source`.
inline const char* D810RowRefusal(const D810WriteTable& table, D810WriteItem item, D810Camera camera, D810PlanSource source) {
    const auto& spec = D810WriteItemOf(item);
    if (source == D810PlanSource::CopyAToB) {
        if (!table.copy_a_to_b) return "copy_disabled";
        if (camera != D810Camera::B) return "copy_targets_cam_b_only";
        if (!spec.accepted_scope) return "copy_covers_five_items_only";
    } else if (!table.chosen_values) return "chosen_values_disabled";
    const auto* row = table.Row(item, camera);
    if (!row || !row->enabled) return "row_disabled";
    if (row->code == 0) return "code_unconfirmed";
    return nullptr;
}
// A body may be written only when it reports the model and firmware the table names.
inline bool D810BodyVerified(const D810WriteTable& table, std::string_view model, std::string_view firmware) {
    return table.verified_model && table.verified_firmware && model == table.verified_model && firmware == table.verified_firmware;
}
// Items whose equality on both cameras releases a capture block: the five, and
// every additional item with an enabled row.
inline std::vector<D810WriteItem> D810BlockItems(const D810WriteTable& table) {
    std::vector<D810WriteItem> items;
    for (const auto& spec : kD810WriteItems) {
        bool enabled = false;
        for (const auto camera : {D810Camera::A, D810Camera::B})
            if (const auto* row = table.Row(spec.item, camera)) enabled |= row->enabled;
        if (spec.accepted_scope || enabled) items.push_back(spec.item);
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
    // Cameras with steps, CAM-A first.
    std::vector<D810Camera> Cameras() const {
        std::vector<D810Camera> cameras;
        for (const auto camera : {D810Camera::A, D810Camera::B})
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
enum class D810WriteTarget : std::uint8_t { A, B, Both };
struct D810WriteChoice {
    D810WriteItem item{};
    D810WriteTarget target{};
    std::int64_t value{};
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

// ADR-0038: copy CAM-A's five exposure items to CAM-B. CAM-A must be in M with
// ISO auto off and a finite, short shutter time. Only items that differ are
// written. A differing item whose row is disabled, or that the operator left
// out (`only` given and the item not in it), is listed in `skipped`; its value
// is never replaced by another one.
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
        if (!spec.accepted_scope) continue;
        const auto code = table.Code(spec.item);
        const auto* from = AvailableD810Reading(a, code);
        const auto* to = AvailableD810Reading(b, code);
        if (!from) { reasons.push_back(Reason("unreadable", spec, D810Camera::A)); continue; }
        if (!to) { reasons.push_back(Reason("unreadable", spec, D810Camera::B)); continue; }
        Observe(plan.observed, D810Camera::A, code, from->desc.current_value);
        Observe(plan.observed, D810Camera::B, code, to->desc.current_value);
        const auto value = spec.fixed ? spec.fixed_value : from->desc.current_value;
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
            reasons.push_back(Reason(refusal, spec, D810Camera::B));
            continue;
        }
        plan.steps.push_back({D810Camera::B, spec.item, code, to->desc.current_value, value});
    }
    CheckPreconditions(plan, D810Camera::B, b, reasons);
    if (!reasons.empty()) return result;
    result.plan = std::move(plan);
    return result;
}

// ADR-0041 (Proposed, disabled in the product table): values chosen by the
// operator. The first version writes the same value to both cameras only
// (target Both); the value must be offered by both bodies. Any invalid choice
// refuses the whole plan.
inline D810PlanResult BuildD810ChosenPlan(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b,
    const std::vector<D810WriteChoice>& choices, const D810WriteTable& table) {
    using namespace d810_plan_detail;
    D810PlanResult result;
    auto& reasons = result.reasons;
    if (!table.chosen_values) { reasons.push_back("chosen_values_disabled"); return result; }
    if (choices.empty()) { reasons.push_back("no_choice"); return result; }
    const auto iso_auto_code = table.Code(D810WriteItem::IsoAuto);
    if (!iso_auto_code) { reasons.push_back("code_unconfirmed:iso_auto"); return result; }
    D810WritePlan plan;
    plan.source = D810PlanSource::Chosen;
    plan.iso_auto_code = iso_auto_code;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto& spec = D810WriteItemOf(choices[i].item);
        if (choices[i].target != D810WriteTarget::Both) reasons.push_back(Reason("both_cameras_only", spec, D810Camera::A));
        for (std::size_t j = 0; j < i; ++j)
            if (choices[j].item == choices[i].item) reasons.push_back(Reason("duplicate_choice", spec, D810Camera::A));
    }
    if (!reasons.empty()) return result;
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        const auto& snapshot = camera == D810Camera::A ? a : b;
        for (const auto& spec : kD810WriteItems) {
            const auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto& c) { return c.item == spec.item; });
            if (choice == choices.end()) continue;
            if (const auto refusal = D810RowRefusal(table, spec.item, camera, plan.source)) { reasons.push_back(Reason(refusal, spec, camera)); continue; }
            const auto code = table.Code(spec.item);
            const auto* reading = AvailableD810Reading(snapshot, code);
            if (!reading) { reasons.push_back(Reason("unreadable", spec, camera)); continue; }
            Observe(plan.observed, camera, code, reading->desc.current_value);
            if (const auto refusal = D810WriteValueRefusal(spec, reading->desc, choice->value)) { reasons.push_back(Reason(refusal, spec, camera)); continue; }
            if (reading->desc.current_value != choice->value) plan.steps.push_back({camera, spec.item, code, reading->desc.current_value, choice->value});
        }
    }
    for (const auto camera : {D810Camera::A, D810Camera::B}) {
        const auto& snapshot = camera == D810Camera::A ? a : b;
        for (const auto code : {kD810ExposureModeCode, iso_auto_code})
            if (const auto* reading = AvailableD810Reading(snapshot, code)) Observe(plan.observed, camera, code, reading->desc.current_value);
        CheckPreconditions(plan, camera, snapshot, reasons);
    }
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
// The table still lets every step through (checked again before the run).
inline std::string D810PlanTableRefusal(const D810WritePlan& plan, const D810WriteTable& table) {
    if (table.Code(D810WriteItem::IsoAuto) != plan.iso_auto_code) return "code_changed:iso_auto";
    for (const auto& step : plan.steps) {
        if (const auto refusal = D810RowRefusal(table, step.item, step.camera, plan.source)) return refusal;
        if (table.Code(step.item) != step.code) return "code_changed";
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
        bool compared = false, same = true, content = false;
        for (const auto code : codes) {
            const auto* left = AvailableD810Reading(a, code);
            const auto* right = AvailableD810Reading(b, code);
            if (!left && !right && codes.size() > 1) continue;
            if (!left || !right) { same = false; break; }
            compared = true;
            content |= D810WriteValueExcluded(spec, left->desc.current_value) || D810WriteValueExcluded(spec, right->desc.current_value);
            same &= left->desc.current_value == right->desc.current_value;
        }
        if (content) check.differences.push_back(std::string(spec.name) + ":content_dependent");
        else if (!compared && same) check.differences.push_back(std::string(spec.name) + ":unreadable");
        else if (!same) check.differences.push_back(spec.name);
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
inline D810CaptureBlock StrongerD810Block(D810CaptureBlock left, D810CaptureBlock right) {
    return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
}
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
struct D810JournalState {
    D810CaptureBlock block{D810CaptureBlock::None};
    std::size_t operations{};
    std::size_t malformed_lines{};
};
// The capture block the record implies. Lines that cannot be read hold capture
// as Unconfirmed until a later reread line releases it; an operation without
// its end line does the same. A later match never releases an earlier block;
// only a reread line with verdict=match does.
inline D810JournalState ParseD810Journal(std::string_view text) {
    D810JournalState state;
    std::optional<std::string> open_op;
    const auto hold = [&](D810CaptureBlock block) { state.block = StrongerD810Block(state.block, block); };
    const auto close_open = [&] { if (open_op) { hold(D810CaptureBlock::Unconfirmed); open_op.reset(); } };
    const auto hex16 = [](std::string_view text) {
        return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    for (const auto line : SplitD810RecordLines(text)) {
        const auto fields = SplitD810Fields(line);
        const auto malformed = [&] { ++state.malformed_lines; close_open(); hold(D810CaptureBlock::Unconfirmed); };
        if (fields.size() < 3 || fields[0] != kD810JournalPrefix) { malformed(); continue; }
        if (fields[1] == "ev=reread") {
            const auto verdict = fields.size() == 4 ? D810FieldValue(fields[3], "verdict") : std::nullopt;
            if (!verdict || !D810FieldValue(fields[2], "t") || (*verdict != "match" && *verdict != "mismatch")) { malformed(); continue; }
            // An operation without its end line counts as unconfirmed first, so
            // a mismatching reread leaves a mismatch, never no block.
            close_open();
            if (*verdict == "match") state.block = D810CaptureBlock::None;
            else if (state.block != D810CaptureBlock::None) state.block = D810CaptureBlock::Mismatch;
            continue;
        }
        const auto op = D810FieldValue(fields[1], "op");
        const auto event = D810FieldValue(fields[2], "ev");
        if (!op || !hex16(*op) || !event) { malformed(); continue; }
        if (*event == "plan") {
            if (fields.size() != 6) { malformed(); continue; }
            close_open();
            open_op = std::string(*op);
            ++state.operations;
        } else if (*event == "step" || *event == "result") {
            if (!open_op || *open_op != *op || fields.size() != (*event == "step" ? 9u : 6u)) { malformed(); continue; }
        } else if (*event == "end") {
            const auto verdict = fields.size() == 7 ? D810FieldValue(fields[4], "verdict") : std::nullopt;
            const auto sent = fields.size() == 7 ? D810FieldValue(fields[5], "sent") : std::nullopt;
            const auto count = sent ? ParseD810Decimal(*sent, 1000) : std::nullopt;
            if (!open_op || *open_op != *op || !verdict || !count) { malformed(); continue; }
            open_op.reset();
            if (*verdict == "mismatch") hold(D810CaptureBlock::Mismatch);
            else if (*verdict == "unknown") hold(D810CaptureBlock::Unknown);
            else if (*verdict == "not_sent") { if (*count != 0) malformed(); }
            else if (*verdict != "match") malformed();
        } else malformed();
    }
    close_open();
    return state;
}
} // namespace a0::phase0::experimental
