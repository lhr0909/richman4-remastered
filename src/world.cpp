// SPDX-License-Identifier: GPL-3.0-or-later
// Portable translation of map/player records described in mytbk/rich4.
// See docs/migrations/010-world.md for source boundaries and retained raw state.
#include "rich4/world.hpp"
#include "rich4/direction.hpp"
#include "rich4/random.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace rich4 {
namespace {
constexpr std::size_t map_offset = 0x274b;
constexpr std::size_t snapshot_bytes = 0x2718;

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw SaveError("Truncated world field");
    return static_cast<std::uint16_t>(bytes[offset] | (std::uint16_t(bytes[offset + 1]) << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::uint32_t(u16(bytes, offset)) | (std::uint32_t(u16(bytes, offset + 2)) << 16);
}
void put16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}
void put32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16));
}
std::string name_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto field = bytes.subspan(offset, 16);
    const auto end = std::find(field.begin(), field.end(), 0);
    // Preserve even an unterminated original fixed-width name safely. No C string
    // read is permitted to run into the next map field.
    return {field.begin(), end};
}
void put_name(std::vector<std::uint8_t> &bytes, std::size_t offset, const std::string &value) {
    if (value == name_at(bytes, offset))
        return; // Preserve original padding exactly.
    if (value.size() > 15 || value.find('\0') != std::string::npos)
        throw SaveError("A changed map name must fit 15 non-NUL encoded bytes");
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 16, 0);
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}
std::size_t record_offset(const MapTableSummary &table, std::size_t id) {
    return map_offset + table.byte_offset + id * table.record_bytes;
}

struct StatusByte {
    std::uint8_t PlayerStatus::*member;
    std::size_t offset;
};
constexpr std::array status_bytes{
    StatusByte{&PlayerStatus::card_strategy, 22},
    StatusByte{&PlayerStatus::rescue_strategy, 23},
    StatusByte{&PlayerStatus::loan_ratio, 24},
    StatusByte{&PlayerStatus::init_cash_ratio, 25},
    StatusByte{&PlayerStatus::stock_investment_ratio, 26},
    StatusByte{&PlayerStatus::saved_direction, 27},
    StatusByte{&PlayerStatus::days_in_hotel, 50},
    StatusByte{&PlayerStatus::days_disappearing, 51},
    StatusByte{&PlayerStatus::days_in_prison, 52},
    StatusByte{&PlayerStatus::days_in_hospital, 53},
    StatusByte{&PlayerStatus::days_sleeping, 54},
    StatusByte{&PlayerStatus::days_sleep_walking, 55},
    StatusByte{&PlayerStatus::days_stopping, 56},
    StatusByte{&PlayerStatus::days_tortoise_walking, 57},
    StatusByte{&PlayerStatus::days_rejected_by_bank, 59},
    StatusByte{&PlayerStatus::days_bank_no_loans, 60},
    StatusByte{&PlayerStatus::allied_days, 61},
    StatusByte{&PlayerStatus::days_assurance, 62},
    StatusByte{&PlayerStatus::god_info, 63},
    StatusByte{&PlayerStatus::attached_bomb, 64},
    StatusByte{&PlayerStatus::allied_player, 65},
    StatusByte{&PlayerStatus::total_winter_sleep_days, 66},
    StatusByte{&PlayerStatus::flags_100, 100},
    StatusByte{&PlayerStatus::flags_101, 101},
    StatusByte{&PlayerStatus::flags_102, 102},
    StatusByte{&PlayerStatus::flags_103, 103},
};

template <std::size_t N> void shuffle_deck(std::array<std::uint8_t, N> &deck, Random &random) {
    std::array<bool, N> used{};
    for (std::size_t index = 0; index < N; ++index) {
        auto choice = static_cast<std::size_t>(random.next()) % (N - index);
        for (std::size_t candidate = 0; candidate < N; ++candidate) {
            if (used[candidate])
                continue;
            if (choice-- == 0) {
                used[candidate] = true;
                deck[index] = static_cast<std::uint8_t>(candidate);
                break;
            }
        }
    }
}

