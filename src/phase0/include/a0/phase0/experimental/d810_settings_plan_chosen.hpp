#pragma once
// The plan of the values an operator picked for each camera (#416, ADR-0041).
//
// Pure functions only: no Windows headers, no device access, no file I/O, and nothing here can send a command.
// It turns the picks of the panel draft (D810CameraChoice, d810_settings_choice.hpp) and two fresh reads into a
// D810WritePlan that the controller (wpd_settings_write.hpp) confirms and runs. The plan is the only thing that
// reaches a gate: the draft, the panel and the picks hold no authority of their own.
//
// Only targets built with A0_ENABLE_WPD_SETTINGS_WRITE (the CMake option A0_BUILD_WPD_SETTINGS_WRITE, OFF by
// default, and the contract tests) can include this header, for the reason given in d810_settings_choice.hpp.
#if !defined(A0_ENABLE_WPD_SETTINGS_WRITE)
#error "d810_settings_plan_chosen.hpp is only for targets built with A0_BUILD_WPD_SETTINGS_WRITE"
#endif
#include "a0/phase0/experimental/d810_settings_choice.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace a0::phase0::experimental {
// The plan for the picks of both cameras.
//
//  - Order (an invariant of the plan, checked by D810PlanProblem): every CAM-B step first, every CAM-A step last;
//    within a camera, the item table's order. The picks may arrive in any order; the plan sorts them. One pick
//    per (camera, item).
//  - A pick that reads the camera's value now is not a step. "Same as CAM-A" is resolved here, from the read made
//    for this plan: CAM-A's pick of the item when there is one, else the value CAM-A reads now (which is then an
//    observation of the plan, so a change of CAM-A before the run stops it).
//  - Any refusal refuses the whole plan; the reasons are ASCII tokens "<what>:<item>:<CAM-x>" (or the bare token
//    for a plan-wide reason). No pick is dropped to let the others through.
//  - `verified` says which bodies matched the model and firmware the table names for them. A camera that gets a
//    step must be verified, so a plan with a CAM-A step is made only when CAM-A matched the model and firmware
//    the table names for it (the product table names CAM-B's version for CAM-A, by reference); when it did not,
//    the whole plan is refused with "body_not_verified:CAM-A", CAM-B's steps included.
//
// `table.chosen_values` is the switch: while it is false (a table without picked values) every call is refused.
// Which rows a plan can use is decided by the table's enabled rows (D810PlanProblem).
inline D810PlanResult BuildD810PerCameraPlan(const D810SettingsSnapshot& a, const D810SettingsSnapshot& b, const D810BodyFlags& verified,
    const std::vector<D810CameraChoice>& choices, const D810WriteTable& table) {
    using namespace d810_plan_detail;
    D810PlanResult result;
    auto& reasons = result.reasons;
    if (!table.chosen_values) { reasons.push_back("chosen_values_disabled"); return result; }
    if (choices.empty()) { reasons.push_back("no_choice"); return result; }
    const auto iso_auto_code = table.Code(D810WriteItem::IsoAuto);
    if (!iso_auto_code) { reasons.push_back("code_unconfirmed:iso_auto"); return result; }

    // The picks themselves: a known item and camera, an item that may be picked, no "same as CAM-A" for CAM-A,
    // one pick per (camera, item).
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto& choice = choices[i];
        if (static_cast<std::size_t>(choice.item) >= kD810WriteItemCount || static_cast<unsigned>(choice.camera) > 1) {
            reasons.push_back("item_not_choosable");
            return result;
        }
        const auto& spec = D810WriteItemOf(choice.item);
        if (D810ChoiceItemRefusal(choice.item)) reasons.push_back(Reason("item_not_choosable", spec, choice.camera));
        if (choice.camera == D810Camera::A && choice.source == D810ChoiceSource::SameAsA) reasons.push_back(Reason("same_as_a_on_cam_a", spec, choice.camera));
        for (std::size_t j = 0; j < i; ++j)
            if (choices[j].item == choice.item && choices[j].camera == choice.camera) reasons.push_back(Reason("duplicate_choice", spec, choice.camera));
    }
    if (!reasons.empty()) return result;

    // CAM-B before CAM-A, the item table's order within a camera.
    std::vector<const D810CameraChoice*> ordered;
    for (const auto& choice : choices) ordered.push_back(&choice);
    std::stable_sort(ordered.begin(), ordered.end(), [](const D810CameraChoice* left, const D810CameraChoice* right) {
        if (left->camera != right->camera) return left->camera == D810Camera::B;
        return static_cast<unsigned>(left->item) < static_cast<unsigned>(right->item);
    });

    D810WritePlan plan;
    plan.source = D810PlanSource::Chosen;
    plan.iso_auto_code = iso_auto_code;
    std::vector<D810PlanObservation> a_sources; // the values of CAM-A that a "same as CAM-A" was resolved from
    for (const auto* choice : ordered) {
        const auto& spec = D810WriteItemOf(choice->item);
        const auto camera = choice->camera;
        const auto& snapshot = camera == D810Camera::A ? a : b;
        if (const auto refusal = D810RowRefusal(table, choice->item, camera, plan.source)) { reasons.push_back(Reason(refusal, spec, camera)); continue; }
        const auto code = table.Code(choice->item);
        const auto* reading = AvailableD810Reading(snapshot, code);
        if (!reading) { reasons.push_back(Reason("unreadable", spec, camera)); continue; }
        std::int64_t value = choice->value;
        if (choice->source == D810ChoiceSource::SameAsA) {
            const auto* from = AvailableD810Reading(a, code);
            if (!from) { reasons.push_back(Reason("unreadable", spec, D810Camera::A)); continue; }
            const auto picked = std::find_if(choices.begin(), choices.end(), [&](const auto& other) {
                return other.camera == D810Camera::A && other.item == choice->item && other.source == D810ChoiceSource::Picked;
            });
            value = picked != choices.end() ? picked->value : from->desc.current_value;
            a_sources.push_back({D810Camera::A, code, from->desc.current_value});
        }
        if (const auto refusal = D810ChoosableRefusal(choice->item, reading->desc, value)) {
            reasons.push_back(Reason(choice->source == D810ChoiceSource::SameAsA ? "same_as_a_not_offered" : refusal, spec, camera));
            continue;
        }
        if (reading->desc.current_value != value) plan.steps.push_back({camera, choice->item, code, reading->desc.current_value, value});
    }
    if (!reasons.empty()) return result;

    // What the plan relies on: for each camera with a step, the values it reads now at the codes it touches and
    // the two it needs to read as they are (manual mode, ISO auto off); CAM-A's values a "same as CAM-A" came from.
    for (const auto& step : plan.steps) Observe(plan.observed, step.camera, step.code, step.before);
    for (const auto& seen : a_sources) Observe(plan.observed, seen.camera, seen.code, seen.value);
    for (const auto camera : plan.Cameras()) {
        const auto& snapshot = camera == D810Camera::A ? a : b;
        for (const auto code : {kD810ExposureModeCode, iso_auto_code})
            if (const auto* reading = AvailableD810Reading(snapshot, code)) Observe(plan.observed, camera, code, reading->desc.current_value);
        CheckPreconditions(plan, camera, snapshot, reasons);
        if (!verified[static_cast<std::size_t>(camera)]) reasons.push_back(std::string("body_not_verified:") + D810CameraAlias(camera));
    }
    if (const auto problem = D810PlanProblem(plan, table); !problem.empty()) reasons.push_back(problem);
    if (!reasons.empty()) return result;
    result.plan = std::move(plan);
    return result;
}
} // namespace a0::phase0::experimental
