#pragma once
// Values chosen per camera for the camera settings panel (#416, ADR-0041 draft).
//
// Pure data and pure functions only: no Windows headers, no device access, no file I/O, and nothing here can
// send a command. It builds on the item table and the write refusals of d810_settings_write.hpp but carries no
// gate, session, plan or journal. Nothing in this header is wired to a plan, a gate or an execution path.
//
// What it provides:
//   1. The items an operator may pick a value for, and the band and the names a pickable value must satisfy.
//   2. The list of pickable values of one item on one body, built from the property description the body
//      returned (an enumeration or a range), with the reason each other advertised value was dropped.
//   3. Display names of the values.
//   4. One panel cell per camera and item, and the draft of the operator's picks (values picked, or "same as
//      CAM-A"), from which the number of changes and the (camera, item, value) list are derived.
//
// The Japanese display names are UTF-8: build with /utf-8.
//
// Only targets built with A0_ENABLE_WPD_SETTINGS_WRITE (the CMake option A0_BUILD_WPD_SETTINGS_WRITE, OFF by
// default, and the contract tests) can include this header. It holds no write code, but it includes the item
// table, which carries the operation record's prefix, and the product binary check
// (preview_settings_write_binary_contracts) must keep finding none of those strings in an OFF build.
#if !defined(A0_ENABLE_WPD_SETTINGS_WRITE)
#error "d810_settings_choice.hpp is only for targets built with A0_BUILD_WPD_SETTINGS_WRITE"
#endif
#include "a0/phase0/experimental/d810_settings_write.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

static_assert(sizeof("あ") == 4, "d810_settings_choice.hpp holds UTF-8 text: compile with /utf-8");