// Scalar character settings from upstream asm/rich4_characters.c. No original
// name pointers, code addresses, sprites or other commercial asset data.
struct CharacterProfile {
    std::uint32_t color;
    std::uint8_t sex, strategy_23, strategy_24, cash_ratio, strategy_26;
};
constexpr std::array<CharacterProfile, 12> profiles{{
    CharacterProfile{0x946126, 1, 2, 60, 50, 30},
    {0xbdc3c6, 1, 1, 100, 40, 45},
    {0x41323b, 1, 2, 0, 70, 0},
    {0xc626c3, 0, 2, 100, 60, 30},
    {0xc5b830, 1, 1, 50, 40, 25},
    {0xed9d9d, 0, 1, 75, 70, 30},
    {0x00f038, 1, 1, 100, 50, 20},
    {0xffffa0, 0, 0, 0, 40, 35},
    {0xe77c08, 0, 0, 0, 60, 20},
    {0xcc1a20, 0, 0, 50, 50, 0},
    {0x2017fe, 1, 1, 30, 55, 15},
    {0x0ebdbd, 0, 2, 80, 80, 0},
}};
// Scalar stock initialization settings from asm/rich4_all_stocks.c.
struct InitialStock {
    std::uint16_t flags, shares;
    float price, volatility;
};
constexpr std::array<InitialStock, 96> initial_stocks{{
    {1, 10000, 100.0F, 1.000000F},  {1, 5000, 40.0F, 0.600000F},    {1, 10000, 25.0F, 1.500000F},
    {0, 10000, 180.0F, 1.600000F},  {0, 10000, 80.0F, 1.200000F},   {0, 10000, 60.0F, 1.000000F},
    {0, 10000, 60.0F, 1.400000F},   {0, 10000, 27.0F, 0.900000F},   {0, 10000, 310.0F, 0.700000F},
    {0, 10000, 66.0F, 1.000000F},   {0, 10000, 171.0F, 1.400000F},  {0, 10000, 280.0F, 0.800000F},
    {1, 10000, 70.0F, 1.200000F},   {1, 5000, 36.0F, 1.000000F},    {1, 10000, 20.0F, 1.600000F},
    {1, 0, 15.0F, 1.500000F},       {0, 10000, 300.0F, 2.000000F},  {0, 10000, 296.0F, 1.800000F},
    {0, 10000, 133.0F, 1.400000F},  {0, 10000, 170.0F, 1.500000F},  {0, 10000, 121.0F, 0.800000F},
    {0, 10000, 60.0F, 0.400000F},   {0, 10000, 92.0F, 0.600000F},   {0, 10000, 366.0F, 1.000000F},
    {1, 10000, 160.0F, 0.800000F},  {1, 5000, 99.0F, 1.400000F},    {1, 10000, 22.0F, 2.000000F},
    {1, 5000, 28.0F, 2.000000F},    {1, 5000, 300.0F, 1.000000F},   {1, 5000, 35.0F, 1.200000F},
    {0, 10000, 540.0F, 1.000000F},  {0, 10000, 830.0F, 1.000000F},  {0, 10000, 300.0F, 1.200000F},
    {0, 10000, 225.0F, 0.700000F},  {0, 10000, 300.0F, 1.000000F},  {0, 10000, 150.0F, 0.800000F},
    {1, 10000, 200.0F, 1.000000F},  {1, 5000, 55.0F, 0.800000F},    {1, 10000, 20.0F, 2.000000F},
    {1, 5000, 18.0F, 1.000000F},    {1, 5000, 16.0F, 1.400000F},    {1, 5000, 550.0F, 2.000000F},
    {0, 10000, 245.0F, 1.500000F},  {0, 10000, 166.0F, 1.300000F},  {0, 10000, 955.0F, 1.400000F},
    {0, 10000, 1030.0F, 0.800000F}, {0, 10000, 1440.0F, 1.000000F}, {0, 10000, 310.0F, 1.200000F},
    {1, 10000, 50.0F, 1.000000F},   {1, 5000, 40.0F, 0.600000F},    {1, 10000, 25.0F, 1.500000F},
    {0, 10000, 90.0F, 1.600000F},   {0, 10000, 45.0F, 1.200000F},   {0, 10000, 60.0F, 1.000000F},
    {0, 10000, 60.0F, 1.400000F},   {0, 10000, 27.0F, 0.900000F},   {0, 10000, 110.0F, 0.700000F},
    {0, 10000, 66.0F, 1.000000F},   {0, 10000, 71.0F, 1.400000F},   {0, 10000, 80.0F, 0.800000F},
    {1, 10000, 70.0F, 1.200000F},   {1, 5000, 70.0F, 1.000000F},    {1, 10000, 20.0F, 1.600000F},
    {1, 3000, 250.0F, 1.500000F},   {1, 3000, 101.0F, 2.000000F},   {1, 5000, 96.0F, 1.800000F},
    {1, 6000, 120.0F, 1.400000F},   {1, 4000, 47.0F, 1.500000F},    {1, 5000, 40.0F, 0.800000F},
    {1, 4000, 30.0F, 0.400000F},    {1, 5000, 90.0F, 0.600000F},    {1, 4000, 300.0F, 1.000000F},
    {1, 10000, 60.0F, 0.800000F},   {1, 5000, 33.0F, 1.400000F},    {1, 10000, 22.0F, 2.000000F},
    {0, 10000, 18.0F, 2.000000F},   {0, 10000, 100.0F, 1.000000F},  {0, 10000, 15.0F, 1.200000F},
    {0, 10000, 40.0F, 1.000000F},   {0, 10000, 30.0F, 1.000000F},   {0, 10000, 100.0F, 1.200000F},
    {0, 10000, 25.0F, 0.700000F},   {0, 10000, 100.0F, 1.000000F},  {0, 10000, 50.0F, 0.800000F},
    {1, 10000, 100.0F, 1.000000F},  {1, 10000, 33.0F, 0.800000F},   {1, 3000, 500.0F, 2.000000F},
    {1, 5000, 100.0F, 1.000000F},   {1, 5000, 130.0F, 1.400000F},   {1, 5000, 150.0F, 2.000000F},
    {1, 5000, 70.0F, 1.500000F},    {0, 10000, 66.0F, 1.300000F},   {0, 10000, 55.0F, 1.400000F},
    {0, 10000, 30.0F, 0.800000F},   {0, 10000, 44.0F, 1.000000F},   {0, 10000, 30.0F, 1.200000F},
}};

