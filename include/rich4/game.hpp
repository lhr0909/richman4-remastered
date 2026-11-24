// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rich4/cards.hpp"
#include "rich4/save.hpp"

namespace rich4 {

class World;
namespace trading {
struct Listing;
}

enum class GamePhase { human_ready, rolling, moving, awaiting_decision, advancing_turn, finished };
enum class DecisionKind {
    buy_land,
    upgrade_land,
    bank,
    card_target,
    choose_card,
    rescue,
    buy_facility,
    construct_facility,
    upgrade_facility,
    research_tool, // 研究所 tool choice (fcn_0044101d): options 1..level (tools 9..13) plus cancel (-1)
    roulette,
    shop,
    magic_category,
    magic_effect,
    auction,
    lottery,
    primary_stock,
    property_target,
    action_target,
    notice,
    money_spinner,
    mini_game // interactive 企鵝挖寶 / 七彩氣球 / 喜從天降; answer with submit_mini_game()
};
enum class GameEventKind {
    turn_started,
    dice_rolled,
    moved,
    land_purchased,
    land_upgraded,
    rent_paid,
    bank_transfer,
    date_changed,
    points_received,
    player_bankrupt,
    game_finished,
    unsupported_event,
    notice
};

struct DecisionLocation {
    std::int32_t x, y;
};

struct DecisionOption {
    std::string label;
    std::int32_t value;
    std::optional<DecisionLocation> location = std::nullopt;
    // Presentation only: what the option names, so the app can show the original name
    // (character, street, facility, company, special actor, object) instead of `label`.
    std::optional<CardTarget> subject = std::nullopt;
};

// Presentation state of the original number-roll UIs (docs/ui/messages.md §2.3), mirrored by the
// core while a spinner decision is pending (GameDecision::spinner) or carried by a spinner effect
// (EffectEvent::spinner) for computer/sleep-walking spins and the wheel's slow-down after STOP.
// Read-only for the app: it never feeds back into a rule or draws randomness.
struct SpinnerState {
    // money_spinner: god kind 1/2/5/6 (fcn_00440706, slot machine Panel.mkf 67).
    // roulette: wheel 0 travel, 1 hotel, 2 mall, 3 insurance (fcn_0044090e, cupid wheel Panel.mkf 68+type).
    std::uint8_t type = 0;
    // Slot machine reel positions 0..19 (digit * 2; odd = between two digits), as MoneySpinner.
    std::array<std::uint8_t, 4> digits{};
    // Slot machine: MoneySpinner phase (0 = not started yet, 1 spinning, 2..8 stopping, 9 result
    // hold, 10 done). Wheel effect: 1 = computer spin (free-runs 39 frames first), 2 = stopped by STOP.
    std::uint8_t phase = 0;
    std::uint8_t cursor = 0;   // Wheel sector 0..11 (Panel frame 2 + cursor).
    std::int32_t subject = -1; // Wheel: hotel/mall owner slot, or commercial id (travel/insurance).
    std::uint32_t seed = 0;    // Money effect: Random state the computer spin starts from.
    std::int32_t result = 0;   // Effect: the amount / wheel value the rule already applied.
    bool operator==(const SpinnerState &) const = default;
};

// A pending interactive mini-game (docs/ui/mini-games.md). The app plays it on a Random seeded
// with `seed` (the core's stream at landing time, exactly what the original's shared rand() would
// continue from) and reports the score and how many rand() calls the game made.
struct MiniGameState {
    std::uint8_t game = 0;      // minigames::Game: 0 企鵝挖寶 (node kind 6), 1 七彩氣球 (7), 2 喜從天降 (8)
    std::uint8_t character = 0; // the lander's character (喜從天降 catcher sprite Panel 100+character)
    std::uint32_t seed = 0;     // core random state when the game starts
    bool operator==(const MiniGameState &) const = default;
};

struct GameDecision {
    DecisionKind kind;
    std::size_t player;
    std::uint16_t land_id = 0;
    std::int32_t amount = 0;
    std::string prompt;
    std::vector<DecisionOption> options;
    std::int32_t max_amount = 0;
    std::optional<SpinnerState> spinner = std::nullopt; // money_spinner / roulette only
    std::optional<MiniGameState> mini_game = std::nullopt; // mini_game only
    // Auction only (presentation): player slots that can no longer bid in this auction (ineligible
    // at the start, or withdrew), for the original "cannot bid" pose Panel 28+3c.
    std::uint8_t inactive_players = 0;
    // 搶奪卡 inventory pick (fcn_0044192a): the player whose card and tool grids are shown.
    std::optional<std::size_t> victim = std::nullopt;
};

// AI settings (託管AI, _rich4_ui_ai_settings_entry; docs/ui/finance-menus.md §11): a human
// player's original profile bytes +22..+26 and the 託管 bit 0x04 of controller_flags (+21).
// With the bit set the core plays that player exactly like a computer player.
struct AiSettings {
    bool autopilot = false;         // controller_flags bit 0x04 (託管)
    std::uint8_t card_strategy = 0; // +22: bit0 use cards, bit1 use tools
    std::uint8_t personality = 0;   // +23: 0 乖寶寶, 1 普通人, 2 大老奸 (PlayerStatus::rescue_strategy)
    std::uint8_t cash_ratio = 50;   // +25: cash vs deposit, 0..100 (init_cash_ratio)
    std::uint8_t stock_ratio = 0;   // +26: stock vs funds, 0..100 (stock_investment_ratio)
    bool operator==(const AiSettings &) const = default;
};

// Original on-screen announcements (docs/ui/messages.md). Presentation only: emitting
// them never changes a rule outcome or consumes random numbers.
enum class Announcement : std::uint8_t {
    none,
    toll,              // Box before a toll: property, subject=owner, second=ally, amount, value=multiplier.
    cash_insufficient, // "您的現金不足！" box when a buy/upgrade costs more than cash.
    news,              // Full-screen news: id 0..35 plus body arguments.
    fortune,           // Full-screen fortune: id 0..48 plus body arguments.
    god_arrival,       // God attached: id = object kind (1..10, 12, 15).
    point_coupon,      // "得點券N點" box (1000 ms): amount 10/30/50.
    absent_status,     // Turn-start "%s坐牢中\n\n還剩%d天！" box: id 0 hotel, 1 away, 2 prison,
                       // 3 hospital, 4 hibernating; value = days left.
    toll_waived,       // _rich4_check_pay_toll_to_owner box "...免收%s！" (1500 ms) before event voice 13:
                       // id 0 seized, 1 allied, 2 god of death, 3 hotel, 4 away, 5 prison, 6 hospital,
                       // 7 hibernating, 8 sleepwalking; subject = owner, property = 2000+land / 4000+facility.
    tool_use,          // "使用%s" box (1500 ms) before a computer/託管 player's tool runs
                       // (rich4_ui_use_tool.asm:1721, the automatic tool loop); id = tool 1..13.
    stock_card,        // "對%s使用%s！" box (0x4653ae, 1500 ms) after a computer's 紅卡/黑卡: id = card
                       // 24/25, subject = stock index (rich4_card_hongka.asm / heika.asm AI branch).
    stolen,            // "搶得%s的\n\n%s" box (0x4652f8, 1500 ms) after a computer's 搶奪卡 pick:
                       // subject = victim, id 0 card / 1 tool, value = card or tool id.
    mini_game_points,  // Closed-mode mini-game "得點券%d點" box (2000 ms, SG:4392): amount 50..69.
    research_complete, // Research institute "%s開發完成！" box (1500 ms, fcn_0041c84f): id = tool 9..13.
};

// Which original per-character speech table a bubble line comes from (docs/ui/messages.md §1.4).
// event: _rich4_event_strings (voice_category); card: _rich4_card_strings, slot = row*30 + card-1
// with row 0 the user, 1 the user targeting itself, 2 the victim; tool: _tool_strings, slot =
// tool-1 for tools 1..13, 14 roadblock hit, 15 mine hit, 16 time bomb picked up.
enum class SpeechTable : std::uint8_t { event, card, tool };

struct GameEvent {
    GameEventKind kind;
    std::size_t player;
    std::int32_t amount = 0;
    std::uint16_t node = 0;
    std::string message;
    std::optional<std::uint8_t> voice_category = std::nullopt;
    Announcement announcement = Announcement::none;
    std::uint8_t announcement_id = 0; // News/fortune handler, god kind or facility type (toll).
    std::uint16_t property = 0;       // Original property code: 2000+land, 4000+facility.
    std::int32_t subject = -1, second_subject = -1, company = -1, value = 0;
    std::uint8_t players = 0; // Affected player slots (portrait rows).
    std::array<std::int32_t, 4> player_amounts{};
    // Card id (1..30) when this event reports a card being used, for the card-art popup.
    std::optional<std::uint8_t> card = std::nullopt;
    // Card/tool speech bubble (presentation only, never draws randomness): `player` speaks
    // table[character][speech_slot] with portrait mood `speech_mood` (the original call-site
    // argument). Event-table speech keeps using voice_category.
    SpeechTable speech_table = SpeechTable::event;
    std::uint8_t speech_slot = 0, speech_mood = 0;
    // Presentation-only sound cue (Effect.mkf index) the app plays when it presents this event,
    // e.g. 54 god leaves, 35/36 gift pickups, 56 forced turn-around, 15 工程車 demolition swing.
    std::optional<std::uint8_t> sound = std::nullopt;
    bool operator==(const GameEvent &) const = default; // pre-choice presentation replay matching
};

// Presentation-only record of an original on-board animation (docs/ui/effects.md §9). The rule
// change has ALREADY happened when the effect is drained; the app replays the picture (FLC movie,
// sprite throw, dice) and may show the pre-mutation board until the movie's commit frame.
// Emitting effects never consumes randomness and never changes rule outcomes.
enum class EffectKind : std::uint8_t {
    dice_roll,         // Panel 4..6 + Panel 3 faces; `dice` holds the values already rolled
    top_floor_sparkle, // Data 523, a property reached level 5
    build_rise,        // Data 553, robot worker free build
    object_throw,      // roadblock/mine/time bomb flies from `from` to `target`; object_kind 16/17/18
    missile,           // Data 528
    nuclear_missile,   // Data 530
    explosion,         // Data 525, mine stepped on / time bomb exploded
    demolish,          // Data 529, demolish card
    monster,           // Data 557, monster card
    god_appear,        // Data 540.., object_kind = god kind 1..15
    demon_puff,        // Data 526, demon god downgraded the property at `target`
    bankrupt,          // Data 555
    money_spinner,     // computer/sleep-walking god slot machine (fcn_00440706); `spinner` set
    roulette,          // cupid wheel: computer spin, or the slow-down after a human STOP; `spinner` set
    // MONTHLY: full-screen monthly-cycle screens, `monthly` set (tools/screens/monthly.cpp).
    dividend,     // 15th: 上市公司分紅 table, Panel 76 (fcn_0042ba97)
    lottery_draw, // 15th: 樂透開獎 show, Panel 15..17 (_rich4_ui_letou_kaijiang_entry)
    month_report, // 1st: bank interest / ranking report, Panel 25 (fcn_00439bfa / fcn_00437e61)
    // ---- Board movies (docs/audit/data-400-601.md). `from` = the actor's old world point when the
    // movie shows the actor leaving (drawn there until the commit frame), else unset. ----
    ambulance,         // Data 524, hospitalised (rich4_hospital_utils.asm:92)
    police,            // Data 538, jailed (rich4_prison_utils.asm:92)
    dog_vehicle,       // Data 552, rode into the dog (core_actions:2919)
    dog_fight,         // Data 532, met the dog on foot (core_actions:2945)
    ufo,               // Data 533, abducted (object_kind 0) or returned (object_kind 1)
    airplane,          // Data 558, left on a trip (object_kind 0) or returned (object_kind 1)
    parachute,         // Data 559 + character, player placed on the board (fcn_0040829d/fcn_00418c55)
    news_movie,        // Data 531/539/527/534/535 for news object_kind 4/5/15/20/21 at `target`
    card_throw,        // Data 415 card flies `from` -> `target` (AI targeted cards, animate_object(0))
    item_box,          // Data 536 spinning "?" before the random card square (core_actions:2507)
    coupon,            // Data 537 spinning ticket before a point-coupon box (core_actions:2394)
    truck_demolish,    // 工程車 pose 12 swing + Data 526 puff (player_utils state 3); object_kind =
                       // facing direction, previous_level = building level before the demolition
    holiday_fireworks, // Data 523 on New Year / national day (holiday table 0x47ff4c)
    christmas,         // Data 554 on Dec 25 (holiday table 0x47ff4c)
    // No picture: the original movement state machine (fcn_0040d7c4, rich4_player_utils.asm:587-625)
    // counts byte 0x498ea5 down once per game tick before acting. `object_kind` = ticks (80 ms at the
    // default speed): 5 between the last step and the landing handler (loc_0040d916), 8 after a
    // property landing (core_actions:2347 `mov byte [esp+0xf4], 0x88`) before the next player.
    pause,
    // _rich4_detach_god (rich4_gods.asm:138-420): the carried god (object_kind) rises 10 px per
    // 60 ms step for up to 24 steps, turning one facing frame per step, from its trailing spot
    // behind the carrier at `target` facing `direction`. Before the god-left speech (slot 23).
    god_leave
};

// Figures of the monthly-cycle screens (docs/ui/finance-menus.md §4 dividend, §7.2 lottery draw,
// §14 month report). Presentation only: the rule has ALREADY applied them when the effect is
// drained. Arrays are indexed by player slot; `active` masks the players the original lists.
struct DividendRow {
    std::uint8_t stock = 0;          // stock index 0..11 (company name)
    std::int32_t earnings = 0;       // 本月盈餘: the company's funds before the payout
    std::array<std::int32_t, 4> payout{}; // trunc(earnings × shares / Σ shares) per slot
    bool operator==(const DividendRow &) const = default;
};
struct MonthlyFigures {
    std::uint8_t active = 0; // bit mask of the listed player slots (original +0x15 flag)
    std::array<std::uint8_t, 4> characters{};
    // dividend
    std::vector<DividendRow> dividends{};
    // lottery_draw
    std::array<std::uint8_t, 36> tickets{}; // owner slot+1 of each number BEFORE the draw
    std::int32_t pool = 0;                  // 累積獎金 before the payout
    std::uint8_t number = 0;                // drawn number 1..36
    std::int32_t winner = -1;               // winning slot, -1 = nobody
    // month_report. Cards (Panel 25:11+i): 存款 = deposit before interest, 利息 = trunc(deposit×0.1)
    // or 貸款中. Champion panel: cash, balance (deposit after interest) and 總資產 (wealth after it).
    std::array<std::int32_t, 4> cash{}, deposit{}, interest{}, balance{}, total{};
    std::uint8_t loans = 0;                            // bit mask: 貸款中 (no interest this month)
    std::array<std::uint8_t, 4> unlucky_days{};        // 本月倒楣天數 (status +66)
    std::array<std::uint32_t, 4> income{}, spending{}; // 本月意外之財 / 本月意外損失 (+96 / +92)
    std::int32_t champion = -1, unlucky = -1;          // 本月冠軍 / 本月悲情人物 slots (-1 = none)
    bool operator==(const MonthlyFigures &) const = default;
};
struct EffectPoint {
    std::int32_t x = 0, y = 0; // Original world coordinates (same space as board::Point).
    bool operator==(const EffectPoint &) const = default;
};
struct EffectEvent {
    EffectKind kind;
    std::size_t player = 0;                         // actor
    std::uint16_t node = 0;                         // actor's road node, or the throw's target node
    EffectPoint target{};                           // world point the camera centres on
    std::optional<EffectPoint> from = std::nullopt; // throws: actor world point
    std::uint8_t object_kind = 0;                   // throws: 16/17/18; god_appear: god kind
    std::vector<std::uint8_t> dice{};               // dice_roll values
    std::uint16_t property = 0;                     // target property code (2001.. / 4001..), if any
    // Presentation order: how many GameEvents of the same drain batch were emitted before this
    // effect, so the app can interleave drain_effects() with drain_events() in true order.
    std::size_t after_events = 0;
    std::optional<SpinnerState> spinner = std::nullopt; // money_spinner / roulette
    std::optional<MonthlyFigures> monthly = std::nullopt; // dividend / lottery_draw / month_report
    // truck_demolish: `property` level and type (land chain flag / facility type) shown until the
    // commit frame, i.e. the building before the rule demolished it.
    std::uint8_t previous_level = 0, previous_type = 0;
    std::uint8_t direction = 0; // god_leave: the carrier's heading (sprite frame and trailing offset)
    bool operator==(const EffectEvent &) const = default;
};

struct MoveAnimation {
    std::size_t player;
    std::int32_t from_x, from_y, to_x, to_y;
    unsigned elapsed_ms, duration_ms;
};

// Owns a portable World and a Watcom random stream. Window/audio code never
// participates in the rule implementation. tick() changes animation timing;
// rolling and path selection consume randomness only once per rule action.
class GameSession {
  public:
    explicit GameSession(World world, std::uint32_t seed = 1);
    ~GameSession();
    GameSession(GameSession &&) noexcept;
    GameSession &operator=(GameSession &&) noexcept;
    GameSession(const GameSession &) = delete;
    GameSession &operator=(const GameSession &) = delete;