namespace a0::phase0::experimental {
// --- Items ----------------------------------------------------------------------------------------------
// The five items a value may be picked for, in the order of the item table (which is the write order).
// Exposure mode, ISO auto control and exposure compensation are not pickable.
enum class D810ChoiceItem : std::uint8_t { Iso, ShutterSpeed, Aperture, WhiteBalance, Metering };
inline constexpr std::size_t kD810ChoiceItemCount = 5;
inline constexpr std::array<D810ChoiceItem, kD810ChoiceItemCount> kD810ChoiceItems{{
    D810ChoiceItem::Iso, D810ChoiceItem::ShutterSpeed, D810ChoiceItem::Aperture, D810ChoiceItem::WhiteBalance, D810ChoiceItem::Metering}};
inline constexpr std::size_t kD810CameraCount = 2;

constexpr D810WriteItem D810ChoiceWriteItem(D810ChoiceItem item) {
    switch (item) {
    case D810ChoiceItem::Iso: return D810WriteItem::Iso;
    case D810ChoiceItem::ShutterSpeed: return D810WriteItem::ShutterSpeed;
    case D810ChoiceItem::Aperture: return D810WriteItem::Aperture;
    case D810ChoiceItem::WhiteBalance: return D810WriteItem::WhiteBalance;
    case D810ChoiceItem::Metering: return D810WriteItem::Metering;
    }
    return D810WriteItem::Iso;
}
// The stage A reading the item is shown from (the same codes the write table confirms).
constexpr D810SettingId D810ChoiceSettingId(D810ChoiceItem item) {
    switch (item) {
    case D810ChoiceItem::Iso: return D810SettingId::Iso;
    case D810ChoiceItem::ShutterSpeed: return D810SettingId::ShutterSpeed;
    case D810ChoiceItem::Aperture: return D810SettingId::Aperture;
    case D810ChoiceItem::WhiteBalance: return D810SettingId::WhiteBalance;
    case D810ChoiceItem::Metering: return D810SettingId::Metering;
    }
    return D810SettingId::Iso;
}
constexpr std::optional<D810ChoiceItem> D810ChoiceItemOf(D810WriteItem item) {
    switch (item) {
    case D810WriteItem::Iso: return D810ChoiceItem::Iso;
    case D810WriteItem::ShutterSpeed: return D810ChoiceItem::ShutterSpeed;
    case D810WriteItem::Aperture: return D810ChoiceItem::Aperture;
    case D810WriteItem::WhiteBalance: return D810ChoiceItem::WhiteBalance;
    case D810WriteItem::Metering: return D810ChoiceItem::Metering;
    case D810WriteItem::ExposureMode:
    case D810WriteItem::IsoAuto:
    case D810WriteItem::ExposureCompensation: break;
    }
    return std::nullopt;
}
// nullptr for a pickable item, "item_not_choosable" for the others.
constexpr const char* D810ChoiceItemRefusal(D810WriteItem item) { return D810ChoiceItemOf(item) ? nullptr : "item_not_choosable"; }

constexpr bool D810ChoiceItemsFollowTable() {
    std::size_t last = 0;
    bool first = true;
    for (const auto item : kD810ChoiceItems) {
        const auto index = static_cast<std::size_t>(D810ChoiceWriteItem(item));
        if (!first && index <= last) return false;
        if (kD810WriteItems[index].item != D810ChoiceWriteItem(item)) return false;
        if (D810ChoiceItemOf(D810ChoiceWriteItem(item)) != item) return false;
        last = index;
        first = false;
    }
    return true;
}
static_assert(D810ChoiceItemsFollowTable(), "the pickable items follow the order of the item table");

// --- Band ------------------------------------------------------------------------------------------------
// The ISO values a panel offers. Provisional (※仮): standard range only; the Lo and Hi extended values are
// not offered, and what the numbers of the body mean is to be confirmed against a real read before these are
// fixed. Compile-time constants: no run-time input changes them. The shutter speed ceiling is the existing
// kD810MaxWritableExposureUnits (1 s).
inline constexpr std::int64_t kD810ChoiceIsoMin = 64;
inline constexpr std::int64_t kD810ChoiceIsoMax = 12800;
// The ceiling of the band: widening it (the ISO extension values, an exposure over 1 s) is a decision and a reviewed
// edit of this assert, never a side effect of changing a constant.
static_assert(kD810ChoiceIsoMin >= 64 && kD810ChoiceIsoMax <= 12800 && kD810MaxWritableExposureUnits <= 10000,
    "the band of picked values stays inside the standard ISO range and 1 s");

constexpr bool D810ChoiceBand(D810ChoiceItem item, std::int64_t value) {
    switch (item) {
    case D810ChoiceItem::Iso: return value >= kD810ChoiceIsoMin && value <= kD810ChoiceIsoMax;
    case D810ChoiceItem::ShutterSpeed: return value >= 1 && value <= kD810MaxWritableExposureUnits;
    case D810ChoiceItem::Aperture:
    case D810ChoiceItem::WhiteBalance:
    case D810ChoiceItem::Metering: return true;
    }
    return false;
}

// --- Names -----------------------------------------------------------------------------------------------
// A value is "named" when its meaning is known: ISO, shutter speed and aperture are numbers; white balance and
// metering need a name. Metering 1 has no name in the public source, so it is not offered (the copy list of
// the item table is not changed by this).
struct D810ChoiceNameEntry {
    std::int64_t value;
    const char* text;
};
// ※未確認: the Japanese names are to be checked against the vendor tool's wording. Until then every name is
// shown with its raw value (kD810ChoiceNamesConfirmed).
inline constexpr std::array<D810ChoiceNameEntry, 7> kD810ChoiceWhiteBalanceNames{{
    {2, "オート"}, {4, "晴天"}, {5, "蛍光灯"}, {6, "白熱電球"}, {7, "フラッシュ"}, {0x8010, "曇天"}, {0x8011, "晴天日陰"}}};
inline constexpr std::array<D810ChoiceNameEntry, 3> kD810ChoiceMeteringNames{{{2, "中央重点"}, {3, "マルチパターン"}, {4, "スポット"}}};
inline constexpr bool kD810ChoiceNamesConfirmed = false;

namespace d810_choice_detail {
template <std::size_t N>
constexpr const char* FindName(const std::array<D810ChoiceNameEntry, N>& names, std::int64_t value) {
    for (const auto& entry : names)
        if (entry.value == value) return entry.text;
    return nullptr;
}
} // namespace d810_choice_detail

constexpr bool D810ChoiceNamed(D810ChoiceItem item, std::int64_t value) {
    switch (item) {
    case D810ChoiceItem::Iso:
    case D810ChoiceItem::ShutterSpeed:
    case D810ChoiceItem::Aperture: return value > 0;
    case D810ChoiceItem::WhiteBalance: return d810_choice_detail::FindName(kD810ChoiceWhiteBalanceNames, value) != nullptr;
    case D810ChoiceItem::Metering: return d810_choice_detail::FindName(kD810ChoiceMeteringNames, value) != nullptr;
    }
    return false;
}

// Why `value` cannot be picked for `item` on a body that returned `desc`, or nullptr when it can. The first
// check is the refusal the plan and the gate use, so a value the panel offers is a value that can be written.
// The other two narrow it: "value_not_named" and "value_out_of_band".
inline const char* D810ChoosableRefusal(D810ChoiceItem item, const D810PropDesc& desc, std::int64_t value) {
    if (const auto refusal = D810WriteValueRefusal(D810WriteItemOf(D810ChoiceWriteItem(item)), desc, value)) return refusal;
    if (!D810ChoiceNamed(item, value)) return "value_not_named";
    if (!D810ChoiceBand(item, value)) return "value_out_of_band";
    return nullptr;
}
inline const char* D810ChoosableRefusal(D810WriteItem item, const D810PropDesc& desc, std::int64_t value) {
    if (const auto refusal = D810ChoiceItemRefusal(item)) return refusal;
    return D810ChoosableRefusal(*D810ChoiceItemOf(item), desc, value);
}

// --- Values the body advertised ---------------------------------------------------------------------------
struct D810AdvertisedValues {
    std::vector<std::int64_t> values;
    // A range with more candidates than kD810EnumMaxValues: nothing is listed.
    bool too_many{};
};
// An enumeration as it is, in the body's order. A range from its minimum in steps. No form, or a range
// without a positive step, advertises nothing.
inline D810AdvertisedValues D810AdvertisedValuesOf(const D810PropDesc& desc) {
    D810AdvertisedValues out;
    if (desc.form_flag == 2) {
        out.values = desc.values;
    } else if (desc.form_flag == 1 && desc.range_step > 0 && desc.range_min <= desc.range_max) {
        const auto count = (desc.range_max - desc.range_min) / desc.range_step + 1;
        if (count > static_cast<std::int64_t>(kD810EnumMaxValues)) {
            out.too_many = true;
        } else {
            for (std::int64_t i = 0; i < count; ++i) out.values.push_back(desc.range_min + i * desc.range_step);
        }
    }
    return out;
}

// --- Display names ----------------------------------------------------------------------------------------
// The name of the value without a raw suffix. ISO: the number. Shutter speed: the fraction (1/60) or "1 秒";
// no unit letter. Aperture: f/ and the number. White balance and metering: the name; a value without a name
// reads as its raw value.
inline std::string D810ChoiceName(D810ChoiceItem item, std::int64_t value) {
    const auto& spec = D810SettingOf(D810ChoiceSettingId(item));
    switch (item) {
    case D810ChoiceItem::Iso:
        return value > 0 ? std::to_string(value) : D810RawText(spec, value);
    case D810ChoiceItem::ShutterSpeed: {
        auto text = FormatD810ShutterSpeed(spec, value);
        if (text.size() > 2 && text.compare(text.size() - 2, 2, " s") == 0) {
            text.resize(text.size() - 2);
            if (text.find('/') == std::string::npos) text += " 秒";
        }
        return text;
    }
    case D810ChoiceItem::Aperture:
        return value > 0 ? "f/" + D810Printf("%g", static_cast<double>(value) / 100.0) : D810RawText(spec, value);
    case D810ChoiceItem::WhiteBalance:
        if (const auto* name = d810_choice_detail::FindName(kD810ChoiceWhiteBalanceNames, value)) return name;
        return D810RawText(spec, value);
    case D810ChoiceItem::Metering:
        if (const auto* name = d810_choice_detail::FindName(kD810ChoiceMeteringNames, value)) return name;
        return D810RawText(spec, value);
    }
    return D810RawText(spec, value);
}
// The label shown in a list: the name, with the raw value in parentheses while the names are unconfirmed
// ("晴天 (0x0004)").
inline std::string D810ChoiceLabel(D810ChoiceItem item, std::int64_t value) {
    auto text = D810ChoiceName(item, value);
    const bool by_name = item == D810ChoiceItem::WhiteBalance || item == D810ChoiceItem::Metering;
    if (by_name && !kD810ChoiceNamesConfirmed && D810ChoiceNamed(item, value))
        text += " (" + D810HexValue(D810SettingOf(D810ChoiceSettingId(item)), value) + ")";
    return text;
}

// --- Pickable values of one item on one body -------------------------------------------------------------
struct D810ChoiceOption {
    std::int64_t value{};
    std::string label;
};
struct D810DroppedValue {
    std::int64_t value{};
    // ASCII token: the refusal of D810ChoosableRefusal.
    const char* reason{};
};
struct D810ChoiceOptions {
    std::vector<D810ChoiceOption> options;
    // Advertised values that are not offered, with the reason, in the order advertised.
    std::vector<D810DroppedValue> dropped;
    bool too_many{};
};
// The values the body advertised that pass D810ChoosableRefusal, in the order advertised (a value advertised
// twice is listed once). Labels within one item never repeat: when two values read the same (shutter speeds
// that round to the same fraction), every one of them gets its raw value appended.
inline D810ChoiceOptions BuildD810ChoiceOptions(D810ChoiceItem item, const D810PropDesc& desc) {
    D810ChoiceOptions out;
    const auto advertised = D810AdvertisedValuesOf(desc);
    out.too_many = advertised.too_many;
    for (const auto value : advertised.values) {
        const auto seen = [&](const auto& list) {
            for (const auto& entry : list)
                if (entry.value == value) return true;
            return false;
        };
        if (seen(out.options) || seen(out.dropped)) continue;
        if (const auto* reason = D810ChoosableRefusal(item, desc, value)) out.dropped.push_back({value, reason});
        else out.options.push_back({value, D810ChoiceLabel(item, value)});
    }
    const auto& spec = D810SettingOf(D810ChoiceSettingId(item));
    // Decide from the labels as they were before any raw value is appended.
    std::vector<bool> repeated(out.options.size(), false);
    for (std::size_t i = 0; i < out.options.size(); ++i)
        for (std::size_t j = i + 1; j < out.options.size(); ++j)
            if (out.options[i].label == out.options[j].label) repeated[i] = repeated[j] = true;
    for (std::size_t i = 0; i < out.options.size(); ++i)
        if (repeated[i]) out.options[i].label += " (" + D810HexValue(spec, out.options[i].value) + ")";
    return out;
}

// --- Panel cells -------------------------------------------------------------------------------------------
// One item on one camera, built from a read each time and never stored.
struct D810PanelCell {
    // The description was read and its type is the table's.
    bool readable{};
    std::int64_t current{};
    // The current value is one of `options`.
    bool current_choosable{};
    // Text for the current value: its label when pickable; otherwise the value with a note that the
    // application cannot pick it ("プリセット（このアプリでは選べません）", "raw 0x0001（…）").
    std::string current_text;
    std::vector<D810ChoiceOption> options;
    std::vector<D810DroppedValue> dropped;
    // nullptr: the cell can be edited. Otherwise an ASCII token: unreadable, not_writable, chosen_values_disabled,
    // row_disabled, code_unconfirmed, body_not_verified, exposure_mode_not_manual, iso_auto_not_off, no_options
    // (the first that applies, in this order). The options are listed also when the cell is locked.
    const char* locked_reason{"unreadable"};