constexpr CardSupply initial_cards{1, 2, 4, 4, 4, 3, 8, 3, 2, 1, 2, 5, 4, 4, 2,
                                   4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 4, 3, 3, 2, 3};

template <class Record> void check_table(const std::vector<Record> &records, const MapTableSummary &table) {
    if (records.size() != static_cast<std::uint64_t>(table.count) + 1)
        throw SaveError("World export cannot resize an original map table");
    for (std::size_t id = 0; id < records.size(); ++id)
        if (records[id].id != id)
            throw SaveError("World map record ID no longer matches its index");
}
} // namespace

World World::from_save(const SaveFile &save) {
    World world;
    world.header = save.header();
    world.map_ = save.map_summary();
    world.backing_ = save.encode();
    const auto &bytes = world.backing_;
    for (std::size_t slot = 0; slot < world.header.player_count; ++slot) {
        WorldPlayer player;
        player.state = save.player(slot);
        const auto offset = 0x10 + slot * 104;
        for (const auto &field : status_bytes)
            player.status.*field.member = bytes[offset + field.offset];
        player.status.loan_due_date = u32(bytes, offset + 44);
        player.status.god_luck_modifier = std::bit_cast<std::int16_t>(u16(bytes, offset + 68));
        player.status.money_fortune_modifier = std::bit_cast<std::int16_t>(u16(bytes, offset + 70));
        player.status.other_fortune_modifier = std::bit_cast<std::int16_t>(u16(bytes, offset + 72));
        player.status.facility_id = u16(bytes, offset + 74);
        for (std::size_t i = 0; i < 4; ++i)
            player.status.hostility[i] = u32(bytes, offset + 76 + 4 * i);
        player.status.cumulative_spending = u32(bytes, offset + 92);
        player.status.cumulative_income = u32(bytes, offset + 96);
        for (std::size_t stock = 0; stock < player.stocks.size(); ++stock) {
            const auto stock_offset = 0x21f6 + slot * 96 + stock * 8;
            player.stocks[stock] = {std::bit_cast<std::int32_t>(u32(bytes, stock_offset)),
                                    u32(bytes, stock_offset + 4)};
        }
        world.players.push_back(player);
    }
    std::copy_n(bytes.begin() + 0x6cc, world.card_supply.size(), world.card_supply.begin());
    std::copy_n(bytes.begin() + 0x6ea, world.tool_supply.size(), world.tool_supply.begin());
    std::copy_n(bytes.begin() + 0x26fa, world.news_deck.size(), world.news_deck.begin());
    std::copy_n(bytes.begin() + 0x271e, world.fortune_deck.size(), world.fortune_deck.begin());
    std::copy_n(bytes.begin() + 0x26e2, world.prison_roster.size(), world.prison_roster.begin());
    std::copy_n(bytes.begin() + 0x26ea, world.hospital_roster.size(), world.hospital_roster.begin());
    std::copy_n(bytes.begin() + 0x26be, world.lottery_tickets.size(), world.lottery_tickets.begin());
    world.news_cursor = u32(bytes, 0x26f2);
    world.fortune_cursor = u32(bytes, 0x26f6);
    world.public_fund = std::bit_cast<std::int32_t>(u32(bytes, 0x26ba));
    world.news_effect_days = std::bit_cast<std::int32_t>(u32(bytes, 0x269a));
    world.lease_mode = u32(bytes, 0x267e);
    world.camera_rotation = u32(bytes, 0x2743);
    world.round_counter = u32(bytes, 0x2692);
    world.month_counter = u32(bytes, 0x2696);
    world.stock_market_index = std::bit_cast<std::int32_t>(u32(bytes, 0x26a2));
    world.stock_market_movement_bits = u32(bytes, 0x26a6);
    world.stock_history_cursor = std::bit_cast<std::int32_t>(u32(bytes, 0x6f2));
    for (std::size_t seller = 0; seller < world.offers.size(); ++seller) {
        for (std::size_t index = 0; index < world.offers[seller].size(); ++index) {
            const auto offset = 0x2526 + seller * 84 + index * 12;
            auto &offer = world.offers[seller][index];
            offer.kind = static_cast<OfferKind>(bytes[offset]);
            offer.flags = bytes[offset + 1];
            offer.item = u16(bytes, offset + 2);
            offer.price = std::bit_cast<std::int32_t>(u32(bytes, offset + 4));
            offer.quantity = u16(bytes, offset + 8);
            offer.property_type = bytes[offset + 10];
            offer.property_level = bytes[offset + 11];
        }
    }
    for (std::size_t id = 0; id < world.special_actors.size(); ++id) {
        const auto offset = 0x1b4 + id * 16;
        world.special_actors[id] = {u16(bytes, offset),     u16(bytes, offset + 2), u16(bytes, offset + 4),
                                    u16(bytes, offset + 6), bytes[offset + 8],      bytes[offset + 9],
                                    bytes[offset + 10],     bytes[offset + 11],     bytes[offset + 12],
                                    bytes[offset + 13],     bytes[offset + 14],     bytes[offset + 15]};
    }
    for (std::size_t id = 0; id < world.stocks.size(); ++id) {
        const auto offset = 0x2376 + id * 36;
        auto &stock = world.stocks[id];
        stock.flags_4 = u16(bytes, offset + 4);
        stock.suspension_days = bytes[offset + 6];
        stock.news_movement = bytes[offset + 7];
        stock.shares_available = u16(bytes, offset + 8);
        stock.flags_10 = u16(bytes, offset + 10);
        stock.raw_32 = u32(bytes, offset + 32);
        for (std::size_t i = 0; i < 5; ++i)
            stock.float_bits[i] = u32(bytes, offset + 12 + 4 * i);
        for (std::size_t i = 0; i < 144; ++i)
            world.stock_history_bits[id][i] = u32(bytes, 0x6f6 + id * 576 + i * 4);
    }
    for (std::size_t id = 0; id < world.objects.size(); ++id) {
        const auto offset = 0x204 + id * 24;
        auto &object = world.objects[id];
        object.kind = bytes[offset];
        object.direction = bytes[offset + 1];
        object.node_id = u16(bytes, offset + 2);
        object.remaining_days = bytes[offset + 4];
        object.attached_player = bytes[offset + 5];
        object.animation_remaining = bytes[offset + 6];
        object.animation_step = bytes[offset + 7];
        for (std::size_t i = 0; i < 4; ++i)
            object.motion_bits[i] = u32(bytes, offset + 8 + i * 4);
    }
    for (std::uint32_t id = 0; id <= world.map_.nodes.count; ++id) {
        const auto offset = record_offset(world.map_.nodes, id);
        MapNode node;
        node.id = id;
        node.x = std::bit_cast<std::int16_t>(u16(bytes, offset));
        node.y = std::bit_cast<std::int16_t>(u16(bytes, offset + 2));
        for (std::size_t i = 0; i < 4; ++i)
            node.neighbors[i] = u16(bytes, offset + 24 + 2 * i);
        node.type_and_index = u16(bytes, offset + 32);
        node.ground_sprite = u16(bytes, offset + 34);
        node.flags = u32(bytes, offset + 36);
        node.name = name_at(bytes, offset + 4);
        world.nodes.push_back(node);
    }
    for (std::uint32_t id = 0; id <= world.map_.lands.count; ++id) {
        const auto offset = record_offset(world.map_.lands, id);
        Land land;
        land.id = id;
        land.x = u16(bytes, offset);
        land.y = u16(bytes, offset + 2);
        land.economy.street_name = name_at(bytes, offset + 4);
        land.temporary_state = bytes[offset + 23];
        land.economy.chain_store = bytes[offset + 24];
        land.economy.owner_code = bytes[offset + 25];
        land.economy.level = bytes[offset + 26];
        land.direction = bytes[offset + 27];
        land.land_price = u16(bytes, offset + 28);
        land.price_per_level = u16(bytes, offset + 30);
        for (std::size_t i = 0; i < 6; ++i)
            land.economy.toll_by_level[i] = u16(bytes, offset + 32 + 2 * i);
        land.removed_building = u32(bytes, offset + 44);
        land.expired_date = u32(bytes, offset + 48);
        world.lands.push_back(std::move(land));
    }
    for (std::uint32_t id = 0; id <= world.map_.facilities.count; ++id) {
        const auto offset = record_offset(world.map_.facilities, id);
        Facility facility;
        facility.id = id;
        facility.x = u16(bytes, offset);
        facility.y = u16(bytes, offset + 2);
        facility.name = name_at(bytes, offset + 4);
        facility.facility_type = bytes[offset + 24];
        facility.owner_code = bytes[offset + 25];
        facility.level = bytes[offset + 26];
        facility.direction = bytes[offset + 27];
        facility.temporary_state = bytes[offset + 28];
        facility.research_tool = bytes[offset + 29];
        facility.flags_30 = bytes[offset + 30];
        facility.land_price = u16(bytes, offset + 34);
        for (std::size_t i = 0; i < 6; ++i)
            facility.toll_by_level[i] = u16(bytes, offset + 36 + 2 * i);
        facility.removed_building = u32(bytes, offset + 48);
        facility.expired_date = u32(bytes, offset + 52);
        world.facilities.push_back(std::move(facility));
    }
    for (std::uint32_t id = 0; id <= world.map_.commercials.count; ++id) {
        const auto offset = record_offset(world.map_.commercials, id);
        world.commercials.push_back({id, u16(bytes, offset), u16(bytes, offset + 2), bytes[offset + 24],
                                     bytes[offset + 26], bytes[offset + 27], u16(bytes, offset + 32),
                                     u16(bytes, offset + 34)});
        auto &commercial = world.commercials.back();
        commercial.stock_index = bytes[offset + 25];
        commercial.funds = std::bit_cast<std::int32_t>(u32(bytes, offset + 40));
        commercial.funds_change = std::bit_cast<std::int32_t>(u32(bytes, offset + 44));
        commercial.shares_remaining = std::bit_cast<std::int32_t>(u32(bytes, offset + 48));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 28), 4,
                    commercial.shareholder_order.begin());
        commercial.valuation = std::bit_cast<std::int32_t>(u32(bytes, offset + 36));
        commercial.name = name_at(bytes, offset + 4);
    }
    for (std::uint32_t id = 0; id <= world.map_.raw_reserved_table_4.count; ++id) {
        const auto offset = record_offset(world.map_.raw_reserved_table_4, id);
        world.landscapes.push_back(
            {id, u16(bytes, offset), u16(bytes, offset + 2), bytes[offset + 24], u16(bytes, offset + 26),
             name_at(bytes, offset + 4)});
    }
    return world;
}

