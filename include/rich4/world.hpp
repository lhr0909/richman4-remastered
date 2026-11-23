// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rich4/economy.hpp"
#include "rich4/inventory.hpp"
#include "rich4/save.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rich4 {

// Runtime values, never overlays of the original packed 32-bit structures.
struct PlayerStatus {
    std::uint8_t card_strategy = 0;   // Original profile byte +22; bit 1 permits active-card AI.
    std::uint8_t rescue_strategy = 0; // Original profile byte +23.
    std::uint8_t loan_ratio = 0;      // Original profile byte +24.
    std::uint8_t init_cash_ratio = 50;
    std::uint8_t stock_investment_ratio = 0; // Original profile byte +26.
    std::uint8_t saved_direction = 0;        // +27, saved facing while absent/in a facility.
    std::uint32_t loan_due_date = 0;
    std::uint8_t days_in_hotel = 0;
    std::uint8_t days_disappearing = 0;
    std::uint8_t days_in_prison = 0;
    std::uint8_t days_in_hospital = 0;
    std::uint8_t days_sleeping = 0;
    std::uint8_t days_sleep_walking = 0;
    std::uint8_t days_stopping = 0;
    std::uint8_t days_tortoise_walking = 0;
    std::uint8_t days_rejected_by_bank = 0;
    std::uint8_t days_bank_no_loans = 0;
    std::uint8_t allied_days = 0;
    std::uint8_t days_assurance = 0;
    std::uint8_t god_info = 0;
    std::uint8_t attached_bomb = 0;
    std::uint8_t allied_player = 0; // One-based owner code, zero means none.
    std::uint8_t total_winter_sleep_days = 0;
    std::int16_t god_luck_modifier = 0;      // Original signed word +68.
    std::int16_t money_fortune_modifier = 0; // Original signed word +70.
    std::int16_t other_fortune_modifier = 0; // Original signed word +72.
    std::uint16_t facility_id = 0;           // Original word +74.
    std::array<std::uint32_t, 4> hostility{};
    std::uint32_t cumulative_spending = 0;
    std::uint32_t cumulative_income = 0;
    std::uint8_t flags_100 = 0;
    std::uint8_t flags_101 = 0;
    std::uint8_t flags_102 = 0;
    std::uint8_t flags_103 = 0;
};

struct StockHolding {
    std::int32_t amount = 0;
    std::uint32_t raw_cost_basis = 0; // Original second word, preserved as bits.
};

enum class OfferKind : std::uint8_t { empty = 0, stock = 1, property = 2, tool = 3, card = 4 };

// Original player-to-player sale table: VA 0x4967e0, four lists of seven
// twelve-byte records. Kept as values rather than an ABI-packed overlay.
struct TradeOffer {
    OfferKind kind = OfferKind::empty;
    std::uint8_t flags = 0;
    std::uint16_t item = 0; // Stock zero-based; card/tool one-based; property 2001+/4001+.
    std::int32_t price = 0;
    std::uint16_t quantity = 0; // Stock quantity; unused for other kinds.
    std::uint8_t property_type = 0, property_level = 0;
    friend bool operator==(const TradeOffer &, const TradeOffer &) = default;
};

struct StockState {
    std::uint16_t flags_4 = 0;
    std::uint8_t suspension_days = 0;
    std::uint8_t news_movement = 0;
    std::uint16_t shares_available = 0;
    std::uint16_t flags_10 = 0;
    // Exact original binary32 encodings at offsets 12,16,20,24,28. Keeping
    // bits avoids canonicalizing NaNs during an unchanged round trip.
    std::array<std::uint32_t, 5> float_bits{};
    std::uint32_t raw_32 = 0;
};

struct WorldPlayer {
    PlayerState state{};
    PlayerStatus status{};
    std::array<StockHolding, 12> stocks{};
};