    bool Editable() const { return locked_reason == nullptr; }
    bool Offers(std::int64_t value) const {
        for (const auto& option : options)
            if (option.value == value) return true;
        return false;
    }
};

inline std::string D810ChoiceCurrentText(D810ChoiceItem item, std::int64_t value) {
    const auto& spec = D810SettingOf(D810ChoiceSettingId(item));
    std::string text;
    if (item == D810ChoiceItem::WhiteBalance && value == 0x8013) text = "プリセット";
    else if (item == D810ChoiceItem::ShutterSpeed || item == D810ChoiceItem::Aperture || item == D810ChoiceItem::Iso) text = D810ChoiceName(item, value);
    else text = FormatD810Setting(spec, value);
    return text + "（このアプリでは選べません）";
}

// `body_verified`: the body's model and firmware are the ones the table names (the caller decides; no value of
// the body is passed in).
inline D810PanelCell BuildD810PanelCell(const D810WriteTable& table, D810Camera camera, D810ChoiceItem item,
    const D810SettingsSnapshot& snapshot, bool body_verified) {
    D810PanelCell cell;
    const auto write_item = D810ChoiceWriteItem(item);
    const auto& write_spec = D810WriteItemOf(write_item);
    const auto* reading = FindD810Reading(snapshot, D810ChoiceSettingId(item));
    if (reading && reading->available && reading->desc.data_type == static_cast<std::uint16_t>(write_spec.type)) {
        cell.readable = true;
        cell.current = reading->desc.current_value;
        auto built = BuildD810ChoiceOptions(item, reading->desc);
        cell.options = std::move(built.options);
        cell.dropped = std::move(built.dropped);
        cell.current_choosable = cell.Offers(cell.current);
        if (cell.current_choosable) {
            for (const auto& option : cell.options)
                if (option.value == cell.current) cell.current_text = option.label;
        } else {
            cell.current_text = D810ChoiceCurrentText(item, cell.current);
        }
    }
    const auto* mode = AvailableD810Reading(snapshot, kD810ExposureModeCode);
    const auto* iso_auto = AvailableD810Reading(snapshot, table.Code(D810WriteItem::IsoAuto));
    if (!cell.readable) cell.locked_reason = "unreadable";
    else if (!D810DescWritable(reading->desc)) cell.locked_reason = "not_writable";
    else if (const auto* refusal = D810RowRefusal(table, write_item, camera, D810PlanSource::Chosen)) cell.locked_reason = refusal;
    else if (!body_verified) cell.locked_reason = "body_not_verified";
    else if (!(mode && mode->desc.current_value == kD810ExposureModeManual)) cell.locked_reason = "exposure_mode_not_manual";
    else if (!(iso_auto && iso_auto->desc.current_value == kD810IsoAutoOff)) cell.locked_reason = "iso_auto_not_off";
    else if (cell.options.empty()) cell.locked_reason = "no_options";
    else cell.locked_reason = nullptr;
    return cell;
}

// Both cameras, every pickable item, from one pair of reads. `read_generation` numbers that read.
struct D810PanelModel {
    std::uint64_t read_generation{};
    std::array<std::array<D810PanelCell, kD810ChoiceItemCount>, kD810CameraCount> cells;
    const D810PanelCell& At(D810Camera camera, D810ChoiceItem item) const {
        return cells[static_cast<std::size_t>(camera)][static_cast<std::size_t>(item)];
    }
};
inline D810PanelModel BuildD810PanelModel(const D810WriteTable& table, const D810SettingsSnapshot& a, bool a_verified,
    const D810SettingsSnapshot& b, bool b_verified, std::uint64_t read_generation) {
    D810PanelModel model;
    model.read_generation = read_generation;
    for (const auto item : kD810ChoiceItems) {
        model.cells[static_cast<std::size_t>(D810Camera::A)][static_cast<std::size_t>(item)] = BuildD810PanelCell(table, D810Camera::A, item, a, a_verified);
        model.cells[static_cast<std::size_t>(D810Camera::B)][static_cast<std::size_t>(item)] = BuildD810PanelCell(table, D810Camera::B, item, b, b_verified);
    }
    return model;
}

// --- Draft ---------------------------------------------------------------------------------------------------
// The operator's picks. Held by the screen thread only; it sends nothing and holds no authority. SameAsA is a
// relation resolved when a plan is made, not a stored value, and exists for CAM-B only.
enum class D810ChoiceSource : std::uint8_t { Picked, SameAsA };
struct D810DraftEntry {
    D810ChoiceSource source{D810ChoiceSource::Picked};
    // Set for Picked only.
    std::optional<std::int64_t> value;
};
// A pick as the plan takes it. `value` is 0 and ignored for SameAsA.
struct D810CameraChoice {
    D810WriteItem item{};
    D810Camera camera{};
    D810ChoiceSource source{};
    std::int64_t value{};
};
// A change the draft stands for, resolved against a read: the value the camera would be given and the value it
// reads now.
struct D810PlannedWrite {
    D810Camera camera{};
    D810ChoiceItem item{};
    std::int64_t value{};
    std::int64_t current{};
};

class D810PanelDraft {
public:
    explicit D810PanelDraft(std::uint64_t read_generation = 0) : read_generation_(read_generation) {}