WorldPlayer &World::active_player() {
    return players.at(header.current_player);
}
const WorldPlayer &World::active_player() const {
    return players.at(header.current_player);
}

void World::clear_bankrupt_player_fields(std::size_t slot) {
    auto &player = players.at(slot);
    if (slot >= u32(backing_, 12))
        throw SaveError("Bankrupt player slot is outside original backing");
    const auto offset = 0x10 + slot * 104 + 28;
    std::fill_n(backing_.begin() + static_cast<std::ptrdiff_t>(offset), 76, 0);
    player.state.cash = 0;
    player.state.bank_balance = 0;
    player.state.loan = 0;
    player.state.special_finance = 0;
    player.state.points = 0;
    const auto ratio = player.status.init_cash_ratio;
    const auto rescue = player.status.rescue_strategy;
    const auto card_strategy = player.status.card_strategy;
    const auto loan_ratio = player.status.loan_ratio;
    const auto investment_ratio = player.status.stock_investment_ratio;
    const auto saved_direction = player.status.saved_direction;
    player.status = {};
    player.status.init_cash_ratio = ratio;
    player.status.rescue_strategy = rescue;
    player.status.card_strategy = card_strategy;
    player.status.loan_ratio = loan_ratio;
    player.status.stock_investment_ratio = investment_ratio;
    player.status.saved_direction = saved_direction;
}

