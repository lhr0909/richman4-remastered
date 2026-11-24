// SPDX-License-Identifier: GPL-3.0-or-later
#include "rich4/game.hpp"
#include "rich4/auction.hpp"
#include "rich4/board_renderer.hpp"
#include "rich4/calendar.hpp"
#include "rich4/cards.hpp"
#include "rich4/direction.hpp"
#include "rich4/economy.hpp"
#include "rich4/events.hpp"
#include "rich4/gods.hpp"
#include "rich4/holidays.hpp"
#include "rich4/inventory.hpp"
#include "rich4/legacy_float.hpp"
#include "rich4/lottery.hpp"
#include "rich4/random.hpp"
#include "rich4/services.hpp"
#include "rich4/special_actors.hpp"
#include "rich4/stocks.hpp"
#include "rich4/stocks_ai.hpp"
#include "rich4/tools.hpp"
#include "rich4/tools_ai.hpp"
#include "rich4/trading.hpp"
#include "rich4/world.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace rich4 {
namespace {
std::int32_t add32(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t subtract32(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
std::int32_t multiply32(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::string player_name(std::size_t slot) {
    return "玩家" + std::to_string(slot + 1);
}
constexpr unsigned roll_duration = 350;
constexpr unsigned step_duration = 160;
struct NeedChoice {
    GameDecision decision;
    std::size_t key;
};
struct UnsupportedRule {
    std::string message;
};
} // namespace

struct GameSession::Impl {
    World world;
    Random random;
    GamePhase phase = GamePhase::human_ready;
    std::optional<GameDecision> decision;
    std::optional<MoveAnimation> animation;
    std::vector<std::uint8_t> faces;
    std::vector<GameEvent> events;
    std::vector<EffectEvent> effects; // Presentation only; truncated with events on rule replay.
    std::optional<CardTarget> chosen_card_target; // Last card target answered through choose_target.
    bool mine_pending = false;                    // gods::touch is resolving a mine (explosion picture).
    // Card being played (presentation only): its monster/demolish picture plays between the user's
    // and the owner's speech bubbles (rich4_card_guaishouka/chaichuka), else after the rule.
    std::optional<CardUse> card_in_use;
    bool card_picture_shown = false;
    unsigned phase_elapsed = 0;
    int remaining = 0;
    std::uint16_t next_node = 0;
    std::uint64_t days = 0;
    std::optional<std::size_t> winner;
    bool resume_movement = false, bank_resume_encounter = false;
    std::map<std::size_t, int> event_choices;
    std::function<void()> pending_continuation;
    std::size_t pending_choice_key = 0;
    unsigned roulette_cursor = 0, roulette_elapsed = 0, roulette_kind = 0;
    std::size_t roulette_facility = 0;
    bool rescue_hospital = false;
    std::size_t choice_sequence = 0;
    std::optional<services::Shop> shop;
    std::uint16_t shop_property = 0;
    unsigned magic_category = 0;
    bool last_action_accepted = false;
    GamePhase unsupported_resume = GamePhase::advancing_turn;
    std::optional<unsigned> forced_roll;
    std::array<std::optional<World>, 4> previous_turn;
    std::map<std::size_t, std::uint32_t> choice_random_states;
    std::optional<gods::MoneySpinner> money_spinner;
    Random spinner_random{0};
    unsigned spinner_elapsed = 0;
    CardAiMemory card_ai_memory{};
    bool property_pending = false, purchased_property = false, research_pending = false;
    bool property_landing = false; // presentation: the 8-tick hold after a property landing (pause)
    bool interactive_mini_games = false; // RICH4.CFG[1]; off = closed mode (replay harness)
    // AI settings (託管) requested mid-turn; applied at the next turn boundary (next_turn).
    std::array<std::optional<AiSettings>, 4> pending_ai_settings;
    // Presentation only (player_costume): gown 19 / stripes 20 per player, and the sprite inputs
    // (traffic, sleep flags) it was put on with; any change is an original sprite update that ends it.
    std::array<std::uint8_t, 4> costumes{};
    std::array<std::uint32_t, 4> costume_keys{};

    explicit Impl(World value, std::uint32_t seed)
        : world(std::move(value)),
          random(world.has_initial_random_state() ? world.initial_random_state() : seed) {
        if (world.players.empty() || world.players.size() > 4 ||
            world.header.current_player >= world.players.size())
            throw std::invalid_argument("invalid native game player table");
        if (!calendar::is_valid({world.header.date.year, world.header.date.month, world.header.date.day}))
            throw std::invalid_argument("invalid native game date");
        for (const auto &player : world.players)
            if (player.state.node_id >= world.nodes.size() || player.state.last_node_id >= world.nodes.size())
                throw std::invalid_argument("player node outside native world");
        if (!world.has_initial_random_state() && check_game_end())
            return;
        if (world.has_initial_random_state()) {
            for (const auto kind : {1U, 3U, 5U, 7U, 9U, 11U, 13U, 14U})
                place_existing_object(kind - 1, random_free_node());
            stocks::refresh_trade_limits(world, random);
        }
        start_turn(false);
    }

    std::size_t actor() const {
        return world.header.current_player;
    }
    PlayerState &player() {
        return world.players[actor()].state;
    }
    const PlayerState &player() const {
        return world.players[actor()].state;
    }
    bool human() const {
        return player().controller_flags == 1;
    }
    PlayerStatus &status() {
        return world.players[actor()].status;
    }
    const PlayerStatus &status() const {
        return world.players[actor()].status;
    }
    void emit(GameEventKind kind, std::string message, std::int32_t amount = 0) {
        events.push_back({kind, actor(), amount, player().node_id, std::move(message)});
    }
    // ---- Presentation effects (docs/ui/effects.md §9): read-only, never draw randomness. ----
    void effect(EffectKind kind, std::size_t slot, EffectPoint target, std::uint16_t property = 0) {
        const auto &state = world.players.at(slot).state;
        effects.push_back({kind, slot, state.node_id, target, std::nullopt, 0, {}, property, events.size()});
    }
    // Board movies: `from` marks the actor's old point when the movie shows them leaving.
    void board_movie(EffectKind kind, std::size_t slot, EffectPoint target, std::optional<EffectPoint> from,
                     std::uint8_t variant = 0) {
        effect(kind, slot, target);
        effects.back().from = from;
        effects.back().object_kind = variant;
    }
    // Original board draw (fcn_0040829d): a player is drawn when placed and not away/confined.
    bool on_board(std::size_t slot) const {
        const auto &entry = world.players.at(slot);
        const auto &value = entry.status;
        return entry.state.controller_flags && (entry.state.x || entry.state.y) &&
               (!(value.days_in_hotel | value.days_disappearing | value.days_in_prison | value.days_in_hospital) ||
                (entry.state.controller_flags & 0x20U));
    }
    std::uint32_t costume_key(std::size_t slot) const {
        const auto &entry = world.players.at(slot);
        return entry.state.traffic_method | (entry.status.days_sleep_walking ? 0x100U : 0U) |
               (entry.status.days_sleeping ? 0x200U : 0U);
    }
    void end_costume(std::size_t slot) { // _rich4_update_player_sprite
        if (slot < costumes.size())
            costumes[slot] = 0;
    }
    void refresh_costumes() {
        for (std::size_t slot = 0; slot < std::min(costumes.size(), world.players.size()); ++slot)
            if (costumes[slot] && costume_keys[slot] != costume_key(slot))
                costumes[slot] = 0;
    }
    EffectPoint actor_point(std::size_t slot) const {
        const auto &state = world.players.at(slot).state;
        return {state.x, state.y};
    }
    EffectPoint property_point(std::uint16_t code) const {
        const auto point = instance_position(code);
        return {point.x, point.y};
    }
    unsigned property_level(std::uint16_t code) const {
        if (code > 2000 && code < 4000 && code - 2000U < world.lands.size())
            return world.lands[code - 2000U].economy.level;
        if (code > 4000 && code < 6000 && code - 4000U < world.facilities.size())
            return world.facilities[code - 4000U].level;
        return 0;
    }
    void sparkle_if_top_floor(std::uint16_t code, unsigned before) {
        if (before < 5 && property_level(code) == 5)
            effect(EffectKind::top_floor_sparkle, actor(), property_point(code), code);
    }
    // Tool pictures that are not driven by a tool callback: roadblock/mine/time bomb throws.
    void tool_effect(unsigned tool, std::uint16_t target) {
        if (tool < 2 || tool > 4 || !target || target >= world.nodes.size())
            return;
        const auto &node = world.nodes[target];
        EffectEvent event{EffectKind::object_throw, actor(), target, {node.x, node.y}, actor_point(actor()),
                          static_cast<std::uint8_t>(tool + 14), {}, 0, events.size()};
        effects.push_back(std::move(event));
    }
    // Card pictures: demolish (12) and monster (11) on a property target.
    void card_effect(std::uint8_t card, std::optional<CardTarget> target) {
        if (card != 11 && card != 12)
            return;
        if (!target)
            target = chosen_card_target;
        if (!target || target->kind != CardTargetKind::property)
            return;
        effect(card == 12 ? EffectKind::demolish : EffectKind::monster, actor(), property_point(target->value),
               target->value);
    }
    // AI targeted cards: the card sprite (Data 415) flies from the user to the target first
    // (rich4_card_*.asm, _rich4_animate_object(0, ..., 100), skipped for a human user).
    void card_throw(const CardUse &use) {
        static constexpr std::array<std::uint8_t, 16> flying{4, 5, 9, 10, 11, 12, 13, 14, 16, 17, 23, 26, 27, 28, 29, 30};
        if (human() || !use.target || std::find(flying.begin(), flying.end(), use.card_id) == flying.end())
            return;
        std::optional<EffectPoint> point;
        const auto value = use.target->value;
        if (use.target->kind == CardTargetKind::actor && value < world.players.size())
            point = actor_point(value);
        else if (use.target->kind == CardTargetKind::property)
            point = property_point(value);
        else if (use.target->kind == CardTargetKind::object && value && value <= world.objects.size()) {
            const auto node = world.objects[value - 1U].node_id;
            if (node && node < world.nodes.size())
                point = EffectPoint{world.nodes[node].x, world.nodes[node].y};
        }
        if (!point)
            return;
        effect(EffectKind::card_throw, actor(), *point, use.target->kind == CardTargetKind::property ? value : 0);
        effects.back().from = actor_point(actor());
    }
    // Presentation-only event for the original message boxes and announcement screens.
    GameEvent &announce(Announcement kind, std::size_t slot, std::uint8_t id = 0) {
        GameEvent event{GameEventKind::notice, slot, 0, world.players.at(slot).state.node_id, {}};
        event.announcement = kind;
        event.announcement_id = id;
        events.push_back(std::move(event));
        return events.back();
    }
    // Card/tool speech bubble (_rich4_player_say with a card or tool table line). Like the
    // original, an absent speaker (disappeared, sleepwalking, hibernating) says nothing.
    void speak(std::size_t slot, SpeechTable table, unsigned line, std::uint8_t mood) {
        if (slot >= world.players.size())
            return;
        const auto &value = world.players[slot].status;
        if (value.days_disappearing || value.days_sleep_walking || value.days_sleeping)
            return;
        GameEvent event{GameEventKind::notice, slot, 0, world.players[slot].state.node_id, {}};
        event.speech_table = table;
        event.speech_slot = static_cast<std::uint8_t>(line);
        event.speech_mood = mood;
        events.push_back(std::move(event));
    }
    void card_speech(std::size_t slot, unsigned row, std::uint8_t card, std::uint8_t mood) {
        speak(slot, SpeechTable::card, row * 30 + card - 1U, mood);
    }
    void begin_card(const CardUse &use) {
        chosen_card_target.reset();
        card_in_use = use;
        card_picture_shown = false;
    }
    void card_picture() {
        if (!card_in_use || card_picture_shown)
            return;
        card_picture_shown = true;
        card_effect(card_in_use->card_id, card_in_use->target);
    }
    void voice(std::size_t slot, std::uint8_t category) {
        events.push_back(
            {GameEventKind::notice, slot, 0, world.players.at(slot).state.node_id, {}, category});
    }
    // AUDIO: presentation-only Effect.mkf cue (GameEvent::sound); never draws randomness.
    void cue(std::size_t slot, std::uint8_t sound) {
        GameEvent event{GameEventKind::notice, slot, 0,
                        slot < world.players.size() ? world.players[slot].state.node_id : std::uint16_t(0), {}};
        event.sound = sound;
        events.push_back(std::move(event));
    }
    // AUDIO: _rich4_check_pay_toll_to_owner (core_actions.asm:5441) shows "...免收%s！" for 1500 ms,
    // then the visitor says event line 13. Presentation only: the caller already decided (with
    // the same conditions, original order kept here) that no toll is due.
    void announce_toll_waived(const PlayerStatus &owner, std::size_t owner_slot, std::uint8_t temporary,
                              std::uint16_t property) {
        const std::uint8_t reason = (temporary & 0xf0U) && (temporary & 0x0fU) ? 0
                                    : owner.allied_player == actor() + 1     ? 1
                                    : owner.god_info == 15                   ? 2
                                    : owner.days_in_hotel                    ? 3
                                    : owner.days_disappearing                ? 4
                                    : owner.days_in_prison                   ? 5
                                    : owner.days_in_hospital                 ? 6
                                    : owner.days_sleeping                    ? 7
                                                                             : 8;
        auto &box = announce(Announcement::toll_waived, actor(), reason);
        box.subject = static_cast<std::int32_t>(owner_slot);
        box.property = property;
        voice(actor(), 13);
    }
    void amount_voice(std::size_t slot, std::int32_t amount, unsigned category) {
        if (amount <= 0)
            return;
        if (amount >= multiply32(9000, world.header.price_index))
            voice(slot, static_cast<std::uint8_t>(category));
        else if (amount >= multiply32(5000, world.header.price_index))
            voice(slot, static_cast<std::uint8_t>(category + (random.next() & 1U)));
        else if (category != 6 || amount >= multiply32(2000, world.header.price_index))
            voice(slot, static_cast<std::uint8_t>(category + 2));
    }
    void absence_voice(std::size_t slot, int days_left) {
        if (days_left > 6)
            voice(slot, 3);
        else if (days_left > 3)
            voice(slot, static_cast<std::uint8_t>(3 + (random.next() & 1U)));
        else if (days_left > 0)
            voice(slot, 5);
    }
    void set_phase(GamePhase next) {
        phase = next;
        phase_elapsed = 0;
    }
    // core_actions loc_0041b0b3 -> fcn_0044101d: after landing on an owned, built, unsealed
    // Research Institute (type 4) while not sleepwalking, a human chooses tool 1..level for 5
    // turns (right-click cancels); a computer/託管 player develops tool `level`.
    bool open_research_institute() {
        if (!player().controller_flags || status().days_sleep_walking)
            return false;
        const auto code = world.nodes.at(player().node_id).type_and_index;
        if (code <= 4000 || code >= 6000)
            return false;
        const auto id = static_cast<std::size_t>(code - 4000);
        if (!id || id >= world.facilities.size())
            return false;
        auto &entry = world.facilities[id];
        if (entry.owner_code != actor() + 1 || entry.facility_type != 4 || !entry.level ||
            (entry.temporary_state & 0x0fU))
            return false;
        const auto max_slot = std::min<unsigned>(entry.level, 5U);
        if (!human()) {
            entry.research_tool = static_cast<std::uint8_t>(max_slot);
            entry.research_days() = 5;
            emit(GameEventKind::notice,
                 "研究所开始研发" + std::string(services::tool_names.at(max_slot + 7U)),
                 static_cast<std::int32_t>(max_slot + 8U));
            return false;
        }
        std::vector<DecisionOption> options;
        options.reserve(max_slot + 1U);
        for (unsigned slot = 1; slot <= max_slot; ++slot)
            options.push_back({std::string(services::tool_names.at(slot + 7U)), static_cast<std::int32_t>(slot)});
        options.push_back({"取消", -1});
        decision = GameDecision{DecisionKind::research_tool,
                                actor(),
                                static_cast<std::uint16_t>(id),
                                static_cast<std::int32_t>(max_slot),
                                "請選擇欲開發道具",
                                std::move(options)};
        set_phase(GamePhase::awaiting_decision);
        return true;
    }
    void finish_turn() {
        if (phase == GamePhase::finished)
            return;
        if (property_pending && phase != GamePhase::finished) {
            property_pending = false;
            const bool purchased = purchased_property;
            purchased_property = false;
            run_rule([this, purchased] {
                if (purchased)
                    check_god(gods::after_successful_purchase(world, random, actor(), player().node_id,
                                                              god_callbacks()));
                check_god(gods::after_property(world, actor(), player().node_id, god_callbacks()));
                construction_vehicle_demolish();
            });
            return;
        }
        if (research_pending && phase != GamePhase::finished) {
            research_pending = false;
            if (open_research_institute())
                return;
        }
        decision.reset();
        animation.reset();
        if (property_landing) { // the landing handler returned 0x88: 8 ticks before the next player
            property_landing = false;
            pause(8);
        }
        set_phase(GamePhase::advancing_turn);
    }
    // Presentation-only wait of `ticks` movement-state ticks (EffectKind::pause).
    void pause(std::uint8_t ticks) {
        effect(EffectKind::pause, actor(), actor_point(actor()));
        effects.back().object_kind = ticks;
    }
    void unsupported(std::string description, GamePhase resume = GamePhase::advancing_turn) {
        unsupported_resume = resume;
        emit(GameEventKind::unsupported_event, description);
        if (human()) {
            // The developer description stays in the event log only; the player sees a
            // neutral Traditional notice (docs/audit/rules-text-hover.md §2).
            decision = GameDecision{DecisionKind::notice, actor(), 0, 0, "本次事件略過。", {{"繼續", 0}}};
            set_phase(GamePhase::awaiting_decision);
        } else
            finish_turn();
    }
    void spawn_if_needed() {
        auto &state = player();
        if (state.x != 0)
            return;
        std::vector<std::uint16_t> candidates;
        for (std::size_t i = 1; i < world.nodes.size(); ++i) {
            const auto &node = world.nodes[i];
            if (!(node.flags & 0x80ffff00U) && std::any_of(node.neighbors.begin(), node.neighbors.end(),
                                                           [](auto link) { return link != 0; })) {
                if (i > 255)
                    throw std::runtime_error("original spawn candidate exceeds byte node ID");
                candidates.push_back(static_cast<std::uint16_t>(i));
            }
        }
        if (candidates.empty())
            throw std::runtime_error("world has no unoccupied spawn node");
        state.node_id = candidates[random.next() % candidates.size()];
        const auto &node = world.nodes[state.node_id];
        std::vector<std::uint16_t> previous;
        for (auto neighbor : node.neighbors)
            if (neighbor) {
                if (neighbor >= world.nodes.size())
                    throw std::runtime_error("spawn neighbor outside table");
                previous.push_back(neighbor);
            }
        state.last_node_id = previous[random.next() % previous.size()];
        state.x = static_cast<std::uint16_t>(node.x);
        state.y = static_cast<std::uint16_t>(node.y);
        const auto &last = world.nodes[state.last_node_id];
        state.direction = direction::from_delta(node.x - last.x, node.y - last.y);
        world.nodes[state.node_id].flags |= 0x100U << actor();
        // fcn_00418c55: the new player parachutes in (Data 559 + character), camera on the node.
        board_movie(EffectKind::parachute, actor(), actor_point(actor()), std::nullopt, state.character);
    }
    static void age_status(std::uint8_t &value) {
        value &= 0x7fU;
        if (value && --value == 0)
            value = 0x80;
    }
    void update_status() {
        auto &value = status();
        const bool left_building =
            (value.days_in_hotel | value.days_in_prison | value.days_in_hospital) & 0x80U;
        // Presentation: fcn_0040d4e5 replays the absence movie (bit 0x40: UFO, else airplane).
        const auto returning = (value.days_disappearing & 0x80U) ? std::optional<std::uint8_t>(value.days_disappearing)
                                                                  : std::nullopt;
        if ((value.days_in_prison | value.days_in_hospital) & 0x80U)
            end_costume(actor()); // released through _rich4_release_player_from_*: sprite update
        bool released = false;
        for (auto *days_left : {&value.days_in_hotel, &value.days_disappearing, &value.days_in_prison,
                                &value.days_in_hospital}) {
            if (*days_left & 0x80U) {
                *days_left = 0;
                released = true;
            } else if (*days_left) {
                --*days_left;
                const auto mask = days_left == &value.days_disappearing ? 0x3fU : 0x7fU;
                if ((*days_left & mask) == 0) // loc_0041c8d5 keeps the UFO mode bit (0x40)
                    *days_left = static_cast<std::uint8_t>(0x80U | (*days_left & ~mask & 0x7fU));
            }
        }
        if (released) {
            if (!value.days_in_prison)
                world.prison_roster[actor()] = 0;
            if (!value.days_in_hospital)
                world.hospital_roster[actor()] = 0;
            auto &state = player();
            const auto &node = world.nodes.at(state.node_id);
            state.controller_flags &= 0x0fU;
            state.direction = value.saved_direction < 8
                                  ? value.saved_direction
                                  : direction::from_delta(node.x - state.x, node.y - state.y);
            state.x = static_cast<std::uint16_t>(node.x);
            state.y = static_cast<std::uint16_t>(node.y);
            world.nodes[state.node_id].flags |= 0x100U << actor();
            if (returning) { // the player reappears at the commit frame
                end_costume(actor());
                board_movie((*returning & 0x40U) ? EffectKind::ufo : EffectKind::airplane, actor(),
                            actor_point(actor()), std::nullopt, 1);
            }
        }
        if (!(value.days_in_hotel | value.days_disappearing | value.days_in_prison |
              value.days_in_hospital)) {
            if (value.days_sleep_walking & 0x80U) {
                const auto previous = value.flags_102 & 3U;
                if (previous == 3 || (previous == 1 && player().tool_amounts[4]) ||
                    (previous == 2 && player().tool_amounts[5])) {
                    player().traffic_method = value.flags_102;
                    player().dice_count = value.flags_103;
                    if (previous == 1 || previous == 2)
                        --player().tool_amounts[previous + 3U];
                } else {
                    player().traffic_method = 0;
                    player().dice_count = 1;
                }
            }
            age_status(value.days_sleeping);
            age_status(value.days_sleep_walking);
            age_status(value.days_tortoise_walking);
        }
        if ((value.allied_days & 0x80U) != 0) {
            if (value.allied_player && value.allied_player <= world.players.size()) {
                auto &partner = world.players[value.allied_player - 1].status;
                partner.allied_player = partner.allied_days = 0;
            }
            value.allied_player = 0;
        } else if (value.allied_days && value.allied_player && value.allied_player <= world.players.size()) {
            update_hostility(actor(), value.allied_player - 1, -multiply32(20, world.header.price_index));
            update_hostility(value.allied_player - 1, actor(), -multiply32(20, world.header.price_index));
        }
        age_status(value.days_stopping);
        age_status(value.days_rejected_by_bank);
        age_status(value.days_bank_no_loans);
        age_status(value.allied_days);
        age_status(value.days_assurance);
        if (value.god_info) {
            auto &god = world.objects.at(value.god_info - 1);
            if (--god.remaining_days == 0)
                check_god(gods::detach(world, actor(), god_callbacks()));
        }
        if ((player().traffic_method & 3U) == 3) {
            player().traffic_method = static_cast<std::uint8_t>(player().traffic_method - 4);
            if (!(player().traffic_method & 0xfcU)) {
                const auto previous = value.flags_100 & 3U;
                if ((previous == 1 && player().tool_amounts[4]) ||
                    (previous == 2 && player().tool_amounts[5])) {
                    player().traffic_method = value.flags_100;
                    player().dice_count = value.flags_101;
                    --player().tool_amounts[previous + 3U];
                } else {
                    player().traffic_method = 0;
                    player().dice_count = 1;
                }
            }
        }
        if (left_building) {
            check_god(gods::after_property(world, actor(), player().node_id, god_callbacks()));
            construction_vehicle_demolish();
        }
        tick_research_institutes();
    }
    // fcn_0041c84f (0x41cd8c..0x41ce38): tick down active Research Institutes owned by the
    // current player at turn start; cancel if downgraded below research_tool; grant tool 9..13
    // and announce "%s開發完成！" when research_days hits 0.
    void tick_research_institutes() {
        if (phase == GamePhase::finished || !player().controller_flags)
            return;
        const auto owner = static_cast<std::uint8_t>(actor() + 1);
        for (std::size_t id = 1; id < world.facilities.size(); ++id) {
            auto &entry = world.facilities[id];
            if (entry.facility_type != 4 || !entry.research_days() || entry.owner_code != owner)
                continue;
            if (!entry.research_tool || entry.research_tool > 5 || entry.research_tool > entry.level) {
                entry.research_days() = 0;
                continue;
            }
            if (--entry.research_days() != 0)
                continue;
            const auto tool_id = static_cast<std::uint8_t>(entry.research_tool + 8U);
            auto &box = announce(Announcement::research_complete, actor(), tool_id);
            box.property = static_cast<std::uint16_t>(4000 + id);
            (void)receive_tool(player().tool_amounts, world.tool_supply, tool_id);
            emit(GameEventKind::notice,
                 std::string(services::tool_names.at(tool_id - 1U)) + "開發完成！",
                 static_cast<std::int32_t>(tool_id));
        }
    }
    void start_turn(bool update = true) {
        property_landing = false;
        research_pending = false;
        spawn_if_needed();
        decision.reset();
        animation.reset();
        remaining = 0;
        if (update) {
            run_rule(
                [this] {
                    stocks::refresh_trade_limits(world, random);
                    collect_due_loan();
                    if (phase != GamePhase::finished && player().controller_flags)
                        update_status();
                },
                true, false, [this] { enter_turn(); });
        } else
            enter_turn();
    }
    void enter_turn() {
        if (phase == GamePhase::finished)
            return;
        if (!player().controller_flags) {
            finish_turn();
            return;
        }
        emit(GameEventKind::turn_started, player_name(actor()) + "的回合");
        const auto &value = status();
        if (value.days_in_hotel || value.days_disappearing || value.days_in_prison ||
            value.days_in_hospital || value.days_sleeping) {
            // Original turn-start status (rich4.asm:6600-6757): bubble 19/20/21, then the box.
            const int status_kind = value.days_in_hotel        ? 0
                                    : value.days_disappearing ? 1
                                    : value.days_in_prison    ? 2
                                    : value.days_in_hospital  ? 3
                                                              : 4;
            const unsigned raw = status_kind == 0   ? value.days_in_hotel
                                 : status_kind == 1 ? value.days_disappearing & 0x3fU
                                 : status_kind == 2 ? value.days_in_prison
                                 : status_kind == 3 ? value.days_in_hospital
                                                    : value.days_sleeping;
            if (status_kind >= 2)
                voice(actor(), status_kind == 2 ? 19 : status_kind == 3 ? 20 : 21);
            announce(Announcement::absent_status, actor(), static_cast<std::uint8_t>(status_kind)).value =
                static_cast<std::int32_t>(raw & 0x7fU);
            emit(GameEventKind::notice, player_name(actor()) + "本回合休息");
            finish_turn();
        } else if (human() && !value.days_sleep_walking)
            set_phase(GamePhase::human_ready);
        else if (value.days_sleep_walking || (player().controller_flags & 0x30U))
            begin_roll();
        else
            begin_ai_turn();
    }
    void ai_dice_count() {
        const auto traffic = player().traffic_method & 3U;
        if (traffic != 1 && traffic != 2)
            return;
        player().dice_count = static_cast<std::uint8_t>(traffic + 1);
        if (status().attached_bomb) {
            if (world.objects.at(status().attached_bomb - 1).remaining_days < 15)
                player().dice_count = 1;
            return;
        }
        const auto preview = lookahead(actor(), 5);
        unsigned good = 0, bad = 0;
        for (auto id : preview.nodes) {
            const auto code = world.nodes.at(id).type_and_index;
            unsigned owner = 0;
            bool property = false;
            if (code > 2000 && code < 4000) {
                owner = world.lands.at(code - 2000).economy.owner_code;
                property = true;
            } else if (code > 4000 && code < 6000) {
                owner = world.facilities.at(code - 4000).owner_code;
                property = true;
            }
            if (property) {
                if (!owner || owner == actor() + 1)
                    ++good;
                else
                    ++bad;
            }
        }
        if (good >= 2 && bad <= 1)
            player().dice_count = 1;
        else if (traffic == 2 && !good && bad > 2)
            player().dice_count = static_cast<std::uint8_t>(2 + (random.next() & 1U));
    }
    void begin_ai_turn() {
        run_rule(
            [this] {
                const bool closed = card_callbacks().market_closed();
                for (const auto &trade : stocks_ai::buy(world, random, actor(), closed))
                    emit(GameEventKind::notice,
                         "电脑买入股票" + std::to_string(trade.stock + 1) + "，" +
                             std::to_string(trade.quantity) + "股",
                         trade.value);
                for (const auto &trade : stocks_ai::sell(world, random, actor(), closed))
                    emit(GameEventKind::notice,
                         "电脑卖出股票" + std::to_string(trade.stock + 1) + "，" +
                             std::to_string(trade.quantity) + "股",
                         trade.value);
                reconcile_financing(false);
                if (phase == GamePhase::finished || !player().controller_flags)
                    return;
                const auto offers = trading::ai_turn(world, random, actor());
                if (offers.posted)
                    emit(GameEventKind::notice, "电脑发布出售报价");
                if (offers.purchased)
                    emit(GameEventKind::notice, "电脑购买出售物品", offers.purchased->offer.price);
                reconcile_financing(false);
                if (phase == GamePhase::finished || !player().controller_flags)
                    return;
                if (random.next() & 1U) {
                    const auto callbacks = card_callbacks();
                    if (const auto request =
                            choose_ai_card(world, random, actor(), callbacks, &card_ai_memory)) {
                        begin_card(*request);
                        card_throw(*request);
                        const auto result = apply_card(world, random, actor(), *request, callbacks);
                        if (result.status == CardUseStatus::missing_callback)
                            throw UnsupportedRule{result.message};
                        if (result.status == CardUseStatus::used) {
                            card_picture();
                            if ((request->card_id == 24 || request->card_id == 25) && request->target &&
                                request->target->kind == CardTargetKind::stock) {
                                auto &box = announce(Announcement::stock_card, actor(), request->card_id);
                                box.subject = request->target->value;
                            }
                            emit(GameEventKind::notice,
                                 std::string("电脑使用") +
                                     std::string(services::card_names.at(request->card_id - 1)));
                            events.back().card = request->card_id;
                        }
                    }
                } else {
                    tools_ai::Callbacks callbacks;
                    callbacks.nearby_instances = [this](auto slot) {
                        const auto &p = world.players.at(slot).state;
                        return instance_codes(p.x, p.y, -1);
                    };
                    const auto selected = tools_ai::choose(world, random, actor(), callbacks);
                    if (!selected.supported)
                        throw UnsupportedRule{selected.message};
                    if (selected.decision) {
                        const auto &request = *selected.decision;
                        // The original announces the tool before calling its function (presentation only).
                        announce(Announcement::tool_use, actor(), static_cast<std::uint8_t>(request.tool));
                        const auto result = tools::use(world, random, actor(), request.tool, request.target,
                                                       request.destination, tool_callbacks());
                        if (!result.supported)
                            throw UnsupportedRule{result.message};
                        if (result.applied) {
                            tool_effect(request.tool, request.target);
                            emit(GameEventKind::notice,
                                 std::string("电脑使用") +
                                     std::string(services::tool_names.at(request.tool - 1)));
                        }
                    }
                }
            },
            true, false,
            [this] {
                if (phase == GamePhase::finished || phase == GamePhase::rolling || phase == GamePhase::moving)
                    return;
                if (!player().controller_flags || status().days_in_hotel || status().days_disappearing ||
                    status().days_in_prison || status().days_in_hospital || status().days_sleeping ||
                    status().days_sleep_walking) {
                    finish_turn();
                    return;
                }
                ai_dice_count();
                begin_roll();
            });
    }
    void begin_roll() {
        if ((player().controller_flags & 1U) && !(player().controller_flags & 0x30U))
            previous_turn[actor()] = world;
        faces.clear();
        remaining = 0;
        if (player().controller_flags & 0x30U)
            remaining = 1;
        else if (status().days_stopping) {
            pause(2); // fcn_0040dd1f: state 0 with counter 2 (rich4_player_utils.asm:1020)
            land();
            return;
        } else if (forced_roll) {
            remaining = static_cast<int>(*forced_roll);
            faces.push_back(static_cast<std::uint8_t>(*forced_roll));
            forced_roll.reset();
        } else if (status().days_tortoise_walking)
            remaining = 1;
        else {
            // Original fcn_00419572: exactly one rand per die, modulo 6 plus one.
            for (unsigned i = 0; i < player().dice_count; ++i) {
                auto face = static_cast<std::uint8_t>(random.next() % 6 + 1);
                faces.push_back(face);
                remaining += face;
            }
        }
        emit(GameEventKind::dice_rolled, player_name(actor()) + "前进" + std::to_string(remaining) + "步",
             remaining);
        if (!faces.empty()) { // fcn_00419572: values first, then the Panel 4..6 dice movie.
            effect(EffectKind::dice_roll, actor(), actor_point(actor()));
            effects.back().dice = faces;
        }
        set_phase(GamePhase::rolling);
    }
    void start_step() {
        auto &state = player();
        const auto &node = world.nodes.at(state.node_id);
        std::vector<std::uint16_t> choices;
        for (unsigned i = 0; i < node.neighbors.size(); ++i) {
            const auto candidate = node.neighbors[i];
            if (candidate && candidate != state.last_node_id && !(node.flags & (0x40000000U >> i))) {
                if (candidate >= world.nodes.size())
                    throw std::runtime_error("map link outside node table");
                choices.push_back(candidate);
            }
        }
        // Original fcn_0040c05c consumes rand even for a single candidate.
        next_node = choices.empty() ? state.last_node_id : choices[random.next() % choices.size()];
        if (next_node >= world.nodes.size())
            throw std::runtime_error("fallback node outside table");
        const auto &next = world.nodes[next_node];
        animation = MoveAnimation{actor(), state.x, state.y, next.x, next.y, 0, step_duration};
        set_phase(GamePhase::moving);
    }
    void complete_step() {
        auto &state = player();
        const auto old_node = state.node_id;
        const auto &next = world.nodes.at(next_node);
        world.nodes.at(old_node).flags &= ~(0x100U << actor());
        world.nodes.at(next_node).flags |= 0x100U << actor();
        state.last_node_id = old_node;
        state.node_id = next_node;
        state.direction = direction::from_delta(static_cast<std::int32_t>(next.x) - state.x,
                                                static_cast<std::int32_t>(next.y) - state.y);
        state.x = static_cast<std::uint16_t>(next.x);
        state.y = static_cast<std::uint16_t>(next.y);
        sync_attached_objects(actor());
        --remaining;
        animation.reset();
        emit(GameEventKind::moved, player_name(actor()) + "到达道路" + std::to_string(next_node));
        const auto object = static_cast<std::uint16_t>((next.flags >> 16U) & 255U);
        if (remaining > 0 && (next.flags & 255U) == 14 && !status().days_sleep_walking &&
            (!object || world.objects.at(object - 1).kind != 16)) {
            bank_resume_encounter = true;
            open_bank(true);
            return;
        }
        step_encounter();
    }
    void step_encounter() {
        const auto object =
            static_cast<std::uint16_t>((world.nodes.at(player().node_id).flags >> 16U) & 255U);
        run_rule(
            [this, object] {
                const auto occupants =
                    (world.nodes.at(player().node_id).flags >> 8U) & 15U & ~(1U << actor());
                if (occupants && remaining == 0) {
                    const auto other = static_cast<std::size_t>(std::countr_zero(occupants));
                    if (other < world.players.size() && !world.players[other].state.controller_flags) {
                        (void)pay(static_cast<int>(actor()), -1, multiply32(1000, world.header.price_index),
                                  0);
                        relocate_bankrupt(other);
                        if (phase == GamePhase::finished)
                            return;
                    }
                }
                if (status().attached_bomb) {
                    const auto bomb_id = status().attached_bomb;
                    auto &bomb = world.objects.at(bomb_id - 1);
                    if (--bomb.remaining_days == 0) {
                        remove_object(bomb_id);
                        change_building(world.nodes.at(player().node_id).type_and_index, false);
                        effect(EffectKind::explosion, actor(), actor_point(actor())); // Data 525
                        injure(actor());
                        add_confinement(actor(), 5, true);
                        remaining = 0;
                        return;
                    }
                    const auto mask = occupants;
                    if (mask) {
                        const auto other = static_cast<std::size_t>(std::countr_zero(mask));
                        if (other < world.players.size() && world.players[other].state.controller_flags &&
                            !world.players[other].status.attached_bomb) {
                            world.players[other].status.attached_bomb = bomb_id;
                            bomb.attached_player = static_cast<std::uint8_t>(other + 1);
                            status().attached_bomb = 0;
                        }
                    }
                }
                if (object) {
                    // Mine (kind 17) explosion picture: emitted from the hospital callback below.
                    mine_pending = world.objects.at(object - 1).kind == 17;
                    struct ClearMine {
                        bool &flag;
                        ~ClearMine() { flag = false; }
                    } clear_mine{mine_pending};
                    const auto kind = world.objects.at(object - 1).kind;
                    const auto result =
                        gods::touch(world, random, actor(), object, remaining == 0, god_callbacks());
                    check_god(result);
                    // rich4_player_core_actions: roadblock hit says tool line 14 (mood 1), a time
                    // bomb picked up says line 16 (mood 2); the mine's line 15 is in hospital below.
                    if (result.applied && kind == 16)
                        speak(actor(), SpeechTable::tool, 14, 1);
                    if (result.applied && kind == 18)
                        speak(actor(), SpeechTable::tool, 16, 2);
                    if (result.stop_movement)
                        remaining = 0;
                }
            },
            true, false,
            [this] {
                if (phase == GamePhase::finished)
                    return;
                if (status().days_in_hospital || status().days_in_prison || status().days_disappearing) {
                    finish_turn();
                    return;
                }
                if (remaining > 0)
                    start_step();
                else {
                    pause(5); // loc_0040d916: the walk ends, 5 ticks before the landing handler
                    land();
                }
            });
    }
    std::int32_t purchase_price(std::size_t land_id) const {
        const auto &land = world.lands.at(land_id);
        // Source offsets, rather than the misleading historical C field names:
        // +0x1c is base price; +0x1e is per building level.
        return multiply32(world.header.price_index,
                          add32(land.land_price, multiply32(land.economy.level, land.price_per_level)));
    }
    std::int32_t upgrade_price(std::size_t land_id) const {
        return multiply32(world.header.price_index, world.lands.at(land_id).price_per_level);
    }
    void buy(std::size_t land_id, bool upgrade) {
        auto &land = world.lands.at(land_id);
        const auto cost = upgrade ? upgrade_price(land_id) : purchase_price(land_id);
        if (cost < 0 || player().cash < cost) {
            finish_turn();
            return;
        }
        player().cash = subtract32(player().cash, cost);
        purchased_property = true;
        if (upgrade)
            ++land.economy.level;
        else {
            land.economy.owner_code = static_cast<std::uint8_t>(actor() + 1);
            land.expired_date = lease_expiry();
        }
        emit(upgrade ? GameEventKind::land_upgraded : GameEventKind::land_purchased,
             player_name(actor()) + (upgrade ? "升级房产" : "买下房产"), cost);
        if (upgrade)
            sparkle_if_top_floor(static_cast<std::uint16_t>(2000 + land_id), 4); // fcn_0040b0cd
        if (upgrade && land.economy.level == 5)
            voice(actor(), 15);
        else {
            unsigned matching = 0;
            for (const auto &other : world.lands)
                if (other.economy.owner_code == actor() + 1 &&
                    other.economy.street_name == land.economy.street_name)
                    ++matching;
            if (matching >= 3 && (!upgrade || random.next() % 3 == 0))
                voice(actor(), upgrade ? 17 : 16);
        }
        finish_turn();
    }
    void property(std::size_t land_id) {
        if (!land_id || land_id >= world.lands.size()) {
            unsupported("房产记录无效，暂时无法结算");
            return;
        }
        auto &land = world.lands[land_id];
        auto &economy = land.economy;
        if (!economy.owner_code || economy.owner_code == actor() + 1) {
            if (status().days_sleep_walking || status().god_info == 12) {
                finish_turn();
                return;
            }
            if (status().god_info == 7 || status().god_info == 8 || status().god_info == 15) {
                emit(GameEventKind::notice, "神明阻止这次房产交易");
                finish_turn();
                return;
            }
            const bool upgrade = economy.owner_code != 0;
            if (upgrade && (economy.chain_store || economy.level >= 5)) {
                finish_turn();
                return;
            }
            const auto cost = upgrade ? upgrade_price(land_id) : purchase_price(land_id);
            if (cost < 0 || player().cash < cost) {
                if (cost >= 0)
                    announce(Announcement::cash_insufficient, actor()).property =
                        static_cast<std::uint16_t>(2000 + land_id);
                finish_turn();
                return;
            }
            if (human()) {
                decision = GameDecision{upgrade ? DecisionKind::upgrade_land : DecisionKind::buy_land,
                                        actor(),
                                        static_cast<std::uint16_t>(land_id),
                                        cost,
                                        upgrade ? "升級這塊房產？" : "購買這塊房產？",
                                        {{upgrade ? "升級" : "購買", 1}, {"放棄", 0}}};
                set_phase(GamePhase::awaiting_decision);
            } else {
                const auto reserve =
                    multiply32(static_cast<std::int32_t>(std::min(world.header.initial_fund * 0.05, 7000.0)),
                               world.header.price_index);
                if (upgrade ||
                    static_cast<std::int64_t>(player().cash) + player().bank_balance - cost > reserve)
                    buy(land_id, upgrade);
                else
                    finish_turn();
            }
            return;
        }
        const auto owner = static_cast<std::size_t>(economy.owner_code - 1);
        if (owner >= world.players.size()) {
            unsupported("房产所有者无效，暂时无法结算");
            return;
        }
        const auto &owner_status = world.players[owner].status;
        if (owner_status.allied_player == actor() + 1 || owner_status.god_info == 15 ||
            owner_status.days_in_hotel || owner_status.days_disappearing || owner_status.days_in_prison ||
            owner_status.days_in_hospital || owner_status.days_sleeping || owner_status.days_sleep_walking ||
            ((land.temporary_state & 0xf0U) && (land.temporary_state & 0x0fU))) {
            announce_toll_waived(owner_status, owner, land.temporary_state, static_cast<std::uint16_t>(2000 + land_id));
            finish_turn();
            return;
        }
        std::vector<LandEconomyRecord> records;
        for (std::size_t i = 1; i < world.lands.size(); ++i)
            records.push_back(world.lands[i].economy);
        const auto street =
            economy.chain_store ? std::nullopt : std::optional<std::string_view>(economy.street_name);
        auto toll = land_toll(records, economy.owner_code, world.header.price_index, street);
        const auto ally_code = owner_status.allied_player;
        const auto ally_toll =
            ally_code ? land_toll(records, ally_code, world.header.price_index, street) : 0;
        if (land.temporary_state)
            toll = multiply32(toll, 2); // The ally's portion is not doubled.
        toll = add32(toll, ally_toll);
        const float ally_ratio =
            ally_code && toll ? (numeric::Extended::from(ally_toll) / numeric::Extended::from(toll)).single()
                              : 0.0F;
        if (status().god_info == 1)
            toll >>= 1;
        else if (status().god_info == 2)
            toll = 0;
        else if (status().god_info == 5)
            toll = add32(toll, toll >> 1);
        else if (status().god_info == 6)
            toll = multiply32(toll, 2);
        if (!toll) {
            finish_turn();
            return;
        }
        run_rule([this, land_id, owner, toll, ally_code, ally_toll, ally_ratio] {
            // Original hostility belongs to the arriving player and is recorded
            // before a free/frame card or death god can alter the payment.
            update_hostility(actor(), owner, subtract32(toll, ally_toll) / 100);
            if (ally_code)
                update_hostility(actor(), ally_code - 1U, ally_toll / 100);
            auto requested = toll;
            const auto payer = select_toll_payer(requested, static_cast<int>(owner));
            if (payer == owner || (ally_code && payer == ally_code - 1U))
                return;
            const auto ally_amount =
                ally_code
                    ? (numeric::Extended::from(requested) * numeric::Extended::from(ally_ratio)).integer()
                    : 0;
            pay_toll(owner, requested, payer,
                     ally_code ? std::optional<std::size_t>(ally_code - 1U) : std::nullopt, ally_amount,
                     static_cast<std::uint16_t>(2000 + land_id));
            world.lands.at(land_id).removed_building = static_cast<std::uint32_t>(requested);
        });
    }

    bool transaction_blocked() const {
        return status().god_info == 7 || status().god_info == 8 || status().god_info == 15;
    }
    bool ai_can_purchase(std::int32_t cost) const {
        const auto reserve =
            multiply32(static_cast<std::int32_t>(std::min(world.header.initial_fund * 0.05, 7000.0)),
                       world.header.price_index);
        return static_cast<std::int64_t>(player().cash) + player().bank_balance - cost > reserve;
    }
    void buy_facility(std::size_t id, int operation, unsigned type = 0) {
        auto &facility = world.facilities.at(id);
        const auto cost = multiply32(operation == 2 ? facility.price_per_level() : facility.land_price,
                                     world.header.price_index);
        // The original assigns the selected construction type before the god
        // veto, but leaves level and balance unchanged if the purchase is blocked.
        if (operation == 1)
            facility.facility_type = static_cast<std::uint8_t>(type);
        if (cost < 0 || player().cash < cost || transaction_blocked()) {
            finish_turn();
            return;
        }
        player().cash = subtract32(player().cash, cost);
        if (operation == 0) {
            facility.owner_code = static_cast<std::uint8_t>(actor() + 1);
            facility.expired_date = lease_expiry();
        } else
            ++facility.level;
        purchased_property = true;
        emit(operation ? GameEventKind::land_upgraded : GameEventKind::land_purchased,
             operation == 0   ? "购买设施用地"
             : operation == 1 ? "建设设施"
                              : "升级设施",
             cost);
        if (operation)
            sparkle_if_top_floor(static_cast<std::uint16_t>(4000 + id), 4);
        if (facility.level == 5)
            voice(actor(), 15);
        finish_turn();
    }
    static int roulette_value(unsigned kind, unsigned cursor) {
        static constexpr int rows[4][12] = {{1, -1, 0, -1, 1, -1, 2, -1, 3, -1, 2, -1},
                                            {-1, -1, 1, -1, -1, 4, -1, 3, -1, -1, 2, -1},
                                            {1, -1, 6, -1, 5, -1, 4, -1, 3, -1, 2, -1},
                                            {5, -1, 3, -1, 30, -1, 20, -1, 15, -1, 10, -1}};
        while (rows[kind][cursor % 12] < 0)
            ++cursor;
        return rows[kind][cursor % 12];
    }
    void charge_facility(std::size_t id, int multiplier) {
        run_rule([this, id, multiplier] { charge_facility_action(id, multiplier); });
    }
    void charge_facility_action(std::size_t id, int multiplier) {
        auto &facility = world.facilities.at(id);
        const auto owner = static_cast<std::size_t>(facility.owner_code - 1);
        std::int32_t toll = 0;
        if (facility.facility_type == 3) {
            const auto traffic = player().traffic_method & 3U;
            if (traffic == 0)
                return;
            unsigned dice_total = 0;
            for (auto face : faces)
                dice_total += face;
            if (faces.empty() && status().days_tortoise_walking)
                dice_total = 1;
            toll = multiply32(multiply32(static_cast<std::int32_t>(dice_total), traffic == 1 ? 500 : 1000),
                              world.header.price_index);
        } else {
            toll = multiply32(multiply32(facility.toll_by_level.at(facility.level), world.header.price_index),
                              multiplier);
            if (facility.temporary_state)
                toll = multiply32(toll, 2);
        }
        if (status().god_info == 1)
            toll >>= 1;
        else if (status().god_info == 2)
            toll = 0;
        else if (status().god_info == 5)
            toll = add32(toll, toll >> 1);
        else if (status().god_info == 6)
            toll = multiply32(toll, 2);
        if (!toll)
            return;
        update_hostility(actor(), owner, toll / 100);
        const auto payer = select_toll_payer(toll, static_cast<int>(owner), facility.facility_type == 1,
                                             facility.facility_type != 1);
        if (payer == owner)
            return;
        pay_toll(owner, toll, payer, std::nullopt, 0, static_cast<std::uint16_t>(4000 + id),
                 facility.facility_type, multiplier);
        facility.removed_building = static_cast<std::uint32_t>(toll);
        if (phase == GamePhase::finished)
            return;
        auto &paying_player = world.players[payer].state;
        auto &paying_status = world.players[payer].status;
        if (facility.facility_type == 1 && paying_player.controller_flags) {
            clear_absences(payer);
            paying_status.days_in_hotel = static_cast<std::uint8_t>(multiplier - 1);
            if (!paying_status.days_in_hotel)
                paying_status.days_in_hotel = 0x80;
            paying_status.total_winter_sleep_days =
                static_cast<std::uint8_t>(paying_status.total_winter_sleep_days + multiplier);
            paying_status.facility_id = static_cast<std::uint16_t>(id);
            paying_status.saved_direction = paying_player.direction;
            world.nodes.at(paying_player.node_id).flags &= ~(0x100U << payer);
            paying_player.x = facility.x;
            paying_player.y = facility.y;
            update_hostility(payer, owner, multiply32(multiply32(multiplier, 20), world.header.price_index));
            insure(payer, multiply32(multiply32(multiplier, 2000), world.header.price_index));
            absence_voice(payer, multiplier);
            emit(GameEventKind::notice, "在旅馆住宿" + std::to_string(multiplier) + "天", multiplier);
        }
    }
    void facility(std::size_t id) {
        if (!id || id >= world.facilities.size()) {
            unsupported("设施记录无效");
            return;
        }
        auto &entry = world.facilities[id];
        if (!entry.owner_code || entry.owner_code == actor() + 1) {
            if (status().days_sleep_walking || (!entry.owner_code && status().god_info == 12)) {
                finish_turn();
                return;
            }
            const int operation = !entry.owner_code ? 0 : entry.level == 0 ? 1 : 2;
            static constexpr unsigned maximum[5]{1, 5, 5, 1, 5};
            if (entry.facility_type >= 5 || (operation == 2 && entry.level >= maximum[entry.facility_type])) {
                finish_turn();
                return;
            }
            const auto cost = multiply32(operation == 2 ? entry.price_per_level() : entry.land_price,
                                         world.header.price_index);
            if (cost < 0 || player().cash < cost) {
                if (cost >= 0)
                    announce(Announcement::cash_insufficient, actor()).property =
                        static_cast<std::uint16_t>(4000 + id);
                finish_turn();
                return;
            }
            if (human()) {
                std::vector<DecisionOption> options;
                if (operation == 1) // fcn_00440aac slots 0..4; a human may build a 公園 (type 0, no toll)
                    options = {{"公園", 0}, {"旅館", 1}, {"百貨公司", 2}, {"加油站", 3}, {"研究所", 4}};
                else
                    options = {{operation == 0 ? "購買" : "升級", 1}, {"放棄", 0}};
                decision = GameDecision{operation == 0   ? DecisionKind::buy_facility
                                        : operation == 1 ? DecisionKind::construct_facility
                                                         : DecisionKind::upgrade_facility,
                                        actor(),
                                        static_cast<std::uint16_t>(id),
                                        cost,
                                        operation == 0   ? "購買設施用地？"
                                        : operation == 1 ? "請選擇設施類別" // 0x465289
                                                         : "升級設施？",
                                        std::move(options)};
                set_phase(GamePhase::awaiting_decision);
            } else if (operation != 0 || ai_can_purchase(cost)) {
                const unsigned type = operation == 1 ? static_cast<unsigned>(random.next() % 4 + 1) : 0U;
                buy_facility(id, operation, type);
            } else
                finish_turn();
            return;
        }
        if (entry.owner_code > world.players.size() || !entry.level || !entry.facility_type ||
            entry.facility_type >= 4) {
            finish_turn();
            return;
        }
        const auto &owner = world.players[entry.owner_code - 1].status;
        if (owner.allied_player == actor() + 1 || owner.god_info == 15 || owner.days_in_hotel ||
            owner.days_disappearing || owner.days_in_prison || owner.days_in_hospital ||
            owner.days_sleeping || owner.days_sleep_walking ||
            ((entry.temporary_state & 0xf0U) && (entry.temporary_state & 0x0fU))) {
            announce_toll_waived(owner, entry.owner_code - 1U, entry.temporary_state,
                                 static_cast<std::uint16_t>(4000 + id));
            finish_turn();
            return;
        }
        if (entry.facility_type == 3) {
            charge_facility(id, 1);
            return;
        }
        roulette_kind = entry.facility_type;
        roulette_facility = id;
        roulette_cursor = random.next() % 12;
        roulette_elapsed = 0;
        const SpinnerState wheel{static_cast<std::uint8_t>(entry.facility_type), {}, 1,
                                 static_cast<std::uint8_t>(roulette_cursor), entry.owner_code - 1};
        if (!human() || status().days_sleep_walking) {
            const auto multiplier = roulette_value(entry.facility_type, roulette_cursor + 51);
            roulette_effect(wheel, multiplier); // fcn_0043f7c6 spins for computers too
            charge_facility(id, multiplier);
            return;
        }
        decision = GameDecision{DecisionKind::roulette,
                                actor(),
                                static_cast<std::uint16_t>(id),
                                0,
                                entry.facility_type == 1 ? "轉盤決定住宿天數，按停止選定"
                                                         : "轉盤決定消費倍數，按停止選定",
                                {{"停止轉盤", 0}}};
        decision->spinner = wheel;
        set_phase(GamePhase::awaiting_decision);
    }
    void rescue(std::size_t target, bool hospital, bool by_human) {
        auto &roster = hospital ? world.hospital_roster : world.prison_roster;
        if (target >= roster.size() || !roster[target])
            return;
        const auto cost = target < 4 ? 30U : 300U;
        if (player().points < cost)
            return;
        if (target < world.players.size()) {
            auto &other = world.players[target];
            auto &field = hospital ? other.status.days_in_hospital : other.status.days_in_prison;
            if (by_human)
                update_hostility(
                    target, actor(),
                    -multiply32(
                        multiply32(static_cast<std::int32_t>(std::max<unsigned>(field & 0x7fU, 1U)), 100),
                        world.header.price_index));
            field = 0x80;
        } else if (target >= 4) {
            auto &special = world.special_actors.at(target - 4);
            const auto exit = std::find_if(world.nodes.begin() + 1, world.nodes.end(), [&](const auto &node) {
                return (node.flags & 0xffU) == (hospital ? 5U : 4U);
            });
            if (exit == world.nodes.end())
                throw std::runtime_error("rescue has no exit node");
            special.owner = static_cast<std::uint8_t>(actor());
            special.inactive_state = 0;
            special.node_id = static_cast<std::uint16_t>(exit - world.nodes.begin());
            special.last_node_id = 0;
            special.x = static_cast<std::uint16_t>(exit->x);
            special.y = static_cast<std::uint16_t>(exit->y);
            special.flags = static_cast<std::uint8_t>(0x80 | (hospital ? 2 : 1));
        } else
            return;
        roster[target] = 0;
        player().points = static_cast<std::uint16_t>(player().points - cost);
        emit(GameEventKind::notice, target < 4 ? "救援" + player_name(target) : "释放特殊角色",
             static_cast<std::int32_t>(cost));
    }
    void visit_confinement(bool hospital) {
        rescue_hospital = hospital;
        const auto &roster = hospital ? world.hospital_roster : world.prison_roster;
        if (std::none_of(roster.begin(), roster.end(), [](auto value) { return value != 0; })) {
            finish_turn();
            return;
        }
        if (human()) {
            GameDecision prompt{
                DecisionKind::rescue, actor(), 0, 0, hospital ? "探望住院角色" : "探望入獄角色", {}};
            for (std::size_t i = 0; i < roster.size(); ++i)
                if (roster[i] && (i >= 4 || i < world.players.size()))
                    prompt.options.push_back(
                        {(i < 4 ? "救援" + player_name(i) : "釋放特殊角色" + std::to_string(i - 3)) +
                             (i < 4 ? "（30點）" : "（300點）"),
                         static_cast<std::int32_t>(i)});
            prompt.options.push_back({"離開", -1});
            decision = std::move(prompt);
            set_phase(GamePhase::awaiting_decision);
            return;
        }
        if ((random.next() & 1U) == 0) {
            finish_turn();
            return;
        }
        const auto strategy = status().rescue_strategy;
        bool normal = strategy == 0 || strategy == 1;
        bool special = strategy == 2 || (strategy == 1 && random.next() % 3 == 0);
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < 8; ++i)
            if (roster[i] && ((i < world.players.size() && normal) || (i >= 4 && special)))
                candidates.push_back(i);
        if (!candidates.empty()) {
            const auto target = candidates[random.next() % candidates.size()];
            if ((target < 4 && player().points > 30) || (target >= 4 && player().points >= 700))
                rescue(target, hospital, false);
        }
        finish_turn();
    }
    // Presentation of a decided cupid wheel (fcn_0043f7c6): `wheel.phase` 1 = computer spin from
    // `wheel.cursor`, 2 = slow-down after a human STOP at `wheel.cursor`. No randomness drawn.
    void roulette_effect(SpinnerState wheel, int value) {
        wheel.result = value;
        effect(EffectKind::roulette, actor(), actor_point(actor()));
        effects.back().spinner = wheel;
    }
    void stopped_roulette_effect(const GameDecision &stopped, int value) {
        if (!stopped.spinner)
            return;
        auto wheel = *stopped.spinner;
        wheel.cursor = static_cast<std::uint8_t>(roulette_cursor);
        wheel.phase = 2; // STOP pressed: cupid shoots, the wheel slows down onto `value`
        roulette_effect(wheel, value);
    }
    int choose_roulette(unsigned kind, std::size_t company) {
        const auto initial = random.next() % 12;
        const SpinnerState wheel{static_cast<std::uint8_t>(kind), {}, 1, static_cast<std::uint8_t>(initial),
                                 static_cast<std::int32_t>(company)};
        if (!human() || status().days_sleep_walking) {
            const auto value = roulette_value(kind, static_cast<unsigned>(initial) + 51);
            roulette_effect(wheel, value);
            return value;
        }
        const auto key = choice_sequence++;
        const auto answer = event_choices.find(key);
        if (answer != event_choices.end())
            return answer->second;
        roulette_kind = kind;
        roulette_cursor = static_cast<unsigned>(initial);
        roulette_elapsed = 0;
        GameDecision prompt{DecisionKind::roulette, actor(), 0, 0, "按停止選定轉盤結果", {{"停止轉盤", 0}}};
        prompt.spinner = wheel;
        throw NeedChoice{std::move(prompt), key};
    }
    std::uint16_t company_construction_target() {
        if (human()) {
            const auto key = choice_sequence++;
            if (const auto answer = event_choices.find(key); answer != event_choices.end())
                return static_cast<std::uint16_t>(answer->second);
            // Original 0x463a4a "%s\n\n請選擇欲加蓋地點" (core_actions:1776); the side panel
            // shows the street/facility names via `subject`. 取消 is the panel's cancel button.
            GameDecision prompt{DecisionKind::property_target, actor(), 0, 0, "請選擇欲加蓋地點", {}};
            for (std::size_t i = 1; i < world.lands.size(); ++i)
                prompt.options.push_back(
                    {"房產" + std::to_string(i), static_cast<std::int32_t>(2000 + i),
                     DecisionLocation{world.lands[i].x, world.lands[i].y},
                     CardTarget{CardTargetKind::property, static_cast<std::uint16_t>(2000 + i)}});
            for (std::size_t i = 1; i < world.facilities.size(); ++i)
                prompt.options.push_back(
                    {"設施" + std::to_string(i), static_cast<std::int32_t>(4000 + i),
                     DecisionLocation{world.facilities[i].x, world.facilities[i].y},
                     CardTarget{CardTargetKind::property, static_cast<std::uint16_t>(4000 + i)}});
            prompt.options.push_back({"取消", 0});
            throw NeedChoice{std::move(prompt), key};
        }
        std::uint16_t selected = 0;
        unsigned score = 0;
        for (std::size_t i = 1; i < world.lands.size(); ++i) {
            const auto &land = world.lands[i].economy;
            if (land.owner_code == actor() + 1 && !land.chain_store && land.level < 5 &&
                land.toll_by_level[land.level] > score) {
                score = land.toll_by_level[land.level];
                selected = static_cast<std::uint16_t>(2000 + i);
            }
        }
        static constexpr unsigned limits[]{1, 5, 5, 1, 5};
        for (std::size_t i = 1; i < world.facilities.size(); ++i) {
            const auto &facility = world.facilities[i];
            if (facility.owner_code == actor() + 1 && facility.facility_type < 5 &&
                facility.level < limits[facility.facility_type] && facility.land_price > score) {
                score = facility.land_price;
                selected = static_cast<std::uint16_t>(4000 + i);
            }
        }
        return selected;
    }
    void offer_primary_stock(std::size_t id) {
        const auto &company = world.commercials.at(id);
        if (!player().controller_flags || status().days_sleep_walking || company.shares_remaining <= 0)
            return;
        const auto price = company.valuation / 10000;
        if (price <= 0)
            return; // Original divides by zero for malformed company valuations.
        const auto maximum = std::min({player().cash / price, 1000, company.shares_remaining});
        if (maximum <= 0)
            return;
        std::int32_t quantity = 0;
        if (human()) {
            const auto key = choice_sequence++;
            if (const auto answer = event_choices.find(key); answer != event_choices.end())
                quantity = answer->second;
            else
                throw NeedChoice{GameDecision{DecisionKind::primary_stock,
                                              actor(),
                                              static_cast<std::uint16_t>(id),
                                              price,
                                              "认购原始股：每股" + std::to_string(price) + "元，最多" +
                                                  std::to_string(maximum) + "股",
                                              {{"認購", 1}, {"放棄", 0}},
                                              maximum},
                                 key};
        } else {
            const auto reserve = multiply32(static_cast<std::int32_t>(world.header.initial_fund * 0.3),
                                            world.header.price_index);
            const auto spendable = subtract32(player().cash, reserve);
            if (spendable > 0)
                quantity = std::min(maximum, spendable / price);
        }
        if (quantity > 0) {
            const auto result = stocks::buy_stock(world, actor(), company.stock_index, quantity, false);
            emit(GameEventKind::bank_transfer, "认购原始股" + std::to_string(quantity) + "股", result.cost);
        }
    }
    void commercial(std::size_t id) {
        if (!id || id >= world.commercials.size()) {
            unsupported("商业公司记录无效");
            return;
        }
        run_rule([this, id] {
            auto &company = world.commercials.at(id);
            const bool own = company.owner_code == actor() + 1;
            std::int32_t toll = 0;
            int absence = 0;
            if (company.owner_code) {
                const auto type = company.commerce_type;
                if (type == 4) {
                    const auto duration = choose_roulette(3, id);
                    status().days_assurance =
                        static_cast<std::uint8_t>((status().days_assurance + duration) & 0x7f);
                    if (!own)
                        toll = multiply32(multiply32(company.toll_fee, duration), world.header.price_index);
                } else if (type == 11) {
                    const auto target = company_construction_target();
                    if (target) {
                        change_building(target, true);
                        if (own)
                            change_building(target, true);
                        else
                            toll = multiply32(target < 4000 ? world.lands.at(target - 2000).land_price
                                                            : world.facilities.at(target - 4000).land_price,
                                              world.header.price_index);
                    } else if (!own)
                        toll = multiply32(1000, world.header.price_index);
                } else if (!own) {
                    if (type == 1) {
                        absence = choose_roulette(0, id);
                        toll = multiply32(multiply32(company.toll_fee, absence), world.header.price_index);
                        if (!absence)
                            voice(actor(), 2);
                    } else if (type == 3)
                        toll = multiply32(company.toll_fee, std::bit_cast<std::int32_t>(world.round_counter));
                    else if (type == 5 || type == 6 || type == 12) {
                        unsigned total = 0;
                        for (auto face : faces)
                            total += face;
                        if (faces.empty() && status().days_tortoise_walking)
                            total = 1;
                        if (type == 12 || player().traffic_method) {
                            const unsigned factor =
                                type == 12 ? 1U : 1U << (((player().traffic_method & 3U) - 1U) & 31U);
                            toll = multiply32(
                                multiply32(company.toll_fee, std::bit_cast<std::int32_t>(total * factor)),
                                world.header.price_index);
                        }
                    }
                }
            }
            if (status().god_info == 1)
                toll >>= 1;
            else if (status().god_info == 2)
                toll = 0;
            else if (status().god_info == 5)
                toll = add32(toll, toll >> 1);
            else if (status().god_info == 6)
                toll = multiply32(toll, 2);
            const auto payer = select_toll_payer(toll, -1, absence != 0);
            if (payer == actor())
                amount_voice(actor(), toll, 9);
            const auto paid = pay(static_cast<int>(payer), static_cast<int>(id + 100), toll, 0);
            if (paid)
                emit(GameEventKind::rent_paid, "向公司支付费用", paid);
            if (phase == GamePhase::finished)
                return;
            if (absence && world.players[payer].state.controller_flags)
                disappear(payer, absence, 0);
            offer_primary_stock(id);
        });
    }
    void open_lottery() {
        if (!human()) {
            if (const auto number = lottery::buy_random_ticket(world, random, actor()))
                emit(GameEventKind::notice, "购买彩票" + std::to_string(*number), lottery::ticket_price);
            finish_turn();
            return;
        }
        GameDecision prompt{DecisionKind::lottery,        actor(), 0, lottery::ticket_price,
                            "選擇彩票號碼（每張1000元）", {}};
        if (player().cash >= lottery::ticket_price)
            for (auto number : lottery::available_numbers(world))
                prompt.options.push_back({std::to_string(number), number});
        prompt.options.push_back({"離開", 0});
        decision = std::move(prompt);
        set_phase(GamePhase::awaiting_decision);
    }
    void receive_random_card() {
        unsigned count = 0;
        for (auto amount : world.card_supply)
            count += amount;
        if (count) {
            unsigned ticket = random.next() % count;
            std::size_t card = 0;
            while (ticket >= world.card_supply.at(card))
                ticket -= world.card_supply[card++];
            receive_card(player().cards, world.card_supply, services::card_prices,
                         static_cast<std::uint8_t>(card + 1));
            const auto price = services::card_prices[card];
            if (price > 100)
                voice(actor(), 0);
            else if (price > 50)
                voice(actor(), static_cast<std::uint8_t>(random.next() & 1U));
            else if (price)
                voice(actor(), 2);
            emit(GameEventKind::notice, "获得卡片" + std::to_string(card + 1));
        }
        finish_turn();
    }
    std::int32_t pay(int payer_code, int recipient_code, std::int32_t requested, unsigned flags) {
        if (requested <= 0)
            return 0;
        auto paid = requested;
        if (payer_code > 100) {
            auto &company = world.commercials.at(static_cast<std::size_t>(payer_code - 100));
            company.funds = subtract32(company.funds, requested);
            company.funds_change = subtract32(company.funds_change, requested);
        } else {
            const auto slot = static_cast<std::size_t>(payer_code);
            auto &payer = world.players.at(slot).state;
            const auto available =
                std::max<std::int64_t>(0, static_cast<std::int64_t>(payer.cash) + payer.bank_balance);
            paid = static_cast<std::int32_t>(std::min<std::int64_t>(requested, available));
            auto &first = flags & 4U ? payer.bank_balance : payer.cash;
            auto &second = flags & 4U ? payer.cash : payer.bank_balance;
            const auto first_payment = std::min(std::max(first, 0), paid);
            first = subtract32(first, first_payment);
            second = subtract32(second, paid - first_payment);
            if (paid < requested)
                bankrupt(slot);
            world.players[slot].status.cumulative_spending += static_cast<std::uint32_t>(paid);
        }
        if (recipient_code == -1)
            world.public_fund = add32(world.public_fund, paid);
        else if (recipient_code > 100) {
            auto &company = world.commercials.at(static_cast<std::size_t>(recipient_code - 100));
            company.funds = add32(company.funds, paid);
            company.funds_change = add32(company.funds_change, paid);
        } else {
            auto &recipient = world.players.at(static_cast<std::size_t>(recipient_code));
            auto &account = flags & 1U ? recipient.state.cash : recipient.state.bank_balance;
            account = add32(account, paid);
            recipient.status.cumulative_income += static_cast<std::uint32_t>(paid);
        }
        return paid;
    }
    void update_hostility(std::size_t from, std::size_t target, std::int32_t delta) {
        if (from == target)
            return;
        auto &value = world.players.at(from).status;
        const auto old = std::bit_cast<std::int32_t>(value.hostility.at(target));
        if (delta < 0 && old == 0)
            return;
        value.hostility[target] = static_cast<std::uint32_t>(std::max(add32(old, delta), 0));
        if (delta > 0 && value.allied_player == target + 1) {
            value.allied_player = 0;
            value.allied_days = 0;
        }
    }
    std::size_t select_toll_payer(std::int32_t &requested, int owner, bool compulsory_absence = false,
                                  bool free_allowed = true) {
        const auto offered = [&] {
            return requested >= multiply32(2000, world.header.price_index) ||
                   requested > add32(player().cash, player().bank_balance);
        };
        if (free_allowed && offered() && try_free_card(actor(), static_cast<std::size_t>(owner), requested))
            requested = 0;
        auto payer = actor();
        if (offered()) {
            const auto target = try_frame_card(actor(), 1, requested);
            if (target >= 0)
                payer = static_cast<std::size_t>(target);
        }
        if (requested || compulsory_absence)
            for (std::size_t i = 0; i < world.players.size(); ++i)
                if (i != payer && world.players[i].state.controller_flags &&
                    (world.players[i].status.god_info == 14 || world.players[i].status.god_info == 15)) {
                    payer = i;
                    break;
                }
        return payer;
    }
    void pay_toll(std::size_t owner, std::int32_t requested, std::size_t payer,
                  std::optional<std::size_t> ally = std::nullopt, std::int32_t ally_amount = 0,
                  std::uint16_t property = 0, std::uint8_t facility_type = 0, int multiplier = 0) {
        {
            auto &box = announce(Announcement::toll, payer, facility_type);
            box.property = property;
            box.subject = static_cast<std::int32_t>(owner);
            box.second_subject = ally ? static_cast<std::int32_t>(*ally) : -1;
            box.amount = requested;
            box.value = multiplier;
        }
        int most_hostile = -1;
        std::int32_t largest_hostility = 0;
        for (std::size_t i = 0; i < world.players.size(); ++i) {
            if (i == payer || !world.players[i].state.controller_flags)
                continue;
            const auto hostility = std::bit_cast<std::int32_t>(world.players[payer].status.hostility[i]);
            if (hostility > largest_hostility) {
                largest_hostility = hostility;
                most_hostile = static_cast<int>(i);
            }
        }
        bool hostile_speech = false;
        if (most_hostile == static_cast<int>(owner) &&
            requested >= multiply32(5000, world.header.price_index)) {
            hostile_speech = (random.next() & 1U) != 0;
            if (hostile_speech)
                voice(payer, 18);
        }
        if (!hostile_speech)
            amount_voice(payer, requested, 9);
        const auto owner_amount = subtract32(requested, ally_amount);
        amount_voice(owner, owner_amount, 6);
        auto paid = pay(static_cast<int>(payer), static_cast<int>(owner), owner_amount, 0);
        if (ally)
            paid = add32(paid, pay(static_cast<int>(payer), static_cast<int>(*ally), ally_amount, 0));
        if (paid)
            emit(GameEventKind::rent_paid, player_name(payer) + "支付租金", paid);
    }
    bool check_game_end() {
        unsigned active = 0, humans = 0;
        std::optional<std::size_t> survivor;
        for (std::size_t i = 0; i < world.players.size(); ++i) {
            if (world.players[i].state.controller_flags) {
                ++active;
                survivor = i;
            }
            if (world.players[i].state.controller_flags & 1U)
                ++humans;
        }
        if (humans && active > 1)
            return false;
        winner = active == 1 && humans ? survivor : std::nullopt;
        events.push_back({GameEventKind::game_finished, winner.value_or(actor()), 0, player().node_id,
                          winner ? player_name(*winner) + "获胜" : "游戏结束"});
        set_phase(GamePhase::finished);
        return true;
    }
    void bankrupt(std::size_t slot) {
        auto &state = world.players.at(slot).state;
        state.controller_flags = 0;
        const auto &node = world.nodes.at(state.node_id);
        state.x = static_cast<std::uint16_t>(node.x);
        state.y = static_cast<std::uint16_t>(node.y);
        world.nodes[state.node_id].flags |= 0x100U << slot;
        for (auto id : {world.players[slot].status.god_info, world.players[slot].status.attached_bomb})
            if (id)
                remove_object(id);
        for (std::size_t i = 0; i < 4; ++i)
            if (!world.special_actors[i].inactive_state && world.special_actors[i].owner == slot)
                add_confinement(i + 4, 0, i >= 2);
        world.clear_bankrupt_player_fields(slot);
        world.prison_roster[slot] = world.hospital_roster[slot] = 0;
        for (auto &other : world.players) {
            other.status.hostility[slot] = 0;
            if (other.status.allied_player == slot + 1) {
                other.status.allied_player = 0;
                other.status.allied_days = 0;
            }
        }
        events.push_back(
            {GameEventKind::player_bankrupt, slot, 0, state.node_id, player_name(slot) + "破产"});
        effect(EffectKind::bankrupt, slot, actor_point(slot)); // Data 555
        if (check_game_end())
            return;
        // Original 0x40d095 retains buildings and auctions three distinct
        // properties only when the bankrupt player owned more than three.
        std::vector<std::uint16_t> properties;
        for (auto &land : world.lands)
            if (land.economy.owner_code == slot + 1) {
                land.economy.owner_code = 0;
                land.expired_date = 0;
                properties.push_back(static_cast<std::uint16_t>(2000 + land.id));
            }
        for (auto &facility : world.facilities)
            if (facility.owner_code == slot + 1) {
                facility.owner_code = 0;
                facility.expired_date = 0;
                properties.push_back(static_cast<std::uint16_t>(4000 + facility.id));
            }
        for (auto &commerce : world.commercials)
            if (commerce.owner_code == slot + 1)
                commerce.owner_code = 0;
        for (std::size_t i = 0; i < 12; ++i)
            if (world.players[slot].stocks[i].amount)
                (void)stocks::sell_stock(world, slot, i, world.players[slot].stocks[i].amount, false);
        // Original ignores sale return values, but removes equipped transport.
        const auto traffic = state.traffic_method & 3U;
        if (traffic)
            ++state.tool_amounts[traffic == 3 ? 11 : traffic + 3];
        state.traffic_method = 0;
        state.dice_count = 1;
        (void)sell_stored_tools(state.tool_amounts, world.tool_supply, services::tool_prices);
        (void)sell_all_cards(state.cards, world.card_supply, services::card_prices);
        lottery::discard_player_tickets(world, slot);
        if (properties.size() > 3)
            for (unsigned sold = 0; sold < 3; ++sold) {
                std::size_t pick;
                do {
                    pick = random.next() % properties.size();
                } while (!properties[pick]);
                (void)auction(properties[pick], -1);
                properties[pick] = 0;
            }
    }
    void insure(std::size_t slot, std::int32_t amount) {
        if (!world.players.at(slot).status.days_assurance)
            return;
        for (std::size_t i = 1; i < world.commercials.size(); ++i)
            if (world.commercials[i].commerce_type == 4) {
                pay(static_cast<int>(i + 100), static_cast<int>(slot), amount, 1);
                return;
            }
    }
    void sync_attached_objects(std::size_t slot) {
        const auto &entry = world.players.at(slot);
        for (const auto id : {entry.status.god_info, entry.status.attached_bomb})
            if (id)
                world.objects.at(id - 1).node_id = entry.state.node_id;
    }
    void clear_absences(std::size_t slot) {
        auto &value = world.players.at(slot).status;
        // fcn_0040d761 -> fcn_0040bf93: pulled out early, the player keeps wearing the stripes (prison)
        // or the gown (hospital wins) until their next sprite update. Presentation only.
        if (slot < costumes.size() && (value.days_in_prison || value.days_in_hospital)) {
            costumes[slot] = value.days_in_hospital ? 19 : 20;
            costume_keys[slot] = costume_key(slot);
        }
        if (value.days_in_prison)
            world.prison_roster[slot] = 0;
        if (value.days_in_hospital)
            world.hospital_roster[slot] = 0;
        value.days_in_hotel = value.days_disappearing = value.days_in_prison = value.days_in_hospital = 0;
    }
    void relocate_bankrupt(std::size_t slot) {
        auto &state = world.players.at(slot).state;
        const auto old = state.node_id;
        world.nodes.at(old).flags &= ~(0x100U << slot);
        const auto destination = random_free_node(old);
        const auto &node = world.nodes.at(destination);
        const auto previous =
            std::find_if(node.neighbors.begin(), node.neighbors.end(), [](auto id) { return id != 0; });
        state.node_id = destination;
        state.last_node_id = previous == node.neighbors.end() ? 0 : *previous;
        state.x = static_cast<std::uint16_t>(node.x);
        state.y = static_cast<std::uint16_t>(node.y);
        const auto &before = world.nodes.at(state.last_node_id);
        state.direction = direction::from_delta(node.x - before.x, node.y - before.y);
        world.nodes[destination].flags |= 0x100U << slot;
    }
    void injure(std::size_t slot) {
        if (slot >= world.players.size())
            return;
        auto &entry = world.players.at(slot);
        auto &state = entry.state;
        const auto &value = entry.status;
        if (!state.controller_flags) {
            relocate_bankrupt(slot);
            return;
        }
        if (value.days_in_hotel || value.days_disappearing || value.days_in_prison || value.days_in_hospital)
            return;
        if (state.traffic_method) {
            if ((state.traffic_method & 3U) == 1)
                ++world.tool_supply[4];
            else if ((state.traffic_method & 3U) == 2)
                ++world.tool_supply[5];
            state.traffic_method = 0;
            state.dice_count = 1;
        }
        state.controller_flags |= 0x40U;
    }
    void add_confinement(std::size_t slot, int days_left, bool hospital) {
        if (slot >= 4 && slot < 8) {
            auto &special = world.special_actors.at(slot - 4);
            world.nodes.at(special.node_id).flags &= ~(0x100U << slot);
            special.inactive_state = hospital ? 2 : 1;
            special.flags = special.days_sleeping = special.days_sleep_walking = 0;
            special.days_stopping = special.days_tortoise_walking = 0;
            (hospital ? world.hospital_roster : world.prison_roster)[slot] = 1;
            return;
        }
        if (slot >= world.players.size())
            throw std::runtime_error("invalid confinement player");
        auto &entry = world.players[slot];
        auto &state = entry.state;
        auto &value = entry.status;
        auto &days_field = hospital ? value.days_in_hospital : value.days_in_prison;
        if (!days_field) {
            // Ambulance / police car movie (cfg-gated in the original): camera on the old spot, the
            // player is driven away at the commit frame. Only drawn there if they were on the board.
            const auto old_point = actor_point(slot);
            const auto old_shown = on_board(slot);
            clear_absences(slot);
            absence_voice(slot, days_left);
            state.controller_flags &= 0x0fU;
            world.nodes.at(state.node_id).flags &= ~(0x100U << slot);
            const auto kind = hospital ? 5U : 4U;
            const auto target =
                std::find_if(world.nodes.begin() + 1, world.nodes.end(),
                             [&](const auto &candidate) { return (candidate.flags & 0xffU) == kind; });
            if (target == world.nodes.end())
                throw std::runtime_error("map has no confinement exit node");
            state.node_id = static_cast<std::uint16_t>(target - world.nodes.begin());
            state.last_node_id = 0;
            value.saved_direction = 15;
            const auto &building = world.landscapes.at(hospital ? 1 : 2);
            state.x = building.x;
            state.y = building.y;
            sync_attached_objects(slot);
            days_field = static_cast<std::uint8_t>(days_left);
            (hospital ? world.hospital_roster : world.prison_roster)[slot] = 1;
            board_movie(hospital ? EffectKind::ambulance : EffectKind::police, slot, old_point,
                        old_shown ? std::optional<EffectPoint>(old_point) : std::nullopt);
        } else
            days_field = static_cast<std::uint8_t>((days_field + days_left) & 0x7f);
        voice(slot, hospital ? 20 : 19);
        insure(slot, multiply32(multiply32(days_left, 2000), world.header.price_index));
        value.total_winter_sleep_days = static_cast<std::uint8_t>(value.total_winter_sleep_days + days_left);
        events.push_back(
            {GameEventKind::notice, slot, days_left, state.node_id,
             player_name(slot) + (hospital ? "住院" : "入狱") + std::to_string(days_left) + "天"});
    }
    void disappear(std::size_t slot, int days_left, int mode) {
        auto &entry = world.players.at(slot);
        auto &value = entry.status;
        const auto encoded = static_cast<std::uint8_t>((mode << 6) | days_left);
        if (!value.days_disappearing) {
            clear_absences(slot);
            absence_voice(slot, days_left);
            insure(slot, multiply32(multiply32(days_left, 2000), world.header.price_index));
            value.total_winter_sleep_days =
                static_cast<std::uint8_t>(value.total_winter_sleep_days + days_left);
            value.days_disappearing = encoded;
            auto &node = world.nodes.at(entry.state.node_id);
            node.flags &= ~(0x100U << slot);
            // fcn_0040d375: UFO (mode != 0, Data 533) or airplane (Data 558) takes the player away from
            // where they stand (the redraw before the movie shows them there, even if just released).
            const auto old_point = actor_point(slot);
            board_movie(mode ? EffectKind::ufo : EffectKind::airplane, slot, old_point, old_point);
            entry.state.x = static_cast<std::uint16_t>(node.x);
            entry.state.y = static_cast<std::uint16_t>(node.y);
        } else
            value.days_disappearing = static_cast<std::uint8_t>((value.days_disappearing & 0x3fU) + encoded);
        events.push_back({GameEventKind::notice, slot, days_left, entry.state.node_id,
                          player_name(slot) + "暂时离开" + std::to_string(days_left) + "天"});
    }
    bool try_free_card(std::size_t slot, std::size_t recipient, std::int32_t amount) {
        auto &entry = world.players.at(slot);
        if (!has_card(entry.state.cards, 20))
            return false;
        bool accepted = false;
        if (entry.state.controller_flags == 1) {
            const auto key = choice_sequence++;
            const auto answer = event_choices.find(key);
            if (answer == event_choices.end())
                // Original yes/no box 0x465388 "%s\n\n是否使用免費卡？" (card_passive:409); the
                // app prefixes the holder's name and draws it with the Data.mkf 440 strip.
                throw NeedChoice{GameDecision{DecisionKind::card_target,
                                              slot,
                                              0,
                                              amount,
                                              "是否使用免費卡？",
                                              {{"使用免費卡", 1}, {"保留卡片", 0}}},
                                 key};
            accepted = answer->second != 0;
        } else {
            const auto limit = multiply32(random.next() % 3000 + 3000, world.header.price_index);
            accepted = amount > entry.state.cash || amount > limit;
        }
        if (accepted) {
            (void)consume_card(entry.state.cards, world.card_supply, 20);
            events.push_back({GameEventKind::notice, slot, 0, entry.state.node_id, "使用免費卡"});
            events.back().card = 20;
            // _rich4_try_use_card_mianfeika: holder (mood 0), then the recipient if any (mood 1).
            card_speech(slot, 0, 20, 0);
            card_speech(recipient, 2, 20, 1);
        }
        return accepted;
    }
    int try_frame_card(std::size_t slot, unsigned mode, std::int32_t amount = 0) {
        auto &entry = world.players.at(slot);
        if (!has_card(entry.state.cards, 19))
            return -1;
        int target = -1;
        if (entry.state.controller_flags == 1) {
            const auto key = choice_sequence++;
            const auto found = event_choices.find(key);
            if (found != event_choices.end())
                target = found->second;
            else {
                // Original: one target -> yes/no box 0x46534e "是否嫁禍給%s？"; several ->
                // picker 0x46535d "請選擇嫁禍對象..." (card_passive:170/202). The app shows names.
                GameDecision prompt{DecisionKind::card_target, slot, 0, amount, "請選擇嫁禍對象...", {}};
                for (std::size_t i = 0; i < world.players.size(); ++i)
                    if (i != slot && world.players[i].state.controller_flags)
                        prompt.options.push_back(
                            {"嫁禍給" + player_name(i), static_cast<std::int32_t>(i), std::nullopt,
                             CardTarget{CardTargetKind::actor, static_cast<std::uint16_t>(i)}});
                if (prompt.options.size() == 1)
                    prompt.prompt = "是否嫁禍給" + player_name(static_cast<std::size_t>(prompt.options[0].value)) + "？";
                prompt.options.push_back({"保留卡片", -1});
                throw NeedChoice{std::move(prompt), key};
            }
        } else {
            std::int32_t hostility = 0;
            for (std::size_t i = 0; i < world.players.size(); ++i) {
                if (i == slot || !world.players[i].state.controller_flags)
                    continue;
                const auto value = std::bit_cast<std::int32_t>(entry.status.hostility[i]);
                if (value > hostility) {
                    hostility = value;
                    target = static_cast<int>(i);
                }
            }
            if (target < 0) {
                std::vector<std::size_t> eligible;
                for (std::size_t i = 0; i < world.players.size(); ++i) {
                    const auto &other = world.players[i];
                    const auto &value = other.status;
                    if (i != slot && other.state.controller_flags && !value.days_in_hotel &&
                        !value.days_disappearing && !value.days_in_prison && !value.days_in_hospital)
                        eligible.push_back(i);
                }
                if (!eligible.empty())
                    target = static_cast<int>(eligible[random.next() % eligible.size()]);
            }
            if (mode == 1) {
                const auto limit = multiply32(random.next() % 4000 + 4000, world.header.price_index);
                if (!(amount > entry.state.cash || amount > limit))
                    target = -1;
            } else if (mode == 2 && entry.state.cash < multiply32(20000, world.header.price_index))
                target = -1;
        }
        if (target >= 0) {
            (void)consume_card(entry.state.cards, world.card_supply, 19);
            events.push_back({GameEventKind::notice, slot, 0, entry.state.node_id,
                              "使用嫁禍卡，目標是" + player_name(static_cast<std::size_t>(target))});
            events.back().card = 19;
            // _rich4_try_use_card_jiahuoka: holder (mood 0), then the framed player (mood 2).
            card_speech(slot, 0, 19, 0);
            card_speech(static_cast<std::size_t>(target), 2, 19, 2);
        }
        return target;
    }
    int try_defense_card(std::size_t slot) {
        auto &entry = world.players.at(slot);
        if (has_card(entry.state.cards, 21)) {
            (void)consume_card(entry.state.cards, world.card_supply, 21);
            events.push_back({GameEventKind::notice, slot, 0, entry.state.node_id, "使用免罪卡"});
            events.back().card = 21;
            card_speech(slot, 0, 21, 0); // _rich4_use_card_mianzuika

            return -1;
        }
        const auto redirected = try_frame_card(slot, 0);
        return redirected >= 0 ? redirected : static_cast<int>(slot);
    }
    std::uint8_t choose_victim_card(std::size_t slot, std::size_t victim) {
        const auto key = choice_sequence++;
        const auto found = event_choices.find(key);
        if (found != event_choices.end())
            return static_cast<std::uint8_t>(found->second);
        GameDecision prompt{DecisionKind::choose_card, slot, 0, 0, "請選擇一張卡片", {}};
        for (auto card : world.players.at(victim).state.cards)
            if (card)
                if (std::none_of(prompt.options.begin(), prompt.options.end(),
                                 [&](const auto &option) { return option.value == card; }))
                    prompt.options.push_back({std::string(services::card_names.at(card - 1)), card});
        if (prompt.options.empty())
            return 0;
        prompt.options.push_back({"放棄", 0});
        throw NeedChoice{std::move(prompt), key};
    }
    void release_hotel_guests() {
        // Original 0x40dffa deliberately releases ALL hotels' guests.
        for (auto &entry : world.players)
            if (entry.state.controller_flags && entry.status.days_in_hotel)
                entry.status.days_in_hotel = 0x80;
    }
    void change_building(std::uint16_t code, bool upgrade) {
        if (code > 2000 && code < 4000) {
            auto &land = world.lands.at(code - 2000).economy;
            if (upgrade) {
                if ((!land.chain_store && land.level < 5) || (land.chain_store == 1 && land.level < 1))
                    ++land.level;
            } else if (land.level) {
                --land.level;
                if (land.chain_store) {
                    land.chain_store = 0;
                    land.level = 0;
                }
            }
        } else if (code > 4000 && code < 6000) {
            auto &entry = world.facilities.at(code - 4000);
            if (upgrade) {
                if (!entry.level) {
                    if (player().controller_flags & 6U) {
                        entry.facility_type = entry.owner_code == actor() + 1
                                                  ? static_cast<std::uint8_t>(random.next() % 4 + 1)
                                                  : 0;
                    } else {
                        const auto key = choice_sequence++;
                        const auto answer = event_choices.find(key);
                        if (answer == event_choices.end())
                            throw NeedChoice{
                                GameDecision{DecisionKind::construct_facility,
                                             actor(),
                                             static_cast<std::uint16_t>(code - 4000),
                                             0,
                                             "請選擇設施類別", // 0x465289; slot 0 公園 as in fcn_00440aac
                                             {{"公園", 0}, {"旅館", 1}, {"百貨公司", 2}, {"加油站", 3}, {"研究所", 4}}},
                                key};
                        entry.facility_type = static_cast<std::uint8_t>(answer->second);
                    }
                    ++entry.level;
                } else {
                    static constexpr unsigned limits[]{1, 5, 5, 1, 5};
                    if (entry.facility_type < 5 && entry.level < limits[entry.facility_type])
                        ++entry.level;
                }
            } else if (entry.level) {
                --entry.level;
                if (!entry.level) {
                    entry.facility_type = 0;
                    release_hotel_guests();
                }
            }
        }
    }
    void construction_vehicle_demolish() {
        // 工程車 (traffic 3) stopping on an opponent's building: 0x448a7e schedules the attack;
        // phase3 at 0x40da4b applies downgrade mode2 (demolish the building, retain ownership/lease).
        if (!player().controller_flags || (player().traffic_method & 0x83U) != 3 ||
            status().days_sleep_walking || status().days_in_hotel)
            return;
        const auto code = world.nodes.at(player().node_id).type_and_index;
        std::uint8_t owner = 0, level = 0, type = 0;
        if (code > 2000 && code < 4000) {
            auto &land = world.lands.at(code - 2000).economy;
            if (land.owner_code == actor() + 1 || !land.level)
                return;
            owner = land.owner_code;
            level = land.level;
            type = land.chain_store;
            land.level = land.chain_store = 0;
        } else if (code > 4000 && code < 6000) {
            auto &facility = world.facilities.at(code - 4000);
            if (facility.owner_code == actor() + 1 || !facility.level)
                return;
            owner = facility.owner_code;
            level = facility.level;
            type = facility.facility_type;
            facility.level = facility.facility_type = 0;
            release_hotel_guests();
        } else
            return;
        if (owner)
            update_hostility(owner - 1U, actor(), multiply32(30, world.header.price_index));
        status().saved_direction = player().direction;
        // Picture: the truck turns to the building and swings (pose 12) under a Data 526 puff.
        const auto &node = world.nodes.at(player().node_id);
        const auto building = property_point(code);
        effect(EffectKind::truck_demolish, actor(), building, code);
        effects.back().from = actor_point(actor());
        effects.back().object_kind = direction::from_delta(building.x - node.x, building.y - node.y);
        effects.back().previous_level = level;
        effects.back().previous_type = type;
        emit(GameEventKind::notice, "工程车拆除脚下建筑");
    }
    void turn_around(std::size_t slot) {
        const auto turn = [&](auto &state) {
            state.direction = static_cast<std::uint8_t>((state.direction + 4) & 7U);
            const auto &node = world.nodes.at(state.node_id);
            std::vector<std::uint16_t> choices;
            for (unsigned i = 0; i < 4; ++i)
                if (node.neighbors[i] && node.neighbors[i] != state.last_node_id &&
                    !(node.flags & (0x40000000U >> i)))
                    choices.push_back(node.neighbors[i]);
            state.last_node_id = choices.empty() ? 0 : choices[random.next() % choices.size()];
        };
        if (slot < 4)
            turn(world.players.at(slot).state);
        else
            turn(world.special_actors.at(slot - 4));
        cue(slot, 56); // AUDIO: _rich4_change_player_direction (rich4.asm:6424) board preset [21]
    }
    AuctionResult auction(std::uint16_t property_code, int seller = -1) {
        AuctionSession auction_session(world, random, property_code, seller);
        static constexpr const char *labels[]{"不加價",   "加價100",   "加價500", "加價1000",
                                              "加價5000", "加價10000", "退出拍賣"};
        for (unsigned count = 0; !auction_session.finished(); ++count) {
            if (count >= 100000)
                throw std::runtime_error("auction action limit reached");
            if (const auto bidder = auction_session.decision()) {
                const auto key = choice_sequence++;
                const auto answer = event_choices.find(key);
                if (answer == event_choices.end()) {
                    GameDecision prompt{DecisionKind::auction,
                                        bidder->player,
                                        property_code,
                                        bidder->price,
                                        "地產拍賣：當前出價" + std::to_string(bidder->price),
                                        {}};
                    for (auto action : bidder->actions)
                        prompt.options.push_back(
                            {labels[static_cast<unsigned>(action)], static_cast<std::int32_t>(action)});
                    prompt.inactive_players = bidder->inactive; // presentation: "cannot bid" poses
                    throw NeedChoice{std::move(prompt), key};
                }
                if (!auction_session.bid(static_cast<AuctionAction>(answer->second)))
                    throw std::logic_error("cached auction bid no longer legal");
            } else if (!auction_session.tick())
                throw std::logic_error("auction made no progress");
        }
        if (auction_session.result().winner) {
            emit(GameEventKind::notice, "拍卖成交", auction_session.result().price);
            // Presentation: the buyer and lot for the original sold sequence (gavel, winner pose 29+3c).
            events.back().subject = static_cast<std::int32_t>(*auction_session.result().winner); // AUDIO: 136+c
            events.back().property = property_code;
        } else
            emit(GameEventKind::notice, "地产拍卖流拍");
        return auction_session.result();
    }
    std::uint16_t random_free_node(std::uint16_t reference = 0) {
        std::vector<std::uint16_t> choices;
        for (std::size_t i = 1; i < world.nodes.size(); ++i) {
            const auto &node = world.nodes[i];
            if (!(node.flags & 0x80ffff00U) &&
                std::any_of(node.neighbors.begin(), node.neighbors.end(), [](auto n) { return n != 0; })) {
                if (i > 255)
                    throw std::runtime_error("original random node exceeds byte range");
                choices.push_back(static_cast<std::uint16_t>(i));
            }
        }
        if (choices.empty())
            throw std::runtime_error("no free object node");
        for (unsigned trial = 0; trial < 100000; ++trial) {
            const auto selected = choices[random.next() % choices.size()];
            if (!reference || std::abs(world.nodes[selected].x - world.nodes.at(reference).x) >= 300 ||
                std::abs(world.nodes[selected].y - world.nodes.at(reference).y) >= 300)
                return selected;
        }
        throw std::runtime_error("original distant-node selection cannot find an eligible node");
    }
    void place_existing_object(std::size_t index, std::uint16_t node_id) {
        auto &object = world.objects.at(index);
        if (object.node_id)
            return;
        object.node_id = node_id;
        object.remaining_days = object.attached_player = 0;
        if (!node_id)
            return;
        auto &node = world.nodes.at(node_id);
        const auto previous =
            std::find_if(node.neighbors.begin(), node.neighbors.end(), [](auto n) { return n != 0; });
        const auto &from = world.nodes.at(previous == node.neighbors.end() ? 0 : *previous);
        object.direction = direction::from_delta(node.x - from.x, node.y - from.y);
        node.flags |= static_cast<std::uint32_t>(index + 1) << 16U;
    }
    void remove_object(std::uint16_t id) {
        if (!id)
            return;
        auto &object = world.objects.at(id - 1);
        const auto old_node = object.node_id;
        if (object.kind >= 16 && object.kind <= 18)
            ++world.tool_supply.at(object.kind - 15);
        if (object.attached_player && object.attached_player <= world.players.size()) {
            auto &status = world.players[object.attached_player - 1].status;
            if (object.kind == 18)
                status.attached_bomb = 0;
            else {
                static constexpr std::array<std::int16_t, 18> luck{
                    0, -100, -200, -100, -200, 100, 200, 100, 200, -100, 100, 0, -500, 0, 0, 1000, 0, 0};
                static constexpr std::array<std::int16_t, 18> money{0,  100, 150, 0, 0, -60, -100, 0, 0,
                                                                    60, -60, 0,   0, 0, 0,   -200, 0, 0};
                static constexpr std::array<std::int16_t, 18> physical{
                    0, 0, 0, 100, 150, 0, 0, -60, -100, 60, -60, 0, 0, 0, 0, -200, 0, 0};
                const auto kind = object.kind;
                if (kind >= luck.size())
                    throw std::runtime_error("invalid attached god type");
                const auto sub16 = [](std::int16_t a, std::int16_t b) {
                    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(a) - static_cast<std::uint16_t>(b)));
                };
                status.god_info = 0;
                status.god_luck_modifier = sub16(status.god_luck_modifier, luck[kind]);
                status.money_fortune_modifier = sub16(status.money_fortune_modifier, money[kind]);
                status.other_fortune_modifier = sub16(status.other_fortune_modifier, physical[kind]);
            }
        }
        if (!object.attached_player)
            world.nodes.at(old_node).flags &= ~0x00ff0000U;
        object.node_id = 0;
        object.remaining_days = object.attached_player = 0;
        if (id <= 12) {
            const auto opposite = static_cast<std::size_t>((id - 1) ^ 1U);
            const auto node = random_free_node(old_node);
            place_existing_object(opposite, node);
        }
    }
    std::vector<std::uint16_t> instance_codes(std::uint16_t center_x, std::uint16_t center_y,
                                              int radius) const {
        if (world.camera_rotation > 7)
            throw std::runtime_error("invalid world camera rotation");
        const board::Camera camera{{center_x, center_y}, static_cast<std::uint8_t>(world.camera_rotation)};
        std::vector<std::uint16_t> pixels(440U * 440U);
        const auto put = [&](int x, int y, std::uint16_t code) {
            if (const auto screen = board::project({x, y}, camera)) {
                const auto row = screen->y - 40;
                if (screen->x >= 0 && screen->x < 440 && row >= 0 && row < 440)
                    pixels[static_cast<std::size_t>(row * 440 + screen->x)] |= code;
            }
        };
        for (std::size_t i = 0; i < world.players.size(); ++i) {
            const auto &entry = world.players[i];
            if (entry.state.x && !entry.status.days_disappearing)
                put(entry.state.x, entry.state.y, static_cast<std::uint16_t>(0x8000U | (1U << i)));
        }
        for (std::size_t i = 0; i < 4; ++i) {
            const auto &entry = world.special_actors[i];
            if (entry.x && !entry.inactive_state)
                put(entry.x, entry.y, static_cast<std::uint16_t>(0x8000U | (0x10U << i)));
        }
        for (std::size_t i = 0; i < world.objects.size(); ++i) {
            const auto &object = world.objects[i];
            if (object.node_id && !object.attached_player) {
                const auto &node = world.nodes.at(object.node_id);
                put(node.x, node.y, static_cast<std::uint16_t>(0x8000U | ((i + 1U) << 8U)));
            }
        }
        for (std::size_t i = 1; i < world.lands.size(); ++i) {
            const auto &land = world.lands[i];
            if (land.economy.level || land.economy.owner_code)
                put(land.x, land.y, static_cast<std::uint16_t>(2000 + i));
        }
        for (std::size_t i = 1; i < world.facilities.size(); ++i) {
            const auto &facility = world.facilities[i];
            if (facility.level || facility.owner_code)
                put(facility.x, facility.y, static_cast<std::uint16_t>(4000 + i));
        }
        for (std::size_t i = 1; i < world.commercials.size(); ++i) {
            const auto &company = world.commercials[i];
            if (company.sprite_index)
                put(company.x, company.y, static_cast<std::uint16_t>(6000 + i));
        }
        std::vector<std::uint16_t> selected;
        const auto first = radius < 0 ? 0 : 220 - radius, last = radius < 0 ? 440 : 220 + radius;
        if (first < 0 || last > 440)
            throw std::invalid_argument("pick radius outside viewport");
        for (int y = first; y < last; ++y)
            for (int x = first; x < last; ++x) {
                const auto code = pixels[static_cast<std::size_t>(y * 440 + x)];
                if (code)
                    selected.push_back(code);
            }
        return selected;
    }
    void area_damage(std::uint16_t x, std::uint16_t y, std::uint8_t mode, bool hit_actors, int source = -1,
                     int radius = 100) {
        const auto targets = instance_codes(x, y, radius);
        for (const auto code : targets) {
            if (code > 2000 && code < 4000) {
                auto &land = world.lands.at(code - 2000);
                if (source >= 0 && land.economy.owner_code)
                    update_hostility(
                        land.economy.owner_code - 1, static_cast<std::size_t>(source),
                        multiply32(mode ? land.economy.level * 30 : 30, world.header.price_index));
                if (mode) {
                    land.economy.owner_code = land.economy.level = land.economy.chain_store = 0;
                    land.expired_date = 0;
                } else {
                    if (land.economy.level)
                        --land.economy.level;
                    if (land.economy.chain_store)
                        land.economy.level = land.economy.chain_store = 0;
                }
            } else if (code > 4000 && code < 6000) {
                auto &facility = world.facilities.at(code - 4000);
                if (source >= 0 && facility.owner_code)
                    update_hostility(facility.owner_code - 1, static_cast<std::size_t>(source),
                                     multiply32(mode ? facility.level * 30 : 30, world.header.price_index));
                if (mode) {
                    facility.owner_code = facility.level = facility.facility_type = 0;
                    facility.expired_date = 0;
                    release_hotel_guests();
                } else {
                    if (facility.level)
                        --facility.level;
                    if (!facility.level) {
                        facility.facility_type = 0;
                        release_hotel_guests();
                    }
                }
            } else if (hit_actors && (code & 0x8000U)) {
                for (unsigned i = 0; i < world.players.size(); ++i)
                    if (code & (1U << i))
                        injure(i);
                for (unsigned i = 4; i < 8; ++i)
                    if (code & (1U << i))
                        add_confinement(i, 0, true);
                const auto id = static_cast<std::uint16_t>((code >> 8) & 0x3fU);
                if (id)
                    remove_object(id);
            }
        }
        if (hit_actors)
            for (std::size_t i = 0; i < world.players.size(); ++i)
                if (world.players[i].state.controller_flags & 0x40U)
                    add_confinement(i, 3, true);
    }
    void check_god(const gods::Result &result) {
        if (!result.supported)
            throw UnsupportedRule{result.message};
        if (!result.message.empty())
            emit(GameEventKind::notice, result.message);
    }
    std::int32_t choose_god_money(std::uint8_t kind) {
        if (!human() || status().days_sleep_walking) {
            // fcn_0043f23e also shows the slot machine for computer/sleep-walking players (it
            // stops itself after 40 frames): the app replays it from the same random state.
            SpinnerState shown{kind, {}, 1, 0, -1, random.state(), 0};
            const auto amount = gods::auto_money(kind, random);
            shown.result = amount;
            effect(EffectKind::money_spinner, actor(), actor_point(actor()));
            effects.back().object_kind = kind;
            effects.back().spinner = shown;
            return amount;
        }
        const auto key = choice_sequence++;
        if (const auto answer = event_choices.find(key); answer != event_choices.end()) {
            random.seed(choice_random_states.at(key));
            return answer->second;
        }
        money_spinner.emplace(kind);
        spinner_random.seed(random.state());
        spinner_elapsed = 0;
        GameDecision prompt{DecisionKind::money_spinner, actor(), 0, 0, "按停止決定金額", {{"停止", 1}}};
        prompt.spinner = SpinnerState{kind, {}, 0}; // phase 0: machine drawn, reels not started
        throw NeedChoice{std::move(prompt), key};
    }
    std::uint32_t upgrade_for_god(std::uint16_t code) {
        unsigned before = 0, after = 0, maximum = 5;
        if (code > 2000 && code < 4000)
            before = world.lands.at(code - 2000).economy.level;
        else if (code > 4000 && code < 6000)
            before = world.facilities.at(code - 4000).level;
        else
            return 0;
        change_building(code, true);
        sparkle_if_top_floor(code, before);
        if (code < 4000) {
            const auto &land = world.lands.at(code - 2000);
            after = land.economy.level;
            if (land.economy.chain_store)
                maximum = 1;
        } else {
            const auto &fac = world.facilities.at(code - 4000);
            after = fac.level;
            constexpr unsigned maxima[]{1, 5, 5, 1, 5};
            if (fac.facility_type < 5)
                maximum = maxima[fac.facility_type];
        }
        return after > before ? 1U | (after == maximum ? 0x80U : 0U) : 0U;
    }
    gods::Callbacks god_callbacks() {
        gods::Callbacks callbacks;
        callbacks.remove_object = [this](auto id) { remove_object(id); };
        callbacks.choose_money = [this](auto kind) { return choose_god_money(kind); };
        callbacks.pay = [this](auto from, auto to, auto amount, auto flags) {
            (void)pay(from, to, amount, flags);
        };
        callbacks.game_over = [this] { return phase == GamePhase::finished; };
        callbacks.hospital = [this](auto slot, auto amount) {
            if (mine_pending) { // core_actions loc_0041be5f: Data 525 after the mine is removed,
                effect(EffectKind::explosion, slot, actor_point(slot));
                speak(slot, SpeechTable::tool, 15, 1); // then tool line 15 (mood 1), then hospital.
            }
            injure(slot);
            add_confinement(slot, amount, true);
        };
        callbacks.voice = [this](auto slot, auto category) { voice(slot, category); };
        callbacks.upgrade_property = [this](auto code) { return upgrade_for_god(code); };
        callbacks.downgrade_property = [this](auto code) {
            change_building(code, false);
            effect(EffectKind::demon_puff, actor(), property_point(code), code); // Data 526
        };
        callbacks.hostility = [this](auto from, auto to, auto delta) { update_hostility(from, to, delta); };
        callbacks.appeared = [this](auto slot, auto kind) {
            effect(EffectKind::god_appear, slot, actor_point(slot));
            effects.back().object_kind = kind;
        };
        callbacks.arrival = [this](auto slot, auto kind) { announce(Announcement::god_arrival, slot, kind); };
        callbacks.sound = [this](auto slot, auto sound) { cue(slot, sound); }; // AUDIO: 54 / 35 / 36
        callbacks.leaving = [this](auto slot, auto kind) {
            effect(EffectKind::god_leave, slot, actor_point(slot));
            effects.back().object_kind = kind;
            effects.back().direction = world.players.at(slot).state.direction;
        };
        callbacks.dog = [this](auto slot, bool on_foot) { // Data 532 dust fight / Data 552 vehicle
            board_movie(on_foot ? EffectKind::dog_fight : EffectKind::dog_vehicle, slot, actor_point(slot),
                        on_foot ? std::optional<EffectPoint>(actor_point(slot)) : std::nullopt);
        };
        return callbacks;
    }
    EventCallbacks event_callbacks() {
        EventCallbacks callbacks;
        callbacks.voice = [this](auto slot, auto category) { voice(slot, category); };
        callbacks.pay = [this](auto payer, auto recipient, auto amount, auto flags) {
            (void)pay(payer, recipient, amount, flags);
        };
        callbacks.add_prison = [this](auto slot, auto days_left) { add_confinement(slot, days_left, false); };
        callbacks.add_hospital = [this](auto slot, auto days_left) {
            injure(slot);
            add_confinement(slot, days_left, true);
        };
        callbacks.disappear = [this](auto slot, auto days_left, auto mode) {
            disappear(slot, days_left, mode);
        };
        callbacks.try_mianzui_jiahuo = [this](auto slot) { return try_defense_card(slot); };
        callbacks.news_picture = [this](auto id, auto code) { // camera on the hit property
            effect(EffectKind::news_movie, actor(), property_point(code), code);
            effects.back().object_kind = id;
        };
        callbacks.set_loan_due_date = [this](auto slot) { set_loan_due_date(slot); };
        callbacks.choose_card = [this](auto slot, auto victim) { return choose_victim_card(slot, victim); };
        callbacks.auction = [this](auto property) { auction(property); };
        callbacks.area_damage = [this](auto x, auto y, auto mode, auto actors) {
            area_damage(x, y, mode, actors);
        };
        callbacks.sell_stock = [this](auto slot, auto stock, auto amount, auto bank) {
            (void)stocks::sell_stock(world, slot, stock, amount, bank);
        };
        callbacks.update_stocks = [this](auto stock) { (void)stocks::update_stocks(world, stock); };
        return callbacks;
    }
    std::string target_label(CardTarget target) const {
        switch (target.kind) {
        case CardTargetKind::actor:
            // Fallback labels; the app shows original names from DecisionOption::subject.
            return target.value < 4 ? player_name(target.value)
                                    : "特殊角色" + std::to_string(target.value - 3);
        case CardTargetKind::property:
            return target.value < 4000 ? "房產" + std::to_string(target.value - 2000)
                                       : "設施" + std::to_string(target.value - 4000);
        case CardTargetKind::stock:
            return "股票" + std::to_string(target.value + 1);
        case CardTargetKind::object:
            return "地圖物件" + std::to_string(target.value);
        case CardTargetKind::card:
            return std::string(services::card_names.at(target.value - 1));
        case CardTargetKind::tool:
            return std::string(services::tool_names.at(target.value - 1));
        }
        throw std::logic_error("invalid card target kind");
    }
    std::optional<CardTarget> choose_target(std::span<const CardTarget> options, const std::string &title,
                                            std::optional<std::size_t> victim = std::nullopt) {
        const auto encode = [](CardTarget value) {
            return static_cast<std::int32_t>((static_cast<unsigned>(value.kind) << 16U) | value.value);
        };
        const auto key = choice_sequence++;
        if (const auto answer = event_choices.find(key); answer != event_choices.end()) {
            if (answer->second < 0)
                return std::nullopt;
            for (auto value : options)
                if (encode(value) == answer->second)
                    return value;
            throw std::logic_error("cached card target no longer valid");
        }
        GameDecision prompt{DecisionKind::action_target, actor(), 0, 0, title, {}};
        for (auto value : options) {
            DecisionOption option{target_label(value), encode(value), std::nullopt, value};
            std::optional<std::uint16_t> code;
            if (value.kind == CardTargetKind::actor)
                code = static_cast<std::uint16_t>(0x8000U | (1U << value.value));
            else if (value.kind == CardTargetKind::property)
                code = value.value;
            else if (value.kind == CardTargetKind::object)
                code = static_cast<std::uint16_t>(0x8000U | (static_cast<unsigned>(value.value) << 8U));
            if (code) {
                const auto point = instance_position(*code);
                option.location = DecisionLocation{point.x, point.y};
            }
            prompt.options.push_back(std::move(option));
        }
        prompt.options.push_back({"取消", -1});
        prompt.victim = victim;
        throw NeedChoice{std::move(prompt), key};
    }
    std::optional<std::uint8_t> choose_facility_type(std::size_t slot) {
        if (world.players.at(slot).state.controller_flags != 1)
            return static_cast<std::uint8_t>(random.next() % 4 + 1);
        const auto key = choice_sequence++;
        if (const auto answer = event_choices.find(key); answer != event_choices.end())
            return answer->second < 0
                       ? std::nullopt
                       : std::optional<std::uint8_t>(static_cast<std::uint8_t>(answer->second));
        throw NeedChoice{
            GameDecision{DecisionKind::action_target,
                         slot,
                         0,
                         0,
                         "請選擇設施類別", // 0x465289; fcn_00440aac(1): right-click returns -1
                         {{"公園", 0}, {"旅館", 1}, {"百貨公司", 2}, {"加油站", 3}, {"研究所", 4}, {"取消", -1}}},
            key};
    }
    CardLookahead lookahead(std::size_t slot, std::size_t count) {
        if (count > 8)
            throw std::invalid_argument("lookahead exceeds original eight nodes");
        CardLookahead result;
        result.nodes.assign(count, 0);
        auto previous = world.players.at(slot).state.last_node_id;
        auto current = world.players[slot].state.node_id;
        for (std::size_t n = 0; n < count; ++n) {
            const auto &node = world.nodes.at(current);
            std::vector<std::uint16_t> candidates;
            for (unsigned i = 0; i < 4; ++i)
                if (node.neighbors[i] && node.neighbors[i] != previous && !(node.flags & (0x40000000U >> i)))
                    candidates.push_back(node.neighbors[i]);
            std::uint16_t next = previous;
            if (candidates.size() == 1)
                next = candidates[0];
            else if (candidates.size() > 1) {
                next = candidates[random.next() % candidates.size()];
                result.crossed_junction = true;
            }
            previous = current;
            current = next;
            result.nodes[n] = next;
        }
        return result;
    }
    CardCallbacks card_callbacks() {
        CardCallbacks callbacks;
        static_cast<EventCallbacks &>(callbacks) = event_callbacks();
        callbacks.hostility = [this](auto from, auto to, auto delta) { update_hostility(from, to, delta); };
        callbacks.turn_around = [this](auto slot) { turn_around(slot); };
        callbacks.change_building = [this](auto code, auto upgrade) { change_building(code, upgrade); };
        callbacks.release_hotel = [this](auto) { release_hotel_guests(); };
        callbacks.auction_property = [this](auto code, auto seller) {
            return auction(code, seller).winner.has_value();
        };
        callbacks.choose_target = [this](auto card, auto options) {
            auto chosen = choose_target(options, std::string(services::card_names.at(card - 1)) + "：選擇目標");
            chosen_card_target = chosen; // Presentation only (card_effect).
            return chosen;
        };
        callbacks.choose_inventory = [this](auto, auto victim) -> std::optional<CardTarget> {
            std::vector<CardTarget> choices;
            for (auto card : world.players.at(victim).state.cards)
                if (card) {
                    const CardTarget value{CardTargetKind::card, card};
                    if (std::find(choices.begin(), choices.end(), value) == choices.end())
                        choices.push_back(value);
                }
            for (std::size_t i = 0; i < 13; ++i)
                if (world.players[victim].state.tool_amounts[i])
                    choices.push_back({CardTargetKind::tool, static_cast<std::uint16_t>(i + 1)});
            return choose_target(choices, "請選擇要搶的卡片或道具", victim);
        };
        callbacks.stolen = [this](auto, auto victim, auto item, bool preset) {
            if (!preset)
                return; // a human chose it on the picker
            auto &box = announce(Announcement::stolen, actor(), item.kind == CardTargetKind::tool ? 1 : 0);
            box.subject = static_cast<std::int32_t>(victim);
            box.value = item.value;
        };
        callbacks.choose_facility_type = [this](auto slot) { return choose_facility_type(slot); };
        callbacks.say = [this](auto slot, auto row, auto card, auto mood) {
            if (row == 2 && (card == 11 || card == 12))
                card_picture(); // The owner complains after the monster/demolish picture.
            card_speech(slot, row, card, mood);
        };
        callbacks.passive_card = [this](auto slot, auto card) {
            events.push_back({GameEventKind::notice, slot, 0, world.players.at(slot).state.node_id,
                              "使用" + std::string(services::card_names.at(card - 1U))});
            events.back().card = card;
        };
        callbacks.try_free = [this](auto victim, auto owner, auto amount) {
            return try_free_card(victim, owner, amount);
        };
        callbacks.try_frame = [this](auto victim, auto mode) { return try_frame_card(victim, mode); };
        callbacks.remove_object = [this](auto id) { remove_object(id); };
        callbacks.attach_god = [this](auto slot, auto id) {
            check_god(
                gods::attach(world, random, slot, id, world.players.at(slot).state.node_id, god_callbacks()));
        };
        callbacks.detach_god = [this](auto slot) { check_god(gods::detach(world, slot, god_callbacks())); };
        callbacks.market_closed = [this] {
            return world.news_effect_days != 0 ||
                   holidays::is_closed(
                       {world.header.date.year, world.header.date.month, world.header.date.day},
                       world.header.map_id, world.header.stage_id);
        };
        callbacks.lookahead_nodes = [this](auto slot, auto count) { return lookahead(slot, count); };
        callbacks.nearby_instances = [this](auto slot) {
            const auto &state = world.players.at(slot).state;
            return instance_codes(state.x, state.y, -1);
        };
        return callbacks;
    }
    std::vector<std::uint16_t> tool_targets(unsigned tool, std::uint16_t source = 0) const {
        const auto flags = tools::selection_flags(tool, source);
        unsigned mask = flags & 0xffU;
        if (mask & 0x40U)
            mask = (mask & 0x80U) | 0x37U;
        const auto filter = (flags >> 8U) & 0xffU;
        const board::Camera camera{{player().x, player().y},
                                   static_cast<std::uint8_t>(world.camera_rotation)};
        const auto visible = [&](int x, int y) {
            if (mask & 0x80U)
                return true;
            const auto screen = board::project({x, y}, camera);
            return screen && screen->x >= 0 && screen->x < 440 && screen->y >= 40 && screen->y < 480;
        };
        std::vector<std::uint16_t> result;
        if (mask & 1U)
            for (std::size_t i = 1; i < world.nodes.size(); ++i) {
                const auto &node = world.nodes[i];
                if (visible(node.x, node.y) &&
                    ((tool != 2 && tool != 3 && tool != 4) || !(node.flags & 0x00ffff00U)))
                    result.push_back(static_cast<std::uint16_t>(i));
            }
        if (mask & 2U)
            for (std::size_t i = 1; i < world.lands.size(); ++i) {
                const auto &land = world.lands[i];
                if (visible(land.x, land.y) &&
                    (filter != 8 || (!land.economy.owner_code && !land.economy.level)))
                    result.push_back(static_cast<std::uint16_t>(2000 + i));
            }
        if (mask & 4U)
            for (std::size_t i = 1; i < world.facilities.size(); ++i) {
                const auto &facility = world.facilities[i];
                if (visible(facility.x, facility.y) &&
                    (filter != 8 || (!facility.owner_code && !facility.level)))
                    result.push_back(static_cast<std::uint16_t>(4000 + i));
            }
        if (mask & 0x10U) {
            for (std::size_t i = 0; i < world.players.size(); ++i) {
                const auto &entry = world.players[i];
                if (entry.state.x && entry.state.controller_flags && !entry.status.days_disappearing &&
                    visible(entry.state.x, entry.state.y))
                    result.push_back(static_cast<std::uint16_t>(0x8000U | (1U << i)));
            }
            for (std::size_t i = 0; i < 4; ++i) {
                const auto &entry = world.special_actors[i];
                if (entry.x && !entry.inactive_state && visible(entry.x, entry.y))
                    result.push_back(static_cast<std::uint16_t>(0x8000U | (0x10U << i)));
            }
        }
        if (mask & 0x20U)
            for (std::size_t i = 0; i < world.objects.size(); ++i) {
                const auto &object = world.objects[i];
                if (object.node_id) {
                    const auto &node = world.nodes.at(object.node_id);
                    if (visible(node.x, node.y))
                        result.push_back(static_cast<std::uint16_t>(0x8000U | ((i + 1U) << 8U)));
                }
            }
        return result;
    }
    std::uint16_t choose_tool_target(unsigned tool, std::uint16_t source) {
        const auto key = choice_sequence++;
        if (const auto answer = event_choices.find(key); answer != event_choices.end())
            return static_cast<std::uint16_t>(answer->second);
        // items.cpp matches "目的" to pick the destination cursor; keep it in that prompt.
        GameDecision prompt{DecisionKind::action_target,
                            actor(),
                            0,
                            0,
                            source ? "選擇目的地" : tool == 8 ? "選擇點數" : "選擇目標",
                            {}};
        if (tool == 8)
            for (int value = 1; value <= 6; ++value)
                prompt.options.push_back({std::to_string(value) + "點", value});
        else
            for (auto code : tool_targets(tool, source)) {
                std::optional<CardTarget> subject; // nullopt: a road node
                if (code >= 2000 && code < 6000)
                    subject = CardTarget{CardTargetKind::property, code};
                else if (code >= 6000 && (code & 0xffU))
                    subject = CardTarget{CardTargetKind::actor, static_cast<std::uint16_t>(std::countr_zero(
                                                                    static_cast<unsigned>(code & 0xffU)))};
                else if (code >= 6000)
                    subject =
                        CardTarget{CardTargetKind::object, static_cast<std::uint16_t>((code >> 8U) & 0x3fU)};
                const auto point = instance_position(code);
                prompt.options.push_back({subject ? target_label(*subject) : std::string("道路"), code,
                                          DecisionLocation{point.x, point.y}, subject});
            }
        prompt.options.push_back({"取消", 0});
        throw NeedChoice{std::move(prompt), key};
    }
    board::Point instance_position(std::uint16_t code) const {
        if (code < 2000) {
            const auto &node = world.nodes.at(code);
            return {node.x, node.y};
        }
        if (code < 4000) {
            const auto &land = world.lands.at(code - 2000);
            return {land.x, land.y};
        }
        if (code < 6000) {
            const auto &facility = world.facilities.at(code - 4000);
            return {facility.x, facility.y};
        }
        if (code & 0xffU) {
            const auto slot = static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(code & 0xffU)));
            if (slot < 4) {
                const auto &p = world.players.at(slot).state;
                return {p.x, p.y};
            }
            const auto &p = world.special_actors.at(slot - 4);
            return {p.x, p.y};
        }
        const auto &node = world.nodes.at(world.objects.at(((code >> 8U) & 0x3fU) - 1).node_id);
        return {node.x, node.y};
    }
    void robot(std::size_t slot) {
        const auto owner = world.players.at(slot).state;
        auto &robot = world.special_actors[4];
        robot.x = owner.x;
        robot.y = owner.y;
        robot.node_id = owner.node_id;
        robot.last_node_id = owner.last_node_id;
        robot.owner = static_cast<std::uint8_t>(slot);
        robot.direction = owner.direction;
        robot.inactive_state = 0;
        // Original 0x40deb9 sets nine steps. Actor8 never sets road occupancy bits.
        for (unsigned step = 0; step < 9; ++step) {
            const auto &old = world.nodes.at(robot.node_id);
            std::vector<std::uint16_t> choices;
            for (unsigned i = 0; i < 4; ++i)
                if (old.neighbors[i] && old.neighbors[i] != robot.last_node_id &&
                    !(old.flags & (0x40000000U >> i)))
                    choices.push_back(old.neighbors[i]);
            const auto next = choices.empty() ? robot.last_node_id : choices[random.next() % choices.size()];
            const auto &node = world.nodes.at(next);
            robot.last_node_id = robot.node_id;
            robot.node_id = next;
            robot.direction = direction::from_delta(node.x - robot.x, node.y - robot.y);
            robot.x = static_cast<std::uint16_t>(node.x);
            robot.y = static_cast<std::uint16_t>(node.y);
            const auto object = static_cast<std::uint16_t>((node.flags >> 16U) & 255U);
            if (object)
                remove_object(object);
            events.push_back({GameEventKind::notice, slot, 0, next, "机器娃娃清理道路"});
        }
        robot.inactive_state = 3;
    }
    bool restore_last_state() {
        const auto slot = actor();
        if (!previous_turn[slot])
            return false;
        const auto header = world.header;
        const auto rotation = world.camera_rotation;
        const auto lease = world.lease_mode;
        world = *previous_turn[slot];
        const auto date = world.header.date;
        const auto price = world.header.price_index;
        const auto initial = world.header.initial_fund;
        world.header = header;
        world.header.date = date;
        world.header.price_index = price;
        world.header.initial_fund = initial;
        world.camera_rotation = rotation;
        world.lease_mode = lease;
        // Original history excludes rand's CRT state and the current player.
        emit(GameEventKind::notice, "时光机回到上次出发前");
        return true;
    }
    tools::Callbacks tool_callbacks() {
        tools::Callbacks callbacks;
        callbacks.robot = [this](auto slot) { robot(slot); };
        callbacks.say = [this](auto slot, auto line) { speak(slot, SpeechTable::tool, line, 0); };
        callbacks.restore_last_state = [this] { return restore_last_state(); };
        callbacks.remote_dice = [this](auto, unsigned amount) {
            forced_roll = amount;
            begin_roll();
        };
        callbacks.upgrade = [this](auto code) {
            const auto before = property_level(code);
            change_building(code, true);
            effect(EffectKind::build_rise, actor(), property_point(code), code); // Data 553
            sparkle_if_top_floor(code, before);
        };
        callbacks.before_teleport = [this](auto slot) {
            if (slot == actor() && (player().controller_flags & 1U))
                previous_turn[slot] = world;
            clear_absences(slot);
        };
        callbacks.blast = [this](auto code, bool nuclear, auto slot) {
            const auto point = instance_position(code);
            area_damage(static_cast<std::uint16_t>(point.x), static_cast<std::uint16_t>(point.y),
                        nuclear ? 1 : 0, true, static_cast<int>(slot), nuclear ? -1 : 100);
            effect(nuclear ? EffectKind::nuclear_missile : EffectKind::missile, slot, {point.x, point.y},
                   code > 2000 && code < 6000 ? code : 0);
        };
        return callbacks;
    }
    // ---- Pre-choice presentation (god entrance movie before the spinner, etc.) ----
    // A rule that needs a human answer throws NeedChoice and is re-run from its checkpoint once the
    // answer is known. Everything it emitted BEFORE the throw is deterministic (same world, same
    // random state, answer not yet read), so it is delivered right away, ahead of the decision, as
    // the original shows it; the re-run emits it again and those copies are dropped here.
    struct DeliveredPresentation {
        std::vector<GameEvent> events;
        std::vector<EffectEvent> effects; // compared without after_events (drain-relative)
    };
    // Removes, from this attempt's output events[event_count..] / effects[effect_count..], the
    // in-order copies of what earlier attempts already delivered. If the copies are not all found
    // (a replay that diverged), nothing is removed: presentation may repeat but is never lost.
    void drop_replayed(std::size_t event_count, std::size_t effect_count, const DeliveredPresentation &shown) {
        std::vector<std::size_t> removed;
        for (std::size_t i = event_count; i < events.size() && removed.size() < shown.events.size(); ++i)
            if (events[i] == shown.events[removed.size()])
                removed.push_back(i);
        if (!removed.empty() && removed.size() == shown.events.size()) {
            for (auto it = removed.rbegin(); it != removed.rend(); ++it)
                events.erase(events.begin() + static_cast<std::ptrdiff_t>(*it));
            for (std::size_t i = effect_count; i < effects.size(); ++i)
                effects[i].after_events -= static_cast<std::size_t>(
                    std::lower_bound(removed.begin(), removed.end(), effects[i].after_events) - removed.begin());
        }
        removed.clear();
        const auto same = [](EffectEvent a, const EffectEvent &b) {
            a.after_events = b.after_events;
            return a == b;
        };
        for (std::size_t i = effect_count; i < effects.size() && removed.size() < shown.effects.size(); ++i)
            if (same(effects[i], shown.effects[removed.size()]))
                removed.push_back(i);
        if (!removed.empty() && removed.size() == shown.effects.size())
            for (auto it = removed.rbegin(); it != removed.rend(); ++it)
                effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    void run_rule(std::function<void()> action, bool first_attempt = true, bool finish_after = true,
                  std::function<void()> on_success = {},
                  std::shared_ptr<const DeliveredPresentation> shown = nullptr) {
        if (first_attempt) {
            event_choices.clear();
            choice_random_states.clear();
        }
        bool completed = false;
        choice_sequence = 0;
        World checkpoint = world;
        const auto checkpoint_random = random.state();
        const auto checkpoint_ai = card_ai_memory;
        const auto event_count = events.size();
        const auto effect_count = effects.size();
        const auto old_phase = phase;
        const auto restore = [&] {
            world = std::move(checkpoint);
            random.seed(checkpoint_random);
            events.resize(event_count);
            effects.resize(effect_count, EffectEvent{EffectKind::dice_roll});
            phase = old_phase;
            card_ai_memory = checkpoint_ai;
        };
        try {
            action();
            if (shown)
                drop_replayed(event_count, effect_count, *shown);
            pending_continuation = {};
            event_choices.clear();
            choice_random_states.clear();
            completed = true;
            if (phase != GamePhase::finished &&
                (finish_after || (phase != GamePhase::rolling && phase != GamePhase::moving))) {
                if (finish_after)
                    finish_turn();
                else {
                    decision.reset();
                    set_phase(GamePhase::human_ready);
                }
            }
        } catch (const NeedChoice &choice) {
            // Nested fortune/card/auction choices replay a transaction with its
            // answers, never its mutations or already-consumed random draws.
            // Its pre-choice presentation is kept (see DeliveredPresentation).
            if (shown)
                drop_replayed(event_count, effect_count, *shown);
            std::vector<GameEvent> fresh_events(events.begin() + static_cast<std::ptrdiff_t>(event_count),
                                                events.end());
            std::vector<EffectEvent> fresh_effects(effects.begin() + static_cast<std::ptrdiff_t>(effect_count),
                                                   effects.end());
            restore();
            events.insert(events.end(), fresh_events.begin(), fresh_events.end());
            effects.insert(effects.end(), fresh_effects.begin(), fresh_effects.end());
            auto delivered = std::make_shared<DeliveredPresentation>(shown ? *shown : DeliveredPresentation{});
            delivered->events.insert(delivered->events.end(), fresh_events.begin(), fresh_events.end());
            delivered->effects.insert(delivered->effects.end(), fresh_effects.begin(), fresh_effects.end());
            decision = choice.decision;
            pending_choice_key = choice.key;
            pending_continuation = [this, action, finish_after, on_success,
                                    delivered = std::shared_ptr<const DeliveredPresentation>(std::move(delivered))] {
                run_rule(action, false, finish_after, on_success, delivered);
            };
            set_phase(GamePhase::awaiting_decision);
        } catch (const UnsupportedRule &missing) {
            restore();
            pending_continuation = {};
            event_choices.clear();
            unsupported(missing.message, finish_after ? GamePhase::advancing_turn : GamePhase::human_ready);
        }
        if (completed && on_success)
            on_success();
    }
    // The original shows the news/fortune screen before the handler's effects (bubbles,
    // boxes), so the announcement is inserted ahead of the events the handler emitted.
    void announce_event(bool news, std::size_t slot, const EventResult &result, std::size_t position) {
        GameEvent event{GameEventKind::notice, slot, 0, world.players.at(slot).state.node_id, {}};
        event.announcement = news ? Announcement::news : Announcement::fortune;
        event.announcement_id = result.event_id;
        event.property = result.property;
        event.subject = result.subject;
        event.company = result.company;
        event.value = result.value;
        event.players = result.players;
        event.player_amounts = result.player_amounts;
        events.insert(events.begin() + static_cast<std::ptrdiff_t>(std::min(position, events.size())),
                      std::move(event));
    }
    // The news/fortune screen is inserted ahead of its handler's output only once the handler has
    // returned, so a handler that stops for a choice must not deliver its output early: it is
    // dropped here and re-emitted (after the screen) when the rule re-runs with the answer.
    template <class Handler> EventResult screen_first(Handler handler) {
        const auto event_count = events.size();
        const auto effect_count = effects.size();
        try {
            return handler();
        } catch (const NeedChoice &) {
            events.resize(event_count);
            effects.resize(effect_count, EffectEvent{EffectKind::dice_roll});
            throw;
        }
    }
    void resolve_event(bool news) {
        run_rule([this, news] {
            const auto position = events.size();
            const auto effect_count = effects.size();
            const auto result = screen_first([&] {
                return news ? apply_news(world, random, event_callbacks())
                            : apply_fortune(world, random, actor(), event_callbacks());
            });
            if (!result.supported)
                throw UnsupportedRule{result.message};
            announce_event(news, actor(), result, position);
            for (auto i = effect_count; i < effects.size(); ++i) // the handler's movies play after the screen
                if (effects[i].after_events >= position)
                    ++effects[i].after_events;
            emit(GameEventKind::notice, result.message);
        });
    }
    void shop_prompt() {
        GameDecision prompt{DecisionKind::shop, actor(), 0, 0, "商店：選擇購買或出售的物品", {}};
        for (auto card : shop->cards)
            prompt.options.push_back({"買 " + std::string(services::card_names.at(card - 1)) + " " +
                                          std::to_string(services::card_prices.at(card - 1)) + "點",
                                      100 + card});
        for (auto tool : shop->tools)
            prompt.options.push_back({"買 " + std::string(services::tool_names.at(tool - 1)) + " " +
                                          std::to_string(services::tool_prices.at(tool - 1)) + "點",
                                      200 + tool});
        for (auto card : player().cards)
            if (card && std::none_of(prompt.options.begin(), prompt.options.end(),
                                     [&](const auto &option) { return option.value == 300 + card; }))
                prompt.options.push_back(
                    {"賣 " + std::string(services::card_names.at(card - 1)), 300 + card});
        for (std::size_t tool = 0; tool < services::tool_names.size(); ++tool)
            if (player().tool_amounts[tool])
                prompt.options.push_back(
                    {"賣 " + std::string(services::tool_names[tool]), static_cast<std::int32_t>(401 + tool)});
        prompt.options.push_back({"離開", 0});
        decision = std::move(prompt);
        set_phase(GamePhase::awaiting_decision);
    }
    void open_shop(std::uint16_t property_code) {
        shop_property = property_code;
        shop = services::open_shop(world, random, actor(), property_code);
        if (human())
            shop_prompt();
        else {
            services::close_shop(world, *shop, property_code);
            shop.reset();
            finish_turn();
        }
    }
    bool shop_action(int action, unsigned quantity) {
        if (!shop)
            return false;
        if (!action) {
            services::close_shop(world, *shop, shop_property);
            shop.reset();
            finish_turn();
            return true;
        }
        bool accepted = false;
        if (action > 100 && action <= 130)
            accepted = services::buy_card(world, *shop, actor(), static_cast<std::uint8_t>(action - 100));
        else if (action > 200 && action <= 213)
            accepted = services::buy_tool(world, *shop, actor(), static_cast<std::uint8_t>(action - 200));
        else if (action > 300 && action <= 330)
            accepted = services::sell_card(world, *shop, actor(), static_cast<std::uint8_t>(action - 300));
        else if (action > 400 && action <= 413)
            accepted =
                services::sell_tool(world, *shop, actor(), static_cast<std::uint8_t>(action - 400), quantity);
        if (accepted) {
            emit(GameEventKind::notice, action < 300 ? "购买完成" : "出售完成");
            shop_prompt();
        }
        return accepted;
    }
    services::MagicCallbacks magic_callbacks() {
        services::MagicCallbacks callbacks;
        callbacks.fortune = [this](auto slot) {
            const auto position = events.size();
            const auto result = screen_first([&] { return apply_fortune(world, random, slot, event_callbacks()); });
            if (!result.supported)
                throw UnsupportedRule{result.message};
            announce_event(false, slot, result, position);
            emit(GameEventKind::notice, result.message);
        };
        callbacks.defense = [this](auto slot) { return try_defense_card(slot); };
        callbacks.hostility = [this](auto from, auto target, auto delta) {
            update_hostility(from, target, delta);
        };
        callbacks.prison = [this](auto slot, auto duration) {
            add_confinement(slot, static_cast<int>(duration), false);
        };
        callbacks.hospital = [this](auto slot, auto duration) {
            add_confinement(slot, static_cast<int>(duration), true);
        };
        callbacks.upgrade = [this](auto property) { change_building(property, true); };
        callbacks.downgrade = [this](auto property) { change_building(property, false); };
        callbacks.turn_around = [this](auto slot) { turn_around(slot); };
        callbacks.auction = [this](auto property, auto seller) {
            auction(property, static_cast<int>(seller));
        };
        return callbacks;
    }
    void magic_effect(unsigned effect) {
        const auto category = magic_category;
        run_rule([this, category, effect] {
            if (!human()) { // presentation only: the computer's cast (group, effect, Panel 20 movie) first
                emit(GameEventKind::notice, "魔法屋施法");
                events.back().value = static_cast<std::int32_t>(effect);
                events.back().second_subject = static_cast<std::int32_t>(category);
            }
            const auto targets = services::magic_targets(world, category);
            if (!services::apply_magic(world, random, actor(), targets, effect, magic_callbacks()))
                throw UnsupportedRule{"这项魔法的效果尚未完成"};
            emit(GameEventKind::notice, std::string(services::magic_effects.at(effect)));
        });
    }
    void magic_effect_prompt(unsigned category) {
        magic_category = category;
        GameDecision prompt{DecisionKind::magic_effect, actor(), 0, 0, "選擇魔法效果", {}};
        for (std::size_t i = 0; i < services::magic_effects.size(); ++i)
            prompt.options.push_back({std::string(services::magic_effects[i]), static_cast<std::int32_t>(i)});
        prompt.options.push_back({"重新選擇角色", -1});
        decision = std::move(prompt);
        set_phase(GamePhase::awaiting_decision);
    }
    void open_magic() {
        if (!human()) {
            const auto selected = services::choose_magic_ai(world, random, actor());
            if (!selected) {
                finish_turn();
                return;
            }
            magic_category = selected->category;
            magic_effect(selected->effect);
            return;
        }
        GameDecision prompt{DecisionKind::magic_category, actor(), 0, 0, "選擇魔法作用的角色", {}};
        for (std::size_t i = 0; i < services::magic_categories.size(); ++i)
            if (!services::magic_targets(world, static_cast<unsigned>(i)).empty())
                prompt.options.push_back(
                    {std::string(services::magic_categories[i]), static_cast<std::int32_t>(i)});
        if (prompt.options.empty()) {
            finish_turn();
            return;
        }
        decision = std::move(prompt);
        set_phase(GamePhase::awaiting_decision);
    }
    void collect_bank_debt(std::size_t slot, std::int32_t amount) {
        auto &state = world.players.at(slot).state;
        state.bank_balance = subtract32(state.bank_balance, amount);
        if (state.bank_balance >= 0)
            return;
        state.cash = add32(state.cash, state.bank_balance);
        state.bank_balance = 0;
        if (state.cash < 0) {
            state.cash = 0;
            bankrupt(slot);
        }
    }
    void collect_due_loan() {
        const auto due = calendar::unpack(status().loan_due_date);
        if (!calendar::is_valid(due))
            return;
        const auto left = calendar::days_between(
            {world.header.date.year, world.header.date.month, world.header.date.day}, due);
        if (left == 0) {
            const auto amount = player().loan;
            collect_bank_debt(actor(), amount);
            player().loan = 0;
            status().loan_due_date = 0;
            emit(GameEventKind::bank_transfer, "贷款到期，银行收回贷款", amount);
        } else if (left > 0 && left <= 3)
            emit(GameEventKind::notice, "贷款将在" + std::to_string(left) + "天后到期");
    }
    void reconcile_financing(bool after_withdrawal) {
        int owner = -1;
        for (std::size_t i = 1; i < world.commercials.size(); ++i)
            if (world.commercials[i].commerce_type == 7 && world.commercials[i].owner_code)
                owner = world.commercials[i].owner_code - 1;
        if (after_withdrawal) {
            if (owner < 0 || owner == static_cast<int>(actor()))
                return;
            auto &state = world.players.at(static_cast<std::size_t>(owner)).state;
            if (!state.special_finance)
                return;
            std::int32_t bank_total = 0;
            for (std::size_t i = 0; i < world.players.size(); ++i)
                if (static_cast<int>(i) != owner && world.players[i].state.controller_flags)
                    bank_total = add32(bank_total, world.players[i].state.bank_balance);
            if (bank_total < state.special_finance) {
                const auto amount = subtract32(state.special_finance, bank_total);
                collect_bank_debt(static_cast<std::size_t>(owner), amount);
                state.special_finance = std::max(0, subtract32(state.special_finance, amount));
            }
        } else
            for (std::size_t i = 0; i < world.players.size(); ++i) {
                auto &state = world.players[i].state;
                if (static_cast<int>(i) != owner && state.controller_flags && state.special_finance) {
                    collect_bank_debt(i, state.special_finance);
                    state.special_finance = 0;
                }
            }
    }
    void set_loan_due_date(std::size_t slot) {
        auto &entry = world.players.at(slot);
        if (!entry.status.loan_due_date)
            entry.status.loan_due_date = calendar::pack(holidays::loan_due_date(
                {world.header.date.year, world.header.date.month, world.header.date.day}, world.header.map_id,
                world.header.stage_id));
    }
    bool borrow(std::int32_t amount) {
        if (amount <= 0 || status().days_bank_no_loans || amount > subtract32(wealth(actor()), player().loan))
            return false;
        player().loan = add32(player().loan, amount);
        player().bank_balance = add32(player().bank_balance, amount);
        set_loan_due_date(actor());
        emit(GameEventKind::bank_transfer, "贷款已存入银行", amount);
        return true;
    }
    bool repay(std::int32_t amount, bool clear_due_date) {
        if (amount <= 0 || amount > player().loan ||
            static_cast<std::int64_t>(player().cash) + player().bank_balance < amount)
            return false;
        const auto from_bank = std::min(std::max(player().bank_balance, 0), amount);
        player().bank_balance = subtract32(player().bank_balance, from_bank);
        player().cash = subtract32(player().cash, amount - from_bank);
        player().loan = subtract32(player().loan, amount);
        if (!player().loan && clear_due_date)
            status().loan_due_date = 0;
        emit(GameEventKind::bank_transfer, "偿还贷款", amount);
        return true;
    }
    void ai_bank_loan() {
        if (player().loan) {
            const auto due = calendar::unpack(status().loan_due_date);
            const auto remaining_days =
                calendar::is_valid(due)
                    ? calendar::days_between(
                          {world.header.date.year, world.header.date.month, world.header.date.day}, due)
                    : 0;
            const auto total = add32(player().cash, player().bank_balance);
            if (player().bank_balance > multiply32(player().loan, 2) ||
                (remaining_days <= 6 && static_cast<double>(total) >= player().loan * 1.1))
                (void)repay(player().loan, false);
        } else {
            const bool attempt = random.next() % 10 == 0;
            if ((attempt || add32(player().cash, player().bank_balance) < 30000) &&
                !status().days_bank_no_loans && status().loan_ratio) {
                const auto amount = multiply32(status().loan_ratio, wealth(actor())) / 100;
                if (amount > 0) {
                    player().loan = amount;
                    player().bank_balance = add32(player().bank_balance, amount);
                    set_loan_due_date(actor());
                    emit(GameEventKind::bank_transfer, "电脑玩家办理贷款", amount);
                }
            }
        }
    }
    bool owns_current_bank() const {
        const auto code = world.nodes.at(player().node_id).type_and_index;
        return code > 6000 && code < 8000 &&
               static_cast<std::size_t>(code - 6000) < world.commercials.size() &&
               world.commercials[code - 6000].owner_code == actor() + 1;
    }
    std::int32_t financing_limit() const {
        std::int32_t total = 0;
        for (std::size_t i = 0; i < world.players.size(); ++i)
            if (i != actor())
                total = add32(total, world.players[i].state.bank_balance);
        return subtract32(total, player().special_finance);
    }
    bool change_financing(std::int32_t amount, bool borrowing) {
        if (amount <= 0 || !owns_current_bank() || resume_movement)
            return false;
        if (borrowing) {
            if (status().days_bank_no_loans || amount > financing_limit())
                return false;
            player().bank_balance = add32(player().bank_balance, amount);
            player().special_finance = add32(player().special_finance, amount);
        } else {
            if (amount > player().special_finance || amount > add32(player().cash, player().bank_balance))
                return false;
            collect_bank_debt(actor(), amount);
            player().special_finance = subtract32(player().special_finance, amount);
        }
        emit(GameEventKind::bank_transfer, borrowing ? "银行融资已存入账户" : "归还银行融资", amount);
        return true;
    }
    void open_bank(bool passing) {
        resume_movement = passing;
        if (status().days_rejected_by_bank) {
            emit(GameEventKind::notice, "银行暂停办理这位玩家的业务");
            leave_bank();
            return;
        }
        if (human()) {
            std::vector<DecisionOption> options{{"存款", 1}};
            if (!status().days_bank_no_loans)
                options.push_back({"提款", 2});
            if (!passing) {
                if (!status().days_bank_no_loans)
                    options.push_back({"貸款", 3});
                if (player().loan > 0)
                    options.push_back({"還貸", 4});
                if (owns_current_bank()) {
                    if (!status().days_bank_no_loans && financing_limit() > 0)
                        options.push_back({"銀行融資", 5});
                    if (player().special_finance > 0)
                        options.push_back({"歸還融資", 6});
                }
            }
            options.push_back({"離開", 0});
            decision = GameDecision{DecisionKind::bank, actor(), 0, 0, "辦理存款或提款", std::move(options)};
            set_phase(GamePhase::awaiting_decision);
        } else {
            run_rule(
                [this, passing] {
                    using E = numeric::Extended;
                    const auto total = add32(player().cash, player().bank_balance);
                    const float ratio = total ? (E::from(player().cash) / E::from(total)).single() : 0.0F;
                    float target = (E::from(status().init_cash_ratio) / E::from(100)).single();
                    if (world.header.date.day <= 7)
                        target = (E::from(target) * E::from(1.5)).single();
                    else if (world.header.date.day >= 26)
                        target = (E::from(target) * E::from(0.5)).single();
                    if (target >= 1.0F)
                        target = 0.9F;
                    if (target <= 0.0F)
                        target = 0.1F;
                    // Original retains this subtraction in x87 and a binary64 spill;
                    // rounding it to float can change the quarter-ratio boundary.
                    const double difference = static_cast<double>(ratio) - target;
                    if ((total && (difference >= 0.25 || difference <= -0.25)) || player().cash == 0) {
                        player().cash = (E::from(total) * E::from(target)).integer();
                        player().bank_balance = subtract32(total, player().cash);
                        emit(GameEventKind::bank_transfer, "电脑玩家调整现金与存款");
                        reconcile_financing(true);
                    }
                    if (!passing && phase != GamePhase::finished)
                        ai_bank_loan();
                },
                true, false,
                [this] {
                    if (phase != GamePhase::finished)
                        leave_bank();
                });
        }
    }
    void leave_bank() {
        decision.reset();
        if (bank_resume_encounter) {
            bank_resume_encounter = false;
            resume_movement = false;
            step_encounter();
        } else if (resume_movement && remaining > 0) {
            resume_movement = false;
            start_step();
        } else {
            resume_movement = false;
            finish_turn();
        }
    }
    void land() {
        const auto &node = world.nodes.at(player().node_id);
        const unsigned kind = node.flags & 0xffU;
        if (kind != 0 && status().days_sleep_walking) {
            finish_turn();
            return;
        }
        if (kind == 0) {
            property_pending = true;
            purchased_property = false;
            research_pending = node.type_and_index > 4000 && node.type_and_index < 6000;
            property_landing = node.type_and_index != 0; // core_actions loc_0041b111 (0x88)
            if (node.type_and_index > 2000 && node.type_and_index < 4000)
                property(node.type_and_index - 2000);
            else if (node.type_and_index > 4000 && node.type_and_index < 6000)
                facility(node.type_and_index - 4000);
            else if (node.type_and_index > 6000 && node.type_and_index < 8000)
                commercial(node.type_and_index - 6000);
            else if (node.type_and_index == 0)
                finish_turn();
            else
                // Hospital/prison buildings 8001/8002 (any code outside the three ranges):
                // the original falls into the common property tail
                // (core_actions:369-371 -> loc_0041b077: after-property god check + flight
                // bombardment), then ends the turn. finish_turn() runs that tail because
                // property_pending is set.
                finish_turn();
        } else if (kind == 1)
            finish_turn();
        else if (kind == 2 || kind == 3)
            resolve_event(kind == 2);
        else if (kind == 4 || kind == 5)
            visit_confinement(kind == 5);
        else if (kind == 9)
            open_lottery();
        else if (kind == 13) {
            effect(EffectKind::item_box, actor(), actor_point(actor())); // Data 536 "?" spin first
            receive_random_card();
        }
        else if (kind == 15)
            open_shop(node.type_and_index);
        else if (kind == 16)
            open_magic();
        else if (kind >= 6 && kind <= 8 && interactive_mini_games && human()) {
            // RICH4.CFG[1] != 0 and a human (small_games.asm:4229): the interactive game. Nothing
            // is drawn here; submit_mini_game advances the stream by the game's own rand() calls.
            GameDecision prompt{DecisionKind::mini_game, actor(), 0, 0, "小遊戲", {}};
            prompt.mini_game = MiniGameState{static_cast<std::uint8_t>(kind - 6), player().character, random.state()};
            decision = std::move(prompt);
            set_phase(GamePhase::awaiting_decision);
        } else if (kind >= 6 && kind <= 8) {
            // Original RICH4.CFG[1] == 0 mode: built-in auto result for each
            // minigame, including humans. Keep both reward and speech RNG draws.
            const auto reward = static_cast<std::uint16_t>(random.next() % 20 + 50);
            const auto category = static_cast<std::uint8_t>(random.next() & 1U);
            player().points = static_cast<std::uint16_t>(player().points + reward);
            // SG:4392-4425: sprintf("得點券%d點") box for 2000 ms, then the player's event line.
            announce(Announcement::mini_game_points, actor()).amount = reward;
            emit(GameEventKind::points_received, "小游戏获得" + std::to_string(reward) + "点", reward);
            voice(actor(), category);
            finish_turn();
        } else if (kind >= 10 && kind <= 12) {
            const auto amount = kind == 10 ? 50 : kind == 11 ? 30 : 10;
            player().points = static_cast<std::uint16_t>(player().points + amount);
            effect(EffectKind::coupon, actor(), actor_point(actor())); // Data 537 ticket, then the box
            // The original shows the coupon box before the speech bubble.
            announce(Announcement::point_coupon, actor()).amount = amount;
            if (kind == 10)
                voice(actor(), static_cast<std::uint8_t>(random.next() & 1U));
            else if (kind == 11)
                voice(actor(), 2);
            emit(GameEventKind::points_received, "获得" + std::to_string(amount) + "点", amount);
            finish_turn();
        } else if (kind == 14)
            open_bank(false);
        else
            // Node kind > 16: the original's `cmp ebx,0x10; ja loc_0041b3d0`
            // (core_actions:361) ends the turn silently. No shipped map has one.
            finish_turn();
    }
    std::int32_t wealth(std::size_t slot) const {
        return stocks::player_wealth(world, slot);
    }
    void update_price_index() {
        std::int32_t total = 0, active = 0;
        for (std::size_t i = 0; i < world.players.size(); ++i)
            if (world.players[i].state.controller_flags) {
                total = add32(total, wealth(i));
                ++active;
            }
        if (active && world.header.initial_fund > 0)
            world.header.price_index =
                std::max(world.header.price_index, (total / active) / world.header.initial_fund);
    }
    // MONTHLY: presentation figures shared by the monthly-cycle screens (no rule effect).
    MonthlyFigures monthly_figures() const {
        MonthlyFigures figures;
        for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
            const auto &state = world.players[slot].state;
            if (state.controller_flags)
                figures.active |= static_cast<std::uint8_t>(1U << slot);
            figures.characters[slot] = state.character;
        }
        return figures;
    }
    void monthly_effect(EffectKind kind, MonthlyFigures figures) {
        EffectEvent event{kind, actor(), player().node_id, actor_point(actor()), std::nullopt, 0, {}, 0,
                          events.size()};
        event.monthly = std::move(figures);
        effects.push_back(std::move(event));
    }
    // Original fcn_0042ba97 (rich4_ui_stock.asm:3052), every 15th: each listed company pays its
    // whole balance (a negative balance is a loss) into the active shareholders' bank deposits, in
    // proportion to their shares; the balance is cleared when anybody holds shares. A deposit
    // driven below zero is taken from cash, and a player whose cash also runs out goes bankrupt.
    void pay_dividends() {
        auto figures = monthly_figures();
        std::array<std::int32_t, 4> totals{};
        for (std::size_t stock = 0; stock < world.stocks.size() && stock < 12; ++stock) {
            const auto id = world.stocks[stock].flags_4;
            if (!id || id >= world.commercials.size())
                continue;
            auto &company = world.commercials[id];
            std::int32_t shares = 0;
            for (std::size_t slot = 0; slot < world.players.size(); ++slot)
                if (world.players[slot].state.controller_flags)
                    shares = add32(shares, world.players[slot].stocks[stock].amount);
            DividendRow row{static_cast<std::uint8_t>(stock), company.funds, {}};
            for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
                const auto held = world.players[slot].stocks[stock].amount;
                if (!world.players[slot].state.controller_flags || !held)
                    continue;
                // FDIVP stored as binary32, then FILD funds × that ratio, truncated (FISTP).
                const auto ratio = (numeric::Extended::from(held) / numeric::Extended::from(shares)).single();
                row.payout[slot] =
                    (numeric::Extended::from(company.funds) * numeric::Extended::from(ratio)).integer();
                totals[slot] = add32(totals[slot], row.payout[slot]);
            }
            if (shares)
                company.funds = 0;
            figures.dividends.push_back(row);
        }
        if (figures.dividends.empty())
            return;
        monthly_effect(EffectKind::dividend, figures); // the table shows before the payout settles
        for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
            auto &state = world.players[slot].state;
            if (!state.controller_flags)
                continue;
            state.bank_balance = add32(state.bank_balance, totals[slot]);
            if (state.bank_balance >= 0)
                continue;
            state.cash = add32(state.cash, state.bank_balance);
            state.bank_balance = 0;
            if (state.cash < 0) {
                state.cash = 0;
                bankrupt(slot);
            }
        }
    }
    void close_month() {
        // MONTHLY: the report's figures (fcn_00439bfa / fcn_00437e61 state 2), captured around the
        // interest without changing it.
        auto report = monthly_figures();
        for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
            const auto &entry = world.players[slot];
            report.deposit[slot] = entry.state.bank_balance;
            report.interest[slot] =
                (numeric::Extended::from(entry.state.bank_balance) * numeric::Extended::from(0.1)).integer();
            if (entry.state.loan)
                report.loans |= static_cast<std::uint8_t>(1U << slot);
            report.unlucky_days[slot] = entry.status.total_winter_sleep_days;
            report.spending[slot] = entry.status.cumulative_spending;
            report.income[slot] = entry.status.cumulative_income;
        }
        for (auto &entry : world.players) {
            auto &state = entry.state;
            if (state.controller_flags && state.loan == 0) {
                // Preserve the original x87 product and FISTP truncation,
                // including its INT32_MIN sentinel for out-of-range amounts.
                state.bank_balance =
                    (numeric::Extended::from(state.bank_balance) * numeric::Extended::from(1.1)).integer();
            }
        }
        month_awards(report);
        for (auto &entry : world.players) {
            entry.status.total_winter_sleep_days = 0;
            entry.status.cumulative_spending = entry.status.cumulative_income = 0;
        }
        emit(GameEventKind::bank_transfer, "月结完成，无贷款玩家获得存款利息");
        monthly_effect(EffectKind::month_report, std::move(report));
    }
    // MONTHLY (presentation only): fcn_00437dfe picks the champion, the strictly richest listed
    // player (ties keep the first); fcn_00437d1a picks the 悲情人物 by the score
    // (+92 − +96) + 倒楣天數 × price index × 2500 + (word +68) × 10: the top score M wins only when
    // M and the runner-up S (scores equal to M excluded) are non-zero and (M − S) / M > 0.4.
    void month_awards(MonthlyFigures &report) const {
        std::int32_t best_wealth = 0;
        std::array<std::int32_t, 4> score{};
        report.champion = -1;
        for (std::size_t slot = 0; slot < world.players.size() && slot < 4; ++slot) {
            if (!((report.active >> slot) & 1U))
                continue;
            const auto &entry = world.players[slot];
            report.cash[slot] = entry.state.cash;
            report.balance[slot] = entry.state.bank_balance;
            report.total[slot] = wealth(slot);
            if (report.champion < 0)
                report.champion = static_cast<std::int32_t>(slot);
            if (report.total[slot] > best_wealth) {
                best_wealth = report.total[slot];
                report.champion = static_cast<std::int32_t>(slot);
            }
            score[slot] = add32(
                add32(subtract32(std::bit_cast<std::int32_t>(entry.status.cumulative_spending),
                                 std::bit_cast<std::int32_t>(entry.status.cumulative_income)),
                      multiply32(multiply32(entry.status.total_winter_sleep_days, world.header.price_index), 2500)),
                multiply32(entry.status.god_luck_modifier, 10));
        }
        std::int32_t top = 0, runner = 0;
        std::int32_t top_slot = -1;
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (((report.active >> slot) & 1U) && score[slot] > top) {
                top = score[slot];
                top_slot = static_cast<std::int32_t>(slot);
            }
        for (std::size_t slot = 0; slot < 4; ++slot)
            if (((report.active >> slot) & 1U) && score[slot] != top && score[slot] > runner)
                runner = score[slot];
        report.unlucky = top && runner && double(top - runner) / double(top) > 0.4 ? top_slot : -1;
    }
    std::uint32_t lease_expiry() const {
        static constexpr std::array<std::uint32_t, 6> intervals{0, 0x20000, 0x10000, 0x600, 0x300, 0x100};
        if (world.lease_mode >= intervals.size())
            throw std::runtime_error("invalid lease mode");
        const auto interval = intervals[world.lease_mode];
        return interval ? calendar::add_packed_interval(
                              calendar::pack(
                                  {world.header.date.year, world.header.date.month, world.header.date.day}),
                              interval)
                        : 0;
    }
    void expire_properties() {
        const auto today =
            calendar::pack({world.header.date.year, world.header.date.month, world.header.date.day});
        const auto age = [](std::uint8_t &state) {
            if (state & 0xf0U) {
                state = static_cast<std::uint8_t>(state - 0x10);
                if (!(state & 0xf0U))
                    state = 0;
            }
        };
        for (auto &land : world.lands) {
            age(land.temporary_state);
            if (land.expired_date == today) {
                land.economy.owner_code = 0;
                land.expired_date = 0;
            }
        }
        for (auto &facility : world.facilities) {
            age(facility.temporary_state);
            if (facility.expired_date == today) {
                facility.owner_code = 0;
                facility.expired_date = 0;
            }
        }
    }
    void apply_ai_settings(std::size_t slot, const AiSettings &settings) {
        auto &entry = world.players.at(slot);
        if (!(entry.state.controller_flags & 1U))
            return; // went bankrupt meanwhile
        entry.state.controller_flags = static_cast<std::uint8_t>((entry.state.controller_flags & ~4U) |
                                                                 (settings.autopilot ? 4U : 0U));
        entry.status.card_strategy = settings.card_strategy;
        entry.status.rescue_strategy = settings.personality;
        entry.status.init_cash_ratio = settings.cash_ratio;
        entry.status.stock_investment_ratio = settings.stock_ratio;
    }
    void next_turn() {
        for (std::size_t slot = 0; slot < pending_ai_settings.size(); ++slot) // 託管 turn boundary
            if (auto &pending = pending_ai_settings[slot]) {
                apply_ai_settings(slot, *pending);
                pending.reset();
            }
        const auto previous = actor();
        std::size_t next = previous;
        bool wrapped = false;
        for (std::size_t count = 0; count < world.players.size(); ++count) {
            ++next;
            if (next == world.players.size()) {
                next = 0;
                wrapped = true;
            }
            if (world.players[next].state.controller_flags != 0)
                break;
        }
        if (world.players[next].state.controller_flags == 0) {
            emit(GameEventKind::game_finished, "游戏结束");
            set_phase(GamePhase::finished);
            return;
        }
        if (wrapped) {
            run_rule(
                [this] {
                    special_actors::Callbacks callbacks;
                    callbacks.pay = [this](auto from, auto to, auto amount, auto flags) {
                        (void)pay(from, to, amount, flags);
                    };
                    callbacks.remove_object = [this](auto id) { remove_object(id); };
                    callbacks.prison = [this](auto slot, auto amount) {
                        add_confinement(slot, amount, false);
                    };
                    callbacks.hospital = [this](auto slot, auto amount) {
                        add_confinement(slot, amount, true);
                    };
                    callbacks.voice = [this](auto slot, auto category) { voice(slot, category); };
                    callbacks.game_over = [this] { return phase == GamePhase::finished; };
                    for (std::size_t slot = 4; slot < 8 && phase != GamePhase::finished; ++slot) {
                        const auto result = special_actors::turn(world, random, slot, callbacks);
                        if (!result.supported)
                            throw UnsupportedRule{result.message};
                        for (const auto &effect : result.effects)
                            events.push_back({GameEventKind::notice, slot, effect.amount, effect.node,
                                              "特殊角色完成行动"});
                    }
                },
                true, false,
                [this, next] {
                    if (phase == GamePhase::finished)
                        return;
                    world.header.current_player = static_cast<std::uint32_t>(next);
                    advance_day();
                    start_turn();
                });
        } else {
            world.header.current_player = static_cast<std::uint32_t>(next);
            start_turn();
        }
    }
    void advance_day() {
        const auto advance =
            calendar::next_day({world.header.date.year, world.header.date.month, world.header.date.day});
        const auto date = advance.date;
        world.header.date = {date.day, date.month, date.year};
        ++days;
        ++world.round_counter;
        update_price_index();
        if (world.news_effect_days & 0x80)
            world.news_effect_days = 0;
        else if (world.news_effect_days && --world.news_effect_days == 0)
            world.news_effect_days = 0x80;
        for (auto &stock : world.stocks) {
            if (stock.suspension_days)
                --stock.suspension_days;
            if (stock.news_movement & 0xf0U)
                stock.news_movement = static_cast<std::uint8_t>(stock.news_movement - 0x10);
            if (stock.news_movement & 0x0fU)
                --stock.news_movement;
        }
        (void)stocks::daily_update(world, random,
                                   holidays::is_closed({date.year, date.month, date.day}, world.header.map_id,
                                                       world.header.stage_id));
        if (date.day == 15) // original core_actions:4836: dividends (fcn_0042ba97) before the lottery draw
            pay_dividends();
        // fcn_00452444 (presentation only): New Year / national day fireworks, Christmas Santa.
        if (const auto party = holidays::celebration(date, world.header.map_id, world.header.stage_id);
            party && party->movie)
            effect(party->movie == 554 ? EffectKind::christmas : EffectKind::holiday_fireworks, actor(),
                   actor_point(actor()));
        if (lottery::draw_due(world.header.date)) {
            auto show = monthly_figures(); // MONTHLY: the draw show needs the tickets before the draw
            show.tickets = world.lottery_tickets;
            show.pool = world.public_fund;
            const auto draw = lottery::draw(world, random);
            if (draw.held) {
                emit(GameEventKind::notice, "彩票开奖：" + std::to_string(draw.number), draw.payout);
                show.number = draw.number;
                show.winner = draw.winner ? static_cast<std::int32_t>(*draw.winner) : -1;
                monthly_effect(EffectKind::lottery_draw, std::move(show));
            }
        }
        if (advance.month_changed) {
            ++world.month_counter;
            close_month();
            for (const auto id : {13U, 14U}) {
                const auto old_node = world.objects[id - 1].node_id;
                remove_object(static_cast<std::uint16_t>(id));
                place_existing_object(id - 1, random_free_node(old_node));
            }
        }
        expire_properties();
        emit(GameEventKind::date_changed, std::to_string(date.year) + "年" + std::to_string(date.month) +
                                              "月" + std::to_string(date.day) + "日");
    }
};