    [[nodiscard]] const World &world() const noexcept;
    [[nodiscard]] GamePhase phase() const noexcept;
    [[nodiscard]] const std::optional<GameDecision> &pending_decision() const noexcept;
    [[nodiscard]] const std::optional<MoveAnimation> &animation() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t> &dice() const noexcept;
    [[nodiscard]] std::uint32_t random_state() const noexcept;
    [[nodiscard]] int steps_remaining() const noexcept;
    [[nodiscard]] std::uint64_t completed_days() const noexcept;
    // Presentation only: 19 (hospital gown) or 20 (prison stripes) while a player pulled out of
    // hospital/prison early still wears it (fcn_0040d761 -> fcn_0040bf93), else 0.
    [[nodiscard]] std::uint8_t player_costume(std::size_t slot) const noexcept;

    // Returns false for input that does not apply to the current phase. option
    // is an index into pending_decision().options. Bank amount must be positive.
    bool roll_dice();
    bool set_dice_count(unsigned count);
    [[nodiscard]] std::optional<std::size_t> winning_player() const noexcept;
    void set_camera_rotation(std::uint8_t rotation);
    [[nodiscard]] std::vector<CardTarget> card_targets(std::uint8_t card) const;
    bool use_card(const CardUse &card);
    bool use_tool(unsigned id, std::uint16_t target = 0, std::uint16_t destination = 0);
    bool dismount();
    bool buy_stock(std::size_t stock, std::int32_t quantity);
    bool sell_stock(std::size_t stock, std::int32_t quantity);
    bool close_stock_market();
    bool open_trading();
    bool post_offer(const trading::Listing &offer);
    bool cancel_offer(std::size_t index);
    bool buy_offer(std::size_t seller, std::size_t index);
    bool close_trading();
    bool respond(std::size_t option, std::int32_t amount = 0);
    // Original Options "動畫過程" (RICH4.CFG[1]). Off (the default, and the headless replay
    // harness) keeps the closed mode: a human on a mini-game tile gets rand()%20+50 points like a
    // computer. On: a human gets a DecisionKind::mini_game decision instead and no random number
    // is drawn until submit_mini_game.
    void set_interactive_mini_games(bool enabled) noexcept;
    [[nodiscard]] bool interactive_mini_games() const noexcept;
    // Ends a pending mini_game decision: advances the random stream by `random_draws` calls (the
    // game's own rand() calls in the shared original stream), adds `score` to the player's points
    // as the original 16-bit word, and finishes the turn. Returns false when no mini-game is
    // pending or the input is out of range (score < 0, draws > 10,000,000).
    bool submit_mini_game(std::int32_t score, std::uint32_t random_draws);
    // AI settings for a human player slot (controller bit0); nullopt for computer or empty slots.
    // Reports a change that is still waiting for the turn boundary.
    [[nodiscard]] std::optional<AiSettings> ai_settings(std::size_t slot) const;
    // Applies at once while the game is idle (human_ready / finished); handing the waiting player
    // to the AI then starts its computer turn immediately. Mid-turn (rolling, moving, a decision)
    // the change waits for the next turn boundary. Consumes no randomness by itself; returns
    // false for non-human slots or out-of-range values.
    bool set_ai_settings(std::size_t slot, const AiSettings &settings);
    void tick(unsigned elapsed_ms);
    [[nodiscard]] std::vector<GameEvent> drain_events();
    // Additive presentation queue, in emission order (see EffectEvent).
    [[nodiscard]] std::vector<EffectEvent> drain_effects();

    // Saving during an animation/decision is rejected, so a reload never loses
    // an in-flight action. World handles all original byte-layout serialization.
    [[nodiscard]] bool can_save() const noexcept;
    [[nodiscard]] SaveFile export_save() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rich4
