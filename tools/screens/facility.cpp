// SPDX-License-Identifier: GPL-3.0-or-later
// Facility-type picker (fcn_00440aac / fcn_0043fae4) and Research Institute tool picker
// (fcn_0044101d / fcn_004402d7), see facility.hpp and docs/ui/facility-build.md.
#include "facility.hpp"

#include "messages.hpp"
#include "rich4/services.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace rich4::native::facility {
namespace {

constexpr std::string_view prompt = "請選擇設施類別";          // 0x465289
constexpr std::string_view research_prompt = "請選擇欲開發道具"; // 0x465298
// 0x475150: facility type names, drawn in the box for the hovered slot.
constexpr std::array<std::string_view, 5> names{"公  園", "旅  館", "購物中心", "加油站", "研究所"};
// create_font(0x10, 0xf0f0f0, 0x101010, 3, 1) in fcn_00440aac / fcn_0044101d: bold 16 px white, drop shadow.
constexpr ui::TextStyle box_style{16, ui::color(240, 240, 240), true, true, false, ui::color(16, 16, 16)};

constexpr int strip_x = 43, strip_y = 279; // Data 517:4 anchor (0x2b, 0x117)
constexpr int slot_x = 50, slot_y = 286, slot_size = 68; // hit zone x 0x32..0x186, y 0x11e..0x162

// Research Institute strip (fcn_0044101d / fcn_004402d7): Data 517:7 (400x89) at (0x14, 0x118).
// Five 66x54 inner slots starting at (0x23, 0x129) with stride 0x4c (76 px); Panel 11:10..14 icons
// centred at (0x23+0x21+76i, 0x118+0x2c) = (68+76i, 324).
constexpr int research_strip_x = 20, research_strip_y = 280, research_strip_w = 400, research_strip_h = 89;
constexpr int research_slot_x = 35, research_slot_y = 297, research_slot_w = 66, research_slot_h = 54;
constexpr int research_stride = 76;

std::optional<std::size_t> option_with_value(const GameDecision &decision, std::int32_t value) {
    for (std::size_t i = 0; i < decision.options.size(); ++i)
        if (decision.options[i].value == value)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> cancel_option(const GameDecision &decision) {
    if (decision.kind == DecisionKind::action_target || decision.kind == DecisionKind::research_tool)
        return option_with_value(decision, -1);
    return std::nullopt;
}

} // namespace

bool handles(const GameDecision &decision) {
    if (decision.kind == DecisionKind::construct_facility || decision.kind == DecisionKind::research_tool)
        return true;
    return decision.kind == DecisionKind::action_target && decision.prompt == prompt &&
           std::all_of(decision.options.begin(), decision.options.end(),
                       [](const auto &option) { return !option.location && option.value >= -1 && option.value <= 4; });
}

bool Picker::cancellable(const GameDecision &decision) {
    return cancel_option(decision).has_value();
}

std::optional<std::size_t> Picker::selected_option(const GameDecision &decision) const {
    if (!selected_)
        return std::nullopt;
    const auto value = decision.kind == DecisionKind::research_tool ? *selected_ + 1 : *selected_;
    return option_with_value(decision, value);
}

void Picker::cancel(screens::Host &host, const GameDecision &decision,
                    const std::function<void(std::size_t)> &respond) {
    if (const auto index = cancel_option(decision)) {
        host.sound(screens::cancel_sound); // right-click: Effect 4 (0x482332), result -1
        respond(*index);
    }
}

void Picker::draw(screens::Host &host, const GameDecision &decision, const std::string &key,
                  const std::function<void(std::size_t)> &respond) {
    auto &frame = host.frame;
    if (key != key_) {
        key_ = key;
        selected_.reset();
    }
    const bool research = decision.kind == DecisionKind::research_tool;
    // Box Data 517:5 (transparent) at (220,140), prompt mode 2 at (220,122).
    graphics::blit(frame.canvas, frame.artwork.frame('d', 517, 5), 220, 140);
    messages::draw_text(frame, research ? research_prompt : prompt, 220, 122, 2, box_style);
    if (research) {
        graphics::blit(frame.canvas, frame.artwork.frame('d', 517, 7), research_strip_x, research_strip_y,
                       graphics::Transparency::opaque);
        for (int slot = 0; slot < 5; ++slot) {
            const int x = research_slot_x + slot * research_stride;
            const graphics::Rect inner{x, research_slot_y, x + research_slot_w, research_slot_y + research_slot_h};
            // Panel 11:(10+slot) icons are tools 9..13 (機器工人, 時光機, 傳送機, 工程車, 核子飛彈).
            graphics::blit(frame.canvas, frame.artwork.frame('p', 11, std::size_t(10 + slot)),
                           x + research_slot_w / 2, research_strip_y + 44);
            const auto index = option_with_value(decision, slot + 1);
            if (!index) {
                // Locked above the institute's current level (fcn_0044101d -> fcn_004553fe).
                screens::greyscale(frame.canvas, inner);
                screens::darken(frame.canvas, inner, -6);
            } else if (frame.pointer_down && frame.pointer_in(inner)) {
                screens::pressed_bevel(frame.canvas, inner); // WM_LBUTTONDOWN: fcn_00451b9e
            }
            if (selected_ == slot) {
                // Hover (fcn_004402d7 WM_MOUSEMOVE): two nested 0xffff00 frames 71x59 and 69x57.
                const int fx = 32 + slot * research_stride, fy = 294;
                screens::outline_rect(frame.canvas, {fx, fy, fx + 71, fy + 59}, screens::rgb(0xffff00));
                screens::outline_rect(frame.canvas, {fx + 1, fy + 1, fx + 70, fy + 58}, screens::rgb(0xffff00));
            }
            const graphics::Rect cell{slot == 0 ? research_strip_x : x - 5, research_strip_y,
                                      slot == 4 ? research_strip_x + research_strip_w : x + research_slot_w + 5,
                                      research_strip_y + research_strip_h};
            if (index) {
                const bool chosen = selected_ == slot;
                screens::hit(host, 210 + int(*index), cell, chosen ? screens::click_sound : screens::hover_sound,
                             [this, slot, chosen, index = *index, respond] {
                                 if (chosen)
                                     respond(index); // WM_LBUTTONUP: Effect 1 (0x482322)
                                 else
                                     selected_ = slot; // entering a slot: Effect 0 (0x48231a)
                             });
            } else {
                // Locked slots still preview their tool name on hover/tap (0x44046a), without committing.
                screens::hit(host, 220 + slot, cell, screens::hover_sound, [this, slot] { selected_ = slot; });
            }
        }
        if (selected_ && *selected_ >= 0 && *selected_ < 5)
            messages::draw_text(frame, services::tool_names[std::size_t(*selected_) + 8U], 220, 154, 4, box_style);
    } else {
        // fcn_00440aac: strip Data 517:4 opaque at (43,279). Slot i covers x 50+68i, y 286, 68x68.
        graphics::blit(frame.canvas, frame.artwork.frame('d', 517, 4), strip_x, strip_y,
                       graphics::Transparency::opaque);
        for (int slot = 0; slot < 5; ++slot) {
            const int x = slot_x + slot * slot_size;
            const auto index = option_with_value(decision, slot);
            // Finger targets: the slot's column over the strip's full height (edges to the frame).
            const graphics::Rect cell{slot == 0 ? strip_x : x, strip_y,
                                      slot == 4 ? x + slot_size + 7 : x + slot_size, strip_y + 83};
            if (!index) { // a type the core does not offer here is dimmed and inert
                screens::darken(frame.canvas, {x, slot_y, x + slot_size, slot_y + slot_size}, -16);
                continue;
            }
            if (selected_ == slot) {
                // Hover (fcn_0043fae4 WM_MOUSEMOVE): three nested 0xffff00 frames 68/66/64 px.
                for (int inset = 0; inset < 3; ++inset)
                    screens::outline_rect(
                        frame.canvas, {x + inset, slot_y + inset, x + slot_size - inset, slot_y + slot_size - inset},
                        screens::rgb(0xffff00));
            }
            const bool chosen = selected_ == slot;
            screens::hit(host, 210 + int(*index), cell, chosen ? screens::click_sound : screens::hover_sound,
                         [this, slot, chosen, index = *index, respond] {
                             if (chosen)
                                 respond(index); // WM_LBUTTONUP: Effect 1 (0x482322), returns the slot
                             else
                                 selected_ = slot; // entering a slot: Effect 0 (0x48231a)
                         });
        }
        if (selected_ && *selected_ >= 0 && *selected_ < 5) // hovered type's name, mode 4 at (220,154)
            messages::draw_text(frame, names[std::size_t(*selected_)], 220, 154, 4, box_style);
    }
    // NATIVE: touch has no hover/right-click: 確定 commits the selection; 取消 replaces the
    // right-click cancel (改建卡 and 研究所; building on a lot cannot be cancelled in the original).
    const auto cancel = cancel_option(decision);
    const int left = cancel ? 128 : 180;
    const int button_top = research ? 376 : 372;
    const auto selected_index = selected_option(decision);
    frame.button(confirm_button, {left, button_top, left + 80, button_top + 40}, "確定", [] {},
                 selected_index.has_value());
    frame.buttons.pop_back(); // re-register with the original click sound instead of the generic one
    screens::hit(host, confirm_button, {left, button_top, left + 80, button_top + 40}, screens::click_sound,
                 [respond, selected_index] {
                     if (selected_index)
                         respond(*selected_index);
                 },
                 selected_index.has_value());
    if (cancel) {
        frame.button(210 + int(*cancel), {232, button_top, 312, button_top + 40}, "取消", [] {});
        frame.buttons.pop_back();
        screens::hit(host, 210 + int(*cancel), {232, button_top, 312, button_top + 40}, screens::cancel_sound,
                     [respond, index = *cancel] { respond(index); });
    }
}

} // namespace rich4::native::facility