std::vector<std::uint8_t> World::export_save() const {
    if (backing_.empty())
        throw SaveError("World has no save backing");
    if (header.signature != 0x26 || players.size() != u32(backing_, 12) ||
        header.player_count != players.size() || header.current_player >= players.size() ||
        header.human_player_count > players.size())
        throw SaveError("World has inconsistent player/header counts");
    check_table(nodes, map_.nodes);
    check_table(lands, map_.lands);
    check_table(facilities, map_.facilities);
    check_table(commercials, map_.commercials);
    check_table(landscapes, map_.raw_reserved_table_4);
    auto bytes = backing_;
    bytes[4] = header.date.day;
    bytes[5] = header.date.month;
    put16(bytes, 6, header.date.year);
    put16(bytes, 8, header.map_id);
    put16(bytes, 10, header.stage_id);
    put32(bytes, 0x1b0, header.human_player_count);
    put32(bytes, 0x2676, header.current_player);
    put32(bytes, 0x268a, std::bit_cast<std::uint32_t>(header.initial_fund));
    put32(bytes, 0x268e, std::bit_cast<std::uint32_t>(header.price_index));
    for (std::size_t slot = 0; slot < players.size(); ++slot) {
        const auto offset = 0x10 + slot * 104;
        const auto &player = players[slot];
        const auto &state = player.state;
        if (state.slot != slot)
            throw SaveError("World player slot no longer matches its index");
        put32(bytes, offset + 4, state.color);
        put16(bytes, offset + 8, state.x);
        put16(bytes, offset + 10, state.y);
        put16(bytes, offset + 12, state.node_id);
        put16(bytes, offset + 14, state.last_node_id);
        bytes[offset + 16] = state.direction;
        bytes[offset + 17] = state.traffic_method;
        bytes[offset + 18] = state.dice_count;
        bytes[offset + 19] = state.character;
        bytes[offset + 20] = state.sex;
        bytes[offset + 21] = state.controller_flags;
        put32(bytes, offset + 28, std::bit_cast<std::uint32_t>(state.cash));
        put32(bytes, offset + 32, std::bit_cast<std::uint32_t>(state.bank_balance));
        put32(bytes, offset + 36, std::bit_cast<std::uint32_t>(state.loan));
        put32(bytes, offset + 40, std::bit_cast<std::uint32_t>(state.special_finance));
        put32(bytes, offset + 44, player.status.loan_due_date);
        put16(bytes, offset + 48, state.points);
        for (const auto &field : status_bytes)
            bytes[offset + field.offset] = player.status.*field.member;
        put16(bytes, offset + 68, std::bit_cast<std::uint16_t>(player.status.god_luck_modifier));
        put16(bytes, offset + 70, std::bit_cast<std::uint16_t>(player.status.money_fortune_modifier));
        put16(bytes, offset + 72, std::bit_cast<std::uint16_t>(player.status.other_fortune_modifier));
        put16(bytes, offset + 74, player.status.facility_id);
        for (std::size_t i = 0; i < 4; ++i)
            put32(bytes, offset + 76 + 4 * i, player.status.hostility[i]);
        put32(bytes, offset + 92, player.status.cumulative_spending);
        put32(bytes, offset + 96, player.status.cumulative_income);
        std::copy(state.cards.begin(), state.cards.end(),
                  bytes.begin() + 0x654 + static_cast<std::ptrdiff_t>(slot * 15));
        std::copy(state.tool_amounts.begin(), state.tool_amounts.end(),
                  bytes.begin() + 0x690 + static_cast<std::ptrdiff_t>(slot * 15));
        for (std::size_t stock = 0; stock < player.stocks.size(); ++stock) {
            const auto stock_offset = 0x21f6 + slot * 96 + stock * 8;
            put32(bytes, stock_offset, std::bit_cast<std::uint32_t>(player.stocks[stock].amount));
            put32(bytes, stock_offset + 4, player.stocks[stock].raw_cost_basis);
        }
    }
    std::copy(card_supply.begin(), card_supply.end(), bytes.begin() + 0x6cc);
    std::copy(tool_supply.begin(), tool_supply.end(), bytes.begin() + 0x6ea);
    std::copy(news_deck.begin(), news_deck.end(), bytes.begin() + 0x26fa);
    std::copy(fortune_deck.begin(), fortune_deck.end(), bytes.begin() + 0x271e);
    std::copy(prison_roster.begin(), prison_roster.end(), bytes.begin() + 0x26e2);
    std::copy(hospital_roster.begin(), hospital_roster.end(), bytes.begin() + 0x26ea);
    std::copy(lottery_tickets.begin(), lottery_tickets.end(), bytes.begin() + 0x26be);
    put32(bytes, 0x26f2, news_cursor);
    put32(bytes, 0x26f6, fortune_cursor);
    put32(bytes, 0x26ba, std::bit_cast<std::uint32_t>(public_fund));
    put32(bytes, 0x269a, std::bit_cast<std::uint32_t>(news_effect_days));
    put32(bytes, 0x267e, lease_mode);
    put32(bytes, 0x2743, camera_rotation);
    put32(bytes, 0x2692, round_counter);
    put32(bytes, 0x2696, month_counter);
    put32(bytes, 0x26a2, std::bit_cast<std::uint32_t>(stock_market_index));
    put32(bytes, 0x26a6, stock_market_movement_bits);
    put32(bytes, 0x6f2, std::bit_cast<std::uint32_t>(stock_history_cursor));
    for (std::size_t seller = 0; seller < offers.size(); ++seller) {
        for (std::size_t index = 0; index < offers[seller].size(); ++index) {
            const auto offset = 0x2526 + seller * 84 + index * 12;
            const auto &offer = offers[seller][index];
            bytes[offset] = static_cast<std::uint8_t>(offer.kind);
            bytes[offset + 1] = offer.flags;
            put16(bytes, offset + 2, offer.item);
            put32(bytes, offset + 4, std::bit_cast<std::uint32_t>(offer.price));
            put16(bytes, offset + 8, offer.quantity);
            bytes[offset + 10] = offer.property_type;
            bytes[offset + 11] = offer.property_level;
        }
    }
    for (std::size_t id = 0; id < special_actors.size(); ++id) {
        const auto offset = 0x1b4 + id * 16;
        const auto &actor = special_actors[id];
        put16(bytes, offset, actor.x);
        put16(bytes, offset + 2, actor.y);
        put16(bytes, offset + 4, actor.node_id);
        put16(bytes, offset + 6, actor.last_node_id);
        bytes[offset + 8] = actor.owner;
        bytes[offset + 9] = actor.direction;
        bytes[offset + 10] = actor.inactive_state;
        bytes[offset + 11] = actor.flags;
        bytes[offset + 12] = actor.days_sleeping;
        bytes[offset + 13] = actor.days_sleep_walking;
        bytes[offset + 14] = actor.days_stopping;
        bytes[offset + 15] = actor.days_tortoise_walking;
    }
    for (std::size_t id = 0; id < stocks.size(); ++id) {
        const auto offset = 0x2376 + id * 36;
        const auto &stock = stocks[id];
        put16(bytes, offset + 4, stock.flags_4);
        bytes[offset + 6] = stock.suspension_days;
        bytes[offset + 7] = stock.news_movement;
        put16(bytes, offset + 8, stock.shares_available);
        put16(bytes, offset + 10, stock.flags_10);
        put32(bytes, offset + 32, stock.raw_32);
        for (std::size_t i = 0; i < 5; ++i)
            put32(bytes, offset + 12 + 4 * i, stock.float_bits[i]);
        for (std::size_t i = 0; i < 144; ++i)
            put32(bytes, 0x6f6 + id * 576 + 4 * i, stock_history_bits[id][i]);
    }
    for (std::size_t id = 0; id < objects.size(); ++id) {
        const auto offset = 0x204 + id * 24;
        const auto &object = objects[id];
        bytes[offset] = object.kind;
        bytes[offset + 1] = object.direction;
        put16(bytes, offset + 2, object.node_id);
        bytes[offset + 4] = object.remaining_days;
        bytes[offset + 5] = object.attached_player;
        bytes[offset + 6] = object.animation_remaining;
        bytes[offset + 7] = object.animation_step;
        for (std::size_t i = 0; i < 4; ++i)
            put32(bytes, offset + 8 + i * 4, object.motion_bits[i]);
    }
    for (const auto &node : nodes) {
        const auto offset = record_offset(map_.nodes, node.id);
        put16(bytes, offset, std::bit_cast<std::uint16_t>(node.x));
        put16(bytes, offset + 2, std::bit_cast<std::uint16_t>(node.y));
        for (std::size_t i = 0; i < 4; ++i)
            put16(bytes, offset + 24 + i * 2, node.neighbors[i]);
        put16(bytes, offset + 32, node.type_and_index);
        put16(bytes, offset + 34, node.ground_sprite);
        put32(bytes, offset + 36, node.flags);
    }
    for (const auto &land : lands) {
        const auto offset = record_offset(map_.lands, land.id);
        put16(bytes, offset, land.x);
        put16(bytes, offset + 2, land.y);
        put_name(bytes, offset + 4, land.economy.street_name);
        bytes[offset + 23] = land.temporary_state;
        bytes[offset + 24] = land.economy.chain_store;
        bytes[offset + 25] = land.economy.owner_code;
        bytes[offset + 26] = land.economy.level;
        bytes[offset + 27] = land.direction;
        put16(bytes, offset + 28, land.land_price);
        put16(bytes, offset + 30, land.price_per_level);
        for (std::size_t i = 0; i < 6; ++i)
            put16(bytes, offset + 32 + i * 2, land.economy.toll_by_level[i]);
        put32(bytes, offset + 44, land.removed_building);
        put32(bytes, offset + 48, land.expired_date);
    }
    for (const auto &facility : facilities) {
        const auto offset = record_offset(map_.facilities, facility.id);
        put16(bytes, offset, facility.x);
        put16(bytes, offset + 2, facility.y);
        put_name(bytes, offset + 4, facility.name);
        bytes[offset + 24] = facility.facility_type;
        bytes[offset + 25] = facility.owner_code;
        bytes[offset + 26] = facility.level;
        bytes[offset + 27] = facility.direction;
        bytes[offset + 28] = facility.temporary_state;
        bytes[offset + 29] = facility.research_tool;
        bytes[offset + 30] = facility.flags_30;
        put16(bytes, offset + 34, facility.land_price);
        for (std::size_t i = 0; i < 6; ++i)
            put16(bytes, offset + 36 + 2 * i, facility.toll_by_level[i]);
        put32(bytes, offset + 48, facility.removed_building);
        put32(bytes, offset + 52, facility.expired_date);
    }
    for (const auto &commerce : commercials) {
        const auto offset = record_offset(map_.commercials, commerce.id);
        put16(bytes, offset, commerce.x);
        put16(bytes, offset + 2, commerce.y);
        bytes[offset + 24] = commerce.owner_code;
        bytes[offset + 26] = commerce.commerce_type;
        bytes[offset + 27] = commerce.direction;
        put16(bytes, offset + 32, commerce.sprite_index);
        put16(bytes, offset + 34, commerce.toll_fee);
        bytes[offset + 25] = commerce.stock_index;
        put32(bytes, offset + 40, std::bit_cast<std::uint32_t>(commerce.funds));
        put32(bytes, offset + 44, std::bit_cast<std::uint32_t>(commerce.funds_change));
        put32(bytes, offset + 48, std::bit_cast<std::uint32_t>(commerce.shares_remaining));
        std::copy(commerce.shareholder_order.begin(), commerce.shareholder_order.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + 28));
        put32(bytes, offset + 36, std::bit_cast<std::uint32_t>(commerce.valuation));
    }
    for (const auto &landscape : landscapes) {
        const auto offset = record_offset(map_.raw_reserved_table_4, landscape.id);
        put16(bytes, offset, landscape.x);
        put16(bytes, offset + 2, landscape.y);
        bytes[offset + 24] = landscape.direction;
        put16(bytes, offset + 26, landscape.sprite_index);
    }
    return bytes;
}

