// SPDX-License-Identifier: GPL-3.0-or-later
// Original facility-type picker (docs/ui/facility-build.md): fcn_00440aac + window proc
// fcn_0043fae4 (Data 517:5 box + Data 517:4 strip of five 68x68 facility icons), and the Research
// Institute tool picker: fcn_0044101d + window proc fcn_004402d7 (Data 517:5 box + Data 517:7
// strip of five 66x54 slots with Panel 11:10..14 tool icons). Touch: the first tap selects a slot
// (the original hover: yellow frame and name in the box), a second tap on an enabled slot or 確定
// commits; 取消 / Back / right-click cancel where the original allows right-click (改建卡 and
// 研究所).
#pragma once

#include "draw.hpp"
#include "rich4/game.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace rich4::native::facility {

inline constexpr int confirm_button = 207; // 確定 (same id as the map-target confirm)

// construct_facility, research_tool, and the 改建卡 type choice (action_target "請選擇設施類別").
[[nodiscard]] bool handles(const GameDecision &decision);

class Picker {
  public:
    // `key` identifies the decision instance; a new key clears the selection. Option ids stay
    // 210 + option index (slot taps and 取消), so the replay harness keeps working.
    void draw(screens::Host &host, const GameDecision &decision, const std::string &key,
              const std::function<void(std::size_t)> &respond);
    // True when the decision has a cancel option (改建卡 / 研究所: the original's right-click).
    [[nodiscard]] static bool cancellable(const GameDecision &decision);
    // Option index of the currently selected enabled slot, if any (for Return/Space confirm).
    [[nodiscard]] std::optional<std::size_t> selected_option(const GameDecision &decision) const;
    // Android Back / Escape: answers the cancel option with Effect 4.
    void cancel(screens::Host &host, const GameDecision &decision, const std::function<void(std::size_t)> &respond);
    // Gallery: preselects a slot (0..4).
    void select(std::optional<int> slot, std::string key) {
        key_ = std::move(key);
        selected_ = slot;
    }

  private:
    std::string key_;
    std::optional<int> selected_; // slot 0..4 (facility type 0..4, or research tool 1..5 minus 1)
};

} // namespace rich4::native::facility