GameSession::GameSession(World world, std::uint32_t seed)
    : impl_(std::make_unique<Impl>(std::move(world), seed)) {}
GameSession::~GameSession() = default;
GameSession::GameSession(GameSession &&) noexcept = default;
GameSession &GameSession::operator=(GameSession &&) noexcept = default;
const World &GameSession::world() const noexcept {
    return impl_->world;
}
GamePhase GameSession::phase() const noexcept {
    return impl_->phase;
}
const std::optional<GameDecision> &GameSession::pending_decision() const noexcept {
    return impl_->decision;
}
const std::optional<MoveAnimation> &GameSession::animation() const noexcept {
    return impl_->animation;
}
const std::vector<std::uint8_t> &GameSession::dice() const noexcept {
    return impl_->faces;
}
std::uint32_t GameSession::random_state() const noexcept {
    return impl_->random.state();
}
int GameSession::steps_remaining() const noexcept {
    return impl_->remaining;
}
std::uint64_t GameSession::completed_days() const noexcept {
    return impl_->days;
}

void GameSession::set_camera_rotation(std::uint8_t rotation) {
    if (rotation > 7)
        throw std::invalid_argument("camera rotation must be 0..7");
    impl_->world.camera_rotation = rotation;
}
std::optional<std::size_t> GameSession::winning_player() const noexcept {
    return impl_->winner;
}
bool GameSession::set_dice_count(unsigned count) {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    const auto traffic = impl_->player().traffic_method & 3U;
    const auto maximum = traffic == 1 ? 2U : traffic == 2 ? 3U : 1U;
    if (!count || count > maximum)
        return false;
    impl_->player().dice_count = static_cast<std::uint8_t>(count);
    return true;
}
bool GameSession::roll_dice() {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    impl_->begin_roll();
    return true;
}
std::vector<CardTarget> GameSession::card_targets(std::uint8_t card) const {
    return legal_card_targets(impl_->world, impl_->actor(), card, impl_->card_callbacks());
}
bool GameSession::use_card(const CardUse &request) {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    impl_->last_action_accepted = true;
    impl_->run_rule(
        [this, request] {
            impl_->begin_card(request);
            const auto result =
                apply_card(impl_->world, impl_->random, impl_->actor(), request, impl_->card_callbacks());
            impl_->last_action_accepted = result.used();
            if (result.status == CardUseStatus::missing_callback)
                throw UnsupportedRule{result.message};
            if (result.used()) {
                impl_->card_picture();
                impl_->emit(GameEventKind::notice,
                            "使用" + std::string(services::card_names.at(request.card_id - 1)));
                impl_->events.back().card = request.card_id;
            }
        },
        true, false);
    return impl_->last_action_accepted || impl_->phase == GamePhase::awaiting_decision;
}
bool GameSession::use_tool(unsigned id, std::uint16_t target, std::uint16_t destination) {
    if (impl_->phase != GamePhase::human_ready || id < 1 || id > 13 || !impl_->player().tool_amounts[id - 1])
        return false;
    impl_->last_action_accepted = true;
    impl_->run_rule(
        [this, id, target, destination] {
            auto chosen = target, finish = destination;
            if ((tools::selection_flags(id) != 0 || id == 8) && !chosen)
                chosen = impl_->choose_tool_target(id, 0);
            if (!chosen && (tools::selection_flags(id) != 0 || id == 8)) {
                impl_->last_action_accepted = false;
                return;
            }
            if (id == 11 && !finish)
                finish = impl_->choose_tool_target(id, chosen);
            if (id == 11 && !finish) {
                impl_->last_action_accepted = false;
                return;
            }
            if (tools::selection_flags(id) != 0) {
                const auto options = impl_->tool_targets(id);
                if (std::find(options.begin(), options.end(), chosen) == options.end()) {
                    impl_->last_action_accepted = false;
                    return;
                }
            }
            if (id == 11) {
                const auto options = impl_->tool_targets(id, chosen);
                if (std::find(options.begin(), options.end(), finish) == options.end()) {
                    impl_->last_action_accepted = false;
                    return;
                }
            }
            const auto result = tools::use(impl_->world, impl_->random, impl_->actor(), id, chosen, finish,
                                           impl_->tool_callbacks());
            impl_->last_action_accepted = result.applied;
            if (!result.supported)
                throw UnsupportedRule{result.message};
            if (result.applied) {
                impl_->tool_effect(id, chosen);
                impl_->emit(GameEventKind::notice, "使用" + std::string(services::tool_names.at(id - 1)));
            }
        },
        true, false);
    return impl_->last_action_accepted || impl_->phase == GamePhase::awaiting_decision;
}
bool GameSession::dismount() {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    return tools::dismount(impl_->world, impl_->actor());
}
bool GameSession::buy_stock(std::size_t stock, std::int32_t quantity) {
    if (impl_->phase != GamePhase::human_ready || stock >= impl_->world.stocks.size() || quantity <= 0 ||
        impl_->card_callbacks().market_closed())
        return false;
    const auto &quote = impl_->world.stocks[stock];
    if (quote.suspension_days || stocks::trend(impl_->world, stock) == stocks::PriceTrend::limit_up ||
        quantity > quote.flags_10)
        return false;
    try {
        const auto result = stocks::buy_stock(impl_->world, impl_->actor(), stock, quantity, true);
        impl_->emit(GameEventKind::bank_transfer, "买入股票", result.cost);
        return true;
    } catch (const std::invalid_argument &) {
        return false;
    } catch (const std::out_of_range &) {
        return false;
    }
}
bool GameSession::sell_stock(std::size_t stock, std::int32_t quantity) {
    if (impl_->phase != GamePhase::human_ready || stock >= impl_->world.stocks.size() || quantity <= 0 ||
        impl_->card_callbacks().market_closed())
        return false;
    const auto &quote = impl_->world.stocks[stock];
    if (quote.suspension_days || stocks::trend(impl_->world, stock) == stocks::PriceTrend::limit_down)
        return false;
    try {
        const auto result = stocks::sell_stock(impl_->world, impl_->actor(), stock, quantity, true);
        impl_->emit(GameEventKind::bank_transfer, "卖出股票", result.proceeds);
        return true;
    } catch (const std::invalid_argument &) {
        return false;
    } catch (const std::out_of_range &) {
        return false;
    }
}
bool GameSession::close_stock_market() {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    impl_->run_rule([this] { impl_->reconcile_financing(false); }, true, false);
    return true;
}
bool GameSession::open_trading() {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    (void)trading::prune(impl_->world);
    return true;
}
bool GameSession::post_offer(const trading::Listing &offer) {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    const auto result = trading::post(impl_->world, impl_->actor(), offer);
    if (result.applied)
        impl_->emit(GameEventKind::notice, "出售报价已发布", offer.price);
    return result.applied;
}
bool GameSession::cancel_offer(std::size_t index) {
    return impl_->phase == GamePhase::human_ready && trading::cancel(impl_->world, impl_->actor(), index);
}
bool GameSession::buy_offer(std::size_t seller, std::size_t index) {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    const auto result = trading::buy(impl_->world, impl_->actor(), seller, index);
    if (result.applied)
        impl_->emit(GameEventKind::notice, "购买出售物品完成");
    return result.applied;
}
bool GameSession::close_trading() {
    if (impl_->phase != GamePhase::human_ready)
        return false;
    impl_->run_rule([this] { impl_->reconcile_financing(false); }, true, false);
    return true;
}
bool GameSession::respond(std::size_t option, std::int32_t amount) {
    if (impl_->phase != GamePhase::awaiting_decision || !impl_->decision ||
        option >= impl_->decision->options.size())
        return false;
    const auto decision = *impl_->decision;
    auto value = decision.options[option].value;
    if (decision.kind == DecisionKind::money_spinner) {
        if (!impl_->money_spinner)
            return false;
        impl_->money_spinner->stop();
        return true;
    }
    if (impl_->pending_continuation) {
        if (decision.kind == DecisionKind::roulette) {
            value = Impl::roulette_value(impl_->roulette_kind, impl_->roulette_cursor + 12);
            impl_->stopped_roulette_effect(decision, value); // shown before the re-run's output
        }
        if (decision.kind == DecisionKind::primary_stock && value) {
            if (amount <= 0 || amount > decision.max_amount)
                return false;
            value = amount;
        }
        impl_->event_choices[impl_->pending_choice_key] = value;
        impl_->decision.reset();
        auto continuation = std::move(impl_->pending_continuation);
        if (!continuation)
            throw std::logic_error("event choice lost its continuation");
        continuation();
        return true;
    }
    if (decision.kind == DecisionKind::lottery) {
        if (value && lottery::buy_ticket(impl_->world, impl_->actor(), static_cast<unsigned>(value)) !=
                         lottery::Purchase::purchased)
            return false;
        if (value)
            impl_->emit(GameEventKind::notice, "购买彩票" + std::to_string(value), lottery::ticket_price);
        impl_->finish_turn();
        return true;
    }
    if (decision.kind == DecisionKind::shop)
        return impl_->shop_action(value, amount > 0 ? static_cast<unsigned>(amount) : 1U);
    if (decision.kind == DecisionKind::magic_category) {
        impl_->magic_effect_prompt(static_cast<unsigned>(value));
        return true;
    }
    if (decision.kind == DecisionKind::magic_effect) {
        if (value < 0)
            impl_->open_magic();
        else
            impl_->magic_effect(static_cast<unsigned>(value));
        return true;
    }
    if (decision.kind == DecisionKind::bank) {
        auto &player = impl_->player();
        if (value == 0) {
            impl_->leave_bank();
            return true;
        }
        if (amount <= 0)
            return false;
        if (value == 3) {
            const bool result = impl_->borrow(amount);
            if (result)
                impl_->open_bank(impl_->resume_movement);
            return result;
        }
        if (value == 4) {
            const bool result = impl_->repay(amount, true);
            if (result)
                impl_->open_bank(impl_->resume_movement);
            return result;
        }
        if (value == 5 || value == 6) {
            const bool result = impl_->change_financing(amount, value == 5);
            if (result)
                impl_->open_bank(impl_->resume_movement);
            return result;
        }
        if (value == 1 && amount <= player.cash) {
            player.cash = subtract32(player.cash, amount);
            player.bank_balance = add32(player.bank_balance, amount);
        } else if (value == 2 && amount <= player.bank_balance) {
            player.bank_balance = subtract32(player.bank_balance, amount);
            player.cash = add32(player.cash, amount);
        } else
            return false;
        impl_->emit(GameEventKind::bank_transfer, value == 1 ? "存款完成" : "提款完成", amount);
        if (value == 2)
            impl_->run_rule([this] { impl_->reconcile_financing(true); }, true, false,
                            [this] {
                                if (impl_->phase != GamePhase::finished)
                                    impl_->open_bank(impl_->resume_movement);
                            });
        return true;
    }
    impl_->decision.reset();
    if (decision.kind == DecisionKind::rescue) {
        if (value >= 0) {
            const auto needed = value < 4 ? 30 : 300;
            if (impl_->player().points < needed) {
                impl_->decision = decision;
                return false;
            }
            impl_->rescue(static_cast<std::size_t>(value), impl_->rescue_hospital, true);
        }
        impl_->finish_turn();
    } else if (decision.kind == DecisionKind::roulette) {
        const auto kind = impl_->world.facilities.at(decision.land_id).facility_type;
        const auto multiplier = Impl::roulette_value(kind, impl_->roulette_cursor + 12);
        impl_->stopped_roulette_effect(decision, multiplier);
        impl_->charge_facility(decision.land_id, multiplier);
    } else if (decision.kind == DecisionKind::research_tool) {
        if (value >= 1 && value <= 5 && decision.land_id > 0 && decision.land_id < impl_->world.facilities.size()) {
            auto &entry = impl_->world.facilities[decision.land_id];
            const auto max_slot = std::min<int>(entry.level, 5);
            if (value > max_slot) {
                impl_->decision = decision;
                return false;
            }
            entry.research_tool = static_cast<std::uint8_t>(value);
            entry.research_days() = 5;
            impl_->emit(GameEventKind::notice,
                        "研究所开始研发" + std::string(services::tool_names.at(static_cast<std::size_t>(value + 7))),
                        value + 8);
        }
        impl_->finish_turn();
    } else if (decision.kind == DecisionKind::buy_facility ||
               decision.kind == DecisionKind::construct_facility ||
               decision.kind == DecisionKind::upgrade_facility) {
        // Buying/upgrading: 0 declines. Building has no decline in the original (fcn_00440aac without
        // the cancellable flag); value 0 is a 公園 there.
        if (value == 0 && decision.kind != DecisionKind::construct_facility)
            impl_->finish_turn();
        else
            impl_->buy_facility(decision.land_id,
                                decision.kind == DecisionKind::buy_facility         ? 0
                                : decision.kind == DecisionKind::construct_facility ? 1
                                                                                    : 2,
                                static_cast<unsigned>(value));
    } else if (decision.kind == DecisionKind::buy_land || decision.kind == DecisionKind::upgrade_land) {
        if (value == 1)
            impl_->buy(decision.land_id, decision.kind == DecisionKind::upgrade_land);
        else
            impl_->finish_turn();
    } else if (decision.kind == DecisionKind::notice && impl_->unsupported_resume == GamePhase::human_ready)
        impl_->set_phase(GamePhase::human_ready);
    else
        impl_->finish_turn();
    return true;
}
void GameSession::set_interactive_mini_games(bool enabled) noexcept {
    impl_->interactive_mini_games = enabled;
}
bool GameSession::interactive_mini_games() const noexcept {
    return impl_->interactive_mini_games;
}
bool GameSession::submit_mini_game(std::int32_t score, std::uint32_t random_draws) {
    if (impl_->phase != GamePhase::awaiting_decision || !impl_->decision ||
        impl_->decision->kind != DecisionKind::mini_game || score < 0 || random_draws > 10'000'000U)
        return false;
    impl_->decision.reset();
    for (std::uint32_t i = 0; i < random_draws; ++i)
        (void)impl_->random.next(); // the game's rand() calls in the original's shared stream
    auto &player = impl_->player();
    // core_actions:2371 `add word [player+48], ax`: the original adds the low 16 bits.
    player.points = static_cast<std::uint16_t>(player.points + static_cast<std::uint32_t>(score));
    impl_->emit(GameEventKind::points_received, "小游戏获得" + std::to_string(score) + "点", score);
    impl_->finish_turn();
    return true;
}
void GameSession::tick(unsigned elapsed_ms) {
    if (impl_->phase == GamePhase::awaiting_decision && impl_->decision &&
        impl_->decision->kind == DecisionKind::money_spinner) {
        impl_->spinner_elapsed += elapsed_ms;
        while (impl_->spinner_elapsed >= 30 && !impl_->money_spinner->finished()) {
            impl_->spinner_elapsed -= 30;
            impl_->money_spinner->tick(impl_->spinner_random);
        }
        impl_->decision->amount = impl_->money_spinner->amount();
        if (impl_->decision->spinner) { // presentation mirror of the reels (docs/ui/messages.md §2.3)
            impl_->decision->spinner->digits = impl_->money_spinner->digits();
            impl_->decision->spinner->phase = static_cast<std::uint8_t>(impl_->money_spinner->phase());
        }
        if (impl_->money_spinner->finished()) {
            impl_->event_choices[impl_->pending_choice_key] = impl_->money_spinner->amount();
            impl_->choice_random_states[impl_->pending_choice_key] = impl_->spinner_random.state();
            impl_->money_spinner.reset();
            impl_->decision.reset();
            auto continuation = std::move(impl_->pending_continuation);
            continuation();
        }
        return;
    }
    if (impl_->phase == GamePhase::awaiting_decision && impl_->decision &&
        impl_->decision->kind == DecisionKind::roulette) {
        // fcn_0043f7c6 advances the wheel one sector per 36 ms (0x24) frame until STOP. Timing only:
        // the spin draws no randomness.
        impl_->roulette_elapsed += elapsed_ms;
        impl_->roulette_cursor = (impl_->roulette_cursor + impl_->roulette_elapsed / 36) % 12;
        impl_->roulette_elapsed %= 36;
        impl_->decision->amount = Impl::roulette_value(impl_->roulette_kind, impl_->roulette_cursor);
        if (impl_->decision->spinner)
            impl_->decision->spinner->cursor = static_cast<std::uint8_t>(impl_->roulette_cursor);
        return;
    }
    // Consume only as much elapsed time as the current animation needs. The
    // bound prevents malformed worlds from spinning forever in one UI frame.
    for (unsigned transitions = 0; transitions < 4096; ++transitions) {
        auto &game = *impl_;
        if (game.phase == GamePhase::human_ready || game.phase == GamePhase::awaiting_decision ||
            game.phase == GamePhase::finished)
            return;
        if (game.phase == GamePhase::advancing_turn) {
            game.next_turn();
            continue;
        }
        const unsigned duration = game.phase == GamePhase::rolling ? roll_duration : step_duration;
        const unsigned needed = duration - game.phase_elapsed;
        const unsigned consumed = std::min(elapsed_ms, needed);
        game.phase_elapsed += consumed;
        elapsed_ms -= consumed;
        if (game.animation)
            game.animation->elapsed_ms = game.phase_elapsed;
        if (game.phase_elapsed < duration)
            return;
        if (game.phase == GamePhase::rolling) {
            if (game.remaining > 0)
                game.start_step();
            else
                game.land();
        } else
            game.complete_step();
        if (!elapsed_ms)
            return;
    }
    throw std::runtime_error("native game exceeded one tick's transition limit");
}
std::uint8_t GameSession::player_costume(std::size_t slot) const noexcept {
    if (slot >= impl_->costumes.size() || slot >= impl_->world.players.size())
        return 0;
    return impl_->costume_keys[slot] == impl_->costume_key(slot) ? impl_->costumes[slot] : 0;
}
std::vector<GameEvent> GameSession::drain_events() {
    impl_->refresh_costumes();
    auto output = std::move(impl_->events);
    impl_->events.clear();
    return output;
}
std::vector<EffectEvent> GameSession::drain_effects() {
    auto output = std::move(impl_->effects);
    impl_->effects.clear();
    return output;
}
std::optional<AiSettings> GameSession::ai_settings(std::size_t slot) const {
    if (slot >= impl_->world.players.size() || !(impl_->world.players[slot].state.controller_flags & 1U))
        return std::nullopt;
    if (slot < impl_->pending_ai_settings.size() && impl_->pending_ai_settings[slot])
        return impl_->pending_ai_settings[slot];
    const auto &entry = impl_->world.players[slot];
    return AiSettings{(entry.state.controller_flags & 4U) != 0, entry.status.card_strategy,
                      entry.status.rescue_strategy, entry.status.init_cash_ratio,
                      entry.status.stock_investment_ratio};
}
bool GameSession::set_ai_settings(std::size_t slot, const AiSettings &settings) {
    auto &game = *impl_;
    if (slot >= game.world.players.size() || slot >= game.pending_ai_settings.size() ||
        !(game.world.players[slot].state.controller_flags & 1U) || settings.card_strategy > 3 ||
        settings.personality > 2 || settings.cash_ratio > 100 || settings.stock_ratio > 100)
        return false;
    if (game.phase != GamePhase::human_ready && game.phase != GamePhase::finished) {
        game.pending_ai_settings[slot] = settings; // mid-turn: wait for the turn boundary
        return true;
    }
    game.pending_ai_settings[slot].reset();
    game.apply_ai_settings(slot, settings);
    // The waiting human was handed to the computer: play this turn as a computer turn now
    // (enter_turn's computer branch).
    if (game.phase == GamePhase::human_ready && slot == game.actor() && !game.human()) {
        if (game.player().controller_flags & 0x30U)
            game.begin_roll();
        else
            game.begin_ai_turn();
    }
    return true;
}
bool GameSession::can_save() const noexcept {
    return impl_->phase == GamePhase::human_ready || impl_->phase == GamePhase::finished;
}
SaveFile GameSession::export_save() const {
    if (!can_save())
        throw std::logic_error("save requires a settled native game turn");
    return SaveFile::decode(impl_->world.export_save());
}

} // namespace rich4