    std::uint64_t ReadGeneration() const { return read_generation_; }
    const std::optional<D810DraftEntry>& Get(D810Camera camera, D810ChoiceItem item) const { return Slot(camera, item); }
    std::size_t EntryCount() const {
        std::size_t count = 0;
        for (const auto& row : entries_)
            for (const auto& entry : row) count += entry.has_value();
        return count;
    }

    // Picks `value` for the cell. Refused (false, nothing stored) when the draft belongs to another read, the
    // cell is locked, or the cell does not offer the value. Replaces an earlier entry of the same cell.
    bool Pick(const D810PanelModel& model, D810Camera camera, D810ChoiceItem item, std::int64_t value) {
        if (model.read_generation != read_generation_) return false;
        const auto& cell = model.At(camera, item);
        if (!cell.Editable() || !cell.Offers(value)) return false;
        Slot(camera, item) = D810DraftEntry{D810ChoiceSource::Picked, value};
        return true;
    }
    // CAM-B follows CAM-A. Refused when the draft belongs to another read, CAM-B's cell is locked, or CAM-A's
    // value (see EffectiveAValue) is unknown or not offered by CAM-B. No value is stored.
    bool PickSameAsA(const D810PanelModel& model, D810ChoiceItem item) {
        if (model.read_generation != read_generation_) return false;
        if (!model.At(D810Camera::B, item).Editable() || !SameAsAOffered(model, item)) return false;
        Slot(D810Camera::B, item) = D810DraftEntry{D810ChoiceSource::SameAsA, std::nullopt};
        return true;
    }
    void Cancel(D810Camera camera, D810ChoiceItem item) { Slot(camera, item).reset(); }