struct SpecialActor {
    std::uint16_t x = 0, y = 0, node_id = 0, last_node_id = 0;
    std::uint8_t owner = 0, direction = 0, inactive_state = 0, flags = 0;
    std::uint8_t days_sleeping = 0, days_sleep_walking = 0;
    std::uint8_t days_stopping = 0, days_tortoise_walking = 0;
};

// IDs in node flags and god_info are one-based; array subscripts are ID-1.
struct MapObject {
    std::uint8_t kind = 0, direction = 0;
    std::uint16_t node_id = 0;
    std::uint8_t remaining_days = 0, attached_player = 0;
    std::uint8_t animation_remaining = 0, animation_step = 0;
    std::array<std::uint32_t, 4> motion_bits{}; // Original binary32 position/delta, never canonicalized.
};

struct MapNode {
    std::uint32_t id = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::array<std::uint16_t, 4> neighbors{}; // Zero is an unused link.
    std::uint16_t type_and_index = 0;         // Original event/property code, not an enum guess.
    std::uint16_t ground_sprite = 0;
    std::uint32_t flags = 0;
    std::string name{}; // Original Big5 bytes at +4 (map-tile popup only; read-only, never saved).
};

struct Land {
    std::uint32_t id = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    LandEconomyRecord economy;
    std::uint8_t temporary_state = 0;
    std::uint8_t direction = 0;
    std::uint16_t price_per_level = 0;
    std::uint16_t land_price = 0;
    std::uint32_t removed_building = 0;
    std::uint32_t expired_date = 0;
};

struct Facility {
    std::uint32_t id = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::string name; // Original Big5 bytes, excluding NUL padding.
    std::uint8_t facility_type = 0;
    std::uint8_t owner_code = 0;
    std::uint8_t level = 0;
    std::uint8_t direction = 0;
    std::uint8_t temporary_state = 0;
    std::uint8_t research_tool = 0; // Original facility byte +0x1d: 1..5 (tools 9..13).
    std::uint8_t flags_30 = 0;      // Original facility byte +0x1e: research turns left (0..5), cleared by sealing card.
    [[nodiscard]] std::uint8_t research_days() const noexcept {
        return flags_30;
    }
    [[nodiscard]] std::uint8_t &research_days() noexcept {
        return flags_30;
    }
    std::uint16_t land_price = 0;
    std::array<std::uint16_t, 6> toll_by_level{};
    [[nodiscard]] std::uint16_t price_per_level() const noexcept {
        return toll_by_level[0];
    }
    [[nodiscard]] std::uint16_t &price_per_level() noexcept {
        return toll_by_level[0];
    }
    std::uint32_t removed_building = 0;
    std::uint32_t expired_date = 0;
};

struct Commercial {
    std::uint32_t id = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t owner_code = 0;
    std::uint8_t commerce_type = 0;
    std::uint8_t direction = 0;
    std::uint16_t sprite_index = 0;
    std::uint16_t toll_fee = 0;
    std::uint8_t stock_index = 0;
    std::int32_t funds = 0;                          // +0x28, company balance.
    std::int32_t funds_change = 0;                   // +0x2c, original running cash-flow counter.
    std::int32_t shares_remaining = 0;               // +0x30.
    std::array<std::uint8_t, 4> shareholder_order{}; // +0x1c, stable original rank order.
    std::int32_t valuation = 0;                      // +0x24.
    std::string name{}; // Original Big5 bytes at +4 (map-tile popup only; read-only, never saved).
};

// Fifth map table. Only rendering placement fields are interpreted; all other
// bytes remain in the backing map and are not assigned invented game semantics.
struct Landscape {
    std::uint32_t id = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t direction = 0;
    std::uint16_t sprite_index = 0;
    std::string name{}; // Original Big5 bytes at +4 (map-tile popup only; read-only, never saved).
};

struct NewGamePlayer {
    std::uint8_t character = 10;
    std::uint8_t controller = 1; // 1 human, 2 computer.
    std::uint8_t traffic_method = 0;
    std::uint16_t start_node = 0; // Zero defers spawning to the player's first turn.
};