World World::from_map_resource(std::span<const std::uint8_t> map, const NewGameOptions &options) {
    if (map.size() < 40 || map.size() > std::numeric_limits<std::uint32_t>::max())
        throw SaveError("Map resource size is outside the original format");
    if (options.players.empty() || options.players.size() > 4 || options.initial_fund < 0 ||
        options.map_id > 3 || options.stage_id > 1 || options.lease_mode > 5)
        throw SaveError("Invalid new-game options");
    const auto total = std::uint64_t(map_offset) + map.size() +
                       options.players.size() * (std::uint64_t(snapshot_bytes) + map.size());
    if (total > std::numeric_limits<std::size_t>::max())
        throw SaveError("New-game save size overflows this host");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total), 0);
    put32(bytes, 0, 0x26);
    put32(bytes, 12, static_cast<std::uint32_t>(options.players.size()));
    put32(bytes, 0x2747, static_cast<std::uint32_t>(map.size()));
    std::copy(map.begin(), map.end(), bytes.begin() + map_offset);
    // SaveFile performs complete table bounds/overlap validation before any
    // world record is dereferenced. Unknown new-game state starts as zero.
    auto world = from_save(SaveFile::decode(bytes));
    if (world.nodes.size() < 2 || world.nodes.size() > 256)
        throw SaveError("Original spawn selection requires 1..255 map nodes");
    world.header.date = options.date;
    world.header.map_id = options.map_id;
    world.header.stage_id = options.stage_id;
    world.header.initial_fund = options.initial_fund;
    world.header.price_index = 1;
    world.lease_mode = options.lease_mode;
    world.card_supply = initial_cards;
    world.tool_supply.fill(10);
    world.prison_roster[4] = world.prison_roster[5] = 1;
    world.hospital_roster[6] = world.hospital_roster[7] = 1;
    for (std::size_t id = 0; id < world.special_actors.size(); ++id)
        world.special_actors[id].inactive_state = static_cast<std::uint8_t>(id / 2 + 1);
    for (std::size_t id = 0; id < world.objects.size(); ++id)
        world.objects[id].kind = static_cast<std::uint8_t>(id < 14   ? id + 1
                                                           : id < 16 ? 15
                                                                     : 16 + (id - 16) / 10);
    for (std::size_t index = 0; index < world.stocks.size(); ++index) {
        const auto &source = initial_stocks[(options.stage_id * 4U + options.map_id) * 12U + index];
        auto &stock = world.stocks[index];
        stock.flags_4 = source.flags;
        stock.shares_available = source.shares;
        const auto price_bits = std::bit_cast<std::uint32_t>(source.price);
        stock.float_bits = {price_bits, price_bits, price_bits,
                            std::bit_cast<std::uint32_t>(source.volatility), 0};
    }
    for (std::size_t id = 1; id < world.commercials.size(); ++id) {
        auto &commercial = world.commercials[id];
        if (commercial.stock_index >= world.stocks.size())
            throw SaveError("Commercial stock index is outside original stock table");
        commercial.shares_remaining = 10000 - world.stocks[commercial.stock_index].shares_available;
        world.stocks[commercial.stock_index].flags_4 = static_cast<std::uint16_t>(id);
    }
    Random random(options.random_seed);
    for (std::size_t id = 1; id < world.nodes.size(); ++id) {
        const auto &node = world.nodes[id];
        for (auto neighbor : node.neighbors)
            if (neighbor >= world.nodes.size())
                throw SaveError("New-game node link is outside map");
    }
    if (std::any_of(options.players.begin(), options.players.end(),
                    [](const auto &player) { return player.start_node == 0; }) &&
        std::none_of(world.nodes.begin() + 1, world.nodes.end(), [](const auto &node) {
            return (node.flags & 0x80ffff00U) == 0 &&
                   std::any_of(node.neighbors.begin(), node.neighbors.end(),
                               [](auto neighbor) { return neighbor != 0; });
        }))
        throw SaveError("Map has no spawnable connected nodes");
    for (std::size_t slot = 0; slot < options.players.size(); ++slot) {
        const auto &option = options.players[slot];
        if (option.character >= profiles.size() || (option.controller != 1 && option.controller != 2) ||
            option.traffic_method > 2 || option.start_node >= world.nodes.size())
            throw SaveError("Invalid new-game player");
        const auto &profile = profiles[option.character];
        auto &player = world.players[slot];
        auto &state = player.state;
        state.character = option.character;
        state.color = profile.color;
        state.sex = profile.sex;
        state.controller_flags = option.controller;
        player.status.flags_100 = option.controller;
        player.status.init_cash_ratio = profile.cash_ratio;
        player.status.stock_investment_ratio = profile.strategy_26;
        player.status.rescue_strategy = profile.strategy_23;
        player.status.card_strategy = 3;
        player.status.loan_ratio = profile.strategy_24;
        state.traffic_method = option.traffic_method;
        state.dice_count = static_cast<std::uint8_t>(option.traffic_method + 1);
        const auto cash_ratio = option.controller == 1 ? 50 : profile.cash_ratio;
        state.cash = static_cast<std::int32_t>(std::int64_t(options.initial_fund) * cash_ratio / 100);
        state.bank_balance = options.initial_fund - state.cash;
        if (option.controller == 1)
            ++world.header.human_player_count;
        if (option.start_node != 0) {
            state.node_id = option.start_node;
            auto &node = world.nodes[state.node_id];
            std::vector<std::uint16_t> neighbors;
            for (auto neighbor : node.neighbors)
                if (neighbor != 0)
                    neighbors.push_back(neighbor);
            if (neighbors.empty())
                throw SaveError("Player start node has no route");
            state.last_node_id = neighbors[random.next() % neighbors.size()];
            state.x = std::bit_cast<std::uint16_t>(node.x);
            state.y = std::bit_cast<std::uint16_t>(node.y);
            state.direction = direction::from_delta(node.x - world.nodes[state.last_node_id].x,
                                                    node.y - world.nodes[state.last_node_id].y);
            node.flags |= std::uint32_t(0x100) << slot;
        }
        for (const auto tool : std::array<std::uint8_t, 6>{1, 2, 3, 4, 8, 9})
            receive_tool(state.tool_amounts, world.tool_supply, tool);
        if (option.traffic_method != 0)
            --world.tool_supply[option.traffic_method + 3];
        const auto offset = 0x10 + slot * 104;
        world.backing_[offset + 22] = 3;
        world.backing_[offset + 23] = profile.strategy_23;
        world.backing_[offset + 24] = profile.strategy_24;
        world.backing_[offset + 26] = profile.strategy_26;
    }
    shuffle_deck(world.news_deck, random);
    shuffle_deck(world.fortune_deck, random);
    world.random_state_ = random.state();
    world.random_state_available_ = true;
    return world;
}

} // namespace rich4