    // CAM-A's value of the item as a plan would take it: the value picked for CAM-A, else what CAM-A reads now.
    std::optional<std::int64_t> EffectiveAValue(const D810PanelModel& model, D810ChoiceItem item) const {
        const auto& entry = Slot(D810Camera::A, item);
        if (entry && entry->source == D810ChoiceSource::Picked) return entry->value;
        const auto& cell = model.At(D810Camera::A, item);
        if (cell.readable) return cell.current;
        return std::nullopt;
    }
    bool SameAsAOffered(const D810PanelModel& model, D810ChoiceItem item) const {
        const auto value = EffectiveAValue(model, item);
        return value && model.At(D810Camera::B, item).Offers(*value);
    }

    // The entries that would change a camera, resolved against `model`: CAM-B before CAM-A, the item table's
    // order within a camera. An entry that resolves to the camera's current value is not listed. A draft that
    // belongs to another read lists nothing.
    std::vector<D810PlannedWrite> PlannedWrites(const D810PanelModel& model) const {
        std::vector<D810PlannedWrite> out;
        if (model.read_generation != read_generation_) return out;
        for (const auto camera : {D810Camera::B, D810Camera::A})
            for (const auto item : kD810ChoiceItems) {
                const auto& entry = Slot(camera, item);
                const auto& cell = model.At(camera, item);
                if (!entry || !cell.readable) continue;
                const auto value = entry->source == D810ChoiceSource::Picked ? entry->value : EffectiveAValue(model, item);
                if (!value || *value == cell.current) continue;
                out.push_back({camera, item, *value, cell.current});
            }
        return out;
    }
    // The number of changes ("N 件を書き込む").
    std::size_t Dirty(const D810PanelModel& model) const { return PlannedWrites(model).size(); }