struct NewGameOptions {
    // The application supplies its current local date; the core reads no clock.
    SaveDate date{5, 10, 2026};
    std::uint16_t map_id = 0;
    std::uint16_t stage_id = 0;
    std::int32_t initial_fund = 200000;
    std::uint32_t lease_mode = 0; // Original 0..5 packed-calendar lease options.
    std::uint32_t random_seed = 1;
    std::vector<NewGamePlayer> players{{10, 1, 0, 0}, {3, 2, 0, 0}, {0, 2, 0, 0}, {6, 2, 0, 0}};
};

class World {
  public:
    static World from_save(const SaveFile &save);
    // Accepts the decoded odd MAP.MKF metadata entry, not its compressed bytes.
    static World from_map_resource(std::span<const std::uint8_t> map, const NewGameOptions &options = {});

    SaveHeader header{};
    std::vector<WorldPlayer> players;
    // Map vectors retain the original index-zero sentinel. IDs are array indices.
    std::vector<MapNode> nodes;
    std::vector<Land> lands;
    std::vector<Facility> facilities;
    std::vector<Commercial> commercials;
    std::vector<Landscape> landscapes;
    CardSupply card_supply{};
    ToolSupply tool_supply{};
    std::array<std::uint8_t, 36> news_deck{};
    std::array<std::uint8_t, 37> fortune_deck{};
    std::uint32_t news_cursor = 0;
    std::uint32_t fortune_cursor = 0;
    std::array<std::uint8_t, 8> prison_roster{};
    std::array<std::uint8_t, 8> hospital_roster{};
    std::array<SpecialActor, 5> special_actors{};
    std::array<MapObject, 46> objects{};
    std::array<std::uint8_t, 36> lottery_tickets{}; // Number 1..36: owner slot+1, zero unowned.
    std::array<StockState, 12> stocks{};
    std::array<std::array<TradeOffer, 7>, 4> offers{};
    std::array<std::array<std::uint32_t, 144>, 12> stock_history_bits{};
    std::int32_t stock_history_cursor = 0;
    std::int32_t stock_market_index = 0;          // Original 0x499078.
    std::uint32_t stock_market_movement_bits = 0; // Original binary32 at 0x4990ec.
    std::int32_t public_fund = 0;                 // Original recipient -1 payment accumulator.
    std::int32_t news_effect_days = 0;            // Original 0x4990dc, news event 26.
    std::uint32_t lease_mode = 0;                 // Original 0x499110, not inferred from expiry dates.
    std::uint32_t camera_rotation = 0;            // Original 0x499088.
    std::uint32_t round_counter = 0;              // Original 0x4990e4, company type 3 fee multiplier.
    std::uint32_t month_counter = 0;              // Original 0x499084, increments at each month boundary.

    [[nodiscard]] WorldPlayer &active_player();
    [[nodiscard]] const WorldPlayer &active_player() const;
    // Original bankruptcy clears record bytes 28..103, including unknown fields.
    // It does not clear separate card/tool inventories or the controller byte.
    void clear_bankrupt_player_fields(std::size_t slot);
    [[nodiscard]] const MapSummary &map_summary() const noexcept {
        return map_;
    }
    // The initial spawning stream's final state, for a fresh session to continue.
    // An old SAVE does not persist Watcom's PRNG state. Zero is a valid seed.
    [[nodiscard]] std::uint32_t initial_random_state() const noexcept {
        return random_state_;
    }
    [[nodiscard]] bool has_initial_random_state() const noexcept {
        return random_state_available_;
    }
    // Known fields are patched into the retained original bytes. Unknown fields,
    // historical snapshots and trailing bytes are preserved. Resizing is rejected.
    [[nodiscard]] std::vector<std::uint8_t> export_save() const;

  private:
    MapSummary map_{};
    std::vector<std::uint8_t> backing_;
    std::uint32_t random_state_ = 0;
    bool random_state_available_ = false;
};

} // namespace rich4