    // Every entry, no-ops included, in the order a plan takes them: CAM-B before CAM-A, the item table's order
    // within a camera.
    std::vector<D810CameraChoice> Choices() const {
        std::vector<D810CameraChoice> out;
        for (const auto camera : {D810Camera::B, D810Camera::A})
            for (const auto item : kD810ChoiceItems) {
                const auto& entry = Slot(camera, item);
                if (!entry) continue;
                out.push_back({D810ChoiceWriteItem(item), camera, entry->source,
                    entry->source == D810ChoiceSource::Picked ? entry->value.value_or(0) : 0});
            }
        return out;
    }

    // Drops every entry and returns the number of changes dropped, counted against `model` (the read the
    // screen shows; call it before the model is replaced, since a draft of another read counts 0). The draft
    // then belongs to `model`'s read.
    std::size_t Discard(const D810PanelModel& model) {
        const auto count = Dirty(model);
        for (auto& row : entries_)
            for (auto& entry : row) entry.reset();
        read_generation_ = model.read_generation;
        return count;
    }

private:
    std::optional<D810DraftEntry>& Slot(D810Camera camera, D810ChoiceItem item) {
        return entries_[static_cast<std::size_t>(camera)][static_cast<std::size_t>(item)];
    }
    const std::optional<D810DraftEntry>& Slot(D810Camera camera, D810ChoiceItem item) const {
        return entries_[static_cast<std::size_t>(camera)][static_cast<std::size_t>(item)];
    }

    std::uint64_t read_generation_{};
    std::array<std::array<std::optional<D810DraftEntry>, kD810ChoiceItemCount>, kD810CameraCount> entries_{};
};
}
